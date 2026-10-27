# 安装信息接口与宿主逻辑迁移

## 1. 所属边界

本次将重建版 EXE 内的 exe 特征识别、默认安装路径发现和 lazer 存储解析迁入可选 `ttp_osu.dll`。

| DLL 负责 | 播放器负责 |
| --- | --- |
| PE32/PE32+、版本资源交叉识别 | 设置窗口和文件/目录浏览器 |
| Windows 用户目录及默认程序位置 | 配置文件保存，保留用户原始路径 |
| stable 的 `BeatmapDirectory`、Songs 位置 | 显示识别结果、合并刷新和菜单可见性 |
| lazer 便携标志、`storage.ini`、files 位置 | 向插件提供 PCM、窗口、鼠标、时钟及通用 I/O |
| OSZ、谱面、歌曲库和两种星环效果 | 歌曲播放、列表展示和音频解码插件调度 |

宿主删除 `src/settings/osu_executable_identity.h` 和 `osu_storage_path.h`。
宿主剩下的环境变量展开是通用配置路径操作，不包含游戏目录、PE 身份规则或存储文件语法。

## 2. 新接口

增加 `ttp_osu_installation_query`，固定序号 **4**，安装信息 **ABI 1**。
既有序号 1（OSZ）、2（视觉 ABI 1–5）、3（歌曲库 ABI 1）及结构布局不变。

- `identify(path, kind)`：只读检查程序，输出 none / stable / lazer。识别成功返回 `S_OK`，不存在或未识别返回 `S_FALSE`。不加载、不执行待识别 exe。
- `discover(request, visitor, context)`：同步返回程序位置、歌曲目录及程序身份。字符串只在回调内有效，由宿主自行复制，不跨 DLL 传递 STL 或释放权。
- `executable == NULL` 请求默认位置；`L""` 保留显式空值。`roaming_root == NULL` 使用当前用户漫游目录，可显式传入其他根目录。
- 非法来源、flags、结构大小、过长输入返回错误；不改调用方结构尾部；传回回调的错误码。

## 3. 路径规则统一

默认目录由 `SHGetFolderPathW` 提供，适用于 XP、Win7 与现代 Windows 的实际用户目录。stable 默认 `osu!\osu!.exe`；lazer 优先 `osulazer\current\osu!.exe`，该入口缺失而根入口存在时使用 `osulazer\osu!.exe`。

stable 的路径发现与实际枚举共用既有配置读取函数。发现阶段保留离线的自定义 `BeatmapDirectory`，不因磁盘暂未挂载而改回默认目录；真正枚举时仍返回读盘错误。

lazer 只保留一套存储解析：

1. 默认使用当前用户漫游目录中的 `osu`。
2. 程序同目录存在 `framework.ini` 时，使用程序目录作为便携数据根。
3. 读取该根中的 `storage.ini`；有效绝对 `FullPath` 替换数据根，歌曲位置为其 `files` 子目录。
4. 缺失或损坏的配置保留当前数据根；便携模式回到便携根，不跳到漫游目录。

统一支持 UTF-8/BOM、UTF-16 LE/BE、最后一项同名键、空白和路径中的等号；限制为 64 KiB，拒绝相对/非法路径。修正了旧 DLL 与宿主分别采用首项/末项键、编码支持和便携回退不同的问题。操作全程只读，不建立 Realm 锁、不迁移或修改游戏配置。

## 4. 兼容与配置

- **旧宿主＋新 DLL**：原接口仍有效，旧视觉设置宿主回归通过。
- **新宿主＋旧 DLL**：OSZ/歌曲库原能力保留；没有安装信息接口时不在 EXE 中复制识别实现，星环验证不通过，设置页提示更新 DLL。
- **没有 DLL**：相关功能隐藏。不能发现默认路径时，不把空默认值写成用户主动清空的配置。
- 宿主记录路径属性是否由 XML 或用户明确提供，保留原路径、环境变量和显式空值；此状态仅用于保存语义，不增加 XML 属性。
- 默认发现仅在播放器初始化/选项刷新时调用。配置构造、读取和保存不加载本 DLL，避免更新器或后台工作进程占用待替换插件。
- 插件仍为可选组件，安装/替换后重启播放器；主程序 ZIP 不强制包含它。

## 5. 验证及发行

- DLL 实际调用的 36 项身份识别测试通过，包括真实 stable、x64 lazer、改名文件、损坏 PE 和字段篡改。
- Windows 11 的 59 项 DLL 接口/路径/配置检查通过，包含原视觉 ABI 1–5、原歌曲库 ABI、stable 实际枚举及既有 Realm 三曲目样本。
- XP、Win7 各通过 58 项接口/路径检查（未包含该 Realm 样本），包括用 x86 插件识别 x64 lazer。
- Windows 11、XP、Win7 的媒体库页面、动态视觉可用条件、配置保存、便携路径测试通过。
- 旧 DLL、无 DLL、新 DLL 配旧宿主的隔离目录测试通过。
- DLL 和播放器的 XP/Win7 静态导入检查通过；DLL 新增依赖仅为系统 `version.dll`，没有新的第三方库。
- 下载及 ExtraAddIn 打包校验接纳精确的第 4 个导出，仍检查摘要、版本和旧系统导入。

私有测试位于 `rebuild/tests/osu_installation_20261027`、`osu_identity_20261027` 及既有 UI 测试中，不进入仓库发行包和 Action 测试。游戏样本不分发。

输出：`build/Release/ttp_osu.dll`、`ttp_osu-2026.10.27.zip`。插件 ZIP 仍只含 `AddIn/ttp_osu.dll` 与 `SHA256SUMS.txt`；许可证保留在仓库。
