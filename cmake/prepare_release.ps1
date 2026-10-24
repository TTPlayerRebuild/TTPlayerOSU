[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$Version,
    [string]$PreviousVersion,
    [Parameter(Mandatory = $true)][string]$Repository,
    [Parameter(Mandatory = $true)][string]$Commit,
    [string]$ArtifactDirectory = 'artifact',
    [string]$ServerUrl = 'https://github.com'
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'version.ps1')
$version = (Get-OsuBuildVersion $Version).Name
if ($PreviousVersion -and -not (Read-OsuVersionTag $PreviousVersion)) { throw 'Invalid previous release version.' }
if ($Repository -notmatch '^[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+$' -or $Commit -notmatch '^[0-9a-fA-F]{40}$') {
    throw 'Invalid repository or commit.'
}
$archiveName = "ttp_osu-$version.zip"
$hash = (Get-FileHash -LiteralPath (Join-Path $ArtifactDirectory $archiveName) -Algorithm SHA256).Hash.ToLowerInvariant()
$expected = (Get-Content -LiteralPath (Join-Path $ArtifactDirectory 'SHA256SUMS.txt') -Encoding UTF8 -Raw).Trim()
if ($expected -cne "$hash  $archiveName") { throw 'OSU package SHA-256 verification failed.' }
$repoUrl = "$ServerUrl/$Repository"
$logUrl = if ($PreviousVersion) { "$repoUrl/compare/$PreviousVersion...$version" } else { "$repoUrl/commits/$version" }
$notes = @"
**更新记录**: $logUrl

解压 ttp_osu-$version.zip，关闭播放器后，将 AddIn/ttp_osu.dll 放入安装目录。
提供 OSZ 压缩包解析、包内枚举、多音频去重、引用路径校验及逐曲目背景封面。
需配合已接入 OSU ABI 1 的重建版；原版播放器不会调用此扩展接口。
同一 x86 DLL 面向 XP SP3、Win7 及新系统，CPU 需要 SSE2。
音频解码、ZIP 流、密码交互和缓存由播放器提供；压缩包保持只读。
ZIP 只包含 AddIn/ttp_osu.dll 和 SHA256SUMS.txt，许可证保留在源码仓库。

[源码、兼容范围及验证记录]($repoUrl/tree/$Commit)
[VC-LTL 许可证]($repoUrl/blob/$Commit/docs/licenses/VC-LTL-LICENSE.txt)
[YY-Thunks 许可证]($repoUrl/blob/$Commit/docs/licenses/YY-Thunks-LICENSE.txt)
"@
$notes | Set-Content -LiteralPath (Join-Path $ArtifactDirectory 'release-notes.md') -Encoding UTF8
