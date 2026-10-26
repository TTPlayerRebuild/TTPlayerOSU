# 中心图案资源

## stable 环形频谱资源（2026-10-27）

原版 `osu!ui.dll` 的 `menu-vis` / `menu-vis@2x` 提取产物原样嵌入，未重新绘制、缩放或补行。源程序集 SHA256 为 `22e041674931161e2c2c15241fb6c5f3125f8d57e4660ef7c0b7fb64449253c7`。

| 文件 | 实际尺寸 | RCDATA | SHA256 |
| --- | --- | ---: | --- |
| `osu-menu-spectrum.png` | 115×38 | 116 | `e626e7ac0b597a71b32993b69f686d04c41591222b08ab8cee545d4209b39f7d` |
| `osu-menu-spectrum@2x.png` | 230×75 | 115 | `38936a2ff65da10047c1de065897a9242b7bf7260bd4302a03c1514822a61d0f` |

保留左侧开口的长空心线、右侧圆头、只有约43.5%长度的短实心条以及原始透明通道。高清版本按原程序整数尺寸规则使用前74行、37逻辑像素高度；裁剪通过UV完成，源PNG不改。版权声明与下述 stable 原美术相同；本项目MIT许可证不为这些第三方资源授予新的许可。技术证据与验证见 [环形频谱修复说明](../docs/STABLE_SPECTRUM_PARITY_20261027.md)。

## 中心图案

- `TTPlayer.png`：以用户指定的 488×488 原图为来源，按用户要求调整源图片的布局：完整图案等比缩放至 1019×1019，放入 1080×1080 透明画布的 (30,30)，左/上留白 30 像素、右/下 31 像素，匹配原版实际 @2x 图案主体位置；保留原来的橙色外环，不添加白环、阴影或其它装饰。调整后 SHA256 `ebe8e9d93bdc59ed28a429f4bf970622376d4dd81e16ea54fe9094a9f9163d28`，嵌入 RCDATA 101。下载目录的同名图片已同步，488px 原始输入备份为 `TTPlayer-before-osu-layout-20261025.png`，SHA256 `8dad422e586e99990de755aad60484fc23f143a99020edbd22eee7dc5470a3ee`。
- `osu-menu-logo.png`：按用户要求提供的 OSU! 图案，来自本机 `osu!ui.dll` 中的 `menu-osu@2x`，1080×1080；SHA256 `52108713bacd8a15b555dd79eb429b5c26373cc9d2b79494b5b6f6192c29ae3b`。原样嵌入 RCDATA 102。源程序集 SHA256 `22e041674931161e2c2c15241fb6c5f3125f8d57e4660ef7c0b7fb64449253c7`。
- `osu-welcome-text.png`：来自同一原版 `osu!ui.dll` 的 `welcome_text@2x`，750×156；SHA256 `7d83701260b8e7285083baf31f27a932f23f6a8b9a788e2a3e9aa9164b8d71cf`。保留提取的 PNG 原字节，嵌入 RCDATA 103，仅启用进场效果时延迟解码；使用其原透明通道，不把透明像素内的蓝色 RGB 变成不透明底色。不替换用户选定的中心徽标。

osu! 名称、徽标及原美术归其各自权利人所有；本项目的 MIT 源码许可证不为这份第三方美术授予新的许可。默认使用千千静听图案；本插件不是 osu! 官方产品。资源没有账号或安装路径依赖。

两份内置 PNG 的画布均为 1080×1080，使用原 OSU! 的 540px 标准分辨率四边形布局（480 高逻辑坐标下为 337.5）。千千静听源图片通过画布留白匹配 OSU! 外圆的名义占比，橙色环直接充当外环；不额外在绘制时补白环。OSU! 原资源的阴影保留。自定义图案由宿主读取并解码为预乘 BGRA，DLL 在设置时复制像素，并在同样大小的框内居中等比容纳，不修改用户选择的图片文件。

## 星环lazer 资源（2026-10-25）

下列文件原样复制自用户提供的 `D:/Git/OSU/osu-resources/osu.Game.Resources`，美术版权归 ppy Pty Ltd 及原作者；源资源仓库为 <https://github.com/ppy/osu-resources>。它们使用原资源仓库的 CC BY-NC 4.0 许可，完整条文保存在 `licenses/osu-resources-CC-BY-NC-4.0.md`。Torus 字体另保留原 `Fonts/Torus/LICENCE` 至 `licenses/Torus.txt`。PNG 均没有重新编码或修改颜色/像素；重命名仅用于与 stable 资源区分。

| 本地资源 | 原资源 | 内部资源号 |
|---|---|---|
| `osu-lazer-logo.png` | `Textures/Menu/logo.png` | 104 |
| `osu-lazer-star.png` | `Textures/Menu/fountain-star.png` | 105 |
| `osu-lazer-intro-highlight.png` | `Textures/Intro/Triangles/logo-highlight.png` | 106 |
| `osu-lazer-intro-background.png` | `Textures/Intro/Triangles/logo-background.png` | 107 |
| `osu-lazer-font.png` | `Fonts/Torus/Torus-Light_0.png` | 108 |
| `osu-lazer-ruleset-Osu.png` | `Textures/Icons/RulesetOsu.png` | 109 |
| `osu-lazer-ruleset-Taiko.png` | `Textures/Icons/RulesetTaiko.png` | 110 |
| `osu-lazer-ruleset-Catch.png` | `Textures/Icons/RulesetCatch.png` | 111 |
| `osu-lazer-ruleset-Mania.png` | `Textures/Icons/RulesetMania.png` | 112 |

lazer 的 `Menu/logo.png` 是 1024×1024 的透明边框与文字，正常模式以 512 逻辑像素绘制；内部的粉色渐变、圆形裁剪和向上移动的三角形由 DLL 根据原 `OsuLogo`/`TrianglesV2` 算法生成。选择千千静听或自定义图案时，正常模式只绘制选定 PNG，不附加这个白边或粉色底层。入场保留原 Triangles 时间轴；选择 OSU! 时读取原两张 R=揭示进度、G=透明度的纹理，千千静听使用下述专用 RG 数据，自定义图案沿用原 PNG 的缩放/透明度演出。`TTPlayer.png` 原字节保持不变。

`visual_lazer.cpp` 中的布局和动画算法，以及 `visual_renderer.cpp` 中对应的原着色器等价运算，来自/改写自 ppy/osu 的 MIT 源码；声明保存在 `licenses/osu-MIT.txt`。CPU 和 D3D9 共用图元和预生成的程序纹理，不依赖 .NET、osu! 安装目录或用户账号。

`src/visual_torus_metrics.h` 从同一 `Fonts/Torus/Torus-Light.bin` 复制完整的 329 项字形位置、偏移和 advance，源文件 SHA256 记在该头文件。它们索引已有的、未修改的 1020×810 PNG，不新增字体文件。由此 `TTPlayer` 中的大写字母及其它原字体字符也使用原版字形。原字库未包含的 Unicode 字符使用系统字体生成临时灰阶透明字形图集，仅在文字改变时生成；系统字体不打包进 DLL。因此 XP 与新版系统的中文字体外观可能不同。

## TTPlayer 专用 lazer 进场数据

| 文件 | 用途 | 资源号 |
| --- | --- | --- |
| `ttplayer-lazer-intro-highlight.png` | 白色轮廓揭示进度与覆盖率，1080×1080 | 113 |
| `ttplayer-lazer-intro-background.png` | 灰色后续轮廓揭示进度与覆盖率，1080×1080 | 114 |
| `ttplayer-lazer-intro.json` | 原图摘要、通道含义、轮廓及生成结果摘要 | 不嵌入 |

两张 PNG 是 **RGB 数据纹理**：R 是严格小于当前进度时才显示的阈值，G 是轮廓 alpha，B=0，PNG 自身 alpha 恒为255，防止图像解码预乘破坏数据。不是将 TTPlayer 原图的颜色通道当作进度。

`tools/generate_lazer_intro.py` 按原 PNG 的结构标定橙环内外边缘、耳机主体和中央波形。圆环使用解析圆，耳机和波形使用三次贝塞尔曲线，避免将金属反光误识别为小圈、毛刺及台阶；以4倍超采样生成抗锯齿覆盖率。沿闭合轮廓按弧长分配进度，以原版两张 RG 纹理的阈值分布匹配描绘节奏，不复制 osu! 字形。外环描边向内收2.5像素，使完整线宽处于原图轮廓之内；覆盖率仍限制在原图 alpha 内。进度分布参考上述原版资源，保留其来源及许可说明；TTPlayer 图案仍来自用户提供的图片。

开发者可安装 Pillow 后运行 `python tools/generate_lazer_intro.py` 重新生成。源图片 SHA256 不符合标定值时会拒绝生成，需重新标定；生成过程不写入原图。普通构建直接嵌入已生成 PNG，不运行生成器，不新增 Pillow 或 Python 的运行时依赖。
