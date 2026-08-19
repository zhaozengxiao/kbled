#!/bin/bash
# kbled 安装脚本：编译 + 安装 + 开机自启 + 免密 sudo
# 用法: sudo ./install.sh
set -e

if [ "$(id -u)" -ne 0 ]; then
    echo "请用 root 运行: sudo ./install.sh"
    exit 1
fi

DIR="$(cd "$(dirname "$0")" && pwd)"
BIN=/usr/local/sbin/kbled
CONF=/etc/kbled.conf
SVC=/etc/systemd/system/kbled.service
SUDOCONF=/etc/sudoers.d/kbled
USER_NAME="${SUDO_USER:-$USER}"

echo "==> 编译"
gcc -O2 -Wall -o "$DIR/kbled" "$DIR/kbled.c"

echo "==> 安装到 $BIN"
install -m 755 "$DIR/kbled" "$BIN"

if [ ! -f "$CONF" ]; then
    echo "==> 写入默认配置 $CONF (白色, 亮度 200, 开启)"
    printf 'enabled=1\ncolor=FFFFFF\nbrightness=200\n' > "$CONF"
fi

echo "==> 安装 systemd 服务"
cat > "$SVC" <<'EOF'
[Unit]
Description=Apply Clevo keyboard backlight settings
After=multi-user.target suspend.target hibernate.target

[Service]
Type=oneshot
ExecStart=/usr/local/sbin/kbled apply
RemainAfterExit=yes

[Install]
WantedBy=multi-user.target suspend.target hibernate.target
EOF
systemctl daemon-reload
systemctl enable kbled.service
systemctl start kbled.service

if [ -n "$USER_NAME" ] && [ "$USER_NAME" != "root" ]; then
    echo "==> 为 $USER_NAME 配置免密 sudo"
    echo "$USER_NAME ALL=(root) NOPASSWD: $BIN" > "$SUDOCONF"
    chmod 440 "$SUDOCONF"
    visudo -c -f "$SUDOCONF"
fi

echo
echo "安装完成。用法:"
echo "  sudo $BIN on                 # 开灯"
echo "  sudo $BIN off                # 关灯"
echo "  sudo $BIN color 00C8FF       # 颜色"
echo "  sudo $BIN brightness 150     # 亮度 0-255"
echo "  sudo $BIN status             # 状态"