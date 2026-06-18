# T5 Pixel Agent Monitor（BLE 版）介绍与使用指南

> 适用硬件：涂鸦 **T5AI Pixel** 开发板（32×32 LED 矩阵 + OK / A / B 三键）

---

## 一、这是什么？

**T5 Pixel Agent Monitor（BLE 版）** 是一块放在桌面上的 **AI 编程助手状态屏**：当你在 **Cursor / Claude Code** 里写代码、跑工具、等权限时，板子上的 **Clawd 小螃蟹** 会跟着切换动画（思考、敲键盘、抛球、报错等）。

### 主要功能

| 按键 | 操作 |
|------|------|
| **OK 单击** | 切换动画（特效 / 像素画 / 写轮眼等） |
| **OK 双击** | **进入 Agent Monitor** |
| **OK 长按** | 进入忍者跑酷 |
| **A 单击** | 切换像素画 |
| **A 双击** | 进入 AI 语音频谱模式 |
| **A 长按** | 进入贪吃蛇 |
| **B 长按** | 进入俄罗斯方块  |
| **B 双击** | 进入沙盒模拟 |

视频展示：https://www.douyin.com/video/7652591946045865083

## 快速开始

```bash
# 1. 编译烧录（TuyaOpen 根目录已 source export.sh）
cd apps/tuya_t5_pixel/tuya_t5_pixel_ble
tos.py build
tos.py flash -p /dev/cu.wchusbserialXXXX   # 先停 Bridge

# 2. PC Bridge
cd tools/pixel-agent-bridge-ble
npm install                # 安装依赖
npm run setup:ble          # 安装 Python bleak：pip install bleak

npm run install-hooks      # 安装agent Hooks
    - **Claude Code**：`~/.claude/settings.json`（含权限阻塞 `/permission`）
    - **Cursor**：`~/.cursor/hooks.json`（仅状态同步）
    - 若存在 Codex / Copilot 目录，也会尝试写入

npm run scan:ble           # 扫描附件蓝牙设备
npm run bind:ble -- "xxx"  # 绑定板子 


常用启动方式：
npm start                  #推荐**：BLE 连上走无线；断线时串口兜底 
npm run start:ble          #仅蓝牙连接
npm run start:serial       #仅串口

# 3. 板子双击 OK → 进入 Agent 模式
```

健康检查：`curl http://127.0.0.1:23340/health`

## 文档

| 文档 | 说明 |
|------|------|
| **[docs/USER_GUIDE.zh-CN.md](docs/USER_GUIDE.zh-CN.md)** | **完整介绍与使用指南**（推荐给新用户） |
| [docs/GITHUB_PUBLISH.md](docs/GITHUB_PUBLISH.md) | 发布与脱敏说明 |
| [patches/README.md](patches/README.md) | TuyaOpen SDK 补丁 |

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