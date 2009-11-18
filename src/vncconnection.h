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

#ifndef _GVNC_H_
#define _GVNC_H_

#include <glib.h>

#include "vncframebuffer.h"

typedef struct _VncConnection VncConnection;

typedef void (rgb24_render_func)(void *, int, int, int, int, guint8 *, int);

struct vnc_connection_ops
{
	gboolean (*auth_cred)(void *);
	gboolean (*auth_type)(void *, unsigned int, unsigned int *);
	gboolean (*auth_subtype)(void *, unsigned int, unsigned int *);
	gboolean (*auth_failure)(void *, const char *);
	gboolean (*update)(void *, int, int, int, int);
	gboolean (*set_color_map_entry)(void *, int, int, int, int);
	gboolean (*bell)(void *);
	gboolean (*server_cut_text)(void *, const void *, size_t);
	gboolean (*resize)(void *, int, int);
	gboolean (*pixel_format)(void *, VncPixelFormat *);
	gboolean (*pointer_type_change)(void *, int);
	gboolean (*local_cursor)(void *, int, int, int, int, guint8 *);
	gboolean (*auth_unsupported)(void *, unsigned int);
	gboolean (*render_jpeg)(void *, rgb24_render_func *render, void *,
				int, int, int, int, guint8 *, int);
	gboolean (*get_preferred_pixel_format)(void *, VncPixelFormat *);
};


typedef enum {
	GVNC_ENCODING_RAW = 0,
	GVNC_ENCODING_COPY_RECT = 1,
	GVNC_ENCODING_RRE = 2,
	GVNC_ENCODING_CORRE = 4,
	GVNC_ENCODING_HEXTILE = 5,
	GVNC_ENCODING_TIGHT = 7,
	GVNC_ENCODING_ZRLE = 16,

	/* Tight JPEG quality levels */
	GVNC_ENCODING_TIGHT_JPEG0 = -32,
	GVNC_ENCODING_TIGHT_JPEG1 = -31,
	GVNC_ENCODING_TIGHT_JPEG2 = -30,
	GVNC_ENCODING_TIGHT_JPEG3 = -29,
	GVNC_ENCODING_TIGHT_JPEG4 = -28,
	GVNC_ENCODING_TIGHT_JPEG5 = -27,
	GVNC_ENCODING_TIGHT_JPEG6 = -26,
	GVNC_ENCODING_TIGHT_JPEG7 = -25,
	GVNC_ENCODING_TIGHT_JPEG8 = -24,
	GVNC_ENCODING_TIGHT_JPEG9 = -23,

	/* Pseudo encodings */
	GVNC_ENCODING_DESKTOP_RESIZE = -223,
        GVNC_ENCODING_WMVi = 0x574D5669,

	GVNC_ENCODING_CURSOR_POS = -232,
	GVNC_ENCODING_RICH_CURSOR = -239,
	GVNC_ENCODING_XCURSOR = -240,

	GVNC_ENCODING_POINTER_CHANGE = -257,
	GVNC_ENCODING_EXT_KEY_EVENT = -258,
} VncConnectionEncoding;

typedef enum {
	GVNC_AUTH_INVALID = 0,
	GVNC_AUTH_NONE = 1,
	GVNC_AUTH_VNC = 2,
	GVNC_AUTH_RA2 = 5,
	GVNC_AUTH_RA2NE = 6,
	GVNC_AUTH_TIGHT = 16,
	GVNC_AUTH_ULTRA = 17,
	GVNC_AUTH_TLS = 18,  /* Used by VINO */
	GVNC_AUTH_VENCRYPT = 19, /* Used by VeNCrypt and QEMU */
 	GVNC_AUTH_SASL = 20, /* SASL type used by VINO and QEMU */
	GVNC_AUTH_MSLOGON = 0xfffffffa, /* Used by UltraVNC */
} VncConnectionAuth;

typedef enum {
	GVNC_AUTH_VENCRYPT_PLAIN = 256,
	GVNC_AUTH_VENCRYPT_TLSNONE = 257,
	GVNC_AUTH_VENCRYPT_TLSVNC = 258,
	GVNC_AUTH_VENCRYPT_TLSPLAIN = 259,
	GVNC_AUTH_VENCRYPT_X509NONE = 260,
	GVNC_AUTH_VENCRYPT_X509VNC = 261,
	GVNC_AUTH_VENCRYPT_X509PLAIN = 262,
	GVNC_AUTH_VENCRYPT_X509SASL = 263,
	GVNC_AUTH_VENCRYPT_TLSSASL = 264,
} VncConnectionAuthVencrypt;


VncConnection *vnc_connection_new(const struct vnc_connection_ops *ops, gpointer ops_data);
void vnc_connection_free(VncConnection *conn);

void vnc_connection_close(VncConnection *conn);
void vnc_connection_shutdown(VncConnection *conn);

gboolean vnc_connection_open_fd(VncConnection *conn, int fd);
gboolean vnc_connection_open_host(VncConnection *conn, const char *host, const char *port);
gboolean vnc_connection_is_open(VncConnection *conn);

gboolean vnc_connection_set_auth_type(VncConnection *conn, unsigned int type);
gboolean vnc_connection_set_auth_subtype(VncConnection *conn, unsigned int type);

gboolean vnc_connection_set_credential_password(VncConnection *conn, const char *password);
gboolean vnc_connection_set_credential_username(VncConnection *conn, const char *username);
gboolean vnc_connection_set_credential_x509_cacert(VncConnection *conn, const char *file);
gboolean vnc_connection_set_credential_x509_cacrl(VncConnection *conn, const char *file);
gboolean vnc_connection_set_credential_x509_key(VncConnection *conn, const char *file);
gboolean vnc_connection_set_credential_x509_cert(VncConnection *conn, const char *file);

gboolean vnc_connection_wants_credential_password(VncConnection *conn);
gboolean vnc_connection_wants_credential_username(VncConnection *conn);
gboolean vnc_connection_wants_credential_x509(VncConnection *conn);

gboolean vnc_connection_initialize(VncConnection *conn, gboolean shared_flag);
gboolean vnc_connection_is_initialized(VncConnection *conn);

gboolean vnc_connection_server_message(VncConnection *conn);

gboolean vnc_connection_client_cut_text(VncConnection *conn,
					const void *data, size_t length);

gboolean vnc_connection_pointer_event(VncConnection *conn, guint8 button_mask,
				      guint16 x, guint16 y);

gboolean vnc_connection_key_event(VncConnection *conn, guint8 down_flag,
				  guint32 key, guint16 scancode);

gboolean vnc_connection_framebuffer_update_request(VncConnection *conn,
						   guint8 incremental,
						   guint16 x, guint16 y,
						   guint16 width, guint16 height);

gboolean vnc_connection_set_encodings(VncConnection *conn, int n_encoding, gint32 *encoding);

gboolean vnc_connection_set_pixel_format(VncConnection *conn,
					 const VncPixelFormat *fmt);

const VncPixelFormat *vnc_connection_get_pixel_format(VncConnection *conn);

gboolean vnc_connection_has_error(VncConnection *conn);

gboolean vnc_connection_set_framebuffer(VncConnection *conn,
					VncFramebuffer *fb);

const char *vnc_connection_get_name(VncConnection *conn);
int vnc_connection_get_width(VncConnection *conn);
int vnc_connection_get_height(VncConnection *conn);

/* HACK this is temporary */
gboolean vnc_connection_using_raw_keycodes(VncConnection *conn);

#endif
/*
 * Local variables:
 *  c-indent-level: 8
 *  c-basic-offset: 8
 *  tab-width: 8
 * End:
 */
