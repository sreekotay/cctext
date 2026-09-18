/*
 * cctext-ui paint hook. The area Draw callback is the only place a
 * uiDrawContext exists, so the host paints there — no display list.
 */
#ifndef RTX_UI_HOOK_H
#define RTX_UI_HOOK_H

void ui_set_paint(void (*fn)(void));
/* Run the paint hook now (synchronous area display). */
void ui_sync_paint(void);

#endif
