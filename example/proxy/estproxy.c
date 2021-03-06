/*------------------------------------------------------------------
 * estproxy.c -  Example application that utilizes libest.so for
 *               EST proxy operations.  libest does not manage
 *               sockets and pthreads.  This responsibility is
 *               placed on the application.  This module shows
 *               a fairly trivial example of how to setup a
 *               listening socket and serve EST requests.
 *
 * May, 2013
 *
 * Copyright (c) 2013 by cisco Systems, Inc.
 * All rights reserved.
 **------------------------------------------------------------------
 */

/* Main routine */
#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <stdint.h>
#include <signal.h>
#include <pthread.h>
//#include <fcntl.h>
#include <getopt.h>
#include <openssl/ssl.h>
#include <openssl/bio.h>
#include <est.h>
#include <sys/types.h>
#include <netinet/in.h>
#include "../util/utils.h"
#include "../util/simple_server.h"


#define MAX_SERVER_LEN 32
#define PROXY_PORT 8086  

static char est_server[MAX_SERVER_LEN];
static int est_server_port;
static int listen_port = PROXY_PORT;
static int verbose = 0;
static int pop = 0;
static int v6 = 0;
static int srp = 0;
static int http_auth_disable = 0;
static int http_digest_auth = 0;
static int set_fips_return = 0;
static unsigned long set_fips_error = 0;

EST_CTX *ectx;
char certfile[EST_MAX_FILE_LEN];
char keyfile[EST_MAX_FILE_LEN];
char realm[MAX_REALM];
unsigned char *cacerts_raw = NULL;
int cacerts_len = 0;
unsigned char *trustcerts = NULL;
int trustcerts_len = 0;

SRP_VBASE *srp_db = NULL;

static void print_version (FILE *fp)
{
    fprintf(fp, "Using %s\n", SSLeay_version(SSLEAY_VERSION));
}

static void show_usage_and_exit (void)
{
    fprintf(stderr, "\nAvailable estserver options\n"
            "  -v           Verbose operation\n"
            "  -n           Disable HTTP authentication\n"
            "  -h           Use HTTP Digest auth instead of Basic auth\n"
            "  -t           Enable PoP check of TLS UID\n"
            "  -c <file>    PEM file to use for server cert\n"
            "  -k <file>    PEM file to use for server key\n"
            "  -s <server>  Upstream server IP address\n"
            "  -p <port#>   Upstream server TCP port#\n"
	    "  -l <port#>   Downstream client TCP port# to listen on\n"
            "  -r <value>   HTTP realm to present to clients\n"
#ifndef DISABLE_PTHREADS
	    "  -d <seconds> Sleep timer to auto-shut the server\n"
#endif
	    "  -f           Runs EST Proxy in FIPS MODE = ON\n"
	    "  -6           Enable IPv6\n"
	    "  --srp <file> Enable TLS-SRP authentication of client using the specified SRP parameters file\n"
            "\n");
    exit(255);
}

static char digest_user[3][32] = 
    {
	"estuser", 
	"estrealm", 
	"36807fa200741bb0e8fb04fcf08e2de6" //This is the HA1 precaculated value
    };

int process_http_auth (EST_CTX *ctx, EST_HTTP_AUTH_HDR *ah, X509 *peer_cert,
	               void *app_data)
{
    int user_valid = 0; 
    char *digest;

    switch (ah->mode) {
    case AUTH_BASIC:
	/*
	 * this is where we might hook into a Radius server
	 * or some external database to authenticate a 
	 * userID/password.  But for this example code,
	 * we just hard-code a local user for testing
	 * the libest API.
	 */
	if (!strcmp(ah->user, "estuser") && !strcmp(ah->pwd, "estpwd")) {
	    /* The user is valid */
	    user_valid = 1;
	} 
	break;
    case AUTH_DIGEST:
	/*
	 * Check the user's name
	 */
	if (strcmp(ah->user, digest_user[0])) {
	    return 0;
	}

	/*
	 * Call the helper function to calculate the digest,
	 * This is then compared against the digest provided
	 * by the client.
	 */
	digest = est_server_generate_auth_digest(ah, digest_user[2]);
	if (!strncmp(digest, ah->response, 32)) {
	    user_valid = 1;
	}
	free(digest);
	break;
    case AUTH_FAIL:
    case AUTH_NONE:
    default:
	return 0;
	break;
    }
    return user_valid;
}

/*
 * This callback is issued during the TLS-SRP handshake.  
 * We can use this to get the userid from the TLS-SRP handshake.
 * If a verifier file as provided, we must pull the SRP verifier 
 * parameters and invoke SSL_set_srp_server_param() with these
 * values to allow the TLS handshake to succeed.  If the application
 * layer wants to use their own verifier store, they would
 * hook into it here.  They would lookup the verifier parameters
 * based on the userid and return those parameters by invoking
 * SSL_set_srp_server_param().
 */
static int process_ssl_srp_auth (SSL *s, int *ad, void *arg) {

    char *login = SSL_get_srp_username(s);
    SRP_user_pwd *user;

    if (!login) return (-1);

    printf("SRP username = %s\n", login);

    user = SRP_VBASE_get_by_user(srp_db, login); 

    if (user == NULL) {
	printf("User %s doesn't exist in SRP database\n", login);
	return SSL3_AL_FATAL;
    }

    /*
     * Get the SRP parameters for the user from the verifier database.
     * Provide these parameters to TLS to complete the handshake
     */
    if (SSL_set_srp_server_param(s, user->N, user->g, user->s, user->v, user->info) < 0) {
	*ad = SSL_AD_INTERNAL_ERROR;
	return SSL3_AL_FATAL;
    }
		
    printf("SRP parameters set: username = \"%s\" info=\"%s\" \n", login, user->info);

    user = NULL;
    login = NULL;
    fflush(stdout);
    return SSL_ERROR_NONE;
}

/*
 * We're using OpenSSL, both as the CA and libest
 * requires it.  OpenSSL requires these platform specific
 * locking callbacks to be set when multi-threaded support
 * is needed.  
 */
static pthread_mutex_t *ssl_mutexes;
static void ssl_locking_callback (int mode, int mutex_num, const char *file,
                                  int line)
{
    if (mode & CRYPTO_LOCK) {
        (void)pthread_mutex_lock(&ssl_mutexes[mutex_num]);
    } else {
        (void)pthread_mutex_unlock(&ssl_mutexes[mutex_num]);
    }
}
static unsigned long ssl_id_callback (void)
{
    return (unsigned long)pthread_self();
}

void cleanup (void)
{
    int i;

    /*
     * Tear down the mutexes used by OpenSSL
     */
    CRYPTO_set_locking_callback(NULL);
    for (i = 0; i < CRYPTO_num_locks(); i++) {
        pthread_mutex_destroy(&ssl_mutexes[i]);
    }
    CRYPTO_set_locking_callback(NULL);
    CRYPTO_set_id_callback(NULL);
    free(ssl_mutexes);

    est_proxy_stop(ectx);
    est_destroy(ectx);
    if (srp_db) {
	SRP_VBASE_free(srp_db);
    }
    free(cacerts_raw);
    free(trustcerts);
    est_apps_shutdown();
}

int main (int argc, char **argv)
{
    char c;
    int i, size;
    EVP_PKEY *priv_key;
    BIO *certin, *keyin;
    X509 *x;
    EST_ERROR rv;
    int sleep_delay = 0;
    char vfile[255];
    int option_index = 0;
    static struct option long_options[] = {
        {"srp", 1, NULL, 0},
        {NULL, 0, NULL, 0}
    };
    
    /* Show usage if -h or --help options are specified or if no parameters have
     * been specified.  Upstream server and port are required.
     */
    if ((argc == 1) ||
        (argc == 2 && (!strcmp(argv[1], "-h") || !strcmp(argv[1], "--help")))) {
        show_usage_and_exit();
    }

    while ((c = getopt_long(argc, argv, "vt6nhfr:c:k:s:p:l:d:", long_options, &option_index)) != -1) {
        switch (c) {
	case 0:
            if (!strncmp(long_options[option_index].name,"srp", strlen("srp"))) {
		srp = 1;
                strncpy(vfile, optarg, 255);
            }
	    break;
        case 'v':
            verbose = 1;
            break;
        case 't':
            pop = 1;
            break;
        case '6':
            v6 = 1;
            break;
        case 'h':
            http_digest_auth = 1;
            break;
        case 'n':
            http_auth_disable = 1;
            break;
        case 'c':
	    strncpy(certfile, optarg, EST_MAX_FILE_LEN);
            break;
        case 'k':
	    strncpy(keyfile, optarg, EST_MAX_FILE_LEN);
            break;
        case 'r':
	    strncpy(realm, optarg, MAX_REALM);
            break;
        case 's':
            strncpy(est_server, optarg, MAX_SERVER_LEN);
            break;
#ifndef DISABLE_PTHREADS
        case 'd':
	    sleep_delay = atoi(optarg);
            break;
#endif
        case 'p':
            est_server_port = atoi(optarg);
            break;
        case 'l':
            listen_port = atoi(optarg);
            break;
        case 'f':
           /*
            * Turn FIPS on if user requested it and exit if failure
            */
            set_fips_return = FIPS_mode_set(1);
            if (set_fips_return != 1) {
              set_fips_error = ERR_get_error();
              printf("\nERROR WHILE SETTING FIPS MODE ON exiting ....\n");
              exit(1);
            } else {
              printf("\nRunning EST Sample Proxy with FIPS MODE = ON !\n");
            };
            break;
        default:
            show_usage_and_exit();
            break;
        }
    }
    argc -= optind;
    argv += optind;

    if (verbose) {
        fprintf(stdout, "\nEST Proxy start up values:\n");
        print_version(stdout);
	fprintf(stdout, "Using EST server %s:%d", est_server, est_server_port);
	fprintf(stdout, "\nListening on port: %d", listen_port);
	fprintf(stdout, "\nUsing identity cert file: %s", certfile);
	fprintf(stdout, "\nUsing identity private key file: %s", keyfile);
	fprintf(stdout, "\nUsing realm value: %s\n", realm);
        fflush(stdout);
    }

    /*
     * Initialize EST context from libest.a
     */
    if (!getenv("EST_TRUSTED_CERTS")) {
        printf("\nEST_TRUSTED_CERTS file not set, set this env variable to resolve\n");
        exit(1);
    }

    /*
     * Read in the CA certificates
     */
    if (getenv("EST_CACERTS_RESP")) {
        cacerts_len = read_binary_file(getenv("EST_CACERTS_RESP"), &cacerts_raw);
        if (cacerts_len <= 0) {
            printf("\nEST_CACERTS_RESP file could not be read\n");
            exit(1);
        }
    }
    /*
     * Read in the trusted CA certificates for the local TLS context
     */
    if (getenv("EST_TRUSTED_CERTS")) {
        trustcerts_len = read_binary_file(getenv("EST_TRUSTED_CERTS"), &trustcerts);
        if (trustcerts_len <= 0) {
            printf("\nEST_TRUSTED_CERTS file could not be read\n");
            exit(1);
        }
    }

    est_apps_startup();

    /*
     * Read in the local server certificate 
     */
    certin = BIO_new(BIO_s_file_internal());
    if (BIO_read_filename(certin, certfile) <= 0) {
	printf("\nUnable to read server certificate file %s\n", certfile);
	exit(1);
    }
    /*
     * This reads the file, which is expected to be PEM encoded.  If you're using 
     * DER encoded certs, you would invoke d2i_X509_bio() instead.
     */
    x = PEM_read_bio_X509(certin, NULL, NULL, NULL);
    if (x == NULL) {
	printf("\nError while reading PEM encoded server certificate file %s\n", certfile);
	exit(1);
    }
    BIO_free(certin);

    /* 
     * Read in the server's private key
     */
    keyin = BIO_new(BIO_s_file_internal());
    if (BIO_read_filename(keyin, keyfile) <= 0) {
	printf("\nUnable to read server private key file %s\n", keyfile);
	exit(1);
    }
    /*
     * This reads in the private key file, which is expected to be a PEM
     * encoded private key.  If using DER encoding, you would invoke
     * d2i_PrivateKey_bio() instead. 
     */
    priv_key = PEM_read_bio_PrivateKey(keyin, NULL, NULL, NULL);
    if (priv_key == NULL) {
	printf("\nError while reading PEM encoded private key file %s\n", keyfile);
	ERR_print_errors_fp(stderr);
	exit(1);
    }
    BIO_free(keyin);

    est_init_logger(EST_LOG_LVL_INFO, NULL);
    if (verbose) {
	est_enable_backtrace(1);
    }
    ectx = est_proxy_init(trustcerts, trustcerts_len,
                          cacerts_raw, cacerts_len, EST_CERT_FORMAT_PEM,
                          realm, x, priv_key, "estuser", "estpwd");
    if (!ectx) {
        printf("\nUnable to initialize EST context.  Aborting!!!\n");
        exit(1);
    }
    if (!http_auth_disable) {
	if (est_set_http_auth_cb(ectx, &process_http_auth)) {
	    printf("\nUnable to set EST HTTP AUTH callback.  Aborting!!!\n");
	    exit(1);
	}    
    }

    if (http_digest_auth) {
	rv = est_proxy_set_auth_mode(ectx, AUTH_DIGEST);
	if (rv != EST_ERR_NONE) {
            printf("\nUnable to enable HTTP digest authentication.  Aborting!!!\n");
            exit(1);
	}
    }

    if (!pop) {
	printf("\nDisabling PoP check");
	est_server_disable_pop(ectx);
    }

    est_proxy_set_server(ectx, est_server, est_server_port);

    if (srp) {
	srp_db = SRP_VBASE_new(NULL);
	if (!srp_db) {
	    printf("\nUnable allocate SRP verifier database.  Aborting!!!\n");
	    exit(1); 
	}
	if (SRP_VBASE_init(srp_db, vfile) != SRP_NO_ERROR) {
	    printf("\nUnable initialize SRP verifier database.  Aborting!!!\n");
	    exit(1); 
	}
	
	if (est_server_enable_srp(ectx, &process_ssl_srp_auth)) { 
	    printf("\nUnable to enable SRP.  Aborting!!!\n");
	    exit(1);
	}
    }

    /*
     * Install thread locking mechanism for OpenSSL
     */
    size = sizeof(pthread_mutex_t) * CRYPTO_num_locks();
    if ((ssl_mutexes = (pthread_mutex_t*)malloc((size_t)size)) == NULL) {
        printf("Cannot allocate mutexes");
	exit(1);
    }   

    for (i = 0; i < CRYPTO_num_locks(); i++) {
        pthread_mutex_init(&ssl_mutexes[i], NULL);
    }
    CRYPTO_set_locking_callback(&ssl_locking_callback);
    CRYPTO_set_id_callback(&ssl_id_callback);

    printf("\nLaunching EST proxy...\n");

    rv = est_proxy_start(ectx);
    if (rv != EST_ERR_NONE) {
        printf("\nFailed to init mg (rv=%d)\n", rv);
        exit(1);
    }

    /*
     * Start the simple server, which opens a TCP
     * socket, waits for incoming connections, and
     * invokes the EST handler for each connection.
     */
    start_simple_server(ectx, listen_port, sleep_delay, v6);

    cleanup();
    return 0;
}

