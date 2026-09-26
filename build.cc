// cctext core tests + console (cctext) + libui-ng GUI (cctext-ui).
//
//   ./make.shcc @smoke
//   ./make.shcc @cctext
//   ./make.shcc @cctext_ui
//   ./make.shcc @                 # list tasks
//
// Leaf modules are real linked TUs (not textual includes):
//   core/page_store.ccs, core/hex.ccs, core/grid.ccs, core/md_table.ccs + core/md_block.ccs
//   (one target: the Markdown block pass), core/browse.ccs, core/piece_tree.ccs,
//   core/nav.ccs, core/scope.ccs, core/find.ccs, core/safe.ccs, core/ui.ccs,
//   core/ui_help.ccs (+ cmd.ccs, keymap.ccs, palette.ccs, fuzzy.ccs: the command
//   table, keys.json / settings.json, the palette model, the fuzzy scorer),
//   core/workspace.ccs, core/layout.ccs, core/document.ccs
//   frontend/gui_draw.ccs, frontend/gui_input.ccs, frontend/gui_chrome.ccs,
//   frontend/cctext_draw.ccs, frontend/cctext_grid_draw.ccs,
//   frontend/cctext_input.ccs, frontend/cctext_keys.ccs, frontend/cctext_osx.ccs
//   cctext-ui platform (outside ccc, linked in by @cctext_ui): frontend/ui_plat.c
//   (portable libui-ng) + frontend/ui_os_darwin.m (AppKit) or ui_os_gtk.c (GTK 3)
// piece_tree_rb.cch / piece_tree_lines.cch stay chapters of the tree TU.

CC_DEFAULT piece_tree_smoke

CC_TARGET rtx_page_store obj core/page_store.ccs core/lf.ccs
CC_TARGET_INCLUDE rtx_page_store .

CC_TARGET rtx_hex obj core/hex.ccs
CC_TARGET_INCLUDE rtx_hex .

CC_TARGET rtx_grid obj core/grid.ccs
CC_TARGET_INCLUDE rtx_grid .

CC_TARGET rtx_md_table obj core/md_table.ccs core/md_block.ccs
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

CC_TARGET rtx_document obj core/document.ccs core/slides.ccs
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

CC_TARGET rtx_find obj core/find.ccs core/proj.ccs
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

CC_TARGET piece_tree_smoke exe tests/piece_tree_smoke.ccs
CC_TARGET_INCLUDE piece_tree_smoke .
CC_TARGET_DEPS piece_tree_smoke rtx_piece_tree

CC_TARGET utf8_cluster_smoke exe tests/utf8_cluster_smoke.ccs
CC_TARGET_INCLUDE utf8_cluster_smoke .
CC_TARGET_DEPS utf8_cluster_smoke rtx_piece_tree rtx_nav rtx_scope rtx_find rtx_document

CC_TARGET layout_measure_smoke exe tests/layout_measure_smoke.ccs
CC_TARGET_INCLUDE layout_measure_smoke .
CC_TARGET_DEPS layout_measure_smoke rtx_piece_tree rtx_hex rtx_grid rtx_md_table rtx_browse rtx_nav rtx_scope rtx_find rtx_safe rtx_document rtx_layout rtx_workspace rtx_batch rtx_ui

CC_TARGET relayout_smoke exe tests/relayout_smoke.ccs
CC_TARGET_INCLUDE relayout_smoke .
CC_TARGET_DEPS relayout_smoke rtx_piece_tree rtx_hex rtx_grid rtx_md_table rtx_browse rtx_nav rtx_scope rtx_find rtx_safe rtx_document rtx_layout rtx_workspace rtx_batch rtx_ui

CC_TARGET edit_session_smoke exe tests/edit_session_smoke.ccs
CC_TARGET_INCLUDE edit_session_smoke .
CC_TARGET_DEPS edit_session_smoke rtx_piece_tree rtx_hex rtx_grid rtx_md_table rtx_browse rtx_nav rtx_scope rtx_find rtx_safe rtx_document rtx_layout rtx_workspace rtx_batch rtx_ui

CC_TARGET find_smoke exe tests/find_smoke.ccs
CC_TARGET_INCLUDE find_smoke .
CC_TARGET_DEPS find_smoke rtx_piece_tree rtx_hex rtx_grid rtx_md_table rtx_browse rtx_nav rtx_scope rtx_find rtx_safe rtx_document rtx_layout rtx_workspace rtx_batch rtx_ui

CC_TARGET host_turn_smoke exe tests/host_turn_smoke.ccs
CC_TARGET_INCLUDE host_turn_smoke .
CC_TARGET_DEPS host_turn_smoke rtx_piece_tree rtx_hex rtx_grid rtx_md_table rtx_browse rtx_nav rtx_scope rtx_find rtx_safe rtx_document rtx_layout rtx_workspace rtx_batch rtx_ui

CC_TARGET draw_diff_smoke exe tests/draw_diff_smoke.ccs
CC_TARGET_INCLUDE draw_diff_smoke .
CC_TARGET_DEPS draw_diff_smoke rtx_piece_tree rtx_hex rtx_grid rtx_md_table rtx_browse rtx_nav rtx_scope rtx_find rtx_safe rtx_document rtx_layout rtx_workspace rtx_batch rtx_ui rtx_ui_help rtx_cctext_draw rtx_cctext_grid_draw rtx_cctext_input

CC_TARGET large_file_smoke exe tests/large_file_smoke.ccs
CC_TARGET_INCLUDE large_file_smoke .
CC_TARGET_DEPS large_file_smoke rtx_piece_tree rtx_hex rtx_grid rtx_md_table rtx_browse rtx_nav rtx_scope rtx_find rtx_safe rtx_document rtx_layout rtx_workspace rtx_batch rtx_ui

CC_TARGET ws_mem_smoke exe tests/ws_mem_smoke.ccs
CC_TARGET_INCLUDE ws_mem_smoke .
CC_TARGET_DEPS ws_mem_smoke rtx_piece_tree rtx_hex rtx_grid rtx_md_table rtx_browse rtx_nav rtx_scope rtx_find rtx_safe rtx_document rtx_layout rtx_workspace rtx_batch rtx_ui

CC_TARGET dup_scale_smoke exe tests/dup_scale_smoke.ccs
CC_TARGET_INCLUDE dup_scale_smoke .
CC_TARGET_DEPS dup_scale_smoke rtx_piece_tree rtx_nav rtx_scope rtx_find rtx_document

CC_TARGET giant_open_smoke exe tests/giant_open_smoke.ccs
CC_TARGET_INCLUDE giant_open_smoke .
CC_TARGET_DEPS giant_open_smoke rtx_piece_tree rtx_nav rtx_scope rtx_find rtx_document

CC_TARGET perf_matrix_smoke exe tests/perf_matrix_smoke.ccs
CC_TARGET_INCLUDE perf_matrix_smoke .
CC_TARGET_DEPS perf_matrix_smoke rtx_piece_tree rtx_hex rtx_grid rtx_md_table rtx_nav rtx_scope rtx_find rtx_document rtx_layout

CC_TARGET insert_profile exe tests/insert_profile.ccs
CC_TARGET_INCLUDE insert_profile .
CC_TARGET_DEPS insert_profile rtx_piece_tree rtx_nav rtx_scope rtx_find rtx_document

CC_TARGET hex_view_smoke exe tests/hex_view_smoke.ccs
CC_TARGET_INCLUDE hex_view_smoke .
CC_TARGET_DEPS hex_view_smoke rtx_piece_tree rtx_hex rtx_grid rtx_md_table rtx_browse rtx_nav rtx_scope rtx_find rtx_safe rtx_document rtx_layout rtx_workspace rtx_batch rtx_ui

CC_TARGET edit_group_smoke exe tests/edit_group_smoke.ccs
CC_TARGET_INCLUDE edit_group_smoke .
CC_TARGET_DEPS edit_group_smoke rtx_piece_tree rtx_hex rtx_grid rtx_md_table rtx_browse rtx_nav rtx_scope rtx_find rtx_safe rtx_document rtx_layout rtx_workspace rtx_batch rtx_ui rtx_ui_help

CC_TARGET safe_smoke exe tests/safe_smoke.ccs
CC_TARGET_INCLUDE safe_smoke .
CC_TARGET_DEPS safe_smoke rtx_piece_tree rtx_hex rtx_grid rtx_md_table rtx_browse rtx_nav rtx_scope rtx_find rtx_safe rtx_document rtx_layout rtx_workspace rtx_batch

CC_TARGET grid_view_smoke exe tests/grid_view_smoke.ccs
CC_TARGET_INCLUDE grid_view_smoke .
CC_TARGET_DEPS grid_view_smoke rtx_piece_tree rtx_hex rtx_grid rtx_md_table rtx_browse rtx_nav rtx_scope rtx_find rtx_safe rtx_document rtx_layout rtx_workspace rtx_batch rtx_ui

CC_TARGET md_table_smoke exe tests/md_table_smoke.ccs
CC_TARGET_INCLUDE md_table_smoke .
CC_TARGET_DEPS md_table_smoke rtx_md_table

CC_TARGET batch_smoke exe tests/batch_smoke.ccs
CC_TARGET_INCLUDE batch_smoke .
CC_TARGET_DEPS batch_smoke rtx_piece_tree rtx_hex rtx_grid rtx_md_table rtx_browse rtx_nav rtx_scope rtx_find rtx_safe rtx_document rtx_layout rtx_workspace rtx_batch rtx_ui

CC_TARGET scan_race_smoke exe tests/scan_race_smoke.ccs
CC_TARGET_INCLUDE scan_race_smoke .
CC_TARGET_DEPS scan_race_smoke rtx_piece_tree rtx_hex rtx_grid rtx_md_table rtx_browse rtx_nav rtx_scope rtx_find rtx_safe rtx_document rtx_layout rtx_workspace rtx_batch rtx_ui

CC_TARGET line_index_file_prop_smoke exe tests/line_index_file_prop_smoke.ccs
CC_TARGET_INCLUDE line_index_file_prop_smoke .
CC_TARGET_DEPS line_index_file_prop_smoke rtx_piece_tree

CC_TARGET line_index_prop_smoke exe tests/line_index_prop_smoke.ccs
CC_TARGET_INCLUDE line_index_prop_smoke .
CC_TARGET_DEPS line_index_prop_smoke rtx_piece_tree

CC_TARGET tm_grammar_smoke exe tests/tm_grammar_smoke.ccs
CC_TARGET_INCLUDE tm_grammar_smoke .
CC_TARGET_DEPS tm_grammar_smoke rtx_piece_tree rtx_hex rtx_grid rtx_nav rtx_scope rtx_find rtx_document

CC_TARGET tm_lookback_probe exe tests/tm_lookback_probe.ccs
CC_TARGET_INCLUDE tm_lookback_probe .
CC_TARGET_DEPS tm_lookback_probe rtx_piece_tree rtx_hex rtx_grid rtx_nav rtx_scope rtx_find rtx_document

CC_TARGET incr_smoke exe tests/incr_smoke.ccs
CC_TARGET_INCLUDE incr_smoke .
CC_TARGET_DEPS incr_smoke rtx_piece_tree rtx_hex rtx_grid rtx_nav rtx_scope rtx_find rtx_document

CC_TARGET relayout_perf exe tests/relayout_perf.ccs
CC_TARGET_INCLUDE relayout_perf .
CC_TARGET_DEPS relayout_perf rtx_piece_tree rtx_hex rtx_grid rtx_md_table rtx_browse rtx_nav rtx_scope rtx_find rtx_safe rtx_document rtx_layout rtx_workspace rtx_batch rtx_ui

CC_TARGET pair_page_probe exe tests/pair_page_probe.ccs
CC_TARGET_INCLUDE pair_page_probe .
CC_TARGET_DEPS pair_page_probe rtx_piece_tree rtx_hex rtx_grid rtx_md_table rtx_nav rtx_scope rtx_find rtx_document rtx_layout

CC_TARGET rtx_cctext_draw obj frontend/cctext_draw.ccs frontend/cctext_present.ccs
CC_TARGET_INCLUDE rtx_cctext_draw .
CC_TARGET_DEPS rtx_cctext_draw rtx_ui

CC_TARGET rtx_cctext_grid_draw obj frontend/cctext_grid_draw.ccs
CC_TARGET_INCLUDE rtx_cctext_grid_draw .
CC_TARGET_DEPS rtx_cctext_grid_draw rtx_ui

CC_TARGET rtx_cctext_input obj frontend/cctext_input.ccs
CC_TARGET_INCLUDE rtx_cctext_input .
CC_TARGET_DEPS rtx_cctext_input rtx_ui

CC_TARGET rtx_cctext_keys obj frontend/cctext_keys.ccs
CC_TARGET_INCLUDE rtx_cctext_keys .
CC_TARGET_DEPS rtx_cctext_keys rtx_nav

CC_TARGET key_decode_smoke exe tests/key_decode_smoke.ccs
CC_TARGET_INCLUDE key_decode_smoke .
CC_TARGET_DEPS key_decode_smoke rtx_cctext_keys rtx_nav rtx_scope rtx_piece_tree rtx_page_store

CC_TARGET term_safe_smoke exe tests/term_safe_smoke.ccs
CC_TARGET_INCLUDE term_safe_smoke .
CC_TARGET_DEPS term_safe_smoke rtx_piece_tree rtx_hex rtx_grid rtx_md_table rtx_browse rtx_nav rtx_scope rtx_find rtx_safe rtx_document rtx_layout rtx_workspace rtx_batch rtx_ui rtx_ui_help rtx_cctext_draw rtx_cctext_grid_draw rtx_cctext_input

CC_TARGET browse_walk_smoke exe tests/browse_walk_smoke.ccs
CC_TARGET_INCLUDE browse_walk_smoke .
CC_TARGET_DEPS browse_walk_smoke rtx_browse rtx_piece_tree rtx_hex rtx_grid rtx_page_store

CC_TARGET tui_pane_smoke exe tests/tui_pane_smoke.ccs
CC_TARGET_INCLUDE tui_pane_smoke .
CC_TARGET_DEPS tui_pane_smoke rtx_piece_tree rtx_hex rtx_grid rtx_md_table rtx_browse rtx_nav rtx_scope rtx_find rtx_safe rtx_document rtx_layout rtx_workspace rtx_batch rtx_ui rtx_ui_help rtx_cctext_draw rtx_cctext_grid_draw rtx_cctext_input

CC_TARGET rtx_cctext_osx obj frontend/cctext_osx.ccs
CC_TARGET_INCLUDE rtx_cctext_osx .

CC_TARGET cctext exe frontend/cctext.ccs
CC_TARGET_INCLUDE cctext .
CC_TARGET_DEPS cctext rtx_piece_tree rtx_hex rtx_grid rtx_md_table rtx_browse rtx_nav rtx_scope rtx_find rtx_safe rtx_document rtx_layout rtx_workspace rtx_batch rtx_ui rtx_ui_help rtx_cctext_draw rtx_cctext_grid_draw rtx_cctext_input rtx_cctext_keys rtx_cctext_osx

CC_TARGET rtx_gui_chrome obj frontend/gui_chrome.ccs
CC_TARGET_INCLUDE rtx_gui_chrome .

CC_TARGET rtx_gui_draw obj frontend/gui_draw.ccs frontend/gui_present.ccs
CC_TARGET_INCLUDE rtx_gui_draw .
CC_TARGET_DEPS rtx_gui_draw rtx_ui

CC_TARGET rtx_gui_input obj frontend/gui_input.ccs
CC_TARGET_INCLUDE rtx_gui_input .
CC_TARGET_DEPS rtx_gui_input rtx_ui

CC_TARGET cctext_ui exe frontend/ui.ccs
CC_TARGET_INCLUDE cctext_ui .
CC_TARGET_DEPS cctext_ui rtx_piece_tree rtx_hex rtx_grid rtx_md_table rtx_browse rtx_nav rtx_scope rtx_find rtx_safe rtx_document rtx_layout rtx_workspace rtx_batch rtx_ui rtx_ui_help rtx_gui_chrome rtx_gui_draw rtx_gui_input

CC_TARGET rx_smoke exe tests/rx_smoke.ccs
CC_TARGET_INCLUDE rx_smoke .
CC_TARGET_DEPS rx_smoke rtx_rx

CC_TARGET tm_dump exe tests/tm_dump.ccs
CC_TARGET_INCLUDE tm_dump .
CC_TARGET_DEPS tm_dump rtx_piece_tree rtx_hex rtx_grid rtx_nav rtx_scope rtx_find rtx_document

CC_TARGET tm_rx_diff_smoke exe tests/tm_rx_diff_smoke.ccs
CC_TARGET_INCLUDE tm_rx_diff_smoke .
CC_TARGET_DEPS tm_rx_diff_smoke rtx_piece_tree rtx_hex rtx_grid rtx_nav rtx_scope rtx_find rtx_document

CC_TARGET find_opts_smoke exe tests/find_opts_smoke.ccs
CC_TARGET_INCLUDE find_opts_smoke .
CC_TARGET_DEPS find_opts_smoke rtx_piece_tree rtx_hex rtx_grid rtx_md_table rtx_browse rtx_nav rtx_scope rtx_find rtx_safe rtx_document rtx_layout rtx_workspace rtx_batch rtx_ui

CC_TARGET find_perf exe tests/find_perf.ccs
CC_TARGET_INCLUDE find_perf .
CC_TARGET_DEPS find_perf rtx_piece_tree rtx_hex rtx_grid rtx_nav rtx_scope rtx_find rtx_document
