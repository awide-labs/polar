/*-------------------------------------------------------------------------
 *
 * polar_px_openssl_auth.c
 *	  Routines to handle TLS authentication of PX Query Coordinator
 *
 * Portions Copyright (c) 1996-2022, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/libpq/polar_px_openssl_auth.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include <openssl/x509v3.h>
#include <sys/stat.h>

#include "libpq/libpq.h"
#include "libpq/auth.h"
#include "miscadmin.h"
#include "utils/guc.h"

#include "px/px_vars.h"

#define px_errmsg \
	pre_format_elog_string(errno, TEXTDOMAIN), \
	errmsg_string = format_elog_string

#define px_errdetail \
	pre_format_elog_string(errno, TEXTDOMAIN), \
	errdetail_string = format_elog_string

#define exit_with_error(exit_label, errmsg_str,...) \
do { \
	pre_format_elog_string(errno, TEXTDOMAIN); \
	(errmsg_str) = format_elog_string(__VA_ARGS__); \
	goto exit_label; \
} while(0)

typedef enum PxTlsCertStatus
{
	PX_TLS_CERT_MISSING,
	PX_TLS_CERT_LOAD_ERROR,
	PX_TLS_CERT_LOADED,
} PxTlsCertStatus;

static struct
{
	bool		reloaded;
	X509	   *cert;
	EVP_PKEY   *private_key;
	PxTlsCertStatus status;
}			px_qc_auth_ctl = {0};

static struct
{
	bool		reloaded;
	X509	   *ca;
	PxTlsCertStatus status;
}			px_qe_auth_ctl = {0};

static X509 *parse_px_qc_cert(const char **errmsg_p);
static int	validate_px_qc_cert(Port *port, X509 *cert, const char **errmsg_p);
static int	validate_px_qc_message(Port *port, X509 *cert, const char **errmsg_p);
static StringInfo recv_message_packet(void);

/* Borrowed from be-secure-openssl.c */
static const char *
SSLerrmessage()
{
	const char *errreason;
	static char errbuf[36];
	unsigned long ecode = ERR_get_error();

	if (ecode == 0)
		return _("no SSL error reported");
	errreason = ERR_reason_error_string(ecode);
	if (errreason != NULL)
		return errreason;

	/*
	 * In OpenSSL 3.0.0 and later, ERR_reason_error_string does not map system
	 * errno values anymore.  (See OpenSSL source code for the explanation.)
	 * We can cover that shortcoming with this bit of code.  Older OpenSSL
	 * versions don't have the ERR_SYSTEM_ERROR macro, but that's okay because
	 * they don't have the shortcoming either.
	 */
#ifdef ERR_SYSTEM_ERROR
	if (ERR_SYSTEM_ERROR(ecode))
		return strerror(ERR_GET_REASON(ecode));
#endif

	/* No choice but to report the numeric ecode */
	snprintf(errbuf, sizeof(errbuf), _("SSL error code %lu"), ecode);
	return errbuf;
}

void
px_need_reload_ca(const char *newval, void *extra)
{
	px_qe_auth_ctl.reloaded = true;
}

void
px_need_reload_cert_pkey(const char *newval, void *extra)
{
	px_qc_auth_ctl.reloaded = true;
}

bool
px_tls_auth_enabled_on_qc(void)
{
	return px_qc_auth_ctl.status != PX_TLS_CERT_MISSING;
}

bool
px_tls_auth_ready_on_qc(void)
{
	return px_qc_auth_ctl.status == PX_TLS_CERT_LOADED;
}

static int
px_cert_pkey_init(int log_lvl)
{
	FILE	   *cert_fp = NULL;
	X509	   *cert = NULL;
	FILE	   *priv_key_fp = NULL;
	EVP_PKEY   *private_key = NULL;
	const char *errmsg_string = NULL;
	const char *errdetail_string = NULL;

	/* Both empty settings is normal and discards TLS authentication from QC */
	if (!px_peer_cert_file[0] && !px_peer_key_file[0])
	{
		px_qc_auth_ctl.status = PX_TLS_CERT_MISSING;
		goto set_result;
	}

	/* Warn if just one setting is set */
	if ((!px_peer_cert_file[0] && px_peer_key_file[0]) ||
		(px_peer_cert_file[0] && !px_peer_key_file[0]))
	{
		px_errmsg("\"polar_px_peer_cert_file\" and \"polar_px_peer_key_file\" options"
				  " requires both to be set");
		goto error;
	}

	/* Check content of cert file */
	if ((cert_fp = fopen(px_peer_cert_file, "rb")) == NULL)
	{
		px_errmsg("invalid value for parameter \"polar_px_peer_cert_file\": \"%s\"", px_peer_cert_file);
		px_errdetail("could not open \"%s\": %m", px_peer_cert_file);
		goto error;
	}
	if ((cert = PEM_read_X509(cert_fp, NULL, NULL, NULL)) == NULL)
	{
		px_errmsg("invalid value for parameter \"polar_px_peer_cert_file\": \"%s\"", px_peer_cert_file);
		px_errdetail("could not read valid content: %s", SSLerrmessage());
		goto error;
	}
	fclose(cert_fp);
	cert_fp = NULL;

	/* Check content of private_key file */
	if ((priv_key_fp = fopen(px_peer_key_file, "rb")) == NULL)
	{
		px_errmsg("invalid value for parameter \"polar_px_peer_key_file\": \"%s\"", px_peer_key_file);
		px_errdetail("could not open \"%s\": %m", px_peer_key_file);
		goto error;
	}
	if ((private_key = PEM_read_PrivateKey(priv_key_fp, NULL, NULL, NULL)) == NULL)
	{
		px_errmsg("invalid value for parameter \"polar_px_peer_key_file\": \"%s\"", px_peer_key_file);
		px_errdetail("could not load valid content: %s", SSLerrmessage());
		goto error;
	}
	fclose(priv_key_fp);
	priv_key_fp = NULL;
	if (EVP_PKEY_id(private_key) != EVP_PKEY_RSA)
	{
		px_errmsg("invalid value for parameter \"polar_px_peer_key_file\": \"%s\"", px_peer_key_file);
		px_errdetail("unsupported algo used for PX peer cert/key, use RSA");
		goto error;
	}

	/* Validate key-pair (private key and pub key of certificate) */
	if (X509_check_private_key(cert, private_key) != 1)
	{
		px_errmsg("invalid \"polar_px_peer_cert_file\" (%s) and \"polar_px_peer_key_file\" (%s) pair",
				  px_peer_cert_file, px_peer_key_file);
		px_errdetail("failed to check private key PX cert: %s", SSLerrmessage());
		goto error;
	}

	px_qc_auth_ctl.status = PX_TLS_CERT_LOADED;

set_result:

	/* Re-assign global TLS certificate and private key objects */
	if (px_qc_auth_ctl.cert != NULL)
		X509_free(px_qc_auth_ctl.cert);
	px_qc_auth_ctl.cert = cert;
	if (px_qc_auth_ctl.private_key != NULL)
		EVP_PKEY_free(px_qc_auth_ctl.private_key);
	px_qc_auth_ctl.private_key = private_key;

	return 0;

error:
	if (private_key)
		EVP_PKEY_free(private_key);
	if (priv_key_fp)
		fclose(priv_key_fp);
	if (cert)
		X509_free(cert);
	if (cert_fp)
		fclose(cert_fp);
	px_qc_auth_ctl.status = PX_TLS_CERT_LOAD_ERROR;

	Assert(errmsg_string);
	ereport(log_lvl,
			(errcode(ERRCODE_CONFIG_FILE_ERROR),
			 errmsg_internal("%s", errmsg_string),
			 errdetail_string ? errdetail_internal("%s", errdetail_string) : 0));

	return -1;
}

static int
px_ca_init(int log_lvl)
{
	struct stat sb;
	FILE	   *ca_fp = NULL;
	X509	   *ca = NULL;
	const char *errdetail_string = NULL;

	/* Empty setting is valid and discards TLS authentication on QE side */
	if (!px_ca_file[0])
	{
		px_qe_auth_ctl.status = PX_TLS_CERT_MISSING;
		goto set_result;
	}

	/* Check validity of CA file */
	if (stat(px_ca_file, &sb) != 0)
		exit_with_error(error, errdetail_string, "could not access \"%s\": %m", px_ca_file);
	if (!S_ISREG(sb.st_mode))
		exit_with_error(error, errdetail_string, "\"%s\" is not a regular file", px_ca_file);

	/* Check content of CA file */
	if ((ca_fp = fopen(px_ca_file, "rb")) == NULL)
		exit_with_error(error, errdetail_string, "could not open \"%s\": %m", px_ca_file);
	if ((ca = PEM_read_X509(ca_fp, NULL, NULL, NULL)) == NULL)
		exit_with_error(error, errdetail_string, "could not read valid content: %s", SSLerrmessage());
	fclose(ca_fp);
	ca_fp = NULL;

	px_qe_auth_ctl.status = PX_TLS_CERT_LOADED;

set_result:

	/* Re-assign global CA object */
	if (px_qe_auth_ctl.ca != NULL)
		X509_free(px_qe_auth_ctl.ca);
	px_qe_auth_ctl.ca = ca;

	return 0;

error:
	if (ca)
		X509_free(ca);
	if (ca_fp)
		fclose(ca_fp);
	px_qe_auth_ctl.status = PX_TLS_CERT_LOAD_ERROR;

	Assert(errdetail_string);
	ereport(log_lvl,
			(errcode(ERRCODE_CONFIG_FILE_ERROR),
			 errmsg("invalid value for parameter \"polar_px_ca_file\": \"%s\"", px_ca_file),
			 errdetail_internal("%s", errdetail_string)));

	return -1;
}

void
px_tls_auth_init(bool isServerStart)
{
	int			log_lvl = (isServerStart) ? FATAL : ((!IsUnderPostmaster) ? LOG : DEBUG2);
	int			rc;

	OSSL_init();

	if (isServerStart || px_qc_auth_ctl.reloaded)
	{
		rc = px_cert_pkey_init(log_lvl);
		if (rc != 0 && !IsUnderPostmaster)
			ereport(WARNING,
					(errmsg("All outgoing PX connections will be forbidden "
							"due to certificate configuration error")));
		px_qc_auth_ctl.reloaded = false;
	}

	if (isServerStart || px_qe_auth_ctl.reloaded)
	{
		rc = px_ca_init(log_lvl);
		if (rc != 0 && !IsUnderPostmaster)
			ereport(WARNING,
					(errmsg("All incoming PX connections will be forbidden due "
							"to certificate configuration error")));
		px_qe_auth_ctl.reloaded = false;
	}
}

const char *
px_get_serial_tls_cert(void)
{
	int			buf_len = 0;
	char	   *bio_buf;
	BIO		   *bio = NULL;
	char	   *result = NULL;
	const char *errdetail_string = NULL;

	Assert(px_tls_auth_ready_on_qc() && px_qc_auth_ctl.cert);


	if ((bio = BIO_new(BIO_s_mem())) == NULL)
		exit_with_error(exit, errdetail_string, "failed to allocate BIO object: %s", SSLerrmessage());
	if (PEM_write_bio_X509(bio, px_qc_auth_ctl.cert) != 1)
		exit_with_error(exit, errdetail_string, "failed to encode certificate: %s", SSLerrmessage());
	buf_len = BIO_get_mem_data(bio, &bio_buf);
	result = palloc(buf_len + 1);
	memcpy(result, bio_buf, buf_len);
	result[buf_len] = '\0';

exit:
	if (bio)
		BIO_free(bio);
	if (errdetail_string)
		ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("error in serialization of TLS certificate for PX connection"),
				 errdetail_internal("%s", errdetail_string)));

	return result;
}

EVP_PKEY *
px_get_tls_private_key(void)
{
	return px_qc_auth_ctl.private_key;
}

/*
 * Functions below used at auth stage (backend startup), postgres backend
 * doesn't set PG_exception_stack yet, so possible errors (from for ex. palloc)
 * with level ERROR would be treated as FATAL - the backend would be
 * killed so additional cleanup for longjmp is not needed.
 */
void
px_auth_qc_tls(Port *port)
{
	X509	   *qc_cert = NULL;
	const char *errmsg_string = NULL;
	const char *errdetail_string = NULL;

	if (px_qe_auth_ctl.status != PX_TLS_CERT_LOADED)
	{
		if (px_qe_auth_ctl.status == PX_TLS_CERT_MISSING)
			px_errdetail("CA certificate missing");
		else if (px_qe_auth_ctl.status == PX_TLS_CERT_LOAD_ERROR)
			px_errdetail("CA certificate could not be loaded on QE");
		else
			Assert(0);
		exit_with_error(error, errmsg_string, "QE is unable to accept PX connections");
	}

	if (px_auth_cert == NULL)
		exit_with_error(error, errmsg_string, "missing PX QC certificate");

	if ((qc_cert = parse_px_qc_cert(&errdetail_string)) == NULL)
		exit_with_error(error, errmsg_string, "failed to parse PX QC certificate");

	if (validate_px_qc_cert(port, qc_cert, &errdetail_string) != 0)
		exit_with_error(error, errmsg_string, "invalid PX QC certificate passed");

	if (validate_px_qc_message(port, qc_cert, &errdetail_string) != 0)
		exit_with_error(error, errmsg_string, "invalid PX QC handshake");

	X509_free(qc_cert);
	return;

error:
	Assert(errmsg_string);
	ereport(FATAL,
			(errcode(ERRCODE_INVALID_AUTHORIZATION_SPECIFICATION),
			 errmsg_internal("%s", errmsg_string),
			 errdetail_string ? errdetail_internal("%s", errdetail_string) : 0));
}

static X509 *
parse_px_qc_cert(const char **errmsg_p)
{
	X509	   *qc_cert = NULL;
	BIO		   *qc_cert_bio = NULL;

	Assert(px_auth_cert);

	if ((qc_cert_bio = BIO_new_mem_buf(px_auth_cert, -1)) == NULL)
		exit_with_error(exit, *errmsg_p, "failed to create in memory BIO: %s", SSLerrmessage());

	if ((qc_cert = PEM_read_bio_X509(qc_cert_bio, NULL, NULL, NULL)) == NULL)
		exit_with_error(exit, *errmsg_p, "failed to read QC cert into BIO: %s", SSLerrmessage());

exit:
	BIO_free(qc_cert_bio);
	pfree(px_auth_cert);
	px_auth_cert = NULL;

	return qc_cert;
}

int
validate_px_qc_cert(Port *port, X509 *qc_cert, const char **errmsg_p)
{
	int			rc = 1;
	X509_STORE *ca_store = NULL;
	X509_STORE_CTX *ctx = NULL;
	X509_NAME  *x509name;
	char	   *peer_cn;
	int			cn_len;
	int			effective_cn_len;

	Assert(px_qe_auth_ctl.ca);

	if ((ca_store = X509_STORE_new()) == NULL)
		exit_with_error(exit, *errmsg_p, "failed to create x509 store: %s", SSLerrmessage());
	if (X509_STORE_add_cert(ca_store, px_qe_auth_ctl.ca) != 1)
		exit_with_error(exit, *errmsg_p, "failed to add CA cert to x509 store: %s", SSLerrmessage());

	if ((ctx = X509_STORE_CTX_new()) == NULL)
		exit_with_error(exit, *errmsg_p, "failed to create x509 store context: %s", SSLerrmessage());
	if (X509_STORE_CTX_init(ctx, ca_store, qc_cert, NULL) != 1)
		exit_with_error(exit, *errmsg_p, "failed to init x509 store context: %s", SSLerrmessage());

	/* Explicitly set the intended purpose for validation (client auth) */
	if (X509_STORE_CTX_set_purpose(ctx, X509_PURPOSE_SSL_CLIENT) != 1)
		exit_with_error(exit, *errmsg_p, "failed to set x509 store context purpose: %s", SSLerrmessage());

	if (X509_verify_cert(ctx) != 1)
		exit_with_error(exit, *errmsg_p, "failed to validate PX QC certificate: %s",
						X509_verify_cert_error_string(X509_STORE_CTX_get_error(ctx)));

	/* Check subject's Common Name value to be equal to internal px user */
	x509name = X509_get_subject_name(qc_cert);
	cn_len = X509_NAME_get_text_by_NID(x509name, NID_commonName, NULL, 0);
	if (cn_len == -1)
		exit_with_error(exit, *errmsg_p, "TLS certificate's common name is empty");
	peer_cn = palloc(cn_len + 1);
	effective_cn_len =
		X509_NAME_get_text_by_NID(x509name, NID_commonName, peer_cn, cn_len + 1);
	peer_cn[cn_len] = '\0';
	if (effective_cn_len != cn_len)
		exit_with_error(exit, *errmsg_p, "invalid length of TLS certificate's common name");
	if (cn_len != strlen(peer_cn))
		exit_with_error(exit, *errmsg_p, "TLS certificate's common name contains embedded null");
	if (strcmp(peer_cn, PX_INTERNAL_USER) != 0)
		exit_with_error(exit, *errmsg_p, "invalid TLS certificate's common name: \"%s\"", peer_cn);

	rc = 0;

exit:
	X509_STORE_CTX_free(ctx);
	X509_STORE_free(ca_store);

	return rc;
}

int
validate_px_qc_message(Port *port, X509 *cert, const char **errmsg_p)
{
#define	MESSAGE_SIZE 64

	int			rc = 1;
	size_t		encrypt_len = 0;
	char		message[MESSAGE_SIZE] = {0};
	char	   *encrypted = NULL;
	StringInfo	recv_message = NULL;
	EVP_PKEY_CTX *ctx = NULL;
	EVP_PKEY   *pubkey;

	if ((pubkey = X509_get0_pubkey(cert)) == NULL)
		exit_with_error(exit, *errmsg_p, "failed extract pubkey from PX QC x509 certifiacte: %s", SSLerrmessage());

	if (EVP_PKEY_id(pubkey) != EVP_PKEY_RSA)
		exit_with_error(exit, *errmsg_p, "unsupported PX QC cert pubkey algo, use RSA");

	if (!pg_strong_random(&message[0], MESSAGE_SIZE))
		exit_with_error(exit, *errmsg_p, "failed to generate random message");

	if ((ctx = EVP_PKEY_CTX_new(pubkey, NULL)) == NULL)
		exit_with_error(exit, *errmsg_p, "failed to create PKEY context: %s", SSLerrmessage());

	if (EVP_PKEY_encrypt_init(ctx) != 1)
		exit_with_error(exit, *errmsg_p, "failed to init PKEY encrypt: %s", SSLerrmessage());

	/* Can't find in the doc that return value must be explicitly 1 */
	if (EVP_PKEY_CTX_set_rsa_padding(ctx, RSA_PKCS1_OAEP_PADDING) <= 0)
		exit_with_error(exit, *errmsg_p, "failed to set PKEY rsa padding: %s", SSLerrmessage());

	if (EVP_PKEY_encrypt(ctx, NULL, &encrypt_len,
						 (const unsigned char *) message, MESSAGE_SIZE) != 1)
		exit_with_error(exit, *errmsg_p, "failed to calculate encrypted size: %s", SSLerrmessage());

	encrypted = palloc(encrypt_len);
	if (EVP_PKEY_encrypt(ctx, (unsigned char *) encrypted, &encrypt_len,
						 (const unsigned char *) message, MESSAGE_SIZE) != 1)
		exit_with_error(exit, *errmsg_p, "failed to encrypt the message: %s", SSLerrmessage());

	sendAuthRequest(port, AUTH_REQ_PX_QC, encrypted, encrypt_len);

	if ((recv_message = recv_message_packet()) == NULL)
		exit_with_error(exit, *errmsg_p, "received decrypted message is empty");

	if (recv_message->len != MESSAGE_SIZE ||
		memcmp(message, recv_message->data, MESSAGE_SIZE) != 0)
		exit_with_error(exit, *errmsg_p, "got different decrypted message");

	rc = 0;

exit:
	if (recv_message)
	{
		pfree(recv_message->data);
		pfree(recv_message);
	}
	if (encrypted)
		pfree(encrypted);
	EVP_PKEY_CTX_free(ctx);

	return rc;
}

/* Main logic is borrowed from recv_password_packet() */
static StringInfo
recv_message_packet(void)
{
	char		mtype;
	StringInfo	buf;
	const int	PG_MAX_AUTH_TOKEN_LENGTH = 65535;	/* Borrowed from auth.c */

	pq_startmsgread();

	/* Expect 'm' message type */
	mtype = pq_getbyte();
	if (mtype != 'm')
	{
		/*
		 * If the client just disconnects without offering a message, don't
		 * make a log entry.  This is legal per protocol spec and in fact
		 * commonly done by psql, so complaining just clutters the log.
		 */
		if (mtype != EOF)
			ereport(WARNING,
					(errcode(ERRCODE_PROTOCOL_VIOLATION),
					 errmsg("expected message ('m') response, got message type %d",
							mtype)));
		return NULL;			/* EOF or bad message type */
	}


	buf = makeStringInfo();
	if (pq_getmessage(buf, PG_MAX_AUTH_TOKEN_LENGTH))	/* receive messages */
	{
		/* EOF - pq_getmessage already logged a suitable message */
		pfree(buf->data);
		pfree(buf);
		return NULL;
	}

	return buf;
}
