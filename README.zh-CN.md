<div align="center">

# pw-mpris-visualcard

**把本机正在播放的音乐渲染成一张卡片，以 PipeWire 视频节点的形式输出给 OBS。**

[English](README.md) · [简体中文](README.zh-CN.md)

<a href="LICENSE"><img src="https://img.shields.io/badge/license-MIT-informational?style=flat-square" alt="MIT License"></a>
<a href="https://aur.archlinux.org/packages/pw-mpris-visualcard-git"><img src="https://img.shields.io/badge/AUR-pw--mpris--visualcard--git-informational?style=flat-square" alt="AUR package"></a>
<img src="https://img.shields.io/badge/platform-Linux-informational?style=flat-square" alt="Linux">

</div>

<br>

<div align="center">
<table>
  <tr>
    <td align="center" valign="bottom"><img src="docs/card-460x690.png" width="280" alt="460x690 with lyrics, time and album"><br><sub><code>460x690</code> · 歌词 4 行 + 时间 + 专辑名</sub></td>
    <td align="center" valign="bottom"><img src="docs/card-viz-460x690.png" width="280" alt="the same card with the spectrum ring"><br><sub>同上 + <code>--viz 1</code> 频谱环</sub></td>
    <td align="center" valign="bottom"><img src="docs/card-360x360.png" width="280" alt="the default 360x360 card"><br><sub><code>360x360</code> · 默认参数</sub></td>
  </tr>
</table>
</div>

三张 PNG 均为实机播放时 `--dump` 的输出，**背景本身是透明的**：叠在场景上不会出现任何底板。中间那张的频谱环抓的是播放器自己的音频，默认关闭。

---

> 单进程 · 无浏览器 · **零子进程** · 没有消费者连接时不渲染 · 私有内存 6–25 MB

## 特性

- **播放信息**来自 D-Bus / MPRIS（musicfox、Spotify、VLC、mpv、Rhythmbox 等），一条常驻连接、零子进程。
- **默认完全透明**：画面上只有封面圆盘与文字，两者自带投影，叠在明亮内容上依然可读。
- **同步歌词**读取 MPRIS 的 `xesam:asText`（LRC）。当前句固定在首行槽位并高亮，整块不跳动。
- **封面自转**，像一张唱片；暂停时冻结。
- **进度环、时间、专辑名**可分别开关。
- **可选频谱环**（`--viz`，默认关闭），分析播放器自己的音频；单色、硬朗，不压到文字，与封面反向转动。
- **尺寸任意**（`WxH`），版式按高度等比缩放；**帧率上限**可调，消费者可以协商到更低，不会超过上限。
- **字体自由**：fontconfig 认得的都能用，也能直接指定系统字体目录之外的字体文件。
- **调版面不用开 OBS**：`--dump` 直接输出 PNG。

## 安装

### 从 AUR 安装

```bash
paru -S pw-mpris-visualcard-git     # 或 yay -S pw-mpris-visualcard-git
```

二进制名为 `pw-mpris-visualcard-native`，装好后已在 `PATH` 中，无需自己维护一份仓库。
该包提供并冲突于 `pw-mpris-visualcard`，支持 `x86_64` 与 `aarch64`，随最新提交更新；它以
`PORTABLE=1` 构建，并把视频节点库作为第二个 source 拉取，因此不需要子模块检出。
`obs-pwvideo` 与 CJK 字体（`noto-fonts-cjk`）是可选依赖。卸载：`pacman -Rns pw-mpris-visualcard-git`。

### 从源码安装

```bash
# 依赖（Arch）
sudo pacman -S --needed base-devel cairo pango gdk-pixbuf2 libpipewire sdbus-cpp curl

git submodule update --init --recursive   # 视频节点库是子模块
make                                      # → ./pw-mpris-visualcard-native
make PORTABLE=1                           # 同上，但不加 -march=native（分发或换机器时用）
```

依赖全部来自发行版仓库，不涉及语言包管理器。未初始化子模块时 `make` 会带明确提示直接报错；
编译参数、各构建目标与 `-march=native` 的取舍见 [docs/development.md](docs/development.md)。

## 使用

### 跑起来

```bash
./pw-mpris-visualcard-native
```

终端会打印节点名（默认 `pw-mpris-visualcard`）。

### 接入 OBS

OBS 自带的 `linux-pipewire` 走 xdg-desktop-portal，只能捕获屏幕与窗口，**选不到本节点**；需要配合插件 [**obs-pwvideo**](https://github.com/tasokait/obs-pwvideo) 使用。

1. 来源 **+** → **PipeWire Video**（由 `obs-pwvideo` 提供）。
2. 选择 **Music Card**。下拉框里显示的是节点描述（`--desc`，默认 `Music Card`），实际连接的是节点名（`--node`，默认 `pw-mpris-visualcard`）；两者是不同的字段。
3. 把源的宽高设置为与 `--size` 一致（默认 `360x360`），保证不被缩放。

透明通道原样透传，直接叠加到场景中即可。obs-pwvideo 只在打开属性对话框时枚举一次节点，不会刷新已打开的窗口；看不到节点时先确认进程在运行，再重开对话框。

### 开机自启

从 AUR 安装的话，unit 已渲染好放在 `/usr/lib/systemd/user/pw-mpris-visualcard.service`，参数为 `--node pw-mpris-visualcard --size 460x690 --fps 30 --lyrics 3`：

```bash
systemctl --user enable --now pw-mpris-visualcard
systemctl --user edit pw-mpris-visualcard    # 改参数：覆盖 ExecStart=
```

这种情况下不要跑 `make install-service`：它写出的 unit 落在 `~/.config/systemd/user/`，会遮蔽包里的那一个。从源码安装的话，仓库里的 `pw-mpris-visualcard.service` 是含 `@REPO@` 与 `@ARGS@` 两个占位符的模板，需要渲染而不是复制：

```bash
make install-service                            # 渲染 unit 到 ~/.config/systemd/user/
make install-service SERVICE_ARGS="--node pw-mpris-visualcard --size 360x360 --fps 30"
make uninstall-service
systemctl --user restart pw-mpris-visualcard    # 改过参数后重启才生效
```

`make install-service` 只写 unit 并 `daemon-reload`，不会替你 enable 或启动。

### 不开 OBS 调版面

```bash
make dump                                                    # 假数据输出一张 PNG
./pw-mpris-visualcard-native --dump /tmp/card.png --time 1   # 使用真实播放数据
./pw-mpris-visualcard-native --demo --dump /tmp/card.png --lyrics 4 --time 1 --album 1
```

## 参数

### 画面

版式整体按 `--size` 的**高度**等比缩放，宽度只决定左右留白。

| 参数 | 默认 | 说明 |
| --- | --- | --- |
| `--size WxH` | `360x360` | 输出尺寸；只写一个数字表示正方形。**方形画布左右必然富余**，收窄到 2:3 可消除留白 |
| `--bg MODE` | `none` | `none` 完全透明；`solid` 不透明深色底；也可给 `#rrggbb` |
| `--progress 0\|1` | `1` | 进度环 |
| `--time 0\|1` | `0` | 显示 `1:23 / 3:12` |
| `--album 0\|1` | `0` | 歌手后追加专辑名 |
| `--lyrics N` | `0` | 歌词行数，`0` 为关闭 |
| `--spin SEC` | `24` | 封面自转一圈的秒数，`0` 为不转 |
| `--idle last\|hide` | `hide` | 停止播放后是否保留最后一首 |

### 字体

| 参数 | 默认 | 说明 |
| --- | --- | --- |
| `--font NAME[,NAME...]` | `sans-serif` | 卡片所有文字的字体 family。逗号分隔即回退链，由 pango **逐字符**回退，所以可以把拉丁字体和中文字体配成一条链 |
| `--font-file PATH` | | 启动时把字体文件（或整个目录）注册进 fontconfig，只在本进程内生效。可重复传入；`~/` 会展开 |

详见 [docs/fonts.md](docs/fonts.md)。

### 频谱环

默认关闭，不显式打开就不会动到版面。用法与调参见 [docs/spectrum-ring.md](docs/spectrum-ring.md)。

| 参数 | 默认 | 说明 |
| --- | --- | --- |
| `--viz 0\|1` | `0` | 封面周围的柱状频谱环。**关闭时版面与功能加入之前逐字节一致** |
| `--viz-bars N` | `72` | 环上的柱数，范围 `8`..`256` |
| `--viz-source NAME` | | 不按 MPRIS 推断，直接抓这个 PipeWire 音频节点 / 应用。自动匹配失败时用它，节点名即可 |
| `--viz-fx 0\|1` | `1` | 后处理：柱子的运动模型、频段轴整形、滑动窗口自动增益。**默认开启——正是这条链让柱子看上去有重量**；`0` 则环显示的就是实测频谱 |
| `--viz-gain DB` | `0` | 以 dB 为单位在最前面做展开 |
| `--viz-gravity N` | `77` | 柱子的「重量」，`0`..`100`；≤`10` 关闭运动模型。对应 cava 的 `noise_reduction`，那是平滑强度，**不是**降噪 |
| `--viz-shape N` | `50` | 沿频段轴向 1-2-1 模糊的混合比例。`100` 时一根孤柱变成三柱的小山 |
| `--viz-norm MS` | `2000` | 滑动窗口自动增益的窗口长度（毫秒），`0` 为关 |

### 输出与诊断

| 参数 | 默认 | 说明 |
| --- | --- | --- |
| `--fps N` | `30` | **帧率上限**。向 PipeWire 声明的是 `[N/4, N]` 区间，消费者可协商到更低但不会超过 N；实际推帧按协商结果执行，下限 5fps |
| `--node NAME` | `pw-mpris-visualcard` | PipeWire 节点名，也是 OBS 下拉项背后的取值。同名节点会各自带 `(id)` 后缀区分 |
| `--desc TEXT` | `Music Card` | 节点描述。**OBS 下拉框里显示的就是它**，不是 `--node` |
| `--verbose`, `-v` | | 打印协商、推帧与每帧耗时 |
| `--dump FILE` | | 采样一次渲染为 PNG 后退出 |
| `--demo` | | 使用假数据，不连接 D-Bus |
| `--help`, `-h` | | 打印参数简表 |

## 性能

| 指标 | 数值 |
| --- | --- |
| CPU | **约 5.5% 单核**（`460x690` @30fps，其中封面自转占 3.5 个百分点） |
| 无消费者连接时 | 0 帧推送，CPU **约 0.25%** |
| 私有（匿名）内存 | **6–25 MB** |
| 子进程 | **0**（常驻 D-Bus 连接） |

`--viz 1` 增量约 0.9% 单核；`--spin 0` 能再砍掉约 60%。实测数据、尺寸选择表与进一步省 CPU 的做法见 [docs/performance.md](docs/performance.md)。

## 文档

| 文档 | 面向 | 内容 |
| --- | --- | --- |
| [docs/spectrum-ring.md](docs/spectrum-ring.md) | 用户 | 频谱环：抓的是什么音频、版面取舍、`--viz-fx` 调参 |
| [docs/fonts.md](docs/fonts.md) | 用户 | 自定义字体、`--font-file`、回退链 |
| [docs/performance.md](docs/performance.md) | 用户、打包者 | CPU 与内存实测、尺寸选择、降低开销 |
| [docs/development.md](docs/development.md) | 贡献者、打包者 | 架构、编译参数与构建目标、目录结构、依赖与许可 |
| [docs/internals.md](docs/internals.md) | 开发者 | 渲染侧约束、实现不变量、实测数据、调试命令——**改渲染路径前先读** |

`docs/` 下各篇目前仅中文；两份 README 同步维护。

## 许可证

本项目采用 **MIT License**，著作权归 **ZokuTe**（2026 年起），全文见 [LICENSE](LICENSE)。提交的贡献按同一许可进入本项目，本项目没有 CLA。

运行期使用的系统库均为动态链接，未捆绑、未修改其代码，各自适用其自身许可（cairo、pango、gdk-pixbuf、PipeWire、sdbus-c++、libcurl、fontconfig），对照表见 [docs/development.md](docs/development.md)。OBS 侧的 [obs-pwvideo](https://github.com/tasokait/obs-pwvideo) 是 GPLv2 的独立程序，与本项目之间只有 PipeWire 节点的运行时数据流，不构成链接或派生关系，故各自许可互不影响。

视频节点实现取自 [`pw-video-simple-interface`](https://github.com/zlinux-live-util/pw-video-simple-interface)（git 子模块）。