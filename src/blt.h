/* Ordering of the SPLICE calls here is important to avoid
 * a Solaris compiler/cpp  whitespace bug
 */
#define src_pixel_t SPLICE(SPLICE(uint, SRC), _t)
#define ssrc_pixel_t SPLICE(SPLICE(int, SRC), _t)
#define dst_pixel_t SPLICE(SPLICE(uint, DST), _t)
#define SUFFIX() SPLICE(SRC,SPLICE(x,DST))
#define SET_PIXEL SPLICE(gvnc_set_pixel_, SUFFIX())
#define SET_PIXEL_AT SPLICE(gvnc_set_pixel_at_, SUFFIX())
#define BLIT SPLICE(gvnc_blt_, SUFFIX())
#define FILL SPLICE(gvnc_fill_, SUFFIX())
#define FAST_FILL SPLICE(gvnc_fill_fast_, SUFFIX())
#define HEXTILE SPLICE(gvnc_hextile_, SUFFIX())
#define RRE SPLICE(gvnc_rre_, SUFFIX())
#define RICH_CURSOR_BLIT SPLICE(gvnc_rich_cursor_blt_, SUFFIX())
#define RGB24_BLIT SPLICE(gvnc_rgb24_blt_, SUFFIX())
#define TIGHT_COMPUTE_PREDICTED SPLICE(gvnc_tight_compute_predicted_, SUFFIX())
#define TIGHT_SUM_PIXEL SPLICE(gvnc_tight_sum_pixel_, SUFFIX())
#define SWAP(gvnc, pixel) SPLICE(gvnc_swap_, SRC)(gvnc, pixel)
#define COMPONENT(color, pixel) ((SWAP(gvnc, pixel) >> gvnc->fmt.SPLICE(color, _shift) & gvnc->fmt.SPLICE(color, _max)))

static void FAST_FILL(struct gvnc *gvnc, src_pixel_t *sp,
		      int x, int y, int width, int height)
{
	uint8_t *dst = gvnc_get_local(gvnc, x, y);
	int i;

	for (i = 0; i < 1; i++) {
		int j;
		dst_pixel_t *dp = (dst_pixel_t *)dst;

		for (j = 0; j < width; j++) {
			*dp = *sp;
			dp++;
		}
		dst += gvnc->local.linesize;
	}
	for (i = 1; i < height; i++) {
		memcpy(dst, dst - gvnc->local.linesize, width * sizeof(*sp));
		dst += gvnc->local.linesize;
	}
}

static void SET_PIXEL(struct gvnc *gvnc, dst_pixel_t *dp, src_pixel_t *sp)
{
	*dp = ((*sp >> gvnc->rrs) & gvnc->rm) << gvnc->rls
		| ((*sp >> gvnc->grs) & gvnc->gm) << gvnc->gls
		| ((*sp >> gvnc->brs) & gvnc->bm) << gvnc->bls;
}

static void SET_PIXEL_AT(struct gvnc *gvnc, int x, int y, src_pixel_t *sp)
{
	dst_pixel_t *dp = (dst_pixel_t *)gvnc_get_local(gvnc, x, y);

	SET_PIXEL(gvnc, dp, sp);
}

static void FILL(struct gvnc *gvnc, src_pixel_t *sp,
		 int x, int y, int width, int height)
{
	uint8_t *dst = gvnc_get_local(gvnc, x, y);
	int i;

	for (i = 0; i < 1; i++) {
		dst_pixel_t *dp = (dst_pixel_t *)dst;
		int j;

		for (j = 0; j < width; j++) {
			SET_PIXEL(gvnc, dp, sp);
			dp++;
		}
		dst += gvnc->local.linesize;
	}
	for (i = 1; i < height; i++) {
		memcpy(dst, dst - gvnc->local.linesize, width * sizeof(dst_pixel_t));
		dst += gvnc->local.linesize;
	}
}

static void BLIT(struct gvnc *gvnc, uint8_t *src, int pitch, int x, int y, int w, int h)
{
	uint8_t *dst = gvnc_get_local(gvnc, x, y);
	int i;

	for (i = 0; i < h; i++) {
		dst_pixel_t *dp = (dst_pixel_t *)dst;
		src_pixel_t *sp = (src_pixel_t *)src;
		int j;

		for (j = 0; j < w; j++) {
			SET_PIXEL(gvnc, dp, sp);
			dp++;
			sp++;
		}
		dst += gvnc->local.linesize;
		src += pitch;
	}
}

static void HEXTILE(struct gvnc *gvnc, uint8_t flags, uint16_t x, uint16_t y,
		    uint16_t width, uint16_t height, src_pixel_t *fg, src_pixel_t *bg)
{
	int stride = width * sizeof(src_pixel_t);
	int i;

	if (flags & 0x01) {
		/* Raw tile */
		if (gvnc->perfect_match) {
			uint8_t *dst = gvnc_get_local(gvnc, x, y);

			for (i = 0; i < height; i++) {
				gvnc_read(gvnc, dst, stride);
				dst += gvnc->local.linesize;
			}
		} else {
			uint8_t data[16 * 16 * sizeof(src_pixel_t)];

			gvnc_read(gvnc, data, stride * height);
			BLIT(gvnc, data, stride, x, y, width, height);
		}
	} else {
		/* Background Specified */
		if (flags & 0x02)
			gvnc_read(gvnc, bg, sizeof(*bg));

		/* Foreground Specified */
		if (flags & 0x04)
			gvnc_read(gvnc, fg, sizeof(*fg));

		if (gvnc->perfect_match)
			FAST_FILL(gvnc, bg, x, y, width, height);
		else
			FILL(gvnc, bg, x, y, width, height);
			

		/* AnySubrects */
		if (flags & 0x08) {
			uint8_t n_rects = gvnc_read_u8(gvnc);

			for (i = 0; i < n_rects; i++) {
				uint8_t xy, wh;

				/* SubrectsColored */
				if (flags & 0x10)
					gvnc_read(gvnc, fg, sizeof(*fg));

				xy = gvnc_read_u8(gvnc);
				wh = gvnc_read_u8(gvnc);


				if (gvnc->perfect_match)
					FAST_FILL(gvnc, fg,
						  x + nibhi(xy), y + niblo(xy),
						  nibhi(wh) + 1, niblo(wh) + 1);
				else
					FILL(gvnc, fg,
					     x + nibhi(xy), y + niblo(xy),
					     nibhi(wh) + 1, niblo(wh) + 1);
			}
		}
	}
}

/* We need to convert to a GdkPixbuf which is always 32-bit */
#if DST == 32
static void RICH_CURSOR_BLIT(struct gvnc *gvnc, uint8_t *pixbuf,
			     uint8_t *image, uint8_t *mask, int pitch,
			     uint16_t width, uint16_t height)
{
	int x1, y1;
	uint32_t *dst = (uint32_t *)pixbuf;
	uint8_t *src = image;
	uint8_t *alpha = mask;
	int rs, gs, bs;

	rs = 24 - ((sizeof(src_pixel_t) * 8) - gvnc->fmt.red_shift);
	gs = 16 - (gvnc->fmt.red_shift - gvnc->fmt.green_shift);
	bs = 8 - (gvnc->fmt.green_shift - gvnc->fmt.blue_shift);

	for (y1 = 0; y1 < height; y1++) {
		src_pixel_t *sp = (src_pixel_t *)src;
		uint8_t *mp = alpha;
		for (x1 = 0; x1 < width; x1++) {
			*dst = (COMPONENT(red, *sp) << rs)
				| (COMPONENT(green, *sp) << gs)
				| (COMPONENT(blue, *sp) << bs);

			if ((mp[x1 / 8] >> (7 - (x1 % 8))) & 1)
				*dst |= 0xFF000000;

			dst++;
			sp++;
		}
		src += pitch;
		alpha += ((width + 7) / 8);
	}
}
#endif

#if SRC == 32
static void RGB24_BLIT(struct gvnc *gvnc, int x, int y, int width, int height,
		       uint8_t *data, int pitch)
{
	uint8_t *dst = gvnc_get_local(gvnc, x, y);
	int i, j;

	for (j = 0; j < height; j++) {
		dst_pixel_t *dp = (dst_pixel_t *)dst;
		uint8_t *sp = data;

		for (i = 0; i < width; i++) {
			*dp = (((sp[0] * gvnc->fmt.red_max) / 255) << gvnc->fmt.red_shift) |
				(((sp[1] * gvnc->fmt.green_max) / 255) << gvnc->fmt.green_shift) |
				(((sp[2] * gvnc->fmt.blue_max) / 255) << gvnc->fmt.blue_shift);
			dp++;
			sp += 3;
		}

		dst += gvnc->local.linesize;
		data += pitch;
	}
}
#endif

#if SRC == DST

static void TIGHT_COMPUTE_PREDICTED(struct gvnc *gvnc, src_pixel_t *ppixel,
				    src_pixel_t *lp, src_pixel_t *cp,
				    src_pixel_t *llp)
{
	ssrc_pixel_t red, green, blue;

	red = COMPONENT(red, *lp) + COMPONENT(red, *cp) - COMPONENT(red, *llp);
	red = MAX(red, 0);
	red = MIN(red, gvnc->fmt.red_max);

	green = COMPONENT(green, *lp) + COMPONENT(green, *cp) - COMPONENT(green, *llp);
	green = MAX(green, 0);
	green = MIN(green, gvnc->fmt.green_max);

	blue = COMPONENT(blue, *lp) + COMPONENT(blue, *cp) - COMPONENT(blue, *llp);
	blue = MAX(blue, 0);
	blue = MIN(blue, gvnc->fmt.blue_max);

	*ppixel = SWAP(gvnc,
		       (red << gvnc->fmt.red_shift) |
		       (green << gvnc->fmt.green_shift) |
		       (blue << gvnc->fmt.blue_shift));
}

static void TIGHT_SUM_PIXEL(struct gvnc *gvnc,
			    src_pixel_t *lhs, src_pixel_t *rhs)
{
	src_pixel_t red, green, blue;

	red = COMPONENT(red, *lhs) + COMPONENT(red, *rhs);
	green = COMPONENT(green, *lhs) + COMPONENT(green, *rhs);
	blue = COMPONENT(blue, *lhs) + COMPONENT(blue, *rhs);

	*lhs = SWAP(gvnc,
		    ((red & gvnc->fmt.red_max) << gvnc->fmt.red_shift) |
		    ((green & gvnc->fmt.green_max) << gvnc->fmt.green_shift) |
		    ((blue & gvnc->fmt.blue_max) << gvnc->fmt.blue_shift));
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
