#include "ai_usage.h"
#include "utc_time.h"
#include "json_reader.h"

#include <algorithm>
#include <cstdio>
#include <string_view>
#include <vector>

namespace ai {
namespace events {

constexpr ULONGLONG EPOCH = 116444736000000000ULL;
constexpr ULONGLONG TICKS_PER_SECOND = 10000000;
constexpr ULONGLONG TICKS_PER_DAY = 24 * 60 * 60 * TICKS_PER_SECOND;
constexpr size_t MAX_TAIL = 256 * 1024;
constexpr size_t HISTORY_FILES = 16;

struct FileHandle {
    HANDLE handle = INVALID_HANDLE_VALUE;
    ~FileHandle() { if (handle && handle != INVALID_HANDLE_VALUE) CloseHandle(handle); }
    operator HANDLE() const { return handle; }
};

ULONGLONG FileTimeValue(const FILETIME& ft) {
    return (static_cast<ULONGLONG>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
}

long long UnixSeconds(ULONGLONG time) {
    return static_cast<long long>((time - EPOCH) / TICKS_PER_SECOND);
}

bool FieldIs(const json::Json* field, const char* text) {
    return field && field->type == json::Json::Str && field->str == text;
}

long long IntInRange(const json::Json* field, long long missing, double max) {
    if (!field || field->type != json::Json::Num || !(field->num >= 0 && field->num <= max))
        return missing;
    return static_cast<long long>(field->num);
}

void ReadWindow(const json::Json& window, const char* percentKey, int& percent, long long& reset) {
    auto used = IntInRange(window.get(percentKey), -1, 1e9);
    if (used < 0) return;
    percent = static_cast<int>(std::min(used, 100LL));
    reset = IntInRange(window.get("resets_at"), 0, 1e11);
}

UsageUpdate ParseCodexEvent(const std::string& line) {
    // Codex writes compact JSON. Avoid parsing unrelated conversation events.
    if (line.find("\"type\":\"token_count\"") == std::string::npos) return {};
    json::Json root;
    json::Parser parser{line.data(), line.data() + line.size()};
    if (!parser.parse(root) || !FieldIs(root.get("type"), "event_msg")) return {};
    const auto* timestamp = root.get("timestamp");
    const auto* payload = root.get("payload");
    if (!timestamp || timestamp->type != json::Json::Str || !payload ||
        !FieldIs(payload->get("type"), "token_count")) return {};
    const auto* limits = payload->get("rate_limits");
    if (!limits) return {};
    const auto* bucket = limits->get("limit_id");
    if (bucket && bucket->type == json::Json::Str && bucket->str != "codex") return {};
    UsageUpdate update{{}, UtcFileTime(timestamp->str), UsageSource::Live};
    if (!update.observed) return {};
    for (const char* key : {"primary", "secondary"}) {
        const auto* window = limits->get(key);
        if (!window) continue;
        switch (IntInRange(window->get("window_minutes"), -1, 10080)) {
            case 300: ReadWindow(*window, "used_percent", update.usage.used[ai::FiveHours], update.usage.resetsAt[0]); break;
            case 10080: ReadWindow(*window, "used_percent", update.usage.used[ai::Week], update.usage.resetsAt[1]); break;
        }
    }
    update.usage.installed = update.usage.Available();
    return update;
}

Usage ParseClaudeStatus(const std::string& input) {
    json::Json root;
    json::Parser parser{input.data(), input.data() + input.size()};
    if (!parser.parse(root)) return {};
    parser.ws();
    if (parser.p != parser.e) return {};
    const auto* limits = root.get("rate_limits");
    if (!limits) return {};
    Usage usage;
    if (const auto* fiveHours = limits->get("five_hour"))
        ReadWindow(*fiveHours, "used_percentage", usage.used[ai::FiveHours], usage.resetsAt[0]);
    if (const auto* week = limits->get("seven_day"))
        ReadWindow(*week, "used_percentage", usage.used[ai::Week], usage.resetsAt[1]);
    usage.installed = usage.Available();
    return usage;
}

template<class Receiver>
void ReadCodexTail(const std::wstring& path, const Receiver& receive) {
    FileHandle file{CreateFileW(path.c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT, nullptr)};
    FILE_ATTRIBUTE_TAG_INFO attributes{};
    LARGE_INTEGER size{};
    if (file.handle == INVALID_HANDLE_VALUE ||
        !GetFileInformationByHandleEx(file, FileAttributeTagInfo, &attributes, sizeof(attributes)) ||
        (attributes.FileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) ||
        !GetFileSizeEx(file, &size) || size.QuadPart <= 0) return;
    LARGE_INTEGER offset{};
    offset.QuadPart = std::max(0LL, size.QuadPart - (long long)MAX_TAIL);
    if (!SetFilePointerEx(file, offset, nullptr, FILE_BEGIN)) return;
    std::string tail(static_cast<size_t>(size.QuadPart - offset.QuadPart), '\0');
    DWORD got = 0;
    if (!ReadFile(file, tail.data(), static_cast<DWORD>(tail.size()), &got, nullptr)) return;
    tail.resize(got);
    size_t start = 0;
    if (offset.QuadPart) {
        start = tail.find('\n');
        if (start == std::string::npos) return;
        ++start;
    }
    ULONGLONG latestAllowed = Now() + 5 * TICKS_PER_SECOND;
    for (size_t end; (end = tail.find('\n', start)) != std::string::npos; start = end + 1) {
        auto update = ParseCodexEvent(tail.substr(start, end - start));
        if (update.usage.Available() && update.observed <= latestAllowed) receive(update);
    }
}

struct SessionFile {
    std::wstring path;
    ULONGLONG modified;
};

void FindRecentSessions(const std::wstring& dir, unsigned depth, HANDLE stop,
                        ULONGLONG cutoff, std::vector<SessionFile>& files) {
    WIN32_FIND_DATAW item{};
    auto pattern = dir + (depth == 3 ? L"\\rollout-*.jsonl" : L"\\*");
    HANDLE find = FindFirstFileW(pattern.c_str(), &item);
    if (find == INVALID_HANDLE_VALUE) return;
    do {
        if (WaitForSingleObject(stop, 0) != WAIT_TIMEOUT) break;
        if (item.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;
        std::wstring_view name(item.cFileName);
        bool folder = (item.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        if (depth == 3) {
            auto modified = FileTimeValue(item.ftLastWriteTime);
            if (!folder && modified >= cutoff) files.push_back({dir + L"\\" + item.cFileName, modified});
        } else if (folder && name.size() == (depth == 0 ? 4 : 2) &&
                   name.find_first_not_of(L"0123456789") == std::wstring_view::npos) {
            FindRecentSessions(dir + L"\\" + item.cFileName, depth + 1, stop, cutoff, files);
        }
    } while (FindNextFileW(find, &item));
    FindClose(find);
}

void ReplayCodexHistory(const std::wstring& dir, HANDLE stop, void (*publish)(const UsageUpdate&)) {
    const auto now = Now();
    const auto cutoff = now - 7 * TICKS_PER_DAY;
    std::vector<SessionFile> files;
    FindRecentSessions(dir, 0, stop, cutoff, files);
    const auto count = std::min(files.size(), HISTORY_FILES);
    std::partial_sort(files.begin(), files.begin() + count, files.end(),
                      [](const auto& a, const auto& b) { return a.modified > b.modified; });

    std::array<UsageUpdate, 2> newest{};
    for (size_t file = 0; file < count && WaitForSingleObject(stop, 0) == WAIT_TIMEOUT; ++file) {
        ReadCodexTail(files[file].path, [&](const UsageUpdate& update) {
            const auto& percentages = update.usage.used;
            for (size_t i = 0; i < newest.size(); ++i) {
                if (percentages[i] < 0 || update.observed < cutoff ||
                    update.observed <= newest[i].observed ||
                    update.usage.resetsAt[i] <= UnixSeconds(now)) continue;
                newest[i] = update;
                newest[i].source = UsageSource::LastKnown;
                (i == 0 ? newest[i].usage.used[ai::Week] : newest[i].usage.used[ai::FiveHours]) = -1;
            }
        });
    }
    for (const auto& update : newest)
        if (update.usage.Available()) publish(update);
}

void WatchDirectory(const std::wstring& dir, HANDLE stop, bool& replayed, std::array<ULONGLONG, 2>& latest,
                    void (*publish)(const UsageUpdate&)) {
    FileHandle directory{CreateFileW(dir.c_str(), FILE_LIST_DIRECTORY,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, nullptr)};
    FileHandle ready{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    if (directory.handle == INVALID_HANDLE_VALUE || !ready.handle) return;
    alignas(DWORD) BYTE buffer[64 * 1024];
    while (WaitForSingleObject(stop, 0) == WAIT_TIMEOUT) {
        OVERLAPPED request{};
        request.hEvent = ready;
        ResetEvent(ready);
        if (!ReadDirectoryChangesW(directory, buffer, sizeof(buffer), TRUE, FILE_NOTIFY_CHANGE_FILE_NAME |
            FILE_NOTIFY_CHANGE_SIZE | FILE_NOTIFY_CHANGE_LAST_WRITE, nullptr, &request, nullptr)) return;
        if (!replayed) {
            replayed = true;
            ReplayCodexHistory(dir, stop, publish);
        }
        const HANDLE handles[] = {stop, ready};
        if (WaitForMultipleObjects(2, handles, FALSE, INFINITE) != WAIT_OBJECT_0 + 1) {
            CancelIoEx(directory, &request);
            DWORD ignored;
            GetOverlappedResult(directory, &request, &ignored, TRUE);
            return;
        }
        DWORD bytes = 0;
        if (!GetOverlappedResult(directory, &request, &bytes, FALSE)) return;
        for (DWORD offset = 0; offset < bytes;) {
            auto* item = reinterpret_cast<const FILE_NOTIFY_INFORMATION*>(buffer + offset);
            if (item->Action == FILE_ACTION_MODIFIED || item->Action == FILE_ACTION_ADDED ||
                item->Action == FILE_ACTION_RENAMED_NEW_NAME) {
                std::wstring name(item->FileName, item->FileNameLength / sizeof(wchar_t));
                const size_t lastSlash = name.find_last_of(L"\\/");
                auto file = std::wstring_view(name).substr(lastSlash == std::wstring::npos ? 0 : lastSlash + 1);
                if (file.size() > 14 && file.substr(0, 8) == L"rollout-" &&
                    file.substr(file.size() - 6) == L".jsonl") {
                    ReadCodexTail(dir + L"\\" + name, [&](UsageUpdate update) {
                        auto& percentages = update.usage.used;
                        for (size_t i = 0; i < latest.size(); ++i) {
                            if (update.observed <= latest[i]) percentages[i] = -1;
                            else if (percentages[i] >= 0) latest[i] = update.observed;
                        }
                        if (update.usage.Available()) publish(update);
                    });
                }
            }
            if (!item->NextEntryOffset) break;
            offset += item->NextEntryOffset;
            if (WaitForSingleObject(stop, 0) != WAIT_TIMEOUT) break;
        }
    }
}

} // namespace events

ULONGLONG Now() {
    FILETIME ft{};
    GetSystemTimePreciseAsFileTime(&ft);
    return events::FileTimeValue(ft);
}

bool UsageState::Expire(long long now) {
    bool expired = false;
    for (size_t i = 0; i < value.used.size(); ++i) {
        if (value.resetsAt[i] <= 0 || value.resetsAt[i] > now) continue;
        value.used[i] = -1;
        value.resetsAt[i] = 0;
        lastKnown_[i] = false;
        expired = true;
    }
    return expired;
}

bool UsageState::Apply(const UsageUpdate& update) {
    bool installed = update.usage.fullSnapshot ? update.usage.installed : value.installed || update.usage.installed;
    bool changed = installed != value.installed;
    value.installed = installed;
    const auto now = events::UnixSeconds(Now());
    const bool fromHistory = update.source == UsageSource::LastKnown;
    for (size_t i = 0; i < observed_.size(); ++i) {
        if (lastKnown_[i] && value.resetsAt[i] <= now) {
            value.used[i] = -1;
            observed_[i] = 0;
            value.resetsAt[i] = 0;
            lastKnown_[i] = false;
            changed = true;
        }
        int next = update.usage.used[i];
        if (update.usage.fullSnapshot && update.observed > observed_[i] && next < 0) {
            changed |= value.used[i] >= 0;
            value.used[i] = -1;
            value.resetsAt[i] = 0;
            lastKnown_[i] = false;
            observed_[i] = update.observed;
            continue;
        }
        if (next < 0 || next > 100 || !update.observed || update.observed <= observed_[i]) continue;
        auto reset = update.usage.resetsAt[i];
        auto previousReset = value.resetsAt[i];
        if (fromHistory && (reset <= now || value.used[i] >= 0)) continue;
        if (update.source == UsageSource::Live) {
            if (reset > 0 && (reset <= now || reset < previousReset)) continue;
            // A second active session can publish a cached lower percentage for the same window.
            if (!lastKnown_[i] && next <= value.used[i] && (!reset || reset == previousReset)) continue;
        }
        observed_[i] = update.observed;
        if (reset > 0 || update.usage.fullSnapshot) value.resetsAt[i] = reset;
        changed |= value.used[i] != next || lastKnown_[i] != fromHistory || value.resetsAt[i] != previousReset;
        lastKnown_[i] = fromHistory;
        value.used[i] = next;
    }
    return changed;
}

void WatchCodex(HANDLE stop, void (*publish)(const UsageUpdate&)) {
    std::wstring home(32768, L'\0');
    DWORD length = GetEnvironmentVariableW(L"CODEX_HOME", home.data(), (DWORD)home.size());
    if (length && length < home.size()) {
        home.resize(length);
    } else {
        length = GetEnvironmentVariableW(L"USERPROFILE", home.data(), (DWORD)home.size());
        if (!length || length >= home.size()) return;
        home.resize(length);
        home += L"\\.codex";
    }
    auto start = Now();
    std::array<ULONGLONG, 2> latest = {start, start};
    bool replayed = false;
    do {
        events::WatchDirectory(home + L"\\sessions", stop, replayed, latest, publish);
    } while (WaitForSingleObject(stop, 2000) == WAIT_TIMEOUT);
}

void SubmitClaudeStatus(HWND monitor) {
    if (!monitor) return;
    HANDLE stdinHandle = GetStdHandle(STD_INPUT_HANDLE);
    DWORD type = GetFileType(stdinHandle);
    if (type != FILE_TYPE_PIPE && type != FILE_TYPE_DISK) return;
    std::string input;
    char buffer[4096];
    for (DWORD got; ReadFile(stdinHandle, buffer, sizeof(buffer), &got, nullptr) && got;) {
        input.append(buffer, got);
        if (input.size() > 64 * 1024) return;
    }
    auto reading = events::ParseClaudeStatus(input);
    if (!reading.Available()) return;
    ClaudeStatusData data{reading.used[ai::FiveHours], reading.used[ai::Week], {reading.resetsAt[0], reading.resetsAt[1]}};
    COPYDATASTRUCT message{CLAUDE_STATUS_MESSAGE, sizeof(data), &data};
    SendMessageTimeoutW(monitor, WM_COPYDATA, 0, reinterpret_cast<LPARAM>(&message),
                        SMTO_ABORTIFHUNG | SMTO_BLOCK, 500, nullptr);
}

} // namespace ai
