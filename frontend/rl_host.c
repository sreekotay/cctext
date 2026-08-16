#include "rl_host.h"
#include <raylib.h>
#include <stdlib.h>
#include <string.h>

struct RtxRl {
    Font mono;
    Font prose;
    float mono_sz;
    float prose_sz;
    int loaded_mono;
    int loaded_prose;
};

static Color hl_color(int hl) {
    switch (hl) {
        case 1: return (Color){80, 200, 220, 255};
        case 2: return (Color){120, 200, 120, 255};
        case 3: return (Color){140, 140, 140, 255};
        case 4: return (Color){220, 180, 80, 255};
        default: return (Color){230, 230, 230, 255};
    }
}

RtxRl *rtx_rl_open(int w, int h, const char *title) {
    RtxRl *rl = (RtxRl *)calloc(1, sizeof(RtxRl));
    const char *mono_path = "/System/Library/Fonts/Menlo.ttc";
    const char *prose_path = "/System/Library/Fonts/Supplemental/Arial.ttf";
    if (!rl) return NULL;
    SetConfigFlags(FLAG_WINDOW_RESIZABLE);
    InitWindow(w, h, title ? title : "raytext");
    SetTargetFPS(60);
    rl->mono_sz = 18;
    rl->prose_sz = 20;
    if (FileExists(mono_path)) {
        rl->mono = LoadFontEx(mono_path, 36, NULL, 0);
        rl->loaded_mono = 1;
    } else {
        rl->mono = GetFontDefault();
    }
    if (FileExists(prose_path)) {
        rl->prose = LoadFontEx(prose_path, 40, NULL, 0);
        rl->loaded_prose = 1;
    } else {
        rl->prose = GetFontDefault();
    }
    if (rl->mono.texture.id) SetTextureFilter(rl->mono.texture, TEXTURE_FILTER_BILINEAR);
    if (rl->prose.texture.id) SetTextureFilter(rl->prose.texture, TEXTURE_FILTER_BILINEAR);
    return rl;
}

void rtx_rl_close(RtxRl *rl) {
    if (!rl) return;
    if (rl->loaded_mono) UnloadFont(rl->mono);
    if (rl->loaded_prose) UnloadFont(rl->prose);
    CloseWindow();
    free(rl);
}

int rtx_rl_should_close(RtxRl *rl) {
    (void)rl;
    return WindowShouldClose();
}

void rtx_rl_begin(RtxRl *rl) {
    (void)rl;
    BeginDrawing();
    ClearBackground((Color){24, 24, 28, 255});
}

void rtx_rl_end(RtxRl *rl) {
    (void)rl;
    EndDrawing();
}

int rtx_rl_width(RtxRl *rl) {
    (void)rl;
    return GetScreenWidth();
}

int rtx_rl_height(RtxRl *rl) {
    (void)rl;
    return GetScreenHeight();
}

double rtx_rl_measure(RtxRl *rl, int mono, int ch) {
    char tmp[2];
    Font font;
    float sz;
    Vector2 dim;
    if (!rl) return 8;
    tmp[0] = (char)ch;
    tmp[1] = 0;
    font = mono ? rl->mono : rl->prose;
    sz = mono ? rl->mono_sz : rl->prose_sz;
    dim = MeasureTextEx(font, tmp, sz, 0);
    return (double)dim.x;
}

double rtx_rl_line_height(RtxRl *rl, int mono) {
    if (!rl) return 20;
    return (double)(mono ? rl->mono_sz : rl->prose_sz) * 1.25;
}

void rtx_rl_draw_char(RtxRl *rl, int mono, int ch, float x, float y, int hl, int caret) {
    char tmp[2];
    Font font;
    float sz;
    if (!rl) return;
    tmp[0] = (char)ch;
    tmp[1] = 0;
    font = mono ? rl->mono : rl->prose;
    sz = mono ? rl->mono_sz : rl->prose_sz;
    if (caret) DrawRectangle((int)x, (int)y, 2, (int)(sz * 1.25f), (Color){255, 200, 80, 255});
    DrawTextEx(font, tmp, (Vector2){x, y}, sz, 0, hl_color(hl));
}

int rtx_rl_click(RtxRl *rl, double *x, double *y) {
    Vector2 p;
    (void)rl;
    if (!IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) return 0;
    p = GetMousePosition();
    if (x) *x = (double)p.x;
    if (y) *y = (double)p.y;
    return 1;
}

int rtx_rl_char(RtxRl *rl) {
    (void)rl;
    return GetCharPressed();
}

int rtx_rl_keydown_backspace(RtxRl *rl) {
    (void)rl;
    return IsKeyPressed(KEY_BACKSPACE);
}

int rtx_rl_keydown_enter(RtxRl *rl) {
    (void)rl;
    return IsKeyPressed(KEY_ENTER);
}

int rtx_rl_keydown_left(RtxRl *rl) {
    (void)rl;
    return IsKeyPressed(KEY_LEFT);
}

int rtx_rl_keydown_right(RtxRl *rl) {
    (void)rl;
    return IsKeyPressed(KEY_RIGHT);
}

double rtx_rl_wheel(RtxRl *rl) {
    (void)rl;
    return (double)GetMouseWheelMove();
}

int rtx_rl_resized(RtxRl *rl) {
    (void)rl;
    return IsWindowResized();
}

void rtx_rl_status(RtxRl *rl, const char *msg) {
    int h = GetScreenHeight();
    (void)rl;
    DrawText(msg ? msg : "", 16, h - 22, 14, (Color){160, 160, 160, 255});
}
