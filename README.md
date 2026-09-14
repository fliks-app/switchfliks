# Fliks for Nintendo Switch

A homebrew client for [Fliks](https://github.com/fliks-app/fliks), rendered
with **deko3d**. It reproduces the look and feel of the Fliks TV client — the
same dark daisyUI palette, the same poster rails, the same focus ring, the
same spatial navigation — on a D-pad instead of a remote.

> Requires a Fliks server of your own; this ships no content.

## What it does

- **Server + sign-in** — enter a server address on the system keyboard, pick
  an account from the public user list, sign in. The session is stored on the
  SD card and refreshed automatically.
- **Home** — the client's zone order: libraries, continue watching,
  recommendations, recently added, then a rail per library.
- **Library / search** — poster grid with paging as you reach the end.
- **Detail** — fanart background, poster, metadata row (rating, date, runtime,
  *ends at*, library), genres, resume/play plus **watched** and **like**
  toggles, synopsis, season tabs with episode stills, the cast rail, and a
  **file information** panel: path, size, bitrate, and the video and audio
  stream details the scanner stored.
- **Person** — a cast member opens their page: portrait, dates, biography,
  and what they appear in *in this library*.
- **Quality** — the server's own ladder, bitrates and all, pickable during
  playback without leaving the film.
- **Playback** — the server's HLS or direct stream, decoded on the X1's
  hardware video engine, colour-converted on the GPU, audio through `audout`,
  with seek, resume and progress reported back so your place follows you to
  other devices.

## Building

The toolchain is devkitPro (`devkitA64` + libnx + deko3d). Either install it
and run `make`, or use the container:

```bash
./build.sh            # docker run devkitpro/devkita64, make
./build.sh clean
```

The output is `fliks.nro`. Copy it to `/switch/fliks/fliks.nro` on the SD card
and launch it from the homebrew menu.

### Launch it with title takeover

**Hold R while starting an installed game** to open the homebrew menu in
application mode, then launch Fliks from there.

Opened the usual way — over the album — homebrew runs as a *library applet*
and gets a much smaller slice of system and GPU memory than an application
does. The app adapts (the artwork cache shrinks from 80 MB to 20 MB, the
software decoder drops a thread) and logs which mode it is in, but a poster
wall plus a video decoder is a poor fit for that budget.

### HTTPS

`romfs/cacert.pem` is not committed. If your server is behind TLS:

```bash
make cacert           # fetches the Mozilla CA bundle into romfs/
./build.sh
```

Without a bundle the app cannot verify a certificate, says so on the server
screen, and offers an explicit **Allow unverified TLS** toggle — appropriate
for a self-signed certificate on your own LAN, and nothing more.

Plain `http://` servers need none of this.

### Regenerating assets

Both are committed; rerun only if the brand assets change.

```bash
python3 tools/build_icons.py <lucide-static>/icons   # romfs/icons.png
python3 tools/build_logo.py  external/fliks/client/public/fliks-logo-ondark.svg
```

## Controls

| Button | Action |
| --- | --- |
| D-pad / left stick | Move focus |
| A | Activate |
| B | Back |
| Y | Search (home) |
| X | Reload (home) · statistics overlay (player) |
| − | Open the navigation drawer |
| + | Quit — asks first, from any screen |
| Left at the left edge | Open the navigation drawer |
| ← → (player) | Seek ±10 s |
| L / R (player) | Seek ±60 s |
| Touch | Tap to activate, drag to scroll, swipe from the left edge for the drawer |
| Touch (player) | Tap to show controls, tap a button, drag the scrubber |
| Double-tap left/right (player) | Seek ∓10 s |

## How it is put together

```
source/
  gfx/       deko3d renderer, glyph atlas, artwork cache, image decode
  ui/        spatial navigation, widgets, scrolling, input
  net/       HTTP/TLS client, Fliks API client
  player/    HLS feed, ffmpeg decode (NVDEC), audio out, YUV/NV12 pass
  app/       screens and the application shell
  shaders/   GLSL compiled to .dksh by uam at build time
```

### Rendering

One shader pair draws the entire UI. Every primitive is a quad carrying its
own rounded-rect description (half-size, radius, stroke width) in its
vertices, so a card's fill, its focus ring, its artwork and its label all
batch together and only a texture or scissor change splits a draw. Rounded
corners and the focus ring are a signed-distance field in the fragment
shader, which is also how the client gets them (`border-radius` plus a
two-layer `box-shadow`).

Layout happens in a **960×540 virtual canvas**. That is not arbitrary: a
1080p TV reports `innerWidth = 960` at DPR 2, which is the viewport the Fliks
TV build actually lays out in. Docked, the Switch reproduces the 10-foot
sizing one-to-one; handheld is the same layout at 1.33×.

Colours are daisyUI 5's `dark` theme converted from oklch to sRGB
(`base-100 #1d232a`, `primary #605dff`), and the metrics in
`source/app/Theme.h` are read off `client/src/styles.css` — including the
`body.tv` overrides, which is why cards are 120 px wide and captions 20 px.

### Navigation

`ui::FocusManager` is a port of the client's `tv-spatial-nav`: an opt-in
container tree handles the common cases (step along a rail, drop into the
next rail at the card you left it on) and falls back to the same three-pass
rect score — same band first, then a 45° cone with a 16× cross-axis penalty,
then anything in the half-plane at 64×.

The sidebar is a drawer rather than a column, because the client explicitly
refuses to pin it on a TV: *"the main layout there is a 10-foot browse
surface and a permanent column steals from it."* Pressing Left at the
leftmost column reaches it, which is the same spatial step the web client
makes.

### Networking

devkitPro's `switch-curl` is built with **every TLS backend compiled out**
(the `mbedtls.o` member in the archive has no symbols), so `https://` is
dead there. `source/net/Http.cpp` is therefore a small HTTP/1.1 client on
mbedtls directly. It also gives the player a seekable reader, which curl
would not.

ffmpeg keeps its own HTTPS — devkitPro patches in a libnx `ssl` backend — but
every stream byte still goes through the app's client anyway, because its HLS
demuxer wedges after a segment boundary when driven through a custom
`io_open`. `source/player/HlsFeed.cpp` walks the playlist instead and hands
ffmpeg one continuous fMP4 stream, which is what those fragments already are
concatenated.

### Bandwidth

Direct play hands over the original file, so the link has to carry its full
bitrate in real time. The device profile advertises 12 Mbit/s, which suits a
server on the same LAN; across the internet it is usually a fiction, and the
failure is silent and confusing — video queue empty, audio device dry, decode
times perfectly healthy. The player measures it and says so outright:

```
player: stream bitrate 6.36 Mbit/s (776 KiB/s sustained)
player: the link is delivering 1.04 Mbit/s and this stream needs 6.36 Mbit/s
        — direct play cannot keep up. Set maxBitrate in config.txt below the
        link speed so the server transcodes.
```

The periodic line carries `net KiB/s` and a cumulative `stalled=` for the same
reason, and a read that blocks longer than 250 ms logs itself. The counters sit
in `net::Stream`, below both paths, so the HLS feed's background reads are
counted like direct play's.

`net: socket rcvbuf 1024 KiB, connect 180ms -> ceiling ~46.5 Mbit/s` is logged
once per run. Throughput over one connection is the receive window divided by
the round trip, so if that ceiling comes out near the observed rate the limit
is this app's socket, and if it comes out far above it the limit is the link.

For reference, the same 6.36 Mbit/s file on the same Switch: **82-311 KiB/s
over WiFi** — unplayable, the audio device dry within seconds — and **774-869
KiB/s over a USB ethernet dongle**, against 776 KiB/s required. Direct play at
1080p needs wired networking or a server on the LAN.

### Quality

The rung is the server's to offer and the viewer's to pick, so neither end of
that is hardcoded here. `playback-info` returns the whole ladder — id, label,
height, bitrate — and both menus are built from it: **Y** during playback
lists it live, and the Settings row steps through the same set. The preference
is stored as a rung **id**, not a height, because `1080p`, `eco-1080p` and the
remuxed original all report 1080 and differ only in bitrate:

```
Original     6.36 Mbit/s        720p       4.13 Mbit/s
1080p eco    3.19 Mbit/s        720p eco   1.63 Mbit/s
480p         2.10 Mbit/s        360p       1.06 Mbit/s
```

Which is the point: on a link that cannot carry 4.13, `eco-720p` at 1.63 is
the same resolution and plays. Picking on height alone always takes the
expensive rung — so `selectHlsVariant` takes a bandwidth ceiling too, or it
would pick the 4.13 variant out of a master playlist that lists both at 720p.

Switching reopens the session at the current position; a rung change means a
different manifest, so there is no way across without rebuilding.

The choice persists in `sdmc:/switch/fliks/settings.json`, written whenever it
changes and read at startup. Unknown keys keep their defaults, so a file from
an older build still loads.

### Tracks and subtitles

**Y** opens one sheet with three tabs — quality, audio, subtitles — and Left
and Right step between them, skipping any that is empty. Three near-identical
menus would have been three places to fix the next layout problem.

Audio tracks come from the container when the server muxed them in: ffmpeg
already enumerated every stream, and switching is a decoder swap the demuxer
performs at a packet boundary, then a seek to the current position to
re-anchor the clock. On a fed stream there is no index to seek, so the session
is rebuilt instead — the same path a rung change takes.

For a source with **more than one audio track** the server does not mux at
all. `pickAudioLayout` switches to ffmpeg's `var_stream_map`: the variant
playlist carries video only and each track becomes an `#EXT-X-MEDIA` rendition
with its own playlist, its own init segment and its own segments. So the
master is parsed for those renditions too, and the chosen one gets a second
`HlsFeed` and a second `AVFormatContext`, read on the demux thread alongside
the video one.

Subtitles are WebVTT, fetched whole from the server, which renders embedded
streams, downloaded files and OCR output to that one format. A film's cues are
a few hundred kilobytes, so fetching once beats streaming them alongside the
video and keeping two positions in step. `at()` resumes its search from the
last hit, so playing forward costs one comparison a frame. Bitmap subtitles
(PGS, VOBSUB) are skipped: they are pictures, and the server burns those into
a transcode.

### Touch

The panel reports 1280×720 and the UI lays out at 960×540, so `ui::Input`
divides by the renderer's scale and every screen keeps working in the
coordinates it already uses. A press that travels more than 12 px stops being
a tap, which is what separates one from the start of a drag.

Outside the player **no screen handles touch at all**. Every focusable thing
already registers its rect with `ui::FocusManager` each frame, so a tap is
answered by `hitTest` against the layout the user was actually looking at,
and the shell then moves focus there and injects the button press the screens
were already written for. One implementation, and Home, the grid, detail,
search, settings, sign-in and the drawer all became touchable without a line
changing in any of them.

Drag-to-scroll is the one thing that needs a screen to opt in, because only
the screen knows which of its scrollers a drag belongs to — one call in
`handleInput`, and Home tries its rails before the page so a sideways drag
lands on the rail under the finger. A drag begins at 14 px, above the 12 px
tap slop, so a scroll has always cancelled the tap before it starts and a
flick never also activates the card it passed over.

The drawer has no button anywhere: the D-pad reaches it by pressing Left at
the leftmost column, and the touch equivalent is a swipe from the left edge.
A hamburger would have put chrome on the 10-foot layout to serve the handheld
one.

Seeks accumulate rather than firing per press. On a fed stream each seek tears
the session down and rebuilds it, so holding Right used to mean a dozen
rebuilds and several seconds of black; the target now commits 450 ms after the
last press, and the scrubber previews where it will land.

### Playback

Decoding runs on the X1's **NVDEC block**, through averne's `nvtegra`
backend — devkitPro's `switch-ffmpeg` is built with `--enable-nvtegra`, so
it is already in the ffmpeg this links against. The device is looked up by
name at runtime rather than by enum, so a toolchain without it still builds
and quietly decodes in software. `hwdec=0` forces that path for comparison.

Hardware frames come back as NV12 and software frames as YUV420p; the video
shader handles both, so the only difference downstream is whether chroma is
one interleaved plane or two. Either way the colour matrix runs on the GPU —
1.5 bytes per pixel uploaded instead of 4.

Video decodes on its own thread. The demuxer reads one packet a pass, and
decoding inline meant an audio packet — twenty-one milliseconds of sound —
arrived only every forty, because ten-odd video packets separate two audio
ones and each cost a four-millisecond decode. `audout` ran dry at twice the
rate it was filled. Reading and audio now never wait on a frame.

Audio is the master clock, with a watchdog: fragments put a fragment's video
samples before its audio, so the first audio packet can arrive well after
video has started, and a clock that never ticks would freeze the picture.
Because the clock rides on it, audio gets the deepest reserve in the
pipeline — five seconds of ring, two seconds held ahead. It costs 192 KB a
second, which is nothing beside a queue of decoded frames, and it is what
carries playback across a stalled HTTP read.

## Status and known gaps

Written against the Fliks API as it is used by `client/src/app/core/services`.
Browsing works; playback is the least exercised path.

Diagnostics go to **`sdmc:/switch/fliks/log.txt`**, line-flushed, with the
deko3d validation layer (`-ldeko3dd`) reporting into it. Attach that file to
any bug report — a black screen or a fatal leaves nothing else behind. Build
with `DEKO3D_LIB=-ldeko3d make` to drop the validation layer once stable.

`sdmc:/switch/fliks/settings.json` holds what the Settings screen and the
quality menu change. `sdmc:/switch/fliks/config.txt` is optional and normally
absent. When a
playback problem needs narrowing on-device, it overrides a few defaults
without a rebuild — see the key list in `source/util/Config.h`:

```
hwdec=0           # software decode instead of NVDEC
decodeThreads=3   # software path only
skipLoopFilter=1  # trade deblocking for ~20% of the decode budget
maxHeight=480     # force a rung by height, overriding the quality menu
maxBitrate=3000000  # what the server is told the link can pull, bits/s
trace=1           # per-frame playback state (costly: writes to SD)
```

Not implemented in this pass: playlists, likes, requests, downloads,
Chromecast, and the admin surfaces. Subtitle *styling* is fixed (white on a
plate, bottom-centred) and bitmap subtitles are skipped entirely. The
`TextRenderer` wraps on spaces only, so CJK prose does not soft-wrap.

## Licences

- `source/gfx/dkfw/` — deko3d sample framework, © 2020 fincs, zlib licence
  (see the `LICENSE` in that directory).
- `romfs/icons.png` is baked from [Lucide](https://lucide.dev) (ISC), the
  same icon set the Fliks client uses.
- The Fliks name and logo belong to the Fliks project.
