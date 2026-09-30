# 实现细节

修改代码前先读本文。以下每条都来自实机验证，而非文档转述：其中若干条官方文档没有记载，或文档描述与本插件的实际行为不符。

本文与项目代码均在 LLM 辅助下编写。所有结论都经过实机复现——未经复现的推测不收录，因此每项都附有可重跑的对照数据或命令。

## 进程与线程

主线程跑 `pwvideo::VideoNode::run()`（PipeWire 主循环），`process` 回调也在其中执行；另有两条本项目自己的后台线程：

| 线程 | 职责 | 频率 |
| --- | --- | --- |
| MPRIS 采样线程 | 通过常驻 D-Bus 连接读取一次 `GetAll`，产出 `NowPlaying` 快照 | 播放中每 500ms，停止播放后放宽到 2000ms |
| 封面线程 | 仅在曲目变化时通过子模块的 `AssetCache` 抓取并解码封面 | 换歌触发 |

数据在各模块之间的流转：

| 组件 | 输入 | 输出 |
| --- | --- | --- |
| `mpris` | 播放器的 MPRIS 接口（D-Bus） | `NowPlaying` 快照 |
| 子模块 `extras/assetcache` | 封面 URL | cairo 表面（LRU 最多 3 张） |
| `card` | 快照 + 封面表面 | BGRA 帧（预乘 alpha） |
| `pwvideo`（子模块） | BGRA 帧 | `Stream/Output/Video` 节点，供 OBS 消费 |

**关键约束**：`process` 回调内只做渲染与拷贝，绝不进行网络访问 —— 这条现在是库对调用方的契约，写在 `lib/pw-video-simple-interface/src/pwvideo.hpp` 里。封面由后台线程准备完成后挂到 `art_`（`shared_ptr` + 互斥锁），渲染时只取一次引用。

**没有消费者就不渲染**：流在没有消费者连接时停在 `PAUSED`，库的 driver 线程只在 `STREAMING` 状态下触发图周期，因此回调不会被调用，CPU 约 0.25%。帧率协商同样在库内，见下节。

## 视频节点输出

节点注册、缓冲声明、帧率协商与推帧全部在 **`lib/pw-video-simple-interface`**（git 子模块）里，
本项目只提供一个「给我一帧」的回调。那一层的四条硬性要求、各自的定位过程与调试命令见
[`lib/pw-video-simple-interface/docs/internals.md`](../lib/pw-video-simple-interface/docs/internals.md)。

要改输出行为就改库、再更新子模块指针；本项目不再持有任何 PipeWire 代码：
`streaming()` 与 `onStreaming` 是库暴露的两个状态入口，需要「接入时重置时间基」这类动作时用它们。

## 渲染管线：四项性能约束

### 1. cairo 的线性渐变填充很贵

360×360 上一次约 **0.6 ms**，比画一整张卡片还贵。所以阴影、卡片底、封面圆底渐变、进度环底环**以及全部文字**都烘焙进 `Card::staticLayer()`，每帧只做一次 `CAIRO_OPERATOR_SOURCE` 拷贝。

改版面时不要把这些绘制挪回逐帧路径。

### 2. 封面旋转必须每帧真做

曾按 2° 分档缓存来省 0.4ms，结果 24 秒一圈时每帧只转 0.5°，**刷新率只有约 7.5Hz**，肉眼可见明显卡顿；省下的开销不足以抵消画质损失。

`Card::rotateInto()` 是手写的逐行步进旋转（旋转后同一行内源坐标等步长，整行只有起始值需要两次乘法），并使用 16.16 定点 + 双通道打包插值。已评估并排除的路径：

| 实现 | @327px |
| --- | --- |
| 双精度 + 逐通道（最初） | 0.754 ms |
| 三剪切分解 | 0.943 ms（更慢） |
| **定点 + 双通道打包（当前）** | **0.431 ms** |

**三剪切为什么更慢**：它是为访存局部性服务的经典算法，但那张源图（428KB）本来就装得进 L2，局部性不是瓶颈，**算术才是**（每像素约 10 次 double 运算 + 16 次通道乘法）。三剪切要做三遍，等于把算术量翻三倍。

### 3. 封面源图要带 1px 边框

旋转采样时 `ix`/`iy` 会落到 `-1`，源图带边框就不必逐像素 clamp。不用担心越界：目标圆半径等于源图内切圆半径，旋转是绕中心的刚性变换，采样点必然落在 `[-1, sw-2]` 内。

### 4. 插值必须逐通道累加，不要用权重和为 65536 的打包写法

`0x00FF00FF` 打包两个通道的技巧**只在权重和为 256 时安全**。若权重和为 65536（四像素双线性插值的常见写法）：

```
x = (R << 16) | B
x * 65536  →  R 占了 bit 32–47，直接溢出 uint32 被打飞
```

表现为**封面颜色整体错乱**（红色通道变成垃圾值）。逐通道计算的单通道上限是 `255 × 65536 ≈ 1.67e7`，int32 装得下；慢约 25%，但结果正确。

**不过**：拆成两阶段、每阶段权重和为 256 时，打包就是安全的——现在的 `lerp2()` 正是这么做的，既快又对：

```c
// w ∈ [0, 256]，iw = 256 - w
lo = (((a & 0x00FF00FF) * iw + (b & 0x00FF00FF) * w) >> 8) & 0x00FF00FF;  // R + B
hi = ((((a >> 8) & 0x00FF00FF) * iw + ((b >> 8) & 0x00FF00FF) * w) >> 8) & 0x00FF00FF;  // G + A
return lo | (hi << 8);
```

## 字体：一条必须遵守的时序约束

`--font` / `--font-file` 落到 `Card::drawLine()` 的 `LabelSpec::family`，由
`pango_font_description_set_family()` 交给 pango。逗号分隔的链由 pango 解析：每个族名都交给
fontconfig 查询，pango 再按字符挑有覆盖的字体，所以「拉丁字体 + 中文字体」配成一条链即可；没有
semibold 字面的字体，标题会退回常规字重而不是合成假粗体。

唯一需要当心的是**注册时机**：

> `FcConfigAppFontAddFile()` 必须在进程里第一个 `PangoContext` 创建之前调用。

`PangoFcFontMap` 在构造时会把 fontconfig 当时的字体族列表**快照**下来。之后再注册的文件，
即使 family 名在匹配期能解析，也永远不会被 pango 看到——而 `Card` 成员 `text_` 的 `TextRenderer`
构造函数里就会建好一个 `PangoLayout`。所以 `applyFontConfig()` 必须留在 `main()` 里、
`App`（它构造 `Card`）之前。

验证方式（把系统字体目录摘掉，只留注册进来的那个文件）：

```bash
mkdir -p /tmp/fc/fonts /tmp/fc/dl /tmp/fc/cache
printf '<fontconfig><dir>/tmp/fc/fonts</dir><cachedir>/tmp/fc/cache</cachedir></fontconfig>' \
    > /tmp/fc/fonts.conf
cp 某个字体.ttf /tmp/fc/dl/

# 1) 空字体目录：族名解析不到，两条警告，渲染回退
FONTCONFIG_FILE=/tmp/fc/fonts.conf ./pw-mpris-visualcard-native --demo --progress 0 \
    --font "某个字体族名" --dump /tmp/a.png

# 2) 只注册文件：不再报警
FONTCONFIG_FILE=/tmp/fc/fonts.conf ./pw-mpris-visualcard-native --demo --progress 0 \
    --font-file /tmp/fc/dl/某个字体.ttf --dump /tmp/b.png

# 3) baseline：把同一个文件放进配置的字体目录，当作「已安装」。fc-cache 不能省，
#    目录缓存的秒级时间戳会漏掉同一秒内的复制
cp 某个字体.ttf /tmp/fc/fonts/
FONTCONFIG_FILE=/tmp/fc/fonts.conf fc-cache -f
FONTCONFIG_FILE=/tmp/fc/fonts.conf ./pw-mpris-visualcard-native --demo --progress 0 \
    --font "某个字体族名" --dump /tmp/base.png

cmp /tmp/b.png /tmp/base.png   # 必须逐像素一致
```

`--progress 0` 是为了去掉时钟：进度环跟着墙上时间走，不关掉整图永远对不齐。`--demo` 的曲目每
10s 一换，所以两次运行还得落在同一个 10s 窗口内。不要拿系统环境的渲染当 baseline——裸
`FONTCONFIG_FILE` 会连 conf.d 的 hinting/antialias 规则一起丢掉，字体选对了像素也会差；baseline
只能取自同一份配置。另外，只给 `--font-file`（族名自动取）与再手写 `--font "族名" --font-file
x.ttf` 必须逐像素一致。

## 调试方法

节点层「在不在、属性对不对、帧有没有真的流」的排查手段归库，见
[`lib/pw-video-simple-interface/docs/internals.md`](../lib/pw-video-simple-interface/docs/internals.md)。

本项目自己的手段是不出画面直接渲染 PNG，版式问题不必开 OBS：

```bash
./pw-mpris-visualcard-native --dump /tmp/card.png --time 1 --lyrics 4 --album 1
```


### 验证渲染正确性的两个不变量

比肉眼可靠：

1. **旋转不变性**：旋转是绕中心的保面积映射，所以封面圆内的**平均颜色在任何角度下都必须相同**。通道被打飞会让均值剧烈漂移。
2. **角度 0 的恒等性**：`--spin 0` 时旋转是恒等变换，输出应与源图逐字节一致。

### 测量注意事项

- **测 CPU 别用日志里的推帧计数**——那是每 60 帧打一行，取样会滞后。数消费者实际收到的帧数除以精确时长才准。
- **改热循环前先写微基准**。上文「三剪切更慢」和「定点快 43%」都是先测后写；只靠直觉的话会写出一个慢 25% 的 "优化"。
