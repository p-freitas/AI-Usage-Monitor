#include "presentation.h"
#include <algorithm>
#include <ctime>

namespace ui {

void Preferences::Validate() {
    if (display < Display::Complete || display > Display::Weekly) display = Display::Complete;
    if (reset < Reset::Relative || reset > Reset::Both) reset = Reset::Relative;
    if (warning < 1 || critical > 99 || warning >= critical) {
        warning = 80;
        critical = 90;
    }
}

Severity Level(int used, const Preferences& prefs) {
    if (used == 100) return Severity::Exhausted;
    if (used >= prefs.critical) return Severity::Critical;
    if (used >= prefs.warning) return Severity::Warning;
    return Severity::Normal;
}

bool Segment::operator==(const Segment& other) const {
    return label == other.label && value == other.value && severity == other.severity && stale == other.stale;
}

ProviderView Describe(ai::Provider provider, int icon, const ai::UsageState& state,
                      const Preferences& prefs, bool disconnected) {
    ProviderView view{provider, icon};
    struct Definition { ai::Limit limit; const wchar_t* shortName; const wchar_t* label; };
    static constexpr Definition limits[] = {
        {ai::FiveHours, L"5h", L"5 hours"}, {ai::Week, L"7d", L"7 days"},
        {ai::ArgonWeek, L"A", L"Argon \u00b7 7 days"}
    };
    for (const auto& def : limits) {
        int used = state.value.used[def.limit];
        bool gemini = provider == ai::Provider::Gemini;
        if ((gemini && used < 0) || (!gemini && def.limit == ai::ArgonWeek)) continue;
        auto label = def.label;
        if (gemini && def.limit == ai::FiveHours) label = L"Gemini \u00b7 5 hours";
        if (gemini && def.limit == ai::Week) label = L"Gemini \u00b7 7 days";
        bool stale = used >= 0 && (state.LastKnown(def.limit) || disconnected);
        auto level = Level(used, prefs);
        view.rows.push_back({def.limit, label, used, state.value.resetsAt[def.limit], stale, level});
        if (prefs.display == Display::Weekly && def.limit == ai::FiveHours) continue;
        if (prefs.display == Display::Weekly && used < 0) continue;
        bool labeled = prefs.display != Display::Minimal || def.limit == ai::ArgonWeek;
        auto value = used < 0 ? std::wstring(L"\u2014") : std::to_wstring(used) + L"%";
        if (level == Severity::Exhausted) value += L"!";
        view.strip.push_back({labeled ? def.shortName : L"", value, prefs.colors ? level : Severity::Normal, stale});
    }
    return view;
}

static std::wstring LocalTimeLabel(long long stamp, long long now, bool& todayOnly) {
    // Convert each instant with the current Windows time zone, including its DST rules.
    auto local = [](long long unixTime, SYSTEMTIME& result) {
        if (unixTime < 0 || unixTime > 253402300799LL) return false;
        ULARGE_INTEGER ticks{};
        ticks.QuadPart = (unixTime + 11644473600LL) * 10000000ULL;
        FILETIME ft{ticks.LowPart, ticks.HighPart};
        SYSTEMTIME utc{};
        return FileTimeToSystemTime(&ft, &utc) &&
               SystemTimeToTzSpecificLocalTime(nullptr, &utc, &result);
    };
    SYSTEMTIME at{}, today{};
    if (!local(stamp, at) || !local(now, today)) return {};
    wchar_t time[96]{}, day[96]{};
    if (!GetTimeFormatEx(LOCALE_NAME_USER_DEFAULT, TIME_NOSECONDS, &at, nullptr, time, ARRAYSIZE(time))) return {};
    todayOnly = today.wYear == at.wYear && today.wMonth == at.wMonth && today.wDay == at.wDay;
    if (todayOnly) return time;
    if (!GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, 0, &at, L"dddd", day, ARRAYSIZE(day), nullptr)) return {};
    return std::wstring(day) + L" at " + time;
}

std::wstring ResetText(long long reset, long long now, Reset mode) {
    if (reset <= 0 || reset > 253402300799LL || now < 0) return L"Reset unknown";
    if (reset <= now) return L"Reset due";
    bool todayOnly = false;
    if (mode == Reset::Absolute) {
        auto at = LocalTimeLabel(reset, now, todayOnly);
        return at.empty() ? L"Reset unknown" : (todayOnly ? L"Resets at " : L"Resets ") + at;
    }
    auto minutes = (reset - now + 59) / 60;
    std::wstring duration;
    if (minutes >= 1440) duration = std::to_wstring(minutes / 1440) + L"d ";
    if (minutes >= 60) duration += std::to_wstring(minutes / 60 % 24) + L"h ";
    if (minutes >= 60 && minutes % 60 < 10) duration += L"0";
    duration += std::to_wstring(minutes % 60) + L"m";
    if (mode == Reset::Both) {
        auto at = LocalTimeLabel(reset, now, todayOnly);
        if (!at.empty()) duration += L" (" + at + L")";
    }
    return L"Resets in " + duration;
}

std::wstring PlainText(const ProviderView& view, Reset mode, long long now) {
    std::wstring text = ai::Name(view.provider);
    for (const auto& row : view.rows) {
        text += L"\n";
        text += row.label;
        if (row.used < 0) { text += L" unavailable"; continue; }
        text += L" " + std::to_wstring(row.used) + L"%";
        if (row.stale) text += L" (last known)";
        text += L" - " + ResetText(row.reset, now, mode);
    }
    if (!view.status.empty()) text += L"\n" + view.status;
    return text;
}

std::wstring TrayText(const ProviderView& view, Reset mode, long long now) {
    // Shell_NotifyIcon limits szTip to 127 characters. Keep every percentage
    // ahead of optional reset detail so a long localized date cannot hide Argon.
    std::wstring text = ai::Name(view.provider), resets;
    for (const auto& row : view.rows) {
        std::wstring label = row.limit == ai::FiveHours ? L"5h" : row.limit == ai::Week ? L"7d" : L"Argon";
        text += L"  " + label + L" " + (row.used < 0 ? L"\u2014" : std::to_wstring(row.used) + L"%");
        if (row.stale) text += L"*";
        if (row.used < 0) continue;
        auto time = ResetText(row.reset, now, mode);
        if (time.compare(0, 6, L"Resets") == 0) time.erase(0, 7);
        resets += L"\n" + label + L" " + time;
    }
    if (text.find(L'*') != std::wstring::npos) text += L"\n* Last known";
    if (!view.status.empty()) text += L"\n" + view.status;
    if (text.size() + resets.size() <= 127) return text + resets;
    return text;
}

bool Crossing::Observe(int used, long long nextReset, const Preferences& prefs) {
    if (used < 0 || used > 100) return false;
    bool newWindow = reset && nextReset && nextReset != reset;
    bool crossed = previous >= 0 && !newWindow && Level(used, prefs) > Level(previous, prefs);
    if (newWindow || previous < 0) previous = used;
    else previous = std::max(previous, used);
    if (nextReset) reset = nextReset;
    return crossed;
}

} // namespace ui
