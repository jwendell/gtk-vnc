#ifndef _GVNC_H_
#define _GVNC_H_

#include <glib.h>
#include <stdint.h>

struct gvnc;

struct gvnc_ops
{
	gboolean (*auth_cred)(void *);
	gboolean (*auth_type)(void *, unsigned int, unsigned int *);
	gboolean (*auth_subtype)(void *, unsigned int, unsigned int *);
	gboolean (*update)(void *, int, int, int, int);
	gboolean (*set_color_map_entry)(void *, int, int, int, int);
	gboolean (*bell)(void *);
	gboolean (*server_cut_text)(void *, const void *, size_t);
	gboolean (*resize)(void *, int, int);
	gboolean (*pointer_type_change)(void *, int);
	gboolean (*shared_memory_rmid)(void *, int);
	gboolean (*local_cursor)(void *, int, int, int, int, uint8_t *);
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

	int shm_id;

	int width;
	int height;

	int linesize;

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
	GVNC_ENCODING_ZRLE = 16,

	GVNC_ENCODING_DESKTOP_RESIZE = -223,
	GVNC_ENCODING_CURSOR_POS = -232,
	GVNC_ENCODING_RICH_CURSOR = -239,
	GVNC_ENCODING_XCURSOR = -240,

	GVNC_ENCODING_POINTER_CHANGE = -257,
	GVNC_ENCODING_SHARED_MEMORY = -258,
} gvnc_encoding;

typedef enum {
	GVNC_AUTH_INVALID = 0,
	GVNC_AUTH_NONE = 1,
	GVNC_AUTH_VNC = 2,
	GVNC_AUTH_RA2 = 5,
	GVNC_AUTH_RA2NE = 6,
	GVNC_AUTH_TIGHT = 16,
	GVNC_AUTH_ULTRA = 17,
	GVNC_AUTH_TLS = 18,
	GVNC_AUTH_VENCRYPT = 19
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

gboolean gvnc_key_event(struct gvnc *gvnc, uint8_t down_flag, uint32_t key);

gboolean gvnc_framebuffer_update_request(struct gvnc *gvnc,
					 uint8_t incremental,
					 uint16_t x, uint16_t y,
					 uint16_t width, uint16_t height);

gboolean gvnc_set_encodings(struct gvnc *gvnc, int n_encoding, int32_t *encoding);

gboolean gvnc_set_pixel_format(struct gvnc *gvnc,
			       const struct gvnc_pixel_format *fmt);

gboolean gvnc_set_shared_buffer(struct gvnc *gvnc, int line_size, int shmid);

gboolean gvnc_has_error(struct gvnc *gvnc);

gboolean gvnc_set_local(struct gvnc *gvnc, struct gvnc_framebuffer *fb);

gboolean gvnc_shared_memory_enabled(struct gvnc *gvnc);

const char *gvnc_get_name(struct gvnc *gvnc);
int gvnc_get_width(struct gvnc *gvnc);
int gvnc_get_height(struct gvnc *gvnc);

#endif
/*
 * Local variables:
 *  c-indent-level: 8
 *  c-basic-offset: 8
 *  tab-width: 8
 * End:
 */
