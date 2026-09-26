#!/bin/sh
# Build third_party/libui-ng into $OUTDIR/libui.a (default out/). No
# display-server code of ours — this is the window toolkit cctext-ui links.
#
#   macOS: clang over libui's common/ + darwin/ sources (no meson needed).
#   Linux: meson + ninja, static, GTK 3 backend
#          (apt: libgtk-3-dev meson ninja-build pkg-config).
#
# COPT: compiler flags (default -O2; sanitizer builds pass -g -fsanitize=…).
# Pin: 43ba1ef553c8993a43a67f1ce6e35983a2660d8c (libui-ng master, 2026-09).
set -e
cd "$(dirname "$0")/.."
root=third_party/libui-ng
pin=43ba1ef553c8993a43a67f1ce6e35983a2660d8c
if [ ! -f "$root/ui.h" ]; then
    rm -rf "$root"
    mkdir -p "$root"
    git -C "$root" init -q
    git -C "$root" remote add origin https://github.com/libui-ng/libui-ng.git
    git -C "$root" fetch -q --depth 1 origin "$pin"
    git -C "$root" checkout -q FETCH_HEAD
fi
outdir="${OUTDIR:-out}"
mkdir -p "$outdir/libui"
copt="${COPT:--O2}"

case "$(uname -s)" in
Darwin)
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
    ;;
*)
    for tool in meson ninja pkg-config; do
        if ! command -v "$tool" >/dev/null 2>&1; then
            echo "build_libui: $tool not found (apt install meson ninja-build pkg-config libgtk-3-dev)" >&2
            exit 1
        fi
    done
    if ! pkg-config --exists gtk+-3.0; then
        echo "build_libui: gtk+-3.0 not found (apt install libgtk-3-dev)" >&2
        exit 1
    fi
    bdir="$outdir/libui/meson"
    stamp="$bdir/.cctext-copt"
    # CFLAGS are read at setup; a different COPT is a fresh build dir.
    if [ -f "$stamp" ] && [ "$(cat "$stamp")" != "$copt" ]; then
        rm -rf "$bdir"
    fi
    if [ ! -f "$bdir/build.ninja" ]; then
        rm -rf "$bdir"
        CFLAGS="$copt" meson setup "$bdir" "$root" \
            -Ddefault_library=static -Dbuildtype=plain \
            -Dtests=false -Dexamples=false >/dev/null
        printf '%s' "$copt" >"$stamp"
    fi
    ninja -C "$bdir" >/dev/null
    # layout=flat (libui's default) puts targets in meson-out/.
    lib="$bdir/meson-out/libui.a"
    [ -f "$lib" ] || lib="$bdir/libui.a"
    cp -f "$lib" "$outdir/libui.a"
    ;;
esac
echo "$outdir/libui.a"
