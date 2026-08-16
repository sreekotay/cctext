// Raytext core tests + cctext (console). GUI is a later target.
//
//   make smoke
//   make cctext

CC_DEFAULT piece_tree_smoke

CC_TARGET piece_tree_smoke exe tests/piece_tree_smoke.ccs
CC_TARGET_INCLUDE piece_tree_smoke .

CC_TARGET layout_measure_smoke exe tests/layout_measure_smoke.ccs
CC_TARGET_INCLUDE layout_measure_smoke .

CC_TARGET cctext exe frontend/cctext.ccs
CC_TARGET_INCLUDE cctext .
