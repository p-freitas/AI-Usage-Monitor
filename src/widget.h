#pragma once
#include <windows.h>
#include "presentation.h"

namespace widget {
// Creates the widget on the primary taskbar. Clicks are reported by posting `clickMsg` to `notifyWnd`.
bool Create(HINSTANCE inst, HWND notifyWnd, UINT clickMsg, bool lightTheme);
void Destroy();
void OpenPanel(HWND owner, const RECT& anchor, int dpi, bool light);
bool Alive();
bool FitsTaskbar();
void SetItems(std::vector<ui::ProviderView> items, const ui::Preferences& prefs);
void SetLightTheme(bool light);
// Draws a rounded plate in the taskbar's colours behind the text (kept across Create/Destroy).
void SetBackground(bool on);
// Re-checks the taskbar layout and moves the widget if needed; call about once a second.
// Returns false when the taskbar it was attached to is gone.
bool Reposition();
}
