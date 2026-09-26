/*
 * cctext-ui platform: libui-ng window, menus, and event step (portable C).
 * Text is uiDrawText (Core Text on macOS, Pango on GTK, DirectWrite on
 * Windows) drawn inside the area callback. No op buffer and no string
 * pool.
 *
 * What libui does not expose — typed text, wheel, clipboard, menu retitle,
 * a deadline event step — is the per-OS shim in ui_os.h
 * (ui_os_darwin.m / ui_os_gtk.c / ui_os_win32.c).
 */
#include "gui_plat.h"
#include "../core/cmd.cch"
#include "ui_hook.h"
#include "ui_turn.h"
#include "ui.h"
#include "ui_os.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <time.h>

enum {
    UI_KEY_MAX = 512,
    UI_WHEEL_CAP = 8,
    UI_BLINK_RECTS = 8,
    UI_CARET_RECTS = 8, /* hex: 2; a box caret: one per row, capped */
    UI_SCISSOR_MAX = 8,
    UI_CARET_FADE_MS = 100
};

typedef struct UiRect {
    double x, y, w, h;
} UiRect;

/* One size of a face: whether every printable ASCII glyph has one
 * advance (probed once: "i", "W", "0", "m", " "). kind 0 unknown, 1
 * fixed pitch, 2 proportional. */
enum { UI_MONO_SIZES = 6 };
typedef struct UiMono {
    float sz;
    int kind;
    double adv;
    double h;
} UiMono;

typedef struct UiFace {
    const char *family;
    float size;
    int weight;
    int italic;
    int heap;
    float adv_at;
    double space_w;
    double tab_w;
    UiMono mono[UI_MONO_SIZES]; /* per size: fixed pitch? (mono_of) */
    int mono_next;
} UiFace;

static uiWindow *g_window;
static uiArea *g_area;
static uiMenu *g_apply_menu;
static uiMenuItem *g_apply_item[UI_APPLY_SLOTS];
static uiDrawContext *g_ctx;
static void (*g_paint)(void);
static RtxUiTurn g_turn_state;
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
static int g_mouse_mid_pressed; /* middle button (a tab close) */
static float g_wheel_x, g_wheel_y;
static float g_wheel_carry_x, g_wheel_carry_y;

static char *g_clip;
static int g_menu_cmd;
static int g_apply_pick = -1;

/* Draw clip (area coordinates). g_clip_on only inside on_draw. */
static int g_clip_on;
static UiRect g_clip_r;
static int g_clip_whole;

/* Blink rects noted by the last full Draw. */
static UiRect g_blink_r[UI_BLINK_RECTS];
static int g_blink_n;
static int g_blink_noting;
static int g_blink_valid;
static int g_blink_drawn;

static int g_focused = 1;
static int g_focus_edge = -1;

/* Scissor stack (area coordinates): caret bars are clipped to it. */
static UiRect g_scis[UI_SCISSOR_MAX];
static int g_scis_n;

/* Caret layer. new = what the whole Draw in progress (or the last one)
 * found; cur = what the layer shows. dirty: new differs, apply it after
 * the Draw (a toolkit must not move widgets / views inside one). */
static int g_caret_want = 1;
static int g_caret_fade;
static int g_caret_ok;
static int g_caret_collect;
static UiCaretRect g_caret_new[UI_CARET_RECTS];
static int g_caret_nn;
static UiCaretRect g_caret_cur[UI_CARET_RECTS];
static int g_caret_nc;
static int g_caret_dirty;
static int g_caret_on = 1;

static void script_note(const char *msg);
static void run_flush(void);
static void run_drop(void);

/* RTX_UI_FRAME_LOG (ui_hook.h): frame stamps and the layouts made. */
static long g_layouts_made;
static double g_frame_t[3];
static long g_frame_lay0;

/* family filled on first use: ui_os_font_family(NULL). */
static UiFace g_default_face = {
    NULL, 16, uiTextWeightNormal, uiTextItalicNormal, 0, 0, 0, 0
};

static UiFace *face_of(Font font) {
    if (!font.ct) {
        if (!g_default_face.family)
            g_default_face.family = ui_os_font_family(NULL);
        return &g_default_face;
    }
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
    case uiExtKeyPageUp: return KEY_PAGE_UP;
    case uiExtKeyPageDown: return KEY_PAGE_DOWN;
    case uiExtKeyF1: return KEY_F1;
    case uiExtKeyF2: return KEY_F2;
    case uiExtKeyF3: return KEY_F3;
    case uiExtKeyF4: return KEY_F4;
    case uiExtKeyF5: return KEY_F5;
    case uiExtKeyF6: return KEY_F6;
    case uiExtKeyF7: return KEY_F7;
    case uiExtKeyF8: return KEY_F8;
    case uiExtKeyF9: return KEY_F9;
    case uiExtKeyF10: return KEY_F10;
    case uiExtKeyF11: return KEY_F11;
    case uiExtKeyF12: return KEY_F12;
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
    if (c == '/') return KEY_SLASH;
    if (c == ',') return KEY_COMMA;
    if (c == '-') return KEY_MINUS;
    if (c == ';') return KEY_SEMICOLON;
    if (c == '[') return KEY_LEFT_BRACKET;
    if (c == ']') return KEY_RIGHT_BRACKET;
    if (c == '\'') return KEY_APOSTROPHE;
    return -1;
}

static void push_typed_char(const uiAreaKeyEvent *e) {
    int buf[8];
    int n, i;
    if (!e || e->Up) return;
    if (e->ExtKey) return;
    if (e->Modifiers & (uiModifierCtrl | uiModifierSuper | uiModifierAlt))
        return;
    n = ui_os_typed_text(e, buf, 8);
    for (i = 0; i < n; i++) {
        int u = buf[i];
        /* U+F700.. are AppKit function-key private use (arrows, Home, …). */
        if (u >= 32 && u != 127 && !(u >= 0xF700 && u <= 0xF8FF) && g_nchar < 64)
            g_chars[g_nchar++] = u;
    }
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
    if (k >= 0 && k < UI_KEY_MAX) {
        if (e->Up) {
            g_key_down[k] = 0;
            return;
        }
        if (g_key_down[k]) g_key_repeat[k] = 1;
        else g_key_pressed[k] = 1;
        g_key_down[k] = 1;
        if (k == g_exit_key) g_should_close = 1;
    } else if (e->Up) {
        return;
    }
    /* Space, '*', '>', etc. are not raylib keys. Still queue the character
     * or the edit lands and g_dirty never sees it. */
    push_typed_char(e);
}

/* RTX_UI_LOG: one line per area Draw ("draw x y w h kind") for the
 * blink test's Draw count. */
static int draw_log_on(void) {
    const char *lp = getenv("RTX_UI_LOG");
    return lp && lp[0];
}

static void draw_log(const char *kind) {
    char msg[96];
    snprintf(msg, sizeof msg, "draw %d %d %d %d %s", (int)g_clip_r.x,
             (int)g_clip_r.y, (int)g_clip_r.w, (int)g_clip_r.h, kind);
    script_note(msg);
}

/* The whole Draw is done: keep the pixels under the new rects (a layer
 * that needs them) and move the layer once the Draw has returned. The
 * pixels are re-taken every whole Draw: the text under the caret may have
 * changed even where the caret did not move. */
static void caret_found(uiDrawContext *ctx) {
    ui_os_caret_capture(ctx, g_caret_new, g_caret_nn);
    g_caret_dirty = 1;
}

/* Clip inside one layer rect: the layer covers every pixel of it. */
static int caret_covers_clip(void) {
    int i;
    for (i = 0; i < g_caret_nc; i++) {
        const UiCaretRect *r = &g_caret_cur[i];
        if (g_clip_r.x >= r->x - 0.5 && g_clip_r.y >= r->y - 0.5 &&
            g_clip_r.x + g_clip_r.w <= r->x + r->w + 0.5 &&
            g_clip_r.y + g_clip_r.h <= r->y + r->h + 0.5)
            return 1;
    }
    return 0;
}

/* Outside any Draw (after a flush or a toolkit step). */
static void caret_apply(void) {
    if (!g_caret_dirty || !g_caret_ok) return;
    g_caret_dirty = 0;
    /* Same rects are still set: the pixels under them are new. */
    if (g_caret_nn)
        memcpy(g_caret_cur, g_caret_new, sizeof(UiCaretRect) * (size_t)g_caret_nn);
    g_caret_nc = g_caret_nn;
    ui_os_caret_set(g_caret_cur, g_caret_nc);
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
    g_clip_r.x = p->ClipX;
    g_clip_r.y = p->ClipY;
    g_clip_r.w = p->ClipWidth;
    g_clip_r.h = p->ClipHeight;
    /* A toolkit that reports no clip (0 x 0) means the whole area. */
    if (g_clip_r.w <= 0 || g_clip_r.h <= 0) {
        g_clip_r.x = g_clip_r.y = 0;
        g_clip_r.w = g_ww;
        g_clip_r.h = g_hh;
    }
    g_clip_whole = g_clip_r.x <= 0.5 && g_clip_r.y <= 0.5 &&
                   g_clip_r.x + g_clip_r.w >= (double)g_ww - 0.5 &&
                   g_clip_r.y + g_clip_r.h >= (double)g_hh - 0.5;
    g_clip_on = 1;
    /* A partial Draw under the caret layer only (a toolkit that composites
     * the layer by redrawing what is below it): the layer paints those
     * pixels itself (the caret, or what the last whole Draw left there). */
    if (!g_clip_whole && caret_covers_clip()) {
        if (draw_log_on()) draw_log("layer-only");
        g_clip_on = 0;
        g_ctx = NULL;
        return;
    }
    if (draw_log_on()) draw_log(g_clip_whole ? "whole" : "part");
    g_scis_n = 0;
    /* Only a whole-area Draw paints every caret: it alone re-notes them. */
    if (g_clip_whole) {
        g_blink_n = 0;
        g_blink_noting = 1;
        g_blink_valid = g_paint != NULL; /* the painter may forget */
        g_caret_nn = 0;
        g_caret_collect = g_caret_ok && g_paint != NULL;
    }
    /* A Draw is a turn wherever the toolkit runs it (a flush, the idle
     * wait, a nested dialog loop): scans pause for the paint only. */
    ui_turn(1);
    run_drop();
    if (g_paint) g_paint();
    run_flush(); /* before the caret layer reads the pixels */
    ui_turn(0);
    if (g_clip_whole) {
        g_blink_noting = 0;
        if (g_caret_collect) caret_found(p->Context);
        g_caret_collect = 0;
    }
    g_clip_on = 0;
    g_ctx = NULL;
}

int gui_clip_hit(double x, double y, double w, double h) {
    if (!g_clip_on || g_clip_whole) return 1;
    if (w < 0 || h < 0) return 0;
    return x < g_clip_r.x + g_clip_r.w && x + w > g_clip_r.x &&
           y < g_clip_r.y + g_clip_r.h && y + h > g_clip_r.y;
}

int gui_clip_full(void) { return !g_clip_on || g_clip_whole; }

void gui_blink_note(double x, double y, double w, double h) {
    int i;
    if (!g_blink_noting || w <= 0 || h <= 0) return;
    /* Clamp to the area; drop a rect that is off it. */
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > g_ww) w = g_ww - x;
    if (y + h > g_hh) h = g_hh - y;
    if (w <= 0 || h <= 0) return;
    for (i = 0; i < g_blink_n; i++) {
        UiRect *r = &g_blink_r[i];
        if (r->x == x && r->y == y && r->w == w && r->h == h) return;
    }
    if (g_blink_n >= UI_BLINK_RECTS) {
        /* More carets than slots (many-row box caret): whole area. */
        g_blink_valid = 0;
        g_blink_noting = 0;
        g_blink_n = UI_BLINK_RECTS + 1;
        return;
    }
    g_blink_r[g_blink_n].x = x;
    g_blink_r[g_blink_n].y = y;
    g_blink_r[g_blink_n].w = w;
    g_blink_r[g_blink_n].h = h;
    g_blink_n++;
}

void gui_blink_forget(void) { g_blink_valid = 0; }

int gui_blink_rects(void) { return g_blink_drawn; }

int gui_blink_paint(void) {
    int i, n;
    const char *lp = getenv("RTX_UI_LOG");
    int blink_log = lp && lp[0];
    g_blink_drawn = 0;
    if (!g_area || !g_paint || !g_blink_valid || g_blink_n > UI_BLINK_RECTS)
        return 0;
    n = g_blink_n;
    ui_turn(1);
    for (i = 0; i < n; i++) {
        UiRect r = g_blink_r[i];
        /* Whole pixels: the toolkit rounds the damage out anyway. */
        double x0 = (double)(long)r.x, y0 = (double)(long)r.y;
        double x1 = (double)(long)(r.x + r.w + 0.999);
        double y1 = (double)(long)(r.y + r.h + 0.999);
        ui_os_sync_paint_rect(x0, y0, x1 - x0, y1 - y0);
        g_blink_drawn++;
        if (blink_log) {
            char msg[96];
            snprintf(msg, sizeof msg, "blink rect %d %d %d %d", (int)x0,
                     (int)y0, (int)(x1 - x0), (int)(y1 - y0));
            script_note(msg);
        }
    }
    ui_turn(0);
    return 1;
}

void gui_caret_setup(int want, int fade) {
    g_caret_want = want ? 1 : 0;
    g_caret_fade = fade ? 1 : 0;
    g_caret_ok = g_caret_want && ui_os_caret_ok();
    if (!g_caret_ok && g_caret_nc) {
        g_caret_nc = 0;
        ui_os_caret_set(NULL, 0);
    }
}

int gui_caret_layer(void) { return g_caret_ok; }

void gui_caret_bar(double x, double y, double w, double h, Color c) {
    UiCaretRect *r;
    double x1, y1;
    if (!g_caret_collect || g_caret_nn >= UI_CARET_RECTS || w <= 0 || h <= 0)
        return;
    x1 = x + w;
    y1 = y + h;
    if (g_scis_n > 0) {
        const UiRect *s = &g_scis[(g_scis_n < UI_SCISSOR_MAX ? g_scis_n
                                                            : UI_SCISSOR_MAX) - 1];
        /* A bar the scissor's left or right edge cuts (the caret at a
         * row's first column sits one pixel left of the glyph) keeps its
         * width just inside: a sliver is hard to see, and GTK does not
         * show a 1 px client-side overlay window at all. */
        if (x < s->x && x1 > s->x) {
            x = s->x;
            x1 = x + w;
        } else if (x1 > s->x + s->w && x < s->x + s->w) {
            x1 = s->x + s->w;
            x = x1 - w;
        }
        if (x < s->x) x = s->x;
        if (y < s->y) y = s->y;
        if (x1 > s->x + s->w) x1 = s->x + s->w;
        if (y1 > s->y + s->h) y1 = s->y + s->h;
    }
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x1 > g_ww) x1 = g_ww;
    if (y1 > g_hh) y1 = g_hh;
    if (x1 - x < 0.5 || y1 - y < 0.5) return;
    r = &g_caret_new[g_caret_nn++];
    /* Whole pixels: the layer is a widget / view frame. */
    r->x = (double)(long)x;
    r->y = (double)(long)y;
    r->w = (double)(long)(x1 + 0.5) - r->x;
    r->h = (double)(long)(y1 + 0.5) - r->y;
    if (r->w < 1) r->w = 1;
    if (r->h < 1) r->h = 1;
    r->r = c.r;
    r->g = c.g;
    r->b = c.b;
    r->a = c.a;
}

void gui_caret_phase(int on) {
    on = on ? 1 : 0;
    if (!g_caret_ok || on == g_caret_on) return;
    g_caret_on = on;
    ui_os_caret_show(on, g_caret_fade ? UI_CARET_FADE_MS : 0);
}

int gui_caret_rect(double *x, double *y, double *w, double *h) {
    if (!g_caret_nc) return 0;
    if (x) *x = g_caret_cur[0].x;
    if (y) *y = g_caret_cur[0].y;
    if (w) *w = g_caret_cur[0].w;
    if (h) *h = g_caret_cur[0].h;
    return 1;
}

void ui_plat_focus(int in) {
    in = in ? 1 : 0;
    if (in == g_focused) return;
    g_focused = in;
    g_focus_edge = in;
}

int IsWindowFocused(void) { return g_focused; }

int gui_focus_edge(void) {
    int e = g_focus_edge;
    g_focus_edge = -1;
    return e;
}

static int script_open(void);

static void on_mouse(uiAreaHandler *h, uiArea *a, uiAreaMouseEvent *e) {
    (void)h;
    (void)a;
    if (!e) return;
    if (script_open()) return;
    g_mouse_x = (int)e->X;
    g_mouse_y = (int)e->Y;
    if (e->Down == 2) {
        g_mouse_mid_pressed = 1;
    } else if (e->Down == 1) {
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
    /* A scripted session injects its own edges. Real keydowns (the window
     * is key) would land in the document beside the script. */
    if (script_open()) return 1;
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

void ui_plat_text(const int *cps, int n) {
    int i;
    if (script_open() || !cps) return;
    for (i = 0; i < n && g_nchar < 64; i++) {
        if (cps[i] >= 32 && cps[i] != 127) g_chars[g_nchar++] = cps[i];
    }
}

void ui_plat_wheel(float dx, float dy) {
    if (script_open()) return;
    g_wheel_carry_x += dx;
    g_wheel_carry_y += dy;
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

/* libui has no set-text / hide, and newItem() aborts once uiNewWindow
 * has finalized the bar. Slots are made up front and retitled or hidden
 * through ui_os_menu_slot (gui_menu_set_apply) after the window exists. */
static void apply_slots(void) {
    int i;
    uiMenuAppendSeparator(g_apply_menu);
    for (i = 0; i < UI_APPLY_SLOTS; i++) {
        g_apply_item[i] = uiMenuAppendItem(g_apply_menu, "Apply");
        uiMenuItemOnClicked(g_apply_item[i], on_menu,
                            (void *)(intptr_t)(RTX_APPLY_TAG0 + i));
    }
}

/* One command-table row (core/cmd.cch): its title and CMD_* tag; a row
 * with its own `run` hook tags RTX_ROW_TAG0 + row. */
static void add_row(uiMenu *menu, int row, int first) {
    const RtxCmdDef *d = rtx_cmd_at((size_t)row);
    int tag;
    if (!d) return;
    if ((d->flags & RTX_CMDF_SEP) && !first) uiMenuAppendSeparator(menu);
    tag = d->run ? RTX_ROW_TAG0 + row : d->cmd;
    add_item(menu, d->title, tag);
    if ((d->flags & RTX_CMDF_SLOTS) && menu == g_apply_menu) apply_slots();
}

/* Menus in bar order; each lists the table rows naming it. A menu no row
 * names is not made (other changes may add "Workspace" / "Buffer" rows). */
static void menu_build(void) {
    static const char *const names[] = {
        "File", "Edit", "Apply", "View", "Go", "Workspace", "Buffer", NULL
    };
    int rows[RTX_CMD_MAX];
    int i;
    if (g_apply_menu) return;
#ifdef __APPLE__
    {
        /* First menu is the application menu on macOS. */
        uiMenu *app = uiNewMenu("cctext");
        add_item(app, "Quit cctext", CMD_QUIT);
    }
#endif
    for (i = 0; names[i]; i++) {
        size_t n = rtx_cmd_menu_rows(names[i], rows, RTX_CMD_MAX);
        size_t k;
        uiMenu *menu;
        if (!n && strcmp(names[i], "Apply") != 0) continue;
        menu = uiNewMenu(names[i]);
        if (strcmp(names[i], "Apply") == 0) g_apply_menu = menu;
        for (k = 0; k < n; k++) add_row(menu, rows[k], k == 0);
#ifndef __APPLE__
        /* macOS quits from the app menu. A "Close Window" item that quit
         * sat where Close Buffer (Cmd-W) belongs. */
        if (strcmp(names[i], "File") == 0) {
            uiMenuAppendSeparator(menu);
            add_item(menu, "Quit", CMD_QUIT);
        }
#endif
    }
    if (!g_apply_item[0] && g_apply_menu) apply_slots();
}

static int g_dlg = -1;
static int g_dlg_open;
static uiWindow *g_dlg_win;

/* RTX_UI_SCRIPT plays one line per frame into the same key edges the
 * area handler fills. macOS denies System Events keystrokes, so a
 * session check cannot use them. `dlg` is read by the modal loop. */
static FILE *g_script;
static int g_script_have;
static int g_script_wait;
static int g_script_super;
static int g_script_eof;
static char g_script_line[96];

static void script_note(const char *msg) {
    const char *path = getenv("RTX_UI_LOG");
    FILE *fp;
    if (!path || !path[0] || !msg) return;
    fp = fopen(path, "a");
    if (!fp) return;
    fprintf(fp, "%s\n", msg);
    fclose(fp);
}

static int script_open(void) {
    const char *path;
    if (g_script) return 1;
    path = getenv("RTX_UI_SCRIPT");
    if (!path || !path[0]) return 0;
    g_script = fopen(path, "r");
    return g_script != NULL;
}

static int script_pull(void) {
    if (g_script_have) return 1;
    if (!script_open()) return 0;
    while (fgets(g_script_line, sizeof g_script_line, g_script)) {
        char *s = g_script_line;
        size_t n;
        while (*s == ' ' || *s == '\t') s++;
        if (*s == 0 || *s == '#' || *s == '\n' || *s == '\r') continue;
        if (s != g_script_line) memmove(g_script_line, s, strlen(s) + 1);
        n = strlen(g_script_line);
        while (n && (g_script_line[n - 1] == '\n' || g_script_line[n - 1] == '\r'))
            g_script_line[--n] = 0;
        g_script_have = 1;
        return 1;
    }
    g_script_eof = 1;
    return 0;
}

/* A script with lines (or a wait) still to play. */
static int script_live(void) {
    if (!script_open()) return 0;
    return !g_script_eof || g_script_have || g_script_wait > 0;
}

static void script_arm_frame(void) {
    if (g_script_super) {
        g_key_down[KEY_LEFT_SUPER] = 0;
        if (g_script_super == 2) g_key_down[KEY_LEFT_SHIFT] = 0;
        g_script_super = 0;
    }
    if (g_script_wait > 0) {
        g_script_wait--;
        return;
    }
    if (!script_pull()) return;
    script_note(g_script_line);
    if (strncmp(g_script_line, "dlg", 3) == 0) return;
    if (strcmp(g_script_line, "enter") == 0) {
        g_key_pressed[KEY_ENTER] = 1;
        g_script_have = 0;
        return;
    }
    if (strncmp(g_script_line, "wait ", 5) == 0) {
        g_script_wait = atoi(g_script_line + 5);
        g_script_have = 0;
        if (g_script_wait > 0) g_script_wait--;
        return;
    }
    if (strncmp(g_script_line, "key ", 4) == 0 && g_script_line[4]) {
        int ch = (unsigned char)g_script_line[4];
        if (g_nchar < 64) g_chars[g_nchar++] = ch;
        g_script_have = 0;
        return;
    }
    if (strncmp(g_script_line, "wheel ", 6) == 0) {
        /* wheel <dy> [dx]: notches this frame, +dy = up. */
        char *end = NULL;
        g_wheel_y = (float)strtol(g_script_line + 6, &end, 10);
        g_wheel_x = end ? (float)strtol(end, NULL, 10) : 0.0f;
        g_script_have = 0;
        return;
    }
    if (strncmp(g_script_line, "esc", 3) == 0) {
        g_key_pressed[KEY_ESCAPE] = 1;
        g_script_have = 0;
        return;
    }
    if (strncmp(g_script_line, "scmd ", 5) == 0 && g_script_line[5]) {
        /* Cmd-Shift-<key> (e.g. `scmd f`: project search). */
        int ch = g_script_line[5];
        int k = (ch >= 'a' && ch <= 'z') ? ch - 'a' + 'A' : ch;
        g_key_down[KEY_LEFT_SUPER] = 1;
        g_key_down[KEY_LEFT_SHIFT] = 1;
        g_script_super = 2;
        if (k >= 0 && k < UI_KEY_MAX) g_key_pressed[k] = 1;
        g_script_have = 0;
        return;
    }
    if (strncmp(g_script_line, "cmd ", 4) == 0 && g_script_line[4]) {
        int ch = g_script_line[4];
        int k = (ch >= 'a' && ch <= 'z') ? ch - 'a' + 'A' : ch;
        g_key_down[KEY_LEFT_SUPER] = 1;
        g_script_super = 1;
        if (k >= 0 && k < UI_KEY_MAX) g_key_pressed[k] = 1;
        g_script_have = 0;
        return;
    }
    g_script_have = 0;
}

static void script_dialog(void) {
    if (g_script_wait > 0) {
        g_script_wait--;
        return;
    }
    if (!script_pull()) return;
    if (strncmp(g_script_line, "wait ", 5) == 0) {
        g_script_wait = atoi(g_script_line + 5);
        g_script_have = 0;
        return;
    }
    if (strncmp(g_script_line, "dlg", 3) == 0) {
        g_dlg = atoi(g_script_line + 3);
        g_script_have = 0;
        script_note(g_script_line);
    }
}

static int dlg_closing(uiWindow *w, void *data) {
    (void)w;
    (void)data;
    if (g_dlg < 0) g_dlg = 0;
    g_dlg_open = 0;
    return 1;
}

/* uiMainStep(1) blocks until an event. A scripted dialog answers from
 * the loop, so wake it while the dialog waits; the timer ends with it. */
static int dlg_wake(void *data) {
    (void)data;
    return g_dlg_open && g_dlg < 0;
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
    ui_turn(1);
    uiControlShow(uiControl(w));
    script_note(title ? title : "dialog");
    if (script_open()) uiTimer(16, dlg_wake, NULL);
    while (g_dlg < 0) {
        script_dialog();
        if (g_dlg >= 0) break;
        uiMainStep(1);
    }
    ui_turn(0);
    if (g_dlg_open) {
        g_dlg_open = 0;
        uiControlDestroy(uiControl(w));
    }
    g_dlg_win = NULL;
    return g_dlg;
}

static UiFace *face_new(const char *family, float size, int weight, int italic) {
    UiFace *f = (UiFace *)calloc(1, sizeof *f);
    if (!f) return face_of((Font){0});
    f->family = family ? family : ui_os_font_family(NULL);
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
    desc.Size = (sz > 0 ? sz : face->size) * ui_os_pt_per_px();
    desc.Weight = face->weight > 0 ? face->weight : uiTextWeightNormal;
    desc.Italic = face->italic ? uiTextItalicItalic : uiTextItalicNormal;
    desc.Stretch = uiTextStretchNormal;
    memset(&tp, 0, sizeof tp);
    tp.String = s;
    tp.DefaultFont = &desc;
    tp.Width = 1.0e7;
    tp.Align = uiDrawTextAlignLeft;
    tl = uiDrawNewTextLayout(&tp);
    g_layouts_made++;
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

/*
 * Text measure without a layout per call. The painter and the layout
 * measure one grapheme cluster at a time (sum of cluster widths is the
 * row's geometry, and each cluster is drawn at that x), so a cluster's
 * width is a pure function of (face, size, bytes):
 *   - a fixed-pitch face (mono_of): printable ASCII is n × advance;
 *   - anything else short: one layout the first time, then a hash hit.
 * Every width is the value text_width returned before, so caret, hit
 * testing and selection bounds do not move.
 */
static UiMono *mono_of(UiFace *face, float sz) {
    static const char *const probe[] = { "i", "W", "0", "m", " " };
    UiMono *m;
    double w0 = 0, h0 = 0;
    int i, same = 1;
    if (!face) return NULL;
    for (i = 0; i < UI_MONO_SIZES; i++) {
        if (face->mono[i].kind && face->mono[i].sz == sz) return &face->mono[i];
    }
    m = &face->mono[face->mono_next];
    face->mono_next = (face->mono_next + 1) % UI_MONO_SIZES;
    for (i = 0; i < 5; i++) {
        double h = 0;
        double w = text_width(face, sz, probe[i], &h);
        if (i == 0) {
            w0 = w;
            h0 = h;
        } else if (w != w0) {
            same = 0;
        }
    }
    m->sz = sz;
    m->kind = (same && w0 > 0) ? 1 : 2;
    m->adv = w0;
    m->h = h0;
    return m;
}

/* Printable ASCII only (no tab / control): n cells in a fixed-pitch face. */
static int ascii_plain(const char *s, size_t *n_out) {
    size_t n = 0;
    while (s[n]) {
        unsigned char c = (unsigned char)s[n];
        if (c < 0x20 || c > 0x7e) return 0;
        n++;
    }
    *n_out = n;
    return 1;
}

enum { MC_SLOTS = 8192, MC_MAX = 6144, MC_KEY = 23 };

typedef struct MeasEnt {
    const UiFace *face;
    float sz;
    float w, h;
    unsigned char n; /* 0 = empty slot */
    char s[MC_KEY];
} MeasEnt;

static MeasEnt *g_mc;
static int g_mc_n;

static uint32_t hash_bytes(uint32_t h, const void *p, size_t n) {
    const unsigned char *b = (const unsigned char *)p;
    size_t i;
    for (i = 0; i < n; i++) h = (h ^ b[i]) * 16777619u;
    return h;
}

static void measure_cache_clear(void) {
    if (g_mc) memset(g_mc, 0, sizeof(MeasEnt) * MC_SLOTS);
    g_mc_n = 0;
}

static double measure_text(UiFace *face, float sz, const char *text, double *out_h) {
    size_t n = 0;
    UiMono *mono;
    uint32_t hv;
    size_t at;
    double w, h = 0;
    if (!text || !text[0]) {
        if (out_h) *out_h = 0;
        return 0;
    }
    mono = mono_of(face, sz);
    if (mono && mono->kind == 1 && ascii_plain(text, &n)) {
        if (out_h) *out_h = mono->h;
        return (double)n * mono->adv;
    }
    n = strlen(text);
    if (n > MC_KEY) return text_width(face, sz, text, out_h);
    if (!g_mc) {
        g_mc = (MeasEnt *)calloc(MC_SLOTS, sizeof(MeasEnt));
        if (!g_mc) return text_width(face, sz, text, out_h);
    }
    hv = hash_bytes(2166136261u, &face, sizeof face);
    hv = hash_bytes(hv, &sz, sizeof sz);
    hv = hash_bytes(hv, text, n);
    at = hv & (MC_SLOTS - 1);
    for (;;) {
        MeasEnt *e = &g_mc[at];
        if (!e->n) break;
        if (e->n == n && e->face == face && e->sz == sz &&
            memcmp(e->s, text, n) == 0) {
            if (out_h) *out_h = e->h;
            return e->w;
        }
        at = (at + 1) & (MC_SLOTS - 1);
    }
    w = text_width(face, sz, text, &h);
    if (g_mc_n >= MC_MAX) {
        measure_cache_clear();
        at = hv & (MC_SLOTS - 1);
    }
    {
        MeasEnt *e = &g_mc[at];
        e->face = face;
        e->sz = sz;
        e->w = (float)w;
        e->h = (float)h;
        e->n = (unsigned char)n;
        memcpy(e->s, text, n);
        g_mc_n++;
        /* The caller sees the float the cache keeps (MeasureTextEx is
         * float): a hit and a miss return the same value. */
        w = e->w;
        h = e->h;
    }
    if (out_h) *out_h = h;
    return w;
}

static void fill_rect(double x, double y, double w, double h, Color c) {
    uiDrawPath *path;
    uiDrawBrush b;
    run_flush();
    if (!g_ctx || w <= 0 || h <= 0) return;
    if (!gui_clip_hit(x, y, w, h)) return;
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
    run_flush();
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
    run_flush();
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

/*
 * Text paint. The painter draws one grapheme cluster per DrawTextEx at the
 * x the cluster widths add up to. Building a layout for each (a PangoLayout
 * on GTK) was most of a frame, so:
 *   - Batch: single printable-ASCII clusters of one face / size / baseline
 *     whose x continues the previous one (x_end = x + its measured width)
 *     join one run, coloured by spans. The run is drawn with kerning and
 *     ligatures off, so each glyph lands where the per-cluster sum put it
 *     (the old per-cluster draw had no kerning either). Any other paint
 *     (rect, line, scissor, a non-ASCII cluster, the end of the Draw)
 *     flushes the run first: paint order is unchanged.
 *   - Cache: a run's layout is kept by (face, size, text, spans, features)
 *     and reused while the row is unchanged. Bounded; cleared when full and
 *     by UnloadFont. A theme change keys new colours; old entries age out.
 */
enum { RUN_CAP = 1024, RUN_SPANS = 128, LC_SLOTS = 4096, LC_MAX = 2048,
       LC_KEY_MAX = 8192 };

typedef struct RunSpan {
    unsigned short at, n;
    Color c;
} RunSpan;

static struct {
    UiFace *face;
    float sz;
    double x, y, x_end;
    char s[RUN_CAP + 1];
    size_t n;
    RunSpan sp[RUN_SPANS];
    int nsp;
} g_run;

typedef struct LayEnt {
    uint32_t hash;
    size_t klen;
    unsigned char *key;
    uiDrawTextLayout *tl;
} LayEnt;

static LayEnt *g_lc;
static int g_lc_n;

static void layout_cache_clear(void) {
    int i;
    if (!g_lc) return;
    for (i = 0; i < LC_SLOTS; i++) {
        if (g_lc[i].tl) uiDrawFreeTextLayout(g_lc[i].tl);
        free(g_lc[i].key);
    }
    memset(g_lc, 0, sizeof(LayEnt) * LC_SLOTS);
    g_lc_n = 0;
}

static uiDrawTextLayout *layout_new(UiFace *face, float fontSize, const char *text,
                                    size_t n, const RunSpan *sp, int nsp,
                                    int plain) {
    uiAttributedString *s;
    uiFontDescriptor desc;
    uiDrawTextLayoutParams tp;
    uiDrawTextLayout *tl;
    char *tmp;
    int i;
    tmp = (char *)malloc(n + 1);
    if (!tmp) return NULL;
    memcpy(tmp, text, n);
    tmp[n] = 0;
    s = uiNewAttributedString(tmp);
    free(tmp);
    if (!s) return NULL;
    for (i = 0; i < nsp; i++) {
        uiAttribute *color = uiNewColorAttribute(sp[i].c.r / 255.0, sp[i].c.g / 255.0,
                                                 sp[i].c.b / 255.0, sp[i].c.a / 255.0);
        uiAttributedStringSetAttribute(s, color, sp[i].at, (size_t)sp[i].at + sp[i].n);
    }
    if (plain && n > 1) {
        /* Glyph i at the sum of the advances before it. */
        uiOpenTypeFeatures *otf = uiNewOpenTypeFeatures();
        if (otf) {
            uiOpenTypeFeaturesAdd(otf, 'k', 'e', 'r', 'n', 0);
            uiOpenTypeFeaturesAdd(otf, 'l', 'i', 'g', 'a', 0);
            uiOpenTypeFeaturesAdd(otf, 'c', 'l', 'i', 'g', 0);
            uiOpenTypeFeaturesAdd(otf, 'c', 'a', 'l', 't', 0);
            uiAttributedStringSetAttribute(s, uiNewFeaturesAttribute(otf), 0, n);
            uiFreeOpenTypeFeatures(otf);
        }
    }
    memset(&desc, 0, sizeof desc);
    desc.Family = (char *)face->family;
    desc.Size = (fontSize > 0 ? fontSize : face->size) * ui_os_pt_per_px();
    desc.Weight = face->weight > 0 ? face->weight : uiTextWeightNormal;
    desc.Italic = face->italic ? uiTextItalicItalic : uiTextItalicNormal;
    desc.Stretch = uiTextStretchNormal;
    memset(&tp, 0, sizeof tp);
    tp.String = s;
    tp.DefaultFont = &desc;
    tp.Width = 1.0e7;
    tp.Align = uiDrawTextAlignLeft;
    tl = uiDrawNewTextLayout(&tp);
    g_layouts_made++;
    uiFreeAttributedString(s);
    return tl;
}

/* The cached layout for this run, or a new one; *own = 1 when the caller
 * must free it (no cache slot: key too long, out of memory). */
static uiDrawTextLayout *layout_get(UiFace *face, float fontSize, const char *text,
                                    size_t n, const RunSpan *sp, int nsp,
                                    int plain, int *own) {
    unsigned char kbuf[512];
    unsigned char *key = kbuf;
    size_t klen, o = 0;
    uint32_t hv;
    size_t at;
    uiDrawTextLayout *tl;
    *own = 0;
    klen = sizeof face + sizeof fontSize + 2 * sizeof(int) +
           (size_t)nsp * sizeof(RunSpan) + n;
    if (klen > LC_KEY_MAX) {
        *own = 1;
        return layout_new(face, fontSize, text, n, sp, nsp, plain);
    }
    if (klen > sizeof kbuf) {
        key = (unsigned char *)malloc(klen);
        if (!key) {
            *own = 1;
            return layout_new(face, fontSize, text, n, sp, nsp, plain);
        }
    }
    memcpy(key + o, &face, sizeof face);
    o += sizeof face;
    memcpy(key + o, &fontSize, sizeof fontSize);
    o += sizeof fontSize;
    memcpy(key + o, &plain, sizeof plain);
    o += sizeof plain;
    memcpy(key + o, &nsp, sizeof nsp);
    o += sizeof nsp;
    memcpy(key + o, sp, (size_t)nsp * sizeof(RunSpan));
    o += (size_t)nsp * sizeof(RunSpan);
    memcpy(key + o, text, n);
    hv = hash_bytes(2166136261u, key, klen);
    if (!g_lc) g_lc = (LayEnt *)calloc(LC_SLOTS, sizeof(LayEnt));
    if (!g_lc) {
        if (key != kbuf) free(key);
        *own = 1;
        return layout_new(face, fontSize, text, n, sp, nsp, plain);
    }
    at = hv & (LC_SLOTS - 1);
    while (g_lc[at].key) {
        LayEnt *e = &g_lc[at];
        if (e->hash == hv && e->klen == klen && memcmp(e->key, key, klen) == 0) {
            if (key != kbuf) free(key);
            return e->tl;
        }
        at = (at + 1) & (LC_SLOTS - 1);
    }
    tl = layout_new(face, fontSize, text, n, sp, nsp, plain);
    if (!tl) {
        if (key != kbuf) free(key);
        return NULL;
    }
    if (g_lc_n >= LC_MAX) {
        layout_cache_clear();
        at = hv & (LC_SLOTS - 1);
    }
    if (key == kbuf) {
        key = (unsigned char *)malloc(klen);
        if (!key) {
            *own = 1;
            return tl;
        }
        memcpy(key, kbuf, klen);
    }
    g_lc[at].hash = hv;
    g_lc[at].klen = klen;
    g_lc[at].key = key;
    g_lc[at].tl = tl;
    g_lc_n++;
    return tl;
}

static void draw_run(UiFace *face, float fontSize, const char *text, size_t n,
                     double x, double y, const RunSpan *sp, int nsp, int plain) {
    uiDrawTextLayout *tl;
    int own = 0;
    if (!g_ctx || !n) return;
    /* A line of text sits in [y, y + ~1.3 size) and runs right from x. Skip
     * the layout when a partial Draw's clip cannot meet it. */
    if (g_clip_on && !g_clip_whole) {
        double sz = (fontSize > 0 ? fontSize : face->size);
        if (!gui_clip_hit(x, y - sz * 0.5, 1.0e7, sz * 2.5)) return;
    }
    tl = layout_get(face, fontSize, text, n, sp, nsp, plain, &own);
    if (!tl) return;
    uiDrawText(g_ctx, tl, x, y);
    if (own) uiDrawFreeTextLayout(tl);
}

static void run_drop(void) {
    g_run.n = 0;
    g_run.nsp = 0;
}

static void run_flush(void) {
    if (!g_run.n) return;
    g_run.s[g_run.n] = 0;
    draw_run(g_run.face, g_run.sz, g_run.s, g_run.n, g_run.x, g_run.y, g_run.sp,
             g_run.nsp, 1);
    g_run.n = 0;
    g_run.nsp = 0;
}

/* One printable ASCII byte: batched. The Cocoa build batches only a
 * fixed-pitch face: whether a Core Text line honours 'kern' off has not
 * been checked there, and a proportional face's pairs would shift glyphs
 * off the per-cluster x. */
static int run_batchable(UiFace *face, float fontSize, const char *text) {
    unsigned char c = (unsigned char)text[0];
    if (c < 0x20 || c > 0x7e || text[1]) return 0;
#if defined(__APPLE__)
    {
        UiMono *m = mono_of(face, fontSize);
        if (!m || m->kind != 1) return 0;
    }
#else
    (void)face;
    (void)fontSize;
#endif
    return 1;
}

static int same_color(Color a, Color b) {
    return a.r == b.r && a.g == b.g && a.b == b.b && a.a == b.a;
}

static void draw_text(Font font, const char *text, double x, double y,
                      float fontSize, Color tint) {
    UiFace *face;
    if (!g_ctx || !text || !text[0]) return;
    face = face_of(font);
    if (run_batchable(face, fontSize, text)) {
        double w = measure_text(face, fontSize, text, NULL);
        double dx = x - g_run.x_end;
        if (g_run.n && (g_run.face != face || g_run.sz != fontSize || g_run.y != y ||
                        dx > 0.01 || dx < -0.01 || g_run.n >= RUN_CAP))
            run_flush();
        if (g_run.n && !same_color(g_run.sp[g_run.nsp - 1].c, tint) &&
            g_run.nsp >= RUN_SPANS)
            run_flush();
        if (!g_run.n) {
            g_run.face = face;
            g_run.sz = fontSize;
            g_run.x = x;
            g_run.y = y;
            g_run.nsp = 0;
        }
        if (g_run.nsp && same_color(g_run.sp[g_run.nsp - 1].c, tint)) {
            g_run.sp[g_run.nsp - 1].n++;
        } else {
            g_run.sp[g_run.nsp].at = (unsigned short)g_run.n;
            g_run.sp[g_run.nsp].n = 1;
            g_run.sp[g_run.nsp].c = tint;
            g_run.nsp++;
        }
        g_run.s[g_run.n++] = text[0];
        g_run.x_end = x + w;
        return;
    }
    run_flush();
    {
        RunSpan one;
        size_t n = strlen(text);
        if (n > 0xffff) n = 0xffff;
        one.at = 0;
        one.n = (unsigned short)n;
        one.c = tint;
        draw_run(face, fontSize, text, n, x, y, &one, 1, 0);
    }
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
    g_mouse_mid_pressed = 0;
    g_mouse_released = 0;
    g_wheel_x = g_wheel_y = 0;
    g_nchar = g_char_rd = 0;
    g_resized = 0;
}

/* libui's uiMainStep(1) blocks with no deadline, and on AppKit skips
 * updateWindows when the queue is empty. The shim steps with a deadline
 * and paints. Not a turn: Draw brackets itself (on_draw), so dest-live
 * scans keep publishing while the host sleeps. */
static void step_for(double wait_s) {
    /* RTX_UI_SCRIPT plays one line per frame: keep frames coming until
     * the script is spent, the way the old 16 ms wake timer did. */
    if ((wait_s < 0 || wait_s > 0.016) && script_live()) wait_s = 0.016;
    ui_os_step(wait_s);
    caret_apply(); /* a Draw inside the step (expose, resize) */
}

static double now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1.0e6 + (double)ts.tv_nsec / 1.0e3;
}

void ui_frame_mark(int at) {
    if (at < 0 || at > 2) return;
    g_frame_t[at] = now_us();
    if (at == 0) g_frame_lay0 = g_layouts_made;
}

void ui_frame_end(int saw) {
    static const char *path;
    static int init;
    FILE *fp;
    if (!init) {
        path = getenv("RTX_UI_FRAME_LOG");
        init = 1;
    }
    if (!saw || !path || !path[0]) return;
    fp = fopen(path, "a");
    if (!fp) return;
    fprintf(fp, "frame %.0f %.0f %.0f %ld\n", g_frame_t[2] - g_frame_t[0],
            g_frame_t[1] - g_frame_t[0], g_frame_t[2] - g_frame_t[1],
            g_layouts_made - g_frame_lay0);
    fclose(fp);
}

void ui_set_paint(void (*fn)(void)) { g_paint = fn; }

void ui_set_turn(void (*fn)(int enter)) { g_turn_state.fn = fn; }

void ui_turn(int enter) { rtx_ui_turn(&g_turn_state, enter); }

void ui_sync_paint(void) {
    if (!g_area || !g_paint) return;
    ui_turn(1);
    ui_os_sync_paint();
    caret_apply();
    ui_turn(0);
}

int ui_chars_waiting(void) {
    return g_char_rd < g_nchar;
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
    ui_os_init(g_window, g_area);
    gui_menu_set_apply(NULL, 0);
    g_ready = 1;
    g_should_close = 0;
    (void)g_flags;
    (void)g_fps;
}

void CloseWindow(void) {
    ui_os_fini();
    /* The cached layouts hold toolkit objects: free them before uiUninit. */
    run_drop();
    layout_cache_clear();
    free(g_lc);
    g_lc = NULL;
    free(g_mc);
    g_mc = NULL;
    g_mc_n = 0;
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
    step_for(0);
    wheel_commit();
    script_arm_frame();
}

void gui_input_consume(void) { clear_edges(); }

void PollInputEvents(void) { step_for(0); }

void WaitTime(double seconds) {
    step_for(seconds < 0 ? -1.0 : seconds);
}

void ClearBackground(Color color) {
    fill_rect(0, 0, g_ww, g_hh, color);
}

void BeginScissorMode(int x, int y, int w, int h) {
    uiDrawPath *path;
    run_flush();
    if (!g_ctx || w <= 0 || h <= 0) return;
    uiDrawSave(g_ctx);
    path = uiDrawNewPath(uiDrawFillModeWinding);
    uiDrawPathAddRectangle(path, x, y, w, h);
    uiDrawPathEnd(path);
    uiDrawClip(g_ctx, path);
    uiDrawFreePath(path);
    if (g_scis_n < UI_SCISSOR_MAX) {
        UiRect r = {x, y, w, h};
        if (g_scis_n > 0) {
            /* Nested: the intersection, as cairo / CG clip it (the stack
             * never exceeds its size here: g_scis_n < UI_SCISSOR_MAX). */
            const UiRect *o = &g_scis[g_scis_n - 1];
            double x1 = r.x + r.w, y1 = r.y + r.h;
            if (r.x < o->x) r.x = o->x;
            if (r.y < o->y) r.y = o->y;
            if (x1 > o->x + o->w) x1 = o->x + o->w;
            if (y1 > o->y + o->h) y1 = o->y + o->h;
            r.w = x1 > r.x ? x1 - r.x : 0;
            r.h = y1 > r.y ? y1 - r.y : 0;
        }
        g_scis[g_scis_n] = r;
    }
    g_scis_n++;
}

void EndScissorMode(void) {
    run_flush();
    if (!g_ctx) return;
    uiDrawRestore(g_ctx);
    if (g_scis_n > 0) g_scis_n--;
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
    out.x = (float)measure_text(face_of(font), fontSize, text, &h);
    out.y = (float)h;
    if (out.y <= 0) out.y = fontSize > 0 ? fontSize : 16;
    return out;
}

Font LoadFontEx(const char *fileName, int fontSize, int *codepoints, int codepointCount) {
    (void)codepoints;
    (void)codepointCount;
    return face_font(face_new(ui_os_font_family(fileName),
                              (float)(fontSize > 0 ? fontSize : 16),
                              uiTextWeightNormal, uiTextItalicNormal));
}

Font GetFontDefault(void) { return face_font(face_of((Font){0})); }

void UnloadFont(Font font) {
    UiFace *f = (UiFace *)font.ct;
    if (!f || f == &g_default_face || !f->heap) return;
    /* Caches are keyed by the face pointer: a new face may reuse it. */
    measure_cache_clear();
    layout_cache_clear();
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
    if (!fileName || !fileName[0]) return 0;
    /* A family name for the toolkit's font matcher, not a file. */
    if (strncmp(fileName, "family:", 7) == 0) return fileName[7] != 0;
    return access(fileName, F_OK) == 0;
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
    if (button == MOUSE_BUTTON_MIDDLE) return g_mouse_mid_pressed;
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
    ui_os_clip_set(text ? text : "");
}

/* Valid until the next call (raylib contract). The toolkit may run a
 * nested loop while it fetches (GTK), so this is an OS turn. */
const char *GetClipboardText(void) {
    char *u;
    ui_turn(1);
    u = ui_os_clip_get();
    ui_turn(0);
    free(g_clip);
    g_clip = u;
    return g_clip ? g_clip : "";
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
    if (!g_apply_menu || !g_window) return;
    if (n > UI_APPLY_SLOTS) n = UI_APPLY_SLOTS;
    for (i = 0; i < UI_APPLY_SLOTS; i++) {
        const char *t = (names && i < n) ? names[i] : NULL;
        if (!g_apply_item[i]) continue;
        ui_os_menu_slot(g_apply_item[i], (int)i, t && t[0] ? t : NULL);
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

int gui_alert_close(const char *what, size_t nfiles) {
    char msg[400];
    if (nfiles > 1)
        snprintf(msg, sizeof msg, "%zu files have unsaved edits. Save before closing?",
                 nfiles);
    else
        snprintf(msg, sizeof msg, "%s has unsaved edits. Save before closing?",
                 what && what[0] ? what : "This file");
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

int gui_alert_xact(void) {
    return dlg_choice("Undo across files",
                      "This edit also changed other files, and some of them "
                      "have changed since. Undo it in every file (undoing "
                      "their later edits too), or only in this file?",
                      "All Files", 1, "This File", 2, "Cancel", 0);
}

int gui_save_panel(const char *dir, char *out, size_t n) {
    char *p;
    (void)dir;
    if (!out || n == 0 || !g_window) return 0;
    out[0] = 0;
    ui_turn(1);
    p = uiSaveFile(g_window);
    ui_turn(0);
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
    ui_turn(1);
    p = uiOpenFile(g_window);
    ui_turn(0);
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
