#pragma once
#include <windows.h>
#include <array>
#include <string>

namespace ai {

enum class Provider { Claude, Codex, Gemini };
enum class UsageSource { Account, Live, LastKnown };
enum class Connection { Missing, Waiting, SignIn, Setup, Unavailable, Unsupported, NoLimits, Ready };
enum Limit : size_t { FiveHours, Week, ArgonWeek, LimitCount };
struct Usage {
    std::array<int, LimitCount> used;
    std::array<long long, LimitCount> resetsAt{};
    bool installed;
    bool fullSnapshot = false; // Omitted buckets in a successful account snapshot are unavailable.

    Usage(int five = -1, int weekly = -1, bool present = false,
          std::array<long long, 2> resets = {}) : installed(present) {
        used.fill(-1);
        used[FiveHours] = five;
        used[Week] = weekly;
        resetsAt[FiveHours] = resets[0];
        resetsAt[Week] = resets[1];
    }
    bool Available() const {
        for (int percent : used) if (percent >= 0) return true;
        return false;
    }
};

struct UsageUpdate {
    Usage usage;
    ULONGLONG observed = 0;  // UTC FILETIME, for ordering live events and background reads
    UsageSource source = UsageSource::Account;
    Connection connection = Connection::Waiting;
};

struct Reading {
    Usage usage;
    Connection connection = Connection::Missing;
};

struct UsageState {
    Usage value;
    bool Apply(const UsageUpdate& update);
    bool Expire(long long now);
    bool LastKnown() const {
        for (bool cached : lastKnown_) if (cached) return true;
        return false;
    }

    bool LastKnown(size_t limit) const { return lastKnown_[limit]; }

private:
    std::array<ULONGLONG, LimitCount> observed_{};
    std::array<bool, LimitCount> lastKnown_{};
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
std::wstring ClaudeTrustWorkspace(bool enable);
int ConfigureClaudeTrust(bool enable);

const wchar_t* Name(Provider provider);
bool IsInstalled(Provider provider);
Reading Poll(Provider provider, HANDLE stop = nullptr);
bool OpenClaudeSetup();
bool PrepareClaudeSetup();

} // namespace ai
