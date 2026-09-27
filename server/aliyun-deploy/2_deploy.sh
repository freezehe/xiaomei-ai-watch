#!/bin/bash
# 第 2 步：部署小智服务端（Docker + Nginx 8888 + OTA 隐藏路径）
# 前提：已执行 1_prepare_server.sh；本目录已上传到 /home/<你的用户名>/aliyun-deploy
# 用法（root）：bash /home/<你的用户名>/aliyun-deploy/2_deploy.sh 2>&1 | tee /root/deploy.log
set -uo pipefail

SRC=$(cd "$(dirname "$0")" && pwd)
BASE=/root/xiaozhi-server
NGINX_HOME=/usr/local/nginx
NGINX_SITE=$NGINX_HOME/conf/conf.d/xiaozhi.conf
PUBLIC_IP="${PUBLIC_IP:?请先执行 export PUBLIC_IP=你的服务器公网IP}"

step() { echo; echo "==================== $* ===================="; }
ok()   { echo "  [OK]   $*"; }
warn() { echo "  [WARN] $*"; }
fail() { echo "  [FAIL] $*"; exit 1; }

[ "$(id -u)" = 0 ] || fail "请先 su - root 再执行"
command -v docker >/dev/null || fail "未安装 Docker，请先执行 1_prepare_server.sh"

step "1. 放置文件"
mkdir -p $BASE/data
for f in docker-compose.yml init_secrets.sh data/.config.yaml nginx-xiaozhi.conf; do
    [ -f "$SRC/$f" ] || fail "缺少 $SRC/$f（上传不完整）"
done
sed -i 's/\r$//' "$SRC"/*.sh "$SRC"/*.yml "$SRC"/*.conf "$SRC"/data/.config.yaml
cp "$SRC/docker-compose.yml" "$SRC/init_secrets.sh" $BASE/
if [ -f $BASE/data/.config.yaml ] && ! grep -q '__AUTH_KEY__' $BASE/data/.config.yaml; then
    warn "$BASE/data/.config.yaml 已存在且已生成过密钥，保留现有文件（新版本放在 .config.yaml.new）"
    cp "$SRC/data/.config.yaml" $BASE/data/.config.yaml.new
else
    cp "$SRC/data/.config.yaml" $BASE/data/.config.yaml
fi
if [ -f $NGINX_SITE ] && ! grep -q '__SECRET__' $NGINX_SITE; then
    warn "$NGINX_SITE 已存在且已生成过隐藏路径，保留"
else
    cp "$SRC/nginx-xiaozhi.conf" $NGINX_SITE
fi
chmod 700 $BASE; chmod 600 $BASE/data/.config.yaml*
ok "文件已就位"

step "2. 生成 OTA 隐藏路径和 token 密钥"
bash $BASE/init_secrets.sh || fail "init_secrets.sh 失败"
[ -f $BASE/ota_url.txt ] || fail "没有生成 ota_url.txt"
grep -q '__' $NGINX_SITE $BASE/data/.config.yaml && fail "仍有未替换的占位符"
ok "密钥已生成"

step "3. Nginx"
# 日志用绝对路径：这台机器的 /usr/local/nginx 下没有 logs 目录；兼容旧版配置里的相对路径
mkdir -p /var/log/xiaozhi-nginx
sed -i 's#logs/xiaozhi_access.log#/var/log/xiaozhi-nginx/access.log#; s#logs/xiaozhi_error.log#/var/log/xiaozhi-nginx/error.log#' $NGINX_SITE
$NGINX_HOME/sbin/nginx -t 2>&1 || fail "nginx 配置测试失败"
if pgrep -x nginx >/dev/null; then
    # -s reload 依赖 pid 文件；这台机器的 nginx 目录结构非默认，失败时直接给 master 进程发 HUP（等价于 reload）
    if $NGINX_HOME/sbin/nginx -s reload 2>/dev/null; then
        ok "nginx 已重载"
    else
        master=$(pgrep -o -x nginx)
        kill -HUP "$master" && ok "nginx 已重载（HUP → master pid $master）" || fail "nginx 重载失败"
    fi
else
    $NGINX_HOME/sbin/nginx && ok "nginx 已启动"
fi
sleep 1
ss -lntp | grep -q ':8888 ' && ok "nginx 正在监听 8888" || fail "nginx 没有监听 8888"

step "4. 拉取镜像并启动容器（首次需要几分钟）"
cd $BASE
docker compose pull || fail "拉取镜像失败（网络问题可重试本脚本）"
docker compose up -d || fail "启动容器失败"
echo "  等待服务启动..."
for i in $(seq 1 60); do
    docker logs xiaozhi-esp32-server 2>&1 | grep -q 'Websocket地址' && break
    sleep 3
done
docker logs xiaozhi-esp32-server 2>&1 | grep -avE 'X-Api-Access-Key|api_key' | grep -aE '初始化组件|Websocket地址|ERROR|Error|Traceback' | tail -12
docker logs xiaozhi-esp32-server 2>&1 | grep -q 'Websocket地址' && ok "服务端已启动" || fail "90 秒内未启动成功，看上面的日志"

step "5. 自检"
OTA_URL=$(cat $BASE/ota_url.txt)
SECRET_PATH=${OTA_URL#http://$PUBLIC_IP:8888}
r=$(curl -s -m 10 "http://127.0.0.1:8888$SECRET_PATH")
echo "$r" | grep -q 'OTA接口运行正常' && ok "隐藏路径 OTA 正常：$r" || fail "隐藏路径 OTA 异常：$r"
code=$(curl -s -o /dev/null -w '%{http_code}' -m 10 http://127.0.0.1:8888/xiaozhi/ota/)
[ "$code" = 404 ] && ok "不带隐藏路径返回 404（正确）" || warn "不带隐藏路径返回 $code（应为 404）"
ss -lntp | grep -E ':(8000|8003) ' | grep -vq '127.0.0.1' && warn "8000/8003 绑定到了非本机地址！" || ok "8000/8003 只绑定 127.0.0.1"
docker stats --no-stream --format '  容器内存 {{.MemUsage}}  CPU {{.CPUPerc}}' xiaozhi-esp32-server
free -h

step "完成"
echo "板子上要填的 OTA 地址（只有 root 可读：$BASE/ota_url.txt）："
echo "  $OTA_URL"
echo
echo "请把以上输出发给 Claude。注意：OTA 地址相当于密码，发给 Claude 可以，不要发到公开场合。"
