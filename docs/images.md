# Images

cctext-ui paints pictures in the Markdown Rich lens, in slides, in the
browse preview and in an image viewer (phase 1); cctext paints them in
the same places with the kitty graphics protocol, sixel, iTerm2 inline
images or Unicode block art, whichever the terminal answers for, else
the text stand-in `[image: alt WxH]` (phase 2, [Terminal](#terminal-cctext)).
SVG goes through the same cache and surfaces, rendered by a separate
sandboxed process, `cctext-render`
([Renderer](#svg-the-renderer-cctext-render)); so do ` ```mermaid `
fences, drawn by the official Mermaid in QuickJS inside that helper
([Mermaid](#mermaid)), and TeX math (`$…$`, `$$…$$`, ` ```math `),
typeset by the official MathJax in the same helper ([Math](#math)).

Bytes stay the truth. `![alt](path)` stays in the file exactly as typed;
the picture is a painted stand-in over those bytes (the Rich lens, the
same way a workbook value paints over its formula), never a second copy
of the document, and selection, copy, search and undo all act on the
text.

Code: `core/img.cch` / `img.ccs` (loader, limits, policy, cache, jobs),
`core/img_wuffs.c` / `.h` (the decoder), `core/img_net.c` / `.h` (fetch
shim), `core/layout.ccs` (stand-ins and picture rows),
`frontend/gui_img.ccs` (pictures, placeholders, dialog, viewer, preview),
`frontend/gui_present.ccs` (slides), `frontend/ui_os_*` (the blit),
`frontend/cctext_input.ccs` / `cctext_draw.ccs` / `cctext_present.ccs`
(terminal stand-ins, prompt, picture rows, viewer, slides),
`frontend/cctext_img.ccs` (terminal detection, encode cache and jobs,
overlays, kitty ids), `core/img_term.c` / `.h` (terminal encoders).
SVG: `core/img_svg.c` / `.h` (the helper processes, their protocol,
the content-hash cache), `render/` (the helper), `scripts/render_build.cch`
(its build). Mermaid: `render/cr_js.c` (the QuickJS host), `render/js/`
(our shims and entry point), `third_party/quickjs`, `third_party/mermaid`;
in the editor the fence scan in `core/layout.ccs`, the diagram source,
theme and stale display in `core/img.ccs`, the painters in
`frontend/gui_img.ccs` and `frontend/cctext_img.ccs`. Math:
`render/js/mj_shim.js` / `mj_glue.js`, `third_party/mathjax*`; the `$$` /
inline scan in `core/layout.ccs`, the formula source, look and cache in
`core/img.ccs`, the Unicode approximation in `core/img_math.c`.

## Decoder

[Wuffs](https://github.com/google/wuffs) 0.4 (Apache-2.0 / MIT), the
single-file C release vendored as `third_party/wuffs/wuffs-v0.4.c` with
its licence files. It compiles in exactly one translation unit,
`core/img_wuffs.c`, with `WUFFS_CONFIG__MODULES` limited to PNG, JPEG,
GIF, BMP, WebP (lossless; Wuffs' VP8 module comes along) and QOI, and
every Wuffs symbol static; nothing else includes it. No platform decoder
and no stb_image. Wuffs is memory-safe by construction (bounds and
overflow checks are proved at its compile time) and never allocates:
the caller hands it the pixel buffer and its scratch, which is what lets
the limits below run before any allocation.

Input is fed to Wuffs in 256 KiB slices (`RTX_WUFFS_CHUNK`); between
slices a decode polls its job's cancel flag, so a cancelled 100-megapixel
JPEG stops within one slice.

## Limits

Checked from the header, before a pixel buffer or the decoder's scratch
exists. Every one is a setting (`settings.json`, beside `keys.json`):

| Limit | Default | Setting | Placeholder says |
|---|---|---|---|
| File size read | 64 MiB | `image_file_mb` | file over the 64 MiB limit |
| Either side | 16384 px | `image_max_side` | over the 16384 px side limit |
| Canvas area | 100 megapixels | `image_max_mp` | over the 100 megapixel limit |
| Decoded pixels held (all images, LRU) | 256 MiB | `image_budget_mb` | over the 256 MiB pixel budget (one image alone) |
| Rich display height | 480 px | `image_max_height` | — |
| Animation | off | `image_animate`, `image_anim_frames` (64) | — |
| `data:` URI payload | 1 MiB | — | data: URI malformed or over 1 MiB |
| One download | 16 MiB | `image_remote_mb` | download over the 16 MiB limit |
| One download | 20 s | `image_timeout_s` | fetch timed out |

A decoder whose scratch claim is out of proportion to its canvas is
refused as corrupt. `testdata/img/bomb_dim.png` (a valid 69-byte PNG
claiming 100000 × 100000) and `bomb_px.png` (12000 × 12000) are refused
from their headers in well under a millisecond.

**Display size.** A picture is decoded at the size it paints (Rich: the
pane width, capped at `image_max_height`; slides: its box at the slide's
own scale; the viewer: fit or zoom, never above natural size). Wuffs
0.4 has no scaled JPEG (IDCT) decode, so every format decodes to a
full-size canvas and then box-filters down (`rtx_img_scale`, at most
~5×5 samples per output pixel); only the scaled result is kept, the
canvas is freed before the job returns, and one decode runs at a time,
so the transient peak is one canvas (4 bytes a pixel of the header's
size, within the megapixel limit). A cached bitmap within 10 % of a new
size is reused; a bigger one is shown scaled while the right one
decodes.

**EXIF orientation** (tags 1-8 from a JPEG APP1, a PNG `eXIf` chunk or a
WebP `EXIF` chunk) is applied in the same scaling pass; the reported size
is the displayed one. No other metadata is read (no ICC, no XMP, no
colour management).

**Animation.** Off by default: a GIF (or animated WebP) shows its first
frame and costs nothing more. `"image_animate": true` decodes up to
`image_anim_frames` frames (each scaled; the whole set counts against the
budget) with their delays; cctext-ui wakes for the next frame only while
an animated picture was painted in the last pass (`rtx_img_next_ms`), so
a still window stays at zero wakeups. (The editor's threads do; the
ccc runtime's scheduler monitor, started by the loader's first job, ticks
every 20 ms on the pinned ccc until it carries
`scripts/ccc_sysmon_quiescent.patch` — FRICTION.md.)

## Sources and permissions

`rtx_img_resolve(doc_path, src)`:

**Local paths** are percent-decoded (and `\` escapes dropped), relative
to the document's directory (`file:///abs` and absolute paths work too),
then `realpath` — symlinks followed. Inside the document's project root
(the nearest directory at or above it holding `.git`, else its
directory) they load. Outside it (a `..` escape, a symlink pointing out,
an absolute path) the placeholder says "outside the project: click to
allow"; the click (cctext-ui) or **Load Image** (palette, both hosts)
asks:

- **Allow this file** (the resolved real path),
- **Allow this directory** (the file's directory and everything below),
- **Allow ALL local images** (this project),
- Cancel.

**Remote `http(s)`** is off by default and never fetched on its own: the
placeholder names the alt and the host ("remote image from host: click to
load"). The dialog / prompt offers:

- **Load this image** (this URL, this session only — not remembered),
- **Always load from this host**,
- **Always load remote images in this project**,
- Cancel.

Plain `http` is refused unless the user allowed http for that host: a
decision taken on an `http:` URL (the dialog says "over plain http (not
encrypted)") allows http for its host; "always in this project" covers
https only.

**`data:` URIs** load up to 1 MiB of payload (base64 or percent-encoded),
no question asked (the bytes are in the document).

**Where decisions live.** Per project, in a sidecar of the Safe home
(`<safe>/img/<fnv64 of the project root>.allow`, beside the journals and
workspace snapshots; `RTX_SAFE_HOME` / `--no-safe` apply): text lines
`RTXI 1`, `root <path>`, then `file` / `dir` / `all-local` / `host` /
`http` / `all-remote` entries, and an FNV-1a 64 trailer over every byte
before it. A file whose magic, root or trailer does not match is ignored
whole (as `core/safe.ccs` treats its leaves); writes go to `.tmp`, fsync,
rename. **Revoke Image Permissions** (palette) empties the project's
file and forgets session decisions; pictures that needed them go back to
placeholders at once.

**Fetching.** A background transfer, polled from the UI thread's pump,
cancellable, with the byte cap and the timeout above. This build spawns
`curl` (`posix_spawnp`, a fixed argv, no shell, stdin / stdout / stderr on
`/dev/null`): `--silent --fail --location --max-redirs 3 --proto =https
--proto-redir =https` (`=http,https` only for a host allowed http),
`--max-filesize`, `--max-time`, `--connect-timeout`, `--noproxy` for
loopback only, `--output <tmp>`, `--etag-save` / `--etag-compare` for the
validator. The shim also stats the growing file and kills the transfer
past the cap (a chunked reply has no length for `--max-filesize`) and
past the deadline. libcurl is not used because its headers are not a
build dependency (the reference build machine has none), and the macOS
NSURLSession backend is not wired yet: both platforms use the `curl`
binary every macOS and Linux install ships (`RTX_IMG_CURL` names another
one). The shim (`core/img_net.h`) is the seam for either backend.

**Cache.** `<safe>/img/cache/<fnv64 of the URL>.img`, its ETag beside it
(`.etag`); oldest files are deleted past 256 MiB. A cached image is shown
on a later open without asking **only** when the decision was an
"always" (host or project); after "Load this image" the next session asks
again, cache or not. A fetch after "Load this image" revalidates with the
saved ETag.

## Never block the frame

The same shape as highlighting and workbooks: the frame asks, a job
works, the pump adopts.

- **Header first.** A layout that meets an image asks `rtx_img_info`:
  known → the picture's box; unknown → a probe job reads 64 KiB of the
  file (then 1 MiB, then the rest within the file limit, for a JPEG with
  its tables late) on lane 0, and the layout reserves a placeholder box
  (three lines) until the header arrives (`rtx_img_layout_changed`: the
  host refills panes holding images, `rtx_img_gen` rebuilds a cached
  slide). Pixels never change a box: nothing jumps when they arrive
  (`img_layout_smoke`, `ui_img_test`).
- **Pixels.** `rtx_img_want` (during paint) marks a bitmap wanted in this
  paint pass and queues its decode on lane 1; the queue runs most
  recently wanted first. Each lane is one dest-live `@parallel` arm (the
  browse-preview shape: a job record, a done flag, adopt on the UI
  thread); `RTX_PARALLEL_INLINE=1` runs jobs on the caller.
- **Scrolling away cancels.** A whole paint starts a pass
  (`rtx_img_frame`); at the next pump a queued decode nobody painted in
  the last pass is dropped and a running one is told to stop.
- **Eviction.** Bitmaps leave least-recently-painted first when a new one
  would pass the budget, and — like far highlight runs — one not painted
  in the last 600 whole paints goes even under the budget (an idle window
  paints nothing, so it keeps what it shows); their platform images go
  with them.
- **Idle.** Hosts poll at the 16 ms frame tick only while a job or a
  fetch runs (`rtx_img_busy`); otherwise they sleep until input
  (`ui_img_test`: 0 paints, 0 CPU ticks in 1.6 s with a picture on
  screen).
- **Files that change** on disk (mtime or size, checked at most once a
  second while painted) are probed again.

## The Rich lens

A lexed Markdown image — the grammar's path mark whose label opens with
`![` and whose destination closes with `)` (a reference image
`![a][ref]` stays text: leftover) — is hidden source with a stand-in
while the caret (or the selection's anchor) is not on its line. When it
is, the image's bytes show like any revealed mark (`rtx_layout_reveal`
unions the images on the caret's and anchor's lines), and they edit as
text.

- **An image inside a line of text** (both hosts), and every image in a
  terminal that cannot draw pictures: the stand-in is text,
  `[image: alt WxH]` — `[image: alt]` while the header is on its way,
  `[image: alt - host]` for a remote image not yet allowed,
  `[image: alt - outside project]`, `[image: alt - reason]` (a terminal
  keeps this one-line stand-in for an image that failed or waits for a
  decision, even when it draws pictures).
- **cctext, a line that is one image**: a picture row of cell rows
  ([Terminal](#terminal-cctext)), no padding; revealed, the picture sits
  under the source line.
- **cctext-ui, a line that is one image** (blanks around it allowed) is
  a **picture row**: its height is the picture's box (the pane's width
  and `image_max_height`, never upscaled) plus 3 px above and below. The
  row paints the picture, or a placeholder (a dim frame with the alt and
  the size, the reason or the host). Revealed, the source lays out as
  text rows and the picture joins the last one under the text; the caret
  and the selection are one line tall there. A click on a picture opens
  it full size (the viewer); a click on a placeholder that needs a
  decision asks. A click beside it places the caret.

Picture rows are layout-epoch records (`RtxLayout.img_recs`, the MD
table's record shape); a window holding `![` refills instead of patching
after an edit. Everything is gated by `L->img_on` (Rich, and `![` in the
window's bytes), so a file without images pays one byte scan per fill.

## Slides

`![bg](…)` is the first item of its slide's display list (under the
text): Marp's default fit is **cover** (fills, cropped), `fit` /
`contain` letterbox, `auto` is the natural size; `left:N%` / `right:N%`
put it in its side of the slide and the content in the other. A content
image lays out at its header size, or Marp's `w:` / `h:` (`width:` /
`height:`) from the alt, fitted to the content width and what is left of
the slide. Transitions animate images with everything else (fade, move,
wipe clips, zoom and morph scale them); the bitmap is decoded once at the
slide's own scale, so a zoom scales one bitmap instead of decoding per
frame. The terminal slide paints them as pictures too
([Terminal](#terminal-cctext)); a terminal without pictures shows the
stand-in text.

## Browse preview and the viewer

An image file in the browse preview is its picture scaled to the pane,
with its format, size and byte count under it (cctext:
`[image: PNG 1200x900, 4.4 KiB]` under the picture). Opened directly, an
image file is the **viewer** in both hosts: fitted (never above 100 %),
`+` / `-` zoom, `0` fit, `1` actual size, arrows pan; typing never edits
the bytes there. `Ctrl-D` switches the
pane to hex and back (the view is the journal's, so it sticks). These are
files the user picked, so no document policy applies.

## Terminal (cctext)

Phase 2: cctext paints pictures too — in the Rich lens, in slides, in the
browse preview and in the image viewer — with whatever the terminal can
draw, found by asking it, never by guessing from `TERM`.

| Protocol | When (`tui_images: auto`) | How it paints |
|---|---|---|
| **kitty** graphics, Unicode placeholders | the terminal answers a kitty query (`a=q`) OK (not WezTerm / Konsole by XTVERSION: no placeholders there) | the image is sent once per (bitmap, box) with a virtual placement (`a=T,U=1,c=…,r=…,f=32,o=z,q=2`); then it is ordinary cells: `U+10EEEE` plus the row and column diacritics, the id's low byte in the fg colour (256-colour), its top byte in a third diacritic |
| **iTerm2** inline images | XTVERSION says iTerm2 or WezTerm | `OSC 1337 ; File=inline=1;width=N;height=M` (cells) with a PNG we encode, over blank cells |
| **sixel** | DA1 lists attribute 4 | a median-cut palette (up to 256, or what `XTSMGRAPHICS` reports) and sixel bands, over blank cells |
| **block art** | none of those, and the terminal has colours | Unicode `▀` half blocks with 24-bit fg / bg (1 × 2 pixels a cell); quadrants (2 × 2) or sextants (`U+1FB00` block, 2 × 3) by setting; xterm-256 colours when there is no truecolor |
| text | no colours at all (`TERM=xterm`, a console), or `off` | the stand-in `[image: alt WxH]` as before |

Block art and kitty cells are text: they go through the row diff, the
region scrolls, split panes and clipping unchanged (and through tmux,
which only sees cells). Sixel and iTerm2 draw outside the text grid, so
the frame writer treats them as overlays: the rows under one are blank
cells; an overlay is drawn after the rows whenever it is new, moved, or
any row it covers was just written; rows whose overlay went away are
rewritten (which erases its pixels); a region scroll never moves rows
that an overlay covers (they take the plain row diff); a picture in a
side-by-side pane, or partly scrolled out, is cropped and encoded for
that crop.

**Pixels** come only from the loader: `rtx_img_want_pix(…, RTX_IMG_PIX_RGBA)`
asks for straight RGBA at the display size (never above natural size),
kept apart from cctext-ui's premultiplied bitmaps; never the file's
bytes. The encoders (`core/img_term.c`, pure C, no libm) resample in
linear light (an area average of premultiplied samples; nearest when
zooming in), pick each cell's two colours as the best split of its
sub-pixels (least squared error, means in linear light), quantize to the
xterm cube / grey ramp (Floyd–Steinberg with `tui_dither`), build sixel
bands with run-length repeats, and write PNG and zlib (a fixed-Huffman
deflate with LZ77) for kitty's `o=z` and iTerm2 — no zlib dependency.

**Detection** runs once the TTY is raw, before the first frame: OSC 11
(the background colour; asked even with pictures off, for the theme), a
kitty query (and, when not over SSH, a second one naming a temp file,
`t=t`: an OK means the terminal reads our files), XTVERSION, `CSI 16 t`
when `TIOCGWINSZ` has no pixel size, `XTSMGRAPHICS`, a truecolor
`DECRQSS` probe (unless `COLORTERM` says so), then DA1. Every terminal answers DA1,
so its reply ends the wait; with no reply at all the wait is at most
150 ms (`RTX_TUI_DETECT_MS`), then block art (or the text stand-in).
Keys typed meanwhile stay input; a reply that comes later is swallowed
by the key decoder (an `APC G`, a DCS or an OSC string), never typed.
The cell size is `ws_xpixel / ws_col` × `ws_ypixel / ws_row`, else the
`CSI 16 t` reply, else 8 × 16 (1:2).

**The background** (OSC 11's answer, any X11 spec: `rgb:RRRR/GGGG/BBBB`
with 1-4 hex digits a channel, `rgba:`, `#rrggbb`; ST or BEL) decides
theme auto by luminance (light above a relative luminance of 0.18, where
black text contrasts more than white), is the diagram background Mermaid
gets in the terminal, and is what pictures composite over: block art and
sixel keep only clear (alpha < 50 %) or opaque, so a translucent pixel's
colour is first blended over it (`rtx_timg_matte`; kitty and iTerm2
composite alpha themselves), and a stale picture fades toward it rather
than toward black. With no answer the theme's guess stands in (white for
light, black for dark); a terminal that answers late (after DA1) has its
reply swallowed like any other.

**tmux**: the kitty query and XTVERSION go through DCS passthrough; only
if the outer terminal's echo comes back (`allow-passthrough` on) are
kitty transmits wrapped for passthrough — the placeholders are plain
cells tmux keeps. Otherwise block art (tmux's own DA1 decides sixel).
Nothing is spawned (`tmux show` is never run).

**SSH** (`SSH_TTY` / `SSH_CONNECTION`): kitty sends chunked base64 (4096
a chunk, `m=1`), never a temp file; a picture over `tui_ssh_kpx`
thousand display pixels (64) is block art unless `tui_images` names a
protocol. Locally kitty gets a temp file per image
(`$TMPDIR/tty-graphics-protocol-cctext-<pid>-<n>`, `t=t`: the terminal
reads and deletes it).

**Kitty images are ours to delete**: ids are `(top byte << 24) | low
byte`, the top byte from the pid (below 2³¹), 255 live ids recycled
least recently painted first (a `d=I` delete for the old one). Exit, ^Z
and the fatal-signal handler write a delete for every image sent
(`a=d,d=I,i=…`, one async-signal-safe `write` of a prebuilt string, next
to the scroll-region reset); resume sends them again.

**Where pictures show**:

- **Rich Markdown**: a line that is one image is a picture row of N
  terminal rows — `rtx_img_cells_set` switches the layout's picture rows
  on for cells, the box from the header and the cell pixel size (the
  pane's width by `image_max_height` or the screen's rows minus 4,
  never above natural size). The rows are reserved from the header
  before a pixel is decoded; an image that failed or waits for a
  decision keeps its one-line stand-in. The caret on the line shows the
  source with the picture under it, as in cctext-ui; an inline image
  stays `[image: alt WxH]`. While pixels are on the way the first row
  shows the stand-in text dimmed.
- **Slides** (`Shift-F5`): a content image at its natural size at the
  slide's scale (Marp `w:` / `h:`), fitted to what is left of the slide;
  `![bg]` cover (cropped to the middle), contain / fit or auto, the whole
  slide or its `left:` / `right:` side. A transition moves cells, so
  pictures are cells then (sixel / iTerm2 fall back to block art for
  those frames), as is a whole-slide background with text over it.
- **Browse preview**: an image file's picture fitted to the preview,
  its `[image: PNG 1200x900, 4.4 KiB]` line under it.
- **The viewer**: an image file opened in cctext shows the picture,
  fitted (never above 100 %); `+` / `=` zoom in, `-` out, `1` actual size,
  `0` fit, arrows / wheel / PgUp / PgDn pan, Home back to the corner;
  typing never edits the bytes; `Ctrl-D` shows the hex and back. A
  picture's box is capped at 296 cells a side (what a kitty placement
  can address).

**Never block**: scaling and encoding run on one background lane (the
loader's lane shape: a job record owning a copy of its pixels, a done
flag, adopt on the UI thread), most recently painted first, cached per
(bitmap, frame, protocol, box, crop) under 96 MiB; a paint pass that no
longer asks for a queued or running encode drops or cancels it (sixel
polls the flag between bands); one not painted for 600 whole paints
goes. The UI thread only copies pixels and writes what is ready. An
idle editor with pictures on screen writes nothing and does not wake;
with `image_animate` an animated GIF repaints at most every 80 ms (the
terminal frame budget), each frame one cached encode.

**Settings** (`settings.json`): `tui_images` (`auto` | `kitty` | `sixel`
| `iterm` | `blocks` | `off`; `RTX_TUI_IMAGES` overrides), `tui_blocks`
(`half` | `quadrant` | `sextant`; `RTX_TUI_BLOCKS`), `tui_dither`
(Floyd–Steinberg in 256 colours), `tui_ssh_kpx`.

## Drawing (cctext-ui)

libui-ng's draw context has no image call; `ui_os_image_new` /
`_draw` / `_free` add one. GTK: a cairo image surface over the bitmap's
own pixels (ARGB32 is premultiplied BGRA on little-endian hosts), drawn
with `cairo_scale` and a GOOD / BILINEAR filter, clipped to the scissor.
macOS: a `CGImage` over the same pixels (`kCGBitmapByteOrder32Little |
kCGImageAlphaPremultipliedFirst`), drawn through a flip because the area
view is flipped — **not built or run here** (no macOS in this
environment). Windows: a stub (placeholders only). One platform image is
kept per decoded bitmap (per frame when animated) and freed with it.

## SVG: the renderer (cctext-render)

SVG is a program-like format (a parser, CSS, text layout, a rasterizer
with fixed-point geometry): none of it runs in the editor. A separate
helper, `cctext-render`, parses and draws; it is locked down before it
reads its first request, and the editor treats everything it sends as
untrusted. Step 1 of the renderer plan was SVG (C / C++ only); step 2
is Mermaid, QuickJS in the same helper ([Mermaid](#mermaid)), then TeX /
MathML, MathJax in the same QuickJS host ([Math](#math)).

**Engine.** [lunasvg](https://github.com/sammycage/lunasvg) 3.5.0
(`cf3594d`, MIT) over [plutovg](https://github.com/sammycage/plutovg)
1.3.3 (MIT), vendored in `third_party/lunasvg` and `third_party/plutovg`
with readable cctext patches (`patches/`, each with its reason; pins in
`README.cctext.md`): `<switch>` / `systemLanguage` / `requiredExtensions`
(draw.io labels), a 256-level nesting cap, the nested-`<svg>` viewport
resolved in O(depth) (was 2^depth: 25 levels took 3.5 s), family names
matched case-insensitively, bounded offscreen canvases; in plutovg,
coordinates clamped before the fixed-point stroker, over-dense dashes
drawn solid, non-finite curves drawn as chords (the fuzz findings below).
Built with `LUNASVG_DISABLE_EXTERNAL_RESOURCES`: an `<image
href="file:…">`, `http:` or relative href is never opened and renders as
missing; a `data:` image inside the SVG is decoded by plutovg's
stb_image, inside the sandbox, capped at 16384 px a side.

**Fonts.** Only what is bundled: Noto Sans Regular / Bold / Italic, Noto
Serif, Noto Sans Mono 2.0 (OFL, `third_party/fonts`). No system font
discovery. A `font-family` list is walked in order; generic families
(`sans-serif`, `serif`, `monospace`, `system-ui`, `cursive`, …) and the
names drawing tools write (Arial, Helvetica, Verdana, Segoe UI, Times New
Roman, Georgia, Courier New, Menlo, Consolas, DejaVu Sans, …) map to the
bundled faces (`render/cr_svg.cpp`); anything else falls back to Noto
Sans.

**Build.** Only a C / C++ compiler and ccc: `./make.shcc @cctext_render`
(`@cctext` and `@cctext_ui` run it first) spawns `$CC` / `$CXX` (default
`cc` / `c++`) directly from `scripts/render_build.cch` — ccc builds C
only, and no make, cmake, meson or shell script is involved — one
compile per CPU, rebuilt when a source, a header or the flags change.
The helper then builds its own font pack: `bin/cctext-render
--build-pack render/manifest.txt bin/cctext-render.pack` (every entry
zlib-compressed at level 9 with miniz's deflate (`third_party/miniz`,
build time only) and checked by a round trip through Wuffs, which
inflates it at run time; format `CRPK0003`: the table of
names, lengths and offsets at the front, guarded by an FNV-1a trailer;
the helper maps the file read-only and pages in only what it uses).
Outputs: `bin/cctext-render` (1.4 MiB stripped: ≈ 450 KiB of it
the SVG engine, the rest QuickJS), the pack (6.9 MiB: 1.1 MiB of fonts,
2.2 MiB of Mermaid bytecode, 3.6 MiB of math — [Mermaid](#mermaid) has
the two-step build, [Math](#math) the breakdown) and
`bin/cctext-render-selftest` (tests only). `@dist_cctext` packs the
helper and its pack beside the editors; the editor looks for the helper
beside its own binary (`RTX_RENDER_BIN` overrides; `../bin/` is tried for
`bin-asan/` builds).

**Protocol** (`render/cr_proto.h`). Framed, little-endian, one request at
a time over a socketpair on the helper's stdin / stdout. After lockdown
the helper sends a hello (version 2, the kinds it carries, sandboxed
or not). A request (`CRQ2`, 52 bytes, then the payload) carries a
generation stamp, a kind (SVG, MERMAID, TEX, MATHML, STATS),
flags (size only), the output box or a scale, a pixel cap, the payload
length, and for a script kind its time budget and node cap. The helper
answers **SIZE** (the CSS size; for math also the baseline) or
**ERROR** (a code and one line of text: parse, too large, timeout,
script, engine, …); then, unless
size-only, **PIXELS** (premultiplied RGBA8 at exactly the box) or
**ERROR**. STATS answers **INFO** (the engine's counters as JSON, for
tests and the bench). A box equal to the rounded-up CSS size draws 1:1
(no fractional stretch). A frame
it cannot parse (bad magic, a length over 64 MiB) ends the process; EOF
ends it cleanly.

**Sandbox.**

- Linux (`render/sandbox_linux.c`): after the fds above 2 are closed, the
  pack read, the fonts registered and the engine warmed once (lazy
  statics initialise before the lockdown): `RLIMIT_AS` 2 GiB,
  `RLIMIT_CPU` 600 s over the process's life, `RLIMIT_NOFILE` /
  `RLIMIT_NPROC` / `RLIMIT_CORE` 0 (no `RLIMIT_FSIZE`: stderr may be a
  log the editor owns), `no_new_privs`, then a seccomp-bpf filter (raw
  BPF): `read` on fd 0 only, `write` on fds 1 / 2 only, memory
  (`mmap` / `mprotect` without `PROT_EXEC`, `munmap`, `mremap`,
  `madvise`, `brk`), `futex`, signal return / mask, clocks, `getrandom`,
  `close`, `fstat`, `lseek`, `exit`; `open` / `openat` / `stat` / `access`
  / `readlink` fail with `EACCES`; everything else — `socket`, `execve`,
  `fork` / `clone`, `ptrace`, an executable mapping, a read of any other
  fd, the x32 ABI — kills the process (`SIGSYS`).
- macOS (`render/sandbox_darwin.c`): the same rlimits (`RLIMIT_DATA`
  where XNU ignores `RLIMIT_AS`) and `sandbox_init` with a deny-default
  SBPL profile allowing only `sysctl-read` and signals to itself (open
  descriptors keep working: Seatbelt checks at open). **Written against
  the documented API and not built or run here** (no macOS in this
  environment): the first macOS build must run the self-tests and
  `svg_smoke` before it ships.
- Windows (`render/sandbox_other.c`, `core/img_svg.c`): no launcher yet
  (AppContainer is the plan), so no helper is spawned and every SVG is a
  placeholder ("SVG renderer not available on this platform yet"); a
  helper built there says so in its hello and refuses every request.

**The editor side** (`core/img_svg.c`, plain C). A pool of three
helpers, one per slot — SVG, Mermaid and math, each with its own lock,
process and stamps, so a slow diagram never holds back an SVG or a
formula — each spawned
lazily by its first request (from a background job, never the UI
thread), kept for the next ones, and told to quit at exit (it also exits
on EOF when the editor dies). Its environment is empty (nothing of the
editor's leaks into the process that parses untrusted input). Requests
are serialized; each has a generation stamp. The **time budget** is the
editor's (`svg_timeout_ms`, 5 s): past it the helper is killed and the
next SVG respawns one; a crash (EOF or a broken frame) is reported the
same way (an old helper that dies is retried once on a fresh one, in
case it died of `RLIMIT_CPU` old age). **Cancel** (the paint pass no
longer wants the picture) returns at once and leaves the helper working;
its late reply carries the old stamp and the next request drops it
(a cancel lands only between whole frames, never inside one).

**Caches.** Keyed by the content (FNV-1a 64 of the bytes and their
length), so the same SVG under two names is one entry: the intrinsic
size in memory and, in the Safe home (`<safe>/img/svg/`), a `.size`
file and one `.px` file per rendered size (RLE of premultiplied pixels,
FNV trailer; a corrupt file is ignored). Sizes and pixels are separate
files, so laying out a cached SVG reads only its size: a later session
lays out and paints cached SVGs without starting the helper. Oldest
files go past `svg_cache_mb` (64 MiB; 0 turns the disk cache off).

**In the image cache.** An SVG is sniffed from its bytes (a BOM, blanks,
an XML declaration, comments or a DOCTYPE with an internal subset, then
`<svg`), not its name, so `data:image/svg+xml` URIs and files without the
extension work. It is `RTX_IMG_FMT_SVG` in `core/img.ccs`: the header
probe is a size-only request (the whole file, within `svg_file_mb`), the
decode renders at the display box — scaled up as well as down (a vector
stays sharp), within `svg_max_mp` and the side limit (past them it
renders smaller and the blit scales) — and the pixels are converted to
the layout the caller keeps (BGRA premultiplied for the GUI, straight
RGBA for a terminal protocol). Everything else is the raster path:
the permission rules (project root, the allow file / dir / ALL prompt,
remote consent, `data:` URIs), the pixel budget and LRU, the jobs
(visible first, cancelled when scrolled away), the placeholders.

**Surfaces.** Everywhere raster images work: `![](x.svg)` in the Rich
lens (a picture row; its box from the size, so nothing moves when the
pixels arrive), slides (content and `![bg]`), the browse preview, and the
viewer. **An `.svg` file opens as text** (it is text, and usually opened
to edit); `Ctrl-D` (Rich / Source) swaps the pane to its picture and
back, with the viewer's zoom keys (a zoom re-renders sharp) — the
preview shows the file on disk, so save to see an edit. cctext (the
terminal) shows the stand-in `[image: alt WxH]` until terminal graphics
land. SVGs draw as they are; in cctext-ui's dark theme a light backing
plate goes under them (`svg_backing`, default on; slides paint their own
background and skip it), since many SVGs are dark lines on transparency.
The light theme never draws the plate (the pane already is light).

**Limits** (`settings.json`):

| Limit | Default | Setting |
|---|---|---|
| SVG bytes one render may carry | 16 MiB | `svg_file_mb` |
| Rendered pixels | 16 megapixels | `svg_max_mp` |
| One render (then killed) | 5000 ms | `svg_timeout_ms` |
| Disk cache (sizes + pixels) | 64 MiB | `svg_cache_mb` (0 = off) |
| Backing plate in the dark theme | on | `svg_backing` |

The helper's own ceilings sit above these: 64 MiB of input, 64 megapixels
and 32768 px a side per request, 2 GiB of address space.

**Measured** (Linux x86-64, this container; `python3
bench/render_client.py bench`, best of noisy runs): spawn to ready
(fonts inflated and registered, sandbox on) **13.6 ms** median; warm
size-only 0.1–0.9 ms and size + pixels **1–5 ms** per sample SVG
(402 × 232 draw.io-shaped 1.2 ms, 461 × 288 matplotlib 2.5 ms, 569 × 450
Mermaid pie 5.3 ms); helper RSS 7.6 MiB after hello, 10.2 MiB after the
samples. `svg_smoke` sees the same through the editor's client.

**Fuzzing.** `render/cr_fuzz.c` runs the same backend in process under
ASan + UBSan (`bin/cctext-render-fuzz`) over `testdata/svg` with bit
flips, truncations, duplicated chunks, extreme numbers (1e38, NaN, inf),
deep nesting, self / mutual references, splices, attribute garbage and
nested percentage `<svg>`. `@smoke` runs 400 mutants; `./make.shcc
@render_fuzz` runs `RENDER_FUZZ_ITERS` (20000; `RENDER_FUZZ_SEED`).
Findings, all fixed by the patches above: signed overflows in plutovg's
CORDIC / MulDiv / rasterizer on huge coordinates, a `memcpy` from NULL,
an int overflow from 1e-38 dashes, a NULL surface dereference after a
failed 4 GiB group canvas, undefined float -> int conversions in the
texture blenders (`0 - INT_MIN`), and slow renders from flattening (a
curve at FLT_MAX: 16 s; a dashed arc 1e8 units long: 3.3 s, now 60 ms).
After the last fix, three runs of 20000 mutants (seeds 11-13) are clean,
about 2 minutes each; the slowest mutants took 23 s, 1.5 s and 0.5 s
under ASan (the first, a mutated Mermaid diagram, is the kind of input
the editor's 5 s budget ends; not yet reduced).

**Follow-ups** (not in this step):

- lunasvg gaps: **filters** (`<filter>` is ignored: content draws
  unfiltered), **kerning** (none; text is a little wider than a
  browser's), **per-glyph fallback** between faces plus a **CJK** face
  (a glyph missing from the chosen face is a box), **synthetic bold /
  italic** for Noto Serif and Noto Sans Mono (they use the regular face),
  `<foreignObject>` (HTML labels are skipped; draw.io's `<switch>`
  fallback text shows instead).
- HiDPI: cctext-ui has no device scale yet; pixels are at 1x.
- More than one helper per kind; `.svgz`; previewing an unsaved SVG
  buffer.
- Pixel jobs share one lane: a slow SVG render (up to its budget) holds
  back raster decodes queued behind it; SVG could get a lane of its own
  (Mermaid has one).
- The terminal's browse preview sniffs the first 32 bytes, so an SVG
  whose root follows a prolog previews as text there (cctext-ui reads
  2 KiB); left to the terminal-images work in `cctext_draw.ccs`.
- Windows: the AppContainer launcher; macOS: run the self-tests.

### Pack compression

The pack's entries are compressed once, by `--build-pack`, and inflated
by Wuffs in the helper; only the build side changed. Measured on this
container (`python3 bench/render_client.py`, and a harness running each
encoder over every entry and Wuffs over each result, best of 5):

| | stb (fixed Huffman) | miniz level 9 | zopfli (15 iterations) |
|---|---|---|---|
| Fonts (5 faces, 2.2 MB) | 1.31 MB | 1.14 MB | 1.06 MB |
| `js:mermaid` bytecode (8.46 MB with line tables) | 4.22 MB | 3.45 MB | 3.33 MB |
| `js:math` bytecode (1.76 MB with line tables) | 0.96 MB | 0.71 MB | 0.65 MB |
| Math source modules (74 entries, 10.2 MB) | 4.90 MB | 3.20 MB | 2.88 MB |
| Pack | 11.39 MB | 8.49 MB | 7.91 MB |
| Encoder time, all entries, one core | 1.5 s | 2.3 s | 89 s (42 s at 1 iteration: 7.97 MB) |
| Wuffs inflate, all entries | 67 ms | 72 ms | 76 ms |

miniz (`third_party/miniz`, MIT, only its deflate encoder, compiled into
`cr_pack.c`) is the default and the only encoder: zopfli would save
another 0.6 MB (7 %) for 40 times the encoder time on every pack
rebuild, so it is not vendored. The bytecode is also compiled without
debug information (`JS_STRIP_DEBUG`, [Mermaid](#mermaid)): `js:mermaid`
is 6.15 MB raw and 2.26 MB packed, `js:math` 1.59 and 0.61 MB. Together:

| | before | now |
|---|---|---|
| `bin/cctext-render.pack` | 11,393,329 B (10.9 MiB) | 7,211,183 B (6.9 MiB) |
| `--build-pack` wall time | 5.5–6.0 s | 4.1–4.3 s (less bytecode to write outweighs the slower encoder) |
| Mermaid engine: bytecode inflate / start / first pie | 30–32 / 59–65 / 153–162 ms | 22–23 / 54–55 / 132–137 ms |
| Math engine: bytecode inflate / first formula | 6.0–6.2 / 43–46 ms | 5.3–5.6 / 38–40 ms |
| RSS with the Mermaid / math engine up | 42.0 / 21.8 MiB | 34.5 / 19.4 MiB |

Dynamic Huffman blocks inflate about 7 % slower than stb's fixed ones for
the same bytes; stripping the bytecode more than pays for it. Not done:
the fonts and MathJax's files are the vendored files byte for byte
(pinned by SHA-256), never minified; no two entries are identical, and
compressing the 40 font modules as one stream would save 13 KB (a
32 KiB window finds little across files), so entries stay independent
streams. The format is `CRPK0003` as before and the pack is still
deterministic; a pack changes only in its payload bytes, whose hashes
the build writes into `cr_pack_hash.h` as before.

## Mermaid

A fenced block whose info string is `mermaid` (any case, backticks or
tildes) is a diagram in the Rich lens and on slides, in both frontends.
The official Mermaid 12.0.0 runs, unmodified, in QuickJS inside
`cctext-render`, behind the same sandbox as SVG; the SVG it writes is
drawn by the same lunasvg. Nothing needs a browser, Node, npm, Rust or a
bundler to build: a C / C++ compiler and ccc.

**Using it.**

- In a Rich Markdown pane the fence is one picture row (its opening
  line; the others lay out nothing, like a fold). Put the caret into the
  fence (the arrow keys, or a jump to its line) and its source
  shows as text with the diagram under the closing line; leave it and
  the diagram is back. The bytes are the fence as typed: selection,
  copy, search and undo act on the text.
- While a diagram renders its row is a small box (`rendering diagram…`
  in cctext-ui, `[diagram: rendering...]` in a terminal), sized from the
  diagram once its size is known. Typing in a fence keeps the previous
  diagram up, dimmed, with its box, until the new one has pixels (the
  workbook's stale convention); a source that does not parse keeps it up
  with the parse error as its caption in cctext-ui
  (`mermaid: Parse error on line 3: …`); with no earlier diagram the box
  says why (`[diagram: Parse error on line 3: …]`,
  `diagram too large to render (N nodes, limit 150)`).
- In a terminal a diagram is a picture like any other (kitty, sixel,
  iTerm2 or block art: `tui_images` / `RTX_TUI_IMAGES`).
- On a slide a fence is a diagram too, fitted to the content box and
  moved, faded, clipped or scaled by transitions with everything else;
  it takes the slide's colours (dark when the slide's background is).
- Settings (`settings.json`): `mermaid` (on; off lays fences out as
  code), `mermaid_nodes` (150; 0 = no cap), `mermaid_timeout_ms`
  (10000), `mermaid_max_kb` (64: a longer source is not rendered),
  `mermaid_max_height` (720 px: taller diagrams are scaled down),
  `mermaid_recycle_jobs` (0: a fresh engine every N diagrams when set).

<a id="themes"></a>**Themes.** A diagram is keyed by its source's hash, its length, the
theme and the renderer version (`RTX_MERMAID_VERSION`). The theme is
Mermaid's `dark` (the dark editor theme) or `default` (the light one),
plus `themeVariables` from the palette's diagram roles (`mm_bg`,
`mm_fg`, `mm_accent` in `core/theme.cch`): the node fill is the
background mixed toward the accent (22 % dark, 12 % light), text and
labels the foreground, lines the foreground toward the background. In a
terminal that answered OSC 11 the background is the terminal's. A slide
passes its own colours (dark or light by its background's luminance).
Switching the editor theme (`theme` auto following the OS or the
terminal, **Toggle Light/Dark**) calls `rtx_img_mm_theme_set` and
`rtx_img_math_fg_set` once (`rtx_ui_theme_sync`): every diagram and
formula is re-keyed, the layouts refill, and the visible ones render
again first on their lanes (the old pictures stay up, dimmed,
meanwhile); diagrams and formulas off screen are not rendered until they
are scrolled to, and switching back finds the first theme's pictures
still in the memory / disk caches.

**Security.** Everything the SVG path has (the seccomp sandbox, an empty
environment, no fetches, the editor treating every byte back as
untrusted, the budget enforced by killing), plus Mermaid's own strict
mode: `securityLevel: 'strict'` (no click handlers, no links out, labels
sanitized), `htmlLabels: false` everywhere (labels are SVG `<text>`; a
`<foreignObject>` would need a browser), `deterministicIds`,
`suppressErrorRendering`; and a `secure` list so no `%%{init}%%`
directive or front matter can change those keys, the text limit, the
edge limit or the font. The DOM Mermaid draws into (`render/js/mm_dom.js`)
has no network, no timers beyond the job, no `Image` loading (its
`decode()` rejects), no layout engine but the host's font metrics. The
QuickJS bytecode is trusted input to QuickJS, so it is only ever read
from the pack this build made: the pack's `js:mermaid` entry is checked
against a SHA-256 compiled into the helper (`CR_PACK_JS_HASHES`) before
`JS_ReadObject` sees it; a pack with other bytecode renders SVG and
refuses Mermaid ("the Mermaid engine does not match this renderer
(rebuild the pack)").

**Limits.** Layout is what costs (≈ 25 ms a node at worst), so the node
cap is counted from Mermaid's own parse before any layout — flowchart
vertices and subgraphs, sequence actors and messages, classes and
namespaces, every (nested) state, ER entities, Gantt tasks, pie slices —
and a diagram over it is refused in milliseconds with "diagram too large
to render (N nodes, limit 150)", keeping the engine. The time budget
(`mermaid_timeout_ms`) is enforced twice: QuickJS's interrupt handler
ends the script at the budget (the helper answers "diagram took too long
to render" and lives), and the editor kills the helper 2 s past it
should the helper itself stop answering. A source longer than
`mermaid_max_kb` is not sent; Mermaid's `maxTextSize` is set to match.

**The engine** (`render/cr_js.c`). Started lazily by the first diagram
(an SVG-only session never pays for it): the bytecode is inflated once
and kept, the realm starts from it, and each request is one call of
`mmRender(id, source, options)` with its promise settled by a small
event loop (timers are run to completion; a promise that can never
settle is an error, not a hang). The allocator keeps an exact heap count
(a 16-byte size header), so recycling decisions are O(1). A fresh engine
replaces the old one after a script failure or a timeout (a failed call
may leave the realm in any state), when the heap passes 256 MiB
(`--recycle-mb`), and, when set, every N diagrams (`--recycle-jobs`,
`mermaid_recycle_jobs`); a source that does not parse, or a node-cap
refusal, keeps the engine (typing makes one every other keystroke). A
cycle collection runs after every 16 MiB of heap growth. The helper keeps
the last four diagrams' SVG by payload hash, so a pixel request after a
size request (or at a second size) does not run the script again.

**The editor side.** The fence scan is in the layout fill
(`rtx_layout_mm_*`, gated like images: Rich, and a fence in the
window); the source is hashed and handed to the image cache as
`RTX_IMG_SRC_MERMAID` / `RTX_IMG_FMT_MERMAID`. Sizes and pixels are jobs
on a lane of their own (lane 2), visible first, cancelled when scrolled
away or typed past (a source nobody asked for in the last pass stops its
job; the helper's late reply is dropped by its stamp). Sizes and pixels
are cached in memory and in the Safe home (`<safe>/img/mermaid/`, the SVG
cache's format: a `.size` file apart from the `.px` files, so a later
session lays out cached diagrams without starting the helper). The stale
display remembers, per (document, fence start), the last diagram that
had pixels; its faded copy is made from those pixels at once (resampled
when the box re-fits), never queued behind the render it is waiting
for. Old diagram entries nobody asked for in 120 passes are freed. An
idle editor with diagrams on screen does nothing (zero wakeups).

**Vendored, pinned.**

- QuickJS (Bellard / Gorlich, MIT) at `a38171d` (2026-06-04), the engine
  files only: `third_party/quickjs` (`README.cctext.md`: the pin, the
  files, how to update). Built with `-fwrapv`, warnings off for its
  files only.
- Mermaid 12.0.0: `third_party/mermaid/mermaid.min.js`, the npm dist file
  byte for byte (SHA-256
  `28fca7ae6ebc7ed7bb63bde63136a74bfef14f296a57e403657eeb8b32836073`,
  checked against npm's integrity and again by every build: the manifest
  names the hash and the build refuses another file). MIT; its bundled
  dependencies' licences are in `THIRD_PARTY_LICENSES.txt` (83 packages:
  MIT, ISC, BSD, Apache-2.0, and elkjs under EPL-2.0 — shipped as part of
  the bundle, unused by the diagrams we lay out, which use dagre).
- Our code around it, each file commented: `render/js/rt_shim.js`
  (`console`, timers, `atob` / `btoa`, `TextEncoder` / `TextDecoder`, the
  legacy `RegExp.$1`, an empty `Intl`), `render/js/mm_dom.js` (the DOM
  subset Mermaid and d3 touch: elements, attributes, a CSS-selector
  subset, `getBBox` / `getComputedTextLength` from the host's font
  metrics, serialization), `render/js/mm_glue.js` (configuration, the
  node count, the entry point).

**Build.** Two steps, both driven by `scripts/render_build.cch`:
`out/render/cctext-render-boot` (the helper built with `-DCR_BOOTSTRAP`)
compiles each manifest bundle (`bundle|mermaid`: the shims, the verified
Mermaid, the glue, concatenated) to QuickJS bytecode and writes the pack
(deterministic: the same inputs give the same bytes) and
`out/render/cr_pack_hash.h`; the helper proper is then compiled with that
header. The pack is rebuilt when a font, a script, the manifest or the
boot binary changes. Bytecode is compiled with all debug information
stripped (QuickJS's `JS_STRIP_DEBUG`, as `qjsc -s`: no source text, file
name, line table or local variable names; the helper reports an
exception's message, never its stack, so what a user sees is the same):
6.2 MB raw (from 5.6 MB of source; 8.5 MB with line tables) and 2.3 MB
in the pack (4.2 MB before: line tables and stb's fixed-Huffman deflate).

**Measured** (Linux x86-64, this container, release build;
`python3 bench/render_client.py mermaid`, `mermaid_smoke`):

| | |
|---|---|
| Helper spawn to ready (SVG only; the engine not started) | 14–20 ms |
| First diagram in a helper (inflate 22–23 ms + engine start 54–55 ms + a small pie) | ≈ 135 ms (was 150–160 ms with line tables in the bytecode) |
| Warm render, size + pixels: pie / Gantt | 17–21 / 26–32 ms |
| sequence / ER / flowchart | 70–116 / 160–210 / 190–245 ms |
| class / state | 325–425 / 275–400 ms |
| Re-raster of a cached diagram (another size) | 3–5 ms |
| Helper RSS: after hello (lazy) / engine loaded / after a flowchart | 12.1 / 34.5 / 46.5 MiB (engine loaded: 38.6–42 before the bytecode lost its debug info) |
| after the seven types / after 250 diagrams | 66 / 73 MiB (JS heap 33–47 MiB, stable) |
| Parse error or node-cap refusal | a few ms, engine kept |
| Pack / helper binary (before math; with it: [Math](#math)) | 5.3 MiB / 1.4 MiB |

Lazy start is what keeps an SVG-only session at 12 MiB: starting the
engine eagerly would put every helper at ≈ 39 MiB (the inflated bytecode
and the realm built from it) and add ≈ 100 ms to every spawn.

Against Chromium (the same sources through mermaid 12.0.0 in a browser
with `htmlLabels: false`, the SVGs in `testdata/mermaid/chromium`, drawn
by our lunasvg): widths within +3.5 %, heights +0 to +11.7 % (sequence
diagrams are the tallest: our text metrics lack kerning and line boxes
round up), and the 16 × 16 ink maps correlate at r 0.929–1.000.

**Fuzzing.** `bin/cctext-render-fuzz --mermaid` runs the engine in
process under ASan + UBSan over `testdata/mermaid`: byte flips,
truncation, lines deleted / duplicated / swapped, token garbage (arrows,
brackets, `%%`, `click`, `<script>`, `javascript:`, bidi and astral
characters), hostile lines (`%%{init}%%` overrides and the like), another
type's lines spliced in, long labels and ids, deep subgraph / state /
namespace nesting, node counts around the cap, control bytes and invalid
UTF-8, extreme numbers, each in the light or dark theme within a budget
(`CR_FUZZ_MM_BUDGET_MS`); a rendered SVG goes through the SVG path too. `@smoke` runs 16 mutants (5 s budget, ≈ 1
minute under ASan); `@render_fuzz` runs `RENDER_FUZZ_MM_ITERS` (1000).
The seeds must render. Found on the way (all in our shims and host,
none in QuickJS or Mermaid): scripts reaching for `Intl` and `Image`, a
node cap that nested subgraphs / namespaces / states slipped past, the
engine recycled by every refusal and parse error. No sanitizer finding in 316 mutants under ASan and 400
in a release build (outcomes: renders, parse errors, node-cap refusals,
budget timeouts).

**Leftovers.** macOS and Windows are untested (Windows has no helper at
all). A terminal shows the stale diagram without its caption (the error
reaches the one-line box only when there is no earlier diagram). 
A fence whose opening line is above the layout window's start lays out
as text until the view reaches it. `htmlLabels` stays off, so Markdown
strings in labels draw as plain text; KaTeX in labels, icons (`@{ icon
}`), and the ELK layout are not supported. Diagram types beyond the seven
we test (mindmap, timeline, git graph, quadrant, sankey, XY, block,
architecture, …) are rendered by Mermaid but not checked against a
browser.

## Math

TeX in Markdown — `$…$` inline, `$$…$$` display, and ` ```math `
fences — is typeset in the Rich lens and on slides in both frontends by
the official MathJax 4.1.3, unmodified, in QuickJS inside
`cctext-render`, behind the same sandbox as SVG and Mermaid. The SVG
MathJax writes is drawn by the same lunasvg. Building still needs only a
C / C++ compiler and ccc: no Node, npm, Rust or bundler.

**Using it.**

- **Display math**: a `$$` block (the `$$` lines and the lines between)
  or a ` ```math ` / `~~~math` fence is one picture row, centred, in the
  text colour; the other lines lay out nothing (like a Mermaid fence). A
  formula wider than the pane is broken by MathJax at its operators to
  fit (the pane's width, in 32 px steps, is part of the request); until
  that one is sized, and when it cannot break, the formula is scaled
  down to the pane. Put the caret into it and the source shows as text with
  the formula under it; leave it and the picture is back.
- **Inline math** in cctext-ui: `$…$` in a line of text is a picture on
  the text's baseline (MathJax reports the depth below the baseline, so
  `x_1`, `\frac{a}{b}` and `\sum` sit as in a typeset line); a line
  holding a tall formula grows to fit it, a line without one keeps its
  height. `$$…$$` inside a line of text is display style in the line.
  The caret inside a formula shows its source (the Rich lens's reveal
  rule); selection, copy, search and undo act on the bytes.
- **Inline math in a terminal** stays source, styled as math
  (`markup.math`: violet), with the caret or without. `"math_unicode":
  true` shows a Unicode approximation when the formula has one (`x^2` →
  `x²`, `\alpha \le \beta` → `α ≤ β`, `\sqrt{x}` → `√x`, `\frac{a}{b}` →
  `a/b`, `\mathbb{R}` → `ℝ`; anything it cannot write — a fraction of
  fractions, a matrix, an unknown command — stays source).
- **Display math in a terminal** goes through the picture path (kitty,
  sixel, iTerm2 or block art): block art draws formulas at 2.5 times the
  text size (a cell is too coarse for text-sized glyphs), the others at
  1.25 times.
- **While it renders** a display formula is a small box
  (`rendering math…` in cctext-ui, `[math: rendering...]` in a terminal);
  an inline formula shows its source until its size is known (then the
  line is laid out once, with the picture's box). Typing in a formula
  keeps the previous one up, dimmed, until the new one has pixels (the
  workbook's stale convention; inline and display alike).
- **Errors**: a formula that does not parse is not drawn. MathJax's
  message comes back as an ERROR (`math: Missing close brace`, `math:
  Undefined control sequence \foo`): under the previous formula as a
  caption while one is up, else in the box (`[math: Missing close
  brace]`). An inline formula that does not parse stays source (the
  source is what to fix). We chose this over MathJax's own red `merror`
  rendering: a picture of an error in the text colour looks like
  typeset math, and the source is right there.
- **Slides** (both frontends): `$$` blocks and ` ```math ` fences are
  formulas in the slide's text colour, fitted to the content box and
  moved by transitions with everything else. A terminal slide without
  pictures shows the Unicode approximation, else the source.
- **Theme**: the formula takes the text colour (MathJax's
  `currentColor` becomes the request's `fg`), which is part of its key:
  a colour change (`rtx_img_math_fg_set`) re-keys every formula; the
  visible ones render again (the old pictures stay up, dimmed,
  meanwhile), the rest when scrolled to. The colour is the palette's
  `math_fg`: light text in the dark theme, dark text in the light one
  ([Themes](#themes)).
- Settings (`settings.json`):

| Setting | Default | |
|---|---|---|
| `math` | on | off: `$`, `$$` and math fences stay source |
| `math_inline` | on | off: inline `$…$` stays source in cctext-ui too |
| `math_unicode` | off | terminal: the Unicode approximation of inline math |
| `math_max_kb` | 8 | a longer formula is not rendered (1–1024) |
| `math_nodes` | 4000 | MathML nodes one formula may make (0: no cap) |
| `math_timeout_ms` | 4000 | one formula (100–120000) |
| `math_recycle_jobs` | 0 | a fresh engine every N formulas (0: never) |

**The helper's third slot.** Math has a helper process and a job lane
of its own (slot 2 in `core/img_svg.c`, lane 3 in `core/img.ccs`), so a
formula never waits behind a Mermaid layout (which can take its whole
10 s budget) or an SVG. A document with math and no diagrams starts only
the math helper; each helper starts only the engine its requests need.

**Protocol** (`render/cr_proto.h`, still version 2: the fields existed).
`CR_KIND_TEX` (TeX source, no delimiters) and `CR_KIND_MATHML` (a
`<math>` element). Flag bit 0 is display (else inline, `\textstyle`);
`em_px` is the font size the formula is set at (the editor passes its
text's em: from the GUI font's metrics; in a terminal from the cell
size, larger for block art);
`fg` is the colour (RGBA; MathJax's `currentColor`); `max_w` (the old
reserved field) is the width display math breaks to fit (0: no limit).
The reply is **SIZE** with a third float, the **baseline's distance from
the bottom** of the box (the depth), then **PIXELS** at the box. The
node cap and time budget are the script kinds' fields. The hello
advertises TEX and MATHML.

**The engine** (`render/js/mj_shim.js`, `mj_glue.js`). MathJax's own
components — `startup`, `core`, `input/tex` (base packages), `input/mml`,
`output/svg`, `adaptors/liteDOM`, the `mathjax-newcm` SVG font — are
wrapped at build time in `__cctextMods[key] = function () {…}` and
compiled with our loader shim into one bytecode bundle (`js:math`). Its
startup is `mjRender(kind, source, options)`: TeX or MathML → MathJax's
SVG (`fontCache: 'local'`, `currentColor`, inline line breaking off,
display overflow `linebreak` at `max_w`) → `{w, h, baseline}` and the
SVG → lunasvg. The configuration: every TeX extension in the pack
autoloads on first use (`\require` and `autoload` go through
`__mjRequire`, which only ever reads the pack), `noundefined` is off (an
unknown macro is an error, not red text), `maxMacros` 1000 and
`maxBuffer` 5 KiB (TeX's own guards against macro recursion and
expansion; MathJax's default `maxMacros` is 10000: lower, a
`\def\a{\a\a}` bomb ends long before the time budget),
`formatError` throws, and each TeX formula runs inside
`\begingroup … \endgroup` (the `begingroup` extension), so a `\def` or
`\newcommand` in one formula never reaches the next. After the input
jax, a filter counts the MathML nodes (`math_nodes`: "formula too large
(N nodes, limit M)") and turns an `merror` into an ERROR.
Engine lifetime is Mermaid's: lazy, recycled after a timeout, a script
failure, 256 MiB of heap or `math_recycle_jobs` formulas; a parse error
or a node-cap refusal keeps it. The helper keeps the last four formulas'
SVG, so the pixel request after a size request does not typeset again.

**What is in the pack, what is left out.** No speech rule engine (SRE),
no assistive MathML, no menu, no HTML output: the editor has the source
for a screen reader, and SRE alone is several MB. The TeX extensions are
in the pack as **source** (`src:` entries), not in the bytecode: most
formulas use none, and one is compiled only when a formula first needs
it (`\require{…}`, or a macro that autoloads one: `\cancel`,
`\boldsymbol`, `\bbox`, `\enclose`, `\mathtools` macros, `\color`
with its extended forms, …). So are MathJax's **dynamic font files**
(the glyph ranges past the base font: double-struck, script,
fraktur, arrows, large operators, …, 40 files and 10 MB of source):
loaded on first use of a glyph in their range, then freed with the
engine. Left out: `html`, `texhtml` (HTML in math), `setoptions`
(changes our configuration from a formula), and `mhchem`, `bbm`,
`bboldx`, `dsfont` (they need MathJax font extensions, not vendored:
`\ce{…}` is an unknown macro). The list is in `render/manifest.txt`.

**Security.** Everything the SVG and Mermaid paths have (the seccomp
sandbox, an empty environment, no fetches, every reply untrusted, the
budget enforced twice: QuickJS's interrupt handler at the budget, a kill
by the editor 2 s past it). `\href` (base TeX) lands as a link in an SVG
that lunasvg draws and nothing follows; the `html` extension (`\class`,
`\style`, `\cssId`, `\data`) is not in the pack. A formula too deep for
QuickJS's stack is "formula too deeply nested" and the engine is
recycled. **Bytecode and source alike are
only ever read from this build's pack**: the `js:math` bundle and every
`src:` module are checked against SHA-256 digests compiled into the
helper (`CR_PACK_JS_HASHES`) before QuickJS sees them; a tampered module
is refused ("the math engine does not match this renderer (rebuild the
pack)"; for a `src:` module, "`<key>` does not match this renderer": the
formula that needed it fails, the other formulas carry on). A module is read only through the
host's `__hostLoad(key)`, which resolves keys in the pack and nowhere
else (`__mjRequire` refuses `html`, `texhtml`, `setoptions` by name).
The pack is mapped read-only before lockdown; nothing is read from a
writable place.

**Caches.** A formula is keyed by its source's FNV-1a 64 hash and
length, its look (display or inline, em, colour, max width) and the
renderer's version (`RTX_MATH_VERSION`: helper protocol, MathJax, font,
glue). In memory: the size and baseline, and pixels (the image cache's
LRU). On disk in the Safe home (`<safe>/img/math/`): a `.size` file
(`RTXS 1 w h baseline`) apart from the `.px` files, so a later session
lays out cached formulas — inline ones need their size and baseline
before the line can be laid out — without starting the helper. The
stale display remembers, per (document, formula start), the last formula
that had pixels.

**Never blocking.** Every formula is a job on lane 3, visible first,
cancelled when scrolled away or typed past; the frame paints what is
there (the source, the box, or the stale picture). An idle editor with
formulas on screen does nothing (zero wakeups; `ui_math_test`).

**Vendored, pinned.** `third_party/mathjax` (MathJax 4.1.3, the npm
`mathjax@4.1.3` files byte for byte) and `third_party/mathjax-newcm-font`
(`@mathjax/mathjax-newcm-font@4.1.3`), Apache-2.0, `LICENSE` beside
them; `README.cctext.md` lists every file with its SHA-256 (checked
against npm's sha512 integrity when vendored) and how to update. The
manifest names each hash and the build refuses a different file. Our
code around them, each file commented: `render/js/mj_shim.js` (the
module loader and the configuration), `render/js/mj_glue.js` (the entry
point, the node count, errors), and `render/js/rt_shim.js` (shared with
Mermaid).

**Measured** (Linux x86-64, this container, release build;
`python3 bench/render_client.py math`, `math_smoke`):

| | |
|---|---|
| Pack: before math / with math | 5.3 / 10.9 MiB with stb's deflate (+0.9 MiB bytecode, +4.9 MiB source modules); 6.9 MiB now (miniz level 9, bytecode without debug info) |
| `js:math` bytecode | 1.59 MB raw (from 1.49 MB of source), 0.61 MB in the pack |
| Dynamic font files / TeX extensions (source) | 10.0 / 0.2 MB raw, 3.1 / 0.08 MB in the pack |
| Helper binary | 1.4 MiB (unchanged: MathJax is data) |
| Spawn to ready (any slot; the engine not started) | 12 ms (was 13.6 ms before math) |
| Helper RSS after hello | 8.7 MiB |
| First formula in a helper (inflate 5.5 ms + engine start 7 ms + `x^2+1`) | ≈ 40–50 ms |
| First use of a formula (33 in `formulas.json`, fonts loading on demand) | median 11.6 ms, max 63–80 ms |
| Warm formula, size + pixels / size only | median 9.8 / 8.8 ms (max 18 ms) |
| Re-raster of a cached formula | ≈ 2 ms |
| Helper RSS: first formula / after 33 formulas | 19.5 / 33 MiB (JS heap 6 / 14 MiB) |

The pack doubled; a helper does not pay for it: the pack is mapped
read-only, not read, and its table sits together at the front (format
`CRPK0003`), so an SVG helper pages in the table and the fonts, a math
helper the fonts, `js:math` and the source modules it loads. Reading the
pack whole cost 36 ms and 17 MiB per spawn at 10.9 MiB. [Pack
compression](#pack-compression) has how the pack went from 10.9 to 6.9 MiB.

**Fuzzing.** `bin/cctext-render-fuzz --math PACK testdata/math [ITERS]
[SEED]` runs the engine in process under ASan + UBSan: the TeX of
`formulas.json` and hostile seeds mutated (byte flips, truncation,
duplicated and swapped chunks, token garbage: `\def`, `\newcommand`
bombs, `\require`, `\href{javascript:…}`, deep `{`/`\left` nesting,
huge `\hspace`, `\rule`, `\unicode`, astral characters, control bytes,
invalid UTF-8), MathML seeds likewise, each display or inline within a
budget; a rendered SVG goes through the SVG path too. `@smoke` runs 40
mutants; `@render_fuzz` runs `RENDER_FUZZ_MATH_ITERS` (1000). 300
mutants run clean under ASan + UBSan (111 rendered, 174 errors, 3 node
cap refusals, 12 budget timeouts; 180 s): no sanitizer finding in
QuickJS, MathJax or our glue.

**Leftovers.** Inline math on a slide stays source (display math is a
picture). A glyph outside the NewCM font (CJK in `\text{…}`) is a box.
`\binom` differs slightly between TeX and the equivalent MathML (width
within 5 %: MathJax sets the TeX one with its own spacing). No
`\ce{…}` (mhchem needs a font extension). The terminal's inline formula
is never a picture (a line of text is cells); `math_unicode` is the
approximation. macOS and Windows untested (Windows has no helper). The
math helper's pack mapping means the pack file must not be rewritten
under a running helper (the build writes a new file and renames it).

## Tests

- `img_smoke` (`@smoke`): every format (sizes, quadrant colours,
  transparency), a short prefix asks for more, scaled decode holds only
  the result; truncated, corrupt and garbage files; the 100k × 100k and
  12k × 12k bombs refused from the header (and each limit setting);
  cancel; EXIF 1-8 (JPEG) and 6 (PNG `eXIf`) upright; animation off /
  on / frame cap; the Markdown image parser; the path policy (`..`
  escape, nested escape, symlink escape, percent-decoding, missing file,
  allow file / dir / all persisted through a reload and revoked,
  per-project allowances, untitled documents, `data:` up to 1 MiB); the
  cache (header then pixels, LRU under a small budget, re-decode after
  eviction, near-size reuse, one bitmap over budget named); remote
  against a C HTTP server thread (asks, **zero requests before
  consent**, load-once fetches and decodes, not remembered, "always"
  reuses the cache with no request, https-only rules, the byte cap on a
  chunked reply, the timeout).
- `img_fuzz` (`@smoke`: 2000 mutants of `testdata/img`; standalone:
  `RTX_IMG_FUZZ_ITERS`, `RTX_IMG_FUZZ_SEED`, a corpus path): sniff,
  probe, scaled animated decode, EXIF and the Markdown parser on bit
  flips, truncations, byte runs, extreme header fields, splices and
  duplicated chunks. 20000 mutants run clean under ASan + UBSan.
  `rtx_img_fuzz_one` is the per-input body for a libFuzzer build.
- `img_layout_smoke` (`@smoke`): picture rows (placeholder box, then the
  header's box, fitted to the pane), heights equal before and after the
  pixels, the caret on the line reveals the source with the picture
  under it, inline images and fenced ones, the terminal's stand-in text,
  bytes unchanged.
- `tests/tui_pty_test.py image_placeholder`: the stand-ins in a real
  terminal (`tui_images` off: header size, inline, remote host), the
  caret shows the source, **Load Image** prompts, Esc cancels, the file
  is unchanged.
- `img_term_smoke` (`@smoke`): golden block art for `quad.png` (half
  blocks 24-bit / 256 / dithered, quadrants, sextants, one-cell splits),
  `anim.gif` and `exif6.png`; the resampler averages in linear light and
  premultiplied (black + white is sRGB 188); the xterm-256 quantiser; a
  sixel round trip through a decoder in the test (pixels within 3,
  transparency untouched, cancel); PNG + zlib read back by Wuffs,
  identical; kitty placeholder bytes, the diacritics table, base64.
- `tests/tui_pty_test.py image_*`: a fake terminal on the pty
  (`FakeTerm`) answers the startup queries as kitty, sixel, iTerm2 or a
  silent terminal (in tmux, passthrough on or off), reads kitty temp
  files as kitty does and records every command; `fake_screen` feeds
  the output to pyte with a pixel layer (sixel decoded at the cursor;
  text, EL, ED and line moves erase or move it). `image_blocks`: half
  blocks and colours, the first frame within the detection timeout with
  no replies, inline stays text, the caret reveal, scrolled frames equal
  a full repaint with and without scroll regions, 24-bit, quadrants,
  sextants, no colours → the stand-in. `image_kitty`: two transmits with
  a virtual placement, temp files locally and chunked base64 over SSH,
  the data inflates to s × v × 4, placeholder ids / rows / columns match
  the cells, scrolling and splits leave no stale cells, ^Z deletes every
  image and resume sends them again, exit deletes them.
  `image_sixel` / `image_iterm`: pictures drawn in their cells, and after
  every scroll, split and close the text and pixels equal a full repaint
  (nothing torn or stale). `image_tmux`: passthrough on → wrapped kitty
  transmits, off → block art. `image_viewer`: fit, actual size, pan,
  typing eaten, `Ctrl-D` hex and back, bytes unchanged, the browse
  preview's picture. `image_present`: a content image and a split
  `![bg left]` (block art and kitty). `image_idle`: pictures on screen,
  no output and no main-thread wakeups; `image_animate` frames within
  the frame budget. `key_decode_smoke`: late replies are swallowed.
- `tests/ui_img_test.py` (Xvfb): a Markdown picture on screen and still,
  the caret reveal, idle with a picture (no paints, no CPU), an animated
  GIF painting its frames with `image_animate`, a slide's
  `![bg left:40%]`, the viewer and its hex, the browse preview, and a
  remote image end to end (click, dialog via `RTX_UI_SCRIPT`, one fetch
  from a local server, the picture).
- `svg_smoke` (`@smoke`; builds on `@cctext_render`): the seccomp
  self-tests (`bin/cctext-render-selftest` with `CR_SELFTEST=socket`,
  `exec`, `fork`, `mmapx`, `readfd`, `thread` each die of `SIGSYS`;
  `open` gets `EACCES`); the protocol raw (hello, SIZE then PIXELS at the
  box, the request's and the helper's pixel caps, parse / size / kind
  errors with the helper serving on, bad magic and an oversized frame end
  it with status 3, truncated frames end it cleanly); the client (no
  helper before the first SVG, an input over the limit refused without a
  spawn, lazy spawn once, sizes from memory, the time budget kills a
  hanging render and the next request respawns, a crash likewise, a
  cancelled request's late reply dropped by its stamp on the same
  helper, a missing helper); the disk cache (a later "session" with no
  helper binary lays out and paints from it, byte-identical; another
  size needs the helper; a corrupted pixel file is ignored); hostile
  input (40 nested percentage `<svg>` in well under a second, 100k
  nested groups, external `file:` / `http:` references never fetched —
  a listening socket sees no connection — and rendered as missing); the
  seven samples against their reference PNGs (≤ 0.5 % of pixels more
  than 24 levels off; they match exactly here); straight RGBA and BGRA
  premultiplied from the same render; upscaling; SVG in the image cache
  (sniffing, the permission rules, `data:image/svg+xml` base64 and
  percent-encoded, a Rich picture row whose height is the same before and
  after the pixels, the terminal stand-in).
- `render/cr_fuzz.c` (`@smoke`: 400 SVG, 16 Mermaid and 40 math
  mutants; `@render_fuzz`: 20000, 1000 and 1000): see
  [Fuzzing](#svg-the-renderer-cctext-render), [Mermaid](#mermaid) and
  [Math](#math).
- `mermaid_smoke` (`@smoke`): the protocol raw (hello v2, no engine
  before the first diagram, SIZE then PIXELS at the box, the pixel
  request reusing the script's SVG, a parse error naming its line in one
  line and keeping the engine, no diagram type, the node cap refused
  before layout with its message and the engine kept, hostile
  `%%{init}%%` directives ignored, `htmlLabels` forced off); the budget
  (an endless script ends with a timeout and a fresh engine; the editor's
  kill of a hung helper, budget + grace, and the respawn); the heap
  recycle (`--recycle-mb`) and `--recycle-jobs`; the pack hash (a pack
  with other bytecode renders SVG and refuses Mermaid before QuickJS);
  the seven types (flowchart, sequence, class, state, Gantt, pie, ER)
  against reference PNGs from our renderer (±2 px in size, ≤ 1 % of
  pixels more than 24 levels off) and against Chromium's SVGs (width
  ≤ 6 %, height ≤ 13 %, ink map r ≥ 0.90); light and dark themes and
  `themeVariables`; the client (two slots, lazy spawn of the Mermaid slot
  only, the setting for the cap, messages); the caches (memory, disk
  with no helper, the theme in the key); the layout (a fence is one
  picture row, the other lines nothing, the box from the size, the caret
  reveal with the diagram under the closing line, `mermaid` off lays out
  code, the bytes unchanged); the stale display (the old diagram and its
  box while the new one renders, a parse error keeps it with its
  message, replaced once sized); a theme change re-renders only the
  visible diagram and keeps the old one up; idle once settled.
- `tests/tui_pty_test.py mermaid_blocks` (`RTX_TUI_IMAGES=blocks`): a
  fence becomes block art under its line with the fence lines hidden and
  no graphics escapes; idle with no output or CPU once settled; the
  caret shows the source with the diagram under it; typing keeps the old
  diagram up until the new one lands.
- `tests/ui_mermaid_test.py` (Xvfb, cctext-ui): a diagram on screen and
  still, two helper processes (SVG and Mermaid), the dimmed stale
  diagram while an edit renders and the new one after, the helpers exit
  with the editor, a slide's diagram in the slide's light theme.
- `tests/ui_theme_test.py` (Xvfb): the light theme's Markdown (Mermaid's
  light theme with the palette's fill, no SVG plate — the dark theme
  keeps it), formulas in the theme's text colour, a light slide's
  letterbox, a live GTK prefer-dark switch re-rendering the diagram
  (zero wakeups after it), and Toggle Light/Dark. The other Xvfb tests
  run pinned to the dark theme (`RTX_THEME=dark`).
- `tests/ui_svg_test.py` (Xvfb): an SVG picture in Rich Markdown, still
  while idle, the backing plate under a transparent SVG, one helper
  process that exits with the editor; an `.svg` opens as text and
  `Ctrl-D` swaps its picture in and out; a slide's `![bg left:40%]`; the
  browse preview. It saves `testdata/generated/ui_svg_markdown.png`.
- `math_smoke` (`@smoke`): the protocol raw (hello advertises TeX and
  MathML, no engine before the first formula, SIZE with a baseline that
  is ≈ 0 for `x` and the depth for `y`, `\frac` and subscripts, then
  PIXELS at the box with ink, the helper's SVG cache, `fg` colouring the
  formula); errors in one line with the engine kept (missing brace,
  undefined macro), the `\def` recursion bomb ended by `maxMacros`,
  `\require{texhtml}` and `\require{setoptions}` refused, `\href`
  harmless, a `\def` not leaking into the next formula (`\begingroup`),
  the node cap, a huge `\hspace` against the pixel caps, MathML (a
  formula, an error, a non-`<math>` payload), `max_w` breaking a wide
  display formula; the budget (an endless script times out, a fresh
  engine), the heap recycle and `--recycle-jobs`; the pack hash (a pack
  with other `js:math` bytecode, and one with another
  `src:…/double-struck` font file: `\mathbb{R}` refused, `x+1` still
  renders); the client (lazy spawn of the math slot only, size +
  baseline, pixels, messages, the byte limit refused without a request,
  the editor's kill past budget + grace and the respawn, a cancelled
  request's late reply dropped); the disk cache (a later "session" with
  no helper lays out and paints from it, the baseline included); the 31
  formulas of `testdata/math/formulas.json` (fractions, continued
  fractions, roots, sums, integrals, limits, matrices, `cases`, `align`,
  accents, braces, text with Unicode and CJK, Greek, fonts, big
  delimiters, arrows, relations, Maxwell's equations, `\color` with
  `\cancel`, …; two must fail: a missing brace, an undefined macro)
  against committed reference PNGs (±1 px in size, ≤ 0.5 % of pixels
  more than 24 levels off) and against their MathML
  (`testdata/math/mml`, same baseline within 0.05 px): 30 of 31
  identical, `\binom` within 5 % in width; the layout (a `$$` block and
  a ` ```math ` fence are picture rows, the other lines nothing, a
  one-line block, `$$` inside text inline, inline sizes and baselines,
  the grown line's `ytop`, a table cell keeps its source, the caret
  reveal of both, the stale formula inline and display while an edit
  renders and after a parse error, a theme change re-keys and keeps the
  old one up, `math` off and the terminal's Unicode mode, idle once
  settled).
- `tests/tui_pty_test.py math_blocks` (`RTX_TUI_IMAGES=blocks`): a `$$`
  block is block art under its line with the block's lines hidden; the
  caret shows the source with the formula under it; typing keeps the old
  formula up until the new one lands; inline math stays source, or
  Unicode with `math_unicode`; idle once settled.
- `tests/ui_math_test.py` (Xvfb, cctext-ui): a display formula centred
  and still; an inline fraction on its line's baseline, reaching above
  and below the text and centred on it; one helper (the math slot), idle
  with formulas on screen, gone with the editor; the caret reveal; the
  dimmed stale formula while an edit renders and the new, wider one
  after; `"math": false` leaves source and starts no helper; a Marp
  slide's `$$` in the slide's text colour. It saves
  `testdata/generated/ui_math_markdown.png`.
- `testdata/math`: `formulas.json`, `ref/` (our PNGs) and `mml/` (the
  MathML of each formula, from MathJax's own `tex2mml`), regenerated by
  `gen_refs.py` through the helper; `bench/render_client.py math`
  measures.
- `testdata/mermaid`: one source per type, `chromium/` (the browser's
  SVGs with `htmlLabels: false`), `ref/` (our PNGs), regenerated by
  `gen_refs.py`; `bench/render_client.py mermaid` / `leak` measure.
- Samples (`testdata/svg/samples`, our own content: a draw.io-shaped
  diagram, matplotlib plots, Mermaid diagrams from our `.mmd` sources)
  and their references are regenerated by `testdata/svg/gen_samples.py`
  through the protocol; `bench/render_client.py bench` measures.

## Limits and leftovers

- Reference-style images (`![a][ref]`) and HTML `<img>` are text.
- An image inside a line of text in cctext-ui is the text stand-in, not
  a picture (only a line that is one image becomes a picture row;
  inline math is the exception, [Math](#math)).
- No AVIF, HEIC, TIFF or lossy-WebP guarantee (Wuffs 0.4's WebP is
  lossless; its VP8 support is partial). SVG goes through cctext-render
  (its own follow-ups are listed there); no `.svgz`.
- No scaled JPEG decode: the transient canvas is the full image.
- The GUI's camera counts rows against 18 px slots (`GUI_ROW_H`) while a
  prose line is taller; with tall picture rows the caret can sit a few
  lines below the visible bottom before the pane scrolls (the same
  pre-existing approximation prose rows have).
- Terminal: the protocols are checked against a fake terminal on a pty
  here, not against real kitty / foot / iTerm2 / WezTerm / tmux builds
  (none in this environment). Kitty picks its own scale inside the
  placement box. A sixel terminal that does not erase pixels when text
  overwrites their cells would keep stale pixels under rewritten text.
  Pictures in a transition, or under text on a whole-slide background,
  are block art in sixel / iTerm2 terminals. A picture row is one layout
  row N cells tall: like a tall table record it leaves the top of the
  pane whole when the view scrolls past it (its bottom is clipped, never
  its top). A terminal that does not answer OSC 11 gets translucency
  composited over the theme's guess (white / black), which is off for a
  light-grey or tinted background.
- macOS drawing and the Win32 blit are untested / stubbed; NSURLSession
  and libcurl backends are not wired (curl is spawned).
- Decisions are per project root; a document outside any project (no
  `.git` above it) keeps them for its directory.
