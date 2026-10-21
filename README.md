<p align="right">
 <a href="README.en.md">English</a> · <strong>简体中文</strong>
</p>

# 蹲坑阅读神器（解闷阅读）

> [!IMPORTANT]
> **学玩法、在浏览器里直接刷机，请先打开社区页面：**
>
> ## [蹲坑阅读神器 · 社区玩法（学玩法 + 浏览器刷机）](https://ai-passport.folotoy.cn/plays/1427/?v=3375-3)
>
> 这是了解本玩法、并直接在浏览器刷入固件的最佳方式。安装与刷机请走这一页。

![蹲坑阅读神器](docs/assets/cover-bathroom-short-read.jpg)

基于 [FoloToy AI Passport](https://github.com/FoloToy/ai-passport) 的中文短文阅读玩法。  
当前版本：**v1.2.0** · 玩法代码在分支 `feature/bathroom-short-read`。  
只需自行下载固件包时，备用入口是 [Release v1.2.0](https://github.com/RadyWu/ai-passport/releases/tag/v1.2.0-bathroom-short-read)。

> 本仓库面向中文用户。玩法代码在分支 `feature/bathroom-short-read`。

---

## 简介

蹲坑上厕所要是刷手机，一不小心就被信息流拽走，入了资本家设的局，又或者玩游戏上头，不知不觉蹲了半个小时，不仅腿麻还影响健康。不带手机又无聊得很。

现在有了这款蹲坑阅读神器，蹲坑的人有福了！把 AI Passport 变成一个 AI 短文生成器，只要填入 DeepSeek API Key，就可以按照配置好的提示词自动生成你喜欢读的内容，每一次拉取的文章话题都不一样，常看常新。每次生成文章的字数大约是一千多字，正好够蹲坑时阅读 3 到 5 分钟。为了不让你一直刷新一直读舍不得起来，程序设定了每 10 分钟只能获取 1 次，读完一篇就该起来啦！

默认使用最快最便宜的模型，API token 消耗非常非常微不足道，开发过程中一天拉取几十次才消耗了几分钱，安装 DeepSeek Harness 送的 6 元 token 足够看到天荒地老……

---

## 界面示意

![界面示意](docs/assets/ui-mock-bathroom-short-read.jpg)

> **仅供示意，不是实机截图。** 上图为 AI 合成的界面示意图，用于帮助理解屏幕布局；实机显示以刷入固件后为准。

## 使用方法

* 首次准备（配网）：刷入本玩法完整固件后开机，屏幕会提示配网。用手机连上设备热点 `AIPassport-XXXX`（XXXX 为设备后缀），浏览器打开 `http://192.168.4.1/`。在配置页填写：**2.4 GHz** Wi‑Fi 名称与密码(可以填写3个WIFI，系统会轮流试)、**DeepSeek API Key**、喜欢读什么的偏好提示词。点保存后，设备会立刻去连网。连上后屏幕显示局域网 IP。配网成功会自动拉取一篇短文。
* 开始阅读：单击上 / 下键翻页；双击上键可直接回到第一页。
* 调整亮度：长按上 / 下键，可以调整屏幕亮度档位。
* 获取新短文：长按 OK，按提示确认后拉取（每 10 分钟只能获取一次）。
* 双击 OK键，或 10 分钟无操作，设备进入深度睡眠省电；按任意功能键唤醒，会接着显示上次读到的位置。
* 使用 5 分钟后会提醒该起身了。
* 同一 Wi‑Fi 下也可在浏览器打开 `http://设备IP/` 再改配置；已配过网时输入栏会预填上次的 Wi‑Fi、API Key 等信息。
* 请勿在截图或对外分享里露出 API Key。DeepSeek 需自行在官网申请 API Key 并留意额度。

---

## 下载与刷机

**推荐：** 在社区页面了解玩法，并直接在浏览器里刷机：

**[蹲坑阅读神器 · 社区玩法（学玩法 + 浏览器刷机）](https://ai-passport.folotoy.cn/plays/1427/?v=3375-3)**

GitHub Release 是备用入口，给需要自行下载固件包的人：

1. 打开 [v1.2.0 Bathroom Short Read](https://github.com/RadyWu/ai-passport/releases/tag/v1.2.0-bathroom-short-read)
2. 下载完整固件 `FoloToy-AI-Passport-full-v1.2.0-1021.bin`（同一 Release 中的 `FoloToy-AI-Passport-full.bin` 为最新完整固件，内容相同）
3. 用 FoloToy 官方网页刷机工具从 `0x0` 写入（须用完整合并固件，不要只用 app 单分区镜像）

刷机可能重置已保存的 Wi‑Fi / API Key，请按上面「首次准备」重新配网。

---

## 源码与分支

| 项目 | 说明 |
| --- | --- |
| 玩法分支 | `feature/bathroom-short-read` |
| 发布 Tag | `v1.2.0-bathroom-short-read` |
| 上游仓库 | [FoloToy/ai-passport](https://github.com/FoloToy/ai-passport) |

本页面（根目录 `README.md`）放在默认分支 `main`，方便打开 GitHub 仓库首页即可看到中文说明。

---

## 许可

与上游一致，见仓库 `LICENSE`（MIT）。
