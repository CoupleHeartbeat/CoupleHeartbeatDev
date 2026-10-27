<p align="right">
  <a href="README.md">English</a> · <strong>简体中文</strong>
</p>

# CoupleHeartbeat · 设备固件

![platform](https://img.shields.io/badge/ESP32--C3-ESP--IDF%205.5.3-blue)
![flash](https://img.shields.io/badge/flash-8%20MB-informational)
![license](https://img.shields.io/badge/license-MIT-green)

一对情侣挂饰设备的固件。两台设备通过低功耗蓝牙互相感知是否在身边；按住说话时
录音经蓝牙实时上传到配套手机，由手机完成语音识别与大模型请求，再把回复文本通过
蓝牙回传到设备显示。硬件平台为 **FoloToy AI Passport**（ESP32-C3，8 MB Flash，
无 PSRAM）。

> **设备不再连接任何网络。** Wi-Fi、TLS 与设备端模型密钥全部移除，固件只保留
> BLE、LVGL、录音通路和小型功耗状态机，约 170 KB 的 RAM 大部分留给界面与
> 蓝牙协议栈。

## 功能

| 模块 | 行为 |
| --- | --- |
| **工牌** | 显示时间、昵称、日期（`2026年10月17日 周五`）、电量、App 连接图标，屏幕四周是靠近光效。 |
| **靠近感知** | 每台设备持续广播 BLE 心跳。对方进入范围后不切换页面，工牌四周的光效随距离变亮、变快，心跳音同步加快；姓名下方一行显示距离与信号强度，对方离开后自动隐藏。 |
| **语音对话** | 长按 OK 说话，录音边采边通过 BLE 上传，手机返回大模型回复文本并显示在屏幕上。按设计不做语音播报，TTS 已移除。 |
| **设置菜单** | 短按 OK 进入菜单（关于设备、立即休眠、恢复出厂设置），UP/DOWN 调节音量并在屏幕左上角显示当前音量。 |
| **低功耗** | 无操作且对方不在附近 60 秒后，背光降到 10%，只保留姓名、时间、年月日；15 分钟无操作时先显示 10 秒警告再深睡，菜单里也可手动“立即休眠”。手机会话保持连接时会抑制深睡。 |
| **配置** | 配对信息、昵称与时间都由配套 App 经 BLE 写入；设备不保存 Wi-Fi 凭据，也不保存任何模型 API Key。 |
| **中文字库** | 预生成 GB2312 全集（6 763 汉字 + 全角标点，16px/20px 共 7 118 字形）放在 Flash 中按需取字，动态中文文本不再缺字，且不占 RAM。 |

## BLE 接口

一个主服务（`0000ffe0-0000-1000-8000-00805f9b34fb`）下提供：

| UUID | 权限 | 用途 |
| --- | --- | --- |
| 0xFFE1 | 写 | 配对配置（角色、配对 ID 与密钥、对方地址、屏幕配对码） |
| 0xFFE2 | 读 / 写 | 工牌昵称 |
| 0xFFE3 | 读 / 通知 | 设备状态（电量、模式、音量、对方信号） |
| 0xFFE8 | 写 | 时间同步：写 `{"unix":秒,"tz_offset":秒}` |
| 0xFFE9 | 通知 | 语音上行：6 字节头 + PCM，设备 → 手机 |
| 0xFFEA | 写 | 回复下行：`[status][UTF-8 文本]`，手机 → 设备 |

### 语音上行协议（0xFFE9）

音频为 8 kHz / 16 bit / 单声道。录音最多 4 秒（64 000 字节），边录边发，
设备端不分配 64 KB 缓冲。

| 偏移 | 字段 |
| --- | --- |
| 0 | 类型：`1` BEGIN，`2` DATA，`3` END，`4` CANCEL |
| 1 | 保留，0 |
| 2..3 | offset，小端 |
| 4..5 | PCM 总字节数，小端 |
| 6.. | PCM 数据（BEGIN / END 包为空） |

`END` 包的 offset 就是实际有效字节数，不补静音。手机侧重组后转成 WAV，调用
语音识别与对话模型，并在 30 秒回复窗口内把 `[0][文本]`（成功）或
`[1][原因]`（失败）写入 0xFFEA。

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
| nvs | data | 0x500000 | 24 KiB | 配对 / 音量 / 上次时间 |
| phy_init | data | 0x506000 | 4 KiB | 射频校准 |
| user | data (FAT) | 0x507000 | ~2.97 MiB | 用户文件，挂载在 `/userdata` |

所有数据分区都位于应用分区**之后**，合并镜像只覆盖 `0x0` 到应用结尾，因此
刷写新固件不会清空 NVS 配置与用户文件。`user` 是带磨损均衡的 FAT 卷，
首次使用时自动格式化。

从带 Wi-Fi 的旧固件升级时，已保存的配对信息仍然保留，但设备不会再联网；
若配对数据有变化，请在 App 里重新配对。

## 目录结构

```
main/              应用层：工牌 UI、AI 对话、BLE/GATT、语音上行、功耗
components/bsp/    板级支持：显示、音频、按键、电池、I2C
assets/fonts/      生成的 LVGL 字体（GB2312 全集）
tools/             校验/归档/验证脚本与字库生成器
tests/             主机测试（靠近逻辑、功耗策略、BSP 契约）
```

## 自检

```bash
./tools/validate.sh --static     # 仓库检查 + 主机测试
./tools/validate.sh --firmware   # 编译 + 合并镜像校验
python3 tools/gen_couple_fonts.py --check   # 中文字形覆盖门禁
```

主机测试覆盖靠近判定与低功耗策略；这些模块刻意不依赖 ESP-IDF 与 LVGL，
可直接在电脑上运行。

## 配套 App

负责配对、语音识别与对话的手机应用：
https://github.com/CoupleHeartbeat/CoupleHeartbeatApp

配对时两台手机各连自己的一台设备，再通过二维码交换 `pair_id` / `pair_key` /
对方地址，完成两台设备的串联。

## 许可证

MIT。本固件派生自 FoloToy AI Passport 项目，请保留 [`LICENSE`](LICENSE) 中的
版权声明。
