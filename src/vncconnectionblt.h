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
#define SPLICE_I(a, b) a ## b
#define SPLICE(a, b) SPLICE_I(a, b)
#define src_pixel_t SPLICE(SPLICE(uint, SRC), _t)
#define ssrc_pixel_t SPLICE(SPLICE(int, SRC), _t)
#define dst_pixel_t SPLICE(SPLICE(uint, DST), _t)
#define SUFFIX() SPLICE(SRC,SPLICE(x,DST))
#define RICH_CURSOR_BLIT SPLICE(vnc_connection_rich_cursor_blt_, SUFFIX())
#define TIGHT_COMPUTE_PREDICTED SPLICE(vnc_connection_tight_compute_predicted_, SUFFIX())
#define TIGHT_SUM_PIXEL SPLICE(vnc_connection_tight_sum_pixel_, SUFFIX())
#define SWAP_RFB(conn, pixel) SPLICE(vnc_connection_swap_rfb_, SRC)(conn, pixel)
#define SWAP_IMG(conn, pixel) SPLICE(vnc_connection_swap_img_, DST)(conn, pixel)
#define COMPONENT(color, pixel) ((SWAP_RFB(conn, pixel) >> conn->fmt.SPLICE(color, _shift) & conn->fmt.SPLICE(color, _max)))


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

#undef SPLICE
#undef SPLICE_T
#undef SUFFIX
#undef RICH_CURSOR_BLIT
#undef TIGHT_SUM_PIXEL
#undef TIGHT_COMPUTE_PREDICTED
#undef SWAP_RFB
#undef SWAP_IMG
#undef COMPONENT
#undef dst_pixel_t
#undef src_pixel_t
#undef ssrc_pixel_t


/*
 * Local variables:
 *  c-indent-level: 8
 *  c-basic-offset: 8
 *  tab-width: 8
 * End:
 */
