#include "../src/main.cpp"
#include "check.h"
#include <cstdio>

// Supply mouse packets without moving the user's pointer or clicking on their desktop.
namespace inputTest {
USHORT flags;
HWND target;
UINT WINAPI Read(HRAWINPUT, UINT, LPVOID buffer, PUINT size, UINT) {
    if (*size < sizeof(RAWINPUT)) return UINT(-1);
    auto& input = *static_cast<RAWINPUT*>(buffer);
    input.header.dwType = RIM_TYPEMOUSE;
    input.data.mouse.usButtonFlags = flags;
    return sizeof(input);
}
HWND WINAPI HitTest(POINT) { return target; }
}
#define GetRawInputData inputTest::Read
#define WindowFromPoint inputTest::HitTest
#include "../src/usage_tip.cpp"
#undef WindowFromPoint
#undef GetRawInputData

namespace {
bool requestedRefresh;
bool runRefresh;
void Pump();
LRESULT CALLBACK Owner(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == widget::WM_USAGE_REFRESH) {
        requestedRefresh = true;
        if (runRefresh) return WndProc(h, msg, wp, lp);
        return 0;
    }
    if (msg == WM_WIDGET_CLICK || msg == WM_TRAY || msg == widget::WM_USAGE_SETTINGS ||
        msg == widget::WM_USAGE_SETTINGS_CLICK)
        return WndProc(h, msg, wp, lp);
    return DefWindowProcW(h, msg, wp, lp);
}

bool checkedSettings;
ULONGLONG openDeadline;
HWND outsideSettings;
void CALLBACK InspectSettings(HWND, UINT, UINT_PTR timer, DWORD) {
    if ((!g_settingsDialog || !IsWindowVisible(g_settingsDialog)) && GetTickCount64() < openDeadline) return;
    KillTimer(nullptr, timer);
    CHECK(g_settingsDialog && IsWindowVisible(g_settingsDialog));
    auto parent = GetWindow(g_settingsDialog, GW_OWNER);
    CHECK(widget::UsagePanelOpen() && !IsWindowEnabled(parent));
    CHECK(SendDlgItemMessageW(g_settingsDialog, ID_DISPLAY, CB_GETCOUNT, 0, 0) == 3);
    CHECK(GetDlgItem(g_settingsDialog, ID_HOVER_TOOLTIP));
    CHECK(GetDlgItem(g_settingsDialog, ID_RESET) && GetDlgItem(g_settingsDialog, ID_RESET+2));
    RECT limits{}, settings{};
    GetWindowRect(parent, &limits);
    GetWindowRect(g_settingsDialog, &settings);
    RECT overlap{};
    CHECK(IntersectRect(&overlap, &limits, &settings));
    CHECK(GetDlgItem(parent, 2)); // Refresh is available directly on the limits panel.
    CHECK(GetDlgItem(g_settingsDialog, ID_USAGE_FIRST+2) && GetDlgItem(g_settingsDialog, ID_EXIT));
    HWND dialog = g_settingsDialog;
    auto mouse = [&](USHORT flags, HWND target) {
        inputTest::flags = flags;
        inputTest::target = target;
        SendMessageW(parent, WM_INPUT, RIM_INPUTSINK, 0);
        Pump();
    };
    mouse(0, outsideSettings);
    CHECK(g_settingsDialog == dialog);
    mouse(RI_MOUSE_LEFT_BUTTON_DOWN, GetDlgItem(dialog, ID_HOVER_TOOLTIP));
    CHECK(g_settingsDialog == dialog);
    auto combo = GetDlgItem(dialog, ID_DISPLAY);
    SendMessageW(combo, CB_SHOWDROPDOWN, TRUE, 0);
    COMBOBOXINFO info{sizeof(info)};
    CHECK(GetComboBoxInfo(combo, &info) && IsWindowVisible(info.hwndList));
    mouse(RI_MOUSE_LEFT_BUTTON_DOWN, info.hwndList);
    CHECK(g_settingsDialog == dialog);
    SendMessageW(combo, CB_SHOWDROPDOWN, FALSE, 0);
    checkedSettings = true;
    inputTest::flags = RI_MOUSE_LEFT_BUTTON_DOWN;
    inputTest::target = outsideSettings;
    SendMessageW(parent, WM_INPUT, RIM_INPUTSINK, 0);
}
void Pump() {
    MSG msg{};
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&msg);
}

void CheckRefresh(HWND anchor) {
    g_useWidget = true;
    for (auto& entry : g_usage) {
        entry.quota.value.installed = true;
        entry.connection = ai::Connection::Unavailable;
        CHECK(ConnectionHint(entry).find(L"CLI update") != std::wstring::npos);
        entry.refresh = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        CHECK(entry.refresh);
        entry.worker = entry.refresh; // Use the actual worker wake event without running provider CLIs.
    }
    SendMessageW(g_hwnd, WM_WIDGET_CLICK, reinterpret_cast<WPARAM>(anchor), 0);
    auto panel = FindWindowW(L"AIUsageMonitorQuotaTip", nullptr);
    auto refresh = GetDlgItem(panel, widget::refreshId);
    CHECK(refresh && IsWindowEnabled(refresh));
    runRefresh = true;
    SendMessageW(refresh, BM_CLICK, 0, 0);
    Pump();
    CHECK(!IsWindowEnabled(refresh));
    for (const auto& entry : g_usage) {
        CHECK(entry.pendingRefresh && WaitForSingleObject(entry.refresh, 0) == WAIT_OBJECT_0);
    }
    RefreshUsage(true);
    for (const auto& entry : g_usage) CHECK(WaitForSingleObject(entry.refresh, 0) == WAIT_TIMEOUT);

    ai::UsageUpdate failure{{-1, -1, true}, ai::Now(), ai::UsageSource::Account, ai::Connection::Unavailable};
    auto ticket = g_usage[0].pendingRefresh;
    ShowUsageUpdate(0, failure, 0);
    CHECK(g_usage[0].pendingRefresh == ticket);
    ShowUsageUpdate(0, failure, ticket);
    CHECK(!g_usage[0].pendingRefresh && g_usage[1].pendingRefresh && g_usage[2].pendingRefresh);
    CHECK(!IsWindowEnabled(refresh));
    CHECK(ControlText(panel).find(L"Claude - Refreshing") == std::wstring::npos);
    CHECK(ControlText(panel).find(L"Codex - Refreshing") != std::wstring::npos);
    for (size_t i = 1; i < g_usage.size(); ++i) ShowUsageUpdate(i, failure, g_usage[i].pendingRefresh);
    CHECK(IsWindowEnabled(refresh) && ControlText(panel).find(L"Refreshing") == std::wstring::npos);

    runRefresh = false;
    widget::HideUsageTip();
    for (auto& entry : g_usage) {
        CloseHandle(entry.refresh);
        entry.refresh = entry.worker = nullptr;
    }
    g_useWidget = false;
}
}

int main() {
    auto original = GetThreadDesktop(GetCurrentThreadId());
    auto name = L"AIUsagePanelTest" + std::to_wstring(GetCurrentProcessId());
    HDESK desktop = CreateDesktopW(name.c_str(), nullptr, nullptr, 0, GENERIC_ALL, nullptr);
    CHECK(desktop && SetThreadDesktop(desktop));
    g_inst = GetModuleHandleW(nullptr);
    WNDCLASSW wc{};
    wc.lpfnWndProc = Owner;
    wc.hInstance = g_inst;
    wc.lpszClassName = L"UsageTestOwner";
    CHECK(RegisterClassW(&wc));
    g_hwnd = CreateWindowW(wc.lpszClassName, L"", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, g_inst, nullptr);
    auto anchor = CreateWindowW(L"STATIC", L"", WS_POPUP, 200, 620, 420, 40, nullptr, nullptr, g_inst, nullptr);
    CHECK(g_hwnd && anchor);
    RECT bounds{};
    GetWindowRect(anchor, &bounds);
    ai::UsageState example;
    example.value = {7, 28, true};
    example.value.used[ai::ArgonWeek] = 0;
    std::vector<ui::ProviderView> rows{ui::Describe(ai::Provider::Gemini, IDI_GEMINI, example, g_prefs)};
    widget::ShowUsageTip(g_hwnd, bounds, 96, false, rows, g_prefs, false);
    CHECK(widget::UsageTipOpen() && !widget::UsagePanelOpen());
    CHECK(!GetDlgItem(FindWindowW(L"AIUsageMonitorQuotaTip", nullptr), widget::exitId));
    g_prefs.hoverTooltip = false;
    widget::RefreshUsageTip(false, rows, g_prefs);
    CHECK(!widget::UsageTipOpen());
    widget::ShowUsageTip(g_hwnd, bounds, 96, false, rows, g_prefs, false);
    CHECK(!widget::UsageTipOpen());

    CheckRefresh(anchor);

    DWORD gdi = 0, user = 0;
    for (int i = 0; i < 8; ++i) {
        SendMessageW(g_hwnd, WM_WIDGET_CLICK, reinterpret_cast<WPARAM>(anchor), 0);
        widget::HideHoverTip();
        widget::RefreshUsageTip(false, rows, g_prefs);
        HWND panel = FindWindowW(L"AIUsageMonitorQuotaTip", nullptr);
        CHECK(panel && widget::UsagePanelOpen());
        HWND gear = FindWindowExW(panel, nullptr, L"BUTTON", L"Settings");
        CHECK(gear);
        requestedRefresh = false;
        SendMessageW(GetDlgItem(panel, 2), BM_CLICK, 0, 0);
        Pump();
        CHECK(requestedRefresh && widget::UsagePanelOpen());
        openDeadline = GetTickCount64() + 5000;
        CHECK(SetTimer(nullptr, 0, 30, InspectSettings));
        checkedSettings = false;
        outsideSettings = i % 2 == 0 ? panel : anchor;
        SendMessageW(gear, BM_CLICK, 0, 0);
        CHECK(checkedSettings && !g_settingsDialog && IsWindowEnabled(panel));

        inputTest::flags = 0;
        inputTest::target = anchor;
        SendMessageW(panel, WM_INPUT, RIM_INPUTSINK, 0);
        Pump();
        CHECK(widget::UsagePanelOpen());
        inputTest::flags = RI_MOUSE_LEFT_BUTTON_DOWN;
        inputTest::target = gear;
        SendMessageW(panel, WM_INPUT, RIM_INPUTSINK, 0);
        Pump();
        CHECK(widget::UsagePanelOpen());
        inputTest::target = anchor;
        SendMessageW(panel, WM_INPUT, RIM_INPUTSINK, 0);
        Pump();
        CHECK(!widget::UsageTipOpen());
        auto process = GetCurrentProcess();
        if (i == 1) {
            gdi = GetGuiResources(process, GR_GDIOBJECTS);
            user = GetGuiResources(process, GR_USEROBJECTS);
        } else if (i == 7) {
            CHECK(GetGuiResources(process, GR_GDIOBJECTS) <= gdi);
            CHECK(GetGuiResources(process, GR_USEROBJECTS) <= user);
        }
    }
    SendMessageW(g_hwnd, WM_TRAY, 0, MAKELPARAM(NIN_KEYSELECT, PLACEHOLDER_ID));
    CHECK(widget::UsagePanelOpen());
    auto panel = FindWindowW(L"AIUsageMonitorQuotaTip", nullptr);
    auto exitButton = FindWindowExW(panel, nullptr, L"BUTTON", L"Exit monitor");
    CHECK(exitButton);
    SendMessageW(exitButton, BM_CLICK, 0, 0);
    Pump();
    CHECK(!IsWindow(g_hwnd) && !widget::UsageTipOpen());
    DestroyWindow(anchor);
    CHECK(SetThreadDesktop(original) && CloseDesktop(desktop));
    puts("Hover, persistent panel, gear modal, outside clicks, tray activation and exit passed");
}
