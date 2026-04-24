#!/system/bin/sh
# test.sh — iOS Brightness 模块测试脚本
# 用法: tsu 然后 sh /data/local/tmp/test.sh

PROC=/proc/ios_brightness

echo ""
echo "========================================"
echo "  iOS Brightness Module Test"
echo "========================================"
echo ""

# 检查模块是否加载
echo "[1/8] 检查模块加载..."
if lsmod | grep -q ios_brightness; then
    echo "      模块已加载 ✓"
else
    echo "      模块未加载 ✗"
    echo "      尝试手动加载:"
    echo "      insmod /data/adb/modules/ios_brightness/ios_brightness.ko"
    exit 1
fi
echo ""

# 检查 procfs
echo "[2/8] 检查 procfs 接口..."
if [ -d "$PROC" ]; then
    echo "      /proc/ios_brightness/ 存在 ✓"
    echo "      文件列表:"
    ls -la $PROC/ | sed 's/^/      /'
else
    echo "      /proc/ios_brightness/ 不存在 ✗"
    exit 1
fi
echo ""

# 查看当前状态
echo "[3/8] 当前状态:"
cat $PROC/status | sed 's/^/      /'
echo ""

# 启用模块
echo "[4/8] 启用模块..."
echo 1 > $PROC/enabled
echo manual > $PROC/mode
echo "      enabled=$(cat $PROC/enabled)"
echo "      mode=$(cat $PROC/mode)"
echo ""

# 亮度曲线测试
echo "[5/8] 亮度曲线测试 (请观察屏幕亮度变化)..."
echo "      每个亮度停留 1 秒..."
for s in 0 50 100 200 300 500 700 1000; do
    echo $s > $PROC/set_brightness
    BRI=$(cat $PROC/brightness)
    echo "      slider=$s → $BRI"
    sleep 1
done
echo ""

# 恢复到 50%
echo "[6/8] 恢复到 50%..."
echo 500 > $PROC/set_brightness
echo "      $(cat $PROC/brightness)"
echo ""

# Gamma 测试
echo "[7/8] Gamma 曲线对比测试..."
echo ""
echo "      --- gamma=2.0 (更线性) ---"
echo 200 > $PROC/gam
for s in 100 300 500; do
    echo $s > $PROC/set_brightness
    echo "      slider=$s → $(cat $PROC/brightness)"
    sleep 1
done

echo ""
echo "      --- gamma=2.5 (iOS 默认) ---"
echo 250 > $PROC/gam
for s in 100 300 500; do
    echo $s > $PROC/set_brightness
    echo "      slider=$s → $(cat $PROC/brightness)"
    sleep 1
done

echo ""
echo "      --- gamma=3.0 (低亮度更精细) ---"
echo 300 > $PROC/gam
for s in 100 300 500; do
    echo $s > $PROC/set_brightness
    echo "      slider=$s → $(cat $PROC/brightness)"
    sleep 1
done

echo ""
echo "      恢复 gamma=2.5..."
echo 250 > $PROC/gam
echo ""

# Lux feed 测试
echo "[8/8] 自动亮度 lux_feed 测试..."
echo auto > $PROC/mode
for lux in 0 50 500 3000 10000; do
    echo $lux > $PROC/lux
    sleep 2
    echo "      lux=$lux → $(cat $PROC/brightness)"
done
echo ""

# 恢复
echo "恢复手动模式 50%..."
echo manual > $PROC/mode
echo 500 > $PROC/set_brightness
echo ""

echo "========================================"
echo "  测试完成!"
echo "========================================"
echo ""
echo "如果屏幕亮度在测试过程中有明显变化,"
echo "说明模块工作正常。"
echo ""
echo "常用命令:"
echo "  cat /proc/ios_brightness/status     # 查看状态"
echo "  echo 500 > /proc/ios_brightness/set_brightness  # 设50%"
echo "  echo auto > /proc/ios_brightness/mode           # 自动模式"
echo "  echo 250 > /proc/ios_brightness/gamma           # 改gamma"
echo ""
echo "卸载: rm -rf /data/adb/modules/ios_brightness && reboot"
echo ""
