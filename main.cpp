// TouchCursorFix - lightweight touch/mouse separator for multi-monitor + touchscreen.
//
// Problem it solves: an HID-digitizer touchscreen drags/leaves the desktop mouse
// cursor on the touch monitor and can steal keyboard focus. This keeps the cursor
// on your real mouse's monitors and restores focus after a tap - with none of the
// heavy per-event work that makes similar tools add latency / wall the cursor.
//
// How it works:
//   * Windows reports which monitor belongs to the touch digitizer
//     (GetPointerDevices). That monitor is "touch land"; everywhere else is
//     "home". The WH_MOUSE_LL hook tracks the last cursor position at home.
//   * Two independent touch triggers feed one state machine:
//       - Raw Input from HID digitizer usages (reliable for most taps), and
//       - the hook spotting a non-injected cursor TELEPORT onto the touch monitor.
//         A mouse can't jump 400+ px in one event; only touch synthesis can. This
//         catches taps whose raw input gets swallowed (observed: VMware windows).
//   * Once the touch settles: restore the pre-touch foreground window FIRST
//     (VMs/games release cursor grabs when they lose focus), clear any foreign
//     ClipCursor, snap the cursor back to its last home position - retrying
//     briefly, and re-restoring if a late synthesized move / VM warp yanks the
//     cursor onto the touch monitor again (post-restore guard).
//   * If you move the mouse onto the touch monitor YOURSELF (gradual movement,
//     never a teleport), the tool stands down until you leave - touches then
//     don't move your cursor at all.
//   * ~0% CPU at idle: the poll timer exists only during a touch (+ a short
//     guard); a once-a-minute watchdog re-installs the hook if Windows ever
//     silently drops it. Raw-input games bypass the hook entirely.
//
// Build: windres TouchCursorFix.rc -O coff -o TouchCursorFix.res.o
//        g++ -O2 -s -municode -mwindows main.cpp TouchCursorFix.res.o -o TouchCursorFix.exe -luser32 -lshell32 -ladvapi32
//
// Run with -log to write a diagnostic TouchCursorFix.log next to the exe.

#define WIN32_LEAN_AND_MEAN
#define WINVER       0x0A00
#define _WIN32_WINNT 0x0A00
#include <windows.h>
#include <shellapi.h>
#include <cstdio>
#include <cstdarg>
#include <cwchar>

#ifndef RIDEV_PAGEONLY
#define RIDEV_PAGEONLY 0x00000020
#endif

#define IDI_APP     1           // icon resource id (TouchCursorFix.rc)

#define WM_TRAY     (WM_APP + 1)
#define WM_TELEPORT (WM_APP + 2)   // hook saw a touch-like cursor teleport
#define ID_TRAY     1
#define IDM_ENABLED 100
#define IDM_FOCUS   101
#define IDM_STARTUP 102
#define IDM_EXIT    103
#define TIMER_ID     1
#define TIMER_REHOOK 2

#define HIST        256     // cursor-position history (power of two)
#define POLL_MS     10      // timer cadence while a touch is in progress
#define RESTORE_MS  30      // snap back this long after the last touch signal
#define SEQ_GAP_MS  300     // gap (ms) that begins a new touch "sequence"
#define MAX_SEQ_MS  1500    // hard cap on how long one sequence can stay alive
#define SUPPRESS_MS 400     // ignore foreground changes during/just after a touch
#define REHOOK_MS   60000   // hook-health watchdog cadence
#define SNAP_WIN_MS 3000    // keep re-snapping this long: Windows pins the cursor to the
                            // touch point until the synthesized click is fully delivered,
                            // which takes SECONDS for slow targets (VMware)
#define GUARD_MS    800     // undo late synthesized moves/VM warps this long after a restore
#define TELEPORT_PX 400     // one-event non-injected jump >= this = touch, not a mouse
#define SEAM_PX     16      // treat this margin around the touch monitor as touch land too

static const wchar_t* APP_NAME = L"TouchCursorFix";
static const wchar_t* RUN_KEY  = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";

static HINSTANCE       g_hInst;
static HWND            g_hwnd;
static HHOOK           g_mouseHook;
static HWINEVENTHOOK   g_winHook;
static NOTIFYICONDATAW g_nid;
static UINT            g_wmTaskbarCreated;

// --- cursor history (written by the mouse hook) ---
static POINT             g_hp[HIST];
static DWORD             g_ht[HIST];
static volatile unsigned g_hidx;

// --- touch monitors (from GetPointerDevices), stored as inflated RECTs so the
// --- seam column can never be misclassified as "home" ---
static RECT g_touchRects[8];
static int  g_touchMonCount;
static BOOL g_lastOnTouch;          // previous hook event was on touch land

// --- "home" = last cursor position NOT on a touch monitor ---
static POINT g_homePos;
static DWORD g_homeTime;
static BOOL  g_userOnTouch;         // user moved onto the touch monitor deliberately

// --- touch state ---
static volatile DWORD g_lastTouchTick;
static DWORD          g_seqStart;
static volatile LONG  g_pending;
static volatile LONG  g_timerOn;
static POINT          g_anchor;
static BOOL           g_haveAnchor;
static HWND           g_restoreFg;
static DWORD          g_snapUntil;  // keep re-snapping until this tick (0 = idle)
static BOOL           g_snapDone;   // current snap window already succeeded
static BOOL           g_ungrabSent; // Ctrl+Alt already sent this window
static DWORD          g_guardUntil;
static volatile LONG  g_warped;     // injected event yanked the cursor onto touch land mid-guard
static DWORD          g_tpTime;       // teleport trigger: event time
static volatile LONG  g_parked;       // foreign injected placement onto touch land (any time)
static volatile DWORD g_mouseRawTick; // last RELATIVE mouse raw input = user's hand on the mouse

// --- foreground tracking (pre-touch window) ---
static HWND            g_realForeground;
static volatile DWORD  g_suppressFgUntil;

// --- settings ---
static BOOL g_enabled      = TRUE;
static BOOL g_restoreFocus = TRUE;

// --- optional debug logging (run with -log) ---
static FILE* g_logf;
static void LogF(const char* fmt, ...) {
    if (!g_logf) return;
    fprintf(g_logf, "%10lu ", GetTickCount());
    va_list ap; va_start(ap, fmt);
    vfprintf(g_logf, fmt, ap);
    va_end(ap);
    fputc('\n', g_logf);
    fflush(g_logf);
}

// ---------------------------------------------------------------------------

// Which monitor(s) belong to integrated touch/pen digitizers. Rects (not
// HMONITORs): handle-compares go stale on display changes and are ambiguous in
// the seam column - a rect with a seam margin is unambiguous.
static void RefreshTouchMonitors() {
    int old = g_touchMonCount;
    g_touchMonCount = 0;
    UINT32 n = 0;
    if (GetPointerDevices(&n, nullptr) && n) {
        POINTER_DEVICE_INFO info[16];
        if (n > 16) n = 16;
        if (GetPointerDevices(&n, info)) {
            for (UINT32 i = 0; i < n && g_touchMonCount < 8; ++i) {
                if ((info[i].pointerDeviceType == POINTER_DEVICE_TYPE_TOUCH ||
                     info[i].pointerDeviceType == POINTER_DEVICE_TYPE_INTEGRATED_PEN) &&
                    info[i].monitor) {
                    MONITORINFO mi = { sizeof(mi), {}, {}, 0 };
                    if (GetMonitorInfoW(info[i].monitor, &mi)) {
                        InflateRect(&mi.rcMonitor, SEAM_PX, SEAM_PX);
                        g_touchRects[g_touchMonCount++] = mi.rcMonitor;
                    }
                }
            }
        }
    }
    if (g_touchMonCount != old) {
        LogF("touch monitors: %d", g_touchMonCount);
        for (int i = 0; i < g_touchMonCount; ++i)
            LogF("  touch rect: (%ld,%ld)-(%ld,%ld) incl. %dpx margin",
                 g_touchRects[i].left, g_touchRects[i].top,
                 g_touchRects[i].right, g_touchRects[i].bottom, SEAM_PX);
    }
}

static BOOL OnTouchMon(POINT pt) {
    for (int i = 0; i < g_touchMonCount; ++i)
        if (PtInRect(&g_touchRects[i], pt)) return TRUE;
    return FALSE;
}

// ---------------------------------------------------------------------------

// Mouse hook: records positions, tracks "home", and classifies events. All
// decisions here are a handful of compares; real work happens on the timer.
static LRESULT CALLBACK MouseProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode == HC_ACTION && g_enabled) {
        const MSLLHOOKSTRUCT* m = reinterpret_cast<const MSLLHOOKSTRUCT*>(lParam);
        unsigned prevI = (g_hidx - 1) & (HIST - 1);
        POINT prev  = g_hp[prevI];
        DWORD prevT = g_ht[prevI];
        unsigned i = g_hidx++ & (HIST - 1);
        g_hp[i] = m->pt;
        g_ht[i] = m->time;          // event timestamp, no syscall

        BOOL injected   = (m->flags & LLMHF_INJECTED) != 0;
        BOOL onTouch    = OnTouchMon(m->pt);
        BOOL wasOnTouch = g_lastOnTouch;
        g_lastOnTouch   = onTouch;
        // Ground truth: RELATIVE mouse raw input within the last 60 ms means the
        // user's hand is on the mouse. Touch synthesis and programmatic warps
        // never produce relative mouse raw input, so this cannot be faked.
        BOOL mouseDriven = (LONG)(m->time - g_mouseRawTick) < 60;

        LONG dx = m->pt.x - prev.x, dy = m->pt.y - prev.y;
        LONG d2 = dx * dx + dy * dy;
        // Only synthesis teleports; a mouse being driven is just moving fast.
        BOOL jump = !injected && !mouseDriven && prevT &&
                    (d2 >= TELEPORT_PX * TELEPORT_PX ||
                     (onTouch && !wasOnTouch && d2 >= 100 * 100));

        if (!onTouch) {
            // Home must be a REAL on-screen position: the LL hook can report
            // unclamped coords (x=-1 while pushing against a boundary), and the
            // desktop has dead zones next to a small touch monitor (XENEON Edge
            // is 2560x720 in a 1440-tall desktop). Teleports are never home.
            if (!jump && MonitorFromPoint(m->pt, MONITOR_DEFAULTTONULL)) {
                g_homePos = m->pt; g_homeTime = m->time;
            }
            g_userOnTouch = FALSE;
        } else if (mouseDriven && !injected && d2 > 0) {
            // The physical mouse is moving on touch land: the user came here on
            // purpose. Stand down instantly and abort anything in flight.
            g_userOnTouch = TRUE;
            InterlockedExchange(&g_pending, 0);
        } else if (jump) {
            // Touch trigger #2: synthesized tap-move landed on the touch monitor.
            // Works even when the tap's raw input is swallowed (VMware windows).
            g_tpTime = m->time;
            PostMessageW(g_hwnd, WM_TELEPORT, 0, 0);
        } else if (!injected) {
            if (g_pending && (LONG)(m->time - g_seqStart) < MAX_SEQ_MS && d2 > 0)
                g_lastTouchTick = m->time;           // finger drag keeps the sequence alive
        } else if (!g_userOnTouch && !g_pending) {
            // Injected event ON the touch monitor (VM warp / foreign SetCursorPos).
            // A programmatic placement of the cursor onto touch land is NEVER
            // legitimate unless the user walked there - whenever it happens.
            if ((LONG)(m->time - g_guardUntil) < 0) {
                InterlockedExchange(&g_warped, 1);       // fresh: guard re-restores focus too
            } else if (!g_snapUntil) {
                // Late park, after all our windows closed (VMware can take many
                // seconds to finish digesting a tap): reopen a snap window.
                InterlockedExchange(&g_parked, 1);
                if (!g_timerOn) { SetTimer(g_hwnd, TIMER_ID, POLL_MS, nullptr); g_timerOn = 1; }
            }
        }
    }
    return CallNextHookEx(g_mouseHook, nCode, wParam, lParam);
}

// Windows silently removes a LL hook whose callback ever blows the hook timeout
// (heavy stall, debugger, ...). Once a minute: if the cursor sits somewhere the
// ring buffer never saw AND nothing was recorded for 5s+, assume the hook is dead
// and re-install it.
static void RehookIfDead() {
    if (!g_enabled) return;
    POINT cur;
    if (!GetCursorPos(&cur)) return;
    unsigned newest = (g_hidx - 1) & (HIST - 1);
    if (g_mouseHook) {
        if (cur.x == g_hp[newest].x && cur.y == g_hp[newest].y) return;   // hook is current
        if ((LONG)(GetTickCount() - g_ht[newest]) < 5000) return;         // recorded recently
        UnhookWindowsHookEx(g_mouseHook);
    }
    g_mouseHook = SetWindowsHookExW(WH_MOUSE_LL, MouseProc, g_hInst, 0);  // null -> retried next minute
    LogF("REHOOK -> %p", (void*)g_mouseHook);
    unsigned i = g_hidx++ & (HIST - 1);                                   // reseed history
    g_hp[i] = cur; g_ht[i] = GetTickCount();
}

// Common touch bookkeeping for both triggers (raw input + teleport fallback).
static void TouchSeen(DWORD evt, const char* src) {
    if (g_userOnTouch) return;                             // user owns touch land right now
    if (evt - g_lastTouchTick > SEQ_GAP_MS) {              // new touch sequence
        if (OnTouchMon(g_homePos)) {
            LogF("SEQ skip (%s): no valid home", src);
            return;                                        // nowhere sane to snap to
        }
        g_anchor     = g_homePos;                          // last real off-touch position
        g_haveAnchor = TRUE;
        g_restoreFg  = g_realForeground;                   // pre-touch window
        g_seqStart   = evt;
        LogF("SEQ start (%s) anchor=(%ld,%ld) lag=%ldms fg=%p", src,
             g_anchor.x, g_anchor.y, (LONG)(GetTickCount() - evt), (void*)g_restoreFg);
    }
    g_lastTouchTick = evt;
    g_suppressFgUntil = GetTickCount() + SUPPRESS_MS;
    InterlockedExchange(&g_pending, 1);
    if (!g_timerOn) { SetTimer(g_hwnd, TIMER_ID, POLL_MS, nullptr); g_timerOn = 1; }
}

// Track the foreground window, ignoring the change the touch itself causes.
static void CALLBACK WinEventProc(HWINEVENTHOOK, DWORD event, HWND hwnd,
                                  LONG, LONG, DWORD, DWORD) {
    if (event == EVENT_SYSTEM_FOREGROUND && hwnd && hwnd != g_hwnd) {
        if ((LONG)(GetTickCount() - g_suppressFgUntil) >= 0)   // not during/just after a touch
            g_realForeground = hwnd;
    }
}

// Restore a window to the foreground reliably (no global setting changes).
static void RestoreForeground(HWND target) {
    if (!target || !IsWindow(target)) return;
    HWND fg = GetForegroundWindow();
    if (fg == target) return;
    DWORD me  = GetCurrentThreadId();
    DWORD fgT = fg ? GetWindowThreadProcessId(fg, nullptr) : 0;
    DWORD tgT = GetWindowThreadProcessId(target, nullptr);
    if (fgT && fgT != me) AttachThreadInput(me, fgT, TRUE);
    if (tgT && tgT != me) AttachThreadInput(me, tgT, TRUE);
    SetForegroundWindow(target);
    BringWindowToTop(target);
    if (tgT && tgT != me) AttachThreadInput(me, tgT, FALSE);
    if (fgT && fgT != me) AttachThreadInput(me, fgT, FALSE);
}

// Snap the cursor to `a`, defeating a foreign cursor grab if needed. If another
// app (VMware, a game) holds a ClipCursor rect that excludes the anchor, a plain
// SetCursorPos would clamp to the clip edge and pin the cursor there - clear the
// clip first, and report whether the cursor actually reached the anchor's monitor
// (so the caller can retry while the grabber asynchronously lets go).
// Move the cursor via a REAL injected mouse event. Unlike SetCursorPos (a bare
// position poke that the pointer stack overrides while a touch interaction is
// unresolved), SendInput goes through the input pipeline and switches the active
// pointing device back to the mouse - breaking the touch pin.
static void InjectMove(POINT a) {
    int vx = GetSystemMetrics(SM_XVIRTUALSCREEN), vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN), vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    if (vw <= 0 || vh <= 0) return;
    INPUT in = {};
    in.type = INPUT_MOUSE;
    in.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
    in.mi.dx = MulDiv(a.x - vx, 65535, vw - 1);
    in.mi.dy = MulDiv(a.y - vy, 65535, vh - 1);
    SendInput(1, &in, sizeof(in));
}

static int TrySnap(POINT a) {       // 1 = ok, 0 = overridden/pinned, -1 = clip blocked
    RECT c;
    if (GetClipCursor(&c) && !PtInRect(&c, a)) {
        ClipCursor(nullptr);                                   // release foreign clip
        if (GetClipCursor(&c) && !PtInRect(&c, a)) return -1;  // re-asserted / elevated owner
    }
    SetCursorPos(a.x, a.y);
    POINT p;
    if (!GetCursorPos(&p)) return 1;
    if (!OnTouchMon(p)) return 1;   // mission accomplished = cursor is off touch land
    InjectMove(a);                  // position poke was overridden: use real input
    if (!GetCursorPos(&p)) return 1;
    return OnTouchMon(p) ? 0 : 1;
}

// Is the foreground window owned by the given exe (basename, case-insensitive)?
static BOOL FgIsExe(const wchar_t* name) {
    DWORD pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    if (!pid) return FALSE;
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return FALSE;
    wchar_t exe[MAX_PATH]; DWORD n = MAX_PATH;
    BOOL ok = QueryFullProcessImageNameW(h, 0, exe, &n);
    CloseHandle(h);
    if (!ok) return FALSE;
    const wchar_t* base = exe;
    for (const wchar_t* s = exe; *s; ++s) if (*s == L'\\') base = s + 1;
    return lstrcmpiW(base, name) == 0;
}

// VMware's hard mouse grab (VM without Tools) re-asserts its ClipCursor faster
// than we can clear it - but Ctrl+Alt is VMware's own designed ungrab hotkey.
static void SendCtrlAlt() {
    INPUT in[4] = {};
    in[0].type = INPUT_KEYBOARD; in[0].ki.wVk = VK_CONTROL;
    in[1].type = INPUT_KEYBOARD; in[1].ki.wVk = VK_MENU;
    in[2].type = INPUT_KEYBOARD; in[2].ki.wVk = VK_MENU;    in[2].ki.dwFlags = KEYEVENTF_KEYUP;
    in[3].type = INPUT_KEYBOARD; in[3].ki.wVk = VK_CONTROL; in[3].ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(4, in, sizeof(INPUT));
}

// ---------------------------------------------------------------------------
// startup (run-at-login) helpers

static BOOL StartupEnabled() {
    HKEY k; BOOL on = FALSE;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, RUN_KEY, 0, KEY_READ, &k) == ERROR_SUCCESS) {
        on = (RegQueryValueExW(k, APP_NAME, nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS);
        RegCloseKey(k);
    }
    return on;
}
static void SetStartup(BOOL on) {
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, RUN_KEY, 0, KEY_SET_VALUE, &k) != ERROR_SUCCESS) return;
    if (on) {
        wchar_t path[MAX_PATH]; GetModuleFileNameW(nullptr, path, MAX_PATH);
        wchar_t q[MAX_PATH + 2]; wsprintfW(q, L"\"%s\"", path);
        RegSetValueExW(k, APP_NAME, 0, REG_SZ, (const BYTE*)q,
                       (DWORD)((lstrlenW(q) + 1) * sizeof(wchar_t)));
    } else {
        RegDeleteValueW(k, APP_NAME);
    }
    RegCloseKey(k);
}

// ---------------------------------------------------------------------------

static void TrayAdd() {
    g_nid.cbSize           = sizeof(g_nid);
    g_nid.hWnd             = g_hwnd;
    g_nid.uID              = ID_TRAY;
    g_nid.uFlags           = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAY;
    static HICON s_icon;
    if (!s_icon) {
        s_icon = (HICON)LoadImageW(g_hInst, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON,
                                   GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0);
        if (!s_icon) s_icon = LoadIconW(nullptr, IDI_INFORMATION);   // fallback: resource missing
    }
    g_nid.hIcon            = s_icon;
    lstrcpyW(g_nid.szTip, L"TouchCursorFix");
    Shell_NotifyIconW(NIM_ADD, &g_nid);
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM w, LPARAM l) {
    if (msg == g_wmTaskbarCreated) { TrayAdd(); return 0; }

    switch (msg) {
    case WM_INPUT: {
        // Mouse raw input = "hand on mouse" signal; digitizer raw input = touch
        // trigger #1. GetMessageTime = queue timestamp (immune to delivery lag).
        RAWINPUTHEADER hdr; UINT hsz = sizeof(hdr);
        if (GetRawInputData((HRAWINPUT)l, RID_HEADER, &hdr, &hsz, sizeof(RAWINPUTHEADER)) != (UINT)-1) {
            if (hdr.dwType == RIM_TYPEMOUSE) {
                BYTE buf[sizeof(RAWINPUT) + 32]; UINT sz = sizeof(buf);
                if (GetRawInputData((HRAWINPUT)l, RID_INPUT, buf, &sz, sizeof(RAWINPUTHEADER)) != (UINT)-1) {
                    const RAWINPUT* ri = reinterpret_cast<const RAWINPUT*>(buf);
                    if (!(ri->data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE) &&
                        (ri->data.mouse.lLastX || ri->data.mouse.lLastY))
                        g_mouseRawTick = (DWORD)GetMessageTime();   // real relative mouse motion
                }
            } else if (g_enabled) {
                TouchSeen((DWORD)GetMessageTime(), "rawinput");     // digitizer = touch/pen
            }
        }
        return DefWindowProcW(hwnd, msg, w, l);            // raw-input cleanup
    }

    case WM_TELEPORT:
        if (g_enabled) TouchSeen(g_tpTime, "teleport");
        return 0;

    case WM_TIMER:
        if (w == TIMER_REHOOK) { RehookIfDead(); RefreshTouchMonitors(); return 0; }
        if (g_pending && (GetTickCount() - g_lastTouchTick) >= RESTORE_MS) {
            InterlockedExchange(&g_pending, 0);
            // Focus FIRST: a VM/game that grabbed the cursor releases its grab
            // (and its ClipCursor) once it loses the foreground.
            if (g_restoreFocus) RestoreForeground(g_restoreFg);
            g_snapUntil  = g_haveAnchor ? GetTickCount() + SNAP_WIN_MS : 0;
            g_snapDone   = !g_haveAnchor;
            g_ungrabSent = FALSE;
            g_guardUntil = GetTickCount() + GUARD_MS;
            InterlockedExchange(&g_warped, 0);
            g_suppressFgUntil = GetTickCount() + SUPPRESS_MS;
            LogF("RESTORE anchor=(%ld,%ld) fg=%p", g_anchor.x, g_anchor.y, (void*)g_restoreFg);
        }
        if (!g_pending) {
            if (g_parked) {
                InterlockedExchange(&g_parked, 0);
                if (!g_userOnTouch && !OnTouchMon(g_homePos)) {
                    g_anchor = g_homePos; g_haveAnchor = TRUE;
                    g_snapUntil  = GetTickCount() + SNAP_WIN_MS;
                    g_snapDone   = FALSE;
                    g_ungrabSent = FALSE;
                    LogF("PARK detected -> snap window reopened anchor=(%ld,%ld)",
                         g_anchor.x, g_anchor.y);
                }
            }
            if (g_snapUntil && g_userOnTouch) {            // user took over - stand down
                LogF("SNAP canceled (user moving on touch land)");
                g_snapUntil = 0;
            }
            if (g_snapUntil) {
                if ((LONG)(GetTickCount() - g_snapUntil) >= 0) {
                    if (!g_snapDone) LogF("SNAP window expired (cursor still held)");
                    g_snapUntil = 0;
                } else {
                    // Windows pins the cursor to the touch point until the tap's
                    // synthesized click is fully delivered (seconds, for slow
                    // targets like VMware). Poll: whenever the cursor sits on
                    // touch land, snap it home again - until it finally sticks.
                    POINT p;
                    if (GetCursorPos(&p) && !OnTouchMon(p)) {
                        if (!g_snapDone) { LogF("SNAP ok"); g_snapDone = TRUE; }
                    } else {
                        if (g_snapDone) { LogF("SNAP re-engaged (cursor pulled back)"); g_snapDone = FALSE; }
                        int r = TrySnap(g_anchor);
                        if (r == 1) { LogF("SNAP ok"); g_snapDone = TRUE; }
                        else if (r == -1 && !g_ungrabSent && FgIsExe(L"vmware.exe")) {
                            LogF("SNAP clip-blocked by VMware grab -> sending Ctrl+Alt ungrab");
                            SendCtrlAlt();
                            g_ungrabSent = TRUE;
                        }
                    }
                }
            }
            if (g_warped) {
                // A VM warp / foreign injected move yanked the cursor onto the
                // touch monitor right after our restore: put focus back too.
                InterlockedExchange(&g_warped, 0);
                LogF("GUARD re-restore");
                if (g_restoreFocus) RestoreForeground(g_restoreFg);
                g_suppressFgUntil = GetTickCount() + SUPPRESS_MS;
            }
            if (!g_snapUntil && (LONG)(GetTickCount() - g_guardUntil) >= 0) {
                g_restoreFg = nullptr;                     // all windows over - back to sleep
                KillTimer(g_hwnd, TIMER_ID); g_timerOn = 0;
            }
        }
        return 0;

    case WM_DISPLAYCHANGE:
        RefreshTouchMonitors();
        return 0;

    case WM_TRAY:
        if (LOWORD(l) == WM_RBUTTONUP || LOWORD(l) == WM_CONTEXTMENU || LOWORD(l) == WM_LBUTTONUP) {
            POINT pt; GetCursorPos(&pt);
            HMENU m = CreatePopupMenu();
            AppendMenuW(m, MF_STRING | (g_enabled      ? MF_CHECKED : 0), IDM_ENABLED, L"Enabled");
            AppendMenuW(m, MF_STRING | (g_restoreFocus ? MF_CHECKED : 0), IDM_FOCUS,   L"Restore window focus");
            AppendMenuW(m, MF_STRING | (StartupEnabled()? MF_CHECKED : 0), IDM_STARTUP, L"Run at startup");
            AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(m, MF_STRING, IDM_EXIT, L"Exit");
            SetForegroundWindow(hwnd);
            TrackPopupMenu(m, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, nullptr);
            PostMessageW(hwnd, WM_NULL, 0, 0);   // classic fix: menu dismisses cleanly next time
            DestroyMenu(m);
        }
        return 0;

    case WM_COMMAND:
        switch (LOWORD(w)) {
        case IDM_ENABLED: g_enabled = !g_enabled; InterlockedExchange(&g_pending, 0);
                          g_snapUntil = 0; g_guardUntil = 0;
                          InterlockedExchange(&g_warped, 0); break;
        case IDM_FOCUS:   g_restoreFocus = !g_restoreFocus; break;
        case IDM_STARTUP: SetStartup(!StartupEnabled()); break;
        case IDM_EXIT:    DestroyWindow(hwnd); break;
        }
        return 0;

    case WM_DESTROY:
        Shell_NotifyIconW(NIM_DELETE, &g_nid);
        if (g_mouseHook) UnhookWindowsHookEx(g_mouseHook);
        if (g_winHook)   UnhookWinEvent(g_winHook);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, w, l);
}

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR cmd, int) {
    g_hInst = hInst;

    HANDLE mtx = CreateMutexW(nullptr, TRUE, L"TouchCursorFix_singleton_v1");
    if (mtx && GetLastError() == ERROR_ALREADY_EXISTS) return 0;

    // Debug logging: run with -log to write TouchCursorFix.log next to the exe.
    if (cmd && wcsstr(cmd, L"-log")) {
        wchar_t p[MAX_PATH];
        GetModuleFileNameW(nullptr, p, MAX_PATH);
        wchar_t* s = wcsrchr(p, L'\\');
        lstrcpyW(s ? s + 1 : p, L"TouchCursorFix.log");
        g_logf = _wfopen(p, L"w");
        LogF("start (v1.3.0)");
    }

    // Per-Monitor-V2 DPI awareness: the LL-hook coords and SetCursorPos then share
    // one physical-pixel space on mixed-DPI monitor setups (no-op before Win10 1703).
    typedef BOOL (WINAPI *SetDpiCtxFn)(HANDLE);
    SetDpiCtxFn setDpiCtx = (SetDpiCtxFn)(void*)GetProcAddress(
        GetModuleHandleW(L"user32.dll"), "SetProcessDpiAwarenessContext");
    if (setDpiCtx) setDpiCtx((HANDLE)-4 /* DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 */);
    else SetProcessDPIAware();

    RefreshTouchMonitors();

    // seed history + home + foreground with current state
    {
        POINT p; GetCursorPos(&p);
        g_hp[0] = p; g_ht[0] = GetTickCount(); g_hidx = 1;
        g_lastOnTouch = OnTouchMon(p);
        g_userOnTouch = g_lastOnTouch;
        if (g_lastOnTouch) {   // started with cursor on touch land: home = primary center
            g_homePos.x = GetSystemMetrics(SM_CXSCREEN) / 2;
            g_homePos.y = GetSystemMetrics(SM_CYSCREEN) / 2;
        } else {
            g_homePos = p;
        }
        g_homeTime = GetTickCount();
    }
    g_realForeground = GetForegroundWindow();
    g_wmTaskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");

    WNDCLASSW wc = {};
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInst;
    wc.lpszClassName = L"TouchCursorFixWnd";
    wc.hIcon         = LoadIconW(hInst, MAKEINTRESOURCEW(IDI_APP));
    RegisterClassW(&wc);
    g_hwnd = CreateWindowExW(0, wc.lpszClassName, APP_NAME, 0, 0, 0, 0, 0,
                             nullptr, nullptr, hInst, nullptr);   // normal (hidden) window
    if (!g_hwnd) return 1;

    TrayAdd();

    // Raw input, as an input sink: touch screens (0x0D/0x04), pens (0x02), pen
    // digitizers (0x01) - NOT precision touchpads (0x05) - plus generic mice
    // (0x01/0x02) purely as the "hand on mouse" signal.
    RAWINPUTDEVICE rids[4] = {};
    static const USHORT pages[4]  = { 0x0D, 0x0D, 0x0D, 0x01 };
    static const USHORT usages[4] = { 0x04, 0x02, 0x01, 0x02 };
    for (int i = 0; i < 4; ++i) {
        rids[i].usUsagePage = pages[i];
        rids[i].usUsage     = usages[i];
        rids[i].dwFlags     = RIDEV_INPUTSINK;
        rids[i].hwndTarget  = g_hwnd;
    }
    if (!RegisterRawInputDevices(rids, 4, sizeof(rids[0]))) {
        // Fallback: the whole digitizer page (covers exotic devices) + mice.
        RAWINPUTDEVICE fb[2] = {};
        fb[0].usUsagePage = 0x0D; fb[0].usUsage = 0;
        fb[0].dwFlags = RIDEV_PAGEONLY | RIDEV_INPUTSINK; fb[0].hwndTarget = g_hwnd;
        fb[1].usUsagePage = 0x01; fb[1].usUsage = 0x02;
        fb[1].dwFlags = RIDEV_INPUTSINK; fb[1].hwndTarget = g_hwnd;
        if (!RegisterRawInputDevices(fb, 2, sizeof(fb[0])))
            MessageBoxW(nullptr, L"Could not register for touch (raw input) events.\n"
                                 L"Touch detection will rely on teleport detection only.",
                        APP_NAME, MB_ICONWARNING);
    }

    g_mouseHook = SetWindowsHookExW(WH_MOUSE_LL, MouseProc, hInst, 0);
    if (!g_mouseHook)
        MessageBoxW(nullptr, L"Could not install the mouse hook.\n"
                             L"Cursor snap-back will not work.", APP_NAME, MB_ICONWARNING);
    g_winHook   = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND,
                                  nullptr, WinEventProc, 0, 0,
                                  WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    SetTimer(g_hwnd, TIMER_REHOOK, REHOOK_MS, nullptr);   // hook-health watchdog

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {   // > 0: don't spin on -1 (error)
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}
