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

1. Windows reports which monitor belongs to the touch digitizer (`GetPointerDevices`) — that's *touch land*; everywhere else is *home*
2. A `WH_MOUSE_LL` hook records cursor positions and tracks your last *home* position
3. Touches are detected two independent ways: raw digitizer input, **and** the hook spotting a physically-impossible cursor teleport onto the touch monitor (catches taps whose raw input gets swallowed by the target window)
4. Once the touch settles: the pre-touch window gets focus back first (VMs/games release cursor grabs when they lose it), then the cursor snaps home — retrying until it sticks
5. Walk the mouse onto the touch monitor yourself and the tool **stands down** until you leave — touches then won't move your cursor at all

<details>
<summary><b>Implementation notes</b> (for the curious)</summary>
<br>

- Registers digitizer usages `0x0D/0x04` (touch screen), `0x02` (pen), `0x01` (pen digitizer) — deliberately **not** `0x05` (precision touchpads). Falls back to the whole `0x0D` page for exotic devices.
- *Home* is the last **real on-screen** cursor position off touch land — unclamped hook coordinates, monitor-seam pixels, and desktop dead zones (small touch monitors like a 2560x720 touch bar leave one) can never poison it. The seam gets a 16 px margin.
- A teleport = a non-injected single-event jump of 400+ px (or 100+ px crossing onto touch land). Real mice move a few px per event; only touch synthesis teleports.
- Foreign `ClipCursor` grabs are cleared before snapping; VMware's hard mouse grab (VM without Tools) is broken with VMware's own Ctrl+Alt ungrab hotkey — only when `vmware.exe` is the foreground process.
- Snaps escalate from `SetCursorPos` to real injected mouse input (`SendInput`) when the pointer stack overrides the position poke, and keep retrying for up to 3 s. Programmatic cursor placements onto touch land re-open the corrective window whenever they happen.
- Focus restore uses the `AttachThreadInput` technique — no global system settings are touched.
- The 10 ms poll timer exists only around a touch. A once-a-minute watchdog re-installs the hook if Windows ever silently drops it.
- Per-Monitor-V2 DPI aware, tick math survives the 49.7-day `GetTickCount` wrap, single instance via named mutex.
- Run with **`-log`** to write a diagnostic `TouchCursorFix.log` next to the exe.

</details>

> **Known limitation:** taps landing on a VMware Workstation window can leave the cursor stranded on the touch monitor. Windows pins the cursor to the touch point while the tap's synthesized click waits on VMware's message queue, and VMware re-parks the cursor after processing; the app fights back (and usually wins within ~1 s), but some VMware states hold the cursor indefinitely. Every other target tested behaves.

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
