/*
 * Spike: pure Cocoa + Core Text (no Fenster buffer, no Raylib).
 *
 *   clang -O2 -fobjc-arc proto/cocoa_ct.m \
 *     -framework Cocoa -framework CoreText -framework CoreGraphics \
 *     -o bin/cocoa_ct
 *   ./bin/cocoa_ct                 # CTLineDraw into view
 *   COCOA_CT_EMPTY=1 ./bin/cocoa_ct
 *   footprint -p $(pgrep -n cocoa_ct)
 */
#import <Cocoa/Cocoa.h>
#import <CoreText/CoreText.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

enum { W = 960, H = 640 };

static int g_empty;

static CGFloat ct_measure(CTFontRef font, const char *utf8, size_t n) {
    char tmp[512];
    CFStringRef s;
    CFDictionaryRef attrs;
    CFAttributedStringRef as;
    CTLineRef line;
    CGFloat w;
    CFStringRef keys[1];
    CFTypeRef vals[1];
    if (n >= sizeof(tmp)) n = sizeof(tmp) - 1;
    memcpy(tmp, utf8, n);
    tmp[n] = 0;
    s = CFStringCreateWithCString(NULL, tmp, kCFStringEncodingUTF8);
    keys[0] = kCTFontAttributeName;
    vals[0] = font;
    attrs = CFDictionaryCreate(NULL, (const void **)keys, (const void **)vals, 1,
                               &kCFTypeDictionaryKeyCallBacks,
                               &kCFTypeDictionaryValueCallBacks);
    as = CFAttributedStringCreate(NULL, s, attrs);
    line = CTLineCreateWithAttributedString(as);
    w = CTLineGetTypographicBounds(line, NULL, NULL, NULL);
    CFRelease(line);
    CFRelease(as);
    CFRelease(attrs);
    CFRelease(s);
    return w;
}

static void ct_draw(CGContextRef ctx, CTFontRef font, CGColorRef color,
                    const char *utf8, CGFloat x, CGFloat y) {
    CFStringRef s = CFStringCreateWithCString(NULL, utf8, kCFStringEncodingUTF8);
    CFStringRef keys[2] = {kCTFontAttributeName, kCTForegroundColorAttributeName};
    CFTypeRef vals[2] = {font, color};
    CFDictionaryRef attrs = CFDictionaryCreate(
        NULL, (const void **)keys, (const void **)vals, 2,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFAttributedStringRef as = CFAttributedStringCreate(NULL, s, attrs);
    CTLineRef line = CTLineCreateWithAttributedString(as);
    CGFloat ascent = 0, descent = 0, leading = 0;
    CTLineGetTypographicBounds(line, &ascent, &descent, &leading);
    /* View is flipped (y down). CT baseline needs scale(1,-1). */
    CGContextSetTextMatrix(ctx, CGAffineTransformMakeScale(1, -1));
    CGContextSetTextPosition(ctx, x, y + ascent);
    CTLineDraw(line, ctx);
    CFRelease(line);
    CFRelease(as);
    CFRelease(attrs);
    CFRelease(s);
}

@interface CtView : NSView
@end

@implementation CtView

- (BOOL)isFlipped {
    return YES;
}

- (void)drawRect:(NSRect)dirty {
    CTFontRef mono, prose;
    CGColorSpaceRef space;
    CGColorRef fg, key, str, num, dim;
    CGFloat white[4] = {0.90, 0.90, 0.92, 1};
    CGFloat cyan[4] = {0.31, 0.78, 0.86, 1};
    CGFloat green[4] = {0.47, 0.78, 0.47, 1};
    CGFloat amber[4] = {0.86, 0.71, 0.31, 1};
    CGFloat dimc[4] = {0.55, 0.55, 0.60, 1};
    CGContextRef ctx;
    CGFloat x;
    const char *line0;
    const char *prefix;
    (void)dirty;

    [[NSColor colorWithCalibratedRed:0.094 green:0.094 blue:0.110 alpha:1] setFill];
    NSRectFill(self.bounds);

    if (g_empty) {
        [@"empty NSView (COCOA_CT_EMPTY=1) — Esc / close to quit"
            drawAtPoint:NSMakePoint(16, 16)
         withAttributes:@{
             NSForegroundColorAttributeName : [NSColor lightGrayColor],
             NSFontAttributeName :
                 [NSFont monospacedSystemFontOfSize:14 weight:NSFontWeightRegular]
         }];
        return;
    }

    ctx = [[NSGraphicsContext currentContext] CGContext];
    if (!ctx) return;

    mono = CTFontCreateWithName(CFSTR("SF Mono"), 16, NULL);
    if (!mono)
        mono = CTFontCreateUIFontForLanguage(kCTFontUIFontUserFixedPitch, 16, NULL);
    prose = CTFontCreateWithName(CFSTR("Georgia"), 18, NULL);
    if (!prose)
        prose = CTFontCreateUIFontForLanguage(kCTFontUIFontSystem, 18, NULL);

    space = CGColorSpaceCreateDeviceRGB();
    fg = CGColorCreate(space, white);
    key = CGColorCreate(space, cyan);
    str = CGColorCreate(space, green);
    num = CGColorCreate(space, amber);
    dim = CGColorCreate(space, dimc);
    CGColorSpaceRelease(space);

    ct_draw(ctx, mono, dim,
            "cocoa + Core Text spike  (Esc quit) — CTLineDraw into view context",
            12, 8);
    ct_draw(ctx, mono, dim, "mono / themed runs (no CPU framebuffer):", 16, 56);

    x = 16;
    ct_draw(ctx, mono, fg, "{", x, 84);
    x += ct_measure(mono, "{", 1);
    ct_draw(ctx, mono, key, "\"id\"", x, 84);
    x += ct_measure(mono, "\"id\"", 4);
    ct_draw(ctx, mono, fg, ": ", x, 84);
    x += ct_measure(mono, ": ", 2);
    ct_draw(ctx, mono, num, "0", x, 84);
    x += ct_measure(mono, "0", 1);
    ct_draw(ctx, mono, fg, ", ", x, 84);
    x += ct_measure(mono, ", ", 2);
    ct_draw(ctx, mono, key, "\"name\"", x, 84);
    x += ct_measure(mono, "\"name\"", 6);
    ct_draw(ctx, mono, fg, ": ", x, 84);
    x += ct_measure(mono, ": ", 2);
    ct_draw(ctx, mono, str, "\"item_0\"", x, 84);
    x += ct_measure(mono, "\"item_0\"", 8);
    ct_draw(ctx, mono, fg, ", ", x, 84);
    x += ct_measure(mono, ", ", 2);
    ct_draw(ctx, mono, key, "\"n\"", x, 84);
    x += ct_measure(mono, "\"n\"", 3);
    ct_draw(ctx, mono, fg, ": ", x, 84);
    x += ct_measure(mono, ": ", 2);
    ct_draw(ctx, mono, num, "-2.5", x, 84);
    x += ct_measure(mono, "-2.5", 4);
    ct_draw(ctx, mono, fg, "}", x, 84);

    line0 = "{\"id\": 0, \"name\": \"item_0\", \"n\": -2.5}";
    prefix = "{\"id\": ";
    ct_draw(ctx, mono, dim, "caret from CTLineGetTypographicBounds(prefix):", 16, 120);
    ct_draw(ctx, mono, fg, line0, 16, 148);
    {
        CGFloat cx = 16 + ct_measure(mono, line0, strlen(prefix));
        [[NSColor colorWithCalibratedRed:1 green:0.78 blue:0.31 alpha:1] setFill];
        NSRectFill(NSMakeRect(cx, 148, 2, 18));
    }

    ct_draw(ctx, prose, dim, "prose (Georgia):", 16, 200);
    ct_draw(ctx, prose, fg, "The quick brown fox jumps over the lazy dog.", 16, 230);
    ct_draw(ctx, mono, dim, "cluster (e + combining acute):", 16, 280);
    ct_draw(ctx, mono, fg, "cafe\xCC\x81  (cafe + U+0301)", 16, 308);
    ct_draw(ctx, mono, dim, "Compare footprint to fenster_ct / empty Raylib.", 16, 380);

    CFRelease(mono);
    CFRelease(prose);
    CGColorRelease(fg);
    CGColorRelease(key);
    CGColorRelease(str);
    CGColorRelease(num);
    CGColorRelease(dim);
}

@end

@interface AppDelegate : NSObject <NSApplicationDelegate, NSWindowDelegate>
@property (strong) NSWindow *window;
@end

static AppDelegate *g_delegate;

@implementation AppDelegate

- (void)applicationDidFinishLaunching:(NSNotification *)note {
    NSRect frame = NSMakeRect(0, 0, W, H);
    NSUInteger style = NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                       NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable;
    CtView *view;
    (void)note;
    self.window = [[NSWindow alloc] initWithContentRect:frame
                                              styleMask:style
                                                backing:NSBackingStoreBuffered
                                                  defer:NO];
    self.window.title = g_empty ? @"cctext cocoa empty" : @"cctext cocoa+CT";
    self.window.delegate = self;
    view = [[CtView alloc] initWithFrame:frame];
    self.window.contentView = view;
    [self.window center];
    [self.window makeKeyAndOrderFront:nil];
    [NSApp activateIgnoringOtherApps:YES];
    fprintf(stderr, "cocoa_ct: pid %d  %dx%d  %s\n", getpid(), W, H,
            g_empty ? "EMPTY view" : "CTLineDraw into view");
}

- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication *)sender {
    (void)sender;
    return YES;
}

- (void)windowWillClose:(NSNotification *)note {
    (void)note;
    [NSApp terminate:nil];
}

@end

@interface KeyApp : NSApplication
@end

@implementation KeyApp
- (void)sendEvent:(NSEvent *)event {
    if (event.type == NSEventTypeKeyDown && event.keyCode == 53) {
        [NSApp terminate:nil];
        return;
    }
    [super sendEvent:event];
}
@end

int main(int argc, const char **argv) {
    (void)argc;
    (void)argv;
    g_empty = getenv("COCOA_CT_EMPTY") && getenv("COCOA_CT_EMPTY")[0] != '0';
    @autoreleasepool {
        [KeyApp sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
        g_delegate = [AppDelegate new];
        NSApp.delegate = g_delegate;
        [NSApp run];
    }
    return 0;
}
