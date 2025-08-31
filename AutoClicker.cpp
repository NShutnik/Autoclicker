// AutoClicker.cpp — Win32 C++ autoclicker similar to OP Auto Clicker
// Build (MSVC x64):
//   rc /nologo app.rc
//   cl /W4 /O2 /MT AutoClicker.cpp app.res user32.lib gdi32.lib comctl32.lib winmm.lib shell32.lib advapi32.lib /Fe:LightClick.exe
// Notes: Run as admin if you need to click on elevated apps (UIPI).

#define UNICODE
#define _UNICODE
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <mmsystem.h>
#include <shellapi.h>
#include <cstdio>
#include <cwchar>
#include <thread>
#include <atomic>
#include <chrono>
#include <random>
#include <string>
#include <vector>
#include "Icon.h" // resource header with IDI_APPICON

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "advapi32.lib")
// Visual styles for controls (single-line pragma to avoid MSVC parse issues)
#pragma comment(linker, "/manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

// ---------------------- UI control IDs ----------------------
enum : int {
    IDC_EDIT_INTERVAL = 101,
    IDC_COMBO_BUTTON = 102,
    IDC_CHECK_DOUBLE = 103,
    IDC_CHECK_FIXED = 104,
    IDC_EDIT_X = 105,
    IDC_EDIT_Y = 106,
    IDC_BTN_PICK = 107,
    IDC_BTN_TOGGLE = 108,
    IDC_STATUS = 109,

    IDC_CHECK_CPS = 110, // interpret interval as CPS
    IDC_EDIT_CLICKS = 111,
    IDC_EDIT_SECONDS = 112,
    IDC_RADIO_INF = 113,
    IDC_RADIO_CLICKS = 114,
    IDC_RADIO_SECONDS = 115,
    IDC_EDIT_JITTER = 116,
    IDC_STATIC_UNIT = 117,
    IDC_CHECK_HOLD = 118,

    // Tray + hotkey + autostart
    IDC_HOTKEY = 119,
    IDC_BTN_APPLYHK = 120,
    IDC_CHECK_AUTOSTART = 121
};

// Tray menu command IDs
enum : int {
    IDM_TRAY_SHOWHIDE = 40001,
    IDM_TRAY_STARTSTOP,
    IDM_TRAY_EXIT
};

// Custom window messages
static const UINT WM_APP_AUTOSTOP = WM_APP + 1;  // worker finished by condition
static const UINT WM_APP_PICKED = WM_APP + 2;  // mouse pick finished (x in wParam, y in lParam)
static const UINT WM_APP_TRAY = WM_APP + 3;  // tray icon callback

// ---------------------- Globals -----------------------------
static std::atomic<bool> g_running{ false };
static std::thread g_worker;
static UINT g_hotkeyId = 1; // RegisterHotKey id
static UINT g_hotkeyVK = VK_F6; // default
static UINT g_hotkeyMods = 0;   // MOD_*
static HWND g_hMain = nullptr;

// DPI helpers
static UINT g_dpi = 96;
static inline int SX(int v) { return MulDiv(v, (int)g_dpi, 96); } // scale design px → physical px

// Mouse-pick state
static std::atomic<bool> g_pickMode{ false };
static std::atomic<bool> g_waitUp{ false }; // swallow the matching LButtonUp after capture
static HHOOK g_mouseHook = nullptr;

// Tray state
static bool g_hasTray = false;
static NOTIFYICONDATAW g_nid{};
static const UINT kTrayId = 1001;

struct ClickConfig {
    int interval_ms = 100; // base interval in milliseconds (already converted if CPS)
    int button = 0;        // 0=Left,1=Right,2=Middle,3=MB4(X1),4=MB5(X2)
    bool dbl = false;
    bool fixed = false;
    int x = 0, y = 0;

    bool hold = false;     // hold mode: press down on start, release on stop

    // stop conditions
    int stop_mode = 0;     // 0=infinite, 1=max clicks, 2=max seconds
    int max_clicks = 0;    // valid if stop_mode==1 (counts individual clicks, double=2)
    int max_seconds = 0;   // valid if stop_mode==2

    // jitter
    int jitter_percent = 0; // 0..80 typically; randomized each tick as ±percent of base interval
};

// ---------------------- Forwards ----------------------------
static void UpdateUIState(HWND hWnd);
static std::wstring HotkeyToText(UINT vk, UINT mods);
static void SetStartBtnLabel(HWND hWnd);

// ---------------------- Helpers -----------------------------
static int ReadInt(HWND hEdit, int fallback) {
    wchar_t buf[64]{}; GetWindowTextW(hEdit, buf, 63);
    if (buf[0] == L' ') return fallback;
    wchar_t* end = nullptr; long v = wcstol(buf, &end, 10);
    if (end == buf) return fallback;
    if (v < INT_MIN) v = INT_MIN; if (v > INT_MAX) v = INT_MAX;
    return static_cast<int>(v);
}

static void SetInt(HWND hEdit, int value) {
    wchar_t buf[32]; _snwprintf_s(buf, _TRUNCATE, L"%d", value);
    SetWindowTextW(hEdit, buf);
}

static void SetStatus(const wchar_t* s) {
    if (HWND h = GetDlgItem(g_hMain, IDC_STATUS)) SetWindowTextW(h, s);
}

static void DoButtonDown(int button) {
    INPUT in{}; in.type = INPUT_MOUSE; in.mi.dwFlags = 0; in.mi.mouseData = 0;
    switch (button) {
    case 0: in.mi.dwFlags = MOUSEEVENTF_LEFTDOWN; break;
    case 1: in.mi.dwFlags = MOUSEEVENTF_RIGHTDOWN; break;
    case 2: in.mi.dwFlags = MOUSEEVENTF_MIDDLEDOWN; break;
    case 3: in.mi.dwFlags = MOUSEEVENTF_XDOWN; in.mi.mouseData = XBUTTON1; break; // MB4
    case 4: in.mi.dwFlags = MOUSEEVENTF_XDOWN; in.mi.mouseData = XBUTTON2; break; // MB5
    default: in.mi.dwFlags = MOUSEEVENTF_LEFTDOWN; break;
    }
    SendInput(1, &in, sizeof(INPUT));
}

static void DoButtonUp(int button) {
    INPUT in{}; in.type = INPUT_MOUSE; in.mi.dwFlags = 0; in.mi.mouseData = 0;
    switch (button) {
    case 0: in.mi.dwFlags = MOUSEEVENTF_LEFTUP; break;
    case 1: in.mi.dwFlags = MOUSEEVENTF_RIGHTUP; break;
    case 2: in.mi.dwFlags = MOUSEEVENTF_MIDDLEUP; break;
    case 3: in.mi.dwFlags = MOUSEEVENTF_XUP; in.mi.mouseData = XBUTTON1; break; // MB4
    case 4: in.mi.dwFlags = MOUSEEVENTF_XUP; in.mi.mouseData = XBUTTON2; break; // MB5
    default: in.mi.dwFlags = MOUSEEVENTF_LEFTUP; break;
    }
    SendInput(1, &in, sizeof(INPUT));
}

static void DoClickOnce(int button) { DoButtonDown(button); DoButtonUp(button); }

// Key name helpers
static std::wstring VkToString(UINT vk) {
    if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) {
        wchar_t ch[2] = { (wchar_t)vk, 0 }; return ch;
    }
    if (vk >= VK_F1 && vk <= VK_F24) {
        wchar_t buf[8]; _snwprintf_s(buf, _TRUNCATE, L"F%u", vk - VK_F1 + 1); return buf;
    }
    switch (vk) {
    case VK_INSERT: return L"Ins"; case VK_DELETE: return L"Del"; case VK_HOME: return L"Home"; case VK_END: return L"End";
    case VK_PRIOR: return L"PgUp"; case VK_NEXT: return L"PgDn"; case VK_SPACE: return L"Space"; case VK_TAB: return L"Tab";
    case VK_ESCAPE: return L"Esc"; case VK_RETURN: return L"Enter"; case VK_BACK: return L"Backspace";
    case VK_LEFT: return L"Left"; case VK_RIGHT: return L"Right"; case VK_UP: return L"Up"; case VK_DOWN: return L"Down";
    default: { wchar_t b[16]; _snwprintf_s(b, _TRUNCATE, L"VK_%02X", vk); return b; }
    }
}

static std::wstring HotkeyToText(UINT vk, UINT mods) {
    std::wstring s;
    if (mods & MOD_CONTROL) { s += L"Ctrl+"; }
    if (mods & MOD_SHIFT) { s += L"Shift+"; }
    if (mods & MOD_ALT) { s += L"Alt+"; }
    s += VkToString(vk);
    return s;
}

static void SetStartBtnLabel(HWND hWnd) {
    std::wstring hk = HotkeyToText(g_hotkeyVK, g_hotkeyMods);
    std::wstring text = g_running.load() ? (L"Стоп (" + hk + L")") : (L"Старт (" + hk + L")");
    SetWindowTextW(GetDlgItem(hWnd, IDC_BTN_TOGGLE), text.c_str());
}

// Low-level mouse hook to capture next left-click anywhere
static LRESULT CALLBACK LowLevelMouseProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode == HC_ACTION) {
        const MSLLHOOKSTRUCT* p = reinterpret_cast<const MSLLHOOKSTRUCT*>(lParam);
        // If we are in pick mode: capture the first LButtonDown and swallow it
        if (g_pickMode.load(std::memory_order_relaxed)) {
            if (wParam == WM_LBUTTONDOWN) {
                PostMessageW(g_hMain, WM_APP_PICKED, (WPARAM)p->pt.x, (LPARAM)p->pt.y);
                g_pickMode.store(false, std::memory_order_relaxed);
                g_waitUp.store(true, std::memory_order_relaxed); // also swallow the matching UP
                return 1; // block this click from reaching target apps
            }
        }
        else if (g_waitUp.load(std::memory_order_relaxed)) {
            // After we captured DOWN, swallow the corresponding UP, then unhook
            if (wParam == WM_LBUTTONUP) {
                g_waitUp.store(false, std::memory_order_relaxed);
                if (g_mouseHook) { UnhookWindowsHookEx(g_mouseHook); g_mouseHook = nullptr; }
                return 1; // block the UP too
            }
        }
    }
    return CallNextHookEx(nullptr, nCode, wParam, lParam);
}

// ---------------------- Tray helpers ------------------------
static void TrayAdd(HWND hWnd) {
    if (g_hasTray) return;
    g_nid = {};
    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = hWnd;
    g_nid.uID = kTrayId;
    g_nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    g_nid.uCallbackMessage = WM_APP_TRAY;
    g_nid.hIcon = (HICON)LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDI_APPICON), IMAGE_ICON,
        GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0);
    lstrcpynW(g_nid.szTip, L"LightClick — автокликер", ARRAYSIZE(g_nid.szTip));
    Shell_NotifyIconW(NIM_ADD, &g_nid);
    g_hasTray = true;
}

static void TrayRemove() { if (!g_hasTray) return; Shell_NotifyIconW(NIM_DELETE, &g_nid); g_hasTray = false; }
static void HideToTray(HWND hWnd) { TrayAdd(hWnd); ShowWindow(hWnd, SW_HIDE); }
static void RestoreFromTray(HWND hWnd) { ShowWindow(hWnd, SW_SHOW); ShowWindow(hWnd, SW_RESTORE); SetForegroundWindow(hWnd); TrayRemove(); }

static void TrayMenu(HWND hWnd) {
    POINT pt; GetCursorPos(&pt);
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, IDM_TRAY_SHOWHIDE, IsWindowVisible(hWnd) ? L"Скрыть" : L"Показать");
    AppendMenuW(m, MF_STRING, IDM_TRAY_STARTSTOP, g_running.load() ? L"Стоп" : L"Старт");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, IDM_TRAY_EXIT, L"Выход");
    SetForegroundWindow(hWnd);
    TrackPopupMenu(m, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hWnd, nullptr);
    DestroyMenu(m);
}

// ---------------------- Settings (INI + autostart) ----------
static std::wstring IniPath() {
    wchar_t exe[MAX_PATH]; GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring path(exe); size_t pos = path.rfind(L'.'); if (pos != std::wstring::npos) path.resize(pos); path += L".ini"; return path;
}

static void SetRunAtStartup(bool enable) {
    HKEY hKey;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\Microsoft\Windows\CurrentVersion\Run", 0, KEY_SET_VALUE, &hKey) != ERROR_SUCCESS)
        return;
    wchar_t exe[MAX_PATH]; GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring cmd = L"\""; cmd += exe; cmd += L"\" /tray"; // start in tray
    if (enable) {
        RegSetValueExW(hKey, L"LightClick", 0, REG_SZ, (const BYTE*)cmd.c_str(), (DWORD)((cmd.size() + 1) * sizeof(wchar_t)));
    }
    else {
        RegDeleteValueW(hKey, L"LightClick");
    }
    RegCloseKey(hKey);
}

static void SaveSettings(HWND hWnd) {
    std::wstring ini = IniPath();
    auto W = [&](LPCWSTR section, LPCWSTR key, int v) { wchar_t b[32]; _snwprintf_s(b, _TRUNCATE, L"%d", v); WritePrivateProfileStringW(section, key, b, ini.c_str()); };

    int interval = ReadInt(GetDlgItem(hWnd, IDC_EDIT_INTERVAL), 100);
    BOOL isCps = (Button_GetCheck(GetDlgItem(hWnd, IDC_CHECK_CPS)) == BST_CHECKED);
    int btn = (int)SendMessageW(GetDlgItem(hWnd, IDC_COMBO_BUTTON), CB_GETCURSEL, 0, 0);
    BOOL dbl = (Button_GetCheck(GetDlgItem(hWnd, IDC_CHECK_DOUBLE)) == BST_CHECKED);
    BOOL fixed = (Button_GetCheck(GetDlgItem(hWnd, IDC_CHECK_FIXED)) == BST_CHECKED);
    BOOL hold = (Button_GetCheck(GetDlgItem(hWnd, IDC_CHECK_HOLD)) == BST_CHECKED);
    int x = ReadInt(GetDlgItem(hWnd, IDC_EDIT_X), 0);
    int y = ReadInt(GetDlgItem(hWnd, IDC_EDIT_Y), 0);
    int jitter = ReadInt(GetDlgItem(hWnd, IDC_EDIT_JITTER), 0);
    int stop_mode = Button_GetCheck(GetDlgItem(hWnd, IDC_RADIO_CLICKS)) == BST_CHECKED ? 1 :
        Button_GetCheck(GetDlgItem(hWnd, IDC_RADIO_SECONDS)) == BST_CHECKED ? 2 : 0;
    int max_clicks = ReadInt(GetDlgItem(hWnd, IDC_EDIT_CLICKS), 100);
    int max_seconds = ReadInt(GetDlgItem(hWnd, IDC_EDIT_SECONDS), 10);
    BOOL autostart = (Button_GetCheck(GetDlgItem(hWnd, IDC_CHECK_AUTOSTART)) == BST_CHECKED);

    W(L"Main", L"interval", interval);
    W(L"Main", L"cps", isCps);
    W(L"Main", L"button", btn);
    W(L"Main", L"double", dbl);
    W(L"Main", L"fixed", fixed);
    W(L"Main", L"x", x);
    W(L"Main", L"y", y);
    W(L"Main", L"hold", hold);
    W(L"Main", L"jitter", jitter);
    W(L"Main", L"stop_mode", stop_mode);
    W(L"Main", L"max_clicks", max_clicks);
    W(L"Main", L"max_seconds", max_seconds);
    W(L"Main", L"autostart", autostart);

    // Hotkey
    WORD hk = (WORD)SendMessageW(GetDlgItem(hWnd, IDC_HOTKEY), HKM_GETHOTKEY, 0, 0);
    BYTE vk = LOBYTE(hk); BYTE m = HIBYTE(hk);
    W(L"Hotkey", L"vk", vk ? vk : (BYTE)g_hotkeyVK);
    int modsBits = 0;
    if (m & HOTKEYF_CONTROL) modsBits |= MOD_CONTROL;
    if (m & HOTKEYF_SHIFT)   modsBits |= MOD_SHIFT;
    if (m & HOTKEYF_ALT)     modsBits |= MOD_ALT;
    W(L"Hotkey", L"mods", modsBits);

    SetRunAtStartup(autostart);
}

static void LoadSettings(HWND hWnd) {
    std::wstring ini = IniPath();
    auto R = [&](LPCWSTR s, LPCWSTR k, int def) { return GetPrivateProfileIntW(s, k, def, ini.c_str()); };

    int interval = R(L"Main", L"interval", 100);
    int isCps = R(L"Main", L"cps", 0);
    int btn = R(L"Main", L"button", 0);
    int dbl = R(L"Main", L"double", 0);
    int fixed = R(L"Main", L"fixed", 0);
    int x = R(L"Main", L"x", 0);
    int y = R(L"Main", L"y", 0);
    int hold = R(L"Main", L"hold", 0);
    int jitter = R(L"Main", L"jitter", 0);
    int stop_mode = R(L"Main", L"stop_mode", 0);
    int max_clicks = R(L"Main", L"max_clicks", 100);
    int max_seconds = R(L"Main", L"max_seconds", 10);
    int autostart = R(L"Main", L"autostart", 0);

    SetInt(GetDlgItem(hWnd, IDC_EDIT_INTERVAL), interval);
    Button_SetCheck(GetDlgItem(hWnd, IDC_CHECK_CPS), isCps ? BST_CHECKED : BST_UNCHECKED);
    SendMessageW(GetDlgItem(hWnd, IDC_COMBO_BUTTON), CB_SETCURSEL, btn, 0);
    Button_SetCheck(GetDlgItem(hWnd, IDC_CHECK_DOUBLE), dbl ? BST_CHECKED : BST_UNCHECKED);
    Button_SetCheck(GetDlgItem(hWnd, IDC_CHECK_FIXED), fixed ? BST_CHECKED : BST_UNCHECKED);
    SetInt(GetDlgItem(hWnd, IDC_EDIT_X), x);
    SetInt(GetDlgItem(hWnd, IDC_EDIT_Y), y);
    Button_SetCheck(GetDlgItem(hWnd, IDC_CHECK_HOLD), hold ? BST_CHECKED : BST_UNCHECKED);
    SetInt(GetDlgItem(hWnd, IDC_EDIT_JITTER), jitter);
    Button_SetCheck(GetDlgItem(hWnd, IDC_RADIO_INF), stop_mode == 0 ? BST_CHECKED : BST_UNCHECKED);
    Button_SetCheck(GetDlgItem(hWnd, IDC_RADIO_CLICKS), stop_mode == 1 ? BST_CHECKED : BST_UNCHECKED);
    Button_SetCheck(GetDlgItem(hWnd, IDC_RADIO_SECONDS), stop_mode == 2 ? BST_CHECKED : BST_UNCHECKED);
    SetInt(GetDlgItem(hWnd, IDC_EDIT_CLICKS), max_clicks);
    SetInt(GetDlgItem(hWnd, IDC_EDIT_SECONDS), max_seconds);
    Button_SetCheck(GetDlgItem(hWnd, IDC_CHECK_AUTOSTART), autostart ? BST_CHECKED : BST_UNCHECKED);

    // Hotkey
    g_hotkeyVK = (UINT)R(L"Hotkey", L"vk", VK_F6);
    g_hotkeyMods = (UINT)R(L"Hotkey", L"mods", 0);
    WORD hk = MAKEWORD((BYTE)g_hotkeyVK, 0);
    BYTE m = 0;
    if (g_hotkeyMods & MOD_CONTROL) m |= HOTKEYF_CONTROL;
    if (g_hotkeyMods & MOD_SHIFT)   m |= HOTKEYF_SHIFT;
    if (g_hotkeyMods & MOD_ALT)     m |= HOTKEYF_ALT;
    hk = MAKEWORD((BYTE)g_hotkeyVK, m);
    SendMessageW(GetDlgItem(hWnd, IDC_HOTKEY), HKM_SETHOTKEY, hk, 0);

    UpdateUIState(hWnd);
    SetRunAtStartup(autostart != 0);
    SetStartBtnLabel(hWnd); // <<< keep button label in sync with chosen hotkey
}

static bool ApplyHotkey(HWND hWnd) {
    // Unregister previous
    UnregisterHotKey(hWnd, g_hotkeyId);

    WORD hk = (WORD)SendMessageW(GetDlgItem(hWnd, IDC_HOTKEY), HKM_GETHOTKEY, 0, 0);
    BYTE vk = LOBYTE(hk); BYTE m = HIBYTE(hk);
    UINT mods = 0;
    if (m & HOTKEYF_CONTROL) mods |= MOD_CONTROL;
    if (m & HOTKEYF_SHIFT)   mods |= MOD_SHIFT;
    if (m & HOTKEYF_ALT)     mods |= MOD_ALT;
    if (!vk) { vk = VK_F6; mods = 0; }

    if (!RegisterHotKey(hWnd, g_hotkeyId, mods | MOD_NOREPEAT, vk)) {
        SetStatus(L"Хоткей не зарегистрирован (возможно, занят).");
        return false;
    }
    g_hotkeyVK = vk; g_hotkeyMods = mods;
    SetStatus(L"Хоткей назначен.");
    SetStartBtnLabel(hWnd); // <<< update label right away
    return true;
}

// ---------------------- Worker ------------------------------
static void Worker(ClickConfig cfg) {
    timeBeginPeriod(1); // improve Sleep precision

    // HOLD mode: press down once, wait until stopped, then release
    if (cfg.hold) {
        if (cfg.fixed) SetCursorPos(cfg.x, cfg.y);
        DoButtonDown(cfg.button);
        while (g_running.load(std::memory_order_relaxed)) { Sleep(15); }
        DoButtonUp(cfg.button);
        timeEndPeriod(1);
        return;
    }

    std::mt19937 rng((unsigned)std::chrono::high_resolution_clock::now().time_since_epoch().count());

    auto next = std::chrono::steady_clock::now();
    auto start = next;
    auto deadline = start + std::chrono::seconds(cfg.max_seconds);

    if (cfg.fixed) { SetCursorPos(cfg.x, cfg.y); }

    long long clicked = 0; // counts individual clicks; double-click adds 2
    bool autoStopped = false;

    while (g_running.load(std::memory_order_relaxed)) {
        if (cfg.stop_mode == 2) { if (std::chrono::steady_clock::now() >= deadline) { autoStopped = true; break; } }
        if (cfg.fixed) { SetCursorPos(cfg.x, cfg.y); }

        DoClickOnce(cfg.button); clicked += 1;
        if (cfg.dbl) { DWORD gap = (DWORD)max(1u, min((UINT)25, GetDoubleClickTime() / 3)); Sleep(gap); DoClickOnce(cfg.button); clicked += 1; }

        if (cfg.stop_mode == 1 && clicked >= cfg.max_clicks) { autoStopped = true; break; }

        int base = cfg.interval_ms; int jitter = (cfg.jitter_percent > 0) ? (base * cfg.jitter_percent) / 100 : 0; int delta = 0;
        if (jitter > 0) { std::uniform_int_distribution<int> dist(-jitter, +jitter); delta = dist(rng); }
        int interval = base + delta; if (interval < 1) interval = 1;

        next += std::chrono::milliseconds(interval);
        auto now = std::chrono::steady_clock::now();
        if (next > now) { auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(next - now).count(); Sleep((DWORD)ms); }
        else { next = now + std::chrono::milliseconds(interval); Sleep((DWORD)interval); }
    }

    timeEndPeriod(1);

    if (autoStopped) { g_running.store(false, std::memory_order_relaxed); PostMessageW(g_hMain, WM_APP_AUTOSTOP, 0, 0); }
}

static void StartClicking() {
    if (g_running.load()) return; ClickConfig cfg{};

    int raw = max(1, ReadInt(GetDlgItem(g_hMain, IDC_EDIT_INTERVAL), 100));
    bool isCps = (Button_GetCheck(GetDlgItem(g_hMain, IDC_CHECK_CPS)) == BST_CHECKED);
    bool isHold = (Button_GetCheck(GetDlgItem(g_hMain, IDC_CHECK_HOLD)) == BST_CHECKED);

    if (isCps) { int cps = raw; if (cps < 1) cps = 1; if (cps > 1000) cps = 1000; cfg.interval_ms = max(1, (int)(1000 / cps)); }
    else { cfg.interval_ms = raw; }

    cfg.button = (int)SendMessageW(GetDlgItem(g_hMain, IDC_COMBO_BUTTON), CB_GETCURSEL, 0, 0); if (cfg.button < 0) cfg.button = 0;
    cfg.dbl = (Button_GetCheck(GetDlgItem(g_hMain, IDC_CHECK_DOUBLE)) == BST_CHECKED) && !isHold;
    cfg.fixed = (Button_GetCheck(GetDlgItem(g_hMain, IDC_CHECK_FIXED)) == BST_CHECKED);
    cfg.x = ReadInt(GetDlgItem(g_hMain, IDC_EDIT_X), 0); cfg.y = ReadInt(GetDlgItem(g_hMain, IDC_EDIT_Y), 0);
    cfg.hold = isHold;

    if (cfg.interval_ms < 1) cfg.interval_ms = 1; if (cfg.interval_ms > 60000) cfg.interval_ms = 60000;

    if (!isHold) {
        if (Button_GetCheck(GetDlgItem(g_hMain, IDC_RADIO_CLICKS)) == BST_CHECKED) { cfg.stop_mode = 1; cfg.max_clicks = max(1, ReadInt(GetDlgItem(g_hMain, IDC_EDIT_CLICKS), 1)); }
        else if (Button_GetCheck(GetDlgItem(g_hMain, IDC_RADIO_SECONDS)) == BST_CHECKED) { cfg.stop_mode = 2; cfg.max_seconds = max(1, ReadInt(GetDlgItem(g_hMain, IDC_EDIT_SECONDS), 10)); }
        else { cfg.stop_mode = 0; }
    }

    cfg.jitter_percent = isHold ? 0 : ReadInt(GetDlgItem(g_hMain, IDC_EDIT_JITTER), 0);
    if (cfg.jitter_percent < 0) cfg.jitter_percent = 0; if (cfg.jitter_percent > 80) cfg.jitter_percent = 80;

    g_running.store(true);
    SetStartBtnLabel(g_hMain);
    SetStatus(isHold ? L"Удержание… Нажмите хоткей для отпускания." : L"Работает… Нажмите хоткей для остановки.");

    g_worker = std::thread(Worker, cfg);
}

static void StopClicking() {
    g_running.store(false); if (g_worker.joinable()) g_worker.join(); SetStartBtnLabel(g_hMain); SetStatus(L"Остановлено.");
}
static void ToggleClicking() { if (g_running.load()) StopClicking(); else StartClicking(); }

static void CreateLabeledEdit(HWND parent, int x, int y, int wLabel, const wchar_t* label, int wEdit, int idEdit, const wchar_t* initText = L"") {
    const int LABEL_H = SX(18); const int EDIT_H = SX(22); const int GAP = SX(8); const int BASEOFF = SX(3);
    int sx = SX(x); int sy = SX(y);
    CreateWindowW(L"STATIC", label, WS_CHILD | WS_VISIBLE, sx, sy + BASEOFF, SX(wLabel), LABEL_H, parent, nullptr, nullptr, nullptr);
    HWND hEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", initText, WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
        sx + SX(wLabel) + GAP, sy, SX(wEdit), EDIT_H, parent, (HMENU)(INT_PTR)idEdit, nullptr, nullptr);
    SendMessageW(hEdit, WM_SETFONT, (WPARAM)GetStockObject(DEFAULT_GUI_FONT), TRUE);
}

static void UpdateUIState(HWND hWnd) {
    BOOL cps = (Button_GetCheck(GetDlgItem(hWnd, IDC_CHECK_CPS)) == BST_CHECKED);
    BOOL hold = (Button_GetCheck(GetDlgItem(hWnd, IDC_CHECK_HOLD)) == BST_CHECKED);
    SetWindowTextW(GetDlgItem(hWnd, IDC_STATIC_UNIT), cps ? L"[CPS]" : L"[мс]");

    BOOL byClicks = (Button_GetCheck(GetDlgItem(hWnd, IDC_RADIO_CLICKS)) == BST_CHECKED);
    BOOL bySecs = (Button_GetCheck(GetDlgItem(hWnd, IDC_RADIO_SECONDS)) == BST_CHECKED);

    EnableWindow(GetDlgItem(hWnd, IDC_EDIT_INTERVAL), !hold);
    EnableWindow(GetDlgItem(hWnd, IDC_CHECK_CPS), !hold);
    EnableWindow(GetDlgItem(hWnd, IDC_CHECK_DOUBLE), !hold);
    EnableWindow(GetDlgItem(hWnd, IDC_EDIT_JITTER), !hold);

    EnableWindow(GetDlgItem(hWnd, IDC_RADIO_INF), !hold);
    EnableWindow(GetDlgItem(hWnd, IDC_RADIO_CLICKS), !hold);
    EnableWindow(GetDlgItem(hWnd, IDC_RADIO_SECONDS), !hold);
    EnableWindow(GetDlgItem(hWnd, IDC_EDIT_CLICKS), !hold && byClicks);
    EnableWindow(GetDlgItem(hWnd, IDC_EDIT_SECONDS), !hold && bySecs);
}

// ---------------------- Window / UI -------------------------
LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE: {
        g_hMain = hWnd; g_dpi = GetDpiForWindow(hWnd); HFONT hFont = (HFONT)GetStockObject(DEFAULT_GUI_FONT);

        // Interval
        CreateLabeledEdit(hWnd, 16, 20, 160, L"Интервал:", 92, IDC_EDIT_INTERVAL, L"100");
        HWND hUnit = CreateWindowW(L"STATIC", L"[мс]", WS_CHILD | WS_VISIBLE, SX(16 + 160 + 6 + 92 + 6), SX(24), SX(40), SX(18), hWnd, (HMENU)(INT_PTR)IDC_STATIC_UNIT, nullptr, nullptr);
        SendMessageW(hUnit, WM_SETFONT, (WPARAM)hFont, TRUE);

        // CPS checkbox
        HWND hCps = CreateWindowW(L"BUTTON", L"CPS режим", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
            SX(16 + 160 + 6 + 92 + 6 + 50), SX(18), SX(110), SX(24), hWnd, (HMENU)(INT_PTR)IDC_CHECK_CPS, nullptr, nullptr);
        SendMessageW(hCps, WM_SETFONT, (WPARAM)hFont, TRUE);

        // Button combobox
        CreateWindowW(L"STATIC", L"Кнопка:", WS_CHILD | WS_VISIBLE, SX(16), SX(56), SX(160), SX(18), hWnd, nullptr, nullptr, nullptr);
        HWND hCombo = CreateWindowW(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_CHILD | WS_VISIBLE,
            SX(16 + 160 + 6), SX(52), SX(160), SX(200), hWnd, (HMENU)(INT_PTR)IDC_COMBO_BUTTON, nullptr, nullptr);
        SendMessageW(hCombo, WM_SETFONT, (WPARAM)hFont, TRUE);
        SendMessageW(hCombo, CB_ADDSTRING, 0, (LPARAM)L"Левая");
        SendMessageW(hCombo, CB_ADDSTRING, 0, (LPARAM)L"Правая");
        SendMessageW(hCombo, CB_ADDSTRING, 0, (LPARAM)L"Средняя");
        SendMessageW(hCombo, CB_ADDSTRING, 0, (LPARAM)L"Боковая 1 (MB4)");
        SendMessageW(hCombo, CB_ADDSTRING, 0, (LPARAM)L"Боковая 2 (MB5)");
        SendMessageW(hCombo, CB_SETCURSEL, 0, 0);

        // Double / Hold checkboxes
        HWND hDbl = CreateWindowW(L"BUTTON", L"Двойной клик", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
            SX(16), SX(84), SX(180), SX(22), hWnd, (HMENU)(INT_PTR)IDC_CHECK_DOUBLE, nullptr, nullptr);
        SendMessageW(hDbl, WM_SETFONT, (WPARAM)hFont, TRUE);

        HWND hHold = CreateWindowW(L"BUTTON", L"Удерживать кнопку", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
            SX(200), SX(84), SX(180), SX(22), hWnd, (HMENU)(INT_PTR)IDC_CHECK_HOLD, nullptr, nullptr);
        SendMessageW(hHold, WM_SETFONT, (WPARAM)hFont, TRUE);

        // Fixed position
        HWND hFixed = CreateWindowW(L"BUTTON", L"Фиксированная позиция", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
            SX(16), SX(114), SX(200), SX(22), hWnd, (HMENU)(INT_PTR)IDC_CHECK_FIXED, nullptr, nullptr);
        SendMessageW(hFixed, WM_SETFONT, (WPARAM)hFont, TRUE);

        // X/Y edits + Pick
        CreateLabeledEdit(hWnd, 16, 146, 40, L"X:", 50, IDC_EDIT_X, L"0");
        CreateLabeledEdit(hWnd, 180, 146, 40, L"Y:", 50, IDC_EDIT_Y, L"0");
        HWND hPick = CreateWindowW(L"BUTTON", L"Выбрать место клика", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            SX(60), SX(144), SX(190), SX(26), hWnd, (HMENU)(INT_PTR)IDC_BTN_PICK, nullptr, nullptr);
        SendMessageW(hPick, WM_SETFONT, (WPARAM)hFont, TRUE);
        RECT ry; GetWindowRect(GetDlgItem(hWnd, IDC_EDIT_Y), &ry); POINT py = { ry.right, ry.top }; ScreenToClient(hWnd, &py);
        const int PAD = SX(8); SetWindowPos(hPick, nullptr, py.x + PAD, py.y - SX(1), 0, 0, SWP_NOSIZE | SWP_NOZORDER);

        // Stop mode section
        CreateWindowW(L"STATIC", L"Режим остановки:", WS_CHILD | WS_VISIBLE, SX(16), SX(180), SX(160), SX(18), hWnd, nullptr, nullptr, nullptr);
        HWND rInf = CreateWindowW(L"BUTTON", L"Бесконечно", WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON,
            SX(16), SX(202), SX(120), SX(20), hWnd, (HMENU)(INT_PTR)IDC_RADIO_INF, nullptr, nullptr);
        HWND rClicks = CreateWindowW(L"BUTTON", L"N кликов:", WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON,
            SX(16), SX(226), SX(120), SX(20), hWnd, (HMENU)(INT_PTR)IDC_RADIO_CLICKS, nullptr, nullptr);
        HWND rSecs = CreateWindowW(L"BUTTON", L"N секунд:", WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON,
            SX(16), SX(250), SX(120), SX(20), hWnd, (HMENU)(INT_PTR)IDC_RADIO_SECONDS, nullptr, nullptr);
        SendMessageW(rInf, WM_SETFONT, (WPARAM)hFont, TRUE); SendMessageW(rClicks, WM_SETFONT, (WPARAM)hFont, TRUE); SendMessageW(rSecs, WM_SETFONT, (WPARAM)hFont, TRUE);
        SendMessageW(rInf, BM_SETCHECK, BST_CHECKED, 0);

        // Edits for stop mode values
        CreateLabeledEdit(hWnd, 150, 224, 70, L"Клики:", 70, IDC_EDIT_CLICKS, L"100");
        CreateLabeledEdit(hWnd, 150, 248, 70, L"Секунды:", 70, IDC_EDIT_SECONDS, L"10");

        // Jitter
        CreateLabeledEdit(hWnd, 16, 280, 160, L"Джиттер ±%:", 92, IDC_EDIT_JITTER, L"0");

        // Hotkey + apply
        CreateWindowW(L"STATIC", L"Горячая клавиша:", WS_CHILD | WS_VISIBLE, SX(16), SX(312), SX(160), SX(20), hWnd, nullptr, nullptr, nullptr);
        HWND hHot = CreateWindowExW(0, HOTKEY_CLASS, L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
            SX(16 + 160 + 6), SX(310), SX(150), SX(22), hWnd, (HMENU)(INT_PTR)IDC_HOTKEY, GetModuleHandleW(nullptr), nullptr);
        SendMessageW(hHot, WM_SETFONT, (WPARAM)hFont, TRUE);
        HWND hApplyHK = CreateWindowW(L"BUTTON", L"Применить", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            SX(16 + 160 + 6 + 160 + 8), SX(308), SX(100), SX(26), hWnd, (HMENU)(INT_PTR)IDC_BTN_APPLYHK, nullptr, nullptr);
        SendMessageW(hApplyHK, WM_SETFONT, (WPARAM)hFont, TRUE);

        // Autostart
        HWND hAuto = CreateWindowW(L"BUTTON", L"Автозапуск при входе", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
            SX(16), SX(338), SX(220), SX(22), hWnd, (HMENU)(INT_PTR)IDC_CHECK_AUTOSTART, nullptr, nullptr);
        SendMessageW(hAuto, WM_SETFONT, (WPARAM)hFont, TRUE);

        // Start/Stop button
        HWND hToggle = CreateWindowW(L"BUTTON", L"Старт (F6)", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
            SX(16), SX(366), SX(150), SX(34), hWnd, (HMENU)(INT_PTR)IDC_BTN_TOGGLE, nullptr, nullptr);
        SendMessageW(hToggle, WM_SETFONT, (WPARAM)hFont, TRUE);

        // Status
        HWND hStatus = CreateWindowExW(WS_EX_CLIENTEDGE, L"STATIC", L"Готов.", WS_CHILD | WS_VISIBLE | SS_LEFTNOWORDWRAP,
            SX(180), SX(366), SX(340), SX(34), hWnd, (HMENU)(INT_PTR)IDC_STATUS, nullptr, nullptr);
        SendMessageW(hStatus, WM_SETFONT, (WPARAM)hFont, TRUE);

        // Init classes (ensure hotkey control available)
        INITCOMMONCONTROLSEX icc{ sizeof(icc), ICC_WIN95_CLASSES | ICC_STANDARD_CLASSES | ICC_HOTKEY_CLASS }; InitCommonControlsEx(&icc);

        // Load settings and register hotkey
        LoadSettings(hWnd);
        if (!ApplyHotkey(hWnd)) { RegisterHotKey(hWnd, g_hotkeyId, MOD_NOREPEAT, VK_F6); }
        SetStartBtnLabel(hWnd); // ensure label matches actual hotkey at startup

        return 0;
    }
    case WM_COMMAND: {
        int id = LOWORD(wParam);
        switch (id) {
        case IDC_BTN_PICK: {
            if (!g_pickMode.load()) {
                g_pickMode.store(true);
                if (!g_mouseHook) g_mouseHook = SetWindowsHookExW(WH_MOUSE_LL, LowLevelMouseProc, GetModuleHandleW(nullptr), 0);
                SetStatus(L"Выбор точки: наведите курсор и нажмите ЛКМ…");
            }
            return 0;
        }
        case IDC_BTN_TOGGLE: { ToggleClicking(); return 0; }
        case IDC_BTN_APPLYHK: { ApplyHotkey(hWnd); SaveSettings(hWnd); return 0; }
        case IDM_TRAY_SHOWHIDE: { if (IsWindowVisible(hWnd)) HideToTray(hWnd); else RestoreFromTray(hWnd); return 0; }
        case IDM_TRAY_STARTSTOP: { ToggleClicking(); return 0; }
        case IDM_TRAY_EXIT: { TrayRemove(); DestroyWindow(hWnd); return 0; }
        case IDC_CHECK_CPS:
        case IDC_CHECK_HOLD:
        case IDC_RADIO_INF:
        case IDC_RADIO_CLICKS:
        case IDC_RADIO_SECONDS: { UpdateUIState(hWnd); return 0; }
        }
        break;
    }
    case WM_APP_PICKED: {
        int x = (int)(INT_PTR)wParam; int y = (int)(INT_PTR)lParam;
        SetInt(GetDlgItem(hWnd, IDC_EDIT_X), x); SetInt(GetDlgItem(hWnd, IDC_EDIT_Y), y);
        Button_SetCheck(GetDlgItem(hWnd, IDC_CHECK_FIXED), BST_CHECKED);
        SetStatus(L"Точка выбрана. Координаты применены.");
        return 0;
    }
    case WM_APP_AUTOSTOP: {
        if (g_worker.joinable()) g_worker.join();
        SetStartBtnLabel(hWnd);
        SetStatus(L"Остановлено (условие выполнено).");
        return 0;
    }
    case WM_APP_TRAY: {
        switch (LOWORD(lParam)) {
        case WM_LBUTTONDBLCLK:
        case WM_LBUTTONUP: RestoreFromTray(hWnd); return 0;
        case WM_RBUTTONUP: TrayMenu(hWnd); return 0;
        }
        break;
    }
    case WM_SIZE: { if (wParam == SIZE_MINIMIZED) { HideToTray(hWnd); return 0; } break; }
    case WM_HOTKEY: { if ((UINT)wParam == g_hotkeyId) { ToggleClicking(); return 0; } break; }
    case WM_CLOSE: { HideToTray(hWnd); return 0; }
    case WM_DESTROY: { SaveSettings(hWnd); UnregisterHotKey(hWnd, g_hotkeyId); TrayRemove(); PostQuitMessage(0); return 0; }
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR, int nShow) {
    // Better DPI awareness if available (fallback to simple aware)
    HMODULE hUser32 = GetModuleHandleW(L"user32.dll");
    typedef BOOL(WINAPI* SetDpiCtxFn)(HANDLE);
    auto pSetCtx = (SetDpiCtxFn)GetProcAddress(hUser32, "SetProcessDpiAwarenessContext");
    if (pSetCtx) pSetCtx((HANDLE)-4 /*DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2*/); else SetProcessDPIAware();

    INITCOMMONCONTROLSEX icc{ sizeof(icc), ICC_WIN95_CLASSES | ICC_STANDARD_CLASSES | ICC_HOTKEY_CLASS }; InitCommonControlsEx(&icc);

    const wchar_t* kClass = L"AutoClickerWndClass";
    WNDCLASSW wc{}; wc.lpfnWndProc = WndProc; wc.hInstance = hInst; wc.lpszClassName = kClass; wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(hInst, MAKEINTRESOURCEW(IDI_APPICON)); wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    if (!RegisterClassW(&wc)) return 0;

    bool startTray = false; { LPCWSTR cmd = GetCommandLineW(); if (wcsstr(cmd, L"/tray") || wcsstr(cmd, L"-tray")) startTray = true; }

    HWND hWnd = CreateWindowExW(WS_EX_APPWINDOW, kClass, L"LightClick",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        CW_USEDEFAULT, CW_USEDEFAULT, SX(600), SX(520),
        nullptr, nullptr, hInst, nullptr);
    if (!hWnd) return 0;

    HICON hBig = (HICON)LoadImageW(hInst, MAKEINTRESOURCEW(IDI_APPICON), IMAGE_ICON, GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), 0);
    HICON hSmall = (HICON)LoadImageW(hInst, MAKEINTRESOURCEW(IDI_APPICON), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0);
    SendMessageW(hWnd, WM_SETICON, ICON_BIG, (LPARAM)hBig);
    SendMessageW(hWnd, WM_SETICON, ICON_SMALL, (LPARAM)hSmall);

    if (!startTray) { ShowWindow(hWnd, nShow); UpdateWindow(hWnd); }
    else { TrayAdd(hWnd); ShowWindow(hWnd, SW_HIDE); }

    MSG msg; while (GetMessageW(&msg, nullptr, 0, 0) > 0) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    return 0;
}
