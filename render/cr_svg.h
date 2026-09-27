/* cctext-render SVG backend (lunasvg + plutovg). C ABI; cr_svg.cpp. */
#ifndef CR_SVG_H
#define CR_SVG_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct cr_svg cr_svg;

/* Register a bundled face under `family` (lowercased) and, for the three
 * bundled families, their generic / common aliases. `data` must outlive
 * the process (it is shared, not copied). 0 = ok. */
int cr_svg_add_font(const char *family, int bold, int italic, const void *data, size_t n);

/* Parse. Intrinsic size in CSS px (width / height, else the viewBox).
 * NULL when the bytes are not an SVG lunasvg reads. */
cr_svg *cr_svg_parse(const char *data, size_t n, float *w_css, float *h_css);

/* Draw into px_w x px_h, scaled by (sx, sy) from CSS px, over `bg`
 * (0xRRGGBBAA; 0 = transparent). Output: RGBA8 premultiplied, stride
 * px_w * 4. */
void cr_svg_render(cr_svg *s, float sx, float sy, uint32_t px_w, uint32_t px_h, uint32_t bg,
                   uint8_t *rgba);

void cr_svg_free(cr_svg *s);

/* Development: render a file to PNG at `scale` (not sandboxed; writes a
 * file). 0 = ok. */
int cr_svg_png(const char *svg_path, const char *png_path, float scale);

#ifdef __cplusplus
}
#endif

#endif /* CR_SVG_H */
