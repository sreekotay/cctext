/*
 * cctext-ui paint hook. The area Draw callback is the only place a
 * uiDrawContext exists, so the host paints there — no display list.
 */
#ifndef RTX_UI_HOOK_H
#define RTX_UI_HOOK_H

void ui_set_paint(void (*fn)(void));
/* Mark the area and flush it through AppKit (not [NSView display]). */
void ui_sync_paint(void);
/* 1 if a typed character is queued and not yet consumed. */
int ui_chars_waiting(void);
/* AppKit turn. Nested. enter=1 pauses dest-live scans; enter=0 resumes
 * only when the outermost turn returns. Draw may run in between. */
void ui_set_turn(void (*fn)(int enter));
void ui_turn(int enter);

#endif
