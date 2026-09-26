/*
 * cctext-ui shim for GTK 3 (Linux / BSD). See ui_os.h.
 *
 * uiControlHandle(area) is libui's GtkDrawingArea subclass. Two signal
 * handlers on it fill what libui does not report:
 *   - key-press / key-release (run before libui's class handler): a
 *     GtkIMContextSimple turns the key into text (layout, shift, dead
 *     keys, compose); libui then delivers the scancode key and
 *     ui_plat.c asks for that text. A key libui does not map (no
 *     scancode entry) is handed over directly (ui_plat_text).
 *   - scroll-event: wheel notches (smooth deltas where the device has
 *     them).
 *   - focus-in / focus-out on the toplevel: the caret stops blinking
 *     while the window is not focused.
 * Clipboard is GtkClipboard (CLIPBOARD selection). The Apply submenu is
 * found in the window's GtkMenuBar by label and position.
 *
 * Caret layer: the area is moved into a GtkOverlay whose overlay child (a
 * GtkDrawingArea, input pass-through, never focused) is the caret. GTK 3
 * keeps no retained pixels for the area: anything that exposes the area
 * under the layer redraws it, so the layer paints both phases itself —
 * the caret color on, and off the pixels the last whole area Draw left
 * under it (copied from that Draw's cairo target). A blink queues a draw
 * of the layer; GTK then runs the area's Draw for the layer's rectangle
 * first, and ui_plat returns from that Draw at once (its clip is inside
 * the layer), so a blink paints no text.
 *
 * Not a native child window (gdk_window_ensure_native): that would skip
 * the area Draw too, but GDK then clips the area's own paint by the child
 * (a moved frame over its old one keeps stale pixels) and places the X
 * window a move late when the frame is resized (hex: two carets, one
 * frame) — both seen under Xvfb, GTK 3.24.
 */
#include <gtk/gtk.h>
#include "ui_os.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

/* libui unix/draw.h: the Draw context is a cairo_t (first field). */
struct uiDrawContext {
    cairo_t *cr;
    GtkStyleContext *style;
};

enum { UI_OS_TEXT_CAP = 8 };

static uiWindow *g_win;
static uiArea *g_area;
static GtkWidget *g_area_w;
static GtkIMContext *g_im;
static gulong g_sig_press, g_sig_release, g_sig_after, g_sig_scroll;
static gulong g_sig_commit;
static GtkWidget *g_top_w;
static gulong g_sig_focus_in, g_sig_focus_out;

/* Caret layer. */
enum { UI_OS_CARETS = 8 };
static GtkWidget *g_holder;  /* libui's child holder (the area's parent) */
static GtkWidget *g_overlay; /* GtkOverlay: the area, then the caret */
static GtkWidget *g_caret_w; /* the caret layer */
static UiCaretRect g_cr[UI_OS_CARETS];
static int g_crn;
static double g_bx, g_by, g_bw, g_bh; /* layer frame (area coordinates) */
static cairo_surface_t *g_under;      /* pixels under the frame, off phase */
static cairo_surface_t *g_under_next; /* from the whole Draw just done */
static double g_nbx, g_nby, g_nbw, g_nbh;
static double g_alpha = 1.0;          /* caret opacity now */
static double g_alpha_from, g_alpha_to = 1.0;
static gint64 g_fade_t0;
static int g_fade_ms;
static guint g_fade_id;

/* Text the IM produced for the key being dispatched now. */
static int g_text[UI_OS_TEXT_CAP];
static int g_ntext;
static int g_in_key;
static int g_im_ate;

static GtkWidget *area_widget(void) {
    if (!g_area) return NULL;
    return GTK_WIDGET(uiControlHandle(uiControl(g_area)));
}

static void on_commit(GtkIMContext *im, const gchar *str, gpointer data) {
    const gchar *p;
    (void)im;
    (void)data;
    if (!str) return;
    for (p = str; *p && g_ntext < UI_OS_TEXT_CAP; p = g_utf8_next_char(p)) {
        gunichar c = g_utf8_get_char_validated(p, -1);
        if (c == (gunichar)-1 || c == (gunichar)-2) break;
        g_text[g_ntext++] = (int)c;
    }
}

/* Runs before libui's areaWidget key handler (key-press-event is
 * RUN_LAST; this is a normal connect). Never stops the event: libui must
 * still see the key for its edges. */
static gboolean on_key_pre(GtkWidget *w, GdkEventKey *e, gpointer data) {
    (void)w;
    (void)data;
    if (!e) return FALSE;
    if (e->type == GDK_KEY_RELEASE) {
        if (g_im) gtk_im_context_filter_keypress(g_im, e);
        return FALSE;
    }
    g_ntext = 0;
    g_in_key = 1;
    g_im_ate = 0;
    if (g_im && !(e->state & (GDK_CONTROL_MASK | GDK_MOD1_MASK |
                              GDK_SUPER_MASK | GDK_META_MASK)))
        g_im_ate = gtk_im_context_filter_keypress(g_im, e) ? 1 : 0;
    if (!g_im_ate && g_ntext == 0) {
        gunichar c = gdk_keyval_to_unicode(e->keyval);
        if (c >= 32 && c != 127) g_text[g_ntext++] = (int)c;
    }
    return FALSE;
}

/* Runs only when libui did not take the key (its handler stops the
 * emission when it does): a keycode outside libui's scancode table, such
 * as a remapped or non-US key. Its text still goes in. */
static gboolean on_key_post(GtkWidget *w, GdkEventKey *e, gpointer data) {
    (void)w;
    (void)data;
    if (g_in_key && g_ntext > 0 && e &&
        !(e->state & (GDK_CONTROL_MASK | GDK_MOD1_MASK | GDK_SUPER_MASK |
                      GDK_META_MASK)))
        ui_plat_text(g_text, g_ntext);
    g_in_key = 0;
    g_ntext = 0;
    return FALSE;
}

static gboolean on_scroll(GtkWidget *w, GdkEventScroll *e, gpointer data) {
    gdouble dx = 0, dy = 0;
    (void)w;
    (void)data;
    if (!e) return FALSE;
    switch (e->direction) {
    case GDK_SCROLL_UP: ui_plat_wheel(0, 1); break;
    case GDK_SCROLL_DOWN: ui_plat_wheel(0, -1); break;
    case GDK_SCROLL_LEFT: ui_plat_wheel(-1, 0); break;
    case GDK_SCROLL_RIGHT: ui_plat_wheel(1, 0); break;
    case GDK_SCROLL_SMOOTH:
        /* One notch is 1.0; +y is toward the user (down). */
        if (gdk_event_get_scroll_deltas((GdkEvent *)e, &dx, &dy))
            ui_plat_wheel((float)dx, (float)-dy);
        break;
    default: break;
    }
    return TRUE;
}

static gboolean on_focus(GtkWidget *w, GdkEventFocus *e, gpointer data) {
    (void)w;
    (void)e;
    ui_plat_focus(data != NULL);
    return FALSE;
}

static gboolean caret_draw(GtkWidget *w, cairo_t *cr, gpointer data) {
    int i;
    (void)w;
    (void)data;
    if (g_under) {
        cairo_save(cr);
        cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
        cairo_set_source_surface(cr, g_under, 0, 0);
        cairo_paint(cr);
        cairo_restore(cr);
    }
    if (g_alpha <= 0.0) return TRUE;
    for (i = 0; i < g_crn; i++) {
        const UiCaretRect *r = &g_cr[i];
        cairo_set_source_rgba(cr, r->r / 255.0, r->g / 255.0, r->b / 255.0,
                              (r->a / 255.0) * g_alpha);
        cairo_rectangle(cr, r->x - g_bx, r->y - g_by, r->w, r->h);
        cairo_fill(cr);
    }
    return TRUE;
}

/* Queue the layer's redraw for the next frame-clock tick. Forcing it
 * here (gdk_window_process_updates) ran a nested synchronous Draw of the
 * layer and of the area under it inside every keystroke's frame. */
static void caret_flush(void) {
    if (!g_caret_w || !gtk_widget_get_visible(g_caret_w)) return;
    gtk_widget_queue_draw(g_caret_w);
}

/* Move libui's area into an overlay with the caret layer above it. */
static void caret_layer_init(void) {
    GtkWidget *parent = gtk_widget_get_parent(g_area_w);
    if (!parent || !GTK_IS_CONTAINER(parent)) return;
    g_object_ref(g_area_w);
    gtk_container_remove(GTK_CONTAINER(parent), g_area_w);
    g_overlay = gtk_overlay_new();
    gtk_widget_set_hexpand(g_overlay, TRUE);
    gtk_widget_set_vexpand(g_overlay, TRUE);
    gtk_widget_set_halign(g_overlay, GTK_ALIGN_FILL);
    gtk_widget_set_valign(g_overlay, GTK_ALIGN_FILL);
    gtk_container_add(GTK_CONTAINER(g_overlay), g_area_w);
    g_object_unref(g_area_w);
    g_caret_w = gtk_drawing_area_new();
    gtk_widget_set_halign(g_caret_w, GTK_ALIGN_START);
    gtk_widget_set_valign(g_caret_w, GTK_ALIGN_START);
    gtk_widget_set_can_focus(g_caret_w, FALSE);
    gtk_widget_set_no_show_all(g_caret_w, TRUE);
    g_signal_connect(g_caret_w, "draw", G_CALLBACK(caret_draw), NULL);
    gtk_overlay_add_overlay(GTK_OVERLAY(g_overlay), g_caret_w);
    gtk_overlay_set_overlay_pass_through(GTK_OVERLAY(g_overlay), g_caret_w, TRUE);
    gtk_container_add(GTK_CONTAINER(parent), g_overlay);
    gtk_widget_show(g_area_w);
    gtk_widget_show(g_overlay);
    g_holder = parent;
}

static void caret_layer_fini(void) {
    if (g_fade_id) g_source_remove(g_fade_id);
    g_fade_id = 0;
    if (g_overlay && g_holder && g_area_w) {
        /* Hand libui its area back where it put it: its window destroy
         * removes the area from the child holder. */
        g_object_ref(g_area_w);
        gtk_container_remove(GTK_CONTAINER(g_overlay), g_area_w);
        gtk_widget_destroy(g_overlay);
        gtk_container_add(GTK_CONTAINER(g_holder), g_area_w);
        g_object_unref(g_area_w);
    }
    g_overlay = g_caret_w = g_holder = NULL;
    g_crn = 0;
    if (g_under) cairo_surface_destroy(g_under);
    if (g_under_next) cairo_surface_destroy(g_under_next);
    g_under = g_under_next = NULL;
}

int ui_os_caret_ok(void) { return g_caret_w != NULL; }

/* The pixels the whole Draw left under the frame the rects will need. */
void ui_os_caret_capture(uiDrawContext *ctx, const UiCaretRect *r, int n) {
    cairo_t *cr;
    cairo_surface_t *tgt, *img;
    cairo_t *c2;
    double x0, y0, x1, y1, dx, dy, sx = 1, sy = 1;
    int i, pw, ph;
    if (g_under_next) cairo_surface_destroy(g_under_next);
    g_under_next = NULL;
    if (!ctx || !ctx->cr || !r || n <= 0) return;
    x0 = r[0].x;
    y0 = r[0].y;
    x1 = r[0].x + r[0].w;
    y1 = r[0].y + r[0].h;
    for (i = 1; i < n; i++) {
        if (r[i].x < x0) x0 = r[i].x;
        if (r[i].y < y0) y0 = r[i].y;
        if (r[i].x + r[i].w > x1) x1 = r[i].x + r[i].w;
        if (r[i].y + r[i].h > y1) y1 = r[i].y + r[i].h;
    }
    cr = ctx->cr;
    tgt = cairo_get_group_target(cr);
    if (!tgt) return;
    cairo_surface_get_device_scale(tgt, &sx, &sy);
    pw = (int)ceil((x1 - x0) * sx);
    ph = (int)ceil((y1 - y0) * sy);
    if (pw <= 0 || ph <= 0) return;
    img = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, pw, ph);
    if (cairo_surface_status(img) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(img);
        return;
    }
    cairo_surface_set_device_scale(img, sx, sy);
    dx = x0;
    dy = y0;
    cairo_user_to_device(cr, &dx, &dy);
    c2 = cairo_create(img);
    cairo_set_operator(c2, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_surface(c2, tgt, -dx, -dy);
    cairo_paint(c2);
    cairo_destroy(c2);
    g_under_next = img;
    g_nbx = x0;
    g_nby = y0;
    g_nbw = x1 - x0;
    g_nbh = y1 - y0;
}

void ui_os_caret_set(const UiCaretRect *r, int n) {
    int i;
    if (!g_caret_w) return;
    if (n > UI_OS_CARETS) n = UI_OS_CARETS;
    if (!r || n <= 0 || !g_under_next) {
        g_crn = 0;
        if (g_under_next) cairo_surface_destroy(g_under_next);
        g_under_next = NULL;
        gtk_widget_hide(g_caret_w);
        return;
    }
    for (i = 0; i < n; i++) g_cr[i] = r[i];
    g_crn = n;
    if (g_under) cairo_surface_destroy(g_under);
    g_under = g_under_next;
    g_under_next = NULL;
    if (g_nbx != g_bx || g_nby != g_by || g_nbw != g_bw || g_nbh != g_bh) {
        g_bx = g_nbx;
        g_by = g_nby;
        g_bw = g_nbw;
        g_bh = g_nbh;
        gtk_widget_set_margin_start(g_caret_w, (gint)g_bx);
        gtk_widget_set_margin_top(g_caret_w, (gint)g_by);
        gtk_widget_set_size_request(g_caret_w, (gint)g_bw, (gint)g_bh);
        /* The overlay places its children; a child's own resize only
         * re-allocates it at its old rectangle (one move late). */
        if (g_overlay) gtk_widget_queue_allocate(g_overlay);
    }
    if (!gtk_widget_get_visible(g_caret_w)) {
        GdkWindow *pw;
        gtk_widget_show(g_caret_w);
        /* GtkOverlay gives the child its own window; the area, realized
         * after the overlay, may sit above it. The caret goes on top. */
        pw = gtk_widget_get_parent_window(g_caret_w);
        if (pw) gdk_window_raise(pw);
    }
    caret_flush();
    if (g_im) {
        /* The IME's candidate window sits at the caret. */
        GdkRectangle a;
        a.x = (int)r[0].x;
        a.y = (int)r[0].y;
        a.width = (int)r[0].w;
        a.height = (int)r[0].h;
        gtk_im_context_set_cursor_location(g_im, &a);
    }
}

static gboolean caret_fade_tick(gpointer data) {
    double t;
    (void)data;
    t = (double)(g_get_monotonic_time() - g_fade_t0) / (1000.0 * g_fade_ms);
    if (t >= 1.0) t = 1.0;
    g_alpha = g_alpha_from + (g_alpha_to - g_alpha_from) * t;
    caret_flush();
    if (t >= 1.0) {
        g_fade_id = 0;
        return G_SOURCE_REMOVE;
    }
    return G_SOURCE_CONTINUE;
}

void ui_os_caret_show(int on, int fade_ms) {
    g_alpha_to = on ? 1.0 : 0.0;
    if (fade_ms <= 0) {
        if (g_fade_id) g_source_remove(g_fade_id);
        g_fade_id = 0;
        g_alpha = g_alpha_to;
        caret_flush();
        return;
    }
    g_alpha_from = g_alpha;
    g_fade_ms = fade_ms;
    g_fade_t0 = g_get_monotonic_time();
    if (!g_fade_id) g_fade_id = g_timeout_add(16, caret_fade_tick, NULL);
}

static GtkWindow *gtk_of(uiWindow *win) {
    GtkWidget *top = win ? GTK_WIDGET(uiControlHandle(uiControl(win))) : NULL;
    return top && GTK_IS_WINDOW(top) ? GTK_WINDOW(top) : NULL;
}

int ui_os_frame_get(uiWindow *win, int *x, int *y, int *w, int *h) {
    GtkWindow *gw = gtk_of(win);
    GdkWindow *gdk;
    if (!gw) return 0;
    gdk = gtk_widget_get_window(GTK_WIDGET(gw));
    if (gdk && (gdk_window_get_state(gdk) &
                (GDK_WINDOW_STATE_FULLSCREEN | GDK_WINDOW_STATE_MAXIMIZED)))
        return 0;
    gtk_window_get_position(gw, x, y);
    gtk_window_get_size(gw, w, h);
    return *w > 0 && *h > 0;
}

void ui_os_frame_set(uiWindow *win, int x, int y, int w, int h) {
    GtkWindow *gw = gtk_of(win);
    GdkDisplay *dpy = gdk_display_get_default();
    GdkRectangle f = {x, y, w, h}, vis = {0, 0, 0, 0};
    long best_a = 0, best_d = -1;
    int i, n;
    if (!gw || !dpy || w <= 0 || h <= 0) return;
    n = gdk_display_get_n_monitors(dpy);
    for (i = 0; i < n; i++) {
        GdkRectangle v, is;
        long a = 0, dx, dy, d;
        gdk_monitor_get_workarea(gdk_display_get_monitor(dpy, i), &v);
        if (gdk_rectangle_intersect(&f, &v, &is)) a = (long)is.width * is.height;
        dx = (long)(f.x + f.width / 2) - (v.x + v.width / 2);
        dy = (long)(f.y + f.height / 2) - (v.y + v.height / 2);
        d = dx * dx + dy * dy;
        if (a > best_a || (best_a == 0 && a == 0 && (best_d < 0 || d < best_d))) {
            vis = v;
            best_a = a;
            best_d = d;
        }
    }
    if (vis.width <= 0 || vis.height <= 0) return;
    f.width = MAX(MIN(f.width, vis.width), MIN(400, vis.width));
    f.height = MAX(MIN(f.height, vis.height), MIN(300, vis.height));
    f.x = MAX(vis.x, MIN(f.x, vis.x + vis.width - f.width));
    f.y = MAX(vis.y, MIN(f.y, vis.y + vis.height - f.height));
    gtk_window_resize(gw, f.width, f.height);
    gtk_window_move(gw, f.x, f.y);
}

void ui_os_init(uiWindow *win, uiArea *area) {
    GtkWidget *top;
    g_win = win;
    g_area = area;
    g_area_w = area_widget();
    if (!g_area_w) return;
    caret_layer_init();
    /* uiWindowSetChild puts the area in libui's child holder without
     * expand; GTK then allocates it 1 px wide. */
    gtk_widget_set_hexpand(g_area_w, TRUE);
    gtk_widget_set_vexpand(g_area_w, TRUE);
    gtk_widget_set_halign(g_area_w, GTK_ALIGN_FILL);
    gtk_widget_set_valign(g_area_w, GTK_ALIGN_FILL);
    gtk_widget_add_events(g_area_w, GDK_SCROLL_MASK | GDK_SMOOTH_SCROLL_MASK);
    g_sig_scroll = g_signal_connect(g_area_w, "scroll-event",
                                    G_CALLBACK(on_scroll), NULL);
    g_im = gtk_im_context_simple_new();
    g_sig_commit = g_signal_connect(g_im, "commit", G_CALLBACK(on_commit), NULL);
    if (!gtk_widget_get_realized(g_area_w) && g_overlay &&
        gtk_widget_get_realized(g_overlay))
        gtk_widget_realize(g_area_w);
    if (gtk_widget_get_realized(g_area_w))
        gtk_im_context_set_client_window(g_im, gtk_widget_get_window(g_area_w));
    gtk_im_context_focus_in(g_im);
    g_sig_press = g_signal_connect(g_area_w, "key-press-event",
                                   G_CALLBACK(on_key_pre), NULL);
    g_sig_release = g_signal_connect(g_area_w, "key-release-event",
                                     G_CALLBACK(on_key_pre), NULL);
    g_sig_after = g_signal_connect_after(g_area_w, "key-press-event",
                                         G_CALLBACK(on_key_post), NULL);
    gtk_widget_grab_focus(g_area_w);
    top = win ? GTK_WIDGET(uiControlHandle(uiControl(win))) : NULL;
    if (top && GTK_IS_WINDOW(top)) {
        g_top_w = top;
        g_sig_focus_in = g_signal_connect(top, "focus-in-event",
                                          G_CALLBACK(on_focus), (gpointer)1);
        g_sig_focus_out = g_signal_connect(top, "focus-out-event",
                                           G_CALLBACK(on_focus), NULL);
        /* Focused until told otherwise: with no window manager (Xvfb)
         * GTK may never see a focus event at all. */
        gtk_window_present(GTK_WINDOW(top));
    }
}

void ui_os_fini(void) {
    GtkClipboard *cb = gtk_clipboard_get(GDK_SELECTION_CLIPBOARD);
    /* Hand the text to a clipboard manager (if any) before we exit. */
    if (cb) gtk_clipboard_store(cb);
    if (g_area_w) {
        if (g_sig_scroll) g_signal_handler_disconnect(g_area_w, g_sig_scroll);
        if (g_sig_press) g_signal_handler_disconnect(g_area_w, g_sig_press);
        if (g_sig_release) g_signal_handler_disconnect(g_area_w, g_sig_release);
        if (g_sig_after) g_signal_handler_disconnect(g_area_w, g_sig_after);
    }
    g_sig_scroll = g_sig_press = g_sig_release = g_sig_after = 0;
    if (g_top_w) {
        if (g_sig_focus_in) g_signal_handler_disconnect(g_top_w, g_sig_focus_in);
        if (g_sig_focus_out) g_signal_handler_disconnect(g_top_w, g_sig_focus_out);
    }
    g_sig_focus_in = g_sig_focus_out = 0;
    g_top_w = NULL;
    if (g_im) {
        if (g_sig_commit) g_signal_handler_disconnect(g_im, g_sig_commit);
        gtk_im_context_set_client_window(g_im, NULL);
        g_object_unref(g_im);
        g_im = NULL;
    }
    g_sig_commit = 0;
    caret_layer_fini();
    g_area_w = NULL;
    g_area = NULL;
    g_win = NULL;
}

int ui_os_typed_text(const uiAreaKeyEvent *e, int *out, int cap) {
    int i, n;
    (void)e;
    if (!g_in_key || !out || cap <= 0) return 0;
    n = g_ntext < cap ? g_ntext : cap;
    for (i = 0; i < n; i++) out[i] = g_text[i];
    /* One key event, one delivery. */
    g_ntext = 0;
    return n;
}

static gboolean step_wake(gpointer data) {
    (void)data;
    return G_SOURCE_REMOVE;
}

void ui_os_step(double wait_s) {
    int spins = 0;
    if (wait_s < 0 && !gtk_events_pending()) {
        /* Nothing due: block until an event (input, expose, a source). */
        gtk_main_iteration_do(TRUE);
    } else if (wait_s > 0 && !gtk_events_pending()) {
        guint ms = (guint)(wait_s * 1000.0 + 0.5);
        guint id = g_timeout_add(ms ? ms : 1, step_wake, NULL);
        GSource *src;
        gtk_main_iteration_do(TRUE);
        src = g_main_context_find_source_by_id(NULL, id);
        if (src) g_source_destroy(src);
    }
    /* Bounded: a source that is always ready must not starve the host. */
    while (gtk_events_pending() && spins++ < 256)
        gtk_main_iteration_do(FALSE);
}

void ui_os_sync_paint(void) {
    GtkWidget *w = area_widget();
    GdkWindow *gw;
    if (!w) return;
    gtk_widget_queue_draw(w);
    gw = gtk_widget_get_window(w);
    if (gw) {
        G_GNUC_BEGIN_IGNORE_DEPRECATIONS
        gdk_window_process_updates(gw, FALSE);
        G_GNUC_END_IGNORE_DEPRECATIONS
    }
}

void ui_os_sync_paint_rect(double x, double y, double w, double h) {
    GtkWidget *wd = area_widget();
    GdkWindow *gw;
    if (!wd || w <= 0 || h <= 0) return;
    gtk_widget_queue_draw_area(wd, (gint)x, (gint)y, (gint)(w + 0.999),
                               (gint)(h + 0.999));
    gw = gtk_widget_get_window(wd);
    if (gw) {
        G_GNUC_BEGIN_IGNORE_DEPRECATIONS
        gdk_window_process_updates(gw, FALSE);
        G_GNUC_END_IGNORE_DEPRECATIONS
    }
}

void ui_os_clip_set(const char *utf8) {
    GtkClipboard *cb = gtk_clipboard_get(GDK_SELECTION_CLIPBOARD);
    if (!cb) return;
    gtk_clipboard_set_text(cb, utf8 ? utf8 : "", -1);
    gtk_clipboard_set_can_store(cb, NULL, 0);
}

char *ui_os_clip_get(void) {
    GtkClipboard *cb = gtk_clipboard_get(GDK_SELECTION_CLIPBOARD);
    gchar *t;
    char *out;
    size_t n;
    if (!cb) return NULL;
    /* Runs a nested main loop until the owner answers (or times out). */
    t = gtk_clipboard_wait_for_text(cb);
    if (!t) return NULL;
    n = strlen(t);
    out = (char *)malloc(n + 1);
    if (out) memcpy(out, t, n + 1);
    g_free(t);
    return out;
}

static void find_menubar(GtkWidget *w, gpointer data) {
    GtkWidget **hit = (GtkWidget **)data;
    if (*hit || !w) return;
    if (GTK_IS_MENU_BAR(w)) {
        *hit = w;
        return;
    }
    if (GTK_IS_CONTAINER(w))
        gtk_container_forall(GTK_CONTAINER(w), find_menubar, data);
}

static GtkWidget *apply_menu(void) {
    static GtkWidget *cached;
    GtkWidget *top, *bar = NULL;
    GList *items, *l;
    if (cached) return cached;
    if (!g_win) return NULL;
    top = GTK_WIDGET(uiControlHandle(uiControl(g_win)));
    if (!top) return NULL;
    find_menubar(top, &bar);
    if (!bar) return NULL;
    items = gtk_container_get_children(GTK_CONTAINER(bar));
    for (l = items; l; l = l->next) {
        const gchar *label;
        if (!GTK_IS_MENU_ITEM(l->data)) continue;
        label = gtk_menu_item_get_label(GTK_MENU_ITEM(l->data));
        if (label && strcmp(label, "Apply") == 0) {
            cached = gtk_menu_item_get_submenu(GTK_MENU_ITEM(l->data));
            break;
        }
    }
    g_list_free(items);
    return cached;
}

/* libui keeps one GtkMenuItem per window in a private table; the item is
 * found by its position in the Apply submenu instead. */
void ui_os_menu_slot(uiMenuItem *it, int slot, const char *title) {
    GtkWidget *menu = apply_menu();
    GList *kids;
    GtkWidget *w;
    (void)it;
    if (!menu || slot < 0 || slot >= UI_APPLY_SLOTS) return;
    kids = gtk_container_get_children(GTK_CONTAINER(menu));
    w = (GtkWidget *)g_list_nth_data(kids, (guint)(UI_APPLY_SLOT0 + slot));
    g_list_free(kids);
    if (!w || !GTK_IS_MENU_ITEM(w)) return;
    if (!title || !title[0]) {
        gtk_widget_hide(w);
        return;
    }
    gtk_menu_item_set_label(GTK_MENU_ITEM(w), title);
    gtk_widget_show(w);
}

double ui_os_pt_per_px(void) {
    static double cached;
    if (cached <= 0) {
        GdkScreen *scr = gdk_screen_get_default();
        double dpi = scr ? gdk_screen_get_resolution(scr) : -1;
        if (dpi <= 0) dpi = 96.0;
        cached = 72.0 / dpi;
    }
    return cached;
}

/* fontconfig family names. "family:X" is passed through; a file path from
 * an older list maps by its basename; anything else is the monospace
 * alias (DejaVu Sans Mono / Liberation Mono / Noto Sans Mono …). */
const char *ui_os_font_family(const char *path) {
    if (!path) return "Monospace";
    if (strncmp(path, "family:", 7) == 0 && path[7]) return path + 7;
    if (strstr(path, "DejaVuSansMono")) return "DejaVu Sans Mono";
    if (strstr(path, "LiberationMono")) return "Liberation Mono";
    if (strstr(path, "DejaVuSerif")) return "DejaVu Serif";
    if (strstr(path, "LiberationSerif")) return "Liberation Serif";
    if (strstr(path, "DejaVuSans")) return "DejaVu Sans";
    return "Monospace";
}
