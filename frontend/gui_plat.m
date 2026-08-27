/*
 * cctext-gui platform: Cocoa window + Core Text (display-list paint).
 * Linked as a normal clang .m object into bin/cctext-gui.
 */
#import <Cocoa/Cocoa.h>
#import <CoreText/CoreText.h>
#include "gui_plat.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <unistd.h>

enum { FB_OP_MAX = 65536, FB_STR_POOL = 2 * 1024 * 1024, FB_KEY_MAX = 512 };

typedef enum {
    FB_CLEAR = 1,
    FB_RECT,
    FB_RECT_LINES,
    FB_LINE,
    FB_TEXT,
    FB_CLIP,
    FB_CLIP_END
} FbOpKind;

typedef struct {
    FbOpKind kind;
    int x, y, w, h, x1, y1;
    Color c;
    Font font;
    float fontSize;
    uint32_t str_off;
} FbOp;

static FbOp g_ops[FB_OP_MAX];
static int g_nop;
static char g_strs[FB_STR_POOL];
static size_t g_str_used;

static int g_ww = 960, g_hh = 640;
static int g_ready;
static int g_should_close;
static int g_resized;
static int g_exit_key = KEY_ESCAPE;
static int g_fps = 60;
static unsigned int g_flags;
static int g_drawing;

static uint8_t g_key_down[FB_KEY_MAX];
static uint8_t g_key_pressed[FB_KEY_MAX];
static uint8_t g_key_repeat[FB_KEY_MAX];
static int g_chars[64];
static int g_nchar, g_char_rd;

static int g_mouse_x, g_mouse_y;
static int g_mouse_down, g_mouse_pressed, g_mouse_released;
static float g_wheel_x, g_wheel_y;
static float g_wheel_carry_x, g_wheel_carry_y;

static char *g_clip;
static size_t g_clip_cap;
static NSWindow *g_window;
static NSView *g_view;
static id g_app_delegate;
static int g_menu_cmd;
static int g_menu_ready;
static void (*g_live_resize)(void);

@interface FbView : NSView
@end

@interface FbAppDelegate : NSObject <NSApplicationDelegate, NSWindowDelegate>
- (void)menuCommand:(id)sender;
@end

static void gui_menu_add(NSMenu *menu, NSString *title, NSString *key, int mask,
                         int cmd) {
    NSMenuItem *it = [[NSMenuItem alloc] initWithTitle:title
                                                action:@selector(menuCommand:)
                                         keyEquivalent:key ? key : @""];
    it.target = g_app_delegate;
    it.tag = cmd;
    if (key && key.length)
        it.keyEquivalentModifierMask = (NSEventModifierFlags)mask;
    [menu addItem:it];
}

static void gui_menu_build(void) {
    NSMenu *mainMenu;
    NSMenu *appMenu;
    NSMenu *fileMenu;
    NSMenu *editMenu;
    NSMenu *viewMenu;
    NSMenu *goMenu;
    NSMenuItem *appItem;
    NSMenuItem *fileItem;
    NSMenuItem *editItem;
    NSMenuItem *viewItem;
    NSMenuItem *goItem;
    NSEventModifierFlags cmd = NSEventModifierFlagCommand;
    NSEventModifierFlags cmdshift = NSEventModifierFlagCommand | NSEventModifierFlagShift;

    if (g_menu_ready || !g_app_delegate) return;
    mainMenu = [[NSMenu alloc] initWithTitle:@"Main"];

    appMenu = [[NSMenu alloc] initWithTitle:@"cctext"];
    appItem = [[NSMenuItem alloc] init];
    appItem.submenu = appMenu;
    [mainMenu addItem:appItem];
    gui_menu_add(appMenu, @"Quit cctext", @"q", (int)cmd, CMD_QUIT);

    fileMenu = [[NSMenu alloc] initWithTitle:@"File"];
    fileItem = [[NSMenuItem alloc] init];
    fileItem.submenu = fileMenu;
    fileItem.title = @"File";
    [mainMenu addItem:fileItem];
    gui_menu_add(fileMenu, @"Open…", @"", 0, CMD_OPEN);
    gui_menu_add(fileMenu, @"Browse", @"b", (int)cmd, CMD_BROWSE);
    gui_menu_add(fileMenu, @"Save", @"s", (int)cmd, CMD_SAVE);
    [fileMenu addItem:[NSMenuItem separatorItem]];
    gui_menu_add(fileMenu, @"Close Window", @"w", (int)cmd, CMD_QUIT);

    editMenu = [[NSMenu alloc] initWithTitle:@"Edit"];
    editItem = [[NSMenuItem alloc] init];
    editItem.submenu = editMenu;
    editItem.title = @"Edit";
    [mainMenu addItem:editItem];
    gui_menu_add(editMenu, @"Undo", @"z", (int)cmd, CMD_UNDO);
    gui_menu_add(editMenu, @"Redo", @"z", (int)cmdshift, CMD_REDO);
    [editMenu addItem:[NSMenuItem separatorItem]];
    gui_menu_add(editMenu, @"Cut", @"x", (int)cmd, CMD_CUT);
    gui_menu_add(editMenu, @"Copy", @"c", (int)cmd, CMD_COPY);
    gui_menu_add(editMenu, @"Paste", @"v", (int)cmd, CMD_PASTE);
    gui_menu_add(editMenu, @"Select All", @"a", (int)cmd, CMD_SEL_ALL);

    viewMenu = [[NSMenu alloc] initWithTitle:@"View"];
    viewItem = [[NSMenuItem alloc] init];
    viewItem.submenu = viewMenu;
    viewItem.title = @"View";
    [mainMenu addItem:viewItem];
    gui_menu_add(viewMenu, @"Cycle View", @"l", (int)cmd, CMD_VIEW);
    gui_menu_add(viewMenu, @"Follow Caret", @"u", (int)cmd, CMD_FOLLOW);
    gui_menu_add(viewMenu, @"Split", @"\\", (int)cmd, CMD_SPLIT);
    gui_menu_add(viewMenu, @"Next File", @"n", (int)cmd, CMD_NEXT);
    gui_menu_add(viewMenu, @"Other Pane", @"", 0, CMD_PANE);
    [viewMenu addItem:[NSMenuItem separatorItem]];
    gui_menu_add(viewMenu, @"Key Bindings", @"/", (int)cmd, CMD_HELP);
    gui_menu_add(viewMenu, @"Stats", @"=", (int)cmd, CMD_STATS);

    goMenu = [[NSMenu alloc] initWithTitle:@"Go"];
    goItem = [[NSMenuItem alloc] init];
    goItem.submenu = goMenu;
    goItem.title = @"Go";
    [mainMenu addItem:goItem];
    gui_menu_add(goMenu, @"Jump to Line…", @"g", (int)cmd, CMD_JUMP);
    gui_menu_add(goMenu, @"Find…", @"f", (int)cmd, CMD_FIND);
    [goMenu addItem:[NSMenuItem separatorItem]];
    gui_menu_add(goMenu, @"Next Mark", @"k", (int)cmd, CMD_NAV_NEXT);
    gui_menu_add(goMenu, @"Previous Mark", @"p", (int)cmd, CMD_NAV_PREV);
    gui_menu_add(goMenu, @"Next Invalid", @"e", (int)cmd, CMD_NAV_INV);
    gui_menu_add(goMenu, @"Previous Invalid", @"r", (int)cmd, CMD_NAV_INV_PREV);
    gui_menu_add(goMenu, @"Fold Region", @"t", (int)cmd, CMD_FOLD);

    NSApp.mainMenu = mainMenu;
    g_menu_ready = 1;
}

static uint32_t fb_push_str(const char *s) {
    size_t n;
    uint32_t off;
    if (!s) s = "";
    n = strlen(s) + 1;
    if (g_str_used + n > FB_STR_POOL) return 0;
    off = (uint32_t)g_str_used;
    memcpy(g_strs + g_str_used, s, n);
    g_str_used += n;
    return off;
}

static void fb_op(FbOp op) {
    if (g_nop >= FB_OP_MAX) return;
    g_ops[g_nop++] = op;
}

static int map_keycode(unsigned short kc) {
    /* Carbon / Cocoa virtual keycodes → KEY_* (gui_plat.h) */
    switch (kc) {
    case 0x35: return KEY_ESCAPE;
    case 0x24: case 0x4C: return KEY_ENTER;
    case 0x30: return KEY_TAB;
    case 0x33: return KEY_BACKSPACE;
    case 0x75: return KEY_DELETE;
    case 0x7B: return KEY_LEFT;
    case 0x7C: return KEY_RIGHT;
    case 0x7D: return KEY_DOWN;
    case 0x7E: return KEY_UP;
    case 0x73: return KEY_HOME;
    case 0x77: return KEY_END;
    case 0x38: return KEY_LEFT_SHIFT;
    case 0x3C: return KEY_RIGHT_SHIFT;
    case 0x3B: return KEY_LEFT_CONTROL;
    case 0x3E: return KEY_RIGHT_CONTROL;
    case 0x3A: return KEY_LEFT_ALT;
    case 0x3D: return KEY_RIGHT_ALT;
    case 0x37: return KEY_LEFT_SUPER;
    case 0x36: return KEY_RIGHT_SUPER;
    case 0x00: return KEY_A;
    case 0x0B: return KEY_B;
    case 0x08: return KEY_C;
    case 0x02: return KEY_D;
    case 0x0E: return KEY_E;
    case 0x03: return KEY_F;
    case 0x05: return KEY_G;
    case 0x26: return KEY_J;
    case 0x28: return KEY_K;
    case 0x25: return KEY_L;
    case 0x2D: return KEY_N;
    case 0x1F: return KEY_O;
    case 0x23: return KEY_P;
    case 0x0C: return KEY_Q;
    case 0x0F: return KEY_R;
    case 0x01: return KEY_S;
    case 0x11: return KEY_T;
    case 0x20: return KEY_U;
    case 0x09: return KEY_V;
    case 0x0D: return KEY_W;
    case 0x07: return KEY_X;
    case 0x10: return KEY_Y;
    case 0x06: return KEY_Z;
    case 0x18: return KEY_EQUAL;
    case 0x2A: return KEY_BACKSLASH;
    default: return -1;
    }
}

static void fb_clear_frame_input(void) {
    memset(g_key_pressed, 0, sizeof(g_key_pressed));
    memset(g_key_repeat, 0, sizeof(g_key_repeat));
    g_mouse_pressed = 0;
    g_mouse_released = 0;
    g_wheel_x = g_wheel_y = 0;
    g_nchar = g_char_rd = 0;
    g_resized = 0;
}

/* Trackpad points → notches. /40 made tilt die and vertical crawl. */
enum { FB_WHEEL_CAP = 8 };
#define FB_WHEEL_PREC_X 10.0f
#define FB_WHEEL_PREC_Y 16.0f

/* Turn carry into discrete notches for this frame. Leftover stays. */
static void fb_wheel_commit(void) {
    int sx = 0, sy = 0;
    while (g_wheel_carry_x >= 1.0f && sx < FB_WHEEL_CAP) {
        sx++;
        g_wheel_carry_x -= 1.0f;
    }
    while (g_wheel_carry_x <= -1.0f && sx > -FB_WHEEL_CAP) {
        sx--;
        g_wheel_carry_x += 1.0f;
    }
    while (g_wheel_carry_y >= 1.0f && sy < FB_WHEEL_CAP) {
        sy++;
        g_wheel_carry_y -= 1.0f;
    }
    while (g_wheel_carry_y <= -1.0f && sy > -FB_WHEEL_CAP) {
        sy--;
        g_wheel_carry_y += 1.0f;
    }
    /* X is a smaller point threshold than Y (tilt). A vertical swipe then
     * emits more X notches and the editor panned to the end of the row. */
    if (sy != 0) {
        sx = 0;
        g_wheel_carry_x = 0;
    }
    g_wheel_x = (float)sx;
    g_wheel_y = (float)sy;
}

static void fb_sample_mouse(NSEvent *ev) {
    NSPoint p;
    if (!g_view || !ev) return;
    p = [g_view convertPoint:ev.locationInWindow fromView:nil];
    g_mouse_x = (int)p.x;
    g_mouse_y = (int)p.y;
}

static void fb_handle_event(NSEvent *ev) {
    int k;
    if (!ev) return;
    switch (ev.type) {
    case NSEventTypeKeyDown:
        k = map_keycode(ev.keyCode);
        /* Cmd+letter goes to NSMenu — except Cmd-O aliases Browse (no menu key).
         * Super+arrows are Home/End / line motion in the editor. */
        if (ev.modifierFlags & NSEventModifierFlagCommand) {
            if (k == KEY_O && !(ev.modifierFlags & NSEventModifierFlagShift)) {
                g_menu_cmd = CMD_BROWSE;
                break;
            }
            if (k != KEY_LEFT && k != KEY_RIGHT && k != KEY_UP && k != KEY_DOWN &&
                k != KEY_HOME && k != KEY_END)
                break;
        }
        if (k >= 0 && k < FB_KEY_MAX) {
            if (g_key_down[k]) g_key_repeat[k] = 1;
            else g_key_pressed[k] = 1;
            g_key_down[k] = 1;
            if (k == g_exit_key) g_should_close = 1;
        }
        /* Arrows / Home / End / Delete arrive as NS*FunctionKey (U+F700+)
         * in characters. Those must not enter the insert queue — left/right
         * used to type them because the editor only drained chars after
         * up/down. */
        if (!(ev.modifierFlags & NSEventModifierFlagControl) &&
            k != KEY_LEFT && k != KEY_RIGHT && k != KEY_UP && k != KEY_DOWN &&
            k != KEY_HOME && k != KEY_END && k != KEY_TAB &&
            k != KEY_ESCAPE && k != KEY_ENTER && k != KEY_BACKSPACE &&
            k != KEY_DELETE) {
            NSString *chars = ev.characters;
            if (chars.length > 0) {
                unichar u = [chars characterAtIndex:0];
                if (u >= 32 && u != 127 && u < 0xF700 && g_nchar < 64)
                    g_chars[g_nchar++] = (int)u;
            }
        }
        break;
    case NSEventTypeKeyUp:
        k = map_keycode(ev.keyCode);
        if (k >= 0 && k < FB_KEY_MAX) g_key_down[k] = 0;
        break;
    case NSEventTypeFlagsChanged: {
        NSEventModifierFlags m = ev.modifierFlags;
        g_key_down[KEY_LEFT_SHIFT] = g_key_down[KEY_RIGHT_SHIFT] =
            !!(m & NSEventModifierFlagShift);
        g_key_down[KEY_LEFT_CONTROL] = g_key_down[KEY_RIGHT_CONTROL] =
            !!(m & NSEventModifierFlagControl);
        g_key_down[KEY_LEFT_ALT] = g_key_down[KEY_RIGHT_ALT] =
            !!(m & NSEventModifierFlagOption);
        g_key_down[KEY_LEFT_SUPER] = g_key_down[KEY_RIGHT_SUPER] =
            !!(m & NSEventModifierFlagCommand);
        break;
    }
    case NSEventTypeLeftMouseDown:
    case NSEventTypeLeftMouseUp:
    case NSEventTypeMouseMoved:
    case NSEventTypeLeftMouseDragged:
    case NSEventTypeRightMouseDragged:
        fb_sample_mouse(ev);
        if (ev.type == NSEventTypeLeftMouseDown) {
            g_mouse_down = 1;
            g_mouse_pressed = 1;
        } else if (ev.type == NSEventTypeLeftMouseUp) {
            g_mouse_down = 0;
            g_mouse_released = 1;
        }
        break;
    case NSEventTypeScrollWheel: {
        CGFloat dx = ev.scrollingDeltaX;
        CGFloat dy = ev.scrollingDeltaY;
        if (dx == 0 && dy == 0) {
            dx = ev.deltaX;
            dy = ev.deltaY;
        }
        /* Precise = points. Mouse notches stay 1:1. */
        if (ev.hasPreciseScrollingDeltas) {
            g_wheel_carry_x += (float)(dx / (double)FB_WHEEL_PREC_X);
            g_wheel_carry_y += (float)(dy / (double)FB_WHEEL_PREC_Y);
        } else {
            g_wheel_carry_x += (float)dx;
            g_wheel_carry_y += (float)dy;
        }
        break;
    }
    default:
        break;
    }
}

static void fb_pump_events(double timeout_s) {
    @autoreleasepool {
        NSDate *until = timeout_s > 0
                            ? [NSDate dateWithTimeIntervalSinceNow:timeout_s]
                            : [NSDate distantPast];
        for (;;) {
            NSEvent *ev = [NSApp nextEventMatchingMask:NSEventMaskAny
                                             untilDate:until
                                                inMode:NSDefaultRunLoopMode
                                               dequeue:YES];
            if (!ev) break;
            fb_handle_event(ev);
            [NSApp sendEvent:ev];
            until = [NSDate distantPast];
        }
    }
}

static CGColorRef fb_cgcolor(Color c) {
    CGFloat comps[4] = {c.r / 255.0, c.g / 255.0, c.b / 255.0, c.a / 255.0};
    CGColorSpaceRef sp = CGColorSpaceCreateDeviceRGB();
    CGColorRef col = CGColorCreate(sp, comps);
    CGColorSpaceRelease(sp);
    return col;
}

static void fb_replay(CGContextRef ctx) {
    int i;
    for (i = 0; i < g_nop; i++) {
        FbOp *o = &g_ops[i];
        switch (o->kind) {
        case FB_CLEAR:
            CGContextSetRGBFillColor(ctx, o->c.r / 255.0, o->c.g / 255.0,
                                     o->c.b / 255.0, o->c.a / 255.0);
            CGContextFillRect(ctx, CGRectMake(0, 0, g_ww, g_hh));
            break;
        case FB_RECT:
            CGContextSetRGBFillColor(ctx, o->c.r / 255.0, o->c.g / 255.0,
                                     o->c.b / 255.0, o->c.a / 255.0);
            CGContextFillRect(ctx, CGRectMake(o->x, o->y, o->w, o->h));
            break;
        case FB_RECT_LINES:
            CGContextSetRGBStrokeColor(ctx, o->c.r / 255.0, o->c.g / 255.0,
                                       o->c.b / 255.0, o->c.a / 255.0);
            CGContextStrokeRect(ctx, CGRectMake(o->x + 0.5, o->y + 0.5,
                                                o->w - 1, o->h - 1));
            break;
        case FB_LINE:
            CGContextSetRGBStrokeColor(ctx, o->c.r / 255.0, o->c.g / 255.0,
                                       o->c.b / 255.0, o->c.a / 255.0);
            CGContextBeginPath(ctx);
            CGContextMoveToPoint(ctx, o->x + 0.5, o->y + 0.5);
            CGContextAddLineToPoint(ctx, o->x1 + 0.5, o->y1 + 0.5);
            CGContextStrokePath(ctx);
            break;
        case FB_TEXT: {
            CTFontRef base = (CTFontRef)o->font.ct;
            CTFontRef font = base;
            CGColorRef col;
            CFStringRef s;
            CFStringRef keys[2];
            CFTypeRef vals[2];
            CFDictionaryRef attrs;
            CFAttributedStringRef as;
            CTLineRef line;
            CGFloat ascent = 0, descent = 0, leading = 0;
            const char *txt = g_strs + o->str_off;
            if (!base) break;
            if (fabsf(o->fontSize - o->font.size) > 0.5f)
                font = CTFontCreateCopyWithAttributes(base, o->fontSize, NULL, NULL);
            col = fb_cgcolor(o->c);
            s = CFStringCreateWithCString(NULL, txt, kCFStringEncodingUTF8);
            if (!s) {
                CGColorRelease(col);
                if (font != base) CFRelease(font);
                break;
            }
            keys[0] = kCTFontAttributeName;
            keys[1] = kCTForegroundColorAttributeName;
            vals[0] = font;
            vals[1] = col;
            attrs = CFDictionaryCreate(NULL, (const void **)keys, (const void **)vals, 2,
                                       &kCFTypeDictionaryKeyCallBacks,
                                       &kCFTypeDictionaryValueCallBacks);
            as = CFAttributedStringCreate(NULL, s, attrs);
            line = CTLineCreateWithAttributedString(as);
            CTLineGetTypographicBounds(line, &ascent, &descent, &leading);
            CGContextSaveGState(ctx);
            CGContextSetTextMatrix(ctx, CGAffineTransformMakeScale(1, -1));
            CGContextSetTextPosition(ctx, o->x, o->y + ascent);
            CTLineDraw(line, ctx);
            CGContextRestoreGState(ctx);
            CFRelease(line);
            CFRelease(as);
            CFRelease(attrs);
            CFRelease(s);
            CGColorRelease(col);
            if (font != base) CFRelease(font);
            break;
        }
        case FB_CLIP:
            CGContextSaveGState(ctx);
            CGContextClipToRect(ctx, CGRectMake(o->x, o->y, o->w, o->h));
            break;
        case FB_CLIP_END:
            CGContextRestoreGState(ctx);
            break;
        }
    }
}

@implementation FbView
- (BOOL)isFlipped { return YES; }
- (BOOL)acceptsFirstResponder { return YES; }
- (BOOL)acceptsFirstMouse:(NSEvent *)ev {
    (void)ev;
    return YES;
}
- (void)drawRect:(NSRect)dirty {
    @autoreleasepool {
        CGContextRef ctx;
        (void)dirty;
        ctx = [[NSGraphicsContext currentContext] CGContext];
        if (ctx) fb_replay(ctx);
    }
}
- (void)viewDidEndLiveResize {
    NSSize s = self.bounds.size;
    g_ww = (int)s.width;
    g_hh = (int)s.height;
    g_resized = 1;
    [self setNeedsDisplay:YES];
}
@end

@implementation FbAppDelegate
- (void)menuCommand:(id)sender {
    NSMenuItem *it = (NSMenuItem *)sender;
    if (!it) return;
    g_menu_cmd = (int)it.tag;
}
- (BOOL)windowShouldClose:(NSWindow *)sender {
    (void)sender;
    g_should_close = 1;
    return NO; /* host owns quit / ask_quit */
}
- (void)windowWillClose:(NSNotification *)n {
    (void)n;
    g_should_close = 1;
}
- (void)windowDidResize:(NSNotification *)n {
    NSSize s;
    (void)n;
    if (!g_view) return;
    s = g_view.bounds.size;
    g_ww = (int)s.width;
    g_hh = (int)s.height;
    g_resized = 1;
    if (g_live_resize) g_live_resize();
}
@end

void fb_set_live_resize(void (*fn)(void)) { g_live_resize = fn; }

void SetTraceLogLevel(int level) { (void)level; }
void SetConfigFlags(unsigned int flags) { g_flags = flags; }
void SetExitKey(int key) { g_exit_key = key; }
void SetTargetFPS(int fps) { g_fps = fps > 0 ? fps : 60; }

void InitWindow(int width, int height, const char *title) {
    NSRect frame;
    NSUInteger style;
    FbView *view;
    FbAppDelegate *del;
    g_ww = width > 0 ? width : 960;
    g_hh = height > 0 ? height : 640;
    [NSApplication sharedApplication];
    [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
    del = [FbAppDelegate new];
    g_app_delegate = del;
    NSApp.delegate = del;
    style = NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
            NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable;
    frame = NSMakeRect(0, 0, g_ww, g_hh);
    g_window = [[NSWindow alloc] initWithContentRect:frame
                                           styleMask:style
                                             backing:NSBackingStoreBuffered
                                               defer:NO];
    g_window.title = title ? [NSString stringWithUTF8String:title] : @"cctext";
    g_window.delegate = del;
    view = [[FbView alloc] initWithFrame:frame];
    g_view = view;
    g_window.contentView = view;
    [g_window center];
    [g_window makeKeyAndOrderFront:nil];
    [NSApp activateIgnoringOtherApps:YES];
    gui_menu_install();
    g_ready = 1;
    g_should_close = 0;
    (void)g_flags;
    [g_view setNeedsDisplay:YES];
    fb_pump_events(0);
}

void CloseWindow(void) {
    if (g_window) {
        [g_window close];
        g_window = nil;
        g_view = nil;
    }
    g_ready = 0;
}

int WindowShouldClose(void) { return g_should_close; }
int IsWindowReady(void) { return g_ready; }
int IsWindowResized(void) {
    int r = g_resized;
    g_resized = 0;
    return r;
}
void *GetWindowHandle(void) { return (__bridge void *)g_window; }
int GetScreenWidth(void) { return g_ww; }
int GetScreenHeight(void) { return g_hh; }

Vector2 GetWindowScaleDPI(void) {
    CGFloat s = g_window ? g_window.backingScaleFactor : 1.0;
    if (s < 1.0) s = 1.0;
    return (Vector2){(float)s, (float)s};
}

void BeginDrawing(void) {
    g_drawing = 1;
    g_nop = 0;
    g_str_used = 1; /* keep 0 as empty */
    g_strs[0] = 0;
}

void EndDrawing(void) {
    g_drawing = 0;
    if (g_view) [g_view setNeedsDisplay:YES];
    /* Host already sampled this frame; clear edges then wait for the next. */
    fb_clear_frame_input();
    fb_pump_events(1.0 / (double)g_fps);
}

void gui_input_begin_frame(void) {
    fb_pump_events(0);
    fb_wheel_commit();
}

void gui_input_consume(void) {
    fb_clear_frame_input();
}

void PollInputEvents(void) {
    fb_pump_events(0);
    /* keep pressed/repeat for this poll; host may call again */
}

void WaitTime(double seconds) {
    if (seconds < 0) seconds = 0;
    fb_pump_events(seconds);
}

void ClearBackground(Color color) {
    FbOp o = {0};
    o.kind = FB_CLEAR;
    o.c = color;
    fb_op(o);
}

void BeginScissorMode(int x, int y, int w, int h) {
    FbOp o = {0};
    o.kind = FB_CLIP;
    o.x = x;
    o.y = y;
    o.w = w;
    o.h = h;
    fb_op(o);
}

void EndScissorMode(void) {
    FbOp o = {0};
    o.kind = FB_CLIP_END;
    fb_op(o);
}

void DrawRectangle(int x, int y, int w, int h, Color color) {
    FbOp o = {0};
    o.kind = FB_RECT;
    o.x = x;
    o.y = y;
    o.w = w;
    o.h = h;
    o.c = color;
    fb_op(o);
}

void DrawRectangleLines(int x, int y, int w, int h, Color color) {
    FbOp o = {0};
    o.kind = FB_RECT_LINES;
    o.x = x;
    o.y = y;
    o.w = w;
    o.h = h;
    o.c = color;
    fb_op(o);
}

void DrawLine(int x0, int y0, int x1, int y1, Color color) {
    FbOp o = {0};
    o.kind = FB_LINE;
    o.x = x0;
    o.y = y0;
    o.x1 = x1;
    o.y1 = y1;
    o.c = color;
    fb_op(o);
}

void DrawTextEx(Font font, const char *text, Vector2 pos, float fontSize,
                float spacing, Color tint) {
    FbOp o = {0};
    (void)spacing;
    o.kind = FB_TEXT;
    o.x = (int)pos.x;
    o.y = (int)pos.y;
    o.font = font;
    o.fontSize = fontSize;
    o.c = tint;
    o.str_off = fb_push_str(text);
    fb_op(o);
}

void DrawText(const char *text, int x, int y, int fontSize, Color color) {
    Font f = GetFontDefault();
    DrawTextEx(f, text, (Vector2){(float)x, (float)y}, (float)fontSize, 0, color);
}

Vector2 MeasureTextEx(Font font, const char *text, float fontSize, float spacing) {
    CTFontRef base = (CTFontRef)font.ct;
    CTFontRef use;
    CFStringRef s;
    CFDictionaryRef attrs;
    CFAttributedStringRef as;
    CTLineRef line;
    CGFloat w = 0, ascent = 0, descent = 0, leading = 0;
    CFStringRef keys[1];
    CFTypeRef vals[1];
    Vector2 out = {0, 0};
    (void)spacing;
    if (!text) return out;
    if (!base) {
        Font d = GetFontDefault();
        base = (CTFontRef)d.ct;
    }
    if (!base) return out;
    use = base;
    if (fabsf(fontSize - font.size) > 0.5f)
        use = CTFontCreateCopyWithAttributes(base, fontSize, NULL, NULL);
    s = CFStringCreateWithCString(NULL, text, kCFStringEncodingUTF8);
    if (!s) {
        if (use != base) CFRelease(use);
        return out;
    }
    keys[0] = kCTFontAttributeName;
    vals[0] = use;
    attrs = CFDictionaryCreate(NULL, (const void **)keys, (const void **)vals, 1,
                               &kCFTypeDictionaryKeyCallBacks,
                               &kCFTypeDictionaryValueCallBacks);
    as = CFAttributedStringCreate(NULL, s, attrs);
    line = CTLineCreateWithAttributedString(as);
    w = CTLineGetTypographicBounds(line, &ascent, &descent, &leading);
    out.x = (float)w;
    out.y = (float)(ascent + descent + leading);
    CFRelease(line);
    CFRelease(as);
    CFRelease(attrs);
    CFRelease(s);
    if (use != base) CFRelease(use);
    return out;
}

Font LoadFontEx(const char *fileName, int fontSize, int *codepoints, int codepointCount) {
    Font f = {0};
    CTFontRef ct = NULL;
    (void)codepoints;
    (void)codepointCount;
    f.size = (float)(fontSize > 0 ? fontSize : 16);
    if (fileName && fileName[0]) {
        CFStringRef path = CFStringCreateWithCString(NULL, fileName, kCFStringEncodingUTF8);
        CFURLRef url = CFURLCreateWithFileSystemPath(NULL, path, kCFURLPOSIXPathStyle, false);
        CGDataProviderRef prov = url ? CGDataProviderCreateWithURL(url) : NULL;
        CGFontRef cgf = prov ? CGFontCreateWithDataProvider(prov) : NULL;
        if (cgf) ct = CTFontCreateWithGraphicsFont(cgf, f.size, NULL, NULL);
        if (cgf) CFRelease(cgf);
        if (prov) CGDataProviderRelease(prov);
        if (url) CFRelease(url);
        if (path) CFRelease(path);
    }
    if (!ct) {
        /* Fall back: basename without path / extension as PostScript name guess. */
        ct = CTFontCreateWithName(CFSTR("SF Mono"), f.size, NULL);
        if (!ct) ct = CTFontCreateUIFontForLanguage(kCTFontUIFontUserFixedPitch, f.size, NULL);
    }
    f.ct = (void *)ct;
    f.glyphCount = ct ? 256 : 0;
    f.texture.id = ct ? 1 : 0;
    return f;
}

Font GetFontDefault(void) {
    static Font def;
    if (!def.ct) {
        def.size = 16;
        def.ct = (void *)CTFontCreateUIFontForLanguage(kCTFontUIFontUserFixedPitch, 16, NULL);
        def.glyphCount = 256;
        def.texture.id = 1;
    }
    return def;
}

void UnloadFont(Font font) {
    if (font.ct) CFRelease((CTFontRef)font.ct);
}

Font gui_derive_font(Font base, int bold, int italic) {
    CTFontRef src = (CTFontRef)base.ct;
    CTFontSymbolicTraits want = 0;
    CTFontRef derived = NULL;
    Font out = base;
    if (!src) return base;
    if (bold) want |= kCTFontBoldTrait;
    if (italic) want |= kCTFontItalicTrait;
    if (!want) return base;
    derived = CTFontCreateCopyWithSymbolicTraits(src, base.size, NULL, want, want);
    if (!derived && bold && !italic) {
        CFStringRef fam = CTFontCopyFamilyName(src);
        if (fam) {
            CFMutableStringRef name = CFStringCreateMutable(NULL, 0);
            CFStringAppend(name, fam);
            CFStringAppend(name, CFSTR(" Bold"));
            derived = CTFontCreateWithName(name, base.size, NULL);
            CFRelease(name);
            CFRelease(fam);
        }
    }
    if (!derived) return base;
    out.ct = (void *)derived;
    out.texture.id = 1;
    out.glyphCount = base.glyphCount ? base.glyphCount : 256;
    return out;
}

void SetTextureFilter(Texture2D texture, int filter) {
    (void)texture;
    (void)filter;
}

int FileExists(const char *fileName) {
    return fileName && fileName[0] && access(fileName, F_OK) == 0;
}

int IsKeyDown(int key) {
    if (key < 0 || key >= FB_KEY_MAX) return 0;
    return g_key_down[key];
}
int IsKeyPressed(int key) {
    if (key < 0 || key >= FB_KEY_MAX) return 0;
    return g_key_pressed[key];
}
int IsKeyPressedRepeat(int key) {
    if (key < 0 || key >= FB_KEY_MAX) return 0;
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
    if (s)
        [pb setString:s forType:NSPasteboardTypeString];
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

void gui_menu_install(void) {
    gui_menu_build();
}

int gui_menu_poll_cmd(void) {
    int c = g_menu_cmd;
    g_menu_cmd = CMD_NONE;
    return c;
}

int gui_menu_pending(void) {
    return g_menu_cmd != CMD_NONE;
}

void gui_clear_close(void) {
    g_should_close = 0;
}

int gui_alert_unsaved(size_t nfiles) {
    NSAlert *alert;
    NSModalResponse rc;
    NSString *msg;
    @autoreleasepool {
        alert = [[NSAlert alloc] init];
        alert.alertStyle = NSAlertStyleWarning;
        alert.messageText = @"Unsaved changes";
        if (nfiles <= 1)
            msg = @"1 file has unsaved edits. Save before quitting?";
        else
            msg = [NSString stringWithFormat:
                      @"%zu files have unsaved edits. Save before quitting?",
                      nfiles];
        alert.informativeText = msg;
        [alert addButtonWithTitle:@"Save"];
        [alert addButtonWithTitle:@"Don't Save"];
        [alert addButtonWithTitle:@"Cancel"];
        rc = [alert runModal];
        if (rc == NSAlertFirstButtonReturn) return 1;
        if (rc == NSAlertSecondButtonReturn) return 2;
        return 0;
    }
}

int gui_save_panel(const char *dir, char *out, size_t n) {
    NSSavePanel *panel;
    NSModalResponse rc;
    NSURL *url;
    if (!out || n == 0) return 0;
    out[0] = 0;
    @autoreleasepool {
        panel = [NSSavePanel savePanel];
        panel.canCreateDirectories = YES;
        if (dir && dir[0] == '/')
            panel.directoryURL = [NSURL fileURLWithPath:[NSString stringWithUTF8String:dir]];
        rc = [panel runModal];
        if (rc != NSModalResponseOK) return 0;
        url = panel.URL;
        if (!url || !url.path) return 0;
        snprintf(out, n, "%s", url.path.UTF8String);
        return out[0] ? 1 : 0;
    }
}
