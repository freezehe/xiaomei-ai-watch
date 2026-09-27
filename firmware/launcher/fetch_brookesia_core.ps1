# 下载 ESP-Brookesia 核心组件（brookesia_core，约 43MB，乐鑫 / 微雪上游代码，本仓库未修改，所以不重复上传）
# 用法：在本目录执行  .\fetch_brookesia_core.ps1
# 开发时基于微雪仓库提交 6d19f7e 验证；如上游改动导致编译失败，可用 -Ref 6d19f7e 固定版本
param(
    [string]$Ref = "main"
)
$ErrorActionPreference = "Stop"

$dst = Join-Path $PSScriptRoot "components\brookesia_core"
if (Test-Path $dst) {
    Write-Host "brookesia_core 已存在：$dst"
    exit 0
}

$tmp = Join-Path $env:TEMP ("brookesia_core_" + [guid]::NewGuid().ToString("N"))
$sub = "examples/esp-idf/03_esp-brookesia/components/brookesia_core"
git clone --filter=blob:none --no-checkout https://github.com/waveshareteam/ESP32-S3-Touch-AMOLED-1.75C.git $tmp
git -C $tmp sparse-checkout set $sub
git -C $tmp checkout $Ref
Copy-Item -Recurse (Join-Path $tmp $sub) $dst
Remove-Item -Recurse -Force $tmp
Write-Host "完成：$dst"
