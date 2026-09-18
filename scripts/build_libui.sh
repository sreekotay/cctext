#!/bin/sh
# Build third_party/libui-ng into out/libui.a (macOS). No display-server
# code of ours — this is the window toolkit cctext-ui links.
# Pin: 43ba1ef553c8993a43a67f1ce6e35983a2660d8c (libui-ng master, 2026-09).
set -e
cd "$(dirname "$0")/.."
root=third_party/libui-ng
if [ ! -f "$root/ui.h" ]; then
    git clone --depth 1 https://github.com/libui-ng/libui-ng.git "$root"
fi
mkdir -p out/libui
copt="${COPT:--O2}"
cflags="$copt -I$root -Wno-unused-parameter -Wno-switch -Wno-deprecated-declarations"
common="
attribute.c attrlist.c attrstr.c areaevents.c control.c debug.c matrix.c
opentype.c shouldquit.c table.c tablemodel.c tablevalue.c userbugs.c utf.c
"
darwin="
aat.m alloc.m area.m areaevents.m attrstr.m autolayout.m box.m button.m
checkbox.m colorbutton.m combobox.m control.m datetimepicker.m debug.m
draw.m drawtext.m editablecombo.m entry.m event.m fontbutton.m fontmatch.m
fonttraits.m fontvariation.m form.m future.m graphemes.m grid.m group.m
image.m label.m main.m menu.m multilineentry.m opentype.m progressbar.m
radiobuttons.m scrollview.m separator.m slider.m spinbox.m stddialogs.m
tab.m table.m tablecolumn.m text.m undocumented.m util.m window.m
winmoveresize.m nstextfield.m
"
objs=""
for f in $common; do
    src="$root/common/$f"
    obj="out/libui/c_$f.o"
    if [ ! -f "$obj" ] || [ "$src" -nt "$obj" ]; then
        clang $cflags -c "$src" -o "$obj"
    fi
    objs="$objs $obj"
done
for f in $darwin; do
    src="$root/darwin/$f"
    obj="out/libui/d_$f.o"
    if [ ! -f "$obj" ] || [ "$src" -nt "$obj" ]; then
        clang $cflags -fno-objc-arc -c "$src" -o "$obj"
    fi
    objs="$objs $obj"
done
ar rcs out/libui.a $objs
echo "out/libui.a"
