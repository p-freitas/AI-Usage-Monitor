#include <windows.h>
#include <shellapi.h>
#include <commctrl.h>
#include <algorithm>
#include <array>
#include <string>
#include <vector>
#include <ctime>

#include "ai_usage.h"
#include "update_apply.h"
#include "update_win.h"
#include "widget.h"
#include "usage_tip.h"
#include "chrome.h"
#include "../res/resource.h"

namespace {


constexpr UINT WM_TRAY = WM_APP + 1;
constexpr UINT WM_WIDGET_CLICK = WM_APP + 3;
constexpr UINT WM_USAGE_RESULTS = WM_APP + 4;
constexpr UINT_PTR TIMER_WIDGET = 1;
constexpr DWORD USAGE_INTERVAL_MS = 2 * 60 * 1000;
constexpr UINT PLACEHOLDER_ID = 1;

constexpr wchar_t kAppName[] = L"AI Usage Monitor";
constexpr wchar_t kWindowClass[] = L"AIUsageMonitorWnd";
constexpr wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t kRunValue[] = L"AIUsageMonitor";
constexpr wchar_t kSettingsKey[] = L"Software\\AIUsageMonitor";

HINSTANCE g_inst;
HWND g_hwnd, g_settingsDialog;
HFONT g_settingsHeading, g_settingsSection, g_settingsSymbols, g_settingsSmallSymbols;
HBRUSH g_settingsBackground, g_settingsSurface, g_settingsHeaderBrush;
int g_settingsScroll, g_settingsHeight;
constexpr int settingsCombos[] = {IDC_LOCATION, ID_DISPLAY, ID_WARNING, ID_CRITICAL};
HANDLE g_stopEvt, g_codexWatcher;
UINT g_taskbarCreatedMsg;
bool g_lightTaskbar, g_useWidget, g_widgetBackground, g_placeholder;
bool g_wantWidget = true;
ui::Preferences g_prefs;
long long g_nextReset;
bool g_balloonIcon;
constexpr UINT BALLOON_ID = 10;
constexpr int warningOptions[] = {60, 70, 75, 80, 85, 90};
constexpr int criticalOptions[] = {80, 85, 90, 95, 99};

struct UsageEntry {
    ai::Provider provider;
    const wchar_t* key;
    UINT uid;
    int resource;
    ai::UsageState quota;
    bool added = false;
    HANDLE refresh = nullptr;
    HANDLE worker = nullptr;
    ai::Connection connection = ai::Connection::Missing;
    ULONGLONG connectionObserved = 0;
    std::array<ui::Crossing, ai::LimitCount> crossings{};
    time_t trayTipMinute = 0;
};
std::array<UsageEntry, 3> g_usage = {{
    {ai::Provider::Claude, L"HideClaude", 2, IDI_CLAUDE},
    {ai::Provider::Codex, L"HideCodex", 3, IDI_CODEX},
    {ai::Provider::Gemini, L"HideGemini", 4, IDI_GEMINI}
}};

// ---- helpers ----------------------------------------------------------------------------------

bool ReadLightTaskbar() {
    DWORD v = 0, sz = sizeof(v);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                     L"SystemUsesLightTheme", RRF_RT_REG_DWORD, nullptr, &v, &sz) == ERROR_SUCCESS)
        return v != 0;
    return false;
}

std::wstring ExePath() {
    wchar_t p[MAX_PATH];
    GetModuleFileNameW(nullptr, p, MAX_PATH);
    return p;
}

DWORD LoadSetting(const wchar_t* name, DWORD fallback) {
    DWORD v{}, sz = sizeof(v);
    return RegGetValueW(HKEY_CURRENT_USER, kSettingsKey, name, RRF_RT_REG_DWORD, nullptr,
                        &v, &sz) == ERROR_SUCCESS ? v : fallback;
}

void SaveSetting(const wchar_t* name, DWORD v) {
    RegSetKeyValueW(HKEY_CURRENT_USER, kSettingsKey, name, REG_DWORD, &v, sizeof(v));
}
bool LoadBoolSetting(const wchar_t* name, bool fallback) { return LoadSetting(name, fallback) != 0; }
void SaveBoolSetting(const wchar_t* name, bool on) { SaveSetting(name, on); }

void LoadPreferences() {
    g_prefs.display = static_cast<ui::Display>(LoadSetting(L"DisplayMode", 0));
    g_prefs.reset = static_cast<ui::Reset>(LoadSetting(L"ResetFormat", 0));
    g_prefs.colors = LoadBoolSetting(L"WarningColors", true);
    g_prefs.notifications = LoadBoolSetting(L"UsageNotifications", false);
    g_prefs.warning = LoadSetting(L"WarningThreshold", 80);
    g_prefs.critical = LoadSetting(L"CriticalThreshold", 90);
    g_prefs.Validate();
}

bool LoadWidgetMode() { return LoadBoolSetting(L"TaskbarWidget", true); }
void SaveWidgetMode(bool on) { SaveBoolSetting(L"TaskbarWidget", on); }
bool LoadWidgetBackground() { return LoadBoolSetting(L"WidgetBackground", true); }
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

std::wstring ConnectionHint(const UsageEntry& entry) {
    if (entry.provider == ai::Provider::Gemini) {
        if (entry.connection == ai::Connection::Missing) return L"Install Antigravity CLI (agy)";
        if (entry.connection == ai::Connection::SignIn) return L"Sign in with Antigravity CLI";
    }
    switch (entry.connection) {
        case ai::Connection::Missing:
            return entry.provider == ai::Provider::Claude ? L"Install Claude Code CLI" : L"Install Codex CLI or Desktop";
        case ai::Connection::SignIn:
            return entry.provider == ai::Provider::Claude ? L"Sign in to Claude Code" : L"Sign in to Codex (Desktop or codex login)";
        case ai::Connection::Setup: return L"Complete Claude Code setup";
        case ai::Connection::Unavailable: return L"Could not read usage. Try Refresh now";
        case ai::Connection::Waiting: return L"Waiting for the first usage reading";
        case ai::Connection::Unsupported: return L"Unverified Antigravity CLI version (tested: 1.5.15)";
        case ai::Connection::NoLimits: return L"No supported Gemini quota reported for this account";
        case ai::Connection::Ready: return {};
    }
    return {};
}

ui::ProviderView View(const UsageEntry& entry) {
    bool disconnected = entry.connection != ai::Connection::Ready;
    auto view = ui::Describe(entry.provider, entry.resource, entry.quota, g_prefs, disconnected);
    view.status = ConnectionHint(entry);
    return view;
}

void ApplyUsage(UsageEntry& entry) {
    bool visible = (entry.quota.value.Available() || (entry.provider != ai::Provider::Gemini && entry.quota.value.installed)) && !g_useWidget && !IsHidden(entry.key);
    if (!visible) {
        if (entry.added) TrayDelete(entry.uid);
        entry.added = false;
        return;
    }
    HICON icon = LoadIconW(g_inst, MAKEINTRESOURCEW(entry.resource));
    if (icon) {
        auto now = std::time(nullptr);
        entry.trayTipMinute = now / 60;
        TrayCall(entry.added ? NIM_MODIFY : NIM_ADD, entry.uid, icon, ui::TrayText(View(entry), g_prefs.reset, now));
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

void UpdateSettingsStatus();
void UpdateWidget(bool opening = false) {
    if (g_settingsDialog) UpdateSettingsStatus();
    if (!opening && !widget::Alive() && !widget::UsageTipOpen()) return;
    std::vector<ui::ProviderView> items;
    for (const auto& entry : g_usage) {
        if (entry.quota.value.Available() && !IsHidden(entry.key)) items.push_back(View(entry));
    }
    widget::SetItems(std::move(items), g_prefs);
}

void Reconcile() {
    bool attached = g_wantWidget && widget::Alive();
    if (g_wantWidget && !attached) {
        attached = widget::Create(g_inst, g_hwnd, WM_WIDGET_CLICK, g_lightTaskbar);
        if (attached) UpdateWidget();
    } else if (attached) {
        widget::Reposition();
    } else if (!g_wantWidget && widget::Alive()) {
        widget::Destroy();
    }
    bool visible = attached && widget::FitsTaskbar();
    if (visible != g_useWidget) {
        g_useWidget = visible;
        for (auto& entry : g_usage) ApplyUsage(entry);
        UpdatePlaceholder();
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
    MessageBoxW(g_settingsDialog, copied
        ? L"Merge the copied statusLine entry into your Claude Code settings.json. "
          L"If you already use a status line, keep its existing command and chain the monitor as described in the README."
        : L"Could not copy the Claude Code settings.", kAppName, MB_OK | MB_ICONINFORMATION);
}

DWORD WINAPI UsageWorker(LPVOID context);
void StartWorker(UsageEntry& entry) {
    if (entry.worker) return;
    if (!entry.refresh) entry.refresh = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (entry.refresh) entry.worker = CreateThread(nullptr, 0, UsageWorker, &entry, 0, nullptr);
}

void RefreshUsage() {
    for (auto& entry : g_usage) {
        if (IsHidden(entry.key)) continue;
        if (entry.worker) SetEvent(entry.refresh);
        else if (entry.provider != ai::Provider::Gemini || ai::IsInstalled(entry.provider)) StartWorker(entry);
    }
}


bool SetPreference(UINT cmd) {
    if (cmd >= ID_DISPLAY && cmd < ID_DISPLAY+3) {
        g_prefs.display = static_cast<ui::Display>(cmd-ID_DISPLAY);
        SaveSetting(L"DisplayMode", cmd-ID_DISPLAY);
    } else if (cmd >= ID_RESET && cmd < ID_RESET+3) {
        g_prefs.reset = static_cast<ui::Reset>(cmd-ID_RESET);
        SaveSetting(L"ResetFormat", cmd-ID_RESET);
    } else if (cmd == ID_COLORS) {
        g_prefs.colors = !g_prefs.colors;
        SaveSetting(L"WarningColors", g_prefs.colors);
    } else if (cmd == ID_NOTIFICATIONS) {
        g_prefs.notifications = !g_prefs.notifications;
        SaveSetting(L"UsageNotifications", g_prefs.notifications);
        for (auto& entry : g_usage) {
            entry.crossings = {};
            if (!g_prefs.notifications || entry.connection != ai::Connection::Ready) continue;
            for (const auto& row : View(entry).rows)
                if (!row.stale) entry.crossings[row.limit].Observe(row.used, row.reset, g_prefs);
        }
    } else if (cmd >= ID_WARNING && cmd < ID_WARNING+std::size(warningOptions)) {
        g_prefs.warning = warningOptions[cmd-ID_WARNING];
        SaveSetting(L"WarningThreshold", g_prefs.warning);
    } else if (cmd >= ID_CRITICAL && cmd < ID_CRITICAL+std::size(criticalOptions)) {
        g_prefs.critical = criticalOptions[cmd-ID_CRITICAL];
        SaveSetting(L"CriticalThreshold", g_prefs.critical);
    } else return false;
    for (auto& entry : g_usage) ApplyUsage(entry);
    UpdateWidget();
    return true;
}

void ApplyCommand(UINT cmd) {
    if (SetPreference(cmd)) return;
    switch (cmd) {
        case ID_REFRESH: RefreshUsage(); break;
        case ID_APP_UPDATE: updates::Activate(g_settingsDialog); break;
        case ID_AUTO_UPDATE: updates::ToggleAutomatic(); break;
        case ID_AUTOSTART: SetAutostart(!AutostartEnabled()); break;
        case ID_WIDGET_BACKGROUND: SetWidgetBackground(!g_widgetBackground); break;
        case ID_MODE_WIDGET: SetMode(true); break;
        case ID_MODE_TRAY: SetMode(false); break;
        case ID_CLAUDE_SETUP: ai::OpenClaudeSetup(); break;
        case ID_CLAUDE_LIVE_SETTINGS: CopyClaudeLiveSettings(); break;

        default:
            if (cmd >= ID_USAGE_FIRST && cmd < ID_USAGE_FIRST + g_usage.size()) {
                auto& entry = g_usage[cmd - ID_USAGE_FIRST];
                SaveBoolSetting(entry.key, !IsHidden(entry.key));
                if (!IsHidden(entry.key)) {
                    if (entry.worker) SetEvent(entry.refresh);
                    else StartWorker(entry);
                }
                ApplyUsage(entry);
                UpdatePlaceholder();
                UpdateWidget();
            }
    }
}

void UpdateSettingsStatus() {
    for (size_t i = 0; i < g_usage.size(); ++i) {
        int id = ID_USAGE_FIRST + static_cast<int>(i);
        InvalidateRect(GetDlgItem(g_settingsDialog, id), nullptr, FALSE);
    }
    EnableWindow(GetDlgItem(g_settingsDialog, ID_CLAUDE_SETUP),
                 g_usage[0].quota.value.installed && g_usage[0].connection != ai::Connection::Ready);
}

void SyncSettings(HWND dlg) {
    UpdateSettingsStatus();
    for (auto pair : {std::pair{IDC_LOCATION, g_wantWidget ? 0 : 1},
                      std::pair{ID_DISPLAY, static_cast<int>(g_prefs.display)}})
        SendDlgItemMessageW(dlg, pair.first, CB_SETCURSEL, pair.second, 0);
    auto thresholds = [&](int id, const auto& values, bool critical) {
        HWND box = GetDlgItem(dlg, id);
        SendMessageW(box, CB_RESETCONTENT, 0, 0);
        for (size_t i = 0; i < std::size(values); ++i) {
            int value = values[i];
            if (critical ? value <= g_prefs.warning : value >= g_prefs.critical) continue;
            auto label = std::to_wstring(value) + L"%";
            auto row = SendMessageW(box, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
            SendMessageW(box, CB_SETITEMDATA, row, id+i);
            if (value == (critical ? g_prefs.critical : g_prefs.warning))
                SendMessageW(box, CB_SETCURSEL, row, 0);
        }
    };
    thresholds(ID_WARNING, warningOptions, false);
    thresholds(ID_CRITICAL, criticalOptions, true);
    EnableWindow(GetDlgItem(dlg, ID_WIDGET_BACKGROUND), g_wantWidget);
    EnableWindow(GetDlgItem(dlg, ID_AUTO_UPDATE), updates::Supported());
    EnableWindow(GetDlgItem(dlg, ID_APP_UPDATE), !updates::Busy());
    SetDlgItemTextW(dlg, ID_APP_UPDATE, updates::MenuLabel().c_str());
    RedrawWindow(dlg, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN);
}

RECT DialogRect(HWND dlg, RECT units) {
    MapDialogRect(dlg, &units);
    OffsetRect(&units, 0, -g_settingsScroll);
    return units;
}

int DialogPx(HWND dlg, int px) { return MulDiv(px, GetDpiForWindow(dlg), 96); }

// Layout in dialog units; keep in sync with IDD_SETTINGS in res/app.rc.
constexpr int kSettingsWidth = 278, kSettingsHeight = 422, kSettingsHeader = 26;
constexpr int kSettingsRules[] = {115, 191, 253, 346, 386};
constexpr RECT kProvidersCard{13,53,265,109}, kResetGroup{13,219,265,247};

COLORREF LinkColor(const ui::Palette& colors) {
    return g_lightTaskbar ? colors.accent : ui::Mix(colors.accent, RGB(255,255,255), 32);
}

void PaintSettings(HWND dlg, HDC dc) {
    auto colors = ui::Colors(g_lightTaskbar);
    RECT client{};
    GetClientRect(dlg, &client);
    ui::Fill(dc, client, colors.background);
    auto header = DialogRect(dlg, {0,0,kSettingsWidth,kSettingsHeader});
    header.right = client.right;
    ui::Fill(dc, header, colors.header);
    ui::Fill(dc, {0,header.bottom-1,client.right,header.bottom}, colors.border);
    ui::Fill(dc, {0,0,1,client.bottom}, colors.border);
    ui::Fill(dc, {client.right-1,0,client.right,client.bottom}, colors.border);
    int bottom = DialogRect(dlg, {0,kSettingsHeight,0,kSettingsHeight}).top;
    ui::Fill(dc, {0,bottom-1,client.right,bottom}, colors.border);
    for (int line : kSettingsRules) {
        auto rule = DialogRect(dlg, {14,line,264,line});
        rule.bottom++;
        ui::Fill(dc, rule, colors.border);
    }
    ui::Rounded(dc, DialogRect(dlg, kProvidersCard), DialogPx(dlg,10), colors.surface, colors.border);
    ui::Rounded(dc, DialogRect(dlg, kResetGroup), DialogPx(dlg,10), colors.surface, colors.border);
    // Severity dots in front of the threshold labels.
    auto dot = [&](int x, COLORREF color) {
        auto anchor = DialogRect(dlg, {x,304,x,304});
        int r = DialogPx(dlg,4);
        ui::Rounded(dc, {anchor.left-r,anchor.top-r,anchor.left+r,anchor.top+r}, r, color, color);
    };
    dot(18, g_lightTaskbar ? RGB(214,150,0) : RGB(250,196,72));
    dot(150, g_lightTaskbar ? RGB(200,45,60) : RGB(240,96,100));
}

std::wstring ControlText(HWND control) {
    wchar_t buffer[256]{};
    GetWindowTextW(control, buffer, ARRAYSIZE(buffer));
    return buffer;
}

// Draws an optional Segoe MDL2 glyph run followed by a label.
void IconLabel(HDC dc, RECT r, const std::wstring& icon, const std::wstring& text, HFONT font,
               COLORREF iconInk, COLORREF ink, bool center, int gap) {
    int iconWidth = icon.empty() ? 0 : ui::Measure(dc, icon, g_settingsSymbols);
    int total = ui::Measure(dc, text, font) + (icon.empty() ? 0 : iconWidth+gap);
    int x = center ? std::max<int>(r.left, (r.left+r.right-total)/2) : r.left;
    if (!icon.empty()) {
        ui::Text(dc, {x,r.top,x+iconWidth,r.bottom}, icon, g_settingsSymbols, iconInk);
        x += iconWidth+gap;
    }
    ui::Text(dc, {x,r.top,r.right,r.bottom}, text, font, ink);
}

void DrawCheckBox(HDC dc, RECT box, bool checked, int radius, const ui::Palette& colors, HFONT font) {
    ui::Rounded(dc, box, radius, checked ? colors.accent : colors.surface, checked ? colors.accent : colors.border);
    if (checked) ui::Text(dc, box, ui::glyph::Check, font, RGB(255,255,255), DT_CENTER);
}

void DrawSettingsButton(HWND dlg, const DRAWITEMSTRUCT& item) {
    auto colors = ui::Colors(g_lightTaskbar);
    auto dc = item.hDC;
    auto r = item.rcItem;
    auto font = reinterpret_cast<HFONT>(SendMessageW(dlg, WM_GETFONT, 0, 0));
    auto id = item.CtlID;
    auto px = [dlg](int n) { return DialogPx(dlg,n); };
    bool pressed = (item.itemState & ODS_SELECTED) != 0;
    bool disabled = (item.itemState & ODS_DISABLED) != 0;
    COLORREF ink = disabled ? ui::Mix(colors.background, colors.muted, 60) : colors.text;
    bool provider = id >= ID_USAGE_FIRST && id < ID_USAGE_FIRST+g_usage.size();
    bool resetChoice = id >= ID_RESET && id <= ID_RESET+2;
    bool link = id == ID_APP_UPDATE || id == ID_EXIT || id == ID_CLAUDE_LIVE_SETTINGS || id == ID_CLAUDE_SETUP;
    ui::Fill(dc, r, (provider || resetChoice) ? colors.surface : id == IDCANCEL ? colors.header : colors.background);
    if (provider) {
        const auto& entry = g_usage[id-ID_USAGE_FIRST];
        auto box = RECT{px(10),r.top+(r.bottom-r.top-px(16))/2,px(26),r.top+(r.bottom-r.top+px(16))/2};
        DrawCheckBox(dc, box, !IsHidden(entry.key), px(4), colors, g_settingsSymbols);
        ui::Icon(dc, entry.resource, px(36), r.top+(r.bottom-r.top-px(20))/2, px(20));
        int left = px(66), end = r.right-px(12);
        ui::Text(dc, {left,r.top,left+px(76),r.bottom}, ai::Name(entry.provider), g_settingsHeading, ink);
        auto hint = ConnectionHint(entry);
        if (entry.quota.value.Available()) {
            auto prefs = g_prefs;
            prefs.display = ui::Display::Complete;
            auto limits = ui::Describe(entry.provider, entry.resource, entry.quota, prefs,
                                       entry.connection != ai::Connection::Ready);
            for (auto i=limits.strip.rbegin(); i!=limits.strip.rend(); ++i) {
                RECT text{left+px(72),r.top,end,r.bottom};
                auto valueColor = widget::UsageColor(i->severity, g_lightTaskbar, i->stale ? colors.muted : ink);
                end -= ui::Text(dc, text, i->value, font, valueColor, DT_RIGHT);
                text.right = end;
                end -= ui::Text(dc, text, i->label+L" ", font, colors.muted, DT_RIGHT);
                if (i+1 != limits.strip.rend()) {
                    text.right = end;
                    end -= ui::Text(dc, text, L" \u00b7 ", font, colors.muted, DT_RIGHT);
                }
            }
        } else {
            ui::Text(dc, {left+px(74),r.top,end,r.bottom}, hint, font, colors.muted, DT_RIGHT);
        }
        if (id+1 < ID_USAGE_FIRST+g_usage.size())
            ui::Fill(dc, {px(8),r.bottom-1,r.right-px(8),r.bottom}, colors.border);
    } else if (resetChoice) {
        static const wchar_t* icons[] = {ui::glyph::Clock, ui::glyph::Calendar, L"\uE823\uE787"};
        static const wchar_t* examples[] = {L"Resets in 1h 09m", L"Resets at 15:40", L"1h 09m (15:40)"};
        const int index = static_cast<int>(id-ID_RESET);
        const int current = static_cast<int>(g_prefs.reset);
        const bool selected = index == current;
        if (selected) {
            RECT outline = r;
            InflateRect(&outline, -1, -1);
            ui::Rounded(dc, outline, px(8), colors.accent, colors.accent);
            InflateRect(&outline, -px(2), -px(2));
            ui::Rounded(dc, outline, px(6), colors.selected, colors.selected);
        } else if (index < 2 && index+1 != current) {
            ui::Fill(dc, {r.right-1,r.top+px(9),r.right,r.bottom-px(9)}, colors.border);
        }
        RECT top = r;
        top.bottom = (r.top+r.bottom)/2+px(2);
        RECT bottom = r;
        bottom.top = top.bottom-px(3);
        auto labelInk = selected ? colors.accentText : ink;
        IconLabel(dc, top, icons[index], ControlText(item.hwndItem), g_settingsHeading,
                  selected ? colors.accentText : colors.muted, labelInk, true, px(6));
        ui::Text(dc, bottom, examples[index], font, selected ? colors.accentText : colors.muted, DT_CENTER);
    } else if (id == ID_NOTIFICATIONS) {
        RECT box{0,(r.top+r.bottom-px(16))/2,px(16),(r.top+r.bottom+px(16))/2};
        DrawCheckBox(dc, box, g_prefs.notifications, px(4), colors, g_settingsSymbols);
        r.left += px(26);
        IconLabel(dc, r, ui::glyph::Bell, ControlText(item.hwndItem), font, colors.muted, ink, false, px(8));
    } else if (id == ID_COLORS || id == ID_WIDGET_BACKGROUND || id == ID_AUTOSTART || id == ID_AUTO_UPDATE) {
        bool on = id == ID_COLORS ? g_prefs.colors : id == ID_WIDGET_BACKGROUND ? g_widgetBackground :
                  id == ID_AUTOSTART ? AutostartEnabled() : updates::AutomaticEnabled();
        RECT track{0,(r.top+r.bottom-px(18))/2,px(32),(r.top+r.bottom+px(18))/2};
        bool lit = on && !disabled;
        ui::Rounded(dc, track, px(9), lit ? colors.accent : colors.surface, lit ? colors.accent : colors.border);
        int knobSize = px(12);
        int knobX = on ? track.right-px(3)-knobSize : track.left+px(3);
        RECT knob{knobX,track.top+px(3),knobX+knobSize,track.top+px(3)+knobSize};
        auto knobColor = lit ? RGB(250,250,254) : ui::Mix(colors.surface, colors.muted, 75);
        ui::Rounded(dc, knob, px(6), knobColor, knobColor);
        r.left += px(39);
        IconLabel(dc, r, id == ID_AUTO_UPDATE ? ui::glyph::Sync : L"", ControlText(item.hwndItem), font,
                  colors.muted, ink, false, px(5));
    } else if (link) {
        const wchar_t* icon = id == ID_APP_UPDATE ? ui::glyph::External : id == ID_EXIT ? ui::glyph::Power :
                              id == ID_CLAUDE_LIVE_SETTINGS ? ui::glyph::Copy : ui::glyph::Settings;
        auto color = disabled ? ink : id == ID_EXIT ? colors.muted : LinkColor(colors);
        if (pressed && !disabled) color = colors.text;
        IconLabel(dc, r, icon, ControlText(item.hwndItem), font, color, color, false, px(7));
    } else if (id == IDCANCEL) {
        if (pressed) ui::Rounded(dc, r, px(7), colors.selected, colors.border);
        ui::Text(dc, r, ui::glyph::Close, g_settingsSymbols, colors.text, DT_CENTER);
    } else {
        bool primary = id == IDOK;
        auto fill = primary ? (pressed ? ui::Mix(colors.accent, RGB(0,0,0), 15) : colors.accent)
                            : pressed ? colors.selected : colors.surface;
        ui::Rounded(dc, r, px(8), fill, primary ? fill : colors.border);
        auto text = primary ? RGB(255,255,255) : ink;
        IconLabel(dc, r, primary ? ui::glyph::Check : ui::glyph::Sync, ControlText(item.hwndItem),
                  primary ? g_settingsHeading : font, text, text, true, px(8));
    }
    if ((item.itemState & ODS_FOCUS) && !(item.itemState & ODS_NOFOCUSRECT)) {
        r = item.rcItem;
        InflateRect(&r, -2, -2);
        DrawFocusRect(dc, &r);
    }
}

void ScrollSettings(HWND dlg, int target) {
    RECT client{};
    GetClientRect(dlg, &client);
    int next = std::clamp(target, 0, std::max(0, g_settingsHeight-static_cast<int>(client.bottom)));
    if (next == g_settingsScroll) return;
    int delta = g_settingsScroll-next;
    g_settingsScroll = next;
    SetScrollPos(dlg, SB_VERT, next, TRUE);
    for (HWND child = GetWindow(dlg, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT)) {
        RECT bounds{};
        GetWindowRect(child, &bounds);
        POINT pt{bounds.left,bounds.top};
        ScreenToClient(dlg, &pt);
        SetWindowPos(child, nullptr, pt.x, pt.y+delta, 0,0, SWP_NOZORDER|SWP_NOSIZE|SWP_NOACTIVATE);
    }
    RedrawWindow(dlg, nullptr, nullptr, RDW_INVALIDATE|RDW_ALLCHILDREN);
}

void RevealSetting(HWND dlg, HWND child) {
    RECT frame{}, control{};
    GetClientRect(dlg, &frame);
    GetWindowRect(child, &control);
    MapWindowPoints(nullptr, dlg, reinterpret_cast<POINT*>(&control), 2);
    if (control.top < frame.top) ScrollSettings(dlg, g_settingsScroll+control.top);
    else if (control.bottom > frame.bottom) ScrollSettings(dlg, g_settingsScroll+control.bottom-frame.bottom);
}

LRESULT CALLBACK SettingsCombo(HWND box, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR) {
    if (msg == WM_PAINT || msg == WM_PRINTCLIENT) {
        PAINTSTRUCT ps{};
        HDC dc = msg == WM_PAINT ? BeginPaint(box, &ps) : reinterpret_cast<HDC>(wp);
        auto colors = ui::Colors(g_lightTaskbar);
        RECT r{};
        GetClientRect(box, &r);
        ui::Fill(dc, r, colors.background);
        ui::Rounded(dc, r, DialogPx(box,7), colors.surface, GetFocus() == box ? colors.accent : colors.border);
        r.left += DialogPx(box,10);
        r.right -= DialogPx(box,10);
        auto font = reinterpret_cast<HFONT>(SendMessageW(box, WM_GETFONT, 0, 0));
        ui::Text(dc, r, ui::glyph::Chevron, g_settingsSmallSymbols, colors.muted, DT_RIGHT);
        r.right -= DialogPx(box,20);
        auto selected = SendMessageW(box, CB_GETCURSEL, 0, 0);
        if (selected != CB_ERR) {
            auto length = SendMessageW(box, CB_GETLBTEXTLEN, selected, 0);
            if (length >= 0) {
                std::wstring label(static_cast<size_t>(length)+1,L'\0');
                SendMessageW(box, CB_GETLBTEXT, selected, reinterpret_cast<LPARAM>(label.data()));
                label.resize(static_cast<size_t>(length));
                IconLabel(dc, r, GetDlgCtrlID(box) == IDC_LOCATION ? ui::glyph::Monitor : L"", label, font,
                          colors.muted, colors.text, false, DialogPx(box,8));
            }
        }
        if (msg == WM_PAINT) EndPaint(box, &ps);
        return 0;
    }
    if (msg == WM_NCDESTROY) RemoveWindowSubclass(box, SettingsCombo, 1);
    return DefSubclassProc(box, msg, wp, lp);
}

INT_PTR CALLBACK SettingsProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_INITDIALOG) {
        g_settingsDialog = dlg;
        g_settingsScroll = 0;
        auto colors = ui::Colors(g_lightTaskbar);
        g_settingsBackground = CreateSolidBrush(colors.background);
        g_settingsSurface = CreateSolidBrush(colors.surface);
        g_settingsHeaderBrush = CreateSolidBrush(colors.header);
        g_settingsHeading = ui::Font(DialogPx(dlg,14), FW_SEMIBOLD);
        g_settingsSection = ui::Font(DialogPx(dlg,16), FW_SEMIBOLD);
        g_settingsSymbols = ui::SymbolFont(DialogPx(dlg,13));
        g_settingsSmallSymbols = ui::SymbolFont(DialogPx(dlg,10));
        SetDlgItemTextW(dlg, IDC_SETTINGS_TITLE, L"AI Usage Monitor \u2014 Settings");
        for (auto id : {IDC_SETTINGS_TITLE, IDC_PROVIDERS_TITLE, IDC_DISPLAY_TITLE, IDC_RESET_TITLE, IDC_WARNINGS_TITLE})
            SendDlgItemMessageW(dlg, id, WM_SETFONT, reinterpret_cast<WPARAM>(g_settingsSection), FALSE);
        for (auto id : settingsCombos) SetWindowSubclass(GetDlgItem(dlg,id), SettingsCombo, 1, 0);
        auto fill = [&](int id, std::initializer_list<const wchar_t*> options) {
            for (auto label : options) SendDlgItemMessageW(dlg, id, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label));
        };
        fill(IDC_LOCATION, {L"Taskbar widget", L"Notification area"});
        fill(ID_DISPLAY, {L"Complete", L"Minimal", L"Weekly only"});
        SyncSettings(dlg);
        RECT rc{0,0,kSettingsWidth,kSettingsHeight}, panel{};
        // CAPTION in the template implies WS_CAPTION; keep the window title for accessibility
        // but draw our own header instead of the system title bar.
        SetWindowLongPtrW(dlg, GWL_STYLE, GetWindowLongPtrW(dlg, GWL_STYLE) & ~(WS_CAPTION|WS_THICKFRAME));
        MapDialogRect(dlg, &rc);
        GetWindowRect(GetWindow(dlg, GW_OWNER), &panel);
        MONITORINFO monitor{sizeof(monitor)};
        if (GetMonitorInfoW(MonitorFromRect(&panel, MONITOR_DEFAULTTONEAREST), &monitor)) {
            const RECT& area = monitor.rcWork;
            g_settingsHeight = rc.bottom-rc.top;
            int width = rc.right-rc.left;
            int height = std::min<int>(g_settingsHeight, area.bottom-area.top-8);
            if (height < g_settingsHeight) {
                SetWindowLongPtrW(dlg, GWL_STYLE, GetWindowLongPtrW(dlg, GWL_STYLE)|WS_VSCROLL);
                width += GetSystemMetricsForDpi(SM_CXVSCROLL, GetDpiForWindow(dlg));
            }
            // Center over the owner so the complete dialog sits in front of the quota panel.
            int x = std::clamp<int>((panel.left+panel.right-width)/2, area.left,
                                    std::max<LONG>(area.left, area.right-width));
            int y = std::clamp<int>((panel.top+panel.bottom-height)/2, area.top, area.bottom-height);
            SetWindowPos(dlg, nullptr, x, y, width, height, SWP_NOZORDER | SWP_FRAMECHANGED);
            if (height < g_settingsHeight) {
                SCROLLINFO scroll{sizeof(scroll),SIF_RANGE|SIF_PAGE,0,g_settingsHeight-1,static_cast<UINT>(height)};
                SetScrollInfo(dlg, SB_VERT, &scroll, TRUE);
            } else {
                auto region = CreateRoundRectRgn(0,0,width+1,height+1,DialogPx(dlg,20),DialogPx(dlg,20));
                if (region && !SetWindowRgn(dlg, region, TRUE)) DeleteObject(region);
            }
        }
        return TRUE;
    }
    if (msg == WM_PAINT || msg == WM_PRINTCLIENT) {
        PAINTSTRUCT ps{};
        auto dc = msg == WM_PAINT ? BeginPaint(dlg, &ps) : reinterpret_cast<HDC>(wp);
        PaintSettings(dlg, dc);
        if (msg == WM_PAINT) EndPaint(dlg, &ps);
        return TRUE;
    }
    if (msg == WM_ERASEBKGND) return TRUE;
    if (msg == WM_CTLCOLORSTATIC || msg == WM_CTLCOLORLISTBOX) {
        auto dc = reinterpret_cast<HDC>(wp);
        auto colors = ui::Colors(g_lightTaskbar);
        int id = GetDlgCtrlID(reinterpret_cast<HWND>(lp));
        SetTextColor(dc, id == IDC_SETTINGS_HINT ? colors.muted : colors.text);
        if (msg == WM_CTLCOLORLISTBOX) {
            SetBkMode(dc, TRANSPARENT);
            return reinterpret_cast<INT_PTR>(g_settingsSurface);
        }
        // Statics must paint an opaque background: the dialog clips its children, so a
        // hollow brush leaves whatever was behind the label (a white box) on screen.
        bool header = id == IDC_SETTINGS_TITLE;
        SetBkColor(dc, header ? colors.header : colors.background);
        return reinterpret_cast<INT_PTR>(header ? g_settingsHeaderBrush : g_settingsBackground);
    }
    if (msg == WM_CTLCOLORDLG) return reinterpret_cast<INT_PTR>(g_settingsBackground);
    if (msg == WM_DRAWITEM) {
        auto& item = *reinterpret_cast<DRAWITEMSTRUCT*>(lp);
        if (item.CtlType == ODT_BUTTON) DrawSettingsButton(dlg, item);
        else if (item.CtlType == ODT_COMBOBOX) {
            auto colors = ui::Colors(g_lightTaskbar);
            bool selected = (item.itemState & ODS_SELECTED) != 0;
            ui::Fill(item.hDC,item.rcItem,selected ? colors.selected : colors.surface);
            if (item.itemID != UINT(-1)) {
                auto length = SendMessageW(item.hwndItem, CB_GETLBTEXTLEN, item.itemID, 0);
                if (length < 0) return TRUE;
                std::wstring label(static_cast<size_t>(length)+1,L'\0');
                SendMessageW(item.hwndItem, CB_GETLBTEXT, item.itemID, reinterpret_cast<LPARAM>(label.data()));
                RECT r = item.rcItem;
                r.left += DialogPx(dlg,7);
                auto font = reinterpret_cast<HFONT>(SendMessageW(dlg, WM_GETFONT, 0, 0));
                ui::Text(item.hDC, r, label, font, selected ? colors.accentText : colors.text);
            }
        }
        return TRUE;
    }
    if (msg == WM_MEASUREITEM) {
        auto& item = *reinterpret_cast<MEASUREITEMSTRUCT*>(lp);
        if (item.CtlType == ODT_COMBOBOX) item.itemHeight = DialogPx(dlg, item.itemID == UINT(-1) ? 26 : 24);
        return TRUE;
    }
    if (msg == WM_MOUSEWHEEL) {
        ScrollSettings(dlg, g_settingsScroll-GET_WHEEL_DELTA_WPARAM(wp)*DialogPx(dlg,45)/WHEEL_DELTA);
        return TRUE;
    }
    if (msg == WM_VSCROLL && lp == 0) {
        SCROLLINFO scroll{sizeof(scroll),SIF_TRACKPOS|SIF_PAGE};
        GetScrollInfo(dlg, SB_VERT, &scroll);
        auto page = static_cast<int>(scroll.nPage);
        int target = g_settingsScroll;
        switch (LOWORD(wp)) {
            case SB_THUMBTRACK: case SB_THUMBPOSITION: target = scroll.nTrackPos; break;
            case SB_LINEUP: target -= DialogPx(dlg,22); break;
            case SB_LINEDOWN: target += DialogPx(dlg,22); break;
            case SB_PAGEUP: target -= page; break;
            case SB_PAGEDOWN: target += page; break;
            case SB_TOP: target = 0; break;
            case SB_BOTTOM: target = g_settingsHeight; break;
        }
        ScrollSettings(dlg, target);
        return TRUE;
    }
    if (msg == WM_DESTROY) {
        DeleteObject(g_settingsHeading);
        DeleteObject(g_settingsSection);
        DeleteObject(g_settingsSymbols);
        DeleteObject(g_settingsSmallSymbols);
        DeleteObject(g_settingsBackground);
        DeleteObject(g_settingsSurface);
        DeleteObject(g_settingsHeaderBrush);
        g_settingsHeading = g_settingsSection = g_settingsSymbols = g_settingsSmallSymbols = nullptr;
        g_settingsDialog = nullptr;
    }
    if (msg == WM_CLOSE) { EndDialog(dlg, IDCANCEL); return TRUE; }
    if (msg != WM_COMMAND) return FALSE;
    UINT cmd = LOWORD(wp);
    if (HIWORD(wp) == BN_SETFOCUS || HIWORD(wp) == CBN_SETFOCUS) {
        RevealSetting(dlg, reinterpret_cast<HWND>(lp));
        return TRUE;
    }
    if (HIWORD(wp) == BN_KILLFOCUS || HIWORD(wp) == CBN_KILLFOCUS) return TRUE;
    if (cmd == IDOK || cmd == IDCANCEL || cmd == ID_EXIT) {
        EndDialog(dlg, cmd);
        return TRUE;
    }
    if (HIWORD(wp) == CBN_SELCHANGE) {
        auto row = SendDlgItemMessageW(dlg, cmd, CB_GETCURSEL, 0, 0);
        if (row == CB_ERR) return TRUE;
        if (cmd == IDC_LOCATION) cmd = row == 0 ? ID_MODE_WIDGET : ID_MODE_TRAY;
        else if (cmd == ID_WARNING || cmd == ID_CRITICAL)
            cmd = static_cast<UINT>(SendDlgItemMessageW(dlg, cmd, CB_GETITEMDATA, row, 0));
        else cmd += static_cast<UINT>(row);
    } else if (HIWORD(wp) != BN_CLICKED) return FALSE;
    ApplyCommand(cmd);
    SyncSettings(dlg);
    return TRUE;
}

void OpenPanel(const RECT& bounds, HWND dpiWindow) {
    if (g_settingsDialog) { SetForegroundWindow(g_settingsDialog); return; }
    UpdateWidget(true);
    UINT dpi = GetDpiForWindow(dpiWindow);
    widget::OpenPanel(g_hwnd, bounds, dpi ? dpi : 96, g_lightTaskbar);
}

void PublishUsage(size_t provider, const ai::UsageUpdate& reading) {
    auto* update = new ai::UsageUpdate(reading);
    if (!PostMessageW(g_hwnd, WM_USAGE_RESULTS, provider, reinterpret_cast<LPARAM>(update)))
        delete update;
}

void NotifyCrossings(UsageEntry& entry, const ai::UsageUpdate& update) {
    if (!g_prefs.notifications || update.source == ai::UsageSource::LastKnown) return;
    if (update.usage.fullSnapshot) {
        for (size_t i = 0; i < entry.crossings.size(); ++i)
            if (entry.quota.value.used[i] < 0) entry.crossings[i] = {};
    }
    if (!update.usage.Available()) return;
    std::wstring message;
    for (const auto& row : View(entry).rows) {
        if (row.stale || row.used < 0 || update.usage.used[row.limit] != row.used) continue;
        if (!entry.crossings[row.limit].Observe(row.used, row.reset, g_prefs)) continue;
        if (!message.empty()) message += L"\n";
        message += std::wstring(row.label) + L" reached " + std::to_wstring(row.used) + L"%";
    }
    if (message.empty() || IsHidden(entry.key)) return;
    // The shell needs a tray icon to host a balloon even in taskbar-widget mode.
    if (!entry.added && !g_balloonIcon) {
        TrayCall(NIM_ADD, BALLOON_ID, LoadIconW(g_inst, MAKEINTRESOURCEW(IDI_APP)), kAppName);
        g_balloonIcon = true;
    }
    NOTIFYICONDATAW data{sizeof(data)};
    data.hWnd = g_hwnd;
    data.uID = entry.added ? entry.uid : BALLOON_ID;
    data.uFlags = NIF_INFO;
    data.dwInfoFlags = NIIF_WARNING | NIIF_RESPECT_QUIET_TIME;
    CopyTip(data.szInfoTitle, ARRAYSIZE(data.szInfoTitle), std::wstring(ai::Name(entry.provider)) + L" usage");
    CopyTip(data.szInfo, ARRAYSIZE(data.szInfo), message);
    if (!Shell_NotifyIconW(NIM_MODIFY, &data) && data.uID == BALLOON_ID) {
        TrayDelete(BALLOON_ID);
        g_balloonIcon = false;
    }
}

void ScheduleExpiry() {
    g_nextReset = 0;
    for (const auto& entry : g_usage)
        for (auto reset : entry.quota.value.resetsAt)
            if (reset > 0 && (!g_nextReset || reset < g_nextReset)) g_nextReset = reset;
}

void ExpireReadings() {
    auto now = std::time(nullptr);
    if (!g_nextReset || now < g_nextReset) return;
    for (auto& entry : g_usage) {
        if (!entry.quota.Expire(now)) continue;
        if (entry.connection == ai::Connection::Ready && !entry.quota.value.Available())
            entry.connection = ai::Connection::Waiting;
        ApplyUsage(entry);
    }
    ScheduleExpiry();
    UpdatePlaceholder();
    UpdateWidget();
}

void ShowUsageUpdate(size_t provider, const ai::UsageUpdate& update) {
    auto& entry = g_usage[provider];
    bool changed = entry.quota.Apply(update);
    if (update.source != ai::UsageSource::LastKnown && update.observed >= entry.connectionObserved) {
        auto state = update.usage.Available() ? ai::Connection::Ready : update.connection;
        if (entry.connection == ai::Connection::Ready && state == ai::Connection::Waiting)
            state = ai::Connection::Ready;
        changed |= entry.connection != state;
        entry.connection = state;
        entry.connectionObserved = update.observed;
    }
    if (!changed) return;
    ScheduleExpiry();
    if (g_nextReset && g_nextReset <= std::time(nullptr)) ExpireReadings();
    NotifyCrossings(entry, update);
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
        if (IsHidden(entry.key)) {
            if (WaitForMultipleObjects(2, events, FALSE, INFINITE) != WAIT_OBJECT_0 + 1) break;
            continue;
        }
        ULONGLONG start = GetTickCount64();
        const auto observed = ai::Now();
        auto reading = ai::Poll(entry.provider, g_stopEvt);
        PublishUsage(index, {reading.usage, observed, ai::UsageSource::Account, reading.connection});
        if (reading.usage.Available()) startupAttempts = ARRAYSIZE(codexRetryDelays);
        const auto elapsed = GetTickCount64() - start;
        DWORD delay = elapsed < USAGE_INTERVAL_MS ? USAGE_INTERVAL_MS - (DWORD)elapsed : 0;
        if (entry.provider == ai::Provider::Codex && reading.usage.installed &&
            reading.connection != ai::Connection::SignIn &&
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
        g_balloonIcon = false;
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
            if (!usage.Available() || usage.used[ai::FiveHours] < -1 || usage.used[ai::FiveHours] > 100 ||
                usage.used[ai::Week] < -1 || usage.used[ai::Week] > 100) return FALSE;
            ShowUsageUpdate(0, {usage, ai::Now(), ai::UsageSource::Live});
            return TRUE;
        }
        case WM_TRAY:
            switch (LOWORD(lp)) {
                case NIN_BALLOONHIDE: case NIN_BALLOONTIMEOUT: case NIN_BALLOONUSERCLICK:
                    if (HIWORD(lp) == BALLOON_ID && g_balloonIcon) {
                        TrayDelete(BALLOON_ID);
                        g_balloonIcon = false;
                    }
                    break;
                case WM_MOUSEMOVE:
                    for (auto& entry : g_usage)
                        if (entry.added && entry.uid == HIWORD(lp) && entry.trayTipMinute != std::time(nullptr)/60)
                            ApplyUsage(entry);
                    break;
                case WM_CONTEXTMENU:
                case NIN_SELECT:
                case NIN_KEYSELECT: {
                    NOTIFYICONIDENTIFIER id{sizeof(id)};
                    id.hWnd = h;
                    id.uID = HIWORD(lp);
                    RECT bounds{};
                    if (FAILED(Shell_NotifyIconGetRect(&id, &bounds))) {
                        POINT cursor{};
                        GetCursorPos(&cursor);
                        bounds = {cursor.x, cursor.y, cursor.x+1, cursor.y+1};
                    }
                    OpenPanel(bounds, FindWindowW(L"Shell_TrayWnd", nullptr));
                    break;
                }
            }
            return 0;
        case WM_WIDGET_CLICK: {
            auto anchor = reinterpret_cast<HWND>(wp);
            RECT bounds{};
            if (GetWindowRect(anchor, &bounds)) OpenPanel(bounds, anchor);
            return 0;
        }
        case widget::WM_USAGE_REFRESH:
            RefreshUsage();
            return 0;
        case widget::WM_USAGE_SETTINGS: {
            HWND panel = reinterpret_cast<HWND>(wp);
            if (!g_settingsDialog && widget::UsagePanelOpen() && GetWindow(panel, GW_OWNER) == h) {
                if (DialogBoxParamW(g_inst, MAKEINTRESOURCEW(IDD_SETTINGS), panel, SettingsProc, 0) == ID_EXIT)
                    PostMessageW(h, WM_CLOSE, 0, 0);
            }
            return 0;
        }
        case WM_ENDSESSION:  // sign-out / shutdown / installer restart manager
            if (wp) DestroyWindow(h);
            return 0;
        case WM_POWERBROADCAST:
            if (wp == PBT_APMRESUMEAUTOMATIC) RefreshUsage();
            return TRUE;
        case WM_TIMER:
            if (wp == TIMER_WIDGET) { ExpireReadings(); Reconcile();
                bool wasBusy = updates::Busy();
                updates::Tick(g_settingsDialog ? g_settingsDialog : h);
                if (g_settingsDialog && wasBusy != updates::Busy()) SyncSettings(g_settingsDialog); }
            return 0;
        case WM_SETTINGCHANGE:
            if (lp && !lstrcmpW((LPCWSTR)lp, L"ImmersiveColorSet")) {
                bool light = ReadLightTaskbar();
                if (light != g_lightTaskbar) { g_lightTaskbar = light; widget::SetLightTheme(light); }
            }
            return 0;
        case WM_DESTROY:
            KillTimer(h, TIMER_WIDGET);
            if (g_settingsDialog) EndDialog(g_settingsDialog, IDCANCEL);
            widget::HideUsageTip();
            updates::Shutdown();
            widget::Destroy();
            SetEvent(g_stopEvt);
            if (g_codexWatcher) WaitForSingleObject(g_codexWatcher, 2000);
            for (const auto& entry : g_usage)
                if (entry.worker) WaitForSingleObject(entry.worker, 2000);
            for (const auto& entry : g_usage)
                if (entry.added) TrayDelete(entry.uid);
            if (g_placeholder) TrayDelete(PLACEHOLDER_ID);
            if (g_balloonIcon) TrayDelete(BALLOON_ID);
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR command, int) {
    if (wcsncmp(command, L"--apply-update", 14) == 0) {
        int count = 0;
        auto args = CommandLineToArgvW(GetCommandLineW(), &count);
        if (!args) return 2;
        int result = updates::RunApplyMode(count, args);
        LocalFree(args);
        return result < 0 ? 2 : result;
    }
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
            for (int i = 0; i < 150 && IsWindow(window); ++i) Sleep(100);
            if (IsWindow(window)) return 1;
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

    LoadPreferences();
    g_wantWidget = LoadWidgetMode();
    g_widgetBackground = LoadWidgetBackground();
    widget::SetBackground(g_widgetBackground);
    for (auto& entry : g_usage) {
        entry.quota.value.installed = ai::IsInstalled(entry.provider);
        entry.connection = entry.quota.value.installed ? ai::Connection::Waiting : ai::Connection::Missing;
    }
    Reconcile();
    ReAddAll();
    updates::Initialize();
    SetTimer(g_hwnd, TIMER_WIDGET, 1000, nullptr);
    g_codexWatcher = CreateThread(nullptr, 0, CodexWatcher, nullptr, 0, nullptr);
    for (auto& entry : g_usage) {
        if (!IsHidden(entry.key) && (entry.provider != ai::Provider::Gemini || entry.quota.value.installed))
            StartWorker(entry);
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
