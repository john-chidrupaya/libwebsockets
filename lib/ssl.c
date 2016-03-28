/*
 * libwebsockets - small server side websockets and web server implementation
 *
 * Copyright (C) 2010-2014 Andy Green <andy@warmcat.com>
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation:
 *  version 2.1 of the License.
 *
 *  This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public
 *  License along with this library; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston,
 *  MA  02110-1301  USA
 */

#include "private-libwebsockets.h"
#ifndef USE_WOLFSSL
 #include <openssl/err.h>
#endif

#ifdef LWS_HAVE_OPENSSL_ECDH_H
#include <openssl/ecdh.h>
#endif

int openssl_websocket_private_data_index,
    openssl_SSL_CTX_private_data_index;

static int
lws_context_init_ssl_pem_passwd_cb(char * buf, int size, int rwflag, void *userdata)
{
	struct lws_context_creation_info * info =
			(struct lws_context_creation_info *)userdata;

	strncpy(buf, info->ssl_private_key_password, size);
	buf[size - 1] = '\0';

	return strlen(buf);
}

static void lws_ssl_bind_passphrase(SSL_CTX *ssl_ctx, struct lws_context_creation_info *info)
{
	if (!info->ssl_private_key_password)
		return;
	/*
	 * password provided, set ssl callback and user data
	 * for checking password which will be trigered during
	 * SSL_CTX_use_PrivateKey_file function
	 */
	SSL_CTX_set_default_passwd_cb_userdata(ssl_ctx, (void *)info);
	SSL_CTX_set_default_passwd_cb(ssl_ctx, lws_context_init_ssl_pem_passwd_cb);
}

int
lws_context_init_ssl_library(struct lws_context_creation_info *info)
{
#ifdef USE_WOLFSSL
#ifdef USE_OLD_CYASSL
	lwsl_notice(" Compiled with CyaSSL support\n");
#else
	lwsl_notice(" Compiled with wolfSSL support\n");
#endif
#else
	lwsl_notice(" Compiled with OpenSSL support\n");
#endif

	if (!lws_check_opt(info->options, LWS_SERVER_OPTION_DO_SSL_GLOBAL_INIT)) {
		lwsl_notice(" SSL disabled: no LWS_SERVER_OPTION_DO_SSL_GLOBAL_INIT\n");
		return 0;
	}

	/* basic openssl init */

	SSL_library_init();

	OpenSSL_add_all_algorithms();
	SSL_load_error_strings();

	openssl_websocket_private_data_index =
		SSL_get_ex_new_index(0, "lws", NULL, NULL, NULL);

	openssl_SSL_CTX_private_data_index = SSL_CTX_get_ex_new_index(0,
			NULL, NULL, NULL, NULL);

	return 0;
}

#ifndef LWS_NO_SERVER
static int
OpenSSL_verify_callback(int preverify_ok, X509_STORE_CTX *x509_ctx)
{
	SSL *ssl;
	int n;
	struct lws_vhost *vh;
	struct lws wsi;

	ssl = X509_STORE_CTX_get_ex_data(x509_ctx,
		SSL_get_ex_data_X509_STORE_CTX_idx());

	/*
	 * !!! nasty openssl requires the index to come as a library-scope
	 * static
	 */
	vh = SSL_get_ex_data(ssl, openssl_websocket_private_data_index);

	/*
	 * give him a fake wsi with context set, so he can use lws_get_context()
	 * in the callback
	 */
	memset(&wsi, 0, sizeof(wsi));
	wsi.vhost = vh;
	wsi.context = vh->context;

	n = vh->protocols[0].callback(&wsi,
			LWS_CALLBACK_OPENSSL_PERFORM_CLIENT_CERT_VERIFICATION,
					   x509_ctx, ssl, preverify_ok);

	/* convert return code from 0 = OK to 1 = OK */
	return !n;
}

static int
lws_context_ssl_init_ecdh(struct lws_vhost *vhost)
{
#ifdef LWS_SSL_SERVER_WITH_ECDH_CERT
	EC_KEY *EC_key = NULL;
	EVP_PKEY *pkey;
	int KeyType;
	X509 *x;

	if (!lws_check_opt(vhost->context->options, LWS_SERVER_OPTION_SSL_ECDH))
		return 0;

	lwsl_notice(" Using ECDH certificate support\n");

	/* Get X509 certificate from ssl context */
	x = sk_X509_value(vhost->ssl_ctx->extra_certs, 0);
	if (!x) {
		lwsl_err("%s: x is NULL\n", __func__);
		return 1;
	}
	/* Get the public key from certificate */
	pkey = X509_get_pubkey(x);
	if (!pkey) {
		lwsl_err("%s: pkey is NULL\n", __func__);

		return 1;
	}
	/* Get the key type */
	KeyType = EVP_PKEY_type(pkey->type);

	if (EVP_PKEY_EC != KeyType) {
		lwsl_notice("Key type is not EC\n");
		return 0;
	}
	/* Get the key */
	EC_key = EVP_PKEY_get1_EC_KEY(pkey);
	/* Set ECDH parameter */
	if (!EC_key) {
		lwsl_err("%s: ECDH key is NULL \n", __func__);
		return 1;
	}
	SSL_CTX_set_tmp_ecdh(vhost->ssl_ctx, EC_key);
	EC_KEY_free(EC_key);
#endif
	return 0;
}

static int
lws_context_ssl_init_ecdh_curve(struct lws_context_creation_info *info,
				struct lws_vhost *vhost)
{
#ifdef LWS_HAVE_OPENSSL_ECDH_H
	EC_KEY *ecdh;
	int ecdh_nid;
	const char *ecdh_curve = "prime256v1";

	if (info->ecdh_curve)
		ecdh_curve = info->ecdh_curve;

	ecdh_nid = OBJ_sn2nid(ecdh_curve);
	if (NID_undef == ecdh_nid) {
		lwsl_err("SSL: Unknown curve name '%s'", ecdh_curve);
		return 1;
	}

	ecdh = EC_KEY_new_by_curve_name(ecdh_nid);
	if (NULL == ecdh) {
		lwsl_err("SSL: Unable to create curve '%s'", ecdh_curve);
		return 1;
	}
	SSL_CTX_set_tmp_ecdh(vhost->ssl_ctx, ecdh);
	EC_KEY_free(ecdh);

	SSL_CTX_set_options(vhost->ssl_ctx, SSL_OP_SINGLE_ECDH_USE);

	lwsl_notice(" SSL ECDH curve '%s'\n", ecdh_curve);
#else
	lwsl_notice(" OpenSSL doesn't support ECDH\n");
#endif
	return 0;
}

#ifndef OPENSSL_NO_TLSEXT
static int
lws_ssl_server_name_cb(SSL *ssl, int *ad, void *arg)
{
	struct lws_context *context;
	struct lws_vhost *vhost, *vh;
	const char *servername;
	int port;

	if (!ssl)
		return SSL_TLSEXT_ERR_NOACK;

	context = (struct lws_context *)SSL_CTX_get_ex_data(
					SSL_get_SSL_CTX(ssl),
					openssl_SSL_CTX_private_data_index);

	/*
	 * We can only get ssl accepted connections by using a vhost's ssl_ctx
	 * find out which listening one took us and only match vhosts on the
	 * same port.
	 */
	vh = context->vhost_list;
	while (vh) {
		if (vh->ssl_ctx == SSL_get_SSL_CTX(ssl))
			break;
		vh = vh->vhost_next;
	}

	assert(vh); /* we cannot get an ssl without using a vhost ssl_ctx */
	port = vh->listen_port;

	servername = SSL_get_servername(ssl, TLSEXT_NAMETYPE_host_name);

	if (servername) {
		vhost = lws_select_vhost(context, port, servername);
		if (vhost) {
			lwsl_info("SNI: Found: %s\n", servername);
			SSL_set_SSL_CTX(ssl, vhost->ssl_ctx);
			return SSL_TLSEXT_ERR_OK;
		}
		lwsl_err("SNI: Unknown ServerName: %s\n", servername);
	}

	return SSL_TLSEXT_ERR_OK;
}
#endif

LWS_VISIBLE int
lws_context_init_server_ssl(struct lws_context_creation_info *info,
			    struct lws_vhost *vhost)
{
	SSL_METHOD *method;
	struct lws_context *context = vhost->context;
	struct lws wsi;
	int error;
	int n;

	if (!lws_check_opt(info->options, LWS_SERVER_OPTION_DO_SSL_GLOBAL_INIT)) {
		vhost->use_ssl = 0;
		return 0;
	}

	if (info->port != CONTEXT_PORT_NO_LISTEN) {

		vhost->use_ssl = info->ssl_cert_filepath != NULL;

		if (vhost->use_ssl && info->ssl_cipher_list)
			lwsl_notice(" SSL ciphers: '%s'\n", info->ssl_cipher_list);

		if (vhost->use_ssl)
			lwsl_notice(" Using SSL mode\n");
		else
			lwsl_notice(" Using non-SSL mode\n");
	}

	/*
	 * give him a fake wsi with context + vhost set, so he can use
	 * lws_get_context() in the callback
	 */
	memset(&wsi, 0, sizeof(wsi));
	wsi.vhost = vhost;
	wsi.context = vhost->context;

	/*
	 * Firefox insists on SSLv23 not SSLv3
	 * Konq disables SSLv2 by default now, SSLv23 works
	 *
	 * SSLv23_server_method() is the openssl method for "allow all TLS
	 * versions", compared to e.g. TLSv1_2_server_method() which only allows
	 * tlsv1.2. Unwanted versions must be disabled using SSL_CTX_set_options()
	 */

	method = (SSL_METHOD *)SSLv23_server_method();
	if (!method) {
		error = ERR_get_error();
		lwsl_err("problem creating ssl method %lu: %s\n",
			error, ERR_error_string(error,
					      (char *)context->pt[0].serv_buf));
		return 1;
	}
	vhost->ssl_ctx = SSL_CTX_new(method);	/* create context */
	if (!vhost->ssl_ctx) {
		error = ERR_get_error();
		lwsl_err("problem creating ssl context %lu: %s\n",
			error, ERR_error_string(error,
					      (char *)context->pt[0].serv_buf));
		return 1;
	}

	/* associate the lws context with the SSL_CTX */

	SSL_CTX_set_ex_data(vhost->ssl_ctx,
			openssl_SSL_CTX_private_data_index, vhost->context);

	/* Disable SSLv2 and SSLv3 */
	SSL_CTX_set_options(vhost->ssl_ctx, SSL_OP_NO_SSLv2 | SSL_OP_NO_SSLv3);
#ifdef SSL_OP_NO_COMPRESSION
	SSL_CTX_set_options(vhost->ssl_ctx, SSL_OP_NO_COMPRESSION);
#endif
	SSL_CTX_set_options(vhost->ssl_ctx, SSL_OP_SINGLE_DH_USE);
	SSL_CTX_set_options(vhost->ssl_ctx, SSL_OP_CIPHER_SERVER_PREFERENCE);
	if (info->ssl_cipher_list)
		SSL_CTX_set_cipher_list(vhost->ssl_ctx,
						info->ssl_cipher_list);

	/* as a server, are we requiring clients to identify themselves? */

	if (lws_check_opt(info->options, LWS_SERVER_OPTION_REQUIRE_VALID_OPENSSL_CLIENT_CERT)) {
		int verify_options = SSL_VERIFY_PEER;

		if (!lws_check_opt(info->options, LWS_SERVER_OPTION_PEER_CERT_NOT_REQUIRED))
			verify_options |= SSL_VERIFY_FAIL_IF_NO_PEER_CERT;

		SSL_CTX_set_session_id_context(vhost->ssl_ctx,
				(unsigned char *)context, sizeof(void *));

		/* absolutely require the client cert */

		SSL_CTX_set_verify(vhost->ssl_ctx,
		       verify_options, OpenSSL_verify_callback);
	}

#ifndef OPENSSL_NO_TLSEXT
	SSL_CTX_set_tlsext_servername_callback(vhost->ssl_ctx,
					       lws_ssl_server_name_cb);
#endif

	/*
	 * give user code a chance to load certs into the server
	 * allowing it to verify incoming client certs
	 */

	if (info->ssl_ca_filepath &&
	    !SSL_CTX_load_verify_locations(vhost->ssl_ctx,
					   info->ssl_ca_filepath, NULL)) {
		lwsl_err("%s: SSL_CTX_load_verify_locations unhappy\n", __func__);
	}

	if (vhost->use_ssl) {
		if (lws_context_ssl_init_ecdh_curve(info, vhost))
			return -1;

		vhost->protocols[0].callback(&wsi,
			LWS_CALLBACK_OPENSSL_LOAD_EXTRA_SERVER_VERIFY_CERTS,
			vhost->ssl_ctx, NULL, 0);
	}

	if (lws_check_opt(info->options, LWS_SERVER_OPTION_ALLOW_NON_SSL_ON_SSL_PORT))
		/* Normally SSL listener rejects non-ssl, optionally allow */
		vhost->allow_non_ssl_on_ssl_port = 1;

	if (vhost->use_ssl) {
		/* openssl init for server sockets */

		/* set the local certificate from CertFile */
		n = SSL_CTX_use_certificate_chain_file(vhost->ssl_ctx,
					info->ssl_cert_filepath);
		if (n != 1) {
			error = ERR_get_error();
			lwsl_err("problem getting cert '%s' %lu: %s\n",
				info->ssl_cert_filepath,
				error,
				ERR_error_string(error,
					      (char *)context->pt[0].serv_buf));
			return 1;
		}
		lws_ssl_bind_passphrase(vhost->ssl_ctx, info);

		if (info->ssl_private_key_filepath != NULL) {
			/* set the private key from KeyFile */
			if (SSL_CTX_use_PrivateKey_file(vhost->ssl_ctx,
				     info->ssl_private_key_filepath,
						       SSL_FILETYPE_PEM) != 1) {
				error = ERR_get_error();
				lwsl_err("ssl problem getting key '%s' %lu: %s\n",
					 info->ssl_private_key_filepath, error,
					 ERR_error_string(error,
					      (char *)context->pt[0].serv_buf));
				return 1;
			}
		} else
			if (vhost->protocols[0].callback(&wsi,
				LWS_CALLBACK_OPENSSL_CONTEXT_REQUIRES_PRIVATE_KEY,
				vhost->ssl_ctx, NULL, 0)) {
				lwsl_err("ssl private key not set\n");

				return 1;
			}

		/* verify private key */
		if (!SSL_CTX_check_private_key(vhost->ssl_ctx)) {
			lwsl_err("Private SSL key doesn't match cert\n");
			return 1;
		}

		if (lws_context_ssl_init_ecdh(vhost))
			return 1;

		/*
		 * SSL is happy and has a cert it's content with
		 * If we're supporting HTTP2, initialize that
		 */

		lws_context_init_http2_ssl(context);
	}

	return 0;
}
#endif

LWS_VISIBLE void
lws_ssl_destroy(struct lws_vhost *vhost)
{
	if (!lws_check_opt(vhost->context->options, LWS_SERVER_OPTION_DO_SSL_GLOBAL_INIT))
		return;

	if (vhost->ssl_ctx)
		SSL_CTX_free(vhost->ssl_ctx);
	if (!vhost->user_supplied_ssl_ctx && vhost->ssl_client_ctx)
		SSL_CTX_free(vhost->ssl_client_ctx);

#if (OPENSSL_VERSION_NUMBER < 0x01000000) || defined(USE_WOLFSSL)
	ERR_remove_state(0);
#else
	ERR_remove_thread_state(NULL);
#endif
	ERR_free_strings();
	EVP_cleanup();
	CRYPTO_cleanup_all_ex_data();
}

LWS_VISIBLE void
lws_decode_ssl_error(void)
{
	char buf[256];
	u_long err;

	while ((err = ERR_get_error()) != 0) {
		ERR_error_string_n(err, buf, sizeof(buf));
		lwsl_err("*** %lu %s\n", err, buf);
	}
}

#ifndef LWS_NO_CLIENT

int
lws_ssl_client_bio_create(struct lws *wsi)
{
	struct lws_context *context = wsi->context;
#if defined(CYASSL_SNI_HOST_NAME) || defined(WOLFSSL_SNI_HOST_NAME) || defined(SSL_CTRL_SET_TLSEXT_HOSTNAME)
	const char *hostname = lws_hdr_simple_ptr(wsi, _WSI_TOKEN_CLIENT_HOST);
#endif

	wsi->ssl = SSL_new(wsi->vhost->ssl_client_ctx);
#ifndef USE_WOLFSSL
	SSL_set_mode(wsi->ssl,  SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);
#endif
	/*
	 * use server name indication (SNI), if supported,
	 * when establishing connection
	 */
#ifdef USE_WOLFSSL
#ifdef USE_OLD_CYASSL
#ifdef CYASSL_SNI_HOST_NAME
	CyaSSL_UseSNI(wsi->ssl, CYASSL_SNI_HOST_NAME, hostname, strlen(hostname));
#endif
#else
#ifdef WOLFSSL_SNI_HOST_NAME
	wolfSSL_UseSNI(wsi->ssl, WOLFSSL_SNI_HOST_NAME, hostname, strlen(hostname));
#endif
#endif
#else
#ifdef SSL_CTRL_SET_TLSEXT_HOSTNAME
	SSL_set_tlsext_host_name(wsi->ssl, hostname);
#endif
#endif

#ifdef USE_WOLFSSL
	/*
	 * wolfSSL/CyaSSL does certificate verification differently
	 * from OpenSSL.
	 * If we should ignore the certificate, we need to set
	 * this before SSL_new and SSL_connect is called.
	 * Otherwise the connect will simply fail with error code -155
	 */
#ifdef USE_OLD_CYASSL
	if (wsi->use_ssl == 2)
		CyaSSL_set_verify(wsi->ssl, SSL_VERIFY_NONE, NULL);
#else
	if (wsi->use_ssl == 2)
		wolfSSL_set_verify(wsi->ssl, SSL_VERIFY_NONE, NULL);
#endif
#endif /* USE_WOLFSSL */

	wsi->client_bio = BIO_new_socket(wsi->sock, BIO_NOCLOSE);
	SSL_set_bio(wsi->ssl, wsi->client_bio, wsi->client_bio);

#ifdef USE_WOLFSSL
#ifdef USE_OLD_CYASSL
	CyaSSL_set_using_nonblock(wsi->ssl, 1);
#else
	wolfSSL_set_using_nonblock(wsi->ssl, 1);
#endif
#else
	BIO_set_nbio(wsi->client_bio, 1); /* nonblocking */
#endif

	SSL_set_ex_data(wsi->ssl, openssl_websocket_private_data_index,
			context);

	return 0;
}

int
lws_ssl_client_connect1(struct lws *wsi)
{
	struct lws_context *context = wsi->context;
	struct lws_context_per_thread *pt = &context->pt[(int)wsi->tsi];
	char *p = (char *)&pt->serv_buf[0];
	char *sb = p;
	int n;

	lws_latency_pre(context, wsi);
	n = SSL_connect(wsi->ssl);
	lws_latency(context, wsi,
	  "SSL_connect LWSCM_WSCL_ISSUE_HANDSHAKE", n, n > 0);

	if (n < 0) {
		n = SSL_get_error(wsi->ssl, n);

		if (n == SSL_ERROR_WANT_READ)
			goto some_wait;

		if (n == SSL_ERROR_WANT_WRITE) {
			/*
			 * wants us to retry connect due to
			 * state of the underlying ssl layer...
			 * but since it may be stalled on
			 * blocked write, no incoming data may
			 * arrive to trigger the retry.
			 * Force (possibly many times if the SSL
			 * state persists in returning the
			 * condition code, but other sockets
			 * are getting serviced inbetweentimes)
			 * us to get called back when writable.
			 */
			lwsl_info("%s: WANT_WRITE... retrying\n", __func__);
			lws_callback_on_writable(wsi);
some_wait:
			wsi->mode = LWSCM_WSCL_WAITING_SSL;

			return 0; /* no error */
		}
		n = -1;
	}

	if (n <= 0) {
		/*
		 * retry if new data comes until we
		 * run into the connection timeout or win
		 */
		n = ERR_get_error();
		if (n != SSL_ERROR_NONE) {
			lwsl_err("SSL connect error %lu: %s\n",
				n, ERR_error_string(n, sb));
			return 0;
		}
	}

	return 1;
}

int
lws_ssl_client_connect2(struct lws *wsi)
{
	struct lws_context *context = wsi->context;
	struct lws_context_per_thread *pt = &wsi->context->pt[(int)wsi->tsi];
	char *p = (char *)&pt->serv_buf[0];
	char *sb = p;
	int n;

	if (wsi->mode == LWSCM_WSCL_WAITING_SSL) {
		lws_latency_pre(context, wsi);
		n = SSL_connect(wsi->ssl);
		lws_latency(context, wsi,
			    "SSL_connect LWSCM_WSCL_WAITING_SSL", n, n > 0);

		if (n < 0) {
			n = SSL_get_error(wsi->ssl, n);

			if (n == SSL_ERROR_WANT_READ) {
				wsi->mode = LWSCM_WSCL_WAITING_SSL;

				return 0; /* no error */
			}

			if (n == SSL_ERROR_WANT_WRITE) {
				/*
				 * wants us to retry connect due to
				 * state of the underlying ssl layer...
				 * but since it may be stalled on
				 * blocked write, no incoming data may
				 * arrive to trigger the retry.
				 * Force (possibly many times if the SSL
				 * state persists in returning the
				 * condition code, but other sockets
				 * are getting serviced inbetweentimes)
				 * us to get called back when writable.
				 */
				lwsl_info("SSL_connect WANT_WRITE... retrying\n");
				lws_callback_on_writable(wsi);

				wsi->mode = LWSCM_WSCL_WAITING_SSL;

				return 0; /* no error */
			}
			n = -1;
		}

		if (n <= 0) {
			/*
			 * retry if new data comes until we
			 * run into the connection timeout or win
			 */
			n = ERR_get_error();
			if (n != SSL_ERROR_NONE) {
				lwsl_err("SSL connect error %lu: %s\n",
					 n, ERR_error_string(n, sb));
				return 0;
			}
		}
	}

#ifndef USE_WOLFSSL
	/*
	 * See comment above about wolfSSL certificate
	 * verification
	 */
	lws_latency_pre(context, wsi);
	n = SSL_get_verify_result(wsi->ssl);
	lws_latency(context, wsi,
		"SSL_get_verify_result LWS_CONNMODE..HANDSHAKE",
						      n, n > 0);

	if (n != X509_V_OK) {
		if ((n == X509_V_ERR_DEPTH_ZERO_SELF_SIGNED_CERT ||
		     n == X509_V_ERR_SELF_SIGNED_CERT_IN_CHAIN) && wsi->use_ssl == 2) {
			lwsl_notice("accepting self-signed certificate\n");
		} else {
			lwsl_err("server's cert didn't look good, X509_V_ERR = %d: %s\n",
				 n, ERR_error_string(n, sb));
			lws_close_free_wsi(wsi,
				LWS_CLOSE_STATUS_NOSTATUS);
			return 0;
		}
	}
#endif /* USE_WOLFSSL */

	return 1;
}


int lws_context_init_client_ssl(struct lws_context_creation_info *info,
			        struct lws_vhost *vhost)
{
	int error;
	int n;
	SSL_METHOD *method;
	struct lws wsi;

	if (!lws_check_opt(info->options, LWS_SERVER_OPTION_DO_SSL_GLOBAL_INIT))
		return 0;

	if (info->provided_client_ssl_ctx) {
		/* use the provided OpenSSL context if given one */
		vhost->ssl_client_ctx = info->provided_client_ssl_ctx;
		/* nothing for lib to delete */
		vhost->user_supplied_ssl_ctx = 1;
		return 0;
	}

	if (info->port != CONTEXT_PORT_NO_LISTEN)
		return 0;

	/* basic openssl init */

	SSL_library_init();

	OpenSSL_add_all_algorithms();
	SSL_load_error_strings();

	method = (SSL_METHOD *)SSLv23_client_method();
	if (!method) {
		error = ERR_get_error();
		lwsl_err("problem creating ssl method %lu: %s\n",
			error, ERR_error_string(error,
				      (char *)vhost->context->pt[0].serv_buf));
		return 1;
	}
	/* create context */
	vhost->ssl_client_ctx = SSL_CTX_new(method);
	if (!vhost->ssl_client_ctx) {
		error = ERR_get_error();
		lwsl_err("problem creating ssl context %lu: %s\n",
			error, ERR_error_string(error,
				      (char *)vhost->context->pt[0].serv_buf));
		return 1;
	}

#ifdef SSL_OP_NO_COMPRESSION
	SSL_CTX_set_options(vhost->ssl_client_ctx,
						 SSL_OP_NO_COMPRESSION);
#endif
	SSL_CTX_set_options(vhost->ssl_client_ctx,
				       SSL_OP_CIPHER_SERVER_PREFERENCE);
	if (info->ssl_cipher_list)
		SSL_CTX_set_cipher_list(vhost->ssl_client_ctx,
						info->ssl_cipher_list);

#ifdef LWS_SSL_CLIENT_USE_OS_CA_CERTS
	if (!lws_check_opt(info->options, LWS_SERVER_OPTION_DISABLE_OS_CA_CERTS))
		/* loads OS default CA certs */
		SSL_CTX_set_default_verify_paths(vhost->ssl_client_ctx);
#endif

	/* openssl init for cert verification (for client sockets) */
	if (!info->ssl_ca_filepath) {
		if (!SSL_CTX_load_verify_locations(
			vhost->ssl_client_ctx, NULL,
					     LWS_OPENSSL_CLIENT_CERTS))
			lwsl_err(
			    "Unable to load SSL Client certs from %s "
			    "(set by --with-client-cert-dir= "
			    "in configure) --  client ssl isn't "
			    "going to work", LWS_OPENSSL_CLIENT_CERTS);
	} else
		if (!SSL_CTX_load_verify_locations(
			vhost->ssl_client_ctx, info->ssl_ca_filepath,
							  NULL))
			lwsl_err(
				"Unable to load SSL Client certs "
				"file from %s -- client ssl isn't "
				"going to work", info->ssl_ca_filepath);
		else
			lwsl_info("loaded ssl_ca_filepath\n");

	/*
	 * callback allowing user code to load extra verification certs
	 * helping the client to verify server identity
	 */

	/* support for client-side certificate authentication */
	if (info->ssl_cert_filepath) {
		n = SSL_CTX_use_certificate_chain_file(vhost->ssl_client_ctx,
						       info->ssl_cert_filepath);
		if (n != 1) {
			lwsl_err("problem getting cert '%s' %lu: %s\n",
				info->ssl_cert_filepath,
				ERR_get_error(),
				ERR_error_string(ERR_get_error(),
				(char *)vhost->context->pt[0].serv_buf));
			return 1;
		}
	}
	if (info->ssl_private_key_filepath) {
		lws_ssl_bind_passphrase(vhost->ssl_client_ctx, info);
		/* set the private key from KeyFile */
		if (SSL_CTX_use_PrivateKey_file(vhost->ssl_client_ctx,
		    info->ssl_private_key_filepath, SSL_FILETYPE_PEM) != 1) {
			lwsl_err("use_PrivateKey_file '%s' %lu: %s\n",
				info->ssl_private_key_filepath,
				ERR_get_error(),
				ERR_error_string(ERR_get_error(),
				      (char *)vhost->context->pt[0].serv_buf));
			return 1;
		}

		/* verify private key */
		if (!SSL_CTX_check_private_key(vhost->ssl_client_ctx)) {
			lwsl_err("Private SSL key doesn't match cert\n");
			return 1;
		}
	}

	/*
	 * give him a fake wsi with context set, so he can use
	 * lws_get_context() in the callback
	 */
	memset(&wsi, 0, sizeof(wsi));
	wsi.vhost = vhost;
	wsi.context = vhost->context;

	vhost->protocols[0].callback(&wsi,
			LWS_CALLBACK_OPENSSL_LOAD_EXTRA_CLIENT_VERIFY_CERTS,
				       vhost->ssl_client_ctx, NULL, 0);

	return 0;
}
#endif

LWS_VISIBLE void
lws_ssl_remove_wsi_from_buffered_list(struct lws *wsi)
{
	struct lws_context *context = wsi->context;
	struct lws_context_per_thread *pt = &context->pt[(int)wsi->tsi];

	if (!wsi->pending_read_list_prev &&
	    !wsi->pending_read_list_next &&
	    pt->pending_read_list != wsi)
		/* we are not on the list */
		return;

	/* point previous guy's next to our next */
	if (!wsi->pending_read_list_prev)
		pt->pending_read_list = wsi->pending_read_list_next;
	else
		wsi->pending_read_list_prev->pending_read_list_next =
			wsi->pending_read_list_next;

	/* point next guy's previous to our previous */
	if (wsi->pending_read_list_next)
		wsi->pending_read_list_next->pending_read_list_prev =
			wsi->pending_read_list_prev;

	wsi->pending_read_list_prev = NULL;
	wsi->pending_read_list_next = NULL;
}

LWS_VISIBLE int
lws_ssl_capable_read(struct lws *wsi, unsigned char *buf, int len)
{
	struct lws_context *context = wsi->context;
	struct lws_context_per_thread *pt = &context->pt[(int)wsi->tsi];
	int n;

	if (!wsi->ssl)
		return lws_ssl_capable_read_no_ssl(wsi, buf, len);

	n = SSL_read(wsi->ssl, buf, len);
	/* manpage: returning 0 means connection shut down */
	if (!n)
		return LWS_SSL_CAPABLE_ERROR;

	if (n < 0) {
		n = SSL_get_error(wsi->ssl, n);
		if (n ==  SSL_ERROR_WANT_READ || n ==  SSL_ERROR_WANT_WRITE)
			return LWS_SSL_CAPABLE_MORE_SERVICE;

		return LWS_SSL_CAPABLE_ERROR;
	}

	/*
	 * if it was our buffer that limited what we read,
	 * check if SSL has additional data pending inside SSL buffers.
	 *
	 * Because these won't signal at the network layer with POLLIN
	 * and if we don't realize, this data will sit there forever
	 */
	if (n != len)
		goto bail;
	if (!wsi->ssl)
		goto bail;
	if (!SSL_pending(wsi->ssl))
		goto bail;
	if (wsi->pending_read_list_next)
		return n;
	if (wsi->pending_read_list_prev)
		return n;
	if (pt->pending_read_list == wsi)
		return n;

	/* add us to the linked list of guys with pending ssl */
	if (pt->pending_read_list)
		pt->pending_read_list->pending_read_list_prev = wsi;

	wsi->pending_read_list_next = pt->pending_read_list;
	wsi->pending_read_list_prev = NULL;
	pt->pending_read_list = wsi;

	return n;
bail:
	lws_ssl_remove_wsi_from_buffered_list(wsi);

	return n;
}

LWS_VISIBLE int
lws_ssl_pending(struct lws *wsi)
{
	if (!wsi->ssl)
		return 0;

	return SSL_pending(wsi->ssl);
}

LWS_VISIBLE int
lws_ssl_capable_write(struct lws *wsi, unsigned char *buf, int len)
{
	int n;

	if (!wsi->ssl)
		return lws_ssl_capable_write_no_ssl(wsi, buf, len);

	n = SSL_write(wsi->ssl, buf, len);
	if (n > 0)
		return n;

	n = SSL_get_error(wsi->ssl, n);
	if (n == SSL_ERROR_WANT_READ || n == SSL_ERROR_WANT_WRITE) {
		if (n == SSL_ERROR_WANT_WRITE)
			lws_set_blocking_send(wsi);
		return LWS_SSL_CAPABLE_MORE_SERVICE;
	}

	return LWS_SSL_CAPABLE_ERROR;
}

LWS_VISIBLE int
lws_ssl_close(struct lws *wsi)
{
	int n;

	if (!wsi->ssl)
		return 0; /* not handled */

	n = SSL_get_fd(wsi->ssl);
	SSL_shutdown(wsi->ssl);
	compatible_close(n);
	SSL_free(wsi->ssl);
	wsi->ssl = NULL;

	return 1; /* handled */
}

/* leave all wsi close processing to the caller */

LWS_VISIBLE int
lws_server_socket_service_ssl(struct lws *wsi, lws_sockfd_type accept_fd)
{
	struct lws_context *context = wsi->context;
	struct lws_context_per_thread *pt = &context->pt[(int)wsi->tsi];
	int n, m;
#ifndef USE_WOLFSSL
	BIO *bio;
#endif

	if (!LWS_SSL_ENABLED(wsi->vhost))
		return 0;

	switch (wsi->mode) {
	case LWSCM_SSL_INIT:

		wsi->ssl = SSL_new(wsi->vhost->ssl_ctx);
		if (wsi->ssl == NULL) {
			lwsl_err("SSL_new failed: %s\n",
				 ERR_error_string(SSL_get_error(wsi->ssl, 0), NULL));
			lws_decode_ssl_error();
			if (accept_fd != LWS_SOCK_INVALID)
				compatible_close(accept_fd);
			goto fail;
		}

		SSL_set_ex_data(wsi->ssl,
			openssl_websocket_private_data_index, context);

		SSL_set_fd(wsi->ssl, accept_fd);

#ifdef USE_WOLFSSL
#ifdef USE_OLD_CYASSL
		CyaSSL_set_using_nonblock(wsi->ssl, 1);
#else
		wolfSSL_set_using_nonblock(wsi->ssl, 1);
#endif
#else
		SSL_set_mode(wsi->ssl, SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);
		bio = SSL_get_rbio(wsi->ssl);
		if (bio)
			BIO_set_nbio(bio, 1); /* nonblocking */
		else
			lwsl_notice("NULL rbio\n");
		bio = SSL_get_wbio(wsi->ssl);
		if (bio)
			BIO_set_nbio(bio, 1); /* nonblocking */
		else
			lwsl_notice("NULL rbio\n");
#endif

		/*
		 * we are not accepted yet, but we need to enter ourselves
		 * as a live connection.  That way we can retry when more
		 * pieces come if we're not sorted yet
		 */

		wsi->mode = LWSCM_SSL_ACK_PENDING;
		if (insert_wsi_socket_into_fds(context, wsi))
			goto fail;

		lws_set_timeout(wsi, PENDING_TIMEOUT_SSL_ACCEPT,
				context->timeout_secs);

		lwsl_info("inserted SSL accept into fds, trying SSL_accept\n");

		/* fallthru */

	case LWSCM_SSL_ACK_PENDING:

		if (lws_change_pollfd(wsi, LWS_POLLOUT, 0))
			goto fail;

		lws_latency_pre(context, wsi);

		n = recv(wsi->sock, (char *)pt->serv_buf, LWS_MAX_SOCKET_IO_BUF,
			 MSG_PEEK);

		/*
		 * optionally allow non-SSL connect on SSL listening socket
		 * This is disabled by default, if enabled it goes around any
		 * SSL-level access control (eg, client-side certs) so leave
		 * it disabled unless you know it's not a problem for you
		 */

		if (wsi->vhost->allow_non_ssl_on_ssl_port) {
			if (n >= 1 && pt->serv_buf[0] >= ' ') {
				/*
				* TLS content-type for Handshake is 0x16, and
				* for ChangeCipherSpec Record, it's 0x14
				*
				* A non-ssl session will start with the HTTP
				* method in ASCII.  If we see it's not a legit
				* SSL handshake kill the SSL for this
				* connection and try to handle as a HTTP
				* connection upgrade directly.
				*/
				wsi->use_ssl = 0;
				SSL_shutdown(wsi->ssl);
				SSL_free(wsi->ssl);
				wsi->ssl = NULL;
				if (lws_check_opt(context->options,
				    LWS_SERVER_OPTION_REDIRECT_HTTP_TO_HTTPS))
					wsi->redirect_to_https = 1;
				goto accepted;
			}
			if (!n) /*
				 * connection is gone, or nothing to read
				 * if it's gone, we will timeout on
				 * PENDING_TIMEOUT_SSL_ACCEPT
				 */
				break;
			if (n < 0 && (LWS_ERRNO == LWS_EAGAIN ||
				      LWS_ERRNO == LWS_EWOULDBLOCK)) {
				/*
				 * well, we get no way to know ssl or not
				 * so go around again waiting for something
				 * to come and give us a hint, or timeout the
				 * connection.
				 */
				m = SSL_ERROR_WANT_READ;
				goto go_again;
			}
		}

		/* normal SSL connection processing path */

		n = SSL_accept(wsi->ssl);
		lws_latency(context, wsi,
			"SSL_accept LWSCM_SSL_ACK_PENDING\n", n, n == 1);

		if (n == 1)
			goto accepted;

		m = SSL_get_error(wsi->ssl, n);
		lwsl_debug("SSL_accept failed %d / %s\n",
			   m, ERR_error_string(m, NULL));
go_again:
		if (m == SSL_ERROR_WANT_READ) {
			if (lws_change_pollfd(wsi, 0, LWS_POLLIN))
				goto fail;

			lwsl_info("SSL_ERROR_WANT_READ\n");
			break;
		}
		if (m == SSL_ERROR_WANT_WRITE) {
			if (lws_change_pollfd(wsi, 0, LWS_POLLOUT))
				goto fail;

			break;
		}
		lwsl_debug("SSL_accept failed skt %u: %s\n",
			   wsi->sock, ERR_error_string(m, NULL));
		goto fail;

accepted:
		/* OK, we are accepted... give him some time to negotiate */
		lws_set_timeout(wsi, PENDING_TIMEOUT_ESTABLISH_WITH_SERVER,
				context->timeout_secs);

		wsi->mode = LWSCM_HTTP_SERVING;

		lws_http2_configure_if_upgraded(wsi);

		lwsl_debug("accepted new SSL conn\n");
		break;
	}

	return 0;

fail:
	return 1;
}

void
lws_ssl_SSL_CTX_destroy(struct lws_vhost *vhost)
{
	if (vhost->ssl_ctx)
		SSL_CTX_free(vhost->ssl_ctx);
	if (!vhost->user_supplied_ssl_ctx && vhost->ssl_client_ctx)
		SSL_CTX_free(vhost->ssl_client_ctx);
}

void
lws_ssl_context_destroy(struct lws_context *context)
{
#if (OPENSSL_VERSION_NUMBER < 0x01000000) || defined(USE_WOLFSSL)
	ERR_remove_state(0);
#else
	ERR_remove_thread_state(NULL);
#endif
	ERR_free_strings();
	EVP_cleanup();
	CRYPTO_cleanup_all_ex_data();
}
