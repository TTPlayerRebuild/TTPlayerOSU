# TTPlayer OSZ 与节拍视觉插件

仓库：<https://github.com/TTPlayerRebuild/TTPlayerOSU>

`ttp_osu.dll` 负责解析 **`.osz` 压缩包**，并实现播放器的 **节拍星环** 视觉效果。`.osu` 是谱面元数据，不是插件对外支持的歌曲格式。

## 功能

- 输入 OSZ 路径，由插件发起包内枚举、读取引用、匹配成员和返回曲目。
- 多个难度引用同一音频时只返回一次；不同音频全部返回，保持首次引用顺序。
- 按每首音频查找自己的背景，支持 PNG、JPEG、BMP、GIF。播放器优先使用音频内嵌封面，没有时调用插件取得背景。
- 不把击打音效、视频和皮肤图片加入播放列表；不模拟游戏时间轴或裁剪完整音频。
- 保留 Unicode 路径、路径别名去重、进度、取消及读取错误。整个包只读。
- `.osz` 继续被识别为独立归档格式，不提示改为 `.zip`。

底层 ZIP 成员流、CRC 校验、密码交互和缓存复用播放器／`ttpcomm.dll` 的通用归档接口。音频仍由已有解码器播放，插件不重复实现 MP3、Vorbis 等解码器。

## 节拍星环

- DLL 内实现连续 2048 点 FFT、1024 条环形频谱、徽标脉冲、扩散环、双侧喷星、左右闪光和背景视差。
- OSZ 使用当前音频关联谱面的 BPM、拍号与 Kiai 时间线；多难度按原 OSZ 枚举规则选择。普通歌曲使用可设置的备用 BPM，不推断 Kiai。
- 默认中心图案是 `assets/TTPlayer.png`，由用户原图等比排入与 OSU! 相同的 1080×1080 画布，保留橙色外环并增加透明留白。源 PNG 嵌入 DLL，无需单独分发图片。
- 节拍星环仅在视觉全屏及歌词组合全屏显示，优先使用 D3D9，设备不可用时使用 CPU。CPU/GPU 共用场景、层序和预乘透明度；普通窗口及皮肤画布不显示此效果。
- 播放器入口：选项 → 全屏显示，选择节拍星环，或在全屏视觉菜单选择。选项 → 视觉效果 → 节拍星环，可设置五类效果、备用 BPM、基础色和背景色；FPS 范围 10–100。
- 选项 → 常规 → 个性化 → 节拍星环图案切换：千千静听（默认）、OSU!、自定义。自定义图片保持比例，选择取消或无效文件不会替换原图案。

视觉场景与绘制代码全部位于 DLL。宿主只提供连续 PCM、实际播放时钟、窗口/鼠标、通用只读归档 I/O 和已解码的背景。无 BASS、OpenTK 或 osu! 运行时依赖。实现、性能及验证边界见 [实现与验证第 6 节](docs/IMPLEMENTATION_20261025.md#6-节拍视觉扩展)。

## 安装与兼容范围

关闭播放器，将 `AddIn/ttp_osu.dll` 放到播放器目录。需要已接入 **OSU ABI 1** 的重建版。

本插件可选。未安装或接口不兼容时，播放器隐藏对应的 OSZ 文件筛选项、节拍星环菜单/选项标签页及全屏配置项；原来选择节拍星环的配置改用频谱分析。普通 ZIP/RAR 和其它视觉效果正常使用。只有旧 OSZ 接口的插件仍可使用 OSZ 导入，视觉入口保持隐藏。安装、更新或移除 DLL 后重新启动播放器。

插件保留 `ttp_osu_query`（序号 1、OSZ ABI 1），`ttp_osu_query_visual`（序号 2）支持视觉 ABI 2，同时保留 ABI 1 的短函数表。图案切换需要 ABI 2；只有视觉 ABI 1 的旧 DLL 仍能显示全屏效果，但不显示图案选择。它不是旧版 `ttpGetSoundAddIn` 音频解码插件；**原版 5.7.9 播放器不会调用这些接口，单独放入原版 AddIn 不能增加 OSZ 导入或此视觉效果**。

同一个 x86 Release DLL 面向 XP SP3、Win7、Win10/11；CPU 需要 SSE2。使用 VC-LTL 5.3.1、YY-Thunks 1.2.2 构建，不要求用户另装 VC 运行库。实际验证范围见 [实现与验证](docs/IMPLEMENTATION_20261025.md)。

## 独立构建

需要 Windows、Visual Studio 2026 C++ x86 工具、CMake 3.24+、Python 3 和 PowerShell。

```powershell
./build.ps1 -Package -PackageVersion 2026.10.25
```

构建固定版本的兼容依赖，以 SHA256 校验下载；不构建播放器。导入审计拒绝 XP／Win7 不提供的静态入口。版本采用与播放器一致的北京时间 `yyyy.MM.dd[pN]` 规则，并写入文件版本资源。

输出：

- `build/Release/ttp_osu.dll`
- `build/Release/ttp_osu-版本号.zip`
- `build/Release/SHA256SUMS.txt`

ZIP 仅含 `AddIn/ttp_osu.dll` 和内部 `SHA256SUMS.txt`。许可证保留在仓库，不嵌入 DLL、不放进二进制发行包。

## Action 与播放器打包

独立 Action 可手动构建；勾选 `Release a Version` 时创建日期版本 Release。播放器发布 Action 从 **TTPlayerRebuild/TTPlayerOSU 最新稳定 Release** 下载 ZIP，核验 GitHub 资产摘要、包内 SHA256、文件版本、x86／旧系统导入及两个导出，再放入 **ExtraAddIn 可选插件包**。

播放器主程序包不包含或依赖本 DLL，单独构建主程序也不下载它。制作 ExtraAddIn 前需先发布带视觉接口的插件版本；不兼容的旧发行包会被校验拒绝。没有 DLL 时不显示该效果及更新提示，也不在 EXE 内重复实现效果。

## 接口与测试

ABI 头文件：归档 `include/ttp_osu/abi.h`，视觉 `include/ttp_osu/visual.h`。使用带大小/版本的结构、只读借用缓冲区和同步回调；无 STL 对象、内存释放权或 C++ 异常跨 DLL 边界。视觉实例内部同步，D3D9 创建/呈现/销毁必须在 HWND 所属线程执行；销毁或重新设置父窗口前先调用 `detach`。

私有测试放在工作区 `rebuild/tests/archive_support/` 和 `rebuild/tests/osu_visual_20261025/`，不放入本仓库，不上传，不在 Action 中运行。CI 的静态导入与打包校验属于构建验证。

## 许可证

- [本项目 MIT](LICENSE)
- [VC-LTL](docs/licenses/VC-LTL-LICENSE.txt)
- [YY-Thunks](docs/licenses/YY-Thunks-LICENSE.txt)
- [图案资源来源及归属](assets/README.md)
