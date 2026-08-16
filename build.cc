// Raytext core tests + cctext + raytext.
//
//   make smoke
//   make cctext
//   make raytext

CC_DEFAULT piece_tree_smoke

CC_TARGET piece_tree_smoke exe tests/piece_tree_smoke.ccs
CC_TARGET_INCLUDE piece_tree_smoke .

CC_TARGET layout_measure_smoke exe tests/layout_measure_smoke.ccs
CC_TARGET_INCLUDE layout_measure_smoke .

CC_TARGET edit_session_smoke exe tests/edit_session_smoke.ccs
CC_TARGET_INCLUDE edit_session_smoke .

CC_TARGET large_file_smoke exe tests/large_file_smoke.ccs
CC_TARGET_INCLUDE large_file_smoke .

CC_TARGET cctext exe frontend/cctext.ccs
CC_TARGET_INCLUDE cctext .

CC_TARGET raytext exe frontend/gui.ccs
CC_TARGET_INCLUDE raytext .
