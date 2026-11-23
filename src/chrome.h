#pragma once
#include <windows.h>
#include <algorithm>
#include <string>

namespace ui {
struct Palette {
    COLORREF background, header, surface, border, text, muted, accent, selected, accentText;
};

inline Palette Colors(bool light) {
    HIGHCONTRASTW contrast{sizeof(contrast)};
    if (SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(contrast), &contrast, 0) &&
        (contrast.dwFlags & HCF_HIGHCONTRASTON)) {
        auto bg = GetSysColor(COLOR_WINDOW), fg = GetSysColor(COLOR_WINDOWTEXT);
        return {bg, bg, bg, fg, fg, fg, GetSysColor(COLOR_HIGHLIGHT),
                GetSysColor(COLOR_HIGHLIGHT), GetSysColor(COLOR_HIGHLIGHTTEXT)};
    }
    if (light) return {RGB(248,249,252), RGB(238,240,247), RGB(255,255,255), RGB(190,195,208),
                       RGB(26,31,44), RGB(72,79,96), RGB(104,62,225), RGB(233,224,255), RGB(65,32,144)};
    return {RGB(20,24,34), RGB(28,34,48), RGB(32,38,51), RGB(70,78,97),
            RGB(245,246,250), RGB(179,188,205), RGB(139,97,255), RGB(53,39,91), RGB(245,239,255)};
}

inline COLORREF Mix(COLORREF from, COLORREF to, int percent) {
    auto mix = [percent](int a, int b) { return (a*(100-percent)+b*percent+50)/100; };
    return RGB(mix(GetRValue(from), GetRValue(to)), mix(GetGValue(from), GetGValue(to)),
               mix(GetBValue(from), GetBValue(to)));
}

inline void Fill(HDC dc, RECT rect, COLORREF color) {
    SetDCBrushColor(dc, color);
    FillRect(dc, &rect, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
}

inline void Rounded(HDC dc, RECT r, int radius, COLORREF fill, COLORREF border) {
    if (r.right <= r.left || r.bottom <= r.top) return;
    auto pen = SelectObject(dc, GetStockObject(DC_PEN));
    auto brush = SelectObject(dc, GetStockObject(DC_BRUSH));
    SetDCPenColor(dc, border);
    SetDCBrushColor(dc, fill);
    int diameter = std::min<int>({radius*2, r.right-r.left, r.bottom-r.top});
    RoundRect(dc, r.left, r.top, r.right, r.bottom, diameter, diameter);
    SelectObject(dc, brush);
    SelectObject(dc, pen);
}

// Rounded panel with a horizontal tint, used for the provider cards. GDI has no
// anti-aliased gradients, so paint thin vertical strips inside a rounded clip.
inline void Gradient(HDC dc, RECT r, int radius, COLORREF left, COLORREF right, COLORREF border) {
    if (r.right <= r.left || r.bottom <= r.top) return;
    int diameter = std::min<int>({radius*2, r.right-r.left, r.bottom-r.top});
    int saved = SaveDC(dc);
    if (auto clip = CreateRoundRectRgn(r.left, r.top, r.right+1, r.bottom+1, diameter, diameter)) {
        ExtSelectClipRgn(dc, clip, RGN_AND);
        DeleteObject(clip);
    }
    const int width = r.right-r.left, step = 4;
    for (int x = 0; x < width; x += step)
        Fill(dc, {r.left+x, r.top, std::min<int>(r.left+x+step, r.right), r.bottom}, Mix(left, right, x*100/width));
    RestoreDC(dc, saved);
    auto pen = SelectObject(dc, GetStockObject(DC_PEN));
    auto brush = SelectObject(dc, GetStockObject(NULL_BRUSH));
    SetDCPenColor(dc, border);
    RoundRect(dc, r.left, r.top, r.right, r.bottom, diameter, diameter);
    SelectObject(dc, brush);
    SelectObject(dc, pen);
}

inline int Measure(HDC dc, const std::wstring& value, HFONT face) {
    auto oldFont = face ? SelectObject(dc, face) : nullptr;
    SIZE extent{};
    GetTextExtentPoint32W(dc, value.c_str(), static_cast<int>(value.size()), &extent);
    if (oldFont) SelectObject(dc, oldFont);
    return extent.cx;
}

// Segoe MDL2 Assets ships with Windows 10 and 11; Fluent Icons shares its code points.
inline HFONT SymbolFont(int height) {
    return CreateFontW(-std::max(1, height), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                       OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH,
                       L"Segoe MDL2 Assets");
}

namespace glyph {
constexpr wchar_t Sync[] = L"\uE895", Settings[] = L"\uE713", Check[] = L"\uE73E", Chevron[] = L"\uE70D",
                  Clock[] = L"\uE823", Calendar[] = L"\uE787", Monitor[] = L"\uE7F4", Bell[] = L"\uEA8F",
                  External[] = L"\uE8A7", Copy[] = L"\uE8C8", Power[] = L"\uE7E8", Close[] = L"\uE711";
}

inline int Text(HDC dc, RECT r, const std::wstring& value, HFONT face, COLORREF color, UINT align = DT_LEFT) {
    auto oldFont = face ? SelectObject(dc, face) : nullptr;
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, color);
    DrawTextW(dc, value.c_str(), -1, &r, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX | align);
    SIZE extent{};
    GetTextExtentPoint32W(dc, value.c_str(), static_cast<int>(value.size()), &extent);
    if (oldFont) SelectObject(dc, oldFont);
    return extent.cx;
}

inline void Icon(HDC dc, int id, int x, int y, int size) {
    auto image = static_cast<HICON>(LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(id),
                                               IMAGE_ICON, size, size, 0));
    if (!image) return;
    DrawIconEx(dc, x, y, image, size, size, 0, nullptr, DI_NORMAL);
    DestroyIcon(image);
}

inline HFONT Font(int height, int weight) {
    return CreateFontW(-std::max(1, height), 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                       OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
}
} // namespace ui
