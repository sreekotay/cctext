// cctext-render SVG backend: lunasvg 3.5.0 (third_party/lunasvg, cctext
// patches) over plutovg 1.3.3. C ABI (cr_svg.h) so the helper stays C.
//
// Fonts: only the faces in the pack exist (LUNASVG_DISABLE_LOAD_SYSTEM_FONTS,
// PLUTOVG_DISABLE_FONT_FACE_CACHE_LOAD). lunasvg walks the font-family list
// and asks the face cache for each name, lowercased (patch 0004); the names
// below are what a family list can hit. Anything else falls back to "",
// registered as Noto Sans.
#include <lunasvg.h>
#include <plutovg.h>

// lunasvg's own face cache (an internal header): measuring through it picks
// exactly the face the rasterizer will draw with.
#include "graphics.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>

#include "cr_proto.h"
#include "cr_svg.h"

namespace {

struct Alias {
    const char* face;   // bundled family (lowercase)
    const char* name;   // what an SVG may ask for (lowercase)
};

// Generic families (CSS Fonts 4) and the named families common in
// draw.io, Mermaid, matplotlib, Inkscape and Office exports.
const Alias kAliases[] = {
    {"noto sans", ""},
    {"noto sans", "sans-serif"},
    {"noto sans", "system-ui"},
    {"noto sans", "ui-sans-serif"},
    {"noto sans", "cursive"},
    {"noto sans", "fantasy"},
    {"noto sans", "emoji"},
    {"noto sans", "math"},
    {"noto sans", "arial"},
    {"noto sans", "helvetica"},
    {"noto sans", "helvetica neue"},
    {"noto sans", "verdana"},
    {"noto sans", "tahoma"},
    {"noto sans", "trebuchet ms"},
    {"noto sans", "segoe ui"},
    {"noto sans", "roboto"},
    {"noto sans", "open sans"},
    {"noto sans", "lato"},
    {"noto sans", "inter"},
    {"noto sans", "calibri"},
    {"noto sans", "liberation sans"},
    {"noto sans", "dejavu sans"},
    {"noto sans", "bitstream vera sans"},
    {"noto sans", "computer modern sans serif"},
    {"noto sans", "lucida grande"},
    {"noto sans", "geneva"},
    {"noto sans", "-apple-system"},
    {"noto sans", "blinkmacsystemfont"},
    {"noto serif", "serif"},
    {"noto serif", "ui-serif"},
    {"noto serif", "times new roman"},
    {"noto serif", "times"},
    {"noto serif", "georgia"},
    {"noto serif", "cambria"},
    {"noto serif", "liberation serif"},
    {"noto serif", "dejavu serif"},
    {"noto serif", "stix"},
    {"noto serif", "stixgeneral"},
    {"noto serif", "cmr10"},
    {"noto sans mono", "noto mono"},
    {"noto sans mono", "monospace"},
    {"noto sans mono", "ui-monospace"},
    {"noto sans mono", "courier new"},
    {"noto sans mono", "courier"},
    {"noto sans mono", "consolas"},
    {"noto sans mono", "menlo"},
    {"noto sans mono", "monaco"},
    {"noto sans mono", "sf mono"},
    {"noto sans mono", "liberation mono"},
    {"noto sans mono", "dejavu sans mono"},
    {"noto sans mono", "source code pro"},
    {"noto sans mono", "fira code"},
    {"noto sans mono", "fira mono"},
};

void lower(char* s)
{
    for(; *s; ++s) {
        if(*s >= 'A' && *s <= 'Z')
            *s = static_cast<char>(*s - 'A' + 'a');
    }
}

} // namespace

namespace {

// lunasvg's SVGLayoutState::font(): the first family in the list the cache
// knows (quotes and blanks stripped, lowercased), else the "" fallback.
plutovg_font_face_t* pick_face(const char* families, bool bold, bool italic)
{
    std::string_view input(families ? families : "");
    auto* cache = lunasvg::fontFaceCache();
    while(!input.empty()) {
        auto family = input.substr(0, input.find(','));
        input.remove_prefix(family.length());
        if(!input.empty() && input.front() == ',')
            input.remove_prefix(1);
        while(!family.empty() && (family.front() == ' ' || family.front() == '\t'))
            family.remove_prefix(1);
        while(!family.empty() && (family.back() == ' ' || family.back() == '\t'))
            family.remove_suffix(1);
        if(!family.empty() && (family.front() == '\'' || family.front() == '"')) {
            auto quote = family.front();
            family.remove_prefix(1);
            if(!family.empty() && family.back() == quote)
                family.remove_suffix(1);
        }
        std::string name(family);
        for(auto& ch : name) {
            if(ch >= 'A' && ch <= 'Z')
                ch = static_cast<char>(ch - 'A' + 'a');
        }
        if(!name.empty()) {
            auto face = cache->getFontFace(name, bold, italic);
            if(!face.isNull())
                return face.get();
        }
    }
    return cache->getFontFace(std::string(), bold, italic).get();
}

} // namespace

extern "C" double cr_svg_measure(const char* text, size_t n, double px, int weight, int italic, const char* families)
{
    auto* face = pick_face(families, weight >= 600, italic != 0);
    if(!face || !text || !n || n > 0x7fffffff)
        return 0;
    return plutovg_font_face_text_extents(face, static_cast<float>(px), text, static_cast<int>(n),
                                          PLUTOVG_TEXT_ENCODING_UTF8, nullptr);
}

extern "C" void cr_svg_font_metrics(double px, int weight, int italic, const char* families, double* ascent, double* descent)
{
    auto* face = pick_face(families, weight >= 600, italic != 0);
    float a = 0, d = 0, lg = 0;
    if(face)
        plutovg_font_face_get_metrics(face, static_cast<float>(px), &a, &d, &lg, nullptr);
    *ascent = a;
    *descent = -d; // plutovg's descent is negative (below the baseline)
}

struct cr_svg {
    std::unique_ptr<lunasvg::Document> doc;
};

namespace {

// The helper's filter bounds (cr_proto.h), set before the first parse.
void apply_limits()
{
    static bool done = false;
    if(done)
        return;
    done = true;
    lunasvg_set_filter_limits(double(CR_FILTER_PIXELS_MAX), CR_FILTER_BLUR_MAX, CR_FILTER_PRIMITIVES_MAX, size_t(CR_FILTER_BYTES_MAX),
                              double(CR_FILTER_WORK_MAX));
}

} // namespace

extern "C" int cr_svg_add_font(const char* family, int bold, int italic, const void* data, size_t n)
{
    char fam[128];
    if(!family || std::strlen(family) >= sizeof fam)
        return -1;
    std::strcpy(fam, family);
    lower(fam);
    if(!lunasvg_add_font_face_from_data(fam, bold != 0, italic != 0, data, n, nullptr, nullptr))
        return -1;
    // Per-glyph fallback: a glyph the chosen face lacks comes from the first
    // regular face (in registration order: render/manifest.txt) that has it,
    // emboldened / slanted as the text asks (plutovg patch 0004).
    if(!bold && !italic) {
        auto face = lunasvg::fontFaceCache()->getFontFace(fam, false, false);
        if(!face.isNull())
            plutovg_font_face_add_fallback(face.get());
    }
    for(const auto& a : kAliases) {
        if(std::strcmp(a.face, fam) == 0)
            lunasvg_add_font_face_from_data(a.name, bold != 0, italic != 0, data, n, nullptr, nullptr);
    }
    return 0;
}

namespace {

struct LazyFace {
    const void* (*load)(void*, size_t*);
    void* closure;
};

LazyFace g_lazy[4];
int g_nlazy;

plutovg_font_face_t* load_lazy(void* closure)
{
    auto* lazy = static_cast<LazyFace*>(closure);
    size_t n = 0;
    const void* data = lazy->load(lazy->closure, &n);
    if(!data || !n || n > 0x7fffffff)
        return nullptr;
    return plutovg_font_face_load_from_data(data, static_cast<unsigned>(n), 0, nullptr, nullptr);
}

} // namespace

extern "C" int cr_svg_add_lazy_fallback(const void* (*load)(void*, size_t*), void* closure)
{
    if(!load || g_nlazy >= 4)
        return -1;
    g_lazy[g_nlazy] = LazyFace{load, closure};
    plutovg_font_face_add_fallback_loader(load_lazy, &g_lazy[g_nlazy]);
    g_nlazy++;
    return 0;
}

extern "C" cr_svg* cr_svg_parse(const char* data, size_t n, float* w, float* h)
{
    apply_limits();
    auto doc = lunasvg::Document::loadFromData(data, n);
    if(!doc)
        return nullptr;
    *w = doc->width();
    *h = doc->height();
    auto s = new cr_svg;
    s->doc = std::move(doc);
    return s;
}

extern "C" void cr_svg_render(cr_svg* s, float sx, float sy, uint32_t pw, uint32_t ph, uint32_t bg, uint8_t* rgba)
{
    // plutovg surfaces are premultiplied ARGB32 in native order.
    uint32_t a = bg & 0xff;
    uint32_t r = (bg >> 24) & 0xff, g = (bg >> 16) & 0xff, b = (bg >> 8) & 0xff;
    uint32_t fill = (a << 24) | (((r * a + 127) / 255) << 16) | (((g * a + 127) / 255) << 8) | ((b * a + 127) / 255);
    auto* p32 = reinterpret_cast<uint32_t*>(rgba);
    size_t npx = static_cast<size_t>(pw) * ph;
    for(size_t i = 0; i < npx; i++)
        p32[i] = fill;
    lunasvg::Bitmap bm(rgba, static_cast<int>(pw), static_cast<int>(ph), static_cast<int>(pw) * 4);
    s->doc->render(bm, lunasvg::Matrix(sx, 0, 0, sy, 0, 0));
    for(size_t i = 0; i < npx; i++) {
        uint32_t v = p32[i];
        uint8_t* o = rgba + i * 4;
        o[0] = static_cast<uint8_t>(v >> 16);
        o[1] = static_cast<uint8_t>(v >> 8);
        o[2] = static_cast<uint8_t>(v);
        o[3] = static_cast<uint8_t>(v >> 24);
    }
}

extern "C" void cr_svg_free(cr_svg* s)
{
    delete s;
}

extern "C" int cr_svg_png(const char* svg_path, const char* png_path, float scale)
{
    apply_limits();
    auto doc = lunasvg::Document::loadFromFile(svg_path);
    if(!doc)
        return -1;
    float w = doc->width() * scale, h = doc->height() * scale;
    if(!(w >= 1 && h >= 1 && w < 16384 && h < 16384))
        return -1;
    auto pw = static_cast<int>(std::ceil(w)), ph = static_cast<int>(std::ceil(h));
    lunasvg::Bitmap bm(pw, ph);
    bm.clear(0);
    doc->render(bm, lunasvg::Matrix(scale, 0, 0, scale, 0, 0));
    return bm.writeToPng(png_path) ? 0 : -1;
}

extern "C" int cr_svg_png_data(const char* data, size_t n, const char* png_path, float scale, uint32_t bg)
{
    apply_limits();
    auto doc = lunasvg::Document::loadFromData(data, n);
    if(!doc)
        return -1;
    float w = doc->width() * scale, h = doc->height() * scale;
    if(!(w >= 1 && h >= 1 && w < 16384 && h < 16384))
        return -1;
    auto pw = static_cast<int>(std::ceil(w)), ph = static_cast<int>(std::ceil(h));
    lunasvg::Bitmap bm(pw, ph);
    bm.clear(bg);
    doc->render(bm, lunasvg::Matrix(scale, 0, 0, scale, 0, 0));
    return bm.writeToPng(png_path) ? 0 : -1;
}
