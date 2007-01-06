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

#include "coroutine.h"
#include "d3des.h"

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

	char read_buffer[4096];
	int read_offset;
	int read_size;

	gboolean perfect_match;
	struct framebuffer local;

	int rp, gp, bp;
	int rm, gm, bm;

	gvnc_blt_func *blt;
	gvnc_hextile_func *hextile;

	struct vnc_ops ops;

	int absolute;
};

#if 0
#define GVNC_DEBUG(fmt, ...) do { fprintf(stderr, fmt, ## __VA_ARGS__); } while (0)
#else
#define GVNC_DEBUG(fmt, ...) do { } while (0)
#endif

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
			ssize_t ret;

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

void gvnc_write(struct gvnc *gvnc, const void *data, size_t len)
{
	int fd = g_io_channel_unix_get_fd(gvnc->channel);
	const char *ptr = data;
	size_t offset = 0;

	while (offset < len) {
		ssize_t ret;

		ret = write(fd, ptr + offset, len - offset);
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

		if (ret == 0) {
			gvnc->has_error = TRUE;
			return;
		}

		offset += ret;
	}
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

static void gvnc_read_pixel_format(struct gvnc *gvnc, struct vnc_pixel_format *fmt)
{
	uint8_t pad[3];

	fmt->bits_per_pixel  = gvnc_read_u8(gvnc);
	fmt->depth           = gvnc_read_u8(gvnc);
	fmt->big_endian_flag = gvnc_read_u8(gvnc);
	fmt->true_color_flag = gvnc_read_u8(gvnc);

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

	memcpy(&gvnc->fmt, fmt, sizeof(*fmt));

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

	return gvnc_has_error(gvnc);
}

gboolean gvnc_key_event(struct gvnc *gvnc, uint8_t down_flag, uint32_t key)
{
	uint8_t pad[2] = {0};

	gvnc_write_u8(gvnc, 4);
	gvnc_write_u8(gvnc, down_flag);
	gvnc_write(gvnc, pad, 2);
	gvnc_write_u32(gvnc, key);

	return gvnc_has_error(gvnc);
}

gboolean gvnc_pointer_event(struct gvnc *gvnc, uint8_t button_mask,
			    uint16_t x, uint16_t y)
{
	gvnc_write_u8(gvnc, 5);
	gvnc_write_u8(gvnc, button_mask);
	gvnc_write_u16(gvnc, x);
	gvnc_write_u16(gvnc, y);

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

struct gvnc *gvnc_connect(GIOChannel *channel, gboolean shared_flag, const char *password)
{
	int s;
	int ret;
	char version[13];
	int major, minor;
	uint32_t auth;
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

 	ret = sscanf(version, "RFB %03d.%03d\n", &major, &minor);
	if (ret != 2)
		goto error;

	gvnc_write(gvnc, "RFB 003.003\n", 12);

	auth = gvnc_read_u32(gvnc);
	switch (auth) {
	case 1:
		break;
	case 2: {
		uint8_t challenge[16];
		uint8_t key[8];
		uint32_t result;

		gvnc_read(gvnc, challenge, 16);

		memset(key, 0, 8);
		strncpy((char*)key, (char*)password, 8);

		deskey(key, EN0);
		des(challenge, challenge);
		des(challenge + 8, challenge + 8);

		gvnc_write(gvnc, challenge, 16);

		result = gvnc_read_u32(gvnc);
		if (result != 0) {
			goto error;
		}
	}	break;
	default:
		break;
	}

	gvnc_write_u8(gvnc, shared_flag); /* shared flag */

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
