# Xiaomiao ROM Loader

学而思小喵掌机的 ROM 选择器固件。烧入 factory 分区后常驻，从 TF 卡选择 ROM 镜像并加载到 ota_0 分区运行。Reset 或断电后自动回到 Loader。

## 工作原理

```
上电 → Bootloader 读 otadata → 启动 factory (Loader)
                                    │
                          扫描 TF 卡 /sdcard/boot/*.bin
                          显示 ROM 列表（LVGL 图形界面）
                                    │
                          用户选 ROM，按 A 键
                                    │
                    ┌───────────────┴───────────────┐
                    ▼                               ▼
              ROM 已在 ota_0                    ROM 不同
              (NVS 记录匹配)                  │
                    │                         │
            跳过写入(零擦写)                 从 merged.bin 提取 app
            直接 set_boot(ota_0)            esp_ota_write → ota_0
            重启                            set_boot(ota_0)
                    │                         │
                    └───────────┬─────────────┘
                                ▼
                    ROM 运行 (ota_0 分区)
                    app_main 首行: set_boot(factory)
                                │
                    Reset / 断电 / esp_restart
                                │
                    → 重启 → otadata 已指向 factory → 回到 Loader
```

## 分区表

| 分区 | 偏移 | 大小 | 用途 |
|------|------|------|------|
| bootloader | 0x1000 | ~30KB | ESP-IDF bootloader |
| partition_table | 0x9000 | 4KB | 分区表 |
| nvs | 0xA000 | 20KB | Loader 存储 ROM 状态 |
| phy_init | 0xF000 | 4KB | PHY 校准 |
| **factory** | **0x10000** | **696KB** | **Loader 固件（永不覆写）** |
| otadata | 0xBE000 | 8KB | 启动分区选择 |
| **ota_0** | **0xC0000** | **3.25MB** | **ROM 运行槽** |

## 构建

需要 ESP-IDF v5.5.4 + Python 3.12+。

```bash
idf.py build
```

## 烧录

```bash
# 通过 USB（GD32 UART 桥）
idf.py -p /dev/ttyACM0 flash

# 或用 esptool
esptool.py --chip esp32 -b 460800 write_flash 0x0 build/xiaomiao-loader-merged.bin
```

烧录接口始终可用——Loader 不触碰 bootloader 和分区表，esptool 可随时重新烧录。

## 使用

1. 把 ROM 镜像（.bin 文件）放入 TF 卡的 `/sdcard/boot/` 目录
2. 插入 TF 卡，开机进入 Loader
3. 上下键选择 ROM，A 键加载
4. 已加载的 ROM 前面显示 `>` 标记，重复选择时跳过写入
5. B 键查看系统信息

## ROM 兼容性

Loader 自动识别两种格式：
- **Merged bin**（app 在 0x10000）：自动提取 app 段
- **App-only bin**（app 在 0x0）：直接写入

ROM 大小上限：3.25MB。

### 让 ROM 支持"Reset 回到 Loader"

在 ROM 的 `app_main()` 第一行加入：

```c
#include "return_to_loader.h"

void app_main(void) {
    return_to_loader_setup();   // 必须在最前面
    // ... 正常初始化
}
```

`return_to_loader.h` 在本仓库根目录。不加此代码的 ROM 仍能正常运行，但 Reset 后会重新进入该 ROM 而非 Loader。

## 目标硬件

- ESP32-WROVER-B（4MB Flash, 8MB PSRAM）
- ST7735 SPI TFT 160x128
- MicroSD（SPI2 共享）
- 6 键导航

硬件资料和原理图见 [xueersi-idf](https://github.com/ZyoungInc/xueersi-idf)。
