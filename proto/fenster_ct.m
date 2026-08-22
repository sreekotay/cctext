/*
 * Spike: Fenster presents one CPU buffer; Core Text draws into it.
 * No document core. Goal: native-looking text + sane footprint vs Raylib.
 *
 *   clang -O2 -ObjC proto/fenster_ct.m -Ithird_party \
 *     -framework Cocoa -framework CoreText -framework CoreGraphics \
 *     -o bin/fenster_ct
 *   ./bin/fenster_ct
 *   footprint -p $(pgrep -n fenster_ct)
 */
#include <CoreText/CoreText.h>
#include <CoreGraphics/CoreGraphics.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>

#include "fenster.h" /* one TU: declarations + Apple implementation */
#include <unistd.h>

enum { W = 960, H = 640 };

static uint32_t g_buf[W * H];

static void clear_buf(uint32_t rgb) {
    size_t i;
    for (i = 0; i < (size_t)W * (size_t)H; i++) g_buf[i] = rgb;
}

static void fill_rect(int x0, int y0, int ww, int hh, uint32_t rgb) {
    int y, x;
    if (x0 < 0) { ww += x0; x0 = 0; }
    if (y0 < 0) { hh += y0; y0 = 0; }
    if (x0 + ww > W) ww = W - x0;
    if (y0 + hh > H) hh = H - y0;
    if (ww <= 0 || hh <= 0) return;
    for (y = y0; y < y0 + hh; y++)
        for (x = x0; x < x0 + ww; x++)
            g_buf[y * W + x] = rgb;
}

/* Bitmap matches fenster's mac path: BGRA little-endian, alpha ignored. */
static CGContextRef make_ctx(void) {
    CGColorSpaceRef space = CGColorSpaceCreateDeviceRGB();
    CGContextRef ctx = CGBitmapContextCreate(
        g_buf, W, H, 8, W * 4, space,
        kCGImageAlphaNoneSkipFirst | kCGBitmapByteOrder32Little);
    CGColorSpaceRelease(space);
    /* Core Text y-up; flip so (0,0) is top-left like fenster. */
    CGContextTranslateCTM(ctx, 0, H);
    CGContextScaleCTM(ctx, 1, -1);
    return ctx;
}

static CTFontRef load_font(CFStringRef name, CGFloat px) {
    CTFontRef f = CTFontCreateWithName(name, px, NULL);
    if (f) return f;
    return CTFontCreateUIFontForLanguage(kCTFontUIFontUserFixedPitch, px, NULL);
}

static CGColorRef rgba(CGFloat r, CGFloat g, CGFloat b, CGFloat a) {
    CGFloat c[4] = {r, g, b, a};
    CGColorSpaceRef s = CGColorSpaceCreateDeviceRGB();
    CGColorRef col = CGColorCreate(s, c);
    CGColorSpaceRelease(s);
    return col;
}

static CFAttributedStringRef attr_line(const char *utf8, CTFontRef font,
                                       CGColorRef fg) {
    CFStringRef s = CFStringCreateWithCString(NULL, utf8, kCFStringEncodingUTF8);
    CFStringRef keys[2] = {kCTFontAttributeName, kCTForegroundColorAttributeName};
    CFTypeRef vals[2] = {font, fg};
    CFDictionaryRef attrs = CFDictionaryCreate(
        NULL, (const void **)keys, (const void **)vals, 2,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFAttributedStringRef as = CFAttributedStringCreate(NULL, s, attrs);
    CFRelease(attrs);
    CFRelease(s);
    return as;
}

/* Draw UTF-8 at top-left (x,y). Returns typographic width in pixels. */
static double draw_text(CGContextRef ctx, CTFontRef font, CGColorRef fg,
                        const char *utf8, double x, double y) {
    CFAttributedStringRef as = attr_line(utf8, font, fg);
    CTLineRef line = CTLineCreateWithAttributedString(as);
    double ascent = 0, descent = 0, leading = 0;
    double w = CTLineGetTypographicBounds(line, &ascent, &descent, &leading);
    /* CTLineDraw uses baseline; y is top of line box. */
    CGContextSetTextPosition(ctx, x, y + ascent);
    CTLineDraw(line, ctx);
    CFRelease(line);
    CFRelease(as);
    return w;
}

/* Width of a UTF-8 prefix (byte length n) — measure must match paint. */
static double measure_prefix(CTFontRef font, const char *utf8, size_t n) {
    char tmp[512];
    CFAttributedStringRef as;
    CTLineRef line;
    double w;
    CGColorRef fg;
    if (n >= sizeof(tmp)) n = sizeof(tmp) - 1;
    memcpy(tmp, utf8, n);
    tmp[n] = 0;
    fg = rgba(1, 1, 1, 1);
    as = attr_line(tmp, font, fg);
    line = CTLineCreateWithAttributedString(as);
    w = CTLineGetTypographicBounds(line, NULL, NULL, NULL);
    CFRelease(line);
    CFRelease(as);
    CGColorRelease(fg);
    return w;
}

static void paint(void) {
    CGContextRef ctx = make_ctx();
    CTFontRef mono = load_font(CFSTR("SF Mono"), 16);
    CTFontRef prose = load_font(CFSTR("Georgia"), 18);
    CGColorRef fg = rgba(0.90, 0.90, 0.92, 1);
    CGColorRef key = rgba(0.31, 0.78, 0.86, 1);   /* keyword cyan */
    CGColorRef str = rgba(0.47, 0.78, 0.47, 1);   /* string green */
    CGColorRef num = rgba(0.86, 0.71, 0.31, 1);   /* number amber */
    CGColorRef dim = rgba(0.55, 0.55, 0.60, 1);
    double x, caret_x;
    const char *line0 = "{\"id\": 0, \"name\": \"item_0\", \"n\": -2.5}";
    const char *prefix = "{\"id\": "; /* caret after this */

    clear_buf(0x0018181c); /* dark bg, fenster XRGB */

    fill_rect(0, 0, W, 28, 0x0024262e);
    draw_text(ctx, mono, fg, "fenster + Core Text spike  (Esc quit)", 12, 6);

    draw_text(ctx, mono, dim, "mono / themed runs (measure == paint):", 16, 56);
    x = 16;
    x += draw_text(ctx, mono, fg, "{", x, 84);
    x += draw_text(ctx, mono, key, "\"id\"", x, 84);
    x += draw_text(ctx, mono, fg, ": ", x, 84);
    x += draw_text(ctx, mono, num, "0", x, 84);
    x += draw_text(ctx, mono, fg, ", ", x, 84);
    x += draw_text(ctx, mono, key, "\"name\"", x, 84);
    x += draw_text(ctx, mono, fg, ": ", x, 84);
    x += draw_text(ctx, mono, str, "\"item_0\"", x, 84);
    x += draw_text(ctx, mono, fg, ", ", x, 84);
    x += draw_text(ctx, mono, key, "\"n\"", x, 84);
    x += draw_text(ctx, mono, fg, ": ", x, 84);
    draw_text(ctx, mono, num, "-2.5", x, 84);
    draw_text(ctx, mono, fg, "}", x + measure_prefix(mono, "-2.5", 4), 84);

    /* Same line again as one string + caret from prefix measure. */
    draw_text(ctx, mono, dim, "caret from CTLineGetTypographicBounds(prefix):", 16, 120);
    draw_text(ctx, mono, fg, line0, 16, 148);
    caret_x = 16 + measure_prefix(mono, line0, strlen(prefix));
    fill_rect((int)caret_x, 148, 2, 18, 0x00ffc850);

    draw_text(ctx, prose, dim, "prose (Georgia):", 16, 200);
    draw_text(ctx, prose, fg,
              "The quick brown fox jumps over the lazy dog.", 16, 230);

    draw_text(ctx, mono, dim, "cluster (e + combining acute):", 16, 280);
    draw_text(ctx, mono, fg, "cafe\xCC\x81  (cafe + U+0301)", 16, 308);

    draw_text(ctx, mono, dim,
              "If this looks like system text and footprint is << Raylib, port.",
              16, 380);

    CFRelease(mono);
    CFRelease(prose);
    CGColorRelease(fg);
    CGColorRelease(key);
    CGColorRelease(str);
    CGColorRelease(num);
    CGColorRelease(dim);
    CGContextRelease(ctx);
}

int main(void) {
    struct fenster f = {
        .title = "cctext fenster+CT spike",
        .width = W,
        .height = H,
        .buf = g_buf,
    };
    int64_t t0;
    paint();
    if (fenster_open(&f) != 0) {
        fprintf(stderr, "fenster_open failed\n");
        return 1;
    }
    fprintf(stderr, "fenster_ct: pid %d  %dx%d  one CPU buffer\n", getpid(), W, H);
    t0 = fenster_time();
    while (fenster_loop(&f) == 0) {
        if (f.keys[27]) break; /* Esc */
        /* Keep ~30 fps without spinning. */
        {
            int64_t now = fenster_time();
            if (now - t0 < 33) fenster_sleep(33 - (now - t0));
            t0 = fenster_time();
        }
    }
    fenster_close(&f);
    return 0;
}
