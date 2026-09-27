# 把本目录的板级代码和补丁应用到 xiaozhi-esp32 源码（https://github.com/78/xiaozhi-esp32，v2.5.0 验证通过）
# 用法：.\apply_to_xiaozhi.ps1 [-XiaozhiRoot <xiaozhi-esp32 目录>]
#   默认 xiaozhi-esp32 与本仓库放在同一级目录
param(
    [string]$XiaozhiRoot = (Join-Path $PSScriptRoot "..\..\..\xiaozhi-esp32")
)
$ErrorActionPreference = "Stop"

$XiaozhiRoot = (Resolve-Path $XiaozhiRoot).Path
$board = Join-Path $XiaozhiRoot "main\boards\waveshare\esp32-s3-touch-amoled-1.75"
if (-not (Test-Path $board)) { throw "找不到 $board，请确认 -XiaozhiRoot 指向 xiaozhi-esp32 源码" }

# 1. 板级代码：专业聊天界面、双系统切换、熄屏、唤醒词「小美同学」
Copy-Item -Force (Join-Path $PSScriptRoot "esp32-s3-touch-amoled-1.75\*") $board
Write-Host "已复制板级代码到 $board"

# 2. OTA 补丁：双系统下不覆盖另一个分区里的桌面固件
git -C $XiaozhiRoot apply --whitespace=nowarn (Join-Path $PSScriptRoot "patches\ota-dual-boot-guard.patch")
Write-Host "已应用 patches\ota-dual-boot-guard.patch"
Write-Host "下一步：在 xiaozhi-esp32 目录执行  python scripts/build.py esp32-s3-touch-amoled-1.75 --name esp32-s3-touch-amoled-1.75c"
