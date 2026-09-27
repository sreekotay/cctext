# Images

cctext-ui paints pictures in the Markdown Rich lens, in slides, in the
browse preview and in an image viewer (phase 1); cctext paints them in
the same places with the kitty graphics protocol, sixel, iTerm2 inline
images or Unicode block art, whichever the terminal answers for, else
the text stand-in `[image: alt WxH]` (phase 2, [Terminal](#terminal-cctext)).
SVG goes through the same cache and surfaces, rendered by a separate
sandboxed process, `cctext-render`
([Renderer](#svg-the-renderer-cctext-render)).

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
SVG: `core/img_svg.c` / `.h` (the helper process, its protocol, the
content-hash cache), `render/` (the helper), `scripts/render_build.cch`
(its build).

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
a still window stays at zero wakeups.

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

**Detection** runs once the TTY is raw, before the first frame: a kitty
query (and, when not over SSH, a second one naming a temp file, `t=t`:
an OK means the terminal reads our files), XTVERSION, `CSI 16 t` when
`TIOCGWINSZ` has no pixel size, `XTSMGRAPHICS`, a truecolor `DECRQSS`
probe (unless `COLORTERM` says so), then DA1. Every terminal answers DA1,
so its reply ends the wait; with no reply at all the wait is at most
150 ms (`RTX_TUI_DETECT_MS`), then block art (or the text stand-in).
Keys typed meanwhile stay input; a reply that comes later is swallowed
by the key decoder (an `APC G`, a DCS or an OSC string), never typed.
The cell size is `ws_xpixel / ws_col` × `ws_ypixel / ws_row`, else the
`CSI 16 t` reply, else 8 × 16 (1:2).

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
untrusted. This is step 1 of the renderer plan (C / C++ only); TeX /
MathML (MathJax in QuickJS) and Mermaid are later steps, and the
protocol and the pack already have room for them.

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
zlib-compressed with stb's deflate and checked by a round trip through
Wuffs, which inflates it at run time; an FNV-1a trailer guards the file).
Outputs: `bin/cctext-render` (≈ 450 KiB stripped), the pack (1.3 MiB) and
`bin/cctext-render-selftest` (tests only). `@dist_cctext` packs the
helper and its pack beside the editors; the editor looks for the helper
beside its own binary (`RTX_RENDER_BIN` overrides; `../bin/` is tried for
`bin-asan/` builds).

**Protocol** (`render/cr_proto.h`). Framed, little-endian, one request at
a time over a socketpair on the helper's stdin / stdout. After lockdown
the helper sends a hello (version, the kinds it carries, sandboxed or
not). A request (`CRQ1`, 44 bytes, then the payload) carries a generation
stamp, a kind (SVG; TeX / MathML / Mermaid reserved), flags (size only),
the output box or a scale, a pixel cap and the payload length. The helper
answers **SIZE** (the CSS size) or **ERROR**; then, unless size-only,
**PIXELS** (premultiplied RGBA8 at exactly the box) or **ERROR**. A frame
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

**The editor side** (`core/img_svg.c`, plain C). A pool of one helper,
spawned lazily by the first SVG (from a background job, never the UI
thread), kept for the next ones, and told to quit at exit (it also exits
on EOF when the editor dies). Its environment is empty (nothing of the
editor's leaks into the process that parses untrusted input). Requests
are serialized; each has a generation stamp. The **time budget** is the
editor's (`svg_timeout_ms`, 5 s): past it the helper is killed and the
next SVG respawns one; a crash (EOF or a broken frame) is reported the
same way (an old helper that dies is retried once on a fresh one, in
case it died of `RLIMIT_CPU` old age). **Cancel** (the paint pass no
longer wants the picture) returns at once and leaves the helper working;
its late reply carries the old stamp and the next request drops it.

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
land. SVGs draw as they are; in cctext-ui's dark panes a light backing
plate goes under them (`svg_backing`, default on; slides paint their own
background and skip it), since many SVGs are dark lines on transparency.

**Limits** (`settings.json`):

| Limit | Default | Setting |
|---|---|---|
| SVG bytes one render may carry | 16 MiB | `svg_file_mb` |
| Rendered pixels | 16 megapixels | `svg_max_mp` |
| One render (then killed) | 5000 ms | `svg_timeout_ms` |
| Disk cache (sizes + pixels) | 64 MiB | `svg_cache_mb` (0 = off) |
| Backing plate in dark panes | on | `svg_backing` |

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
failed 4 GiB group canvas, and a 16 s render from a curve at FLT_MAX.
Three runs of 20000 are clean; the slowest mutant takes seconds under
ASan, which is what the editor's time budget is for.

**Follow-ups** (not in this step):

- lunasvg gaps: **filters** (`<filter>` is ignored: content draws
  unfiltered), **kerning** (none; text is a little wider than a
  browser's), **per-glyph fallback** between faces plus a **CJK** face
  (a glyph missing from the chosen face is a box), **synthetic bold /
  italic** for Noto Serif and Noto Sans Mono (they use the regular face),
  `<foreignObject>` (HTML labels are skipped; draw.io's `<switch>`
  fallback text shows instead).
- HiDPI: cctext-ui has no device scale yet; pixels are at 1x.
- A pool larger than one; `.svgz`; previewing an unsaved SVG buffer.
- The terminal's browse preview sniffs the first 32 bytes, so an SVG
  whose root follows a prolog previews as text there (cctext-ui reads
  2 KiB); left to the terminal-images work in `cctext_draw.ccs`.
- Windows: the AppContainer launcher; macOS: run the self-tests.
- Math (MathJax in QuickJS) and Mermaid through the same helper.

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
- `render/cr_fuzz.c` (`@smoke`: 400 mutants; `@render_fuzz`: 20000):
  see [Fuzzing](#svg-the-renderer-cctext-render).
- `tests/ui_svg_test.py` (Xvfb): an SVG picture in Rich Markdown, still
  while idle, the backing plate under a transparent SVG, one helper
  process that exits with the editor; an `.svg` opens as text and
  `Ctrl-D` swaps its picture in and out; a slide's `![bg left:40%]`; the
  browse preview. It saves `testdata/generated/ui_svg_markdown.png`.
- Samples (`testdata/svg/samples`, our own content: a draw.io-shaped
  diagram, matplotlib plots, Mermaid diagrams from our `.mmd` sources)
  and their references are regenerated by `testdata/svg/gen_samples.py`
  through the protocol; `bench/render_client.py bench` measures.

## Limits and leftovers

- Reference-style images (`![a][ref]`) and HTML `<img>` are text.
- An image inside a line of text in cctext-ui is the text stand-in, not
  a picture (only a line that is one image becomes a picture row).
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
  its top). OSC 11 (the background
  colour) is not asked: a half-transparent pixel composites against the
  terminal's own background only at the 50 % alpha cut.
- macOS drawing and the Win32 blit are untested / stubbed; NSURLSession
  and libcurl backends are not wired (curl is spawned).
- Decisions are per project root; a document outside any project (no
  `.git` above it) keeps them for its directory.
