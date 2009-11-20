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
#include "vncconnectionenums.h"
#include "vncmarshal.h"

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

#ifdef HAVE_PWD_H
#include <pwd.h>
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


typedef void vnc_connection_rich_cursor_blt_func(VncConnection *conn, guint8 *, guint8 *,
						  guint8 *, int, guint16, guint16);

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

#define VNC_CONNECTION_GET_PRIVATE(obj)				\
	(G_TYPE_INSTANCE_GET_PRIVATE((obj), VNC_TYPE_CONNECTION, VncConnectionPrivate))


struct _VncConnectionPrivate
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

	VncFramebuffer *fb;
	gboolean fbSwapRemote;

	VncCursor *cursor;
	gboolean absPointer;

	vnc_connection_rich_cursor_blt_func *rich_cursor_blt;
	vnc_connection_tight_compute_predicted_func *tight_compute_predicted;
	vnc_connection_tight_sum_pixel_func *tight_sum_pixel;

	struct vnc_connection_ops ops;
	gpointer ops_data;

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

G_DEFINE_TYPE(VncConnection, vnc_connection, G_TYPE_OBJECT);


enum {
	VNC_CURSOR_CHANGED,
	VNC_POINTER_MODE_CHANGED,
	VNC_BELL,
	VNC_SERVER_CUT_TEXT,
	VNC_FRAMEBUFFER_UPDATE,
	VNC_DESKTOP_RESIZE,
	VNC_PIXEL_FORMAT_CHANGED,

	VNC_AUTH_FAILURE,
	VNC_AUTH_UNSUPPORTED,
	VNC_AUTH_CREDENTIAL,
	VNC_AUTH_CHOOSE_TYPE,
	VNC_AUTH_CHOOSE_SUBTYPE,

	VNC_LAST_SIGNAL,
};

static guint signals[VNC_LAST_SIGNAL] = { 0, 0, 0, 0,
					  0, 0, 0, 0,
					  0, 0, 0, 0};

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


enum {
	PROP_0,
	PROP_FRAMEBUFFER,
};


static void vnc_connection_get_property(GObject *object,
					guint prop_id,
					GValue *value,
					GParamSpec *pspec)
{
	VncConnection *conn = VNC_CONNECTION(object);
	VncConnectionPrivate *priv = conn->priv;

	switch (prop_id) {
	case PROP_FRAMEBUFFER:
		g_value_set_object(value, priv->fb);
		break;

	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
	}
}

static void vnc_connection_set_property(GObject *object,
					guint prop_id,
					const GValue *value,
					GParamSpec *pspec)
{
	VncConnection *conn = VNC_CONNECTION(object);

	switch (prop_id) {
	case PROP_FRAMEBUFFER:
		vnc_connection_set_framebuffer(conn, g_value_get_object(value));
		break;

        default:
            G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
        }
}

struct signal_data
{
	VncConnection *conn;
	struct coroutine *caller;

	int signum;

	union {
		VncCursor *cursor;
		gboolean absPointer;
		GString *text;
		struct {
			int x;
			int y;
			int width;
			int height;
		} area;
		struct {
			int width;
			int height;
		} size;
		VncPixelFormat *pixelFormat;
		const char *authReason;
		unsigned int authUnsupported;
		GValueArray *authCred;
		GValueArray *authTypes;
	} params;
};

static gboolean do_vnc_connection_emit_main_context(gpointer opaque)
{
	struct signal_data *data = opaque;

	switch (data->signum) {
	case VNC_CURSOR_CHANGED:
		g_signal_emit(G_OBJECT(data->conn),
			      signals[data->signum],
			      0,
			      data->params.cursor);
		break;

	case VNC_POINTER_MODE_CHANGED:
		g_signal_emit(G_OBJECT(data->conn),
			      signals[data->signum],
			      0,
			      data->params.absPointer);
		break;

	case VNC_BELL:
		g_signal_emit(G_OBJECT(data->conn),
			      signals[data->signum],
			      0);
		break;

	case VNC_SERVER_CUT_TEXT:
		g_signal_emit(G_OBJECT(data->conn),
			      signals[data->signum],
			      0,
			      data->params.text);
		break;

	case VNC_FRAMEBUFFER_UPDATE:
		g_signal_emit(G_OBJECT(data->conn),
			      signals[data->signum],
			      0,
			      data->params.area.x,
			      data->params.area.y,
			      data->params.area.width,
			      data->params.area.height);
		break;

	case VNC_DESKTOP_RESIZE:
		g_signal_emit(G_OBJECT(data->conn),
			      signals[data->signum],
			      0,
			      data->params.size.width,
			      data->params.size.height);
		break;

	case VNC_PIXEL_FORMAT_CHANGED:
		g_signal_emit(G_OBJECT(data->conn),
			      signals[data->signum],
			      0,
			      data->params.pixelFormat);
		break;

	case VNC_AUTH_FAILURE:
		g_signal_emit(G_OBJECT(data->conn),
			      signals[data->signum],
			      0,
			      data->params.authReason);
		break;

	case VNC_AUTH_UNSUPPORTED:
		g_signal_emit(G_OBJECT(data->conn),
			      signals[data->signum],
			      0,
			      data->params.authUnsupported);
		break;

	case VNC_AUTH_CREDENTIAL:
		g_signal_emit(G_OBJECT(data->conn),
			      signals[data->signum],
			      0,
			      data->params.authCred);
		break;

	case VNC_AUTH_CHOOSE_TYPE:
		g_signal_emit(G_OBJECT(data->conn),
			      signals[data->signum],
			      0,
			      data->params.authTypes);
		break;

	case VNC_AUTH_CHOOSE_SUBTYPE:
		g_signal_emit(G_OBJECT(data->conn),
			      signals[data->signum],
			      0,
			      data->conn->priv->auth_type,
			      data->params.authTypes);
		break;

	}

	coroutine_yieldto(data->caller, NULL);

	return FALSE;
}

static void vnc_connection_emit_main_context(VncConnection *conn,
					     int signum,
					     struct signal_data *data)
{
	data->conn = conn;
	data->caller = coroutine_self();
	data->signum = signum;

	g_idle_add(do_vnc_connection_emit_main_context, data);

	/* This switches to the system coroutine context, lets
	 * the idle function run to dispatch the signal, and
	 * finally returns once complete. ie this is synchronous
	 * from the POV of the VNC coroutine despite there being
	 * an idle function involved
	 */
	coroutine_yield(NULL);
}


static gboolean vnc_connection_use_compression(VncConnection *conn)
{
	VncConnectionPrivate *priv = conn->priv;

	return priv->compressed_buffer != NULL;
}

static int vnc_connection_zread(VncConnection *conn, void *buffer, size_t size)
{
	VncConnectionPrivate *priv = conn->priv;
	char *ptr = buffer;
	size_t offset = 0;

	while (offset < size) {
		/* if data is available in the uncompressed buffer, then
		 * copy */
		if (priv->uncompressed_length) {
			size_t len = MIN(priv->uncompressed_length,
					 size - offset);

			memcpy(ptr + offset,
			       priv->uncompressed_buffer,
			       len);

			priv->uncompressed_length -= len;
			if (priv->uncompressed_length)
				memmove(priv->uncompressed_buffer,
					priv->uncompressed_buffer + len,
					priv->uncompressed_length);
			offset += len;
		} else {
			int err;

			priv->strm->next_in = priv->compressed_buffer;
			priv->strm->avail_in = priv->compressed_length;
			priv->strm->next_out = priv->uncompressed_buffer;
			priv->strm->avail_out = sizeof(priv->uncompressed_buffer);

			/* inflate as much as possible */
			err = inflate(priv->strm, Z_SYNC_FLUSH);
			if (err != Z_OK) {
				errno = EIO;
				return -1;
			}

			priv->uncompressed_length = (guint8 *)priv->strm->next_out - priv->uncompressed_buffer;
			priv->compressed_length -= (guint8 *)priv->strm->next_in - priv->compressed_buffer;
			priv->compressed_buffer = priv->strm->next_in;
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
	VncConnectionPrivate *priv = conn->priv;
	int ret;

 reread:
	if (priv->tls_session) {
		ret = gnutls_read(priv->tls_session, data, len);
		if (ret < 0) {
			if (ret == GNUTLS_E_AGAIN)
				errno = EAGAIN;
			else
				errno = EIO;
			ret = -1;
		}
	} else
		ret = recv (priv->fd, data, len, 0);

	if (ret == -1) {
		switch (errno) {
		case EWOULDBLOCK:
			if (priv->wait_interruptable) {
				if (!g_io_wait_interruptable(&priv->wait,
							     priv->channel, G_IO_IN)) {
					GVNC_DEBUG("Read blocking interrupted %d", priv->has_error);
					return -EAGAIN;
				}
			} else
				g_io_wait(priv->channel, G_IO_IN);
		case EINTR:
			goto reread;

		default:
			GVNC_DEBUG("Closing the connection: vnc_connection_read() - errno=%d", errno);
			priv->has_error = TRUE;
			return -errno;
		}
	}
	if (ret == 0) {
		GVNC_DEBUG("Closing the connection: vnc_connection_read() - ret=0");
		priv->has_error = TRUE;
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
	VncConnectionPrivate *priv = conn->priv;
	size_t want;

	//GVNC_DEBUG("Read SASL %p size %d offset %d", priv->saslDecoded,
	//	   priv->saslDecodedLength, priv->saslDecodedOffset);
	if (priv->saslDecoded == NULL) {
		char encoded[8192];
		int encodedLen = sizeof(encoded);
		int err, ret;

		ret = vnc_connection_read_wire(conn, encoded, encodedLen);
		if (ret < 0) {
			return ret;
		}

		err = sasl_decode(priv->saslconn, encoded, ret,
				  &priv->saslDecoded, &priv->saslDecodedLength);
		if (err != SASL_OK) {
			GVNC_DEBUG("Failed to decode SASL data %s",
				   sasl_errstring(err, NULL, NULL));
			priv->has_error = TRUE;
			return -EINVAL;
		}
		priv->saslDecodedOffset = 0;
	}

	want = priv->saslDecodedLength - priv->saslDecodedOffset;
	if (want > sizeof(priv->read_buffer))
		want = sizeof(priv->read_buffer);

	memcpy(priv->read_buffer,
	       priv->saslDecoded + priv->saslDecodedOffset,
	       want);
	priv->saslDecodedOffset += want;
	if (priv->saslDecodedOffset == priv->saslDecodedLength) {
		priv->saslDecodedLength = priv->saslDecodedOffset = 0;
		priv->saslDecoded = NULL;
	}
	//GVNC_DEBUG("Done read write %d - %d", want, priv->has_error);
	return want;
}
#endif


/*
 * Read at least 1 more byte of data straight off the wire
 * into the internal read buffer
 */
static int vnc_connection_read_plain(VncConnection *conn)
{
	VncConnectionPrivate *priv = conn->priv;

	//GVNC_DEBUG("Read plain %d", sizeof(priv->read_buffer));
	return vnc_connection_read_wire(conn, priv->read_buffer, sizeof(priv->read_buffer));
}

/*
 * Read at least 1 more byte of data into the internal read_buffer
 */
static int vnc_connection_read_buf(VncConnection *conn)
{
	VncConnectionPrivate *priv = conn->priv;

	//GVNC_DEBUG("Start read %d", priv->has_error);
#if HAVE_SASL
	if (priv->saslconn)
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
	VncConnectionPrivate *priv = conn->priv;
	char *ptr = data;
	size_t offset = 0;

	if (priv->has_error) return -EINVAL;

	while (offset < len) {
		size_t tmp;

		/* compressed data is buffered independently of the read buffer
		 * so we must by-pass it */
		if (vnc_connection_use_compression(conn)) {
			int ret = vnc_connection_zread(conn, ptr + offset, len);
			if (ret == -1) {
				GVNC_DEBUG("Closing the connection: vnc_connection_read() - zread() failed");
				priv->has_error = TRUE;
				return -errno;
			}
			offset += ret;
			continue;
		} else if (priv->read_offset == priv->read_size) {
			int ret = vnc_connection_read_buf(conn);

			if (ret < 0)
				return ret;
			priv->read_offset = 0;
			priv->read_size = ret;
		}

		tmp = MIN(priv->read_size - priv->read_offset, len - offset);

		memcpy(ptr + offset, priv->read_buffer + priv->read_offset, tmp);

		priv->read_offset += tmp;
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
	VncConnectionPrivate *priv = conn->priv;
	const char *ptr = data;
	size_t offset = 0;
	//GVNC_DEBUG("Flush write %p %d", data, datalen);
	while (offset < datalen) {
		int ret;

		if (priv->tls_session) {
			ret = gnutls_write(priv->tls_session,
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
			ret = send (priv->fd,
				    ptr+offset,
				    datalen-offset, 0);
		if (ret == -1) {
			switch (errno) {
			case EWOULDBLOCK:
				g_io_wait(priv->channel, G_IO_OUT);
			case EINTR:
				continue;
			default:
				GVNC_DEBUG("Closing the connection: vnc_connection_flush %d", errno);
				priv->has_error = TRUE;
				return;
			}
		}
		if (ret == 0) {
			GVNC_DEBUG("Closing the connection: vnc_connection_flush");
			priv->has_error = TRUE;
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
	VncConnectionPrivate *priv = conn->priv;
	const char *output;
	unsigned int outputlen;
	int err;

	err = sasl_encode(priv->saslconn,
			  priv->write_buffer,
			  priv->write_offset,
			  &output, &outputlen);
	if (err != SASL_OK) {
		GVNC_DEBUG("Failed to encode SASL data %s",
			   sasl_errstring(err, NULL, NULL));
		priv->has_error = TRUE;
		return;
	}
	//GVNC_DEBUG("Flush SASL %d: %p %d", priv->write_offset, output, outputlen);
	vnc_connection_flush_wire(conn, output, outputlen);
}
#endif

/*
 * Write all buffered data straight out to the wire
 */
static void vnc_connection_flush_plain(VncConnection *conn)
{
	VncConnectionPrivate *priv = conn->priv;

	//GVNC_DEBUG("Flush plain %d", priv->write_offset);
	vnc_connection_flush_wire(conn,
				  priv->write_buffer,
				  priv->write_offset);
}


/*
 * Write all buffered data out to the wire
 */
static void vnc_connection_flush(VncConnection *conn)
{
	VncConnectionPrivate *priv = conn->priv;

	//GVNC_DEBUG("STart write %d", priv->has_error);
#if HAVE_SASL
	if (priv->saslconn)
		vnc_connection_flush_sasl(conn);
	else
#endif
		vnc_connection_flush_plain(conn);
	priv->write_offset = 0;
}

static void vnc_connection_write(VncConnection *conn, const void *data, size_t len)
{
	VncConnectionPrivate *priv = conn->priv;
	const char *ptr = data;
	size_t offset = 0;

	while (offset < len) {
		ssize_t tmp;

		if (priv->write_offset == sizeof(priv->write_buffer)) {
			vnc_connection_flush(conn);
		}

		tmp = MIN(sizeof(priv->write_buffer), len - offset);

		memcpy(priv->write_buffer+priv->write_offset, ptr + offset, tmp);

		priv->write_offset += tmp;
		offset += tmp;
	}
}


static ssize_t vnc_connection_tls_push(gnutls_transport_ptr_t transport,
				       const void *data,
				       size_t len) {
	VncConnection *conn = transport;
	VncConnectionPrivate *priv = conn->priv;
	int ret;

 retry:
	ret = write(priv->fd, data, len);
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
	VncConnectionPrivate *priv = conn->priv;
	int ret;

 retry:
	ret = read(priv->fd, data, len);
	if (ret < 0) {
		if (errno == EINTR)
			goto retry;
		return -1;
	}
	return ret;
}

static size_t vnc_connection_pixel_size(VncConnection *conn)
{
	VncConnectionPrivate *priv = conn->priv;

	return priv->fmt.bits_per_pixel / 8;
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
	VncConnectionPrivate *priv = conn->priv;
	int ret;

	priv->wait_interruptable = 1;
	ret = vnc_connection_read(conn, value, sizeof(*value));
	priv->wait_interruptable = 0;

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
	VncConnectionPrivate *priv = conn->priv;
	gnutls_certificate_credentials_t x509_cred;
	int ret;

	if ((ret = gnutls_certificate_allocate_credentials(&x509_cred)) < 0) {
		GVNC_DEBUG("Cannot allocate credentials %s", gnutls_strerror(ret));
		return NULL;
	}
	if (priv->cred_x509_cacert) {
		if ((ret = gnutls_certificate_set_x509_trust_file(x509_cred,
								  priv->cred_x509_cacert,
								  GNUTLS_X509_FMT_PEM)) < 0) {
			GVNC_DEBUG("Cannot load CA certificate %s", gnutls_strerror(ret));
			return NULL;
		}
	} else {
		GVNC_DEBUG("No CA certificate provided");
		return NULL;
	}

	if (priv->cred_x509_cert && priv->cred_x509_key) {
		if ((ret = gnutls_certificate_set_x509_key_file (x509_cred,
								 priv->cred_x509_cert,
								 priv->cred_x509_key,
								 GNUTLS_X509_FMT_PEM)) < 0) {
			GVNC_DEBUG("Cannot load certificate & key %s", gnutls_strerror(ret));
			return NULL;
		}
	} else {
		GVNC_DEBUG("No client cert or key provided");
	}

	if (priv->cred_x509_cacrl) {
		if ((ret = gnutls_certificate_set_x509_crl_file(x509_cred,
								priv->cred_x509_cacrl,
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
	VncConnectionPrivate *priv = conn->priv;
	int ret;
	unsigned int status;
	const gnutls_datum_t *certs;
	unsigned int nCerts, i;
	time_t now;

	GVNC_DEBUG("Validating");
	if ((ret = gnutls_certificate_verify_peers2 (priv->tls_session, &status)) < 0) {
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

	if (gnutls_certificate_type_get(priv->tls_session) != GNUTLS_CRT_X509)
		return FALSE;

	if (!(certs = gnutls_certificate_get_peers(priv->tls_session, &nCerts)))
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
			if (!priv->host) {
				GVNC_DEBUG ("No hostname provided for certificate verification");
				gnutls_x509_crt_deinit (cert);
				return FALSE;
			}
			if (!gnutls_x509_crt_check_hostname (cert, priv->host)) {
				GVNC_DEBUG ("The certificate's owner does not match hostname '%s'",
					    priv->host);
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
	VncConnectionPrivate *priv = conn->priv;

	return priv->has_error;
}

gboolean vnc_connection_set_pixel_format(VncConnection *conn,
					 const VncPixelFormat *fmt)
{
	VncConnectionPrivate *priv = conn->priv;
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

	memcpy(&priv->fmt, fmt, sizeof(*fmt));

	return !vnc_connection_has_error(conn);
}


const VncPixelFormat *vnc_connection_get_pixel_format(VncConnection *conn)
{
	VncConnectionPrivate *priv = conn->priv;

	return &priv->fmt;
}

gboolean vnc_connection_set_encodings(VncConnection *conn, int n_encoding, gint32 *encoding)
{
	VncConnectionPrivate *priv = conn->priv;
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
		if (priv->fmt.depth == 32 &&
		    (priv->fmt.red_max > 255 ||
		     priv->fmt.blue_max > 255 ||
		     priv->fmt.green_max > 255) &&
		    encoding[i] == GVNC_ENCODING_ZRLE) {
			GVNC_DEBUG("Dropping ZRLE encoding for broken pixel format");
			skip_zrle++;
		}

	priv->has_ext_key_event = FALSE;
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
	VncConnectionPrivate *priv = conn->priv;
	size_t left;

	left = priv->xmit_buffer_capacity - priv->xmit_buffer_size;
	if (left < size) {
		priv->xmit_buffer_capacity += size + 4095;
		priv->xmit_buffer_capacity &= ~4095;

		priv->xmit_buffer = g_realloc(priv->xmit_buffer, priv->xmit_buffer_capacity);
	}

	memcpy(&priv->xmit_buffer[priv->xmit_buffer_size],
	       data, size);

	priv->xmit_buffer_size += size;
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
	VncConnectionPrivate *priv = conn->priv;

	g_io_wakeup(&priv->wait);
}

gboolean vnc_connection_key_event(VncConnection *conn, guint8 down_flag,
				  guint32 key, guint16 scancode)
{
	VncConnectionPrivate *priv = conn->priv;
	guint8 pad[2] = {0};

	GVNC_DEBUG("Key event %d %d %d %d", key, scancode, down_flag, priv->has_ext_key_event);
	if (priv->has_ext_key_event) {
		scancode = x_keycode_to_pc_keycode(priv->keycode_map, scancode);

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
	VncConnectionPrivate *priv = conn->priv;
	const VncPixelFormat *local = vnc_framebuffer_get_local_format(priv->fb);
	int rowstride = vnc_framebuffer_get_rowstride(priv->fb);

	return vnc_framebuffer_get_buffer(priv->fb) +
		(y * rowstride) +
		(x * (local->bits_per_pixel / 8));
}


static guint8 vnc_connection_swap_rfb_8(VncConnection *conn G_GNUC_UNUSED, guint8 pixel)
{
	return pixel;
}

/* VNC server RFB  format ->  local host native format */
static guint16 vnc_connection_swap_rfb_16(VncConnection *conn, guint16 pixel)
{
	VncConnectionPrivate *priv = conn->priv;

	if (priv->fbSwapRemote)
		return  (((pixel >> 8) & 0xFF) << 0) |
			(((pixel >> 0) & 0xFF) << 8);
	else
		return pixel;
}

/* VNC server RFB  format ->  local host native format */
static guint32 vnc_connection_swap_rfb_32(VncConnection *conn, guint32 pixel)
{
	VncConnectionPrivate *priv = conn->priv;

	if (priv->fbSwapRemote)
		return  (((pixel >> 24) & 0xFF) <<  0) |
			(((pixel >> 16) & 0xFF) <<  8) |
			(((pixel >>  8) & 0xFF) << 16) |
			(((pixel >>  0) & 0xFF) << 24);
	else
		return pixel;
}

#define SRC 8
#define DST 8
#include "vncconnectionblt.h"
#undef SRC
#undef DST

#define SRC 8
#define DST 16
#include "vncconnectionblt.h"
#undef SRC
#undef DST

#define SRC 8
#define DST 32
#include "vncconnectionblt.h"
#undef SRC
#undef DST


#define SRC 16
#define DST 8
#include "vncconnectionblt.h"
#undef SRC
#undef DST

#define SRC 16
#define DST 16
#include "vncconnectionblt.h"
#undef SRC
#undef DST

#define SRC 16
#define DST 32
#include "vncconnectionblt.h"
#undef SRC
#undef DST


#define SRC 32
#define DST 8
#include "vncconnectionblt.h"
#undef SRC
#undef DST

#define SRC 32
#define DST 16
#include "vncconnectionblt.h"
#undef SRC
#undef DST

#define SRC 32
#define DST 32
#include "vncconnectionblt.h"
#undef SRC
#undef DST

static vnc_connection_rich_cursor_blt_func *vnc_connection_rich_cursor_blt_table[3] = {
	vnc_connection_rich_cursor_blt_8x32,
	vnc_connection_rich_cursor_blt_16x32,
	vnc_connection_rich_cursor_blt_32x32,
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


static void vnc_connection_raw_update(VncConnection *conn,
				      guint16 x, guint16 y,
				      guint16 width, guint16 height)
{
	VncConnectionPrivate *priv = conn->priv;

	/* optimize for perfect match between server/client
	   FWIW, in the local case, we ought to be doing a write
	   directly from the source framebuffer and a read directly
	   into the client framebuffer
	*/
	if (vnc_framebuffer_perfect_format_match(priv->fb)) {
		int i;
		int rowstride = vnc_framebuffer_get_rowstride(priv->fb);
		guint8 *dst = vnc_framebuffer_get_buffer(priv->fb);

		dst += (y * rowstride) + (x * (priv->fmt.bits_per_pixel/8));

		for (i = 0; i < height; i++) {
			vnc_connection_read(conn, dst,
					    width * (priv->fmt.bits_per_pixel/8));
			dst += rowstride;
		}
	} else {
		guint8 *dst;
		int i;

		dst = g_malloc(width * (priv->fmt.bits_per_pixel / 8));
		for (i = 0; i < height; i++) {
			vnc_connection_read(conn, dst, width * (priv->fmt.bits_per_pixel / 8));
			vnc_framebuffer_blt(priv->fb, dst, 0, x, y + i, width, 1);
		}
		g_free(dst);
	}
}

static void vnc_connection_copyrect_update(VncConnection *conn,
					   guint16 dst_x, guint16 dst_y,
					   guint16 width, guint16 height)
{
	VncConnectionPrivate *priv = conn->priv;
	int src_x, src_y;

	src_x = vnc_connection_read_u16(conn);
	src_y = vnc_connection_read_u16(conn);

	vnc_framebuffer_copyrect(priv->fb,
				 src_x, src_y,
				 dst_x, dst_y,
				 width, height);
}

static void vnc_connection_hextile_rect(VncConnection *conn,
					guint8 flags,
					guint16 x, guint16 y,
					guint16 width, guint16 height,
					guint8 *fg, guint8 *bg)
{
	VncConnectionPrivate *priv = conn->priv;
	int i;

	if (flags & 0x01) {
		vnc_connection_raw_update(conn, x, y, width, height);
	} else {
		/* Background Specified */
		if (flags & 0x02)
			vnc_connection_read_pixel(conn, bg);

		/* Foreground Specified */
		if (flags & 0x04)
			vnc_connection_read_pixel(conn, fg);

		vnc_framebuffer_fill(priv->fb, bg, x, y, width, height);

		/* AnySubrects */
		if (flags & 0x08) {
			guint8 n_rects = vnc_connection_read_u8(conn);

			for (i = 0; i < n_rects; i++) {
				guint8 xy, wh;

				/* SubrectsColored */
				if (flags & 0x10)
					vnc_connection_read_pixel(conn, fg);

				xy = vnc_connection_read_u8(conn);
				wh = vnc_connection_read_u8(conn);

				vnc_framebuffer_fill(priv->fb, fg,
						     x + nibhi(xy), y + niblo(xy),
						     nibhi(wh) + 1, niblo(wh) + 1);
			}
		}
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
			vnc_connection_hextile_rect(conn, flags,
						    x + i, y + j,
						    w, h,
						    fg, bg);
		}
	}
}

static void vnc_connection_rre_update(VncConnection *conn,
				      guint16 x, guint16 y,
				      guint16 width, guint16 height)
{
	VncConnectionPrivate *priv = conn->priv;
	guint8 bg[4];
	guint32 num;
	guint32 i;

	num = vnc_connection_read_u32(conn);
	vnc_connection_read_pixel(conn, bg);
	vnc_framebuffer_fill(priv->fb, bg, x, y, width, height);

	for (i = 0; i < num; i++) {
		guint8 fg[4];
		guint16 sub_x, sub_y, sub_w, sub_h;

		vnc_connection_read_pixel(conn, fg);
		sub_x = vnc_connection_read_u16(conn);
		sub_y = vnc_connection_read_u16(conn);
		sub_w = vnc_connection_read_u16(conn);
		sub_h = vnc_connection_read_u16(conn);

		vnc_framebuffer_fill(priv->fb, fg,
				     x + sub_x, y + sub_y, sub_w, sub_h);
	}
}

/* CPIXELs are optimized slightly.  32-bit pixel values are packed into 24-bit
 * values. */
static void vnc_connection_read_cpixel(VncConnection *conn, guint8 *pixel)
{
	VncConnectionPrivate *priv = conn->priv;
	int bpp = vnc_connection_pixel_size(conn);

	memset(pixel, 0, bpp);

	if (bpp == 4 && priv->fmt.true_color_flag) {
		int fitsInMSB = ((priv->fmt.red_shift > 7) &&
				 (priv->fmt.green_shift > 7) &&
				 (priv->fmt.blue_shift > 7));
		int fitsInLSB = (((priv->fmt.red_max << priv->fmt.red_shift) < (1 << 24)) &&
				 ((priv->fmt.green_max << priv->fmt.green_shift) < (1 << 24)) &&
				 ((priv->fmt.blue_max << priv->fmt.blue_shift) < (1 << 24)));

		/*
		 * We need to analyse the shifts to see if they fit in 3 bytes,
		 * rather than looking at the declared  'depth' for the format
		 * because despite what the RFB spec says, this is what RealVNC
		 * server actually does in practice.
		 */
		if (fitsInMSB || fitsInLSB) {
			bpp = 3;
			if (priv->fmt.depth == 24 &&
			    priv->fmt.byte_order == G_BIG_ENDIAN)
				pixel++;
		}
	}

	vnc_connection_read(conn, pixel, bpp);
}

static void vnc_connection_zrle_update_tile_blit(VncConnection *conn,
						 guint16 x, guint16 y,
						 guint16 width, guint16 height)
{
	VncConnectionPrivate *priv = conn->priv;
	guint8 blit_data[4 * 64 * 64];
	int i, bpp;

	bpp = vnc_connection_pixel_size(conn);

	for (i = 0; i < width * height; i++)
		vnc_connection_read_cpixel(conn, blit_data + (i * bpp));

	vnc_framebuffer_blt(priv->fb, blit_data, width * bpp, x, y, width, height);
}

static guint8 vnc_connection_read_zrle_pi(VncConnection *conn, int palette_size)
{
	VncConnectionPrivate *priv = conn->priv;
	guint8 pi = 0;

	if (priv->zrle_pi_bits == 0) {
		priv->zrle_pi = vnc_connection_read_u8(conn);
		priv->zrle_pi_bits = 8;
	}
	if ( palette_size == 2) {
		pi = (priv->zrle_pi >> (priv->zrle_pi_bits - 1)) & 1;
		priv->zrle_pi_bits -= 1;
	} else if ((palette_size == 3) || (palette_size == 4)) {
		pi = (priv->zrle_pi >> (priv->zrle_pi_bits - 2)) & 3;
		priv->zrle_pi_bits -= 2;
	} else if ((palette_size >=5) && (palette_size <=16)){
		pi = (priv->zrle_pi >> (priv->zrle_pi_bits - 4)) & 15;
		priv->zrle_pi_bits -= 4;
	}

	return pi;
}

static void vnc_connection_zrle_update_tile_palette(VncConnection *conn,
						    guint8 palette_size,
						    guint16 x, guint16 y,
						    guint16 width, guint16 height)
{
	VncConnectionPrivate *priv = conn->priv;
	guint8 palette[128][4];
	int i, j;

	for (i = 0; i < palette_size; i++)
		vnc_connection_read_cpixel(conn, palette[i]);

	for (j = 0; j < height; j++) {
		/* discard any padding bits */
		priv->zrle_pi_bits = 0;

		for (i = 0; i < width; i++) {
			int ind = vnc_connection_read_zrle_pi(conn, palette_size);

			vnc_framebuffer_set_pixel_at(priv->fb, palette[ind & 0x7F],
						     x + i, y + j);
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
	VncConnectionPrivate *priv = conn->priv;
	int i, j, rl = 0;
	guint8 pixel[4];

	for (j = 0; j < height; j++) {
		for (i = 0; i < width; i++) {
			if (rl == 0) {
				vnc_connection_read_cpixel(conn, pixel);
				rl = vnc_connection_read_zrle_rl(conn);
			}
			vnc_framebuffer_set_pixel_at(priv->fb, pixel, x + i, y + j);
			rl -= 1;
		}
	}
}

static void vnc_connection_zrle_update_tile_prle(VncConnection *conn,
						 guint8 palette_size,
						 guint16 x, guint16 y,
						 guint16 width, guint16 height)
{
	VncConnectionPrivate *priv = conn->priv;
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

			vnc_framebuffer_set_pixel_at(priv->fb, palette[pi], x + i, y + j);
			rl -= 1;
		}
	}
}

static void vnc_connection_zrle_update_tile(VncConnection *conn, guint16 x, guint16 y,
					    guint16 width, guint16 height)
{
	VncConnectionPrivate *priv = conn->priv;
	guint8 subencoding = vnc_connection_read_u8(conn);
	guint8 pixel[4];

	if (subencoding == 0 ) {
		/* Raw pixel data */
		vnc_connection_zrle_update_tile_blit(conn, x, y, width, height);
	} else if (subencoding == 1) {
		/* Solid tile of a single color */
		vnc_connection_read_cpixel(conn, pixel);
		vnc_framebuffer_fill(priv->fb, pixel, x, y, width, height);
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
	VncConnectionPrivate *priv = conn->priv;
	guint32 length;
	guint32 offset;
	guint16 i, j;
	guint8 *zlib_data;

	length = vnc_connection_read_u32(conn);
	zlib_data = g_malloc(length);
	vnc_connection_read(conn, zlib_data, length);

	/* setup subsequent calls to vnc_connection_read*() to use the compressed data */
	priv->uncompressed_length = 0;
	priv->compressed_length = length;
	priv->compressed_buffer = zlib_data;
	priv->strm = &priv->streams[0];

	offset = 0;
	for (j = 0; j < height; j += 64) {
		for (i = 0; i < width; i += 64) {
			guint16 w, h;

			w = MIN(width - i, 64);
			h = MIN(height - j, 64);
			vnc_connection_zrle_update_tile(conn, x + i, y + j, w, h);
		}
	}

	priv->strm = NULL;
	priv->uncompressed_length = 0;
	priv->compressed_length = 0;
	priv->compressed_buffer = NULL;

	g_free(zlib_data);
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
	VncConnectionPrivate *priv = conn->priv;

	if (priv->fmt.depth == 24)
		return 3;
	return priv->fmt.bits_per_pixel / 8;
}

static void vnc_connection_read_tpixel(VncConnection *conn, guint8 *pixel)
{
	VncConnectionPrivate *priv = conn->priv;

	if (priv->fmt.depth == 24) {
		uint32_t val;
		vnc_connection_read(conn, pixel, 3);
		val = (pixel[0] << priv->fmt.red_shift)
			| (pixel[1] << priv->fmt.green_shift)
			| (pixel[2] << priv->fmt.blue_shift);

		if (priv->fmt.byte_order != G_BYTE_ORDER)
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
	VncConnectionPrivate *priv = conn->priv;
	guint8 pixel[4];
	int i, j;

	for (j = 0; j < height; j++) {
		for (i = 0; i < width; i++) {
			vnc_connection_read_tpixel(conn, pixel);
			vnc_framebuffer_set_pixel_at(priv->fb, pixel, x + i, y + j);
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
	VncConnectionPrivate *priv = conn->priv;
	int i, j;

	for (j = 0; j < height; j++) {
		guint8 ra = 0;

		for (i = 0; i < width; i++) {
			guint8 ind;

			ind = vnc_connection_tight_get_pi(conn, &ra, i, palette_size);
			vnc_framebuffer_set_pixel_at(priv->fb, &palette[ind * 4], x + i, y + j);
		}
	}
}

static void vnc_connection_tight_compute_predicted(VncConnection *conn, guint8 *ppixel,
						   guint8 *lp, guint8 *cp,
						   guint8 *llp)
{
	VncConnectionPrivate *priv = conn->priv;

	priv->tight_compute_predicted(conn, ppixel, lp, cp, llp);
}

static void vnc_connection_tight_sum_pixel(VncConnection *conn,
					   guint8 *lhs, guint8 *rhs)
{
	VncConnectionPrivate *priv = conn->priv;

	priv->tight_sum_pixel(conn, lhs, rhs);
}

static void vnc_connection_tight_update_gradient(VncConnection *conn,
						 guint16 x, guint16 y,
						 guint16 width, guint16 height)
{
	int i, j;
	guint8 zero_pixel[4];
	guint8 *last_row, *row;
	int bpp;
	VncConnectionPrivate *priv = conn->priv;

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
		vnc_framebuffer_blt(priv->fb, row, width * bpp, x, y + j, width, 1);

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
	VncConnectionPrivate *priv = conn->priv;

	vnc_framebuffer_rgb24_blt(priv->fb, data, stride, x, y, w, h);
}

static void vnc_connection_tight_update_jpeg(VncConnection *conn, guint16 x, guint16 y,
					     guint16 width, guint16 height,
					     guint8 *data, size_t length)
{
	VncConnectionPrivate *priv = conn->priv;

	if (priv->ops.render_jpeg == NULL)
		return;

	priv->ops.render_jpeg(priv->ops_data, jpeg_draw, conn,
			      x, y, width, height, data, length);
}

static void vnc_connection_tight_update(VncConnection *conn,
					guint16 x, guint16 y,
					guint16 width, guint16 height)
{
	VncConnectionPrivate *priv = conn->priv;
	guint8 ccontrol;
	guint8 pixel[4];
	int i;

	ccontrol = vnc_connection_read_u8(conn);

	for (i = 0; i < 4; i++) {
		if (ccontrol & (1 << i)) {
			inflateEnd(&priv->streams[i + 1]);
			inflateInit(&priv->streams[i + 1]);
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

		priv->strm = &priv->streams[(ccontrol & 0x03) + 1];

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

			priv->uncompressed_length = 0;
			priv->compressed_length = zlib_length;
			priv->compressed_buffer = zlib_data;
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
			priv->has_error = TRUE;
			break;
		}

		if (data_size >= 12) {
			priv->uncompressed_length = 0;
			priv->compressed_length = 0;
			priv->compressed_buffer = NULL;

			g_free(zlib_data);
		}

		priv->strm = NULL;
	} else if (ccontrol == 8) {
		/* fill */
		/* FIXME check each width; endianness */
		vnc_connection_read_tpixel(conn, pixel);
		vnc_framebuffer_fill(priv->fb, pixel, x, y, width, height);
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
		priv->has_error = TRUE;
	}
}

static void vnc_connection_update(VncConnection *conn, int x, int y, int width, int height)
{
	VncConnectionPrivate *priv = conn->priv;
	struct signal_data sigdata;

	if (priv->has_error)
		return;

	GVNC_DEBUG("Notify update area (%dx%d) at location %d,%d", width, height, x, y);

	sigdata.params.area.x = x;
	sigdata.params.area.y = y;
	sigdata.params.area.width = width;
	sigdata.params.area.height = height;
	vnc_connection_emit_main_context(conn, VNC_FRAMEBUFFER_UPDATE, &sigdata);
}

static void vnc_connection_set_color_map_entry(VncConnection *conn, guint16 color,
					       guint16 red, guint16 green,
					       guint16 blue)
{
	VncConnectionPrivate *priv = conn->priv;

	if (priv->has_error || !priv->ops.set_color_map_entry)
		return;
	if (!priv->ops.set_color_map_entry(priv->ops_data, color,
					   red, green, blue)) {
		GVNC_DEBUG("Closing the connection: vnc_connection_set_color_map_entry");
		priv->has_error = TRUE;
	}
}

static void vnc_connection_bell(VncConnection *conn)
{
	VncConnectionPrivate *priv = conn->priv;
	struct signal_data sigdata;

	if (priv->has_error)
		return;

	GVNC_DEBUG("Server beep");

	vnc_connection_emit_main_context(conn, VNC_BELL, &sigdata);
}

static void vnc_connection_server_cut_text(VncConnection *conn,
					   const void *data,
					   size_t len)
{
	VncConnectionPrivate *priv = conn->priv;
	struct signal_data sigdata;
	GString *text;

	if (priv->has_error)
		return;

	text = g_string_new_len ((const gchar *)data, len);
	sigdata.params.text = text;

	vnc_connection_emit_main_context(conn, VNC_SERVER_CUT_TEXT, &sigdata);

	g_free(text);
}

static void vnc_connection_resize(VncConnection *conn, int width, int height)
{
	VncConnectionPrivate *priv = conn->priv;
	struct signal_data sigdata;

	if (priv->has_error)
		return;

	priv->width = width;
	priv->height = height;

	sigdata.params.size.width = width;
	sigdata.params.size.height = height;
	vnc_connection_emit_main_context(conn, VNC_DESKTOP_RESIZE, &sigdata);
}

static void vnc_connection_pixel_format(VncConnection *conn)
{
	VncConnectionPrivate *priv = conn->priv;
	struct signal_data sigdata;

        if (priv->has_error)
                return;

	sigdata.params.pixelFormat = &priv->fmt;
	vnc_connection_emit_main_context(conn, VNC_PIXEL_FORMAT_CHANGED, &sigdata);
}

static void vnc_connection_pointer_type_change(VncConnection *conn, gboolean absPointer)
{
	VncConnectionPrivate *priv = conn->priv;
	struct signal_data sigdata;

	if (priv->absPointer == absPointer)
		return;
	priv->absPointer = absPointer;

	if (priv->has_error)
		return;

	sigdata.params.absPointer = absPointer;
	vnc_connection_emit_main_context(conn, VNC_POINTER_MODE_CHANGED, &sigdata);
}

static void vnc_connection_rich_cursor_blt(VncConnection *conn, guint8 *pixbuf,
					   guint8 *image, guint8 *mask,
					   int pitch, guint16 width, guint16 height)
{
	VncConnectionPrivate *priv = conn->priv;

	priv->rich_cursor_blt(conn, pixbuf, image, mask, pitch, width, height);
}

static void vnc_connection_rich_cursor(VncConnection *conn, int x, int y, int width, int height)
{
	VncConnectionPrivate *priv = conn->priv;
	struct signal_data sigdata;

	if (priv->cursor) {
		g_object_unref(priv->cursor);
		priv->cursor = NULL;
	}

	if (width && height) {
		guint8 *pixbuf = NULL;
		guint8 *image = NULL, *mask = NULL;
		int imagelen, masklen;

		imagelen = width * height * (priv->fmt.bits_per_pixel / 8);
		masklen = ((width + 7)/8) * height;

		image = g_malloc(imagelen);
		mask = g_malloc(masklen);
		pixbuf = g_malloc(width * height * 4); /* RGB-A 8bit */

		vnc_connection_read(conn, image, imagelen);
		vnc_connection_read(conn, mask, masklen);

		vnc_connection_rich_cursor_blt(conn, pixbuf, image, mask,
					       width * (priv->fmt.bits_per_pixel/8),
					       width, height);

		g_free(image);
		g_free(mask);

		priv->cursor = vnc_cursor_new(pixbuf, x, y, width, height);
	}

	if (priv->has_error)
		return;

	sigdata.params.cursor = priv->cursor;

	vnc_connection_emit_main_context(conn, VNC_CURSOR_CHANGED, &sigdata);
}

static void vnc_connection_xcursor(VncConnection *conn, int x, int y, int width, int height)
{
	VncConnectionPrivate *priv = conn->priv;
	struct signal_data sigdata;

	if (priv->cursor) {
		g_object_unref(priv->cursor);
		priv->cursor = NULL;
	}

	if (width && height) {
		guint8 *pixbuf = NULL;
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

		priv->cursor = vnc_cursor_new(pixbuf, x, y, width, height);
	}

	if (priv->has_error)
		return;

	sigdata.params.cursor = priv->cursor;

	vnc_connection_emit_main_context(conn, VNC_CURSOR_CHANGED, &sigdata);
}

static void vnc_connection_ext_key_event(VncConnection *conn)
{
	VncConnectionPrivate *priv = conn->priv;

	priv->has_ext_key_event = TRUE;
	priv->keycode_map = x_keycode_to_pc_keycode_map();
}

static void vnc_connection_framebuffer_update(VncConnection *conn, gint32 etype,
					      guint16 x, guint16 y,
					      guint16 width, guint16 height)
{
	VncConnectionPrivate *priv = conn->priv;

	GVNC_DEBUG("FramebufferUpdate type=%d area (%dx%d) at location %d,%d",
		   etype, width, height, x, y);

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
                vnc_connection_read_pixel_format(conn, &priv->fmt);
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
		priv->has_error = TRUE;
		break;
	}
}

gboolean vnc_connection_server_message(VncConnection *conn)
{
	VncConnectionPrivate *priv = conn->priv;
	guint8 msg;
	int ret;

	/* NB: make sure that all server message functions
	   handle has_error appropriately */

	do {
		if (priv->xmit_buffer_size) {
			vnc_connection_write(conn, priv->xmit_buffer, priv->xmit_buffer_size);
			vnc_connection_flush(conn);
			priv->xmit_buffer_size = 0;
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
			priv->has_error = TRUE;
			break;
		}

		data = g_new(char, n_text + 1);
		if (data == NULL) {
			GVNC_DEBUG("Closing the connection: vnc_connection_server_message() - cutText - !data");
			priv->has_error = TRUE;
			break;
		}

		vnc_connection_read(conn, data, n_text);
		data[n_text] = 0;

		vnc_connection_server_cut_text(conn, data, n_text);
		g_free(data);
	}	break;
	default:
		GVNC_DEBUG("Received an unknown message: %u", msg);
		priv->has_error = TRUE;
		break;
	}

	return !vnc_connection_has_error(conn);
}

static gboolean vnc_connection_has_credentials(gpointer data)
{
	VncConnection *conn = data;
	VncConnectionPrivate *priv = conn->priv;

	if (priv->has_error)
		return TRUE;
	if (priv->want_cred_username && !priv->cred_username)
		return FALSE;
	if (priv->want_cred_password && !priv->cred_password)
		return FALSE;
	/*
	 * For x509 we require a minimum of the CA cert.
	 * Anything else is a bonus - though the server
	 * may reject auth if it decides it wants a client
	 * cert. We can't express that based on auth type
	 * alone though - we'll merely find out when TLS
	 * negotiation takes place.
	 */
	if (priv->want_cred_x509 && !priv->cred_x509_cacert)
		return FALSE;
	return TRUE;
}

static gboolean vnc_connection_gather_credentials(VncConnection *conn)
{
	VncConnectionPrivate *priv = conn->priv;

	if (priv->has_error)
		return FALSE;

	if (!vnc_connection_has_credentials(conn)) {
		GValueArray *authCred;
		GValue username, password, clientname;
		struct signal_data sigdata;

		memset(&username, 0, sizeof(username));
		memset(&password, 0, sizeof(password));
		memset(&clientname, 0, sizeof(clientname));

		authCred = g_value_array_new(0);
		if (priv->want_cred_username) {
			g_value_init(&username, VNC_TYPE_CONNECTION_CREDENTIAL);
			g_value_set_enum(&username, VNC_CONNECTION_CREDENTIAL_USERNAME);
			authCred = g_value_array_append(authCred, &username);
		}
		if (priv->want_cred_password) {
			g_value_init(&password, VNC_TYPE_CONNECTION_CREDENTIAL);
			g_value_set_enum(&password, VNC_CONNECTION_CREDENTIAL_PASSWORD);
			authCred = g_value_array_append(authCred, &password);
		}
		if (priv->want_cred_x509) {
			g_value_init(&clientname, VNC_TYPE_CONNECTION_CREDENTIAL);
			g_value_set_enum(&clientname, VNC_CONNECTION_CREDENTIAL_CLIENTNAME);
			authCred = g_value_array_append(authCred, &clientname);
		}

		sigdata.params.authCred = authCred;
		GVNC_DEBUG("Requesting missing credentials");
		vnc_connection_emit_main_context(conn, VNC_AUTH_CREDENTIAL, &sigdata);

		g_value_array_free(authCred);

		if (priv->has_error)
			return FALSE;
		GVNC_DEBUG("Waiting for missing credentials");
		g_condition_wait(vnc_connection_has_credentials, conn);
		GVNC_DEBUG("Got all credentials");
	}
	return !vnc_connection_has_error(conn);
}


static gboolean vnc_connection_check_auth_result(VncConnection *conn)
{
	VncConnectionPrivate *priv = conn->priv;
	guint32 result;

	GVNC_DEBUG("Checking auth result");
	result = vnc_connection_read_u32(conn);
	if (!result) {
		GVNC_DEBUG("Success");
		return TRUE;
	}

	if (priv->minor >= 8) {
		guint32 len;
		char reason[1024];
		len = vnc_connection_read_u32(conn);
		if (len > (sizeof(reason)-1))
			return FALSE;
		vnc_connection_read(conn, reason, len);
		reason[len] = '\0';
		GVNC_DEBUG("Fail %s", reason);
		if (!priv->has_error) {
			struct signal_data sigdata;
			sigdata.params.authReason = reason;
			vnc_connection_emit_main_context(conn, VNC_AUTH_FAILURE, &sigdata);
		}
	} else {
		GVNC_DEBUG("Fail auth no result");
		if (!priv->has_error) {
			struct signal_data sigdata;
			sigdata.params.authReason = "Unknown authentication failure";
			vnc_connection_emit_main_context(conn, VNC_AUTH_FAILURE, &sigdata);
		}
	}
	return FALSE;
}

static gboolean vnc_connection_perform_auth_vnc(VncConnection *conn)
{
	VncConnectionPrivate *priv = conn->priv;
	guint8 challenge[16];
	guint8 key[8];

	GVNC_DEBUG("Do Challenge");
	priv->want_cred_password = TRUE;
	priv->want_cred_username = FALSE;
	priv->want_cred_x509 = FALSE;
	if (!vnc_connection_gather_credentials(conn))
		return FALSE;

	if (!priv->cred_password)
		return FALSE;

	vnc_connection_read(conn, challenge, 16);

	memset(key, 0, 8);
	strncpy((char*)key, (char*)priv->cred_password, 8);

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
vncEncryptBytes2(unsigned char *where, const int length, unsigned char *key)
{
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
	VncConnectionPrivate *priv = conn->priv;
	struct vnc_dh *dh;
	guchar gen[8], mod[8], resp[8], pub[8], key[8];
	gcry_mpi_t genmpi, modmpi, respmpi, pubmpi, keympi;
	guchar username[256], password[64];
	guint passwordLen, usernameLen;

	GVNC_DEBUG("Do Challenge");
	priv->want_cred_password = TRUE;
	priv->want_cred_username = TRUE;
	priv->want_cred_x509 = FALSE;
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

	passwordLen = strlen(priv->cred_password);
	usernameLen = strlen(priv->cred_username);
	if (passwordLen > sizeof(password))
		passwordLen = sizeof(password);
	if (usernameLen > sizeof(username))
		usernameLen = sizeof(username);

	memset(password, 0, sizeof password);
	memset(username, 0, sizeof username);
	memcpy(password, priv->cred_password, passwordLen);
	memcpy(username, priv->cred_username, usernameLen);

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
	VncConnectionPrivate *priv = conn->priv;
	int ninteract;

	priv->want_cred_password = FALSE;
	priv->want_cred_username = FALSE;
	priv->want_cred_x509 = FALSE;

	for (ninteract = 0 ; interact[ninteract].id != 0 ; ninteract++) {
		switch (interact[ninteract].id) {
		case SASL_CB_AUTHNAME:
		case SASL_CB_USER:
			priv->want_cred_username = TRUE;
			break;

		case SASL_CB_PASS:
			priv->want_cred_password = TRUE;
			break;

		default:
			GVNC_DEBUG("Unsupported credential %lu",
				   interact[ninteract].id);
			/* Unsupported */
			return FALSE;
		}
	}

	if ((priv->want_cred_password ||
	     priv->want_cred_username) &&
	    !vnc_connection_gather_credentials(conn)) {
		GVNC_DEBUG("%s", "cannot gather sasl credentials");
		return FALSE;
	}

	for (ninteract = 0 ; interact[ninteract].id != 0 ; ninteract++) {
		switch (interact[ninteract].id) {
		case SASL_CB_AUTHNAME:
		case SASL_CB_USER:
			interact[ninteract].result = priv->cred_username;
			interact[ninteract].len = strlen(priv->cred_username);
			GVNC_DEBUG("Gather Username %s", priv->cred_username);
			break;

		case SASL_CB_PASS:
			interact[ninteract].result =  priv->cred_password;
			interact[ninteract].len = strlen(priv->cred_password);
			//GVNC_DEBUG("Gather Password %s", priv->cred_password);
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
	VncConnectionPrivate *priv = conn->priv;
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
	if (getsockname(priv->fd, (struct sockaddr*)&sa, &salen) < 0) {
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
	if (getpeername(priv->fd, (struct sockaddr*)&sa, &salen) < 0) {
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

	GVNC_DEBUG("Client SASL new host:'%s' local:'%s' remote:'%s'", priv->host, localAddr, remoteAddr);

	/* Setup a handle for being a client */
	err = sasl_client_new("vnc",
			      priv->host,
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
	if (priv->tls_session) {
		gnutls_cipher_algorithm_t cipher;

		cipher = gnutls_cipher_get(priv->tls_session);
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
	secprops.min_ssf = priv->tls_session ? 0 : 56; /* Equiv to DES supported by all Kerberos */
	secprops.max_ssf = priv->tls_session ? 0 : 100000; /* Very strong ! AES == 256 */
	secprops.maxbufsize = 100000;
	/* If we're not TLS, then forbid any anonymous or trivially crackable auth */
	secprops.security_flags = priv->tls_session ? 0 :
		SASL_SEC_NOANONYMOUS | SASL_SEC_NOPLAINTEXT;

	err = sasl_setprop(saslconn, SASL_SEC_PROPS, &secprops);
	if (err != SASL_OK) {
		GVNC_DEBUG("cannot set security props %d (%s)",
			   err, sasl_errstring(err, NULL, NULL));
		goto error;
	}

	/* Get the supported mechanisms from the server */
	mechlistlen = vnc_connection_read_u32(conn);
	if (priv->has_error)
		goto error;
	if (mechlistlen > SASL_MAX_MECHLIST_LEN) {
		GVNC_DEBUG("mechlistlen %d too long", mechlistlen);
		goto error;
	}

	mechlist = g_malloc(mechlistlen+1);
        vnc_connection_read(conn, mechlist, mechlistlen);
	mechlist[mechlistlen] = '\0';
	if (priv->has_error) {
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
	if (priv->has_error)
		goto error;


	GVNC_DEBUG("%s", "Getting sever start negotiation reply");
	/* Read the 'START' message reply from server */
	serverinlen = vnc_connection_read_u32(conn);
	if (priv->has_error)
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
	if (priv->has_error)
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
		if (priv->has_error)
			goto error;

		GVNC_DEBUG("Server step with %d bytes %p", clientoutlen, clientout);

		serverinlen = vnc_connection_read_u32(conn);
		if (priv->has_error)
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
		if (priv->has_error)
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
	if (!priv->tls_session) {
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
	priv->saslconn = saslconn;
	return ret;

 error:
	priv->has_error = TRUE;
	if (saslconn)
		sasl_dispose(&saslconn);
	return FALSE;
}
#endif /* HAVE_SASL */


static gboolean vnc_connection_start_tls(VncConnection *conn, int anonTLS)
{
	VncConnectionPrivate *priv = conn->priv;
	static const int cert_type_priority[] = { GNUTLS_CRT_X509, 0 };
	static const int protocol_priority[]= { GNUTLS_TLS1_1, GNUTLS_TLS1_0, GNUTLS_SSL3, 0 };
	static const int kx_priority[] = {GNUTLS_KX_DHE_DSS, GNUTLS_KX_RSA, GNUTLS_KX_DHE_RSA, GNUTLS_KX_SRP, 0};
	static const int kx_anon[] = {GNUTLS_KX_ANON_DH, 0};
	int ret;

	GVNC_DEBUG("Do TLS handshake");
	if (vnc_connection_tls_initialize() < 0) {
		GVNC_DEBUG("Failed to init TLS");
		priv->has_error = TRUE;
		return FALSE;
	}
	if (priv->tls_session == NULL) {
		if (gnutls_init(&priv->tls_session, GNUTLS_CLIENT) < 0) {
			priv->has_error = TRUE;
			return FALSE;
		}

		if (gnutls_set_default_priority(priv->tls_session) < 0) {
			gnutls_deinit(priv->tls_session);
			priv->has_error = TRUE;
			return FALSE;
		}

		if (gnutls_kx_set_priority(priv->tls_session, anonTLS ? kx_anon : kx_priority) < 0) {
			gnutls_deinit(priv->tls_session);
			priv->has_error = TRUE;
			return FALSE;
		}

		if (gnutls_certificate_type_set_priority(priv->tls_session, cert_type_priority) < 0) {
			gnutls_deinit(priv->tls_session);
			priv->has_error = TRUE;
			return FALSE;
		}

		if (gnutls_protocol_set_priority(priv->tls_session, protocol_priority) < 0) {
			gnutls_deinit(priv->tls_session);
			priv->has_error = TRUE;
			return FALSE;
		}

		if (anonTLS) {
			gnutls_anon_client_credentials anon_cred = vnc_connection_tls_initialize_anon_cred();
			if (!anon_cred) {
				gnutls_deinit(priv->tls_session);
				priv->has_error = TRUE;
				return FALSE;
			}
			if (gnutls_credentials_set(priv->tls_session, GNUTLS_CRD_ANON, anon_cred) < 0) {
				gnutls_deinit(priv->tls_session);
				priv->has_error = TRUE;
				return FALSE;
			}
		} else {
			priv->want_cred_password = FALSE;
			priv->want_cred_username = FALSE;
			priv->want_cred_x509 = TRUE;
			if (!vnc_connection_gather_credentials(conn))
				return FALSE;

			gnutls_certificate_credentials_t x509_cred = vnc_connection_tls_initialize_cert_cred(conn);
			if (!x509_cred) {
				gnutls_deinit(priv->tls_session);
				priv->has_error = TRUE;
				return FALSE;
			}
			if (gnutls_credentials_set(priv->tls_session, GNUTLS_CRD_CERTIFICATE, x509_cred) < 0) {
				gnutls_deinit(priv->tls_session);
				priv->has_error = TRUE;
				return FALSE;
			}
		}

		gnutls_transport_set_ptr(priv->tls_session, (gnutls_transport_ptr_t)conn);
		gnutls_transport_set_push_function(priv->tls_session, vnc_connection_tls_push);
		gnutls_transport_set_pull_function(priv->tls_session, vnc_connection_tls_pull);
	}

 retry:
	if ((ret = gnutls_handshake(priv->tls_session)) < 0) {
		if (!gnutls_error_is_fatal(ret)) {
			GVNC_DEBUG("Handshake was blocking");
			if (!gnutls_record_get_direction(priv->tls_session))
				g_io_wait(priv->channel, G_IO_IN);
			else
				g_io_wait(priv->channel, G_IO_OUT);
			goto retry;
		}
		GVNC_DEBUG("Handshake failed %s", gnutls_strerror(ret));
		gnutls_deinit(priv->tls_session);
		priv->tls_session = NULL;
		priv->has_error = TRUE;
		return FALSE;
	}

	GVNC_DEBUG("Handshake done");

	if (anonTLS) {
		return TRUE;
	} else {
		if (!vnc_connection_validate_certificate(conn)) {
			GVNC_DEBUG("Certificate validation failed");
			priv->has_error = TRUE;
			return FALSE;
		}
		return TRUE;
	}
}

static gboolean vnc_connection_has_auth_subtype(gpointer data)
{
	VncConnection *conn = data;
	VncConnectionPrivate *priv = conn->priv;

	if (priv->has_error)
		return TRUE;
	if (priv->auth_subtype == GVNC_AUTH_INVALID)
		return FALSE;
	return TRUE;
}

static void vnc_connection_choose_auth(VncConnection *conn,
				       int signum,
				       unsigned int ntypes,
				       unsigned int *types)
{
	VncConnectionPrivate *priv = conn->priv;
	struct signal_data sigdata;
	GValueArray *authTypes;
	GValue authType;

	authTypes = g_value_array_new(0);

	for (int i = 0 ; i < ntypes ; i++) {
		memset(&authType, 0, sizeof(authType));

		if (signum == VNC_AUTH_CHOOSE_TYPE) {
			g_value_init(&authType, VNC_TYPE_CONNECTION_AUTH);
		} else {
			if (priv->auth_type == GVNC_AUTH_VENCRYPT)
				g_value_init(&authType, VNC_TYPE_CONNECTION_AUTH_VENCRYPT);
			else
				g_value_init(&authType, VNC_TYPE_CONNECTION_AUTH);
		}
		g_value_set_enum(&authType, types[i]);
		authTypes = g_value_array_append(authTypes, &authType);
	}

	sigdata.params.authCred = authTypes;
	vnc_connection_emit_main_context(conn, signum, &sigdata);
	g_value_array_free(authTypes);
}

static gboolean vnc_connection_perform_auth_tls(VncConnection *conn)
{
	VncConnectionPrivate *priv = conn->priv;
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
		priv->has_error = TRUE;
		return FALSE;
	}
	for (i = 0 ; i < nauth ; i++) {
		auth[i] = vnc_connection_read_u8(conn);
	}

	for (i = 0 ; i < nauth ; i++) {
		GVNC_DEBUG("Possible sub-auth %d", auth[i]);
	}

	if (priv->has_error)
		return FALSE;
	vnc_connection_choose_auth(conn, VNC_AUTH_CHOOSE_TYPE, nauth, auth);
	if (priv->has_error)
		return FALSE;

	GVNC_DEBUG("Waiting for auth subtype");
	g_condition_wait(vnc_connection_has_auth_subtype, conn);
	if (priv->has_error)
		return FALSE;

	GVNC_DEBUG("Choose auth %d", priv->auth_subtype);

	vnc_connection_write_u8(conn, priv->auth_subtype);
	vnc_connection_flush(conn);

	switch (priv->auth_subtype) {
	case GVNC_AUTH_NONE:
		if (priv->minor == 8)
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
	VncConnectionPrivate *priv = conn->priv;
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

	if (priv->has_error)
		return FALSE;
	vnc_connection_choose_auth(conn, VNC_AUTH_CHOOSE_SUBTYPE, nauth, auth);
	if (priv->has_error)
		return FALSE;

	GVNC_DEBUG("Waiting for auth subtype");
	g_condition_wait(vnc_connection_has_auth_subtype, conn);
	if (priv->has_error)
		return FALSE;

	GVNC_DEBUG("Choose auth %d", priv->auth_subtype);

	if (!vnc_connection_gather_credentials(conn))
		return FALSE;

#if !DEBUG
	if (priv->auth_subtype == GVNC_AUTH_VENCRYPT_PLAIN) {
		GVNC_DEBUG("Cowardly refusing to transmit plain text password");
		return FALSE;
	}
#endif

	vnc_connection_write_u32(conn, priv->auth_subtype);
	vnc_connection_flush(conn);
	status = vnc_connection_read_u8(conn);
	if (status != 1) {
		GVNC_DEBUG("Server refused VeNCrypt auth %d %d", priv->auth_subtype, status);
		return FALSE;
	}

	switch (priv->auth_subtype) {
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
	GVNC_DEBUG("Completed TLS setup, do subauth %d", priv->auth_subtype);

	switch (priv->auth_subtype) {
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
		GVNC_DEBUG("Unknown auth subtype %d", priv->auth_subtype);
		return FALSE;
	}
}

static gboolean vnc_connection_has_auth_type(gpointer data)
{
	VncConnection *conn = data;
	VncConnectionPrivate *priv = conn->priv;

	if (priv->has_error)
		return TRUE;
	if (priv->auth_type == GVNC_AUTH_INVALID)
		return FALSE;
	return TRUE;
}

static gboolean vnc_connection_perform_auth(VncConnection *conn)
{
	VncConnectionPrivate *priv = conn->priv;
	unsigned int nauth, i;
	unsigned int auth[10];

	if (priv->minor <= 6) {
		nauth = 1;
		auth[0] = vnc_connection_read_u32(conn);
	} else {
		nauth = vnc_connection_read_u8(conn);
		if (vnc_connection_has_error(conn))
			return FALSE;

		if (nauth == 0)
			return vnc_connection_check_auth_result(conn);

		if (nauth > sizeof(auth)) {
			priv->has_error = TRUE;
			return FALSE;
		}
		for (i = 0 ; i < nauth ; i++)
			auth[i] = vnc_connection_read_u8(conn);
	}

	for (i = 0 ; i < nauth ; i++) {
		GVNC_DEBUG("Possible auth %u", auth[i]);
	}

	if (priv->has_error)
		return FALSE;
	vnc_connection_choose_auth(conn, VNC_AUTH_CHOOSE_TYPE, nauth, auth);
	if (priv->has_error)
		return FALSE;

	GVNC_DEBUG("Waiting for auth type");
	g_condition_wait(vnc_connection_has_auth_type, conn);
	if (priv->has_error)
		return FALSE;

	GVNC_DEBUG("Choose auth %u", priv->auth_type);
	if (!vnc_connection_gather_credentials(conn))
		return FALSE;

	if (priv->minor > 6) {
		vnc_connection_write_u8(conn, priv->auth_type);
		vnc_connection_flush(conn);
	}

	switch (priv->auth_type) {
	case GVNC_AUTH_NONE:
		if (priv->minor == 8)
			return vnc_connection_check_auth_result(conn);
		return TRUE;
	case GVNC_AUTH_VNC:
		return vnc_connection_perform_auth_vnc(conn);

	case GVNC_AUTH_TLS:
		if (priv->minor < 7)
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
		{
			struct signal_data sigdata;
			sigdata.params.authUnsupported = priv->auth_type;
			vnc_connection_emit_main_context(conn, VNC_AUTH_UNSUPPORTED, &sigdata);
			priv->has_error = TRUE;
		}
		return FALSE;
	}

	return TRUE;
}

static void vnc_connection_finalize (GObject *object)
{
	VncConnection *conn = VNC_CONNECTION(object);
	VncConnectionPrivate *priv = conn->priv;

	if (vnc_connection_is_open(conn))
		vnc_connection_close(conn);

	if (priv->cursor)
		g_object_unref(G_OBJECT(priv->cursor));

	if (priv->fb)
		g_object_unref(G_OBJECT(priv->fb));

	G_OBJECT_CLASS(vnc_connection_parent_class)->finalize (object);
}

static void vnc_connection_class_init(VncConnectionClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS (klass);

	object_class->finalize = vnc_connection_finalize;
	object_class->get_property = vnc_connection_get_property;
	object_class->set_property = vnc_connection_set_property;

	g_object_class_install_property(object_class,
					PROP_FRAMEBUFFER,
					g_param_spec_object("framebuffer",
							    "The desktop framebuffer",
							    "The desktop framebuffer instance",
							    VNC_TYPE_FRAMEBUFFER,
							    G_PARAM_READABLE |
							    G_PARAM_WRITABLE |
							    G_PARAM_STATIC_NAME |
							    G_PARAM_STATIC_NICK |
							    G_PARAM_STATIC_BLURB));

	signals[VNC_CURSOR_CHANGED] =
		g_signal_new ("vnc-cursor-changed",
			      G_OBJECT_CLASS_TYPE (object_class),
			      G_SIGNAL_RUN_FIRST,
			      G_STRUCT_OFFSET (VncConnectionClass, vnc_cursor_changed),
			      NULL, NULL,
			      g_cclosure_marshal_VOID__OBJECT,
			      G_TYPE_NONE,
			      1,
			      VNC_TYPE_CURSOR);

	signals[VNC_POINTER_MODE_CHANGED] =
		g_signal_new ("vnc-pointer-mode-changed",
			      G_OBJECT_CLASS_TYPE (object_class),
			      G_SIGNAL_RUN_FIRST,
			      G_STRUCT_OFFSET (VncConnectionClass, vnc_pointer_mode_changed),
			      NULL, NULL,
			      g_cclosure_marshal_VOID__BOOLEAN,
			      G_TYPE_NONE,
			      1,
			      G_TYPE_BOOLEAN);

	signals[VNC_BELL] =
		g_signal_new ("vnc-bell",
			      G_OBJECT_CLASS_TYPE (object_class),
			      G_SIGNAL_RUN_FIRST,
			      G_STRUCT_OFFSET (VncConnectionClass, vnc_bell),
			      NULL, NULL,
			      g_cclosure_marshal_VOID__VOID,
			      G_TYPE_NONE,
			      0);

	signals[VNC_SERVER_CUT_TEXT] =
		g_signal_new ("vnc-server-cut-text",
			      G_OBJECT_CLASS_TYPE (object_class),
			      G_SIGNAL_RUN_FIRST,
			      G_STRUCT_OFFSET (VncConnectionClass, vnc_server_cut_text),
			      NULL, NULL,
			      g_cclosure_marshal_VOID__STRING,
			      G_TYPE_NONE,
			      1,
			      G_TYPE_STRING);

	signals[VNC_FRAMEBUFFER_UPDATE] =
		g_signal_new ("vnc-framebuffer-update",
			      G_OBJECT_CLASS_TYPE (object_class),
			      G_SIGNAL_RUN_FIRST,
			      G_STRUCT_OFFSET (VncConnectionClass, vnc_framebuffer_update),
			      NULL, NULL,
			      g_cclosure_user_marshal_VOID__INT_INT_INT_INT,
			      G_TYPE_NONE,
			      4,
			      G_TYPE_INT,
			      G_TYPE_INT,
			      G_TYPE_INT,
			      G_TYPE_INT);

	signals[VNC_DESKTOP_RESIZE] =
		g_signal_new ("vnc-desktop-resize",
			      G_OBJECT_CLASS_TYPE (object_class),
			      G_SIGNAL_RUN_FIRST,
			      G_STRUCT_OFFSET (VncConnectionClass, vnc_desktop_resize),
			      NULL, NULL,
			      g_cclosure_user_marshal_VOID__INT_INT,
			      G_TYPE_NONE,
			      2,
			      G_TYPE_INT,
			      G_TYPE_INT);

	signals[VNC_PIXEL_FORMAT_CHANGED] =
		g_signal_new ("vnc-pixel-format-changed",
			      G_OBJECT_CLASS_TYPE (object_class),
			      G_SIGNAL_RUN_FIRST,
			      G_STRUCT_OFFSET (VncConnectionClass, vnc_pixel_format_changed),
			      NULL, NULL,
			      g_cclosure_marshal_VOID__POINTER,
			      G_TYPE_NONE,
			      1,
			      G_TYPE_POINTER);

	signals[VNC_AUTH_FAILURE] =
		g_signal_new ("vnc-auth-failure",
			      G_OBJECT_CLASS_TYPE (object_class),
			      G_SIGNAL_RUN_FIRST,
			      G_STRUCT_OFFSET (VncConnectionClass, vnc_auth_failure),
			      NULL, NULL,
			      g_cclosure_marshal_VOID__STRING,
			      G_TYPE_NONE,
			      1,
			      G_TYPE_STRING);


	signals[VNC_AUTH_UNSUPPORTED] =
		g_signal_new ("vnc-auth-unsupported",
			      G_OBJECT_CLASS_TYPE (object_class),
			      G_SIGNAL_RUN_FIRST,
			      G_STRUCT_OFFSET (VncConnectionClass, vnc_auth_unsupported),
			      NULL, NULL,
			      g_cclosure_marshal_VOID__UINT,
			      G_TYPE_NONE,
			      1,
			      G_TYPE_UINT);

	signals[VNC_AUTH_CREDENTIAL] =
		g_signal_new ("vnc-auth-credential",
			      G_OBJECT_CLASS_TYPE (object_class),
			      G_SIGNAL_RUN_FIRST,
			      G_STRUCT_OFFSET (VncConnectionClass, vnc_auth_credential),
			      NULL, NULL,
			      g_cclosure_marshal_VOID__BOXED,
			      G_TYPE_NONE,
			      1,
			      G_TYPE_VALUE_ARRAY);

	signals[VNC_AUTH_CHOOSE_TYPE] =
		g_signal_new ("vnc-auth-choose-type",
			      G_OBJECT_CLASS_TYPE (object_class),
			      G_SIGNAL_RUN_FIRST,
			      G_STRUCT_OFFSET (VncConnectionClass, vnc_auth_choose_type),
			      NULL, NULL,
			      g_cclosure_marshal_VOID__BOXED,
			      G_TYPE_NONE,
			      1,
			      G_TYPE_VALUE_ARRAY);

	signals[VNC_AUTH_CHOOSE_SUBTYPE] =
		g_signal_new ("vnc-auth-choose-subtype",
			      G_OBJECT_CLASS_TYPE (object_class),
			      G_SIGNAL_RUN_FIRST,
			      G_STRUCT_OFFSET (VncConnectionClass, vnc_auth_choose_subtype),
			      NULL, NULL,
			      g_cclosure_user_marshal_VOID__UINT_BOXED,
			      G_TYPE_NONE,
			      2,
			      G_TYPE_UINT,
			      G_TYPE_VALUE_ARRAY);


	g_type_class_add_private(klass, sizeof(VncConnectionPrivate));
}


void vnc_connection_init(VncConnection *fb)
{
	VncConnectionPrivate *priv;

	priv = fb->priv = VNC_CONNECTION_GET_PRIVATE(fb);

	memset(priv, 0, sizeof(*priv));

	priv->fd = -1;
	priv->auth_type = GVNC_AUTH_INVALID;
	priv->auth_subtype = GVNC_AUTH_INVALID;
}


VncConnection *vnc_connection_new(const struct vnc_connection_ops *ops, gpointer ops_data)
{
	VncConnection *conn;
	VncConnectionPrivate *priv;

	conn = VNC_CONNECTION(g_object_new(VNC_TYPE_CONNECTION,
					   NULL));

	priv = conn->priv;

	/* XXX kill this */
	memcpy(&priv->ops, ops, sizeof(*ops));
	priv->ops_data = ops_data;

	return conn;
}

void vnc_connection_close(VncConnection *conn)
{
	VncConnectionPrivate *priv = conn->priv;
	int i;

	if (priv->tls_session) {
		gnutls_bye(priv->tls_session, GNUTLS_SHUT_RDWR);
		priv->tls_session = NULL;
	}
#if HAVE_SASL
	if (priv->saslconn)
		sasl_dispose (&priv->saslconn);
#endif

	if (priv->channel) {
		g_io_channel_unref(priv->channel);
		priv->channel = NULL;
	}
	if (priv->fd != -1) {
		close(priv->fd);
		priv->fd = -1;
	}

	if (priv->host) {
		g_free(priv->host);
		priv->host = NULL;
	}

	if (priv->port) {
		g_free(priv->port);
		priv->port = NULL;
	}

	if (priv->name) {
		g_free(priv->name);
		priv->name = NULL;
	}

	g_free (priv->xmit_buffer);

	if (priv->cred_username) {
		g_free(priv->cred_username);
		priv->cred_username = NULL;
	}
	if (priv->cred_password) {
		g_free(priv->cred_password);
		priv->cred_password = NULL;
	}

	if (priv->cred_x509_cacert) {
		g_free(priv->cred_x509_cacert);
		priv->cred_x509_cacert = NULL;
	}
	if (priv->cred_x509_cacrl) {
		g_free(priv->cred_x509_cacrl);
		priv->cred_x509_cacrl = NULL;
	}
	if (priv->cred_x509_cert) {
		g_free(priv->cred_x509_cert);
		priv->cred_x509_cert = NULL;
	}
	if (priv->cred_x509_key) {
		g_free(priv->cred_x509_key);
		priv->cred_x509_key = NULL;
	}

	for (i = 0; i < 5; i++)
		inflateEnd(&priv->streams[i]);

	priv->auth_type = GVNC_AUTH_INVALID;
	priv->auth_subtype = GVNC_AUTH_INVALID;

	priv->has_error = 0;
}

void vnc_connection_shutdown(VncConnection *conn)
{
	VncConnectionPrivate *priv = conn->priv;

	close(priv->fd);
	priv->fd = -1;
	priv->has_error = 1;
	GVNC_DEBUG("Waking up couroutine to shutdown gracefully");
	g_io_wakeup(&priv->wait);
}

gboolean vnc_connection_is_open(VncConnection *conn)
{
	VncConnectionPrivate *priv = conn->priv;

	if (!conn)
		return FALSE;

	if (priv->fd != -1)
		return TRUE;
	if (priv->host)
		return TRUE;
	return FALSE;
}


gboolean vnc_connection_is_initialized(VncConnection *conn)
{
	VncConnectionPrivate *priv = conn->priv;

	if (!vnc_connection_is_open(conn))
		return FALSE;
	if (priv->name)
		return TRUE;
	return FALSE;
}


static gboolean vnc_connection_before_version (VncConnection *conn, int major, int minor)
{
	VncConnectionPrivate *priv = conn->priv;

	return (priv->major < major) || (priv->major == major && priv->minor < minor);
}


static gboolean vnc_connection_after_version (VncConnection *conn, int major, int minor)
{
	return !vnc_connection_before_version (conn, major, minor+1);
}


gboolean vnc_connection_initialize(VncConnection *conn, gboolean shared_flag)
{
	VncConnectionPrivate *priv = conn->priv;
	int ret, i;
	char version[13];
	guint32 n_name;

	priv->absPointer = TRUE;

	vnc_connection_read(conn, version, 12);
	version[12] = 0;

 	ret = sscanf(version, "RFB %03d.%03d\n", &priv->major, &priv->minor);
	if (ret != 2) {
		GVNC_DEBUG("Error while getting server version");
		goto fail;
	}

	GVNC_DEBUG("Server version: %d.%d", priv->major, priv->minor);

	if (vnc_connection_before_version(conn, 3, 3)) {
		GVNC_DEBUG("Server version is not supported (%d.%d)", priv->major, priv->minor);
		goto fail;
	} else if (vnc_connection_before_version(conn, 3, 7)) {
		priv->minor = 3;
	} else if (vnc_connection_after_version(conn, 3, 8)) {
		priv->major = 3;
		priv->minor = 8;
	}

	snprintf(version, 12, "RFB %03d.%03d\n", priv->major, priv->minor);
	vnc_connection_write(conn, version, 12);
	vnc_connection_flush(conn);
	GVNC_DEBUG("Using version: %d.%d", priv->major, priv->minor);

	if (!vnc_connection_perform_auth(conn)) {
		GVNC_DEBUG("Auth failed");
		goto fail;
	}

	vnc_connection_write_u8(conn, shared_flag); /* shared flag */
	vnc_connection_flush(conn);
	priv->width = vnc_connection_read_u16(conn);
	priv->height = vnc_connection_read_u16(conn);

	if (vnc_connection_has_error(conn))
		return FALSE;

	vnc_connection_read_pixel_format(conn, &priv->fmt);

	n_name = vnc_connection_read_u32(conn);
	if (n_name > 4096)
		goto fail;

	priv->name = g_new(char, n_name + 1);

	vnc_connection_read(conn, priv->name, n_name);
	priv->name[n_name] = 0;
	GVNC_DEBUG("Display name '%s'", priv->name);

	if (vnc_connection_has_error(conn))
		return FALSE;

	memset(&priv->strm, 0, sizeof(priv->strm));
	/* FIXME what level? */
	for (i = 0; i < 5; i++)
		inflateInit(&priv->streams[i]);
	priv->strm = NULL;

	return !vnc_connection_has_error(conn);

 fail:
	priv->has_error = 1;
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
	VncConnectionPrivate *priv = conn->priv;

	if (vnc_connection_is_open(conn)) {
		GVNC_DEBUG ("Error: already connected?");
		return FALSE;
	}

	GVNC_DEBUG("Connecting to FD %d", fd);

	if (!vnc_connection_set_nonblock(fd))
		return FALSE;

	if (!(priv->channel =
#ifdef WIN32
	      g_io_channel_win32_new_socket(_get_osfhandle(fd))
#else
	      g_io_channel_unix_new(fd)
#endif
	      )) {
		GVNC_DEBUG ("Failed to g_io_channel_unix_new()");
		return FALSE;
	}
	priv->fd = fd;

	return !vnc_connection_has_error(conn);
}

gboolean vnc_connection_open_host(VncConnection *conn, const char *host, const char *port)
{
	VncConnectionPrivate *priv = conn->priv;
        struct addrinfo *ai, *runp, hints;
        int ret;
	if (vnc_connection_is_open(conn))
		return FALSE;

	priv->host = g_strdup(host);
	priv->port = g_strdup(port);

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
                        priv->channel = chan;
                        priv->fd = fd;
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
	VncConnectionPrivate *priv = conn->priv;

        GVNC_DEBUG("Thinking about auth type %u", type);
        if (priv->auth_type != GVNC_AUTH_INVALID) {
                priv->has_error = TRUE;
                return !vnc_connection_has_error(conn);
        }
        if (type != GVNC_AUTH_NONE &&
            type != GVNC_AUTH_VNC &&
            type != GVNC_AUTH_MSLOGON &&
            type != GVNC_AUTH_TLS &&
            type != GVNC_AUTH_VENCRYPT &&
            type != GVNC_AUTH_SASL) {
		struct signal_data sigdata;
		GVNC_DEBUG("Unsupported auth type %u", type);
		sigdata.params.authUnsupported = type;
		vnc_connection_emit_main_context(conn, VNC_AUTH_UNSUPPORTED, &sigdata);
                priv->has_error = TRUE;
                return !vnc_connection_has_error(conn);
        }
        GVNC_DEBUG("Decided on auth type %u", type);
        priv->auth_type = type;
        priv->auth_subtype = GVNC_AUTH_INVALID;

	return !vnc_connection_has_error(conn);
}

gboolean vnc_connection_set_auth_subtype(VncConnection *conn, unsigned int type)
{
	VncConnectionPrivate *priv = conn->priv;

        GVNC_DEBUG("Requested auth subtype %d", type);
        if (priv->auth_type != GVNC_AUTH_VENCRYPT &&
	    priv->auth_type != GVNC_AUTH_TLS) {
                priv->has_error = TRUE;
		return !vnc_connection_has_error(conn);
        }
        if (priv->auth_subtype != GVNC_AUTH_INVALID) {
                priv->has_error = TRUE;
		return !vnc_connection_has_error(conn);
        }
        priv->auth_subtype = type;

	return !vnc_connection_has_error(conn);
}


static int vnc_connection_best_path(char **buf,
				    const char *basedir,
				    const char *basefile,
				    char **dirs,
				    unsigned int ndirs)
{
	unsigned int i;
	gchar *tmp;
	for (i = 0 ; i < ndirs ; i++) {
		struct stat sb;
		tmp = g_strdup_printf("%s/%s/%s", dirs[i], basedir, basefile);
		if (stat(tmp, &sb) == 0) {
			*buf = tmp;
			return 0;
		}
		g_free(tmp);
	}
	return -1;
}



static gboolean vnc_connection_set_credential_x509(VncConnection *conn,
						   const gchar *name)
{
	VncConnectionPrivate *priv = conn->priv;
	char *sysdir = g_strdup_printf("%s/pki", SYSCONFDIR);
#ifndef WIN32
	struct passwd *pw;

	if (!(pw = getpwuid(getuid())))
		return TRUE;

	char *userdir = g_strdup_printf("%s/.pki", pw->pw_dir);
	char *dirs[] = { sysdir, userdir };
#else
	char *dirs[] = { sysdir };
#endif

	if (vnc_connection_best_path(&priv->cred_x509_cacert, "CA", "cacert.pem",
				     dirs, sizeof(dirs)/sizeof(dirs[0])) < 0)
		return FALSE;

	/* Don't mind failures of CRL */
	vnc_connection_best_path(&priv->cred_x509_cacrl, "CA", "cacrl.pem",
				 dirs, sizeof(dirs)/sizeof(dirs[0]));

	/* Set client key & cert if we have them. Server will reject auth
	 * if it decides it requires them*/
	vnc_connection_best_path(&priv->cred_x509_key, name, "private/clientkey.pem",
				 dirs, sizeof(dirs)/sizeof(dirs[0]));
	vnc_connection_best_path(&priv->cred_x509_cert, name, "clientcert.pem",
				 dirs, sizeof(dirs)/sizeof(dirs[0]));

	return TRUE;
}

gboolean vnc_connection_set_credential(VncConnection *conn, int type, const gchar *data)
{
	VncConnectionPrivate *priv = conn->priv;

        GVNC_DEBUG("Set credential %d %s", type, data);
	switch (type) {
	case VNC_CONNECTION_CREDENTIAL_PASSWORD:
		g_free(priv->cred_password);
		priv->cred_password = g_strdup(data);
		break;

	case VNC_CONNECTION_CREDENTIAL_USERNAME:
		g_free(priv->cred_username);
		priv->cred_username = g_strdup(data);
		break;

	case VNC_CONNECTION_CREDENTIAL_CLIENTNAME:
                g_free(priv->cred_x509_cacert);
		g_free(priv->cred_x509_cacrl);
		g_free(priv->cred_x509_key);
                g_free(priv->cred_x509_cert);
		return vnc_connection_set_credential_x509(conn, data);

	default:
		priv->has_error = TRUE;
	}

	return !vnc_connection_has_error(conn);
}


gboolean vnc_connection_set_framebuffer(VncConnection *conn, VncFramebuffer *fb)
{
	VncConnectionPrivate *priv = conn->priv;
	const VncPixelFormat *remote;
	int i;

	GVNC_DEBUG("Set framebuffer %p", fb);

	if (priv->fb)
		g_object_unref(G_OBJECT(priv->fb));
	priv->fb = fb;
	g_object_ref(G_OBJECT(priv->fb));

	remote = vnc_framebuffer_get_remote_format(priv->fb);

	priv->fbSwapRemote = remote->byte_order != G_BYTE_ORDER;

        i = priv->fmt.bits_per_pixel / 8;

        if (i == 4) i = 3;

	priv->rich_cursor_blt = vnc_connection_rich_cursor_blt_table[i - 1];
	priv->tight_compute_predicted = vnc_connection_tight_compute_predicted_table[i - 1];
	priv->tight_sum_pixel = vnc_connection_tight_sum_pixel_table[i - 1];

	return !vnc_connection_has_error(conn);
}

const char *vnc_connection_get_name(VncConnection *conn)
{
	VncConnectionPrivate *priv = conn->priv;

	return priv->name;
}

int vnc_connection_get_width(VncConnection *conn)
{
	VncConnectionPrivate *priv = conn->priv;

	return priv->width;
}

int vnc_connection_get_height(VncConnection *conn)
{
	VncConnectionPrivate *priv = conn->priv;

	return priv->height;
}

gboolean vnc_connection_using_raw_keycodes(VncConnection *conn)
{
	VncConnectionPrivate *priv = conn->priv;

	return priv->has_ext_key_event;
}

VncCursor *vnc_connection_get_cursor(VncConnection *conn)
{
	VncConnectionPrivate *priv = conn->priv;

	return priv->cursor;
}


gboolean vnc_connection_abs_pointer(VncConnection *conn)
{
	VncConnectionPrivate *priv = conn->priv;

	return priv->absPointer;
}

/*
 * Local variables:
 *  c-indent-level: 8
 *  c-basic-offset: 8
 *  tab-width: 8
 * End:
 */
