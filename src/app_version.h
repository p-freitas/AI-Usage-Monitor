#pragma once
#include "../res/version.h"

#ifndef AUM_OFFICIAL_RELEASE
#define AUM_OFFICIAL_RELEASE 0
#endif

namespace appversion {
inline constexpr char text[] = AUM_VERSION_STRING;
inline constexpr wchar_t wide[] = AUM_VERSION_WSTRING;
inline constexpr bool official = AUM_OFFICIAL_RELEASE != 0;
}
