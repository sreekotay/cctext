/*
 * Platform draw/input surface for cctext-gui (Cocoa + Core Text).
 * Raylib-shaped names keep the editor TUs simple; paint is a display list
 * replayed in drawRect (see proto/cocoa_ct.m).
 */
#ifndef RTX_GUI_PLAT_H
#define RTX_GUI_PLAT_H

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
    KEY_F = 70, KEY_G = 71, KEY_J = 74, KEY_K = 75, KEY_L = 76,
    KEY_N = 78, KEY_O = 79, KEY_P = 80, KEY_Q = 81, KEY_R = 82, KEY_S = 83,
    KEY_T = 84, KEY_U = 85, KEY_V = 86, KEY_W = 87, KEY_X = 88,
    KEY_Y = 89, KEY_Z = 90, KEY_BACKSLASH = 92,
    KEY_ESCAPE = 256, KEY_ENTER = 257, KEY_TAB = 258, KEY_BACKSPACE = 259,
    KEY_DELETE = 261, KEY_RIGHT = 262, KEY_LEFT = 263, KEY_DOWN = 264,
    KEY_UP = 265, KEY_HOME = 268, KEY_END = 269,
    KEY_LEFT_SHIFT = 340, KEY_LEFT_CONTROL = 341, KEY_LEFT_ALT = 342,
    KEY_LEFT_SUPER = 343, KEY_RIGHT_SHIFT = 344, KEY_RIGHT_CONTROL = 345,
    KEY_RIGHT_ALT = 346, KEY_RIGHT_SUPER = 347
};

enum {
    MOUSE_BUTTON_LEFT = 0,
    MOUSE_BUTTON_RIGHT = 1
};

enum {
    FLAG_WINDOW_RESIZABLE = 4,
    FLAG_WINDOW_HIGHDPI = 8192,
    LOG_NONE = 0,
    TEXTURE_FILTER_BILINEAR = 1
};

/* Menu / command ids (native NSMenu tags). Same values as core/ui_cmd.h. */
#include "../core/ui_cmd.h"

void SetTraceLogLevel(int level);
void SetConfigFlags(unsigned int flags);
void InitWindow(int width, int height, const char *title);
void CloseWindow(void);
int WindowShouldClose(void);
int IsWindowReady(void);
int IsWindowResized(void);
void SetExitKey(int key);
void SetTargetFPS(int fps);
void *GetWindowHandle(void);
int GetScreenWidth(void);
int GetScreenHeight(void);
Vector2 GetWindowScaleDPI(void);

void BeginDrawing(void);
void EndDrawing(void);
void ClearBackground(Color color);
void PollInputEvents(void);
void WaitTime(double seconds);

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
/* Bold/italic face from base (Core Text traits). Caller owns when ct differs. */
Font gui_derive_font(Font base, int bold, int italic);
void SetTextureFilter(Texture2D texture, int filter);
int FileExists(const char *fileName);

void SetClipboardText(const char *text);
const char *GetClipboardText(void);

/* Clear edge-triggered input after the host has sampled this frame. */
void gui_input_begin_frame(void);
void gui_input_consume(void);

/* Relayout + paint from windowDidResize (nested tracking loop). */
void fb_set_live_resize(void (*fn)(void));

/* Native menu: install once after InitWindow; poll CMD_* each frame. */
void gui_menu_install(void);
int gui_menu_poll_cmd(void);
int gui_menu_pending(void);
void gui_clear_close(void); /* cancel WindowShouldClose latch */

/*
 * Unsaved-quit NSAlert. Returns 1=Save, 2=Don't save, 0=Cancel.
 * nfiles is for the message only.
 */
int gui_alert_unsaved(size_t nfiles);

/* NSSavePanel; 1=path chosen, 0=cancel, -1=unavailable. */
int gui_save_panel(const char *dir, char *out, size_t n);

#endif
