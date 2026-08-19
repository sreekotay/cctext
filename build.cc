// cctext core tests + console + Raylib GUI (cctext-ray).
//
//   ./make.shcc @smoke
//   ./make.shcc @cctext
//   ./make.shcc @cctext_ray
//
// Leaf modules are real linked TUs (not textual includes):
//   core/page_store.ccs, core/hex.ccs, core/piece_tree.ccs
// piece_tree_rb.cch / piece_tree_lines.cch stay chapters of the tree TU.

CC_DEFAULT piece_tree_smoke

CC_TARGET rtx_page_store obj core/page_store.ccs
CC_TARGET_INCLUDE rtx_page_store .

CC_TARGET rtx_hex obj core/hex.ccs
CC_TARGET_INCLUDE rtx_hex .

CC_TARGET rtx_piece_tree obj core/piece_tree.ccs
CC_TARGET_INCLUDE rtx_piece_tree .
CC_TARGET_DEPS rtx_piece_tree rtx_page_store

CC_TARGET piece_tree_smoke exe tests/piece_tree_smoke.ccs
CC_TARGET_INCLUDE piece_tree_smoke .
CC_TARGET_DEPS piece_tree_smoke rtx_piece_tree

CC_TARGET layout_measure_smoke exe tests/layout_measure_smoke.ccs
CC_TARGET_INCLUDE layout_measure_smoke .
CC_TARGET_DEPS layout_measure_smoke rtx_piece_tree rtx_hex

CC_TARGET edit_session_smoke exe tests/edit_session_smoke.ccs
CC_TARGET_INCLUDE edit_session_smoke .
CC_TARGET_DEPS edit_session_smoke rtx_piece_tree rtx_hex

CC_TARGET find_smoke exe tests/find_smoke.ccs
CC_TARGET_INCLUDE find_smoke .
CC_TARGET_DEPS find_smoke rtx_piece_tree rtx_hex

CC_TARGET large_file_smoke exe tests/large_file_smoke.ccs
CC_TARGET_INCLUDE large_file_smoke .
CC_TARGET_DEPS large_file_smoke rtx_piece_tree rtx_hex

CC_TARGET dup_scale_smoke exe tests/dup_scale_smoke.ccs
CC_TARGET_INCLUDE dup_scale_smoke .
CC_TARGET_DEPS dup_scale_smoke rtx_piece_tree

CC_TARGET giant_open_smoke exe tests/giant_open_smoke.ccs
CC_TARGET_INCLUDE giant_open_smoke .
CC_TARGET_DEPS giant_open_smoke rtx_piece_tree

CC_TARGET perf_matrix_smoke exe tests/perf_matrix_smoke.ccs
CC_TARGET_INCLUDE perf_matrix_smoke .
CC_TARGET_DEPS perf_matrix_smoke rtx_piece_tree rtx_hex

CC_TARGET insert_profile exe tests/insert_profile.ccs
CC_TARGET_INCLUDE insert_profile .
CC_TARGET_DEPS insert_profile rtx_piece_tree

CC_TARGET hex_view_smoke exe tests/hex_view_smoke.ccs
CC_TARGET_INCLUDE hex_view_smoke .
CC_TARGET_DEPS hex_view_smoke rtx_piece_tree rtx_hex

CC_TARGET line_index_prop_smoke exe tests/line_index_prop_smoke.ccs
CC_TARGET_INCLUDE line_index_prop_smoke .
CC_TARGET_DEPS line_index_prop_smoke rtx_piece_tree

CC_TARGET tm_grammar_smoke exe tests/tm_grammar_smoke.ccs
CC_TARGET_INCLUDE tm_grammar_smoke .
CC_TARGET_DEPS tm_grammar_smoke rtx_piece_tree rtx_hex

CC_TARGET tm_lookback_probe exe tests/tm_lookback_probe.ccs
CC_TARGET_INCLUDE tm_lookback_probe .
CC_TARGET_DEPS tm_lookback_probe rtx_piece_tree rtx_hex

CC_TARGET cctext exe frontend/cctext.ccs
CC_TARGET_INCLUDE cctext .
CC_TARGET_DEPS cctext rtx_piece_tree rtx_hex

CC_TARGET cctext_ray exe frontend/gui.ccs
CC_TARGET_INCLUDE cctext_ray .
CC_TARGET_DEPS cctext_ray rtx_piece_tree rtx_hex
