<p align="right">
  <a href="README.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# AI Passport · Bathroom Short-Read (v1.0.1)

Chinese-first companion notes live in [README.zh_CN.md](README.zh_CN.md).

- Branch: `feature/bathroom-short-read`
- Release tag: `v1.0.1-bathroom-short-read`
- Wi-Fi: SoftAP + LAN HTTP config page (up to 3 SSIDs / passwords, tried in order, plus DeepSeek API key / preference prompt)
- Merged firmware (after validate): `build/FoloToy-AI-Passport-full.bin`

## Config save

The config page stores up to three 2.4 GHz networks (Wi-Fi 1, Wi-Fi 2, Wi-Fi 3).
Saved names are prefilled. Passwords are not prefilled; leaving one blank keeps
that slot's saved password. A password that is only spaces stores an open
network. Empty names are skipped, and a single filled slot is enough.

On boot, after a save, and while searching, the device tries non-empty slots in
order 1 then 2 then 3. The first association that gets an IP wins and the rest
are not tried. A slot that is missing, rejects the password, or times out is
abandoned after the same per-network failure budget as before, then the next
slot is installed. The Wi-Fi failure screen and the still-up SoftAP appear only
after every filled slot has failed. Search continues in the background so a
network that shows up later can still connect.

The first save that gets online fetches one article and shows that settings were
saved and content is updating. A later save, including a corrected set while
SoftAP is up, disconnects, installs the new set starting at Wi-Fi 1, and joins
immediately. It does not replace the article on screen; long-press OK to fetch.
Deep sleep is not required to start using the new networks. NVS keeps Wi-Fi 1
under the original `ssid` / `pass` keys. Wi-Fi 2 and Wi-Fi 3 use `ssid2` /
`pass2` and `ssid3` / `pass3`. Older firmware that only wrote Wi-Fi 1 still
loads.

## Chinese glyphs

UI chrome and the local sample article use `assets/fonts/font_toilet_14` (PLAIN 4bpp subset, ~3600 code points: ASCII + common punctuation + Table of General Standard Chinese Characters level-1). Hardcoded source strings are gated by `tests/test_toilet_font_coverage.py`. DeepSeek body text outside level-1 may still show placeholder boxes.

## No auto-reboot after web flash?

AI Passport uses **USB-Serial/JTAG**. Some web flashers default to `hard-reset` (RTS), which on ESP32-C3 can leave the chip in download mode instead of running the app.

- Power-cycle the device (always works).
- If the tool allows, use **`watchdog-reset`** after write (`esptool --after watchdog-reset`).
- This is host/flasher behavior, not an application image defect.
