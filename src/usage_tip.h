#pragma once
#include "presentation.h"

namespace widget {
COLORREF UsageColor(ui::Severity severity, bool light, COLORREF normal);
constexpr UINT WM_USAGE_SETTINGS = WM_APP + 12;
constexpr UINT WM_USAGE_REFRESH = WM_APP + 13;
constexpr UINT WM_USAGE_SETTINGS_CLICK = WM_APP + 14;
void ShowUsageTip(HWND owner, const RECT& anchor, int dpi, bool light,
                  const std::vector<ui::ProviderView>& providers, const ui::Preferences& prefs, bool pin);
void RefreshUsageTip(bool light, const std::vector<ui::ProviderView>& providers, const ui::Preferences& prefs);
void HideUsageTip();
void HideHoverTip();
bool UsageTipOpen();
bool UsagePanelOpen();
} // namespace widget
