<p align="right">
  <strong>简体中文</strong> · <a href="README.md">English</a>
</p>

# AI护照 · 解闷阅读（v1.0.0）

蹲厕时读约 3 分钟的个性化中文短文，读完就起身。本玩法在分支 `feature/bathroom-short-read`，发布 tag 为 `v1.0.0-bathroom-short-read`。

## 刷机文件

验证通过后：

`build/FoloToy-AI-Passport-full.bin`（整片从 `0x0` 写入）

未授权刷机前请自行确认目标设备。合并镜像可能重置 NVS（含 Wi‑Fi / API Key）。


## 刷机后不自动重启？

AI护照走 **USB-Serial/JTAG**。部分网页刷机工具默认 `hard-reset`（依赖 RTS），在 C3 上可能只回到下载模式而不跑应用，看起来像「刷完没重启」。

- 可先 **拔电再开机**（一定能进应用）。
- 若工具支持，把写完后的动作改成 **`watchdog-reset`**（esptool：`--after watchdog-reset`）。
- 这与本玩法固件内容无关；官方/本仓用 esptool 且 after-reset 正确时应能自动起来。

## 配网与配置页

本版采用 **SoftAP 热点 + 网页配置**（未接微信小程序 BLUFI；后续可再改）。

1. 开机后若尚未保存 Wi‑Fi：屏幕提示配网，手机连接热点 **`AIPassport-XXXX`**（XXXX 为 MAC 后缀）。
2. 浏览器打开 **`http://192.168.4.1/`**。
3. 填写：
   - 2.4 GHz Wi‑Fi 名称与密码
   - DeepSeek API Key
   - 偏好提示词（喜欢读什么）
   - 可选模型，默认 `deepseek-chat`
4. 点保存。设备切到 STA 联网后，屏幕显示局域网 IP；之后可在同一 Wi‑Fi 下用 **`http://设备IP/`** 再改配置。

断网后会防抖片刻再开 SoftAP；家用 Wi‑Fi 恢复后后台约每 8 秒重试，连上后自动关热点并恢复局域网配置页。

**请勿在日志或截图里暴露 API Key。**

## 按键

| 操作 | 作用 |
| --- | --- |
| 上 / 下 | 翻页 |
| 长按 OK | 冷却外：询问「是否获取新内容」；冷却内：提示还剩 x 分钟 |
| 任意键（弹窗） | 关闭提示（确认拉取框内短按 OK 仍确认） |
| 上键（弹窗时） | 关闭弹窗 |
| 双击 OK | 深度睡眠 |
| 任意功能键 | 从深度睡眠唤醒（应用重启） |
| 电源键 | 硬件开关机；睡眠中不要长按 |

## 其它行为

- 目标短文约 1000–1200 字；成功拉取覆盖旧文并写时间戳。
- 每 30 分钟最多成功拉取 1 次；x 为真实剩余分钟（向上取整）。
- 顶部：左 Wi‑Fi 状态，中亮屏时长 `分:秒`，右时钟（联网后 SNTP）。
- 亮屏约 5 分钟提醒一次起身；10 分钟无操作自动深度睡眠。
- 开机 / 亮屏 **不会** 自动拉取。


## 中文显示

正文使用 `font_toilet_20`（约 20px）、状态栏用 `font_toilet_14`；同为 PLAIN 4bpp/2bpp 子集（ASCII + 常用标点 + 一二级字）。无白卡片，正文浅色字画在青绿背景上；分页按行高整行换页，不裁半行。配置页保存后屏幕提示「设置修改完成，正在更新内容」并自动拉取一篇。源码中文由 `tests/test_toilet_font_coverage.py` 门禁；字库外的 LLM 生僻字仍可能方框。

## DeepSeek

在 [DeepSeek 开放平台](https://platform.deepseek.com/) 开户并创建 API Key。轻量阅读通常消耗很少；额度与计费以官网为准。
