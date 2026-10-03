# 开发与构建

面向贡献者与打包者：架构、构建细节、目录结构、依赖许可、贡献约定。

实现层面的硬性约束与实测结论在 [internals.md](internals.md)，**改渲染路径前先读那篇**。

## 架构

单进程，各阶段在同一进程内通过内存传递数据。

| 阶段 | 输入 | 输出 | 实现 |
| --- | --- | --- | --- |
| 播放状态 | 播放器的 MPRIS 接口（D-Bus） | `NowPlaying` 快照：标题、歌手、专辑、进度、歌词、封面 URL | `mpris`，常驻 D-Bus 连接，不 fork 子进程 |
| 素材获取 | 封面 URL | cairo 表面（LRU 缓存，最多 3 张） | 后台线程 + 子模块的 `AssetCache` |
| 版面渲染 | 快照 + 封面表面 + 频段电平 | BGRA 帧（预乘 alpha） | `card`，cairo + pango |
| 频谱（可选） | 播放器自己的 PipeWire monitor 流 | 频段电平 | `audio` + `analyser`，libpipewire + 2048 点 FFT |
| 显示端后处理（可选） | 频段电平 | 有重量的柱高 | `fx`，取自 cava 的运动模型（`--viz-fx`，默认开） |
| 视频输出 | BGRA 帧 | `Stream/Output/Video` 节点 | [`pw-video-simple-interface`](https://github.com/zlinux-live-util/pw-video-simple-interface)，libpipewire |

进程私有内存 6–25 MB，详见 [performance.md](performance.md)。

PipeWire 侧（节点注册、缓冲声明、帧率协商、帧回调契约）全部在
[`pw-video-simple-interface`](https://github.com/zlinux-live-util/pw-video-simple-interface)
子模块里，本项目只提供「给我一帧」的回调。该层的排查手段见它的
[internals.md](../lib/pw-video-simple-interface/docs/internals.md)。

## 构建

```bash
git submodule update --init --recursive   # 视频节点库是子模块
make                                      # → ./pw-mpris-visualcard-native
make PORTABLE=1                           # 同上，但不加 -march=native
make dump                                 # 假数据渲染一张 PNG 到 /tmp/card.png
make run                                  # 构建并运行
make compile-commands                     # 生成 compile_commands.json 供 clangd 使用
```

子模块的源码直接编进本项目的构建树：同一套编译参数、没有需要跟踪的 ABI、子模块目录里不留
`.o`。未初始化子模块时 `make` 会带明确提示直接报错。

### 编译参数

| 参数 | 说明 |
| --- | --- |
| `-O3 -g -funroll-loops` | 默认优化级别。`-g` 保留调试信息，发布构建去掉它即可 |
| `-march=native` | 封面旋转的热循环收益明显，实测整帧降低 22%。代价是二进制绑定本机指令集 |
| `PORTABLE=1` | 去掉 `-march=native`。换机器运行或分发 AUR 包时用它（AUR 包即以 `PORTABLE=1` 构建） |
| `EXTRA_CXXFLAGS` | 追加自己的编译参数 |
| `SERVICE_ARGS` | `make install-service` 时写进 unit 的启动参数 |

改热循环之前先写微基准，别凭直觉改：`docs/internals.md` 里「三剪切更慢」和「定点快 43%」
两条都是先测后写的结论，只靠直觉容易写出一个慢 25% 的"优化"。

### systemd unit

仓库里的 `pw-mpris-visualcard.service` 是含 `@REPO@` 与 `@ARGS@` 的模板，由
`make install-service` 渲染（而不是复制）到 `~/.config/systemd/user/`。该目标只写 unit 并
`daemon-reload`，不会 enable 或启动。

```bash
make install-service
make install-service SERVICE_ARGS="--node pw-mpris-visualcard --size 360x360 --fps 30"
make uninstall-service
```

用 AUR 包安装时不要跑这个目标：它写出的 unit 落在 `~/.config/systemd/user/`，会遮蔽包放在
`/usr/lib/systemd/user/` 的那一份。改参数用 `systemctl --user edit pw-mpris-visualcard`。

### 编辑器工具链

`make compile-commands` 生成 `compile_commands.json`。子模块头文件只能通过 Makefile 里的
`-I` 找到，没有这份数据库 clangd 解析不了 `text.hpp` / `assetcache.hpp`，会报一片假错误。
`.clangd` 与 `.typos.toml` 分别是 clangd 与 typos 的配置。

## 目录结构

| 路径 | 内容 |
| --- | --- |
| `README.md` | 英文 README |
| `README.zh-CN.md` | 中文 README |
| `LICENSE` | MIT 许可证全文 |
| `docs/` | 本目录下的文档与 `--dump` 效果图 |
| `docs/internals.md` | 渲染侧约束、实测数据与调试命令（改代码前先读） |
| `docs/spectrum-ring.md` | 频谱环的使用与调参指南 |
| `docs/fonts.md` | 自定义字体指南 |
| `docs/performance.md` | 性能实测数据与省 CPU 的做法 |
| `docs/development.md` | 本文：架构、构建、目录结构、贡献约定 |
| `Makefile` | 构建脚本，含 `dump` / `run` / `install-service` / `uninstall-service` / `compile-commands` 目标 |
| `pw-mpris-visualcard.service` | systemd 用户服务模板，由 `make install-service` 渲染安装 |
| `src/types.hpp` | `Track` / `NowPlaying` / `Config` 数据结构 |
| `src/mpris.{hpp,cpp}` | sdbus-c++ 常驻连接 + 采样线程 + LRC 解析 |
| `src/audio.{hpp,cpp}` | PipeWire 抓取：注册表、播放器名匹配、原始单声道采样环形缓冲（`--viz`） |
| `src/analyser.{hpp,cpp}` | FFT、窗函数、频段折叠与固定 dB 窗口（`--viz`） |
| `src/fx.{hpp,cpp}` | 频谱环的后处理：展开、cava 运动模型、频段整形、滑动窗口自动增益（`--viz-fx`，默认开） |
| `src/card.{hpp,cpp}` | cairo + pango 版面绘制 |
| `src/fonts.{hpp,cpp}` | fontconfig 字体注册（`--font-file`） |
| `src/main.cpp` | 模块组装与命令行解析 |
| `lib/pw-video-simple-interface/` | git 子模块：视频节点（注册、缓冲、帧率协商）与 cairo 辅助模块（帧、文字、素材缓存、HTTP） |

## 依赖与许可

运行期依赖全部是发行版仓库里的系统库，不涉及语言包管理器：

| 库 | 用途 | 本机已装版本声明的许可 |
| --- | --- | --- |
| cairo | 绘制 | LGPL-2.1-only OR MPL-1.1 |
| pango | 文字排版 | LGPL-2.0-or-later |
| gdk-pixbuf | 封面解码 | LGPL-2.0-or-later |
| PipeWire | 视频节点与音频抓取 | MIT，LGPL-2.1-or-later |
| sdbus-c++ | MPRIS / D-Bus | LGPL-2.1-only，含 sdbus-c++ LGPL 例外条款 |
| libcurl | 封面下载 | curl 许可（MIT 类） |
| fontconfig | 字体解析与注册 | MIT-style |

这些库均以动态链接使用，没有捆绑或修改其代码，各自适用其自身许可。

OBS 侧的 [obs-pwvideo](https://github.com/tasokait/obs-pwvideo) 是 GPLv2 的独立程序，与本
项目之间只有 PipeWire 节点的运行时数据流，不构成链接或派生关系，故各自许可互不影响。

## 贡献

- 行为改动：附命令与其输出。
- 性能结论：附测量数据。没有测量来源的数字不写进文档。
- 改渲染路径前先读 [internals.md](internals.md)；PipeWire 相关代码在 [`pw-video-simple-interface`](https://github.com/zlinux-live-util/pw-video-simple-interface)。
- 说明工作是怎么产出的（建议而非要求），便于回溯。
- 补丁的审核、验证与后续维护由提交者承担。

本项目没有 CLA；贡献按 MIT 进入本项目。

写文档时区分两类内容：README 只留普通用户需要的东西（怎么装、怎么用、参数含义），实现细节、
实测数据与调参原理放进 `docs/`。README 是英文为主、中文镜像同步维护的两份，`docs/` 目前仅中文。
