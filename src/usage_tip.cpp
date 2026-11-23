#include "usage_tip.h"
#include "chrome.h"
#include "../res/resource.h"
#include <algorithm>
#include <ctime>

namespace widget {
namespace {
HWND tip, gear, refresh, exitButton;
RECT anchorBounds;
int anchorDpi;
bool pinned, inSettings, mouseRegistered;
constexpr UINT gearId = 1;
constexpr UINT refreshId = 2;
constexpr UINT exitId = 3;
constexpr int panelWidth = 414, headerHeight = 36, padding = 7;
constexpr int cardHeading = 28, rowHeight = 34, cardBottom = 4, statusHeight = 15;
bool tailBelow;
int tailX;
constexpr UINT checkActivation = WM_APP + 1;
const std::vector<ui::ProviderView>* views;
ui::Preferences settings;
bool lightTheme;
int scale;
HFONT font, heading, title, symbols, value, glyphs;
constexpr wchar_t className[] = L"AIUsageMonitorQuotaTip";
int Px(int n) { return MulDiv(n, scale, 96); }

bool ShowCard(const ui::ProviderView& provider, bool pin) {
    return pin || !provider.rows.empty();
}

int CardRows(const ui::ProviderView& provider, bool pin) {
    return std::max(pin && provider.refreshing ? 1 : 0, static_cast<int>(provider.rows.size()));
}

int CardHeight(const ui::ProviderView& provider) {
    return Px(cardHeading) + CardRows(provider, pinned)*Px(rowHeight) + Px(cardBottom) +
           (provider.status.empty() ? 0 : Px(statusHeight));
}

COLORREF Accent(ai::Provider provider) {
    if (provider == ai::Provider::Claude) return RGB(245,139,88);
    if (provider == ai::Provider::Codex) return RGB(64,145,250);
    return RGB(166,107,242);
}

std::wstring UsedValue(const ui::Row& row) {
    if (row.used < 0) return L"\u2014";
    auto text = std::to_wstring(row.used) + L"%";
    if (row.severity == ui::Severity::Exhausted) text += L"!";
    return text;
}

void Draw(HDC dc, RECT client) {
    const auto colors = ui::Colors(lightTheme);
    const int tail = pinned ? Px(9) : 0;
    const int panelTop = tailBelow ? 0 : tail;
    const int panelBottom = client.bottom - (tailBelow ? tail : 0);
    ui::Fill(dc, client, colors.background);
    if (tail) {
        int edge = tailBelow ? panelBottom-1 : panelTop;
        POINT points[] = {{tailX-Px(9), edge}, {tailX, tailBelow ? client.bottom-1 : 0},
                          {tailX+Px(9), edge}};
        auto pen = SelectObject(dc, GetStockObject(DC_PEN));
        SetDCPenColor(dc, colors.border);
        Polyline(dc, points, 3);
        SelectObject(dc, pen);
    }
    ui::Rounded(dc, {0,panelTop,client.right,panelBottom}, Px(10), colors.background, colors.border);
    ui::Fill(dc, {1,panelTop+Px(10),client.right-1,panelTop+Px(headerHeight)}, colors.header);
    ui::Rounded(dc, {0,panelTop,client.right,panelTop+Px(headerHeight+10)}, Px(10), colors.header, colors.border);
    ui::Fill(dc, {1,panelTop+Px(headerHeight),client.right-1,panelTop+Px(headerHeight+10)}, colors.background);
    ui::Fill(dc, {1,panelTop+Px(headerHeight)-1,client.right-1,panelTop+Px(headerHeight)}, colors.border);
    // Bar-chart mark: three rounded columns of different heights, bottom aligned.
    const int markBase = panelTop+Px(headerHeight/2+7);
    for (int i=0; i<3; ++i) {
        int tall = i == 0 ? 8 : i == 1 ? 14 : 11;
        RECT bar{Px(14+i*5), markBase-Px(tall), Px(17+i*5), markBase};
        ui::Rounded(dc, bar, Px(2), colors.muted, colors.muted);
    }
    ui::Text(dc, {Px(36),panelTop,client.right-Px(pinned ? 112 : 12),panelTop+Px(headerHeight)},
             L"Usage limits", title, colors.text);
    int left = Px(padding), right = client.right-left;
    int y = panelTop+Px(headerHeight)+left;
    const int contentTop = y;
    auto now = std::time(nullptr);
    for (const auto& provider : *views) {
        if (!ShowCard(provider, pinned)) continue;
        auto accent = Accent(provider.provider);
        auto tintStart = ui::Mix(colors.background, accent, lightTheme ? 12 : 17);
        auto tintEnd = ui::Mix(colors.background, accent, lightTheme ? 3 : 5);
        auto border = ui::Mix(colors.border, accent, 50);
        int bottom = y+CardHeight(provider);
        ui::Gradient(dc, {left,y,right,bottom}, Px(8), tintStart, tintEnd, border);
        ui::Icon(dc, provider.icon, left+Px(10), y+Px(6), Px(18));
        ui::Text(dc, {left+Px(34), y+Px(2), right-Px(10), y+Px(cardHeading)},
                 ai::Name(provider.provider), value, colors.text);
        y += Px(cardHeading);

        if (pinned && provider.refreshing) {
            ui::Text(dc, {left+Px(125),y-Px(cardHeading),right-Px(10),y},
                     L"Refreshing...", font, colors.muted, DT_RIGHT);
            const auto placeholder = ui::Mix(tintEnd, colors.muted, lightTheme ? 22 : 28);
            const int labelX = left+Px(10), numberX = left+Px(105), barX = left+Px(165);
            for (int i = 0; i < CardRows(provider, pinned); ++i) {
                ui::Rounded(dc, {labelX,y+Px(5),numberX-Px(12),y+Px(19)}, Px(4), placeholder, placeholder);
                ui::Rounded(dc, {numberX,y+Px(6),barX-Px(18),y+Px(16)}, Px(4), placeholder, placeholder);
                ui::Rounded(dc, {barX,y+Px(8),right-Px(10),y+Px(14)}, Px(3), placeholder, placeholder);
                ui::Rounded(dc, {numberX,y+Px(23),numberX+Px(121),y+Px(29)}, Px(3), placeholder, placeholder);
                y += Px(rowHeight);
            }
            y = bottom+left;
            continue;
        }

        // Columns follow the content of this card, so long labels such as "Gemini · 7 days"
        // push the values right instead of being clipped.
        int badgeWidth = 0, valueWidth = 0;
        for (const auto& row : provider.rows) {
            badgeWidth = std::max(badgeWidth, ui::Measure(dc, row.label, heading));
            int used = ui::Measure(dc, UsedValue(row), value);
            if (row.used >= 0) used += Px(4) + ui::Measure(dc, L"used", font);
            valueWidth = std::max(valueWidth, used);
        }
        badgeWidth = std::max(Px(54), badgeWidth + Px(12));
        int labelX = left+Px(10);
        int valueX = labelX+badgeWidth+Px(10);
        int barX = std::min(valueX+valueWidth+Px(10), right-Px(80));
        auto badgeFill = lightTheme ? colors.surface : ui::Mix(colors.surface, accent, 10);
        auto badgeBorder = ui::Mix(colors.border, colors.text, lightTheme ? 5 : 22);
        auto divider = ui::Mix(tintEnd, colors.border, 70);
        auto track = lightTheme ? ui::Mix(colors.header, colors.border, 45)
                                : ui::Mix(colors.background, colors.border, 55);
        for (size_t i=0; i<provider.rows.size(); ++i) {
            const auto& row = provider.rows[i];
            if (i) ui::Fill(dc, {labelX,y,right-Px(10),y+1}, divider);
            RECT badge{labelX,y+Px(4),labelX+badgeWidth,y+Px(21)};
            ui::Rounded(dc, badge, Px(5), badgeFill, badgeBorder);
            ui::Text(dc, badge, row.label, heading, colors.text, DT_CENTER);
            const auto level = settings.colors ? row.severity : ui::Severity::Normal;
            auto ink = row.stale ? colors.muted : UsageColor(level, lightTheme, colors.text);
            RECT number{valueX,y+Px(2),barX,y+Px(20)};
            int usedWidth = ui::Text(dc, number, UsedValue(row), value, ink);
            number.left += usedWidth+Px(4);
            if (row.used >= 0) ui::Text(dc, number, L"used", font, row.stale ? colors.muted : colors.text);
            RECT bar{barX,y+Px(8),right-Px(10),y+Px(14)};
            ui::Rounded(dc, bar, Px(3), track, track);
            if (row.used > 0) {
                bar.right = bar.left+std::max(Px(6), MulDiv(bar.right-bar.left, std::min(row.used, 100), 100));
                auto fill = UsageColor(level, lightTheme, accent);
                if (row.stale) fill = ui::Mix(track, fill, 55);
                ui::Rounded(dc, bar, Px(3), fill, fill);
            }
            auto reset = ui::ResetText(row.reset, now, settings.reset);
            if (row.stale) reset += L"  \u00b7  Last known";
            ui::Text(dc, {valueX,y+Px(19),right-Px(8),y+Px(33)}, reset, font, colors.muted);
            y += Px(rowHeight);
        }
        if (!provider.status.empty())
            ui::Text(dc, {left+Px(10),y,right-Px(10),y+Px(statusHeight)}, provider.status, font, colors.muted);
        y = bottom+left;
    }
    if (y == contentTop)
        ui::Text(dc, {left,y,right,y+Px(20)}, L"No visible quota readings", font, colors.muted);
}

void Shape(HWND window, int width, int height) {
    int tail = pinned ? Px(9) : 0;
    auto region = CreateRoundRectRgn(0, tailBelow ? 0 : tail, width+1,
                                    height-(tailBelow ? tail : 0)+1, Px(20), Px(20));
    if (!region) return;
    if (tail) {
        int base = tailBelow ? height-tail-1 : tail+1;
        POINT arrow[] = {{tailX-Px(11),base}, {tailX,tailBelow ? height+1 : -1}, {tailX+Px(11),base}};
        auto point = CreatePolygonRgn(arrow, 3, WINDING);
        if (point) { CombineRgn(region, region, point, RGN_OR); DeleteObject(point); }
    }
    if (!SetWindowRgn(window, region, TRUE)) DeleteObject(region);
}

LRESULT CALLBACK TipProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_MOUSEACTIVATE:
            return pinned ? MA_ACTIVATE : MA_NOACTIVATE;
        case WM_NCHITTEST:
            if (!pinned) return HTTRANSPARENT;
            break;
        case WM_INPUT: {
            // No capture or hook: the original click still goes to the window underneath.
            RAWINPUT input{};
            UINT size = sizeof(input);
            if (GetRawInputData(reinterpret_cast<HRAWINPUT>(lp), RID_INPUT, &input,
                                &size, sizeof(RAWINPUTHEADER)) == sizeof(input) &&
                input.header.dwType == RIM_TYPEMOUSE &&
                (input.data.mouse.usButtonFlags & (RI_MOUSE_LEFT_BUTTON_DOWN | RI_MOUSE_RIGHT_BUTTON_DOWN |
                    RI_MOUSE_MIDDLE_BUTTON_DOWN | RI_MOUSE_BUTTON_4_DOWN | RI_MOUSE_BUTTON_5_DOWN))) {
                DWORD pos = GetMessagePos();
                POINT pt{static_cast<short>(LOWORD(pos)), static_cast<short>(HIWORD(pos))};
                HWND target = WindowFromPoint(pt);
                if (inSettings)
                    SendMessageW(GetWindow(h, GW_OWNER), WM_USAGE_SETTINGS_CLICK, reinterpret_cast<WPARAM>(target), 0);
                else if (target != h && !IsChild(h, target))
                    PostMessageW(h, WM_CLOSE, 0, 0);
            }
            break;
        }
        case WM_ACTIVATE:
            // Fallback for systems where raw mouse registration was unavailable.
            if (!mouseRegistered && pinned && LOWORD(wp) == WA_INACTIVE && !inSettings)
                PostMessageW(h, checkActivation, 0, 0);
            break;
        case checkActivation: {
            HWND foreground = GetForegroundWindow();
            if (pinned && !inSettings && IsWindowEnabled(h) && foreground != h &&
                (!foreground || GetAncestor(foreground, GA_ROOTOWNER) != GetAncestor(h, GA_ROOTOWNER)))
                DestroyWindow(h);
            return 0;
        }
        case WM_COMMAND:
            if (LOWORD(wp) == exitId && pinned && !inSettings) {
                PostMessageW(GetWindow(h, GW_OWNER), WM_CLOSE, 0, 0);
                return 0;
            }
            if (LOWORD(wp) == refreshId && pinned) {
                PostMessageW(GetWindow(h, GW_OWNER), WM_USAGE_REFRESH, 0, 0);
                return 0;
            }
            if (LOWORD(wp) == gearId && pinned && !inSettings) {
                inSettings = true;
                SendMessageW(GetWindow(h, GW_OWNER), WM_USAGE_SETTINGS, reinterpret_cast<WPARAM>(h), mouseRegistered);
                inSettings = false;
            }
            return 0;
        case WM_DRAWITEM: {
            auto* item = reinterpret_cast<DRAWITEMSTRUCT*>(lp);
            const wchar_t* icon;
            switch (item->CtlID) {
                case gearId: icon = ui::glyph::Settings; break;
                case refreshId: icon = ui::glyph::Sync; break;
                case exitId: icon = ui::glyph::Power; break;
                default: return DefWindowProcW(h, msg, wp, lp);
            }
            auto dc = item->hDC;
            int saved = SaveDC(dc);
            auto colors = ui::Colors(lightTheme);
            bool pressed = (item->itemState & ODS_SELECTED) != 0;
            bool disabled = (item->itemState & ODS_DISABLED) != 0;
            ui::Fill(dc, item->rcItem, colors.header);
            auto face = pressed ? colors.selected : ui::Mix(colors.header, colors.surface, 60);
            ui::Rounded(dc, item->rcItem, Px(8), face, ui::Mix(colors.border, colors.text, lightTheme ? 0 : 12));
            ui::Text(dc, item->rcItem, icon, symbols, disabled ? colors.muted : colors.text, DT_CENTER);
            if ((item->itemState & ODS_FOCUS) && !(item->itemState & ODS_NOFOCUSRECT)) {
                auto focus = item->rcItem;
                InflateRect(&focus, -3, -3);
                DrawFocusRect(dc, &focus);
            }
            RestoreDC(dc, saved);
            return TRUE;
        }
        case WM_ERASEBKGND: return 1;
        case WM_TIMER:
        case WM_TIMECHANGE:
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        case WM_PRINTCLIENT: {
            RECT client{};
            GetClientRect(h, &client);
            Draw(reinterpret_cast<HDC>(wp), client);
            return 0;
        }
        case WM_PAINT: {
            PAINTSTRUCT ps{};
            HDC dc = BeginPaint(h, &ps);
            RECT rc{};
            GetClientRect(h, &rc);
            int saved = SaveDC(dc);
            Draw(dc, rc);
            RestoreDC(dc, saved);
            EndPaint(h, &ps);
            return 0;
        }
        case WM_NCDESTROY:
            if (mouseRegistered) {
                RAWINPUTDEVICE mouse{1, 2, RIDEV_REMOVE, nullptr};
                RegisterRawInputDevices(&mouse, 1, sizeof(mouse));
                mouseRegistered = false;
            }
            DeleteObject(font);
            DeleteObject(heading);
            DeleteObject(title);
            DeleteObject(symbols);
            DeleteObject(value);
            DeleteObject(glyphs);
            font = heading = title = symbols = value = glyphs = nullptr;
            tip = gear = refresh = exitButton = nullptr;
            pinned = false;
            break;
    }
    return DefWindowProcW(h, msg, wp, lp);
}
} // namespace

COLORREF UsageColor(ui::Severity level, bool light, COLORREF normal) {
    if (level == ui::Severity::Warning) return light ? RGB(137,80,0) : RGB(255,203,99);
    if (level >= ui::Severity::Critical) return light ? RGB(169,31,48) : RGB(255,126,133);
    return normal;
}

void ShowUsageTip(HWND owner, const RECT& bounds, int dpi, bool light,
                  const std::vector<ui::ProviderView>& providers, const ui::Preferences& prefs, bool pin) {
    if (!pin && !prefs.hoverTooltip) {
        HideHoverTip();
        return;
    }
    MONITORINFO monitor{sizeof(monitor)};
    if (!GetMonitorInfoW(MonitorFromRect(&bounds, MONITOR_DEFAULTTONEAREST), &monitor)) return;
    if (tip && pinned && !pin) return;
    if (tip && pin != pinned) HideUsageTip();
    anchorBounds = bounds;
    anchorDpi = dpi;
    int logicalHeight = headerHeight+2*padding+(pin ? 9 : 0);
    const bool empty = std::none_of(providers.begin(), providers.end(), [pin](const auto& provider) {
        return ShowCard(provider, pin);
    });
    if (empty) logicalHeight += 20;
    for (const auto& provider : providers) {
        if (!ShowCard(provider, pin)) continue;
        logicalHeight += cardHeading + rowHeight*CardRows(provider, pin) + cardBottom +
                         padding + (provider.status.empty() ? 0 : statusHeight);
    }
    const RECT& area = monitor.rcWork;
    const int heightBudget = std::max<int>(1, area.bottom-area.top-12);
    const int widthBudget = std::max<int>(1, area.right-area.left-2);
    dpi = std::max(1, std::min({dpi, heightBudget*96/logicalHeight, widthBudget*96/panelWidth}));
    bool resizeFont = scale != dpi || !tip;
    bool opening = !tip;
    auto instance = GetModuleHandleW(nullptr);
    scale = dpi;
    lightTheme = light;
    settings = prefs;
    views = &providers;
    if (!tip) {
        static ATOM atom;
        if (!atom) {
            WNDCLASSW wc{};
            wc.lpfnWndProc = TipProc;
            wc.hInstance = instance;
            wc.lpszClassName = className;
            atom = RegisterClassW(&wc);
        }
        if (!atom) return;
        pinned = pin;
        DWORD exStyle = WS_EX_TOOLWINDOW | WS_EX_TOPMOST;
        if (!pin) exStyle |= WS_EX_NOACTIVATE | WS_EX_TRANSPARENT;
        tip = CreateWindowExW(exStyle,
                              className, L"AI Usage Monitor", WS_POPUP | WS_CLIPCHILDREN, 0, 0, 0, 0,
                              owner, nullptr, instance, nullptr);
        if (!tip) { pinned = false; return; }
        if (pin) {
            RAWINPUTDEVICE mouse{1, 2, RIDEV_INPUTSINK, tip};
            mouseRegistered = RegisterRawInputDevices(&mouse, 1, sizeof(mouse)) != FALSE;
        }
        if (pin) {
            auto button = [&](UINT id, const wchar_t* label) {
                return CreateWindowExW(0, L"BUTTON", label, WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                                       0,0,1,1,tip,reinterpret_cast<HMENU>(static_cast<UINT_PTR>(id)),instance,nullptr);
            };
            gear = button(gearId, L"Settings");
            refresh = button(refreshId, L"Refresh now");
            exitButton = button(exitId, L"Exit monitor");
        }
    }
    if (resizeFont) {
        DeleteObject(font);
        DeleteObject(heading);
        DeleteObject(title);
        DeleteObject(symbols);
        DeleteObject(value);
        DeleteObject(glyphs);
        font = ui::Font(Px(12), FW_NORMAL);
        heading = ui::Font(Px(11), FW_SEMIBOLD);
        title = ui::Font(Px(15), FW_SEMIBOLD);
        value = ui::Font(Px(14), FW_SEMIBOLD);
        symbols = ui::SymbolFont(Px(14));
        glyphs = ui::SymbolFont(Px(11));
    }
    const auto now = std::time(nullptr);
    std::wstring description;
    int height = Px(headerHeight)+2*Px(padding)+(pin ? Px(9) : 0);
    bool countdown = false;
    bool refreshing = false;
    for (const auto& provider : providers) {
        if (!ShowCard(provider, pin)) continue;
        height += CardHeight(provider)+Px(padding);
        if (!description.empty()) description += L"\n";
        if (pin && provider.refreshing) {
            description += ai::Name(provider.provider);
            description += L" - Refreshing...";
            refreshing = true;
        } else {
            description += ui::PlainText(provider, prefs.reset, now);
        }
        for (const auto& row : provider.rows) countdown |= row.reset > now;
    }
    if (empty) { height += Px(20); description = L"No visible quota readings"; }
    SetWindowTextW(tip, description.c_str());
    int width = std::min<int>(Px(panelWidth), area.right-area.left);
    height = std::min<int>(height + 2, area.bottom-area.top);
    int x = std::clamp<int>(bounds.right-width, area.left, area.right-width);
    int y = bounds.top-height-Px(4);
    tailBelow = y >= area.top || bounds.bottom+height > area.bottom;
    if (!tailBelow) y = bounds.bottom+Px(4);
    y = std::clamp<int>(y, area.top, area.bottom-height);
    tailX = std::clamp<int>((bounds.left+bounds.right)/2-x, Px(24), width-Px(24));
    SetWindowPos(tip, nullptr, x, y, width, height, SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    Shape(tip, width, height);
    if (pin) {
        if (refresh) {
            EnableWindow(refresh, !refreshing);
            SetWindowTextW(refresh, refreshing ? L"Refreshing usage" : L"Refresh now");
        }
        int buttonTop = Px(6)+(!tailBelow ? Px(9) : 0);
        int fromRight = 36;
        for (auto button : {exitButton, gear, refresh}) {
            if (!button) continue;
            SetWindowPos(button, nullptr, width-Px(fromRight), buttonTop,
                         Px(28), Px(24), SWP_NOZORDER | SWP_NOACTIVATE);
            InvalidateRect(button, nullptr, FALSE);
            fromRight += 34;
        }
    }
    if (pin && opening) { SetForegroundWindow(tip); SetFocus(gear); }
    InvalidateRect(tip, nullptr, FALSE);
    if (countdown) SetTimer(tip, 1, 60000, nullptr);
    else KillTimer(tip, 1);
}

void RefreshUsageTip(bool light, const std::vector<ui::ProviderView>& providers, const ui::Preferences& prefs) {
    if (tip) ShowUsageTip(GetWindow(tip, GW_OWNER), anchorBounds, anchorDpi, light, providers, prefs, pinned);
}
bool UsagePanelOpen() { return tip && pinned; }
void HideHoverTip() { if (!pinned) HideUsageTip(); }
void HideUsageTip() { if (tip) DestroyWindow(tip); }
bool UsageTipOpen() { return tip != nullptr; }
} // namespace widget
