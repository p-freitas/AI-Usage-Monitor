#include "ai_usage.h"
#include "app_version.h"
#include "json_reader.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <ctime>
#include <cwchar>
#include <string_view>
#include <vector>

namespace ai {
namespace {

constexpr DWORD QUERY_MS = 16000;
constexpr size_t MAX_OUTPUT = 256 * 1024;
constexpr short TERMINAL_COLUMNS = 120;
constexpr short TERMINAL_ROWS = 42;

struct Handle {
    HANDLE value = nullptr;
    ~Handle() { Close(); }
    operator HANDLE() const { return value; }
    void Close() { if (value) CloseHandle(value); value = nullptr; }
};

std::wstring Env(const wchar_t* name) {
    DWORD n = GetEnvironmentVariableW(name, nullptr, 0);
    std::wstring value(n, L'\0');
    if (!n) return value;
    DWORD used = GetEnvironmentVariableW(name, value.data(), n);
    value.resize(used < n ? used : 0);
    return value;
}

std::wstring ClaudeWorkspacePath() {
    auto base = Env(L"LOCALAPPDATA");
    wchar_t full[MAX_PATH];
    DWORD length = GetFullPathNameW(base.c_str(), MAX_PATH, full, nullptr);
    if (!length || length >= MAX_PATH || base != full ||
        base.size() < 3 || base[1] != L':' || base[2] != L'\\') return {};
    return base + L"\\AIUsageMonitor\\ClaudeCode";
}

bool EnsureDirectory(const std::wstring& path) {
    if (!CreateDirectoryW(path.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) return false;
    DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES &&
        (attributes & FILE_ATTRIBUTE_DIRECTORY) && !(attributes & FILE_ATTRIBUTE_REPARSE_POINT);
}

std::wstring ClaudeWorkDir() {
    auto path = ClaudeWorkspacePath();
    if (path.empty() || !EnsureDirectory(path.substr(0, path.rfind(L'\\'))) || !EnsureDirectory(path))
        return {};
    WIN32_FIND_DATAW entry{};
    HANDLE search = FindFirstFileW((path + L"\\*").c_str(), &entry);
    if (search == INVALID_HANDLE_VALUE)
        return GetLastError() == ERROR_FILE_NOT_FOUND ? path : L"";
    do {
        if (wcscmp(entry.cFileName, L".") != 0 && wcscmp(entry.cFileName, L"..") != 0) {
            FindClose(search);
            return {};
        }
    } while (FindNextFileW(search, &entry));
    bool empty = GetLastError() == ERROR_NO_MORE_FILES;
    FindClose(search);
    return empty ? path : L"";
}

std::wstring ClaudeArgs() {
    return LR"( --safe-mode --setting-sources "" --strict-mcp-config --mcp-config "{\"mcpServers\":{}}" --settings "{\"disableAllHooks\":true,\"language\":\"english\"}" --tools "" --no-chrome)";
}

std::wstring ClaudeEnvironment() {
    constexpr std::wstring_view HISTORY = L"CLAUDE_CODE_SKIP_PROMPT_HISTORY=";
    // The blanket nonessential-traffic flag also prevents /usage from refreshing
    // the account meters. Disable the updater without disabling quota reads.
    constexpr std::wstring_view UPDATE = L"DISABLE_AUTOUPDATER=";
    std::vector<std::wstring> entries = {std::wstring(HISTORY) + L"1", std::wstring(UPDATE) + L"1"};
    auto env = GetEnvironmentStringsW();
    if (!env) return {};
    for (const wchar_t* entry = env; *entry; entry += wcslen(entry) + 1) {
        if (_wcsnicmp(entry, HISTORY.data(), HISTORY.size()) == 0 ||
            _wcsnicmp(entry, UPDATE.data(), UPDATE.size()) == 0 ||
            _wcsnicmp(entry, L"CLAUDECODE=", 11) == 0) continue;
        entries.emplace_back(entry);
    }
    FreeEnvironmentStringsW(env);
    std::sort(entries.begin(), entries.end(), [](const auto& a, const auto& b) {
        return _wcsicmp(a.c_str(), b.c_str()) < 0;
    });
    std::wstring block;
    for (const auto& entry : entries) {
        block += entry;
        block.push_back(L'\0');
    }
    return block;
}

std::wstring FindCli(Provider provider) {
    const wchar_t* exe = provider == Provider::Claude ? L"claude.exe" : L"codex.exe";
    auto path = Env(L"PATH");
    for (size_t start = 0; start < path.size();) {
        size_t end = path.find(L';', start);
        auto dir = path.substr(start, end - start);
        if (dir.size() > 1 && dir.front() == L'"' && dir.back() == L'"')
            dir = dir.substr(1, dir.size() - 2);
        if (dir.size() > 2 && ((dir[1] == L':' && (dir[2] == L'\\' || dir[2] == L'/')) ||
                               dir.compare(0, 2, L"\\\\") == 0)) {
            auto candidate = dir + L"\\" + exe;
            DWORD attr = GetFileAttributesW(candidate.c_str());
            if (attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY)) return candidate;
        }
        if (end == std::wstring::npos) break;
        start = end + 1;
    }
    auto root = Env(provider == Provider::Claude ? L"USERPROFILE" : L"LOCALAPPDATA");
    if (root.empty()) return {};
    root += provider == Provider::Claude ? L"\\.local\\bin\\claude.exe"
                                         : L"\\Programs\\OpenAI\\Codex\\bin\\codex.exe";
    return GetFileAttributesW(root.c_str()) == INVALID_FILE_ATTRIBUTES ? L"" : root;
}

std::wstring FindCodexDesktopCli() {
    auto root = Env(L"LOCALAPPDATA");
    if (root.empty()) return {};
    root += L"\\OpenAI\\Codex\\bin";
    WIN32_FIND_DATAW entry{};
    HANDLE find = FindFirstFileW((root + L"\\*").c_str(), &entry);
    if (find == INVALID_HANDLE_VALUE) return {};
    std::wstring newest;
    FILETIME newestTime{};
    do {
        std::wstring_view name(entry.cFileName);
        if (name.empty() || name.front() == L'.' || !(entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
            (entry.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) continue;
        auto exe = root + L"\\" + entry.cFileName + L"\\codex.exe";
        WIN32_FILE_ATTRIBUTE_DATA file{};
        if (GetFileAttributesExW(exe.c_str(), GetFileExInfoStandard, &file) &&
            !(file.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) &&
            (newest.empty() || CompareFileTime(&file.ftLastWriteTime, &newestTime) > 0)) {
            newest = std::move(exe);
            newestTime = file.ftLastWriteTime;
        }
    } while (FindNextFileW(find, &entry));
    FindClose(find);
    return newest;
}

// A short-lived child owns the installed CLI authentication. Only quota output is consumed.
class Cli {
    using CreateConsole = HRESULT (WINAPI*)(COORD, HANDLE, HANDLE, DWORD, HPCON*);
    using CloseConsole = void (WINAPI*)(HPCON);
    HPCON console_ = nullptr;
    CloseConsole closeConsole_ = nullptr;
    Handle process_, job_, input_, output_;
    ULONGLONG deadline_ = GetTickCount64() + QUERY_MS;
    HANDLE stop_;

public:
    explicit Cli(HANDLE stop) : stop_(stop) {}
    ~Cli() {
        job_.Close();
        input_.Close();
        output_.Close();
        if (console_) closeConsole_(console_);
    }

    bool Start(const std::wstring& exe, Provider provider) {
        bool terminal = provider == Provider::Claude;
        std::wstring cwd = terminal ? ClaudeWorkDir() : Env(L"USERPROFILE");
        if (terminal && cwd.empty()) return false;
        Handle childInput, childOutput;
        if (!CreatePipe(&childInput.value, &input_.value, nullptr, 0) ||
            !CreatePipe(&output_.value, &childOutput.value, nullptr, 0)) return false;

        if (terminal) {
            auto kernel = GetModuleHandleW(L"kernel32.dll");
            auto create = reinterpret_cast<CreateConsole>(GetProcAddress(kernel, "CreatePseudoConsole"));
            closeConsole_ = reinterpret_cast<CloseConsole>(GetProcAddress(kernel, "ClosePseudoConsole"));
            if (!create || !closeConsole_ ||
                FAILED(create({TERMINAL_COLUMNS, TERMINAL_ROWS}, childInput, childOutput, 0, &console_))) return false;
        } else if (!SetHandleInformation(childInput, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT) ||
                   !SetHandleInformation(childOutput, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT)) {
            return false;
        }

        SIZE_T bytes = 0;
        InitializeProcThreadAttributeList(nullptr, 1, 0, &bytes);
        std::vector<unsigned char> storage(bytes);
        auto attrs = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
        if (!InitializeProcThreadAttributeList(attrs, 1, 0, &bytes)) return false;
        STARTUPINFOEXW startup{};
        startup.StartupInfo.cb = sizeof(startup);
        startup.lpAttributeList = attrs;
        HANDLE streamHandles[] = {childInput, childOutput};
        if (!terminal) {
            startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
            startup.StartupInfo.hStdInput = childInput;
            startup.StartupInfo.hStdOutput = childOutput;
            startup.StartupInfo.hStdError = childOutput;
        }
        bool configured = terminal
            ? UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE,
                                        console_, sizeof(console_), nullptr, nullptr)
            : UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                        streamHandles, sizeof(streamHandles), nullptr, nullptr);

        std::wstring args = L"\"" + exe + L"\"";
        args += terminal ? ClaudeArgs() + L" --ax-screen-reader" : L" app-server --listen stdio://";
        auto env = terminal ? ClaudeEnvironment() : std::wstring{};
        PROCESS_INFORMATION info{};
        job_.value = CreateJobObjectW(nullptr, nullptr);
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        bool ready = configured && job_.value &&
            SetInformationJobObject(job_, JobObjectExtendedLimitInformation, &limits, sizeof(limits));
        BOOL started = ready && CreateProcessW(exe.c_str(), args.data(), nullptr, nullptr, !terminal,
            EXTENDED_STARTUPINFO_PRESENT | CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT |
                (terminal ? 0 : CREATE_NO_WINDOW),
            env.empty() ? nullptr : env.data(), cwd.empty() ? nullptr : cwd.c_str(),
            &startup.StartupInfo, &info);
        DeleteProcThreadAttributeList(attrs);
        if (!started) return false;
        process_.value = info.hProcess;
        Handle thread{info.hThread};
        if (!AssignProcessToJobObject(job_, process_) || ResumeThread(thread) == DWORD(-1)) {
            TerminateProcess(process_, 1);
            return false;
        }
        return true;
    }

    bool Send(std::string_view input) {
        DWORD written = 0;
        return WriteFile(input_, input.data(), (DWORD)input.size(), &written, nullptr) &&
            written == input.size();
    }

    bool Read(std::string& result) {
        DWORD available = 0;
        if (GetTickCount64() >= deadline_ || result.size() >= MAX_OUTPUT ||
            (stop_ && WaitForSingleObject(stop_, 0) == WAIT_OBJECT_0) ||
            !PeekNamedPipe(output_, nullptr, 0, nullptr, &available, nullptr)) return false;
        if (!available) {
            return WaitForSingleObject(process_, 30) == WAIT_TIMEOUT;
        }
        char buffer[4096];
        DWORD received = 0;
        if (!ReadFile(output_, buffer, std::min<DWORD>(available, sizeof(buffer)), &received, nullptr))
            return false;
        result.append(buffer, received);
        return true;
    }
};

const json::Json* Object(const json::Json* parent, const char* field) {
    return parent && parent->type == json::Json::Obj ? parent->get(field) : nullptr;
}

bool Number(const json::Json* value, double expected) {
    return value && value->type == json::Json::Num && value->num == expected;
}

Usage ParseCodex(const json::Json* reply) {
    const auto* bucket = Object(Object(reply, "rateLimitsByLimitId"), "codex");
    if (!bucket) bucket = Object(reply, "rateLimits");
    Usage usage;
    for (const char* name : {"primary", "secondary"}) {
        const auto* window = Object(bucket, name);
        const auto* minutes = Object(window, "windowDurationMins");
        const auto* used = Object(window, "usedPercent");
        if (!used || used->type != json::Json::Num || !std::isfinite(used->num) || used->num < 0) continue;
        int percent = (int)std::min(100.0, used->num);
        const auto* reset = Object(window, "resetsAt");
        long long resetsAt = reset && reset->type == json::Json::Num &&
            reset->num > 0 && reset->num < 1e11 ? (long long)reset->num : 0;
        if (Number(minutes, 300)) {
            usage.fiveHours = percent;
            usage.resetsAt[0] = resetsAt;
        }
        if (Number(minutes, 10080)) {
            usage.week = percent;
            usage.resetsAt[1] = resetsAt;
        }
    }
    return usage;
}

bool CodexNeedsSignIn(const json::Json* result) {
    const auto* account = Object(result, "account");
    const auto* auth = Object(result, "requiresOpenaiAuth");
    return (!account || account->type == json::Json::Null) &&
        auth && auth->type == json::Json::Bool && auth->num;
}

Reading ReadCodex(Cli& cli) {
    if (!cli.Send(R"({"id":1,"method":"initialize","params":{"clientInfo":{"name":"ai_usage_monitor","version":")"
                  AUM_VERSION_STRING R"("}}})" "\n"))
        return {{}, Connection::Unavailable};
    std::string response;
    size_t start = 0;
    while (cli.Read(response)) {
        size_t end;
        while ((end = response.find('\n', start)) != std::string::npos) {
            auto line = response.substr(start, end - start);
            start = end + 1;
            json::Json message;
            json::Parser parser{line.data(), line.data() + line.size()};
            if (!parser.parse(message)) continue;
            const auto* result = Object(&message, "result");
            const auto* id = Object(&message, "id");
            if (Number(id, 1) && result) {
                if (!cli.Send(R"({"method":"initialized"})" "\n"
                              R"({"id":2,"method":"account/read","params":{"refreshToken":false}})" "\n"))
                    return {{}, Connection::Unavailable};
            }
            if (Number(id, 2)) {
                if (CodexNeedsSignIn(result)) return {{}, Connection::SignIn};
                if (!cli.Send(R"({"id":3,"method":"account/rateLimits/read"})" "\n"))
                    return {{}, Connection::Unavailable};
            }
            if (Number(id, 3)) {
                auto usage = ParseCodex(result);
                return {usage, usage.Available() ? Connection::Ready :
                    (result ? Connection::Waiting : Connection::Unavailable)};
            }
        }
    }
    return {{}, Connection::Unavailable};
}

// Rebuild the accessibility screen because ConPTY sends only the changed rows
// when Claude replaces the cached percentages with the refreshed values.
std::array<std::string, TERMINAL_ROWS> TerminalRows(const std::string& output) {
    std::array<std::string, TERMINAL_ROWS> rows;
    int row = 0, col = 0;
    for (size_t i = 0; i < output.size(); ++i) {
        unsigned char c = output[i];
        if (c == 27 && i + 1 < output.size()) {
            if (output[i + 1] == '[') {
                size_t first = i + 2;
                i = first;
                while (i < output.size() && (output[i] < '@' || output[i] > '~')) ++i;
                if (i == output.size()) break;
                char* next = nullptr;
                long n = std::strtol(output.c_str() + first, &next, 10);
                switch (output[i]) {
                    case 'H':
                    case 'f': {
                        long m = *next == ';' ? std::strtol(next + 1, nullptr, 10) : 1;
                        row = (int)std::clamp(n, 1L, (long)TERMINAL_ROWS) - 1;
                        col = (int)std::clamp(m, 1L, (long)TERMINAL_COLUMNS) - 1;
                        break;
                    }
                    case 'J':
                        if (n == 2 || n == 3) rows = {};
                        else if (n == 0) {
                            rows[row].resize(std::min(rows[row].size(), (size_t)col));
                            for (int j = row + 1; j < TERMINAL_ROWS; ++j) rows[j].clear();
                        }
                        break;
                    case 'K':
                        if (n == 2) rows[row].clear();
                        else if (n == 0) rows[row].resize(std::min(rows[row].size(), (size_t)col));
                        break;
                    case 'X': {
                        auto& line = rows[row];
                        if ((size_t)col < line.size()) {
                            auto count = std::min(line.size() - col, (size_t)std::max(1L, n));
                            line.replace(col, count, count, ' ');
                        }
                        break;
                    }
                }
            } else if (output[i + 1] == ']') {
                for (i += 2; i < output.size() && output[i] != '\a'; ++i) {
                    if (output[i] == 27 && i + 1 < output.size() && output[i + 1] == '\\') {
                        ++i;
                        break;
                    }
                }
            } else {
                ++i;
            }
        } else if (c == '\r') {
            col = 0;
        } else if (c == '\n') {
            if (row < TERMINAL_ROWS - 1) ++row;
            else {
                std::rotate(rows.begin(), rows.begin() + 1, rows.end());
                rows.back().clear();
            }
        } else if (c >= 32 && (c & 0xc0) != 0x80 && col < TERMINAL_COLUMNS) {
            // Usage labels and percentages are ASCII. Count other UTF-8 glyphs as one column.
            auto& line = rows[row];
            if ((size_t)col >= line.size()) line.resize(col + 1, ' ');
            line[col++] = c < 128 ? static_cast<char>(c) : ' ';
        }
    }
    return rows;
}

Usage ParseClaude(const std::array<std::string, TERMINAL_ROWS>& rows, bool& refreshing) {
    Usage usage;
    refreshing = false;
    for (size_t i = 0; i + 1 < rows.size(); ++i) {
        const auto& title = rows[i];
        if (title.find("Refreshing usage") != std::string::npos) refreshing = true;
        bool session = title.find("Current session") != std::string::npos;
        bool week = title.find("Current week (all models)") != std::string::npos;
        if (!session && !week) continue;
        const auto& meter = rows[i + 1];
        size_t end = meter.find("% used");
        if (end == std::string::npos) continue;
        size_t start = end;
        while (start && meter[start - 1] >= '0' && meter[start - 1] <= '9') --start;
        if (end - start >= 1 && end - start <= 3)
            (session ? usage.fiveHours : usage.week) = std::min(100, std::stoi(meter.substr(start)));
    }
    return usage;
}

bool ContainsText(const std::array<std::string, TERMINAL_ROWS>& rows, const char* text) {
    return std::any_of(rows.begin(), rows.end(), [&](const auto& row) {
        return row.find(text) != std::string::npos;
    });
}

enum class ClaudeStartup { Waiting, NeedsSetup, NeedsSignIn, Ready };

ClaudeStartup ClaudeStartupState(const std::array<std::string, TERMINAL_ROWS>& rows) {
    if (ContainsText(rows, "Select login method") || ContainsText(rows, "Please run /login") ||
        ContainsText(rows, "Not logged in")) return ClaudeStartup::NeedsSignIn;
    if (ContainsText(rows, "Choose the text style") || ContainsText(rows, "Quick safety check") ||
        ContainsText(rows, "Do you trust the files in this folder?") ||
        ContainsText(rows, "Yes, I trust this folder"))
        return ClaudeStartup::NeedsSetup;
    for (const auto& row : rows) {
        if (row.find('$') != std::string::npos && row.find_first_not_of(" $") == std::string::npos)
            return ClaudeStartup::Ready;
    }
    return ClaudeStartup::Waiting;
}

Reading ReadClaude(Cli& cli) {
    std::string output;
    size_t processed = 0;
    bool typed = false, submitted = false, refreshing = false;
    ULONGLONG lastChange = 0;
    Usage usage;
    while (cli.Read(output)) {
        if (processed != output.size()) {
            processed = output.size();
            lastChange = GetTickCount64();
            auto rows = TerminalRows(output);
            if (submitted) {
                usage = ParseClaude(rows, refreshing);
            } else {
                auto startup = ClaudeStartupState(rows);
                if (startup == ClaudeStartup::NeedsSetup) return {{}, Connection::Setup};
                if (startup == ClaudeStartup::NeedsSignIn) return {{}, Connection::SignIn};
                if (!typed && startup == ClaudeStartup::Ready) {
                    if (!cli.Send("/usage")) return {{}, Connection::Unavailable};
                    typed = true;
                }
                // Confirm the built-in command in autocomplete before pressing Enter.
                if (typed && ContainsText(rows, "Show session cost, plan usage")) {
                    if (!cli.Send("\r")) return {{}, Connection::Unavailable};
                    submitted = true;
                }
            }
        }
        if (usage.Available() && !refreshing && GetTickCount64() - lastChange >= 250)
            return {usage, Connection::Ready};
    }
    if (refreshing) return {{}, Connection::Waiting};
    return {usage, usage.Available() ? Connection::Ready :
        (submitted ? Connection::Waiting : Connection::Unavailable)};
}

} // namespace

std::wstring ClaudeTrustWorkspace(bool enable) {
    return enable ? ClaudeWorkDir() : ClaudeWorkspacePath();
}

const wchar_t* Name(Provider provider) {
    return provider == Provider::Claude ? L"Claude" : L"Codex";
}

std::wstring Format(const Usage& usage) {
    auto percent = [](int value) {
        return value >= 0 ? std::to_wstring(value) + L"%" : std::wstring(L"\u2014");
    };
    return L"5h " + percent(usage.fiveHours) + L"  7d " + percent(usage.week);
}

std::wstring FormatResets(const Usage& usage) {
    std::wstring text;
    for (size_t i = 0; i < usage.resetsAt.size(); ++i) {
        const time_t reset = usage.resetsAt[i];
        std::tm local{};
        if (reset <= 0 || localtime_s(&local, &reset) != 0) continue;

        if (!text.empty()) text += L", ";
        wchar_t time[6];
        if (i == 0) {
            std::wcsftime(time, ARRAYSIZE(time), L"%H:%M", &local);
            text += L"5h ";
        } else {
            std::wcsftime(time, ARRAYSIZE(time), L"%Hh", &local);
            text += L"7d ";
            SYSTEMTIME date{};
            date.wYear = local.tm_year + 1900;
            date.wMonth = local.tm_mon + 1;
            date.wDay = local.tm_mday;
            wchar_t weekday[64];
            if (GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, 0, &date, L"dddd",
                                weekday, ARRAYSIZE(weekday), nullptr)) {
                text += weekday;
                text += L" ";
            }
        }
        text += time;
    }
    return text;
}

bool IsInstalled(Provider provider) {
    return !FindCli(provider).empty() ||
        (provider == Provider::Codex && !FindCodexDesktopCli().empty());
}

Reading Poll(Provider provider, HANDLE stop) {
    auto read = [&](const std::wstring& exe) {
        if (exe.empty()) return Reading{};
        Reading reading{{}, Connection::Unavailable};
        Cli cli(stop);
        if (cli.Start(exe, provider))
            reading = provider == Provider::Claude ? ReadClaude(cli) : ReadCodex(cli);
        reading.usage.installed = true;
        return reading;
    };
    auto exe = FindCli(provider);
    auto usage = read(exe);
    if (provider == Provider::Codex && !usage.usage.Available() &&
        (!stop || WaitForSingleObject(stop, 0) == WAIT_TIMEOUT)) {
        auto desktop = FindCodexDesktopCli();
        if (!desktop.empty() && _wcsicmp(exe.c_str(), desktop.c_str()) != 0) {
            auto fallback = read(desktop);
            if (fallback.connection != Connection::Unavailable || usage.connection != Connection::SignIn)
                usage = fallback;
        }
    }
    return usage;
}

bool OpenClaudeSetup() {
    auto exe = FindCli(Provider::Claude);
    auto dir = ClaudeWorkDir();
    if (exe.empty() || dir.empty()) return false;
    auto args = L"\"" + exe + L"\"" + ClaudeArgs();
    auto env = ClaudeEnvironment();
    STARTUPINFOW startup{sizeof(startup)};
    PROCESS_INFORMATION info{};
    if (!CreateProcessW(exe.c_str(), args.data(), nullptr, nullptr, FALSE,
                        CREATE_NEW_CONSOLE | CREATE_UNICODE_ENVIRONMENT,
                        env.empty() ? nullptr : env.data(), dir.c_str(), &startup, &info)) return false;
    CloseHandle(info.hThread);
    CloseHandle(info.hProcess);
    return true;
}

bool PrepareClaudeSetup() {
    auto exe = FindCli(Provider::Claude);
    if (exe.empty()) return false;
    auto startup = ClaudeStartup::Waiting;
    {
        Cli cli(nullptr);
        if (!cli.Start(exe, Provider::Claude)) return false;
        std::string output;
        size_t processed = 0;
        while (cli.Read(output)) {
            if (output.size() == processed) continue;
            processed = output.size();
            startup = ClaudeStartupState(TerminalRows(output));
            if (startup != ClaudeStartup::Waiting) break;
        }
    }
    return startup == ClaudeStartup::Ready ||
        ((startup == ClaudeStartup::NeedsSetup || startup == ClaudeStartup::NeedsSignIn) && OpenClaudeSetup());
}

} // namespace ai
