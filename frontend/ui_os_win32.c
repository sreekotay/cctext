/*
 * cctext-ui shim for Windows (Win32). See ui_os.h.
 *
 * STUB: not built yet (no Windows target in make.shcc / CI). It compiles
 * to "nothing typed, no wheel, no clipboard" so the portable host could
 * link; each TODO names the Win32 piece that replaces it. Beyond this
 * shim, a Windows port also needs: core/ POSIX bits (posix_spawn, mmap /
 * pread page store, realpath, termios stays TUI-only), libui-ng's
 * windows/ backend (meson, MSVC or MinGW), and a build recipe.
 */
#include <windows.h>
#include "ui_os.h"
#include <stdlib.h>
#include <string.h>

static uiWindow *g_win;
static uiArea *g_area;

void ui_os_init(uiWindow *win, uiArea *area) {
    g_win = win;
    g_area = area;
    /* TODO: HWND hwnd = (HWND)uiControlHandle(uiControl(area));
     * SetWindowSubclass(hwnd, area_proc, 1, 0) to see WM_SETFOCUS /
     * WM_KILLFOCUS on the toplevel (or WM_ACTIVATE) → ui_plat_focus, WM_CHAR /
     * WM_UNICHAR (typed text, IME commits arrive as WM_CHAR after
     * WM_IME_COMPOSITION) and WM_MOUSEWHEEL / WM_MOUSEHWHEEL
     * (GET_WHEEL_DELTA_WPARAM / WHEEL_DELTA notches → ui_plat_wheel;
     * vertical is +up already, horizontal +right). Then SetFocus(hwnd).
     * libui's area already handles WM_MOUSEWHEEL for scrolling areas only;
     * cctext uses a non-scrolling area, so the subclass sees them. */
}

void ui_os_fini(void) {
    /* TODO: RemoveWindowSubclass(hwnd, area_proc, 1). */
    g_win = NULL;
    g_area = NULL;
}

int ui_os_frame_get(uiWindow *win, int *x, int *y, int *w, int *h) {
    /* TODO: GetWindowPlacement(hwnd) → rcNormalPosition (skip when
     * showCmd is SW_SHOWMAXIMIZED). */
    (void)win;
    (void)x;
    (void)y;
    (void)w;
    (void)h;
    return 0;
}

void ui_os_frame_set(uiWindow *win, int x, int y, int w, int h) {
    /* TODO: MonitorFromRect(MONITOR_DEFAULTTONEAREST) → GetMonitorInfo
     * rcWork; clamp into it; SetWindowPos(SWP_NOZORDER | SWP_NOACTIVATE). */
    (void)win;
    (void)x;
    (void)y;
    (void)w;
    (void)h;
}

int ui_os_typed_text(const uiAreaKeyEvent *e, int *out, int cap) {
    (void)e;
    (void)out;
    (void)cap;
    /* TODO: libui dispatches its key event from WM_KEYDOWN; the WM_CHAR
     * that TranslateMessage posts comes after. Queue WM_CHAR code units in
     * area_proc (join UTF-16 surrogates), and return them here on the next
     * key-down, or push them straight into the portable char queue from
     * area_proc (add a ui_plat_text(int cp) callback to ui_os.h). */
    return 0;
}

void ui_os_step(double wait_s) {
    /* TODO: if (wait_s > 0 && !PeekMessageW(&m, NULL, 0, 0, PM_NOREMOVE))
     *     MsgWaitForMultipleObjectsEx(0, NULL, (DWORD)(wait_s * 1000),
     *                                 QS_ALLINPUT, MWMO_INPUTAVAILABLE);
     * then while (PeekMessageW(&m, NULL, 0, 0, PM_REMOVE)) { if the
     * message is for a dialog, IsDialogMessage; TranslateMessage;
     * DispatchMessageW; }  (uiMainStep(0) does at most one message and
     * returns 1 either way, so it cannot drain by itself.) wait_s < 0 is
     * INFINITE (WaitMessage). */
    (void)wait_s;
    uiMainStep(0);
}

void ui_os_sync_paint(void) {
    /* TODO: HWND hwnd = (HWND)uiControlHandle(uiControl(g_area));
     * InvalidateRect(hwnd, NULL, FALSE); UpdateWindow(hwnd); — Direct2D
     * paint runs synchronously in WM_PAINT. uiAreaQueueRedrawAll is the
     * async half. */
    if (g_area) uiAreaQueueRedrawAll(g_area);
}

void ui_os_sync_paint_rect(double x, double y, double w, double h) {
    /* TODO: RECT rc = { (LONG)x, (LONG)y, (LONG)(x + w + 0.999),
     * (LONG)(y + h + 0.999) }; InvalidateRect(hwnd, &rc, FALSE);
     * UpdateWindow(hwnd); — libui's WM_PAINT hands ps.rcPaint to the Draw
     * as its clip (windows/areadraw.cpp). Until then, the whole area. */
    (void)x;
    (void)y;
    (void)w;
    (void)h;
    if (g_area) uiAreaQueueRedrawAll(g_area);
}

/*
 * Caret layer — design (stub returns 0: ui_plat paints the carets and
 * repaints their rows on a blink until this lands).
 *
 * Win32 has a real caret layer: the system caret. It belongs to the
 * window with keyboard focus, is drawn and blinked by the OS (XOR over
 * the window, so a blink never sends WM_PAINT), and is what IMEs and
 * accessibility (MSAA / UIA caret events, the magnifier) follow.
 *
 *   ok:      return 1 once area_proc (ui_os_init TODO) sees WM_SETFOCUS.
 *   set:     on WM_SETFOCUS and whenever rect 0 changes size,
 *            CreateCaret(hwnd, NULL, w, h) (NULL bitmap: a solid block the
 *            caret width; GetSystemMetrics(SM_CXFOCUSBORDER) or the
 *            SPI_GETCARETWIDTH value is the user's preferred width), then
 *            SetCaretPos(x, y) and ShowCaret(hwnd). n = 0: HideCaret.
 *            WM_KILLFOCUS: DestroyCaret (one caret per thread).
 *            The IME anchor comes free (SetCaretPos moves the composition
 *            window); ImmSetCompositionWindow(CFS_POINT) for an explicit one.
 *            Only one system caret exists: hex's second bar stays painted
 *            (solid, the mirror of the one that blinks).
 *   show:    nothing — the OS blinks it at GetCaretBlinkTime() (530 ms by
 *            default; INFINITE when the user turned blinking off, which
 *            RTX_UI_BLINK_MS mirrors) and stops on its own after the
 *            user's caret timeout. The host's blink timer can stay off
 *            (RtxUi.host_blinks = 1, as the terminal cursor does).
 *   capture: nothing — the caret is XOR'd over the painted pixels.
 *            Direct2D paint must HideCaret / ShowCaret around WM_PAINT
 *            (BeginPaint does it for GDI; libui's Direct2D render target
 *            presents outside it) or the XOR leaves a stale bar.
 *   fade:    not available on the system caret.
 */
int ui_os_caret_ok(void) { return 0; }

void ui_os_caret_capture(uiDrawContext *ctx, const UiCaretRect *r, int n) {
    (void)ctx;
    (void)r;
    (void)n;
}

void ui_os_caret_set(const UiCaretRect *r, int n) {
    (void)r;
    (void)n;
}

void ui_os_caret_show(int on, int fade_ms) {
    (void)on;
    (void)fade_ms;
}

void ui_os_clip_set(const char *utf8) {
    /* TODO: MultiByteToWideChar(CP_UTF8) into a GlobalAlloc(GMEM_MOVEABLE)
     * block; OpenClipboard(hwnd); EmptyClipboard();
     * SetClipboardData(CF_UNICODETEXT, h); CloseClipboard(). */
    (void)utf8;
}

char *ui_os_clip_get(void) {
    /* TODO: OpenClipboard(hwnd); GetClipboardData(CF_UNICODETEXT);
     * GlobalLock; WideCharToMultiByte(CP_UTF8) into malloc; GlobalUnlock;
     * CloseClipboard(). Normalise CRLF → LF for the document. */
    return NULL;
}

void ui_os_menu_slot(uiMenuItem *it, int slot, const char *title) {
    /* TODO: libui builds one HMENU per window from its item list; walk
     * GetMenu(hwnd) → the "Apply" popup (GetSubMenu by position) →
     * ModifyMenuW(popup, UI_APPLY_SLOT0 + slot, MF_BYPOSITION | MF_STRING,
     * id, title) and DrawMenuBar(hwnd). Win32 menus have no "hidden":
     * RemoveMenu / InsertMenuItemW to hide and show, keeping the item id
     * libui assigned (GetMenuItemID) so WM_COMMAND still reaches it. */
    (void)it;
    (void)slot;
    (void)title;
}

/* TODO: confirm how libui's windows/ backend reads uiFontDescriptor.Size
 * (points → DIPs at 96/72 would make 0.75 right, as on GTK); per-monitor
 * DPI is DirectWrite's job, not this factor. */
double ui_os_pt_per_px(void) { return 0.75; }

const char *ui_os_font_family(const char *path) {
    if (!path) return "Consolas";
    if (strncmp(path, "family:", 7) == 0 && path[7]) return path + 7;
    return "Consolas";
}
