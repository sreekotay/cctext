#ifndef RTX_RL_HOST_H
#define RTX_RL_HOST_H

typedef struct RtxRl RtxRl;

RtxRl *rtx_rl_open(int w, int h, const char *title);
void rtx_rl_close(RtxRl *rl);
int rtx_rl_should_close(RtxRl *rl);
void rtx_rl_begin(RtxRl *rl);
void rtx_rl_end(RtxRl *rl);
int rtx_rl_width(RtxRl *rl);
int rtx_rl_height(RtxRl *rl);
double rtx_rl_measure(RtxRl *rl, int mono, int ch);
double rtx_rl_line_height(RtxRl *rl, int mono);
void rtx_rl_draw_char(RtxRl *rl, int mono, int ch, float x, float y, int hl, int caret);
int rtx_rl_click(RtxRl *rl, double *x, double *y);
int rtx_rl_char(RtxRl *rl);
int rtx_rl_keydown_backspace(RtxRl *rl);
int rtx_rl_keydown_enter(RtxRl *rl);
int rtx_rl_keydown_left(RtxRl *rl);
int rtx_rl_keydown_right(RtxRl *rl);
double rtx_rl_wheel(RtxRl *rl);
int rtx_rl_resized(RtxRl *rl);
void rtx_rl_status(RtxRl *rl, const char *msg);

#endif
