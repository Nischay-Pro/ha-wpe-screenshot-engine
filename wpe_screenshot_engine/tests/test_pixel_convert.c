#include "pixel_convert.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define MAX_TEST_WIDTH 4096u
#define CANARY_BYTES 16u

static void
reference_bgra_to_rgba(const guint8 *source, guint8 *destination)
{
    guint alpha = source[3];
    guint red = source[2];
    guint green = source[1];
    guint blue = source[0];
    if (alpha && alpha < 255u) {
        red = MIN(255u, (red * 255u + alpha / 2u) / alpha);
        green = MIN(255u, (green * 255u + alpha / 2u) / alpha);
        blue = MIN(255u, (blue * 255u + alpha / 2u) / alpha);
    } else if (!alpha) {
        red = green = blue = 0;
    }
    destination[0] = (guint8) red;
    destination[1] = (guint8) green;
    destination[2] = (guint8) blue;
    destination[3] = (guint8) alpha;
}

static void
reference_bgra_to_rgb_white(const guint8 *source, guint8 *destination)
{
    guint complement = 255u - source[3];
    destination[0] = (guint8) MIN(255u, (guint) source[2] + complement);
    destination[1] = (guint8) MIN(255u, (guint) source[1] + complement);
    destination[2] = (guint8) MIN(255u, (guint) source[0] + complement);
}

static gboolean
check_case(guint width, guint scenario, guint partial_position)
{
    static const guint8 alpha_values[] = {0u, 1u, 2u, 127u, 128u, 254u, 255u};
    static const guint8 invalid_alpha_values[] = {0u, 1u, 2u, 127u, 128u, 254u};
    guint8 source_storage[MAX_TEST_WIDTH * 4u + 2u];
    guint8 rgba_storage[MAX_TEST_WIDTH * 4u + CANARY_BYTES + 2u];
    guint8 expected_rgba[MAX_TEST_WIDTH * 4u] = {0};
    guint8 rgb_storage[MAX_TEST_WIDTH * 3u + CANARY_BYTES + 2u];
    guint8 expected_rgb[MAX_TEST_WIDTH * 3u] = {0};
    guint8 bgr_storage[MAX_TEST_WIDTH * 3u + CANARY_BYTES + 2u];
    guint8 expected_bgr[MAX_TEST_WIDTH * 3u] = {0};
    guint32 argb_storage[MAX_TEST_WIDTH + CANARY_BYTES / sizeof(guint32) + 2u];
    guint32 *argb = argb_storage + 1u;
    guint32 random_state = UINT32_C(0x8f31a5c7) ^ width ^
        (scenario * UINT32_C(0x9e3779b9));
    guint32 expected_argb[MAX_TEST_WIDTH] = {0};
    guint8 *source = source_storage + 1u;
    g_autofree guint8 *exact_source = NULL;
    guint8 *rgba = rgba_storage + 1u;
    guint8 *rgb = rgb_storage + 1u;
    guint8 *bgr = bgr_storage + 1u;
    gboolean expected_opaque = TRUE;

    if (width > MAX_TEST_WIDTH || scenario > 5u || partial_position > 7u)
        return FALSE;

    memset(source_storage, 0xa5, sizeof(source_storage));
    for (guint x = 0; x < width; ++x) {
        guint alpha;
        if (scenario >= 3u && scenario <= 4u) {
            random_state ^= random_state << 13;
            random_state ^= random_state >> 17;
            random_state ^= random_state << 5;
            alpha = scenario == 3u ? 255u : random_state & 0xffu;
            guint red = (random_state >> 8) & 0xffu;
            guint green = (random_state >> 16) & 0xffu;
            guint blue = random_state >> 24;
            source[(gsize) x * 4u] = (guint8) (blue * alpha / 255u);
            source[(gsize) x * 4u + 1u] = (guint8) (green * alpha / 255u);
            source[(gsize) x * 4u + 2u] = (guint8) (red * alpha / 255u);
        } else if (scenario == 5u) {
            alpha = invalid_alpha_values[(x + width) %
                                         G_N_ELEMENTS(invalid_alpha_values)];
            source[(gsize) x * 4u] = 255u;
            source[(gsize) x * 4u + 1u] = (guint8) MIN(255u, alpha + 2u);
            source[(gsize) x * 4u + 2u] = (guint8) (alpha + 1u);
        } else {
            alpha = scenario == 0u ? 255u :
                (scenario == 1u ? alpha_values[(x + width) % G_N_ELEMENTS(alpha_values)] :
                 (x % 8u == partial_position ? 128u : 255u));
            source[(gsize) x * 4u] =
                (guint8) (alpha * ((x * 17u + 31u) % 256u) / 255u);
            source[(gsize) x * 4u + 1u] =
                (guint8) (alpha * ((x * 29u + 71u) % 256u) / 255u);
            source[(gsize) x * 4u + 2u] =
                (guint8) (alpha * ((x * 43u + 19u) % 256u) / 255u);
        }
        source[(gsize) x * 4u + 3u] = (guint8) alpha;
        if (alpha != 255u)
            expected_opaque = FALSE;
        reference_bgra_to_rgba(source + (gsize) x * 4u,
                               expected_rgba + (gsize) x * 4u);
        reference_bgra_to_rgb_white(source + (gsize) x * 4u,
                                    expected_rgb + (gsize) x * 3u);
        expected_bgr[(gsize) x * 3u] = expected_rgb[(gsize) x * 3u + 2u];
        expected_bgr[(gsize) x * 3u + 1u] = expected_rgb[(gsize) x * 3u + 1u];
        expected_bgr[(gsize) x * 3u + 2u] = expected_rgb[(gsize) x * 3u];
        expected_argb[x] = ((guint32) expected_rgba[(gsize) x * 4u + 3u] << 24) |
            ((guint32) expected_rgba[(gsize) x * 4u] << 16) |
            ((guint32) expected_rgba[(gsize) x * 4u + 1u] << 8) |
            expected_rgba[(gsize) x * 4u + 2u];
    }

    exact_source = g_malloc((gsize) width * 4u + 1u);
    memcpy(exact_source + 1u, source, (gsize) width * 4u);
    source = exact_source + 1u;

    memset(rgba_storage, 0xa5, sizeof(rgba_storage));
    memset(rgb_storage, 0xa5, sizeof(rgb_storage));
    memset(bgr_storage, 0xa5, sizeof(bgr_storage));
    memset(argb_storage, 0xa5, sizeof(argb_storage));
    wpe_bgra_to_rgba_row(source, rgba, width);
    wpe_bgra_to_rgb_white_row(source, rgb, width);
    wpe_bgra_to_bgr_white_row(source, bgr, width);
    wpe_bgra_to_webp_argb_row(source, argb, width);

    if (wpe_bgra_row_is_opaque(source, width) != expected_opaque ||
        memcmp(rgba, expected_rgba, (gsize) width * 4u) ||
        memcmp(rgb, expected_rgb, (gsize) width * 3u) ||
        memcmp(bgr, expected_bgr, (gsize) width * 3u) ||
        memcmp(argb, expected_argb, (gsize) width * sizeof(guint32))) {
        fprintf(stderr,
                "pixel conversion mismatch: width=%u scenario=%u partial=%u\n",
                width, scenario, partial_position);
        return FALSE;
    }
    if (memcmp(source, source_storage + 1u, (gsize) width * 4u)) {
        fprintf(stderr, "pixel conversion modified its input: width=%u\n", width);
        return FALSE;
    }
    for (gsize i = 0; i < sizeof(rgba_storage); ++i) {
        gboolean is_output = i >= 1u && i < (gsize) width * 4u + 1u;
        if (!is_output && rgba_storage[i] != 0xa5u) {
            fprintf(stderr, "RGBA conversion overwrote its output: width=%u\n", width);
            return FALSE;
        }
    }
    for (gsize i = 0; i < sizeof(rgb_storage); ++i) {
        gboolean is_output = i >= 1u && i < (gsize) width * 3u + 1u;
        if (!is_output && rgb_storage[i] != 0xa5u) {
            fprintf(stderr, "RGB conversion overwrote its output: width=%u\n", width);
            return FALSE;
        }
    }
    for (gsize i = 0; i < sizeof(bgr_storage); ++i) {
        gboolean is_output = i >= 1u && i < (gsize) width * 3u + 1u;
        if (!is_output && bgr_storage[i] != 0xa5u) {
            fprintf(stderr, "BGR conversion overwrote its output: width=%u\n", width);
            return FALSE;
        }
    }
    if (argb_storage[0] != UINT32_C(0xa5a5a5a5)) {
        fprintf(stderr, "WebP conversion overwrote its output: width=%u\n", width);
        return FALSE;
    }
    for (gsize i = (gsize) width + 1u; i < G_N_ELEMENTS(argb_storage); ++i) {
        if (argb_storage[i] != UINT32_C(0xa5a5a5a5)) {
            fprintf(stderr, "WebP conversion overwrote its output: width=%u\n", width);
            return FALSE;
        }
    }
    return TRUE;
}

static gboolean
check_single_translucent_pixel(void)
{
    guint8 pixels[80u * 4u + 2u];

    for (guint width = 1u; width <= 80u; ++width) {
        memset(pixels, 0, sizeof(pixels));
        for (guint x = 0; x < width; ++x)
            memset(pixels + 1u + (gsize) x * 4u, 0xff, 4u);

        if (!wpe_bgra_row_is_opaque(pixels + 1u, width)) {
            fprintf(stderr, "opaque row rejected: width=%u\n", width);
            return FALSE;
        }

        for (guint x = 0; x < width; ++x) {
            memset(pixels + 1u + (gsize) x * 4u, 254, 4u);
            if (wpe_bgra_row_is_opaque(pixels + 1u, width)) {
                fprintf(stderr,
                        "translucent pixel missed: width=%u pixel=%u\n",
                        width, x);
                return FALSE;
            }
            memset(pixels + 1u + (gsize) x * 4u, 0xff, 4u);
        }
    }
    return TRUE;
}

int
main(void)
{
    static const guint widths[] = {
        0u, 1u, 2u, 3u, 4u, 5u, 6u, 7u, 8u, 9u, 10u, 11u,
        12u, 13u, 14u, 15u, 16u, 17u, 24u, 25u, 31u, 32u, 33u,
        40u, 63u, 64u, 65u, 4095u, 4096u,
    };
    for (guint w = 0; w < G_N_ELEMENTS(widths); ++w) {
        for (guint scenario = 0u; scenario <= 5u; ++scenario) {
            guint partial_positions = scenario == 2u ? 8u : 1u;
            for (guint partial = 0u; partial < partial_positions; ++partial) {
                if (!check_case(widths[w], scenario, partial))
                    return 1;
            }
        }
    }
    if (!check_single_translucent_pixel())
        return 1;
    puts("pixel conversion tests passed");
    return 0;
}
