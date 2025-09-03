// AutoClicker.cpp — Win32 C++ autoclicker (single-file build) with RU/EN language switch
// Build (MSVC x86/x64):
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
// Visual styles for controls (keep it strictly one line)
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
    IDC_CHECK_AUTOSTART = 121,

    // Sequence mode controls
    IDC_CHECK_SEQUENCE = 122,
    IDC_LIST_SEQ = 123,
    IDC_BTN_ADD_STEP = 124,
    IDC_BTN_REMOVE_STEP = 125,
    IDC_BTN_UP = 126,
    IDC_BTN_DOWN = 127,
    IDC_EDIT_STEP_DELAY = 128,

    // Language
    IDC_BTN_LANG = 129,

    // Static labels (for localization)
    IDC_LBL_INTERVAL = 200,
    IDC_LBL_BUTTON = 201,
    IDC_LBL_STOPMODE = 202,
    IDC_LBL_CLICKS = 203,
    IDC_LBL_SECONDS = 204,
    IDC_LBL_JITTER = 205,
    IDC_LBL_HOTKEY = 206,
    IDC_LBL_SEQ_DELAY = 207
};

// Tray menu command IDs
enum : int { IDM_TRAY_SHOWHIDE = 40001, IDM_TRAY_STARTSTOP, IDM_TRAY_EXIT };

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
static std::atomic<bool> g_pickMode{ false }; // single fixed-point picker
static std::atomic<bool> g_pickSeq{ false };  // sequence pick
static std::atomic<bool> g_waitUp{ false };   // swallow matching UP
static HHOOK g_mouseHook = nullptr;

// Tray state
static bool g_hasTray = false;
static NOTIFYICONDATAW g_nid{};
static const UINT kTrayId = 1001;

// Language
enum Lang { LANG_RU = 0, LANG_EN = 1 };
static int g_lang = LANG_RU;

// Strings
enum SId : int {
    S_INTERVAL, S_UNIT_MS, S_CPS_MODE, S_BUTTON,
    S_BTN_LEFT, S_BTN_RIGHT, S_BTN_MIDDLE, S_BTN_X1, S_BTN_X2,
    S_DBL, S_HOLD, S_FIXED, S_PICK,
    S_STOPMODE, S_INF, S_NCLICKS, S_NSECS,
    S_JITTER, S_HOTKEY, S_APPLY, S_AUTOSTART,
    S_SEQ_CHECK, S_SEQ_DELAY, S_ADD_POINT,
    S_COL_NUM, S_COL_X, S_COL_Y, S_COL_INTERVAL,
    S_DELETE, S_UP, S_DOWN,
    S_START, S_STOP, S_READY,
    S_PICK_PROMPT, S_ADD_PROMPT, S_SEQ_ENABLE_FIRST,
    S_POINT_ADDED, S_POINT_APPLIED,
    S_HK_FAIL, S_HK_OK,
    S_RUNNING, S_HOLDING, S_SEQ_RUNNING,
    S_STOPPED, S_STOPPED_COND,
    S_TRAY_TIP, S_MENU_SHOW, S_MENU_HIDE, S_MENU_START, S_MENU_STOP, S_MENU_EXIT,
    S_LANG_BTN
};

static const wchar_t* RU[] = {
    L"Интервал:", L"[мс]", L"CPS режим", L"Кнопка:",
    L"Левая", L"Правая", L"Средняя", L"Боковая 1 (MB4)", L"Боковая 2 (MB5)",
    L"Двойной клик", L"Удерживать кнопку", L"Фиксированная позиция", L"Выбрать место клика",
    L"Режим остановки:", L"Бесконечно", L"N кликов:", L"N секунд:",
    L"Джиттер ±%:", L"Горячая клавиша:", L"Применить", L"Автозапуск при входе",
    L"Режим сценария (последовательные клики)", L"Задержка перед точкой (мс):", L"Добавить точку (выбор кликом)",
    L"#", L"X", L"Y", L"Интервал, мс",
    L"Удалить", L"Вверх", L"Вниз",
    L"Старт", L"Стоп", L"Готов.",
    L"Выбор точки: наведите курсор и нажмите ЛКМ…", L"Добавление точки: наведите и кликните ЛКМ…", L"Сначала включите режим сценария.",
    L"Точка добавлена в сценарий.", L"Точка выбрана. Координаты применены.",
    L"Хоткей не зарегистрирован (возможно, занят).", L"Хоткей назначен.",
    L"Работает… Нажмите хоткей для остановки.", L"Удержание… Нажмите хоткей для отпускания.", L"Сценарий запущен… Нажмите хоткей для остановки.",
    L"Остановлено.", L"Остановлено (условие выполнено).",
    L"LightClick — автокликер", L"Показать", L"Скрыть", L"Старт", L"Стоп", L"Выход",
    L"Язык: Русский"
};
static const wchar_t* EN[] = {
    L"Interval:", L"[ms]", L"CPS mode", L"Mouse button:",
    L"Left", L"Right", L"Middle", L"Side 1 (MB4)", L"Side 2 (MB5)",
    L"Double click", L"Hold button", L"Fixed position", L"Pick position",
    L"Stop mode:", L"Infinite", L"N clicks:", L"N seconds:",
    L"Jitter ±%:", L"Hotkey:", L"Apply", L"Run at login",
    L"Sequence mode (multi-points)", L"Delay before point (ms):", L"Add point (pick by click)",
    L"#", L"X", L"Y", L"Interval, ms",
    L"Delete", L"Up", L"Down",
    L"Start", L"Stop", L"Ready.",
    L"Pick: move cursor and press LMB…", L"Add point: move and click LMB…", L"Enable sequence mode first.",
    L"Point added to sequence.", L"Point selected. Coordinates applied.",
    L"Hotkey not registered (maybe in use).", L"Hotkey applied.",
    L"Running… Press hotkey to stop.", L"Holding… Press hotkey to release.", L"Sequence started… Press hotkey to stop.",
    L"Stopped.", L"Stopped (condition met).",
    L"LightClick — autoclicker", L"Show", L"Hide", L"Start", L"Stop", L"Exit",
    L"Language: English"
};
static inline LPCWSTR LS(SId id) { return (g_lang == LANG_EN) ? EN[id] : RU[id]; }

// ---------------------- Data -------------------------------
struct Step { int x = 0, y = 0, delay_ms = 100; };
static std::vector<Step> g_steps; // in-memory list for sequence mode

struct ClickConfig {
    int interval_ms = 100; // base interval in milliseconds (already converted if CPS). Ignored in sequence mode.
    int button = 0;        // 0=Left,1=Right,2=Middle,3=MB4(X1),4=MB5(X2)
    bool dbl = false;
    bool fixed = false;
    int x = 0, y = 0;

    bool hold = false;     // hold mode: press down on start, release on stop (disabled in sequence)

    // stop conditions
    int stop_mode = 0;     // 0=infinite, 1=max clicks, 2=max seconds
    int max_clicks = 0;    // valid if stop_mode==1 (counts individual clicks, double=2)
    int max_seconds = 0;   // valid if stop_mode==2

    // jitter
    int jitter_percent = 0; // 0..80; randomized each tick as ±percent of base or step delay

    // sequence
    bool sequence = false;
    std::vector<Step> steps; // copied from g_steps at start
};

// ---------------------- Small helpers -----------------------
static int ReadInt(HWND hEdit, int fallback) {
    wchar_t buf[64]{}; GetWindowTextW(hEdit, buf, 63);
    if (buf[0] == L' ') return fallback;
    wchar_t* end = nullptr; long v = wcstol(buf, &end, 10);
    if (end == buf) return fallback;
    if (v < INT_MIN) v = INT_MIN; if (v > INT_MAX) v = INT_MAX;
    return (int)v;
}
static void SetInt(HWND hEdit, int value) { wchar_t buf[32]; _snwprintf_s(buf, _TRUNCATE, L"%d", value); SetWindowTextW(hEdit, buf); }
static void SetStatus(const wchar_t* s) { if (HWND h = GetDlgItem(g_hMain, IDC_STATUS)) SetWindowTextW(h, s); }

// Key name helpers
static std::wstring VkToString(UINT vk) {
    if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) { wchar_t ch[2] = { (wchar_t)vk, 0 }; return ch; }
    if (vk >= VK_F1 && vk <= VK_F24) { wchar_t buf[8]; _snwprintf_s(buf, _TRUNCATE, L"F%u", vk - VK_F1 + 1); return buf; }
    switch (vk) {
    case VK_INSERT: return L"Ins"; case VK_DELETE: return L"Del"; case VK_HOME: return L"Home"; case VK_END: return L"End";
    case VK_PRIOR: return L"PgUp"; case VK_NEXT: return L"PgDn"; case VK_SPACE: return L"Space"; case VK_TAB: return L"Tab";
    case VK_ESCAPE: return L"Esc"; case VK_RETURN: return L"Enter"; case VK_BACK: return L"Backspace";
    case VK_LEFT: return L"Left"; case VK_RIGHT: return L"Right"; case VK_UP: return L"Up"; case VK_DOWN: return L"Down";
    default: { wchar_t b[16]; _snwprintf_s(b, _TRUNCATE, L"VK_%02X", vk); return b; }
    }
}
static std::wstring HotkeyToText(UINT vk, UINT mods) { std::wstring s; if (mods & MOD_CONTROL) s += L"Ctrl+"; if (mods & MOD_SHIFT) s += L"Shift+"; if (mods & MOD_ALT) s += L"Alt+"; s += VkToString(vk); return s; }
static void SetStartBtnLabel(HWND hWnd) { std::wstring hk = HotkeyToText(g_hotkeyVK, g_hotkeyMods); std::wstring text = g_running.load() ? (std::wstring(LS(S_STOP)) + L" (" + hk + L")") : (std::wstring(LS(S_START)) + L" (" + hk + L")"); SetWindowTextW(GetDlgItem(hWnd, IDC_BTN_TOGGLE), text.c_str()); }

// ---------------------- Tray helpers ------------------------
static void TrayAdd(HWND hWnd) {
    if (g_hasTray) return; g_nid = {}; g_nid.cbSize = sizeof(g_nid); g_nid.hWnd = hWnd; g_nid.uID = kTrayId;
    g_nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP; g_nid.uCallbackMessage = WM_APP_TRAY;
    g_nid.hIcon = (HICON)LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDI_APPICON), IMAGE_ICON,
        GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0);
    lstrcpynW(g_nid.szTip, LS(S_TRAY_TIP), ARRAYSIZE(g_nid.szTip)); Shell_NotifyIconW(NIM_ADD, &g_nid); g_hasTray = true;
}
static void TrayRemove() { if (!g_hasTray) return; Shell_NotifyIconW(NIM_DELETE, &g_nid); g_hasTray = false; }
static void HideToTray(HWND hWnd) { TrayAdd(hWnd); ShowWindow(hWnd, SW_HIDE); }
static void RestoreFromTray(HWND hWnd) { ShowWindow(hWnd, SW_SHOW); ShowWindow(hWnd, SW_RESTORE); SetForegroundWindow(hWnd); TrayRemove(); }
static void TrayMenu(HWND hWnd) {
    POINT pt; GetCursorPos(&pt); HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, IDM_TRAY_SHOWHIDE, IsWindowVisible(hWnd) ? LS(S_MENU_HIDE) : LS(S_MENU_SHOW));
    AppendMenuW(m, MF_STRING, IDM_TRAY_STARTSTOP, g_running.load() ? LS(S_MENU_STOP) : LS(S_MENU_START));
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, IDM_TRAY_EXIT, LS(S_MENU_EXIT));
    SetForegroundWindow(hWnd); TrackPopupMenu(m, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hWnd, nullptr); DestroyMenu(m);
}
static void TrayUpdateTip() { if (!g_hasTray) return; lstrcpynW(g_nid.szTip, LS(S_TRAY_TIP), ARRAYSIZE(g_nid.szTip)); Shell_NotifyIconW(NIM_MODIFY, &g_nid); }

// ---------------------- INI + autostart ---------------------
static std::wstring IniPath() { wchar_t exe[MAX_PATH]; GetModuleFileNameW(nullptr, exe, MAX_PATH); std::wstring p(exe); size_t pos = p.rfind(L'.'); if (pos != std::wstring::npos) p.resize(pos); p += L".ini"; return p; }
static void SetRunAtStartup(bool enable) {
    HKEY hKey; if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, KEY_SET_VALUE, &hKey) != ERROR_SUCCESS) return;
    wchar_t exe[MAX_PATH]; GetModuleFileNameW(nullptr, exe, MAX_PATH); std::wstring cmd = L"\""; cmd += exe; cmd += L"\" /tray"; if (enable) RegSetValueExW(hKey, L"LightClick", 0, REG_SZ, (const BYTE*)cmd.c_str(), (DWORD)((cmd.size() + 1) * sizeof(wchar_t))); else RegDeleteValueW(hKey, L"LightClick"); RegCloseKey(hKey);
}
static void SaveSettings(HWND hWnd) {
    std::wstring ini = IniPath(); auto W = [&](LPCWSTR s, LPCWSTR k, int v) { wchar_t b[32]; _snwprintf_s(b, _TRUNCATE, L"%d", v); WritePrivateProfileStringW(s, k, b, ini.c_str()); };
    int interval = ReadInt(GetDlgItem(hWnd, IDC_EDIT_INTERVAL), 100); BOOL isCps = (Button_GetCheck(GetDlgItem(hWnd, IDC_CHECK_CPS)) == BST_CHECKED);
    int btn = (int)SendMessageW(GetDlgItem(hWnd, IDC_COMBO_BUTTON), CB_GETCURSEL, 0, 0); BOOL dbl = (Button_GetCheck(GetDlgItem(hWnd, IDC_CHECK_DOUBLE)) == BST_CHECKED);
    BOOL fixed = (Button_GetCheck(GetDlgItem(hWnd, IDC_CHECK_FIXED)) == BST_CHECKED); BOOL hold = (Button_GetCheck(GetDlgItem(hWnd, IDC_CHECK_HOLD)) == BST_CHECKED);
    int x = ReadInt(GetDlgItem(hWnd, IDC_EDIT_X), 0); int y = ReadInt(GetDlgItem(hWnd, IDC_EDIT_Y), 0); int jitter = ReadInt(GetDlgItem(hWnd, IDC_EDIT_JITTER), 0);
    int stop_mode = Button_GetCheck(GetDlgItem(hWnd, IDC_RADIO_CLICKS)) == BST_CHECKED ? 1 : Button_GetCheck(GetDlgItem(hWnd, IDC_RADIO_SECONDS)) == BST_CHECKED ? 2 : 0;
    int max_clicks = ReadInt(GetDlgItem(hWnd, IDC_EDIT_CLICKS), 100); int max_seconds = ReadInt(GetDlgItem(hWnd, IDC_EDIT_SECONDS), 10);
    BOOL autostart = (Button_GetCheck(GetDlgItem(hWnd, IDC_CHECK_AUTOSTART)) == BST_CHECKED); BOOL sequence = (Button_GetCheck(GetDlgItem(hWnd, IDC_CHECK_SEQUENCE)) == BST_CHECKED);
    W(L"Main", L"interval", interval); W(L"Main", L"cps", isCps); W(L"Main", L"button", btn); W(L"Main", L"double", dbl); W(L"Main", L"fixed", fixed); W(L"Main", L"x", x); W(L"Main", L"y", y); W(L"Main", L"hold", hold); W(L"Main", L"jitter", jitter); W(L"Main", L"stop_mode", stop_mode); W(L"Main", L"max_clicks", max_clicks); W(L"Main", L"max_seconds", max_seconds); W(L"Main", L"autostart", autostart); W(L"Main", L"sequence", sequence); W(L"Main", L"lang", g_lang);
    // Hotkey
    WORD hk = (WORD)SendMessageW(GetDlgItem(hWnd, IDC_HOTKEY), HKM_GETHOTKEY, 0, 0); BYTE vk = LOBYTE(hk); BYTE m = HIBYTE(hk);
    W(L"Hotkey", L"vk", vk ? vk : (BYTE)g_hotkeyVK); int modsBits = 0; if (m & HOTKEYF_CONTROL) modsBits |= MOD_CONTROL; if (m & HOTKEYF_SHIFT) modsBits |= MOD_SHIFT; if (m & HOTKEYF_ALT) modsBits |= MOD_ALT; W(L"Hotkey", L"mods", modsBits);
    // Sequence
    WritePrivateProfileStringW(L"Seq", L"", L"", ini.c_str()); W(L"Seq", L"count", (int)g_steps.size()); for (size_t i = 0; i < g_steps.size(); ++i) { wchar_t kx[32], ky[32], kd[32]; _snwprintf_s(kx, _TRUNCATE, L"x%u", (unsigned)i); _snwprintf_s(ky, _TRUNCATE, L"y%u", (unsigned)i); _snwprintf_s(kd, _TRUNCATE, L"d%u", (unsigned)i); W(L"Seq", kx, g_steps[i].x); W(L"Seq", ky, g_steps[i].y); W(L"Seq", kd, g_steps[i].delay_ms); } SetRunAtStartup(autostart);
}
static void LoadSettings(HWND hWnd) {
    std::wstring ini = IniPath(); auto R = [&](LPCWSTR s, LPCWSTR k, int d) { return GetPrivateProfileIntW(s, k, d, ini.c_str()); };
    // Language first
    g_lang = R(L"Main", L"lang", LANG_RU);

    int interval = R(L"Main", L"interval", 100); int isCps = R(L"Main", L"cps", 0); int btn = R(L"Main", L"button", 0); int dbl = R(L"Main", L"double", 0); int fixed = R(L"Main", L"fixed", 0);
    int x = R(L"Main", L"x", 0); int y = R(L"Main", L"y", 0); int hold = R(L"Main", L"hold", 0); int jitter = R(L"Main", L"jitter", 0); int stop_mode = R(L"Main", L"stop_mode", 0);
    int max_clicks = R(L"Main", L"max_clicks", 100); int max_seconds = R(L"Main", L"max_seconds", 10); int autostart = R(L"Main", L"autostart", 0); int sequence = R(L"Main", L"sequence", 0);
    SetInt(GetDlgItem(hWnd, IDC_EDIT_INTERVAL), interval); Button_SetCheck(GetDlgItem(hWnd, IDC_CHECK_CPS), isCps ? BST_CHECKED : BST_UNCHECKED);
    SendMessageW(GetDlgItem(hWnd, IDC_COMBO_BUTTON), CB_SETCURSEL, btn, 0); Button_SetCheck(GetDlgItem(hWnd, IDC_CHECK_DOUBLE), dbl ? BST_CHECKED : BST_UNCHECKED);
    Button_SetCheck(GetDlgItem(hWnd, IDC_CHECK_FIXED), fixed ? BST_CHECKED : BST_UNCHECKED); SetInt(GetDlgItem(hWnd, IDC_EDIT_X), x); SetInt(GetDlgItem(hWnd, IDC_EDIT_Y), y);
    Button_SetCheck(GetDlgItem(hWnd, IDC_CHECK_HOLD), hold ? BST_CHECKED : BST_UNCHECKED); SetInt(GetDlgItem(hWnd, IDC_EDIT_JITTER), jitter);
    Button_SetCheck(GetDlgItem(hWnd, IDC_RADIO_INF), stop_mode == 0 ? BST_CHECKED : BST_UNCHECKED); Button_SetCheck(GetDlgItem(hWnd, IDC_RADIO_CLICKS), stop_mode == 1 ? BST_CHECKED : BST_UNCHECKED); Button_SetCheck(GetDlgItem(hWnd, IDC_RADIO_SECONDS), stop_mode == 2 ? BST_CHECKED : BST_UNCHECKED);
    SetInt(GetDlgItem(hWnd, IDC_EDIT_CLICKS), max_clicks); SetInt(GetDlgItem(hWnd, IDC_EDIT_SECONDS), max_seconds); Button_SetCheck(GetDlgItem(hWnd, IDC_CHECK_AUTOSTART), autostart ? BST_CHECKED : BST_UNCHECKED); Button_SetCheck(GetDlgItem(hWnd, IDC_CHECK_SEQUENCE), sequence ? BST_CHECKED : BST_UNCHECKED);
    // Sequence
    g_steps.clear(); int cnt = R(L"Seq", L"count", 0); for (int i = 0; i < cnt; ++i) { wchar_t kx[32], ky[32], kd[32]; _snwprintf_s(kx, _TRUNCATE, L"x%d", i); _snwprintf_s(ky, _TRUNCATE, L"y%d", i); _snwprintf_s(kd, _TRUNCATE, L"d%d", i); Step s; s.x = R(L"Seq", kx, 0); s.y = R(L"Seq", ky, 0); s.delay_ms = R(L"Seq", kd, 100); g_steps.push_back(s); } SetRunAtStartup(autostart != 0);
}

// ---------------------- UI state ----------------------------
static void UpdateButtonCombo(HWND hWnd) {
    HWND cb = GetDlgItem(hWnd, IDC_COMBO_BUTTON); int sel = (int)SendMessageW(cb, CB_GETCURSEL, 0, 0);
    SendMessageW(cb, CB_RESETCONTENT, 0, 0);
    SendMessageW(cb, CB_ADDSTRING, 0, (LPARAM)LS(S_BTN_LEFT));
    SendMessageW(cb, CB_ADDSTRING, 0, (LPARAM)LS(S_BTN_RIGHT));
    SendMessageW(cb, CB_ADDSTRING, 0, (LPARAM)LS(S_BTN_MIDDLE));
    SendMessageW(cb, CB_ADDSTRING, 0, (LPARAM)LS(S_BTN_X1));
    SendMessageW(cb, CB_ADDSTRING, 0, (LPARAM)LS(S_BTN_X2));
    SendMessageW(cb, CB_SETCURSEL, sel >= 0 ? sel : 0, 0);
}

static void UpdateTexts(HWND hWnd) {
    // Labels/static
    SetWindowTextW(GetDlgItem(hWnd, IDC_LBL_INTERVAL), LS(S_INTERVAL));
    SetWindowTextW(GetDlgItem(hWnd, IDC_CHECK_CPS), LS(S_CPS_MODE));
    SetWindowTextW(GetDlgItem(hWnd, IDC_LBL_BUTTON), LS(S_BUTTON));
    SetWindowTextW(GetDlgItem(hWnd, IDC_CHECK_DOUBLE), LS(S_DBL));
    SetWindowTextW(GetDlgItem(hWnd, IDC_CHECK_HOLD), LS(S_HOLD));
    SetWindowTextW(GetDlgItem(hWnd, IDC_CHECK_FIXED), LS(S_FIXED));
    SetWindowTextW(GetDlgItem(hWnd, IDC_BTN_PICK), LS(S_PICK));
    SetWindowTextW(GetDlgItem(hWnd, IDC_LBL_STOPMODE), LS(S_STOPMODE));
    SetWindowTextW(GetDlgItem(hWnd, IDC_RADIO_INF), LS(S_INF));
    SetWindowTextW(GetDlgItem(hWnd, IDC_LBL_CLICKS), LS(S_NCLICKS));
    SetWindowTextW(GetDlgItem(hWnd, IDC_LBL_SECONDS), LS(S_NSECS));
    SetWindowTextW(GetDlgItem(hWnd, IDC_LBL_JITTER), LS(S_JITTER));
    SetWindowTextW(GetDlgItem(hWnd, IDC_LBL_HOTKEY), LS(S_HOTKEY));
    SetWindowTextW(GetDlgItem(hWnd, IDC_BTN_APPLYHK), LS(S_APPLY));
    SetWindowTextW(GetDlgItem(hWnd, IDC_CHECK_AUTOSTART), LS(S_AUTOSTART));
    SetWindowTextW(GetDlgItem(hWnd, IDC_CHECK_SEQUENCE), LS(S_SEQ_CHECK));
    SetWindowTextW(GetDlgItem(hWnd, IDC_LBL_SEQ_DELAY), LS(S_SEQ_DELAY));
    SetWindowTextW(GetDlgItem(hWnd, IDC_BTN_ADD_STEP), LS(S_ADD_POINT));
    SetWindowTextW(GetDlgItem(hWnd, IDC_BTN_LANG), LS(S_LANG_BTN));

    // Unit label depends on CPS + lang
    SetWindowTextW(GetDlgItem(hWnd, IDC_STATIC_UNIT), LS(S_UNIT_MS));

    // ListView columns
    HWND lv = GetDlgItem(hWnd, IDC_LIST_SEQ);
    if (lv) {
        LVCOLUMNW c{}; c.mask = LVCF_TEXT;
        c.pszText = (LPWSTR)LS(S_COL_NUM); ListView_SetColumn(lv, 0, &c);
        c.pszText = (LPWSTR)LS(S_COL_X);   ListView_SetColumn(lv, 1, &c);
        c.pszText = (LPWSTR)LS(S_COL_Y);   ListView_SetColumn(lv, 2, &c);
        c.pszText = (LPWSTR)LS(S_COL_INTERVAL); ListView_SetColumn(lv, 3, &c);
    }

    // Button combo items
    UpdateButtonCombo(hWnd);

    // Start button text
    SetStartBtnLabel(hWnd);

    // Tray tip
    TrayUpdateTip();
}

static void UpdateUIState(HWND hWnd) {
    BOOL cps = (Button_GetCheck(GetDlgItem(hWnd, IDC_CHECK_CPS)) == BST_CHECKED);
    BOOL hold = (Button_GetCheck(GetDlgItem(hWnd, IDC_CHECK_HOLD)) == BST_CHECKED);
    BOOL seq = (Button_GetCheck(GetDlgItem(hWnd, IDC_CHECK_SEQUENCE)) == BST_CHECKED);
    SetWindowTextW(GetDlgItem(hWnd, IDC_STATIC_UNIT), cps ? L"[CPS]" : LS(S_UNIT_MS));
    BOOL byClicks = (Button_GetCheck(GetDlgItem(hWnd, IDC_RADIO_CLICKS)) == BST_CHECKED);
    BOOL bySecs = (Button_GetCheck(GetDlgItem(hWnd, IDC_RADIO_SECONDS)) == BST_CHECKED);
    EnableWindow(GetDlgItem(hWnd, IDC_EDIT_INTERVAL), !seq && !hold);
    EnableWindow(GetDlgItem(hWnd, IDC_CHECK_CPS), !seq && !hold);
    EnableWindow(GetDlgItem(hWnd, IDC_CHECK_DOUBLE), TRUE);
    EnableWindow(GetDlgItem(hWnd, IDC_EDIT_JITTER), !hold);
    EnableWindow(GetDlgItem(hWnd, IDC_CHECK_FIXED), !seq);
    EnableWindow(GetDlgItem(hWnd, IDC_EDIT_X), !seq);
    EnableWindow(GetDlgItem(hWnd, IDC_EDIT_Y), !seq);
    EnableWindow(GetDlgItem(hWnd, IDC_BTN_PICK), !seq);
    EnableWindow(GetDlgItem(hWnd, IDC_CHECK_HOLD), !seq);
    EnableWindow(GetDlgItem(hWnd, IDC_RADIO_INF), !hold);
    EnableWindow(GetDlgItem(hWnd, IDC_RADIO_CLICKS), !hold);
    EnableWindow(GetDlgItem(hWnd, IDC_RADIO_SECONDS), !hold);
    EnableWindow(GetDlgItem(hWnd, IDC_EDIT_CLICKS), !hold && byClicks);
    EnableWindow(GetDlgItem(hWnd, IDC_EDIT_SECONDS), !hold && bySecs);
    EnableWindow(GetDlgItem(hWnd, IDC_LIST_SEQ), seq);
    EnableWindow(GetDlgItem(hWnd, IDC_BTN_ADD_STEP), seq);
    EnableWindow(GetDlgItem(hWnd, IDC_BTN_REMOVE_STEP), seq);
    EnableWindow(GetDlgItem(hWnd, IDC_BTN_UP), seq);
    EnableWindow(GetDlgItem(hWnd, IDC_BTN_DOWN), seq);
    EnableWindow(GetDlgItem(hWnd, IDC_EDIT_STEP_DELAY), seq);
}

// ---------------------- Picker hook -------------------------
static LRESULT CALLBACK LowLevelMouseProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode == HC_ACTION) {
        const MSLLHOOKSTRUCT* p = reinterpret_cast<const MSLLHOOKSTRUCT*>(lParam);
        if (g_pickMode.load(std::memory_order_relaxed) || g_pickSeq.load(std::memory_order_relaxed)) {
            if (wParam == WM_LBUTTONDOWN) {
                PostMessageW(g_hMain, WM_APP_PICKED, (WPARAM)p->pt.x, (LPARAM)p->pt.y);
                g_waitUp.store(true, std::memory_order_relaxed);
                g_pickMode.store(false, std::memory_order_relaxed);
                return 1; // swallow DOWN
            }
        }
        else if (g_waitUp.load(std::memory_order_relaxed)) {
            if (wParam == WM_LBUTTONUP) {
                g_waitUp.store(false, std::memory_order_relaxed);
                if (g_mouseHook) { UnhookWindowsHookEx(g_mouseHook); g_mouseHook = nullptr; }
                return 1; // swallow UP
            }
        }
    }
    return CallNextHookEx(nullptr, nCode, wParam, lParam);
}

// ---------------------- Sequence LV helpers -----------------
static void RefreshSequenceList(HWND hWnd) {
    HWND lv = GetDlgItem(hWnd, IDC_LIST_SEQ); if (!lv) return; ListView_DeleteAllItems(lv);
    for (size_t i = 0; i < g_steps.size(); ++i) {
        wchar_t buf[32]; LVITEMW it{}; it.mask = LVIF_TEXT; it.iItem = (int)i; it.iSubItem = 0;
        _snwprintf_s(buf, _TRUNCATE, L"%u", (unsigned)(i + 1)); it.pszText = buf; ListView_InsertItem(lv, &it);
        _snwprintf_s(buf, _TRUNCATE, L"%d", g_steps[i].x); ListView_SetItemText(lv, (int)i, 1, buf);
        _snwprintf_s(buf, _TRUNCATE, L"%d", g_steps[i].y); ListView_SetItemText(lv, (int)i, 2, buf);
        _snwprintf_s(buf, _TRUNCATE, L"%d", g_steps[i].delay_ms); ListView_SetItemText(lv, (int)i, 3, buf);
    }
}
static int GetSelectedStep(HWND hWnd) { HWND lv = GetDlgItem(hWnd, IDC_LIST_SEQ); return ListView_GetNextItem(lv, -1, LVNI_SELECTED); }
static void MoveSelectedStep(HWND hWnd, int dir) { int idx = GetSelectedStep(hWnd); if (idx < 0) return; int j = idx + dir; if (j < 0 || j >= (int)g_steps.size()) return; std::swap(g_steps[idx], g_steps[j]); RefreshSequenceList(hWnd); HWND lv = GetDlgItem(hWnd, IDC_LIST_SEQ); ListView_SetItemState(lv, j, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED); }
static void RemoveSelectedStep(HWND hWnd) { int idx = GetSelectedStep(hWnd); if (idx < 0) return; g_steps.erase(g_steps.begin() + idx); RefreshSequenceList(hWnd); }

// ---------------------- Worker ------------------------------
// Универсальный отправитель
static inline void SendMouse(UINT flags, DWORD data = 0) {
    INPUT in{};
    in.type = INPUT_MOUSE;
    in.mi.dx = 0;
    in.mi.dy = 0;
    in.mi.mouseData = data;   // XBUTTON1/XBUTTON2 для XDOWN/XUP, иначе 0
    in.mi.dwFlags = flags;
    in.mi.time = 0;
    in.mi.dwExtraInfo = GetMessageExtraInfo();
    SendInput(1, &in, sizeof(in));
}

static void DoButtonDown(int button) {
    switch (button) {
    case 0: SendMouse(MOUSEEVENTF_LEFTDOWN); break;
    case 1: SendMouse(MOUSEEVENTF_RIGHTDOWN); break;
    case 2: SendMouse(MOUSEEVENTF_MIDDLEDOWN); break;
    case 3: SendMouse(MOUSEEVENTF_XDOWN, XBUTTON1); break;
    case 4: SendMouse(MOUSEEVENTF_XDOWN, XBUTTON2); break;
    default: SendMouse(MOUSEEVENTF_LEFTDOWN); break;
    }
}

static void DoButtonUp(int button) {
    switch (button) {
    case 0: SendMouse(MOUSEEVENTF_LEFTUP); break;
    case 1: SendMouse(MOUSEEVENTF_RIGHTUP); break;
    case 2: SendMouse(MOUSEEVENTF_MIDDLEUP); break;
    case 3: SendMouse(MOUSEEVENTF_XUP, XBUTTON1); break;
    case 4: SendMouse(MOUSEEVENTF_XUP, XBUTTON2); break;
    default: SendMouse(MOUSEEVENTF_LEFTUP); break;
    }
}

// Если где-то было:
static inline void DoClickOnce(int button) { DoButtonDown(button); DoButtonUp(button); }


static void Worker(ClickConfig cfg) {
    timeBeginPeriod(1);
    if (cfg.sequence) {
        auto start = std::chrono::steady_clock::now(); auto deadline = start + std::chrono::seconds(cfg.max_seconds);
        std::mt19937 rng((unsigned)std::chrono::high_resolution_clock::now().time_since_epoch().count()); long long clicked = 0; bool autoStopped = false;
        while (g_running.load(std::memory_order_relaxed)) {
            for (size_t i = 0; i < cfg.steps.size() && g_running.load(); ++i) {
                if (cfg.stop_mode == 2 && std::chrono::steady_clock::now() >= deadline) { autoStopped = true; goto seq_end; }
                int base = cfg.steps[i].delay_ms; int jitter = (cfg.jitter_percent > 0) ? (base * cfg.jitter_percent) / 100 : 0; int delta = 0; if (jitter > 0) { std::uniform_int_distribution<int> d(-jitter, +jitter); delta = d(rng); } int waitms = base + delta; if (waitms < 0) waitms = 0; Sleep((DWORD)waitms);
                SetCursorPos(cfg.steps[i].x, cfg.steps[i].y); DoClickOnce(cfg.button); clicked += 1; if (cfg.dbl) { DWORD gap = (DWORD)max(1u, min((UINT)25, GetDoubleClickTime() / 3)); Sleep(gap); DoClickOnce(cfg.button); clicked += 1; }
                if (cfg.stop_mode == 1 && clicked >= cfg.max_clicks) { autoStopped = true; goto seq_end; }
            }
        }
    seq_end:
        timeEndPeriod(1); if (autoStopped) { g_running.store(false, std::memory_order_relaxed); PostMessageW(g_hMain, WM_APP_AUTOSTOP, 0, 0); } return;
    }

    if (cfg.hold) { if (cfg.fixed) SetCursorPos(cfg.x, cfg.y); DoButtonDown(cfg.button); while (g_running.load(std::memory_order_relaxed)) Sleep(15); DoButtonUp(cfg.button); timeEndPeriod(1); return; }

    std::mt19937 rng((unsigned)std::chrono::high_resolution_clock::now().time_since_epoch().count()); auto next = std::chrono::steady_clock::now(); auto startt = next; auto deadline = startt + std::chrono::seconds(cfg.max_seconds);
    if (cfg.fixed) SetCursorPos(cfg.x, cfg.y); long long clicked = 0; bool autoStopped = false;
    while (g_running.load(std::memory_order_relaxed)) {
        if (cfg.stop_mode == 2 && std::chrono::steady_clock::now() >= deadline) { autoStopped = true; break; }
        if (cfg.fixed) SetCursorPos(cfg.x, cfg.y); DoClickOnce(cfg.button); clicked += 1; if (cfg.dbl) { DWORD gap = (DWORD)max(1u, min((UINT)25, GetDoubleClickTime() / 3)); Sleep(gap); DoClickOnce(cfg.button); clicked += 1; }
        if (cfg.stop_mode == 1 && clicked >= cfg.max_clicks) { autoStopped = true; break; }
        int base = cfg.interval_ms; int jitter = (cfg.jitter_percent > 0) ? (base * cfg.jitter_percent) / 100 : 0; int delta = 0; if (jitter > 0) { std::uniform_int_distribution<int> d(-jitter, +jitter); delta = d(rng); } int interval = base + delta; if (interval < 1) interval = 1; next += std::chrono::milliseconds(interval); auto now = std::chrono::steady_clock::now(); if (next > now) { auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(next - now).count(); Sleep((DWORD)ms); }
        else { next = now + std::chrono::milliseconds(interval); Sleep((DWORD)interval); }
    }
    timeEndPeriod(1); if (autoStopped) { g_running.store(false, std::memory_order_relaxed); PostMessageW(g_hMain, WM_APP_AUTOSTOP, 0, 0); }
}

static void StartClicking() {
    if (g_running.load()) return; ClickConfig cfg{}; BOOL seq = (Button_GetCheck(GetDlgItem(g_hMain, IDC_CHECK_SEQUENCE)) == BST_CHECKED);
    if (seq) {
        if (g_steps.empty()) { SetStatus(LS(S_SEQ_ENABLE_FIRST)); return; }
        cfg.sequence = true; cfg.steps = g_steps; cfg.button = (int)SendMessageW(GetDlgItem(g_hMain, IDC_COMBO_BUTTON), CB_GETCURSEL, 0, 0); if (cfg.button < 0) cfg.button = 0; cfg.dbl = (Button_GetCheck(GetDlgItem(g_hMain, IDC_CHECK_DOUBLE)) == BST_CHECKED);
        cfg.stop_mode = Button_GetCheck(GetDlgItem(g_hMain, IDC_RADIO_CLICKS)) == BST_CHECKED ? 1 : Button_GetCheck(GetDlgItem(g_hMain, IDC_RADIO_SECONDS)) == BST_CHECKED ? 2 : 0; if (cfg.stop_mode == 1) cfg.max_clicks = max(1, ReadInt(GetDlgItem(g_hMain, IDC_EDIT_CLICKS), 1)); if (cfg.stop_mode == 2) cfg.max_seconds = max(1, ReadInt(GetDlgItem(g_hMain, IDC_EDIT_SECONDS), 10)); cfg.jitter_percent = ReadInt(GetDlgItem(g_hMain, IDC_EDIT_JITTER), 0); if (cfg.jitter_percent < 0) cfg.jitter_percent = 0; if (cfg.jitter_percent > 80) cfg.jitter_percent = 80; cfg.hold = false;
    }
    else {
        int raw = max(1, ReadInt(GetDlgItem(g_hMain, IDC_EDIT_INTERVAL), 100)); bool isCps = (Button_GetCheck(GetDlgItem(g_hMain, IDC_CHECK_CPS)) == BST_CHECKED); bool isHold = (Button_GetCheck(GetDlgItem(g_hMain, IDC_CHECK_HOLD)) == BST_CHECKED);
        if (isCps) { int cps = raw; if (cps < 1) cps = 1; if (cps > 1000) cps = 1000; cfg.interval_ms = max(1, (int)(1000 / cps)); }
        else cfg.interval_ms = raw;
        cfg.button = (int)SendMessageW(GetDlgItem(g_hMain, IDC_COMBO_BUTTON), CB_GETCURSEL, 0, 0); if (cfg.button < 0) cfg.button = 0; cfg.dbl = (Button_GetCheck(GetDlgItem(g_hMain, IDC_CHECK_DOUBLE)) == BST_CHECKED) && !isHold; cfg.fixed = (Button_GetCheck(GetDlgItem(g_hMain, IDC_CHECK_FIXED)) == BST_CHECKED);
        cfg.x = ReadInt(GetDlgItem(g_hMain, IDC_EDIT_X), 0); cfg.y = ReadInt(GetDlgItem(g_hMain, IDC_EDIT_Y), 0); cfg.hold = isHold;
        if (cfg.interval_ms < 1) cfg.interval_ms = 1; if (cfg.interval_ms > 60000) cfg.interval_ms = 60000;
        if (!isHold) {
            if (Button_GetCheck(GetDlgItem(g_hMain, IDC_RADIO_CLICKS)) == BST_CHECKED) { cfg.stop_mode = 1; cfg.max_clicks = max(1, ReadInt(GetDlgItem(g_hMain, IDC_EDIT_CLICKS), 1)); }
            else if (Button_GetCheck(GetDlgItem(g_hMain, IDC_RADIO_SECONDS)) == BST_CHECKED) { cfg.stop_mode = 2; cfg.max_seconds = max(1, ReadInt(GetDlgItem(g_hMain, IDC_EDIT_SECONDS), 10)); }
            else cfg.stop_mode = 0;
        }
        cfg.jitter_percent = isHold ? 0 : ReadInt(GetDlgItem(g_hMain, IDC_EDIT_JITTER), 0); if (cfg.jitter_percent < 0) cfg.jitter_percent = 0; if (cfg.jitter_percent > 80) cfg.jitter_percent = 80;
    }
    g_running.store(true); SetStartBtnLabel(g_hMain); SetStatus(seq ? LS(S_SEQ_RUNNING) : (cfg.hold ? LS(S_HOLDING) : LS(S_RUNNING))); g_worker = std::thread(Worker, cfg);
}
static void StopClicking() { g_running.store(false); if (g_worker.joinable()) g_worker.join(); SetStartBtnLabel(g_hMain); SetStatus(LS(S_STOPPED)); }
static void ToggleClicking() { if (g_running.load()) StopClicking(); else StartClicking(); }

// ---------------------- Hotkey ------------------------------
static bool ApplyHotkey(HWND hWnd) {
    UnregisterHotKey(hWnd, g_hotkeyId);
    WORD hk = (WORD)SendMessageW(GetDlgItem(hWnd, IDC_HOTKEY), HKM_GETHOTKEY, 0, 0); BYTE vk = LOBYTE(hk); BYTE m = HIBYTE(hk);
    UINT mods = 0; if (m & HOTKEYF_CONTROL) mods |= MOD_CONTROL; if (m & HOTKEYF_SHIFT) mods |= MOD_SHIFT; if (m & HOTKEYF_ALT) mods |= MOD_ALT; if (!vk) { vk = VK_F6; mods = 0; }
    if (!RegisterHotKey(hWnd, g_hotkeyId, mods | MOD_NOREPEAT, vk)) { SetStatus(LS(S_HK_FAIL)); return false; }
    g_hotkeyVK = vk; g_hotkeyMods = mods; SetStartBtnLabel(hWnd); SetStatus(LS(S_HK_OK)); return true;
}

// ---------------------- UI creation -------------------------
static void CreateLabeledEdit(HWND parent, int x, int y, int wLabel, int idStatic, const wchar_t* label, int wEdit, int idEdit, const wchar_t* initText = L"") {
    const int LABEL_H = SX(18); const int EDIT_H = SX(22); const int GAP = SX(8); const int BASEOFF = SX(3); int sx = SX(x); int sy = SX(y);
    if (wLabel > 0) CreateWindowW(L"STATIC", label, WS_CHILD | WS_VISIBLE, sx, sy + BASEOFF, SX(wLabel), LABEL_H, parent, (HMENU)(INT_PTR)idStatic, nullptr, nullptr);
    HWND hEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", initText, WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, sx + SX(wLabel) + (wLabel ? GAP : 0), sy, SX(wEdit), EDIT_H, parent, (HMENU)(INT_PTR)idEdit, nullptr, nullptr);
    SendMessageW(hEdit, WM_SETFONT, (WPARAM)GetStockObject(DEFAULT_GUI_FONT), TRUE);
}

static void BuildUI(HWND hWnd) {
    HFONT hFont = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    // Language toggle
    HWND hLang = CreateWindowW(L"BUTTON", LS(S_LANG_BTN), WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, SX(420), SX(12), SX(160), SX(26), hWnd, (HMENU)(INT_PTR)IDC_BTN_LANG, nullptr, nullptr); SendMessageW(hLang, WM_SETFONT, (WPARAM)hFont, TRUE);

    // Interval + CPS
    CreateLabeledEdit(hWnd, 16, 20, 160, IDC_LBL_INTERVAL, LS(S_INTERVAL), 92, IDC_EDIT_INTERVAL, L"100");
    HWND hUnit = CreateWindowW(L"STATIC", LS(S_UNIT_MS), WS_CHILD | WS_VISIBLE, SX(16 + 160 + 6 + 92 + 6), SX(24), SX(40), SX(18), hWnd, (HMENU)(INT_PTR)IDC_STATIC_UNIT, nullptr, nullptr); SendMessageW(hUnit, WM_SETFONT, (WPARAM)hFont, TRUE);
    HWND hCps = CreateWindowW(L"BUTTON", LS(S_CPS_MODE), WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX, SX(16 + 160 + 6 + 92 + 6 + 50), SX(18), SX(85), SX(24), hWnd, (HMENU)(INT_PTR)IDC_CHECK_CPS, nullptr, nullptr); SendMessageW(hCps, WM_SETFONT, (WPARAM)hFont, TRUE);

    // Button combobox
    CreateWindowW(L"STATIC", LS(S_BUTTON), WS_CHILD | WS_VISIBLE, SX(16), SX(56), SX(160), SX(18), hWnd, (HMENU)(INT_PTR)IDC_LBL_BUTTON, nullptr, nullptr);
    HWND hCombo = CreateWindowW(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_CHILD | WS_VISIBLE, SX(16 + 160 + 6), SX(52), SX(160), SX(200), hWnd, (HMENU)(INT_PTR)IDC_COMBO_BUTTON, nullptr, nullptr);
    SendMessageW(hCombo, WM_SETFONT, (WPARAM)hFont, TRUE);
    UpdateButtonCombo(hWnd);

    // Double / Hold
    HWND hDbl = CreateWindowW(L"BUTTON", LS(S_DBL), WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX, SX(16), SX(84), SX(180), SX(22), hWnd, (HMENU)(INT_PTR)IDC_CHECK_DOUBLE, nullptr, nullptr); SendMessageW(hDbl, WM_SETFONT, (WPARAM)hFont, TRUE);
    HWND hHold = CreateWindowW(L"BUTTON", LS(S_HOLD), WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX, SX(200), SX(84), SX(180), SX(22), hWnd, (HMENU)(INT_PTR)IDC_CHECK_HOLD, nullptr, nullptr); SendMessageW(hHold, WM_SETFONT, (WPARAM)hFont, TRUE);

    // Fixed position + pick
    HWND hFixed = CreateWindowW(L"BUTTON", LS(S_FIXED), WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX, SX(16), SX(114), SX(200), SX(22), hWnd, (HMENU)(INT_PTR)IDC_CHECK_FIXED, nullptr, nullptr); SendMessageW(hFixed, WM_SETFONT, (WPARAM)hFont, TRUE);
    CreateLabeledEdit(hWnd, 16, 146, 40, 0, L"X:", 50, IDC_EDIT_X, L"0");
    CreateLabeledEdit(hWnd, 180, 146, 40, 0, L"Y:", 50, IDC_EDIT_Y, L"0");
    HWND hPick = CreateWindowW(L"BUTTON", LS(S_PICK), WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, SX(60), SX(144), SX(190), SX(26), hWnd, (HMENU)(INT_PTR)IDC_BTN_PICK, nullptr, nullptr); SendMessageW(hPick, WM_SETFONT, (WPARAM)hFont, TRUE);
    RECT ry; GetWindowRect(GetDlgItem(hWnd, IDC_EDIT_Y), &ry); POINT py = { ry.right, ry.top }; ScreenToClient(hWnd, &py); const int PAD = SX(8); SetWindowPos(hPick, nullptr, py.x + PAD, py.y - SX(1), 0, 0, SWP_NOSIZE | SWP_NOZORDER);

    // Stop mode
    CreateWindowW(L"STATIC", LS(S_STOPMODE), WS_CHILD | WS_VISIBLE, SX(16), SX(180), SX(160), SX(18), hWnd, (HMENU)(INT_PTR)IDC_LBL_STOPMODE, nullptr, nullptr);
    HWND rInf = CreateWindowW(L"BUTTON", LS(S_INF), WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON, SX(16), SX(202), SX(200), SX(20), hWnd, (HMENU)(INT_PTR)IDC_RADIO_INF, nullptr, nullptr);
    HWND rClicks = CreateWindowW(L"BUTTON", L"", WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON, SX(16), SX(226), SX(15), SX(20), hWnd, (HMENU)(INT_PTR)IDC_RADIO_CLICKS, nullptr, nullptr);
    HWND rSecs = CreateWindowW(L"BUTTON", L"", WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON, SX(16), SX(250), SX(15), SX(20), hWnd, (HMENU)(INT_PTR)IDC_RADIO_SECONDS, nullptr, nullptr);
    SendMessageW(rInf, WM_SETFONT, (WPARAM)hFont, TRUE); SendMessageW(rClicks, WM_SETFONT, (WPARAM)hFont, TRUE); SendMessageW(rSecs, WM_SETFONT, (WPARAM)hFont, TRUE); SendMessageW(rInf, BM_SETCHECK, BST_CHECKED, 0);
    CreateLabeledEdit(hWnd, 50, 224, 70, IDC_LBL_CLICKS, LS(S_NCLICKS), 70, IDC_EDIT_CLICKS, L"100");
    CreateLabeledEdit(hWnd, 50, 248, 70, IDC_LBL_SECONDS, LS(S_NSECS), 70, IDC_EDIT_SECONDS, L"10");

    // Jitter
    CreateLabeledEdit(hWnd, 16, 280, 100, IDC_LBL_JITTER, LS(S_JITTER), 92, IDC_EDIT_JITTER, L"0");

    // Hotkey
    CreateWindowW(L"STATIC", LS(S_HOTKEY), WS_CHILD | WS_VISIBLE, SX(16), SX(312), SX(160), SX(20), hWnd, (HMENU)(INT_PTR)IDC_LBL_HOTKEY, nullptr, nullptr);
    HWND hHot = CreateWindowExW(0, HOTKEY_CLASS, L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP, SX(16 + 160 + 6), SX(310), SX(150), SX(22), hWnd, (HMENU)(INT_PTR)IDC_HOTKEY, GetModuleHandleW(nullptr), nullptr); SendMessageW(hHot, WM_SETFONT, (WPARAM)hFont, TRUE);
    HWND hApplyHK = CreateWindowW(L"BUTTON", LS(S_APPLY), WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, SX(16 + 160 + 6 + 160 + 8), SX(308), SX(100), SX(26), hWnd, (HMENU)(INT_PTR)IDC_BTN_APPLYHK, nullptr, nullptr); SendMessageW(hApplyHK, WM_SETFONT, (WPARAM)hFont, TRUE);

    // Autostart
    HWND hAuto = CreateWindowW(L"BUTTON", LS(S_AUTOSTART), WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX, SX(16), SX(338), SX(220), SX(22), hWnd, (HMENU)(INT_PTR)IDC_CHECK_AUTOSTART, nullptr, nullptr); SendMessageW(hAuto, WM_SETFONT, (WPARAM)hFont, TRUE);

    // Sequence section
    HWND hSeqCheck = CreateWindowW(L"BUTTON", LS(S_SEQ_CHECK), WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX, SX(16), SX(370), SX(360), SX(22), hWnd, (HMENU)(INT_PTR)IDC_CHECK_SEQUENCE, nullptr, nullptr); SendMessageW(hSeqCheck, WM_SETFONT, (WPARAM)hFont, TRUE);
    CreateWindowW(L"STATIC", LS(S_SEQ_DELAY), WS_CHILD | WS_VISIBLE, SX(16), SX(398), SX(220), SX(20), hWnd, (HMENU)(INT_PTR)IDC_LBL_SEQ_DELAY, nullptr, nullptr);
    CreateLabeledEdit(hWnd, 210, 396, 0, 0, L"", 80, IDC_EDIT_STEP_DELAY, L"100");
    HWND hAdd = CreateWindowW(L"BUTTON", LS(S_ADD_POINT), WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, SX(300), SX(394), SX(260), SX(26), hWnd, (HMENU)(INT_PTR)IDC_BTN_ADD_STEP, nullptr, nullptr); SendMessageW(hAdd, WM_SETFONT, (WPARAM)hFont, TRUE);

    HWND lv = CreateWindowW(WC_LISTVIEWW, L"", WS_CHILD | WS_VISIBLE | WS_BORDER | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS, SX(16), SX(426), SX(544), SX(140), hWnd, (HMENU)(INT_PTR)IDC_LIST_SEQ, nullptr, nullptr); SendMessageW(lv, WM_SETFONT, (WPARAM)hFont, TRUE); ListView_SetExtendedListViewStyle(lv, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);
    LVCOLUMNW col{}; col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM; col.pszText = (LPWSTR)LS(S_COL_NUM); col.cx = SX(40); col.iSubItem = 0; ListView_InsertColumn(lv, 0, &col);
    col.pszText = (LPWSTR)LS(S_COL_X); col.cx = SX(120); col.iSubItem = 1; ListView_InsertColumn(lv, 1, &col);
    col.pszText = (LPWSTR)LS(S_COL_Y); col.cx = SX(120); col.iSubItem = 2; ListView_InsertColumn(lv, 2, &col);
    col.pszText = (LPWSTR)LS(S_COL_INTERVAL); col.cx = SX(220); col.iSubItem = 3; ListView_InsertColumn(lv, 3, &col);

    HWND hRem = CreateWindowW(L"BUTTON", LS(S_DELETE), WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, SX(16), SX(570), SX(100), SX(26), hWnd, (HMENU)(INT_PTR)IDC_BTN_REMOVE_STEP, nullptr, nullptr); SendMessageW(hRem, WM_SETFONT, (WPARAM)hFont, TRUE);
    HWND hUp = CreateWindowW(L"BUTTON", LS(S_UP), WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, SX(122), SX(570), SX(100), SX(26), hWnd, (HMENU)(INT_PTR)IDC_BTN_UP, nullptr, nullptr); SendMessageW(hUp, WM_SETFONT, (WPARAM)hFont, TRUE);
    HWND hDn = CreateWindowW(L"BUTTON", LS(S_DOWN), WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, SX(228), SX(570), SX(100), SX(26), hWnd, (HMENU)(INT_PTR)IDC_BTN_DOWN, nullptr, nullptr); SendMessageW(hDn, WM_SETFONT, (WPARAM)hFont, TRUE);

    // Start/Status
    HWND hToggle = CreateWindowW(L"BUTTON", L"", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON, SX(16), SX(610), SX(150), SX(34), hWnd, (HMENU)(INT_PTR)IDC_BTN_TOGGLE, nullptr, nullptr); SendMessageW(hToggle, WM_SETFONT, (WPARAM)hFont, TRUE);
    HWND hStatus = CreateWindowExW(WS_EX_CLIENTEDGE, L"STATIC", LS(S_READY), WS_CHILD | WS_VISIBLE | SS_LEFTNOWORDWRAP, SX(180), SX(610), SX(340), SX(34), hWnd, (HMENU)(INT_PTR)IDC_STATUS, nullptr, nullptr); SendMessageW(hStatus, WM_SETFONT, (WPARAM)hFont, TRUE);
    SetStartBtnLabel(hWnd);
}

static void ToggleLanguage(HWND hWnd) {
    g_lang = (g_lang == LANG_RU) ? LANG_EN : LANG_RU; SaveSettings(hWnd); UpdateTexts(hWnd); UpdateUIState(hWnd); SetStartBtnLabel(hWnd);
}

static LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE: {
        g_hMain = hWnd; g_dpi = GetDpiForWindow(hWnd); INITCOMMONCONTROLSEX icc{ sizeof(icc), ICC_WIN95_CLASSES | ICC_STANDARD_CLASSES | ICC_HOTKEY_CLASS }; InitCommonControlsEx(&icc);
        BuildUI(hWnd); LoadSettings(hWnd); RefreshSequenceList(hWnd); UpdateTexts(hWnd);
        // Hotkey
        UnregisterHotKey(hWnd, g_hotkeyId); if (!RegisterHotKey(hWnd, g_hotkeyId, g_hotkeyMods | MOD_NOREPEAT, g_hotkeyVK)) RegisterHotKey(hWnd, g_hotkeyId, MOD_NOREPEAT, VK_F6); SetStartBtnLabel(hWnd); return 0;
    }
    case WM_COMMAND: {
        int id = LOWORD(wParam); switch (id) {
        case IDC_BTN_LANG: { ToggleLanguage(hWnd); return 0; }
        case IDC_BTN_PICK: { if (!g_pickMode.load()) { g_pickMode.store(true); if (!g_mouseHook) g_mouseHook = SetWindowsHookExW(WH_MOUSE_LL, LowLevelMouseProc, GetModuleHandleW(nullptr), 0); SetStatus(LS(S_PICK_PROMPT)); } return 0; }
        case IDC_BTN_ADD_STEP: { if (Button_GetCheck(GetDlgItem(hWnd, IDC_CHECK_SEQUENCE)) != BST_CHECKED) { SetStatus(LS(S_SEQ_ENABLE_FIRST)); return 0; } if (!g_pickSeq.load()) { g_pickSeq.store(true); if (!g_mouseHook) g_mouseHook = SetWindowsHookExW(WH_MOUSE_LL, LowLevelMouseProc, GetModuleHandleW(nullptr), 0); SetStatus(LS(S_ADD_PROMPT)); } return 0; }
        case IDC_BTN_REMOVE_STEP: { RemoveSelectedStep(hWnd); SaveSettings(hWnd); return 0; }
        case IDC_BTN_UP: { MoveSelectedStep(hWnd, -1); SaveSettings(hWnd); return 0; }
        case IDC_BTN_DOWN: { MoveSelectedStep(hWnd, +1); SaveSettings(hWnd); return 0; }
        case IDC_BTN_TOGGLE: { ToggleClicking(); return 0; }
        case IDC_BTN_APPLYHK: { ApplyHotkey(hWnd); SaveSettings(hWnd); return 0; }
        case IDM_TRAY_SHOWHIDE: { if (IsWindowVisible(hWnd)) HideToTray(hWnd); else RestoreFromTray(hWnd); return 0; }
        case IDM_TRAY_STARTSTOP: { ToggleClicking(); return 0; }
        case IDM_TRAY_EXIT: { TrayRemove(); DestroyWindow(hWnd); return 0; }
        case IDC_CHECK_CPS:
        case IDC_CHECK_HOLD:
        case IDC_CHECK_SEQUENCE:
        case IDC_RADIO_INF:
        case IDC_RADIO_CLICKS:
        case IDC_RADIO_SECONDS: { UpdateUIState(hWnd); return 0; }
        } break;
    }
    case WM_APP_PICKED: { int x = (int)(INT_PTR)wParam; int y = (int)(INT_PTR)lParam; if (g_pickSeq.load()) { int delay = ReadInt(GetDlgItem(hWnd, IDC_EDIT_STEP_DELAY), 100); if (delay < 0) delay = 0; if (delay > 60000) delay = 60000; g_steps.push_back(Step{ x, y, delay }); g_pickSeq.store(false); RefreshSequenceList(hWnd); SaveSettings(hWnd); SetStatus(LS(S_POINT_ADDED)); } else { SetInt(GetDlgItem(hWnd, IDC_EDIT_X), x); SetInt(GetDlgItem(hWnd, IDC_EDIT_Y), y); Button_SetCheck(GetDlgItem(hWnd, IDC_CHECK_FIXED), BST_CHECKED); SetStatus(LS(S_POINT_APPLIED)); } return 0; }
    case WM_APP_AUTOSTOP: { if (g_worker.joinable()) g_worker.join(); SetStartBtnLabel(hWnd); SetStatus(LS(S_STOPPED_COND)); return 0; }
    case WM_APP_TRAY: { switch (LOWORD(lParam)) { case WM_LBUTTONDBLCLK: case WM_LBUTTONUP: RestoreFromTray(hWnd); return 0; case WM_RBUTTONUP: TrayMenu(hWnd); return 0; } break; }
    case WM_SIZE: { if (wParam == SIZE_MINIMIZED) { HideToTray(hWnd); return 0; } break; }
    case WM_HOTKEY: { if ((UINT)wParam == g_hotkeyId) { ToggleClicking(); return 0; } break; }
    case WM_CLOSE: { HideToTray(hWnd); return 0; }
    case WM_DESTROY: { SaveSettings(hWnd); UnregisterHotKey(hWnd, g_hotkeyId); TrayRemove(); PostQuitMessage(0); return 0; }
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

// ---------------------- Entry point -------------------------
int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR, int nShow) {
    // Better DPI awareness if available
    HMODULE hUser32 = GetModuleHandleW(L"user32.dll"); typedef BOOL(WINAPI* SetDpiCtxFn)(HANDLE); auto pSetCtx = (SetDpiCtxFn)GetProcAddress(hUser32, "SetProcessDpiAwarenessContext"); if (pSetCtx) pSetCtx((HANDLE)-4 /*PER_MONITOR_AWARE_V2*/); else SetProcessDPIAware();
    INITCOMMONCONTROLSEX icc{ sizeof(icc), ICC_WIN95_CLASSES | ICC_STANDARD_CLASSES | ICC_HOTKEY_CLASS }; InitCommonControlsEx(&icc);
    const wchar_t* kClass = L"AutoClickerWndClass"; WNDCLASSW wc{}; wc.lpfnWndProc = WndProc; wc.hInstance = hInst; wc.lpszClassName = kClass; wc.hCursor = LoadCursor(nullptr, IDC_ARROW); wc.hIcon = LoadIconW(hInst, MAKEINTRESOURCEW(IDI_APPICON)); wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1); if (!RegisterClassW(&wc)) return 0;
    bool startTray = false; { LPCWSTR cmd = GetCommandLineW(); if (wcsstr(cmd, L"/tray") || wcsstr(cmd, L"-tray")) startTray = true; }
    HWND hWnd = CreateWindowExW(WS_EX_APPWINDOW, kClass, L"LightClick", WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, CW_USEDEFAULT, CW_USEDEFAULT, SX(600), SX(780), nullptr, nullptr, hInst, nullptr); if (!hWnd) return 0;
    HICON hBig = (HICON)LoadImageW(hInst, MAKEINTRESOURCEW(IDI_APPICON), IMAGE_ICON, GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), 0); HICON hSmall = (HICON)LoadImageW(hInst, MAKEINTRESOURCEW(IDI_APPICON), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0); SendMessageW(hWnd, WM_SETICON, ICON_BIG, (LPARAM)hBig); SendMessageW(hWnd, WM_SETICON, ICON_SMALL, (LPARAM)hSmall);
    if (!startTray) { ShowWindow(hWnd, nShow); UpdateWindow(hWnd); }
    else { TrayAdd(hWnd); ShowWindow(hWnd, SW_HIDE); }
    MSG msg; while (GetMessageW(&msg, nullptr, 0, 0) > 0) { TranslateMessage(&msg); DispatchMessageW(&msg); } return 0;
}
