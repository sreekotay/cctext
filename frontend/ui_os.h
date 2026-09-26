/*
 * cctext-ui per-OS shim. ui_plat.c is portable libui-ng code; the few
 * things libui does not expose live behind this interface, one file per
 * toolkit:
 *
 *   ui_os_darwin.m  AppKit (macOS)
 *   ui_os_gtk.c     GTK 3 (Linux / BSD)
 *   ui_os_win32.c   Win32 (stub, not built yet)
 *
 * libui has no typed-text event (only scancode keys), no wheel event, no
 * clipboard, no menu-item retitle / hide, and uiMainStep(1) blocks with
 * no deadline. Everything else stays in ui_plat.c.
 */
#ifndef RTX_UI_OS_H
#define RTX_UI_OS_H

#include "ui.h"

/* Apply submenu: "Show Menu", "Cycle Prefix", separator, then the slots.
 * A toolkit that finds slots by position (GTK) counts from here. */
#define UI_APPLY_SLOT0 3
#define UI_APPLY_SLOTS 16

/* After the window and area are shown: hook wheel / key text, focus the
 * area. fini undoes the hooks before the window is destroyed. */
void ui_os_init(uiWindow *win, uiArea *area);
void ui_os_fini(void);

/* Outer window frame in the toolkit's own screen coordinates (only this
 * OS reads them back). get returns 0 when there is nothing worth keeping
 * (full screen, no window). set may run before the window is shown; it
 * clamps the frame into the work area of the monitor holding most of it
 * (the nearest monitor when none does), so a frame saved on a monitor
 * that is gone never lands off screen. */
int ui_os_frame_get(uiWindow *win, int *x, int *y, int *w, int *h);
void ui_os_frame_set(uiWindow *win, int x, int y, int w, int h);

/* Unicode scalars typed by the key event being dispatched right now
 * (layout, shift, dead keys / compose applied by the OS). Writes at most
 * cap, returns the count (0 = nothing typed, e.g. a dead key). Only
 * called for a key-down with no Ctrl / Alt / Super and no ExtKey. */
int ui_os_typed_text(const uiAreaKeyEvent *e, int *out, int cap);

/* Dispatch pending toolkit events. If none were pending, wait up to
 * wait_s seconds for one (0 = do not wait, < 0 = until an event), then
 * drain. Area Draw may run inside (the caller brackets this with ui_turn). */
void ui_os_step(double wait_s);

/* Invalidate the area and flush a paint now where the toolkit allows it
 * (AppKit displayIfNeeded); otherwise the next ui_os_step paints. */
void ui_os_sync_paint(void);

/* Same for one rectangle of the area (area coordinates, y down): the Draw
 * that follows carries it as its clip (uiAreaDrawParams.Clip*), so a
 * caret blink repaints a caret, not the window. */
void ui_os_sync_paint_rect(double x, double y, double w, double h);

/*
 * Caret layer: the carets live on their own layer above the area, so a
 * blink changes that layer and repaints no text. GTK: a child widget over
 * libui's area (a native X child window where GDK allows one); AppKit:
 * layer-backed subviews of the area view; Win32: the system caret
 * (CreateCaret / SetCaretPos; the OS blinks it) — stub.
 *
 * ok returns 1 when this toolkit has the layer (else ui_plat paints the
 * carets into the text and repaints their rows to blink them).
 *
 * capture runs at the end of a whole-area Draw with the rects that Draw
 * found (area coordinates, y down): a layer that cannot composite over
 * the area (GTK 3 keeps no retained pixels) keeps the pixels under them,
 * so the off phase shows what the text paint put there.
 *
 * set places the layer outside any Draw: n rects (n = 0 hides it), each
 * filled with its color while on. It is also the IME anchor: rect 0 is
 * the caret for gtk_im_context_set_cursor_location /
 * firstRectForCharacterRange. show sets the phase (1 = caret visible);
 * fade_ms > 0 eases it (the optional smooth blink).
 */
typedef struct UiCaretRect {
    double x, y, w, h;
    unsigned char r, g, b, a;
} UiCaretRect;

int ui_os_caret_ok(void);
void ui_os_caret_capture(uiDrawContext *ctx, const UiCaretRect *r, int n);
void ui_os_caret_set(const UiCaretRect *r, int n);
void ui_os_caret_show(int on, int fade_ms);

/* System clipboard, UTF-8. get returns malloc'd text (caller frees) or
 * NULL. get may run a nested toolkit loop (GTK); callers bracket it. */
void ui_os_clip_set(const char *utf8);
char *ui_os_clip_get(void);

/* Retitle an Apply submenu slot; title NULL or "" hides it. it is the
 * libui item, slot is 0..UI_APPLY_SLOTS-1. */
void ui_os_menu_slot(uiMenuItem *it, int slot, const char *title);

/* Font family for a gui_chrome font "path". "family:<name>" entries are
 * fontconfig / system family names. NULL path = the default monospace
 * family for this OS. */
const char *ui_os_font_family(const char *path);

/* Editor font sizes are pixels (Core Text points are pixels at 1x).
 * uiFontDescriptor.Size is points at the toolkit's DPI: Pango uses the
 * screen resolution (96 by default), so 16 px is 12 pt there. */
double ui_os_pt_per_px(void);

/* Toolkit → portable: wheel notches this event. +y = up (away from the
 * user); +x = scroll_x(+1), the view moves right. Fractions accumulate. */
void ui_plat_wheel(float dx, float dy);

/* Toolkit → portable: the window gained (1) or lost (0) keyboard focus.
 * libui reports neither. */
void ui_plat_focus(int in);

/* Toolkit → portable: text that arrived with no libui key event (a key
 * libui's scancode table does not know, an IME commit). Queued like typed
 * characters. */
void ui_plat_text(const int *cps, int n);

#endif
