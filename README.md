<div align="center">

# pw-mpris-visualcard

**Renders whatever music is playing on this machine as a card, published to OBS as a PipeWire video node.**

[English](README.md) · [简体中文](README.zh-CN.md)

<a href="LICENSE"><img src="https://img.shields.io/badge/license-MIT-informational?style=flat-square" alt="MIT License"></a>
<a href="https://aur.archlinux.org/packages/pw-mpris-visualcard-git"><img src="https://img.shields.io/badge/AUR-pw--mpris--visualcard--git-informational?style=flat-square" alt="AUR package"></a>
<img src="https://img.shields.io/badge/platform-Linux-informational?style=flat-square" alt="Linux">

</div>

<br>

<div align="center">
<table>
  <tr>
    <td align="center" valign="bottom"><img src="docs/card-460x690.png" width="280" alt="460x690 with lyrics, time and album"><br><sub><code>460x690</code> · 4 lyric lines + time + album</sub></td>
    <td align="center" valign="bottom"><img src="docs/card-viz-460x690.png" width="280" alt="the same card with the spectrum ring"><br><sub>same, plus the <code>--viz 1</code> spectrum ring</sub></td>
    <td align="center" valign="bottom"><img src="docs/card-360x360.png" width="280" alt="the default 360x360 card"><br><sub><code>360x360</code> · default options</sub></td>
  </tr>
</table>
</div>

All three PNGs are `--dump` output captured during real playback, and the background is **genuinely transparent**: there is no plate behind the card when you overlay it on a scene. The ring in the middle one is analysed from the player's own audio, and is off by default.

---

> One process · No browser · **Zero child processes** · Nothing rendered while no consumer is attached · 6–25 MB private memory

## Features

- **Playback metadata** comes from D-Bus / MPRIS (musicfox, Spotify, VLC, mpv, Rhythmbox, …) over one persistent connection, with zero child processes.
- **Fully transparent by default**: only the cover disc and the text are drawn, each with its own drop shadow, so they stay legible over bright content.
- **Synced lyrics** are read from the MPRIS `xesam:asText` property (LRC). The current line keeps the first slot and is highlighted; the block never jumps.
- **The cover rotates** like a record, and freezes while paused.
- **Progress ring, elapsed time and album name** toggle independently.
- **Optional spectrum ring** around the cover (`--viz`, off by default), analysed from the player's own audio: monochrome, hard-edged, never over the text, turning the opposite way from the cover.
- **Any output size** (`WxH`) with the layout scaling by height, and an **adjustable frame-rate ceiling** — consumers may negotiate lower, never higher.
- **Any font** fontconfig knows about, plus font files you keep outside the system font directories.
- **Tune the layout without OBS**: `--dump` writes a PNG directly.

## Install

### From the AUR

```bash
paru -S pw-mpris-visualcard-git     # or: yay -S pw-mpris-visualcard-git
```

The binary is `pw-mpris-visualcard-native` and lands on `PATH`, so there is no checkout to maintain. The package provides and conflicts with `pw-mpris-visualcard`, ships `x86_64` and `aarch64`, and follows the latest commit; it builds with `PORTABLE=1` and fetches the video-node library as a second source, so no submodule checkout is needed. `obs-pwvideo` and a CJK font (`noto-fonts-cjk`) are optional dependencies. Removal: `pacman -Rns pw-mpris-visualcard-git`.

### From source

```bash
# Dependencies (Arch)
sudo pacman -S --needed base-devel cairo pango gdk-pixbuf2 libpipewire sdbus-cpp curl

git submodule update --init --recursive   # the video-node library is a submodule
make                                      # → ./pw-mpris-visualcard-native
make PORTABLE=1                           # the same, without -march=native (distribution)
```

Everything comes from the distribution repositories; no language package manager is involved. `make` fails with an explicit message if the submodule is not checked out; the build flags, the other `make` targets and the trade-off behind `-march=native` are in [docs/development.md](docs/development.md).

## Usage

### Run it

```bash
./pw-mpris-visualcard-native
```

The node name (default `pw-mpris-visualcard`) is printed to the terminal.

### Add it to OBS

OBS ships `linux-pipewire`, which goes through xdg-desktop-portal, can only capture screens and windows, and therefore **cannot select this node**. Use the [**obs-pwvideo**](https://github.com/tasokait/obs-pwvideo) plugin instead.

1. Sources **+** → **PipeWire Video** (provided by `obs-pwvideo`).
2. Select **Music Card**. The dropdown displays the node description (`--desc`, default `Music Card`) and connects to the node name (`--node`, default `pw-mpris-visualcard`); the two are separate fields.
3. Set the source width and height to match `--size` (default `360x360`) so the card is shown unscaled.

The alpha channel passes through unchanged, so the card can be overlaid on the scene as is. obs-pwvideo enumerates nodes when the properties dialog is opened and does not refresh an open dialog; if the node is absent, confirm the process is running and reopen it.

### Autostart

Installed from the AUR, the unit is already rendered at `/usr/lib/systemd/user/pw-mpris-visualcard.service` with the arguments `--node pw-mpris-visualcard --size 460x690 --fps 30 --lyrics 3`:

```bash
systemctl --user enable --now pw-mpris-visualcard
systemctl --user edit pw-mpris-visualcard    # override ExecStart= to change the arguments
```

`make install-service` must not be used on that install: it writes to `~/.config/systemd/user/`, which shadows the packaged unit. From source, `pw-mpris-visualcard.service` is a template holding `@REPO@` and `@ARGS@`, so it is rendered rather than copied:

```bash
make install-service                            # render into ~/.config/systemd/user/
make install-service SERVICE_ARGS="--node pw-mpris-visualcard --size 360x360 --fps 30"
make uninstall-service
systemctl --user restart pw-mpris-visualcard    # after changing the arguments
```

`make install-service` writes the unit and runs `daemon-reload`; it never enables or starts anything.

### Tuning the layout without opening OBS

```bash
make dump                                                    # one PNG from fake data
./pw-mpris-visualcard-native --dump /tmp/card.png --time 1   # from real playback data
./pw-mpris-visualcard-native --demo --dump /tmp/card.png --lyrics 4 --time 1 --album 1
```

## Options

### Card

The layout scales with the **height** of `--size`; the width only sets the side margins.

| Option | Default | Description |
| --- | --- | --- |
| `--size WxH` | `360x360` | Output size; a single number means a square. **A square canvas always leaves wide margins** — narrow it towards 2:3 to remove them |
| `--bg MODE` | `none` | `none` is fully transparent, `solid` is an opaque dark background, `#rrggbb` sets a specific colour |
| `--progress 0\|1` | `1` | Progress ring |
| `--time 0\|1` | `0` | Show `1:23 / 3:12` |
| `--album 0\|1` | `0` | Append the album name after the artist |
| `--lyrics N` | `0` | Number of lyric lines; `0` disables them |
| `--spin SEC` | `24` | Seconds per full cover rotation; `0` disables rotation |
| `--idle last\|hide` | `hide` | Keep the last track on screen after playback stops |

### Fonts

| Option | Default | Description |
| --- | --- | --- |
| `--font NAME[,NAME...]` | `sans-serif` | Font family for all card text. A comma-separated list is a fallback chain, resolved per character by pango, so a Latin family can be paired with a CJK one |
| `--font-file PATH` | | Register a font file — or a whole directory of them — with fontconfig for this process only. Repeatable; `~/` is expanded |

Details: [docs/fonts.md](docs/fonts.md).

### Spectrum ring

Off by default, and nothing about the layout changes unless you ask for it. Usage and tuning: [docs/spectrum-ring.md](docs/spectrum-ring.md).

| Option | Default | Description |
| --- | --- | --- |
| `--viz 0\|1` | `0` | Radial spectrum ring around the cover. **With it off the layout is byte-for-byte what it always was** |
| `--viz-bars N` | `72` | Bars in the ring, `8`..`256` |
| `--viz-source NAME` | | Capture this PipeWire audio node / app instead of the one MPRIS names. Use it when the automatic match fails; the node name works |
| `--viz-fx 0\|1` | `1` | Post-processing: bar motion, band-axis shaping, sliding-window auto-gain. **On by default — this chain is what makes the bars read as having weight.** `0` shows the measured spectrum unchanged |
| `--viz-gain DB` | `0` | Expansion in dB, applied before everything else |
| `--viz-gravity N` | `77` | How heavy the bars are, `0`..`100`; at `10` or below the motion model is off (cava's own threshold) |
| `--viz-shape N` | `50` | `0`..`100`, blend towards a 1-2-1 blur along the band axis. At `100` one lone tall band becomes a three-band mound |
| `--viz-norm MS` | `2000` | Sliding-window auto-gain length; `0` turns it off |

### Output and diagnostics

| Option | Default | Description |
| --- | --- | --- |
| `--fps N` | `30` | **Frame-rate ceiling.** The range advertised to PipeWire is `[N/4, N]`; consumers may negotiate lower but never higher. Frames are pushed at the negotiated rate, floored at 5fps |
| `--node NAME` | `pw-mpris-visualcard` | PipeWire node name; this is the value behind the OBS dropdown entry. Duplicate names get an `(id)` suffix to tell them apart |
| `--desc TEXT` | `Music Card` | Node description. **This is what the OBS dropdown displays**, not `--node` |
| `--verbose`, `-v` | | Log negotiation, frame pushes and per-frame timings |
| `--dump FILE` | | Render one sample to a PNG and exit |
| `--demo` | | Use fake data; do not connect to D-Bus |
| `--help`, `-h` | | Print a short option summary |

## Performance

| Metric | Value |
| --- | --- |
| CPU | **≈5.5% of one core** (`460x690` at 30fps, of which cover rotation accounts for 3.5 points) |
| With no consumer attached | zero frames pushed, **≈0.25% CPU** |
| Private (anonymous) memory | **6–25 MB** |
| Child processes | **0** |

`--viz 1` adds ≈0.9% of one core; `--spin 0` cuts about 60% again. Measurements, the size-selection table and further ways to cut CPU: [docs/performance.md](docs/performance.md).

## Documentation

| Document | For | Contents |
| --- | --- | --- |
| [docs/spectrum-ring.md](docs/spectrum-ring.md) | users | The spectrum ring: which audio it captures, layout trade-offs, tuning `--viz-fx` |
| [docs/fonts.md](docs/fonts.md) | users | Custom fonts, `--font-file`, fallback chains |
| [docs/performance.md](docs/performance.md) | users, packagers | Measured CPU and memory, size selection, cutting CPU |
| [docs/development.md](docs/development.md) | contributors, packagers | Architecture, build flags and targets, repository layout, dependencies |
| [docs/internals.md](docs/internals.md) | developers | Rendering constraints, implementation invariants, measurements, debug commands — **read before changing the rendering path** |

The `docs/` articles are Chinese-only for now; both READMEs are maintained.

## License

Released under the **MIT License**, copyright **ZokuTe** (from 2026); see [LICENSE](LICENSE) for the full text. Contributions enter the project under the same license, and there is no CLA.

The system libraries used at runtime are dynamically linked, none of their code is bundled or modified, and each remains under its own license (cairo, pango, gdk-pixbuf, PipeWire, sdbus-c++, libcurl, fontconfig); the table is in [docs/development.md](docs/development.md). [obs-pwvideo](https://github.com/tasokait/obs-pwvideo) on the OBS side is a separate GPLv2 program whose only interaction with this project is the runtime data flow through a PipeWire node — no linking, no derivative work — so the two licenses do not affect each other.

The video node itself comes from [`pw-video-simple-interface`](https://github.com/zlinux-live-util/pw-video-simple-interface), a git submodule.
