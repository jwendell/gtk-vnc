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
#include <stdint.h>

struct gvnc;

struct gvnc_pixel_format;

typedef void (rgb24_render_func)(void *, int, int, int, int, uint8_t *, int);

struct gvnc_ops
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
        gboolean (*pixel_format)(void *, struct gvnc_pixel_format *);
	gboolean (*pointer_type_change)(void *, int);
	gboolean (*local_cursor)(void *, int, int, int, int, uint8_t *);
	gboolean (*auth_unsupported)(void *, unsigned int);
	gboolean (*render_jpeg)(void *, rgb24_render_func *render, void *,
				int, int, int, int, uint8_t *, int);
	gboolean (*get_preferred_pixel_format)(void *, struct gvnc_pixel_format *);
};

struct gvnc_pixel_format
{
	uint8_t bits_per_pixel;
	uint8_t depth;
	uint16_t byte_order;
	uint8_t true_color_flag;
	uint16_t red_max;
	uint16_t green_max;
	uint16_t blue_max;
	uint8_t red_shift;
	uint8_t green_shift;
	uint8_t blue_shift;
};

struct gvnc_framebuffer
{
	uint8_t *data;

	int width;
	int height;

	int linesize;

	uint16_t byte_order;
	int depth;
	int bpp;

	int red_mask;
	int green_mask;
	int blue_mask;

	int red_shift;
	int blue_shift;
	int green_shift;
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
} gvnc_encoding;

typedef enum {
	GVNC_AUTH_INVALID = 0,
	GVNC_AUTH_NONE = 1,
	GVNC_AUTH_VNC = 2,
	GVNC_AUTH_RA2 = 5,
	GVNC_AUTH_RA2NE = 6,
	GVNC_AUTH_TIGHT = 16,
	GVNC_AUTH_ULTRA = 17,
	GVNC_AUTH_TLS = 18,  /* Used by VINO */
	GVNC_AUTH_VENCRYPT = 19 /* Used by VeNCrypt and QEMU */
} gvnc_auth;

typedef enum {
	GVNC_AUTH_VENCRYPT_PLAIN = 256,
	GVNC_AUTH_VENCRYPT_TLSNONE = 257,
	GVNC_AUTH_VENCRYPT_TLSVNC = 258,
	GVNC_AUTH_VENCRYPT_TLSPLAIN = 259,
	GVNC_AUTH_VENCRYPT_X509NONE = 260,
	GVNC_AUTH_VENCRYPT_X509VNC = 261,
	GVNC_AUTH_VENCRYPT_X509PLAIN = 262,
} gvnc_auth_vencrypt;


struct gvnc *gvnc_new(const struct gvnc_ops *ops, gpointer ops_data);
void gvnc_free(struct gvnc *gvnc);

void gvnc_close(struct gvnc *gvnc);
void gvnc_shutdown(struct gvnc *gvnc);

gboolean gvnc_open_fd(struct gvnc *gvnc, int fd);
gboolean gvnc_open_host(struct gvnc *gvnc, const char *host, const char *port);
gboolean gvnc_is_open(struct gvnc *gvnc);

gboolean gvnc_set_auth_type(struct gvnc *gvnc, unsigned int type);
gboolean gvnc_set_auth_subtype(struct gvnc *gvnc, unsigned int type);

gboolean gvnc_set_credential_password(struct gvnc *gvnc, const char *password);
gboolean gvnc_set_credential_username(struct gvnc *gvnc, const char *username);
gboolean gvnc_set_credential_x509_cacert(struct gvnc *gvnc, const char *file);
gboolean gvnc_set_credential_x509_cacrl(struct gvnc *gvnc, const char *file);
gboolean gvnc_set_credential_x509_key(struct gvnc *gvnc, const char *file);
gboolean gvnc_set_credential_x509_cert(struct gvnc *gvnc, const char *file);

gboolean gvnc_wants_credential_password(struct gvnc *gvnc);
gboolean gvnc_wants_credential_username(struct gvnc *gvnc);
gboolean gvnc_wants_credential_x509(struct gvnc *gvnc);

gboolean gvnc_initialize(struct gvnc *gvnc, gboolean shared_flag);
gboolean gvnc_is_initialized(struct gvnc *gvnc);

gboolean gvnc_server_message(struct gvnc *gvnc);

gboolean gvnc_client_cut_text(struct gvnc *gvnc,
			      const void *data, size_t length);

gboolean gvnc_pointer_event(struct gvnc *gvnc, uint8_t button_mask,
			    uint16_t x, uint16_t y);

gboolean gvnc_key_event(struct gvnc *gvnc, uint8_t down_flag,
			uint32_t key, uint16_t scancode);

gboolean gvnc_framebuffer_update_request(struct gvnc *gvnc,
					 uint8_t incremental,
					 uint16_t x, uint16_t y,
					 uint16_t width, uint16_t height);

gboolean gvnc_set_encodings(struct gvnc *gvnc, int n_encoding, int32_t *encoding);

gboolean gvnc_set_pixel_format(struct gvnc *gvnc,
			       const struct gvnc_pixel_format *fmt);

gboolean gvnc_has_error(struct gvnc *gvnc);

gboolean gvnc_set_local(struct gvnc *gvnc, struct gvnc_framebuffer *fb);

gboolean gvnc_shared_memory_enabled(struct gvnc *gvnc);

const char *gvnc_get_name(struct gvnc *gvnc);
int gvnc_get_width(struct gvnc *gvnc);
int gvnc_get_height(struct gvnc *gvnc);

/* HACK this is temporary */
gboolean gvnc_using_raw_keycodes(struct gvnc *gvnc);

#endif
/*
 * Local variables:
 *  c-indent-level: 8
 *  c-basic-offset: 8
 *  tab-width: 8
 * End:
 */
