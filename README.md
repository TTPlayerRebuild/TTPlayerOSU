# TTPlayer OSZ 压缩包插件

仓库：<https://github.com/TTPlayerRebuild/TTPlayerOSU>

`ttp_osu.dll` 负责解析 **`.osz` 压缩包**，将其中谱面实际引用的音乐提供给重建版播放器。`.osu` 是包内元数据，不是插件对外支持的歌曲格式。

## 功能

- 输入 OSZ 路径，由插件发起包内枚举、读取引用、匹配成员和返回曲目。
- 多个难度引用同一音频时只返回一次；不同音频全部返回，保持首次引用顺序。
- 按每首音频查找自己的背景，支持 PNG、JPEG、BMP、GIF。播放器优先使用音频内嵌封面，没有时调用插件取得背景。
- 不把击打音效、视频和皮肤图片加入播放列表；不模拟游戏时间轴或裁剪完整音频。
- 保留 Unicode 路径、路径别名去重、进度、取消及读取错误。整个包只读。
- `.osz` 继续被识别为独立归档格式，不提示改为 `.zip`。

底层 ZIP 成员流、CRC 校验、密码交互和缓存复用播放器／`ttpcomm.dll` 的通用归档接口。音频仍由已有解码器播放，插件不重复实现 MP3、Vorbis 等解码器。

## 安装与兼容范围

关闭播放器，将 `AddIn/ttp_osu.dll` 放到播放器目录。需要已接入 **OSU ABI 1** 的重建版。

插件导出 `ttp_osu_query`（序号 1），提供 OSZ 归档扩展接口。它不是旧版 `ttpGetSoundAddIn` 音频解码插件；**原版 5.7.9 播放器不会调用此接口，单独放入原版 AddIn 不能增加 OSZ 导入功能**。

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

独立 Action 可手动构建；勾选 `Release a Version` 时创建日期版本 Release。播放器 Action 从 **TTPlayerRebuild/TTPlayerOSU 最新稳定 Release** 下载 ZIP，核验 GitHub 资产摘要、包内 SHA256、文件版本、x86／旧系统导入及导出名称，再放入播放器通用包。

接入播放器 Action 前，需要先发布此仓库的首个稳定版本。不存在发行包或校验失败时构建失败，不下载其它仓库或使用内置 OSZ 解析回退。

## 接口与测试

ABI 头文件：`include/ttp_osu/abi.h`。使用 UTF-16 路径、只读借用缓冲区和同步回调；无 STL 对象、内存释放权或 C++ 异常跨 DLL 边界。每次调用的状态独立，可并发调用；宿主负责其 I/O 回调的线程安全。

私有测试放在工作区 `rebuild/tests/archive_support/`，不放入本仓库，不上传，不在 Action 中运行。CI 的静态导入与打包校验属于构建验证。

## 许可证

- [本项目 MIT](LICENSE)
- [VC-LTL](docs/licenses/VC-LTL-LICENSE.txt)
- [YY-Thunks](docs/licenses/YY-Thunks-LICENSE.txt)
