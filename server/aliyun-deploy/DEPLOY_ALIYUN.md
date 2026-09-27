# 小智服务端部署到阿里云 ECS（<你的服务器IP>）完整手册

> 适用于你这台服务器：CentOS 7.9 / 2 核（经济型 e）/ 3.6GB 内存 / 40GB ESSD / 3Mbps / SSH 端口自定义 / 已有 Nginx（`/usr/local/nginx`）。
> 方案：**Docker + 云端语音识别（豆包流式 ASR）+ 豆包 Seed 2.1 Lite + 台湾女声**，Java 服务不用停。

```
板子 ──(手机热点 / 任意 WiFi)──> 公网 ──> <你的服务器IP>:8888 （已有 Nginx）
                                          ├─ /<SECRET>/xiaozhi/ota/ → 127.0.0.1:8003  OTA（隐藏路径）
                                          └─ /xiaozhi/v1/           → 127.0.0.1:8000  WebSocket（token 认证）
                                                        │
                                              Docker 容器 xiaozhi-esp32-server（≤800MB）
                                                        ├─ ASR：豆包流式语音识别（火山语音）
                                                        ├─ LLM：豆包 doubao-seed-2-1-lite-260915（方舟）
                                                        └─ TTS：EdgeTTS 台湾女声 / 湾湾小何（火山语音）
```

---

## ⚡ 快速部署（推荐：脚本自动完成第 4~10 步）

1. 阿里云控制台：安全组入方向放行 **TCP 8888**（第 2 节）。
2. 电脑上传整个目录（PowerShell，提示输入登录用户的密码）：
   ```powershell
   scp -P <SSH端口> -r <本仓库>\server\aliyun-deploy <你的用户名>@<你的服务器IP>:~/
   ```
   （也可以用 Xshell 自带的 Xftp 把 `aliyun-deploy` 文件夹拖到 `/home/<你的用户名>/`）
3. Xshell 登录后：
   ```bash
   su - root
   bash /home/<你的用户名>/aliyun-deploy/1_prepare_server.sh 2>&1 | tee /root/prepare.log   # swap + Docker + nginx include
   bash /home/<你的用户名>/aliyun-deploy/2_deploy.sh        2>&1 | tee /root/deploy.log    # 放文件 + 密钥 + 启动 + 自检
   ```
4. `2_deploy.sh` 最后会打印板子要填的 OTA 地址 → 按第 11 节配置板子。

两个脚本都可以重复执行（已完成的步骤会跳过），每一步都会打印 `[OK]/[WARN]/[FAIL]`。下面第 0~14 节是脚本背后每一步的详细说明，出问题时对照排查。

---

## 0. 评估结论（为什么这么部署）

| 问题 | 结论 | 依据 |
|---|---|---|
| 要不要装 Docker？ | **要**。不用现有 conda 直接跑 | CentOS 7 的 glibc 是 2.17，服务端依赖的新版 onnxruntime 等二进制包要求更高的 glibc，pip 装不上或会装到旧版；Docker 镜像自带运行环境，也不会动你现有的 conda 和 Java |
| 内存够不够？ | **够**（云端 ASR） | 本机实测：云端识别+湾湾小何的服务端空闲常驻 **135MB**；估算合计：系统 ~350MB + Docker ~100MB + 容器 150~400MB ≈ 0.6~0.9GB，Java 不用停 |
| 为什么不用本地 FunASR？ | 装不下 | 本机实测 FunASR 版常驻 3.1GB，官方要求 2 核 4G |
| 带宽 3Mbps 够不够？ | 够 | 语音用 Opus 编码，单路约 16~24kbps |
| 要不要域名/备案？ | 不需要 | 板子直接访问 `IP:8888`；只有用域名走 80/443 才要备案 |

> 部署后第 9 步会用 `docker stats` 和 `free -h` 实测，确认真实占用。

---

## 1. 开通云服务（浏览器里操作）

### 1.1 火山引擎语音：语音识别（必须）+ 湾湾小何（可选）

**应用 ≠ 服务**：控制台里"创建应用"只会得到 APP ID / Access Token；还必须**为这个应用开通具体服务**，否则调用会被拒（HTTP 403 `requested resource not granted`）。

| 步骤 | 操作 | 说明 |
|---|---|---|
| ① 创建应用 | 豆包语音控制台（旧版）→ 应用管理 → 创建应用 | 记下 APP ID 和 Access Token，填入 `data/.config.yaml`（注意：默认的 `default` 应用可能没开通服务） |
| ② 开通语音识别 | 左侧 **"流式语音识别大模型"** → 选择刚建的应用 → **小时版** → 开通 | 新账号一般有免费试用包（小时数 / 并发数以控制台为准） |
| ③ 湾湾小何 | 左侧 **"语音合成大模型"** | 需要授权该音色；实测首包约 0.58s |

**实测结果（2026-09-25）**

| 项目 | 结果 |
|---|---|
| 识别准确率 | 普通话、台湾腔女声测试句都能正确识别，标点也对（"欸"偶尔识别成"给"） |
| 识别尾延迟 | 说完后约 **0.1s** 出结果（1.0 和 2.0 差不多） |
| 湾湾小何 | 首包 **0.58s**，5.4s 语音总共合成 1.78s |
| 服务端内存（云端识别+湾湾小何） | 空闲常驻 **135MB**（本地 FunASR 版是 3.1GB） |
| `end_window_size` | **必须调到 500**：默认 200 会把"帮我把音量调到60，然后讲个笑话"切成两句，自动模式下服务端只取第一句，**后半句丢失**；500 能保留整句，只多等约 0.3s |

**试用额度够不够用**

- 计费时长怎么算：服务端**检测到有人说话才会连接识别服务**，一句话结束就断开；唤醒词检测在板子本地完成，**待机不产生用量**。每轮计费时长 ≈ 说话时长 + 约 1.1s（0.6s 预缓存 + 0.5s 句尾判定）。
- 估算：每轮按 4~5 秒计，20 小时 ≈ **1.4 万~1.8 万轮对话**。每天聊 50 轮，也要 300 多天才用完，而试用包 **2027-03-25** 就到期了（约 6 个月），所以**个人学习完全够用，大概率到期前都用不完**。
- 并发 3：一块板子同一时间只占 1 路，够用。
- 会多用额度的情况：环境很吵导致误触发语音检测、开着"实时对话"模式长时间聆听。可以在控制台"预警配置"里设一个用量告警（比如用到 15 小时就提醒）。
- 湾湾小何的试用额度（按字数计）请在"语音合成大模型"页面查看。每次回复约 30~60 字，一般试用额度能支撑几百到几千轮，**用完前记得看一眼余量**。

**判断是否开通成功**：步骤 ② 页面的"服务详情"表格里应该出现一行服务（版本、实例 ID、用量限额），"服务开通"一列为已开通。**表格为空 = 没开通**。

**用脚本验证**（在你的电脑上即可，只做握手，不产生费用）：

```powershell
cd <本仓库>\server\aliyun-deploy
python verify_volc.py   # 需要先按上面的说明准备好 data\.config.yaml
```

```
流式语音识别 1.0（小时版）  volc.bigasr.sauc.duration   ✅ 已授权        ← 看到这个才算开通成功
流式语音识别 2.0（小时版）  volc.seedasr.sauc.duration  ❌ HTTP 403 ...  ← 只开通 1.0 时这行 403 是正常的
语音合成大模型（湾湾小何）    volc.service_type.10029     ❌ HTTP 403 ...  ← 没买湾湾小何时正常
```

- 1.0 或 2.0 **任意一个 ✅** 即可：如果只有 **2.0** 是 ✅，把 `.config.yaml` 里 `ASR.DoubaoStreamASR.resource_id` 改成 `volc.seedasr.sauc.duration`。
- 两个都 403：服务没开通，或开通时选的不是 `default` 这个应用。
- 刚开通/购买后可能要等几分钟到半小时才生效，过会儿再跑一次脚本。

> ⚠️ 火山**语音**的 appid/token 和火山**方舟**的 `ark-...` Key 是两套东西，不能混用。
> ⚠️ 截图顶部提示"新版控制台使用 API Key 接入"：本服务端目前用的是**旧版 APP ID + Access Token** 方式，请继续在**旧版**控制台操作。

### 1.2 火山方舟：大模型（已完成）

你已开通，API Key 已验证可用。实测结论（2026-09-25，你的账号）：

| 模型 | 首字延迟 | 工具调用 | 说明 |
|---|---|---|---|
| `doubao-seed-2-1-lite-260915` | ~0.87s | ✅ | **默认**，最新 2.1 |
| `doubao-seed-character-260628` | ~0.34s | ✅ | 角色扮演，更快、更像聊天伙伴 |
| `doubao-seed-2-0-lite-260215` | — | — | **404**，你账号访问不到这个版本（能用的是 `260428`） |

建议在方舟控制台 **费用 → 预算/告警** 里设一个每月上限。

---

## 2. 阿里云安全组（控制台操作）

ECS 实例 → **网络与安全组 → 安全组 → 配置规则 → 入方向**：

| 动作 | 协议端口 | 授权对象 | 说明 |
|---|---|---|---|
| 添加 | TCP **8888** | 0.0.0.0/0 | 小智唯一入口 |
| **修改** | TCP <SSH端口>（SSH） | **你自己的公网 IP/32** | 公网 SSH 端口经常被暴力破解，强烈建议只放行自己的 IP |

**不要**放行 8000、8003。

---

## 3. 登录服务器并切换 root

```bash
ssh -p <SSH端口> <你的用户名>@<你的服务器IP>
su - root
```

后续命令都以 root 执行。

---

## 4. 加 2GB swap（内存兜底，防止 OOM 误杀 Java）

```bash
free -h                                   # 先确认 Swap 是 0B
dd if=/dev/zero of=/swapfile bs=1M count=2048
chmod 600 /swapfile
mkswap /swapfile
swapon /swapfile
echo '/swapfile none swap sw 0 0' >> /etc/fstab
sysctl vm.swappiness=10                   # 尽量少用 swap，只在紧张时兜底
echo 'vm.swappiness=10' >> /etc/sysctl.conf
free -h                                   # Swap 应显示 2.0G
```

---

## 5. 安装 Docker（CentOS 7）

CentOS 7 已停止维护，Docker 官方支持的最后一个版本是 26.1.x，用阿里云镜像源安装：

```bash
yum install -y yum-utils
yum-config-manager --add-repo https://mirrors.aliyun.com/docker-ce/linux/centos/docker-ce.repo
yum install -y docker-ce docker-ce-cli containerd.io docker-compose-plugin
```

> 如果 `yum` 报 `Could not retrieve mirrorlist` / 404：说明系统源还指向已下线的官方地址，改用阿里云的 vault 源：
> ```bash
> sed -i -e 's|^mirrorlist=|#mirrorlist=|g' \
>        -e 's|^#baseurl=http://mirror.centos.org|baseurl=https://mirrors.aliyun.com/centos-vault|g' \
>        /etc/yum.repos.d/CentOS-*.repo
> yum clean all && yum makecache
> ```
> 然后重新执行上面的安装命令。

配置 Docker（日志限制 + 镜像加速），启动并设置开机自启：

```bash
mkdir -p /etc/docker
cat > /etc/docker/daemon.json <<'EOF'
{
  "log-driver": "json-file",
  "log-opts": { "max-size": "20m", "max-file": "3" }
}
EOF
systemctl enable --now docker
docker version            # Server 版本应显示 26.x
docker compose version    # 应显示 v2.x
```

> 镜像地址 `ghcr.nju.edu.cn`（南京大学镜像）在国内可直接拉取，一般不需要额外加速器。

---

## 6. 上传部署文件

### 6.1 在服务器上建目录（root）

```bash
mkdir -p /root/xiaozhi-server/data /usr/local/nginx/conf/conf.d
```

### 6.2 在你的电脑上传（PowerShell）

root 一般不能直接 SSH 登录，所以先传到登录用户的家目录：

```powershell
cd <本仓库>\server\aliyun-deploy
scp -P <SSH端口> docker-compose.yml nginx-xiaozhi.conf init_secrets.sh <你的用户名>@<你的服务器IP>:~/
# 先复制示例配置：copy data\.config.example.yaml data\.config.yaml，再填入你自己的 appid/token/Key（该文件已被 .gitignore 忽略）
# 方舟 Key 等其余占位符也可以到第 7.2 步在服务器上填
scp -P <SSH端口> data\.config.yaml <你的用户名>@<你的服务器IP>:~/xiaozhi.config.yaml
```

### 6.3 在服务器上放到正确位置（root）

```bash
mv /home/<你的用户名>/docker-compose.yml   /root/xiaozhi-server/
mv /home/<你的用户名>/init_secrets.sh      /root/xiaozhi-server/
mv /home/<你的用户名>/xiaozhi.config.yaml  /root/xiaozhi-server/data/.config.yaml
mv /home/<你的用户名>/nginx-xiaozhi.conf   /usr/local/nginx/conf/conf.d/xiaozhi.conf
chown -R root:root /root/xiaozhi-server /usr/local/nginx/conf/conf.d
sed -i 's/\r$//' /root/xiaozhi-server/init_secrets.sh   # 去掉 Windows 换行符
```

---

## 7. 生成密钥、填写 Key

### 7.1 自动生成 OTA 隐藏路径和 token 密钥

```bash
bash /root/xiaozhi-server/init_secrets.sh
```

会输出类似：

```
完成。板子上要填的 OTA 地址（也保存在 /root/xiaozhi-server/ota_url.txt）：
  http://<你的服务器IP>:8888/3f9c0a...e1/xiaozhi/ota/
```

**把这个地址记下来**（第 10 步要填到板子上）。

### 7.2 填写 API Key

```bash
vi /root/xiaozhi-server/data/.config.yaml
```

需要替换的占位符：

| 占位符 | 填什么 | 出现次数 |
|---|---|---|
| 占位符 | 状态 |
|---|---|
| 火山语音 appid / access_token（ASR + 湾湾小何） | 按第 3 节在火山语音控制台获取后填入 |
| 方舟 API Key（DoubaoLLM / DoubaoCharacterLLM） | ✅ 已在本地填好 |
| DeepSeek API Key | ✅ 已在本地填好 |
| `<百炼 API Key>` | 不用百炼就不填 |

> 所以这一步通常**不用改**，只需确认一下。上传的 `.config.yaml` 里有所有 Key，第 6.3 步之后务必 `chmod 600`（`init_secrets.sh` 会自动做）。

确认没有漏填：

```bash
grep -n '<' /root/xiaozhi-server/data/.config.yaml | grep -vE '百炼|DeepSeek'   # 应无输出
```

---

## 8. 配置 Nginx（你已有的 /usr/local/nginx）

### 8.1 让主配置加载 conf.d

```bash
grep -n 'conf.d' /usr/local/nginx/conf/nginx.conf
```

- **有输出**（已经 include 了）：跳过。
- **没有输出**：编辑主配置，在 `http {` 块里（比如 `include mime.types;` 那一行下面）加一行：

```bash
vi /usr/local/nginx/conf/nginx.conf
```

```nginx
    include conf.d/*.conf;
```

### 8.2 确认 8888 端口没被占用，检查并重载

```bash
ss -lntp | grep ':8888 '                   # 应无输出
/usr/local/nginx/sbin/nginx -t             # 应显示 syntax is ok / test is successful
/usr/local/nginx/sbin/nginx -s reload
ss -lntp | grep ':8888 '                   # 现在应看到 nginx 在监听
```

> 你之前试的 `--version`、`version`、`help` 都不是 nginx 的参数；常用的是：
> `-v` 看版本、`-V` 看编译参数、`-t` 测试配置、`-s reload` 重载、`-s stop` 停止。

---

## 9. 启动服务端

```bash
cd /root/xiaozhi-server
docker compose pull                 # 首次拉镜像，几 GB，需要几分钟
docker compose up -d
docker logs -f xiaozhi-esp32-server
```

看到以下几行就是启动成功（`Ctrl+C` 退出日志，不影响运行）：

```
初始化组件: asr成功 DoubaoStreamASR
初始化组件: llm成功 DoubaoLLM
Websocket地址是	ws://<你的服务器IP>:8888/xiaozhi/v1/
```

**实测内存**（请把结果发给我确认）：

```bash
docker stats --no-stream xiaozhi-esp32-server
free -h
```

---

## 10. 验证（在你的电脑上）

```powershell
# 1) 带隐藏路径：应返回 "OTA接口运行正常，向设备发送的websocket地址是：ws://<你的服务器IP>:8888/xiaozhi/v1/"
curl http://<你的服务器IP>:8888/<SECRET>/xiaozhi/ota/

# 2) 不带隐藏路径：应返回 404
curl -i http://<你的服务器IP>:8888/xiaozhi/ota/

# 3) 8003 不应能从公网访问：应超时/拒绝
curl -m 5 http://<你的服务器IP>:8003/xiaozhi/ota/
```

---

## 11. 板子配置（一次性）

1. 小智开机过程中**单击 BOOT** 进入配网模式（或对话中说"重新配网"并确认）。
2. 手机连热点 **`Xiaozhi-XXXX`（XXXX 为设备 MAC 后四位）**，浏览器打开 **http://192.168.4.1**。
3. **添加 WiFi**：家里 WiFi + **手机热点**都加上（固件会保存多个，自动连能连上的）。
   - iPhone：设置 → 个人热点 → 打开 **"最大兼容性"**（ESP32-S3 只支持 2.4GHz）。
   - 安卓：热点频段选 **2.4GHz**。
4. **高级选项 → OTA 地址**：填第 7.1 步的 `http://<你的服务器IP>:8888/<SECRET>/xiaozhi/ota/`
5. 保存 → 重启 → 说"你好小智"测试。
6. 服务器上看日志确认是阿里云在回答：`docker logs --tail 50 xiaozhi-esp32-server`

> 改回本地服务端：OTA 地址填 `http://<电脑局域网IP>:8003/xiaozhi/ota/` 即可。

---

## 12. 日常操作

| 操作 | 命令 / 做法 |
|---|---|
| 看实时日志 | `docker logs -f --tail 100 xiaozhi-esp32-server` |
| 改配置后生效 | `cd /root/xiaozhi-server && docker compose restart` |
| 切换大模型 | `.config.yaml` 里 `selected_module.LLM` 改成 `DoubaoLLM` / `DoubaoCharacterLLM` / `DeepSeekLLM` → restart |
| 换湾湾小何 | `selected_module.TTS` 改成 `HuoshanDoubleStreamTTS` → restart |
| 换 EdgeTTS 台湾女声 | `TTS.EdgeTTS.voice`：`zh-TW-HsiaoChenNeural`（甜美）/ `zh-TW-HsiaoYuNeural`（轻快） |
| 升级服务端 | `docker compose pull && docker compose up -d` |
| 看资源 | `docker stats --no-stream`、`free -h` |
| 停止 | `docker compose down` |

---

## 13. 安全说明

- **为什么要隐藏路径**：服务端的 OTA 接口会给任何调用者发放 WebSocket token（`core/api/ota_handler.py` 第 284~290 行），只开 `auth.enabled` 挡不住陌生人。所以 8000/8003 只绑定 127.0.0.1，公网只开 Nginx 8888，OTA 放在随机路径下。
- **SECRET 泄露了**：重新生成一个（手动 `openssl rand -hex 16` 替换 `/usr/local/nginx/conf/conf.d/xiaozhi.conf` 里的路径），`nginx -s reload`，板子 OTA 地址同步更新；同时换 `auth_key` 并 restart，旧 token 全部失效。
- **明文传输**：当前是 `ws://`，语音和 token 在公网未加密。学习用可以接受；要加密需域名 + 备案 + 证书，Nginx 改 443 + `wss://`。
- **日志里有 Access Token**：服务端连接 ASR 时会把请求头（含火山 Access Token）以 INFO 级别写进日志（`core/providers/asr/doubao_stream.py` 第 79 行）。所以 `docker logs` 的输出**不要原样发给别人或贴到网上**；需要求助时先删掉 `X-Api-Access-Key` 那一行。
- `data/bin/` 保持为空：服务端不会推送固件（双系统下我们也加了保护，固件不会覆盖桌面分区）。
- 各平台都设置**消费上限/告警**。

---

## 14. 常见问题

| 现象 | 排查 |
|---|---|
| `yum` 报 mirrorlist / 404 | 按第 5 步改用 centos-vault 源 |
| `docker compose pull` 很慢或失败 | 重试；或在阿里云"容器镜像服务 → 镜像加速器"拿专属地址，写进 `/etc/docker/daemon.json` 的 `registry-mirrors` 后 `systemctl restart docker` |
| `nginx -t` 报 `unknown directive "map"` 或 `include` 路径错误 | `include conf.d/*.conf;` 必须放在 `http { }` 里面；路径相对于 `/usr/local/nginx/conf/` |
| 板子提示"检查新版本失败" | 在电脑上 curl OTA 地址；安全组 8888 是否放行；SECRET 是否一致；地址末尾的 `/` 不能少 |
| OTA 正常但对话连不上 | `.config.yaml` 的 websocket 是否是 `<你的服务器IP>:8888`；`docker logs` 里有无 `Invalid token` |
| 日志报 ASR `403 requested resource not granted` | 凭证对但服务没开通/没绑定到该应用：按 1.1 开通，并用 `verify_volc.py` 验证 |
| 日志报 ASR 401 / invalid token | appid 或 access_token 填错，或误填了方舟 Key |
| 豆包 LLM 报 404 | 模型版本写错，用手册 1.2 表里实测可用的型号 |
| 湾湾小何报错 | 音色刚购买要等约半小时；appid/token 与 ASR 共用同一个应用 |
| 容器反复重启 | `docker logs` 看报错；`dmesg | grep -i oom` 看是否内存不足 |
| 回复慢 | 看 `docker logs` 里各阶段耗时；LLM 换 `DoubaoCharacterLLM` |
