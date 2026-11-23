#include "../src/ai_usage.cpp"
#include "../src/ai_usage_events.cpp"
#include "check.h"
#include <cstdio>

int main() {
    const std::string codex = R"({
        "rateLimits": {"primary": {"windowDurationMins": 300, "usedPercent": 99}},
        "rateLimitsByLimitId": {
            "another-model": {"primary": {"windowDurationMins": 300, "usedPercent": 98}},
            "codex": {
                "primary": {"windowDurationMins": 10080, "usedPercent": 22},
                "secondary": {"windowDurationMins": 300, "usedPercent": 0}
            }
        }
    })";
    json::Json reply;
    json::Parser parser{codex.data(), codex.data() + codex.size()};
    CHECK(parser.parse(reply));
    auto usage = ai::ParseCodex(&reply);
    CHECK(usage.fiveHours == 0 && usage.week == 22);

    std::string screen =
        "\x1b[5;1HCurrent session\x1b[K\r\n"
        "12% 12% used\x1b[K\r\n"
        "Resets in 1h\x1b[K\r\n"
        "Current week (all models)\x1b[K\r\n"
        "42% 42% used\x1b[K\r\n"
        "Current week (Sonnet only)\x1b[K\r\n"
        "98% 98% used\x1b[K\r\n"
        "Refreshing usage\x1b[K";
    bool refreshing = false;
    usage = ai::ParseClaude(ai::TerminalRows(screen), refreshing);
    CHECK(refreshing && usage.fiveHours == 12 && usage.week == 42);
    screen += "\x1b[6;1H13% 13% used\x1b[K\x1b[9;1H43% 43% used\x1b[K\x1b[12;1H\x1b[K";
    usage = ai::ParseClaude(ai::TerminalRows(screen), refreshing);
    CHECK(!refreshing && usage.fiveHours == 13 && usage.week == 43);
    screen += "\x1b[9;1HUsage unavailable\x1b[K";
    usage = ai::ParseClaude(ai::TerminalRows(screen), refreshing);
    CHECK(usage.fiveHours == 13 && usage.week == -1);
    CHECK(ai::Format(usage) == L"5h 13%  7d \u2014");
    CHECK(ai::Format({75, 34, true}) == L"5h 75%  7d 34%");
    CHECK(ai::FormatResets(usage).empty());
    const time_t sampleReset = 1795120000;
    std::tm local{};
    CHECK(localtime_s(&local, &sampleReset) == 0);
    wchar_t expectedTime[6];
    CHECK(std::wcsftime(expectedTime, ARRAYSIZE(expectedTime), L"%H:%M", &local) == 5);
    CHECK(ai::FormatResets({0, -1, true, {sampleReset, 0}}) == std::wstring(L"5h ") + expectedTime);
    const auto weeklyReset = ai::FormatResets({-1, 0, true, {0, sampleReset}});
    CHECK(weeklyReset.size() > 6 && weeklyReset.substr(0, 3) == L"7d ");
    CHECK(weeklyReset.substr(weeklyReset.size() - 3) == std::wstring(expectedTime, 2) + L"h");

    auto localTime = [](int year, int month, int day, int hour, int minute) {
        std::tm date{};
        date.tm_year = year - 1900;
        date.tm_mon = month - 1;
        date.tm_mday = day;
        date.tm_hour = hour;
        date.tm_min = minute;
        date.tm_isdst = -1;
        return std::mktime(&date);
    };
    const auto claudeNow = localTime(2026, 12, 31, 23, 30);
    const char* usageScreen =
        "Current session\r\n4% used\r\n  Resets 12:04am (America/Sao_Paulo)\r\n"
        "Current week (all models)\r\n93% used\r\n  Resets Jan 2, 2027, 3pm (America/Sao_Paulo)\r\n"
        "Current week (Sonnet only)\r\n98% used\r\n  Resets Jan 1, 2027, 5pm (America/Sao_Paulo)";
    usage = ai::ParseClaude(ai::TerminalRows(usageScreen), refreshing, claudeNow);
    CHECK(usage.fiveHours == 4 && usage.week == 93);
    CHECK(usage.resetsAt[0] == localTime(2027, 1, 1, 0, 4));
    CHECK(usage.resetsAt[1] == localTime(2027, 1, 2, 15, 0));
    CHECK(ai::FormatResets(usage).find(L"5h 00:04, 7d ") == 0);
    CHECK(ai::ParseClaudeReset("Resets Nov 25, 12pm (America/Sao_Paulo)",
        localTime(2026, 11, 23, 1, 0)) == localTime(2026, 11, 25, 12, 0));
    CHECK(ai::ParseClaudeReset("Resets Feb 30, 3pm", localTime(2027, 2, 27, 8, 0)) == 0);
    CHECK(ai::ParseClaudeReset("Resets 12:70am", claudeNow) == 0);
    CHECK(ai::ClaudeStartupState(ai::TerminalRows("Starting Claude Code")) == ai::ClaudeStartup::Waiting);
    CHECK(ai::ClaudeStartupState(ai::TerminalRows("  $  ")) == ai::ClaudeStartup::Ready);
    CHECK(ai::ClaudeStartupState(ai::TerminalRows("Quick safety check\r\n  $  ")) == ai::ClaudeStartup::NeedsSetup);
    CHECK(ai::ClaudeStartupState(ai::TerminalRows("Select login method")) == ai::ClaudeStartup::NeedsSignIn);
    for (const auto& sample : {std::pair{R"({"account":null,"requiresOpenaiAuth":true})", true},
                               std::pair{R"({"account":null,"requiresOpenaiAuth":false})", false},
                               std::pair{R"({"account":{"type":"chatgpt"},"requiresOpenaiAuth":true})", false}}) {
        std::string response = sample.first;
        json::Parser accountParser{response.data(), response.data() + response.size()};
        json::Json account;
        CHECK(accountParser.parse(account) && ai::CodexNeedsSignIn(&account) == sample.second);
    }

    auto live = ai::events::ParseCodexEvent(
        R"({"timestamp":"2026-11-19T20:00:01.123456Z","type":"event_msg","payload":{"type":"token_count","rate_limits":{"limit_id":"codex","primary":{"used_percent":25.5,"window_minutes":300,"resets_at":1795120000},"secondary":{"used_percent":44,"window_minutes":10080,"resets_at":1795500000}}}})");
    CHECK(live.observed == ai::events::RolloutTime("2026-11-19T20:00:01Z") + 1234560);
    CHECK(live.source == ai::UsageSource::Live && live.usage.fiveHours == 25 && live.usage.week == 44);
    CHECK(!ai::events::ParseCodexEvent(
        R"({"timestamp":"2026-11-19T20:00:01Z","type":"event_msg","payload":{"type":"token_count","rate_limits":{"limit_id":"other","primary":{"used_percent":99,"window_minutes":300}}}})"
    ).usage.Available());
    CHECK(!ai::events::ParseCodexEvent(R"({"type":"agent_message","message":"token_count"})").usage.Available());

    usage = ai::events::ParseClaudeStatus(
        R"({"context_window":{"used_percentage":99},"rate_limits":{"five_hour":{"used_percentage":0,"resets_at":1795120000},"seven_day":null}})");
    CHECK(usage.installed && usage.fiveHours == 0 && usage.week == -1);
    CHECK(!ai::events::ParseClaudeStatus(R"({"context_window":{"used_percentage":50}})").Available());

    ai::UsageState display;
    const auto now = ai::Now();
    const auto reset = (long long)((now - ai::events::EPOCH) / ai::events::TICKS_PER_SECOND) + 3600;
    CHECK(display.Apply({{20, 40, true, {reset, reset + 86400}}, now}));
    CHECK(display.Apply({{25, -1, true, {reset, 0}}, now + 3, ai::UsageSource::Live}));
    CHECK(!display.Apply({{23, 40, true, {reset, reset + 86400}}, now + 2}));
    CHECK(!display.Apply({{22, -1, true, {reset, 0}}, now + 4, ai::UsageSource::Live}));
    CHECK(!display.Apply({{25, -1, true, {reset, 0}}, now + 5, ai::UsageSource::Live}));
    CHECK(display.Apply({{26, 45, true, {reset, reset + 86400}}, now + 4}));
    CHECK(display.Apply({{0, -1, true, {reset + 18000, 0}}, now + 6, ai::UsageSource::Live}));
    CHECK(!display.Apply({{-1, -1, true}, now + 7}));
    CHECK(display.value.fiveHours == 0 && display.value.week == 45);
    CHECK(display.Apply({{0, -1, true, {reset + 36000, 0}}, now + 8, ai::UsageSource::Live}));
    CHECK(display.value.resetsAt[0] == reset + 36000);

    ai::UsageState startup;
    const auto history = ai::UsageSource::LastKnown;
    CHECK(startup.Apply({{24, 41, true, {reset, reset + 86400}}, now - 100, history}));
    CHECK(startup.LastKnown() && startup.value.fiveHours == 24 && startup.value.week == 41);
    CHECK(startup.Apply({{24, 41, true, {reset, reset + 86400}}, now}));
    CHECK(!startup.LastKnown());
    CHECK(!startup.Apply({{26, 43, true, {reset, reset + 86400}}, now + 1, history}));
    ai::UsageState expired;
    CHECK(expired.Apply({{24, -1, true, {reset - 7200, 0}}, now - 100, history}));
    CHECK(!expired.value.Available() && !expired.LastKnown());

    CHECK(ai::ClaudeStatusSettings(L"C:\\Program Files\\Wireless Battery\\WirelessBattery.exe")
        .find(L"powershell.exe -NoProfile -NonInteractive -EncodedCommand ") != std::wstring::npos);
    puts("AI usage tests passed");
}
