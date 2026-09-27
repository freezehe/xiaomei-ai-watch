#!/bin/bash
# 第 1 步：准备服务器环境（可重复执行，已完成的步骤会自动跳过）
#   - 2GB swap
#   - Docker CE + compose 插件（CentOS 7，必要时切换到 centos-vault 源）
#   - 让 /usr/local/nginx 加载 conf.d/*.conf
# 用法（root）：bash /home/<你的用户名>/aliyun-deploy/1_prepare_server.sh 2>&1 | tee /root/prepare.log
set -uo pipefail

NGINX_HOME=/usr/local/nginx
NGINX_CONF=$NGINX_HOME/conf/nginx.conf

step() { echo; echo "==================== $* ===================="; }
ok()   { echo "  [OK]   $*"; }
warn() { echo "  [WARN] $*"; }
fail() { echo "  [FAIL] $*"; exit 1; }

[ "$(id -u)" = 0 ] || fail "请先 su - root 再执行"

step "0. 系统信息"
cat /etc/redhat-release
uname -r
free -h
df -h / | tail -1

step "1. swap（2GB）"
if swapon --show 2>/dev/null | grep -q .; then
    ok "已有 swap，跳过"; swapon --show
else
    dd if=/dev/zero of=/swapfile bs=1M count=2048 status=none || fail "创建 swapfile 失败"
    chmod 600 /swapfile && mkswap /swapfile >/dev/null && swapon /swapfile || fail "启用 swap 失败"
    grep -q '^/swapfile' /etc/fstab || echo '/swapfile none swap sw 0 0' >> /etc/fstab
    sysctl -q vm.swappiness=10
    grep -q '^vm.swappiness' /etc/sysctl.conf || echo 'vm.swappiness=10' >> /etc/sysctl.conf
    ok "swap 已启用"; free -h | grep -i swap
fi

step "2. yum 源可用性"
if yum -q makecache fast >/dev/null 2>&1; then
    ok "yum 源正常"
else
    warn "yum 源不可用，切换到阿里云 centos-vault（原文件备份到 /etc/yum.repos.d/bak-*）"
    bak=/etc/yum.repos.d/bak-$(date +%Y%m%d%H%M%S); mkdir -p "$bak"; cp /etc/yum.repos.d/*.repo "$bak"/
    sed -i -e 's|^mirrorlist=|#mirrorlist=|g' \
           -e 's|^#\?baseurl=http://mirror.centos.org|baseurl=https://mirrors.aliyun.com/centos-vault|g' \
           /etc/yum.repos.d/CentOS-*.repo
    yum clean all >/dev/null; yum -q makecache fast >/dev/null 2>&1 && ok "切换后 yum 正常" || fail "yum 源仍不可用，请把本输出发给 Claude"
fi

step "3. Docker"
if command -v docker >/dev/null 2>&1 && docker compose version >/dev/null 2>&1; then
    ok "Docker 和 compose 插件已安装，跳过"
else
    yum install -y -q yum-utils || fail "安装 yum-utils 失败"
    yum-config-manager -q --add-repo https://mirrors.aliyun.com/docker-ce/linux/centos/docker-ce.repo >/dev/null
    sed -i 's|download.docker.com|mirrors.aliyun.com/docker-ce|g' /etc/yum.repos.d/docker-ce.repo
    yum install -y -q docker-ce docker-ce-cli containerd.io docker-compose-plugin || fail "安装 Docker 失败"
    ok "Docker 已安装"
fi
mkdir -p /etc/docker
if [ ! -f /etc/docker/daemon.json ]; then
    cat > /etc/docker/daemon.json <<'EOF'
{
  "log-driver": "json-file",
  "log-opts": { "max-size": "20m", "max-file": "3" }
}
EOF
    ok "已写入 /etc/docker/daemon.json（日志大小限制）"
else
    warn "/etc/docker/daemon.json 已存在，保持不变："; cat /etc/docker/daemon.json
fi
systemctl enable --now docker >/dev/null 2>&1 || fail "启动 docker 失败"
docker version --format '  Docker Server {{.Server.Version}}' || fail "docker 未正常运行"
docker compose version

step "4. Nginx（$NGINX_HOME）"
[ -x $NGINX_HOME/sbin/nginx ] || fail "找不到 $NGINX_HOME/sbin/nginx"
$NGINX_HOME/sbin/nginx -v 2>&1
mkdir -p $NGINX_HOME/conf/conf.d
if grep -Eq '^\s*include\s+conf\.d/\*\.conf\s*;' $NGINX_CONF; then
    ok "nginx.conf 已包含 conf.d/*.conf"
else
    cp $NGINX_CONF $NGINX_CONF.bak-$(date +%Y%m%d%H%M%S)
    # 在第一个 "http {" 后插入 include
    sed -i '0,/^\s*http\s*{/s//&\n    include conf.d\/*.conf;/' $NGINX_CONF
    grep -Eq '^\s*include\s+conf\.d/\*\.conf\s*;' $NGINX_CONF && ok "已在 http {} 中加入 include conf.d/*.conf（原文件已备份）" \
        || fail "自动修改 nginx.conf 失败，请把 nginx.conf 的 http { 附近内容发给 Claude"
fi
$NGINX_HOME/sbin/nginx -t 2>&1 || fail "nginx 配置测试失败（原配置备份在 nginx.conf.bak-*）"
if ss -lntp | grep -q ':8888 '; then
    warn "8888 端口已被占用："; ss -lntp | grep ':8888 '
else
    ok "8888 端口空闲"
fi

step "完成"
free -h
echo "第 1 步完成。请把以上全部输出发给 Claude，然后继续第 2 步（2_deploy.sh）。"
