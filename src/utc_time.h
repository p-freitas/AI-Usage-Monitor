#pragma once
#include <windows.h>
#include <cstdio>
#include <string>

namespace ai {
// Both Codex events and Antigravity resets use UTC with optional fractional seconds.
inline ULONGLONG UtcFileTime(const std::string& text) {
    SYSTEMTIME time{};
    int pos = 0;
    if (std::sscanf(text.c_str(), "%4hu-%2hu-%2huT%2hu:%2hu:%2hu%n", &time.wYear,
                    &time.wMonth, &time.wDay, &time.wHour, &time.wMinute, &time.wSecond, &pos) != 6)
        return 0;
    ULONGLONG fraction = 0;
    if (text[pos] == '.') {
        ULONGLONG place = 1000000;
        while (++pos < (int)text.size() && text[pos] >= '0' && text[pos] <= '9') {
            fraction += (text[pos] - '0') * place;
            place /= 10;
        }
    }
    FILETIME ft{};
    if (text.substr(pos) != "Z" || !SystemTimeToFileTime(&time, &ft)) return 0;
    return ((static_cast<ULONGLONG>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime) + fraction;
}

} // namespace ai
