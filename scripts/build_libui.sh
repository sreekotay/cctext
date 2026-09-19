#!/bin/sh
# Build third_party/libui-ng into out/libui.a (macOS). No display-server
# code of ours — this is the window toolkit cctext-ui links.
# Pin: 43ba1ef553c8993a43a67f1ce6e35983a2660d8c (libui-ng master, 2026-09).
set -e
cd "$(dirname "$0")/.."
root=third_party/libui-ng
pin=43ba1ef553c8993a43a67f1ce6e35983a2660d8c
if [ ! -f "$root/ui.h" ]; then
    rm -rf "$root"
    mkdir -p "$root"
    git -C "$root" init
    git -C "$root" remote add origin https://github.com/libui-ng/libui-ng.git
    git -C "$root" fetch --depth 1 origin "$pin"
    git -C "$root" checkout FETCH_HEAD
fi
outdir="${OUTDIR:-out}"
mkdir -p "$outdir/libui"
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
    obj="$outdir/libui/c_$f.o"
    if [ ! -f "$obj" ] || [ "$src" -nt "$obj" ]; then
        clang $cflags -c "$src" -o "$obj"
    fi
    objs="$objs $obj"
done
for f in $darwin; do
    src="$root/darwin/$f"
    obj="$outdir/libui/d_$f.o"
    if [ ! -f "$obj" ] || [ "$src" -nt "$obj" ]; then
        clang $cflags -fno-objc-arc -c "$src" -o "$obj"
    fi
    objs="$objs $obj"
done
ar rcs "$outdir/libui.a" $objs
echo "$outdir/libui.a"
