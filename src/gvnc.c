/*
 * Copyright (C) 2006  Anthony Liguori <anthony@codemonkey.ws>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License version 2 or
 * later as published by the Free Software Foundation.
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

#include "x_keymap.h"

#include "utils.h"
#include <gnutls/gnutls.h>
#include <gnutls/x509.h>

#include <zlib.h>

#include <gdk/gdkkeysyms.h>

#include "vnc_keycodes.h"

struct wait_queue
{
	gboolean waiting;
	struct coroutine *context;
};


typedef void gvnc_blt_func(struct gvnc *, uint8_t *, int, int, int, int, int);

typedef void gvnc_fill_func(struct gvnc *, uint8_t *, int, int, int, int);

typedef void gvnc_set_pixel_at_func(struct gvnc *, int, int, uint8_t *);

typedef void gvnc_hextile_func(struct gvnc *gvnc, uint8_t flags,
			       uint16_t x, uint16_t y,
			       uint16_t width, uint16_t height,
			       uint8_t *fg, uint8_t *bg);

typedef void gvnc_rich_cursor_blt_func(struct gvnc *, uint8_t *, uint8_t *,
				       uint8_t *, int, uint16_t, uint16_t);

typedef void gvnc_rgb24_blt_func(struct gvnc *, int, int, int, int,
				 uint8_t *, int);

typedef void gvnc_tight_compute_predicted_func(struct gvnc *, uint8_t *,
					       uint8_t *, uint8_t *,
					       uint8_t *);

typedef void gvnc_tight_sum_pixel_func(struct gvnc *, uint8_t *, uint8_t *);

/*
 * A special GSource impl which allows us to wait on a certain
 * condition to be satisfied. This is effectively a boolean test
 * run on each iteration of the main loop. So whenever a file has
 * new I/O, or a timer occurs, etc we'll do the check. This is
 * pretty efficient compared to a normal GLib Idle func which has
 * to busy wait on a timeout, since our condition is only checked
 * when some other source's state changes
 */
typedef gboolean (*g_condition_wait_func)(gpointer);

struct g_condition_wait_source
{
        GSource src;
        struct coroutine *co;
	g_condition_wait_func func;
	gpointer data;
};

struct gvnc
{
	GIOChannel *channel;
	int fd;
	char *host;
	char *port;
	struct gvnc_pixel_format fmt;
	gboolean has_error;
	int width;
	int height;
	char *name;

	int major;
	int minor;
	gnutls_session_t tls_session;

	/* Auth related params */
	unsigned int auth_type;
	unsigned int auth_subtype;
	char *cred_username;
	char *cred_password;
	char *cred_x509_cacert;
	char *cred_x509_cacrl;
	char *cred_x509_cert;
	char *cred_x509_key;

	char read_buffer[4096];
	size_t read_offset;
	size_t read_size;

	char write_buffer[4096];
	size_t write_offset;

	gboolean perfect_match;
	struct gvnc_framebuffer local;

	int rm, gm, bm;
	int rrs, grs, brs;
	int rls, gls, bls;

	gvnc_blt_func *blt;
	gvnc_fill_func *fill;
	gvnc_set_pixel_at_func *set_pixel_at;
	gvnc_hextile_func *hextile;
	gvnc_rich_cursor_blt_func *rich_cursor_blt;
	gvnc_rgb24_blt_func *rgb24_blt;
	gvnc_tight_compute_predicted_func *tight_compute_predicted;
	gvnc_tight_sum_pixel_func *tight_sum_pixel;

	struct gvnc_ops ops;
	gpointer ops_data;

	int absolute;

	int wait_interruptable;
	struct wait_queue wait;

	char *xmit_buffer;
	int xmit_buffer_capacity;
	int xmit_buffer_size;

	z_stream *strm;
	z_stream streams[5];

	size_t uncompressed_length;
	uint8_t uncompressed_buffer[4096];

	size_t compressed_length;
	uint8_t *compressed_buffer;

	uint8_t zrle_pi;
	int zrle_pi_bits;

	gboolean has_ext_key_event;
};

#define nibhi(a) (((a) >> 4) & 0x0F)
#define niblo(a) ((a) & 0x0F)

/* Main loop helper functions */
static gboolean g_io_wait_helper(GIOChannel *channel G_GNUC_UNUSED,
				 GIOCondition cond,
				 gpointer data)
{
	struct coroutine *to = data;
	coroutine_yieldto(to, &cond);
	return FALSE;
}

static GIOCondition g_io_wait(GIOChannel *channel, GIOCondition cond)
{
	GIOCondition *ret;

	g_io_add_watch(channel, cond | G_IO_HUP | G_IO_ERR | G_IO_NVAL, g_io_wait_helper, coroutine_self());
	ret = coroutine_yield(NULL);

	return *ret;
}


static GIOCondition g_io_wait_interruptable(struct wait_queue *wait,
					    GIOChannel *channel,
					    GIOCondition cond)
{
	GIOCondition *ret;
	gint id;

	wait->context = coroutine_self();
	id = g_io_add_watch(channel, cond | G_IO_HUP | G_IO_ERR | G_IO_NVAL, g_io_wait_helper, wait->context);

	wait->waiting = TRUE;
	ret = coroutine_yield(NULL);
	wait->waiting = FALSE;

	if (ret == NULL) {
		g_source_remove(id);
		return 0;
	} else
		return *ret;
}

static void g_io_wakeup(struct wait_queue *wait)
{
	if (wait->waiting)
		coroutine_yieldto(wait->context, NULL);
}


/*
 * Call immediately before the main loop does an iteration. Returns
 * true if the condition we're checking is ready for dispatch
 */
static gboolean g_condition_wait_prepare(GSource *src,
					 int *timeout) {
        struct g_condition_wait_source *vsrc = (struct g_condition_wait_source *)src;
        *timeout = -1;
        return vsrc->func(vsrc->data);
}

/*
 * Call immediately after the main loop does an iteration. Returns
 * true if the condition we're checking is ready for dispatch
 */
static gboolean g_condition_wait_check(GSource *src) {
        struct g_condition_wait_source *vsrc = (struct g_condition_wait_source *)src;
        return vsrc->func(vsrc->data);
}

static gboolean g_condition_wait_dispatch(GSource *src G_GNUC_UNUSED,
					  GSourceFunc cb,
					  gpointer data) {
        return cb(data);
}

GSourceFuncs waitFuncs = {
        .prepare = g_condition_wait_prepare,
        .check = g_condition_wait_check,
        .dispatch = g_condition_wait_dispatch,
  };

static gboolean g_condition_wait_helper(gpointer data)
{
        struct coroutine *co = (struct coroutine *)data;
        coroutine_yieldto(co, NULL);
        return FALSE;
}

static gboolean g_condition_wait(g_condition_wait_func func, gpointer data)
{
	GSource *src;
	struct g_condition_wait_source *vsrc;

	/* Short-circuit check in case we've got it ahead of time */
	if (func(data)) {
		return TRUE;
	}

	/*
	 * Don't have it, so yield to the main loop, checking the condition
	 * on each iteration of the main loop
	 */
	src = g_source_new(&waitFuncs, sizeof(struct g_condition_wait_source));
	vsrc = (struct g_condition_wait_source *)src;

	vsrc->func = func;
	vsrc->data = data;
	vsrc->co = coroutine_self();

	g_source_attach(src, NULL);
	g_source_set_callback(src, g_condition_wait_helper, coroutine_self(), NULL);
	coroutine_yield(NULL);
	return TRUE;
}

static gboolean gvnc_use_compression(struct gvnc *gvnc)
{
	return gvnc->compressed_buffer != NULL;
}

static int gvnc_zread(struct gvnc *gvnc, void *buffer, size_t size)
{
	size_t offset = 0;

	while (offset < size) {
		/* if data is available in the uncompressed buffer, then
		 * copy */
		if (gvnc->uncompressed_length) {
			size_t len = MIN(gvnc->uncompressed_length,
					 size - offset);

			memcpy(buffer + offset,
			       gvnc->uncompressed_buffer,
			       len);

			gvnc->uncompressed_length -= len;
			if (gvnc->uncompressed_length)
				memmove(gvnc->uncompressed_buffer,
					gvnc->uncompressed_buffer + len,
					gvnc->uncompressed_length);
			offset += len;
		} else {
			int err;

			gvnc->strm->next_in = gvnc->compressed_buffer;
			gvnc->strm->avail_in = gvnc->compressed_length;
			gvnc->strm->next_out = gvnc->uncompressed_buffer;
			gvnc->strm->avail_out = sizeof(gvnc->uncompressed_buffer);

			/* inflate as much as possible */
			err = inflate(gvnc->strm, Z_SYNC_FLUSH);
			if (err != Z_OK) {
				errno = EIO;
				return -1;
			}

			gvnc->uncompressed_length = (uint8_t *)gvnc->strm->next_out - gvnc->uncompressed_buffer;
			gvnc->compressed_length -= (uint8_t *)gvnc->strm->next_in - gvnc->compressed_buffer;
			gvnc->compressed_buffer = gvnc->strm->next_in;
		}
	}

	return offset;
}

/* IO functions */

static int gvnc_read(struct gvnc *gvnc, void *data, size_t len)
{
	int fd = g_io_channel_unix_get_fd(gvnc->channel);
	char *ptr = data;
	size_t offset = 0;

	if (gvnc->has_error) return -EINVAL;
	
	while (offset < len) {
		size_t tmp;

		/* compressed data is buffered independently of the read buffer
		 * so we must by-pass it */
		if (gvnc_use_compression(gvnc)) {
			int ret = gvnc_zread(gvnc, data + offset, len);
			if (ret == -1) {
				GVNC_DEBUG("Closing the connection: gvnc_read() - gvnc_zread() failed\n");
				gvnc->has_error = TRUE;
				return -errno;
			}
			offset += ret;
			continue;
		} else if (gvnc->read_offset == gvnc->read_size) {
			int ret;

			if (gvnc->tls_session) {
				ret = gnutls_read(gvnc->tls_session, gvnc->read_buffer, 4096);
				if (ret < 0) {
					if (ret == GNUTLS_E_AGAIN)
						errno = EAGAIN;
					else
						errno = EIO;
					ret = -1;
				}
			} else
				ret = read(fd, gvnc->read_buffer, 4096);

			if (ret == -1) {
				switch (errno) {
				case EAGAIN:
					if (gvnc->wait_interruptable) {
						if (!g_io_wait_interruptable(&gvnc->wait,
									     gvnc->channel, G_IO_IN))
							return -EAGAIN;
					} else
						g_io_wait(gvnc->channel, G_IO_IN);
				case EINTR:
					continue;
				default:
					GVNC_DEBUG("Closing the connection: gvnc_read() - ret=-1\n");
					gvnc->has_error = TRUE;
					return -errno;
				}
			}
			if (ret == 0) {
				GVNC_DEBUG("Closing the connection: gvnc_read() - ret=0\n");
				gvnc->has_error = TRUE;
				return -EPIPE;
			}

			gvnc->read_offset = 0;
			gvnc->read_size = ret;
		}

		tmp = MIN(gvnc->read_size - gvnc->read_offset, len - offset);

		memcpy(ptr + offset, gvnc->read_buffer + gvnc->read_offset, tmp);

		gvnc->read_offset += tmp;
		offset += tmp;
	}

	return 0;
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
				if (ret == GNUTLS_E_AGAIN)
					errno = EAGAIN;
				else
					errno = EIO;
				ret = -1;
			}
		} else
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
				GVNC_DEBUG("Closing the connection: gvnc_flush\n");
				gvnc->has_error = TRUE;
				return;
			}
		}
		if (ret == 0) {
			GVNC_DEBUG("Closing the connection: gvnc_flush\n");
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

 retry:
	ret = write(fd, data, len);
	if (ret < 0) {
		if (errno == EINTR)
			goto retry;
		return -1;
	}
	return ret;
}


static ssize_t gvnc_tls_pull(gnutls_transport_ptr_t transport,
			     void *data,
			     size_t len) {
	struct gvnc *gvnc = (struct gvnc *)transport;
	int fd = g_io_channel_unix_get_fd(gvnc->channel);
	int ret;

 retry:
	ret = read(fd, data, len);
	if (ret < 0) {
		if (errno == EINTR)
			goto retry;
		return -1;
	}
	return ret;
}

static size_t gvnc_pixel_size(struct gvnc *gvnc)
{
	return gvnc->fmt.bits_per_pixel / 8;
}

static void gvnc_read_pixel(struct gvnc *gvnc, uint8_t *pixel)
{
	gvnc_read(gvnc, pixel, gvnc_pixel_size(gvnc));
}

static uint8_t gvnc_read_u8(struct gvnc *gvnc)
{
	uint8_t value = 0;
	gvnc_read(gvnc, &value, sizeof(value));
	return value;
}

static int gvnc_read_u8_interruptable(struct gvnc *gvnc, uint8_t *value)
{
	int ret;

	gvnc->wait_interruptable = 1;
	ret = gvnc_read(gvnc, value, sizeof(*value));
	gvnc->wait_interruptable = 0;

	return ret;
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

static gnutls_certificate_credentials_t gvnc_tls_initialize_cert_cred(struct gvnc *vnc)
{
	gnutls_certificate_credentials_t x509_cred;
	int ret;

	if ((ret = gnutls_certificate_allocate_credentials(&x509_cred)) < 0) {
		GVNC_DEBUG("Cannot allocate credentials %s\n", gnutls_strerror(ret));
		return NULL;
	}
	if (vnc->cred_x509_cacert) {
		if ((ret = gnutls_certificate_set_x509_trust_file(x509_cred,
								  vnc->cred_x509_cacert,
								  GNUTLS_X509_FMT_PEM)) < 0) {
			GVNC_DEBUG("Cannot load CA certificate %s\n", gnutls_strerror(ret));
			return NULL;
		}
	} else {
		GVNC_DEBUG("No CA certificate provided\n");
		return NULL;
	}

	if (vnc->cred_x509_cert && vnc->cred_x509_key) {
		if ((ret = gnutls_certificate_set_x509_key_file (x509_cred,
								 vnc->cred_x509_cert,
								 vnc->cred_x509_key,
								 GNUTLS_X509_FMT_PEM)) < 0) {
			GVNC_DEBUG("Cannot load certificate & key %s\n", gnutls_strerror(ret));
			return NULL;
		}
	} else {
		GVNC_DEBUG("No client cert or key provided\n");
	}

	if (vnc->cred_x509_cacrl) {
		if ((ret = gnutls_certificate_set_x509_crl_file(x509_cred,
								vnc->cred_x509_cacrl,
								GNUTLS_X509_FMT_PEM)) < 0) {
			GVNC_DEBUG("Cannot load CRL %s\n", gnutls_strerror(ret));
			return NULL;
		}
	} else {
		GVNC_DEBUG("No CA revocation list provided\n");
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
			if (!vnc->host) {
				GVNC_DEBUG ("No hostname provided for certificate verification\n");
				gnutls_x509_crt_deinit (cert);
				return FALSE;
			}
			if (!gnutls_x509_crt_check_hostname (cert, vnc->host)) {
				GVNC_DEBUG ("The certificate's owner does not match hostname '%s'\n",
					    vnc->host);
				gnutls_x509_crt_deinit (cert);
				return FALSE;
			}
		}
	}

	return TRUE;
}


static void gvnc_read_pixel_format(struct gvnc *gvnc, struct gvnc_pixel_format *fmt)
{
	uint8_t pad[3];

	fmt->bits_per_pixel  = gvnc_read_u8(gvnc);
	fmt->depth           = gvnc_read_u8(gvnc);
	fmt->byte_order      = gvnc_read_u8(gvnc) ? G_BIG_ENDIAN : G_LITTLE_ENDIAN;
	fmt->true_color_flag = gvnc_read_u8(gvnc);

	fmt->red_max         = gvnc_read_u16(gvnc);
	fmt->green_max       = gvnc_read_u16(gvnc);
	fmt->blue_max        = gvnc_read_u16(gvnc);

	fmt->red_shift       = gvnc_read_u8(gvnc);
	fmt->green_shift     = gvnc_read_u8(gvnc);
	fmt->blue_shift      = gvnc_read_u8(gvnc);

	gvnc_read(gvnc, pad, 3);

	GVNC_DEBUG("Pixel format BPP: %d,  Depth: %d, Byte order: %d, True color: %d\n"
		   "             Mask  red: %3d, green: %3d, blue: %3d\n"
		   "             Shift red: %3d, green: %3d, blue: %3d\n",
		   fmt->bits_per_pixel, fmt->depth, fmt->byte_order, fmt->true_color_flag,
		   fmt->red_max, fmt->green_max, fmt->blue_max,
		   fmt->red_shift, fmt->green_shift, fmt->blue_shift);
}

/* initialize function */

gboolean gvnc_has_error(struct gvnc *gvnc)
{
	return gvnc->has_error;
}

gboolean gvnc_set_pixel_format(struct gvnc *gvnc,
			       const struct gvnc_pixel_format *fmt)
{
	uint8_t pad[3] = {0};

	gvnc_write_u8(gvnc, 0);
	gvnc_write(gvnc, pad, 3);

	gvnc_write_u8(gvnc, fmt->bits_per_pixel);
	gvnc_write_u8(gvnc, fmt->depth);
	gvnc_write_u8(gvnc, fmt->byte_order == G_BIG_ENDIAN ? 1 : 0);
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

	return !gvnc_has_error(gvnc);
}

gboolean gvnc_set_encodings(struct gvnc *gvnc, int n_encoding, int32_t *encoding)
{
	uint8_t pad[1] = {0};
	int i;

	gvnc->has_ext_key_event = FALSE;
	gvnc_write_u8(gvnc, 2);
	gvnc_write(gvnc, pad, 1);
	gvnc_write_u16(gvnc, n_encoding);
	for (i = 0; i < n_encoding; i++)
		gvnc_write_s32(gvnc, encoding[i]);
	gvnc_flush(gvnc);
	return !gvnc_has_error(gvnc);
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
	return !gvnc_has_error(gvnc);
}

static void gvnc_buffered_write(struct gvnc *gvnc, const void *data, size_t size)
{
	size_t left;

	left = gvnc->xmit_buffer_capacity - gvnc->xmit_buffer_size;
	if (left < size) {
		gvnc->xmit_buffer_capacity += size + 4095;
		gvnc->xmit_buffer_capacity &= ~4095;

		gvnc->xmit_buffer = g_realloc(gvnc->xmit_buffer, gvnc->xmit_buffer_capacity);
	}

	memcpy(&gvnc->xmit_buffer[gvnc->xmit_buffer_size],
	       data, size);

	gvnc->xmit_buffer_size += size;
}

static void gvnc_buffered_write_u8(struct gvnc *gvnc, uint8_t value)
{
	gvnc_buffered_write(gvnc, &value, 1);
}

static void gvnc_buffered_write_u16(struct gvnc *gvnc, uint16_t value)
{
	value = htons(value);
	gvnc_buffered_write(gvnc, &value, 2);
}

static void gvnc_buffered_write_u32(struct gvnc *gvnc, uint32_t value)
{
	value = htonl(value);
	gvnc_buffered_write(gvnc, &value, 4);
}

static void gvnc_buffered_flush(struct gvnc *gvnc)
{
	g_io_wakeup(&gvnc->wait);
}

gboolean gvnc_key_event(struct gvnc *gvnc, uint8_t down_flag,
			uint32_t key, uint16_t scancode)
{
	uint8_t pad[2] = {0};

	if (gvnc->has_ext_key_event) {
		if (key == GDK_Pause)
			scancode = VKC_PAUSE;
		else
			scancode = x_keycode_to_pc_keycode(scancode);

		gvnc_buffered_write_u8(gvnc, 255);
		gvnc_buffered_write_u8(gvnc, 0);
		gvnc_buffered_write_u16(gvnc, down_flag);
		gvnc_buffered_write_u32(gvnc, key);
		gvnc_buffered_write_u32(gvnc, scancode);
	} else {
		gvnc_buffered_write_u8(gvnc, 4);
		gvnc_buffered_write_u8(gvnc, down_flag);
		gvnc_buffered_write(gvnc, pad, 2);
		gvnc_buffered_write_u32(gvnc, key);
	}

	gvnc_buffered_flush(gvnc);
	return !gvnc_has_error(gvnc);
}

gboolean gvnc_pointer_event(struct gvnc *gvnc, uint8_t button_mask,
			    uint16_t x, uint16_t y)
{
	gvnc_buffered_write_u8(gvnc, 5);
	gvnc_buffered_write_u8(gvnc, button_mask);
	gvnc_buffered_write_u16(gvnc, x);
	gvnc_buffered_write_u16(gvnc, y);
	gvnc_buffered_flush(gvnc);
	return !gvnc_has_error(gvnc);	
}

gboolean gvnc_client_cut_text(struct gvnc *gvnc,
			      const void *data, size_t length)
{
	uint8_t pad[3] = {0};

	gvnc_buffered_write_u8(gvnc, 6);
	gvnc_buffered_write(gvnc, pad, 3);
	gvnc_buffered_write_u32(gvnc, length);
	gvnc_buffered_write(gvnc, data, length);
	gvnc_buffered_flush(gvnc);
	return !gvnc_has_error(gvnc);
}

static inline uint8_t *gvnc_get_local(struct gvnc *gvnc, int x, int y)
{
	return gvnc->local.data +
		(y * gvnc->local.linesize) +
		(x * gvnc->local.bpp);
}

static uint8_t gvnc_swap_8(struct gvnc *gvnc G_GNUC_UNUSED, uint8_t pixel)
{
	return pixel;
}

static uint16_t gvnc_swap_16(struct gvnc *gvnc, uint16_t pixel)
{
	if (gvnc->fmt.byte_order != gvnc->local.byte_order)
		return  (((pixel >> 8) & 0xFF) << 0) |
			(((pixel >> 0) & 0xFF) << 8);
	else
		return pixel;
}

static uint32_t gvnc_swap_32(struct gvnc *gvnc, uint32_t pixel)
{
	if (gvnc->fmt.byte_order != gvnc->local.byte_order)
		return  (((pixel >> 24) & 0xFF) <<  0) |
			(((pixel >> 16) & 0xFF) <<  8) |
			(((pixel >>  8) & 0xFF) << 16) |
			(((pixel >>  0) & 0xFF) << 24);
	else
		return pixel;
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

static gvnc_set_pixel_at_func *gvnc_set_pixel_at_table[3][3] = {
	{ (gvnc_set_pixel_at_func *)gvnc_set_pixel_at_8x8,
	  (gvnc_set_pixel_at_func *)gvnc_set_pixel_at_8x16,
	  (gvnc_set_pixel_at_func *)gvnc_set_pixel_at_8x32 },
	{ (gvnc_set_pixel_at_func *)gvnc_set_pixel_at_16x8,
	  (gvnc_set_pixel_at_func *)gvnc_set_pixel_at_16x16,
	  (gvnc_set_pixel_at_func *)gvnc_set_pixel_at_16x32 },
	{ (gvnc_set_pixel_at_func *)gvnc_set_pixel_at_32x8,
	  (gvnc_set_pixel_at_func *)gvnc_set_pixel_at_32x16,
	  (gvnc_set_pixel_at_func *)gvnc_set_pixel_at_32x32 },
};

static gvnc_fill_func *gvnc_fill_table[3][3] = {
	{ (gvnc_fill_func *)gvnc_fill_8x8,
	  (gvnc_fill_func *)gvnc_fill_8x16,
	  (gvnc_fill_func *)gvnc_fill_8x32 },
	{ (gvnc_fill_func *)gvnc_fill_16x8,
	  (gvnc_fill_func *)gvnc_fill_16x16,
	  (gvnc_fill_func *)gvnc_fill_16x32 },
	{ (gvnc_fill_func *)gvnc_fill_32x8,
	  (gvnc_fill_func *)gvnc_fill_32x16,
	  (gvnc_fill_func *)gvnc_fill_32x32 },
};

static gvnc_rich_cursor_blt_func *gvnc_rich_cursor_blt_table[3] = {
	gvnc_rich_cursor_blt_8x32,
	gvnc_rich_cursor_blt_16x32,
	gvnc_rich_cursor_blt_32x32,
};

static gvnc_rgb24_blt_func *gvnc_rgb24_blt_table[3] = {
	(gvnc_rgb24_blt_func *)gvnc_rgb24_blt_32x8,
	(gvnc_rgb24_blt_func *)gvnc_rgb24_blt_32x16,
	(gvnc_rgb24_blt_func *)gvnc_rgb24_blt_32x32,
};

static gvnc_tight_compute_predicted_func *gvnc_tight_compute_predicted_table[3] = {
	(gvnc_tight_compute_predicted_func *)gvnc_tight_compute_predicted_8x8,
	(gvnc_tight_compute_predicted_func *)gvnc_tight_compute_predicted_16x16,
	(gvnc_tight_compute_predicted_func *)gvnc_tight_compute_predicted_32x32,
};

static gvnc_tight_sum_pixel_func *gvnc_tight_sum_pixel_table[3] = {
	(gvnc_tight_sum_pixel_func *)gvnc_tight_sum_pixel_8x8,
	(gvnc_tight_sum_pixel_func *)gvnc_tight_sum_pixel_16x16,
	(gvnc_tight_sum_pixel_func *)gvnc_tight_sum_pixel_32x32,
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
		GVNC_DEBUG("Closing the connection: gvnc_raw_update\n");
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

static void gvnc_fill(struct gvnc *gvnc, uint8_t *color,
		      uint16_t x, uint16_t y, uint16_t width, uint16_t height)
{
	gvnc->fill(gvnc, color, x, y, width, height);
}

static void gvnc_set_pixel_at(struct gvnc *gvnc, int x, int y, uint8_t *pixel)
{
	gvnc->set_pixel_at(gvnc, x, y, pixel);
}

static void gvnc_rre_update(struct gvnc *gvnc,
			    uint16_t x, uint16_t y,
			    uint16_t width, uint16_t height)
{
	uint8_t bg[4];
	uint32_t num;
	uint32_t i;

	num = gvnc_read_u32(gvnc);
	gvnc_read_pixel(gvnc, bg);
	gvnc_fill(gvnc, bg, x, y, width, height);

	for (i = 0; i < num; i++) {
		uint8_t fg[4];
		uint16_t sub_x, sub_y, sub_w, sub_h;

		gvnc_read_pixel(gvnc, fg);
		sub_x = gvnc_read_u16(gvnc);
		sub_y = gvnc_read_u16(gvnc);
		sub_w = gvnc_read_u16(gvnc);
		sub_h = gvnc_read_u16(gvnc);

		gvnc_fill(gvnc, fg,
			  x + sub_x, y + sub_y, sub_w, sub_h);
	}
}

/* CPIXELs are optimized slightly.  32-bit pixel values are packed into 24-bit
 * values. */
static void gvnc_read_cpixel(struct gvnc *gvnc, uint8_t *pixel)
{
	int bpp = gvnc_pixel_size(gvnc);

	memset(pixel, 0, bpp);

	if (bpp == 4 && gvnc->fmt.true_color_flag && gvnc->fmt.depth == 24) {
		bpp = 3;
		if (gvnc->fmt.byte_order == G_BIG_ENDIAN)
			pixel += 1;
	}

	gvnc_read(gvnc, pixel, bpp);
}

static void gvnc_zrle_update_tile_blit(struct gvnc *gvnc,
				       uint16_t x, uint16_t y,
				       uint16_t width, uint16_t height)
{
	uint8_t blit_data[4 * 64 * 64];
	int i, bpp;

	bpp = gvnc_pixel_size(gvnc);

	for (i = 0; i < width * height; i++)
		gvnc_read_cpixel(gvnc, blit_data + (i * bpp));

	gvnc_blt(gvnc, blit_data, width * bpp, x, y, width, height);
}

static uint8_t gvnc_read_zrle_pi(struct gvnc *gvnc, int palette_size)
{
	uint8_t pi = 0;

	if (gvnc->zrle_pi_bits == 0) {
		gvnc->zrle_pi = gvnc_read_u8(gvnc);
		gvnc->zrle_pi_bits = 8;
	}
	if ( palette_size == 2) {
		pi = (gvnc->zrle_pi >> (gvnc->zrle_pi_bits - 1)) & 1;
		gvnc->zrle_pi_bits -= 1;
	} else if ((palette_size == 3) || (palette_size == 4)) {
		pi = (gvnc->zrle_pi >> (gvnc->zrle_pi_bits - 2)) & 3;
		gvnc->zrle_pi_bits -= 2;
	} else if ((palette_size >=5) && (palette_size <=16)){
		pi = (gvnc->zrle_pi >> (gvnc->zrle_pi_bits - 4)) & 15;
		gvnc->zrle_pi_bits -= 4;
	}

	return pi;
}

static void gvnc_zrle_update_tile_palette(struct gvnc *gvnc,
					  uint8_t palette_size,
					  uint16_t x, uint16_t y,
					  uint16_t width, uint16_t height)
{
	uint8_t palette[128][4];
	int i, j;

	for (i = 0; i < palette_size; i++)
		gvnc_read_cpixel(gvnc, palette[i]);

	for (j = 0; j < height; j++) {
		/* discard any padding bits */
		gvnc->zrle_pi_bits = 0;

		for (i = 0; i < width; i++) {
			int ind = gvnc_read_zrle_pi(gvnc, palette_size);

			gvnc_set_pixel_at(gvnc, x + i, y + j,
					  palette[ind & 0x7F]);
		}
	}
}

static int gvnc_read_zrle_rl(struct gvnc *gvnc)
{
	int rl = 1;
	uint8_t byte;

	do {
		byte = gvnc_read_u8(gvnc);
		rl += byte;
	} while (!gvnc_has_error(gvnc) && byte == 255);

	return rl;
}

static void gvnc_zrle_update_tile_rle(struct gvnc *gvnc,
				      uint16_t x, uint16_t y,
				      uint16_t width, uint16_t height)
{
	int i, j, rl = 0;
	uint8_t pixel[4];

	for (j = 0; j < height; j++) {
		for (i = 0; i < width; i++) {
			if (rl == 0) {
				gvnc_read_cpixel(gvnc, pixel);
				rl = gvnc_read_zrle_rl(gvnc);
			}
			gvnc_set_pixel_at(gvnc, x + i, y + j, pixel);
			rl -= 1;
		}
	}
}

static void gvnc_zrle_update_tile_prle(struct gvnc *gvnc,
				       uint8_t palette_size,
				       uint16_t x, uint16_t y,
				       uint16_t width, uint16_t height)
{
	int i, j, rl = 0;
	uint8_t palette[128][4];
	uint8_t pi = 0;

	for (i = 0; i < palette_size; i++)
		gvnc_read_cpixel(gvnc, palette[i]);

	for (j = 0; j < height; j++) {
		for (i = 0; i < width; i++) {
			if (rl == 0) {
				pi = gvnc_read_u8(gvnc);
				if (pi & 0x80) {
					rl = gvnc_read_zrle_rl(gvnc);
					pi &= 0x7F;
				} else
					rl = 1;
			}

			gvnc_set_pixel_at(gvnc, x + i, y + j, palette[pi]);
			rl -= 1;
		}
	}
}

static void gvnc_zrle_update_tile(struct gvnc *gvnc, uint16_t x, uint16_t y,
				  uint16_t width, uint16_t height)
{
	uint8_t subencoding = gvnc_read_u8(gvnc);
	uint8_t pixel[4];

	if (subencoding == 0 ) {
		/* Raw pixel data */
		gvnc_zrle_update_tile_blit(gvnc, x, y, width, height);
	} else if (subencoding == 1) {
		/* Solid tile of a single color */
		gvnc_read_cpixel(gvnc, pixel);
		gvnc_fill(gvnc, pixel, x, y, width, height);
	} else if ((subencoding >= 2) && (subencoding <= 16)) {
		/* Packed palette types */
		gvnc_zrle_update_tile_palette(gvnc, subencoding,
					      x, y, width, height);
	} else if ((subencoding >= 17) && (subencoding <= 127)) {
		/* FIXME raise error? */
	} else if (subencoding == 128) {
		/* Plain RLE */
		gvnc_zrle_update_tile_rle(gvnc, x, y, width, height);
	} else if (subencoding == 129) { 
	
	} else if (subencoding >= 130) {
		/* Palette RLE */
		gvnc_zrle_update_tile_prle(gvnc, subencoding - 128,
					   x, y, width, height);
	}
}

static void gvnc_zrle_update(struct gvnc *gvnc,
			     uint16_t x, uint16_t y,
			     uint16_t width, uint16_t height)

{
	uint32_t length;
	uint32_t offset;
	uint16_t i, j;
	uint8_t *zlib_data;

	length = gvnc_read_u32(gvnc);
	zlib_data = malloc(length);
	if (zlib_data == NULL) {
		GVNC_DEBUG("Closing the connection: gvnc_zrle_update\n");
		gvnc->has_error = TRUE;
		return;
	}

	gvnc_read(gvnc, zlib_data, length);

	/* setup subsequent calls to gvnc_read*() to use the compressed data */
	gvnc->uncompressed_length = 0;
	gvnc->compressed_length = length;
	gvnc->compressed_buffer = zlib_data;
	gvnc->strm = &gvnc->streams[0];

	offset = 0;
	for (j = 0; j < height; j += 64) {
		for (i = 0; i < width; i += 64) {
			uint16_t w, h;

			w = MIN(width - i, 64);
			h = MIN(height - j, 64);
			gvnc_zrle_update_tile(gvnc, x + i, y + j, w, h);
		}
	}

	gvnc->strm = NULL;
	gvnc->uncompressed_length = 0;
	gvnc->compressed_length = 0;
	gvnc->compressed_buffer = NULL;

	free(zlib_data);
}

static void gvnc_rgb24_blt(struct gvnc *gvnc, int x, int y,
			   int width, int height, uint8_t *data, int pitch)
{
	gvnc->rgb24_blt(gvnc, x, y, width, height, data, pitch);
}

static uint32_t gvnc_read_cint(struct gvnc *gvnc)
{
	uint32_t value = 0;
	uint8_t val;

	val = gvnc_read_u8(gvnc);
	value = (val & 0x7F);
	if (!(val & 0x80))
		return value;

	val = gvnc_read_u8(gvnc);
	value |= (val & 0x7F) << 7;

	if (!(val & 0x80))
		return value;

	value |= gvnc_read_u8(gvnc) << 14;

	return value;
}

static int gvnc_tpixel_size(struct gvnc *gvnc)
{
	if (gvnc->fmt.depth == 24)
		return 3;
	return gvnc->fmt.bits_per_pixel / 8;
}

static void gvnc_read_tpixel(struct gvnc *gvnc, uint8_t *pixel)
{
	if (gvnc->fmt.depth == 24) {
		uint32_t val;
		gvnc_read(gvnc, pixel, 3);
		val = (pixel[0] << gvnc->fmt.red_shift)
			| (pixel[1] << gvnc->fmt.green_shift)
			| (pixel[2] << gvnc->fmt.blue_shift);

		if (gvnc->fmt.byte_order != G_BYTE_ORDER)
			val =   (((val >>  0) & 0xFF) << 24) |
				(((val >>  8) & 0xFF) << 16) |
				(((val >> 16) & 0xFF) << 8) |
				(((val >> 24) & 0xFF) << 0);

		memcpy(pixel, &val, 4);
	} else
		gvnc_read_pixel(gvnc, pixel);
}

static void gvnc_tight_update_copy(struct gvnc *gvnc,
				   uint16_t x, uint16_t y,
				   uint16_t width, uint16_t height)
{
	uint8_t pixel[4];
	int i, j;

	for (j = 0; j < height; j++) {
		for (i = 0; i < width; i++) {
			gvnc_read_tpixel(gvnc, pixel);
			gvnc_set_pixel_at(gvnc, x + i, y + j, pixel);
		}
	}
}

static int gvnc_tight_get_pi(struct gvnc *gvnc, uint8_t *ra,
			     int i, uint8_t palette_size)
{
	if (palette_size == 2) {
		if ((i % 8) == 0)
			*ra = gvnc_read_u8(gvnc);
		return (*ra >> (7 - (i % 8))) & 1;
	}

	return gvnc_read_u8(gvnc);
}

static void gvnc_tight_update_palette(struct gvnc *gvnc,
				      int palette_size, uint8_t *palette,
				      uint16_t x, uint16_t y,
				      uint16_t width, uint16_t height)
{
	int i, j;

	for (j = 0; j < height; j++) {
		uint8_t ra = 0;

		for (i = 0; i < width; i++) {
			uint8_t ind;

			ind = gvnc_tight_get_pi(gvnc, &ra, i, palette_size);
			gvnc_set_pixel_at(gvnc, x + i, y + j,
					  &palette[ind * 4]);
		}
	}
}

static void gvnc_tight_compute_predicted(struct gvnc *gvnc, uint8_t *ppixel,
					  uint8_t *lp, uint8_t *cp,
					  uint8_t *llp)
{
	gvnc->tight_compute_predicted(gvnc, ppixel, lp, cp, llp);
}

static void gvnc_tight_sum_pixel(struct gvnc *gvnc,
				 uint8_t *lhs, uint8_t *rhs)
{
	gvnc->tight_sum_pixel(gvnc, lhs, rhs);
}

static void gvnc_tight_update_gradient(struct gvnc *gvnc,
				       uint16_t x, uint16_t y,
				       uint16_t width, uint16_t height)
{
	int i, j;
	uint8_t zero_pixel[4];
	uint8_t *last_row, *row;
	int bpp;

	bpp = gvnc_pixel_size(gvnc);
	last_row = g_malloc(width * bpp);
	row = g_malloc(width * bpp);

	memset(last_row, 0, width * bpp);
	memset(zero_pixel, 0, 4);

	for (j = 0; j < height; j++) {
		uint8_t *tmp_row;
		uint8_t *llp, *lp;

		/* use zero pixels for the edge cases */
		llp = zero_pixel;
		lp = zero_pixel;

		for (i = 0; i < width; i++) {
			uint8_t predicted_pixel[4];

			/* compute predicted pixel value */
			gvnc_tight_compute_predicted(gvnc, predicted_pixel,
						     lp, last_row + i * bpp,
						     llp);

			/* read the difference pixel from the wire */
			gvnc_read_tpixel(gvnc, row + i * bpp);

			/* sum the predicted pixel and the difference to get
			 * the original pixel value */
			gvnc_tight_sum_pixel(gvnc, row + i * bpp,
					     predicted_pixel);

			llp = last_row + i * bpp;
			lp = row + i * bpp;
		}

		/* write out row of pixel data */
		gvnc_blt(gvnc, row, width * bpp, x, y + j, width, 1);

		/* swap last row and current row */
		tmp_row = last_row;
		last_row = row;
		row = tmp_row;
	}

	g_free(row);
	g_free(last_row);
}

static void jpeg_draw(void *opaque, int x, int y, int w, int h,
		      uint8_t *data, int stride)
{
	struct gvnc *gvnc = opaque;

	gvnc_rgb24_blt(gvnc, x, y, w, h, data, stride);
}

static void gvnc_tight_update_jpeg(struct gvnc *gvnc, uint16_t x, uint16_t y,
				   uint16_t width, uint16_t height,
				   uint8_t *data, size_t length)
{
	if (gvnc->ops.render_jpeg == NULL)
		return;

	gvnc->ops.render_jpeg(gvnc->ops_data, jpeg_draw, gvnc,
			      x, y, width, height, data, length);
}

static void gvnc_tight_update(struct gvnc *gvnc,
			      uint16_t x, uint16_t y,
			      uint16_t width, uint16_t height)
{
	uint8_t ccontrol;
	uint8_t pixel[4];
	int i;

	ccontrol = gvnc_read_u8(gvnc);

	for (i = 0; i < 4; i++) {
		if (ccontrol & (1 << i)) {
			inflateEnd(&gvnc->streams[i + 1]);
			inflateInit(&gvnc->streams[i + 1]);
		}
	}

	ccontrol >>= 4;
	ccontrol &= 0x0F;

	if (ccontrol <= 7) {
		/* basic */
		uint8_t filter_id = 0;
		uint32_t data_size, zlib_length;
		uint8_t *zlib_data = NULL;
		uint8_t palette[256][4];
		int palette_size = 0;

		if (ccontrol & 0x04)
			filter_id = gvnc_read_u8(gvnc);

		gvnc->strm = &gvnc->streams[(ccontrol & 0x03) + 1];

		if (filter_id == 1) {
			palette_size = gvnc_read_u8(gvnc);
			palette_size += 1;
			for (i = 0; i < palette_size; i++)
				gvnc_read_tpixel(gvnc, palette[i]);
		}

		if (filter_id == 1) {
			if (palette_size == 2)
				data_size = ((width + 7) / 8) * height;
			else
				data_size = width * height;
		} else
			data_size = width * height * gvnc_tpixel_size(gvnc);

		if (data_size >= 12) {
			zlib_length = gvnc_read_cint(gvnc);
			zlib_data = g_malloc(zlib_length);

			gvnc_read(gvnc, zlib_data, zlib_length);

			gvnc->uncompressed_length = 0;
			gvnc->compressed_length = zlib_length;
			gvnc->compressed_buffer = zlib_data;
		}

		switch (filter_id) {
		case 0: /* copy */
			gvnc_tight_update_copy(gvnc, x, y, width, height);
			break;
		case 1: /* palette */
			gvnc_tight_update_palette(gvnc, palette_size,
						  (uint8_t *)palette,
						  x, y, width, height);
			break;
		case 2: /* gradient */
			gvnc_tight_update_gradient(gvnc, x, y, width, height);
			break;
		default: /* error */
			GVNC_DEBUG("Closing the connection: gvnc_tight_update() - filter_id unknown\n");
			gvnc->has_error = TRUE;
			break;
		}

		if (data_size >= 12) {
			gvnc->uncompressed_length = 0;
			gvnc->compressed_length = 0;
			gvnc->compressed_buffer = NULL;

			g_free(zlib_data);
		}

		gvnc->strm = NULL;
	} else if (ccontrol == 8) {
		/* fill */
		/* FIXME check each width; endianness */
		gvnc_read_tpixel(gvnc, pixel);
		gvnc_fill(gvnc, pixel, x, y, width, height);
	} else if (ccontrol == 9) {
		/* jpeg */
		uint32_t length;
		uint8_t *jpeg_data;

		length = gvnc_read_cint(gvnc);
		jpeg_data = g_malloc(length);
		gvnc_read(gvnc, jpeg_data, length);
		gvnc_tight_update_jpeg(gvnc, x, y, width, height,
				       jpeg_data, length);
		g_free(jpeg_data);
	} else {
		/* error */
		GVNC_DEBUG("Closing the connection: gvnc_tight_update() - ccontrol unknown\n");
		gvnc->has_error = TRUE;
	}
}

static void gvnc_update(struct gvnc *gvnc, int x, int y, int width, int height)
{
	if (gvnc->has_error || !gvnc->ops.update)
		return;
	if (!gvnc->ops.update(gvnc->ops_data, x, y, width, height)) {
		GVNC_DEBUG("Closing the connection: gvnc_update\n");
		gvnc->has_error = TRUE;
	}
}

static void gvnc_set_color_map_entry(struct gvnc *gvnc, uint16_t color,
				     uint16_t red, uint16_t green,
				     uint16_t blue)
{
	if (gvnc->has_error || !gvnc->ops.set_color_map_entry)
		return;
	if (!gvnc->ops.set_color_map_entry(gvnc->ops_data, color,
					    red, green, blue)) {
		GVNC_DEBUG("Closing the connection: gvnc_set_color_map_entry\n");
		gvnc->has_error = TRUE;
	}
}

static void gvnc_bell(struct gvnc *gvnc)
{
	if (gvnc->has_error || !gvnc->ops.bell)
		return;

	GVNC_DEBUG("Server beep\n");

	if (!gvnc->ops.bell(gvnc->ops_data)) {
		GVNC_DEBUG("Closing the connection: gvnc_bell\n");
		gvnc->has_error = TRUE;
	}
}

static void gvnc_server_cut_text(struct gvnc *gvnc, const void *data,
				 size_t len)
{
	if (gvnc->has_error || !gvnc->ops.server_cut_text)
		return;

	if (!gvnc->ops.server_cut_text(gvnc->ops_data, data, len)) {
		GVNC_DEBUG("Closing the connection: gvnc_server_cut_text\n");
		gvnc->has_error = TRUE;
	}
}

static void gvnc_resize(struct gvnc *gvnc, int width, int height)
{
	if (gvnc->has_error || !gvnc->ops.resize)
		return;
	if (!gvnc->ops.resize(gvnc->ops_data, width, height)) {
		GVNC_DEBUG("Closing the connection: gvnc_resize\n");
		gvnc->has_error = TRUE;
	}
}

static void gvnc_pixel_format(struct gvnc *gvnc)
{
        if (gvnc->has_error || !gvnc->ops.pixel_format)
                return;
        if (!gvnc->ops.pixel_format(gvnc->ops_data, &gvnc->fmt))
                gvnc->has_error = TRUE;
}

static void gvnc_pointer_type_change(struct gvnc *gvnc, int absolute)
{
	if (gvnc->has_error || !gvnc->ops.pointer_type_change)
		return;
	if (!gvnc->ops.pointer_type_change(gvnc->ops_data, absolute)) {
		GVNC_DEBUG("Closing the connection: gvnc_pointer_type_change\n");
		gvnc->has_error = TRUE;
	}
}

static void gvnc_rich_cursor_blt(struct gvnc *gvnc, uint8_t *pixbuf,
				 uint8_t *image, uint8_t *mask,
				 int pitch, uint16_t width, uint16_t height)
{
	gvnc->rich_cursor_blt(gvnc, pixbuf, image, mask, pitch, width, height);
}

static void gvnc_rich_cursor(struct gvnc *gvnc, int x, int y, int width, int height)
{
	uint8_t *pixbuf = NULL;

	if (width && height) {
		uint8_t *image, *mask;
		int imagelen, masklen;

		imagelen = width * height * (gvnc->fmt.bits_per_pixel / 8);
		masklen = ((width + 7)/8) * height;

		image = malloc(imagelen);
		if (!image) {
			GVNC_DEBUG("Closing the connection: gvnc_rich_cursor() - !image \n");
			gvnc->has_error = TRUE;
			return;
		}
		mask = malloc(masklen);
		if (!mask) {
			free(image);
			GVNC_DEBUG("Closing the connection: gvnc_rich_cursor() - !mask \n");
			gvnc->has_error = TRUE;
			return;
		}
		pixbuf = malloc(width * height * 4); /* RGB-A 8bit */
		if (!pixbuf) {
			free(mask);
			free(image);
			GVNC_DEBUG("Closing the connection: gvnc_rich_cursor() - !pixbuf \n");
			gvnc->has_error = TRUE;
			return;
		}
		gvnc_read(gvnc, image, imagelen);
		gvnc_read(gvnc, mask, masklen);

		gvnc_rich_cursor_blt(gvnc, pixbuf, image, mask,
				     width * (gvnc->fmt.bits_per_pixel/8),
				     width, height);

		free(image);
		free(mask);
	}

	if (gvnc->has_error || !gvnc->ops.local_cursor)
		return;
	if (!gvnc->ops.local_cursor(gvnc->ops_data, x, y, width, height, pixbuf)) {
		GVNC_DEBUG("Closing the connection: gvnc_rich_cursor() - !ops.local_cursor() \n");
		gvnc->has_error = TRUE;
	}

	free(pixbuf);
}


static void gvnc_xcursor(struct gvnc *gvnc, int x, int y, int width, int height)
{
	uint8_t *pixbuf = NULL;

	if (width && height) {
		uint8_t *data, *mask, *datap, *maskp;
		uint32_t *pixp;
		int rowlen;
		int x1, y1;
		uint8_t fgrgb[3], bgrgb[3];
		uint32_t fg, bg;
		gvnc_read(gvnc, fgrgb, 3);
		gvnc_read(gvnc, bgrgb, 3);
		fg = (255 << 24) | (fgrgb[0] << 16) | (fgrgb[1] << 8) | fgrgb[2];
		bg = (255 << 24) | (bgrgb[0] << 16) | (bgrgb[1] << 8) | bgrgb[2];

		rowlen = ((width + 7)/8);
		if (!(data = malloc(rowlen*height))) {
			GVNC_DEBUG("Closing the connection: gvnc_xcursor() - !data \n");
			gvnc->has_error = TRUE;
			return;
		}
		if (!(mask = malloc(rowlen*height))) {
			free(data);
			GVNC_DEBUG("Closing the connection: gvnc_xcursor() - !mask \n");
			gvnc->has_error = TRUE;
			return;
		}
		pixbuf = malloc(width * height * 4); /* RGB-A 8bit */
		gvnc_read(gvnc, data, rowlen*height);
		gvnc_read(gvnc, mask, rowlen*height);
		datap = data;
		maskp = mask;
		pixp = (uint32_t*)pixbuf;
		for (y1 = 0; y1 < height; y1++) {
			for (x1 = 0; x1 < width; x1++) {
				*pixp++ = ((maskp[x1 / 8] >> (7-(x1 % 8))) & 1) ?
					(((datap[x1 / 8] >> (7-(x1 % 8))) & 1) ? fg : bg) : 0;
			}
			datap += rowlen;
			maskp += rowlen;
		}
		free(data);
		free(mask);
	}

	if (gvnc->has_error || !gvnc->ops.local_cursor)
		return;
	if (!gvnc->ops.local_cursor(gvnc->ops_data, x, y, width, height, pixbuf)) {
		GVNC_DEBUG("Closing the connection: gvnc_xcursor() - !ops.local_cursor() \n");
		gvnc->has_error = TRUE;
	}

	free(pixbuf);
}


static void gvnc_framebuffer_update(struct gvnc *gvnc, int32_t etype,
				    uint16_t x, uint16_t y,
				    uint16_t width, uint16_t height)
{
	GVNC_DEBUG("FramebufferUpdate(%d, %d, %d, %d, %d)\n",
		   etype, x, y, width, height);

	switch (etype) {
	case GVNC_ENCODING_RAW:
		gvnc_raw_update(gvnc, x, y, width, height);
		break;
	case GVNC_ENCODING_COPY_RECT:
		gvnc_copyrect_update(gvnc, x, y, width, height);
		break;
	case GVNC_ENCODING_RRE:
		gvnc_rre_update(gvnc, x, y, width, height);
		break;
	case GVNC_ENCODING_HEXTILE:
		gvnc_hextile_update(gvnc, x, y, width, height);
		break;
	case GVNC_ENCODING_ZRLE:
		gvnc_zrle_update(gvnc, x, y, width, height);
		break;
	case GVNC_ENCODING_TIGHT:
		gvnc_tight_update(gvnc, x, y, width, height);
		break;
	case GVNC_ENCODING_DESKTOP_RESIZE:
		gvnc_resize(gvnc, width, height);
		break;
	case GVNC_ENCODING_POINTER_CHANGE:
		gvnc_pointer_type_change(gvnc, x);
		break;
        case GVNC_ENCODING_WMVi:
                gvnc_read_pixel_format(gvnc, &gvnc->fmt);
                gvnc_pixel_format(gvnc);
                break;
	case GVNC_ENCODING_RICH_CURSOR:
		gvnc_rich_cursor(gvnc, x, y, width, height);
		break;
	case GVNC_ENCODING_XCURSOR:
		gvnc_xcursor(gvnc, x, y, width, height);
		break;
	case GVNC_ENCODING_EXT_KEY_EVENT:
		gvnc->has_ext_key_event = TRUE;
		break;
	default:
		GVNC_DEBUG("Received an unknown encoding type: %d\n", etype);
		gvnc->has_error = TRUE;
		break;
	}

	gvnc_update(gvnc, x, y, width, height);
}

gboolean gvnc_server_message(struct gvnc *gvnc)
{
	uint8_t msg;
	int ret;

	/* NB: make sure that all server message functions
	   handle has_error appropriately */

	do {
		if (gvnc->xmit_buffer_size) {
			gvnc_write(gvnc, gvnc->xmit_buffer, gvnc->xmit_buffer_size);
			gvnc_flush(gvnc);
			gvnc->xmit_buffer_size = 0;
		}
	} while ((ret = gvnc_read_u8_interruptable(gvnc, &msg)) == -EAGAIN);

	if (ret < 0) {
		GVNC_DEBUG("Aborting message processing on error\n");
		return !gvnc_has_error(gvnc);
	}

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
			GVNC_DEBUG("Closing the connection: gvnc_server_message() - cutText > allowed \n");
			gvnc->has_error = TRUE;
			break;
		}

		data = malloc(n_text + 1);
		if (data == NULL) {
			GVNC_DEBUG("Closing the connection: gvnc_server_message() - cutText - !data \n");
			gvnc->has_error = TRUE;
			break;
		}

		gvnc_read(gvnc, data, n_text);
		data[n_text] = 0;

		gvnc_server_cut_text(gvnc, data, n_text);
		free(data);
	}	break;
	default:
		GVNC_DEBUG("Received an unknown message: %u\n", msg);
		gvnc->has_error = TRUE;
		break;
	}

	return !gvnc_has_error(gvnc);
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
		if (!gvnc->has_error && gvnc->ops.auth_failure)
			gvnc->ops.auth_failure(gvnc->ops_data, reason);
	} else {
		GVNC_DEBUG("Fail\n");
		if (!gvnc->has_error && gvnc->ops.auth_failure)
			gvnc->ops.auth_failure(gvnc->ops_data, NULL);
	}
	return FALSE;
}

static gboolean gvnc_perform_auth_vnc(struct gvnc *gvnc)
{
	uint8_t challenge[16];
	uint8_t key[8];

	GVNC_DEBUG("Do Challenge\n");
	if (!gvnc->cred_password)
		return FALSE;

	gvnc_read(gvnc, challenge, 16);

	memset(key, 0, 8);
	strncpy((char*)key, (char*)gvnc->cred_password, 8);

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
			gnutls_certificate_credentials_t x509_cred = gvnc_tls_initialize_cert_cred(gvnc);
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

 retry:
	if ((ret = gnutls_handshake(gvnc->tls_session)) < 0) {
		if (!gnutls_error_is_fatal(ret)) {
			GVNC_DEBUG("Handshake was blocking\n");
			if (!gnutls_record_get_direction(gvnc->tls_session))
				g_io_wait(gvnc->channel, G_IO_IN);
			else
				g_io_wait(gvnc->channel, G_IO_OUT);
			goto retry;
		}
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

gboolean gvnc_wants_credential_password(struct gvnc *gvnc)
{
        if (gvnc->auth_type == GVNC_AUTH_VNC)
                return TRUE;

        if (gvnc->auth_type == GVNC_AUTH_TLS &&
	    gvnc->auth_subtype == GVNC_AUTH_VNC)
                return TRUE;

        if (gvnc->auth_type == GVNC_AUTH_VENCRYPT) {
                if (gvnc->auth_subtype == GVNC_AUTH_VENCRYPT_PLAIN ||
                    gvnc->auth_subtype == GVNC_AUTH_VENCRYPT_TLSVNC ||
                    gvnc->auth_subtype == GVNC_AUTH_VENCRYPT_TLSPLAIN ||
                    gvnc->auth_subtype == GVNC_AUTH_VENCRYPT_X509VNC ||
                    gvnc->auth_subtype == GVNC_AUTH_VENCRYPT_X509PLAIN)
                        return TRUE;
        }

        return FALSE;
}

gboolean gvnc_wants_credential_username(struct gvnc *gvnc)
{
        if (gvnc->auth_type == GVNC_AUTH_VENCRYPT) {
                if (gvnc->auth_subtype == GVNC_AUTH_VENCRYPT_PLAIN ||
                    gvnc->auth_subtype == GVNC_AUTH_VENCRYPT_TLSPLAIN ||
                    gvnc->auth_subtype == GVNC_AUTH_VENCRYPT_X509PLAIN)
                        return TRUE;
        }

        return FALSE;
}

gboolean gvnc_wants_credential_x509(struct gvnc *gvnc)
{
        if (gvnc->auth_type == GVNC_AUTH_VENCRYPT) {
                if (gvnc->auth_subtype == GVNC_AUTH_VENCRYPT_X509NONE ||
                    gvnc->auth_subtype == GVNC_AUTH_VENCRYPT_X509PLAIN ||
                    gvnc->auth_subtype == GVNC_AUTH_VENCRYPT_X509VNC)
                        return TRUE;
        }

        return FALSE;
}

static gboolean gvnc_has_credentials(gpointer data)
{
	struct gvnc *gvnc = (struct gvnc *)data;

	if (gvnc->has_error)
		return TRUE;
	if (gvnc_wants_credential_username(gvnc) && !gvnc->cred_username)
		return FALSE;
	if (gvnc_wants_credential_password(gvnc) && !gvnc->cred_password)
		return FALSE;
	/*
	 * For x509 we require a minimum of the CA cert.
	 * Anything else is a bonus - though the server
	 * may reject auth if it decides it wants a client
	 * cert. We can't express that based on auth type
	 * alone though - we'll merely find out when TLS
	 * negotiation takes place.
	 */
	if (gvnc_wants_credential_x509(gvnc) && !gvnc->cred_x509_cacert)
		return FALSE;
	return TRUE;
}

static gboolean gvnc_gather_credentials(struct gvnc *gvnc)
{
	if (!gvnc_has_credentials(gvnc)) {
		GVNC_DEBUG("Requesting missing credentials\n");
		if (gvnc->has_error || !gvnc->ops.auth_cred) {
			gvnc->has_error = TRUE;
			return TRUE;
		}
		if (!gvnc->ops.auth_cred(gvnc->ops_data))
		    gvnc->has_error = TRUE;
		if (gvnc->has_error)
			return TRUE;
		GVNC_DEBUG("Waiting for missing credentials\n");
		g_condition_wait(gvnc_has_credentials, gvnc);
		GVNC_DEBUG("Got all credentials\n");
	}
	return !gvnc_has_error(gvnc);
}

static gboolean gvnc_has_auth_subtype(gpointer data)
{
	struct gvnc *gvnc = (struct gvnc *)data;

	if (gvnc->has_error)
		return TRUE;
	if (gvnc->auth_subtype == GVNC_AUTH_INVALID)
		return FALSE;
	return TRUE;
}


static gboolean gvnc_perform_auth_tls(struct gvnc *gvnc)
{
	unsigned int nauth, i;
	unsigned int auth[20];

	if (!gvnc_start_tls(gvnc, 1)) {
		GVNC_DEBUG("Could not start TLS\n");
		return FALSE;
	}
	GVNC_DEBUG("Completed TLS setup\n");

	nauth = gvnc_read_u8(gvnc);
	if (gvnc_has_error(gvnc))
		return FALSE;

	if (nauth == 0)
		return gvnc_check_auth_result(gvnc);

	if (nauth > sizeof(auth)) {
		GVNC_DEBUG("Too many (%d) auth types\n", nauth);
		gvnc->has_error = TRUE;
		return FALSE;
	}
	for (i = 0 ; i < nauth ; i++) {
		auth[i] = gvnc_read_u8(gvnc);
	}

	for (i = 0 ; i < nauth ; i++) {
		GVNC_DEBUG("Possible sub-auth %d\n", auth[i]);
	}

	if (gvnc->has_error || !gvnc->ops.auth_subtype)
		return FALSE;

	if (!gvnc->ops.auth_subtype(gvnc->ops_data, nauth, auth))
		gvnc->has_error = TRUE;
	if (gvnc->has_error)
		return FALSE;

	GVNC_DEBUG("Waiting for auth subtype\n");
	g_condition_wait(gvnc_has_auth_subtype, gvnc);
	if (gvnc->has_error)
		return FALSE;

	GVNC_DEBUG("Choose auth %d\n", gvnc->auth_subtype);

	if (!gvnc_gather_credentials(gvnc))
		return FALSE;

	gvnc_write_u8(gvnc, gvnc->auth_subtype);
	gvnc_flush(gvnc);

	switch (gvnc->auth_subtype) {
	case GVNC_AUTH_NONE:
		if (gvnc->minor == 8)
			return gvnc_check_auth_result(gvnc);
		return TRUE;
	case GVNC_AUTH_VNC:
		return gvnc_perform_auth_vnc(gvnc);
	default:
		return FALSE;
	}

	return TRUE;
}

static gboolean gvnc_perform_auth_vencrypt(struct gvnc *gvnc)
{
	int major, minor, status, anonTLS;
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

	if (gvnc->has_error || !gvnc->ops.auth_subtype)
		return FALSE;

	if (!gvnc->ops.auth_subtype(gvnc->ops_data, nauth, auth))
		gvnc->has_error = TRUE;
	if (gvnc->has_error)
		return FALSE;

	GVNC_DEBUG("Waiting for auth subtype\n");
	g_condition_wait(gvnc_has_auth_subtype, gvnc);
	if (gvnc->has_error)
		return FALSE;

	GVNC_DEBUG("Choose auth %d\n", gvnc->auth_subtype);

	if (!gvnc_gather_credentials(gvnc))
		return FALSE;

#if !DEBUG
	if (gvnc->auth_subtype == GVNC_AUTH_VENCRYPT_PLAIN) {
		GVNC_DEBUG("Cowardly refusing to transmit plain text password\n");
		return FALSE;
	}
#endif

	gvnc_write_u32(gvnc, gvnc->auth_subtype);
	gvnc_flush(gvnc);
	status = gvnc_read_u8(gvnc);
	if (status != 1) {
		GVNC_DEBUG("Server refused VeNCrypt auth %d %d\n", gvnc->auth_subtype, status);
		return FALSE;
	}

	switch (gvnc->auth_subtype) {
	case GVNC_AUTH_VENCRYPT_TLSNONE:
	case GVNC_AUTH_VENCRYPT_TLSPLAIN:
	case GVNC_AUTH_VENCRYPT_TLSVNC:
		anonTLS = 1;
		break;
	default:
		anonTLS = 0;
	}

	if (!gvnc_start_tls(gvnc, anonTLS)) {
		GVNC_DEBUG("Could not start TLS\n");
		return FALSE;
	}
	GVNC_DEBUG("Completed TLS setup\n");

	switch (gvnc->auth_subtype) {
		/* Plain certificate based auth */
	case GVNC_AUTH_VENCRYPT_TLSNONE:
	case GVNC_AUTH_VENCRYPT_X509NONE:
		GVNC_DEBUG("Completing auth\n");
		return gvnc_check_auth_result(gvnc);

		/* Regular VNC layered over TLS */
	case GVNC_AUTH_VENCRYPT_TLSVNC:
	case GVNC_AUTH_VENCRYPT_X509VNC:
		GVNC_DEBUG("Handing off to VNC auth\n");
		return gvnc_perform_auth_vnc(gvnc);

	default:
		return FALSE;
	}
}

static gboolean gvnc_has_auth_type(gpointer data)
{
	struct gvnc *gvnc = (struct gvnc *)data;

	if (gvnc->has_error)
		return TRUE;
	if (gvnc->auth_type == GVNC_AUTH_INVALID)
		return FALSE;
	return TRUE;
}

static gboolean gvnc_perform_auth(struct gvnc *gvnc)
{
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
		GVNC_DEBUG("Possible auth %u\n", auth[i]);
	}

	if (gvnc->has_error || !gvnc->ops.auth_type)
		return FALSE;

	if (!gvnc->ops.auth_type(gvnc->ops_data, nauth, auth))
		gvnc->has_error = TRUE;
	if (gvnc->has_error)
		return FALSE;

	GVNC_DEBUG("Waiting for auth type\n");
	g_condition_wait(gvnc_has_auth_type, gvnc);
	if (gvnc->has_error)
		return FALSE;

	GVNC_DEBUG("Choose auth %u\n", gvnc->auth_type);
	if (!gvnc_gather_credentials(gvnc))
		return FALSE;

	if (gvnc->minor > 6) {
		gvnc_write_u8(gvnc, gvnc->auth_type);
		gvnc_flush(gvnc);
	}

	switch (gvnc->auth_type) {
	case GVNC_AUTH_NONE:
		if (gvnc->minor == 8)
			return gvnc_check_auth_result(gvnc);
		return TRUE;
	case GVNC_AUTH_VNC:
		return gvnc_perform_auth_vnc(gvnc);

	case GVNC_AUTH_TLS:
		if (gvnc->minor < 7)
			return FALSE;
		return gvnc_perform_auth_tls(gvnc);

	case GVNC_AUTH_VENCRYPT:
		return gvnc_perform_auth_vencrypt(gvnc);

	default:
		if (gvnc->ops.auth_unsupported)
			gvnc->ops.auth_unsupported (gvnc->ops_data, gvnc->auth_type);
		gvnc->has_error = TRUE;

		return FALSE;
	}

	return TRUE;
}

struct gvnc *gvnc_new(const struct gvnc_ops *ops, gpointer ops_data)
{
	struct gvnc *gvnc = malloc(sizeof(*gvnc));
	if (gvnc == NULL)
		return NULL;

	memset(gvnc, 0, sizeof(*gvnc));
	gvnc->fd = -1;

	memcpy(&gvnc->ops, ops, sizeof(*ops));
	gvnc->ops_data = ops_data;
	gvnc->auth_type = GVNC_AUTH_INVALID;
	gvnc->auth_subtype = GVNC_AUTH_INVALID;

	return gvnc;
}

void gvnc_free(struct gvnc *gvnc)
{
	if (!gvnc)
		return;

	if (gvnc_is_open(gvnc))
		gvnc_close(gvnc);

	free(gvnc);
	gvnc = NULL;
}

void gvnc_close(struct gvnc *gvnc)
{
	int i;

	if (gvnc->tls_session) {
		gnutls_bye(gvnc->tls_session, GNUTLS_SHUT_RDWR);
		gvnc->tls_session = NULL;
	}
	if (gvnc->channel) {
		g_io_channel_unref(gvnc->channel);
		gvnc->channel = NULL;
	}
	if (gvnc->fd != -1) {
		close(gvnc->fd);
		gvnc->fd = -1;
	}

	if (gvnc->host) {
		free(gvnc->host);
		gvnc->host = NULL;
	}

	if (gvnc->port) {
		free(gvnc->port);
		gvnc->port = NULL;
	}

	if (gvnc->name) {
		free(gvnc->name);
		gvnc->name = NULL;
	}

	if (gvnc->cred_username) {
		free(gvnc->cred_username);
		gvnc->cred_username = NULL;
	}
	if (gvnc->cred_password) {
		free(gvnc->cred_password);
		gvnc->cred_password = NULL;
	}

	if (gvnc->cred_x509_cacert) {
		free(gvnc->cred_x509_cacert);
		gvnc->cred_x509_cacert = NULL;
	}
	if (gvnc->cred_x509_cacrl) {
		free(gvnc->cred_x509_cacrl);
		gvnc->cred_x509_cacrl = NULL;
	}
	if (gvnc->cred_x509_cert) {
		free(gvnc->cred_x509_cert);
		gvnc->cred_x509_cert = NULL;
	}
	if (gvnc->cred_x509_key) {
		free(gvnc->cred_x509_key);
		gvnc->cred_x509_key = NULL;
	}

	for (i = 0; i < 5; i++)
		inflateEnd(&gvnc->streams[i]);

	gvnc->auth_type = GVNC_AUTH_INVALID;
	gvnc->auth_subtype = GVNC_AUTH_INVALID;

	gvnc->has_error = 0;
}

void gvnc_shutdown(struct gvnc *gvnc)
{
	close(gvnc->fd);
	gvnc->fd = -1;
	gvnc->has_error = 1;
	GVNC_DEBUG("Waking up couroutine to shutdown gracefully\n");
	g_io_wakeup(&gvnc->wait);
}

gboolean gvnc_is_open(struct gvnc *gvnc)
{
	if (!gvnc)
		return FALSE;

	if (gvnc->fd != -1)
		return TRUE;
	if (gvnc->host)
		return TRUE;
	return FALSE;
}


gboolean gvnc_is_initialized(struct gvnc *gvnc)
{
	if (!gvnc_is_open(gvnc))
		return FALSE;
	if (gvnc->name)
		return TRUE;
	return FALSE;
}

gboolean gvnc_initialize(struct gvnc *gvnc, gboolean shared_flag)
{
	int ret, i;
	char version[13];
	uint32_t n_name;

	gvnc->absolute = 1;

	gvnc_read(gvnc, version, 12);
	version[12] = 0;

 	ret = sscanf(version, "RFB %03d.%03d\n", &gvnc->major, &gvnc->minor);
	if (ret != 2)
		goto fail;

	if (gvnc->major != 3)
		goto fail;
	if (gvnc->minor != 3 &&
	    gvnc->minor != 4 &&
	    gvnc->minor != 5 &&
	    gvnc->minor != 6 &&
	    gvnc->minor != 7 &&
	    gvnc->minor != 8)
		goto fail;

	/* For UltraVNC ... */
	if  (gvnc->minor > 3 && gvnc->minor < 7) gvnc->minor = 3;

	snprintf(version, 12, "RFB %03d.%03d\n", gvnc->major, gvnc->minor);
	gvnc_write(gvnc, version, 12);
	gvnc_flush(gvnc);
	GVNC_DEBUG("Negotiated protocol %d %d\n", gvnc->major, gvnc->minor);

	if (!gvnc_perform_auth(gvnc)) {
		GVNC_DEBUG("Auth failed\n");
		goto fail;
	}

	gvnc_write_u8(gvnc, shared_flag); /* shared flag */
	gvnc_flush(gvnc);
	gvnc->width = gvnc_read_u16(gvnc);
	gvnc->height = gvnc_read_u16(gvnc);

	gvnc_read_pixel_format(gvnc, &gvnc->fmt);

	n_name = gvnc_read_u32(gvnc);
	if (n_name > 4096)
		goto fail;

	gvnc->name = malloc(n_name + 1);
	if (gvnc->name == NULL)
		goto fail;

	gvnc_read(gvnc, gvnc->name, n_name);
	gvnc->name[n_name] = 0;
	GVNC_DEBUG("Display name '%s'\n", gvnc->name);

	memset(&gvnc->strm, 0, sizeof(gvnc->strm));
	/* FIXME what level? */
	for (i = 0; i < 5; i++)
		inflateInit(&gvnc->streams[i]);
	gvnc->strm = NULL;

	gvnc_resize(gvnc, gvnc->width, gvnc->height);
	return !gvnc_has_error(gvnc);

 fail:
	gvnc->has_error = 1;
	return !gvnc_has_error(gvnc);
}

gboolean gvnc_open_fd(struct gvnc *gvnc, int fd)
{
	int flags;
	if (gvnc_is_open(gvnc)) {
		GVNC_DEBUG ("Error: already connected?\n");
		return FALSE;
	}

	GVNC_DEBUG("Connecting to FD %d\n", fd);
	if ((flags = fcntl(fd, F_GETFL)) < 0) {
		GVNC_DEBUG ("Failed to fcntl()\n");
		return FALSE;
	}
	flags |= O_NONBLOCK;
	if (fcntl(fd, F_SETFL, flags) < 0) {
		GVNC_DEBUG ("Failed to fcntl()\n");
		return FALSE;
	}

	if (!(gvnc->channel = g_io_channel_unix_new(fd))) {
		GVNC_DEBUG ("Failed to g_io_channel_unix_new()\n");
		return FALSE;
	}
	gvnc->fd = fd;

	return !gvnc_has_error(gvnc);
}

gboolean gvnc_open_host(struct gvnc *gvnc, const char *host, const char *port)
{
        struct addrinfo *ai, *runp, hints;
        int ret;
	if (gvnc_is_open(gvnc))
		return FALSE;

	gvnc->host = g_strdup(host);
	gvnc->port = g_strdup(port);

        GVNC_DEBUG("Resolving host %s %s\n", host, port);
        memset (&hints, '\0', sizeof (hints));
        hints.ai_flags = AI_ADDRCONFIG;
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_protocol = IPPROTO_TCP;

        if ((ret = getaddrinfo(host, port, &hints, &ai)) != 0) {
		GVNC_DEBUG ("Failed to resolve hostname\n");
		return FALSE;
	}

        runp = ai;
        while (runp != NULL) {
                int flags, fd;
                GIOChannel *chan;

		if ((fd = socket(runp->ai_family, runp->ai_socktype,
			runp->ai_protocol)) < 0) {
			GVNC_DEBUG ("Failed to socket()\n");
			break;
		}

                GVNC_DEBUG("Trying socket %d\n", fd);
                if ((flags = fcntl(fd, F_GETFL)) < 0) {
                        close(fd);
                        GVNC_DEBUG ("Failed to fcntl()\n");
                        break;
                }
                flags |= O_NONBLOCK;
                if (fcntl(fd, F_SETFL, flags) < 0) {
                        close(fd);
                        GVNC_DEBUG ("Failed to fcntl()\n");
                        break;
                }

                if (!(chan = g_io_channel_unix_new(fd))) {
                        close(fd);
                        GVNC_DEBUG ("Failed to g_io_channel_unix_new()\n");
                        break;
                }

        reconnect:
                /* FIXME: Better handle EINPROGRESS/EISCONN return values,
                   as explained in connect(2) man page */
                if ( (connect(fd, runp->ai_addr, runp->ai_addrlen) == 0) || errno == EISCONN) {
                        gvnc->channel = chan;
                        gvnc->fd = fd;
                        freeaddrinfo(ai);
                        return !gvnc_has_error(gvnc);
                }

                if (errno == EINPROGRESS) {
                        g_io_wait(chan, G_IO_OUT|G_IO_ERR|G_IO_HUP);
                        goto reconnect;
                } else if (errno != ECONNREFUSED &&
                           errno != EHOSTUNREACH) {
                        g_io_channel_unref(chan);
                        close(fd);
                        GVNC_DEBUG ("Failed with errno = %d\n", errno);
                        break;
                }
                close(fd);
                g_io_channel_unref(chan);
                runp = runp->ai_next;
        }
        freeaddrinfo (ai);
	return FALSE;
}


gboolean gvnc_set_auth_type(struct gvnc *gvnc, unsigned int type)
{
        GVNC_DEBUG("Requested auth type %u\n", type);
        if (gvnc->auth_type != GVNC_AUTH_INVALID) {
                gvnc->has_error = TRUE;
                return !gvnc_has_error(gvnc);
        }
        if (type != GVNC_AUTH_NONE &&
            type != GVNC_AUTH_VNC &&
            type != GVNC_AUTH_TLS &&
            type != GVNC_AUTH_VENCRYPT) {
            	if (gvnc->ops.auth_unsupported)
					gvnc->ops.auth_unsupported (gvnc->ops_data, type);

                gvnc->has_error = TRUE;
                return !gvnc_has_error(gvnc);
        }
        gvnc->auth_type = type;
        gvnc->auth_subtype = GVNC_AUTH_INVALID;

	return !gvnc_has_error(gvnc);
}

gboolean gvnc_set_auth_subtype(struct gvnc *gvnc, unsigned int type)
{
        GVNC_DEBUG("Requested auth subtype %d\n", type);
        if (gvnc->auth_type != GVNC_AUTH_VENCRYPT &&
	    gvnc->auth_type != GVNC_AUTH_TLS) {
                gvnc->has_error = TRUE;
		return !gvnc_has_error(gvnc);
        }
        if (gvnc->auth_subtype != GVNC_AUTH_INVALID) {
                gvnc->has_error = TRUE;
		return !gvnc_has_error(gvnc);
        }
        gvnc->auth_subtype = type;

	return !gvnc_has_error(gvnc);
}

gboolean gvnc_set_credential_password(struct gvnc *gvnc, const char *password)
{
        GVNC_DEBUG("Set password credential\n");
        if (gvnc->cred_password)
                free(gvnc->cred_password);
        if (!(gvnc->cred_password = strdup(password))) {
                gvnc->has_error = TRUE;
                return FALSE;
        }
        return TRUE;
}

gboolean gvnc_set_credential_username(struct gvnc *gvnc, const char *username)
{
        GVNC_DEBUG("Set username credential %s\n", username);
        if (gvnc->cred_username)
                free(gvnc->cred_username);
        if (!(gvnc->cred_username = strdup(username))) {
                gvnc->has_error = TRUE;
                return FALSE;
        }
        return TRUE;
}

gboolean gvnc_set_credential_x509_cacert(struct gvnc *gvnc, const char *file)
{
        GVNC_DEBUG("Set x509 cacert %s\n", file);
        if (gvnc->cred_x509_cacert)
                free(gvnc->cred_x509_cacert);
        if (!(gvnc->cred_x509_cacert = strdup(file))) {
                gvnc->has_error = TRUE;
                return FALSE;
        }
        return TRUE;
}

gboolean gvnc_set_credential_x509_cacrl(struct gvnc *gvnc, const char *file)
{
        GVNC_DEBUG("Set x509 cacrl %s\n", file);
        if (gvnc->cred_x509_cacrl)
                free(gvnc->cred_x509_cacrl);
        if (!(gvnc->cred_x509_cacrl = strdup(file))) {
                gvnc->has_error = TRUE;
                return FALSE;
        }
        return TRUE;
}

gboolean gvnc_set_credential_x509_key(struct gvnc *gvnc, const char *file)
{
        GVNC_DEBUG("Set x509 key %s\n", file);
        if (gvnc->cred_x509_key)
                free(gvnc->cred_x509_key);
        if (!(gvnc->cred_x509_key = strdup(file))) {
                gvnc->has_error = TRUE;
                return FALSE;
        }
        return TRUE;
}

gboolean gvnc_set_credential_x509_cert(struct gvnc *gvnc, const char *file)
{
        GVNC_DEBUG("Set x509 cert %s\n", file);
        if (gvnc->cred_x509_cert)
                free(gvnc->cred_x509_cert);
        if (!(gvnc->cred_x509_cert = strdup(file))) {
                gvnc->has_error = TRUE;
                return FALSE;
        }
        return TRUE;
}


gboolean gvnc_set_local(struct gvnc *gvnc, struct gvnc_framebuffer *fb)
{
	int i, j, n;
	int depth;

	memcpy(&gvnc->local, fb, sizeof(*fb));

	if (fb->bpp == (gvnc->fmt.bits_per_pixel / 8) &&
	    fb->red_mask == gvnc->fmt.red_max &&
	    fb->green_mask == gvnc->fmt.green_max &&
	    fb->blue_mask == gvnc->fmt.blue_max &&
	    fb->red_shift == gvnc->fmt.red_shift &&
	    fb->green_shift == gvnc->fmt.green_shift &&
	    fb->blue_shift == gvnc->fmt.blue_shift &&
	    fb->byte_order == gvnc->fmt.byte_order)
		gvnc->perfect_match = TRUE;
	else
		gvnc->perfect_match = FALSE;

	depth = gvnc->fmt.depth;
	if (depth == 32)
		depth = 24;

	gvnc->rm = gvnc->local.red_mask & gvnc->fmt.red_max;
	gvnc->gm = gvnc->local.green_mask & gvnc->fmt.green_max;
	gvnc->bm = gvnc->local.blue_mask & gvnc->fmt.blue_max;
	GVNC_DEBUG("Mask local: %3d %3d %3d\n"
		   "    remote: %3d %3d %3d\n"
		   "    merged: %3d %3d %3d\n",
		   gvnc->local.red_mask, gvnc->local.green_mask, gvnc->local.blue_mask,
		   gvnc->fmt.red_max, gvnc->fmt.green_max, gvnc->fmt.blue_max,
		   gvnc->rm, gvnc->gm, gvnc->bm);

	/* Setup shifts assuming matched bpp (but not necessarily match rgb order)*/
	gvnc->rrs = gvnc->fmt.red_shift;
	gvnc->grs = gvnc->fmt.green_shift;
	gvnc->brs = gvnc->fmt.blue_shift;

	gvnc->rls = gvnc->local.red_shift;
	gvnc->gls = gvnc->local.green_shift;
	gvnc->bls = gvnc->local.blue_shift;


	/* This adjusts for server/client endianness mismatch */
	if (G_BYTE_ORDER != gvnc->fmt.byte_order) {
		gvnc->rrs = gvnc->fmt.bits_per_pixel - gvnc->rrs - (gvnc->fmt.bits_per_pixel - gvnc->fmt.depth);
		gvnc->grs = gvnc->fmt.bits_per_pixel - gvnc->grs - (gvnc->fmt.bits_per_pixel - gvnc->fmt.depth);
		gvnc->brs = gvnc->fmt.bits_per_pixel - gvnc->brs - (gvnc->fmt.bits_per_pixel - gvnc->fmt.depth);

		GVNC_DEBUG("Flipped shifts red: %3d, green: %3d, blue: %3d\n",
			   gvnc->rrs, gvnc->grs, gvnc->brs);
	}


	/* This adjusts for remote having more bpp than local */
	for (n = gvnc->fmt.red_max; n > gvnc->local.red_mask ; n>>= 1)
		gvnc->rrs++;
	for (n = gvnc->fmt.green_max; n > gvnc->local.green_mask ; n>>= 1)
		gvnc->grs++;
	for (n = gvnc->fmt.blue_max; n > gvnc->local.blue_mask ; n>>= 1)
		gvnc->brs++;

	/* This adjusts for remote having less bpp than remote */
	for (n = gvnc->local.red_mask ; n > gvnc->fmt.red_max ; n>>= 1)
		gvnc->rls++;
	for (n = gvnc->local.green_mask ; n > gvnc->fmt.green_max ; n>>= 1)
		gvnc->gls++;
	for (n = gvnc->local.blue_mask ; n > gvnc->fmt.blue_max ; n>>= 1)
		gvnc->bls++;
	GVNC_DEBUG("Pixel shifts\n   right: %3d %3d %3d\n    left: %3d %3d %3d\n",
		   gvnc->rrs, gvnc->grs, gvnc->brs,
		   gvnc->rls, gvnc->gls, gvnc->bls);

	i = gvnc->fmt.bits_per_pixel / 8;
	j = gvnc->local.bpp;

	if (i == 4) i = 3;
	if (j == 4) j = 3;

	gvnc->blt = gvnc_blt_table[i - 1][j - 1];
	gvnc->fill = gvnc_fill_table[i - 1][j - 1];
	gvnc->set_pixel_at = gvnc_set_pixel_at_table[i - 1][j - 1];
	gvnc->hextile = gvnc_hextile_table[i - 1][j - 1];
	gvnc->rich_cursor_blt = gvnc_rich_cursor_blt_table[i - 1];
	gvnc->rgb24_blt = gvnc_rgb24_blt_table[i - 1];
	gvnc->tight_compute_predicted = gvnc_tight_compute_predicted_table[i - 1];
	gvnc->tight_sum_pixel = gvnc_tight_sum_pixel_table[i - 1];

	if (gvnc->perfect_match)
		gvnc->blt = gvnc_blt_fast;

	return !gvnc_has_error(gvnc);
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

gboolean gvnc_using_raw_keycodes(struct gvnc *gvnc)
{
	return gvnc->has_ext_key_event;
}

/*
 * Local variables:
 *  c-indent-level: 8
 *  c-basic-offset: 8
 *  tab-width: 8
 * End:
 */
