#include "pixel_convert.h"

#include <stdint.h>
#include <string.h>

#if !defined(WPE_PIXEL_DISABLE_SIMD) && \
    (defined(__x86_64__) || defined(__i386__))
#include <immintrin.h>
#define WPE_HAVE_X86_AVX2 1
#elif !defined(WPE_PIXEL_DISABLE_SIMD) && \
    (defined(__aarch64__) || defined(__ARM_NEON))
#include <arm_neon.h>
#define WPE_HAVE_ARM_NEON 1
#endif

static inline void
bgra_to_rgba(const guint8 *source, guint8 *destination)
{
    guint8 alpha = source[3];
    guint red = source[2];
    guint green = source[1];
    guint blue = source[0];
    if (alpha && alpha < 255) {
        red = MIN(255u, (red * 255u + alpha / 2u) / alpha);
        green = MIN(255u, (green * 255u + alpha / 2u) / alpha);
        blue = MIN(255u, (blue * 255u + alpha / 2u) / alpha);
    } else if (!alpha) {
        red = green = blue = 0;
    }
    destination[0] = (guint8) red;
    destination[1] = (guint8) green;
    destination[2] = (guint8) blue;
    destination[3] = alpha;
}

#if defined(WPE_HAVE_X86_AVX2)
static gboolean
cpu_supports_avx2(void)
{
    static gsize detection_state;
    if (g_once_init_enter(&detection_state)) {
        gsize state = __builtin_cpu_supports("avx2") ? 1u : 2u;
        g_once_init_leave(&detection_state, state);
    }
    return detection_state == 1u;
}

__attribute__((target("avx2")))
static gboolean
bgra_row_is_opaque_avx2(const guint8 *source, guint width)
{
    const __m256i alpha_mask = _mm256_setr_epi8(
        0, 0, 0, -1, 0, 0, 0, -1, 0, 0, 0, -1, 0, 0, 0, -1,
        0, 0, 0, -1, 0, 0, 0, -1, 0, 0, 0, -1, 0, 0, 0, -1);
    __m256i alpha_bits = _mm256_set1_epi8((char) 0xff);
    guint x = 0;
    for (; width - x >= 8u; x += 8u) {
        __m256i pixels = _mm256_loadu_si256(
            (const __m256i *) (const void *) (source + (gsize) x * 4u));
        alpha_bits = _mm256_and_si256(alpha_bits, pixels);
    }
    for (; x < width; ++x) {
        if (source[(gsize) x * 4u + 3u] != 255u)
            return FALSE;
    }
    __m256i alpha_channels = _mm256_and_si256(alpha_bits, alpha_mask);
    return _mm256_movemask_epi8(
        _mm256_cmpeq_epi8(alpha_channels, alpha_mask)) == -1;
}

/* RGBA un-premultiplication still needs scalar work for mixed-alpha blocks. */
__attribute__((target("avx2")))
static void
bgra_to_rgba_row_avx2(const guint8 *source, guint8 *destination, guint width)
{
    const __m256i channel_order = _mm256_setr_epi8(
        2, 1, 0, 3, 6, 5, 4, 7, 10, 9, 8, 11, 14, 13, 12, 15,
        2, 1, 0, 3, 6, 5, 4, 7, 10, 9, 8, 11, 14, 13, 12, 15);
    const __m256i alpha_order = _mm256_setr_epi8(
        3, 3, 3, 3, 7, 7, 7, 7, 11, 11, 11, 11, 15, 15, 15, 15,
        3, 3, 3, 3, 7, 7, 7, 7, 11, 11, 11, 11, 15, 15, 15, 15);
    const __m256i opaque = _mm256_set1_epi8((char) 0xff);
    guint x = 0;

    for (; x + 8u <= width; x += 8u) {
        const guint8 *source_block = source + (gsize) x * 4u;
        guint8 *destination_block = destination + (gsize) x * 4u;
        __m256i pixels = _mm256_loadu_si256(
            (const __m256i *) (const void *) source_block);
        __m256i alpha = _mm256_shuffle_epi8(pixels, alpha_order);
        if (_mm256_movemask_epi8(_mm256_cmpeq_epi8(alpha, opaque)) == -1) {
            __m256i rgba = _mm256_shuffle_epi8(pixels, channel_order);
            _mm256_storeu_si256((__m256i *) (void *) destination_block, rgba);
        } else {
            for (guint i = 0; i < 8u; ++i)
                bgra_to_rgba(source_block + (gsize) i * 4u,
                             destination_block + (gsize) i * 4u);
        }
    }
    for (; x < width; ++x)
        bgra_to_rgba(source + (gsize) x * 4u,
                     destination + (gsize) x * 4u);
}
#endif

#if defined(WPE_HAVE_ARM_NEON)
static gboolean
bgra_row_is_opaque_neon(const guint8 *source, guint width)
{
    const uint8x16_t opaque16 = vdupq_n_u8(0xffu);
    guint x = 0;
    for (; width - x >= 16u; x += 16u) {
        uint8x16x4_t bgra = vld4q_u8(source + (gsize) x * 4u);
        uint8x16_t matches = vceqq_u8(bgra.val[3], opaque16);
        uint64x2_t halves = vreinterpretq_u64_u8(matches);
        if (vgetq_lane_u64(halves, 0) != UINT64_MAX ||
            vgetq_lane_u64(halves, 1) != UINT64_MAX)
            return FALSE;
    }
    const uint8x8_t opaque8 = vdup_n_u8(0xffu);
    for (; width - x >= 8u; x += 8u) {
        uint8x8x4_t bgra = vld4_u8(source + (gsize) x * 4u);
        uint8x8_t matches = vceq_u8(bgra.val[3], opaque8);
        if (vget_lane_u64(vreinterpret_u64_u8(matches), 0) != UINT64_MAX)
            return FALSE;
    }
    for (; x < width; ++x) {
        if (source[(gsize) x * 4u + 3u] != 255u)
            return FALSE;
    }
    return TRUE;
}

static void
bgra_to_rgba_row_neon(const guint8 *source, guint8 *destination, guint width)
{
    guint x = 0;
    const uint8x8_t opaque = vdup_n_u8(0xffu);

    for (; x + 8u <= width; x += 8u) {
        const guint8 *source_block = source + (gsize) x * 4u;
        guint8 *destination_block = destination + (gsize) x * 4u;
        uint8x8x4_t bgra = vld4_u8(source_block);
        uint8x8_t alpha_matches = vceq_u8(bgra.val[3], opaque);
        if (vget_lane_u64(vreinterpret_u64_u8(alpha_matches), 0) == UINT64_MAX) {
            uint8x8x4_t rgba;
            rgba.val[0] = bgra.val[2];
            rgba.val[1] = bgra.val[1];
            rgba.val[2] = bgra.val[0];
            rgba.val[3] = bgra.val[3];
            vst4_u8(destination_block, rgba);
        } else {
            for (guint i = 0; i < 8u; ++i)
                bgra_to_rgba(source_block + (gsize) i * 4u,
                             destination_block + (gsize) i * 4u);
        }
    }
    for (; x < width; ++x)
        bgra_to_rgba(source + (gsize) x * 4u,
                     destination + (gsize) x * 4u);
}
#endif

gboolean
wpe_bgra_row_is_opaque(const guint8 *source, guint width)
{
#if defined(WPE_HAVE_X86_AVX2)
    if (cpu_supports_avx2())
        return bgra_row_is_opaque_avx2(source, width);
#elif defined(WPE_HAVE_ARM_NEON)
    return bgra_row_is_opaque_neon(source, width);
#endif
    for (guint x = 0; x < width; ++x) {
        if (source[(gsize) x * 4u + 3u] != 255u)
            return FALSE;
    }
    return TRUE;
}

void
wpe_bgra_to_rgba_row(const guint8 *source, guint8 *destination, guint width)
{
#if defined(WPE_HAVE_X86_AVX2)
    if (cpu_supports_avx2()) {
        bgra_to_rgba_row_avx2(source, destination, width);
        return;
    }
#elif defined(WPE_HAVE_ARM_NEON)
    bgra_to_rgba_row_neon(source, destination, width);
    return;
#endif
    for (guint x = 0; x < width; ++x)
        bgra_to_rgba(source + (gsize) x * 4u,
                     destination + (gsize) x * 4u);
}

static inline guint32
bgra_to_webp_argb(const guint8 *source)
{
    guint8 rgba[4];
    bgra_to_rgba(source, rgba);
    return ((guint32) rgba[3] << 24) | ((guint32) rgba[0] << 16) |
           ((guint32) rgba[1] << 8) | (guint32) rgba[2];
}

#if defined(WPE_HAVE_X86_AVX2) && defined(__BYTE_ORDER__) && \
    (__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__)
/* On little-endian targets opaque BGRA bytes already form WebP's ARGB words. */
__attribute__((target("avx2")))
static void
bgra_to_webp_argb_row_avx2(const guint8 *source, guint32 *destination,
                          guint width)
{
    const __m256i alpha_order = _mm256_setr_epi8(
        3, 3, 3, 3, 7, 7, 7, 7, 11, 11, 11, 11, 15, 15, 15, 15,
        3, 3, 3, 3, 7, 7, 7, 7, 11, 11, 11, 11, 15, 15, 15, 15);
    const __m256i opaque = _mm256_set1_epi8((char) 0xff);
    guint x = 0;

    for (; x + 8u <= width; x += 8u) {
        const guint8 *source_block = source + (gsize) x * 4u;
        __m256i pixels = _mm256_loadu_si256(
            (const __m256i *) (const void *) source_block);
        __m256i alpha = _mm256_shuffle_epi8(pixels, alpha_order);
        if (_mm256_movemask_epi8(_mm256_cmpeq_epi8(alpha, opaque)) == -1) {
            _mm256_storeu_si256((__m256i *) (void *) (destination + x), pixels);
        } else {
            for (guint i = 0; i < 8u; ++i)
                destination[x + i] = bgra_to_webp_argb(
                    source_block + (gsize) i * 4u);
        }
    }
    for (; x < width; ++x)
        destination[x] = bgra_to_webp_argb(source + (gsize) x * 4u);
}
#endif

#if defined(WPE_HAVE_ARM_NEON) && defined(__BYTE_ORDER__) && \
    (__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__)
static void
bgra_to_webp_argb_row_neon(const guint8 *source, guint32 *destination,
                          guint width)
{
    guint x = 0;
    const uint8x8_t opaque = vdup_n_u8(0xffu);

    for (; x + 8u <= width; x += 8u) {
        const guint8 *source_block = source + (gsize) x * 4u;
        uint8x8x4_t bgra = vld4_u8(source_block);
        uint8x8_t alpha_matches = vceq_u8(bgra.val[3], opaque);
        if (vget_lane_u64(vreinterpret_u64_u8(alpha_matches), 0) == UINT64_MAX) {
            vst4_u8((guint8 *) (void *) (destination + x), bgra);
        } else {
            for (guint i = 0; i < 8u; ++i)
                destination[x + i] = bgra_to_webp_argb(
                    source_block + (gsize) i * 4u);
        }
    }
    for (; x < width; ++x)
        destination[x] = bgra_to_webp_argb(source + (gsize) x * 4u);
}
#endif

void
wpe_bgra_to_webp_argb_row(const guint8 *source, guint32 *destination, guint width)
{
#if defined(WPE_HAVE_X86_AVX2) && defined(__BYTE_ORDER__) && \
    (__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__)
    if (cpu_supports_avx2()) {
        bgra_to_webp_argb_row_avx2(source, destination, width);
        return;
    }
#elif defined(WPE_HAVE_ARM_NEON) && defined(__BYTE_ORDER__) && \
    (__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__)
    bgra_to_webp_argb_row_neon(source, destination, width);
    return;
#endif
    for (guint x = 0; x < width; ++x)
        destination[x] = bgra_to_webp_argb(source + (gsize) x * 4u);
}

static inline void
bgra_to_white_pixel(const guint8 *source, guint8 *destination, gboolean bgr)
{
    guint alpha_complement = 255u - source[3];
    guint red = MIN(255u, (guint) source[2] + alpha_complement);
    guint green = MIN(255u, (guint) source[1] + alpha_complement);
    guint blue = MIN(255u, (guint) source[0] + alpha_complement);
    destination[0] = (guint8) (bgr ? blue : red);
    destination[1] = (guint8) green;
    destination[2] = (guint8) (bgr ? red : blue);
}

#if defined(WPE_HAVE_X86_AVX2)
__attribute__((target("avx2")))
static void
bgra_to_white_row_avx2(const guint8 *source, guint8 *destination,
                       guint width, gboolean bgr)
{
    const __m256i rgb_order = _mm256_setr_epi8(
        2, 1, 0, 6, 5, 4, 10, 9, 8, 14, 13, 12, -128, -128, -128, -128,
        2, 1, 0, 6, 5, 4, 10, 9, 8, 14, 13, 12, -128, -128, -128, -128);
    const __m256i bgr_order = _mm256_setr_epi8(
        0, 1, 2, 4, 5, 6, 8, 9, 10, 12, 13, 14, -128, -128, -128, -128,
        0, 1, 2, 4, 5, 6, 8, 9, 10, 12, 13, 14, -128, -128, -128, -128);
    const __m256i channel_order = bgr ? bgr_order : rgb_order;
    const __m256i alpha_order = _mm256_setr_epi8(
        3, 3, 3, 3, 7, 7, 7, 7, 11, 11, 11, 11, 15, 15, 15, 15,
        3, 3, 3, 3, 7, 7, 7, 7, 11, 11, 11, 11, 15, 15, 15, 15);
    const __m256i all_ones = _mm256_set1_epi8((char) 0xff);
    guint x = 0;

    for (; x + 8u <= width; x += 8u) {
        const guint8 *source_block = source + (gsize) x * 4u;
        guint8 *destination_block = destination + (gsize) x * 3u;
        __m256i pixels = _mm256_loadu_si256(
            (const __m256i *) (const void *) source_block);
        __m256i alpha = _mm256_shuffle_epi8(pixels, alpha_order);
        __m256i inverse_alpha = _mm256_xor_si256(alpha, all_ones);
        __m256i composited = _mm256_adds_epu8(pixels, inverse_alpha);
        __m256i rgb = _mm256_shuffle_epi8(composited, channel_order);
        __m128i first = _mm256_castsi256_si128(rgb);
        __m128i second = _mm256_extracti128_si256(rgb, 1);
        _mm_storeu_si128((__m128i *) (void *) destination_block, first);
        _mm_storel_epi64((__m128i *) (void *) (destination_block + 12u),
                         second);
        guint32 last_four = (guint32) _mm_cvtsi128_si32(
            _mm_srli_si128(second, 8));
        memcpy(destination_block + 20u, &last_four, sizeof(last_four));
    }
    for (; x < width; ++x)
        bgra_to_white_pixel(source + (gsize) x * 4u,
                            destination + (gsize) x * 3u, bgr);
}
#endif

#if defined(WPE_HAVE_ARM_NEON)
static void
bgra_to_white_row_neon(const guint8 *source, guint8 *destination,
                       guint width, gboolean bgr)
{
    guint x = 0;

    for (; width - x >= 16u; x += 16u) {
        const guint8 *source_block = source + (gsize) x * 4u;
        guint8 *destination_block = destination + (gsize) x * 3u;
        uint8x16x4_t bgra = vld4q_u8(source_block);
        uint8x16_t inverse_alpha = vmvnq_u8(bgra.val[3]);
        uint8x16_t red = vqaddq_u8(bgra.val[2], inverse_alpha);
        uint8x16_t green = vqaddq_u8(bgra.val[1], inverse_alpha);
        uint8x16_t blue = vqaddq_u8(bgra.val[0], inverse_alpha);
        uint8x16_t reverse = vdupq_n_u8(bgr ? 0xffu : 0u);
        uint8x16x3_t output;
        output.val[0] = vbslq_u8(reverse, blue, red);
        output.val[1] = green;
        output.val[2] = vbslq_u8(reverse, red, blue);
        vst3q_u8(destination_block, output);
    }
    for (; width - x >= 8u; x += 8u) {
        const guint8 *source_block = source + (gsize) x * 4u;
        guint8 *destination_block = destination + (gsize) x * 3u;
        uint8x8x4_t bgra = vld4_u8(source_block);
        uint8x8_t inverse_alpha = vmvn_u8(bgra.val[3]);
        uint8x8_t red = vqadd_u8(bgra.val[2], inverse_alpha);
        uint8x8_t green = vqadd_u8(bgra.val[1], inverse_alpha);
        uint8x8_t blue = vqadd_u8(bgra.val[0], inverse_alpha);
        uint8x8_t reverse = vdup_n_u8(bgr ? 0xffu : 0u);
        uint8x8x3_t output;
        output.val[0] = vbsl_u8(reverse, blue, red);
        output.val[1] = green;
        output.val[2] = vbsl_u8(reverse, red, blue);
        vst3_u8(destination_block, output);
    }
    for (; x < width; ++x)
        bgra_to_white_pixel(source + (gsize) x * 4u,
                            destination + (gsize) x * 3u, bgr);
}
#endif

static void
bgra_to_white_row(const guint8 *source, guint8 *destination, guint width,
                  gboolean bgr)
{
#if defined(WPE_HAVE_X86_AVX2)
    if (cpu_supports_avx2()) {
        bgra_to_white_row_avx2(source, destination, width, bgr);
        return;
    }
#elif defined(WPE_HAVE_ARM_NEON)
    bgra_to_white_row_neon(source, destination, width, bgr);
    return;
#endif
    for (guint x = 0; x < width; ++x)
        bgra_to_white_pixel(source + (gsize) x * 4u,
                            destination + (gsize) x * 3u, bgr);
}

void
wpe_bgra_to_rgb_white_row(const guint8 *source, guint8 *destination, guint width)
{
    bgra_to_white_row(source, destination, width, FALSE);
}

void
wpe_bgra_to_bgr_white_row(const guint8 *source, guint8 *destination, guint width)
{
    bgra_to_white_row(source, destination, width, TRUE);
}
