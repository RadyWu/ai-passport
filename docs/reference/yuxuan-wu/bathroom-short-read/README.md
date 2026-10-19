<p align="right">
  <a href="README.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# AI Passport · Bathroom Short-Read (v1.0.0)

Chinese-first companion notes live in [README.zh_CN.md](README.zh_CN.md).

- Branch: `feature/bathroom-short-read`
- Release tag: `v1.0.0-bathroom-short-read`
- Wi-Fi: SoftAP + LAN HTTP config page (SSID / password / DeepSeek API key / preference prompt)
- Merged firmware (after validate): `build/FoloToy-AI-Passport-full.bin`

## Config save

The first save that gets online fetches one article and shows that settings were saved and content is updating.
A later save, including a new or corrected SSID while SoftAP is up, applies those
credentials and joins that network immediately. It does not replace the article
on screen; long-press OK to fetch. A failed join shows a Wi-Fi error and leaves
SoftAP up. Deep sleep is not required to start using the new network.

## Chinese glyphs

UI chrome and the local sample article use `assets/fonts/font_toilet_14` (PLAIN 4bpp subset, ~3600 code points: ASCII + common punctuation + Table of General Standard Chinese Characters level-1). Hardcoded source strings are gated by `tests/test_toilet_font_coverage.py`. DeepSeek body text outside level-1 may still show placeholder boxes.

## No auto-reboot after web flash?

AI Passport uses **USB-Serial/JTAG**. Some web flashers default to `hard-reset` (RTS), which on ESP32-C3 can leave the chip in download mode instead of running the app.

- Power-cycle the device (always works).
- If the tool allows, use **`watchdog-reset`** after write (`esptool --after watchdog-reset`).
- This is host/flasher behavior, not an application image defect.
