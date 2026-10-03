# 自定义字体

只要 fontconfig 认得就能用，无需配置字体列表。

```bash
# 按字体名
--font "Inter"

# 回退链：拉丁用 Inter，其余交给 Noto
--font "Inter,Noto Sans CJK SC"

# 未安装到系统里的字体——不需要 root，也不用往 /usr/share/fonts 里拷
--font-file ~/Downloads/MyFont.ttf

# 整包字体，先匹配到的生效
--font-file ~/Downloads/fonts --font-file ~/Downloads/Display.ttf
```

`--font-file` 只在本进程内注册 fontconfig，**不需要再指定字体名**：family 名是从字体文件
自身读出来的，所以单写 `--font-file` 就够了。以下情况再搭配 `--font`：不想手打字体自报的
名字、`.ttc` 想用非首个字体面、或者想按顺序试多个已注册文件。传目录则注册该目录下全部字体。
`~/` 会展开，参数可重复。

```bash
--font-file ~/Downloads/MyFont.ttf --font "My Font"
```

## 行为

这里的一切都刻意**不致命**：文件缺失或字体名不认识时只告警，卡片退回默认字体渲染——字形不对
也比起不来强。实际生效的字体会在启动时打印：

```text
  Font: Inter,Noto Sans CJK SC
```

两点值得知道：

- 所选字体没覆盖的字符仍能显示——pango 会**逐字符**回退，所以纯拉丁字体下中文歌词照样
  出得来。
- 标题按 semibold 字重绘制，若字体没有 semibold 字面就退回常规字重，而不是合成一个假粗体。

## 要注意的时序

`FcConfigAppFontAddFile()` 必须在进程里第一个 `PangoContext` 创建之前调用，否则注册的文件
永远不会被 pango 看到。所以 `applyFontConfig()` 必须留在 `main()` 里、`App`（它构造 `Card`）
之前。

这条约束的成因、复现脚本（把系统字体目录摘掉、只留注册进来的那个文件，逐像素比对 baseline）
写在 [internals.md](internals.md) 的「字体」一节。改字体相关代码时按那套方法验一遍：只给
`--font-file`（族名自动取）与再手写 `--font "族名" --font-file x.ttf` 必须逐像素一致。
