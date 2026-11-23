#include "../src/presentation.h"
#include "check.h"
#include <cstdio>

int main() {
    ui::Preferences prefs;
    ai::UsageState state;
    state.value = {38, 72, true, {1000, 2000}};
    state.value.used[ai::ArgonWeek] = 91;
    state.value.resetsAt[ai::ArgonWeek] = 3000;
    auto view = [&] { return ui::Describe(ai::Provider::Gemini, 0, state, prefs); };
    auto complete = view();
    CHECK(complete.strip.size() == 3 && complete.rows.size() == 3);
    CHECK(complete.strip[0].label == L"5h" && complete.strip[0].value == L"38%");
    CHECK(complete.strip[1].label == L"7d" && complete.strip[1].value == L"72%");
    CHECK(complete.strip[2].label == L"A" && complete.strip[2].value == L"91%");
    CHECK(complete.rows[1].reset == 2000 && complete.rows[2].reset == 3000);
    CHECK(complete.rows[1].severity == ui::Severity::Normal && complete.rows[2].severity == ui::Severity::Critical);
    auto tray = ui::TrayText(complete, ui::Reset::Both, 0);
    CHECK(tray.size() <= 127 && tray.find(L"Argon 91%") != std::wstring::npos);
    prefs.display = ui::Display::Minimal;
    auto minimal = view();
    CHECK(minimal.strip[0].label.empty() && minimal.strip[1].label.empty() && minimal.strip[2].label == L"A");
    prefs.display = ui::Display::Weekly;
    auto weekly = view();
    CHECK(weekly.rows.size() == 3 && weekly.strip.size() == 2 && weekly.strip[0].label == L"7d");
    state.value.used[ai::Week] = -1;
    CHECK(view().rows.size() == 2 && view().strip.size() == 1 && view().strip[0].label == L"A");
    state.value.used[ai::ArgonWeek] = -1;
    CHECK(view().strip.empty() && view().rows.size() == 1);
    prefs.display = ui::Display::Complete;
    auto codex = ui::Describe(ai::Provider::Codex, 0, state, prefs);
    CHECK(codex.strip.size() == 2 && codex.strip[1].value == L"\u2014");
    state.value.used[ai::ArgonWeek] = 100;
    prefs.colors = false;
    CHECK(view().strip.back().value == L"100%!" && view().strip.back().severity == ui::Severity::Normal);

    CHECK(ui::Level(79, prefs) == ui::Severity::Normal && ui::Level(80, prefs) == ui::Severity::Warning);
    CHECK(ui::Level(89, prefs) == ui::Severity::Warning && ui::Level(90, prefs) == ui::Severity::Critical);
    CHECK(ui::Level(99, prefs) == ui::Severity::Critical && ui::Level(100, prefs) == ui::Severity::Exhausted);
    ui::Crossing crossing;
    CHECK(!crossing.Observe(79, 1000, prefs));
    CHECK(crossing.Observe(80, 1000, prefs) && !crossing.Observe(81, 1000, prefs));
    CHECK(crossing.Observe(90, 1000, prefs) && !crossing.Observe(87, 1000, prefs));
    CHECK(!crossing.Observe(91, 1000, prefs) && crossing.Observe(100, 1000, prefs));
    CHECK(!crossing.Observe(0, 2000, prefs) && crossing.Observe(92, 2000, prefs));
    ui::Crossing startup;
    CHECK(!startup.Observe(95, 1000, prefs));

    CHECK(ui::ResetText(0, 0, ui::Reset::Both) == L"Reset unknown");
    CHECK(ui::ResetText(100, 101, ui::Reset::Relative) == L"Reset due");
    CHECK(ui::ResetText(100, 100, ui::Reset::Both) == L"Reset due");
    CHECK(ui::ResetText(101, 100, ui::Reset::Relative) == L"Resets in 1m");
    CHECK(ui::ResetText(4240, 100, ui::Reset::Relative) == L"Resets in 1h 09m");
    CHECK(ui::ResetText(90061, 0, ui::Reset::Relative) == L"Resets in 1d 1h 02m");
    constexpr long long now = 1800000000;
    auto absolute = ui::ResetText(now+600, now, ui::Reset::Absolute);
    CHECK(absolute.find(L"Resets at ") == 0);
    CHECK(ui::ResetText(now+600, now, ui::Reset::Both) == L"Resets in 10m (" + absolute.substr(std::wstring(L"Resets at ").size()) + L")");
    prefs.display = static_cast<ui::Display>(-1);
    prefs.reset = static_cast<ui::Reset>(99);
    prefs.warning = 95;
    prefs.critical = 90;
    prefs.Validate();
    CHECK(prefs.display == ui::Display::Complete && prefs.reset == ui::Reset::Relative);
    CHECK(prefs.warning == 80 && prefs.critical == 90);
    puts("Presentation, thresholds, preferences and countdown tests passed");
}
