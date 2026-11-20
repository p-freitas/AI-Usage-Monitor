#include "../src/ai_usage.cpp"
#include "../src/ai_usage_events.cpp"
#include <cassert>
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
    assert(parser.parse(reply));
    auto usage = ai::ParseCodex(&reply);
    assert(usage.fiveHours == 0 && usage.week == 22);

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
    assert(refreshing && usage.fiveHours == 12 && usage.week == 42);
    screen += "\x1b[6;1H13% 13% used\x1b[K\x1b[9;1H43% 43% used\x1b[K\x1b[12;1H\x1b[K";
    usage = ai::ParseClaude(ai::TerminalRows(screen), refreshing);
    assert(!refreshing && usage.fiveHours == 13 && usage.week == 43);
    screen += "\x1b[9;1HUsage unavailable\x1b[K";
    usage = ai::ParseClaude(ai::TerminalRows(screen), refreshing);
    assert(usage.fiveHours == 13 && usage.week == -1);
    assert(ai::Format(usage) == L"5h 13%  7d \u2014");
    assert(ai::Format({75, 34, true}) == L"5h 75%  7d 34%");
    assert(ai::ClaudeStartupState(ai::TerminalRows("Starting Claude Code")) == ai::ClaudeStartup::Waiting);
    assert(ai::ClaudeStartupState(ai::TerminalRows("  $  ")) == ai::ClaudeStartup::Ready);
    assert(ai::ClaudeStartupState(ai::TerminalRows("Quick safety check\r\n  $  ")) == ai::ClaudeStartup::NeedsSetup);
    assert(ai::ClaudeStartupState(ai::TerminalRows("Select login method")) == ai::ClaudeStartup::NeedsSetup);

    auto live = ai::events::ParseCodexEvent(
        R"({"timestamp":"2026-11-19T20:00:01.123456Z","type":"event_msg","payload":{"type":"token_count","rate_limits":{"limit_id":"codex","primary":{"used_percent":25.5,"window_minutes":300,"resets_at":1795120000},"secondary":{"used_percent":44,"window_minutes":10080,"resets_at":1795500000}}}})");
    assert(live.observed == ai::events::RolloutTime("2026-11-19T20:00:01Z") + 1234560);
    assert(live.source == ai::UsageSource::Live && live.usage.fiveHours == 25 && live.usage.week == 44);
    assert(!ai::events::ParseCodexEvent(
        R"({"timestamp":"2026-11-19T20:00:01Z","type":"event_msg","payload":{"type":"token_count","rate_limits":{"limit_id":"other","primary":{"used_percent":99,"window_minutes":300}}}})"
    ).usage.Available());
    assert(!ai::events::ParseCodexEvent(R"({"type":"agent_message","message":"token_count"})").usage.Available());

    usage = ai::events::ParseClaudeStatus(
        R"({"context_window":{"used_percentage":99},"rate_limits":{"five_hour":{"used_percentage":0,"resets_at":1795120000},"seven_day":null}})");
    assert(usage.installed && usage.fiveHours == 0 && usage.week == -1);
    assert(!ai::events::ParseClaudeStatus(R"({"context_window":{"used_percentage":50}})").Available());

    ai::UsageState display;
    const auto now = ai::Now();
    const auto reset = (long long)((now - ai::events::EPOCH) / ai::events::TICKS_PER_SECOND) + 3600;
    assert(display.Apply({{20, 40, true, {reset, reset + 86400}}, now}));
    assert(display.Apply({{25, -1, true, {reset, 0}}, now + 3, ai::UsageSource::Live}));
    assert(!display.Apply({{23, 40, true, {reset, reset + 86400}}, now + 2}));
    assert(!display.Apply({{22, -1, true, {reset, 0}}, now + 4, ai::UsageSource::Live}));
    assert(!display.Apply({{25, -1, true, {reset, 0}}, now + 5, ai::UsageSource::Live}));
    assert(display.Apply({{26, 45, true, {reset, reset + 86400}}, now + 4}));
    assert(display.Apply({{0, -1, true, {reset + 18000, 0}}, now + 6, ai::UsageSource::Live}));
    assert(!display.Apply({{-1, -1, true}, now + 7}));
    assert(display.value.fiveHours == 0 && display.value.week == 45);

    ai::UsageState startup;
    const auto history = ai::UsageSource::LastKnown;
    assert(startup.Apply({{24, 41, true, {reset, reset + 86400}}, now - 100, history}));
    assert(startup.LastKnown() && startup.value.fiveHours == 24 && startup.value.week == 41);
    assert(startup.Apply({{24, 41, true, {reset, reset + 86400}}, now}));
    assert(!startup.LastKnown());
    assert(!startup.Apply({{26, 43, true, {reset, reset + 86400}}, now + 1, history}));
    ai::UsageState expired;
    assert(expired.Apply({{24, -1, true, {reset - 7200, 0}}, now - 100, history}));
    assert(!expired.value.Available() && !expired.LastKnown());

    assert(ai::ClaudeStatusSettings(L"C:\\Program Files\\Wireless Battery\\WirelessBattery.exe")
        .find(L"powershell.exe -NoProfile -NonInteractive -EncodedCommand ") != std::wstring::npos);
    puts("AI usage tests passed");
}
