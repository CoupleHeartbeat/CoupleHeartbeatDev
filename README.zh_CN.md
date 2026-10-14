<p align="right">
  <a href="README.md">English</a> · <strong>简体中文</strong>
</p>

# CoupleHeartbeat 设备固件

一对情侣挂饰设备的固件：两台设备通过低功耗蓝牙互相感知对方是否在身边，
并且支持直接对设备说话、由云端大模型回答。硬件是 FoloToy AI Passport
（ESP32-C3，8 MB Flash，无 PSRAM），开发环境为 ESP-IDF 5.5.3。

## 功能

- **工牌**：主界面显示时间、昵称与电量；电量左侧是 Wi-Fi 状态图标，屏幕四周是
  靠近光效。
- **靠近光效**：每台设备持续广播携带自身身份的心跳。配对设备进入范围后不切换
  页面：工牌界面保持原样，屏幕四周亮起呼吸光效。距离越近，光效越亮、呼吸越快，
  心跳音也越急；两台设备贴得很近时达到最强档。姓名下方有一行小字显示当前距离与
  信号强度，对方离开后该行自动隐藏。
- **AI 对话**：长按 OK 进入语音页。说话即录音，识别成文字后交给大模型，再把
  回复合成语音从喇叭播出来。
- **配网与配置**：Wi-Fi 凭据与大模型 API Key 都由配套 App 经 BLE 下发，只保存
  在设备本地 NVS 中。设备还能按 App 的请求扫描附近网络，让 App 列出设备真正能
  看到的网络（只支持 2.4GHz）。

## BLE 接口

一个主服务（`0000ffe0-0000-1000-8000-00805f9b34fb`）下提供这些特征值：

| UUID | 权限 | 用途 |
| --- | --- | --- |
| 0xFFE1 | 写 | 配对配置（角色、配对 ID 与密钥、对方地址、屏幕配对码） |
| 0xFFE2 | 读 / 写 | 工牌昵称 |
| 0xFFE3 | 读 / 通知 | 设备状态（电量、模式、音量、对方信号强度） |
| 0xFFE4 | 写 | Wi-Fi 凭据，或清除配网 |
| 0xFFE5 | 读 / 通知 | 配网状态（状态、SSID、IP、信号） |
| 0xFFE6 | 读 / 写 | 大模型 API Key；读取只返回是否已配置 |
| 0xFFE7 | 读 / 写 | 扫描附近 Wi-Fi：写扫描请求，读候选网络列表 |

API Key 不会被日志打印，也不会通过读取接口回传明文。

## 编译

```bash
. "$IDF_PATH/export.sh"          # ESP-IDF 5.5.3
idf.py build
idf.py merge-bin -o FoloToy-AI-Passport-full.bin
```

用支持数据传输的 USB 线把合并镜像写到偏移 `0x0`。

## 自检

```bash
./tools/validate.sh --static     # 仓库检查 + 主机测试
./tools/validate.sh --firmware   # 编译 + 合并镜像校验
```

主机测试覆盖靠近判定逻辑、Wi-Fi 凭据解析与大模型响应解析；这三块都刻意不依赖
ESP-IDF 与 LVGL，因此可以直接在电脑上运行。

## 配套 App

负责配对与配置设备的手机应用在
https://github.com/CoupleHeartbeat/CoupleHeartbeatApp

## 许可证

MIT。本固件派生自 FoloToy AI Passport 项目，请保留 `LICENSE` 中的版权声明。
