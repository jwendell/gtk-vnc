/*
 * GTK VNC Widget
 *
 * Copyright (C) 2006  Anthony Liguori <anthony@codemonkey.ws>
 * Copyright (C) 2009-2010 Daniel P. Berrange <dan@berrange.com>
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

#ifndef VNC_CONNECTION_H
#define VNC_CONNECTION_H

#include <glib.h>

#include "vncframebuffer.h"
#include "vnccursor.h"

G_BEGIN_DECLS

#define VNC_TYPE_CONNECTION            (vnc_connection_get_type ())
#define VNC_CONNECTION(obj)            (G_TYPE_CHECK_INSTANCE_CAST ((obj), VNC_TYPE_CONNECTION, VncConnection))
#define VNC_CONNECTION_CLASS(klass)    (G_TYPE_CHECK_CLASS_CAST ((klass), VNC_TYPE_CONNECTION, VncConnectionClass))
#define VNC_IS_CONNECTION(obj)         (G_TYPE_CHECK_INSTANCE_TYPE ((obj), VNC_TYPE_CONNECTION))
#define VNC_IS_CONNECTION_CLASS(klass) (G_TYPE_CHECK_CLASS_TYPE ((klass), VNC_TYPE_CONNECTION))
#define VNC_CONNECTION_GET_CLASS(obj)  (G_TYPE_INSTANCE_GET_CLASS ((obj), VNC_TYPE_CONNECTION, VncConnectionClass))


typedef struct _VncConnection VncConnection;
typedef struct _VncConnectionPrivate VncConnectionPrivate;
typedef struct _VncConnectionClass VncConnectionClass;

struct _VncConnection
{
	GObject parent;

	VncConnectionPrivate *priv;
};

struct _VncConnectionClass
{
	GObjectClass parent_class;

	/* Signals */
	void (*vnc_cursor_changed)(VncConnection *conn, VncCursor *cursor);
	void (*vnc_pointer_mode_changed)(VncConnection *conn, gboolean absPointer);
	void (*vnc_bell)(VncConnection *conn);
	void (*vnc_server_cut_text)(VncConnection *conn, const GString *text);
	void (*vnc_framebuffer_update)(VncConnection *conn, guint16 x, guint16 y, guint16 width, guint16 height);
	void (*vnc_desktop_resize)(VncConnection *conn, guint16 width, guint16 height);
	void (*vnc_pixel_format_changed)(VncConnection *conn, VncPixelFormat *format);
	void (*vnc_auth_failure)(VncConnection *conn, const char *reason);
	void (*vnc_auth_unsupported)(VncConnection *conn, unsigned int authType);
	void (*vnc_auth_credential)(VncConnection *conn, GValueArray *creds);
	void (*vnc_auth_choose_type)(VncConnection *conn, GValueArray *types);
	void (*vnc_auth_choose_subtype)(VncConnection *conn, unsigned int type, GValueArray *subtypes);
	void (*vnc_connected)(VncConnection *conn);
	void (*vnc_initialized)(VncConnection *conn);
	void (*vnc_disconnected)(VncConnection *conn);
};

struct vnc_connection_ops
{
	gboolean (*set_color_map_entry)(void *, int, int, int, int);
};


typedef enum {
	VNC_CONNECTION_ENCODING_RAW = 0,
	VNC_CONNECTION_ENCODING_COPY_RECT = 1,
	VNC_CONNECTION_ENCODING_RRE = 2,
	VNC_CONNECTION_ENCODING_CORRE = 4,
	VNC_CONNECTION_ENCODING_HEXTILE = 5,
	VNC_CONNECTION_ENCODING_TIGHT = 7,
	VNC_CONNECTION_ENCODING_ZRLE = 16,

	/* Tight JPEG quality levels */
	VNC_CONNECTION_ENCODING_TIGHT_JPEG0 = -32,
	VNC_CONNECTION_ENCODING_TIGHT_JPEG1 = -31,
	VNC_CONNECTION_ENCODING_TIGHT_JPEG2 = -30,
	VNC_CONNECTION_ENCODING_TIGHT_JPEG3 = -29,
	VNC_CONNECTION_ENCODING_TIGHT_JPEG4 = -28,
	VNC_CONNECTION_ENCODING_TIGHT_JPEG5 = -27,
	VNC_CONNECTION_ENCODING_TIGHT_JPEG6 = -26,
	VNC_CONNECTION_ENCODING_TIGHT_JPEG7 = -25,
	VNC_CONNECTION_ENCODING_TIGHT_JPEG8 = -24,
	VNC_CONNECTION_ENCODING_TIGHT_JPEG9 = -23,

	/* Pseudo encodings */
	VNC_CONNECTION_ENCODING_DESKTOP_RESIZE = -223,
        VNC_CONNECTION_ENCODING_WMVi = 0x574D5669,

	VNC_CONNECTION_ENCODING_CURSOR_POS = -232,
	VNC_CONNECTION_ENCODING_RICH_CURSOR = -239,
	VNC_CONNECTION_ENCODING_XCURSOR = -240,

	VNC_CONNECTION_ENCODING_POINTER_CHANGE = -257,
	VNC_CONNECTION_ENCODING_EXT_KEY_EVENT = -258,
} VncConnectionEncoding;

typedef enum {
	VNC_CONNECTION_AUTH_INVALID = 0,
	VNC_CONNECTION_AUTH_NONE = 1,
	VNC_CONNECTION_AUTH_VNC = 2,
	VNC_CONNECTION_AUTH_RA2 = 5,
	VNC_CONNECTION_AUTH_RA2NE = 6,
	VNC_CONNECTION_AUTH_TIGHT = 16,
	VNC_CONNECTION_AUTH_ULTRA = 17,
	VNC_CONNECTION_AUTH_TLS = 18,  /* Used by VINO */
	VNC_CONNECTION_AUTH_VENCRYPT = 19, /* Used by VeNCrypt and QEMU */
 	VNC_CONNECTION_AUTH_SASL = 20, /* SASL type used by VINO and QEMU */
	VNC_CONNECTION_AUTH_MSLOGON = 0xfffffffa, /* Used by UltraVNC */
} VncConnectionAuth;

typedef enum {
	VNC_CONNECTION_AUTH_VENCRYPT_PLAIN = 256,
	VNC_CONNECTION_AUTH_VENCRYPT_TLSNONE = 257,
	VNC_CONNECTION_AUTH_VENCRYPT_TLSVNC = 258,
	VNC_CONNECTION_AUTH_VENCRYPT_TLSPLAIN = 259,
	VNC_CONNECTION_AUTH_VENCRYPT_X509NONE = 260,
	VNC_CONNECTION_AUTH_VENCRYPT_X509VNC = 261,
	VNC_CONNECTION_AUTH_VENCRYPT_X509PLAIN = 262,
	VNC_CONNECTION_AUTH_VENCRYPT_X509SASL = 263,
	VNC_CONNECTION_AUTH_VENCRYPT_TLSSASL = 264,
} VncConnectionAuthVencrypt;

typedef enum
{
	VNC_CONNECTION_CREDENTIAL_PASSWORD,
	VNC_CONNECTION_CREDENTIAL_USERNAME,
	VNC_CONNECTION_CREDENTIAL_CLIENTNAME,
} VncConnectionCredential;


GType vnc_connection_get_type(void) G_GNUC_CONST;

VncConnection *vnc_connection_new(void);

gboolean vnc_connection_open_fd(VncConnection *conn, int fd);
gboolean vnc_connection_open_host(VncConnection *conn, const char *host, const char *port);
gboolean vnc_connection_is_open(VncConnection *conn);
void vnc_connection_shutdown(VncConnection *conn);

gboolean vnc_connection_set_auth_type(VncConnection *conn, unsigned int type);
gboolean vnc_connection_set_auth_subtype(VncConnection *conn, unsigned int type);
gboolean vnc_connection_set_credential(VncConnection *conn, int type, const gchar *data);

gboolean vnc_connection_is_initialized(VncConnection *conn);

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

gboolean vnc_connection_set_shared(VncConnection *conn, gboolean sharedFlag);
gboolean vnc_connection_get_shared(VncConnection *conn);

gboolean vnc_connection_has_error(VncConnection *conn);

gboolean vnc_connection_set_framebuffer(VncConnection *conn,
					VncFramebuffer *fb);

const char *vnc_connection_get_name(VncConnection *conn);
int vnc_connection_get_width(VncConnection *conn);
int vnc_connection_get_height(VncConnection *conn);

VncCursor *vnc_connection_get_cursor(VncConnection *conn);

gboolean vnc_connection_get_abs_pointer(VncConnection *conn);
gboolean vnc_connection_get_ext_key_event(VncConnection *conn);

G_END_DECLS

#endif /* VNC_CONNECTION_H */
/*
 * Local variables:
 *  c-indent-level: 8
 *  c-basic-offset: 8
 *  tab-width: 8
 * End:
 */
