#include "../src/update_core.h"
#include "../src/update_win.h"
#include "../src/update_apply.h"
#include <winhttp.h>

namespace httpTest {
bool rejectTimeouts = false, rejectRedirectPolicy = false;
unsigned sends = 0;

BOOL WINAPI SetTimeouts(HINTERNET session, int resolve, int connect, int send, int receive) {
    if (rejectTimeouts) return FALSE;
    return WinHttpSetTimeouts(session, resolve, connect, send, receive);
}

BOOL WINAPI SetOption(HINTERNET request, DWORD option, LPVOID value, DWORD size) {
    if (rejectRedirectPolicy && option == WINHTTP_OPTION_REDIRECT_POLICY) return FALSE;
    return WinHttpSetOption(request, option, value, size);
}

BOOL WINAPI SendRequest(HINTERNET, LPCWSTR, DWORD, LPVOID, DWORD, DWORD, DWORD_PTR) {
    ++sends;
    SetLastError(ERROR_WINHTTP_CANNOT_CONNECT);
    return FALSE; // No network traffic, even if a configuration regression reaches this call.
}
}

// Exercise private callbacks without adding test-only entry points to the application.
#define WinHttpSetTimeouts httpTest::SetTimeouts
#define WinHttpSetOption httpTest::SetOption
#define WinHttpSendRequest httpTest::SendRequest
#include "../src/update_win.cpp"
#undef WinHttpSetTimeouts
#undef WinHttpSetOption
#undef WinHttpSendRequest
#include "../src/update_apply.cpp"
#include <shellapi.h>
#include "check.h"
#include <cstdio>

int main() {
    httpTest::rejectTimeouts = true;
    auto rejectedTimeouts = updates::FetchRelease();
    CHECK(!rejectedTimeouts.success && !rejectedTimeouts.metadataValid && httpTest::sends == 0);
    httpTest::rejectTimeouts = false;
    httpTest::rejectRedirectPolicy = true;
    auto rejectedRedirects = updates::FetchRelease();
    CHECK(!rejectedRedirects.success && !rejectedRedirects.metadataValid && httpTest::sends == 0);
    httpTest::rejectRedirectPolicy = false;
    auto offline = updates::FetchRelease();
    CHECK(!offline.success && !offline.metadataValid && httpTest::sends == 1);

    // A real Cancel/Escape after the worker finishes must still veto installation.
    updates::done = true;
    updates::cancelled = false;
    CHECK(updates::DownloadDialog(nullptr, TDN_BUTTON_CLICKED, IDCANCEL, 0, 0) == S_OK);
    CHECK(updates::cancelled);
    updates::done = false;
    updates::cancelled = false;
    CHECK(updates::DownloadDialog(nullptr, TDN_BUTTON_CLICKED, IDCANCEL, 0, 0) == S_FALSE);
    CHECK(updates::cancelled);
    updates::done = true;
    updates::cancelled = false;
    updates::DownloadDialogState dialogState;
    dialogState.automaticClose = true;
    CHECK(updates::DownloadDialog(nullptr, TDN_BUTTON_CLICKED, IDCANCEL, 0,
        reinterpret_cast<LONG_PTR>(&dialogState)) == S_OK);
    CHECK(!updates::cancelled); // Only the timer's automatic close may continue to installation.
    dialogState.automaticClose = false;
    CHECK(updates::DownloadDialog(nullptr, TDN_BUTTON_CLICKED, IDCANCEL, 0,
        reinterpret_cast<LONG_PTR>(&dialogState)) == S_OK);
    CHECK(updates::cancelled);
    updates::done = false;
    updates::cancelled = false;
    CHECK(!updates::Busy());
    updates::dialogOpen = true;
    CHECK(updates::Busy());
    updates::dialogOpen = false;
    CHECK(updates::DialogOwner(nullptr) == nullptr);

    updates::Version built;
    CHECK(updates::ParseVersion(appversion::text, built));
    CHECK(updates::InstalledVersion(updates::ExecutablePath(), built));
    auto different = built;
    different.patch = built.patch == 65535 ? 0 : built.patch + 1;
    CHECK(!updates::InstalledVersion(updates::ExecutablePath(), different));

    updates::CheckSchedule p;
    const std::uint64_t now = 1000000;
    CHECK(p.Due(now) && p.ManualAllowed(now));
    p.Finished(now, true);
    CHECK(!p.Due(now + 30) && !p.Due(now + 86399));
    CHECK(p.Due(now + 86400));
    CHECK(!p.ManualAllowed(now + 59) && p.ManualAllowed(now + 60));
    updates::CheckSchedule restarted = p;
    CHECK(!restarted.Due(now + 100));
    p.Finished(now, false);
    CHECK(!p.Due(now + 3599) && p.Due(now + 3600));
    p.Finished(now, false, now + 7200);
    CHECK(!p.ManualAllowed(now + 7000) && p.ManualAllowed(now + 7200));
    CHECK(!p.Due(now + 7199) && p.Due(now + 7200));
    CHECK(p.Due(now - 86401) && p.ManualAllowed(now - 86401));
    wchar_t temporary[MAX_PATH], file[MAX_PATH];
    CHECK(GetTempPathW(ARRAYSIZE(temporary), temporary));
    CHECK(GetTempFileNameW(temporary, L"wbu", 0, file));
    HANDLE handle = CreateFileW(file, GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    CHECK(handle != INVALID_HANDLE_VALUE);
    DWORD written = 0;
    CHECK(WriteFile(handle, "abc", 3, &written, nullptr) && written == 3);
    CloseHandle(handle);
    const std::string correct = "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";
    CHECK(updates::VerifyFile(file, correct, 3));
    CHECK(!updates::VerifyFile(file, correct, 2));
    CHECK(!updates::VerifyFile(file, std::string(64, '0'), 3));
    CHECK(!updates::VerifyFile(file, "", 3));
    CHECK(!updates::VerifyFile(file, correct, updates::MaxDownloadBytes + 1));
    handle = CreateFileW(file, GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    CHECK(handle != INVALID_HANDLE_VALUE && WriteFile(handle, "xyz", 3, &written, nullptr));
    CloseHandle(handle);
    CHECK(!updates::VerifyFile(file, correct, 3));
    CHECK(DeleteFileW(file));
    CHECK(!updates::VerifyFile(file, correct, 3));
    for (const auto* argument : { L"", L"simple", L"C:\\Program Files\\AI Usage Monitor\\", L"spaces and \"quotes\"",
                                  L"C:\\Unicode-\u00e7\\file.exe", L"slashes\\\\\"before quote\\" }) {
        std::wstring command = L"test.exe " + updates::QuoteArgument(argument);
        int count = 0;
        wchar_t** parsed = CommandLineToArgvW(command.c_str(), &count);
        CHECK(parsed && count == 2 && std::wstring(parsed[1]) == argument);
        LocalFree(parsed);
    }
    wchar_t exe[] = L"test.exe", mode[] = L"--apply-update";
    wchar_t* normal[] = { exe };
    wchar_t* invalid[] = { exe, mode };
    CHECK(updates::RunApplyMode(1, normal) == -1);
    CHECK(updates::RunApplyMode(2, invalid) == 2);
    CHECK(!updates::FindInstallation(updates::ExecutablePath()).valid);
    // Downloads go to the app's own folder, %LOCALAPPDATA%\AIUsageMonitor\updates\<32 hex>, not %TEMP%.
    std::wstring root = updates::DownloadRoot();
    const std::wstring suffix = L"\\AIUsageMonitor\\updates";
    CHECK(root.size() > suffix.size() && root.compare(root.size() - suffix.size(), suffix.size(), suffix) == 0);
    std::wstring download = updates::NewDownloadDirectory();
    CHECK(download.size() == root.size() + 33 && download.compare(0, root.size() + 1, root + L"\\") == 0);
    CHECK(GetFileAttributesW(download.c_str()) & FILE_ATTRIBUTE_DIRECTORY);  // empty; the app's cleanup removes it after a day
    std::puts("Update Windows tests passed (HTTP guards, cancel race, dialogs, versions, schedule, SHA-256, arguments).");
}
