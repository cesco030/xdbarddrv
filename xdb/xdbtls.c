/*
 * xdbtls.c - TLS client for the XDB backends.
 *
 * IBM i: GSKit with the *SYSTEM certificate store (DCM).
 * Linux (XDB_LINUX_EMU): OpenSSL, so the callers can be tested there.
 *
 * xtls_start    TLS on a connected, blocking socket (MySQL).
 * xtls_start_io the same with options, and (OpenSSL only) with the transport
 *               supplied by the caller: SQL Server before TDS 8.0 carries
 *               the handshake inside its own packets, which GSKit on IBM i
 *               cannot do.
 * verify = 0 encrypts without checking the server certificate; verify = 1
 * checks the certificate chain (not the host name).
 */
#ifdef __ILEC400__
#ifndef __cplusplus__strings__
#define __cplusplus__strings__ 1
#endif
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include "xdb_be.h"

extern int	ile_conv(int from_ccsid, int to_ccsid, const char *in, size_t inlen,
					 char *out, size_t outsize);

#ifdef __ILEC400__
#include <gskssl.h>

struct xtls
{
	gsk_handle	env;
	gsk_handle	soc;
	int			fd;
};

static void
gsk_msg(const char *what, int rc, char *err, int errsize)
{
	const char *m = gsk_strerror(rc);
	char		u[256];
	int			n = 0;

	if (m)
		n = ile_conv(0, 1208, m, strlen(m), u, sizeof u - 1);
	u[n > 0 ? n : 0] = '\0';
	snprintf(err, errsize, "TLS %s failed (GSKit %d): %s", what, rc, u);
}

xtls *
xtls_start_io(int fd, const xtls_io *io, const char *host, int verify, int max12,
			  const char *alpn, char *err, int errsize)
{
	xtls	   *t = calloc(1, sizeof(xtls));
	char		store[32];
	char		hn[256];
	int			rc,
				n;

	if (t == NULL)
		return NULL;
	if (io)
	{
		/*
		 * GSKit on IBM i has no I/O callbacks (GSK_IO_CALLBACK is documented
		 * as not supported, and answers 412): it can only run on the socket.
		 */
		snprintf(err, errsize, "TLS over a caller-supplied transport is not available with GSKit on IBM i");
		free(t);
		return NULL;
	}
	t->fd = fd;
	rc = gsk_environment_open(&t->env);
	if (rc != GSK_OK)
	{
		gsk_msg("environment open", rc, err, errsize);
		free(t);
		return NULL;
	}
	n = ile_conv(1208, 0, "*SYSTEM", 7, store, sizeof store - 1);
	store[n > 0 ? n : 0] = '\0';
	gsk_attribute_set_buffer(t->env, GSK_KEYRING_FILE, store, 0);
	gsk_attribute_set_enum(t->env, GSK_SESSION_TYPE, GSK_CLIENT_SESSION);
	if (!verify)
		gsk_attribute_set_enum(t->env, GSK_SERVER_AUTH_TYPE, GSK_SERVER_AUTH_PASSTHRU);
	if (max12)
		gsk_attribute_set_enum(t->env, GSK_PROTOCOL_TLSV13, GSK_FALSE);
	rc = gsk_environment_init(t->env);
	if (rc == GSK_OK)
		rc = gsk_secure_soc_open(t->env, &t->soc);
	if (rc == GSK_OK)
		rc = gsk_attribute_set_numeric_value(t->soc, GSK_FD, fd);
	/* server name indication, for names only */
	if (rc == GSK_OK && host && host[0] && strchr(host, ':') == NULL &&
		strspn(host, "0123456789.") != strlen(host) &&
		(n = ile_conv(1208, 0, host, strlen(host), hn, sizeof hn - 1)) > 0)
	{
		hn[n] = '\0';
		gsk_attribute_set_buffer(t->soc, GSK_SSL_EXTN_SERVERNAME_REQUEST, hn, 0);
	}
	if (rc == GSK_OK && alpn && alpn[0] &&
		(n = ile_conv(1208, 0, alpn, strlen(alpn), hn, sizeof hn - 1)) > 0)
	{
		hn[n] = '\0';
		rc = gsk_attribute_set_buffer(t->soc, GSK_SSL_EXTN_ALPN, hn, 0);
		if (rc != GSK_OK)
		{
			gsk_msg("protocol name (ALPN) setup", rc, err, errsize);
			xtls_end(t);
			return NULL;
		}
	}
	if (rc == GSK_OK)
		rc = gsk_secure_soc_init(t->soc);
	if (rc != GSK_OK)
	{
		gsk_msg("handshake", rc, err, errsize);
		xtls_end(t);
		return NULL;
	}
	return t;
}

void
xtls_info(xtls *t, char *out, int outsize)
{
	GSK_ENUM_VALUE used = 0;
	const char *p = NULL;
	const char *level = "TLS";
	char		spec[80];
	int			n = 0,
				k = 0;

	out[0] = '\0';
	if (t == NULL || t->soc == NULL)
		return;
	if (gsk_attribute_get_enum(t->soc, GSK_PROTOCOL_USED, &used) == GSK_OK)
		level = used == GSK_PROTOCOL_USED_TLSV13 ? "TLS 1.3" : used == GSK_PROTOCOL_USED_TLSV12 ? "TLS 1.2" :
			used == GSK_PROTOCOL_USED_TLSV11 ? "TLS 1.1" : used == GSK_PROTOCOL_USED_TLSV1 ? "TLS 1.0" : "TLS";
	spec[0] = '\0';
	if (gsk_attribute_get_buffer(t->soc, GSK_CONNECT_CIPHER_SPEC_EX, &p, &n) == GSK_OK && p && n > 0)
	{
		k = ile_conv(0, 1208, p, n > 64 ? 64 : n, spec, sizeof spec - 1);
		spec[k > 0 ? k : 0] = '\0';
	}
	snprintf(out, outsize, "%s%s%s", level, spec[0] ? ", cipher " : "", spec);
}

int
xtls_read(xtls *t, void *buf, int len)
{
	int			n = 0;

	if (gsk_secure_soc_read(t->soc, (char *) buf, len, &n) != GSK_OK)
		return -1;
	return n;
}

int
xtls_write(xtls *t, const void *buf, int len)
{
	int			n = 0;

	if (gsk_secure_soc_write(t->soc, (char *) buf, len, &n) != GSK_OK)
		return -1;
	return n;
}

void
xtls_end(xtls *t)
{
	if (t == NULL)
		return;
	if (t->soc)
		gsk_secure_soc_close(&t->soc);
	if (t->env)
		gsk_environment_close(&t->env);
	free(t);
}

#else							/* OpenSSL */

#include <openssl/ssl.h>
#include <openssl/err.h>

struct xtls
{
	SSL_CTX    *ctx;
	SSL		   *ssl;
	BIO_METHOD *meth;
	xtls_io		io;
};

static int
bio_wr(BIO *b, const char *buf, int len)
{
	xtls	   *t = BIO_get_data(b);

	return t->io.wr(t->io.ctx, buf, len);
}

static int
bio_rd(BIO *b, char *buf, int len)
{
	xtls	   *t = BIO_get_data(b);

	return t->io.rd(t->io.ctx, buf, len);
}

static long
bio_ctrl(BIO *b, int cmd, long num, void *ptr)
{
	return cmd == BIO_CTRL_FLUSH ? 1 : 0;
}

xtls *
xtls_start_io(int fd, const xtls_io *io, const char *host, int verify, int max12,
			  const char *alpn, char *err, int errsize)
{
	xtls	   *t = calloc(1, sizeof(xtls));

	if (t == NULL)
		return NULL;
	t->ctx = SSL_CTX_new(TLS_client_method());
	if (t->ctx == NULL)
	{
		snprintf(err, errsize, "TLS setup failed");
		free(t);
		return NULL;
	}
	if (verify)
	{
		SSL_CTX_set_default_verify_paths(t->ctx);
		SSL_CTX_set_verify(t->ctx, SSL_VERIFY_PEER, NULL);
	}
	if (max12)
		SSL_CTX_set_max_proto_version(t->ctx, TLS1_2_VERSION);
	t->ssl = SSL_new(t->ctx);
	if (io)
	{
		BIO		   *b;

		t->io = *io;
		t->meth = BIO_meth_new(BIO_get_new_index() | BIO_TYPE_SOURCE_SINK, "xdb");
		BIO_meth_set_write(t->meth, bio_wr);
		BIO_meth_set_read(t->meth, bio_rd);
		BIO_meth_set_ctrl(t->meth, bio_ctrl);
		b = BIO_new(t->meth);
		BIO_set_data(b, t);
		BIO_set_init(b, 1);
		SSL_set_bio(t->ssl, b, b);
	}
	else
		SSL_set_fd(t->ssl, fd);
	if (host && host[0])
		SSL_set_tlsext_host_name(t->ssl, host);
	if (alpn && alpn[0])
	{
		unsigned char a[64];
		size_t		l = strlen(alpn);

		if (l < sizeof a - 1)
		{
			a[0] = (unsigned char) l;
			memcpy(a + 1, alpn, l);
			SSL_set_alpn_protos(t->ssl, a, (unsigned) l + 1);
		}
	}
	if (SSL_connect(t->ssl) != 1)
	{
		unsigned long ec = ERR_get_error();

		snprintf(err, errsize, "TLS handshake failed: %s", ec ? ERR_reason_error_string(ec) : "connection closed");
		xtls_end(t);
		return NULL;
	}
	return t;
}

void
xtls_info(xtls *t, char *out, int outsize)
{
	const char *v,
			   *c;

	out[0] = '\0';
	if (t == NULL || t->ssl == NULL)
		return;
	v = SSL_get_version(t->ssl);
	c = SSL_get_cipher_name(t->ssl);
	if (v && strncmp(v, "TLSv", 4) == 0)
		snprintf(out, outsize, "TLS %s%s%s", v + 4, c ? ", cipher " : "", c ? c : "");
	else
		snprintf(out, outsize, "%s%s%s", v ? v : "TLS", c ? ", cipher " : "", c ? c : "");
}

int
xtls_read(xtls *t, void *buf, int len)
{
	int			n = SSL_read(t->ssl, buf, len);

	return n > 0 ? n : -1;
}

int
xtls_write(xtls *t, const void *buf, int len)
{
	int			n = SSL_write(t->ssl, buf, len);

	return n > 0 ? n : -1;
}

void
xtls_end(xtls *t)
{
	if (t == NULL)
		return;
	if (t->ssl)
		SSL_free(t->ssl);
	if (t->ctx)
		SSL_CTX_free(t->ctx);
	if (t->meth)
		BIO_meth_free(t->meth);
	free(t);
}

#endif

xtls *
xtls_start(int fd, const char *host, int verify, char *err, int errsize)
{
	return xtls_start_io(fd, NULL, host, verify, 0, NULL, err, errsize);
}
