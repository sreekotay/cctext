/*
 * Platform draw/input surface for cctext-ui (libui-ng; ui_plat.c + ui_os_*).
 * Fixed raylib-shaped names keep the editor TUs simple. Paint runs
 * inside the libui area Draw callback — no display list.
 */
#ifndef RTX_GUI_PLAT_H
#define RTX_GUI_PLAT_H

#include <stddef.h>
#include <stdint.h>

typedef struct Vector2 {
    float x;
    float y;
} Vector2;

typedef struct Color {
    unsigned char r, g, b, a;
} Color;

typedef struct Texture2D {
    unsigned int id;
} Texture2D;

typedef struct Font {
    void *ct; /* CTFontRef */
    float size;
    int glyphCount;
    Texture2D texture;
} Font;

enum {
    KEY_EQUAL = 61,
    KEY_A = 65, KEY_B = 66, KEY_C = 67, KEY_D = 68, KEY_E = 69,
    KEY_F = 70, KEY_G = 71, KEY_H = 72, KEY_J = 74, KEY_K = 75, KEY_L = 76,
    KEY_M = 77, KEY_N = 78, KEY_O = 79, KEY_P = 80, KEY_Q = 81, KEY_R = 82, KEY_S = 83,
    KEY_T = 84, KEY_U = 85, KEY_V = 86, KEY_W = 87, KEY_X = 88,
    KEY_Y = 89, KEY_Z = 90, KEY_BACKSLASH = 92,
    KEY_GRAVE = 96,
    KEY_PERIOD = 46, KEY_SLASH = 47,
    KEY_I = 73, KEY_APOSTROPHE = 39, KEY_COMMA = 44, KEY_MINUS = 45,
    KEY_SEMICOLON = 59, KEY_LEFT_BRACKET = 91, KEY_RIGHT_BRACKET = 93,
    KEY_F1 = 290, KEY_F2 = 291, KEY_F3 = 292, KEY_F4 = 293, KEY_F5 = 294,
    KEY_F6 = 295, KEY_F7 = 296, KEY_F8 = 297, KEY_F9 = 298, KEY_F10 = 299,
    KEY_F11 = 300, KEY_F12 = 301,
    KEY_0 = 48, KEY_1 = 49, KEY_2 = 50, KEY_3 = 51, KEY_4 = 52,
    KEY_5 = 53, KEY_6 = 54, KEY_7 = 55, KEY_8 = 56, KEY_9 = 57,
    KEY_ESCAPE = 256, KEY_ENTER = 257, KEY_TAB = 258, KEY_BACKSPACE = 259,
    KEY_DELETE = 261, KEY_RIGHT = 262, KEY_LEFT = 263, KEY_DOWN = 264,
    KEY_UP = 265, KEY_PAGE_UP = 266, KEY_PAGE_DOWN = 267,
    KEY_HOME = 268, KEY_END = 269,
    KEY_LEFT_SHIFT = 340, KEY_LEFT_CONTROL = 341, KEY_LEFT_ALT = 342,
    KEY_LEFT_SUPER = 343, KEY_RIGHT_SHIFT = 344, KEY_RIGHT_CONTROL = 345,
    KEY_RIGHT_ALT = 346, KEY_RIGHT_SUPER = 347
};

enum {
    MOUSE_BUTTON_LEFT = 0,
    MOUSE_BUTTON_RIGHT = 1,
    MOUSE_BUTTON_MIDDLE = 2
};

enum {
    FLAG_WINDOW_RESIZABLE = 4,
    FLAG_WINDOW_HIGHDPI = 8192,
    LOG_NONE = 0,
    TEXTURE_FILTER_BILINEAR = 1
};

/* Menu / command ids (libui menu item data). Same values as core/ui_cmd.cch. */
#include "../core/ui_cmd.cch"

void SetTraceLogLevel(int level);
void SetConfigFlags(unsigned int flags);
void InitWindow(int width, int height, const char *title);
void CloseWindow(void);
int WindowShouldClose(void);
int IsWindowReady(void);
int IsWindowResized(void);
int IsWindowFocused(void);
/* Focus edge since the last call: 1 in, 0 out, -1 none. */
int gui_focus_edge(void);
void SetExitKey(int key);
void SetTargetFPS(int fps);
void *GetWindowHandle(void);
int GetScreenWidth(void);
int GetScreenHeight(void);
Vector2 GetWindowScaleDPI(void);

void BeginDrawing(void);
void EndDrawing(void);
void ClearBackground(Color color);
void BeginScissorMode(int x, int y, int w, int h);
void EndScissorMode(void);
void PollInputEvents(void);
/* seconds < 0: sleep until the next toolkit event. */
void WaitTime(double seconds);

/*
 * Partial paint. A Draw carries the toolkit's clip; the painter skips
 * rows and chrome outside it (no layout, measuring, or text for them).
 * gui_clip_hit is 1 when the rect meets the clip (always, outside a Draw).
 * gui_clip_full is 1 when the clip is the whole area.
 */
int gui_clip_hit(double x, double y, double w, double h);
int gui_clip_full(void);

/*
 * Caret blink with painted carets (--caret=cell, or a toolkit with no
 * caret layer; the layer path is gui_caret_* below). During a full Draw
 * the painter notes each rect a blink changes (caret bar, prompt caret)
 * whether or not it is lit this phase.
 * gui_blink_paint repaints just those rects (one clipped Draw each) and
 * returns 1; 0 = no full paint has noted since gui_blink_forget, so the
 * caller paints the window.
 */
void gui_blink_note(double x, double y, double w, double h);
int gui_blink_paint(void);
void gui_blink_forget(void);
/* Rects the last gui_blink_paint drew (RTX_UI_LOG / tests). */
int gui_blink_rects(void);

/*
 * Caret layer (ui_os_caret_*). The carets are not painted into the text:
 * during a whole Draw the painter hands each caret bar to gui_caret_bar
 * (area coordinates; clipped to the scissor), whatever the blink phase,
 * and the layer moves there after the Draw. A blink is gui_caret_phase —
 * no Draw, no text. gui_caret_setup(0, …) (--caret=cell) or a toolkit
 * with no layer keeps the painted caret and the gui_blink_* path above.
 */
void gui_caret_setup(int want, int fade);
int gui_caret_layer(void);
void gui_caret_bar(double x, double y, double w, double h, Color c);
void gui_caret_phase(int on);
/* IME anchor: the first caret of the last whole paint. 0 = none. */
int gui_caret_rect(double *x, double *y, double *w, double *h);

int IsKeyDown(int key);
int IsKeyPressed(int key);
int IsKeyPressedRepeat(int key);
int GetCharPressed(void);

int IsMouseButtonDown(int button);
int IsMouseButtonPressed(int button);
int IsMouseButtonReleased(int button);
Vector2 GetMousePosition(void);
float GetMouseX(void);
float GetMouseY(void);
Vector2 GetMouseWheelMoveV(void);
float GetMouseWheelMove(void);

void DrawRectangle(int x, int y, int w, int h, Color color);
void DrawRectangleLines(int x, int y, int w, int h, Color color);
void DrawLine(int x0, int y0, int x1, int y1, Color color);
void DrawText(const char *text, int x, int y, int fontSize, Color color);
void DrawTextEx(Font font, const char *text, Vector2 pos, float fontSize,
                float spacing, Color tint);
Vector2 MeasureTextEx(Font font, const char *text, float fontSize, float spacing);

Font LoadFontEx(const char *fileName, int fontSize, int *codepoints, int codepointCount);
Font GetFontDefault(void);
void UnloadFont(Font font);
/* Bold/italic face from base (libui font descriptor). Caller owns when ct differs. */
Font gui_derive_font(Font base, int bold, int italic);
void SetTextureFilter(Texture2D texture, int filter);
int FileExists(const char *fileName);

void SetClipboardText(const char *text);
const char *GetClipboardText(void);

/* Clear edge-triggered input after the host has sampled this frame. */
void gui_input_begin_frame(void);
void gui_input_consume(void);

/* Relayout + paint from a live resize (unused by libui; kept for the host). */
void fb_set_live_resize(void (*fn)(void));

/* Native menu: install once after InitWindow; poll CMD_* each frame. */
void gui_menu_install(void);
int gui_menu_poll_cmd(void);
int gui_menu_pending(void);
void gui_clear_close(void); /* cancel WindowShouldClose latch */
/* Rebuild the Apply submenu from the active grammar's apply names. */
#define RTX_APPLY_TAG0 100
/* Menu tag of a command-table row with its own run hook (row index). */
#define RTX_ROW_TAG0 0x10000
void gui_menu_set_apply(const char **names, size_t n);
int gui_menu_apply_pick(void);

/*
 * Unsaved-quit dialog. Returns 1=Save, 2=Don't save, 0=Cancel.
 * nfiles is for the message only.
 */
int gui_alert_unsaved(size_t nfiles);
/* Closing buffers with unsaved edits: 1 save, 2 don't save, 0 cancel. */
int gui_alert_close(const char *what, size_t nfiles);

/* File-changed-on-disk dialog. Returns 1=Overwrite, 0=Cancel. */
int gui_alert_changed(const char *path);

/* Undo of a workspace transaction whose other files moved on.
 * Returns 1=All files, 2=This file, 0=Cancel. */
int gui_alert_xact(void);

/* Save dialog; 1=path chosen, 0=cancel, -1=unavailable. */
int gui_save_panel(const char *dir, char *out, size_t n);

#endif
