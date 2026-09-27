#!/bin/bash
# 第 4 步：给桌面音乐 App 提供歌曲列表和 mp3 下载（Nginx，复用 OTA 的隐藏路径）
#   歌曲列表：http://<IP>:8888/<SECRET>/music/            （JSON，autoindex_format json）
#   子目录：  http://<IP>:8888/<SECRET>/music/<目录>/
#   歌曲文件：http://<IP>:8888/<SECRET>/music/<文件名>.mp3
# 音乐目录读取自 3_update.sh 写入的 /root/xiaozhi-server/.env（MUSIC_DIR）
# 用法（root）：bash /home/<你的用户名>/aliyun-deploy/4_music_http.sh 2>&1 | tee /root/music_http.log
set -uo pipefail

BASE=/root/xiaozhi-server
NGINX_HOME=/usr/local/nginx
SITE=$NGINX_HOME/conf/conf.d/xiaozhi.conf
PUBLIC_IP="${PUBLIC_IP:?请先执行 export PUBLIC_IP=你的服务器公网IP}"

step() { echo; echo "==================== $* ===================="; }
ok()   { echo "  [OK]   $*"; }
warn() { echo "  [WARN] $*"; }
fail() { echo "  [FAIL] $*"; exit 1; }

[ "$(id -u)" = 0 ] || fail "请先 su - root 再执行"

step "1. 读取音乐目录和隐藏路径"
[ -f $BASE/.env ] || fail "没有 $BASE/.env，请先执行 3_update.sh"
MUSIC_DIR=$(sed -n 's/^MUSIC_DIR=//p' $BASE/.env)
[ -d "$MUSIC_DIR" ] || fail "音乐目录不存在：$MUSIC_DIR"
SECRET=$(sed -E 's#^http://[^/]+/([0-9a-f]+)/xiaozhi/ota/$#\1#' $BASE/ota_url.txt)
[[ "$SECRET" =~ ^[0-9a-f]{32}$ ]] || fail "无法从 ota_url.txt 解析隐藏路径"
ok "音乐目录：$MUSIC_DIR"

step "2. 检查 Nginx 工作进程能否读取音乐文件"
NGX_USER=$(ps -o user= -p "$(pgrep -f 'nginx: worker' | head -1)" 2>/dev/null | tr -d ' ')
[ -n "$NGX_USER" ] || NGX_USER=nobody
ok "Nginx 工作进程用户：$NGX_USER"
SAMPLE=$(find "$MUSIC_DIR" -type f -iname '*.mp3' | head -1)
[ -n "$SAMPLE" ] || fail "$MUSIC_DIR 下没有 mp3"
can_read() { su -s /bin/sh "$NGX_USER" -c 'test -r "$0"' "$1" 2>/dev/null; }

SERVE_DIR=$MUSIC_DIR
if can_read "$SAMPLE"; then
    ok "$NGX_USER 可以直接读取 $SAMPLE"
else
    # 常见原因：上级目录（如 /home/<你的用户名> 为 700）不允许进入。
    # 不改家目录权限，而是把音乐目录只读绑定挂载到 /srv/xiaozhi-music，Nginx 从那里读
    SERVE_DIR=/srv/xiaozhi-music
    mkdir -p $SERVE_DIR
    if mountpoint -q $SERVE_DIR; then
        ok "$SERVE_DIR 已挂载"
    else
        mount --bind "$MUSIC_DIR" $SERVE_DIR && mount -o remount,bind,ro $SERVE_DIR \
            || fail "绑定挂载 $MUSIC_DIR -> $SERVE_DIR 失败"
        ok "已把 $MUSIC_DIR 只读挂载到 $SERVE_DIR（家目录权限保持不变）"
    fi
    FSTAB_SRC=$(printf '%s' "$MUSIC_DIR" | sed 's/ /\040/g')
    if ! grep -q " $SERVE_DIR " /etc/fstab; then
        cp /etc/fstab /etc/fstab.bak-$(date +%Y%m%d%H%M%S)
        echo "$FSTAB_SRC $SERVE_DIR none bind,ro 0 0" >> /etc/fstab
        ok "已写入 /etc/fstab，重启后自动挂载（原文件已备份）"
    fi
    SAMPLE_SERVE="$SERVE_DIR/${SAMPLE#$MUSIC_DIR/}"
    if can_read "$SAMPLE_SERVE"; then
        ok "$NGX_USER 现在可以通过 $SERVE_DIR 读取音乐"
    else
        echo "  [FAIL] 挂载后 $NGX_USER 仍无法读取，权限如下："
        namei -l "$SAMPLE_SERVE" | sed 's/^/         /'
        echo "  音乐目录需要 o+rx（如 755），歌曲文件需要 o+r（如 644）"
        exit 1
    fi
fi
# 个别文件没有 o+r 时只提示，不改权限
bad=$(find "$SERVE_DIR" -type f \( -iname '*.mp3' -o -iname '*.wav' \) ! -perm -o=r | wc -l)
[ "$bad" = 0 ] || warn "$bad 首歌对其他用户不可读，桌面 App 里会播放失败：chmod o+r 这些文件即可"

step "3. 写入 Nginx 配置"
if grep -q "/music/" $SITE; then
    ok "已存在 /music/ 配置，跳过"
else
    cp $SITE $SITE.bak-$(date +%Y%m%d%H%M%S)
    cat > /tmp/xz_music.conf <<EOF
    # 桌面音乐 App：歌曲列表（JSON）与 mp3 文件，和 OTA 共用隐藏路径
    location /$SECRET/music/ {
        alias $SERVE_DIR/;
        autoindex on;
        autoindex_format json;
        charset utf-8;
        types { audio/mpeg mp3; audio/wav wav; }
        default_type application/octet-stream;
    }

EOF
    # 插在兜底的 "location / {" 之前
    awk 'FNR==NR {b = b $0 "\n"; next} /^    location \/ \{/ && !d {printf "%s", b; d = 1} {print}' /tmp/xz_music.conf $SITE > $SITE.tmp && mv $SITE.tmp $SITE
    grep -q "/music/" $SITE && ok "已加入 location /$SECRET/music/（原文件已备份）" || fail "修改 $SITE 失败"
fi
$NGINX_HOME/sbin/nginx -t 2>&1 || fail "nginx 配置测试失败（备份在 $SITE.bak-*）"
$NGINX_HOME/sbin/nginx -s reload 2>/dev/null || kill -HUP "$(pgrep -o -x nginx)" || fail "nginx 重载失败"
ok "nginx 已重载"
sleep 1

step "4. 自检"
LIST="http://127.0.0.1:8888/$SECRET/music/"
code=$(curl -s -o /tmp/xz_list.json -w '%{http_code}' -m 10 "$LIST")
[ "$code" = 200 ] || fail "歌曲列表返回 HTTP $code"
python3 - /tmp/xz_list.json <<'EOF' 2>/dev/null || head -c 300 /tmp/xz_list.json
import json, sys
items = json.load(open(sys.argv[1], encoding='utf-8'))
files = [i for i in items if i['type'] == 'file']; dirs = [i for i in items if i['type'] == 'directory']
print(f"  [OK]   歌曲列表 JSON 正常：{len(files)} 个文件，{len(dirs)} 个子目录")
for i in (dirs + files)[:5]: print("         ", i['type'][:4], i['name'])
EOF
f=$(basename "$SAMPLE"); rel=${SAMPLE#$MUSIC_DIR/}
enc=$(python3 -c "import sys, urllib.parse; print(urllib.parse.quote(sys.argv[1]))" "$rel" 2>/dev/null || echo "$rel")
code=$(curl -s -o /dev/null -w '%{http_code} %{content_type} %{size_download}' -r 0-1023 -m 10 "http://127.0.0.1:8888/$SECRET/music/$enc")
[[ "$code" == 206* ]] && ok "分段下载正常（断点续传可用）：$code" || warn "下载返回：$code"
code=$(curl -s -o /dev/null -w '%{http_code}' -m 10 "http://127.0.0.1:8888/music/")
[ "$code" = 404 ] && ok "不带隐藏路径访问 /music/ 返回 404（正确）" || warn "不带隐藏路径返回 $code（应为 404）"

step "完成"
echo "桌面音乐 App 会从小智保存的 OTA 地址自动推出歌曲列表地址，无需在板子上额外配置。"
echo "请把以上输出发给 Claude。"
