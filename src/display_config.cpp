#include "display_config.h"

#include <algorithm>
#include <cwchar>

namespace
{
    constexpr UINT32 kQdcOnlyActivePaths = 0x00000002;
    constexpr UINT32 kDeviceInfoGetSourceName = 1;
    constexpr UINT32 kDeviceInfoGetTargetName = 2;
    constexpr UINT32 kDeviceInfoGetAdvancedColorInfo = 9;
    constexpr UINT32 kDeviceInfoGetSdrWhiteLevel = 11;
    constexpr UINT32 kDeviceInfoGetAdvancedColorInfo2 = 15;
    // Undocumented companion to DISPLAYCONFIG_DEVICE_INFO_GET_SDR_WHITE_LEVEL.
    constexpr UINT32 kDeviceInfoSetSdrWhiteLevel = 0xFFFFFFEE;

    struct ADVANCED_COLOR_INFO_2
    {
        DISPLAYCONFIG_DEVICE_INFO_HEADER header;
        UINT32 value;
        UINT32 colorEncoding;
        UINT32 bitsPerColorChannel;
        UINT32 activeColorMode;
    };

    struct SET_SDR_WHITE_LEVEL
    {
        DISPLAYCONFIG_DEVICE_INFO_HEADER header;
        UINT32 whiteLevel;
        BYTE finalValue;
    };

    struct MonitorSearch
    {
        std::wstring primaryGdiName;
    };

    BOOL CALLBACK FindPrimaryMonitor(HMONITOR monitor, HDC, LPRECT, LPARAM context)
    {
        auto* search = reinterpret_cast<MonitorSearch*>(context);
        MONITORINFOEXW info{};
        info.cbSize = sizeof(info);
        if (GetMonitorInfoW(monitor, &info) && (info.dwFlags & MONITORINFOF_PRIMARY) != 0)
        {
            search->primaryGdiName = info.szDevice;
            return FALSE;
        }

        return TRUE;
    }

    std::wstring GetPrimaryGdiName()
    {
        MonitorSearch search;
        EnumDisplayMonitors(nullptr, nullptr, FindPrimaryMonitor, reinterpret_cast<LPARAM>(&search));
        return search.primaryGdiName;
    }

    int GetDisplayNumber(const std::wstring& gdiName, int fallback)
    {
        if (gdiName.empty())
        {
            return fallback;
        }

        size_t end = gdiName.size();
        size_t begin = end;
        while (begin > 0 && iswdigit(gdiName[begin - 1]))
        {
            --begin;
        }

        if (begin < end)
        {
            wchar_t* parseEnd = nullptr;
            long value = wcstol(gdiName.c_str() + begin, &parseEnd, 10);
            if (value > 0)
            {
                return static_cast<int>(value);
            }
        }

        return fallback;
    }

    std::wstring Trim(std::wstring value)
    {
        const size_t first = value.find_first_not_of(L" \t\r\n");
        if (first == std::wstring::npos)
        {
            return L"";
        }

        const size_t last = value.find_last_not_of(L" \t\r\n");
        return value.substr(first, last - first + 1);
    }

    bool IsHdrEnabled(const DISPLAYCONFIG_PATH_INFO& path)
    {
        ADVANCED_COLOR_INFO_2 info2{};
        info2.header.type = static_cast<DISPLAYCONFIG_DEVICE_INFO_TYPE>(kDeviceInfoGetAdvancedColorInfo2);
        info2.header.size = sizeof(info2);
        info2.header.adapterId = path.targetInfo.adapterId;
        info2.header.id = path.targetInfo.id;

        LONG result = DisplayConfigGetDeviceInfo(&info2.header);
        if (result == ERROR_SUCCESS)
        {
            constexpr UINT32 kAdvancedColorActive = 1u << 1;
            constexpr UINT32 kHdrUserEnabled = 1u << 5;
            return (info2.value & (kAdvancedColorActive | kHdrUserEnabled)) ==
                   (kAdvancedColorActive | kHdrUserEnabled);
        }

        DISPLAYCONFIG_GET_ADVANCED_COLOR_INFO info{};
        info.header.type = static_cast<DISPLAYCONFIG_DEVICE_INFO_TYPE>(kDeviceInfoGetAdvancedColorInfo);
        info.header.size = sizeof(info);
        info.header.adapterId = path.targetInfo.adapterId;
        info.header.id = path.targetInfo.id;

        result = DisplayConfigGetDeviceInfo(&info.header);
        return result == ERROR_SUCCESS && (info.value & 0x2) != 0;
    }

    std::wstring GetSourceGdiName(const DISPLAYCONFIG_PATH_INFO& path)
    {
        DISPLAYCONFIG_SOURCE_DEVICE_NAME source{};
        source.header.type = static_cast<DISPLAYCONFIG_DEVICE_INFO_TYPE>(kDeviceInfoGetSourceName);
        source.header.size = sizeof(source);
        source.header.adapterId = path.sourceInfo.adapterId;
        source.header.id = path.sourceInfo.id;

        if (DisplayConfigGetDeviceInfo(&source.header) != ERROR_SUCCESS)
        {
            return L"";
        }

        return Trim(source.viewGdiDeviceName);
    }

    bool GetTargetName(const DISPLAYCONFIG_PATH_INFO& path, std::wstring& friendlyName, std::wstring& devicePath)
    {
        DISPLAYCONFIG_TARGET_DEVICE_NAME target{};
        target.header.type = static_cast<DISPLAYCONFIG_DEVICE_INFO_TYPE>(kDeviceInfoGetTargetName);
        target.header.size = sizeof(target);
        target.header.adapterId = path.targetInfo.adapterId;
        target.header.id = path.targetInfo.id;

        if (DisplayConfigGetDeviceInfo(&target.header) != ERROR_SUCCESS)
        {
            friendlyName.clear();
            devicePath.clear();
            return false;
        }

        friendlyName = Trim(target.monitorFriendlyDeviceName);
        devicePath = Trim(target.monitorDevicePath);
        return true;
    }

    bool GetWhiteLevel(const SdrDisplay& display, UINT32& whiteLevel)
    {
        DISPLAYCONFIG_SDR_WHITE_LEVEL level{};
        level.header.type = static_cast<DISPLAYCONFIG_DEVICE_INFO_TYPE>(kDeviceInfoGetSdrWhiteLevel);
        level.header.size = sizeof(level);
        level.header.adapterId = display.adapterId;
        level.header.id = display.targetId;

        if (DisplayConfigGetDeviceInfo(&level.header) != ERROR_SUCCESS)
        {
            return false;
        }

        whiteLevel = level.SDRWhiteLevel;
        return true;
    }

    bool SameTarget(const SdrDisplay& display, const DISPLAYCONFIG_PATH_INFO& path)
    {
        return display.adapterId.HighPart == path.targetInfo.adapterId.HighPart &&
               display.adapterId.LowPart == path.targetInfo.adapterId.LowPart &&
               display.targetId == path.targetInfo.id;
    }
}

std::vector<SdrDisplay> SdrDisplayService::GetHdrDisplays() const
{
    std::vector<SdrDisplay> displays;

    UINT32 pathCount = 0;
    UINT32 modeCount = 0;
    if (GetDisplayConfigBufferSizes(kQdcOnlyActivePaths, &pathCount, &modeCount) != ERROR_SUCCESS ||
        pathCount == 0)
    {
        return displays;
    }

    std::vector<DISPLAYCONFIG_PATH_INFO> paths(pathCount);
    std::vector<DISPLAYCONFIG_MODE_INFO> modes(modeCount);

    LONG result = QueryDisplayConfig(
        kQdcOnlyActivePaths,
        &pathCount,
        paths.data(),
        &modeCount,
        modes.data(),
        nullptr);
    if (result != ERROR_SUCCESS)
    {
        return displays;
    }

    paths.resize(pathCount);
    const std::wstring primaryGdiName = GetPrimaryGdiName();
    int fallbackNumber = 0;

    for (const DISPLAYCONFIG_PATH_INFO& path : paths)
    {
        if ((path.flags & DISPLAYCONFIG_PATH_ACTIVE) == 0 ||
            !path.targetInfo.targetAvailable ||
            !IsHdrEnabled(path))
        {
            continue;
        }

        bool duplicate = false;
        for (const SdrDisplay& existing : displays)
        {
            if (SameTarget(existing, path))
            {
                duplicate = true;
                break;
            }
        }
        if (duplicate)
        {
            continue;
        }

        ++fallbackNumber;
        std::wstring friendlyName;
        std::wstring devicePath;
        GetTargetName(path, friendlyName, devicePath);

        std::wstring gdiName = GetSourceGdiName(path);
        int displayNumber = GetDisplayNumber(gdiName, fallbackNumber);
        if (friendlyName.empty())
        {
            friendlyName = L"显示器 " + std::to_wstring(displayNumber);
        }

        SdrDisplay display;
        display.friendlyName = std::move(friendlyName);
        display.devicePath = std::move(devicePath);
        display.gdiName = std::move(gdiName);
        display.adapterId = path.targetInfo.adapterId;
        display.targetId = path.targetInfo.id;
        display.displayNumber = displayNumber;
        display.isPrimary = !primaryGdiName.empty() &&
                            _wcsicmp(display.gdiName.c_str(), primaryGdiName.c_str()) == 0;
        displays.push_back(std::move(display));
    }

    std::sort(displays.begin(), displays.end(), [](const SdrDisplay& left, const SdrDisplay& right)
    {
        if (left.isPrimary != right.isPrimary)
        {
            return left.isPrimary;
        }
        return left.displayNumber < right.displayNumber;
    });

    return displays;
}

std::vector<std::wstring> SdrDisplayService::GetActiveGdiNames() const
{
    std::vector<std::wstring> names;

    UINT32 pathCount = 0;
    UINT32 modeCount = 0;
    if (GetDisplayConfigBufferSizes(kQdcOnlyActivePaths, &pathCount, &modeCount) != ERROR_SUCCESS ||
        pathCount == 0)
    {
        return names;
    }

    std::vector<DISPLAYCONFIG_PATH_INFO> paths(pathCount);
    std::vector<DISPLAYCONFIG_MODE_INFO> modes(modeCount);
    if (QueryDisplayConfig(
            kQdcOnlyActivePaths,
            &pathCount,
            paths.data(),
            &modeCount,
            modes.data(),
            nullptr) != ERROR_SUCCESS)
    {
        return names;
    }

    paths.resize(pathCount);
    for (const DISPLAYCONFIG_PATH_INFO& path : paths)
    {
        if ((path.flags & DISPLAYCONFIG_PATH_ACTIVE) == 0)
        {
            continue;
        }

        const std::wstring name = GetSourceGdiName(path);
        if (name.empty())
        {
            continue;
        }

        bool duplicate = false;
        for (const std::wstring& existing : names)
        {
            if (_wcsicmp(existing.c_str(), name.c_str()) == 0)
            {
                duplicate = true;
                break;
            }
        }
        if (!duplicate)
        {
            names.push_back(name);
        }
    }

    return names;
}

bool SdrDisplayService::ReadPercent(const SdrDisplay& display, int& percent, UINT32& whiteLevel) const
{
    if (!GetWhiteLevel(display, whiteLevel))
    {
        return false;
    }

    percent = WhiteLevelToPercent(whiteLevel);
    return true;
}

bool SdrDisplayService::SetPercent(
    const SdrDisplay& display,
    int percent,
    int& appliedPercent,
    UINT32& appliedWhiteLevel,
    bool readBack,
    BYTE finalValue) const
{
    percent = std::clamp(percent, 0, 100);
    const UINT32 requested = PercentToWhiteLevel(percent);

    SET_SDR_WHITE_LEVEL request{};
    request.header.type = static_cast<DISPLAYCONFIG_DEVICE_INFO_TYPE>(kDeviceInfoSetSdrWhiteLevel);
    request.header.size = sizeof(request);
    request.header.adapterId = display.adapterId;
    request.header.id = display.targetId;
    request.whiteLevel = requested;
    request.finalValue = finalValue;

    if (DisplayConfigSetDeviceInfo(&request.header) != ERROR_SUCCESS)
    {
        return false;
    }

    appliedPercent = percent;
    appliedWhiteLevel = requested;
    if (readBack && GetWhiteLevel(display, appliedWhiteLevel))
    {
        appliedPercent = WhiteLevelToPercent(appliedWhiteLevel);
    }

    return true;
}

int SdrDisplayService::WhiteLevelToPercent(UINT32 whiteLevel)
{
    if (whiteLevel <= 1000)
    {
        return 0;
    }

    return std::clamp(static_cast<int>((whiteLevel - 1000 + 25) / 50), 0, 100);
}

UINT32 SdrDisplayService::PercentToWhiteLevel(int percent)
{
    percent = std::clamp(percent, 0, 100);
    return 1000u + static_cast<UINT32>(percent) * 50u;
}

double SdrDisplayService::WhiteLevelToNits(UINT32 whiteLevel)
{
    return static_cast<double>(whiteLevel) / 1000.0 * 80.0;
}

int SdrDisplayService::PercentToNits(int percent)
{
    return 80 + std::clamp(percent, 0, 100) * 4;
}
