# Images

Phase 1: cctext-ui paints pictures in the Markdown Rich lens, in slides,
in the browse preview and in an image viewer; cctext (the terminal) shows
the text stand-in `[image: alt WxH]`. Terminal graphics (kitty, iTerm2,
sixel) are phase 2; the loader already decodes to straight RGBA at a
requested size for them.

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
(terminal stand-ins and prompt).

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

- **Terminal** (and an image inside a line of text in cctext-ui): the
  stand-in is text, `[image: alt WxH]` — `[image: alt]` while the header
  is on its way, `[image: alt - host]` for a remote image not yet
  allowed, `[image: alt - outside project]`, `[image: alt - reason]`.
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
frame. The terminal slide shows the stand-in text.

## Browse preview and the viewer

An image file in the browse preview is its picture scaled to the pane,
with its format, size and byte count under it (cctext: the stand-in
line). Opened directly, an image file is the **viewer** in cctext-ui:
fitted (never above 100 %), `+` / `-` zoom, `0` fit, `1` actual size,
arrows pan; typing never edits the bytes there. `Ctrl-D` switches the
pane to hex and back (the view is the journal's, so it sticks). These are
files the user picked, so no document policy applies.

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
  terminal (header size, inline, remote host), the caret shows the
  source, **Load Image** prompts, Esc cancels, the file is unchanged.
- `tests/ui_img_test.py` (Xvfb): a Markdown picture on screen and still,
  the caret reveal, idle with a picture (no paints, no CPU), an animated
  GIF painting its frames with `image_animate`, a slide's
  `![bg left:40%]`, the viewer and its hex, the browse preview, and a
  remote image end to end (click, dialog via `RTX_UI_SCRIPT`, one fetch
  from a local server, the picture).

## Limits and leftovers

- Reference-style images (`![a][ref]`) and HTML `<img>` are text.
- An image inside a line of text in cctext-ui is the text stand-in, not
  a picture (only a line that is one image becomes a picture row).
- No SVG, AVIF, HEIC, TIFF or lossy-WebP guarantee (Wuffs 0.4's WebP is
  lossless; its VP8 support is partial).
- No scaled JPEG decode: the transient canvas is the full image.
- The GUI's camera counts rows against 18 px slots (`GUI_ROW_H`) while a
  prose line is taller; with tall picture rows the caret can sit a few
  lines below the visible bottom before the pane scrolls (the same
  pre-existing approximation prose rows have).
- macOS drawing and the Win32 blit are untested / stubbed; NSURLSession
  and libcurl backends are not wired (curl is spawned).
- Decisions are per project root; a document outside any project (no
  `.git` above it) keeps them for its directory.
