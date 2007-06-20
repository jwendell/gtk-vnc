#ifndef _GVNC_H_
#define _GVNC_H_

#include <glib.h>
#include <stdint.h>

struct gvnc;

struct vnc_ops
{
	gboolean (*update)(void *, int, int, int, int);
	gboolean (*set_color_map_entry)(void *, int, int, int, int);
	gboolean (*bell)(void *);
	gboolean (*server_cut_text)(void *, const void *, size_t);
	gboolean (*resize)(void *, int, int);
	gboolean (*pointer_type_change)(void *, int);
	gboolean (*shared_memory_rmid)(void *, int);
	void *user;
};

struct vnc_pixel_format
{
	uint8_t bits_per_pixel;
	uint8_t depth;
	uint8_t big_endian_flag;
	uint8_t true_color_flag;
	uint16_t red_max;
	uint16_t green_max;
	uint16_t blue_max;
	uint8_t red_shift;
	uint8_t green_shift;
	uint8_t blue_shift;
};

struct framebuffer
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

struct gvnc *gvnc_connect(GIOChannel *channel, gboolean shared_flag, const char *password);

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
			       const struct vnc_pixel_format *fmt);

gboolean gvnc_set_shared_buffer(struct gvnc *gvnc, int line_size, int shmid);

gboolean gvnc_has_error(struct gvnc *gvnc);

gboolean gvnc_set_local(struct gvnc *gvnc, struct framebuffer *fb);

gboolean gvnc_set_vnc_ops(struct gvnc *gvnc, struct vnc_ops *ops);

gboolean gvnc_shared_memory_enabled(struct gvnc *gvnc);

const char *gvnc_get_name(struct gvnc *gvnc);
int gvnc_get_width(struct gvnc *gvnc);
int gvnc_get_height(struct gvnc *gvnc);

#endif
