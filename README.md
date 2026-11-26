# EdgeHMI — 工业级 ESP32-S3 原生 C 底座 + JSON 动态组态引擎

基于 **ESP-IDF 5.x (C11) + LVGL 9 + JSON 声明式组态与确定性规则引擎** 的工业人机交互（HMI）与边缘控制项目。

## 核心特点

- **纯 C11 确定性底座（无脚本虚拟机、无 GC 抖动）**：`data_bus`、`modbus_rtu`、`rule_engine`、`config_loader` 采用定长静态池设计，支持宿主机零依赖编译单测与 ESP32-S3 双核 FreeRTOS 直接部署。
- **双核隔离架构**：Core 0 负责 Wi-Fi / HTTP 原子热更新服务；Core 1 专门负责 RS485 Modbus 轮询、滞环+去抖安全联锁规则引擎、LVGL 9 局部增量渲染及任务看门狗（TWDT）。
- **一份 `config.json` 跨端运行**：内置**智慧水泵站 HMI**（`configs/pump_station.json`）与**光储逆变器防逆流 HMI**（`configs/solar_inverter.json`），支持 PC 仿真工作台实时调试并通过 `POST /api/config` 免编译原子热推到真机。
- **工业量产安全强化**：预置 `sdkconfig.defaults.prod`（Secure Boot v2 + Flash Encryption + NVS 加密）和 `partitions.csv`（双 OTA + LittleFS 原子配置分区）。

## 快速上手

### 1. 校验工业配置文件与 ESP32-S3 内存预算
```bash
uv run python tools/validate_config.py configs/pump_station.json configs/solar_inverter.json
```

### 2. 启动 PC 交互式组态仿真工作台（无需硬件）
```bash
uv run python studio/server.py 8765
```
浏览器打开 `http://127.0.0.1:8765`，即可操作 800×480 工业 HMI、切换模板、注入过流/高液位/光伏逆流故障并在线热更新 `config.json`。

### 3. 局域网设备自动发现、热推配置与固件 OTA
```bash
# 扫描局域网所有 EdgeHMI 设备（UDP 56888 广播）
uv run python tools/discover_devices.py

# 一键校验并热推配置到真机或本地仿真器（自动携带 Bearer Token）
uv run python tools/push_config.py configs/solar_inverter.json http://127.0.0.1:8765

# 打包带 SHA-256 完整性校验的离线/OTA 升级包 (.epk)
uv run python tools/pack_epk.py config 1.0.0 configs/pump_station.json update_cfg.epk
uv run python tools/pack_epk.py --verify update_cfg.epk

# 远程推送固件 OTA（支持原始 .bin 或 .epk）
uv run python tools/ota_push.py firmware/build/edgehmi_esp32s3.bin http://edgehmi-xxxx.local --token edgehmi-secret-2026
```

### 4. 编译并烧录到 ESP32-S3（需要 ESP-IDF 5.x）
```bash
cd firmware
idf.py set-target esp32s3
idf.py build flash monitor
# 量产安全强化编译：
# idf.py -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.prod" build
```
