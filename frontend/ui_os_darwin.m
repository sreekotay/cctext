/*
 * cctext-ui shim for macOS (AppKit). See ui_os.h.
 *
 * libui has no clipboard and no wheel event. Pasteboard and a local
 * scroll monitor are the AppKit calls; the window is not an NSView we
 * own. Typed text is the current NSEvent's characters (layout, shift,
 * dead keys already applied). Key-window notifications stand in for the
 * focus events libui does not report. The caret layer is layer-backed
 * subviews of the (then layer-backed) area view.
 */
#import <Cocoa/Cocoa.h>
#import <QuartzCore/CATransaction.h>
#include "ui_os.h"
#include <stdlib.h>
#include <string.h>

#define UI_WHEEL_PREC_X 10.0f
#define UI_WHEEL_PREC_Y 16.0f

static uiWindow *g_win;
static uiArea *g_area;
static id g_scroll_mon;
static id g_key_obs;
static id g_resign_obs;

/* Caret layer views (RtxCaretView, below "Caret layer"). */
enum { UI_OS_CARETS = 8 };
static NSView *g_caret_v[UI_OS_CARETS];
static int g_caret_nv;

static NSWindow *ns_window(void) {
    if (!g_win) return nil;
    return (__bridge NSWindow *)(void *)uiControlHandle(uiControl(g_win));
}

static NSView *ns_area(void) {
    if (!g_area) return nil;
    return (__bridge NSView *)(void *)uiControlHandle(uiControl(g_area));
}

static void note_scroll(NSEvent *ev) {
    CGFloat dx, dy;
    if (!ev) return;
    dx = ev.scrollingDeltaX;
    dy = ev.scrollingDeltaY;
    if (dx == 0 && dy == 0) {
        dx = ev.deltaX;
        dy = ev.deltaY;
    }
    if (ev.hasPreciseScrollingDeltas)
        ui_plat_wheel((float)(dx / (double)UI_WHEEL_PREC_X),
                      (float)(dy / (double)UI_WHEEL_PREC_Y));
    else
        ui_plat_wheel((float)dx, (float)dy);
}

void ui_os_init(uiWindow *win, uiArea *area) {
    NSWindow *w;
    NSView *view;
    g_win = win;
    g_area = area;
    if (!g_scroll_mon && win) {
        g_scroll_mon = [NSEvent addLocalMonitorForEventsMatchingMask:NSEventMaskScrollWheel
                                                             handler:^NSEvent *(NSEvent *ev) {
            NSWindow *mine = ns_window();
            if (mine && ev.window == mine) note_scroll(ev);
            return ev;
        }];
    }
    w = ns_window();
    view = ns_area();
    if (w && !g_key_obs) {
        NSNotificationCenter *nc = [NSNotificationCenter defaultCenter];
        g_key_obs = [nc addObserverForName:NSWindowDidBecomeKeyNotification
                                    object:w
                                     queue:nil
                                usingBlock:^(NSNotification *n) {
            (void)n;
            ui_plat_focus(1);
        }];
        g_resign_obs = [nc addObserverForName:NSWindowDidResignKeyNotification
                                       object:w
                                        queue:nil
                                   usingBlock:^(NSNotification *n) {
            (void)n;
            ui_plat_focus(0);
        }];
    }
    if (view && !view.wantsLayer) {
        /* The caret subviews composite over the area's backing layer. */
        view.wantsLayer = YES;
    }
    if (w && view) {
        [w makeFirstResponder:view];
        [NSApp activateIgnoringOtherApps:YES];
    }
}

void ui_os_fini(void) {
    if (g_scroll_mon) {
        [NSEvent removeMonitor:g_scroll_mon];
        g_scroll_mon = nil;
    }
    if (g_key_obs) {
        [[NSNotificationCenter defaultCenter] removeObserver:g_key_obs];
        g_key_obs = nil;
    }
    if (g_resign_obs) {
        [[NSNotificationCenter defaultCenter] removeObserver:g_resign_obs];
        g_resign_obs = nil;
    }
    {
        int i;
        for (i = 0; i < g_caret_nv; i++) {
            [g_caret_v[i] removeFromSuperview];
            g_caret_v[i] = nil;
        }
        g_caret_nv = 0;
    }
    g_win = NULL;
    g_area = NULL;
}

static NSWindow *ns_of(uiWindow *win) {
    if (!win) return nil;
    return (__bridge NSWindow *)(void *)uiControlHandle(uiControl(win));
}

int ui_os_frame_get(uiWindow *win, int *x, int *y, int *w, int *h) {
    NSWindow *nw = ns_of(win);
    NSRect f;
    if (!nw || (nw.styleMask & NSWindowStyleMaskFullScreen)) return 0;
    f = nw.frame;
    *x = (int)f.origin.x;
    *y = (int)f.origin.y;
    *w = (int)f.size.width;
    *h = (int)f.size.height;
    return *w > 0 && *h > 0;
}

void ui_os_frame_set(uiWindow *win, int x, int y, int w, int h) {
    NSWindow *nw = ns_of(win);
    NSRect f = NSMakeRect(x, y, w, h), vis = NSZeroRect;
    CGFloat best_a = 0, best_d = -1;
    if (!nw || w <= 0 || h <= 0) return;
    for (NSScreen *s in [NSScreen screens]) {
        NSRect v = s.visibleFrame;
        NSRect i = NSIntersectionRect(f, v);
        CGFloat a = i.size.width * i.size.height;
        CGFloat dx = NSMidX(f) - NSMidX(v), dy = NSMidY(f) - NSMidY(v);
        CGFloat d = dx * dx + dy * dy;
        if (a > best_a || (best_a == 0 && a == 0 && (best_d < 0 || d < best_d))) {
            vis = v;
            best_a = a;
            best_d = d;
        }
    }
    if (NSIsEmptyRect(vis)) return;
    f.size.width = MAX(MIN(f.size.width, vis.size.width), MIN(400, vis.size.width));
    f.size.height = MAX(MIN(f.size.height, vis.size.height), MIN(300, vis.size.height));
    f.origin.x = MAX(vis.origin.x, MIN(f.origin.x, NSMaxX(vis) - f.size.width));
    f.origin.y = MAX(vis.origin.y, MIN(f.origin.y, NSMaxY(vis) - f.size.height));
    [nw setFrame:f display:NO];
}

int ui_os_typed_text(const uiAreaKeyEvent *e, int *out, int cap) {
    NSEvent *ev;
    NSString *chars;
    NSUInteger i, len;
    int n = 0;
    (void)e;
    if (!out || cap <= 0) return 0;
    ev = [NSApp currentEvent];
    if (!ev || ev.type != NSEventTypeKeyDown) return 0;
    chars = ev.characters;
    len = chars.length;
    for (i = 0; i < len && n < cap; i++) {
        unichar u = [chars characterAtIndex:i];
        if (u >= 0xD800 && u <= 0xDBFF && i + 1 < len) {
            unichar lo = [chars characterAtIndex:i + 1];
            if (lo >= 0xDC00 && lo <= 0xDFFF) {
                out[n++] = 0x10000 + (((int)u - 0xD800) << 10) + ((int)lo - 0xDC00);
                i++;
            }
            continue;
        }
        if (u >= 0xD800 && u <= 0xDFFF) continue;
        out[n++] = (int)u;
    }
    return n;
}

/* libui's uiMainStep(1) blocks until an NSEvent and skips updateWindows
 * when the queue is empty, so a setNeedsDisplay never hits the screen and
 * NSTimer does not bring the host loop back. Drain with a deadline, and
 * always updateWindows so the area's drawRect runs. */
void ui_os_step(double wait_s) {
    NSDate *until = wait_s > 0 ? [NSDate dateWithTimeIntervalSinceNow:wait_s]
                    : wait_s < 0 ? [NSDate distantFuture]
                                 : [NSDate distantPast];
    for (;;) {
        @autoreleasepool {
            NSEvent *e = [NSApp nextEventMatchingMask:NSEventMaskAny
                                            untilDate:until
                                               inMode:NSDefaultRunLoopMode
                                              dequeue:YES];
            if (!e) {
                [NSApp updateWindows];
                break;
            }
            [NSApp sendEvent:e];
            [NSApp updateWindows];
            until = [NSDate distantPast];
        }
    }
}

void ui_os_sync_paint(void) {
    NSView *view = ns_area();
    if (!view) return;
    [view setNeedsDisplay:YES];
    if (view.window) [view.window displayIfNeeded];
    else [NSApp updateWindows];
}

/* libui's area view is flipped (y down), so area coordinates are view
 * coordinates. displayRect: runs drawRect: now with just this rect. */
void ui_os_sync_paint_rect(double x, double y, double w, double h) {
    NSView *view = ns_area();
    NSRect r = NSMakeRect(x, y, w, h);
    if (!view || w <= 0 || h <= 0) return;
    [view setNeedsDisplayInRect:r];
    if (view.window) [view displayRect:r];
    else [NSApp updateWindows];
}

/*
 * Caret layer: one layer-backed subview per caret rect, over libui's
 * area view (made layer-backed in ui_os_init). Core Animation composites
 * the area's backing layer and these, so hiding a caret (alphaValue 0)
 * redraws nothing: no drawRect:, no text. The subviews take no events
 * (hitTest: nil) and never become first responder. UNTESTED: written
 * without a Mac build; see DESIGN.md.
 */
@interface RtxCaretView : NSView
@end

@implementation RtxCaretView
- (NSView *)hitTest:(NSPoint)point {
    (void)point;
    return nil;
}
- (BOOL)acceptsFirstResponder {
    return NO;
}
- (BOOL)wantsUpdateLayer {
    return YES;
}
- (void)updateLayer {
    /* The layer's backgroundColor is the whole content. */
}
@end

static CGFloat g_caret_alpha = 1.0;
/* IME anchor (area coordinates, y down). libui's area is not an
 * NSTextInputClient yet; when it is, firstRectForCharacterRange: returns
 * [win convertRectToScreen:[area convertRect:g_ime_rect toView:nil]]. */
static NSRect g_ime_rect;

int ui_os_caret_ok(void) { return ns_area() != nil; }

/* Core Animation keeps the area's pixels: nothing to capture. */
void ui_os_caret_capture(uiDrawContext *ctx, const UiCaretRect *r, int n) {
    (void)ctx;
    (void)r;
    (void)n;
}

void ui_os_caret_set(const UiCaretRect *r, int n) {
    NSView *area = ns_area();
    int i;
    if (!area) return;
    if (n > UI_OS_CARETS) n = UI_OS_CARETS;
    if (!r) n = 0;
    [CATransaction begin];
    [CATransaction setDisableActions:YES];
    for (i = 0; i < n; i++) {
        NSView *v = g_caret_v[i];
        CGFloat y = area.isFlipped ? (CGFloat)r[i].y
                                   : NSHeight(area.bounds) - (CGFloat)(r[i].y + r[i].h);
        CGColorRef c;
        if (!v) {
            v = [[RtxCaretView alloc] initWithFrame:NSZeroRect];
            v.wantsLayer = YES;
            v.layerContentsRedrawPolicy = NSViewLayerContentsRedrawNever;
            [area addSubview:v positioned:NSWindowAbove relativeTo:nil];
            g_caret_v[i] = v;
            if (g_caret_nv < i + 1) g_caret_nv = i + 1;
        }
        v.frame = NSMakeRect((CGFloat)r[i].x, y, (CGFloat)r[i].w, (CGFloat)r[i].h);
        c = CGColorCreateGenericRGB(r[i].r / 255.0, r[i].g / 255.0, r[i].b / 255.0,
                                    r[i].a / 255.0);
        v.layer.backgroundColor = c;
        CGColorRelease(c);
        v.alphaValue = g_caret_alpha;
        v.hidden = NO;
    }
    for (; i < g_caret_nv; i++) {
        if (g_caret_v[i]) g_caret_v[i].hidden = YES;
    }
    [CATransaction commit];
    if (n > 0) g_ime_rect = NSMakeRect(r[0].x, r[0].y, r[0].w, r[0].h);
}

void ui_os_caret_show(int on, int fade_ms) {
    int i;
    g_caret_alpha = on ? 1.0 : 0.0;
    if (fade_ms > 0) {
        [NSAnimationContext runAnimationGroup:^(NSAnimationContext *ctx) {
            int k;
            ctx.duration = fade_ms / 1000.0;
            for (k = 0; k < g_caret_nv; k++)
                if (g_caret_v[k]) g_caret_v[k].animator.alphaValue = g_caret_alpha;
        } completionHandler:nil];
        return;
    }
    [CATransaction begin];
    [CATransaction setDisableActions:YES];
    for (i = 0; i < g_caret_nv; i++)
        if (g_caret_v[i]) g_caret_v[i].alphaValue = g_caret_alpha;
    [CATransaction commit];
}

void ui_os_clip_set(const char *utf8) {
    NSPasteboard *pb = [NSPasteboard generalPasteboard];
    NSString *s;
    [pb clearContents];
    if (!utf8) return;
    s = [[NSString alloc] initWithUTF8String:utf8];
    if (!s)
        s = [[NSString alloc] initWithBytes:utf8 length:strlen(utf8)
                                   encoding:NSISOLatin1StringEncoding];
    if (s) [pb setString:s forType:NSPasteboardTypeString];
}

char *ui_os_clip_get(void) {
    NSString *s = [[NSPasteboard generalPasteboard] stringForType:NSPasteboardTypeString];
    const char *u = s.UTF8String;
    size_t n;
    char *out;
    if (!u) return NULL;
    n = strlen(u);
    out = (char *)malloc(n + 1);
    if (!out) return NULL;
    memcpy(out, u, n + 1);
    return out;
}

/* darwin/menu.m: first field of uiMenuItem is its NSMenuItem. */
void ui_os_menu_slot(uiMenuItem *it, int slot, const char *title) {
    NSMenuItem *ns;
    (void)slot;
    if (!it) return;
    ns = *(__unsafe_unretained NSMenuItem **)(void *)it;
    if (!ns) return;
    if (!title || !title[0]) {
        [ns setHidden:YES];
        return;
    }
    [ns setTitle:[NSString stringWithUTF8String:title]];
    [ns setHidden:NO];
}

/* Core Text: one point per (1x) pixel. */
double ui_os_pt_per_px(void) { return 1.0; }

const char *ui_os_font_family(const char *path) {
    if (!path) return "Menlo";
    if (strncmp(path, "family:", 7) == 0 && path[7]) return path + 7;
    if (strstr(path, "Georgia")) return "Georgia";
    if (strstr(path, "Arial")) return "Arial";
    if (strstr(path, "Courier")) return "Courier New";
    return "Menlo";
}
