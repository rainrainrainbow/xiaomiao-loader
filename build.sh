#!/bin/bash
# 一键构建脚本

set -e

# 检查 ESP-IDF 环境
if [ -z "$IDF_PATH" ]; then
    echo "请先设置 ESP-IDF 环境："
    echo "  source ~/esp/esp-idf/export.sh"
    exit 1
fi

echo "=== 开始构建 Xiaomiao Loader ==="

# 清理旧的构建
echo "清理旧的构建文件..."
rm -rf build/

# 设置目标芯片
echo "设置目标芯片为 ESP32..."
idf.py set-target esp32

# 构建固件
echo "构建固件..."
idf.py build

# 生成合并固件
echo "生成合并固件..."
idf.py merge-bin -o xiaomiao-loader-merged.bin

echo ""
echo "=== 构建完成 ==="
echo "固件文件："
echo "  - build/xiaomiao-loader.bin (应用程序)"
echo "  - xiaomiao-loader-merged.bin (完整固件)"
echo ""
echo "烧录命令："
echo "  esptool.py --chip esp32 -b 460800 write_flash 0x0 xiaomiao-loader-merged.bin"
