<p align="right">
  <a href="README.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# AI Passport · Bathroom Short-Read (v1.1.0)

Chinese-first companion notes live in [README.zh_CN.md](README.zh_CN.md).

- Branch: `feature/bathroom-short-read`
- Release tag: `v1.1.0-bathroom-short-read`
- Wi-Fi: SoftAP + LAN HTTP config page (up to 3 SSIDs / passwords, tried in order, plus DeepSeek API key / preference prompt)
- Merged firmware (after validate): `build/FoloToy-AI-Passport-full.bin`

## Config save

The config page stores up to three 2.4 GHz networks (Wi-Fi 1, Wi-Fi 2, Wi-Fi 3).
Saved names are prefilled. Passwords are not prefilled; leaving one blank keeps
that slot's saved password. A password that is only spaces stores an open
network. Empty names are skipped, and a single filled slot is enough.

On boot, after a save, and on reconnect — including after deep sleep — the
device scans and joins only saved names that are visible. The last slot that
got an IP is stored in NVS (`wpref`, slot number 1–3) and is tried first when
that slot is filled and its name is in the scan. Other visible saved slots
follow in order 1 → 2 → 3. Filled slots absent from the scan are not joined,
so an out-of-range network does not add a connect timeout. If none of the
saved names are visible, the device takes the existing all-failed path
(SoftAP for reconfig, STA keeps scanning) without those timeouts. With no
stored last-success, visible slots are tried in order 1 → 2 → 3. ASCII
letter case is ignored when matching (`MyWifi` matches `mywifi`); the join
uses the scanned spelling, and NVS keeps the name as typed. Hidden
SSIDs are not in the scan, so they are not joined.

The first save that gets online fetches one article and shows that settings were
saved and content is updating. A later save, including a corrected set while
SoftAP is up, still disconnects and joins immediately when a saved name is in
range, so leaving the old network and saving the new one does not wait out the
old slots. It does not replace the article on screen; long-press OK to fetch.
A short disconnect retries the current visible network before rotating or
showing the network-lost notice. The Wi-Fi failure screen appears when a
just-saved set is not visible or every visible saved network has failed its
join budget; search continues in the background. Deep sleep is not required
to start using the new networks. NVS keeps Wi-Fi 1 under the original `ssid` /
`pass` keys. Wi-Fi 2 and Wi-Fi 3 use `ssid2` / `pass2` and `ssid3` / `pass3`.
Older firmware that only wrote Wi-Fi 1 still loads. `wpref` is new and is
ignored by older firmware.

## Debug capture on the config page

The same SoftAP and LAN page (`/`, no login) has two read-only boxes at the
bottom. Labels are Chinese; the paired Chinese notes quote them. Order is
**newest first**. Two saved entries are separated by one blank line. An empty
history shows a short Chinese empty-state word, not sample text.

1. Recent system prompts — the last two `role:system` texts sent to DeepSeek,
   after preference, avoid-topics, and salt are included. This is the text the
   model sees, not the JSON envelope.
2. Recent raw LLM replies — the last two assistant content strings parsed from
   the API, before the `##title##` marker is removed.

A third value overwrites the oldest slot. Both rings survive reboot in NVS
namespace `toilet`:

| Key | Type | Max size (including NUL) |
| --- | --- | --- |
| `spi` | u8 | prompt ring (`count<<2 \| newest slot`) |
| `sp0`, `sp1` | string | 2048 bytes |
| `lri` | u8 | raw-reply ring, same packing |
| `lr0`, `lr1` | blob | 8192 bytes |

Prompts stay NVS strings because they fit under the single-page string limit
(~4000 bytes). Raw replies are blobs so they can hold a full parsed article.
The 64 KiB NVS partition still has room for the article blob and Wi-Fi keys:
two full raw replies plus the article use about 9 of 16 pages, and an update
writes the new blob before erasing the old one.

Over-budget text is cut with an ellipsis plus a Chinese "truncated" tag
instead of failing the fetch. Prompts keep the head (marker at the end). Raw
replies keep the tail (marker at the start) so the title marker remains. A
normal prompt is about 1.4 KB and is stored whole. If the parser fills the
8192-byte article buffer before the closing quote, the stored raw copy ends
with the marker. If NVS reports it is out of space, the raw value is retried
at 4096 then 2048 bytes, still keeping the tail. The system prompt is saved
once the request JSON is built, including when HTTPS fails before a body
exists. Raw text is saved only after assistant content parses.

## Chinese glyphs

UI chrome and the local sample article use `assets/fonts/font_toilet_14` (PLAIN 4bpp subset, ~3600 code points: ASCII + common punctuation + Table of General Standard Chinese Characters level-1). Hardcoded source strings are gated by `tests/test_toilet_font_coverage.py`. DeepSeek body text outside level-1 may still show placeholder boxes.

## No auto-reboot after web flash?

AI Passport uses **USB-Serial/JTAG**. Some web flashers default to `hard-reset` (RTS), which on ESP32-C3 can leave the chip in download mode instead of running the app.

- Power-cycle the device (always works).
- If the tool allows, use **`watchdog-reset`** after write (`esptool --after watchdog-reset`).
- This is host/flasher behavior, not an application image defect.
