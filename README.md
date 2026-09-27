# 小美同学 · AI 圆屏手表

把微雪 **ESP32-S3-Touch-AMOLED-1.75C**（1.75 英寸 466×466 圆形 AMOLED）做成一块能聊天的 AI 手表。

- **桌面系统**：基于 ESP-Brookesia，包含时钟、音乐、设置、AI Chat 四个应用
- **AI 助手**：基于小智 xiaozhi-esp32，唤醒词「小美同学」，为圆屏重新设计了聊天界面
- **云端**：xiaozhi-esp32-server 部署在阿里云，使用豆包大模型、火山流式语音识别和合成，并自研了股票行情插件

> 一块板子装两套固件：桌面在 `ota_0`，小智在 `ota_1`，通过切换启动分区互相跳转；两者共享同一份 Wi-Fi 配置。

## 功能一览

| 应用 | 功能 | 技术点 |
|---|---|---|
| 时钟 | 数字 / 指针双表盘、农历（2000–2099） | 板上没有 RTC 芯片：SNTP 对时 + 每 10 分钟备份到 NVS |
| 音乐 | 在线播放服务器上的 MP3，退出应用后继续播放 | HTTP 流式下载 → 256 KB PSRAM 环形缓冲 → esp_audio_codec 解码 |
| 设置 | 扫描、选择 Wi-Fi，忘记已保存的网络 | 与小智 SsidManager 完全兼容的 NVS 格式；单任务状态机 |
| AI Chat | 本地唤醒「小美同学」、语音对话、查天气 / 股票 / 新闻 | WakeNet9、Opus + WebSocket、Function Call、MCP 工具 |

## 系统架构

```
┌──────────── 设备端 ESP32-S3 ────────────┐
│ Brookesia 桌面 (ota_0) │ XiaoZhi AI (ota_1) │   共享 NVS：Wi-Fi / 服务器地址
└──────────────┬──────────────────────────┘
               │ HTTP（OTA 取地址和令牌）+ WebSocket（Opus 音频 / JSON 消息）
┌──────────────▼────────────── 接入层 ─────┐
│ Nginx：OTA 接口藏在随机路径后 · WS 转发 · Token 鉴权 │
└──────────────┬──────────────────────────┘
┌──────────────▼──── xiaozhi-esp32-server（Docker）──┐
│ Silero VAD · ASR · LLM · Function Call 插件 · TTS · MCP │
└──────────────┬──────────────────────────┘
               ▼
  火山流式 ASR · 豆包 Seed 2.1 Lite · 火山双向流式 TTS · 天气 / 行情 / 新闻
```

完整的链路讲解见 [docs/article.html](docs/article.html)。

## 目录结构

```
firmware/
├─ launcher/                 桌面固件（ESP-IDF 工程，烧到 ota_0）
│  ├─ components/
│  │  ├─ brookesia_sys/          Wi-Fi 状态机、SNTP 时间、电量
│  │  ├─ brookesia_app_clock/    时钟（含农历）
│  │  ├─ brookesia_app_music/    网络音乐
│  │  ├─ brookesia_app_settings/ Wi-Fi 设置
│  │  ├─ brookesia_app_xiaozhi/  切换到 AI 固件
│  │  ├─ brookesia_lv_mem/       LVGL 内存放到 PSRAM
│  │  └─ brookesia_screen_sleep/ 自动熄屏
│  ├─ fetch_brookesia_core.ps1   下载上游 brookesia_core 组件
│  └─ flash_xiaozhi.ps1          把小智固件和资源烧到 ota_1 / assets
└─ xiaozhi-board/            小智的板级代码（覆盖到 xiaozhi-esp32 源码中）
   ├─ esp32-s3-touch-amoled-1.75/   专业聊天界面 pro_chat_display.cc、双系统、唤醒词配置
   ├─ patches/                      OTA 双系统保护补丁
   └─ apply_to_xiaozhi.ps1
server/
└─ custom-plugins/           服务端插件：A 股 / 指数行情（腾讯财经免费接口）
tools/
└─ xiaozhi-skin/             生成圆屏背景皮肤并打包进小智资源分区
docs/
└─ article.html              项目介绍文章（架构、链路、踩坑记录）
```

## 编译与烧录

环境：ESP-IDF **v6.1**；目录约定为本仓库与 `xiaozhi-esp32` 放在同一级。

### 1. 桌面固件（ota_0）

```powershell
cd firmware/launcher
.\fetch_brookesia_core.ps1           # 首次：下载上游 brookesia_core
idf.py set-target esp32s3
idf.py build
idf.py -p COM4 flash                 # 分区表、桌面固件和 otadata
```

### 2. AI 固件（ota_1 + assets）

```powershell
git clone https://github.com/78/xiaozhi-esp32.git   # 与本仓库同级
.\firmware\xiaozhi-board\apply_to_xiaozhi.ps1
cd ..\xiaozhi-esp32
python scripts/build.py esp32-s3-touch-amoled-1.75 --name esp32-s3-touch-amoled-1.75c
cd ..\xiaomei-ai-watch\firmware\launcher
.\flash_xiaozhi.ps1                  # 烧录 ota_1 与 assets
```

可选：用 `tools/xiaozhi-skin/make_skin.py pack --light nebula --dark aurora` 生成带背景皮肤的资源包，再用 `flash_xiaozhi.ps1 -Assets <文件>` 烧录。

### 3. 服务端

参考 [xiaozhi-esp32-server](https://github.com/xinnan-tech/xiaozhi-esp32-server) 的部署文档。自定义插件挂载到容器的 `plugins_func/functions/` 目录，并在配置的 `Intent.function_call.functions` 中加入 `get_stock_quote`。

> ⚠️ 所有 API Key（大模型、语音、天气等）只放在服务器的 `data/.config.yaml` 中，**不要提交到仓库**；`.gitignore` 已忽略该文件。

## 踩坑记录（节选）

- **PSRAM 栈 + 写 Flash = 断言**：LVGL 任务栈在 PSRAM 上，读写 NVS、切换启动分区必须放到内部 RAM 栈的任务里执行。
- **内部 RAM 被 LVGL 耗尽**：`CONFIG_LV_USE_CLIB_MALLOC` 配合 `SPIRAM_MALLOC_ALWAYSINTERNAL=4096`，会把所有 UI 对象放进内部 RAM，导致音乐播放时屏幕 SPI 申请不到缓冲而"卡死"。解决办法见 `brookesia_lv_mem`。
- **共享 NVS 命名空间**：Wi-Fi 列表和小智的 `ota_url` 在同一个命名空间里，只能逐个键改写，不能整体清空。

## 致谢

- [78/xiaozhi-esp32](https://github.com/78/xiaozhi-esp32)（MIT）
- [espressif/esp-brookesia](https://github.com/espressif/esp-brookesia)（Apache-2.0）
- [waveshareteam/ESP32-S3-Touch-AMOLED-1.75C](https://github.com/waveshareteam/ESP32-S3-Touch-AMOLED-1.75C)
- [xinnan-tech/xiaozhi-esp32-server](https://github.com/xinnan-tech/xiaozhi-esp32-server)（MIT）
- [espressif/esp-sr](https://github.com/espressif/esp-sr) · [lvgl/lvgl](https://github.com/lvgl/lvgl)
