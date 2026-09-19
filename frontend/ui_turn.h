/*
 * Nested OS turn. The callback runs on 0→1 and 1→0 only.
 * ui_plat.m is the AppKit caller; host_turn_smoke locks the depth.
 */
#ifndef RTX_UI_TURN_H
#define RTX_UI_TURN_H

typedef struct {
    int depth;
    void (*fn)(int enter);
} RtxUiTurn;

static inline void rtx_ui_turn(RtxUiTurn *t, int enter) {
    if (!t) return;
    if (enter) {
        if (t->depth++ == 0 && t->fn) t->fn(1);
    } else if (t->depth > 0) {
        if (--t->depth == 0 && t->fn) t->fn(0);
    }
}

#endif
