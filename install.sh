#!/system/bin/sh
# install.sh — 安装 iOS Brightness 模块到 SukiSU Ultra
# 用法: tsu 然后 sh /data/local/tmp/install.sh

set -e

MOD=/data/adb/modules/ios_brightness
SRC=/data/local/tmp

echo ""
echo "========================================"
echo "  iOS Brightness Module Installer"
echo "  for Redmi K90 Pro Max (SukiSU Ultra)"
echo "========================================"
echo ""

# 检查 root
if [ "$(id -u)" != "0" ]; then
    echo "ERROR: 需要 root 权限"
    echo "请先运行: tsu"
    exit 1
fi

# 检查 .ko 文件
if [ ! -f "$SRC/ios_brightness.ko" ]; then
    echo "ERROR: $SRC/ios_brightness.ko 不存在"
    echo ""
    echo "请先将编译好的文件推送到手机:"
    echo "  adb push ios_brightness.ko /data/local/tmp/"
    echo "  adb push lux_feed /data/local/tmp/"
    exit 1
fi

echo "[1/6] 创建模块目录..."
mkdir -p $MOD
echo "      $MOD"

echo "[2/6] 复制内核模块..."
cp $SRC/ios_brightness.ko $MOD/
echo "      ios_brightness.ko"

if [ -f "$SRC/lux_feed" ]; then
    cp $SRC/lux_feed $MOD/
    echo "      lux_feed"
fi

echo "[3/6] 创建 module.prop..."
cat > $MOD/module.prop << 'EOF'
id=ios_brightness
name=iOS Brightness Control
version=v2.0.0
versionCode=2
author=MiMo
description=iOS-style brightness with smooth transitions and auto-brightness for K90 Pro Max on SukiSU Ultra
EOF
echo "      done"

echo "[4/6] 创建 service.sh..."
cat > $MOD/service.sh << 'SEOF'
#!/system/bin/sh
# service.sh — 开机自动加载并配置模块
MODDIR=${0%/*}
PROC=/proc/ios_brightness
LOG=/data/local/tmp/ios_brightness.log

log() { echo "[$(date '+%H:%M:%S')] $1" >> $LOG; }

log "=== service.sh start ==="

# 等待系统完全启动
sleep 12

# 加载内核模块（如果还没加载）
if ! lsmod | grep -q ios_brightness; then
    insmod ${MODDIR}/ios_brightness.ko \
        bl_dev_name="panel0-backlight" \
        def_max_raw=16383 \
        def_min_raw=1 \
        def_gamma_x100=250 \
        def_transition_ms=300 \
        2>>$LOG

    if [ $? -eq 0 ]; then
        log "module loaded successfully"
    else
        log "ERROR: insmod failed!"
        exit 1
    fi
fi

# 等待 procfs 就绪
sleep 2

# 启用模块
echo 1 > $PROC/enabled
log "module enabled"

# 读取 Android 自动亮度设置并同步
AUTO=$(settings get system screen_brightness_mode 2>/dev/null)
if [ "$AUTO" = "1" ]; then
    echo auto > $PROC/mode
    log "mode=auto (synced from Android)"
else
    echo manual > $PROC/mode
    # 读取 Android 当前亮度 (0-255) 并映射到 slider (0-1000)
    B=$(settings get system screen_brightness 2>/dev/null)
    if [ -n "$B" ] && [ "$B" -ge 0 ] 2>/dev/null; then
        S=$((B * 1000 / 255))
        echo $S > $PROC/set_brightness
        log "mode=manual slider=$S (android_brightness=$B)"
    else
        echo 500 > $PROC/set_brightness
        log "mode=manual slider=500 (default)"
    fi
fi

# 设置 lux_feed 权限（允许其他用户写入）
chmod 0222 $PROC/lux_feed 2>/dev/null

# 启动 lux feed 守护进程
if [ -x "${MODDIR}/lux_feed" ]; then
    nohup ${MODDIR}/lux_feed -v >> $LOG 2>&1 &
    log "lux_feed daemon started (pid=$!)"
else
    log "lux_feed not found, auto-brightness requires manual lux_feed"
fi

log "=== service.sh done ==="
SEOF
chmod 755 $MOD/service.sh
echo "      done"

echo "[5/6] 创建 auto_sync.sh..."
cat > $MOD/auto_sync.sh << 'ASEOF'
#!/system/bin/sh
# auto_sync.sh — 监听 Android 自动亮度开关变化并同步到模块
PROC=/proc/ios_brightness
LAST_STATE=""

while true; do
    # 读取 Android 自动亮度设置 (0=关, 1=开)
    CUR=$(settings get system screen_brightness_mode 2>/dev/null)

    if [ "$CUR" != "$LAST_STATE" ]; then
        MODE=$(cat $PROC/mode 2>/dev/null | tr -d '\n')

        if [ "$CUR" = "1" ] && [ "$MODE" != "auto" ]; then
            # Android 开启了自动亮度 → 切换模块到 auto 模式
            echo auto > $PROC/mode
            echo "[$(date)] synced: auto brightness ON"
        elif [ "$CUR" = "0" ] && [ "$MODE" = "auto" ]; then
            # Android 关闭了自动亮度 → 切换模块到 manual 模式
            echo manual > $PROC/mode
            # 读取 Android 当前亮度并设置
            B=$(settings get system screen_brightness 2>/dev/null)
            S=$((B * 1000 / 255))
            echo $S > $PROC/set_brightness
            echo "[$(date)] synced: manual brightness slider=$S"
        fi

        LAST_STATE="$CUR"
    fi

    sleep 3
done
ASEOF
chmod 755 $MOD/auto_sync.sh
echo "      done"

echo "[6/6] 设置权限..."
chmod 755 $MOD/service.sh
chmod 755 $MOD/auto_sync.sh
[ -f "$MOD/lux_feed" ] && chmod 755 $MOD/lux_feed
echo "      done"

echo ""
echo "========================================"
echo "  安装完成!"
echo "========================================"
echo ""
echo "已安装文件:"
ls -la $MOD/
echo ""
echo "下一步:"
echo "  1. 重启手机: reboot"
echo "  2. 重启后验证: cat /proc/ios_brightness/status"
echo "  3. 测试亮度: sh /data/local/tmp/test.sh"
echo ""
