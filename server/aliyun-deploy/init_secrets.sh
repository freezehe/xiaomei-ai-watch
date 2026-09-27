#!/bin/bash
# 生成 OTA 隐藏路径 SECRET 和 token 签名密钥 auth_key，并写入配置文件
# 用法（服务器上，root）：bash ~/xiaozhi-server/init_secrets.sh
set -euo pipefail

BASE=~/xiaozhi-server
NGINX_CONF=/usr/local/nginx/conf/conf.d/xiaozhi.conf
CONFIG=$BASE/data/.config.yaml
PUBLIC_IP="${PUBLIC_IP:?请先执行 export PUBLIC_IP=你的服务器公网IP}"

if ! grep -q '__SECRET__' "$NGINX_CONF" || ! grep -q '__AUTH_KEY__' "$CONFIG"; then
    echo "占位符已被替换过，跳过（要重新生成请先恢复原始文件）"
    exit 0
fi

SECRET=$(openssl rand -hex 16)
AUTH_KEY=$(openssl rand -hex 32)

sed -i "s/__SECRET__/$SECRET/g" "$NGINX_CONF"
sed -i "s/__AUTH_KEY__/$AUTH_KEY/" "$CONFIG"
chmod 600 "$CONFIG"

# 只有 root 可读，方便以后查
echo "http://$PUBLIC_IP:8888/$SECRET/xiaozhi/ota/" > $BASE/ota_url.txt
chmod 600 $BASE/ota_url.txt

echo "完成。板子上要填的 OTA 地址（也保存在 $BASE/ota_url.txt）："
echo "  http://$PUBLIC_IP:8888/$SECRET/xiaozhi/ota/"
