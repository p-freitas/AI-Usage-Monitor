#pragma once
#include <windows.h>
#include <array>
#include <string>

namespace ai {

enum class Provider { Claude, Codex };
enum class UsageSource { Account, Live, LastKnown };
struct Usage {
    int fiveHours = -1;
    int week = -1;
    bool installed = false;
    std::array<long long, 2> resetsAt{};
    bool Available() const { return fiveHours >= 0 || week >= 0; }
};

struct UsageUpdate {
    Usage usage;
    ULONGLONG observed = 0;  // UTC FILETIME, for ordering live events and background reads
    UsageSource source = UsageSource::Account;
};

struct UsageState {
    Usage value;
    bool Apply(const UsageUpdate& update);
    bool LastKnown() const { return lastKnown_[0] || lastKnown_[1]; }

private:
    std::array<ULONGLONG, 2> observed_{};
    std::array<bool, 2> lastKnown_{};
};

constexpr ULONG_PTR CLAUDE_STATUS_MESSAGE = 0x5742434c; // fixed WM_COPYDATA payload tag
struct ClaudeStatusData {
    int fiveHours;
    int week;
    std::array<long long, 2> resetsAt;
};

ULONGLONG Now();
void WatchCodex(HANDLE stop, void (*publish)(const UsageUpdate&));
void SubmitClaudeStatus(HWND monitor);
std::wstring ClaudeStatusSettings(const std::wstring& exe);
// 0: success, 2: custom status line, 3: unreadable or invalid settings, 4: write failed.
int ConfigureClaudeStatus(const std::wstring& exe, bool enable);

const wchar_t* Name(Provider provider);
std::wstring Format(const Usage& usage);
bool IsInstalled(Provider provider);
Usage Poll(Provider provider, HANDLE stop = nullptr);
bool OpenClaudeSetup();

} // namespace ai
