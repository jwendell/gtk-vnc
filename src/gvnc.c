/*
 * Copyright (C) 2006  Anthony Liguori <anthony@codemonkey.ws>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License version 2 as
 * published by the Free Software Foundation.
 *
 *  GTK VNC Widget
 */

#include "gvnc.h"

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include <netdb.h>
#include <string.h>
#include <malloc.h>
#include <unistd.h>
#include <stdio.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>

#include "coroutine.h"
#include "d3des.h"

#include <gnutls/gnutls.h>
#include <gnutls/x509.h>

#define CA_FILE "ca-cert.pem"
#define CRL_FILE "ca-crl.pem"
#define KEY_FILE "key.pem"
#define CERT_FILE "cert.pem"

static gboolean g_io_wait_helper(GIOChannel *channel, GIOCondition cond,
				 gpointer data)
{
	struct coroutine *to = data;
	yieldto(to, &cond);
	return FALSE;
}

GIOCondition g_io_wait(GIOChannel *channel, GIOCondition cond)
{
	GIOCondition *ret;

	g_io_add_watch(channel, cond, g_io_wait_helper, coroutine_self());
	ret = yield(NULL);

	return *ret;
}

typedef void gvnc_blt_func(struct gvnc *, uint8_t *, int, int, int, int, int);

typedef void gvnc_hextile_func(struct gvnc *gvnc, uint8_t flags,
			       uint16_t x, uint16_t y,
			       uint16_t width, uint16_t height,
			       uint8_t *fg, uint8_t *bg);

struct gvnc
{
	GIOChannel *channel;
	struct vnc_pixel_format fmt;
	gboolean has_error;
	int width;
	int height;
	char *name;

	int major;
	int minor;
	gnutls_session_t tls_session;

	char read_buffer[4096];
	size_t read_offset;
	size_t read_size;

	char write_buffer[4096];
	size_t write_offset;

	gboolean perfect_match;
	struct framebuffer local;

	int rp, gp, bp;
	int rm, gm, bm;

	gvnc_blt_func *blt;
	gvnc_hextile_func *hextile;

	int shared_memory_enabled;

	struct vnc_ops ops;

	int absolute;
};

enum {
  GVNC_AUTH_INVALID = 0,
  GVNC_AUTH_NONE = 1,
  GVNC_AUTH_VNC = 2,
  GVNC_AUTH_RA2 = 5,
  GVNC_AUTH_RA2NE = 6,
  GVNC_AUTH_TIGHT = 16,
  GVNC_AUTH_ULTRA = 17,
  GVNC_AUTH_TLS = 18,
  GVNC_AUTH_VENCRYPT = 19
};

enum {
  GVNC_AUTH_VENCRYPT_PLAIN = 256,
  GVNC_AUTH_VENCRYPT_TLSNONE = 257,
  GVNC_AUTH_VENCRYPT_TLSVNC = 258,
  GVNC_AUTH_VENCRYPT_TLSPLAIN = 259,
  GVNC_AUTH_VENCRYPT_X509NONE = 260,
  GVNC_AUTH_VENCRYPT_X509VNC = 261,
  GVNC_AUTH_VENCRYPT_X509PLAIN = 262,
};


#if 0
#define GVNC_DEBUG(fmt, ...) do { fprintf(stderr, fmt, ## __VA_ARGS__); } while (0)
#else
#define GVNC_DEBUG(fmt, ...) do { } while (0)
#endif

static void debug_log(int level, const char* str) {
#if 0
	GVNC_DEBUG("%d %s", level, str);
#endif
}

#define nibhi(a) (((a) >> 4) & 0x0F)
#define niblo(a) ((a) & 0x0F)


/* IO functions */

static void gvnc_read(struct gvnc *gvnc, void *data, size_t len)
{
	int fd = g_io_channel_unix_get_fd(gvnc->channel);
	char *ptr = data;
	size_t offset = 0;

	if (gvnc->has_error) return;
	
	while (offset < len) {
		size_t tmp;

		if (gvnc->read_offset == gvnc->read_size) {
			int ret;

			if (gvnc->tls_session) {
				ret = gnutls_read(gvnc->tls_session, gvnc->read_buffer, 4096);
				if (ret < 0) {
					gvnc->has_error = TRUE;
					return;
				}
			} else {
				ret = read(fd, gvnc->read_buffer, 4096);
				if (ret == -1) {
					switch (errno) {
					case EAGAIN:
						g_io_wait(gvnc->channel, G_IO_IN);
					case EINTR:
						continue;
					default:
						gvnc->has_error = TRUE;
						return;
					}
				}
			}
			if (ret == 0) {
				gvnc->has_error = TRUE;
				return;
			}

			gvnc->read_offset = 0;
			gvnc->read_size = ret;
		}

		tmp = MIN(gvnc->read_size - gvnc->read_offset, len - offset);

		memcpy(ptr + offset, gvnc->read_buffer + gvnc->read_offset, tmp);

		gvnc->read_offset += tmp;
		offset += tmp;
	}
}

static void gvnc_flush(struct gvnc *gvnc)
{
	int fd = g_io_channel_unix_get_fd(gvnc->channel);
	size_t offset = 0;
	while (offset < gvnc->write_offset) {
		int ret;

		if (gvnc->tls_session) {
			ret = gnutls_write(gvnc->tls_session,
					   gvnc->write_buffer+offset,
					   gvnc->write_offset-offset);
			if (ret < 0) {
				gvnc->has_error = TRUE;
				return;
			}
		} else {
			ret = write(fd,
				    gvnc->write_buffer+offset,
				    gvnc->write_offset-offset);
			if (ret == -1) {
				switch (errno) {
				case EAGAIN:
					g_io_wait(gvnc->channel, G_IO_OUT);
				case EINTR:
					continue;
				default:
					gvnc->has_error = TRUE;
					return;
				}
			}
		}
		if (ret == 0) {
			gvnc->has_error = TRUE;
			return;
		}
		offset += ret;
	}
	gvnc->write_offset = 0;
}

static void gvnc_write(struct gvnc *gvnc, const void *data, size_t len)
{
	const char *ptr = data;
	size_t offset = 0;

	while (offset < len) {
		ssize_t tmp;

		if (gvnc->write_offset == sizeof(gvnc->write_buffer)) {
			gvnc_flush(gvnc);
		}

		tmp = MIN(sizeof(gvnc->write_buffer), len - offset);

		memcpy(gvnc->write_buffer+gvnc->write_offset, ptr + offset, tmp);

		gvnc->write_offset += tmp;
		offset += tmp;
	}
}


static ssize_t gvnc_tls_push(gnutls_transport_ptr_t transport,
			      const void *data,
			      size_t len) {
	struct gvnc *gvnc = (struct gvnc *)transport;
	int fd = g_io_channel_unix_get_fd(gvnc->channel);
	int ret;
	size_t sent = 0;

	while (sent < len) {
		ret = write(fd, data+sent, len-sent);
		if (ret == -1 && errno == EINTR)
			continue;
		if (ret == -1 && errno == EAGAIN) {
			g_io_wait(gvnc->channel, G_IO_OUT);
			continue;
		}
		if (ret <= 0) {
			gvnc->has_error = TRUE;
			return -1;
		} else {
			sent += ret;
		}
	}

	return len;
}


static ssize_t gvnc_tls_pull(gnutls_transport_ptr_t transport,
			     void *data,
			     size_t len) {
	struct gvnc *gvnc = (struct gvnc *)transport;
	int fd = g_io_channel_unix_get_fd(gvnc->channel);
	int ret;
	size_t got = 0;

	while (got < len) {
		ret = read(fd, data+got, len-got);
		if (ret == -1 && errno == EINTR)
			continue;
		if (ret == -1 && errno == EAGAIN) {
			g_io_wait(gvnc->channel, G_IO_IN);
			continue;
		}
		if (ret <= 0) {
			gvnc->has_error = TRUE;
			return -1;
		} else {
			got += ret;
		}
	}

	return len;
}



static uint8_t gvnc_read_u8(struct gvnc *gvnc)
{
	uint8_t value = 0;
	gvnc_read(gvnc, &value, sizeof(value));
	return value;
}

static uint16_t gvnc_read_u16(struct gvnc *gvnc)
{
	uint16_t value = 0;
	gvnc_read(gvnc, &value, sizeof(value));
	return ntohs(value);
}

static uint32_t gvnc_read_u32(struct gvnc *gvnc)
{
	uint32_t value = 0;
	gvnc_read(gvnc, &value, sizeof(value));
	return ntohl(value);
}

static int32_t gvnc_read_s32(struct gvnc *gvnc)
{
	int32_t value = 0;
	gvnc_read(gvnc, &value, sizeof(value));
	return ntohl(value);
}

static void gvnc_write_u8(struct gvnc *gvnc, uint8_t value)
{
	gvnc_write(gvnc, &value, sizeof(value));
}

static void gvnc_write_u16(struct gvnc *gvnc, uint16_t value)
{
	value = htons(value);
	gvnc_write(gvnc, &value, sizeof(value));
}

static void gvnc_write_u32(struct gvnc *gvnc, uint32_t value)
{
	value = htonl(value);
	gvnc_write(gvnc, &value, sizeof(value));
}

static void gvnc_write_s32(struct gvnc *gvnc, int32_t value)
{
	value = htonl(value);
	gvnc_write(gvnc, &value, sizeof(value));
}

#define DH_BITS 1024
static gnutls_dh_params_t dh_params;

static gboolean gvnc_tls_initialize(void)
{
	static int tlsinitialized = 0;

	if (tlsinitialized)
		return TRUE;

	if (gnutls_global_init () < 0)
		return FALSE;

	if (gnutls_dh_params_init (&dh_params) < 0)
		return FALSE;
	if (gnutls_dh_params_generate2 (dh_params, DH_BITS) < 0)
		return FALSE;

	gnutls_global_set_log_level(10);
	gnutls_global_set_log_function(debug_log);

	tlsinitialized = TRUE;

	return TRUE;
}

static gnutls_anon_client_credentials gvnc_tls_initialize_anon_cred(void)
{
	gnutls_anon_client_credentials anon_cred;
	int ret;

	if ((ret = gnutls_anon_allocate_client_credentials(&anon_cred)) < 0) {
		GVNC_DEBUG("Cannot allocate credentials %s\n", gnutls_strerror(ret));
		return NULL;
	}

	return anon_cred;
}

static gnutls_certificate_credentials_t gvnc_tls_initialize_cert_cred(void)
{
	gnutls_certificate_credentials_t x509_cred;
	int ret;
	struct stat st;

	if ((ret = gnutls_certificate_allocate_credentials(&x509_cred)) < 0) {
		GVNC_DEBUG("Cannot allocate credentials %s\n", gnutls_strerror(ret));
		return NULL;
	}
	if ((ret = gnutls_certificate_set_x509_trust_file(x509_cred, CA_FILE, GNUTLS_X509_FMT_PEM)) < 0) {
		GVNC_DEBUG("Cannot load CA certificate %s\n", gnutls_strerror(ret));
		return NULL;
	}

	if ((ret = gnutls_certificate_set_x509_key_file (x509_cred, CERT_FILE, KEY_FILE,
							 GNUTLS_X509_FMT_PEM)) < 0) {
		GVNC_DEBUG("Cannot load certificate & key %s\n", gnutls_strerror(ret));
		return NULL;
	}

	if (stat(CRL_FILE, &st) < 0) {
		if (errno != ENOENT) {
			return NULL;
		}
	} else {
		if ((ret = gnutls_certificate_set_x509_crl_file(x509_cred, CRL_FILE, GNUTLS_X509_FMT_PEM)) < 0) {
			GVNC_DEBUG("Cannot load CRL %s\n", gnutls_strerror(ret));
			return NULL;
		}
	}

	gnutls_certificate_set_dh_params (x509_cred, dh_params);

	return x509_cred;
}

static int gvnc_validate_certificate(struct gvnc *vnc)
{
	int ret;
	unsigned int status;
	const gnutls_datum_t *certs;
	unsigned int nCerts, i;
	time_t now;

	GVNC_DEBUG("Validating\n");
	if ((ret = gnutls_certificate_verify_peers2 (vnc->tls_session, &status)) < 0) {
		GVNC_DEBUG("Verify failed %s\n", gnutls_strerror(ret));
		return FALSE;
	}

	if ((now = time(NULL)) == ((time_t)-1)) {
		return FALSE;
	}

	if (status != 0) {
		if (status & GNUTLS_CERT_INVALID)
			GVNC_DEBUG ("The certificate is not trusted.\n");

		if (status & GNUTLS_CERT_SIGNER_NOT_FOUND)
			GVNC_DEBUG ("The certificate hasn't got a known issuer.\n");

		if (status & GNUTLS_CERT_REVOKED)
			GVNC_DEBUG ("The certificate has been revoked.\n");

		if (status & GNUTLS_CERT_INSECURE_ALGORITHM)
			GVNC_DEBUG ("The certificate uses an insecure algorithm\n");

		return FALSE;
	} else {
		GVNC_DEBUG("Certificate is valid!\n");
	}

	if (gnutls_certificate_type_get(vnc->tls_session) != GNUTLS_CRT_X509)
		return FALSE;

	if (!(certs = gnutls_certificate_get_peers(vnc->tls_session, &nCerts)))
		return FALSE;

	for (i = 0 ; i < nCerts ; i++) {
		gnutls_x509_crt_t cert;
		GVNC_DEBUG ("Checking chain %d\n", i);
		if (gnutls_x509_crt_init (&cert) < 0)
			return FALSE;

		if (gnutls_x509_crt_import(cert, &certs[i], GNUTLS_X509_FMT_DER) < 0) {
			gnutls_x509_crt_deinit (cert);
			return FALSE;
		}

		if (gnutls_x509_crt_get_expiration_time (cert) < now) {
			GVNC_DEBUG("The certificate has expired\n");
			gnutls_x509_crt_deinit (cert);
			return FALSE;
		}

		if (gnutls_x509_crt_get_activation_time (cert) > now) {
			GVNC_DEBUG("The certificate is not yet activated\n");
			gnutls_x509_crt_deinit (cert);
			return FALSE;
		}

		if (gnutls_x509_crt_get_activation_time (cert) > now) {
			GVNC_DEBUG("The certificate is not yet activated\n");
			gnutls_x509_crt_deinit (cert);
			return FALSE;
		}

		if (i == 0) {
			/* XXX Fixme */
			const char *hostname = "foo";
			if (!gnutls_x509_crt_check_hostname (cert, hostname)) {
				GVNC_DEBUG ("The certificate's owner does not match hostname '%s'\n",
					    hostname);
				gnutls_x509_crt_deinit (cert);
				return FALSE;
			}
		}
	}

	return TRUE;
}


static void gvnc_read_pixel_format(struct gvnc *gvnc, struct vnc_pixel_format *fmt)
{
	uint8_t pad[3];

	fmt->bits_per_pixel  = gvnc_read_u8(gvnc);
	fmt->depth           = gvnc_read_u8(gvnc);
	fmt->big_endian_flag = gvnc_read_u8(gvnc);
	fmt->true_color_flag = gvnc_read_u8(gvnc);

	GVNC_DEBUG("Pixel format BPP: %d,  Depth: %d, Endian: %d, True color: %d\n",
		   fmt->bits_per_pixel, fmt->depth, fmt->big_endian_flag, fmt->true_color_flag);

	fmt->red_max         = gvnc_read_u16(gvnc);
	fmt->green_max       = gvnc_read_u16(gvnc);
	fmt->blue_max        = gvnc_read_u16(gvnc);

	fmt->red_shift       = gvnc_read_u8(gvnc);
	fmt->green_shift     = gvnc_read_u8(gvnc);
	fmt->blue_shift      = gvnc_read_u8(gvnc);

	gvnc_read(gvnc, pad, 3);
}

/* initialize function */

gboolean gvnc_has_error(struct gvnc *gvnc)
{
	return gvnc->has_error;
}

gboolean gvnc_set_pixel_format(struct gvnc *gvnc,
			       const struct vnc_pixel_format *fmt)
{
	uint8_t pad[3] = {0};

	gvnc_write_u8(gvnc, 0);
	gvnc_write(gvnc, pad, 3);

	gvnc_write_u8(gvnc, fmt->bits_per_pixel);
	gvnc_write_u8(gvnc, fmt->depth);
	gvnc_write_u8(gvnc, fmt->big_endian_flag);
	gvnc_write_u8(gvnc, fmt->true_color_flag);

	gvnc_write_u16(gvnc, fmt->red_max);
	gvnc_write_u16(gvnc, fmt->green_max);
	gvnc_write_u16(gvnc, fmt->blue_max);

	gvnc_write_u8(gvnc, fmt->red_shift);
	gvnc_write_u8(gvnc, fmt->green_shift);
	gvnc_write_u8(gvnc, fmt->blue_shift);

	gvnc_write(gvnc, pad, 3);
	gvnc_flush(gvnc);
	memcpy(&gvnc->fmt, fmt, sizeof(*fmt));

	return gvnc_has_error(gvnc);
}

gboolean gvnc_set_shared_buffer(struct gvnc *gvnc, int line_size, int shmid)
{
	gvnc_write_u8(gvnc, 255);
	gvnc_write_u8(gvnc, 0);
	gvnc_write_u16(gvnc, line_size);
	gvnc_write_u32(gvnc, shmid);
	gvnc_flush(gvnc);

	return gvnc_has_error(gvnc);
}

gboolean gvnc_set_encodings(struct gvnc *gvnc, int n_encoding, int32_t *encoding)
{
	uint8_t pad[1] = {0};
	int i;

	gvnc_write_u8(gvnc, 2);
	gvnc_write(gvnc, pad, 1);
	gvnc_write_u16(gvnc, n_encoding);
	for (i = 0; i < n_encoding; i++)
		gvnc_write_s32(gvnc, encoding[i]);
	gvnc_flush(gvnc);
	return gvnc_has_error(gvnc);
}

gboolean gvnc_framebuffer_update_request(struct gvnc *gvnc,
					 uint8_t incremental,
					 uint16_t x, uint16_t y,
					 uint16_t width, uint16_t height)
{
	gvnc_write_u8(gvnc, 3);
	gvnc_write_u8(gvnc, incremental);
	gvnc_write_u16(gvnc, x);
	gvnc_write_u16(gvnc, y);
	gvnc_write_u16(gvnc, width);
	gvnc_write_u16(gvnc, height);
	gvnc_flush(gvnc);
	return gvnc_has_error(gvnc);
}

gboolean gvnc_key_event(struct gvnc *gvnc, uint8_t down_flag, uint32_t key)
{
	uint8_t pad[2] = {0};

	gvnc_write_u8(gvnc, 4);
	gvnc_write_u8(gvnc, down_flag);
	gvnc_write(gvnc, pad, 2);
	gvnc_write_u32(gvnc, key);
	gvnc_flush(gvnc);
	return gvnc_has_error(gvnc);
}

gboolean gvnc_pointer_event(struct gvnc *gvnc, uint8_t button_mask,
			    uint16_t x, uint16_t y)
{
	gvnc_write_u8(gvnc, 5);
	gvnc_write_u8(gvnc, button_mask);
	gvnc_write_u16(gvnc, x);
	gvnc_write_u16(gvnc, y);
	gvnc_flush(gvnc);
	return gvnc_has_error(gvnc);	
}

gboolean gvnc_client_cut_text(struct gvnc *gvnc,
			      const void *data, size_t length)
{
	uint8_t pad[3] = {0};

	gvnc_write_u8(gvnc, 6);
	gvnc_write(gvnc, pad, 3);
	gvnc_write_u32(gvnc, length);
	gvnc_write(gvnc, data, length);
	gvnc_flush(gvnc);
	return gvnc_has_error(gvnc);
}

static inline uint8_t *gvnc_get_local(struct gvnc *gvnc, int x, int y)
{
	return gvnc->local.data +
		(y * gvnc->local.linesize) +
		(x * gvnc->local.bpp);
}

#define SPLICE_I(a, b) a ## b
#define SPLICE(a, b) SPLICE_I(a, b)

#define SRC 8
#include "blt1.h"
#undef SRC

#define SRC 16
#include "blt1.h"
#undef SRC

#define SRC 32
#include "blt1.h"
#undef SRC

static gvnc_blt_func *gvnc_blt_table[3][3] = {
	{  gvnc_blt_8x8,  gvnc_blt_8x16,  gvnc_blt_8x32 },
	{ gvnc_blt_16x8, gvnc_blt_16x16, gvnc_blt_16x32 },
	{ gvnc_blt_32x8, gvnc_blt_32x16, gvnc_blt_32x32 },
};

static gvnc_hextile_func *gvnc_hextile_table[3][3] = {
	{ (gvnc_hextile_func *)gvnc_hextile_8x8,
	  (gvnc_hextile_func *)gvnc_hextile_8x16,
	  (gvnc_hextile_func *)gvnc_hextile_8x32 },
	{ (gvnc_hextile_func *)gvnc_hextile_16x8,
	  (gvnc_hextile_func *)gvnc_hextile_16x16,
	  (gvnc_hextile_func *)gvnc_hextile_16x32 },
	{ (gvnc_hextile_func *)gvnc_hextile_32x8,
	  (gvnc_hextile_func *)gvnc_hextile_32x16,
	  (gvnc_hextile_func *)gvnc_hextile_32x32 },
};

/* a fast blit for the perfect match scenario */
static void gvnc_blt_fast(struct gvnc *gvnc, uint8_t *src, int pitch,
			  int x, int y, int width, int height)
{
	uint8_t *dst = gvnc_get_local(gvnc, x, y);
	int i;
	for (i = 0; i < height; i++) {
		memcpy(dst, src, width * gvnc->local.bpp);
		dst += gvnc->local.linesize;
		src += pitch;
	}
}

static void gvnc_blt(struct gvnc *gvnc, uint8_t *src, int pitch,
		     int x, int y, int width, int height)
{
	gvnc->blt(gvnc, src, pitch, x, y, width, height);
}

static void gvnc_raw_update(struct gvnc *gvnc,
			    uint16_t x, uint16_t y,
			    uint16_t width, uint16_t height)
{
	uint8_t *dst;
	int i;

	/* optimize for perfect match between server/client
	   FWIW, in the local case, we ought to be doing a write
	   directly from the source framebuffer and a read directly
	   into the client framebuffer
	 */
	if (gvnc->perfect_match) {
		dst = gvnc_get_local(gvnc, x, y);
		for (i = 0; i < height; i++) {
			gvnc_read(gvnc, dst, width * gvnc->local.bpp);
			dst += gvnc->local.linesize;
		}
		return;
	}

	dst = malloc(width * (gvnc->fmt.bits_per_pixel / 8));
	if (dst == NULL) {
		gvnc->has_error = TRUE;
		return;
	}
	
	for (i = 0; i < height; i++) {
		gvnc_read(gvnc, dst, width * (gvnc->fmt.bits_per_pixel / 8));
		gvnc_blt(gvnc, dst, 0, x, y + i, width, 1);
	}
	
	free(dst);
}

static void gvnc_copyrect_update(struct gvnc *gvnc,
				 uint16_t dst_x, uint16_t dst_y,
				 uint16_t width, uint16_t height)
{
	int src_x, src_y;
	uint8_t *dst, *src;
	int pitch = gvnc->local.linesize;
	int i;
	
	src_x = gvnc_read_u16(gvnc);
	src_y = gvnc_read_u16(gvnc);
	
	if (src_y < dst_y) {
		pitch = -pitch;
		src_y += (height - 1);
		dst_y += (height - 1);
	}
	
	dst = gvnc_get_local(gvnc, dst_x, dst_y);
	src = gvnc_get_local(gvnc, src_x, src_y);
	for (i = 0; i < height; i++) {
		memmove(dst, src, width * gvnc->local.bpp);
		dst += pitch;
		src += pitch;
	}
}

static void gvnc_hextile_update(struct gvnc *gvnc,
				uint16_t x, uint16_t y,
				uint16_t width, uint16_t height)
{
	uint8_t fg[4];
	uint8_t bg[4];

	int j;
	for (j = 0; j < height; j += 16) {
		int i;
		for (i = 0; i < width; i += 16) {
			uint8_t flags;
			int w = MIN(16, width - i);
			int h = MIN(16, height - j);

			flags = gvnc_read_u8(gvnc);
			gvnc->hextile(gvnc, flags, x + i, y + j, w, h, fg, bg);
		}
	}
}

static void gvnc_update(struct gvnc *gvnc, int x, int y, int width, int height)
{
	if (gvnc->has_error || !gvnc->ops.update)
		return;
	gvnc->has_error = !gvnc->ops.update(gvnc->ops.user, x, y, width, height);
}

static void gvnc_set_color_map_entry(struct gvnc *gvnc, uint16_t color,
				     uint16_t red, uint16_t green,
				     uint16_t blue)
{
	if (gvnc->has_error || !gvnc->ops.set_color_map_entry)
		return;
	gvnc->has_error = !gvnc->ops.set_color_map_entry(gvnc->ops.user, color,
							 red, green, blue);
}

static void gvnc_bell(struct gvnc *gvnc)
{
	if (gvnc->has_error || !gvnc->ops.bell)
		return;
	gvnc->has_error = !gvnc->ops.bell(gvnc->ops.user);
}

static void gvnc_server_cut_text(struct gvnc *gvnc, const void *data,
				 size_t len)
{
	if (gvnc->has_error || !gvnc->ops.server_cut_text)
		return;
	gvnc->has_error = !gvnc->ops.server_cut_text(gvnc->ops.user, data, len);
}

static void gvnc_resize(struct gvnc *gvnc, int width, int height)
{
	if (gvnc->has_error || !gvnc->ops.resize)
		return;
	gvnc->has_error = !gvnc->ops.resize(gvnc->ops.user, width, height);
}

static void gvnc_pointer_type_change(struct gvnc *gvnc, int absolute)
{
	if (gvnc->has_error || !gvnc->ops.pointer_type_change)
		return;
	gvnc->has_error = !gvnc->ops.pointer_type_change(gvnc->ops.user, absolute);
}

static void gvnc_shared_memory_rmid(struct gvnc *gvnc, int shmid)
{
	if (gvnc->has_error || !gvnc->ops.shared_memory_rmid)
		return;
	gvnc->has_error = !gvnc->ops.shared_memory_rmid(gvnc->ops.user, shmid);
}

static void gvnc_framebuffer_update(struct gvnc *gvnc, int32_t etype,
				    uint16_t x, uint16_t y,
				    uint16_t width, uint16_t height)
{
	GVNC_DEBUG("FramebufferUpdate(%d, %d, %d, %d, %d)\n",
		   etype, x, y, width, height);

	switch (etype) {
	case 0: /* Raw */
		gvnc_raw_update(gvnc, x, y, width, height);
		break;
	case 1: /* CopyRect */
		gvnc_copyrect_update(gvnc, x, y, width, height);
		break;
	case 5: /* Hextile */
		gvnc_hextile_update(gvnc, x, y, width, height);
		break;
	case -223: /* DesktopResize */
		gvnc_resize(gvnc, width, height);
		break;
	case -257: /* PointerChangeType */
		gvnc_pointer_type_change(gvnc, x);
		break;
	case -258: /* SharedMemory */
		switch (gvnc_read_u32(gvnc)) {
		case 0:
			gvnc->shared_memory_enabled = 1;
			break;
		case 1:
			gvnc_shared_memory_rmid(gvnc, gvnc_read_u32(gvnc));
			break;
		case 2:
			gvnc_resize(gvnc, gvnc->width, gvnc->height);
			break;
		case 3:
			break;
		} 
		break;
	default:
		gvnc->has_error = TRUE;
		break;
	}

	gvnc_update(gvnc, x, y, width, height);
}

gboolean gvnc_server_message(struct gvnc *gvnc)
{
	uint8_t msg;

	/* NB: make sure that all server message functions
	   handle has_error appropriately */

	msg = gvnc_read_u8(gvnc);
	switch (msg) {
	case 0: { /* FramebufferUpdate */
		uint8_t pad[1];
		uint16_t n_rects;
		int i;

		gvnc_read(gvnc, pad, 1);
		n_rects = gvnc_read_u16(gvnc);
		for (i = 0; i < n_rects; i++) {
			uint16_t x, y, w, h;
			int32_t etype;

			x = gvnc_read_u16(gvnc);
			y = gvnc_read_u16(gvnc);
			w = gvnc_read_u16(gvnc);
			h = gvnc_read_u16(gvnc);
			etype = gvnc_read_s32(gvnc);

			gvnc_framebuffer_update(gvnc, etype, x, y, w, h);
		}
	}	break;
	case 1: { /* SetColorMapEntries */
		uint16_t first_color;
		uint16_t n_colors;
		uint8_t pad[1];
		int i;

		gvnc_read(gvnc, pad, 1);
		first_color = gvnc_read_u16(gvnc);
		n_colors = gvnc_read_u16(gvnc);

		for (i = 0; i < n_colors; i++) {
			uint16_t red, green, blue;

			red = gvnc_read_u16(gvnc);
			green = gvnc_read_u16(gvnc);
			blue = gvnc_read_u16(gvnc);

			gvnc_set_color_map_entry(gvnc,
						 i + first_color,
						 red, green, blue);
		}
	}	break;
	case 2: /* Bell */
		gvnc_bell(gvnc);
		break;
	case 3: { /* ServerCutText */
		uint8_t pad[3];
		uint32_t n_text;
		char *data;

		gvnc_read(gvnc, pad, 3);
		n_text = gvnc_read_u32(gvnc);
		if (n_text > (32 << 20)) {
			gvnc->has_error = TRUE;
			break;
		}

		data = malloc(n_text + 1);
		if (data == NULL) {
			gvnc->has_error = TRUE;
			break;
		}

		gvnc_read(gvnc, data, n_text);
		data[n_text] = 0;

		gvnc_server_cut_text(gvnc, data, n_text);
		free(data);
	}	break;
	default:
		gvnc->has_error = TRUE;
		break;
	}

	return gvnc_has_error(gvnc);
}

static gboolean gvnc_check_auth_result(struct gvnc *gvnc)
{
	uint32_t result;
	GVNC_DEBUG("Checking auth result\n");
	result = gvnc_read_u32(gvnc);
	if (!result) {
		GVNC_DEBUG("Success\n");
		return TRUE;
	}

	if (gvnc->minor >= 8) {
		uint32_t len;
		char reason[1024];
		len = gvnc_read_u32(gvnc);
		if (len > (sizeof(reason)-1))
			return FALSE;
		gvnc_read(gvnc, reason, len);
		reason[len] = '\0';
		GVNC_DEBUG("Fail %s\n", reason);
	} else {
		GVNC_DEBUG("Fail\n");
	}
	return FALSE;
}

static gboolean gvnc_perform_auth_vnc(struct gvnc *gvnc, const char *password)
{
	uint8_t challenge[16];
	uint8_t key[8];

	GVNC_DEBUG("Do Challenge\n");
	if (!password)
		return FALSE;

	gvnc_read(gvnc, challenge, 16);

	memset(key, 0, 8);
	strncpy((char*)key, (char*)password, 8);

	deskey(key, EN0);
	des(challenge, challenge);
	des(challenge + 8, challenge + 8);

	gvnc_write(gvnc, challenge, 16);
	gvnc_flush(gvnc);
	return gvnc_check_auth_result(gvnc);
}


static gboolean gvnc_start_tls(struct gvnc *gvnc, int anonTLS) {
	static const int cert_type_priority[] = { GNUTLS_CRT_X509, 0 };
	static const int protocol_priority[]= { GNUTLS_TLS1_1, GNUTLS_TLS1_0, GNUTLS_SSL3, 0 };
	static const int kx_priority[] = {GNUTLS_KX_DHE_DSS, GNUTLS_KX_RSA, GNUTLS_KX_DHE_RSA, GNUTLS_KX_SRP, 0};
	static const int kx_anon[] = {GNUTLS_KX_ANON_DH, 0};
	int ret;

	GVNC_DEBUG("Do TLS handshake\n");
	if (gvnc_tls_initialize() < 0) {
		GVNC_DEBUG("Failed to init TLS\n");
		gvnc->has_error = TRUE;
		return FALSE;
	}
	if (gvnc->tls_session == NULL) {
		if (gnutls_init(&gvnc->tls_session, GNUTLS_CLIENT) < 0) {
			gvnc->has_error = TRUE;
			return FALSE;
		}

		if (gnutls_set_default_priority(gvnc->tls_session) < 0) {
			gnutls_deinit(gvnc->tls_session);
			gvnc->has_error = TRUE;
			return FALSE;
		}

		if (gnutls_kx_set_priority(gvnc->tls_session, anonTLS ? kx_anon : kx_priority) < 0) {
			gnutls_deinit(gvnc->tls_session);
			gvnc->has_error = TRUE;
			return FALSE;
		}

		if (gnutls_certificate_type_set_priority(gvnc->tls_session, cert_type_priority) < 0) {
			gnutls_deinit(gvnc->tls_session);
			gvnc->has_error = TRUE;
			return FALSE;
		}

		if (gnutls_protocol_set_priority(gvnc->tls_session, protocol_priority) < 0) {
			gnutls_deinit(gvnc->tls_session);
			gvnc->has_error = TRUE;
			return FALSE;
		}

		if (anonTLS) {
			gnutls_anon_client_credentials anon_cred = gvnc_tls_initialize_anon_cred();
			if (!anon_cred) {
				gnutls_deinit(gvnc->tls_session);
				gvnc->has_error = TRUE;
				return FALSE;
			}
			if (gnutls_credentials_set(gvnc->tls_session, GNUTLS_CRD_ANON, anon_cred) < 0) {
				gnutls_deinit(gvnc->tls_session);
				gvnc->has_error = TRUE;
				return FALSE;
			}
		} else {
			gnutls_certificate_credentials_t x509_cred = gvnc_tls_initialize_cert_cred();
			if (!x509_cred) {
				gnutls_deinit(gvnc->tls_session);
				gvnc->has_error = TRUE;
				return FALSE;
			}
			if (gnutls_credentials_set(gvnc->tls_session, GNUTLS_CRD_CERTIFICATE, x509_cred) < 0) {
				gnutls_deinit(gvnc->tls_session);
				gvnc->has_error = TRUE;
				return FALSE;
			}
		}

		gnutls_transport_set_ptr(gvnc->tls_session, (gnutls_transport_ptr_t)gvnc);
		gnutls_transport_set_push_function(gvnc->tls_session, gvnc_tls_push);
		gnutls_transport_set_pull_function(gvnc->tls_session, gvnc_tls_pull);
	}

	if ((ret = gnutls_handshake(gvnc->tls_session)) < 0) {
		GVNC_DEBUG("Handshake failed %s\n", gnutls_strerror(ret));
		gnutls_deinit(gvnc->tls_session);
		gvnc->tls_session = NULL;
		gvnc->has_error = TRUE;
		return FALSE;
	}
	
	GVNC_DEBUG("Handshake done\n");

	if (anonTLS) {
		return TRUE;
	} else {
		if (!gvnc_validate_certificate(gvnc)) {
			GVNC_DEBUG("Certificate validation failed\n");
			gvnc->has_error = TRUE;
			return FALSE;
		}
		return TRUE;
	}
}

static gboolean gvnc_perform_auth_vencrypt(struct gvnc *gvnc, const char *password)
{
	int major, minor, status, wantAuth = GVNC_AUTH_INVALID, anonTLS;
	unsigned int nauth, i;
	unsigned int auth[20];

	major = gvnc_read_u8(gvnc);
	minor = gvnc_read_u8(gvnc);

	if (major != 0 &&
	    minor != 2) {
		GVNC_DEBUG("Unsupported VeNCrypt version %d %d\n", major, minor);
		return FALSE;
	}

	gvnc_write_u8(gvnc, major);
	gvnc_write_u8(gvnc, minor);
	gvnc_flush(gvnc);
	status = gvnc_read_u8(gvnc);
	if (status != 0) {
		GVNC_DEBUG("Server refused VeNCrypt version %d %d\n", major, minor);
		return FALSE;
	}

	nauth = gvnc_read_u8(gvnc);
	if (nauth > (sizeof(auth)/sizeof(auth[0]))) {
		GVNC_DEBUG("Too many (%d) auth types\n", nauth);
		return FALSE;
	}

	for (i = 0 ; i < nauth ; i++) {
		auth[i] = gvnc_read_u32(gvnc);
	}

	for (i = 0 ; i < nauth ; i++) {
		GVNC_DEBUG("Possible auth %d\n", auth[i]);
	}

	if (gvnc->has_error)
		return FALSE;

	for (i = 0 ; i < nauth ; i++) {
		if (auth[i] == GVNC_AUTH_VENCRYPT_TLSNONE ||
		    auth[i] == GVNC_AUTH_VENCRYPT_TLSPLAIN ||
		    auth[i] == GVNC_AUTH_VENCRYPT_TLSVNC ||
		    auth[i] == GVNC_AUTH_VENCRYPT_X509NONE ||
		    auth[i] == GVNC_AUTH_VENCRYPT_X509PLAIN ||
		    auth[i] == GVNC_AUTH_VENCRYPT_X509VNC) {
			wantAuth = auth[i];
			break;
		}
	}

	if (wantAuth == GVNC_AUTH_VENCRYPT_PLAIN) {
		GVNC_DEBUG("Cowardly refusing to transmit plain text password\n");
		return FALSE;
	}

	GVNC_DEBUG("Choose auth %d\n", wantAuth);
	gvnc_write_u32(gvnc, wantAuth);
	gvnc_flush(gvnc);
	status = gvnc_read_u8(gvnc);
	if (status != 1) {
		GVNC_DEBUG("Server refused VeNCrypt auth %d %d\n", wantAuth, status);
		return FALSE;
	}

	if (wantAuth == GVNC_AUTH_VENCRYPT_TLSNONE ||
	    wantAuth ==  GVNC_AUTH_VENCRYPT_TLSPLAIN ||
	    wantAuth ==  GVNC_AUTH_VENCRYPT_TLSVNC)
		anonTLS = 1;
	else
		anonTLS = 0;

	if (!gvnc_start_tls(gvnc, anonTLS)) {
		GVNC_DEBUG("Could not start TLS\n");
		return FALSE;
	}
	GVNC_DEBUG("Completed TLS setup\n");

	switch (wantAuth) {
		/* Plain certificate based auth */
	case GVNC_AUTH_VENCRYPT_TLSNONE:
	case GVNC_AUTH_VENCRYPT_X509NONE:
		GVNC_DEBUG("Completing auth\n");
		return gvnc_check_auth_result(gvnc);

		/* Regular VNC layered over TLS */
	case GVNC_AUTH_VENCRYPT_TLSVNC:
	case GVNC_AUTH_VENCRYPT_X509VNC:
		GVNC_DEBUG("Handing off to VNC auth\n");
		return gvnc_perform_auth_vnc(gvnc, password);

	default:
		return FALSE;
	}
}

static gboolean gvnc_perform_auth(struct gvnc *gvnc, const char *password)
{
	int wantAuth = GVNC_AUTH_INVALID;
	unsigned int nauth, i;
	unsigned int auth[10];

	if (gvnc->minor <= 6) {
		nauth = 1;
		auth[0] = gvnc_read_u32(gvnc);
	} else {
		nauth = gvnc_read_u8(gvnc);
		if (gvnc_has_error(gvnc))
			return FALSE;

		if (nauth == 0)
			return gvnc_check_auth_result(gvnc);

		if (nauth > sizeof(auth)) {
			gvnc->has_error = TRUE;
			return FALSE;
		}
		for (i = 0 ; i < nauth ; i++)
			auth[i] = gvnc_read_u8(gvnc);
	}

	for (i = 0 ; i < nauth ; i++) {
		GVNC_DEBUG("Possible auth %d\n", auth[i]);
	}

	if (gvnc->has_error)
		return FALSE;

	for (i = 0 ; i < nauth ; i++) {
		if (auth[i] == GVNC_AUTH_NONE ||
		    auth[i] == GVNC_AUTH_VNC ||
		    auth[i] == GVNC_AUTH_VENCRYPT) {
			wantAuth = auth[i];
			break;
		}
	}

	if (gvnc->minor > 6) {
		GVNC_DEBUG("Chose auth %d\n", wantAuth);
		gvnc_write_u8(gvnc, wantAuth);
		gvnc_flush(gvnc);
	}

	switch (wantAuth) {
	case GVNC_AUTH_NONE:
		if (gvnc->minor == 8)
			return gvnc_check_auth_result(gvnc);
		return TRUE;
	case GVNC_AUTH_VNC:
		return gvnc_perform_auth_vnc(gvnc, password);

	case GVNC_AUTH_VENCRYPT:
		return gvnc_perform_auth_vencrypt(gvnc, password);

	default:
		return FALSE;
	}

	return TRUE;
}

struct gvnc *gvnc_connect(GIOChannel *channel, gboolean shared_flag, const char *password)
{
	int s;
	int ret;
	char version[13];
	uint32_t n_name;
	struct gvnc *gvnc = NULL;

	s = g_io_channel_unix_get_fd(channel);

	if (fcntl(s, F_SETFL, O_NONBLOCK) == -1)
		goto error;

	gvnc = malloc(sizeof(*gvnc));
	if (gvnc == NULL)
		goto error;

	memset(gvnc, 0, sizeof(*gvnc));

	gvnc->channel = channel;
	gvnc->absolute = 1;

	gvnc_read(gvnc, version, 12);
	version[12] = 0;

 	ret = sscanf(version, "RFB %03d.%03d\n", &gvnc->major, &gvnc->minor);
	if (ret != 2)
		goto error;

	if (gvnc->major != 3)
		goto error;
	if (gvnc->minor != 3 &&
	    gvnc->minor != 5 &&
	    gvnc->minor != 6 &&
	    gvnc->minor != 7 &&
	    gvnc->minor != 8)
		goto error;

	snprintf(version, 12, "RFB %03d.%03d\n", gvnc->major, gvnc->minor);
	gvnc_write(gvnc, version, 12);
	gvnc_flush(gvnc);
	GVNC_DEBUG("Negotiated protocol %d %d\n", gvnc->major, gvnc->minor);

	if (!gvnc_perform_auth(gvnc, password)) {
		GVNC_DEBUG("Auth failed\n");
		goto error;
	}

	gvnc_write_u8(gvnc, shared_flag); /* shared flag */
	gvnc_flush(gvnc);
	gvnc->width = gvnc_read_u16(gvnc);
	gvnc->height = gvnc_read_u16(gvnc);

	gvnc_read_pixel_format(gvnc, &gvnc->fmt);

	n_name = gvnc_read_u32(gvnc);
	if (n_name > 4096)
		goto error;

	gvnc->name = malloc(n_name + 1);
	if (gvnc->name == NULL)
		goto error;

	gvnc_read(gvnc, gvnc->name, n_name);
	gvnc->name[n_name] = 0;
	GVNC_DEBUG("Display name '%s'\n", gvnc->name);

	gvnc_resize(gvnc, gvnc->width, gvnc->height);

	return gvnc;

 error:
	free(gvnc);
	return NULL;
}

gboolean gvnc_set_local(struct gvnc *gvnc, struct framebuffer *fb)
{
	int i, j;
	int depth;

	memcpy(&gvnc->local, fb, sizeof(*fb));

	if (fb->bpp == (gvnc->fmt.bits_per_pixel / 8) &&
	    fb->red_mask == gvnc->fmt.red_max &&
	    fb->green_mask == gvnc->fmt.green_max &&
	    fb->blue_mask == gvnc->fmt.blue_max &&
	    fb->red_shift == gvnc->fmt.red_shift &&
	    fb->green_shift == gvnc->fmt.green_shift &&
	    fb->blue_shift == gvnc->fmt.blue_shift)
		gvnc->perfect_match = TRUE;
	else
		gvnc->perfect_match = FALSE;

	depth = gvnc->fmt.depth;
	if (depth == 32)
		depth = 24;

	gvnc->rp =  (gvnc->local.depth - gvnc->local.red_shift);
	gvnc->rp -= (depth - gvnc->fmt.red_shift);
	gvnc->gp =  (gvnc->local.red_shift - gvnc->local.green_shift);
	gvnc->gp -= (gvnc->fmt.red_shift - gvnc->fmt.green_shift);
	gvnc->bp =  (gvnc->local.green_shift - gvnc->local.blue_shift);
	gvnc->bp -= (gvnc->fmt.green_shift - gvnc->fmt.blue_shift);

	gvnc->rp = gvnc->local.red_shift + gvnc->rp;
	gvnc->gp = gvnc->local.green_shift + gvnc->gp;
	gvnc->bp = gvnc->local.blue_shift + gvnc->bp;

	gvnc->rm = gvnc->local.red_mask & gvnc->fmt.red_max;
	gvnc->gm = gvnc->local.green_mask & gvnc->fmt.green_max;
	gvnc->bm = gvnc->local.blue_mask & gvnc->fmt.blue_max;

	i = gvnc->fmt.bits_per_pixel / 8;
	j = gvnc->local.bpp;

	if (i == 4) i = 3;
	if (j == 4) j = 3;

	gvnc->blt = gvnc_blt_table[i - 1][j - 1];
	gvnc->hextile = gvnc_hextile_table[i - 1][j - 1];

	if (gvnc->perfect_match)
		gvnc->blt = gvnc_blt_fast;

	return gvnc_has_error(gvnc);
}

gboolean gvnc_shared_memory_enabled(struct gvnc *gvnc)
{
	return gvnc->shared_memory_enabled;
}

gboolean gvnc_set_vnc_ops(struct gvnc *gvnc, struct vnc_ops *ops)
{
	memcpy(&gvnc->ops, ops, sizeof(*ops));
	gvnc_resize(gvnc, gvnc->width, gvnc->height);
	return gvnc_has_error(gvnc);
}

const char *gvnc_get_name(struct gvnc *gvnc)
{
	return gvnc->name;
}

int gvnc_get_width(struct gvnc *gvnc)
{
	return gvnc->width;
}

int gvnc_get_height(struct gvnc *gvnc)
{
	return gvnc->height;
}

/*
 * Local variables:
 *  c-indent-level: 8
 *  c-basic-offset: 8
 *  tab-width: 8
 * End:
 */
