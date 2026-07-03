# TouchCursorFix

<img src="assets/icon.png" align="right" width="110" alt="TouchCursorFix icon">

A tiny Windows tray utility for multi-monitor + touchscreen setups. A HID
touchscreen normally drags the desktop mouse cursor onto the touch monitor and
steals keyboard focus with every tap - this keeps the cursor parked where your
*real* mouse is: the moment a touch settles, the cursor snaps back and the
window you were working in gets focus again.

Built because the existing tools ("Touch Mouse Pointer" and friends) do heavy
per-event work that adds cursor latency or walls the cursor in. This one does
**nothing** on the mouse path except record positions:

- **~0% CPU at idle** - the restore timer only exists *during* a touch.
- Single ~80 KB exe. No installer, no dependencies, no admin.
- Raw-input games (WoW mouselook etc.) bypass its hook entirely - zero impact.
- Touch screens and pens only - precision touchpads are explicitly *not* captured.

## Install

Grab `TouchCursorFix.exe` from [Releases](../../releases/latest), drop it
anywhere, run it. A tray icon appears (Windows hides new tray icons in the `^`
overflow by default - drag it onto the taskbar to pin it).

| tray option | what it does |
|---|---|
| **Enabled** | master toggle |
| **Restore window focus** | give focus back to the pre-touch window after a tap |
| **Run at startup** | auto-start at login (HKCU `Run` key - no admin, no scheduled task) |
| **Exit** | quit |

## How it works

- A **`WH_MOUSE_LL` hook** is the only thing on the per-event path, and all it
  does is record the cursor position + timestamp into a 256-slot ring buffer.
- **Raw Input** (HID digitizer usages: touch screen `0x0D/0x04`, pen `0x02`,
  pen digitizer `0x01`) says *when* a touch is happening. Precision touchpads
  (`0x05`) are deliberately not registered; if your digitizer is exotic,
  registration falls back to the whole `0x0D` page automatically.
- On a new touch sequence the anchor is looked up from the ring buffer: the
  cursor position from just *before* the first touch report, plus the
  pre-touch foreground window.
- Once the touch settles (30 ms after the last report): `SetCursorPos` back to
  the anchor and restore the foreground window (`AttachThreadInput` technique -
  no global system settings are touched).
- A 10 ms timer runs **only while a touch is in progress**, then stops. A
  once-a-minute watchdog re-installs the hook if Windows ever silently drops it
  (which it does to hooks that blow the hook timeout during a system stall).
- Per-Monitor-V2 DPI aware, so hook coordinates and `SetCursorPos` agree on
  mixed-DPI monitor layouts. Tick math survives the 49.7-day `GetTickCount`
  wrap. Single instance via named mutex.

## Building

MinGW-w64 (g++ + windres), no other dependencies:

```powershell
windres TouchCursorFix.rc -O coff -o TouchCursorFix.res.o
g++ -O2 -s -municode -mwindows main.cpp TouchCursorFix.res.o -o TouchCursorFix.exe -luser32 -lshell32 -ladvapi32
```

The icon is generated, not hand-drawn - `make_icon.ps1` redraws
`TouchCursorFix.ico` (GDI+, 16/20/24/32/48/64 px BMP frames + 256 px PNG frame)
if you want to restyle it.

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

## Files

| file | purpose |
|---|---|
| `main.cpp` | the whole app |
| `TouchCursorFix.rc` | icon + version-info resources |
| `TouchCursorFix.ico` | the icon (all frames; regenerate with `make_icon.ps1`) |
| `make_icon.ps1` | draws the icon from scratch and packs the `.ico` |

## License

[MIT](LICENSE)
