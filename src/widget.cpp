#include "widget.h"
#include "placement.h"
#include "../res/resource.h"

#include "usage_tip.h"
#include <uiautomation.h>
#include <algorithm>

namespace {

HWND g_wnd, g_taskbar, g_notify, g_rebar;
HINSTANCE g_instance;
UINT g_clickMsg;
bool g_light, g_hover, g_fits;
bool g_background;  // opaque rounded plate behind the text, for transparent / restyled taskbars
std::vector<ui::ProviderView> g_items;
ui::Preferences g_prefs;
RECT g_lastTray{}, g_lastBar{}, g_lastWidgets{};
int g_lastW = -1, g_lastDpi = 0, g_lastLeft = -1;
SIZE g_size{};

const wchar_t* kClass = L"AIUsageMonitorWidget";

int Dpi() {
    UINT d = g_taskbar ? GetDpiForWindow(g_taskbar) : 0;
    return d ? (int)d : 96;
}
int Px(int v) { return MulDiv(v, Dpi(), 96); }

// Windows 11 puts the Widgets (weather) button right next to the tray when the taskbar is
// left-aligned (start-aligned on right-to-left Windows). It is XAML, so only UI Automation knows
// where it is. The element is looked up once per taskbar; its rectangle is re-read every few
// seconds because the weather text changes its width.
//
// UI Automation calls go to Explorer and wait for its UI thread, while our widget is a child window
// of the taskbar, so Explorer in turn waits for our UI thread whenever it lays out or sends input to
// the widget. Making those calls from our UI thread deadlocks both: Explorer is reported hung
// ("Cross-process", waiting on AIUsageMonitor.exe) and restarted, again and again. So the lookup
// runs on its own thread, which owns no windows, and the UI thread only reads the last rectangle.
SRWLOCK g_wbLock = SRWLOCK_INIT;
RECT g_wbRect{};             // last Widgets button rectangle (screen), empty when unknown
HWND g_wbRectBar;            // taskbar g_wbRect was found in (guarded by g_wbLock)
HWND g_wbTaskbar;            // taskbar to search, null while there is no widget (guarded by g_wbLock)
HANDLE g_wbWake;             // auto-reset: wakes the lookup thread early

DWORD WINAPI WidgetsButtonThread(LPVOID) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IUIAutomation* uia = nullptr;
    IUIAutomationElement* btn = nullptr;
    HWND btnBar = nullptr;
    for (;;) {
        WaitForSingleObject(g_wbWake, btnBar ? 5000 : INFINITE);  // no widget (tray-icon mode): sleep until one
        AcquireSRWLockShared(&g_wbLock);
        HWND bar = g_wbTaskbar;
        ReleaseSRWLockShared(&g_wbLock);
        if (btn && bar != btnBar) { btn->Release(); btn = nullptr; }
        btnBar = bar;
        RECT r{};
        if (bar && IsWindow(bar)) {
            if (!uia)
                CoCreateInstance(__uuidof(CUIAutomation), nullptr, CLSCTX_INPROC_SERVER, __uuidof(IUIAutomation), (void**)&uia);
            if (uia && !btn) {
                IUIAutomationElement* root = nullptr;
                IUIAutomationCondition* cond = nullptr;
                VARIANT id;
                VariantInit(&id);
                id.vt = VT_BSTR;
                id.bstrVal = SysAllocString(L"WidgetsButton");
                if (SUCCEEDED(uia->ElementFromHandle(bar, &root)) && root &&
                    SUCCEEDED(uia->CreatePropertyCondition(UIA_AutomationIdPropertyId, id, &cond)) && cond)
                    root->FindFirst(TreeScope_Descendants, cond, &btn);
                VariantClear(&id);
                if (cond) cond->Release();
                if (root) root->Release();
            }
            if (btn && FAILED(btn->get_CurrentBoundingRectangle(&r))) {
                btn->Release();  // stale (Widgets turned off, Explorer restarted): look it up again next time
                btn = nullptr;
                r = {};
            }
        }
        AcquireSRWLockExclusive(&g_wbLock);
        if (bar && g_wbTaskbar == bar) {  // drop a result for a taskbar we no longer sit on
            g_wbRect = r;
            g_wbRectBar = bar;
        }
        ReleaseSRWLockExclusive(&g_wbLock);
    }
}

// Points the lookup thread at `taskbar` (null to idle it), starting the thread on first use.
// The last rectangle is kept for its taskbar, so switching from tray-icon back to taskbar mode on the
// same taskbar steps past the Widgets button straight away instead of after the next lookup.
void WatchWidgetsButton(HWND taskbar) {
    AcquireSRWLockExclusive(&g_wbLock);
    g_wbTaskbar = taskbar;
    ReleaseSRWLockExclusive(&g_wbLock);
    if (!g_wbWake) {
        g_wbWake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        HANDLE t = g_wbWake ? CreateThread(nullptr, 0, WidgetsButtonThread, nullptr, 0, nullptr) : nullptr;
        if (t) CloseHandle(t);
    }
    if (g_wbWake) SetEvent(g_wbWake);
}

// Never blocks: returns what the lookup thread last found on the current taskbar.
RECT WidgetsButtonRect() {
    AcquireSRWLockShared(&g_wbLock);
    RECT r = g_wbRectBar && g_wbRectBar == g_wbTaskbar ? g_wbRect : RECT{};
    ReleaseSRWLockShared(&g_wbLock);
    return r;
}

void ShowTip() {
    RECT bounds{};
    if (GetWindowRect(g_wnd, &bounds))
        widget::ShowUsageTip(g_notify, bounds, Dpi(), g_light, g_items, g_prefs, false);
}

// Creates a font, falling back when the preferred face is not installed (GDI would silently substitute).
HFONT MakeFont(int px, int weight, const wchar_t* face, const wchar_t* fallback) {
    for (const wchar_t* f : { face, fallback }) {
        HFONT h = CreateFontW(-px, 0, 0, 0, weight, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                              ANTIALIASED_QUALITY, DEFAULT_PITCH, f);
        HDC dc = CreateCompatibleDC(nullptr);
        HGDIOBJ o = SelectObject(dc, h);
        wchar_t got[LF_FACESIZE] = {};
        GetTextFaceW(dc, LF_FACESIZE, got);
        SelectObject(dc, o);
        DeleteDC(dc);
        if (!_wcsicmp(got, f) || f == fallback) return h;
        DeleteObject(h);
    }
    return nullptr;
}

// Crop horizontal side bearings, but keep the font line box for a common baseline.
struct Mask {
    int w = 0, h = 0;
    std::vector<BYTE> a;
    std::vector<DWORD> iconPixels;
};

Mask RenderMask(const std::wstring& s, HFONT font) {
    Mask m;
    HDC dc = CreateCompatibleDC(nullptr);
    HGDIOBJ of = SelectObject(dc, font);
    SIZE sz{};
    GetTextExtentPoint32W(dc, s.c_str(), (int)s.size(), &sz);
    int W = sz.cx + 8, H = sz.cy + 8;
    BITMAPINFO bi{};
    bi.bmiHeader = { sizeof(BITMAPINFOHEADER), W, -H, 1, 32, BI_RGB };
    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!bmp || !bits) {
        DeleteObject(bmp);
        SelectObject(dc, of);
        DeleteDC(dc);
        return m;
    }
    HGDIOBJ ob = SelectObject(dc, bmp);
    memset(bits, 0, (size_t)W * H * 4);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(255, 255, 255));
    TextOutW(dc, 4, 4, s.c_str(), (int)s.size());
    GdiFlush();
    auto* p = static_cast<DWORD*>(bits);
    int x0 = W, x1 = -1;
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++)
            if (p[y * W + x] & 0xFF) { x0 = std::min(x0, x); x1 = std::max(x1, x); }
    if (x1 >= 0) {
        m.w = x1 - x0 + 1;
        m.h = sz.cy;
        m.a.resize((size_t)m.w * m.h);
        for (int y = 0; y < m.h; y++)
            for (int x = 0; x < m.w; x++) m.a[y * m.w + x] = (BYTE)(p[(4 + y) * W + x0 + x] & 0xFF);
    }
    SelectObject(dc, ob);
    SelectObject(dc, of);
    DeleteObject(bmp);
    DeleteDC(dc);
    return m;
}

Mask RenderResourceIcon(int resource) {
    int size = Px(15);
    auto icon = static_cast<HICON>(LoadImageW(g_instance, MAKEINTRESOURCEW(resource),
                                             IMAGE_ICON, size, size, LR_SHARED));
    if (!icon) return {};
    HDC dc = CreateCompatibleDC(nullptr);
    BITMAPINFO bi{};
    bi.bmiHeader = {sizeof(BITMAPINFOHEADER), size, -size, 1, 32, BI_RGB};
    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    Mask mask;
    if (bmp) {
        HGDIOBJ previous = SelectObject(dc, bmp);
        auto* pixels = static_cast<DWORD*>(bits);
        std::fill_n(pixels, size * size, 0);
        DrawIconEx(dc, 0, 0, icon, size, size, 0, nullptr, DI_NORMAL);
        GdiFlush();
        mask.w = mask.h = size;
        mask.iconPixels.assign(pixels, pixels + size * size);
        SelectObject(dc, previous);
        DeleteObject(bmp);
    }
    DeleteDC(dc);
    return mask;
}

struct Rgb { int r, g, b; };

struct Canvas {
    int w, h;
    std::vector<DWORD> px;  // premultiplied BGRA
    Canvas(int w_, int h_) : w(w_), h(h_), px((size_t)w_ * h_, 0) {}
    void Blend(int x, int y, Rgb c, int a) {  // source-over, a = 0..255
        if (x < 0 || y < 0 || x >= w || y >= h || a <= 0) return;
        DWORD d = px[y * w + x];
        int inv = 255 - a;
        int da = d >> 24, dr = (d >> 16) & 255, dg = (d >> 8) & 255, db = d & 255;
        int oa = a + da * inv / 255;
        int orr = c.r * a / 255 + dr * inv / 255;
        int og = c.g * a / 255 + dg * inv / 255;
        int ob = c.b * a / 255 + db * inv / 255;
        px[y * w + x] = ((DWORD)oa << 24) | ((DWORD)orr << 16) | ((DWORD)og << 8) | (DWORD)ob;
    }
    void Draw(const Mask& m, int x, int y, Rgb c, int alpha) {
        for (int j = 0; j < m.h; j++) {
            for (int i = 0; i < m.w; i++) {
                int index = j * m.w + i;
                if (m.iconPixels.empty()) {
                    Blend(x + i, y + j, c, m.a[index] * alpha / 255);
                    continue;
                }
                DWORD p = m.iconPixels[index];
                int a = p >> 24;
                if (!a) continue;
                // DrawIconEx gives us premultiplied RGB, while Blend expects straight RGB.
                Rgb color{std::min(255, (int)((p >> 16) & 255) * 255 / a),
                          std::min(255, (int)((p >> 8) & 255) * 255 / a),
                          std::min(255, (int)(p & 255) * 255 / a)};
                Blend(x + i, y + j, color, a * alpha / 255);
            }
        }
    }
};

struct Piece {
    Mask m;
    Rgb color;
    int alpha, gapAfter;
    int badgePadding = 0;
    int Width() const { return m.w + 2 * badgePadding; }
};

std::vector<Piece> Layout(int& width) {
    Rgb fg = g_light ? Rgb{0, 0, 0} : Rgb{255, 255, 255};
    HFONT text = MakeFont(Px(13), FW_NORMAL, L"Segoe UI Variable Text", L"Segoe UI");
    HFONT numeric = MakeFont(Px(13), FW_SEMIBOLD, L"Segoe UI Variable Text", L"Segoe UI");
    std::vector<Piece> out;
    Mask divider;
    divider.w = Px(1);
    divider.h = Px(15);
    divider.a.assign(divider.w * divider.h, 255);
    auto dot = RenderMask(L"\u00b7", text);
    for (const auto& item : g_items) {
        if (item.strip.empty()) continue;
        if (!out.empty()) {
            out.back().gapAfter = Px(7);
            out.push_back({divider, fg, 84, Px(7)});
        }
        out.push_back({RenderResourceIcon(item.icon), fg, 255, Px(6)});
        bool first = true;
        for (const auto& part : item.strip) {
            if (!first) out.push_back({dot, fg, 165, Px(4)});
            if (!part.label.empty()) out.push_back({RenderMask(part.label, text), fg, 235, Px(4), part.label == L"A" ? Px(3) : 0});
            auto mask = RenderMask(part.value, part.severity >= ui::Severity::Critical ? numeric : text);
            auto c = widget::UsageColor(part.severity, g_light, RGB(fg.r, fg.g, fg.b));
            out.push_back({std::move(mask), {GetRValue(c), GetGValue(c), GetBValue(c)},
                           part.stale ? 150 : 255, Px(4)});
            first = false;
        }
        out.back().gapAfter = 0;
    }
    if (out.empty()) out.push_back({RenderResourceIcon(IDI_APP), fg, 170, 0});
    DeleteObject(numeric);
    DeleteObject(text);
    width = Px(8) * 2;
    for (const auto& piece : out) width += piece.Width() + piece.gapAfter;
    return out;
}

void Paint() {
    if (!g_wnd) return;
    RECT bar;
    GetClientRect(g_taskbar, &bar);
    int H = bar.bottom - bar.top;
    int W = 0;
    auto pieces = Layout(W);
    if (W <= 0 || H <= 0) return;
    Canvas cv(W, H);

    // Nearly transparent background so the whole strip receives mouse input,
    // plus a rounded hover highlight like the system tray icons have.
    for (auto& p : cv.px) p = 0x01000000;
    auto rounded = [&](int left, int top, int right, int bottom, int r, Rgb c, int alpha) {
        for (int y = top; y < bottom; ++y) {
            for (int x = left; x < right; ++x) {
                int dx = std::max({left+r-x, x-(right-r-1), 0});
                int dy = std::max({top+r-y, y-(bottom-r-1), 0});
                if (dx*dx + dy*dy <= r*r) cv.Blend(x, y, c, alpha);
            }
        }
    };
    Rgb ink = g_light ? Rgb{0,0,0} : Rgb{255,255,255};
    if (g_background) {
        int inset = Px(4);
        rounded(0, inset, W, H-inset, Px(8), ink, 65);
        rounded(1, inset+1, W-1, H-inset-1, Px(7), g_light ? Rgb{243,243,243} : Rgb{32,34,40}, 245);
    }
    if (g_hover) rounded(0, Px(4), W, H-Px(4), Px(8), ink, g_light ? 20 : 26);

    int x = Px(8);
    for (const auto& p : pieces) {
        int y = (H - p.m.h) / 2;
        if (p.badgePadding) rounded(x, y, x+p.Width(), y+p.m.h, Px(3), ink, 44);
        cv.Draw(p.m, x+p.badgePadding, y, p.color, p.alpha);
        x += p.Width() + p.gapAfter;
    }

    HDC screen = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(screen);
    BITMAPINFO bi{};
    bi.bmiHeader = { sizeof(BITMAPINFOHEADER), W, -H, 1, 32, BI_RGB };
    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(mem, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!bmp || !bits) {
        DeleteObject(bmp);
        DeleteDC(mem);
        ReleaseDC(nullptr, screen);
        return;
    }
    memcpy(bits, cv.px.data(), (size_t)W * H * 4);
    HGDIOBJ ob = SelectObject(mem, bmp);
    g_size = { W, H };
    SIZE sz{ W, H };
    POINT src{ 0, 0 };
    BLENDFUNCTION bf{ AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
    UpdateLayeredWindow(g_wnd, screen, nullptr, &sz, mem, &src, 0, &bf, ULW_ALPHA);
    SelectObject(mem, ob);
    DeleteObject(bmp);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);
    g_lastW = -1;  // force a re-position with the new size
}

LRESULT CALLBACK WidgetProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_MOUSEMOVE:
            if (!g_hover) {
                g_hover = true;
                TRACKMOUSEEVENT t{ sizeof(t), TME_LEAVE | TME_HOVER, h, HOVER_DEFAULT };
                TrackMouseEvent(&t);
                Paint();
                widget::Reposition();
            }
            return 0;
        case WM_MOUSEHOVER:
            if (g_hover) ShowTip();
            return 0;
        case WM_MOUSELEAVE:
            widget::HideHoverTip();
            g_hover = false;
            Paint();
            widget::Reposition();
            return 0;
        case WM_LBUTTONUP:
        case WM_RBUTTONUP:
            widget::HideHoverTip();
            PostMessageW(g_notify, g_clickMsg, reinterpret_cast<WPARAM>(h), 0);
            return 0;
        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

}  // namespace

namespace widget {

bool Create(HINSTANCE inst, HWND notifyWnd, UINT clickMsg, bool lightTheme) {
    Destroy();
    g_instance = inst;
    g_taskbar = FindWindowW(L"Shell_TrayWnd", nullptr);
    if (!g_taskbar || !FindWindowExW(g_taskbar, nullptr, L"TrayNotifyWnd", nullptr)) { g_taskbar = nullptr; return false; }
    RECT tb;
    GetWindowRect(g_taskbar, &tb);
    if (tb.bottom - tb.top > tb.right - tb.left) { g_taskbar = nullptr; return false; }  // vertical taskbar: use tray icons
    // Windows 11's taskbar is a XAML island; on Windows 10 the task buttons live in ReBarWindow32
    // and have to be narrowed so they never slide under the widget.
    g_rebar = FindWindowExW(g_taskbar, nullptr, L"Windows.UI.Composition.DesktopWindowContentBridge", nullptr)
                  ? nullptr
                  : FindWindowExW(g_taskbar, nullptr, L"ReBarWindow32", nullptr);
    g_notify = notifyWnd;
    g_clickMsg = clickMsg;
    g_light = lightTheme;

    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{ sizeof(wc) };
        wc.lpfnWndProc = WidgetProc;
        wc.hInstance = inst;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = kClass;
        registered = RegisterClassExW(&wc) != 0;
    }
    g_wnd = CreateWindowExW(WS_EX_LAYERED | WS_EX_NOACTIVATE, kClass, L"", WS_CHILD | WS_CLIPSIBLINGS, 0, 0, 1, 1,
                            g_taskbar, nullptr, inst, nullptr);
    if (!g_wnd) { g_taskbar = nullptr; return false; }

    // The classic taskbar exposes its weather widget as a native window, handled by Reposition.
    if (!g_rebar) WatchWidgetsButton(g_taskbar);
    g_lastDpi = Dpi();
    Paint();
    g_lastW = -1;
    Reposition();
    return true;
}

void Destroy() {
    if (g_rebar && IsWindow(g_rebar) && g_taskbar && IsWindow(g_taskbar)) {
        // Let Explorer lay the task buttons out again over the full width.
        RECT r;
        GetClientRect(g_taskbar, &r);
        PostMessageW(g_taskbar, WM_SIZE, SIZE_RESTORED, MAKELPARAM(r.right, r.bottom));
    }
    g_rebar = nullptr;
    if (g_wbWake) WatchWidgetsButton(nullptr);
    HideHoverTip();
    if (g_wnd) { DestroyWindow(g_wnd); g_wnd = nullptr; }
    g_taskbar = nullptr;
    g_hover = false;
    g_fits = false;
}

void OpenPanel(HWND owner, const RECT& anchor, int dpi, bool light) {
    ShowUsageTip(owner, anchor, dpi, light, g_items, g_prefs, true);
}

bool FitsTaskbar() { return g_fits; }

bool Alive() { return g_wnd && IsWindow(g_wnd) && g_taskbar && IsWindow(g_taskbar); }

void SetItems(std::vector<ui::ProviderView> items, const ui::Preferences& prefs) {
    bool changed = items.size() != g_items.size() ||
        !std::equal(items.begin(), items.end(), g_items.begin(), [](const auto& a, const auto& b) {
            return a.icon == b.icon && a.strip == b.strip;
        });
    g_items = std::move(items);
    g_prefs = prefs;
    if (changed) { Paint(); Reposition(); }
    RefreshUsageTip(g_light, g_items, g_prefs);
}

void SetLightTheme(bool light) {
    g_light = light;
    RefreshUsageTip(g_light, g_items, g_prefs);
    Paint();
}

void SetBackground(bool on) {
    g_background = on;
    Paint();
}

bool Reposition() {
    if (!Alive()) return false;
    RECT client{};
    GetClientRect(g_taskbar, &client);
    if (Dpi() != g_lastDpi || client.bottom != g_size.cy) { g_lastDpi = Dpi(); Paint(); }
    HWND tray = FindWindowExW(g_taskbar, nullptr, L"TrayNotifyWnd", nullptr);
    if (!tray) return true;
    RECT tr, bar;
    GetWindowRect(tray, &tr);
    GetWindowRect(g_taskbar, &bar);
    // Right-to-left Windows (Hebrew, Arabic) mirrors the taskbar: client x runs from the screen's right
    // edge, so a window's client-space left edge is its screen-space right edge.
    bool rtl = GetWindowLongW(g_taskbar, GWL_EXSTYLE) & WS_EX_LAYOUTRTL;
    int edge = rtl ? tr.right : tr.left;  // screen x we sit next to
    // Windows 10: News and Interests / Weather sits in ShellFeedsTaskbarViewWindow immediately left of the tray.
    HWND feeds = FindWindowExW(g_taskbar, nullptr, L"ShellFeedsTaskbarViewWindow", nullptr);
    if (!rtl && feeds && IsWindowVisible(feeds)) {
        RECT fr;
        GetWindowRect(feeds, &fr);
        if (fr.left < edge && fr.right > fr.left) edge = fr.left;
    }
    // Step past the Widgets button when it sits where we would go.
    RECT wb = WidgetsButtonRect();
    if (wb.right > wb.left && wb.top < bar.bottom && wb.bottom > bar.top &&
        (rtl ? wb.left < edge + g_size.cx + Px(2) && wb.right > edge : wb.right > edge - g_size.cx - Px(2) && wb.left < edge))
        edge = rtl ? wb.right : wb.left;
    HWND peer = FindWindowExW(g_taskbar, nullptr, L"WirelessBatteryWidget", nullptr);
    RECT peerBounds{};
    if (peer && IsWindowVisible(peer) && GetWindowRect(peer, &peerBounds))
        edge = widget::EdgeBeforePeer(edge, bar, peerBounds, rtl);
    POINT p{ edge, bar.top };
    ScreenToClient(g_taskbar, &p);
    int left = p.x - g_size.cx - Px(2);

    // Optional user override in DPI-independent pixels (e.g. for custom padding).
    DWORD customOffset = 0, sz = sizeof(customOffset);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\AIUsageMonitor", L"TaskbarOffset",
                     RRF_RT_REG_DWORD, nullptr, &customOffset, &sz) == ERROR_SUCCESS) {
        left -= Px((int)customOffset);
    }

    // If even the taskbar or peer boundary cannot accommodate the strip, use tray icons.
    if (left < 0 || left + g_size.cx > bar.right - bar.left) {
        if (g_fits) {
            HideHoverTip();
            ShowWindow(g_wnd, SW_HIDE);
            g_lastW = -1;
            g_hover = false;
            if (g_rebar) PostMessageW(g_taskbar, WM_SIZE, SIZE_RESTORED, MAKELPARAM(bar.right-bar.left, bar.bottom-bar.top));
        }
        g_fits = false;
        return true;
    }
    g_fits = true;

    // Give Explorer back the space freed by a shorter strip or a moving peer.
    if (g_rebar && g_lastLeft >= 0 && left > g_lastLeft)
        PostMessageW(g_taskbar, WM_SIZE, SIZE_RESTORED, MAKELPARAM(bar.right-bar.left, bar.bottom-bar.top));

    // Windows 10: Explorer re-expands the task buttons on its own layout passes; narrow them again.
    if (g_rebar && IsWindow(g_rebar)) {
        RECT rr;
        GetWindowRect(g_rebar, &rr);
        POINT tl{ rtl ? rr.right : rr.left, rr.top };
        ScreenToClient(g_taskbar, &tl);
        if (tl.x + (rr.right - rr.left) > left && left > tl.x)
            MoveWindow(g_rebar, tl.x, tl.y, left - tl.x, rr.bottom - rr.top, TRUE);
    }

    // Keep us above the taskbar's own content (our sibling windows).
    bool covered = false;
    RECT own{}, sibling{}, overlap{};
    if (GetWindowRect(g_wnd, &own)) {
        for (HWND above = GetWindow(g_wnd, GW_HWNDPREV); above; above = GetWindow(above, GW_HWNDPREV)) {
            if (IsWindowVisible(above) && GetWindowRect(above, &sibling) &&
                IntersectRect(&overlap, &own, &sibling)) {
                covered = true;
                break;
            }
        }
    }
    if (!covered && EqualRect(&tr, &g_lastTray) && EqualRect(&bar, &g_lastBar) && EqualRect(&wb, &g_lastWidgets) &&
        g_lastW == g_size.cx && g_lastLeft == left)
        return true;
    g_lastTray = tr;
    g_lastWidgets = wb;
    g_lastBar = bar;
    g_lastW = g_size.cx;
    g_lastLeft = left;
    SetWindowPos(g_wnd, HWND_TOP, left, 0, g_size.cx, g_size.cy, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    RefreshUsageTip(g_light, g_items, g_prefs);
    return true;
}

}  // namespace widget
