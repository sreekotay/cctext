// More test targets. ccc 0.4.0 caps one build file at 64 CC_TARGETs and
// build.cc is at the cap (claude.md), so tests past it live here with a
// copy of the core library targets. Keep this block equal to build.cc's.
//
//   ccc build --build-file build_tests.cc run replace_smoke
//   ./make.shcc @smoke          (runs these too)

CC_DEFAULT replace_smoke

CC_TARGET rtx_page_store obj core/page_store.ccs core/lf.ccs
CC_TARGET_INCLUDE rtx_page_store .

CC_TARGET rtx_hex obj core/hex.ccs
CC_TARGET_INCLUDE rtx_hex .

CC_TARGET rtx_grid obj core/grid.ccs
CC_TARGET_INCLUDE rtx_grid .

CC_TARGET rtx_md_table obj core/md_table.ccs core/md_block.ccs core/md_refs.ccs
CC_TARGET_INCLUDE rtx_md_table .

CC_TARGET rtx_browse obj core/browse.ccs
CC_TARGET_INCLUDE rtx_browse .
CC_TARGET_DEPS rtx_browse rtx_piece_tree rtx_hex rtx_grid

CC_TARGET rtx_safe obj core/safe.ccs
CC_TARGET_INCLUDE rtx_safe .
CC_TARGET_DEPS rtx_safe rtx_piece_tree rtx_document

CC_TARGET rtx_batch obj core/batch.ccs
CC_TARGET_INCLUDE rtx_batch .
CC_TARGET_DEPS rtx_batch rtx_document rtx_workspace rtx_piece_tree rtx_nav rtx_find

CC_TARGET rtx_document obj core/document.ccs core/slides.ccs core/wb.ccs core/wb_num.ccs
CC_TARGET_INCLUDE rtx_document .
CC_TARGET_DEPS rtx_document rtx_piece_tree rtx_nav rtx_scope rtx_rx rtx_md_table

CC_TARGET rtx_layout obj core/layout.ccs
CC_TARGET_INCLUDE rtx_layout .
CC_TARGET_DEPS rtx_layout rtx_document rtx_hex rtx_grid rtx_md_table

CC_TARGET rtx_workspace obj core/workspace.ccs
CC_TARGET_INCLUDE rtx_workspace .
CC_TARGET_DEPS rtx_workspace rtx_document rtx_layout rtx_piece_tree rtx_hex rtx_grid rtx_md_table rtx_browse rtx_nav rtx_scope rtx_find rtx_safe

CC_TARGET rtx_nav obj core/nav.ccs
CC_TARGET_INCLUDE rtx_nav .
CC_TARGET_DEPS rtx_nav rtx_scope rtx_piece_tree

CC_TARGET rtx_find obj core/find.ccs core/proj.ccs core/sindex.ccs
CC_TARGET_INCLUDE rtx_find .
CC_TARGET_DEPS rtx_find rtx_piece_tree rtx_scope rtx_document rtx_rx

CC_TARGET rtx_scope obj core/scope.ccs
CC_TARGET_INCLUDE rtx_scope .

CC_TARGET rtx_rx obj core/rx.ccs
CC_TARGET_INCLUDE rtx_rx .

CC_TARGET rtx_ui obj core/ui.ccs core/ui_proj.ccs
CC_TARGET_INCLUDE rtx_ui .
CC_TARGET_DEPS rtx_ui rtx_piece_tree rtx_hex rtx_grid rtx_md_table rtx_nav rtx_find rtx_document rtx_layout rtx_workspace rtx_batch rtx_ui_help

CC_TARGET rtx_ui_help obj core/ui_help.ccs core/cmd.ccs core/keymap.ccs core/palette.ccs core/fuzzy.ccs
CC_TARGET_INCLUDE rtx_ui_help .

CC_TARGET rtx_piece_tree obj core/piece_tree.ccs
CC_TARGET_INCLUDE rtx_piece_tree .
CC_TARGET_DEPS rtx_piece_tree rtx_page_store

CC_TARGET replace_smoke exe tests/replace_smoke.ccs
CC_TARGET_INCLUDE replace_smoke .
CC_TARGET_DEPS replace_smoke rtx_piece_tree rtx_hex rtx_grid rtx_md_table rtx_browse rtx_nav rtx_scope rtx_find rtx_safe rtx_document rtx_layout rtx_workspace rtx_batch rtx_ui

CC_TARGET replace_perf exe tests/replace_perf.ccs
CC_TARGET_INCLUDE replace_perf .
CC_TARGET_DEPS replace_perf rtx_piece_tree rtx_hex rtx_grid rtx_md_table rtx_browse rtx_nav rtx_scope rtx_find rtx_safe rtx_document rtx_layout rtx_workspace rtx_batch rtx_ui

CC_TARGET md_edit_smoke exe tests/md_edit_smoke.ccs
CC_TARGET_INCLUDE md_edit_smoke .
CC_TARGET_DEPS md_edit_smoke rtx_piece_tree rtx_hex rtx_grid rtx_md_table rtx_browse rtx_nav rtx_scope rtx_find rtx_safe rtx_document rtx_layout rtx_workspace rtx_batch rtx_ui

CC_TARGET style_cur_smoke exe tests/style_cur_smoke.ccs
CC_TARGET_INCLUDE style_cur_smoke .
CC_TARGET_DEPS style_cur_smoke rtx_piece_tree rtx_hex rtx_grid rtx_md_table rtx_browse rtx_nav rtx_scope rtx_find rtx_safe rtx_document rtx_layout rtx_workspace rtx_batch rtx_ui

CC_TARGET md_block_smoke exe tests/md_block_smoke.ccs
CC_TARGET_INCLUDE md_block_smoke .
CC_TARGET_DEPS md_block_smoke rtx_piece_tree rtx_hex rtx_grid rtx_md_table rtx_nav rtx_scope rtx_find rtx_document

CC_TARGET slides_smoke exe tests/slides_smoke.ccs
CC_TARGET_INCLUDE slides_smoke .
CC_TARGET_DEPS slides_smoke rtx_piece_tree rtx_hex rtx_grid rtx_md_table rtx_nav rtx_scope rtx_find rtx_document

CC_TARGET cmd_table_smoke exe tests/cmd_table_smoke.ccs
CC_TARGET_INCLUDE cmd_table_smoke .
CC_TARGET_DEPS cmd_table_smoke rtx_ui_help rtx_piece_tree rtx_hex rtx_grid rtx_md_table rtx_nav rtx_scope rtx_find rtx_document rtx_rx rtx_page_store
CC_TARGET workspace_smoke exe tests/workspace_smoke.ccs
CC_TARGET_INCLUDE workspace_smoke .
CC_TARGET_DEPS workspace_smoke rtx_piece_tree rtx_hex rtx_grid rtx_md_table rtx_browse rtx_nav rtx_scope rtx_find rtx_safe rtx_document rtx_layout rtx_workspace rtx_batch rtx_ui

CC_TARGET lf_smoke exe tests/lf_smoke.ccs
CC_TARGET_INCLUDE lf_smoke .
CC_TARGET_DEPS lf_smoke rtx_page_store

CC_TARGET lf_perf exe tests/lf_perf.ccs
CC_TARGET_INCLUDE lf_perf .
CC_TARGET_DEPS lf_perf rtx_piece_tree rtx_hex rtx_grid rtx_nav rtx_scope rtx_find rtx_document

CC_TARGET proj_smoke exe tests/proj_smoke.ccs
CC_TARGET_INCLUDE proj_smoke .
CC_TARGET_DEPS proj_smoke rtx_piece_tree rtx_hex rtx_grid rtx_md_table rtx_browse rtx_nav rtx_scope rtx_find rtx_safe rtx_document rtx_layout rtx_workspace rtx_batch rtx_ui

CC_TARGET scroll_smoke exe tests/scroll_smoke.ccs
CC_TARGET_INCLUDE scroll_smoke .
CC_TARGET_DEPS scroll_smoke rtx_piece_tree rtx_hex rtx_grid rtx_md_table rtx_browse rtx_nav rtx_scope rtx_find rtx_safe rtx_document rtx_layout rtx_workspace rtx_batch rtx_ui

CC_TARGET rx_perf exe tests/rx_perf.ccs
CC_TARGET_INCLUDE rx_perf .
CC_TARGET_DEPS rx_perf rtx_rx

CC_TARGET wb_smoke exe tests/wb_smoke.ccs
CC_TARGET_INCLUDE wb_smoke .
CC_TARGET_DEPS wb_smoke rtx_piece_tree rtx_hex rtx_grid rtx_md_table rtx_nav rtx_scope rtx_find rtx_document rtx_layout rtx_rx rtx_page_store

CC_TARGET wb_prop_smoke exe tests/wb_prop_smoke.ccs
CC_TARGET_INCLUDE wb_prop_smoke .
CC_TARGET_DEPS wb_prop_smoke rtx_piece_tree rtx_hex rtx_grid rtx_md_table rtx_nav rtx_scope rtx_find rtx_document rtx_layout rtx_rx rtx_page_store

CC_TARGET wb_scale_perf exe tests/wb_scale_perf.ccs
CC_TARGET_INCLUDE wb_scale_perf .
CC_TARGET_DEPS wb_scale_perf rtx_piece_tree rtx_hex rtx_grid rtx_md_table rtx_browse rtx_nav rtx_scope rtx_find rtx_safe rtx_document rtx_layout rtx_workspace rtx_batch rtx_ui rtx_ui_help

CC_TARGET wb_perf exe tests/wb_perf.ccs
CC_TARGET_INCLUDE wb_perf .
CC_TARGET_DEPS wb_perf rtx_piece_tree rtx_hex rtx_grid rtx_md_table rtx_browse rtx_nav rtx_scope rtx_find rtx_safe rtx_document rtx_layout rtx_workspace rtx_batch rtx_ui rtx_ui_help

CC_TARGET rx_conform exe tests/rx_conform.ccs
CC_TARGET_INCLUDE rx_conform .
CC_TARGET_DEPS rx_conform rtx_rx

CC_TARGET sindex_smoke exe tests/sindex_smoke.ccs
CC_TARGET_INCLUDE sindex_smoke .
CC_TARGET_DEPS sindex_smoke rtx_piece_tree rtx_hex rtx_grid rtx_md_table rtx_browse rtx_nav rtx_scope rtx_find rtx_safe rtx_document rtx_layout rtx_workspace rtx_batch rtx_ui
