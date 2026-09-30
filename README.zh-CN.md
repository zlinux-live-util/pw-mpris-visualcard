# pw-mpris-visualcard

[English](README.md) · [简体中文](README.zh-CN.md)

把本机正在播放的音乐渲染成一张卡片，以 **PipeWire 视频节点**的形式输出给 OBS。单进程，无浏览器，无子进程；没有消费者连接时不渲染。

## 工作方式

| 阶段 | 输入 | 输出 | 实现 |
| --- | --- | --- | --- |
| 状态采样 | 播放器的 MPRIS 接口（D-Bus） | `NowPlaying` 快照：标题、歌手、专辑、进度、歌词、封面 URL | `mpris`，常驻 D-Bus 连接，不 fork 子进程 |
| 素材获取 | 封面 URL | cairo 表面（LRU 缓存，最多 3 张） | 后台线程 + 子模块的 `AssetCache` |
| 版面渲染 | 快照 + 封面表面 | BGRA 帧（预乘 alpha） | `card`，cairo + pango |
| 频谱（可选） | 播放器自己的 PipeWire monitor 流 | 72 段频段电平 | `audio` + `analyser`，libpipewire + 2048 点 FFT |
| 视频输出 | BGRA 帧 | `Stream/Output/Video` 节点 | [`pw-video-simple-interface`](https://github.com/zlinux-live-util/pw-video-simple-interface)，libpipewire |

各阶段都在同一进程内通过内存传递数据，进程私有内存 6–25 MB。

## 特性

- 播放信息来自 D-Bus / MPRIS（musicfox、Spotify、VLC、mpv、Rhythmbox 等），一条常驻连接、零子进程。
- 默认完全透明背景：画面上只有封面圆盘与文字，两者自带投影，叠在明亮内容上依然可读。
- 同步歌词读取 MPRIS 的 `xesam:asText`（LRC）。当前句固定在首行槽位并高亮，整块不跳动。
- 封面自转，暂停时冻结。
- 进度环、时间、专辑名可分别开关。
- 可选的径向频谱环（`--viz`，**默认关闭**）：经 PipeWire 定位播放器自己的音频节点，分析该原始流。单色、硬朗、不压到文字，且与封面反向转动。
- 尺寸任意（`WxH`），版式按高度等比缩放。
- 帧率上限可调；消费者可以协商到更低，不会超过上限。
- 调版面无需打开 OBS：`--dump` 直接输出 PNG。

## 效果

下列 PNG 由实机播放时 `--dump` 输出，背景本身是透明的。

![卡片效果](docs/card-460x690.png)

`--size 460x690 --lyrics 4 --time 1 --album 1`：圆封面（自转）+ 标题 + 歌手 · 专辑 + 当前句高亮 + 后续歌词压暗 + 进度环 + `0:38 / 2:47`。

默认参数（`360x360`，不开歌词 / 时间 / 专辑）即最小形态：

![默认形态](docs/card-360x360.png)

## 快速开始

### 1. 安装

两条路径，二进制同名（`pw-mpris-visualcard-native`）。

**从 AUR 安装**——由包管理器构建，不必自己维护一份仓库：

```bash
paru -S pw-mpris-visualcard-git     # 或 yay -S pw-mpris-visualcard-git
```

`pw-mpris-visualcard-git` 提供并冲突于 `pw-mpris-visualcard`，支持 `x86_64` 与 `aarch64`。装出来的是 `/usr/bin/pw-mpris-visualcard-native`、`/usr/share/doc/pw-mpris-visualcard/` 下的文档，以及已渲染好的 systemd 用户 unit。构建走 `PORTABLE=1`（见第 3 步），并把视频节点库作为第二个 source 一起拉取，因此不需要子模块检出。`obs-pwvideo` 与 CJK 字体（`noto-fonts-cjk`）为可选依赖。作为 `-git` 包，它跟随最新提交。

**从源码安装**——即下面几步。

### 2. 依赖

依赖全部来自发行版仓库，不涉及语言包管理器。（仅源码构建需要；AUR 包已把这些库作为硬依赖拉入。）

```bash
# Arch
sudo pacman -S --needed base-devel cairo pango gdk-pixbuf2 libpipewire sdbus-cpp curl
```

OBS 自带的 `linux-pipewire` 走 xdg-desktop-portal，只能捕获屏幕与窗口，选不到本节点；需要配合插件 [**obs-pwvideo**](https://github.com/tasokait/obs-pwvideo) 使用。

### 3. 构建

```bash
git submodule update --init --recursive   # 视频节点库是子模块
make             # → ./pw-mpris-visualcard-native
make PORTABLE=1  # 同上，但不加 -march=native（换机器运行或分发时使用）
```

视频节点输出（节点注册、缓冲声明、帧率协商、帧回调契约）在 [`pw-video-simple-interface`](https://github.com/zlinux-live-util/pw-video-simple-interface) 里，作为 git 子模块挂在 `lib/` 下，用同一套编译参数编进本项目的构建树。未初始化子模块时 `make` 会带明确提示直接报错。

默认使用 `-O3 -march=native -funroll-loops`：封面旋转的热循环收益明显，整帧实测降低 22%。代价是二进制绑定本机指令集，因此保留 `PORTABLE=1`。

### 4. 运行

```bash
./pw-mpris-visualcard-native
```

用 AUR 包安装的话二进制已在 `PATH` 中。终端会打印节点名（默认 `pw-mpris-visualcard`）。

### 5. 在 OBS 中接入

1. 来源 **+** → **PipeWire Video**（由 `obs-pwvideo` 提供）。
2. 选择 **Music Card**。下拉框里显示的是节点描述（`--desc`，默认 `Music Card`），实际连接的是节点名（`--node`，默认 `pw-mpris-visualcard`）；两者是不同的字段。
3. 把源的宽高设置为与 `--size` 一致（默认 `360x360`），保证不被缩放。

透明通道原样透传，直接叠加到场景中即可。obs-pwvideo 只在打开属性对话框时枚举一次节点，不会刷新已打开的窗口；看不到节点时先确认进程在运行，再重开对话框。节点可见性与帧投递的细节见 [docs/internals.md](docs/internals.md)。

### 6. 开机自启（可选）

从 AUR 安装的话，unit 已渲染好放在 `/usr/lib/systemd/user/pw-mpris-visualcard.service`，参数为 `--node pw-mpris-visualcard --size 460x690 --fps 30 --lyrics 3`：

```bash
systemctl --user enable --now pw-mpris-visualcard
systemctl --user edit pw-mpris-visualcard    # 改参数：覆盖 ExecStart=
```

这种情况下不要跑 `make install-service`：它写出的 unit 落在 `~/.config/systemd/user/`，会遮蔽包里的那一个。卸载用 `pacman -Rns pw-mpris-visualcard-git`。

从源码安装的话，仓库里的 `pw-mpris-visualcard.service` 是含 `@REPO@` 与 `@ARGS@` 两个占位符的模板，需要渲染而不是复制：

```bash
make install-service                            # 渲染 unit 到 ~/.config/systemd/user/
make install-service SERVICE_ARGS="--node pw-mpris-visualcard --size 360x360 --fps 30"
make uninstall-service
systemctl --user restart pw-mpris-visualcard    # 改过参数后重启才生效
```

`make install-service` 只写 unit 并 `daemon-reload`，不会替你 enable 或启动。

## 命令行参数

| 参数 | 默认 | 说明 |
| --- | --- | --- |
| `--size WxH` | `360x360` | 输出尺寸；只写一个数字表示正方形。**版式全部按高度等比缩放，宽度只决定左右留白**——圆形封面受纵向预算约束，方画布横向必然富余较多 |
| `--fps N` | `30` | **帧率上限**。向 PipeWire 声明的是 `[N/4, N]` 区间，消费者可协商到更低但不会超过 N；实际推帧按协商结果执行，下限 5fps |
| `--bg MODE` | `none` | `none` 完全透明；`solid` 不透明深色底；也可给 `#rrggbb` |
| `--font NAME[,NAME...]` | `sans-serif` | 卡片所有文字的字体 family。逗号分隔即回退链，由 pango **逐字符**回退，所以可以把拉丁字体和中文字体配成一条链 |
| `--font-file PATH` | | 启动时把字体文件（或整个目录）注册进 fontconfig，从而免安装、免 root 就能用下载来的 `.ttf`/`.otf`/`.ttc`。可重复传入；`~/` 会展开 |
| `--progress 0\|1` | `1` | 进度环 |
| `--time 0\|1` | `0` | 显示 `1:23 / 3:12` |
| `--album 0\|1` | `0` | 歌手后追加专辑名 |
| `--lyrics N` | `0` | 歌词行数，`0` 为关闭 |
| `--spin SEC` | `24` | 封面自转一圈的秒数，`0` 为不转 |
| `--viz 0\|1` | `0` | 封面周围的柱状频谱环。**默认关闭；关闭时版面与功能加入之前逐字节一致** |
| `--viz-bars N` | `72` | 环上的柱数，范围 `8`..`256` |
| `--viz-source NAME` | | 不按 MPRIS 推断，直接抓这个 PipeWire 音频节点 / 应用。自动匹配失败时用它，节点名即可 |
| `--viz-fx 0\|1` | `1` | 频谱环后处理：cava 运动模型、频段轴整形、滑动窗口自动增益。**默认开启——正是这条链让柱子看上去有重量**；`--viz-fx 0` 全部关掉，环显示的就是实测频谱 |
| `--viz-gain DB` | `0` | 以 dB 为单位在最前面做展开 |
| `--viz-gravity N` | `77` | 柱子的「重量」，`0`..`100`；≤`10` 关闭运动模型（cava 自己的阈值）。对应 cava 的 `noise_reduction`，那是平滑强度，**不是**降噪 |
| `--viz-shape N` | `50` | 沿频段轴向 1-2-1 模糊的混合比例。`100` 时一根孤柱变成三柱的小山 |
| `--viz-norm MS` | `2000` | 滑动窗口自动增益的窗口长度（毫秒），`0` 为关 |
| `--idle last\|hide` | `hide` | 停止播放后是否保留最后一首 |
| `--node NAME` | `pw-mpris-visualcard` | PipeWire 节点名，也是 OBS 下拉项背后的取值。同名节点会各自带 `(id)` 后缀区分 |
| `--desc TEXT` | `Music Card` | 节点描述。**OBS 下拉框里显示的就是它**，不是 `--node` |
| `--verbose`, `-v` | | 打印协商、推帧与每帧耗时 |
| `--dump FILE` | | 采样一次渲染为 PNG 后退出 |
| `--demo` | | 使用假数据，不连接 D-Bus |
| `--help`, `-h` | | 打印参数简表 |

### 频谱环

`--viz 1` 在封面周围画一圈径向柱子，颜色与进度环同一套单色。默认关闭，不显式打开就不会动到版面。

![带频谱环的卡片](docs/card-viz-460x690.png)

`--size 460x690 --viz 1 --lyrics 4 --time 1 --album 1`，实机播放时截取，用的是默认的 `--viz-fx` 链路。环上的信号就是播放器自己的音频，从 3 点钟方向的最低频段顺时针走：右侧到底部长出的柱是低音，顶部的短柱是几乎空的空气频段。

```bash
./pw-mpris-visualcard-native --viz 1                          # 跟随 MPRIS 里的播放器
./pw-mpris-visualcard-native --viz 1 --viz-bars 96             # 更密的环
./pw-mpris-visualcard-native --viz 1 --viz-source musicfox     # 钉死目标
```

输入是怎么来的：

1. MPRIS 告诉当前是哪个播放器在播。
2. 从 PipeWire 注册表枚举音频节点，与该名字匹配。
3. 把输入流指向该节点的 `object.serial`，PipeWire 就会交出它的 **monitor** 端口——也就是这个播放器在任何设备混音、音量、效果之前送出的信号。
4. 2048 点 Hann 窗 FFT 折叠成对数间隔的频段，柱子向外生长。

音频进来后**什么都不做**：不加增益、不加噪声门、不做 AGC、不做平滑（这些都是 `--viz-fx` 这条**显示端**链上的事，默认开启，`--viz-fx 0` 可整条关掉）。抓到的本来就是播放器自己的信号，再加一道电平处理，画出来的就不是音乐本身的频谱了。柱高来自一个**固定**的 dB 窗口，所以与实际播放量成正比，曲目之间也可比。

版面方面：环需要整圈留白，而环的下缘正是文字开始的地方，所以开启时是把封面上方的间距**撑开**、同时从纵向预算里扣掉顶部那一份，而不是让柱子长到第一行文字上或跑出画布。环与封面反向、且慢 4 倍，读起来是两个独立的运动。

匹配不到节点时，环会留空、只显示底圈，并打印一行指明问题的提示——不会静默空白。`--demo --viz 1` 会渲染一条假频谱，因此没有播放器也能调版面。

### 频谱环后处理

`--viz-fx 1`（即默认）接上一条处理链，其中**柱子的运动模型直接取自 [cava](https://github.com/karlstav/cava) 的 `cavacore.c`**，常数照抄。默认开启：正是这条链让柱子有重量；`--viz-fx 0` 把它整条拿掉，柱高就回到分析器实测到的值。

```bash
./pw-mpris-visualcard-native --viz 1                          # 环 + 默认后处理
./pw-mpris-visualcard-native --viz 1 --viz-fx 0                # 柱高就是实测值
./pw-mpris-visualcard-native --viz 1 --viz-gravity 95          # 更重，落得更慢
./pw-mpris-visualcard-native --viz 1 --viz-shape 100           # 最大的小山整形
```

| 环节 | 开关 | 作用 |
| --- | --- | --- |
| 展开 | `--viz-gain DB` | 给每个频段加 dB。允许超过满格、由绘制时按柱钳位，这样各频段保住相对差距，不会在顶部被压成一条平线 |
| 重力 | `--viz-gravity N` | cava 的运动模型。**上升时**直接用实测值，零平滑；**下落时丢掉实测值**，改由本轮上升的峰值沿一条平方曲线落下来——下落中的柱子是一条合成曲线，无论音频多抖都影响不到它。再加一道积分给动量。这就是它看上去有重量的原因 |
| 整形 | `--viz-shape N` | 沿频段轴的 1-2-1 模糊，让一根孤柱变成三柱的小山而不是一个孤立尖峰。边界复制，首尾两根不会被绕回去抹成一团 |
| 自动增益 | `--viz-norm MS` | 把环整体缩放，使其在安静与响亮段落都填满自己的带宽。参考取窗口内最响的一帧并做平滑，增益不会跟瞬态抖。频段之间的相对动态完整保留——它只是一个标量 |

两点需要知道：

- **没有降噪，也没有噪声门。** 抓到的已经是播放器自己的输出，里面每个起伏都是音乐；门限唯一能做的就是削掉音乐本体、把瞬态切掉。环拿到的是一个整体缩放，而不是「决定哪些频段有资格存在」。（cava 那个叫 `noise_reduction` 的旋钮其实是平滑强度，本项目改叫 `--viz-gravity`，免得被误解成降噪。）
- cava 的帧率补偿是单个 `pow(66/fps, 2.5)`，到 20fps 都还成立，再低就会塌——整根柱子一帧落完，看起来是闪断。因此运动模型单独钳了 dt，让极低的协商帧率变成「落得慢几帧」而不是「柱子闪烁」。

开 `--viz-fx` 时 `--dump` 会先连续渲染一小段再取最后一帧：运动模型与自动增益都需要历史，第一帧不代表实际画面。

**故意没做**的：cava 的圆头柱、颜色渐变、上下翻转——它们与本项目单色硬朗的基调相冲。

抓取侧、匹配规则与文中数字的实测来源，见 [docs/internals.md](docs/internals.md)。

### 自定义字体

只要 fontconfig 认得就能用，无需配置字体列表。

```bash
# 按字体名
--font "Inter"

# 回退链：拉丁用 Inter，其余交给 Noto
--font "Inter,Noto Sans CJK SC"

# 未安装到系统里的字体——不需要 root，也不用往 /usr/share/fonts 里拷
--font-file ~/Downloads/MyFont.ttf
```

`--font-file` 只在本进程内注册，**不需要再指定字体名**：family 名是从字体文件自身读出来的，所以单写 `--font-file` 就够了。以下情况再搭配 `--font`：不想手打字体自报的名字、`.ttc` 想用非首个字体面、或者想按顺序试多个已注册文件。传目录则注册该目录下全部字体。

```bash
# 整包字体，先匹配到的生效
--font-file ~/Downloads/fonts --font-file ~/Downloads/Display.ttf
```

这里的一切都刻意**不致命**：文件缺失或字体名不认识时只告警，卡片退回默认字体渲染——字形不对也比起不来强。实际生效的字体会在启动时打印：

```
  Font: Inter,Noto Sans CJK SC
```

两点值得知道。所选字体没覆盖的字符仍能显示——pango 会逐字符回退，所以纯拉丁字体下中文歌词照样出得来。标题按 semibold 字重绘制，若字体没有 semibold 字面就退回常规字重，而不是合成一个假粗体。

### 调版面无需打开 OBS

```bash
make dump                                                    # 假数据输出一张 PNG
./pw-mpris-visualcard-native --dump /tmp/card.png --time 1   # 使用真实播放数据
./pw-mpris-visualcard-native --demo --dump /tmp/card.png --lyrics 4 --time 1 --album 1
```

## 性能

| 指标 | 数值 |
| --- | --- |
| RSS | 40–70 MB（随输出尺寸增长） |
| 私有（匿名）内存 | **6–25 MB**（随封面缓存与 cairo/pango 字形缓存增长） |
| CPU | **约 5.5% 单核**（`460x690` @30fps，其中封面自转占 3.5 个百分点） |
| 无消费者连接时 | 0 帧推送，CPU **约 0.25%** |
| 子进程 | **0**（常驻 D-Bus 连接） |

私有内存之外的部分是 cairo / pango / dbus / curl / gdk-pixbuf / PipeWire 等共享库与 fontconfig 缓存，属于系统级共享页，不会与其它进程重复占用。

### 尺寸选择

版面是「圆封面 + 下方文字」的纵向堆叠，圆直径受纵向预算约束，所以方形画布左右必然空掉一截。把宽度收窄到刚好包住内容即可消除留白：

| `--size` | 内容占宽 | 左右各余 | CPU (单核) |
| --- | --- | --- | --- |
| `540x540` | 81% | 19% | ~4.1% |
| `360x540` | 96% | 4% | ~3.6% |
| `400x600` | 96% | 4% | ~4.2% |
| **`460x690`** | **96%** | **4%** | **~5.5%** |
| `480x720` | 96% | 4% | ~6.3% |

放大时宽高同比缩放、保持 `W:H = 2:3`，左右留白恒为 4%。开销主要由封面和文字决定，两者都随高度增长，所以收窄宽度几乎不省 CPU，收窄只为消除空白。

### 进一步降低 CPU

- `--spin 0`：直接降低约 60%（自转是逐帧重采样，为全流程开销最大的环节）。
- `--fps 24`：约省六分之一。
- 把尺寸高度调小一档。

`--viz 1` 的增量开销实测约 **0.9% 单核**（`460x690` @30fps，72 柱：绘制 +0.27 ms/帧、FFT +0.20 ms/帧。有封面时环会把它缩小，反而可能更快，两种方向见 [docs/internals.md](docs/internals.md)）；`--viz-fx`（默认开启）再加约 **0.004% 单核**（实测整条链 0.0013 ms/帧）：这些环节只是 72 个元素上的算术扫描，比一次 cairo 填充便宜三个数量级。柱的绘制开销与柱子填满带宽的比例成正比（柱子厚了 50%），与后处理无关。

## 目录结构

| 路径 | 内容 |
| --- | --- |
| `README.zh-CN.md` | 本文件（中文版） |
| `README.md` | 英文版（默认） |
| `LICENSE` | MIT 许可证全文 |
| `docs/internals.md` | 渲染侧约束、实测数据与调试命令（改代码前先读，目前仅中文） |
| `docs/card-*.png` | `--dump` 输出的效果图 |
| `Makefile` | 构建脚本，含 `dump` / `run` / `install-service` / `uninstall-service` / `compile-commands` 目标 |
| `pw-mpris-visualcard.service` | systemd 用户服务模板，由 `make install-service` 渲染安装 |
| `src/types.hpp` | `Track` / `NowPlaying` / `Config` 数据结构 |
| `src/mpris.{hpp,cpp}` | sdbus-c++ 常驻连接 + 采样线程 + LRC 解析 |
| `src/audio.{hpp,cpp}` | PipeWire 抓取：注册表、播放器名匹配、原始单声道采样环形缓冲（`--viz`） |
| `src/analyser.{hpp,cpp}` | FFT、窗函数、频段折叠与固定 dB 窗口（`--viz`） |
| `src/fx.{hpp,cpp}` | 频谱环的后处理：展开、cava 运动模型、频段整形、滑动窗口自动增益（`--viz-fx`，默认开启） |
| `src/card.{hpp,cpp}` | cairo + pango 版面绘制 |
| `lib/pw-video-simple-interface/` | git 子模块：视频节点（注册、缓冲、帧率协商）与 cairo 辅助模块（帧、文字、素材缓存、HTTP） |
| `src/main.cpp` | 模块组装与命令行解析 |
| `.clangd` + `make compile-commands` | 编辑器工具链：生成编译数据库，让 clangd 能解析 PipeWire 与子模块头文件 |

## 贡献

- 行为改动：附命令与其输出。
- 性能结论：附测量数据。没有测量来源的数字不写进文档。
- 改渲染路径前先读 [docs/internals.md](docs/internals.md)；PipeWire 相关代码在 [`pw-video-simple-interface`](https://github.com/zlinux-live-util/pw-video-simple-interface)。
- 说明工作是怎么产出的（建议而非要求），便于回溯。
- 补丁的审核、验证与后续维护由提交者承担。

本项目没有 CLA。

## 许可证

本项目采用 **MIT License**，著作权归 **ZokuTe**（2026 年起），全文见 [LICENSE](LICENSE)。提交的贡献按同一许可进入本项目。

运行期以动态链接方式使用以下系统库，未捆绑、未修改其代码，各自适用其自身许可：

| 库 | 本机已装版本声明的许可 |
| --- | --- |
| cairo | LGPL-2.1-only OR MPL-1.1 |
| pango | LGPL-2.0-or-later |
| gdk-pixbuf | LGPL-2.0-or-later |
| PipeWire | MIT，LGPL-2.1-or-later |
| sdbus-c++ | LGPL-2.1-only，含 sdbus-c++ LGPL 例外条款 |
| libcurl | curl 许可（MIT 类） |

OBS 侧的 [obs-pwvideo](https://github.com/tasokait/obs-pwvideo) 是 GPLv2 的独立程序，与本项目之间只有 PipeWire 节点的运行时数据流，不构成链接或派生关系，故各自许可互不影响。
