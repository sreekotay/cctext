// Raytext core tests + cctext + raytext.
//
//   ./make.shcc @smoke
//   ./make.shcc @cctext
//   ./make.shcc @raytext

CC_DEFAULT piece_tree_smoke

CC_TARGET piece_tree_smoke exe tests/piece_tree_smoke.ccs
CC_TARGET_INCLUDE piece_tree_smoke .

CC_TARGET layout_measure_smoke exe tests/layout_measure_smoke.ccs
CC_TARGET_INCLUDE layout_measure_smoke .

CC_TARGET edit_session_smoke exe tests/edit_session_smoke.ccs
CC_TARGET_INCLUDE edit_session_smoke .

CC_TARGET find_smoke exe tests/find_smoke.ccs
CC_TARGET_INCLUDE find_smoke .

CC_TARGET large_file_smoke exe tests/large_file_smoke.ccs
CC_TARGET_INCLUDE large_file_smoke .

CC_TARGET dup_scale_smoke exe tests/dup_scale_smoke.ccs
CC_TARGET_INCLUDE dup_scale_smoke .

CC_TARGET giant_open_smoke exe tests/giant_open_smoke.ccs
CC_TARGET_INCLUDE giant_open_smoke .

CC_TARGET cctext exe frontend/cctext.ccs
CC_TARGET_INCLUDE cctext .

CC_TARGET raytext exe frontend/gui.ccs
CC_TARGET_INCLUDE raytext .
