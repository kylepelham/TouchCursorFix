// TouchCursorFix - lightweight touch/mouse separator for multi-monitor + touchscreen.
//
// Problem it solves: an HID-digitizer touchscreen drags/leaves the desktop mouse
// cursor on the touch monitor and can steal keyboard focus. This keeps the cursor
// on your real mouse's monitor and restores focus after a tap - with none of the
// heavy per-event work that makes "Touch Mouse Tools" add latency / wall the cursor.
//
// How it works:
//   * WH_MOUSE_LL hook: ONLY records the cursor position into a ring buffer. Nothing
//     else. (Raw-input games like WoW mouselook bypass this hook entirely.)
//   * Raw Input (HID digitizer, usage page 0x0D): tells us WHEN a touch is happening.
//   * On a touch we look up the cursor position from just BEFORE the touch (ring
//     buffer) and, once the touch settles, snap the cursor back there + restore the
//     pre-touch foreground window.
//   * A one-shot-ish timer runs only during a touch, then stops -> ~0% CPU at idle.
//     (Plus a once-a-minute watchdog that re-installs the mouse hook if Windows
//     ever silently drops it, e.g. after a heavy system stall.)
//
// Build: windres TouchCursorFix.rc -O coff -o TouchCursorFix.res.o
//        g++ -O2 -s -municode -mwindows main.cpp TouchCursorFix.res.o -o TouchCursorFix.exe -luser32 -lshell32 -ladvapi32

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>

#ifndef RIDEV_PAGEONLY
#define RIDEV_PAGEONLY 0x00000020
#endif

#define IDI_APP     1           // icon resource id (TouchCursorFix.rc)

#define WM_TRAY     (WM_APP + 1)
#define ID_TRAY     1
#define IDM_ENABLED 100
#define IDM_FOCUS   101
#define IDM_STARTUP 102
#define IDM_EXIT    103
#define TIMER_ID     1
#define TIMER_REHOOK 2

#define HIST        256     // cursor-position history (power of two)
#define POLL_MS     10      // timer cadence while a touch is in progress
#define RESTORE_MS  30      // snap back this long after the last touch report
#define SEQ_GAP_MS  300     // gap (ms) that begins a new touch "sequence"
#define PRE_MS      40      // anchor = cursor pos at least this long before touch start
#define SUPPRESS_MS 400     // ignore foreground changes during/just after a touch
#define REHOOK_MS   60000   // hook-health watchdog cadence

static const wchar_t* APP_NAME    = L"TouchCursorFix";
static const wchar_t* RUN_KEY     = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";

static HINSTANCE       g_hInst;
static HWND            g_hwnd;
static HHOOK           g_mouseHook;
static HWINEVENTHOOK   g_winHook;
static NOTIFYICONDATAW g_nid;
static UINT            g_wmTaskbarCreated;

// --- cursor history (written by the mouse hook, read at touch start) ---
static POINT             g_hp[HIST];
static DWORD             g_ht[HIST];
static volatile unsigned g_hidx;

// --- touch state ---
static volatile DWORD g_lastTouchTick;
static volatile LONG  g_pending;
static volatile LONG  g_timerOn;
static POINT          g_anchor;
static BOOL           g_haveAnchor;
static HWND           g_restoreFg;

// --- foreground tracking (pre-touch window) ---
static HWND            g_realForeground;
static volatile DWORD  g_suppressFgUntil;   // ignore foreground changes until this tick

// --- settings ---
static BOOL g_enabled      = TRUE;
static BOOL g_restoreFocus = TRUE;

// ---------------------------------------------------------------------------

// Mouse hook: the ONLY thing on the per-event path. Keep it trivial.
static LRESULT CALLBACK MouseProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode == HC_ACTION && g_enabled) {
        const MSLLHOOKSTRUCT* m = reinterpret_cast<const MSLLHOOKSTRUCT*>(lParam);
        unsigned i = g_hidx++ & (HIST - 1);
        g_hp[i] = m->pt;
        g_ht[i] = m->time;          // event timestamp, no syscall
    }
    return CallNextHookEx(g_mouseHook, nCode, wParam, lParam);
}

// Windows silently removes a LL hook whose callback ever blows the hook timeout
// (heavy stall, debugger, ...). Once a minute: if the cursor sits somewhere the
// ring buffer never saw AND nothing was recorded for 5s+, assume the hook is dead
// and re-install it. (Touch- and SetCursorPos-moves also pass through a live hook,
// so a live hook always keeps the newest entry current.)
static void RehookIfDead() {
    if (!g_enabled || !g_mouseHook) return;
    POINT cur;
    if (!GetCursorPos(&cur)) return;
    unsigned newest = (g_hidx - 1) & (HIST - 1);
    if (cur.x == g_hp[newest].x && cur.y == g_hp[newest].y) return;   // hook is current
    if ((LONG)(GetTickCount() - g_ht[newest]) < 5000) return;         // recorded recently
    UnhookWindowsHookEx(g_mouseHook);
    g_mouseHook = SetWindowsHookExW(WH_MOUSE_LL, MouseProc, g_hInst, 0);
    unsigned i = g_hidx++ & (HIST - 1);                               // reseed history
    g_hp[i] = cur; g_ht[i] = GetTickCount();
}

// Most recent recorded position with timestamp <= t (signed tick deltas, so the
// comparison survives the 49.7-day GetTickCount wrap on a long-running session).
static BOOL FindBefore(DWORD t, POINT* out) {
    LONG bestAge = MAXLONG; BOOL found = FALSE;
    for (unsigned i = 0; i < HIST; ++i) {
        DWORD ts = g_ht[i];
        if (!ts) continue;
        LONG age = (LONG)(t - ts);
        if (age >= 0 && age <= bestAge) { bestAge = age; *out = g_hp[i]; found = TRUE; }
    }
    return found;
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
        // We only registered digitizer usages, so any WM_INPUT == touch/pen.
        if (g_enabled) {
            DWORD now = GetTickCount();
            if (now - g_lastTouchTick > SEQ_GAP_MS) {      // new touch sequence
                g_haveAnchor = FindBefore(now - PRE_MS, &g_anchor);
                g_restoreFg  = g_realForeground;           // pre-touch window
            }
            g_lastTouchTick = now;
            g_suppressFgUntil = now + SUPPRESS_MS;
            InterlockedExchange(&g_pending, 1);
            if (!g_timerOn) { SetTimer(g_hwnd, TIMER_ID, POLL_MS, nullptr); g_timerOn = 1; }
        }
        return DefWindowProcW(hwnd, msg, w, l);            // raw-input cleanup
    }

    case WM_TIMER:
        if (w == TIMER_REHOOK) { RehookIfDead(); return 0; }
        if (g_pending && (GetTickCount() - g_lastTouchTick) >= RESTORE_MS) {
            InterlockedExchange(&g_pending, 0);
            if (g_haveAnchor) SetCursorPos(g_anchor.x, g_anchor.y);
            if (g_restoreFocus) RestoreForeground(g_restoreFg);
            g_restoreFg = nullptr;
            g_suppressFgUntil = GetTickCount() + SUPPRESS_MS;
        }
        if (!g_pending) { KillTimer(g_hwnd, TIMER_ID); g_timerOn = 0; }
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
        case IDM_ENABLED: g_enabled = !g_enabled; InterlockedExchange(&g_pending, 0); break;
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

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR, int) {
    g_hInst = hInst;

    HANDLE mtx = CreateMutexW(nullptr, TRUE, L"TouchCursorFix_singleton_v1");
    if (mtx && GetLastError() == ERROR_ALREADY_EXISTS) return 0;

    // Per-Monitor-V2 DPI awareness: the LL-hook coords and SetCursorPos then share
    // one physical-pixel space on mixed-DPI monitor setups (no-op before Win10 1703).
    typedef BOOL (WINAPI *SetDpiCtxFn)(HANDLE);
    SetDpiCtxFn setDpiCtx = (SetDpiCtxFn)(void*)GetProcAddress(
        GetModuleHandleW(L"user32.dll"), "SetProcessDpiAwarenessContext");
    if (setDpiCtx) setDpiCtx((HANDLE)-4 /* DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 */);
    else SetProcessDPIAware();

    // seed history + foreground with current state
    { POINT p; GetCursorPos(&p); g_hp[0] = p; g_ht[0] = GetTickCount(); g_hidx = 1; }
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

    // Raw input, as an input sink: touch screens (0x0D/0x04), pens (0x02) and pen
    // digitizers (0x01) - but NOT precision touchpads (0x05), which the old
    // page-wide registration also captured (it would fight the touchpad on laptops).
    RAWINPUTDEVICE rids[3] = {};
    static const USHORT usages[3] = { 0x04, 0x02, 0x01 };
    for (int i = 0; i < 3; ++i) {
        rids[i].usUsagePage = 0x0D;
        rids[i].usUsage     = usages[i];
        rids[i].dwFlags     = RIDEV_INPUTSINK;
        rids[i].hwndTarget  = g_hwnd;
    }
    if (!RegisterRawInputDevices(rids, 3, sizeof(rids[0]))) {
        // Fallback: the whole digitizer page (covers exotic devices).
        RAWINPUTDEVICE rid = {};
        rid.usUsagePage = 0x0D;
        rid.usUsage     = 0;
        rid.dwFlags     = RIDEV_PAGEONLY | RIDEV_INPUTSINK;
        rid.hwndTarget  = g_hwnd;
        if (!RegisterRawInputDevices(&rid, 1, sizeof(rid)))
            MessageBoxW(nullptr, L"Could not register for touch (raw input) events.\n"
                                 L"Touch detection will not work.", APP_NAME, MB_ICONWARNING);
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
