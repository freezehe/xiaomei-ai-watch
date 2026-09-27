# 双系统：把小智固件烧到 ota_1，把小智资源烧到 assets 分区
# 桌面（本工程）照常用 `idf.py -p COM4 flash` 烧到 ota_0，互不覆盖
#
# 分区布局见 partitions.csv:
#   0x020000 ota_0  = Brookesia 桌面（默认启动）
#   0x410000 ota_1  = XiaoZhi
#   0x800000 assets = XiaoZhi 资源（字体/表情/唤醒词/皮肤）
#
# 用法:
#   .\flash_xiaozhi.ps1                 # 烧 app + assets
#   .\flash_xiaozhi.ps1 -AppOnly        # 只改了代码时，只烧 app（更快）
#   .\flash_xiaozhi.ps1 -Assets D:\my_assets.bin   # 换皮肤：烧自定义 assets
param(
    [string]$Port = "COM4",
    [string]$XiaozhiBuild = (Join-Path $PSScriptRoot "..\..\..\xiaozhi-esp32\build"),  # xiaozhi-esp32 cloned next to this repo
    [string]$Assets = "",
    [switch]$AppOnly
)
$ErrorActionPreference = "Stop"

$python = "C:\Espressif\tools\python\v6.1\venv\Scripts\python.exe"
$app = Join-Path $XiaozhiBuild "xiaozhi.bin"
if (-not $Assets) { $Assets = Join-Path $XiaozhiBuild "generated_assets.bin" }

if (-not (Test-Path $app)) { throw "找不到 $app，请先编译小智 (scripts/build.py)" }
if ((Get-Item $app).Length -gt 0x3f0000) { throw "xiaozhi.bin 超过 ota_1 分区大小 (0x3f0000)" }

$args_ = @("-m", "esptool", "--chip", "esp32s3", "-p", $Port, "write-flash", "0x410000", $app)
if (-not $AppOnly) {
    if (-not (Test-Path $Assets)) { throw "找不到 assets 文件 $Assets" }
    if ((Get-Item $Assets).Length -gt 0x800000) { throw "assets 超过 8MB 分区大小" }
    $args_ += @("0x800000", $Assets)
}

Write-Host "烧录: $($args_[7..($args_.Count-1)] -join ' ')"
& $python @args_
if ($LASTEXITCODE -ne 0) { throw "esptool 烧录失败 (exit $LASTEXITCODE)" }
Write-Host "完成。开机进入桌面，点 'AI Chat' 进入小智；小智里长按 BOOT 或说'回到桌面'返回。"
