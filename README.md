# T5 Pixel Agent Monitor — BLE 版

在 T5AI Pixel 32×32 矩阵屏上实时显示 **Cursor / Claude Code** 的 Agent 状态（Clawd 小螃蟹动画），支持 **蓝牙无线** + USB 串口双通道，以及按键遥控 IDE、语音转文字。

| | |
|---|---|
| **固件版本** | `1.0.2-ble` |
| **硬件** | TUYA T5AI Pixel |
| **BLE 广播名** | `TYBLE_XXXX`（UUID 后 4 位，如 `TYBLE_A1B2`） |
| **Bridge 端口** | `http://127.0.0.1:23340` |

## 功能一览

- Agent 状态屏：idle / thinking / working / juggling / notify / error / happy
- **BLE 无线**连接 Mac/PC，连上后 Clawd **蓝色主题**
- 按键：清除 / 退格 / 回车；Claude 权限审批（B/A/OK）
- 长按 OK：云端 STT，文字粘贴到电脑
- 主页 mao 动图 + 写轮眼、俄罗斯方块、贪吃蛇等娱乐模式

## 快速开始

```bash
# 1. 编译烧录（TuyaOpen 根目录已 source export.sh）
cd apps/tuya_t5_pixel/tuya_t5_pixel_demo_ble
tos.py build
tos.py flash -p /dev/cu.wchusbserialXXXX   # 先停 Bridge

# 2. PC Bridge
cd tools/pixel-agent-bridge-ble
npm install && npm run setup:ble
npm run scan:ble
npm run bind:ble -- <address>   # 保存 chip 标签，防连错板
npm run install-hooks
npm run start:ble               # 多板环境推荐纯 BLE

# 3. 板子双击 OK → 进入 Agent 模式
```

健康检查：`curl http://127.0.0.1:23340/health`

## 文档

| 文档 | 说明 |
|------|------|
| **[docs/USER_GUIDE.zh-CN.md](docs/USER_GUIDE.zh-CN.md)** | **完整介绍与使用指南**（推荐给新用户） |
| [docs/GITHUB_PUBLISH.md](docs/GITHUB_PUBLISH.md) | 发布与脱敏说明 |
| [patches/README.md](patches/README.md) | TuyaOpen SDK 补丁 |

## 与串口版 `tuya_t5_pixel_demo` 的区别

| 项 | 串口版 | BLE 版（本项目） |
|----|--------|------------------|
| 版本 | 1.0.1 | 1.0.2-ble |
| 传输 | USB 串口 | BLE + 串口 |
| Bridge 端口 | 23335 | **23340** |
| 配置目录 | `~/.pixel-agent-bridge/` | `~/.pixel-agent-bridge-ble/` |

**不要**对两个项目同时运行 `install-hooks` 或同时启动两套 Bridge。

## Bridge 命令速查

```bash
npm run start:dual          # 推荐
npm run start:ble           # 仅 BLE
npm run start:serial        # 仅串口
npm run scan:ble            # 扫描 TYBLE（列出 address）
npm run bind:ble -- <uuid>  # 绑定你的像素屏（防连错）
npm run list-ports          # 列出 WCH 串口
```

## 目录

```
src/                    # 固件（tuya_main, pixel_agent_ble, clawd…）
tools/pixel-agent-bridge-ble/   # Node + Python BLE Bridge
# dist/ 为本地编译产物，已 gitignore
docs/USER_GUIDE.zh-CN.md
```

## 授权配置（首次克隆）

```bash
cp include/pixel_agent_license.h.example include/pixel_agent_license.h
# 编辑填入涂鸦平台申请的 UUID / AuthKey（勿提交到 Git）
```

## 发布到 GitHub

见 **[docs/GITHUB_PUBLISH.md](docs/GITHUB_PUBLISH.md)**。SDK 补丁见 **[patches/](patches/)**。

## 构建产物

本地 `tos.py build` 后生成于 `dist/`（已 gitignore，不上传）。
