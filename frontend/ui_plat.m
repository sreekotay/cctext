/*
 * cctext-ui platform: libui-ng window, menus, and event step.
 * Text is uiDrawText (Core Text on macOS) drawn inside the area
 * callback. No op buffer and no string pool — cctext-gui keeps those.
 *
 * libui has no clipboard and no wheel event. Pasteboard and a local
 * scroll monitor are the two AppKit calls; the window is not an NSView
 * we own.
 */
#import <Cocoa/Cocoa.h>
#include "gui_plat.h"
#include "ui_hook.h"
#include "ui.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>

enum { UI_KEY_MAX = 512, UI_WHEEL_CAP = 8 };
#define UI_WHEEL_PREC_X 10.0f
#define UI_WHEEL_PREC_Y 16.0f

typedef struct UiFace {
    const char *family;
    float size;
    int weight;
    int italic;
    int heap;
    float adv_at;
    double space_w;
    double tab_w;
} UiFace;

static uiWindow *g_window;
static uiArea *g_area;
static uiMenu *g_apply_menu;
static uiMenuItem *g_apply_item[16];
static uiDrawContext *g_ctx;
static void (*g_paint)(void);
static void (*g_live_resize)(void);

static int g_ww = 960, g_hh = 640;
static int g_ready;
static int g_should_close;
static int g_resized;
static int g_fps = 60;
static unsigned int g_flags;
static int g_exit_key = 256;

static uint8_t g_key_down[UI_KEY_MAX];
static uint8_t g_key_pressed[UI_KEY_MAX];
static uint8_t g_key_repeat[UI_KEY_MAX];
static int g_chars[64];
static int g_nchar, g_char_rd;

static int g_mouse_x, g_mouse_y;
static int g_mouse_down, g_mouse_pressed, g_mouse_released;
static float g_wheel_x, g_wheel_y;
static float g_wheel_carry_x, g_wheel_carry_y;

static char *g_clip;
static size_t g_clip_cap;
static int g_menu_cmd;
static int g_apply_pick = -1;
static id g_scroll_mon;

static UiFace g_default_face = {
    "Menlo", 16, uiTextWeightNormal, uiTextItalicNormal, 0, 0, 0, 0
};

static UiFace *face_of(Font font) {
    if (!font.ct) return &g_default_face;
    return (UiFace *)font.ct;
}

static Font face_font(UiFace *face) {
    Font f;
    memset(&f, 0, sizeof f);
    f.ct = face;
    f.size = face ? face->size : 16;
    f.glyphCount = 256;
    f.texture.id = 1;
    return f;
}

static void sync_mods(uiModifiers m) {
    g_key_down[KEY_LEFT_SHIFT] = g_key_down[KEY_RIGHT_SHIFT] =
        (m & uiModifierShift) != 0;
    g_key_down[KEY_LEFT_CONTROL] = g_key_down[KEY_RIGHT_CONTROL] =
        (m & uiModifierCtrl) != 0;
    g_key_down[KEY_LEFT_ALT] = g_key_down[KEY_RIGHT_ALT] =
        (m & uiModifierAlt) != 0;
    g_key_down[KEY_LEFT_SUPER] = g_key_down[KEY_RIGHT_SUPER] =
        (m & uiModifierSuper) != 0;
}

static int key_of(const uiAreaKeyEvent *e) {
    unsigned c;
    if (!e) return -1;
    switch (e->ExtKey) {
    case uiExtKeyEscape: return KEY_ESCAPE;
    case uiExtKeyDelete: return KEY_DELETE;
    case uiExtKeyHome: return KEY_HOME;
    case uiExtKeyEnd: return KEY_END;
    case uiExtKeyUp: return KEY_UP;
    case uiExtKeyDown: return KEY_DOWN;
    case uiExtKeyLeft: return KEY_LEFT;
    case uiExtKeyRight: return KEY_RIGHT;
    case uiExtKeyNEnter: return KEY_ENTER;
    default: break;
    }
    c = (unsigned char)e->Key;
    if (c == '\n' || c == '\r') return KEY_ENTER;
    if (c == '\t') return KEY_TAB;
    if (c == '\b') return KEY_BACKSPACE;
    if (c >= 'A' && c <= 'Z') return KEY_A + (int)(c - 'A');
    if (c >= 'a' && c <= 'z') return KEY_A + (int)(c - 'a');
    if (c >= '0' && c <= '9') return KEY_0 + (int)(c - '0');
    if (c == '=') return KEY_EQUAL;
    if (c == '\\') return KEY_BACKSLASH;
    if (c == '`') return KEY_GRAVE;
    if (c == '.') return KEY_PERIOD;
    return -1;
}

static void push_typed_char(const uiAreaKeyEvent *e) {
    NSEvent *ev;
    NSString *chars;
    unichar u;
    if (!e || e->Up) return;
    if (e->ExtKey) return;
    if (e->Modifiers & (uiModifierCtrl | uiModifierSuper | uiModifierAlt))
        return;
    ev = [NSApp currentEvent];
    if (!ev || ev.type != NSEventTypeKeyDown) return;
    chars = ev.characters;
    if (chars.length < 1) return;
    u = [chars characterAtIndex:0];
    if (u >= 32 && u != 127 && u < 0xF700 && g_nchar < 64)
        g_chars[g_nchar++] = (int)u;
}

static void note_key(uiAreaKeyEvent *e) {
    int k;
    uiModifiers m;
    if (!e) return;
    m = e->Modifiers;
    if (e->Modifier) {
        if (e->Up) m &= ~e->Modifier;
        else m |= e->Modifier;
    }
    sync_mods(m);
    if (e->Modifier && !e->Key && !e->ExtKey) return;
    k = key_of(e);
    if (k < 0 || k >= UI_KEY_MAX) return;
    if (e->Up) {
        g_key_down[k] = 0;
        return;
    }
    if (g_key_down[k]) g_key_repeat[k] = 1;
    else g_key_pressed[k] = 1;
    g_key_down[k] = 1;
    if (k == g_exit_key) g_should_close = 1;
    push_typed_char(e);
}

static void on_draw(uiAreaHandler *h, uiArea *a, uiAreaDrawParams *p) {
    (void)h;
    (void)a;
    if (!p) return;
    if (p->AreaWidth > 1 && p->AreaHeight > 1) {
        int ww = (int)p->AreaWidth;
        int hh = (int)p->AreaHeight;
        if (ww != g_ww || hh != g_hh) {
            g_ww = ww;
            g_hh = hh;
            g_resized = 1;
        }
    }
    g_ctx = p->Context;
    if (g_paint) g_paint();
    g_ctx = NULL;
}

static void on_mouse(uiAreaHandler *h, uiArea *a, uiAreaMouseEvent *e) {
    (void)h;
    (void)a;
    if (!e) return;
    g_mouse_x = (int)e->X;
    g_mouse_y = (int)e->Y;
    if (e->Down == 1) {
        g_mouse_down = 1;
        g_mouse_pressed = 1;
    } else if (e->Up == 1) {
        g_mouse_down = 0;
        g_mouse_released = 1;
    } else {
        g_mouse_down = (e->Held1To64 & 1) != 0;
    }
    if (e->Modifiers) sync_mods(e->Modifiers);
}

static void on_crossed(uiAreaHandler *h, uiArea *a, int left) {
    (void)h;
    (void)a;
    (void)left;
}

static void on_drag_broken(uiAreaHandler *h, uiArea *a) {
    (void)h;
    (void)a;
    g_mouse_down = 0;
}

static int on_key(uiAreaHandler *h, uiArea *a, uiAreaKeyEvent *e) {
    (void)h;
    (void)a;
    note_key(e);
    return 1;
}

static uiAreaHandler g_handler = {
    on_draw,
    on_mouse,
    on_crossed,
    on_drag_broken,
    on_key
};

static void note_scroll(NSEvent *ev) {
    CGFloat dx, dy;
    if (!ev) return;
    dx = ev.scrollingDeltaX;
    dy = ev.scrollingDeltaY;
    if (dx == 0 && dy == 0) {
        dx = ev.deltaX;
        dy = ev.deltaY;
    }
    if (ev.hasPreciseScrollingDeltas) {
        g_wheel_carry_x += (float)(dx / (double)UI_WHEEL_PREC_X);
        g_wheel_carry_y += (float)(dy / (double)UI_WHEEL_PREC_Y);
    } else {
        g_wheel_carry_x += (float)dx;
        g_wheel_carry_y += (float)dy;
    }
}

static void install_scroll(void) {
    if (g_scroll_mon || !g_window) return;
    g_scroll_mon = [NSEvent addLocalMonitorForEventsMatchingMask:NSEventMaskScrollWheel
                                                         handler:^NSEvent *(NSEvent *ev) {
        NSWindow *win = (__bridge NSWindow *)(void *)uiControlHandle(uiControl(g_window));
        if (win && ev.window == win) note_scroll(ev);
        return ev;
    }];
}

static int wake_timer(void *data) {
    (void)data;
    return 1;
}

static int on_should_quit(void *data) {
    (void)data;
    g_should_close = 1;
    return 0;
}

static int on_closing(uiWindow *w, void *data) {
    (void)w;
    (void)data;
    g_should_close = 1;
    return 0;
}

static void on_sized(uiWindow *w, void *data) {
    int ww = 0, hh = 0;
    (void)data;
    uiWindowContentSize(w, &ww, &hh);
    if (ww > 1 && hh > 1 && (ww != g_ww || hh != g_hh)) {
        g_ww = ww;
        g_hh = hh;
        g_resized = 1;
    }
    (void)g_live_resize;
}

static void on_menu(uiMenuItem *item, uiWindow *w, void *data) {
    int tag = (int)(intptr_t)data;
    (void)item;
    (void)w;
    if (tag >= RTX_APPLY_TAG0 && tag < RTX_APPLY_TAG0 + 16) {
        g_menu_cmd = CMD_APPLY_NAMED;
        g_apply_pick = tag - RTX_APPLY_TAG0;
    } else {
        g_menu_cmd = tag;
        g_apply_pick = -1;
    }
}

static void add_item(uiMenu *menu, const char *title, int cmd) {
    uiMenuItem *it = uiMenuAppendItem(menu, title);
    uiMenuItemOnClicked(it, on_menu, (void *)(intptr_t)cmd);
}

/* darwin/menu.m: first field is the NSMenuItem. libui has no set-text,
 * and newItem() aborts once uiNewWindow has finalized the bar. */
static NSMenuItem *menu_ns(uiMenuItem *it) {
    return it ? *(__unsafe_unretained NSMenuItem **)(void *)it : nil;
}

static void apply_slots(void) {
    int i;
    uiMenuAppendSeparator(g_apply_menu);
    for (i = 0; i < 16; i++) {
        g_apply_item[i] = uiMenuAppendItem(g_apply_menu, "Apply");
        uiMenuItemOnClicked(g_apply_item[i], on_menu,
                            (void *)(intptr_t)(RTX_APPLY_TAG0 + i));
        [menu_ns(g_apply_item[i]) setHidden:YES];
    }
}

static void menu_build(void) {
    uiMenu *app, *file, *edit, *view, *go;
    if (g_apply_menu) return;
    app = uiNewMenu("cctext");
    add_item(app, "Quit cctext", CMD_QUIT);
    file = uiNewMenu("File");
    add_item(file, "Open…", CMD_OPEN);
    add_item(file, "Browse", CMD_BROWSE);
    add_item(file, "Save", CMD_SAVE);
    uiMenuAppendSeparator(file);
    add_item(file, "Close Window", CMD_QUIT);
    edit = uiNewMenu("Edit");
    add_item(edit, "Undo", CMD_UNDO);
    add_item(edit, "Redo", CMD_REDO);
    uiMenuAppendSeparator(edit);
    add_item(edit, "Cut", CMD_CUT);
    add_item(edit, "Copy", CMD_COPY);
    add_item(edit, "Paste", CMD_PASTE);
    add_item(edit, "Select All", CMD_SEL_ALL);
    g_apply_menu = uiNewMenu("Apply");
    add_item(g_apply_menu, "Show Menu", CMD_APPLY_MENU);
    add_item(g_apply_menu, "Cycle Prefix", CMD_APPLY);
    apply_slots();
    view = uiNewMenu("View");
    add_item(view, "Cycle View", CMD_VIEW);
    add_item(view, "Wrap", CMD_WRAP);
    add_item(view, "Rich / Source", CMD_RICH);
    add_item(view, "Follow Caret", CMD_FOLLOW);
    add_item(view, "Split", CMD_SPLIT);
    add_item(view, "Next File", CMD_NEXT);
    add_item(view, "Other Pane", CMD_PANE);
    uiMenuAppendSeparator(view);
    add_item(view, "Key Bindings", CMD_HELP);
    add_item(view, "Stats", CMD_STATS);
    go = uiNewMenu("Go");
    add_item(go, "Jump to Line…", CMD_JUMP);
    add_item(go, "Find…", CMD_FIND);
    uiMenuAppendSeparator(go);
    add_item(go, "Next Mark", CMD_NAV_NEXT);
    add_item(go, "Previous Mark", CMD_NAV_PREV);
    add_item(go, "Next Invalid", CMD_NAV_INV);
    add_item(go, "Previous Invalid", CMD_NAV_INV_PREV);
    add_item(go, "Fold Region", CMD_FOLD);
}

static void focus_area(void) {
    NSWindow *win;
    NSView *view;
    if (!g_window || !g_area) return;
    win = (__bridge NSWindow *)(void *)uiControlHandle(uiControl(g_window));
    view = (__bridge NSView *)(void *)uiControlHandle(uiControl(g_area));
    if (win && view) {
        [win makeFirstResponder:view];
        [NSApp activateIgnoringOtherApps:YES];
    }
}

static int g_dlg = -1;
static int g_dlg_open;
static uiWindow *g_dlg_win;

static int dlg_closing(uiWindow *w, void *data) {
    (void)w;
    (void)data;
    if (g_dlg < 0) g_dlg = 0;
    g_dlg_open = 0;
    return 1;
}

static void dlg_click(uiButton *b, void *data) {
    (void)b;
    g_dlg = (int)(intptr_t)data;
}

static int dlg_choice(const char *title, const char *msg,
                      const char *a, int av,
                      const char *b, int bv,
                      const char *c, int cv) {
    uiBox *box, *row;
    uiWindow *w;
    uiButton *ba, *bb, *bc;
    g_dlg = -1;
    g_dlg_open = 1;
    w = uiNewWindow(title, 440, 120, 0);
    g_dlg_win = w;
    uiWindowOnClosing(w, dlg_closing, NULL);
    box = uiNewVerticalBox();
    uiBoxSetPadded(box, 1);
    uiBoxAppend(box, uiControl(uiNewLabel(msg)), 0);
    row = uiNewHorizontalBox();
    uiBoxSetPadded(row, 1);
    ba = uiNewButton(a);
    uiButtonOnClicked(ba, dlg_click, (void *)(intptr_t)av);
    uiBoxAppend(row, uiControl(ba), 1);
    if (b && b[0]) {
        bb = uiNewButton(b);
        uiButtonOnClicked(bb, dlg_click, (void *)(intptr_t)bv);
        uiBoxAppend(row, uiControl(bb), 1);
    }
    if (c && c[0]) {
        bc = uiNewButton(c);
        uiButtonOnClicked(bc, dlg_click, (void *)(intptr_t)cv);
        uiBoxAppend(row, uiControl(bc), 1);
    }
    uiBoxAppend(box, uiControl(row), 0);
    uiWindowSetMargined(w, 1);
    uiWindowSetChild(w, uiControl(box));
    uiControlShow(uiControl(w));
    while (g_dlg < 0) uiMainStep(1);
    if (g_dlg_open) {
        g_dlg_open = 0;
        uiControlDestroy(uiControl(w));
    }
    g_dlg_win = NULL;
    return g_dlg;
}

static const char *family_from_path(const char *path) {
    if (!path) return "Menlo";
    if (strstr(path, "Georgia")) return "Georgia";
    if (strstr(path, "Arial")) return "Arial";
    if (strstr(path, "Courier")) return "Courier New";
    return "Menlo";
}

static UiFace *face_new(const char *family, float size, int weight, int italic) {
    UiFace *f = (UiFace *)calloc(1, sizeof *f);
    if (!f) return &g_default_face;
    f->family = family ? family : "Menlo";
    f->size = size > 0 ? size : 16;
    f->weight = weight > 0 ? weight : uiTextWeightNormal;
    f->italic = italic;
    f->heap = 1;
    return f;
}

/* libui drops trailing whitespace from layout extents. A space cluster
 * would measure 0 and the caret would sit on the previous glyph. */
static double layout_width(UiFace *face, float sz, const char *text, double *out_h) {
    uiAttributedString *s;
    uiFontDescriptor desc;
    uiDrawTextLayoutParams tp;
    uiDrawTextLayout *tl;
    double w = 0, h = 0;
    if (out_h) *out_h = 0;
    if (!face || !text || !text[0]) return 0;
    s = uiNewAttributedString(text);
    if (!s) return 0;
    memset(&desc, 0, sizeof desc);
    desc.Family = (char *)face->family;
    desc.Size = sz > 0 ? sz : face->size;
    desc.Weight = face->weight > 0 ? face->weight : uiTextWeightNormal;
    desc.Italic = face->italic ? uiTextItalicItalic : uiTextItalicNormal;
    desc.Stretch = uiTextStretchNormal;
    memset(&tp, 0, sizeof tp);
    tp.String = s;
    tp.DefaultFont = &desc;
    tp.Width = 1.0e7;
    tp.Align = uiDrawTextAlignLeft;
    tl = uiDrawNewTextLayout(&tp);
    if (tl) {
        uiDrawTextLayoutExtents(tl, &w, &h);
        uiDrawFreeTextLayout(tl);
    }
    uiFreeAttributedString(s);
    if (out_h) *out_h = h;
    return w;
}

static void face_adv(UiFace *face, float sz) {
    if (!face) return;
    if (face->adv_at == sz && face->space_w > 0) return;
    face->space_w = layout_width(face, sz, "n n", NULL) -
                    layout_width(face, sz, "nn", NULL);
    face->tab_w = layout_width(face, sz, "n\tn", NULL) -
                  layout_width(face, sz, "nn", NULL);
    if (face->space_w < 0) face->space_w = 0;
    if (face->tab_w < face->space_w) face->tab_w = face->space_w * 4;
    face->adv_at = sz;
}

static double text_width(UiFace *face, float sz, const char *text, double *out_h) {
    size_t n, i;
    int spaces = 0, tabs = 0;
    double w, h = 0;
    char *tmp = NULL;
    const char *prefix;
    if (!text || !text[0]) {
        if (out_h) *out_h = 0;
        return 0;
    }
    n = strlen(text);
    i = n;
    while (i > 0 && (text[i - 1] == ' ' || text[i - 1] == '\t')) {
        if (text[i - 1] == ' ') spaces++;
        else tabs++;
        i--;
    }
    prefix = text;
    if (i < n) {
        if (i == 0) {
            w = 0;
        } else {
            tmp = (char *)malloc(i + 1);
            if (!tmp) return 0;
            memcpy(tmp, text, i);
            tmp[i] = 0;
            prefix = tmp;
            w = layout_width(face, sz, prefix, &h);
        }
    } else {
        w = layout_width(face, sz, text, &h);
    }
    free(tmp);
    if (spaces || tabs) {
        face_adv(face, sz);
        w += spaces * face->space_w + tabs * face->tab_w;
        if (h <= 0) layout_width(face, sz, "M", &h);
    }
    if (out_h) *out_h = h;
    return w;
}

static void fill_rect(double x, double y, double w, double h, Color c) {
    uiDrawPath *path;
    uiDrawBrush b;
    if (!g_ctx || w <= 0 || h <= 0) return;
    memset(&b, 0, sizeof b);
    b.Type = uiDrawBrushTypeSolid;
    b.R = c.r / 255.0;
    b.G = c.g / 255.0;
    b.B = c.b / 255.0;
    b.A = c.a / 255.0;
    path = uiDrawNewPath(uiDrawFillModeWinding);
    uiDrawPathAddRectangle(path, x, y, w, h);
    uiDrawPathEnd(path);
    uiDrawFill(g_ctx, path, &b);
    uiDrawFreePath(path);
}

static void stroke_rect(double x, double y, double w, double h, Color c) {
    uiDrawPath *path;
    uiDrawBrush b;
    uiDrawStrokeParams sp;
    if (!g_ctx || w <= 0 || h <= 0) return;
    memset(&b, 0, sizeof b);
    b.Type = uiDrawBrushTypeSolid;
    b.R = c.r / 255.0;
    b.G = c.g / 255.0;
    b.B = c.b / 255.0;
    b.A = c.a / 255.0;
    memset(&sp, 0, sizeof sp);
    sp.Cap = uiDrawLineCapFlat;
    sp.Join = uiDrawLineJoinMiter;
    sp.Thickness = 1;
    sp.MiterLimit = uiDrawDefaultMiterLimit;
    path = uiDrawNewPath(uiDrawFillModeWinding);
    uiDrawPathAddRectangle(path, x + 0.5, y + 0.5, w - 1, h - 1);
    uiDrawPathEnd(path);
    uiDrawStroke(g_ctx, path, &b, &sp);
    uiDrawFreePath(path);
}

static void stroke_line(double x0, double y0, double x1, double y1, Color c) {
    uiDrawPath *path;
    uiDrawBrush b;
    uiDrawStrokeParams sp;
    if (!g_ctx) return;
    memset(&b, 0, sizeof b);
    b.Type = uiDrawBrushTypeSolid;
    b.R = c.r / 255.0;
    b.G = c.g / 255.0;
    b.B = c.b / 255.0;
    b.A = c.a / 255.0;
    memset(&sp, 0, sizeof sp);
    sp.Cap = uiDrawLineCapFlat;
    sp.Join = uiDrawLineJoinMiter;
    sp.Thickness = 1;
    sp.MiterLimit = uiDrawDefaultMiterLimit;
    path = uiDrawNewPath(uiDrawFillModeWinding);
    uiDrawPathNewFigure(path, x0 + 0.5, y0 + 0.5);
    uiDrawPathLineTo(path, x1 + 0.5, y1 + 0.5);
    uiDrawPathEnd(path);
    uiDrawStroke(g_ctx, path, &b, &sp);
    uiDrawFreePath(path);
}

static void draw_text(Font font, const char *text, double x, double y,
                      float fontSize, Color tint) {
    UiFace *face;
    uiAttributedString *s;
    uiAttribute *color;
    uiFontDescriptor desc;
    uiDrawTextLayoutParams tp;
    uiDrawTextLayout *tl;
    size_t n;
    if (!g_ctx || !text || !text[0]) return;
    face = face_of(font);
    s = uiNewAttributedString(text);
    if (!s) return;
    n = strlen(text);
    color = uiNewColorAttribute(tint.r / 255.0, tint.g / 255.0, tint.b / 255.0,
                                tint.a / 255.0);
    uiAttributedStringSetAttribute(s, color, 0, n);
    memset(&desc, 0, sizeof desc);
    desc.Family = (char *)face->family;
    desc.Size = fontSize > 0 ? fontSize : face->size;
    desc.Weight = face->weight > 0 ? face->weight : uiTextWeightNormal;
    desc.Italic = face->italic ? uiTextItalicItalic : uiTextItalicNormal;
    desc.Stretch = uiTextStretchNormal;
    memset(&tp, 0, sizeof tp);
    tp.String = s;
    tp.DefaultFont = &desc;
    tp.Width = 1.0e7;
    tp.Align = uiDrawTextAlignLeft;
    tl = uiDrawNewTextLayout(&tp);
    if (tl) {
        uiDrawText(g_ctx, tl, x, y);
        uiDrawFreeTextLayout(tl);
    }
    uiFreeAttributedString(s);
}

static void wheel_commit(void) {
    int sx = 0, sy = 0;
    while (g_wheel_carry_x >= 1.0f && sx < UI_WHEEL_CAP) {
        sx++;
        g_wheel_carry_x -= 1.0f;
    }
    while (g_wheel_carry_x <= -1.0f && sx > -UI_WHEEL_CAP) {
        sx--;
        g_wheel_carry_x += 1.0f;
    }
    while (g_wheel_carry_y >= 1.0f && sy < UI_WHEEL_CAP) {
        sy++;
        g_wheel_carry_y -= 1.0f;
    }
    while (g_wheel_carry_y <= -1.0f && sy > -UI_WHEEL_CAP) {
        sy--;
        g_wheel_carry_y += 1.0f;
    }
    if (sy != 0) {
        sx = 0;
        g_wheel_carry_x = 0;
    }
    g_wheel_x = (float)sx;
    g_wheel_y = (float)sy;
}

static void clear_edges(void) {
    memset(g_key_pressed, 0, sizeof g_key_pressed);
    memset(g_key_repeat, 0, sizeof g_key_repeat);
    g_mouse_pressed = 0;
    g_mouse_released = 0;
    g_wheel_x = g_wheel_y = 0;
    g_nchar = g_char_rd = 0;
    g_resized = 0;
}

static void pump(int wait) {
    int i, n;
    n = wait ? 1 : 32;
    for (i = 0; i < n; i++) uiMainStep(wait);
}

void ui_set_paint(void (*fn)(void)) { g_paint = fn; }

void ui_sync_paint(void) {
    NSView *view;
    if (!g_area || !g_paint) return;
    view = (__bridge NSView *)(void *)uiControlHandle(uiControl(g_area));
    if (!view) return;
    [view setNeedsDisplay:YES];
    [view display];
}

void fb_set_live_resize(void (*fn)(void)) { g_live_resize = fn; }

void SetTraceLogLevel(int level) { (void)level; }
void SetConfigFlags(unsigned int flags) { g_flags = flags; }
void SetExitKey(int key) { g_exit_key = key; }
void SetTargetFPS(int fps) { g_fps = fps > 0 ? fps : 60; }

void InitWindow(int width, int height, const char *title) {
    uiInitOptions opt;
    const char *err;
    memset(&opt, 0, sizeof opt);
    opt.Size = sizeof opt;
    err = uiInit(&opt);
    if (err) {
        fprintf(stderr, "cctext-ui: %s\n", err);
        uiFreeInitError(err);
        return;
    }
    uiOnShouldQuit(on_should_quit, NULL);
    uiMainSteps();
    menu_build();
    g_ww = width > 0 ? width : 960;
    g_hh = height > 0 ? height : 640;
    g_window = uiNewWindow(title ? title : "cctext", g_ww, g_hh, 1);
    uiWindowSetResizeable(g_window, 1);
    uiWindowSetMargined(g_window, 0);
    uiWindowOnClosing(g_window, on_closing, NULL);
    uiWindowOnContentSizeChanged(g_window, on_sized, NULL);
    g_area = uiNewArea(&g_handler);
    uiWindowSetChild(g_window, uiControl(g_area));
    uiControlShow(uiControl(g_window));
    uiTimer(16, wake_timer, NULL);
    install_scroll();
    focus_area();
    g_ready = 1;
    g_should_close = 0;
    (void)g_flags;
    (void)g_fps;
}

void CloseWindow(void) {
    if (g_scroll_mon) {
        [NSEvent removeMonitor:g_scroll_mon];
        g_scroll_mon = nil;
    }
    g_paint = NULL;
    g_ready = 0;
    if (g_window) {
        uiControlDestroy(uiControl(g_window));
        g_window = NULL;
        g_area = NULL;
    }
    uiUninit();
}

int WindowShouldClose(void) { return g_should_close; }
int IsWindowReady(void) { return g_ready; }
int IsWindowResized(void) {
    int r = g_resized;
    g_resized = 0;
    return r;
}
void *GetWindowHandle(void) {
    if (!g_window) return NULL;
    return (void *)uiControlHandle(uiControl(g_window));
}
int GetScreenWidth(void) { return g_ww; }
int GetScreenHeight(void) { return g_hh; }
Vector2 GetWindowScaleDPI(void) {
    return (Vector2){1, 1};
}

void BeginDrawing(void) { }
void EndDrawing(void) { ui_sync_paint(); }

void gui_input_begin_frame(void) {
    pump(0);
    wheel_commit();
}

void gui_input_consume(void) { clear_edges(); }

void PollInputEvents(void) { pump(0); }

void WaitTime(double seconds) {
    (void)seconds;
    pump(1);
}

void ClearBackground(Color color) {
    fill_rect(0, 0, g_ww, g_hh, color);
}

void BeginScissorMode(int x, int y, int w, int h) {
    uiDrawPath *path;
    if (!g_ctx || w <= 0 || h <= 0) return;
    uiDrawSave(g_ctx);
    path = uiDrawNewPath(uiDrawFillModeWinding);
    uiDrawPathAddRectangle(path, x, y, w, h);
    uiDrawPathEnd(path);
    uiDrawClip(g_ctx, path);
    uiDrawFreePath(path);
}

void EndScissorMode(void) {
    if (g_ctx) uiDrawRestore(g_ctx);
}

void DrawRectangle(int x, int y, int w, int h, Color color) {
    fill_rect(x, y, w, h, color);
}

void DrawRectangleLines(int x, int y, int w, int h, Color color) {
    stroke_rect(x, y, w, h, color);
}

void DrawLine(int x0, int y0, int x1, int y1, Color color) {
    stroke_line(x0, y0, x1, y1, color);
}

void DrawTextEx(Font font, const char *text, Vector2 pos, float fontSize,
                float spacing, Color tint) {
    (void)spacing;
    draw_text(font, text, pos.x, pos.y, fontSize, tint);
}

void DrawText(const char *text, int x, int y, int fontSize, Color color) {
    DrawTextEx(GetFontDefault(), text, (Vector2){(float)x, (float)y},
               (float)fontSize, 0, color);
}

Vector2 MeasureTextEx(Font font, const char *text, float fontSize, float spacing) {
    double h = 0;
    Vector2 out;
    (void)spacing;
    out.x = 0;
    out.y = 0;
    if (!text || !text[0]) return out;
    out.x = (float)text_width(face_of(font), fontSize, text, &h);
    out.y = (float)h;
    if (out.y <= 0) out.y = fontSize > 0 ? fontSize : 16;
    return out;
}

Font LoadFontEx(const char *fileName, int fontSize, int *codepoints, int codepointCount) {
    (void)codepoints;
    (void)codepointCount;
    return face_font(face_new(family_from_path(fileName),
                              (float)(fontSize > 0 ? fontSize : 16),
                              uiTextWeightNormal, uiTextItalicNormal));
}

Font GetFontDefault(void) { return face_font(&g_default_face); }

void UnloadFont(Font font) {
    UiFace *f = (UiFace *)font.ct;
    if (!f || f == &g_default_face || !f->heap) return;
    free(f);
}

Font gui_derive_font(Font base, int bold, int italic) {
    UiFace *src = face_of(base);
    int weight, it;
    if (!src) return base;
    if (!bold && !italic) return base;
    weight = bold ? uiTextWeightBold : src->weight;
    it = italic ? uiTextItalicItalic : src->italic;
    if (bold && italic && src->italic) it = uiTextItalicItalic;
    return face_font(face_new(src->family, src->size, weight, it));
}

void SetTextureFilter(Texture2D texture, int filter) {
    (void)texture;
    (void)filter;
}

int FileExists(const char *fileName) {
    return fileName && fileName[0] && access(fileName, F_OK) == 0;
}

int IsKeyDown(int key) {
    if (key < 0 || key >= UI_KEY_MAX) return 0;
    return g_key_down[key];
}
int IsKeyPressed(int key) {
    if (key < 0 || key >= UI_KEY_MAX) return 0;
    return g_key_pressed[key];
}
int IsKeyPressedRepeat(int key) {
    if (key < 0 || key >= UI_KEY_MAX) return 0;
    return g_key_repeat[key];
}
int GetCharPressed(void) {
    if (g_char_rd >= g_nchar) return 0;
    return g_chars[g_char_rd++];
}
int IsMouseButtonDown(int button) {
    return button == MOUSE_BUTTON_LEFT ? g_mouse_down : 0;
}
int IsMouseButtonPressed(int button) {
    return button == MOUSE_BUTTON_LEFT ? g_mouse_pressed : 0;
}
int IsMouseButtonReleased(int button) {
    return button == MOUSE_BUTTON_LEFT ? g_mouse_released : 0;
}
Vector2 GetMousePosition(void) {
    return (Vector2){(float)g_mouse_x, (float)g_mouse_y};
}
float GetMouseX(void) { return (float)g_mouse_x; }
float GetMouseY(void) { return (float)g_mouse_y; }
Vector2 GetMouseWheelMoveV(void) {
    return (Vector2){g_wheel_x, g_wheel_y};
}
float GetMouseWheelMove(void) { return g_wheel_y; }

void SetClipboardText(const char *text) {
    NSPasteboard *pb = [NSPasteboard generalPasteboard];
    NSString *s;
    [pb clearContents];
    if (!text) return;
    s = [[NSString alloc] initWithUTF8String:text];
    if (!s)
        s = [[NSString alloc] initWithBytes:text length:strlen(text)
                                   encoding:NSISOLatin1StringEncoding];
    if (s) [pb setString:s forType:NSPasteboardTypeString];
}

const char *GetClipboardText(void) {
    NSString *s = [[NSPasteboard generalPasteboard] stringForType:NSPasteboardTypeString];
    const char *u = s.UTF8String;
    size_t n = u ? strlen(u) : 0;
    if (n + 1 > g_clip_cap) {
        char *p = realloc(g_clip, n + 1);
        if (!p) return "";
        g_clip = p;
        g_clip_cap = n + 1;
    }
    if (!g_clip) return "";
    if (u) memcpy(g_clip, u, n + 1);
    else g_clip[0] = 0;
    return g_clip;
}

void gui_menu_install(void) { menu_build(); }

int gui_menu_poll_cmd(void) {
    int c = g_menu_cmd;
    g_menu_cmd = CMD_NONE;
    return c;
}

int gui_menu_pending(void) { return g_menu_cmd != CMD_NONE; }

int gui_menu_apply_pick(void) { return g_apply_pick; }

void gui_menu_set_apply(const char **names, size_t n) {
    size_t i;
    if (!g_apply_menu) return;
    if (n > 16) n = 16;
    for (i = 0; i < 16; i++) {
        NSMenuItem *it = menu_ns(g_apply_item[i]);
        const char *s;
        if (!it) continue;
        if (!names || i >= n || !names[i] || !names[i][0]) {
            [it setHidden:YES];
            continue;
        }
        s = names[i];
        [it setTitle:[NSString stringWithUTF8String:s]];
        [it setHidden:NO];
    }
}

void gui_clear_close(void) { g_should_close = 0; }

int gui_alert_unsaved(size_t nfiles) {
    char msg[160];
    if (nfiles <= 1)
        snprintf(msg, sizeof msg, "1 file has unsaved edits. Save before quitting?");
    else
        snprintf(msg, sizeof msg,
                 "%zu files have unsaved edits. Save before quitting?", nfiles);
    return dlg_choice("Unsaved changes", msg, "Save", 1, "Don't Save", 2, "Cancel", 0);
}

int gui_alert_changed(const char *path) {
    char msg[400];
    if (path && path[0])
        snprintf(msg, sizeof msg,
                 "%s was modified by another program. Overwrite it?", path);
    else
        snprintf(msg, sizeof msg,
                 "This file was modified by another program. Overwrite it?");
    return dlg_choice("File changed on disk", msg, "Overwrite", 1, "Cancel", 0, NULL, 0);
}

int gui_save_panel(const char *dir, char *out, size_t n) {
    char *p;
    (void)dir;
    if (!out || n == 0 || !g_window) return 0;
    out[0] = 0;
    p = uiSaveFile(g_window);
    if (!p) return 0;
    snprintf(out, n, "%s", p);
    uiFreeText(p);
    return out[0] ? 1 : 0;
}

int gui_osx_pick_file(const char *dir, char *out, size_t n) {
    char *p;
    (void)dir;
    if (!out || n == 0) return 0;
    out[0] = 0;
    if (!g_window) return -1;
    p = uiOpenFile(g_window);
    if (!p) return 0;
    snprintf(out, n, "%s", p);
    uiFreeText(p);
    return out[0] ? 1 : 0;
}

int gui_osx_install_resize(void *window, void (*fn)(void),
                           void **observer, void **center) {
    (void)window;
    (void)fn;
    (void)observer;
    (void)center;
    return 0;
}

void gui_osx_uninstall_resize(void *observer, void *center) {
    (void)observer;
    (void)center;
}

int gui_osx_shift_down(void) {
    return g_key_down[KEY_LEFT_SHIFT] || g_key_down[KEY_RIGHT_SHIFT];
}

int gui_osx_alt_down(void) {
    return g_key_down[KEY_LEFT_ALT] || g_key_down[KEY_RIGHT_ALT];
}
