<p align="right">
  <a href="README.md">English</a> · <strong>简体中文</strong>
</p>

# CoupleHeartbeat · 设备固件

![platform](https://img.shields.io/badge/ESP32--C3-ESP--IDF%205.5.3-blue)
![flash](https://img.shields.io/badge/flash-8%20MB-informational)
![license](https://img.shields.io/badge/license-MIT-green)

一对情侣挂饰设备的固件。两台设备通过低功耗蓝牙互相感知是否在身边，并支持
按住说话、由云端大模型回答。硬件平台为 **FoloToy AI Passport**（ESP32-C3，
8 MB Flash，无 PSRAM）。

> **真正的硬约束是内存。** 整机可用 RAM 约 170 KB，Wi-Fi、BLE、LVGL 与音频
> 通路都跑起来后只剩约 30 KB。TLS 握手、Wi-Fi 缓冲、甚至屏幕绘制缓冲都是
> 围绕这个预算来设计的。

## 功能

| 模块 | 行为 |
| --- | --- |
| **工牌** | 显示时间、昵称、日期（`2026年10月17日 周五`）、电量、Wi-Fi 图标，屏幕四周是靠近光效。 |
| **靠近感知** | 每台设备持续广播 BLE 心跳。对方进入范围后不切换页面，工牌四周的光效随距离变亮、变快，心跳音同步加快；姓名下方一行显示距离与信号强度，对方离开后自动隐藏。 |
| **语音对话** | 长按 OK 说话，松手后音频上传识别，识别文本交给对话模型，回复以文字显示在屏幕上。按设计不做语音播报，TTS 已移除。 |
| **设置菜单** | 短按 OK 进入菜单（关于设备、立即休眠、恢复出厂设置），UP/DOWN 调节音量并在屏幕左上角显示当前音量。 |
| **低功耗** | 无操作且对方不在附近 60 秒后，背光降到 10%，只保留姓名、时间、年月日；15 分钟无操作时先显示 10 秒警告再深睡，菜单里也可手动“立即休眠”。 |
| **配网与配置** | Wi-Fi 凭据与大模型 API Key 都由配套 App 经 BLE 下发，只保存在设备 NVS；设备可按请求扫描附近 2.4GHz 网络，并接受手机下发的时间与时区。 |
| **中文字库** | 预生成 GB2312 全集（6 763 汉字 + 全角标点，16px/20px 共 7 118 字形）放在 Flash 中按需取字，动态中文文本不再缺字，且不占 RAM。 |

## BLE 接口

一个主服务（`0000ffe0-0000-1000-8000-00805f9b34fb`）下提供：

| UUID | 权限 | 用途 |
| --- | --- | --- |
| 0xFFE1 | 写 | 配对配置（角色、配对 ID 与密钥、对方地址、屏幕配对码） |
| 0xFFE2 | 读 / 写 | 工牌昵称 |
| 0xFFE3 | 读 / 通知 | 设备状态（电量、模式、音量、对方信号） |
| 0xFFE4 | 写 | Wi-Fi 凭据，或清除配网 |
| 0xFFE5 | 读 / 通知 | 配网状态（状态、SSID、IP、信号） |
| 0xFFE6 | 读 / 写 | 大模型 API Key；读取只返回是否已配置 |
| 0xFFE7 | 读 / 写 | 扫描附近 Wi-Fi：写扫描请求，读候选网络列表 |
| 0xFFE8 | 写 | 时间同步：写 `{"unix":秒,"tz_offset":秒}` |

读取接口从不回传 API Key 明文，Key 也不会写入日志。

## 编译

```bash
. "$IDF_PATH/export.sh"        # ESP-IDF 5.5.3
./tools/validate.sh --firmware # 编译、合并并校验镜像
```

校验通过的合并镜像位于
`build/firmware/<sha256>/FoloToy-AI-Passport-full.bin`，用支持数据传输的
USB 线写到偏移 `0x0`：

```bash
python3 -m esptool --chip esp32c3 -p <PORT> -b 460800 \
  write_flash --flash_mode dio --flash_size 8MB --flash_freq 80m \
  0x0 build/firmware/<sha256>/FoloToy-AI-Passport-full.bin
```

### 分区布局与数据保留

| 名称 | 类型 | 偏移 | 大小 | 用途 |
| --- | --- | --- | --- | --- |
| factory | app | 0x10000 | ~4.94 MiB | 应用 |
| nvs | data | 0x500000 | 24 KiB | 配对 / Wi-Fi / API Key / 音量 / 上次时间 |
| phy_init | data | 0x506000 | 4 KiB | 射频校准 |
| user | data (FAT) | 0x507000 | ~2.97 MiB | 用户文件，挂载在 `/userdata` |

所有数据分区都位于应用分区**之后**，合并镜像只覆盖 `0x0` 到应用结尾，因此
刷写新固件不会清空 NVS 配置与用户文件。`user` 是带磨损均衡的 FAT 卷，
首次使用时自动格式化。

从旧布局（NVS 在 `0x9000`）迁移时旧 NVS 会被清空一次：首次刷入后需要重新
配对并重新配置 Wi-Fi。

## 目录结构

```
main/              应用层：工牌 UI、AI 对话、BLE/GATT、功耗、存储
components/bsp/    板级支持：显示、音频、按键、电池、I2C
assets/fonts/      生成的 LVGL 字体（GB2312 全集）
tools/             校验/归档/验证脚本与字库生成器
tests/             主机测试（靠近逻辑、解析器、BSP 契约）
```

## 自检

```bash
./tools/validate.sh --static     # 仓库检查 + 主机测试
./tools/validate.sh --firmware   # 编译 + 合并镜像校验
python3 tools/gen_couple_fonts.py --check   # 中文字形覆盖门禁
```

主机测试覆盖靠近判定、Wi-Fi 凭据解析、模型响应解析与低功耗策略；这些模块
刻意不依赖 ESP-IDF 与 LVGL，可直接在电脑上运行。

## 配套 App

负责配对与配置的手机应用：
https://github.com/CoupleHeartbeat/CoupleHeartbeatApp

## 许可证

MIT。本固件派生自 FoloToy AI Passport 项目，请保留 [`LICENSE`](LICENSE) 中的
版权声明。
