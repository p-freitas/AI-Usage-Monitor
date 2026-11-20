#include <windows.h>
#include <shellapi.h>
#include <algorithm>
#include <array>
#include <string>
#include <vector>

#include "ai_usage.h"
#include "widget.h"
#include "../res/resource.h"

namespace {


constexpr UINT WM_TRAY = WM_APP + 1;
constexpr UINT WM_WIDGET_CLICK = WM_APP + 3;
constexpr UINT WM_USAGE_RESULTS = WM_APP + 4;
constexpr UINT_PTR TIMER_WIDGET = 1;
constexpr DWORD USAGE_INTERVAL_MS = 2 * 60 * 1000;
constexpr UINT PLACEHOLDER_ID = 1;

enum MenuId : UINT {
    ID_REFRESH = 100, ID_AUTOSTART, ID_WIDGET_BACKGROUND, ID_EXIT, ID_MODE_WIDGET, ID_MODE_TRAY,
    ID_CLAUDE_SETUP, ID_CLAUDE_LIVE_SETTINGS, ID_USAGE_FIRST
};

constexpr wchar_t kAppName[] = L"AI Usage Monitor";
constexpr wchar_t kWindowClass[] = L"AIUsageMonitorWnd";
constexpr wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t kRunValue[] = L"AIUsageMonitor";
constexpr wchar_t kSettingsKey[] = L"Software\\AIUsageMonitor";

HINSTANCE g_inst;
HWND g_hwnd;
HANDLE g_stopEvt, g_codexWatcher;
UINT g_taskbarCreatedMsg;
bool g_lightTaskbar, g_useWidget, g_widgetBackground, g_placeholder;
bool g_wantWidget = true;

struct UsageEntry {
    ai::Provider provider;
    const wchar_t* key;
    UINT uid;
    int resource;
    ai::UsageState quota;
    bool added = false;
    HANDLE refresh = nullptr;
    HANDLE worker = nullptr;
};
std::array<UsageEntry, 2> g_usage = {{
    {ai::Provider::Claude, L"HideClaude", 2, IDI_CLAUDE},
    {ai::Provider::Codex, L"HideCodex", 3, IDI_CODEX}
}};

// ---- helpers ----------------------------------------------------------------------------------

bool ReadLightTaskbar() {
    DWORD v = 0, sz = sizeof(v);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                     L"SystemUsesLightTheme", RRF_RT_REG_DWORD, nullptr, &v, &sz) == ERROR_SUCCESS)
        return v != 0;
    return false;
}

void ApplyMenuTheme(bool light) {
    using SetPreferredAppModeFn = int(WINAPI*)(int);
    using FlushMenuThemesFn = void(WINAPI*)();
    static HMODULE ux = LoadLibraryExW(L"uxtheme.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    static auto setMode = ux ? (SetPreferredAppModeFn)GetProcAddress(ux, MAKEINTRESOURCEA(135)) : nullptr;
    static auto flush = ux ? (FlushMenuThemesFn)GetProcAddress(ux, MAKEINTRESOURCEA(136)) : nullptr;
    if (setMode) setMode(light ? 3 : 2);
    if (flush) flush();
}

std::wstring ExePath() {
    wchar_t p[MAX_PATH];
    GetModuleFileNameW(nullptr, p, MAX_PATH);
    return p;
}

bool LoadBoolSetting(const wchar_t* name, bool defaultVal) {
    DWORD v = defaultVal ? 1 : 0, sz = sizeof(v);
    RegGetValueW(HKEY_CURRENT_USER, kSettingsKey, name, RRF_RT_REG_DWORD, nullptr, &v, &sz);
    return v != 0;
}

void SaveBoolSetting(const wchar_t* name, bool on) {
    DWORD v = on ? 1 : 0;
    RegSetKeyValueW(HKEY_CURRENT_USER, kSettingsKey, name, REG_DWORD, &v, sizeof(v));
}

bool LoadWidgetMode() { return LoadBoolSetting(L"TaskbarWidget", true); }
void SaveWidgetMode(bool on) { SaveBoolSetting(L"TaskbarWidget", on); }
bool LoadWidgetBackground() { return LoadBoolSetting(L"WidgetBackground", false); }
void SaveWidgetBackground(bool on) { SaveBoolSetting(L"WidgetBackground", on); }

bool AutostartEnabled() {
    wchar_t buf[MAX_PATH * 2];
    DWORD sz = sizeof(buf);
    if (RegGetValueW(HKEY_CURRENT_USER, kRunKey, kRunValue, RRF_RT_REG_SZ, nullptr, buf, &sz) != ERROR_SUCCESS) return false;
    return _wcsicmp(buf, (L"\"" + ExePath() + L"\"").c_str()) == 0;
}

void SetAutostart(bool on) {
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_SET_VALUE, &k) != ERROR_SUCCESS) return;
    if (on) {
        std::wstring v = L"\"" + ExePath() + L"\"";
        RegSetValueExW(k, kRunValue, 0, REG_SZ, (const BYTE*)v.c_str(), (DWORD)((v.size() + 1) * sizeof(wchar_t)));
    } else {
        RegDeleteValueW(k, kRunValue);
    }
    RegCloseKey(k);
}

bool IsHidden(const wchar_t* key) { return LoadBoolSetting(key, false); }

void CopyTip(wchar_t* dst, size_t cap, const std::wstring& s) {
    wcsncpy(dst, s.c_str(), cap - 1);
    dst[cap - 1] = 0;
}

void TrayCall(DWORD msg, UINT uid, HICON icon, const std::wstring& tip) {
    NOTIFYICONDATAW nid{ sizeof(nid) };
    nid.hWnd = g_hwnd;
    nid.uID = uid;
    nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
    nid.uCallbackMessage = WM_TRAY;
    nid.hIcon = icon;
    CopyTip(nid.szTip, ARRAYSIZE(nid.szTip), tip);
    Shell_NotifyIconW(msg, &nid);
    if (msg == NIM_ADD) {
        nid.uVersion = NOTIFYICON_VERSION_4;
        Shell_NotifyIconW(NIM_SETVERSION, &nid);
    }
}

void TrayDelete(UINT uid) {
    NOTIFYICONDATAW nid{ sizeof(nid) };
    nid.hWnd = g_hwnd;
    nid.uID = uid;
    Shell_NotifyIconW(NIM_DELETE, &nid);
}

std::wstring UsageTip(const UsageEntry& entry) {
    std::wstring line = ai::Name(entry.provider);
    line += L": " + ai::Format(entry.quota.value);
    if (entry.quota.value.Available())
        line += L" used";
    else
        line += L" (waiting for CLI reading)";
    if (entry.quota.LastKnown()) line += L" (last known)";
    return line;
}

void ApplyUsage(UsageEntry& entry) {
    bool visible = entry.quota.value.installed && !g_useWidget && !IsHidden(entry.key);
    if (!visible) {
        if (entry.added) TrayDelete(entry.uid);
        entry.added = false;
        return;
    }
    HICON icon = LoadIconW(g_inst, MAKEINTRESOURCEW(entry.resource));
    if (icon) {
        TrayCall(entry.added ? NIM_MODIFY : NIM_ADD, entry.uid, icon, UsageTip(entry));
        entry.added = true;
    }
}

void UpdatePlaceholder() {
    if (g_useWidget) {
        if (g_placeholder) { TrayDelete(PLACEHOLDER_ID); g_placeholder = false; }
        return;
    }
    bool any = false;
    for (const auto& entry : g_usage) any |= entry.added;
    if (!any && !g_placeholder) {
        TrayCall(NIM_ADD, PLACEHOLDER_ID, LoadIconW(g_inst, MAKEINTRESOURCEW(IDI_APP)), L"AI Usage Monitor");
        g_placeholder = true;
    } else if (any && g_placeholder) {
        TrayDelete(PLACEHOLDER_ID);
        g_placeholder = false;
    }
}

void UpdateWidget() {
    if (!g_useWidget) return;
    std::vector<WidgetItem> items;
    std::wstring tip;
    for (const auto& entry : g_usage) {
        if (!entry.quota.value.installed || IsHidden(entry.key)) continue;
        WidgetItem item;
        item.text = ai::Format(entry.quota.value);
        item.iconResource = entry.resource;
        item.dim = !entry.quota.value.Available() || entry.quota.LastKnown();
        items.push_back(std::move(item));
        if (!tip.empty()) tip += L"\n";
        tip += UsageTip(entry);
    }
    if (tip.empty()) tip = L"AI Usage Monitor";
    widget::SetItems(items, tip);
}

void Reconcile() {
    bool alive = g_wantWidget && widget::Alive();
    if (g_wantWidget && !alive) alive = widget::Create(g_inst, g_hwnd, WM_WIDGET_CLICK, g_lightTaskbar);
    if (!g_wantWidget) widget::Destroy();
    bool changed = alive != g_useWidget;
    g_useWidget = alive;
    if (changed) {
        for (auto& entry : g_usage) ApplyUsage(entry);
        UpdatePlaceholder();
        UpdateWidget();
    } else if (g_useWidget) {
        widget::Reposition();
    }
}

void SetMode(bool useWidget) {
    g_wantWidget = useWidget;
    SaveWidgetMode(useWidget);
    Reconcile();
}

void SetWidgetBackground(bool on) {
    g_widgetBackground = on;
    SaveWidgetBackground(on);
    widget::SetBackground(on);
}

void ReAddAll() {
    g_placeholder = false;
    for (auto& entry : g_usage) { entry.added = false; ApplyUsage(entry); }
    UpdatePlaceholder();
}

void CopyClaudeLiveSettings() {
    auto settings = ai::ClaudeStatusSettings(ExePath());
    bool copied = false;
    if (!settings.empty() && OpenClipboard(g_hwnd)) {
        HGLOBAL block = GlobalAlloc(GMEM_MOVEABLE, (settings.size() + 1) * sizeof(wchar_t));
        if (block) {
            auto* text = static_cast<wchar_t*>(GlobalLock(block));
            if (text) {
                std::copy_n(settings.c_str(), settings.size() + 1, text);
                GlobalUnlock(block);
                copied = EmptyClipboard() && SetClipboardData(CF_UNICODETEXT, block);
            }
            if (!copied) GlobalFree(block);
        }
        CloseClipboard();
    }
    MessageBoxW(g_hwnd, copied
        ? L"Merge the copied statusLine entry into your Claude Code settings.json. "
          L"If you already use a status line, keep its existing command and chain the monitor as described in the README."
        : L"Could not copy the Claude Code settings.", kAppName, MB_OK | MB_ICONINFORMATION);
}

void RefreshUsage() {
    for (const auto& entry : g_usage)
        if (entry.refresh) SetEvent(entry.refresh);
}


void ShowMenu() {
    ApplyMenuTheme(g_lightTaskbar);
    HMENU menu = CreatePopupMenu();
    if (!menu) return;
    for (size_t i = 0; i < g_usage.size(); ++i) {
        const auto& entry = g_usage[i];
        if (entry.quota.value.installed) {
            auto line = UsageTip(entry);
            AppendMenuW(menu, MF_STRING | (IsHidden(entry.key) ? 0 : MF_CHECKED),
                        ID_USAGE_FIRST + i, line.c_str());
        }
    }
    if (g_usage[0].quota.value.installed && !g_usage[0].quota.value.Available())
        AppendMenuW(menu, MF_STRING, ID_CLAUDE_SETUP, L"Open Claude Code setup");
    AppendMenuW(menu, MF_STRING, ID_CLAUDE_LIVE_SETTINGS, L"Copy Claude Code live update settings");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, ID_MODE_WIDGET, L"Show on the taskbar");
    AppendMenuW(menu, MF_STRING, ID_MODE_TRAY, L"Show as tray icons");
    CheckMenuRadioItem(menu, ID_MODE_WIDGET, ID_MODE_TRAY,
                       g_wantWidget ? ID_MODE_WIDGET : ID_MODE_TRAY, MF_BYCOMMAND);
    AppendMenuW(menu, MF_STRING | (g_widgetBackground ? MF_CHECKED : 0) | (g_wantWidget ? 0 : MF_GRAYED),
                ID_WIDGET_BACKGROUND, L"Taskbar widget background");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, ID_REFRESH, L"Refresh now");
    AppendMenuW(menu, MF_STRING | (AutostartEnabled() ? MF_CHECKED : 0), ID_AUTOSTART, L"Start with Windows");
    AppendMenuW(menu, MF_STRING, ID_EXIT, L"Exit");

    POINT cursor{};
    GetCursorPos(&cursor);
    SetForegroundWindow(g_hwnd);
    UINT cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON,
                              cursor.x, cursor.y, 0, g_hwnd, nullptr);
    PostMessageW(g_hwnd, WM_NULL, 0, 0);
    DestroyMenu(menu);
    switch (cmd) {
        case ID_REFRESH: RefreshUsage(); break;
        case ID_AUTOSTART: SetAutostart(!AutostartEnabled()); break;
        case ID_WIDGET_BACKGROUND: SetWidgetBackground(!g_widgetBackground); break;
        case ID_MODE_WIDGET: SetMode(true); break;
        case ID_MODE_TRAY: SetMode(false); break;
        case ID_CLAUDE_SETUP: ai::OpenClaudeSetup(); break;
        case ID_CLAUDE_LIVE_SETTINGS: CopyClaudeLiveSettings(); break;
        case ID_EXIT: DestroyWindow(g_hwnd); break;
        default:
            if (cmd >= ID_USAGE_FIRST && cmd < ID_USAGE_FIRST + g_usage.size()) {
                auto& entry = g_usage[cmd - ID_USAGE_FIRST];
                SaveBoolSetting(entry.key, !IsHidden(entry.key));
                ApplyUsage(entry);
                UpdatePlaceholder();
                UpdateWidget();
            }
    }
}

void PublishUsage(size_t provider, const ai::UsageUpdate& reading) {
    auto* update = new ai::UsageUpdate(reading);
    if (!PostMessageW(g_hwnd, WM_USAGE_RESULTS, provider, reinterpret_cast<LPARAM>(update)))
        delete update;
}

void ShowUsageUpdate(size_t provider, const ai::UsageUpdate& update) {
    auto& entry = g_usage[provider];
    if (!entry.quota.Apply(update)) return;
    ApplyUsage(entry);
    UpdatePlaceholder();
    UpdateWidget();
}

DWORD WINAPI CodexWatcher(LPVOID) {
    ai::WatchCodex(g_stopEvt, [](const ai::UsageUpdate& update) { PublishUsage(1, update); });
    return 0;
}

DWORD WINAPI UsageWorker(LPVOID context) {
    const auto& entry = *static_cast<const UsageEntry*>(context);
    const size_t index = &entry - g_usage.data();
    const HANDLE events[] = {g_stopEvt, entry.refresh};
    unsigned startupAttempts = 0;
    const DWORD codexRetryDelays[] = {3000, 15000};
    while (WaitForSingleObject(g_stopEvt, 0) == WAIT_TIMEOUT) {
        ULONGLONG start = GetTickCount64();
        const auto observed = ai::Now();
        auto reading = ai::Poll(entry.provider, g_stopEvt);
        PublishUsage(index, {reading, observed});
        if (reading.Available()) startupAttempts = ARRAYSIZE(codexRetryDelays);
        const auto elapsed = GetTickCount64() - start;
        DWORD delay = elapsed < USAGE_INTERVAL_MS ? USAGE_INTERVAL_MS - (DWORD)elapsed : 0;
        if (entry.provider == ai::Provider::Codex && reading.installed &&
            startupAttempts < ARRAYSIZE(codexRetryDelays)) {
            delay = codexRetryDelays[startupAttempts++];
        }
        DWORD wait = WaitForMultipleObjects(2, events, FALSE, delay);
        if (wait == WAIT_OBJECT_0 || wait == WAIT_FAILED) return 0;
    }
    return 0;
}

LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == g_taskbarCreatedMsg && msg != 0) {  // Explorer restarted
        widget::Destroy();
        g_useWidget = false;
        ReAddAll();
        Reconcile();
        return 0;
    }
    switch (msg) {
        case WM_USAGE_RESULTS: {
            auto* update = reinterpret_cast<ai::UsageUpdate*>(lp);
            ShowUsageUpdate(wp, *update);
            delete update;
            return 0;
        }
        case WM_COPYDATA: {
            auto* copy = reinterpret_cast<const COPYDATASTRUCT*>(lp);
            if (!copy || copy->dwData != ai::CLAUDE_STATUS_MESSAGE ||
                copy->cbData != sizeof(ai::ClaudeStatusData) || !copy->lpData) return FALSE;
            auto* data = static_cast<const ai::ClaudeStatusData*>(copy->lpData);
            ai::Usage usage{data->fiveHours, data->week, true, data->resetsAt};
            if (!usage.Available() || usage.fiveHours < -1 || usage.fiveHours > 100 ||
                usage.week < -1 || usage.week > 100) return FALSE;
            ShowUsageUpdate(0, {usage, ai::Now(), ai::UsageSource::Live});
            return TRUE;
        }
        case WM_TRAY:
            switch (LOWORD(lp)) {
                case WM_CONTEXTMENU:
                case NIN_SELECT:
                case NIN_KEYSELECT: ShowMenu(); break;
            }
            return 0;
        case WM_WIDGET_CLICK:
            ShowMenu();
            return 0;
        case WM_ENDSESSION:  // sign-out / shutdown / installer restart manager
            if (wp) DestroyWindow(h);
            return 0;
        case WM_POWERBROADCAST:
            if (wp == PBT_APMRESUMEAUTOMATIC) RefreshUsage();
            return TRUE;
        case WM_TIMER:
            if (wp == TIMER_WIDGET) Reconcile();
            return 0;
        case WM_SETTINGCHANGE:
            if (lp && !lstrcmpW((LPCWSTR)lp, L"ImmersiveColorSet")) {
                bool light = ReadLightTaskbar();
                if (light != g_lightTaskbar) { g_lightTaskbar = light; ApplyMenuTheme(light); widget::SetLightTheme(light); }
            }
            return 0;
        case WM_DESTROY:
            KillTimer(h, TIMER_WIDGET);
            widget::Destroy();
            SetEvent(g_stopEvt);
            if (g_codexWatcher) WaitForSingleObject(g_codexWatcher, 2000);
            for (const auto& entry : g_usage)
                if (entry.worker) WaitForSingleObject(entry.worker, 2000);
            for (const auto& entry : g_usage)
                if (entry.added) TrayDelete(entry.uid);
            if (g_placeholder) TrayDelete(PLACEHOLDER_ID);
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR command, int) {
    if (wcscmp(command, L"--claude-statusline") == 0) {
        ai::SubmitClaudeStatus(FindWindowW(kWindowClass, nullptr));
        return 0;
    }
    if (wcscmp(command, L"--enable-claude") == 0) return ai::ConfigureClaudeStatus(ExePath(), true);
    if (wcscmp(command, L"--enable-claude-trust") == 0) return ai::ConfigureClaudeTrust(true);
    if (wcscmp(command, L"--prepare-claude") == 0) return ai::PrepareClaudeSetup() ? 0 : 1;
    if (wcscmp(command, L"--disable-claude") == 0) {
        int status = ai::ConfigureClaudeStatus(ExePath(), false);
        int trust = ai::ConfigureClaudeTrust(false);
        return status ? status : trust;
    }
    if (wcscmp(command, L"--quit") == 0) {
        if (auto window = FindWindowW(kWindowClass, nullptr)) {
            PostMessageW(window, WM_CLOSE, 0, 0);
            for (int i = 0; i < 60 && IsWindow(window); ++i) Sleep(100);
        }
        return 0;
    }

    HANDLE mutex = CreateMutexW(nullptr, FALSE, L"Local\\AIUsageMonitor");
    if (!mutex || GetLastError() == ERROR_ALREADY_EXISTS) {
        if (mutex) CloseHandle(mutex);
        return mutex ? 0 : 1;
    }
    g_inst = instance;
    g_lightTaskbar = ReadLightTaskbar();
    g_taskbarCreatedMsg = RegisterWindowMessageW(L"TaskbarCreated");
    g_stopEvt = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    WNDCLASSW wc{};
    wc.hInstance = instance;
    wc.lpfnWndProc = WndProc;
    wc.lpszClassName = kWindowClass;
    if (!g_stopEvt || !RegisterClassW(&wc)) {
        if (g_stopEvt) CloseHandle(g_stopEvt);
        CloseHandle(mutex);
        return 1;
    }
    g_hwnd = CreateWindowExW(WS_EX_TOOLWINDOW, kWindowClass, kAppName, WS_POPUP, 0, 0, 0, 0,
                             nullptr, nullptr, instance, nullptr);
    if (!g_hwnd) { CloseHandle(g_stopEvt); CloseHandle(mutex); return 1; }

    g_wantWidget = LoadWidgetMode();
    g_widgetBackground = LoadWidgetBackground();
    widget::SetBackground(g_widgetBackground);
    for (auto& entry : g_usage) entry.quota.value.installed = ai::IsInstalled(entry.provider);
    Reconcile();
    ReAddAll();
    SetTimer(g_hwnd, TIMER_WIDGET, 1000, nullptr);
    g_codexWatcher = CreateThread(nullptr, 0, CodexWatcher, nullptr, 0, nullptr);
    for (auto& entry : g_usage) {
        entry.refresh = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (entry.refresh) entry.worker = CreateThread(nullptr, 0, UsageWorker, &entry, 0, nullptr);
    }
    MSG message;
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    while (PeekMessageW(&message, nullptr, WM_USAGE_RESULTS, WM_USAGE_RESULTS, PM_REMOVE))
        delete reinterpret_cast<ai::UsageUpdate*>(message.lParam);
    for (auto& entry : g_usage) {
        if (entry.worker) CloseHandle(entry.worker);
        if (entry.refresh) CloseHandle(entry.refresh);
    }
    if (g_codexWatcher) CloseHandle(g_codexWatcher);
    CloseHandle(g_stopEvt);
    CloseHandle(mutex);
    return 0;
}
