# osu!lazer 主菜单与“星环lazer”的证据分析

日期：2026-10-25。目标是用户提供的 `C:\Users\Sonic853\AppData\Local\osulazer\current` 安装版，不是先前分析的 osu!stable。本文记录原程序证据与移植边界；插件的最终构建、兼容测试结果以实施记录为准。

## 1. 目标身份、源码与证据

`osu!.exe` 是本机启动器，真正的主菜单、动画及节拍逻辑位于 `osu.Game.dll`，图形、音频和窗口底层位于 `osu.Framework.dll`。直接把稳定版的徽标参数套在 lazer 上不成立。

| 文件 | 安装版身份 | SHA-256 |
|---|---|---|
| osu!.exe | 原生 PE apphost，256,288 字节 | `44f341bec1d2b786265c321a60d708b476e491286e611d2d19f19f86d1c68933` |
| osu!.dll | 入口托管程序集，251,168 字节 | `bb263b8bfad9ed54b524119e5ff9949bd14d02b51901d43b5496e76055afdb98` |
| osu.Game.dll | 2026.1005.0.0，AnyCPU CIL，.NET 10 | `1d33937f2ddc1045f525342ce1243397589d136ed5be6691093cb91bf64959b7` |
| osu.Framework.dll | 2026.921.1.0 | `36302a02937421361f186d85d6fe608e52799ef8bedac0d8fadbfdba68492ae3` |
| osu.Game.Resources.dll | 2026.918.0.0 | `49522de81337d4da39f7ef42fbffba4abe432e7220abbee816432c7233fb1800` |

`osu!.runtimeconfig.json` 指向随程序分发的 .NET 10.0.12。插件不需要引用或分发这套 CLR；将相关数学、状态和纹理渲染移植到现有原生 DLL 即可。

源码基准：

- `D:\Git\OSU\osu`：HEAD `efac2716be00cf8ab1ffefecab58c46c65715885`，2026-10-25。
- 安装版对应标签 `2026.1005.0-lazer`：`9a10d935b05a3b896f4bd811d620f2023331db93`。
- `D:\Git\OSU\osu-resources`：HEAD `b86b30a9c1109f612fead25c269f8ad6cae4a1c1`，2026-10-24。
- 指定目录没有修改源码。对 `Screens/Menu`、频谱、三角形、视差、粒子等关键源文件比较安装版标签和 HEAD；OsuLogo、LogoVisualisation、三个入场类、粒子、视差的核心代码相同。`BeatSyncedContainer` 的 HEAD 把默认最低拍长从 0 改为 50 ms，不能把这个新默认值误写为安装版行为。MainMenu/IntroScreen 的 HEAD 另有滚轮处理类更名，退出确认界面也有变化。

REA MCP 在本会话没有暴露工具；只读 doctor 报告 Codex 注册项 `config_drift`，没有修改配置。使用可用的 REA 6.5.0 CLI 完成不执行目标代码的托管静态检查，再用本机 dnSpy.Console 定向反编译所需类型。完整记录保存在私有 `rebuild/tests/osu_lazer_20261025/`：

| 证据 | REA Evidence ID | 说明 |
|---|---|---|
| EXE 分类 | `ev_c18b17f86b4b2da0721ca3f1af07af63823459d2ea9413f31388f15f169e0937` | 无 CLI 头，原生启动器 |
| Game 元数据 | `ev_dfaa738d65f116fcdf9ef07110a48e55a81f1b38a77d3b5c4a16ffd08c4a366a` | MVID `f70ba22c-d873-4d4d-87f8-73592f4d211c` |
| Framework 元数据 | `ev_54025002c4f6f723b04594a3462d0da52c18bd5fa72c05e97c7b18f911951854` | MVID `31242d89-69d4-4a14-ac92-b008cbf2cddf` |
| 资源表与逐项哈希 | `ev_4ebac431bf758bfb95a8f298fa8735ea13f3d68d9e543139d64015ad0c1cd5a4` | 资源内容与本地源码资源交叉核对 |

REA 静态检查不代表运行观察。第 10 节单独说明实际原程序运行。

## 2. 布局坐标与层次

[OsuGame.cs:947](D:/Git/OSU/osu/osu.Game/OsuGame.cs:947) 的逻辑基准是 **1024×768**。安装 Framework 的 `DrawSizePreservingFillContainer.Update`（token `0x06002220`）以 `min(窗口宽/1024, 窗口高/768)` 等比缩放，扩大可用逻辑宽高，不把宽屏拉伸为 4:3。

`TextureStore` 默认 `ScaleAdjust=2`。安装版 `Menu/logo.png` 是 **1024×1024**，所以正常 Sprite 尺寸为 **512×512 逻辑单位**。圆形内容与频谱根部使用 `SCALE_ADJUST=.94`，直径为 **481.28**。1280×720 窗口的整体倍率为 .9375，整个徽标纹理为 480×480 像素；必须继续乘节拍、幅度、悬停等容器倍率。

MainMenu 的 Initial 状态徽标固定在窗口中心 `(0.5,0.5)`。展开按钮菜单后，徽标在 200 ms 内缩至 .5，交由 `LogoTrackingContainer` 跟踪菜单的实际布局位置；返回 Initial 用 800 ms OutExpo 回到中心、倍率 1。不能把展开菜单的偏移用作常态视觉效果的中心。

主要绘制顺序来自 [MainMenu.cs:142](D:/Git/OSU/osu/osu.Game/Screens/Menu/MainMenu.cs:142)：

1. 背景屏幕及其切换、视差。
2. 按钮菜单的视差容器。
3. 独立 `logoTarget` 中的徽标代理。
4. 左右闪光。
5. 右上角歌曲栏。
6. Kiai 星星喷泉。
7. 底部提示/横幅、左上角支持者区。
8. 长按退出覆盖层。

“星环lazer”移植的是音乐视觉场景；登录、选曲、游戏按钮、网络横幅不是播放器视觉效果所需的功能。

## 3. 徽标不是一张完整粉色 PNG

[OsuLogo.cs:108](D:/Git/OSU/osu/osu.Game/Screens/Menu/OsuLogo.cs:108) 按以下嵌套实现：

```text
OsuLogo
  IntroSequence
  hover
    bounce / drag
      ripple（原徽标纹理，加法混合）
      amplitude
        beat
          环形频谱（.94，alpha .5）
          LogoElements
            圆形遮罩（.94）
              上下渐变色
              动态空心三角形
              点击/Kiai 白色闪光
            Menu/logo.png（白边、文字、阴影；内部透明）
          Impact 圆形边框
```

安装资源的 `Menu/logo.png` 哈希 `ed58639ac7d05df98047142232a40fb99222280e9cff7d67c09d80e8e2d0acd1`，与提供的 osu-resources 文件一致。它不同于 stable 的完整粉色菜单图。

用户的两种徽标应明确区分：

- **OSU! 原版实现**：原版透明边框文字纹理，加圆内渐变、动态三角形与原版动画。
- **TTPlayer 纯 PNG**：使用现有 `TTPlayer.png` 的颜色、橙色外环、透明度；整体施加同样的节拍、缩放、外部波纹和进退动画。不能叠加原版白圈，也不能用粉色层盖住该 PNG。

TTPlayer 源图不需要重新生成；图案资产调整与运行时逻辑尺寸是两个不同问题。

### 3.1 节拍与交互

安装 `OsuLogo.OnNewBeat` token `0x060044B6`、RVA `0x000F073C`，与 [OsuLogo.cs:308](D:/Git/OSU/osu/osu.Game/Screens/Menu/OsuLogo.cs:308) 相符。

令 `A=min(1,.4+max(leftPeak,rightPeak))`：

- 事件提前 **60 ms**。beat 容器 60 ms Out 缩至 `1-.02A`，随后用 **两拍** OutQuint 回到 1。
- ripple 从当前 amplitude 容器倍率扩至其 `1+.04A` 倍，耗时一拍；alpha 从 `.15A` 用 OutQuint 降至 0。
- Kiai 时，内圆白闪在 60 ms 升至 `.2A`，随后一拍退去；频谱 alpha 同步升至 `.5*1.8*A`，再回 .5。
- 三角形在真正拍点增加速度：普通 `+3A`，Kiai `+6A`。
- 持续播放时 amplitude 容器目标 `1-max(0,peak-.4)*.04`，使用 `Damp(old,target,.9,elapsedMs)`。这是逐帧峰值驱动，与节拍缩放分开。
- hover：500 ms OutElastic 到 1.1；移出同样回 1。
- 左键按下：1000 ms Out 到 .9；松开：500 ms OutElastic 回 1。
- 单击内圆白闪 alpha .4，1500 ms OutExpo 退去。
- 拖动位移为 `delta * |delta|^.6 / |delta|`；松开后 800 ms OutElastic 回中心。
- 菜单缩小落点的 Impact：边框厚 10，.94→1.12，250 ms，alpha 用 In 退去。

`Damp(start,end,b,dt)=start+(end-start)*(1-b^dt)`，dt 为毫秒。不能误当作每秒衰减。

### 3.2 圆内三角形

[TrianglesV2.cs:25](D:/Git/OSU/osu/osu.Game/Graphics/Backgrounds/TrianglesV2.cs:25)：基准三角宽 100，等边高比例 .866；徽标设置 ScaleAdjust=3、Thickness=.009、SpawnRatio=1.4。数量为 `int(width*.02*SpawnRatio)`，按宽度而非面积计算。速度倍率取均值 .5、标准差 .16 的正态分布，下限 .1；实际移动速度下限 .5，按 `-dtSeconds*Velocity*50/DrawHeight` 上升，越界重新生成。

底色上 `#ff66ab`、下 `#cc5289`；三角颜色上 `#ff66ab`、下 `#b6346f`。安装 `sh_TriangleBorder.fs` 用三角形边线距离产生空心边框与内外平滑，不是填满的实心三角拼贴。

## 4. 环形频谱与真实音频数据

安装 `BassAmplitudeProcessor.Update`（Framework token `0x06002861`，RVA `0x00086718`）读取 **512 点 FFT/256 个幅度值**：`ChannelGetData(...,0x80000001)`；左右峰值窗口为 `0.016666668` 秒，Stereo。Framework 没有额外的 stable 1.6 倍增益、平方根或对数压缩。波形、峰值、频谱必须来自当前播放数据。

[LogoVisualisation.cs:28](D:/Git/OSU/osu/osu.Game/Screens/Menu/LogoVisualisation.cs:28) 与安装版 token `0x060043B9`、`0x060043BB`、`0x060043C4` 对应：

| 项目 | 原版值/规则 |
|---|---|
| 有效条目 | 200 |
| 周向重复 | 5 圈，各偏移 72°，共最多 1000 条 |
| 采样更新 | 每 50 ms；积压不补跑全部旧采样 |
| 采样偏移 | 每次 +5，模 200 |
| 更新 | `a[i]=max(a[i], fft[(i+offset)%200]*(kiai?1:.5))` |
| 衰减 | `a=max(0,a-dtMs*.0024*(a+.03))` |
| 最大标尺长度 | `600*a` 逻辑单位 |
| 省略阈值 | `a<1/600` |
| 条宽 | `rootDiameter*sqrt(2*(1-cos(2π/200)))/2` |
| 起点 | 根圆周，不从圆心开始 |
| 混合 | additive，白色 alpha .2，再乘容器默认 alpha .5 |

图形层有 `Magnitude` 属性，但这一版绘制代码没有用它放大条长。移植不应根据属性名字凭空增加增益。

节拍来自 `.osu` 控制点，包括 offset、beatLength、拍号、Kiai。`BeatSyncedContainer` 暂停时以界面时钟和默认控制点产生闲置节拍，而真实音频幅度为空；不是随机生成频谱。没有谱面时使用默认节拍是合理退路，但无法证明与歌曲节奏相同。

## 5. 左右闪光与喷泉

[MenuSideFlashes.cs:40](D:/Git/OSU/osu/osu.Game/Screens/Menu/MenuSideFlashes.cs:40)：左右 Box 宽 400、高 1.5 倍屏幕，分别向屏外偏移 200，实际屏内渐变宽度约 200。普通状态每小节首拍双侧闪；Kiai 偶数拍左、奇数拍右。提前 65 ms 触发，上升 65 ms，随后一拍用 In 退去。

普通除数为 `.75/.55`，Kiai 为 `.7625/.8`。单侧 alpha：`clamp(.1+(对应声道峰值-.25)/除数,.1,1)`。颜色渐变从 baseColour 的 .6 alpha 到 0，计算使用线性颜色；默认是 osuColour.Blue，支持者皮肤可覆盖。

[KiaiMenuFountains.cs:48](D:/Git/OSU/osu/osu.Game/Screens/Menu/KiaiMenuFountains.cs:48) 只在 **进入 Kiai** 且距离该 effect 控制点不足 500 ms 时触发喷泉，避免定位进 Kiai 中途立即误喷。

[StarFountain.cs:37](D:/Git/OSU/osu/osu.Game/Screens/Menu/StarFountain.cs:37) 参数：

- 发射点距左右边缘各 250，Y 为屏幕底；粒子局部起点 `(0,50)`。
- 每边 240 粒子/秒，连续 800 ms；每粒子寿命随机 300～1000 ms。
- 竖速度 `-1400±100`，横速度 `direction*500*(1-2*喷射年龄/800)±60`；两边方向对称，随机选择向外扫、直上、向内扫。
- alpha=`1-progress`；大小从 1 插值到 `2.2±.4`；角度从 ±4 插值到 ±2，单位弧度。
- 位置严格为 `start+(velocity+(0,800*duration/1000*progress))*age/1000`。不是随意取 0.5gt²；最大寿命固定 1000 ms 时，竖向重力项可化为 `800*(age/1000)²`。
- 安装星星 PNG 为 66×68，TextureStore 后是 33×34 逻辑单位，additive。长时间跳帧只补一个当前粒子，不无限补历史粒子。

## 6. 背景视差

[OsuScreenStack.cs:28](D:/Git/OSU/osu/osu.Game/Screens/OsuScreenStack.cs:28) 的 .02 视差容器只包背景屏幕；MainMenu 按钮另用 .01。Initial 徽标在独立 logoTarget，**不跟鼠标漂移**。

[ParallaxContainer.cs:59](D:/Git/OSU/osu/osu.Game/Graphics/Containers/ParallaxContainer.cs:59) 在每个轴上计算：

```text
delta = pointer - screenCentre
relative = sign(delta) * (1 - .999^abs(delta))
targetOffset = relative * halfDrawSize * amount
targetScale = 1 + abs(amount)
position = lerp(old,target,OutQuint(clamp(dt,0,100)/100))
scale = lerp(old,target,OutQuint(clamp(dt,0,100)/1000))
```

方向与鼠标相同，有非线性饱和与平滑；不同于 stable 的反向线性偏移。背景图片使用 FillMode.Fill，保持比例裁切；BackgroundScreen 还有 `1+100/DrawWidth` 的保险缩放，防止其 ±50 逻辑单位转场露边。

## 7. 三套原版进场动画

安装版 `OsuConfigManager` 默认 `IntroSequence.Triangles`、`MenuVoice=true`。不能把 Circles 当作 lazer 唯一或默认启动动画。

### 7.1 默认 Triangles

[IntroTriangles.cs:171](D:/Git/OSU/osu/osu.Game/Screens/Menu/IntroTriangles.cs:171)，安装 `TrianglesIntroSequence.LoadComplete` token `0x0600435A`、RVA `0x000EB544`：

| 动画时刻 | 行为 |
|---|---|
| 200 ms | `wel` |
| 400 ms | `welcome` |
| 700 ms | `welcome to` |
| 900 ms | `welcome to osu!`，字间距从 5 在 5000 ms 内向 50 展开 |
| 1060 ms | 三角故障闪烁，每 22 ms 生成，120 ms 退去 |
| 1450 ms | 文字/故障层消失，四种规则图标出现，scale1、spacing200；父层向 .8 缩放 |
| 1650 ms | 图标 scale2、spacing30 |
| 1850 ms | 图标 scale4、spacing10；父层向1.3缩放 |
| 2080 ms | 图标消失，描绘徽标开始，持续920ms |
| 3000 ms | 描绘层移除，正常徽标/背景出现，进入MainMenu；全屏白色加法闪光在1000ms内以Out退去 |

正文是 Torus Light 42、底 padding10、初始字间距5。规则图标从 AvailableRulesets 创建，每个30逻辑单位，不能用四个普通圆代替并宣称形状相同。

描绘 Logo 的容器为 960×960。两层纹理：highlight 为白色，background 为 Gray(.6)。它们不是两张普通 RGBA 徽标：**红通道保存显现进度阈值，绿通道保存透明度**。片元输出 `R<progress ? (vertexRGB, vertexAlpha*G) : transparent`；进度在920ms线性0→1。

缩放同时分两层：内层起始1.2，前644ms不变，末276ms以 InQuint 到.4；外层起始1.2，920ms以 InQuad 到1.0。用一条均匀缩小曲线代替，会明显改变入场节奏。

TTPlayer PNG 不是该专用 RG 进度纹理，不能把其红/绿颜色通道直接解释为描绘数据。初次实现保持原图，使用同样的时间、缩放和透明度演出。后续按用户要求，从原 PNG 提取轮廓并生成独立 RG 数据，采用相同着色器规则和时间轴；正常显示仍使用原彩色 PNG。该数据是 TTPlayer 图案的适配，不是原程序自带资源，见 [实现第13节](IMPLEMENTATION_20261025.md#13-ttplayer专用rg进场纹理2026-10-25)。

### 7.2 Circles

[IntroCircles.cs:20](D:/Git/OSU/osu/osu.Game/Screens/Menu/IntroCircles.cs:20)：600ms 后开始歌曲，2900ms 加载菜单；`OsuLogo.PlayIntro` 图形序列长3150ms，正常徽标在3150～3350ms淡入。

`IntroSequence` 的小圆/中圆、四根斜线、welcome、圆内填充、四个彩色大圆有独立变换；不是把徽标简单旋转一圈。三套 Ring 的前景黑圆形成圆环；大圆基准460，整个Intro初始scale.5，末段以InOutCubic回1。

### 7.3 Welcome

[IntroWelcome.cs:28](D:/Git/OSU/osu/osu.Game/Screens/Menu/IntroWelcome.cs:28)：黑色480圆、深蓝频谱和welcome_text。父容器 .9→1，持续2142ms；alpha1800ms淡入，文字纵向0→1用400ms Out。2142ms后正常徽标/背景200ms淡入并进入菜单。配套主题音乐可循环，不等于当前曲目必须被替换。

## 8. 退出与界面挂起不是同一条路径

MainMenu 进入其它游戏模式时，`LogoSuspending` 用300ms InSine缩至.2并淡出；**这不是退出程序的动画**。

程序退出会先通过确认，然后 MainMenu 状态设为Exit、禁用覆盖层，菜单整体3000ms淡出、底部/支持者500ms OutQuint退去，再返回 IntroScreen 执行尾声。

[IntroScreen.cs:238](D:/Git/OSU/osu/osu.Game/Screens/Menu/IntroScreen.cs:238)，安装 token `0x06004325`：背景100ms变黑；有MenuVoice时，全局Game在3000ms由1淡到.01，结束后退出；无MenuVoice时缩为500ms。原版会同时淡出音乐，但播放器视觉插件退出全屏不应改变音乐播放状态或主音量。

[IntroScreen.cs:321](D:/Git/OSU/osu/osu.Game/Screens/Menu/IntroScreen.cs:321)，安装 token `0x06004329`：先关闭三角/波纹；将徽标中心和scale1在最多350ms OutQuint内恢复，同时350ms淡入。源码在这个序列后面还安排了 `RotateTo(20,4500)` 与 `FadeOut(3000)`，但**安装版默认Triangles的实际退出没有显示旋转**。2026-10-25 的后续复核明确纠正：不能从这条变换语句推断玩家能看到轻微旋转，也不能把它直接强制实现到“星环lazer”。

### 8.1 退出帧量化

使用两次独立原版运行的退出截图，在圆内提取白色/灰色 `osu!` 文字，排除旋转对称的外环；以安装版 `Menu/logo.png` 相同文字的二阶中心矩主轴为参考。测量不依赖主观判断，等比缩放和渐暗不改变该角度。

| 原版序列 | 取样范围 | 有效帧数 | 相对原纹理文字的旋转角度 |
|---|---|---|---|
| `outro` | 527～2606 ms | 20 | −0.0867°～+0.0876° |
| `second-outro` | 534～2613 ms | 20 | −0.0897°～+0.1260° |

这些约±0.13°的变化属于像素边缘、抗锯齿及阈值提取误差，没有随时间累积的角度。若上述 `RotateTo` 独立运行，退出2秒应已转动约7.33°，远超测量误差；因此两轮实测清楚排除了“已经转动，只是肉眼不明显”的解释。文字尺寸仍有原本节拍起伏，没有向 .2 持续收缩；不能将 `LogoSuspending` 的缩小路径作为退出替代。

原数据与可复算脚本：私有 `measure_outro_rotation.py`、`outro-rotation-measurements.json`。第一批仍有确认弹窗和最后接近全黑的帧不参与统计。

### 8.2 实际调用链和排除结果

1. `MainMenu.OnExiting` 先将 `Buttons.State=Exit`，再调用 `OsuScreen.OnExiting`。
2. `OsuScreen.onExitingLogo` 调用 `OsuLogo.AppendAnimatingAction(...,false)`，清除当时根徽标变换，再调用 `MainMenu.LogoExiting`。该方法只归还 proxy，不执行缩至 .2 的 `LogoSuspending`。
3. `IntroScreen.OnResuming` 启动黑背景和全局淡出，再经 `OsuScreen.applyArrivingDefaults(true)` 调用 `AppendAnimatingAction(...,true)`，等待已有根变换结束后才运行 `LogoArriving`。
4. `IntroTriangles.LogoArriving` 总是先调用 base；`resuming=true` 时不执行新建Triangles入场层的分支。因此不存在“派生类把base旋转直接改写成另一段退出”的代码。
5. MainMenu、IntroScreen 都从依赖容器取得同一个全局 `OsuLogo`；proxy只是切换绘制父容器，没有创建另一个退出徽标。
6. `ButtonSystem.Exit` 另安排归中和800ms OutExpo scale1。安装Framework的独立变换探针表明：单独执行原scale/alpha/rotation链确实会旋转，而后到的scale1变换本身不足以取消后段旋转，不能把“800ms缩放覆盖了旋转”当成已证实原因。

安装Framework `TransformSequence.TransformAborted`（token `0x06001815`）会移除同一序列中尚未开始的变换。独立探针中，再写入alpha淡入可同时移除未来旋转与未来自身淡出；这是该框架存在的机制，**不是已经追踪到原程序实际发生的具体覆盖调用**。当前已经定位到“根徽标变换的排队/取消与显示生命周期”这一层，尚未对原程序运行中的每一次变换增删做插桩，因此不指认未证实的唯一覆盖点。

### 8.3 重建应采用的行为

针对本次明确的安装版、默认Triangles流程，采用已复现的**背景转黑、徽标保留当前朝向、恢复中心与正常尺度、整体渐暗**；不额外施加20°旋转，也不额外加入缩至 .2。退出全屏不改播放器音乐状态或音量。其它intro/菜单起始状态不能凭这两轮默认场景测试一概宣称全部相同。

插件应保存“退出进行中”的状态并继续绘制到完成，由宿主结束全屏窗口。立刻销毁 HWND 后调用淡出没有可见效果。

## 9. 插件实现分工与边界

推荐维持 `ttp_osu.dll` 为可选插件：追加能力/样式接口区分已有stable与新增lazer；老ABI前缀不变。宿主只传播放位置、PCM、谱面控制点、背景/图案和鼠标/生命周期，不把lazer专用动画复制进主程序。

实现优先级：

1. 原版512逻辑图案、固定中心、层次与512点FFT。
2. 原版徽标组合、TTPlayer纯PNG分支，分别保留纹理透明度。
3. 原版节拍、hover/click、左右闪、Kiai进入喷泉、背景视差。
4. 默认Triangles入场和真实退出生命周期；独立计时，不改歌曲。
5. 原有D3D9及软件后端共用状态与几何，测试速度、输出一致性和旧ABI兼容。

TTPlayer的全屏视觉不恢复lazer账号、在线资源、游戏按钮、歌曲控制器或样本音效；无谱面歌曲的BPM/Kiai也无法从原程序逻辑“凭空恢复”。这些是场景适配边界，不应写为原版完整主界面已移植。

## 10. 原版程序实际运行对照

静态确认安装 Framework 的 `DesktopGameHost.GetDefaultGameStorage`：程序目录存在 `framework.ini` 时采用便携存储。将安装目录中的293个exe/dll/json等程序文件复制到私有 `original-runtime`，新建仅用于测试的 `framework.ini`（1280×720、Windowed、静音），没有复制或修改用户账号、Realm数据库、歌曲库或原配置。

运行日志确认原版采用 **Veldrid / Direct3D 11**，而不是根据依赖列表猜测正在使用OpenGL。日志、配置、缓存、数据库均落在测试目录。测试只截取所属窗口客户区，关闭后停止截取。

完成的观察：

- 第一轮90帧记录启动，实际出现分段welcome文字、规则图标、描绘徽标及菜单。首次运行向导随后覆盖界面，按Escape只关闭测试副本的向导。
- 常态、中心悬停、左上指针分别采样。Win32 SetCursorPos单独使用没有让SDL输入完整更新，因此最初hover样本不能当作悬停通过；第二轮通过系统鼠标移动事件确认了指针与1.1倍悬停效果。
- 常态徽标圆心在 `(639.5,359.5)` 附近，前一轮最终样本中心拟合误差小于0.13px；第二轮悬停/移出最后6帧中心误差均小于0.01px。粉色内圆半径悬停为215.26～219.42px，移出为197.33～200.23px，叠加节拍后的约1.1倍关系与源码相符；无stable式反向漂移。
- 确认退出后连续28帧观察黑底、徽标与频谱淡出；日志10:51:07进入退出流程，10:51:10停止，符合3000ms有MenuVoice分支。后续对两次退出各20帧测量文字朝向，确认默认Triangles退出不显示旋转，详见第8节。
- 测试后源EXE和osu.Game.dll哈希未变，测试进程正常停止。该原版.NET10程序的本轮运行是Windows11证据，不代表它能在XP/Win7运行。

私有产物包括 `entry.*.png`、`entry-second.*.png`、`normal.*.png`、`hover-real.*.png`、`corner-real.*.png`、`outro.*.png`，逐帧采集时刻JSON、原版运行日志和 `runtime-measurements.json`。图像为原程序实际输出，不是重绘mockup。

### 10.1 已回答与尚不能宣称的事项

| 问题 | 状态 |
|---|---|
| 安装版身份、主逻辑在哪个DLL | 已由REA静态证据确认 |
| 常态布局、图形层次、徽标构成 | 源码、安装反编译、资源、运行交叉确认 |
| 三套进场、默认Triangles时序 | 静态完整分析；默认Triangles有运行观察 |
| 退出不同于挂起 | 静态与运行日志确认 |
| 频谱、Kiai、粒子、视差参数 | 安装反编译与对应源码确认 |
| 原版与原生插件逐像素相同 | 尚不能宣称；渲染器、抗锯齿、随机数、FFT实现、显示色彩处理存在边界 |
| 原版在线/节日素材与用户皮肤全部变种 | 不在本次默认场景恢复范围 |
| Circles/Welcome在该安装版全部分支逐帧运行 | 本次未运行穷举，保留明确区分 |

## 11. 许可归属

提供的 osu 源码按仓库 `LICENCE` 为 MIT；osu-resources 的 `LICENCE.md` 为 CC BY-NC 4.0，素材不能当成MIT代码处理。移植的代码归属、使用的原版资源及更改说明应在插件仓库保留；按照用户要求，许可证无需嵌入DLL。TTPlayer图案继续使用用户提供素材，不用原OSU圆内内容覆盖它。
