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


---

## 二、你需要准备什么

### 硬件

- T5AI Pixel 开发板 + USB 线（至少用于**首次烧录**；日常使用可只用蓝牙）
- Mac 或 Linux/Windows 电脑（**BLE 桥接在 macOS 上测试最充分**）
- 电脑蓝牙开启；系统设置里可搜索到名为 **TYBLE** 的设备

### 软件

| 组件 | 版本建议 | 用途 |
|------|----------|------|
| [TuyaOpen](https://github.com/tuya/TuyaOpen) SDK | 与项目同仓库 | 编译、烧录固件 |
| Node.js | ≥ 18 | 运行 PC Bridge |
| Python 3 | ≥ 3.9 | BLE 子进程（`bleak` 库） |
| Cursor 或 Claude Code | 可选 | Agent 状态同步来源 |

### USB 串口说明（CH342 双口）

板载 USB 会枚举 **两个** 串口，编号较小的一般是 **UART0**：
- **UART0（第一个口）**：烧录固件 + Bridge 串口镜像 + Agent 协议
- **UART1（第二个口）**：系统日志，**不要**接 Bridge
---

## 三、5 分钟快速上手

### 步骤 1：编译并烧录固件

```bash
cd /path/to/TuyaOpen
. ./export.sh
cd apps/tuya_t5_pixel/tuya_t5_pixel_demo_ble

编译
tos.py build
```
产物：
```
dist/ 目录下的 QIO 固件（本地编译，不上传 Git）
```

烧录（**先关闭 Bridge**，否则会占用 UART0）：
```bash
# macOS 示例，请替换为你的实际端口
tos.py flash -p /dev/cu.wchusbserialXXXX
```

### 步骤 2：安装并启动 PC Bridge

```bash
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






健康检查：

```bash
curl http://127.0.0.1:23340/health
```


### 步骤 3：在板子上进入 Agent 模式

1. 上电后默认主页为 **mao 动图**
2. **双击 OK** → 进入 **Agent Monitor**（Clawd 界面）
3. 确保 PC Bridge 已启动；日志出现 `LINK: up` 表示 BLE notify 就绪
4. 在 Cursor / Claude 里正常使用，屏幕会随 Agent 状态变化

---



### Agent Monitor 模式下的按键

> 以下功能**仅在 Agent 模式**生效（双击 OK 进入）。

| 按键 | 无权限弹窗时 | 有权限弹窗时（Claude） |
|------|----------------|------------------------|
| **B 单击** | 清除电脑输入框 `K:clear` | 拒绝 `B:deny` |
| **A 单击** | 退格 `K:backspace` | 允许 `B:allow` |
| **OK 单击** | 回车 `K:enter` | 总是允许 `B:always` |
| **OK 长按** | 开始云端 STT 录音 | 同左 |
| **OK 松开** | 结束录音并粘贴文字 | 同左 |
| **B 长按** | 退出 Agent，回到主页 | 同左 |

Bridge 通过 macOS `osascript` / Linux `xdotool` 模拟键盘，请确保 **IDE 窗口在前台**。

### 状态动画（Clawd）

| 串口协议 `S:` 状态 | 画面 |
|--------------------|------|
| `idle` | 待机 |
| `thinking` | 思考 |
| `working` | 敲键盘 |
| `juggling` | 抛球（多任务） |
| `notify` | 通知 / 权限 |
| `error` | 报错 |
| `happy` | 开心（attention） |

### BLE 蓝色主题

- PC Bridge BLE notify 就绪后发送 `L:1`，Clawd 变为**冷蓝色**
- 断开 BLE 或收到 `L:0` 恢复默认配色
- **仅在 Agent 模式**能看到 Clawd；主页 mao 动图不会变蓝

### 提示音

| 事件 | 旋律 |
|------|------|
| MQTT 云上线 | C5 → E5 → G5 |
| BLE 连接就绪 | A5 → C6（每连接周期一次） |

---

## 通信协议（开发者参考）

行协议，UTF-8，以 `\n` 结尾。

### PC → 设备（下行）

| 行 | 含义 |
|----|------|
| `S:idle` / `S:thinking` / … | 切换可视化状态 |
| `P:tool=Bash` | 权限 pending，并请求进入 Agent 模式 |
| `P:` | 清除权限 UI |
| `L:1` / `L:0` | PC Bridge 链路就绪 / 断开（蓝色主题，不进 Agent） |

### 设备 → PC（上行）

| 行 | 含义 |
|----|------|
| `B:deny` / `B:allow` / `B:always` | 权限审批结果 |
| `K:clear` / `K:backspace` / `K:enter` | 键盘操作 |
| `T:文字内容` | STT 转写结果 |

### BLE GATT（Tuya FD50）

| UUID | 方向 |
|------|------|
| `00000001-…` | PC → 设备 Write |
| `00000002-…` | 设备 → PC Notify |
| `00000003-…` | 读特征 |

Nordic NUS（`6e400001-…`）亦存在，Bridge 优先 FD50。

整体架构：

```
┌──────────────── PC（开发机）────────────────────────────────────┐
│  Cursor / Claude Code Hooks                                      │
│       │ POST http://127.0.0.1:23340/state                        │
│       ▼                                                          │
│  pixel-agent-bridge-ble (bridge.js + ble_bridge.py)              │
│       │ BLE FD50  notify/write  或  USB UART0 115200            │
└───────┼──────────────────────────────────────────────────────────┘
        ▼
┌──────────────── T5 Pixel 固件 ────────────────────────────────────┐
│  pixel_agent_ble.c  →  pixel_agent_bridge.c  →  pixel_agent_clawd │
│       │                              │                            │
│       └──────── 32×32 LED 矩阵 + 蜂鸣器 + 按键 ────────────────────┘
└───────────────────────────────────────────────────────────────────┘
```
---

## 目录结构

```
tuya_t5_pixel_demo_ble/
├── app_default.config          # 构建配置（BLE、AI STT 等）
├── src/
│   ├── tuya_main.c             # 主程序、按键、模式切换
│   ├── pixel_agent_bridge.c    # 串口行协议
│   ├── pixel_agent_ble.c       # BLE 传输与主题
│   └── pixel_agent_clawd.c     # Clawd GIF 渲染
├── tools/pixel-agent-bridge-ble/
│   ├── bridge.js               # HTTP 服务 + 串口 + BLE 调度
│   ├── ble_bridge.py           # Bleak BLE 子进程
│   ├── hooks/                  # IDE Hook 脚本
│   └── install-hooks.js        # 一键安装 Hook
├── dist/                       # 编译产物
└── docs/
    └── USER_GUIDE.zh-CN.md     # 本文档
```

---

## 常见问题

### 1. 屏幕不随 Cursor 变化

- 是否 **双击 OK** 进了 Agent 模式？
- Bridge 是否在跑？`curl http://127.0.0.1:23340/health`
- 是否执行过 `npm run install-hooks`？
- 串口是否接在 **CH342 第一个口（UART0）**？

### 2. BLE 连上但 Clawd 不变蓝

- 需先 **进入 Agent 模式**（主页看不到蓝色）
- Bridge 日志应有 `LINK: up` 和 `-> device (link): L:1`
- 固件串口日志应有 `agent ble: host link L:1` 或 `agent bridge: serial L:1`

### 3. 按键有时没反应

- 仅在 **Agent 模式** 下 B/A/OK 才发给 PC
- BLE 模式下按键走 **Notify 上行**；请保持 Bridge 运行且 `ble_linked: true`
- IDE 窗口需在前台；macOS 需授予**辅助功能**权限（自动化控制键盘）
- 权限弹窗时：B=拒绝，A=允许，OK=总是允许；此时不退格/回车

### 4. 按键执行两次

- 确保使用**最新** `bridge.js` 与固件：BLE 成功时不应同时走串口上行
- 重启 Bridge：`node bridge.js --ble`

### 5. 烧录失败

- 关闭所有 Bridge / 串口监视器
- 确认端口为 UART0：`npm run list-ports`

### 6. BLE 扫描不到 TYBLE

- 固件需已烧录 demo_ble 版本
- 系统蓝牙中「忽略」旧配对后重新连接
- `npm run scan:ble` 查看广播

### 7. 与串口版 demo 冲突

- 不要同时运行两个 Bridge
- Hook 端口不同（23335 vs 23340），但 Hook 脚本路径会互相覆盖，只保留一套即可

---


---

## 许可证与致谢

- 固件基于 [TuyaOpen](https://github.com/tuya/TuyaOpen) SDK
- Clawd 主题 GIF 来自 clawd-on-desk / buddy_pixel 资源
- PC Bridge：`pixel-agent-bridge-ble`（MIT）
