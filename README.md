# pw-mpris-visualcard

[English](README.md) · [简体中文](README.zh-CN.md)

Renders whatever music is playing on this machine as a card and publishes it to OBS as a **PipeWire video node**. One process, no browser, no child processes, and no rendering while no consumer is connected.

## How it works

| Stage | Input | Output | Implementation |
| --- | --- | --- | --- |
| Playback state | The player's MPRIS interface (D-Bus) | A `NowPlaying` snapshot: title, artist, album, position, lyrics, cover URL | `mpris`, one persistent D-Bus connection, no forked processes |
| Artwork | Cover URL | cairo surface (LRU cache, up to 3 entries) | background thread + `AssetCache` from the submodule |
| Layout | Snapshot + cover surface | BGRA frame (premultiplied alpha) | `card`, cairo + pango |
| Video output | BGRA frame | `Stream/Output/Video` node | [`pw-video-simple-interface`](https://github.com/zlinux-live-util/pw-video-simple-interface), libpipewire |

Every stage runs in the same process and passes data in memory; private memory stays at 6–25 MB.

## Features

- Playback metadata comes from D-Bus / MPRIS (musicfox, Spotify, VLC, mpv, Rhythmbox, …) over one persistent connection, with zero child processes.
- Transparent background by default: only the cover disc and the text are drawn, each with its own drop shadow so they stay legible over bright content.
- Synced lyrics from the MPRIS `xesam:asText` property (LRC). The current line keeps the first slot and is highlighted; the block never jumps.
- The cover rotates like a record and freezes while paused.
- Progress ring, elapsed time and album name toggle independently.
- Any output size (`WxH`); the layout scales with height.
- Adjustable frame-rate ceiling; consumers may negotiate lower, never higher.
- Layout tuning without OBS: `--dump` writes a PNG directly.

## Preview

Both PNGs are `--dump` output captured during real playback, with a transparent background.

![Example card](docs/card-460x690.png)

`--size 460x690 --lyrics 4 --time 1 --album 1`: circular cover (rotating) + title + artist · album + highlighted current line + dimmed following lines + progress ring + `0:38 / 2:47`.

With the default options (`360x360`, no lyrics / time / album):

![Default card](docs/card-360x360.png)

## Getting started

### 1. Install

Two paths, same binary name (`pw-mpris-visualcard-native`).

**From the AUR** — packaged build, no checkout to maintain:

```bash
paru -S pw-mpris-visualcard-git     # or: yay -S pw-mpris-visualcard-git
```

`pw-mpris-visualcard-git` provides and conflicts with `pw-mpris-visualcard`, and ships `x86_64` and `aarch64`. It installs `/usr/bin/pw-mpris-visualcard-native`, the documentation under `/usr/share/doc/pw-mpris-visualcard/`, and the systemd user unit already rendered. It builds with `PORTABLE=1` (see step 3) and fetches the video-node library as a second source, so no submodule checkout is needed. `obs-pwvideo` and a CJK font (`noto-fonts-cjk`) are optional dependencies. As a `-git` package it follows the latest commit.

**From source** — the steps below.

### 2. Dependencies

Everything comes from the distribution repositories; no language package manager is involved. (Source build only; the AUR package pulls the same libraries as hard dependencies.)

```bash
# Arch
sudo pacman -S --needed base-devel cairo pango gdk-pixbuf2 libpipewire sdbus-cpp curl
```

OBS ships `linux-pipewire`, which goes through xdg-desktop-portal and can only capture screens and windows; it cannot select this node. Use the [**obs-pwvideo**](https://github.com/tasokait/obs-pwvideo) plugin instead.

### 3. Build

```bash
git submodule update --init --recursive   # the video-node library is a submodule
make             # → ./pw-mpris-visualcard-native
make PORTABLE=1  # the same, without -march=native (other machines, redistribution)
```

The PipeWire output (node registration, buffer declarations, frame-rate negotiation, the frame callback contract) lives in [`pw-video-simple-interface`](https://github.com/zlinux-live-util/pw-video-simple-interface), pulled in as a git submodule under `lib/` and compiled into this project's build tree with the same flags. `make` fails with an explicit message if the submodule is not checked out.

The default flags are `-O3 -march=native -funroll-loops`: the cover-rotation hot loop benefits measurably, cutting 22% off a whole frame. The trade-off is a binary tied to the local instruction set, which is what `PORTABLE=1` exists for.

### 4. Run

```bash
./pw-mpris-visualcard-native
```

With the AUR package the binary is on `PATH`. The node name (default `pw-mpris-visualcard`) is printed to the terminal.

### 5. Add it to OBS

1. Sources **+** → **PipeWire Video** (provided by `obs-pwvideo`).
2. Select **Music Card**. The dropdown displays the node description (`--desc`, default `Music Card`) and connects to the node name (`--node`, default `pw-mpris-visualcard`); the two are separate fields.
3. Set the source width and height to match `--size` (default `360x360`) so the card is shown unscaled.

The alpha channel passes through unchanged, so the card can be overlaid on the scene as is. obs-pwvideo enumerates nodes when the properties dialog is opened and does not refresh an open dialog; if the node is absent, confirm the process is running and reopen it. Node visibility and frame delivery are covered in [docs/internals.md](docs/internals.md).

### 6. Autostart (optional)

Installed from the AUR, the unit is already rendered at `/usr/lib/systemd/user/pw-mpris-visualcard.service` with the arguments `--node pw-mpris-visualcard --size 460x690 --fps 30 --lyrics 3`:

```bash
systemctl --user enable --now pw-mpris-visualcard
systemctl --user edit pw-mpris-visualcard    # override ExecStart= to change the arguments
```

`make install-service` must not be used on that install: it writes to `~/.config/systemd/user/`, which shadows the packaged unit. Removal is `pacman -Rns pw-mpris-visualcard-git`.

From source, `pw-mpris-visualcard.service` is a template holding `@REPO@` and `@ARGS@`, so it is rendered rather than copied:

```bash
make install-service                            # render into ~/.config/systemd/user/
make install-service SERVICE_ARGS="--node pw-mpris-visualcard --size 360x360 --fps 30"
make uninstall-service
systemctl --user restart pw-mpris-visualcard    # after changing the arguments
```

`make install-service` writes the unit and runs `daemon-reload`; it never enables or starts anything.

## Command-line options

| Option | Default | Description |
| --- | --- | --- |
| `--size WxH` | `360x360` | Output size; a single number means a square. **The entire layout scales with height, width only sets the side margins** — the circular cover is constrained by the vertical budget, so a square canvas always leaves a wide margin |
| `--fps N` | `30` | **Frame-rate ceiling.** The range advertised to PipeWire is `[N/4, N]`; consumers may negotiate lower but never higher. Frames are pushed at the negotiated rate, floored at 5fps |
| `--bg MODE` | `none` | `none` is fully transparent, `solid` is an opaque dark background, `#rrggbb` sets a specific colour |
| `--font NAME[,NAME...]` | `sans-serif` | Font family for all card text. A comma-separated list is a fallback chain, resolved per character by pango, so a Latin family can be paired with a CJK one |
| `--font-file PATH` | | Register a font file — or a whole directory of them — with fontconfig at startup, so a downloaded `.ttf`/`.otf`/`.ttc` can be used without installing it system-wide. Repeatable; `~/` is expanded |
| `--progress 0\|1` | `1` | Progress ring |
| `--time 0\|1` | `0` | Show `1:23 / 3:12` |
| `--album 0\|1` | `0` | Append the album name after the artist |
| `--lyrics N` | `0` | Number of lyric lines; `0` disables them |
| `--spin SEC` | `24` | Seconds per full cover rotation; `0` disables rotation |
| `--idle last\|hide` | `hide` | Keep the last track on screen after playback stops |
| `--node NAME` | `pw-mpris-visualcard` | PipeWire node name; this is the value behind the OBS dropdown entry. Duplicate names get an `(id)` suffix to tell them apart |
| `--desc TEXT` | `Music Card` | Node description. **This is what the OBS dropdown displays**, not `--node` |
| `--verbose`, `-v` | | Log negotiation, frame pushes and per-frame timings |
| `--dump FILE` | | Render one sample to a PNG and exit |
| `--demo` | | Use fake data; do not connect to D-Bus |
| `--help`, `-h` | | Print a short option summary |

### Custom fonts

Any font fontconfig knows about works; there is no font list to configure.

```bash
# by family name
--font "Inter"

# a fallback chain: Inter for Latin, Noto for everything else it does not cover
--font "Inter,Noto Sans CJK SC"

# a font that is not installed system-wide -- no root, no copying into /usr/share/fonts
--font-file ~/Downloads/MyFont.ttf
```

`--font-file` registers the file with fontconfig for this process only, and nothing else is needed: the family name is read out of the font itself, so `--font-file` on its own is enough. Combine it with `--font` when the file declares a name you would rather not type, when a `.ttc` should use a face other than the first, or when several registered files should be tried in order. A directory registers everything inside it.

```bash
# a whole font pack, first family wins
--font-file ~/Downloads/fonts --font-file ~/Downloads/Display.ttf
```

Nothing here is fatal by design. A missing file or an unknown family prints a warning and the card renders in the default font, because a card in the wrong typeface is better than a card that refuses to start. The font actually in use is printed at startup:

```
  Font: Inter,Noto Sans CJK SC
```

Two details worth knowing. Characters the chosen family does not cover still resolve — pango falls back per character, so CJK lyrics show up even under a Latin-only family. And the title is drawn at semibold weight; a family with no semibold face gets the regular one, not a synthesised fake bold.

### Tuning the layout without opening OBS

```bash
make dump                                                    # one PNG from fake data
./pw-mpris-visualcard-native --dump /tmp/card.png --time 1   # from real playback data
./pw-mpris-visualcard-native --demo --dump /tmp/card.png --lyrics 4 --time 1 --album 1
```

## Performance

| Metric | Value |
| --- | --- |
| RSS | 40–70 MB (scales with output size) |
| Private (anonymous) memory | **6–25 MB** (grows with the cover cache and the cairo/pango glyph caches) |
| CPU | **≈5.5% of one core** (`460x690` at 30fps, of which cover rotation accounts for 3.5 points) |
| With no consumer attached | zero frames pushed, **≈0.25%** CPU |
| Child processes | **0** (persistent D-Bus connection) |

What lies beyond the private memory is shared libraries (cairo, pango, dbus, curl, gdk-pixbuf, PipeWire) and fontconfig caches: system-wide shared pages that are not duplicated per process.

### Choosing a size

The layout stacks a circular cover above the text, and the circle's diameter is constrained by the vertical budget, so a square canvas always leaves margins on both sides. Narrowing the width until it just fits the content removes them:

| `--size` | Content width | Side margins | CPU (one core) |
| --- | --- | --- | --- |
| `540x540` | 81% | 19% | ~4.1% |
| `360x540` | 96% | 4% | ~3.6% |
| `400x600` | 96% | 4% | ~4.2% |
| **`460x690`** | **96%** | **4%** | **~5.5%** |
| `480x720` | 96% | 4% | ~6.3% |

When scaling up, multiply **both dimensions** and keep `W:H = 2:3`; the side margins stay at 4%. Cost is dominated by the cover and the text, both of which scale with **height**, so narrowing the width saves almost no CPU; it only removes empty space.

### Reducing CPU further

- `--spin 0` — cuts roughly 60% outright (rotation resamples every frame and is the most expensive stage in the pipeline).
- `--fps 24` — saves about one sixth.
- Drop the height by one step.

## Repository layout

| Path | Contents |
| --- | --- |
| `README.md` | This file (English) |
| `README.zh-CN.md` | Chinese version |
| `LICENSE` | Full text of the MIT license |
| `docs/internals.md` | Rendering constraints, measurements and debug commands (read before changing code; Chinese only for now) |
| `docs/card-*.png` | Example output from `--dump` |
| `Makefile` | Build script with the `dump` / `run` / `install-service` / `uninstall-service` targets |
| `pw-mpris-visualcard.service` | systemd user service template, rendered by `make install-service` |
| `src/types.hpp` | `Track` / `NowPlaying` / `Config` data structures |
| `src/mpris.{hpp,cpp}` | sdbus-c++ persistent connection + sampling thread + LRC parsing |
| `src/card.{hpp,cpp}` | Layout rendering with cairo + pango |
| `lib/pw-video-simple-interface/` | Git submodule: the video node (registration, buffers, frame-rate negotiation) plus the cairo helpers (frame, text, asset cache, HTTP) |
| `src/main.cpp` | Module wiring and command-line parsing |

## Contributing

- Behaviour changes: include the command and its output.
- Performance claims: include the measurement. Unverified numbers do not go into the documentation.
- Read [docs/internals.md](docs/internals.md) before changing the rendering path. The PipeWire side lives in [`pw-video-simple-interface`](https://github.com/zlinux-live-util/pw-video-simple-interface).
- State how the work was produced (recommended, not required), so it can be traced.
- Review, verification and long-term maintenance of a patch rest with its submitter.

There is no CLA.

## License

Released under the **MIT License**, copyright **ZokuTe** (from 2026); see [LICENSE](LICENSE) for the full text. Contributions enter the project under the same license.

The following system libraries are used at runtime through dynamic linking. None of their code is bundled or modified, and each remains under its own license:

| Library | License declared by the installed package |
| --- | --- |
| cairo | LGPL-2.1-only OR MPL-1.1 |
| pango | LGPL-2.0-or-later |
| gdk-pixbuf | LGPL-2.0-or-later |
| PipeWire | MIT, LGPL-2.1-or-later |
| sdbus-c++ | LGPL-2.1-only, with the sdbus-c++ LGPL exception |
| libcurl | curl license (MIT-style) |

[obs-pwvideo](https://github.com/tasokait/obs-pwvideo) on the OBS side is a separate GPLv2 program. Its only interaction with this project is the runtime data flow through a PipeWire node — no linking, no derivative work — so the two licenses do not affect each other.
