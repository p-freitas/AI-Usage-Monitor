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
LRESULT CALLBACK Owner(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == widget::WM_USAGE_REFRESH) {
        requestedRefresh = true;
        return 0;
    }
    if (msg == WM_WIDGET_CLICK || msg == WM_TRAY || msg == widget::WM_USAGE_SETTINGS)
        return WndProc(h, msg, wp, lp);
    return DefWindowProcW(h, msg, wp, lp);
}

bool checkedSettings;
ULONGLONG openDeadline;
void CALLBACK InspectSettings(HWND, UINT, UINT_PTR timer, DWORD) {
    if ((!g_settingsDialog || !IsWindowVisible(g_settingsDialog)) && GetTickCount64() < openDeadline) return;
    KillTimer(nullptr, timer);
    CHECK(g_settingsDialog && IsWindowVisible(g_settingsDialog));
    auto parent = GetWindow(g_settingsDialog, GW_OWNER);
    CHECK(widget::UsagePanelOpen() && !IsWindowEnabled(parent));
    CHECK(SendDlgItemMessageW(g_settingsDialog, ID_DISPLAY, CB_GETCOUNT, 0, 0) == 3);
    CHECK(GetDlgItem(g_settingsDialog, ID_RESET) && GetDlgItem(g_settingsDialog, ID_RESET+2));
    RECT limits{}, settings{};
    GetWindowRect(parent, &limits);
    GetWindowRect(g_settingsDialog, &settings);
    RECT overlap{};
    CHECK(IntersectRect(&overlap, &limits, &settings));
    CHECK(GetDlgItem(parent, 2)); // Refresh is available directly on the limits panel.
    CHECK(GetDlgItem(g_settingsDialog, ID_USAGE_FIRST+2) && GetDlgItem(g_settingsDialog, ID_EXIT));
    checkedSettings = true;
    SendMessageW(g_settingsDialog, WM_COMMAND, IDOK, 0);
}
void Pump() {
    MSG msg{};
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&msg);
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
    widget::HideHoverTip();
    CHECK(!widget::UsageTipOpen());

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
