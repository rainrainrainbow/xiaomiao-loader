# Xiaomiao ROM Loader (Lightweight Version)

学而思小喵掌机的 ROM 选择器固件。支持 WiFi 文件管理和本地文件浏览。

## 主要特性

- ✅ **轻量级 UI**：移除 LVGL 依赖，使用自定义 framebuffer 渲染
- ✅ **WiFi 文件管理器**：通过 Web 界面管理 SD 卡文件
- ✅ **本地文件浏览**：设备端浏览和删除文件
- ✅ **ROM 选择器**：从 SD 卡选择 ROM 镜像并加载
- ✅ **OTA 写入**：将 ROM 写入 ota_0 分区并重启

## 硬件要求

- ESP32-WROVER-B（4MB Flash, 8MB PSRAM）
- ST7735 SPI TFT 160x128（RGB565）
- MicroSD 卡槽
- 6 键导航（上/下/左/右/A/B）

## 引脚定义

```
LCD:
  CS:   GPIO5
  DC:   GPIO4
  CLK:  GPIO18
  MOSI: GPIO23
  RST:  -1 (未使用)

SD Card:
  CS:   GPIO22
  MOSI: GPIO23 (共享)
  MISO: GPIO19
  CLK:  GPIO18 (共享)

Buttons:
  UP:    GPIO32
  DOWN:  GPIO33
  LEFT:  GPIO25
  RIGHT: GPIO26
  A:     GPIO27
  B:     GPIO12
```

## 使用方法

### 1. 准备 SD 卡

在 SD 卡根目录创建 `boot` 文件夹，将 ROM 镜像（`.bin` 或 `.img`）放入其中。

### 2. 烧录固件

```bash
# 使用 esptool 烧录
esptool.py --chip esp32 -b 460800 write_flash 0x0 xiaomiao-loader-merged.bin
```

### 3. 启动设备

- **正常启动**：进入 ROM 选择界面
- **按住 B 键启动**：进入 WiFi 文件管理模式

### 4. 操作说明

#### ROM 选择界面
- **上/下键**：选择 ROM
- **A 键**：加载选中的 ROM
- **B 键**：进入 WiFi 文件管理模式
- **左键**：进入本地文件浏览器

#### WiFi 文件管理模式
1. 设备会创建 WiFi 热点：
   - SSID: `Xiaomiao-Loader`
   - 密码: `12345678`
2. 手机/电脑连接该 WiFi
3. 访问 `http://192.168.4.1`
4. 通过 Web 界面：
   - 浏览 SD 卡文件
   - 上传新 ROM
   - 下载/删除文件
5. 再次按 B 键退出 WiFi 模式

## 分区表

| 分区 | 偏移 | 大小 | 用途 |
|------|------|------|------|
| bootloader | 0x1000 | ~30KB | ESP-IDF bootloader |
| partition_table | 0x8000 | 4KB | 分区表 |
| nvs | 0xA000 | 20KB | 存储 ROM 状态 |
| phy_init | 0xF000 | 4KB | PHY 校准 |
| **factory** | **0x10000** | **568KB** | **Loader 固件** |
| otadata | 0x9E000 | 8KB | 启动分区选择 |
| **launcher** | **0xA0000** | **2.12MB** | **ROM 运行槽 (ota_0)** |
| **retro-core** | **0x2C0000** | **1.25MB** | **retro-go 模拟器核心 (ota_1)** |

## 构建

### 本地构建

需要 ESP-IDF v5.3.1+：

```bash
# 设置环境
. ~/esp/esp-idf/export.sh

# 构建
idf.py set-target esp32
idf.py build

# 生成合并固件
idf.py merge-bin -o xiaomiao-loader-merged.bin
```

### GitHub Actions 自动构建

每次推送到 main 分支会自动触发构建，生成的固件可在 Actions 页面下载。

## 与原版差异

| 特性 | 原版 | 轻量版 |
|------|------|--------|
| UI 框架 | LVGL 9.5.0 | 自定义 framebuffer |
| Flash 占用 | ~400KB | ~200KB |
| WiFi 文件管理 | ❌ | ✅ |
| 本地文件浏览 | ❌ | ✅ |
| 分区表 | 相同 | 相同 |
| ROM 兼容性 | 相同 | 相同 |

## 故障排除

### SD 卡无法挂载
- 检查 SD 卡格式（FAT32）
- 确认引脚连接正确
- 查看串口日志

### WiFi 无法启动
- 检查 sdkconfig 中 WiFi 配置
- 确认 PSRAM 正常工作
- 查看可用内存

### ROM 加载失败
- 确认 ROM 格式正确（app-only bin 或 merged bin）
- 检查 ROM 大小不超过 2.12MB
- 查看串口日志错误信息

## 许可证

MIT License

## 致谢

- 原版 Xiaomiao Loader 作者
- ESP-IDF 团队
- 社区贡献者
