#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <string>
#include <vector>

struct SdrDisplay
{
    std::wstring friendlyName;
    std::wstring devicePath;
    std::wstring gdiName;
    LUID adapterId{};
    UINT32 targetId = 0;
    int displayNumber = 0;
    bool isPrimary = false;
};

class SdrDisplayService
{
public:
    std::vector<SdrDisplay> GetHdrDisplays() const;
    std::vector<std::wstring> GetActiveGdiNames() const;
    bool ReadPercent(const SdrDisplay& display, int& percent, UINT32& whiteLevel) const;
    bool SetPercent(const SdrDisplay& display, int percent, int& appliedPercent, UINT32& appliedWhiteLevel, bool readBack = true, BYTE finalValue = 1) const;

    static int WhiteLevelToPercent(UINT32 whiteLevel);
    static UINT32 PercentToWhiteLevel(int percent);
    static double WhiteLevelToNits(UINT32 whiteLevel);
    static int PercentToNits(int percent);
};
