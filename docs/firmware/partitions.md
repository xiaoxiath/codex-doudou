# Flash 分区方案 (4MB)

目标硬件:ESP32-C3-MINI-1U,4MB SPI Flash。
对应文件:[`partitions.csv`](./partitions.csv)。

## 总表

| 区域 | 起始 | 大小 | 用途 |
|---|---:|---:|---|
| bootloader | `0x0000` | 32 KB | ESP-IDF 第二阶段 bootloader(不在 partitions.csv 中) |
| partition_table | `0x8000` | 4 KB | 分区表本体(不在 partitions.csv 中) |
| nvs | `0x9000` | 16 KB | 系统配置:Wi-Fi 凭据、亮度、休眠时间、UI 主题 |
| otadata | `0xd000` | 8 KB | OTA 选择当前 boot slot(MVP 阶段也必须存在) |
| phy_init | `0xf000` | 4 KB | RF 校准数据 |
| **ota_0** | `0x10000` | **1.5 MB** | 应用槽 A |
| **ota_1** | `0x190000` | **1.5 MB** | 应用槽 B |
| assets | `0x310000` | 768 KB | LVGL 字体、图标、表情、配对二维码模板(SPIFFS) |
| audit | `0x3D0000` | 64 KB | 审批/确认事件本地落盘(NVS namespace) |
| _reserved_ | `0x3E0000` | 128 KB | 不分区,留作未来扩展 |

总计 ~3.97 MB,余 128 KB 安全余量。

## 关键决策

### 为什么没有 `factory` 分区?
OTA 双 slot 设计下不需要 `factory`,首烧也写入 `ota_0` 并由 `otadata` 指向 `ota_0`。这样 MVP 阶段不开 OTA 也兼容 V1 引入 OTA 时的布局,**不需要重新分区**。

### 为什么每个 app slot 1.5 MB 而不是 1.4 MB 或 2 MB?
- ESP-IDF + Wi-Fi (无 BT) + LVGL + cJSON + esp_websocket_client 的 release 镜像实测 1.0–1.3 MB,1.4 MB 太紧没有 OTA 故障回退余量。
- 2 MB × 2 = 4 MB,就装不下任何资源和日志。
- 1.5 MB 给后续加 BLE 配网 stack(BLE host + controller ~150 KB)留 200 KB 头。

### 为什么 audit 用 NVS 而不是 SPIFFS?
- 单条审计记录小(<256B),NVS 的 KV + wear leveling 比 SPIFFS 在小数据高频写场景更省 flash 寿命。
- 数据结构是 ring buffer(满了覆盖最老,详见 [device-protocol.md](../device-protocol.md#审计日志格式))。

### 为什么 assets 用 SPIFFS 而不是 LittleFS?
- 资源是 **read-only**,运行时几乎不写。SPIFFS 在只读场景的内存占用更小、ESP-IDF managed component 更成熟。
- 如果后续 audit 改去 assets 区共存,再换 LittleFS。

### 为什么 nvs 是 16 KB 而不是 24 KB?
NVS 实测 16 KB 够装:WiFi SSID/password (~256B) + pairing token (64B) + Bridge URL (128B) + 几十个配置项 + wear leveling 元数据。如果 V1 引入多 Bridge 绑定或证书,再扩到 24 KB,**届时通过 OTA 更新分区表前需要先迁移 NVS 内容**。

## 升级与回退

- MVP 阶段 OTA 不启用,但 `otadata` + `ota_1` 已存在 — V1 接入 `esp_https_ota` 时无需改分区表。
- OTA 流程:写 `ota_1` → 校验 → 切换 `otadata` 指向 `ota_1` → 重启。失败回退由 bootloader 通过 `otadata` 的 `seq` 字段自动完成。
- 资源升级(assets 区)走独立 OTA 通道,与 app OTA 不耦合。

## 不变量

修改 `partitions.csv` 时必须保持:
1. `ota_0` / `ota_1` 大小 **完全相等**(otherwise OTA 校验失败)。
2. `ota_0` 起始地址必须 64 KB 对齐(`0x10000` 的倍数)。
3. `nvs` 起始地址必须紧跟在 partition table 之后(`0x9000`),否则 ESP-IDF 默认 NVS 找不到分区。
4. 任何新增分区从 **预留区** `0x3E0000` 起切,**不要改动 ota_0/ota_1/assets 的偏移** — 否则全设备需要重刷,不能 OTA。
