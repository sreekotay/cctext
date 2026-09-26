/*
 * cctext-ui paint hook. The area Draw callback is the only place a
 * uiDrawContext exists, so the host paints there — no display list.
 */
#ifndef RTX_UI_HOOK_H
#define RTX_UI_HOOK_H

void ui_set_paint(void (*fn)(void));
/* Mark the area and flush a paint (AppKit displayIfNeeded; GTK process updates). */
void ui_sync_paint(void);
/* 1 if a typed character is queued and not yet consumed. */
int ui_chars_waiting(void);
/* Toolkit (AppKit / GTK) turn. Nested. enter=1 pauses dest-live scans; enter=0 resumes
 * only when the outermost turn returns. Draw may run in between. */
void ui_set_turn(void (*fn)(int enter));
void ui_turn(int enter);

/* RTX_UI_FRAME_LOG=path: one line per input frame that laid out or
 * painted, "frame <total_us> <layout_us> <paint_us> <layouts>" (layouts:
 * toolkit text layouts made during the frame). ui_frame_mark(0) at the
 * loop top, 1 after relayout, 2 after paint, then ui_frame_end(saw). */
void ui_frame_mark(int at);
void ui_frame_end(int saw);

#endif
