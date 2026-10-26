#ifndef WPE_PIXEL_CONVERT_H
#define WPE_PIXEL_CONVERT_H

#include <glib.h>

/*
 * Conversion input is packed 8-bit BGRA with premultiplied color channels
 * (each color channel must be <= alpha). The functions accept unaligned rows
 * and do not validate that input contract. RGB output composites over white;
 * WebP output is a numeric 0xAARRGGBB word per pixel.
 */
gboolean wpe_bgra_row_is_opaque(const guint8 *source, guint width);
void wpe_bgra_to_rgba_row(const guint8 *source, guint8 *destination,
                          guint width);
void wpe_bgra_to_rgb_white_row(const guint8 *source, guint8 *destination,
                               guint width);
void wpe_bgra_to_bgr_white_row(const guint8 *source, guint8 *destination,
                               guint width);
void wpe_bgra_to_webp_argb_row(const guint8 *source, guint32 *destination,
                               guint width);

#endif
