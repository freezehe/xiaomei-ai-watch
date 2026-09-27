#!/bin/bash
# 第 3 步：增量更新（股票行情插件 + 语音点歌的音乐目录）
# 不会覆盖服务器上已生成的密钥（auth_key / OTA 隐藏路径）
# 用法（root）：bash /home/<你的用户名>/aliyun-deploy/3_update.sh <mp3所在目录> 2>&1 | tee /root/update.log
#   例：bash /home/<你的用户名>/aliyun-deploy/3_update.sh /data/music
set -uo pipefail

SRC=$(cd "$(dirname "$0")" && pwd)
BASE=/root/xiaozhi-server
CFG=$BASE/data/.config.yaml
MUSIC_DIR=${1:-}

step() { echo; echo "==================== $* ===================="; }
ok()   { echo "  [OK]   $*"; }
warn() { echo "  [WARN] $*"; }
fail() { echo "  [FAIL] $*"; exit 1; }

[ "$(id -u)" = 0 ] || fail "请先 su - root 再执行"
[ -f $CFG ] || fail "没找到 $CFG，请先完成 2_deploy.sh"

step "1. 音乐目录"
if [ -z "$MUSIC_DIR" ]; then
    fail "请在命令后面加上 mp3 所在目录，例如：bash $0 /data/music"
fi
MUSIC_DIR=$(readlink -f "$MUSIC_DIR")
[ -d "$MUSIC_DIR" ] || fail "目录不存在：$MUSIC_DIR"
n=$(find "$MUSIC_DIR" -type f \( -iname '*.mp3' -o -iname '*.wav' -o -iname '*.p3' \) | wc -l)
[ "$n" -gt 0 ] || fail "$MUSIC_DIR 下没有 mp3/wav 文件"
ok "$MUSIC_DIR 下共有 $n 首歌（含子目录）"
find "$MUSIC_DIR" -type f -iname '*.mp3' | head -5 | sed 's/^/         /'
echo "MUSIC_DIR=$MUSIC_DIR" > $BASE/.env
ok "已写入 $BASE/.env（docker compose 用它挂载音乐目录，只读）"

step "2. 更新 docker-compose.yml 和插件"
sed -i 's/\r$//' "$SRC"/docker-compose.yml "$SRC"/plugins/*.py
cp "$BASE/docker-compose.yml" "$BASE/docker-compose.yml.bak-$(date +%Y%m%d%H%M%S)"
cp "$SRC/docker-compose.yml" $BASE/
mkdir -p $BASE/plugins && cp "$SRC"/plugins/*.py $BASE/plugins/
ok "compose 已更新（旧版已备份），插件：$(ls $BASE/plugins | tr '\n' ' ')"

step "3. 更新 .config.yaml（插件列表 + 音乐目录）"
cp $CFG $CFG.bak-$(date +%Y%m%d%H%M%S)
insert_before_plugins() {   # $1 = 要插入的文本文件；插在第一行 "plugins:" 之前（没有就追加到末尾）
    if grep -q '^plugins:' $CFG; then
        awk 'FNR==NR {b = b $0 "\n"; next} /^plugins:/ && !d {printf "%s", b; d = 1} {print}' "$1" $CFG > $CFG.tmp && mv $CFG.tmp $CFG
    else
        { echo; cat "$1"; } >> $CFG
    fi
}
if grep -q 'get_stock_quote' $CFG; then
    ok "已包含 get_stock_quote，跳过"
elif grep -q '^Intent:' $CFG; then
    fail "$CFG 里已有 Intent 段，请手动在 functions 列表里加一行 '      - get_stock_quote' 后重跑"
else
    cat > /tmp/xz_intent.yaml <<'EOF'
Intent:
  function_call:
    type: function_call
    # 覆盖默认列表（列表会整体替换，所以要写全）；web_search 暂不启用（秘塔 Key 未配置）
    functions:
      - change_role
      - get_weather
      - get_news_from_newsnow
      - play_music
      - get_stock_quote

EOF
    insert_before_plugins /tmp/xz_intent.yaml
    ok "已加入插件列表（原文件已备份）"
fi
if grep -q '^  play_music:' $CFG; then
    ok "已有 play_music 配置，跳过"
elif grep -q '^plugins:' $CFG; then
    # 插在 "plugins:" 行之后，作为它的子项
    awk '{print} /^plugins:/ && !d {print "  play_music:\n    music_dir: \"./music\"   # 容器内路径，对应宿主机 MUSIC_DIR（见 .env）\n    refresh_time: 300"; d = 1}' $CFG > $CFG.tmp && mv $CFG.tmp $CFG
    ok "已加入 play_music 配置"
else
    printf 'plugins:\n  play_music:\n    music_dir: "./music"\n    refresh_time: 300\n' >> $CFG
    ok "已加入 plugins.play_music 配置"
fi
chmod 600 $CFG*

step "4. 重建容器"
cd $BASE
docker compose up -d --force-recreate || fail "重建容器失败"
for i in $(seq 1 40); do
    docker logs --since 2m xiaozhi-esp32-server 2>&1 | grep -q 'Websocket地址' && break
    sleep 3
done
docker logs --since 2m xiaozhi-esp32-server 2>&1 | grep -avE 'X-Api-Access-Key|api_key' | grep -aE '初始化组件|Websocket地址|ERROR|Traceback' | tail -8
docker logs --since 2m xiaozhi-esp32-server 2>&1 | grep -q 'Websocket地址' && ok "服务端已启动" || fail "启动失败，看上面的日志"

step "5. 自检"
docker exec xiaozhi-esp32-server python -c "
import asyncio, sys; sys.path.insert(0, '.')
from config.config_loader import load_config
c = asyncio.run(load_config())
print('  [OK]   配置生效：functions =', c['Intent']['function_call']['functions'], '| music_dir =', c['plugins']['play_music']['music_dir'])
" 2>/dev/null || fail "容器内读取配置失败（YAML 可能写坏了，备份在 $CFG.bak-*）"
docker exec xiaozhi-esp32-server sh -c 'ls plugins_func/functions/get_stock_quote.py' >/dev/null 2>&1 \
    && ok "容器内已有 get_stock_quote 插件" || fail "容器内找不到插件"
docker exec xiaozhi-esp32-server python -c "
import asyncio, sys; sys.path.insert(0, '.')
from plugins_func.functions.get_stock_quote import get_stock_quote
print('  [OK]   行情接口：', asyncio.run(get_stock_quote('上证指数')).result.splitlines()[1][:60])
" 2>/dev/null || warn "容器内行情接口调用失败（可能是服务器访问腾讯财经受限），请把输出发给 Claude"
m=$(docker exec xiaozhi-esp32-server sh -c "find music -type f \( -iname '*.mp3' -o -iname '*.wav' -o -iname '*.p3' \) | wc -l")
[ "$m" = "$n" ] && ok "容器内可见 $m 首歌" || warn "容器内可见 $m 首，宿主机有 $n 首"
docker stats --no-stream --format '  容器内存 {{.MemUsage}}' xiaozhi-esp32-server

step "完成"
echo "现在可以对小智说：\"昨天上证指数多少点\"、\"播放<歌名>\"、\"随便放首歌\"。请把以上输出发给 Claude。"
