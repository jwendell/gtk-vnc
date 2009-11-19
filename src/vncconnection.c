/*
 * GTK VNC Widget
 *
 * Copyright (C) 2006  Anthony Liguori <anthony@codemonkey.ws>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.0 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301 USA
 */

#include <config.h>

#include "vncconnection.h"

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include <string.h>
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

#if HAVE_SASL
#include <sasl/sasl.h>
#endif

#include <zlib.h>

#include <gdk/gdkkeysyms.h>

#include "getaddrinfo.h"
#include "dh.h"

/* AI_ADDRCONFIG is missing on some systems and gnulib won't provide it
   even if its emulated getaddrinfo() for us . */
#ifndef AI_ADDRCONFIG
# define AI_ADDRCONFIG 0
#endif


struct wait_queue
{
	gboolean waiting;
	struct coroutine *context;
};


typedef void vnc_connection_blt_func(VncConnection *conn, guint8 *, int, int, int, int, int);

typedef void vnc_connection_fill_func(VncConnection *conn, guint8 *, int, int, int, int);

typedef void vnc_connection_set_pixel_at_func(VncConnection *conn, int, int, guint8 *);

typedef void vnc_connection_hextile_func(VncConnection *conn, guint8 flags,
					  guint16 x, guint16 y,
					  guint16 width, guint16 height,
					  guint8 *fg, guint8 *bg);

typedef void vnc_connection_rich_cursor_blt_func(VncConnection *conn, guint8 *, guint8 *,
						  guint8 *, int, guint16, guint16);

typedef void vnc_connection_rgb24_blt_func(VncConnection *conn, int, int, int, int,
					    guint8 *, int);

typedef void vnc_connection_tight_compute_predicted_func(VncConnection *conn, guint8 *,
							  guint8 *, guint8 *,
							  guint8 *);

typedef void vnc_connection_tight_sum_pixel_func(VncConnection *conn, guint8 *, guint8 *);

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

struct _VncConnection
{
	GIOChannel *channel;
	int fd;
	char *host;
	char *port;
	VncPixelFormat fmt;
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
	gboolean want_cred_username;
	gboolean want_cred_password;
	gboolean want_cred_x509;

#if HAVE_SASL
	sasl_conn_t *saslconn;      /* SASL context */
	const char *saslDecoded;
	unsigned int saslDecodedLength;
	unsigned int saslDecodedOffset;
#endif

	char read_buffer[4096];
	size_t read_offset;
	size_t read_size;

	char write_buffer[4096];
	size_t write_offset;

	gboolean perfect_match;
	struct vnc_framebuffer local;

	int rm, gm, bm;
	int rrs, grs, brs;
	int rls, gls, bls;

	vnc_connection_blt_func *blt;
	vnc_connection_fill_func *fill;
	vnc_connection_set_pixel_at_func *set_pixel_at;
	vnc_connection_hextile_func *hextile;
	vnc_connection_rich_cursor_blt_func *rich_cursor_blt;
	vnc_connection_rgb24_blt_func *rgb24_blt;
	vnc_connection_tight_compute_predicted_func *tight_compute_predicted;
	vnc_connection_tight_sum_pixel_func *tight_sum_pixel;

	struct vnc_connection_ops ops;
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
	guint8 uncompressed_buffer[4096];

	size_t compressed_length;
	guint8 *compressed_buffer;

	guint8 zrle_pi;
	int zrle_pi_bits;

	gboolean has_ext_key_event;
	const guint8 const *keycode_map;
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
static gboolean g_condition_wait_check(GSource *src)
{
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

static gboolean vnc_connection_use_compression(VncConnection *conn)
{
	return conn->compressed_buffer != NULL;
}

static int vnc_connection_zread(VncConnection *conn, void *buffer, size_t size)
{
	char *ptr = buffer;
	size_t offset = 0;

	while (offset < size) {
		/* if data is available in the uncompressed buffer, then
		 * copy */
		if (conn->uncompressed_length) {
			size_t len = MIN(conn->uncompressed_length,
					 size - offset);

			memcpy(ptr + offset,
			       conn->uncompressed_buffer,
			       len);

			conn->uncompressed_length -= len;
			if (conn->uncompressed_length)
				memmove(conn->uncompressed_buffer,
					conn->uncompressed_buffer + len,
					conn->uncompressed_length);
			offset += len;
		} else {
			int err;

			conn->strm->next_in = conn->compressed_buffer;
			conn->strm->avail_in = conn->compressed_length;
			conn->strm->next_out = conn->uncompressed_buffer;
			conn->strm->avail_out = sizeof(conn->uncompressed_buffer);

			/* inflate as much as possible */
			err = inflate(conn->strm, Z_SYNC_FLUSH);
			if (err != Z_OK) {
				errno = EIO;
				return -1;
			}

			conn->uncompressed_length = (guint8 *)conn->strm->next_out - conn->uncompressed_buffer;
			conn->compressed_length -= (guint8 *)conn->strm->next_in - conn->compressed_buffer;
			conn->compressed_buffer = conn->strm->next_in;
		}
	}

	return offset;
}

/* IO functions */


/*
 * Read at least 1 more byte of data straight off the wire
 * into the requested buffer.
 */
static int vnc_connection_read_wire(VncConnection *conn, void *data, size_t len)
{
	int ret;

 reread:
	if (conn->tls_session) {
		ret = gnutls_read(conn->tls_session, data, len);
		if (ret < 0) {
			if (ret == GNUTLS_E_AGAIN)
				errno = EAGAIN;
			else
				errno = EIO;
			ret = -1;
		}
	} else
		ret = recv (conn->fd, data, len, 0);

	if (ret == -1) {
		switch (errno) {
		case EWOULDBLOCK:
			if (conn->wait_interruptable) {
				if (!g_io_wait_interruptable(&conn->wait,
							     conn->channel, G_IO_IN)) {
					GVNC_DEBUG("Read blocking interrupted %d", conn->has_error);
					return -EAGAIN;
				}
			} else
				g_io_wait(conn->channel, G_IO_IN);
		case EINTR:
			goto reread;

		default:
			GVNC_DEBUG("Closing the connection: vnc_connection_read() - errno=%d", errno);
			conn->has_error = TRUE;
			return -errno;
		}
	}
	if (ret == 0) {
		GVNC_DEBUG("Closing the connection: vnc_connection_read() - ret=0");
		conn->has_error = TRUE;
		return -EPIPE;
	}
	//GVNC_DEBUG("Read wire %p %d -> %d", data, len, ret);

	return ret;
}


#if HAVE_SASL
/*
 * Read at least 1 more byte of data out of the SASL decrypted
 * data buffer, into the internal read buffer
 */
static int vnc_connection_read_sasl(VncConnection *conn)
{
	size_t want;
	//GVNC_DEBUG("Read SASL %p size %d offset %d", conn->saslDecoded,
	//	   conn->saslDecodedLength, conn->saslDecodedOffset);
	if (conn->saslDecoded == NULL) {
		char encoded[8192];
		int encodedLen = sizeof(encoded);
		int err, ret;

		ret = vnc_connection_read_wire(conn, encoded, encodedLen);
		if (ret < 0) {
			return ret;
		}

		err = sasl_decode(conn->saslconn, encoded, ret,
				  &conn->saslDecoded, &conn->saslDecodedLength);
		if (err != SASL_OK) {
			GVNC_DEBUG("Failed to decode SASL data %s",
				   sasl_errstring(err, NULL, NULL));
			conn->has_error = TRUE;
			return -EINVAL;
		}
		conn->saslDecodedOffset = 0;
	}

	want = conn->saslDecodedLength - conn->saslDecodedOffset;
	if (want > sizeof(conn->read_buffer))
		want = sizeof(conn->read_buffer);

	memcpy(conn->read_buffer,
	       conn->saslDecoded + conn->saslDecodedOffset,
	       want);
	conn->saslDecodedOffset += want;
	if (conn->saslDecodedOffset == conn->saslDecodedLength) {
		conn->saslDecodedLength = conn->saslDecodedOffset = 0;
		conn->saslDecoded = NULL;
	}
	//GVNC_DEBUG("Done read write %d - %d", want, conn->has_error);
	return want;
}
#endif


/*
 * Read at least 1 more byte of data straight off the wire
 * into the internal read buffer
 */
static int vnc_connection_read_plain(VncConnection *conn)
{
	//GVNC_DEBUG("Read plain %d", sizeof(conn->read_buffer));
	return vnc_connection_read_wire(conn, conn->read_buffer, sizeof(conn->read_buffer));
}

/*
 * Read at least 1 more byte of data into the internal read_buffer
 */
static int vnc_connection_read_buf(VncConnection *conn)
{
	//GVNC_DEBUG("Start read %d", conn->has_error);
#if HAVE_SASL
	if (conn->saslconn)
		return vnc_connection_read_sasl(conn);
	else
#endif
		return vnc_connection_read_plain(conn);
}

/*
 * Fill the 'data' buffer up with exactly 'len' bytes worth of data
 */
static int vnc_connection_read(VncConnection *conn, void *data, size_t len)
{
	char *ptr = data;
	size_t offset = 0;

	if (conn->has_error) return -EINVAL;

	while (offset < len) {
		size_t tmp;

		/* compressed data is buffered independently of the read buffer
		 * so we must by-pass it */
		if (vnc_connection_use_compression(conn)) {
			int ret = vnc_connection_zread(conn, ptr + offset, len);
			if (ret == -1) {
				GVNC_DEBUG("Closing the connection: vnc_connection_read() - zread() failed");
				conn->has_error = TRUE;
				return -errno;
			}
			offset += ret;
			continue;
		} else if (conn->read_offset == conn->read_size) {
			int ret = vnc_connection_read_buf(conn);

			if (ret < 0)
				return ret;
			conn->read_offset = 0;
			conn->read_size = ret;
		}

		tmp = MIN(conn->read_size - conn->read_offset, len - offset);

		memcpy(ptr + offset, conn->read_buffer + conn->read_offset, tmp);

		conn->read_offset += tmp;
		offset += tmp;
	}

	return 0;
}

/*
 * Write all 'data' of length 'datalen' bytes out to
 * the wire
 */
static void vnc_connection_flush_wire(VncConnection *conn,
				      const void *data,
				      size_t datalen)
{
	const char *ptr = data;
	size_t offset = 0;
	//GVNC_DEBUG("Flush write %p %d", data, datalen);
	while (offset < datalen) {
		int ret;

		if (conn->tls_session) {
			ret = gnutls_write(conn->tls_session,
					   ptr+offset,
					   datalen-offset);
			if (ret < 0) {
				if (ret == GNUTLS_E_AGAIN)
					errno = EAGAIN;
				else
					errno = EIO;
				ret = -1;
			}
		} else
			ret = send (conn->fd,
				    ptr+offset,
				    datalen-offset, 0);
		if (ret == -1) {
			switch (errno) {
			case EWOULDBLOCK:
				g_io_wait(conn->channel, G_IO_OUT);
			case EINTR:
				continue;
			default:
				GVNC_DEBUG("Closing the connection: vnc_connection_flush %d", errno);
				conn->has_error = TRUE;
				return;
			}
		}
		if (ret == 0) {
			GVNC_DEBUG("Closing the connection: vnc_connection_flush");
			conn->has_error = TRUE;
			return;
		}
		offset += ret;
	}
}


#if HAVE_SASL
/*
 * Encode all buffered data, write all encrypted data out
 * to the wire
 */
static void vnc_connection_flush_sasl(VncConnection *conn)
{
	const char *output;
	unsigned int outputlen;
	int err;

	err = sasl_encode(conn->saslconn,
			  conn->write_buffer,
			  conn->write_offset,
			  &output, &outputlen);
	if (err != SASL_OK) {
		GVNC_DEBUG("Failed to encode SASL data %s",
			   sasl_errstring(err, NULL, NULL));
		conn->has_error = TRUE;
		return;
	}
	//GVNC_DEBUG("Flush SASL %d: %p %d", conn->write_offset, output, outputlen);
	vnc_connection_flush_wire(conn, output, outputlen);
}
#endif

/*
 * Write all buffered data straight out to the wire
 */
static void vnc_connection_flush_plain(VncConnection *conn)
{
	//GVNC_DEBUG("Flush plain %d", conn->write_offset);
	vnc_connection_flush_wire(conn,
				  conn->write_buffer,
				  conn->write_offset);
}


/*
 * Write all buffered data out to the wire
 */
static void vnc_connection_flush(VncConnection *conn)
{
	//GVNC_DEBUG("STart write %d", conn->has_error);
#if HAVE_SASL
	if (conn->saslconn)
		vnc_connection_flush_sasl(conn);
	else
#endif
		vnc_connection_flush_plain(conn);
	conn->write_offset = 0;
}

static void vnc_connection_write(VncConnection *conn, const void *data, size_t len)
{
	const char *ptr = data;
	size_t offset = 0;

	while (offset < len) {
		ssize_t tmp;

		if (conn->write_offset == sizeof(conn->write_buffer)) {
			vnc_connection_flush(conn);
		}

		tmp = MIN(sizeof(conn->write_buffer), len - offset);

		memcpy(conn->write_buffer+conn->write_offset, ptr + offset, tmp);

		conn->write_offset += tmp;
		offset += tmp;
	}
}


static ssize_t vnc_connection_tls_push(gnutls_transport_ptr_t transport,
				       const void *data,
				       size_t len) {
	VncConnection *conn = transport;
	int ret;

 retry:
	ret = write(conn->fd, data, len);
	if (ret < 0) {
		if (errno == EINTR)
			goto retry;
		return -1;
	}
	return ret;
}


static ssize_t vnc_connection_tls_pull(gnutls_transport_ptr_t transport,
				       void *data,
				       size_t len) {
	VncConnection *conn = transport;
	int ret;

 retry:
	ret = read(conn->fd, data, len);
	if (ret < 0) {
		if (errno == EINTR)
			goto retry;
		return -1;
	}
	return ret;
}

static size_t vnc_connection_pixel_size(VncConnection *conn)
{
	return conn->fmt.bits_per_pixel / 8;
}

static void vnc_connection_read_pixel(VncConnection *conn, guint8 *pixel)
{
	vnc_connection_read(conn, pixel, vnc_connection_pixel_size(conn));
}

static guint8 vnc_connection_read_u8(VncConnection *conn)
{
	guint8 value = 0;
	vnc_connection_read(conn, &value, sizeof(value));
	return value;
}

static int vnc_connection_read_u8_interruptable(VncConnection *conn, guint8 *value)
{
	int ret;

	conn->wait_interruptable = 1;
	ret = vnc_connection_read(conn, value, sizeof(*value));
	conn->wait_interruptable = 0;

	return ret;
}

static guint16 vnc_connection_read_u16(VncConnection *conn)
{
	guint16 value = 0;
	vnc_connection_read(conn, &value, sizeof(value));
	return ntohs(value);
}

static guint32 vnc_connection_read_u32(VncConnection *conn)
{
	guint32 value = 0;
	vnc_connection_read(conn, &value, sizeof(value));
	return ntohl(value);
}

static gint32 vnc_connection_read_s32(VncConnection *conn)
{
	gint32 value = 0;
	vnc_connection_read(conn, &value, sizeof(value));
	return ntohl(value);
}

static void vnc_connection_write_u8(VncConnection *conn, guint8 value)
{
	vnc_connection_write(conn, &value, sizeof(value));
}

static void vnc_connection_write_u16(VncConnection *conn, guint16 value)
{
	value = htons(value);
	vnc_connection_write(conn, &value, sizeof(value));
}

static void vnc_connection_write_u32(VncConnection *conn, guint32 value)
{
	value = htonl(value);
	vnc_connection_write(conn, &value, sizeof(value));
}

static void vnc_connection_write_s32(VncConnection *conn, gint32 value)
{
	value = htonl(value);
	vnc_connection_write(conn, &value, sizeof(value));
}

#define DH_BITS 1024
static gnutls_dh_params_t dh_params;

#if 0
static void vnc_connection_debug_gnutls_log(int level, const char* str) {
	GVNC_DEBUG("%d %s", level, str);
}
#endif

static int gvnc_tls_mutex_init (void **priv)
{                                                                             \
    GMutex *lock = NULL;
    lock = g_mutex_new();
    *priv = lock;
    return 0;
}

static int gvnc_tls_mutex_destroy(void **priv)
{
    GMutex *lock = *priv;
    g_mutex_free(lock);
    return 0;
}

static int gvnc_tls_mutex_lock(void **priv)
{
    GMutex *lock = *priv;
    g_mutex_lock(lock);
    return 0;
}

static int gvnc_tls_mutex_unlock(void **priv)
{
    GMutex *lock = *priv;
    g_mutex_unlock(lock);
    return 0;
}

static struct gcry_thread_cbs gvnc_thread_impl = {
    (GCRY_THREAD_OPTION_PTHREAD | (GCRY_THREAD_OPTION_VERSION << 8)),
    NULL,
    gvnc_tls_mutex_init,
    gvnc_tls_mutex_destroy,
    gvnc_tls_mutex_lock,
    gvnc_tls_mutex_unlock,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL
};


static gboolean vnc_connection_tls_initialize(void)
{
	static int tlsinitialized = 0;

	if (tlsinitialized)
		return TRUE;

	if (g_thread_supported()) {
		gcry_control(GCRYCTL_SET_THREAD_CBS, &gvnc_thread_impl);
		gcry_check_version(NULL);
	}

	if (gnutls_global_init () < 0)
		return FALSE;

	if (gnutls_dh_params_init (&dh_params) < 0)
		return FALSE;
	if (gnutls_dh_params_generate2 (dh_params, DH_BITS) < 0)
		return FALSE;

#if 0
	if (debug_enabled) {
		gnutls_global_set_log_level(10);
		gnutls_global_set_log_function(vnc_connection_debug_gnutls_log);
	}
#endif

	tlsinitialized = TRUE;

	return TRUE;
}

static gnutls_anon_client_credentials vnc_connection_tls_initialize_anon_cred(void)
{
	gnutls_anon_client_credentials anon_cred;
	int ret;

	if ((ret = gnutls_anon_allocate_client_credentials(&anon_cred)) < 0) {
		GVNC_DEBUG("Cannot allocate credentials %s", gnutls_strerror(ret));
		return NULL;
	}

	return anon_cred;
}

static gnutls_certificate_credentials_t vnc_connection_tls_initialize_cert_cred(VncConnection *conn)
{
	gnutls_certificate_credentials_t x509_cred;
	int ret;

	if ((ret = gnutls_certificate_allocate_credentials(&x509_cred)) < 0) {
		GVNC_DEBUG("Cannot allocate credentials %s", gnutls_strerror(ret));
		return NULL;
	}
	if (conn->cred_x509_cacert) {
		if ((ret = gnutls_certificate_set_x509_trust_file(x509_cred,
								  conn->cred_x509_cacert,
								  GNUTLS_X509_FMT_PEM)) < 0) {
			GVNC_DEBUG("Cannot load CA certificate %s", gnutls_strerror(ret));
			return NULL;
		}
	} else {
		GVNC_DEBUG("No CA certificate provided");
		return NULL;
	}

	if (conn->cred_x509_cert && conn->cred_x509_key) {
		if ((ret = gnutls_certificate_set_x509_key_file (x509_cred,
								 conn->cred_x509_cert,
								 conn->cred_x509_key,
								 GNUTLS_X509_FMT_PEM)) < 0) {
			GVNC_DEBUG("Cannot load certificate & key %s", gnutls_strerror(ret));
			return NULL;
		}
	} else {
		GVNC_DEBUG("No client cert or key provided");
	}

	if (conn->cred_x509_cacrl) {
		if ((ret = gnutls_certificate_set_x509_crl_file(x509_cred,
								conn->cred_x509_cacrl,
								GNUTLS_X509_FMT_PEM)) < 0) {
			GVNC_DEBUG("Cannot load CRL %s", gnutls_strerror(ret));
			return NULL;
		}
	} else {
		GVNC_DEBUG("No CA revocation list provided");
	}

	gnutls_certificate_set_dh_params (x509_cred, dh_params);

	return x509_cred;
}

static int vnc_connection_validate_certificate(VncConnection *conn)
{
	int ret;
	unsigned int status;
	const gnutls_datum_t *certs;
	unsigned int nCerts, i;
	time_t now;

	GVNC_DEBUG("Validating");
	if ((ret = gnutls_certificate_verify_peers2 (conn->tls_session, &status)) < 0) {
		GVNC_DEBUG("Verify failed %s", gnutls_strerror(ret));
		return FALSE;
	}

	if ((now = time(NULL)) == ((time_t)-1)) {
		return FALSE;
	}

	if (status != 0) {
		if (status & GNUTLS_CERT_INVALID)
			GVNC_DEBUG ("The certificate is not trusted.");

		if (status & GNUTLS_CERT_SIGNER_NOT_FOUND)
			GVNC_DEBUG ("The certificate hasn't got a known issuer.");

		if (status & GNUTLS_CERT_REVOKED)
			GVNC_DEBUG ("The certificate has been revoked.");

		if (status & GNUTLS_CERT_INSECURE_ALGORITHM)
			GVNC_DEBUG ("The certificate uses an insecure algorithm");

		return FALSE;
	} else {
		GVNC_DEBUG("Certificate is valid.");
	}

	if (gnutls_certificate_type_get(conn->tls_session) != GNUTLS_CRT_X509)
		return FALSE;

	if (!(certs = gnutls_certificate_get_peers(conn->tls_session, &nCerts)))
		return FALSE;

	for (i = 0 ; i < nCerts ; i++) {
		gnutls_x509_crt_t cert;
		GVNC_DEBUG ("Checking chain %d", i);
		if (gnutls_x509_crt_init (&cert) < 0)
			return FALSE;

		if (gnutls_x509_crt_import(cert, &certs[i], GNUTLS_X509_FMT_DER) < 0) {
			gnutls_x509_crt_deinit (cert);
			return FALSE;
		}

		if (gnutls_x509_crt_get_expiration_time (cert) < now) {
			GVNC_DEBUG("The certificate has expired");
			gnutls_x509_crt_deinit (cert);
			return FALSE;
		}

		if (gnutls_x509_crt_get_activation_time (cert) > now) {
			GVNC_DEBUG("The certificate is not yet activated");
			gnutls_x509_crt_deinit (cert);
			return FALSE;
		}

		if (gnutls_x509_crt_get_activation_time (cert) > now) {
			GVNC_DEBUG("The certificate is not yet activated");
			gnutls_x509_crt_deinit (cert);
			return FALSE;
		}

		if (i == 0) {
			if (!conn->host) {
				GVNC_DEBUG ("No hostname provided for certificate verification");
				gnutls_x509_crt_deinit (cert);
				return FALSE;
			}
			if (!gnutls_x509_crt_check_hostname (cert, conn->host)) {
				GVNC_DEBUG ("The certificate's owner does not match hostname '%s'",
					    conn->host);
				gnutls_x509_crt_deinit (cert);
				return FALSE;
			}
		}
	}

	return TRUE;
}


static void vnc_connection_read_pixel_format(VncConnection *conn, VncPixelFormat *fmt)
{
	guint8 pad[3];

	fmt->bits_per_pixel  = vnc_connection_read_u8(conn);
	fmt->depth           = vnc_connection_read_u8(conn);
	fmt->byte_order      = vnc_connection_read_u8(conn) ? G_BIG_ENDIAN : G_LITTLE_ENDIAN;
	fmt->true_color_flag = vnc_connection_read_u8(conn);

	fmt->red_max         = vnc_connection_read_u16(conn);
	fmt->green_max       = vnc_connection_read_u16(conn);
	fmt->blue_max        = vnc_connection_read_u16(conn);

	fmt->red_shift       = vnc_connection_read_u8(conn);
	fmt->green_shift     = vnc_connection_read_u8(conn);
	fmt->blue_shift      = vnc_connection_read_u8(conn);

	vnc_connection_read(conn, pad, 3);

	GVNC_DEBUG("Pixel format BPP: %d,  Depth: %d, Byte order: %d, True color: %d\n"
		   "             Mask  red: %3d, green: %3d, blue: %3d\n"
		   "             Shift red: %3d, green: %3d, blue: %3d",
		   fmt->bits_per_pixel, fmt->depth, fmt->byte_order, fmt->true_color_flag,
		   fmt->red_max, fmt->green_max, fmt->blue_max,
		   fmt->red_shift, fmt->green_shift, fmt->blue_shift);
}

/* initialize function */

gboolean vnc_connection_has_error(VncConnection *conn)
{
	return conn->has_error;
}

gboolean vnc_connection_set_pixel_format(VncConnection *conn,
					 const VncPixelFormat *fmt)
{
	guint8 pad[3] = {0};

	vnc_connection_write_u8(conn, 0);
	vnc_connection_write(conn, pad, 3);

	vnc_connection_write_u8(conn, fmt->bits_per_pixel);
	vnc_connection_write_u8(conn, fmt->depth);
	vnc_connection_write_u8(conn, fmt->byte_order == G_BIG_ENDIAN ? 1 : 0);
	vnc_connection_write_u8(conn, fmt->true_color_flag);

	vnc_connection_write_u16(conn, fmt->red_max);
	vnc_connection_write_u16(conn, fmt->green_max);
	vnc_connection_write_u16(conn, fmt->blue_max);

	vnc_connection_write_u8(conn, fmt->red_shift);
	vnc_connection_write_u8(conn, fmt->green_shift);
	vnc_connection_write_u8(conn, fmt->blue_shift);

	vnc_connection_write(conn, pad, 3);
	vnc_connection_flush(conn);

	if (&conn->fmt != fmt)
		memcpy(&conn->fmt, fmt, sizeof(*fmt));

	return !vnc_connection_has_error(conn);
}


gboolean vnc_connection_set_encodings(VncConnection *conn, int n_encoding, gint32 *encoding)
{
	guint8 pad[1] = {0};
	int i, skip_zrle=0;

	/*
	 * RealVNC server is broken for ZRLE in some pixel formats.
	 * Specifically if you have a format with either R, G or B
	 * components with a max value > 255, it still uses a CPIXEL
	 * of 3 bytes, even though the colour requirs 4 bytes. It
	 * thus messes up the colours of the server in a way we can't
	 * recover from on the client. Most VNC clients don't see this
	 * problem since they send a 'set pixel format' message instead
	 * of running with the server's default format.
	 *
	 * So we kill off ZRLE encoding for problematic pixel formats
	 */
	for (i = 0; i < n_encoding; i++)
		if (conn->fmt.depth == 32 &&
		    (conn->fmt.red_max > 255 ||
		     conn->fmt.blue_max > 255 ||
		     conn->fmt.green_max > 255) &&
		    encoding[i] == GVNC_ENCODING_ZRLE) {
			GVNC_DEBUG("Dropping ZRLE encoding for broken pixel format");
			skip_zrle++;
		}

	conn->has_ext_key_event = FALSE;
	vnc_connection_write_u8(conn, 2);
	vnc_connection_write(conn, pad, 1);
	vnc_connection_write_u16(conn, n_encoding - skip_zrle);
	for (i = 0; i < n_encoding; i++) {
		if (skip_zrle && encoding[i] == GVNC_ENCODING_ZRLE)
			continue;
		vnc_connection_write_s32(conn, encoding[i]);
	}
	vnc_connection_flush(conn);
	return !vnc_connection_has_error(conn);
}


gboolean vnc_connection_framebuffer_update_request(VncConnection *conn,
						   guint8 incremental,
						   guint16 x, guint16 y,
						   guint16 width, guint16 height)
{
	vnc_connection_write_u8(conn, 3);
	vnc_connection_write_u8(conn, incremental);
	vnc_connection_write_u16(conn, x);
	vnc_connection_write_u16(conn, y);
	vnc_connection_write_u16(conn, width);
	vnc_connection_write_u16(conn, height);
	vnc_connection_flush(conn);
	return !vnc_connection_has_error(conn);
}

static void vnc_connection_buffered_write(VncConnection *conn, const void *data, size_t size)
{
	size_t left;

	left = conn->xmit_buffer_capacity - conn->xmit_buffer_size;
	if (left < size) {
		conn->xmit_buffer_capacity += size + 4095;
		conn->xmit_buffer_capacity &= ~4095;

		conn->xmit_buffer = g_realloc(conn->xmit_buffer, conn->xmit_buffer_capacity);
	}

	memcpy(&conn->xmit_buffer[conn->xmit_buffer_size],
	       data, size);

	conn->xmit_buffer_size += size;
}

static void vnc_connection_buffered_write_u8(VncConnection *conn, guint8 value)
{
	vnc_connection_buffered_write(conn, &value, 1);
}

static void vnc_connection_buffered_write_u16(VncConnection *conn, guint16 value)
{
	value = htons(value);
	vnc_connection_buffered_write(conn, &value, 2);
}

static void vnc_connection_buffered_write_u32(VncConnection *conn, guint32 value)
{
	value = htonl(value);
	vnc_connection_buffered_write(conn, &value, 4);
}

static void vnc_connection_buffered_flush(VncConnection *conn)
{
	g_io_wakeup(&conn->wait);
}

gboolean vnc_connection_key_event(VncConnection *conn, guint8 down_flag,
				  guint32 key, guint16 scancode)
{
	guint8 pad[2] = {0};

	GVNC_DEBUG("Key event %d %d %d %d", key, scancode, down_flag, conn->has_ext_key_event);
	if (conn->has_ext_key_event) {
		scancode = x_keycode_to_pc_keycode(conn->keycode_map, scancode);

		vnc_connection_buffered_write_u8(conn, 255);
		vnc_connection_buffered_write_u8(conn, 0);
		vnc_connection_buffered_write_u16(conn, down_flag);
		vnc_connection_buffered_write_u32(conn, key);
		vnc_connection_buffered_write_u32(conn, scancode);
	} else {
		vnc_connection_buffered_write_u8(conn, 4);
		vnc_connection_buffered_write_u8(conn, down_flag);
		vnc_connection_buffered_write(conn, pad, 2);
		vnc_connection_buffered_write_u32(conn, key);
	}

	vnc_connection_buffered_flush(conn);
	return !vnc_connection_has_error(conn);
}

gboolean vnc_connection_pointer_event(VncConnection *conn, guint8 button_mask,
				      guint16 x, guint16 y)
{
	vnc_connection_buffered_write_u8(conn, 5);
	vnc_connection_buffered_write_u8(conn, button_mask);
	vnc_connection_buffered_write_u16(conn, x);
	vnc_connection_buffered_write_u16(conn, y);
	vnc_connection_buffered_flush(conn);
	return !vnc_connection_has_error(conn);
}

gboolean vnc_connection_client_cut_text(VncConnection *conn,
					const void *data, size_t length)
{
	guint8 pad[3] = {0};

	vnc_connection_buffered_write_u8(conn, 6);
	vnc_connection_buffered_write(conn, pad, 3);
	vnc_connection_buffered_write_u32(conn, length);
	vnc_connection_buffered_write(conn, data, length);
	vnc_connection_buffered_flush(conn);
	return !vnc_connection_has_error(conn);
}

static inline guint8 *vnc_connection_get_local(VncConnection *conn, int x, int y)
{
	return conn->local.data +
		(y * conn->local.linesize) +
		(x * conn->local.bpp);
}

static guint8 vnc_connection_swap_img_8(VncConnection *conn G_GNUC_UNUSED, guint8 pixel)
{
	return pixel;
}

static guint8 vnc_connection_swap_rfb_8(VncConnection *conn G_GNUC_UNUSED, guint8 pixel)
{
	return pixel;
}

/* local host native format -> X server image format */
static guint16 vnc_connection_swap_img_16(VncConnection *conn, guint16 pixel)
{
	if (G_BYTE_ORDER != conn->local.byte_order)
		return  (((pixel >> 8) & 0xFF) << 0) |
			(((pixel >> 0) & 0xFF) << 8);
	else
		return pixel;
}

/* VNC server RFB  format ->  local host native format */
static guint16 vnc_connection_swap_rfb_16(VncConnection *conn, guint16 pixel)
{
	if (conn->fmt.byte_order != G_BYTE_ORDER)
		return  (((pixel >> 8) & 0xFF) << 0) |
			(((pixel >> 0) & 0xFF) << 8);
	else
		return pixel;
}

/* local host native format -> X server image format */
static guint32 vnc_connection_swap_img_32(VncConnection *conn, guint32 pixel)
{
	if (G_BYTE_ORDER != conn->local.byte_order)
		return  (((pixel >> 24) & 0xFF) <<  0) |
			(((pixel >> 16) & 0xFF) <<  8) |
			(((pixel >>  8) & 0xFF) << 16) |
			(((pixel >>  0) & 0xFF) << 24);
	else
		return pixel;
}

/* VNC server RFB  format ->  local host native format */
static guint32 vnc_connection_swap_rfb_32(VncConnection *conn, guint32 pixel)
{
	if (conn->fmt.byte_order != G_BYTE_ORDER)
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

static vnc_connection_blt_func *vnc_connection_blt_table[3][3] = {
	{  vnc_connection_blt_8x8,  vnc_connection_blt_8x16,  vnc_connection_blt_8x32 },
	{ vnc_connection_blt_16x8, vnc_connection_blt_16x16, vnc_connection_blt_16x32 },
	{ vnc_connection_blt_32x8, vnc_connection_blt_32x16, vnc_connection_blt_32x32 },
};

static vnc_connection_hextile_func *vnc_connection_hextile_table[3][3] = {
	{ (vnc_connection_hextile_func *)vnc_connection_hextile_8x8,
	  (vnc_connection_hextile_func *)vnc_connection_hextile_8x16,
	  (vnc_connection_hextile_func *)vnc_connection_hextile_8x32 },
	{ (vnc_connection_hextile_func *)vnc_connection_hextile_16x8,
	  (vnc_connection_hextile_func *)vnc_connection_hextile_16x16,
	  (vnc_connection_hextile_func *)vnc_connection_hextile_16x32 },
	{ (vnc_connection_hextile_func *)vnc_connection_hextile_32x8,
	  (vnc_connection_hextile_func *)vnc_connection_hextile_32x16,
	  (vnc_connection_hextile_func *)vnc_connection_hextile_32x32 },
};

static vnc_connection_set_pixel_at_func *vnc_connection_set_pixel_at_table[3][3] = {
	{ (vnc_connection_set_pixel_at_func *)vnc_connection_set_pixel_at_8x8,
	  (vnc_connection_set_pixel_at_func *)vnc_connection_set_pixel_at_8x16,
	  (vnc_connection_set_pixel_at_func *)vnc_connection_set_pixel_at_8x32 },
	{ (vnc_connection_set_pixel_at_func *)vnc_connection_set_pixel_at_16x8,
	  (vnc_connection_set_pixel_at_func *)vnc_connection_set_pixel_at_16x16,
	  (vnc_connection_set_pixel_at_func *)vnc_connection_set_pixel_at_16x32 },
	{ (vnc_connection_set_pixel_at_func *)vnc_connection_set_pixel_at_32x8,
	  (vnc_connection_set_pixel_at_func *)vnc_connection_set_pixel_at_32x16,
	  (vnc_connection_set_pixel_at_func *)vnc_connection_set_pixel_at_32x32 },
};

static vnc_connection_fill_func *vnc_connection_fill_table[3][3] = {
	{ (vnc_connection_fill_func *)vnc_connection_fill_8x8,
	  (vnc_connection_fill_func *)vnc_connection_fill_8x16,
	  (vnc_connection_fill_func *)vnc_connection_fill_8x32 },
	{ (vnc_connection_fill_func *)vnc_connection_fill_16x8,
	  (vnc_connection_fill_func *)vnc_connection_fill_16x16,
	  (vnc_connection_fill_func *)vnc_connection_fill_16x32 },
	{ (vnc_connection_fill_func *)vnc_connection_fill_32x8,
	  (vnc_connection_fill_func *)vnc_connection_fill_32x16,
	  (vnc_connection_fill_func *)vnc_connection_fill_32x32 },
};

static vnc_connection_rich_cursor_blt_func *vnc_connection_rich_cursor_blt_table[3] = {
	vnc_connection_rich_cursor_blt_8x32,
	vnc_connection_rich_cursor_blt_16x32,
	vnc_connection_rich_cursor_blt_32x32,
};

static vnc_connection_rgb24_blt_func *vnc_connection_rgb24_blt_table[3] = {
	(vnc_connection_rgb24_blt_func *)vnc_connection_rgb24_blt_32x8,
	(vnc_connection_rgb24_blt_func *)vnc_connection_rgb24_blt_32x16,
	(vnc_connection_rgb24_blt_func *)vnc_connection_rgb24_blt_32x32,
};

static vnc_connection_tight_compute_predicted_func *vnc_connection_tight_compute_predicted_table[3] = {
	(vnc_connection_tight_compute_predicted_func *)vnc_connection_tight_compute_predicted_8x8,
	(vnc_connection_tight_compute_predicted_func *)vnc_connection_tight_compute_predicted_16x16,
	(vnc_connection_tight_compute_predicted_func *)vnc_connection_tight_compute_predicted_32x32,
};

static vnc_connection_tight_sum_pixel_func *vnc_connection_tight_sum_pixel_table[3] = {
	(vnc_connection_tight_sum_pixel_func *)vnc_connection_tight_sum_pixel_8x8,
	(vnc_connection_tight_sum_pixel_func *)vnc_connection_tight_sum_pixel_16x16,
	(vnc_connection_tight_sum_pixel_func *)vnc_connection_tight_sum_pixel_32x32,
};

/* a fast blit for the perfect match scenario */
static void vnc_connection_blt_fast(VncConnection *conn, guint8 *src, int pitch,
				    int x, int y, int width, int height)
{
	guint8 *dst = vnc_connection_get_local(conn, x, y);
	int i;
	for (i = 0; i < height; i++) {
		memcpy(dst, src, width * conn->local.bpp);
		dst += conn->local.linesize;
		src += pitch;
	}
}

static void vnc_connection_blt(VncConnection *conn, guint8 *src, int pitch,
			       int x, int y, int width, int height)
{
	conn->blt(conn, src, pitch, x, y, width, height);
}

static void vnc_connection_raw_update(VncConnection *conn,
				      guint16 x, guint16 y,
				      guint16 width, guint16 height)
{
	guint8 *dst;
	int i;

	/* optimize for perfect match between server/client
	   FWIW, in the local case, we ought to be doing a write
	   directly from the source framebuffer and a read directly
	   into the client framebuffer
	*/
	if (conn->perfect_match) {
		dst = vnc_connection_get_local(conn, x, y);
		for (i = 0; i < height; i++) {
			vnc_connection_read(conn, dst, width * conn->local.bpp);
			dst += conn->local.linesize;
		}
		return;
	}

	dst = g_malloc(width * (conn->fmt.bits_per_pixel / 8));
	for (i = 0; i < height; i++) {
		vnc_connection_read(conn, dst, width * (conn->fmt.bits_per_pixel / 8));
		vnc_connection_blt(conn, dst, 0, x, y + i, width, 1);
	}
	g_free(dst);
}

static void vnc_connection_copyrect_update(VncConnection *conn,
					   guint16 dst_x, guint16 dst_y,
					   guint16 width, guint16 height)
{
	int src_x, src_y;
	guint8 *dst, *src;
	int pitch = conn->local.linesize;
	int i;

	src_x = vnc_connection_read_u16(conn);
	src_y = vnc_connection_read_u16(conn);

	if (src_y < dst_y) {
		pitch = -pitch;
		src_y += (height - 1);
		dst_y += (height - 1);
	}

	dst = vnc_connection_get_local(conn, dst_x, dst_y);
	src = vnc_connection_get_local(conn, src_x, src_y);
	for (i = 0; i < height; i++) {
		memmove(dst, src, width * conn->local.bpp);
		dst += pitch;
		src += pitch;
	}
}

static void vnc_connection_hextile_update(VncConnection *conn,
					  guint16 x, guint16 y,
					  guint16 width, guint16 height)
{
	guint8 fg[4];
	guint8 bg[4];

	int j;
	for (j = 0; j < height; j += 16) {
		int i;
		for (i = 0; i < width; i += 16) {
			guint8 flags;
			int w = MIN(16, width - i);
			int h = MIN(16, height - j);

			flags = vnc_connection_read_u8(conn);
			conn->hextile(conn, flags, x + i, y + j, w, h, fg, bg);
		}
	}
}

static void vnc_connection_fill(VncConnection *conn, guint8 *color,
				guint16 x, guint16 y, guint16 width, guint16 height)
{
	conn->fill(conn, color, x, y, width, height);
}

static void vnc_connection_set_pixel_at(VncConnection *conn, int x, int y, guint8 *pixel)
{
	conn->set_pixel_at(conn, x, y, pixel);
}

static void vnc_connection_rre_update(VncConnection *conn,
				      guint16 x, guint16 y,
				      guint16 width, guint16 height)
{
	guint8 bg[4];
	guint32 num;
	guint32 i;

	num = vnc_connection_read_u32(conn);
	vnc_connection_read_pixel(conn, bg);
	vnc_connection_fill(conn, bg, x, y, width, height);

	for (i = 0; i < num; i++) {
		guint8 fg[4];
		guint16 sub_x, sub_y, sub_w, sub_h;

		vnc_connection_read_pixel(conn, fg);
		sub_x = vnc_connection_read_u16(conn);
		sub_y = vnc_connection_read_u16(conn);
		sub_w = vnc_connection_read_u16(conn);
		sub_h = vnc_connection_read_u16(conn);

		vnc_connection_fill(conn, fg,
				    x + sub_x, y + sub_y, sub_w, sub_h);
	}
}

/* CPIXELs are optimized slightly.  32-bit pixel values are packed into 24-bit
 * values. */
static void vnc_connection_read_cpixel(VncConnection *conn, guint8 *pixel)
{
	int bpp = vnc_connection_pixel_size(conn);

	memset(pixel, 0, bpp);

	if (bpp == 4 && conn->fmt.true_color_flag) {
		int fitsInMSB = ((conn->fmt.red_shift > 7) &&
				 (conn->fmt.green_shift > 7) &&
				 (conn->fmt.blue_shift > 7));
		int fitsInLSB = (((conn->fmt.red_max << conn->fmt.red_shift) < (1 << 24)) &&
				 ((conn->fmt.green_max << conn->fmt.green_shift) < (1 << 24)) &&
				 ((conn->fmt.blue_max << conn->fmt.blue_shift) < (1 << 24)));

		/*
		 * We need to analyse the shifts to see if they fit in 3 bytes,
		 * rather than looking at the declared  'depth' for the format
		 * because despite what the RFB spec says, this is what RealVNC
		 * server actually does in practice.
		 */
		if (fitsInMSB || fitsInLSB) {
			bpp = 3;
			if (conn->fmt.depth == 24 &&
			    conn->fmt.byte_order == G_BIG_ENDIAN)
				pixel++;
		}
	}

	vnc_connection_read(conn, pixel, bpp);
}

static void vnc_connection_zrle_update_tile_blit(VncConnection *conn,
						 guint16 x, guint16 y,
						 guint16 width, guint16 height)
{
	guint8 blit_data[4 * 64 * 64];
	int i, bpp;

	bpp = vnc_connection_pixel_size(conn);

	for (i = 0; i < width * height; i++)
		vnc_connection_read_cpixel(conn, blit_data + (i * bpp));

	vnc_connection_blt(conn, blit_data, width * bpp, x, y, width, height);
}

static guint8 vnc_connection_read_zrle_pi(VncConnection *conn, int palette_size)
{
	guint8 pi = 0;

	if (conn->zrle_pi_bits == 0) {
		conn->zrle_pi = vnc_connection_read_u8(conn);
		conn->zrle_pi_bits = 8;
	}
	if ( palette_size == 2) {
		pi = (conn->zrle_pi >> (conn->zrle_pi_bits - 1)) & 1;
		conn->zrle_pi_bits -= 1;
	} else if ((palette_size == 3) || (palette_size == 4)) {
		pi = (conn->zrle_pi >> (conn->zrle_pi_bits - 2)) & 3;
		conn->zrle_pi_bits -= 2;
	} else if ((palette_size >=5) && (palette_size <=16)){
		pi = (conn->zrle_pi >> (conn->zrle_pi_bits - 4)) & 15;
		conn->zrle_pi_bits -= 4;
	}

	return pi;
}

static void vnc_connection_zrle_update_tile_palette(VncConnection *conn,
						    guint8 palette_size,
						    guint16 x, guint16 y,
						    guint16 width, guint16 height)
{
	guint8 palette[128][4];
	int i, j;

	for (i = 0; i < palette_size; i++)
		vnc_connection_read_cpixel(conn, palette[i]);

	for (j = 0; j < height; j++) {
		/* discard any padding bits */
		conn->zrle_pi_bits = 0;

		for (i = 0; i < width; i++) {
			int ind = vnc_connection_read_zrle_pi(conn, palette_size);

			vnc_connection_set_pixel_at(conn, x + i, y + j,
						    palette[ind & 0x7F]);
		}
	}
}

static int vnc_connection_read_zrle_rl(VncConnection *conn)
{
	int rl = 1;
	guint8 b;

	do {
		b = vnc_connection_read_u8(conn);
		rl += b;
	} while (!vnc_connection_has_error(conn) && b == 255);

	return rl;
}

static void vnc_connection_zrle_update_tile_rle(VncConnection *conn,
						guint16 x, guint16 y,
						guint16 width, guint16 height)
{
	int i, j, rl = 0;
	guint8 pixel[4];

	for (j = 0; j < height; j++) {
		for (i = 0; i < width; i++) {
			if (rl == 0) {
				vnc_connection_read_cpixel(conn, pixel);
				rl = vnc_connection_read_zrle_rl(conn);
			}
			vnc_connection_set_pixel_at(conn, x + i, y + j, pixel);
			rl -= 1;
		}
	}
}

static void vnc_connection_zrle_update_tile_prle(VncConnection *conn,
						 guint8 palette_size,
						 guint16 x, guint16 y,
						 guint16 width, guint16 height)
{
	int i, j, rl = 0;
	guint8 palette[128][4];
	guint8 pi = 0;

	for (i = 0; i < palette_size; i++)
		vnc_connection_read_cpixel(conn, palette[i]);

	for (j = 0; j < height; j++) {
		for (i = 0; i < width; i++) {
			if (rl == 0) {
				pi = vnc_connection_read_u8(conn);
				if (pi & 0x80) {
					rl = vnc_connection_read_zrle_rl(conn);
					pi &= 0x7F;
				} else
					rl = 1;
			}

			vnc_connection_set_pixel_at(conn, x + i, y + j, palette[pi]);
			rl -= 1;
		}
	}
}

static void vnc_connection_zrle_update_tile(VncConnection *conn, guint16 x, guint16 y,
					    guint16 width, guint16 height)
{
	guint8 subencoding = vnc_connection_read_u8(conn);
	guint8 pixel[4];

	if (subencoding == 0 ) {
		/* Raw pixel data */
		vnc_connection_zrle_update_tile_blit(conn, x, y, width, height);
	} else if (subencoding == 1) {
		/* Solid tile of a single color */
		vnc_connection_read_cpixel(conn, pixel);
		vnc_connection_fill(conn, pixel, x, y, width, height);
	} else if ((subencoding >= 2) && (subencoding <= 16)) {
		/* Packed palette types */
		vnc_connection_zrle_update_tile_palette(conn, subencoding,
							x, y, width, height);
	} else if ((subencoding >= 17) && (subencoding <= 127)) {
		/* FIXME raise error? */
	} else if (subencoding == 128) {
		/* Plain RLE */
		vnc_connection_zrle_update_tile_rle(conn, x, y, width, height);
	} else if (subencoding == 129) {

	} else if (subencoding >= 130) {
		/* Palette RLE */
		vnc_connection_zrle_update_tile_prle(conn, subencoding - 128,
						     x, y, width, height);
	}
}

static void vnc_connection_zrle_update(VncConnection *conn,
				       guint16 x, guint16 y,
				       guint16 width, guint16 height)
{
	guint32 length;
	guint32 offset;
	guint16 i, j;
	guint8 *zlib_data;

	length = vnc_connection_read_u32(conn);
	zlib_data = g_malloc(length);
	vnc_connection_read(conn, zlib_data, length);

	/* setup subsequent calls to vnc_connection_read*() to use the compressed data */
	conn->uncompressed_length = 0;
	conn->compressed_length = length;
	conn->compressed_buffer = zlib_data;
	conn->strm = &conn->streams[0];

	offset = 0;
	for (j = 0; j < height; j += 64) {
		for (i = 0; i < width; i += 64) {
			guint16 w, h;

			w = MIN(width - i, 64);
			h = MIN(height - j, 64);
			vnc_connection_zrle_update_tile(conn, x + i, y + j, w, h);
		}
	}

	conn->strm = NULL;
	conn->uncompressed_length = 0;
	conn->compressed_length = 0;
	conn->compressed_buffer = NULL;

	g_free(zlib_data);
}

static void vnc_connection_rgb24_blt(VncConnection *conn, int x, int y,
				     int width, int height, guint8 *data, int pitch)
{
	conn->rgb24_blt(conn, x, y, width, height, data, pitch);
}

static guint32 vnc_connection_read_cint(VncConnection *conn)
{
	guint32 value = 0;
	guint8 val;

	val = vnc_connection_read_u8(conn);
	value = (val & 0x7F);
	if (!(val & 0x80))
		return value;

	val = vnc_connection_read_u8(conn);
	value |= (val & 0x7F) << 7;

	if (!(val & 0x80))
		return value;

	value |= vnc_connection_read_u8(conn) << 14;

	return value;
}

static int vnc_connection_tpixel_size(VncConnection *conn)
{
	if (conn->fmt.depth == 24)
		return 3;
	return conn->fmt.bits_per_pixel / 8;
}

static void vnc_connection_read_tpixel(VncConnection *conn, guint8 *pixel)
{
	if (conn->fmt.depth == 24) {
		uint32_t val;
		vnc_connection_read(conn, pixel, 3);
		val = (pixel[0] << conn->fmt.red_shift)
			| (pixel[1] << conn->fmt.green_shift)
			| (pixel[2] << conn->fmt.blue_shift);

		if (conn->fmt.byte_order != G_BYTE_ORDER)
			val =   (((val >>  0) & 0xFF) << 24) |
				(((val >>  8) & 0xFF) << 16) |
				(((val >> 16) & 0xFF) << 8) |
				(((val >> 24) & 0xFF) << 0);

		memcpy(pixel, &val, 4);
	} else
		vnc_connection_read_pixel(conn, pixel);
}

static void vnc_connection_tight_update_copy(VncConnection *conn,
					     guint16 x, guint16 y,
					     guint16 width, guint16 height)
{
	guint8 pixel[4];
	int i, j;

	for (j = 0; j < height; j++) {
		for (i = 0; i < width; i++) {
			vnc_connection_read_tpixel(conn, pixel);
			vnc_connection_set_pixel_at(conn, x + i, y + j, pixel);
		}
	}
}

static int vnc_connection_tight_get_pi(VncConnection *conn, guint8 *ra,
				       int i, guint8 palette_size)
{
	if (palette_size == 2) {
		if ((i % 8) == 0)
			*ra = vnc_connection_read_u8(conn);
		return (*ra >> (7 - (i % 8))) & 1;
	}

	return vnc_connection_read_u8(conn);
}

static void vnc_connection_tight_update_palette(VncConnection *conn,
						int palette_size, guint8 *palette,
						guint16 x, guint16 y,
						guint16 width, guint16 height)
{
	int i, j;

	for (j = 0; j < height; j++) {
		guint8 ra = 0;

		for (i = 0; i < width; i++) {
			guint8 ind;

			ind = vnc_connection_tight_get_pi(conn, &ra, i, palette_size);
			vnc_connection_set_pixel_at(conn, x + i, y + j,
						    &palette[ind * 4]);
		}
	}
}

static void vnc_connection_tight_compute_predicted(VncConnection *conn, guint8 *ppixel,
						   guint8 *lp, guint8 *cp,
						   guint8 *llp)
{
	conn->tight_compute_predicted(conn, ppixel, lp, cp, llp);
}

static void vnc_connection_tight_sum_pixel(VncConnection *conn,
					   guint8 *lhs, guint8 *rhs)
{
	conn->tight_sum_pixel(conn, lhs, rhs);
}

static void vnc_connection_tight_update_gradient(VncConnection *conn,
						 guint16 x, guint16 y,
						 guint16 width, guint16 height)
{
	int i, j;
	guint8 zero_pixel[4];
	guint8 *last_row, *row;
	int bpp;

	bpp = vnc_connection_pixel_size(conn);
	last_row = g_malloc(width * bpp);
	row = g_malloc(width * bpp);

	memset(last_row, 0, width * bpp);
	memset(zero_pixel, 0, 4);

	for (j = 0; j < height; j++) {
		guint8 *tmp_row;
		guint8 *llp, *lp;

		/* use zero pixels for the edge cases */
		llp = zero_pixel;
		lp = zero_pixel;

		for (i = 0; i < width; i++) {
			guint8 predicted_pixel[4];

			/* compute predicted pixel value */
			vnc_connection_tight_compute_predicted(conn, predicted_pixel,
							       lp, last_row + i * bpp,
							       llp);

			/* read the difference pixel from the wire */
			vnc_connection_read_tpixel(conn, row + i * bpp);

			/* sum the predicted pixel and the difference to get
			 * the original pixel value */
			vnc_connection_tight_sum_pixel(conn, row + i * bpp,
						       predicted_pixel);

			llp = last_row + i * bpp;
			lp = row + i * bpp;
		}

		/* write out row of pixel data */
		vnc_connection_blt(conn, row, width * bpp, x, y + j, width, 1);

		/* swap last row and current row */
		tmp_row = last_row;
		last_row = row;
		row = tmp_row;
	}

	g_free(row);
	g_free(last_row);
}

static void jpeg_draw(void *opaque, int x, int y, int w, int h,
		      guint8 *data, int stride)
{
	VncConnection *conn = opaque;

	vnc_connection_rgb24_blt(conn, x, y, w, h, data, stride);
}

static void vnc_connection_tight_update_jpeg(VncConnection *conn, guint16 x, guint16 y,
					     guint16 width, guint16 height,
					     guint8 *data, size_t length)
{
	if (conn->ops.render_jpeg == NULL)
		return;

	conn->ops.render_jpeg(conn->ops_data, jpeg_draw, conn,
			      x, y, width, height, data, length);
}

static void vnc_connection_tight_update(VncConnection *conn,
					guint16 x, guint16 y,
					guint16 width, guint16 height)
{
	guint8 ccontrol;
	guint8 pixel[4];
	int i;

	ccontrol = vnc_connection_read_u8(conn);

	for (i = 0; i < 4; i++) {
		if (ccontrol & (1 << i)) {
			inflateEnd(&conn->streams[i + 1]);
			inflateInit(&conn->streams[i + 1]);
		}
	}

	ccontrol >>= 4;
	ccontrol &= 0x0F;

	if (ccontrol <= 7) {
		/* basic */
		guint8 filter_id = 0;
		guint32 data_size, zlib_length;
		guint8 *zlib_data = NULL;
		guint8 palette[256][4];
		int palette_size = 0;

		if (ccontrol & 0x04)
			filter_id = vnc_connection_read_u8(conn);

		conn->strm = &conn->streams[(ccontrol & 0x03) + 1];

		if (filter_id == 1) {
			palette_size = vnc_connection_read_u8(conn);
			palette_size += 1;
			for (i = 0; i < palette_size; i++)
				vnc_connection_read_tpixel(conn, palette[i]);
		}

		if (filter_id == 1) {
			if (palette_size == 2)
				data_size = ((width + 7) / 8) * height;
			else
				data_size = width * height;
		} else
			data_size = width * height * vnc_connection_tpixel_size(conn);

		if (data_size >= 12) {
			zlib_length = vnc_connection_read_cint(conn);
			zlib_data = g_malloc(zlib_length);

			vnc_connection_read(conn, zlib_data, zlib_length);

			conn->uncompressed_length = 0;
			conn->compressed_length = zlib_length;
			conn->compressed_buffer = zlib_data;
		}

		switch (filter_id) {
		case 0: /* copy */
			vnc_connection_tight_update_copy(conn, x, y, width, height);
			break;
		case 1: /* palette */
			vnc_connection_tight_update_palette(conn, palette_size,
							    (guint8 *)palette,
							    x, y, width, height);
			break;
		case 2: /* gradient */
			vnc_connection_tight_update_gradient(conn, x, y, width, height);
			break;
		default: /* error */
			GVNC_DEBUG("Closing the connection: vnc_connection_tight_update() - filter_id unknown");
			conn->has_error = TRUE;
			break;
		}

		if (data_size >= 12) {
			conn->uncompressed_length = 0;
			conn->compressed_length = 0;
			conn->compressed_buffer = NULL;

			g_free(zlib_data);
		}

		conn->strm = NULL;
	} else if (ccontrol == 8) {
		/* fill */
		/* FIXME check each width; endianness */
		vnc_connection_read_tpixel(conn, pixel);
		vnc_connection_fill(conn, pixel, x, y, width, height);
	} else if (ccontrol == 9) {
		/* jpeg */
		guint32 length;
		guint8 *jpeg_data;

		length = vnc_connection_read_cint(conn);
		jpeg_data = g_malloc(length);
		vnc_connection_read(conn, jpeg_data, length);
		vnc_connection_tight_update_jpeg(conn, x, y, width, height,
						 jpeg_data, length);
		g_free(jpeg_data);
	} else {
		/* error */
		GVNC_DEBUG("Closing the connection: vnc_connection_tight_update() - ccontrol unknown");
		conn->has_error = TRUE;
	}
}

static void vnc_connection_update(VncConnection *conn, int x, int y, int width, int height)
{
	if (conn->has_error || !conn->ops.update)
		return;
	if (!conn->ops.update(conn->ops_data, x, y, width, height)) {
		GVNC_DEBUG("Closing the connection: vnc_connection_update");
		conn->has_error = TRUE;
	}
}

static void vnc_connection_set_color_map_entry(VncConnection *conn, guint16 color,
					       guint16 red, guint16 green,
					       guint16 blue)
{
	if (conn->has_error || !conn->ops.set_color_map_entry)
		return;
	if (!conn->ops.set_color_map_entry(conn->ops_data, color,
					   red, green, blue)) {
		GVNC_DEBUG("Closing the connection: vnc_connection_set_color_map_entry");
		conn->has_error = TRUE;
	}
}

static void vnc_connection_bell(VncConnection *conn)
{
	if (conn->has_error || !conn->ops.bell)
		return;

	GVNC_DEBUG("Server beep");

	if (!conn->ops.bell(conn->ops_data)) {
		GVNC_DEBUG("Closing the connection: vnc_connection_bell");
		conn->has_error = TRUE;
	}
}

static void vnc_connection_server_cut_text(VncConnection *conn, const void *data,
					   size_t len)
{
	if (conn->has_error || !conn->ops.server_cut_text)
		return;

	if (!conn->ops.server_cut_text(conn->ops_data, data, len)) {
		GVNC_DEBUG("Closing the connection: vnc_connection_server_cut_text");
		conn->has_error = TRUE;
	}
}

static void vnc_connection_resize(VncConnection *conn, int width, int height)
{
	if (conn->has_error)
		return;

	conn->width = width;
	conn->height = height;

	if (!conn->ops.resize)
		return;

	if (!conn->ops.resize(conn->ops_data, width, height)) {
		GVNC_DEBUG("Closing the connection: vnc_connection_resize");
		conn->has_error = TRUE;
	}
}

static void vnc_connection_pixel_format(VncConnection *conn)
{
        if (conn->has_error || !conn->ops.pixel_format)
                return;
        if (!conn->ops.pixel_format(conn->ops_data, &conn->fmt))
                conn->has_error = TRUE;
}

static void vnc_connection_pointer_type_change(VncConnection *conn, int absolute)
{
	if (conn->has_error || !conn->ops.pointer_type_change)
		return;
	if (!conn->ops.pointer_type_change(conn->ops_data, absolute)) {
		GVNC_DEBUG("Closing the connection: vnc_connection_pointer_type_change");
		conn->has_error = TRUE;
	}
}

static void vnc_connection_rich_cursor_blt(VncConnection *conn, guint8 *pixbuf,
					   guint8 *image, guint8 *mask,
					   int pitch, guint16 width, guint16 height)
{
	conn->rich_cursor_blt(conn, pixbuf, image, mask, pitch, width, height);
}

static void vnc_connection_rich_cursor(VncConnection *conn, int x, int y, int width, int height)
{
	guint8 *pixbuf = NULL;

	if (width && height) {
		guint8 *image, *mask;
		int imagelen, masklen;

		imagelen = width * height * (conn->fmt.bits_per_pixel / 8);
		masklen = ((width + 7)/8) * height;

		image = g_malloc(imagelen);
		mask = g_malloc(masklen);
		pixbuf = g_malloc(width * height * 4); /* RGB-A 8bit */

		vnc_connection_read(conn, image, imagelen);
		vnc_connection_read(conn, mask, masklen);

		vnc_connection_rich_cursor_blt(conn, pixbuf, image, mask,
					       width * (conn->fmt.bits_per_pixel/8),
					       width, height);

		g_free(image);
		g_free(mask);
	}

	if (conn->has_error || !conn->ops.local_cursor)
		return;
	if (!conn->ops.local_cursor(conn->ops_data, x, y, width, height, pixbuf)) {
		GVNC_DEBUG("Closing the connection: vnc_connection_rich_cursor() - !ops.local_cursor()");
		conn->has_error = TRUE;
	}

	g_free(pixbuf);
}

static void vnc_connection_xcursor(VncConnection *conn, int x, int y, int width, int height)
{
	guint8 *pixbuf = NULL;

	if (width && height) {
		guint8 *data, *mask, *datap, *maskp;
		guint32 *pixp;
		int rowlen;
		int x1, y1;
		guint8 fgrgb[3], bgrgb[3];
		guint32 fg, bg;
		vnc_connection_read(conn, fgrgb, 3);
		vnc_connection_read(conn, bgrgb, 3);
		fg = (255 << 24) | (fgrgb[0] << 16) | (fgrgb[1] << 8) | fgrgb[2];
		bg = (255 << 24) | (bgrgb[0] << 16) | (bgrgb[1] << 8) | bgrgb[2];

		rowlen = ((width + 7)/8);
		data = g_malloc(rowlen*height);
		mask = g_malloc(rowlen*height);
		pixbuf = g_malloc(width * height * 4); /* RGB-A 8bit */

		vnc_connection_read(conn, data, rowlen*height);
		vnc_connection_read(conn, mask, rowlen*height);
		datap = data;
		maskp = mask;
		pixp = (guint32*)pixbuf;
		for (y1 = 0; y1 < height; y1++) {
			for (x1 = 0; x1 < width; x1++) {
				*pixp++ = ((maskp[x1 / 8] >> (7-(x1 % 8))) & 1) ?
					(((datap[x1 / 8] >> (7-(x1 % 8))) & 1) ? fg : bg) : 0;
			}
			datap += rowlen;
			maskp += rowlen;
		}
		g_free(data);
		g_free(mask);
	}

	if (conn->has_error || !conn->ops.local_cursor)
		return;
	if (!conn->ops.local_cursor(conn->ops_data, x, y, width, height, pixbuf)) {
		GVNC_DEBUG("Closing the connection: vnc_connection_xcursor() - !ops.local_cursor()");
		conn->has_error = TRUE;
	}

	g_free(pixbuf);
}

static void vnc_connection_ext_key_event(VncConnection *conn)
{
	conn->has_ext_key_event = TRUE;
	conn->keycode_map = x_keycode_to_pc_keycode_map();
}

static void vnc_connection_framebuffer_update(VncConnection *conn, gint32 etype,
					      guint16 x, guint16 y,
					      guint16 width, guint16 height)
{
	GVNC_DEBUG("FramebufferUpdate(%d, %d, %d, %d, %d)",
		   etype, x, y, width, height);

	switch (etype) {
	case GVNC_ENCODING_RAW:
		vnc_connection_raw_update(conn, x, y, width, height);
		vnc_connection_update(conn, x, y, width, height);
		break;
	case GVNC_ENCODING_COPY_RECT:
		vnc_connection_copyrect_update(conn, x, y, width, height);
		vnc_connection_update(conn, x, y, width, height);
		break;
	case GVNC_ENCODING_RRE:
		vnc_connection_rre_update(conn, x, y, width, height);
		vnc_connection_update(conn, x, y, width, height);
		break;
	case GVNC_ENCODING_HEXTILE:
		vnc_connection_hextile_update(conn, x, y, width, height);
		vnc_connection_update(conn, x, y, width, height);
		break;
	case GVNC_ENCODING_ZRLE:
		vnc_connection_zrle_update(conn, x, y, width, height);
		vnc_connection_update(conn, x, y, width, height);
		break;
	case GVNC_ENCODING_TIGHT:
		vnc_connection_tight_update(conn, x, y, width, height);
		vnc_connection_update(conn, x, y, width, height);
		break;
	case GVNC_ENCODING_DESKTOP_RESIZE:
		vnc_connection_framebuffer_update_request (conn, 0, 0, 0, width, height);
		vnc_connection_resize(conn, width, height);
		break;
	case GVNC_ENCODING_POINTER_CHANGE:
		vnc_connection_pointer_type_change(conn, x);
		break;
        case GVNC_ENCODING_WMVi:
                vnc_connection_read_pixel_format(conn, &conn->fmt);
                vnc_connection_pixel_format(conn);
                break;
	case GVNC_ENCODING_RICH_CURSOR:
		vnc_connection_rich_cursor(conn, x, y, width, height);
		break;
	case GVNC_ENCODING_XCURSOR:
		vnc_connection_xcursor(conn, x, y, width, height);
		break;
	case GVNC_ENCODING_EXT_KEY_EVENT:
		vnc_connection_ext_key_event(conn);
		break;
	default:
		GVNC_DEBUG("Received an unknown encoding type: %d", etype);
		conn->has_error = TRUE;
		break;
	}
}

gboolean vnc_connection_server_message(VncConnection *conn)
{
	guint8 msg;
	int ret;

	/* NB: make sure that all server message functions
	   handle has_error appropriately */

	do {
		if (conn->xmit_buffer_size) {
			vnc_connection_write(conn, conn->xmit_buffer, conn->xmit_buffer_size);
			vnc_connection_flush(conn);
			conn->xmit_buffer_size = 0;
		}
	} while ((ret = vnc_connection_read_u8_interruptable(conn, &msg)) == -EAGAIN);

	if (ret < 0) {
		GVNC_DEBUG("Aborting message processing on error");
		return !vnc_connection_has_error(conn);
	}

	switch (msg) {
	case 0: { /* FramebufferUpdate */
		guint8 pad[1];
		guint16 n_rects;
		int i;

		vnc_connection_read(conn, pad, 1);
		n_rects = vnc_connection_read_u16(conn);
		for (i = 0; i < n_rects; i++) {
			guint16 x, y, w, h;
			gint32 etype;

			x = vnc_connection_read_u16(conn);
			y = vnc_connection_read_u16(conn);
			w = vnc_connection_read_u16(conn);
			h = vnc_connection_read_u16(conn);
			etype = vnc_connection_read_s32(conn);

			vnc_connection_framebuffer_update(conn, etype, x, y, w, h);
		}
	}	break;
	case 1: { /* SetColorMapEntries */
		guint16 first_color;
		guint16 n_colors;
		guint8 pad[1];
		int i;

		vnc_connection_read(conn, pad, 1);
		first_color = vnc_connection_read_u16(conn);
		n_colors = vnc_connection_read_u16(conn);

		for (i = 0; i < n_colors; i++) {
			guint16 red, green, blue;

			red = vnc_connection_read_u16(conn);
			green = vnc_connection_read_u16(conn);
			blue = vnc_connection_read_u16(conn);

			vnc_connection_set_color_map_entry(conn,
							   i + first_color,
							   red, green, blue);
		}
	}	break;
	case 2: /* Bell */
		vnc_connection_bell(conn);
		break;
	case 3: { /* ServerCutText */
		guint8 pad[3];
		guint32 n_text;
		char *data;

		vnc_connection_read(conn, pad, 3);
		n_text = vnc_connection_read_u32(conn);
		if (n_text > (32 << 20)) {
			GVNC_DEBUG("Closing the connection: vnc_connection_server_message() - cutText > allowed");
			conn->has_error = TRUE;
			break;
		}

		data = g_new(char, n_text + 1);
		if (data == NULL) {
			GVNC_DEBUG("Closing the connection: vnc_connection_server_message() - cutText - !data");
			conn->has_error = TRUE;
			break;
		}

		vnc_connection_read(conn, data, n_text);
		data[n_text] = 0;

		vnc_connection_server_cut_text(conn, data, n_text);
		g_free(data);
	}	break;
	default:
		GVNC_DEBUG("Received an unknown message: %u", msg);
		conn->has_error = TRUE;
		break;
	}

	return !vnc_connection_has_error(conn);
}

gboolean vnc_connection_wants_credential_password(VncConnection *conn)
{
	return conn->want_cred_password;
}

gboolean vnc_connection_wants_credential_username(VncConnection *conn)
{
	return conn->want_cred_username;
}

gboolean vnc_connection_wants_credential_x509(VncConnection *conn)
{
	return conn->want_cred_x509;
}

static gboolean vnc_connection_has_credentials(gpointer data)
{
	VncConnection *conn = data;

	if (conn->has_error)
		return TRUE;
	if (vnc_connection_wants_credential_username(conn) && !conn->cred_username)
		return FALSE;
	if (vnc_connection_wants_credential_password(conn) && !conn->cred_password)
		return FALSE;
	/*
	 * For x509 we require a minimum of the CA cert.
	 * Anything else is a bonus - though the server
	 * may reject auth if it decides it wants a client
	 * cert. We can't express that based on auth type
	 * alone though - we'll merely find out when TLS
	 * negotiation takes place.
	 */
	if (vnc_connection_wants_credential_x509(conn) && !conn->cred_x509_cacert)
		return FALSE;
	return TRUE;
}

static gboolean vnc_connection_gather_credentials(VncConnection *conn)
{
	if (!vnc_connection_has_credentials(conn)) {
		GVNC_DEBUG("Requesting missing credentials");
		if (conn->has_error || !conn->ops.auth_cred) {
			conn->has_error = TRUE;
			return FALSE;
		}
		if (!conn->ops.auth_cred(conn->ops_data))
			conn->has_error = TRUE;
		if (conn->has_error)
			return FALSE;
		GVNC_DEBUG("Waiting for missing credentials");
		g_condition_wait(vnc_connection_has_credentials, conn);
		GVNC_DEBUG("Got all credentials");
	}
	return !vnc_connection_has_error(conn);
}


static gboolean vnc_connection_check_auth_result(VncConnection *conn)
{
	guint32 result;
	GVNC_DEBUG("Checking auth result");
	result = vnc_connection_read_u32(conn);
	if (!result) {
		GVNC_DEBUG("Success");
		return TRUE;
	}

	if (conn->minor >= 8) {
		guint32 len;
		char reason[1024];
		len = vnc_connection_read_u32(conn);
		if (len > (sizeof(reason)-1))
			return FALSE;
		vnc_connection_read(conn, reason, len);
		reason[len] = '\0';
		GVNC_DEBUG("Fail %s", reason);
		if (!conn->has_error && conn->ops.auth_failure)
			conn->ops.auth_failure(conn->ops_data, reason);
	} else {
		GVNC_DEBUG("Fail auth no result");
		if (!conn->has_error && conn->ops.auth_failure)
			conn->ops.auth_failure(conn->ops_data, NULL);
	}
	return FALSE;
}

static gboolean vnc_connection_perform_auth_vnc(VncConnection *conn)
{
	guint8 challenge[16];
	guint8 key[8];

	GVNC_DEBUG("Do Challenge");
	conn->want_cred_password = TRUE;
	conn->want_cred_username = FALSE;
	conn->want_cred_x509 = FALSE;
	if (!vnc_connection_gather_credentials(conn))
		return FALSE;

	if (!conn->cred_password)
		return FALSE;

	vnc_connection_read(conn, challenge, 16);

	memset(key, 0, 8);
	strncpy((char*)key, (char*)conn->cred_password, 8);

	deskey(key, EN0);
	des(challenge, challenge);
	des(challenge + 8, challenge + 8);

	vnc_connection_write(conn, challenge, 16);
	vnc_connection_flush(conn);
	return vnc_connection_check_auth_result(conn);
}

/*
 *   marscha@2006 - Martin Scharpf
 *   Encrypt bytes[length] in memory using key.
 *   Key has to be 8 bytes, length a multiple of 8 bytes.
 */
static void
vncEncryptBytes2(unsigned char *where, const int length, unsigned char *key) {
	int i, j;
	deskey(key, EN0);
	for (i = 0; i< 8; i++)
		where[i] ^= key[i];
	des(where, where);
	for (i = 8; i < length; i += 8) {
		for (j = 0; j < 8; j++)
			where[i + j] ^= where[i + j - 8];
		des(where + i, where + i);
	}
}

static gboolean vnc_connection_perform_auth_mslogon(VncConnection *conn)
{
	struct vnc_dh *dh;
	guchar gen[8], mod[8], resp[8], pub[8], key[8];
	gcry_mpi_t genmpi, modmpi, respmpi, pubmpi, keympi;
	guchar username[256], password[64];
	guint passwordLen, usernameLen;

	GVNC_DEBUG("Do Challenge");
	conn->want_cred_password = TRUE;
	conn->want_cred_username = TRUE;
	conn->want_cred_x509 = FALSE;
	if (!vnc_connection_gather_credentials(conn))
		return FALSE;

	vnc_connection_read(conn, gen, sizeof(gen));
	vnc_connection_read(conn, mod, sizeof(mod));
	vnc_connection_read(conn, resp, sizeof(resp));

	genmpi = vnc_bytes_to_mpi(gen);
	modmpi = vnc_bytes_to_mpi(mod);
	respmpi = vnc_bytes_to_mpi(resp);

	dh = vnc_dh_new(genmpi, modmpi);

	pubmpi = vnc_dh_gen_secret(dh);
	vnc_mpi_to_bytes(pubmpi, pub);

	vnc_connection_write(conn, pub, sizeof(pub));

	keympi = vnc_dh_gen_key(dh, respmpi);
	vnc_mpi_to_bytes(keympi, key);

	passwordLen = strlen(conn->cred_password);
	usernameLen = strlen(conn->cred_username);
	if (passwordLen > sizeof(password))
		passwordLen = sizeof(password);
	if (usernameLen > sizeof(username))
		usernameLen = sizeof(username);

	memset(password, 0, sizeof password);
	memset(username, 0, sizeof username);
	memcpy(password, conn->cred_password, passwordLen);
	memcpy(username, conn->cred_username, usernameLen);

	vncEncryptBytes2(username, sizeof(username), key);
	vncEncryptBytes2(password, sizeof(password), key);

	vnc_connection_write(conn, username, sizeof(username));
	vnc_connection_write(conn, password, sizeof(password));
	vnc_connection_flush(conn);

	gcry_mpi_release(genmpi);
	gcry_mpi_release(modmpi);
	gcry_mpi_release(respmpi);
	vnc_dh_free (dh);

	return vnc_connection_check_auth_result(conn);
}

#if HAVE_SASL
/*
 * NB, keep in sync with similar method in qemud/remote.c
 */
static char *vnc_connection_addr_to_string(struct sockaddr_storage *sa, socklen_t salen)
{
	char host[NI_MAXHOST], port[NI_MAXSERV];
	char *addr;
	int err;

	if ((err = getnameinfo((struct sockaddr *)sa, salen,
			       host, sizeof(host),
			       port, sizeof(port),
			       NI_NUMERICHOST | NI_NUMERICSERV)) != 0) {
		GVNC_DEBUG("Cannot resolve address %d: %s",
			   err, gai_strerror(err));
		return NULL;
	}

	addr = g_malloc0(strlen(host) + 1 + strlen(port) + 1);
	strcpy(addr, host);
	strcat(addr, ";");
	strcat(addr, port);
	return addr;
}



static gboolean
vnc_connection_gather_sasl_credentials(VncConnection *conn,
				       sasl_interact_t *interact)
{
	int ninteract;

	conn->want_cred_password = FALSE;
	conn->want_cred_username = FALSE;
	conn->want_cred_x509 = FALSE;

	for (ninteract = 0 ; interact[ninteract].id != 0 ; ninteract++) {
		switch (interact[ninteract].id) {
		case SASL_CB_AUTHNAME:
		case SASL_CB_USER:
			conn->want_cred_username = TRUE;
			break;

		case SASL_CB_PASS:
			conn->want_cred_password = TRUE;
			break;

		default:
			GVNC_DEBUG("Unsupported credential %lu",
				   interact[ninteract].id);
			/* Unsupported */
			return FALSE;
		}
	}

	if ((conn->want_cred_password ||
	     conn->want_cred_username) &&
	    !vnc_connection_gather_credentials(conn)) {
		GVNC_DEBUG("%s", "cannot gather sasl credentials");
		return FALSE;
	}

	for (ninteract = 0 ; interact[ninteract].id != 0 ; ninteract++) {
		switch (interact[ninteract].id) {
		case SASL_CB_AUTHNAME:
		case SASL_CB_USER:
			interact[ninteract].result = conn->cred_username;
			interact[ninteract].len = strlen(conn->cred_username);
			GVNC_DEBUG("Gather Username %s", conn->cred_username);
			break;

		case SASL_CB_PASS:
			interact[ninteract].result =  conn->cred_password;
			interact[ninteract].len = strlen(conn->cred_password);
			//GVNC_DEBUG("Gather Password %s", conn->cred_password);
			break;
		}
	}

	GVNC_DEBUG("%s", "Filled SASL interact");

	return TRUE;
}



/*
 *
 * Init msg from server
 *
 *  u32 mechlist-length
 *  u8-array mechlist-string
 *
 * Start msg to server
 *
 *  u32 mechname-length
 *  u8-array mechname-string
 *  u32 clientout-length
 *  u8-array clientout-string
 *
 * Start msg from server
 *
 *  u32 serverin-length
 *  u8-array serverin-string
 *  u8 continue
 *
 * Step msg to server
 *
 *  u32 clientout-length
 *  u8-array clientout-string
 *
 * Step msg from server
 *
 *  u32 serverin-length
 *  u8-array serverin-string
 *  u8 continue
 */

#define SASL_MAX_MECHLIST_LEN 300
#define SASL_MAX_MECHNAME_LEN 100
#define SASL_MAX_DATA_LEN (1024 * 1024)

/* Perform the SASL authentication process
 */
static gboolean vnc_connection_perform_auth_sasl(VncConnection *conn)
{
	sasl_conn_t *saslconn = NULL;
	sasl_security_properties_t secprops;
	const char *clientout;
	char *serverin = NULL;
	unsigned int clientoutlen, serverinlen;
	int err, complete;
	struct sockaddr_storage sa;
	socklen_t salen;
	char *localAddr = NULL, *remoteAddr = NULL;
	const void *val;
	sasl_ssf_t ssf;
	sasl_callback_t saslcb[] = {
		{ .id = SASL_CB_AUTHNAME },
		//		{ .id = SASL_CB_USER },
		{ .id = SASL_CB_PASS },
		{ .id = 0 },
	};
	sasl_interact_t *interact = NULL;
	guint32 mechlistlen;
        char *mechlist;
	const char *mechname;
	gboolean ret;

	/* Sets up the SASL library as a whole */
	err = sasl_client_init(NULL);
	GVNC_DEBUG("Client initialize SASL authentication %d", err);
	if (err != SASL_OK) {
		GVNC_DEBUG("failed to initialize SASL library: %d (%s)",
			   err, sasl_errstring(err, NULL, NULL));
		goto error;
	}

	/* Get local address in form  IPADDR:PORT */
	salen = sizeof(sa);
	if (getsockname(conn->fd, (struct sockaddr*)&sa, &salen) < 0) {
		GVNC_DEBUG("failed to get sock address %d (%s)",
			   errno, strerror(errno));
		goto error;
	}
	if ((sa.ss_family == AF_INET ||
	     sa.ss_family == AF_INET6) &&
	    (localAddr = vnc_connection_addr_to_string(&sa, salen)) == NULL)
		goto error;

	/* Get remote address in form  IPADDR:PORT */
	salen = sizeof(sa);
	if (getpeername(conn->fd, (struct sockaddr*)&sa, &salen) < 0) {
		GVNC_DEBUG("failed to get peer address %d (%s)",
			   errno, strerror(errno));
		g_free(localAddr);
		goto error;
	}
	if ((sa.ss_family == AF_INET ||
	     sa.ss_family == AF_INET6) &&
	    (remoteAddr = vnc_connection_addr_to_string(&sa, salen)) == NULL) {
		g_free(localAddr);
		goto error;
	}

	GVNC_DEBUG("Client SASL new host:'%s' local:'%s' remote:'%s'", conn->host, localAddr, remoteAddr);

	/* Setup a handle for being a client */
	err = sasl_client_new("vnc",
			      conn->host,
			      localAddr,
			      remoteAddr,
			      saslcb,
			      SASL_SUCCESS_DATA,
			      &saslconn);
	g_free(localAddr);
	g_free(remoteAddr);

	if (err != SASL_OK) {
		GVNC_DEBUG("Failed to create SASL client context: %d (%s)",
			   err, sasl_errstring(err, NULL, NULL));
		goto error;
	}

	/* Initialize some connection props we care about */
	if (conn->tls_session) {
		gnutls_cipher_algorithm_t cipher;

		cipher = gnutls_cipher_get(conn->tls_session);
		if (!(ssf = (sasl_ssf_t)gnutls_cipher_get_key_size(cipher))) {
			GVNC_DEBUG("%s", "invalid cipher size for TLS session");
			goto error;
		}
		ssf *= 8; /* key size is bytes, sasl wants bits */

		GVNC_DEBUG("Setting external SSF %d", ssf);
		err = sasl_setprop(saslconn, SASL_SSF_EXTERNAL, &ssf);
		if (err != SASL_OK) {
			GVNC_DEBUG("cannot set external SSF %d (%s)",
				   err, sasl_errstring(err, NULL, NULL));
			goto error;
		}
	}

	memset (&secprops, 0, sizeof secprops);
	/* If we've got TLS, we don't care about SSF */
	secprops.min_ssf = conn->tls_session ? 0 : 56; /* Equiv to DES supported by all Kerberos */
	secprops.max_ssf = conn->tls_session ? 0 : 100000; /* Very strong ! AES == 256 */
	secprops.maxbufsize = 100000;
	/* If we're not TLS, then forbid any anonymous or trivially crackable auth */
	secprops.security_flags = conn->tls_session ? 0 :
		SASL_SEC_NOANONYMOUS | SASL_SEC_NOPLAINTEXT;

	err = sasl_setprop(saslconn, SASL_SEC_PROPS, &secprops);
	if (err != SASL_OK) {
		GVNC_DEBUG("cannot set security props %d (%s)",
			   err, sasl_errstring(err, NULL, NULL));
		goto error;
	}

	/* Get the supported mechanisms from the server */
	mechlistlen = vnc_connection_read_u32(conn);
	if (conn->has_error)
		goto error;
	if (mechlistlen > SASL_MAX_MECHLIST_LEN) {
		GVNC_DEBUG("mechlistlen %d too long", mechlistlen);
		goto error;
	}

	mechlist = g_malloc(mechlistlen+1);
        vnc_connection_read(conn, mechlist, mechlistlen);
	mechlist[mechlistlen] = '\0';
	if (conn->has_error) {
		g_free(mechlist);
		mechlist = NULL;
		goto error;
	}

#if 0
	if (wantmech) {
		if (strstr(mechlist, wantmech) == NULL) {
			GVNC_DEBUG("SASL mechanism %s not supported by server",
				   wantmech);
			VIR_FREE(iret.mechlist);
			goto error;
		}
		mechlist = wantmech;
	}
#endif

 restart:
	/* Start the auth negotiation on the client end first */
	GVNC_DEBUG("Client start negotiation mechlist '%s'", mechlist);
	err = sasl_client_start(saslconn,
				mechlist,
				&interact,
				&clientout,
				&clientoutlen,
				&mechname);
	if (err != SASL_OK && err != SASL_CONTINUE && err != SASL_INTERACT) {
		GVNC_DEBUG("Failed to start SASL negotiation: %d (%s)",
			   err, sasl_errdetail(saslconn));
		g_free(mechlist);
		mechlist = NULL;
		goto error;
	}

	/* Need to gather some credentials from the client */
	if (err == SASL_INTERACT) {
		if (!vnc_connection_gather_sasl_credentials(conn,
							    interact)) {
			GVNC_DEBUG("%s", "Failed to collect auth credentials");
			goto error;
		}
		goto restart;
	}

	GVNC_DEBUG("Server start negotiation with mech %s. Data %d bytes %p '%s'",
		   mechname, clientoutlen, clientout, clientout);

	if (clientoutlen > SASL_MAX_DATA_LEN) {
		GVNC_DEBUG("SASL negotiation data too long: %d bytes",
			   clientoutlen);
		goto error;
	}

	/* Send back the chosen mechname */
	vnc_connection_write_u32(conn, strlen(mechname));
	vnc_connection_write(conn, mechname, strlen(mechname));

	/* NB, distinction of NULL vs "" is *critical* in SASL */
	if (clientout) {
		vnc_connection_write_u32(conn, clientoutlen + 1);
		vnc_connection_write(conn, clientout, clientoutlen + 1);
	} else {
		vnc_connection_write_u32(conn, 0);
	}
	vnc_connection_flush(conn);
	if (conn->has_error)
		goto error;


	GVNC_DEBUG("%s", "Getting sever start negotiation reply");
	/* Read the 'START' message reply from server */
	serverinlen = vnc_connection_read_u32(conn);
	if (conn->has_error)
		goto error;
	if (serverinlen > SASL_MAX_DATA_LEN) {
		GVNC_DEBUG("SASL negotiation data too long: %d bytes",
			   clientoutlen);
		goto error;
	}

	/* NB, distinction of NULL vs "" is *critical* in SASL */
	if (serverinlen) {
		serverin = g_malloc(serverinlen);
		vnc_connection_read(conn, serverin, serverinlen);
		serverin[serverinlen-1] = '\0';
		serverinlen--;
	} else {
		serverin = NULL;
	}
	complete = vnc_connection_read_u8(conn);
	if (conn->has_error)
		goto error;

	GVNC_DEBUG("Client start result complete: %d. Data %d bytes %p '%s'",
		   complete, serverinlen, serverin, serverin);

	/* Loop-the-loop...
	 * Even if the server has completed, the client must *always* do at least one step
	 * in this loop to verify the server isn't lying about something. Mutual auth */
	for (;;) {
	restep:
		err = sasl_client_step(saslconn,
				       serverin,
				       serverinlen,
				       &interact,
				       &clientout,
				       &clientoutlen);
		if (err != SASL_OK && err != SASL_CONTINUE && err != SASL_INTERACT) {
			GVNC_DEBUG("Failed SASL step: %d (%s)",
				   err, sasl_errdetail(saslconn));
			goto error;
		}

		/* Need to gather some credentials from the client */
		if (err == SASL_INTERACT) {
			if (!vnc_connection_gather_sasl_credentials(conn,
								    interact)) {
				GVNC_DEBUG("%s", "Failed to collect auth credentials");
				goto error;
			}
			goto restep;
		}

		if (serverin) {
			g_free(serverin);
			serverin = NULL;
		}

		GVNC_DEBUG("Client step result %d. Data %d bytes %p '%s'", err, clientoutlen, clientout, clientout);

		/* Previous server call showed completion & we're now locally complete too */
		if (complete && err == SASL_OK)
			break;

		/* Not done, prepare to talk with the server for another iteration */

		/* NB, distinction of NULL vs "" is *critical* in SASL */
		if (clientout) {
			vnc_connection_write_u32(conn, clientoutlen + 1);
			vnc_connection_write(conn, clientout, clientoutlen + 1);
		} else {
			vnc_connection_write_u32(conn, 0);
		}
		vnc_connection_flush(conn);
		if (conn->has_error)
			goto error;

		GVNC_DEBUG("Server step with %d bytes %p", clientoutlen, clientout);

		serverinlen = vnc_connection_read_u32(conn);
		if (conn->has_error)
			goto error;
		if (serverinlen > SASL_MAX_DATA_LEN) {
			GVNC_DEBUG("SASL negotiation data too long: %d bytes",
				   clientoutlen);
			goto error;
		}

		/* NB, distinction of NULL vs "" is *critical* in SASL */
		if (serverinlen) {
			serverin = g_malloc(serverinlen);
			vnc_connection_read(conn, serverin, serverinlen);
			serverin[serverinlen-1] = '\0';
			serverinlen--;
		} else {
			serverin = NULL;
		}
		complete = vnc_connection_read_u8(conn);
		if (conn->has_error)
			goto error;

		GVNC_DEBUG("Client step result complete: %d. Data %d bytes %p '%s'",
			   complete, serverinlen, serverin, serverin);

		/* This server call shows complete, and earlier client step was OK */
		if (complete && err == SASL_OK) {
			g_free(serverin);
			serverin = NULL;
			break;
		}
	}

	/* Check for suitable SSF if non-TLS */
	if (!conn->tls_session) {
		err = sasl_getprop(saslconn, SASL_SSF, &val);
		if (err != SASL_OK) {
			GVNC_DEBUG("cannot query SASL ssf on connection %d (%s)",
				   err, sasl_errstring(err, NULL, NULL));
			goto error;
		}
		ssf = *(const int *)val;
		GVNC_DEBUG("SASL SSF value %d", ssf);
		if (ssf < 56) { /* 56 == DES level, good for Kerberos */
			GVNC_DEBUG("negotiation SSF %d was not strong enough", ssf);
			goto error;
		}
	}

	GVNC_DEBUG("%s", "SASL authentication complete");
	ret = vnc_connection_check_auth_result(conn);
	/* This must come *after* check-auth-result, because the former
	 * is defined to be sent unencrypted, and setting saslconn turns
	 * on the SSF layer encryption processing */
	conn->saslconn = saslconn;
	return ret;

 error:
	conn->has_error = TRUE;
	if (saslconn)
		sasl_dispose(&saslconn);
	return FALSE;
}
#endif /* HAVE_SASL */


static gboolean vnc_connection_start_tls(VncConnection *conn, int anonTLS)
{
	static const int cert_type_priority[] = { GNUTLS_CRT_X509, 0 };
	static const int protocol_priority[]= { GNUTLS_TLS1_1, GNUTLS_TLS1_0, GNUTLS_SSL3, 0 };
	static const int kx_priority[] = {GNUTLS_KX_DHE_DSS, GNUTLS_KX_RSA, GNUTLS_KX_DHE_RSA, GNUTLS_KX_SRP, 0};
	static const int kx_anon[] = {GNUTLS_KX_ANON_DH, 0};
	int ret;

	GVNC_DEBUG("Do TLS handshake");
	if (vnc_connection_tls_initialize() < 0) {
		GVNC_DEBUG("Failed to init TLS");
		conn->has_error = TRUE;
		return FALSE;
	}
	if (conn->tls_session == NULL) {
		if (gnutls_init(&conn->tls_session, GNUTLS_CLIENT) < 0) {
			conn->has_error = TRUE;
			return FALSE;
		}

		if (gnutls_set_default_priority(conn->tls_session) < 0) {
			gnutls_deinit(conn->tls_session);
			conn->has_error = TRUE;
			return FALSE;
		}

		if (gnutls_kx_set_priority(conn->tls_session, anonTLS ? kx_anon : kx_priority) < 0) {
			gnutls_deinit(conn->tls_session);
			conn->has_error = TRUE;
			return FALSE;
		}

		if (gnutls_certificate_type_set_priority(conn->tls_session, cert_type_priority) < 0) {
			gnutls_deinit(conn->tls_session);
			conn->has_error = TRUE;
			return FALSE;
		}

		if (gnutls_protocol_set_priority(conn->tls_session, protocol_priority) < 0) {
			gnutls_deinit(conn->tls_session);
			conn->has_error = TRUE;
			return FALSE;
		}

		if (anonTLS) {
			gnutls_anon_client_credentials anon_cred = vnc_connection_tls_initialize_anon_cred();
			if (!anon_cred) {
				gnutls_deinit(conn->tls_session);
				conn->has_error = TRUE;
				return FALSE;
			}
			if (gnutls_credentials_set(conn->tls_session, GNUTLS_CRD_ANON, anon_cred) < 0) {
				gnutls_deinit(conn->tls_session);
				conn->has_error = TRUE;
				return FALSE;
			}
		} else {
			conn->want_cred_password = FALSE;
			conn->want_cred_username = FALSE;
			conn->want_cred_x509 = TRUE;
			if (!vnc_connection_gather_credentials(conn))
				return FALSE;

			gnutls_certificate_credentials_t x509_cred = vnc_connection_tls_initialize_cert_cred(conn);
			if (!x509_cred) {
				gnutls_deinit(conn->tls_session);
				conn->has_error = TRUE;
				return FALSE;
			}
			if (gnutls_credentials_set(conn->tls_session, GNUTLS_CRD_CERTIFICATE, x509_cred) < 0) {
				gnutls_deinit(conn->tls_session);
				conn->has_error = TRUE;
				return FALSE;
			}
		}

		gnutls_transport_set_ptr(conn->tls_session, (gnutls_transport_ptr_t)conn);
		gnutls_transport_set_push_function(conn->tls_session, vnc_connection_tls_push);
		gnutls_transport_set_pull_function(conn->tls_session, vnc_connection_tls_pull);
	}

 retry:
	if ((ret = gnutls_handshake(conn->tls_session)) < 0) {
		if (!gnutls_error_is_fatal(ret)) {
			GVNC_DEBUG("Handshake was blocking");
			if (!gnutls_record_get_direction(conn->tls_session))
				g_io_wait(conn->channel, G_IO_IN);
			else
				g_io_wait(conn->channel, G_IO_OUT);
			goto retry;
		}
		GVNC_DEBUG("Handshake failed %s", gnutls_strerror(ret));
		gnutls_deinit(conn->tls_session);
		conn->tls_session = NULL;
		conn->has_error = TRUE;
		return FALSE;
	}

	GVNC_DEBUG("Handshake done");

	if (anonTLS) {
		return TRUE;
	} else {
		if (!vnc_connection_validate_certificate(conn)) {
			GVNC_DEBUG("Certificate validation failed");
			conn->has_error = TRUE;
			return FALSE;
		}
		return TRUE;
	}
}

static gboolean vnc_connection_has_auth_subtype(gpointer data)
{
	VncConnection *conn = data;

	if (conn->has_error)
		return TRUE;
	if (conn->auth_subtype == GVNC_AUTH_INVALID)
		return FALSE;
	return TRUE;
}


static gboolean vnc_connection_perform_auth_tls(VncConnection *conn)
{
	unsigned int nauth, i;
	unsigned int auth[20];

	if (!vnc_connection_start_tls(conn, 1)) {
		GVNC_DEBUG("Could not start TLS");
		return FALSE;
	}
	GVNC_DEBUG("Completed TLS setup");

	nauth = vnc_connection_read_u8(conn);
	GVNC_DEBUG("Got %d subauths", nauth);
	if (vnc_connection_has_error(conn))
		return FALSE;

	GVNC_DEBUG("Got %d subauths", nauth);
	if (nauth == 0) {
		GVNC_DEBUG("No sub-auth types requested");
		return vnc_connection_check_auth_result(conn);
	}

	if (nauth > sizeof(auth)) {
		GVNC_DEBUG("Too many (%d) auth types", nauth);
		conn->has_error = TRUE;
		return FALSE;
	}
	for (i = 0 ; i < nauth ; i++) {
		auth[i] = vnc_connection_read_u8(conn);
	}

	for (i = 0 ; i < nauth ; i++) {
		GVNC_DEBUG("Possible sub-auth %d", auth[i]);
	}

	if (conn->has_error || !conn->ops.auth_subtype)
		return FALSE;

	if (!conn->ops.auth_subtype(conn->ops_data, nauth, auth))
		conn->has_error = TRUE;
	if (conn->has_error)
		return FALSE;

	GVNC_DEBUG("Waiting for auth subtype");
	g_condition_wait(vnc_connection_has_auth_subtype, conn);
	if (conn->has_error)
		return FALSE;

	GVNC_DEBUG("Choose auth %d", conn->auth_subtype);

	vnc_connection_write_u8(conn, conn->auth_subtype);
	vnc_connection_flush(conn);

	switch (conn->auth_subtype) {
	case GVNC_AUTH_NONE:
		if (conn->minor == 8)
			return vnc_connection_check_auth_result(conn);
		return TRUE;
	case GVNC_AUTH_VNC:
		return vnc_connection_perform_auth_vnc(conn);
#if HAVE_SASL
	case GVNC_AUTH_SASL:
		return vnc_connection_perform_auth_sasl(conn);
#endif
	default:
		return FALSE;
	}

	return TRUE;
}

static gboolean vnc_connection_perform_auth_vencrypt(VncConnection *conn)
{
	int major, minor, status, anonTLS;
	unsigned int nauth, i;
	unsigned int auth[20];

	major = vnc_connection_read_u8(conn);
	minor = vnc_connection_read_u8(conn);

	if (major != 0 &&
	    minor != 2) {
		GVNC_DEBUG("Unsupported VeNCrypt version %d %d", major, minor);
		return FALSE;
	}

	vnc_connection_write_u8(conn, major);
	vnc_connection_write_u8(conn, minor);
	vnc_connection_flush(conn);
	status = vnc_connection_read_u8(conn);
	if (status != 0) {
		GVNC_DEBUG("Server refused VeNCrypt version %d %d", major, minor);
		return FALSE;
	}

	nauth = vnc_connection_read_u8(conn);
	if (nauth > (sizeof(auth)/sizeof(auth[0]))) {
		GVNC_DEBUG("Too many (%d) auth types", nauth);
		return FALSE;
	}

	for (i = 0 ; i < nauth ; i++) {
		auth[i] = vnc_connection_read_u32(conn);
	}

	for (i = 0 ; i < nauth ; i++) {
		GVNC_DEBUG("Possible auth %d", auth[i]);
	}

	if (conn->has_error || !conn->ops.auth_subtype)
		return FALSE;

	if (!conn->ops.auth_subtype(conn->ops_data, nauth, auth))
		conn->has_error = TRUE;
	if (conn->has_error)
		return FALSE;

	GVNC_DEBUG("Waiting for auth subtype");
	g_condition_wait(vnc_connection_has_auth_subtype, conn);
	if (conn->has_error)
		return FALSE;

	GVNC_DEBUG("Choose auth %d", conn->auth_subtype);

	if (!vnc_connection_gather_credentials(conn))
		return FALSE;

#if !DEBUG
	if (conn->auth_subtype == GVNC_AUTH_VENCRYPT_PLAIN) {
		GVNC_DEBUG("Cowardly refusing to transmit plain text password");
		return FALSE;
	}
#endif

	vnc_connection_write_u32(conn, conn->auth_subtype);
	vnc_connection_flush(conn);
	status = vnc_connection_read_u8(conn);
	if (status != 1) {
		GVNC_DEBUG("Server refused VeNCrypt auth %d %d", conn->auth_subtype, status);
		return FALSE;
	}

	switch (conn->auth_subtype) {
	case GVNC_AUTH_VENCRYPT_TLSNONE:
	case GVNC_AUTH_VENCRYPT_TLSPLAIN:
	case GVNC_AUTH_VENCRYPT_TLSVNC:
	case GVNC_AUTH_VENCRYPT_TLSSASL:
		anonTLS = 1;
		break;
	default:
		anonTLS = 0;
	}

	if (!vnc_connection_start_tls(conn, anonTLS)) {
		GVNC_DEBUG("Could not start TLS");
		return FALSE;
	}
	GVNC_DEBUG("Completed TLS setup, do subauth %d", conn->auth_subtype);

	switch (conn->auth_subtype) {
		/* Plain certificate based auth */
	case GVNC_AUTH_VENCRYPT_TLSNONE:
	case GVNC_AUTH_VENCRYPT_X509NONE:
		GVNC_DEBUG("Completing auth");
		return vnc_connection_check_auth_result(conn);

		/* Regular VNC layered over TLS */
	case GVNC_AUTH_VENCRYPT_TLSVNC:
	case GVNC_AUTH_VENCRYPT_X509VNC:
		GVNC_DEBUG("Handing off to VNC auth");
		return vnc_connection_perform_auth_vnc(conn);

#if HAVE_SASL
		/* SASL layered over TLS */
	case GVNC_AUTH_VENCRYPT_TLSSASL:
	case GVNC_AUTH_VENCRYPT_X509SASL:
		GVNC_DEBUG("Handing off to SASL auth");
		return vnc_connection_perform_auth_sasl(conn);
#endif

	default:
		GVNC_DEBUG("Unknown auth subtype %d", conn->auth_subtype);
		return FALSE;
	}
}

static gboolean vnc_connection_has_auth_type(gpointer data)
{
	VncConnection *conn = data;

	if (conn->has_error)
		return TRUE;
	if (conn->auth_type == GVNC_AUTH_INVALID)
		return FALSE;
	return TRUE;
}

static gboolean vnc_connection_perform_auth(VncConnection *conn)
{
	unsigned int nauth, i;
	unsigned int auth[10];

	if (conn->minor <= 6) {
		nauth = 1;
		auth[0] = vnc_connection_read_u32(conn);
	} else {
		nauth = vnc_connection_read_u8(conn);
		if (vnc_connection_has_error(conn))
			return FALSE;

		if (nauth == 0)
			return vnc_connection_check_auth_result(conn);

		if (nauth > sizeof(auth)) {
			conn->has_error = TRUE;
			return FALSE;
		}
		for (i = 0 ; i < nauth ; i++)
			auth[i] = vnc_connection_read_u8(conn);
	}

	for (i = 0 ; i < nauth ; i++) {
		GVNC_DEBUG("Possible auth %u", auth[i]);
	}

	if (conn->has_error || !conn->ops.auth_type)
		return FALSE;

	if (!conn->ops.auth_type(conn->ops_data, nauth, auth))
		conn->has_error = TRUE;
	if (conn->has_error)
		return FALSE;

	GVNC_DEBUG("Waiting for auth type");
	g_condition_wait(vnc_connection_has_auth_type, conn);
	if (conn->has_error)
		return FALSE;

	GVNC_DEBUG("Choose auth %u", conn->auth_type);
	if (!vnc_connection_gather_credentials(conn))
		return FALSE;

	if (conn->minor > 6) {
		vnc_connection_write_u8(conn, conn->auth_type);
		vnc_connection_flush(conn);
	}

	switch (conn->auth_type) {
	case GVNC_AUTH_NONE:
		if (conn->minor == 8)
			return vnc_connection_check_auth_result(conn);
		return TRUE;
	case GVNC_AUTH_VNC:
		return vnc_connection_perform_auth_vnc(conn);

	case GVNC_AUTH_TLS:
		if (conn->minor < 7)
			return FALSE;
		return vnc_connection_perform_auth_tls(conn);

	case GVNC_AUTH_VENCRYPT:
		return vnc_connection_perform_auth_vencrypt(conn);

#if HAVE_SASL
	case GVNC_AUTH_SASL:
 		return vnc_connection_perform_auth_sasl(conn);
#endif

	case GVNC_AUTH_MSLOGON:
		return vnc_connection_perform_auth_mslogon(conn);

	default:
		if (conn->ops.auth_unsupported)
			conn->ops.auth_unsupported (conn->ops_data, conn->auth_type);
		conn->has_error = TRUE;

		return FALSE;
	}

	return TRUE;
}

VncConnection *vnc_connection_new(const struct vnc_connection_ops *ops, gpointer ops_data)
{
	VncConnection *conn = g_malloc0(sizeof(*conn));

	conn->fd = -1;

	memcpy(&conn->ops, ops, sizeof(*ops));
	conn->ops_data = ops_data;
	conn->auth_type = GVNC_AUTH_INVALID;
	conn->auth_subtype = GVNC_AUTH_INVALID;

	return conn;
}

void vnc_connection_free(VncConnection *conn)
{
	if (!conn)
		return;

	if (vnc_connection_is_open(conn))
		vnc_connection_close(conn);

	g_free(conn);
	conn = NULL;
}

void vnc_connection_close(VncConnection *conn)
{
	int i;

	if (conn->tls_session) {
		gnutls_bye(conn->tls_session, GNUTLS_SHUT_RDWR);
		conn->tls_session = NULL;
	}
#if HAVE_SASL
	if (conn->saslconn)
		sasl_dispose (&conn->saslconn);
#endif

	if (conn->channel) {
		g_io_channel_unref(conn->channel);
		conn->channel = NULL;
	}
	if (conn->fd != -1) {
		close(conn->fd);
		conn->fd = -1;
	}

	if (conn->host) {
		g_free(conn->host);
		conn->host = NULL;
	}

	if (conn->port) {
		g_free(conn->port);
		conn->port = NULL;
	}

	if (conn->name) {
		g_free(conn->name);
		conn->name = NULL;
	}

	g_free (conn->xmit_buffer);

	if (conn->cred_username) {
		g_free(conn->cred_username);
		conn->cred_username = NULL;
	}
	if (conn->cred_password) {
		g_free(conn->cred_password);
		conn->cred_password = NULL;
	}

	if (conn->cred_x509_cacert) {
		g_free(conn->cred_x509_cacert);
		conn->cred_x509_cacert = NULL;
	}
	if (conn->cred_x509_cacrl) {
		g_free(conn->cred_x509_cacrl);
		conn->cred_x509_cacrl = NULL;
	}
	if (conn->cred_x509_cert) {
		g_free(conn->cred_x509_cert);
		conn->cred_x509_cert = NULL;
	}
	if (conn->cred_x509_key) {
		g_free(conn->cred_x509_key);
		conn->cred_x509_key = NULL;
	}

	for (i = 0; i < 5; i++)
		inflateEnd(&conn->streams[i]);

	conn->auth_type = GVNC_AUTH_INVALID;
	conn->auth_subtype = GVNC_AUTH_INVALID;

	conn->has_error = 0;
}

void vnc_connection_shutdown(VncConnection *conn)
{
	close(conn->fd);
	conn->fd = -1;
	conn->has_error = 1;
	GVNC_DEBUG("Waking up couroutine to shutdown gracefully");
	g_io_wakeup(&conn->wait);
}

gboolean vnc_connection_is_open(VncConnection *conn)
{
	if (!conn)
		return FALSE;

	if (conn->fd != -1)
		return TRUE;
	if (conn->host)
		return TRUE;
	return FALSE;
}


gboolean vnc_connection_is_initialized(VncConnection *conn)
{
	if (!vnc_connection_is_open(conn))
		return FALSE;
	if (conn->name)
		return TRUE;
	return FALSE;
}

static gboolean vnc_connection_before_version (VncConnection *conn, int major, int minor) {
	return (conn->major < major) || (conn->major == major && conn->minor < minor);
}
static gboolean vnc_connection_after_version (VncConnection *conn, int major, int minor) {
	return !vnc_connection_before_version (conn, major, minor+1);
}

gboolean vnc_connection_initialize(VncConnection *conn, gboolean shared_flag)
{
	int ret, i;
	char version[13];
	guint32 n_name;

	conn->absolute = 1;

	vnc_connection_read(conn, version, 12);
	version[12] = 0;

 	ret = sscanf(version, "RFB %03d.%03d\n", &conn->major, &conn->minor);
	if (ret != 2) {
		GVNC_DEBUG("Error while getting server version");
		goto fail;
	}

	GVNC_DEBUG("Server version: %d.%d", conn->major, conn->minor);

	if (vnc_connection_before_version(conn, 3, 3)) {
		GVNC_DEBUG("Server version is not supported (%d.%d)", conn->major, conn->minor);
		goto fail;
	} else if (vnc_connection_before_version(conn, 3, 7)) {
		conn->minor = 3;
	} else if (vnc_connection_after_version(conn, 3, 8)) {
		conn->major = 3;
		conn->minor = 8;
	}

	snprintf(version, 12, "RFB %03d.%03d\n", conn->major, conn->minor);
	vnc_connection_write(conn, version, 12);
	vnc_connection_flush(conn);
	GVNC_DEBUG("Using version: %d.%d", conn->major, conn->minor);

	if (!vnc_connection_perform_auth(conn)) {
		GVNC_DEBUG("Auth failed");
		goto fail;
	}

	vnc_connection_write_u8(conn, shared_flag); /* shared flag */
	vnc_connection_flush(conn);
	conn->width = vnc_connection_read_u16(conn);
	conn->height = vnc_connection_read_u16(conn);

	if (vnc_connection_has_error(conn))
		return FALSE;

	vnc_connection_read_pixel_format(conn, &conn->fmt);

	n_name = vnc_connection_read_u32(conn);
	if (n_name > 4096)
		goto fail;

	conn->name = g_new(char, n_name + 1);

	vnc_connection_read(conn, conn->name, n_name);
	conn->name[n_name] = 0;
	GVNC_DEBUG("Display name '%s'", conn->name);

	if (vnc_connection_has_error(conn))
		return FALSE;

	if (!conn->ops.get_preferred_pixel_format)
		goto fail;
	if (conn->ops.get_preferred_pixel_format(conn->ops_data, &conn->fmt))
		vnc_connection_set_pixel_format(conn, &conn->fmt);
	else
		goto fail;
	memset(&conn->strm, 0, sizeof(conn->strm));
	/* FIXME what level? */
	for (i = 0; i < 5; i++)
		inflateInit(&conn->streams[i]);
	conn->strm = NULL;

	vnc_connection_resize(conn, conn->width, conn->height);
	return !vnc_connection_has_error(conn);

 fail:
	conn->has_error = 1;
	return !vnc_connection_has_error(conn);
}

static gboolean vnc_connection_set_nonblock(int fd)
{
#ifndef WIN32
	int flags;
	if ((flags = fcntl(fd, F_GETFL)) < 0) {
		GVNC_DEBUG ("Failed to fcntl()");
		return FALSE;
	}
	flags |= O_NONBLOCK;
	if (fcntl(fd, F_SETFL, flags) < 0) {
		GVNC_DEBUG ("Failed to fcntl()");
		return FALSE;
	}

#else /* WIN32 */
	unsigned long flag = 1;

	/* This is actually Gnulib's replacement rpl_ioctl function.
	 * We can't call ioctlsocket directly in any case.
	 */
	if (ioctl (fd, FIONBIO, (void *) &flag) == -1) {
		GVNC_DEBUG ("Failed to set nonblocking flag, winsock error = %d",
			    WSAGetLastError ());
		return FALSE;
	}
#endif /* WIN32 */

	return TRUE;
}

gboolean vnc_connection_open_fd(VncConnection *conn, int fd)
{
	if (vnc_connection_is_open(conn)) {
		GVNC_DEBUG ("Error: already connected?");
		return FALSE;
	}

	GVNC_DEBUG("Connecting to FD %d", fd);

	if (!vnc_connection_set_nonblock(fd))
		return FALSE;

	if (!(conn->channel =
#ifdef WIN32
	      g_io_channel_win32_new_socket(_get_osfhandle(fd))
#else
	      g_io_channel_unix_new(fd)
#endif
	      )) {
		GVNC_DEBUG ("Failed to g_io_channel_unix_new()");
		return FALSE;
	}
	conn->fd = fd;

	return !vnc_connection_has_error(conn);
}

gboolean vnc_connection_open_host(VncConnection *conn, const char *host, const char *port)
{
        struct addrinfo *ai, *runp, hints;
        int ret;
	if (vnc_connection_is_open(conn))
		return FALSE;

	conn->host = g_strdup(host);
	conn->port = g_strdup(port);

        GVNC_DEBUG("Resolving host %s %s", host, port);
        memset (&hints, '\0', sizeof (hints));
        hints.ai_flags = AI_ADDRCONFIG;
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_protocol = IPPROTO_TCP;

        if ((ret = getaddrinfo(host, port, &hints, &ai)) != 0) {
		GVNC_DEBUG ("Failed to resolve hostname");
		return FALSE;
	}

        runp = ai;
        while (runp != NULL) {
                int fd;
                GIOChannel *chan;

		if ((fd = socket(runp->ai_family, runp->ai_socktype,
				 runp->ai_protocol)) < 0) {
			GVNC_DEBUG ("Failed to socket()");
			break;
		}

                GVNC_DEBUG("Trying socket %d", fd);
		if (!vnc_connection_set_nonblock(fd))
			break;

                if (!(chan =
#ifdef WIN32
		      g_io_channel_win32_new_socket(_get_osfhandle(fd))
#else
		      g_io_channel_unix_new(fd)
#endif
		      )) {
                        close(fd);
                        GVNC_DEBUG ("Failed to g_io_channel_unix_new()");
                        break;
                }

        reconnect:
                /* FIXME: Better handle EINPROGRESS/EISCONN return values,
                   as explained in connect(2) man page */
                if ((connect(fd, runp->ai_addr, runp->ai_addrlen) == 0) ||
		    errno == EISCONN) {
                        conn->channel = chan;
                        conn->fd = fd;
                        freeaddrinfo(ai);
                        return !vnc_connection_has_error(conn);
                }
                if (errno == EINPROGRESS ||
                    errno == EWOULDBLOCK) {
                        g_io_wait(chan, G_IO_OUT|G_IO_ERR|G_IO_HUP);
                        goto reconnect;
                } else if (errno != ECONNREFUSED &&
                           errno != EHOSTUNREACH) {
                        g_io_channel_unref(chan);
                        close(fd);
                        GVNC_DEBUG ("Failed with errno = %d", errno);
                        break;
                }
                close(fd);
                g_io_channel_unref(chan);
                runp = runp->ai_next;
        }
        freeaddrinfo (ai);
	return FALSE;
}


gboolean vnc_connection_set_auth_type(VncConnection *conn, unsigned int type)
{
        GVNC_DEBUG("Thinking about auth type %u", type);
        if (conn->auth_type != GVNC_AUTH_INVALID) {
                conn->has_error = TRUE;
                return !vnc_connection_has_error(conn);
        }
        if (type != GVNC_AUTH_NONE &&
            type != GVNC_AUTH_VNC &&
            type != GVNC_AUTH_MSLOGON &&
            type != GVNC_AUTH_TLS &&
            type != GVNC_AUTH_VENCRYPT &&
            type != GVNC_AUTH_SASL) {
		GVNC_DEBUG("Unsupported auth type %u", type);
            	if (conn->ops.auth_unsupported)
			conn->ops.auth_unsupported (conn->ops_data, type);

                conn->has_error = TRUE;
                return !vnc_connection_has_error(conn);
        }
        GVNC_DEBUG("Decided on auth type %u", type);
        conn->auth_type = type;
        conn->auth_subtype = GVNC_AUTH_INVALID;

	return !vnc_connection_has_error(conn);
}

gboolean vnc_connection_set_auth_subtype(VncConnection *conn, unsigned int type)
{
        GVNC_DEBUG("Requested auth subtype %d", type);
        if (conn->auth_type != GVNC_AUTH_VENCRYPT &&
	    conn->auth_type != GVNC_AUTH_TLS) {
                conn->has_error = TRUE;
		return !vnc_connection_has_error(conn);
        }
        if (conn->auth_subtype != GVNC_AUTH_INVALID) {
                conn->has_error = TRUE;
		return !vnc_connection_has_error(conn);
        }
        conn->auth_subtype = type;

	return !vnc_connection_has_error(conn);
}

gboolean vnc_connection_set_credential_password(VncConnection *conn, const char *password)
{
        GVNC_DEBUG("Set password credential %s", password);
        if (conn->cred_password)
                g_free(conn->cred_password);
        if (!(conn->cred_password = g_strdup(password))) {
                conn->has_error = TRUE;
                return FALSE;
        }
        return TRUE;
}

gboolean vnc_connection_set_credential_username(VncConnection *conn, const char *username)
{
        GVNC_DEBUG("Set username credential %s", username);
        if (conn->cred_username)
                g_free(conn->cred_username);
        if (!(conn->cred_username = g_strdup(username))) {
                conn->has_error = TRUE;
                return FALSE;
        }
        return TRUE;
}

gboolean vnc_connection_set_credential_x509_cacert(VncConnection *conn, const char *file)
{
        GVNC_DEBUG("Set x509 cacert %s", file);
        if (conn->cred_x509_cacert)
                g_free(conn->cred_x509_cacert);
        if (!(conn->cred_x509_cacert = g_strdup(file))) {
                conn->has_error = TRUE;
                return FALSE;
        }
        return TRUE;
}

gboolean vnc_connection_set_credential_x509_cacrl(VncConnection *conn, const char *file)
{
        GVNC_DEBUG("Set x509 cacrl %s", file);
        if (conn->cred_x509_cacrl)
                g_free(conn->cred_x509_cacrl);
        if (!(conn->cred_x509_cacrl = g_strdup(file))) {
                conn->has_error = TRUE;
                return FALSE;
        }
        return TRUE;
}

gboolean vnc_connection_set_credential_x509_key(VncConnection *conn, const char *file)
{
        GVNC_DEBUG("Set x509 key %s", file);
        if (conn->cred_x509_key)
                g_free(conn->cred_x509_key);
        if (!(conn->cred_x509_key = g_strdup(file))) {
                conn->has_error = TRUE;
                return FALSE;
        }
        return TRUE;
}

gboolean vnc_connection_set_credential_x509_cert(VncConnection *conn, const char *file)
{
        GVNC_DEBUG("Set x509 cert %s", file);
        if (conn->cred_x509_cert)
                g_free(conn->cred_x509_cert);
        if (!(conn->cred_x509_cert = g_strdup(file))) {
                conn->has_error = TRUE;
                return FALSE;
        }
        return TRUE;
}


gboolean vnc_connection_set_local(VncConnection *conn, struct vnc_framebuffer *fb)
{
	int i, j, n;
	int depth;

	memcpy(&conn->local, fb, sizeof(*fb));

	if (fb->bpp == (conn->fmt.bits_per_pixel / 8) &&
	    fb->red_mask == conn->fmt.red_max &&
	    fb->green_mask == conn->fmt.green_max &&
	    fb->blue_mask == conn->fmt.blue_max &&
	    fb->red_shift == conn->fmt.red_shift &&
	    fb->green_shift == conn->fmt.green_shift &&
	    fb->blue_shift == conn->fmt.blue_shift &&
	    fb->byte_order == G_BYTE_ORDER &&
	    conn->fmt.byte_order == G_BYTE_ORDER)
		conn->perfect_match = TRUE;
	else
		conn->perfect_match = FALSE;

	depth = conn->fmt.depth;
	if (depth == 32)
		depth = 24;

	conn->rm = conn->local.red_mask & conn->fmt.red_max;
	conn->gm = conn->local.green_mask & conn->fmt.green_max;
	conn->bm = conn->local.blue_mask & conn->fmt.blue_max;
	GVNC_DEBUG("Mask local: %3d %3d %3d\n"
		   "    remote: %3d %3d %3d\n"
		   "    merged: %3d %3d %3d",
		   conn->local.red_mask, conn->local.green_mask, conn->local.blue_mask,
		   conn->fmt.red_max, conn->fmt.green_max, conn->fmt.blue_max,
		   conn->rm, conn->gm, conn->bm);

	/* Setup shifts assuming matched bpp (but not necessarily match rgb order)*/
	conn->rrs = conn->fmt.red_shift;
	conn->grs = conn->fmt.green_shift;
	conn->brs = conn->fmt.blue_shift;

	conn->rls = conn->local.red_shift;
	conn->gls = conn->local.green_shift;
	conn->bls = conn->local.blue_shift;

	/* This adjusts for remote having more bpp than local */
	for (n = conn->fmt.red_max; n > conn->local.red_mask ; n>>= 1)
		conn->rrs++;
	for (n = conn->fmt.green_max; n > conn->local.green_mask ; n>>= 1)
		conn->grs++;
	for (n = conn->fmt.blue_max; n > conn->local.blue_mask ; n>>= 1)
		conn->brs++;

	/* This adjusts for remote having less bpp than remote */
	for (n = conn->local.red_mask ; n > conn->fmt.red_max ; n>>= 1)
		conn->rls++;
	for (n = conn->local.green_mask ; n > conn->fmt.green_max ; n>>= 1)
		conn->gls++;
	for (n = conn->local.blue_mask ; n > conn->fmt.blue_max ; n>>= 1)
		conn->bls++;
	GVNC_DEBUG("Pixel shifts\n   right: %3d %3d %3d\n    left: %3d %3d %3d",
		   conn->rrs, conn->grs, conn->brs,
		   conn->rls, conn->gls, conn->bls);

	i = conn->fmt.bits_per_pixel / 8;
	j = conn->local.bpp;

	if (i == 4) i = 3;
	if (j == 4) j = 3;

	conn->blt = vnc_connection_blt_table[i - 1][j - 1];
	conn->fill = vnc_connection_fill_table[i - 1][j - 1];
	conn->set_pixel_at = vnc_connection_set_pixel_at_table[i - 1][j - 1];
	conn->hextile = vnc_connection_hextile_table[i - 1][j - 1];
	conn->rich_cursor_blt = vnc_connection_rich_cursor_blt_table[i - 1];
	conn->rgb24_blt = vnc_connection_rgb24_blt_table[i - 1];
	conn->tight_compute_predicted = vnc_connection_tight_compute_predicted_table[i - 1];
	conn->tight_sum_pixel = vnc_connection_tight_sum_pixel_table[i - 1];

	if (conn->perfect_match)
		conn->blt = vnc_connection_blt_fast;

	return !vnc_connection_has_error(conn);
}

const char *vnc_connection_get_name(VncConnection *conn)
{
	return conn->name;
}

int vnc_connection_get_width(VncConnection *conn)
{
	return conn->width;
}

int vnc_connection_get_height(VncConnection *conn)
{
	return conn->height;
}

gboolean vnc_connection_using_raw_keycodes(VncConnection *conn)
{
	return conn->has_ext_key_event;
}

/*
 * Local variables:
 *  c-indent-level: 8
 *  c-basic-offset: 8
 *  tab-width: 8
 * End:
 */
