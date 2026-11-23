// Exercise the same commands and registry reads used by the context menu and startup.
#include "../src/main.cpp"
#include "check.h"
#include <cstdio>

int main() {
    HKEY profile{}, software{}, settings{};
    DWORD created{};
    auto path = L"Software\\AIUsageMonitorTest-" + std::to_wstring(GetCurrentProcessId()) +
                L"-" + std::to_wstring(GetTickCount64());
    CHECK(RegCreateKeyExW(HKEY_CURRENT_USER, path.c_str(), 0, nullptr, REG_OPTION_VOLATILE,
                          KEY_ALL_ACCESS, nullptr, &profile, &created) == ERROR_SUCCESS);
    CHECK(created == REG_CREATED_NEW_KEY);
    CHECK(RegCreateKeyExW(profile, L"Software", 0, nullptr, REG_OPTION_VOLATILE,
                          KEY_ALL_ACCESS, nullptr, &software, nullptr) == ERROR_SUCCESS);
    CHECK(RegCreateKeyExW(software, L"AIUsageMonitor", 0, nullptr, REG_OPTION_VOLATILE,
                          KEY_ALL_ACCESS, nullptr, &settings, nullptr) == ERROR_SUCCESS);
    CHECK(RegOverridePredefKey(HKEY_CURRENT_USER, profile) == ERROR_SUCCESS);

    SaveBoolSetting(L"HideClaude", true);
    SaveWidgetBackground(true);
    LoadPreferences();
    CHECK(g_prefs.display == ui::Display::Complete && g_prefs.reset == ui::Reset::Relative);
    CHECK(g_prefs.colors && !g_prefs.notifications && g_prefs.warning == 80 && g_prefs.critical == 90);
    CHECK(SetPreference(ID_DISPLAY+2) && SetPreference(ID_RESET+2));
    CHECK(SetPreference(ID_WARNING+2) && SetPreference(ID_CRITICAL+3));
    CHECK(SetPreference(ID_COLORS) && SetPreference(ID_NOTIFICATIONS));
    SaveBoolSetting(L"HideGemini", true);
    g_prefs = {};
    LoadPreferences();
    CHECK(g_prefs.display == ui::Display::Weekly && g_prefs.reset == ui::Reset::Both);
    CHECK(!g_prefs.colors && g_prefs.notifications && g_prefs.warning == 75 && g_prefs.critical == 95);
    CHECK(IsHidden(L"HideClaude") && IsHidden(L"HideGemini") && !IsHidden(L"HideCodex"));
    CHECK(LoadWidgetBackground() && LoadWidgetMode());
    SaveSetting(L"CriticalThreshold", 0);
    SaveSetting(L"DisplayMode", 99);
    LoadPreferences();
    CHECK(g_prefs.display == ui::Display::Complete && g_prefs.warning == 80 && g_prefs.critical == 90);

    CHECK(RegOverridePredefKey(HKEY_CURRENT_USER, nullptr) == ERROR_SUCCESS);
    RegCloseKey(settings);
    CHECK(RegDeleteKeyW(software, L"AIUsageMonitor") == ERROR_SUCCESS);
    RegCloseKey(software);
    CHECK(RegDeleteKeyW(profile, L"Software") == ERROR_SUCCESS);
    RegCloseKey(profile);
    CHECK(RegDeleteKeyW(HKEY_CURRENT_USER, path.c_str()) == ERROR_SUCCESS);
    puts("Legacy and new menu preferences passed isolated Windows registry round-trip");
}
