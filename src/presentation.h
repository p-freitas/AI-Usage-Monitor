#pragma once
#include "ai_usage.h"
#include <vector>

namespace ui {

enum class Display { Complete, Minimal, Weekly };
enum class Reset { Relative, Absolute, Both };
enum class Severity { Normal, Warning, Critical, Exhausted };

struct Preferences {
    Display display = Display::Complete;
    Reset reset = Reset::Relative;
    bool colors = true;
    int warning = 80;
    int critical = 90;
    bool notifications = false;
    void Validate();
};

struct Row {
    ai::Limit limit;
    const wchar_t* label;
    int used;
    long long reset;
    bool stale;
    Severity severity;
};
struct Segment {
    std::wstring label, value;
    Severity severity;
    bool stale;
    bool operator==(const Segment& other) const;
};
struct ProviderView {
    ai::Provider provider;
    int icon;
    std::vector<Row> rows;
    std::vector<Segment> strip;
    std::wstring status;
};

Severity Level(int used, const Preferences& prefs);
ProviderView Describe(ai::Provider provider, int icon, const ai::UsageState& state,
                      const Preferences& prefs, bool disconnected = false);
std::wstring ResetText(long long reset, long long now, Reset mode);
std::wstring TrayText(const ProviderView& view, Reset mode, long long now);
std::wstring PlainText(const ProviderView& view, Reset mode, long long now);

// Remember the highest announced level within each quota window. The first fresh
// reading establishes a baseline, including after a cached startup or settings change.
struct Crossing {
    int previous = -1;
    long long reset = 0;
    bool Observe(int used, long long nextReset, const Preferences& prefs);
};

} // namespace ui
