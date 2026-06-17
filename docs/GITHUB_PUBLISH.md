# 发布到 GitHub

本项目依赖 [TuyaOpen](https://github.com/tuya/TuyaOpen) SDK，可作为独立仓库分享应用代码 + PC Bridge，或作为 TuyaOpen 子目录提交。

## 上传前检查（必做）

1. **不要提交真实 UUID / AuthKey**
   - 仓库内只保留 `include/pixel_agent_license.h.example`
   - 本地复制：`cp include/pixel_agent_license.h.example include/pixel_agent_license.h` 后填入自己的授权
   - `include/pixel_agent_license.h` 已在 `.gitignore` 中忽略

2. **不要提交本机绑定文件**
   - `tools/pixel-agent-bridge-ble/.pixel-ble-address`
   - `tools/pixel-agent-bridge-ble/.pixel-ble-bind.json`

3. **不要提交编译产物**
   - `.build/`、`dist/`、`node_modules/`

## 方式 A：独立 GitHub 仓库（推荐分享）

在 TuyaOpen 仓库外单独建库，只包含本应用目录内容：

```bash
# 1. 导出目录（不含密钥与构建产物）
cd /path/to/TuyaOpen/apps/tuya_t5_pixel
rsync -a --exclude '.build' --exclude 'dist' --exclude '.cache' \
  --exclude 'include/pixel_agent_license.h' \
  --exclude 'tools/pixel-agent-bridge-ble/node_modules' \
  --exclude 'tools/pixel-agent-bridge-ble/.pixel-ble-*' \
  tuya_t5_pixel_demo_ble/ ~/tuya_t5_pixel_ble/

cd ~/tuya_t5_pixel_ble
cp include/pixel_agent_license.h.example include/pixel_agent_license.h
# 编辑 include/pixel_agent_license.h 填入你的授权（仅本地，不提交）

git init
git add .
git commit -m "Initial commit: T5 Pixel Agent Monitor BLE"

# 2. 在 GitHub 新建空仓库后推送
git remote add origin https://github.com/YOUR_USER/tuya_t5_pixel_ble.git
git branch -M main
git push -u origin main
```

克隆者需要：

1. 克隆 [TuyaOpen](https://github.com/tuya/TuyaOpen)
2. 将本仓库内容放到 `apps/tuya_t5_pixel/tuya_t5_pixel_demo_ble/`
3. 配置 `include/pixel_agent_license.h` 后 `tos.py build`

### TuyaOpen SDK 补丁（BLE 功能依赖）

本应用除应用目录外，还修改了 TuyaOpen 核心少量文件（需一并使用或自行合入）：

| 文件 | 作用 |
|------|------|
| `src/tuya_cloud_service/ble/ble_mgr.c` | `tuya_ble_set_device_name`、agent keep-alive |
| `src/tuya_cloud_service/ble/ble_mgr.h` | 广播名长度、API 声明 |
| `src/tal_bluetooth/nimble/tkl_bluetooth.c` | `I:` 身份行透传 |

若只上传本仓库，请使用 `patches/tuyaopen-ble-agent.patch` 打补丁，或合入已修改的 TuyaOpen 分支。

## 方式 B：作为 TuyaOpen Fork 的一部分

若你已有 TuyaOpen fork，只提交本目录：

```bash
cd /path/to/TuyaOpen
git add apps/tuya_t5_pixel/tuya_t5_pixel_demo_ble
git status   # 确认无 pixel_agent_license.h、.build、node_modules
git commit -m "Add T5 Pixel Agent Monitor BLE app"
git push origin your-branch
```

## 多板子授权说明（公开仓库）

每块像素屏使用不同 UUID 时，烧录前修改 `include/pixel_agent_license.h`：

| 板子 | chip 标签 | 广播名示例 |
|------|-----------|------------|
| 板 A | `A1B2` | `TYBLE_A1B2` |
| 板 B | `C3D4` | `TYBLE_C3D4` |

PC 端用 `npm run bind:ble` 绑定，以 **chip** 区分，勿将 AuthKey 写入 GitHub。

## Windows / macOS

Bridge 支持 macOS 与 Windows（bleak + Node）。Windows 串口为 `COMx`，详见 `docs/USER_GUIDE.zh-CN.md`。
