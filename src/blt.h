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

/* Ordering of the SPLICE calls here is important to avoid
 * a Solaris compiler/cpp  whitespace bug
 */
#define src_pixel_t SPLICE(SPLICE(uint, SRC), _t)
#define ssrc_pixel_t SPLICE(SPLICE(int, SRC), _t)
#define dst_pixel_t SPLICE(SPLICE(uint, DST), _t)
#define SUFFIX() SPLICE(SRC,SPLICE(x,DST))
#define SET_PIXEL SPLICE(vnc_connection_set_pixel_, SUFFIX())
#define SET_PIXEL_AT SPLICE(vnc_connection_set_pixel_at_, SUFFIX())
#define BLIT SPLICE(vnc_connection_blt_, SUFFIX())
#define FILL SPLICE(vnc_connection_fill_, SUFFIX())
#define FAST_FILL SPLICE(vnc_connection_fill_fast_, SUFFIX())
#define HEXTILE SPLICE(vnc_connection_hextile_, SUFFIX())
#define RRE SPLICE(vnc_connection_rre_, SUFFIX())
#define RICH_CURSOR_BLIT SPLICE(vnc_connection_rich_cursor_blt_, SUFFIX())
#define RGB24_BLIT SPLICE(vnc_connection_rgb24_blt_, SUFFIX())
#define TIGHT_COMPUTE_PREDICTED SPLICE(vnc_connection_tight_compute_predicted_, SUFFIX())
#define TIGHT_SUM_PIXEL SPLICE(vnc_connection_tight_sum_pixel_, SUFFIX())
#define SWAP_RFB(conn, pixel) SPLICE(vnc_connection_swap_rfb_, SRC)(conn, pixel)
#define SWAP_IMG(conn, pixel) SPLICE(vnc_connection_swap_img_, DST)(conn, pixel)
#define COMPONENT(color, pixel) ((SWAP_RFB(conn, pixel) >> conn->fmt.SPLICE(color, _shift) & conn->fmt.SPLICE(color, _max)))

static void FAST_FILL(VncConnection *conn, src_pixel_t *sp,
		      int x, int y, int width, int height)
{
	guint8 *dst = vnc_connection_get_local(conn, x, y);
	int i;

	for (i = 0; i < 1; i++) {
		int j;
		dst_pixel_t *dp = (dst_pixel_t *)dst;

		for (j = 0; j < width; j++) {
			*dp = *sp;
			dp++;
		}
		dst += conn->local.linesize;
	}
	for (i = 1; i < height; i++) {
		memcpy(dst, dst - conn->local.linesize, width * sizeof(*sp));
		dst += conn->local.linesize;
	}
}

static void SET_PIXEL(VncConnection *conn, dst_pixel_t *dp, src_pixel_t sp)
{
	*dp = SWAP_IMG(conn, ((sp >> conn->rrs) & conn->rm) << conn->rls
		       | ((sp >> conn->grs) & conn->gm) << conn->gls
		       | ((sp >> conn->brs) & conn->bm) << conn->bls);
}

static void SET_PIXEL_AT(VncConnection *conn, int x, int y, src_pixel_t *sp)
{
	dst_pixel_t *dp = (dst_pixel_t *)vnc_connection_get_local(conn, x, y);

	SET_PIXEL(conn, dp, SWAP_RFB(conn, *sp));
}

static void FILL(VncConnection *conn, src_pixel_t *sp,
		 int x, int y, int width, int height)
{
	guint8 *dst = vnc_connection_get_local(conn, x, y);
	int i;

	for (i = 0; i < 1; i++) {
		dst_pixel_t *dp = (dst_pixel_t *)dst;
		int j;

		for (j = 0; j < width; j++) {
			SET_PIXEL(conn, dp, SWAP_RFB(conn, *sp));
			dp++;
		}
		dst += conn->local.linesize;
	}
	for (i = 1; i < height; i++) {
		memcpy(dst, dst - conn->local.linesize, width * sizeof(dst_pixel_t));
		dst += conn->local.linesize;
	}
}

static void BLIT(VncConnection *conn, guint8 *src, int pitch, int x, int y, int w, int h)
{
	guint8 *dst = vnc_connection_get_local(conn, x, y);
	int i;

	for (i = 0; i < h; i++) {
		dst_pixel_t *dp = (dst_pixel_t *)dst;
		src_pixel_t *sp = (src_pixel_t *)src;
		int j;

		for (j = 0; j < w; j++) {
			SET_PIXEL(conn, dp, SWAP_RFB(conn, *sp));
			dp++;
			sp++;
		}
		dst += conn->local.linesize;
		src += pitch;
	}
}

static void HEXTILE(VncConnection *conn, guint8 flags, guint16 x, guint16 y,
		    guint16 width, guint16 height, src_pixel_t *fg, src_pixel_t *bg)
{
	int stride = width * sizeof(src_pixel_t);
	int i;

	if (flags & 0x01) {
		/* Raw tile */
		if (conn->perfect_match) {
			guint8 *dst = vnc_connection_get_local(conn, x, y);

			for (i = 0; i < height; i++) {
				vnc_connection_read(conn, dst, stride);
				dst += conn->local.linesize;
			}
		} else {
			guint8 data[16 * 16 * sizeof(src_pixel_t)];

			vnc_connection_read(conn, data, stride * height);
			BLIT(conn, data, stride, x, y, width, height);
		}
	} else {
		/* Background Specified */
		if (flags & 0x02)
			vnc_connection_read(conn, bg, sizeof(*bg));

		/* Foreground Specified */
		if (flags & 0x04)
			vnc_connection_read(conn, fg, sizeof(*fg));

		if (conn->perfect_match)
			FAST_FILL(conn, bg, x, y, width, height);
		else
			FILL(conn, bg, x, y, width, height);


		/* AnySubrects */
		if (flags & 0x08) {
			guint8 n_rects = vnc_connection_read_u8(conn);

			for (i = 0; i < n_rects; i++) {
				guint8 xy, wh;

				/* SubrectsColored */
				if (flags & 0x10)
					vnc_connection_read(conn, fg, sizeof(*fg));

				xy = vnc_connection_read_u8(conn);
				wh = vnc_connection_read_u8(conn);


				if (conn->perfect_match)
					FAST_FILL(conn, fg,
						  x + nibhi(xy), y + niblo(xy),
						  nibhi(wh) + 1, niblo(wh) + 1);
				else
					FILL(conn, fg,
					     x + nibhi(xy), y + niblo(xy),
					     nibhi(wh) + 1, niblo(wh) + 1);
			}
		}
	}
}

/* We need to convert to a GdkPixbuf which is always 32-bit */
#if DST == 32
static void RICH_CURSOR_BLIT(VncConnection *conn, guint8 *pixbuf,
			     guint8 *image, guint8 *mask, int pitch,
			     guint16 width, guint16 height)
{
	int x1, y1;
	guint32 *dst = (guint32 *)pixbuf;
	guint8 *src = image;
	guint8 *alpha = mask;
	int as, rs, gs, bs, n;

	/*
	 * GdkPixbuf is always 32-bit RGB, so we can't use the precomputed
	 * left / right shift data from conn->{r,g,b}{r,l}s. The latter
	 * is set for the local display depth, which may be different
	 * to GdkPixbuf's fixed 32-bit RGBA
	 *
	 * This function isn't called often, so just re-compute them now
	 */

#if G_BYTE_ORDER == G_BIG_ENDIAN
	as = 0;
	rs = 8;
	gs = 16;
	bs = 24;
#else
	as = 24;
	rs = 16;
	gs = 8;
	bs = 0;
#endif

	/* Then this adjusts for remote having less bpp than 32 */
	for (n = 255 ; n > conn->fmt.red_max ; n>>= 1)
		rs++;
	for (n = 255 ; n > conn->fmt.green_max ; n>>= 1)
		gs++;
	for (n = 255 ; n > conn->fmt.blue_max ; n>>= 1)
		bs++;

	for (y1 = 0; y1 < height; y1++) {
		src_pixel_t *sp = (src_pixel_t *)src;
		guint8 *mp = alpha;
		for (x1 = 0; x1 < width; x1++) {
			*dst = (COMPONENT(red, *sp) << rs)
				| (COMPONENT(green, *sp) << gs)
				| (COMPONENT(blue, *sp) << bs);

			if ((mp[x1 / 8] >> (7 - (x1 % 8))) & 1)
				*dst |= (0xFF << as);

			dst++;
			sp++;
		}
		src += pitch;
		alpha += ((width + 7) / 8);
	}
}
#endif

#if SRC == 32
static void RGB24_BLIT(VncConnection *conn, int x, int y, int width, int height,
		       guint8 *data, int pitch)
{
	guint8 *dst = vnc_connection_get_local(conn, x, y);
	int i, j;

	for (j = 0; j < height; j++) {
		dst_pixel_t *dp = (dst_pixel_t *)dst;
		guint8 *sp = data;

		for (i = 0; i < width; i++) {
			/*
			 * We use conn->fmt.XXX_shift instead of usual conn->Xls
			 * because the source pixel component is a full 8 bits in
			 * size, and so doesn't need the adjusted shift
			 */
			*dp = (((sp[0] * conn->fmt.red_max) / 255) << conn->fmt.red_shift) |
				(((sp[1] * conn->fmt.green_max) / 255) << conn->fmt.green_shift) |
				(((sp[2] * conn->fmt.blue_max) / 255) << conn->fmt.blue_shift);
			dp++;
			sp += 3;
		}

		dst += conn->local.linesize;
		data += pitch;
	}
}
#endif

#if SRC == DST

static void TIGHT_COMPUTE_PREDICTED(VncConnection *conn, src_pixel_t *ppixel,
				    src_pixel_t *lp, src_pixel_t *cp,
				    src_pixel_t *llp)
{
	ssrc_pixel_t red, green, blue;

	red = COMPONENT(red, *lp) + COMPONENT(red, *cp) - COMPONENT(red, *llp);
	red = MAX(red, 0);
	red = MIN(red, conn->fmt.red_max);

	green = COMPONENT(green, *lp) + COMPONENT(green, *cp) - COMPONENT(green, *llp);
	green = MAX(green, 0);
	green = MIN(green, conn->fmt.green_max);

	blue = COMPONENT(blue, *lp) + COMPONENT(blue, *cp) - COMPONENT(blue, *llp);
	blue = MAX(blue, 0);
	blue = MIN(blue, conn->fmt.blue_max);

	*ppixel = SWAP_RFB(conn,
		       (red << conn->fmt.red_shift) |
		       (green << conn->fmt.green_shift) |
		       (blue << conn->fmt.blue_shift));
}

static void TIGHT_SUM_PIXEL(VncConnection *conn,
			    src_pixel_t *lhs, src_pixel_t *rhs)
{
	src_pixel_t red, green, blue;

	red = COMPONENT(red, *lhs) + COMPONENT(red, *rhs);
	green = COMPONENT(green, *lhs) + COMPONENT(green, *rhs);
	blue = COMPONENT(blue, *lhs) + COMPONENT(blue, *rhs);

	*lhs = SWAP_RFB(conn,
		    ((red & conn->fmt.red_max) << conn->fmt.red_shift) |
		    ((green & conn->fmt.green_max) << conn->fmt.green_shift) |
		    ((blue & conn->fmt.blue_max) << conn->fmt.blue_shift));
}

#endif

#undef COMPONENT
#undef HEXTILE
#undef FILL
#undef FAST_FILL
#undef BLIT
#undef dst_pixel_t
#undef src_pixel_t


/*
 * Local variables:
 *  c-indent-level: 8
 *  c-basic-offset: 8
 *  tab-width: 8
 * End:
 */
