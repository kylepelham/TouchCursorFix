<div align="center">

<img src="assets/icon.png" width="96" alt="TouchCursorFix icon">

# TouchCursorFix

**Tap your touchscreen without losing your mouse cursor.**

Single ~80 KB exe · no installer · no admin · ~0% CPU

[![Release](https://img.shields.io/github/v/release/kylepelham/TouchCursorFix)](../../releases/latest)
[![License](https://img.shields.io/badge/license-MIT-green)](LICENSE)
![Platform](https://img.shields.io/badge/platform-Windows%2010%2F11-0078D6)

</div>

---

> **The problem:** on a multi-monitor setup, every tap on the touchscreen drags your mouse cursor onto that monitor and steals focus from the window you were working in.

**The fix:** the moment a touch ends, TouchCursorFix snaps the cursor back to where your real mouse was and gives your window focus back.

**Why this one:**

- **No added latency** — nothing runs per mouse event except recording the cursor position
- **Games unaffected** — raw-input mouselook (WoW, shooters) bypasses the hook entirely
- **Touch screens + pens only** — your laptop's precision touchpad is left alone
- **~0% CPU** — its timer only exists while a finger is on the screen

## Install

1. Download **`TouchCursorFix.exe`** from [Releases](../../releases/latest)
2. Run it — drop it anywhere, there's nothing to install
3. Find it in the tray (Windows hides new icons behind `^` — drag it onto the taskbar to pin)

Right-click the tray icon:

| option | does |
|---|---|
| **Enabled** | master toggle |
| **Restore window focus** | give focus back to the pre-touch window after a tap |
| **Run at startup** | auto-start at login (HKCU `Run` key — no admin, no scheduled task) |
| **Exit** | quit |

## How it works

1. A `WH_MOUSE_LL` hook records cursor positions into a ring buffer — that's *all* it does
2. Raw Input (HID digitizer) signals that a touch is happening
3. The cursor position from just *before* the touch is looked up in the buffer
4. 30 ms after the last touch report: cursor snaps back, focus is restored

<details>
<summary><b>Implementation notes</b> (for the curious)</summary>
<br>

- Registers digitizer usages `0x0D/0x04` (touch screen), `0x02` (pen), `0x01` (pen digitizer) — deliberately **not** `0x05` (precision touchpads). Falls back to the whole `0x0D` page for exotic devices.
- Focus restore uses the `AttachThreadInput` technique — no global system settings are touched.
- The 10 ms poll timer exists only during a touch. A once-a-minute watchdog re-installs the hook if Windows ever silently drops it (which happens to hooks that blow the hook timeout during a system stall).
- Per-Monitor-V2 DPI aware, so hook coordinates and `SetCursorPos` agree on mixed-DPI monitor layouts.
- Tick math survives the 49.7-day `GetTickCount` wrap. Single instance via named mutex.

</details>

## Building

MinGW-w64 (g++ + windres), nothing else:

```powershell
windres TouchCursorFix.rc -O coff -o TouchCursorFix.res.o
g++ -O2 -s -municode -mwindows main.cpp TouchCursorFix.res.o -o TouchCursorFix.exe -luser32 -lshell32 -ladvapi32
```

The icon is code — `make_icon.ps1` redraws and repacks `TouchCursorFix.ico` from scratch if you want to restyle it.

## Tuning

Every knob is a `#define` at the top of [`main.cpp`](main.cpp):

| knob | default | meaning |
|---|---|---|
| `HIST` | 256 | cursor-history ring size (power of two) |
| `POLL_MS` | 10 | timer cadence while a touch is in progress |
| `RESTORE_MS` | 30 | snap back this long after the last touch report |
| `SEQ_GAP_MS` | 300 | gap (ms) that begins a new touch "sequence" |
| `PRE_MS` | 40 | anchor = cursor pos at least this long before touch start |
| `SUPPRESS_MS` | 400 | ignore foreground changes during/just after a touch |
| `REHOOK_MS` | 60000 | hook-health watchdog cadence |

## License

[MIT](LICENSE) © Kyle Pelham
