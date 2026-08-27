/*
 * Shared command ids. Closed dispatch (no default: at the call site).
 * Hosts map platform events onto these; native menus use the same tags.
 */
#ifndef RTX_UI_CMD_H
#define RTX_UI_CMD_H

enum {
    CMD_NONE = 0,
    CMD_SAVE,
    CMD_UNDO,
    CMD_REDO,
    CMD_CUT,
    CMD_COPY,
    CMD_PASTE,
    CMD_PANE,
    CMD_NEXT,
    CMD_SPLIT,
    CMD_QUIT,
    CMD_JUMP,
    CMD_FIND,
    CMD_OPEN,
    CMD_BROWSE,
    CMD_HELP,
    CMD_SEL_ALL,
    CMD_VIEW,
    CMD_FOLLOW,
    CMD_NAV_NEXT,
    CMD_NAV_PREV,
    CMD_NAV_INV,
    CMD_NAV_INV_PREV,
    CMD_FOLD,
    CMD_STATS
};

#endif /* RTX_UI_CMD_H */
