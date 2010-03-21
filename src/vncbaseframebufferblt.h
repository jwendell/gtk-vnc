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

/* Ordering of the SPLICE calls here is important to avoid
 * a Solaris compiler/cpp  whitespace bug
 */

/* Everything here must be #undef'd at the end of the file */

#define SPLICE_I(a, b) a ## b
#define SPLICE(a, b) SPLICE_I(a, b)
#define SUFFIX() SPLICE(SRC,SPLICE(x,DST))

#define src_pixel_t SPLICE(guint, SRC)
#define ssrc_pixel_t SPLICE(gint, SRC)
#define dst_pixel_t SPLICE(guint, DST)

#define SET_PIXEL SPLICE(vnc_base_framebuffer_set_pixel_, SUFFIX())
#define SET_PIXEL_AT SPLICE(vnc_base_framebuffer_set_pixel_at_, SUFFIX())
#define FAST_FILL SPLICE(vnc_base_framebuffer_fill_fast_, SUFFIX())
#define FILL SPLICE(vnc_base_framebuffer_fill_, SUFFIX())
#define BLT SPLICE(vnc_base_framebuffer_blt_, SUFFIX())
#define RGB24_BLT SPLICE(vnc_base_framebuffer_rgb24_blt_, SUFFIX())

#define SWAP_RFB(priv, pixel) SPLICE(vnc_base_framebuffer_swap_rfb_, SRC)(priv, pixel)
#define SWAP_IMG(priv, pixel) SPLICE(vnc_base_framebuffer_swap_img_, DST)(priv, pixel)
#define COMPONENT(color, pixel) ((SWAP_RFB(priv, pixel) >> priv->remoteFormat->SPLICE(color, _shift) & priv->remoteFormat->SPLICE(color, _max)))

static void SET_PIXEL(VncBaseFramebufferPrivate *priv,
		      dst_pixel_t *dp, src_pixel_t sp)
{
	*dp = SWAP_IMG(priv, ((sp >> priv->rrs) & priv->rm) << priv->rls
		       | ((sp >> priv->grs) & priv->gm) << priv->gls
		       | ((sp >> priv->brs) & priv->bm) << priv->bls);
}

static void SET_PIXEL_AT(VncBaseFramebufferPrivate *priv,
			 src_pixel_t *sp,
			 guint16 x, guint16 y)
{
	dst_pixel_t *dp = (dst_pixel_t *)VNC_BASE_FRAMEBUFFER_AT(priv, x, y);

	SET_PIXEL(priv, dp, SWAP_RFB(priv, *sp));
}


#if SRC == DST
static void FAST_FILL(VncBaseFramebufferPrivate *priv,
		      src_pixel_t *sp,
		      guint16 x, guint16 y,
		      guint16 width, guint16 height)
{
	guint8 *dst = VNC_BASE_FRAMEBUFFER_AT(priv, x, y);
	int i;

	for (i = 0; i < 1; i++) {
		int j;
		dst_pixel_t *dp = (dst_pixel_t *)dst;

		for (j = 0; j < width; j++) {
			*dp = *sp;
			dp++;
		}
		dst += priv->rowstride;
	}
	for (i = 1; i < height; i++) {
		memcpy(dst, dst - priv->rowstride, width * sizeof(*sp));
		dst += priv->rowstride;
	}
}
#endif


static void FILL(VncBaseFramebufferPrivate *priv,
		 src_pixel_t *sp,
		 guint16 x, guint16 y,
		 guint16 width, guint16 height)
{
	guint8 *dst = VNC_BASE_FRAMEBUFFER_AT(priv, x, y);
	int i;

	for (i = 0; i < 1; i++) {
		dst_pixel_t *dp = (dst_pixel_t *)dst;
		int j;

		for (j = 0; j < width; j++) {
			SET_PIXEL(priv, dp, SWAP_RFB(priv, *sp));
			dp++;
		}
		dst += priv->rowstride;
	}
	for (i = 1; i < height; i++) {
		memcpy(dst, dst - priv->rowstride, width * sizeof(dst_pixel_t));
		dst += priv->rowstride;
	}
}

static void BLT(VncBaseFramebufferPrivate *priv,
		guint8 *src, int rowstride,
		guint16 x, guint16 y,
		guint16 width, guint16 height)
{
	guint8 *dst = VNC_BASE_FRAMEBUFFER_AT(priv, x, y);
	int i;

	for (i = 0; i < height; i++) {
		dst_pixel_t *dp = (dst_pixel_t *)dst;
		src_pixel_t *sp = (src_pixel_t *)src;
		int j;

		for (j = 0; j < width; j++) {
			SET_PIXEL(priv, dp, SWAP_RFB(priv, *sp));
			dp++;
			sp++;
		}
		dst += priv->rowstride;
		src += rowstride;
	}
}


#if SRC == 32
static void RGB24_BLT(VncBaseFramebufferPrivate *priv,
		      guint8 *src, int rowstride,
		      guint16 x, gint16 y,
		      guint16 width, guint16 height)
{
	guint8 *dst = VNC_BASE_FRAMEBUFFER_AT(priv, x, y);
	int i, j;

	for (j = 0; j < height; j++) {
		dst_pixel_t *dp = (dst_pixel_t *)dst;
		guint8 *sp = src;

		for (i = 0; i < width; i++) {
			/*
			 * We use priv->remoteFormat->XXX_shift instead of usual priv->Xls
			 * because the source pixel component is a full 8 bits in
			 * size, and so doesn't need the adjusted shift
			 */
			*dp = (((sp[0] * priv->remoteFormat->red_max) / 255) << priv->remoteFormat->red_shift) |
				(((sp[1] * priv->remoteFormat->green_max) / 255) << priv->remoteFormat->green_shift) |
				(((sp[2] * priv->remoteFormat->blue_max) / 255) << priv->remoteFormat->blue_shift);
			dp++;
			sp += 3;
		}

		dst += priv->rowstride;
		src += rowstride;
	}
}
#endif

#undef COMPONENT
#undef SWAP_IMG
#undef SWAP_RGB

#undef RGB24_BLT
#undef BLT
#undef FILL
#undef FAST_FILL
#undef SET_PIXEL_AT
#undef SET_PIXEL

#undef dst_pixel_t
#undef ssrc_pixel_t
#undef src_pixel_t

#undef SUFFIX
#undef SPLICE
#undef SPLICE_I

/*
 * Local variables:
 *  c-indent-level: 8
 *  c-basic-offset: 8
 *  tab-width: 8
 * End:
 */
