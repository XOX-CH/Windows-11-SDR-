#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include <windows.h>
#include <windowsx.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <shellscalingapi.h>
#include <gdiplus.h>

#pragma comment(lib, "gdiplus.lib")

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <cwchar>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "display_config.h"
#include "resource.h"

namespace
{
    constexpr wchar_t kWindowClass[] = L"SdrBrightnessTrayNative.Window";
    constexpr wchar_t kWindowTitle[] = L"SDR 内容亮度";
    constexpr wchar_t kMutexName[] = L"Local\\SdrBrightnessTray.Native.Instance";
    constexpr UINT kTrayMessage = WM_APP + 1;
    constexpr UINT kShowMessage = WM_APP + 2;
    constexpr UINT kWriteResultMessage = WM_APP + 3;
    constexpr UINT kTrayIconId = 1;
    constexpr UINT_PTR kHideTimerId = 1;
    constexpr int kWindowWidthDip = 320;
    constexpr int kWindowHeightDip = 120;
    constexpr int kHideDelayMs = 180;

    constexpr UINT kHotkeyModifiers = MOD_CONTROL | MOD_ALT;
    constexpr int kHotkeyToggle = 1;
    constexpr int kHotkeyUp = 2;
    constexpr int kHotkeyDown = 3;
    constexpr int kHotkeyStep = 5;
    constexpr UINT kAnimFrameMessage = WM_APP + 4;
    constexpr UINT kAnimDoneMessage = WM_APP + 5;
    constexpr UINT kDeferredMenuMessage = WM_APP + 6;
    constexpr int kOpenAnimMs = 220;
    constexpr int kCloseAnimMs = 140;
    constexpr int kOpenSlideDip = 22;
    constexpr int kCloseSlideDip = 12;
    // 缩放：千分比。进场 92% -> 100%，出场 100% -> 96%（退出更快、幅度更小）。
    constexpr int kOpenScaleMilli = 920;
    constexpr int kCloseScaleMilli = 960;
    constexpr int kWheelStep = 5;
    constexpr UINT kFlashHideMs = 1400;

    constexpr int kMenuOpen = 1000;
    constexpr int kMenuSync = 1001;
    constexpr int kMenuStartup = 1002;

    constexpr int kMenuDisplaySettings = 1003;
    constexpr int kMenuExit = 1004;
    constexpr int kMenuDisplayBase = 2000;

    enum Part
    {
        PartNone = 0,
        PartDisplay = 1,
        PartSlider = 2,
        PartSync = 3,
        PartEmptySettings = 5
    };

    struct Layout
    {
        RECT displayRow{};
        RECT sliderRow{};
        RECT sliderHit{};
        RECT track{};
        RECT percent{};
        RECT caption{};
        RECT sync{};
        RECT emptyButton{};
        RECT emptyCircle{};
        int padX = 0;
        int textX = 0;
    };

    using DwmBoostFn = HRESULT(WINAPI*)(HMONITOR monitor, double boost);

    struct AnimJob
    {
        HWND window = nullptr;
        RECT target{};
        int from = 0;
        int to = 0;
        int scaleFrom = 1000;
        int scaleTo = 1000;
        int duration = 0;
        bool opening = false;
        unsigned generation = 0;
    };

    struct AppState
    {
        HINSTANCE instance = nullptr;
        HWND window = nullptr;
        UINT taskbarCreatedMessage = 0;
        SdrDisplayService displayService;
        std::vector<SdrDisplay> displays;
        int currentDisplay = -1;
        int percent = 0;
        UINT32 whiteLevel = 1000;
        float sliderVisualPercent = 0.0f;
        bool hasHdr = false;
        bool isOpen = false;
        bool exitRequested = false;
        bool lightTheme = false;
        bool lightTaskbar = false;
        int dpi = 96;
        bool settingError = false;
        int hoverPart = PartNone;
        int focusPart = PartSlider;
        bool keyboardFocusVisible = false;
        bool dragging = false;
        bool menuOpen = false;
        POINT showAnchor{};
        ULONGLONG lastTrayActivation = 0;
        std::wstring trayTooltip;

        DwmBoostFn dwmBoost = nullptr;
        HMONITOR currentMonitor = nullptr;

        HICON trayIcon = nullptr;
        bool trayAdded = false;
        NOTIFYICONDATAW notifyData{};
        bool trackingMouse = false;
        bool hotkeyToggle = false;
        bool hotkeyUp = false;
        bool hotkeyDown = false;
        bool flashing = false;

        bool sceneDirty = true;
        bool animating = false;
        bool animatingIn = false;
        bool closing = false;
        RECT animTarget{};
        RECT flyoutRect{};
        int renderWidth = 0;
        int renderHeight = 0;
        HDC presentDc = nullptr;
        HBITMAP presentBitmap = nullptr;
        HGDIOBJ oldPresentBitmap = nullptr;
        void* presentBits = nullptr;
        int presentWidth = 0;
        int presentHeight = 0;
        std::atomic<unsigned> animGeneration{0};
        std::atomic<int> animLatestOffset{0};
        std::atomic<int> animLatestScale{1000};
        std::atomic<bool> animFramePosted{false};

        std::thread animThread;
        std::mutex animMutex;
        std::condition_variable animCv;
        bool animStop = false;
        bool animPending = false;
        AnimJob animJob;

        std::thread writerThread;
        std::mutex writerMutex;
        std::condition_variable writerCv;
        bool writerStop = false;
        bool writerHasWork = false;
        SdrDisplay writerDisplay;
        int writerPercent = 0;
        BYTE writerFinalValue = 1;

        HDC bufferDc = nullptr;
        HBITMAP bufferBitmap = nullptr;
        HGDIOBJ oldBufferBitmap = nullptr;
        void* bufferBits = nullptr;
        int bufferWidth = 0;
        int bufferHeight = 0;

        HDC paintDc = nullptr;
        HBITMAP paintBitmap = nullptr;
        HGDIOBJ oldPaintBitmap = nullptr;
        void* paintBits = nullptr;

        HDC maskDc = nullptr;
        HBITMAP maskBitmap = nullptr;
        HGDIOBJ oldMaskBitmap = nullptr;
        void* maskBits = nullptr;

        bool liveAcrylic = false;

        HDC backdropDc = nullptr;
        HBITMAP backdropBitmap = nullptr;
        HGDIOBJ oldBackdropBitmap = nullptr;
        int backdropWidth = 0;
        int backdropHeight = 0;
        bool backdropReady = false;

        HFONT fontTitle = nullptr;
        HFONT fontBody = nullptr;
        HFONT fontBodyBold = nullptr;
        HFONT fontPercent = nullptr;
        HFONT fontBadge = nullptr;
        HFONT fontCaption = nullptr;
        HFONT iconSmall = nullptr;
        HFONT iconHeader = nullptr;
        HFONT iconBrightness = nullptr;
        HFONT iconEmpty = nullptr;
        HFONT iconChevron = nullptr;

        bool gdiplusReady = false;
        Gdiplus::Font* gdipTitle = nullptr;
        Gdiplus::Font* gdipBody = nullptr;
        Gdiplus::Font* gdipBodyBold = nullptr;
        Gdiplus::Font* gdipPercent = nullptr;
        Gdiplus::Font* gdipBadge = nullptr;
        Gdiplus::Font* gdipCaption = nullptr;
        Gdiplus::Font* gdipIconSmall = nullptr;
        Gdiplus::Font* gdipIconHeader = nullptr;
        Gdiplus::Font* gdipIconBrightness = nullptr;
        Gdiplus::Font* gdipIconEmpty = nullptr;
        Gdiplus::Font* gdipIconChevron = nullptr;

        COLORREF surface = RGB(32, 32, 32);
        COLORREF surfaceTop = RGB(38, 38, 38);
        COLORREF surfaceBottom = RGB(26, 26, 26);
        COLORREF surfaceHighlight = RGB(42, 42, 42);
        COLORREF border = RGB(52, 52, 52);
        COLORREF card = RGB(43, 43, 43);
        COLORREF cardHover = RGB(50, 50, 50);
        COLORREF cardBorder = RGB(62, 62, 62);
        COLORREF badgeBackground = RGB(52, 52, 52);
        COLORREF badgeBorder = RGB(76, 76, 76);
        COLORREF hoverBackground = RGB(52, 52, 52);
        COLORREF textPrimary = RGB(255, 255, 255);
        COLORREF textSecondary = RGB(168, 168, 168);
        COLORREF textTertiary = RGB(132, 132, 132);
        COLORREF accent = RGB(96, 205, 255);
        COLORREF track = RGB(74, 74, 74);
        COLORREF hdrDot = RGB(108, 203, 95);
        COLORREF error = RGB(255, 141, 141);
    };

    AppState g;

    int Px(int dip)
    {
        return MulDiv(dip, g.dpi, 96);
    }

    COLORREF BlendColor(COLORREF base, COLORREF overlay, int overlayPercent)
    {
        overlayPercent = std::clamp(overlayPercent, 0, 100);
        const int baseWeight = 100 - overlayPercent;
        const int red = (GetRValue(base) * baseWeight + GetRValue(overlay) * overlayPercent) / 100;
        const int green = (GetGValue(base) * baseWeight + GetGValue(overlay) * overlayPercent) / 100;
        const int blue = (GetBValue(base) * baseWeight + GetBValue(overlay) * overlayPercent) / 100;
        return RGB(red, green, blue);
    }

    void ShowFlyout();
    void HideFlyout();
    void HideWindowForSettings();
    void ExitApplication();

    DWORD ReadDword(const wchar_t* subkey, const wchar_t* name, DWORD fallback)
    {
        DWORD value = 0;
        DWORD size = sizeof(value);
        if (RegGetValueW(
                HKEY_CURRENT_USER,
                subkey,
                name,
                RRF_RT_REG_DWORD,
                nullptr,
                &value,
                &size) != ERROR_SUCCESS)
        {
            return fallback;
        }
        return value;
    }

    std::wstring ReadString(const wchar_t* subkey, const wchar_t* name)
    {
        DWORD type = 0;
        DWORD size = 0;
        if (RegGetValueW(
                HKEY_CURRENT_USER,
                subkey,
                name,
                RRF_RT_REG_SZ,
                &type,
                nullptr,
                &size) != ERROR_SUCCESS ||
            size == 0)
        {
            return L"";
        }

        std::wstring value(size / sizeof(wchar_t), L'\0');
        if (RegGetValueW(
                HKEY_CURRENT_USER,
                subkey,
                name,
                RRF_RT_REG_SZ,
                &type,
                value.data(),
                &size) != ERROR_SUCCESS)
        {
            return L"";
        }

        while (!value.empty() && value.back() == L'\0')
        {
            value.pop_back();
        }
        return value;
    }

    bool ReadBinary(const wchar_t* subkey, const wchar_t* name, std::vector<BYTE>& value)
    {
        DWORD size = 0;
        if (RegGetValueW(
                HKEY_CURRENT_USER,
                subkey,
                name,
                RRF_RT_REG_BINARY,
                nullptr,
                nullptr,
                &size) != ERROR_SUCCESS ||
            size == 0)
        {
            return false;
        }

        value.resize(size);
        return RegGetValueW(
                   HKEY_CURRENT_USER,
                   subkey,
                   name,
                   RRF_RT_REG_BINARY,
                   nullptr,
                   value.data(),
                   &size) == ERROR_SUCCESS;
    }

    COLORREF ReadSystemAccent(bool lightTheme)
    {
        std::vector<BYTE> palette;
        if (ReadBinary(
                L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Accent",
                L"AccentPalette",
                palette) &&
            palette.size() >= 32)
        {
            const size_t index = lightTheme ? 3 : 1;
            const BYTE* entry = palette.data() + index * 4;
            const COLORREF color = RGB(entry[0], entry[1], entry[2]);
            const int maximum = std::max({GetRValue(color), GetGValue(color), GetBValue(color)});
            const int minimum = std::min({GetRValue(color), GetGValue(color), GetBValue(color)});
            if (maximum >= 45 && maximum - minimum >= 20)
            {
                return color;
            }
        }

        return lightTheme ? RGB(0, 103, 192) : RGB(96, 205, 255);
    }

    void LoadTheme()
    {
        constexpr const wchar_t* personalize =
            L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize";

        g.lightTheme = ReadDword(personalize, L"AppsUseLightTheme", 0) != 0;
        g.lightTaskbar = ReadDword(personalize, L"SystemUsesLightTheme", 0) != 0;
        g.accent = ReadSystemAccent(g.lightTheme);

        if (g.lightTheme)
        {
            g.surface = RGB(249, 249, 249);
            g.surfaceHighlight = RGB(255, 255, 255);
            g.border = RGB(220, 220, 220);
            g.card = RGB(255, 255, 255);
            g.cardHover = RGB(250, 250, 250);
            g.cardBorder = RGB(226, 226, 226);
            g.badgeBackground = RGB(247, 247, 247);
            g.badgeBorder = RGB(222, 222, 222);
            g.hoverBackground = RGB(238, 238, 238);
            g.textPrimary = RGB(26, 26, 26);
            g.textSecondary = RGB(92, 92, 92);
            g.textTertiary = RGB(132, 132, 132);
            g.track = RGB(208, 208, 208);
            g.hdrDot = RGB(15, 123, 15);
            g.error = RGB(196, 43, 28);
        }
        else
        {
            g.surface = RGB(32, 32, 32);
            g.surfaceHighlight = RGB(43, 43, 43);
            g.border = RGB(54, 54, 54);
            g.card = RGB(43, 43, 43);
            g.cardHover = RGB(50, 50, 50);
            g.cardBorder = RGB(64, 64, 64);
            g.badgeBackground = RGB(53, 53, 53);
            g.badgeBorder = RGB(76, 76, 76);
            g.hoverBackground = RGB(53, 53, 53);
            g.textPrimary = RGB(255, 255, 255);
            g.textSecondary = RGB(168, 168, 168);
            g.textTertiary = RGB(130, 130, 130);
            g.track = RGB(76, 76, 76);
            g.hdrDot = RGB(108, 203, 95);
            g.error = RGB(255, 141, 141);
        }

        const int surfaceTint = g.lightTheme ? 7 : 18;
        const int cardTint = g.lightTheme ? 4 : 12;
        g.surface = BlendColor(g.surface, g.accent, surfaceTint);
        g.surfaceTop = BlendColor(g.surface, RGB(255, 255, 255), g.lightTheme ? 6 : 3);
        g.surfaceBottom = BlendColor(g.surface, RGB(0, 0, 0), g.lightTheme ? 2 : 7);
        g.surfaceHighlight = BlendColor(g.surface, RGB(255, 255, 255), g.lightTheme ? 30 : 7);
        g.border = BlendColor(g.border, g.accent, g.lightTheme ? 12 : 20);
        g.card = BlendColor(g.card, g.accent, cardTint);
        g.cardHover = BlendColor(g.card, RGB(255, 255, 255), g.lightTheme ? 2 : 5);
        g.cardBorder = BlendColor(g.cardBorder, g.accent, g.lightTheme ? 10 : 24);
        g.badgeBackground = BlendColor(g.badgeBackground, g.accent, g.lightTheme ? 5 : 12);
        g.badgeBorder = BlendColor(g.badgeBorder, g.accent, g.lightTheme ? 10 : 18);
        g.hoverBackground = BlendColor(g.hoverBackground, g.accent, g.lightTheme ? 5 : 12);
        g.track = BlendColor(g.track, g.accent, g.lightTheme ? 8 : 14);
    }

    const wchar_t* PickFace(std::initializer_list<const wchar_t*> candidates, const wchar_t* fallback)
    {
        HDC dc = GetDC(nullptr);
        if (!dc)
        {
            return fallback;
        }

        const wchar_t* chosen = fallback;
        for (const wchar_t* candidate : candidates)
        {
            LOGFONTW probe{};
            probe.lfHeight = -12;
            probe.lfWeight = FW_NORMAL;
            wcscpy_s(probe.lfFaceName, candidate);
            HFONT font = CreateFontIndirectW(&probe);
            if (!font)
            {
                continue;
            }
            HGDIOBJ old = SelectObject(dc, font);
            wchar_t actual[LF_FACESIZE]{};
            GetTextFaceW(dc, LF_FACESIZE, actual);
            SelectObject(dc, old);
            DeleteObject(font);
            if (_wcsicmp(actual, candidate) == 0)
            {
                chosen = candidate;
                break;
            }
        }

        ReleaseDC(nullptr, dc);
        return chosen;
    }

    HFONT CreateUiFont(int dipHeight, int weight, const wchar_t* face)
    {
        LOGFONTW font{};
        font.lfHeight = -Px(dipHeight);
        font.lfWeight = weight;
        font.lfCharSet = DEFAULT_CHARSET;
        font.lfOutPrecision = OUT_TT_PRECIS;
        font.lfClipPrecision = CLIP_DEFAULT_PRECIS;
        font.lfQuality = ANTIALIASED_QUALITY;
        font.lfPitchAndFamily = DEFAULT_PITCH | FF_DONTCARE;
        wcscpy_s(font.lfFaceName, face);
        return CreateFontIndirectW(&font);
    }

    Gdiplus::Font* CreateGdiplusFont(HFONT font)
    {
        if (!g.gdiplusReady || !font)
        {
            return nullptr;
        }
        HDC dc = GetDC(g.window);
        if (!dc)
        {
            return nullptr;
        }
        auto* result = new Gdiplus::Font(dc, font);
        ReleaseDC(g.window, dc);
        if (!result || result->GetLastStatus() != Gdiplus::Ok)
        {
            delete result;
            return nullptr;
        }
        return result;
    }

    void CreateFonts()
    {
        if (g.fontTitle)
        {
            return;
        }

        // 对齐 Windows 11「设置」的排版梯度（Segoe UI Variable + 字号阶梯）：
        //   Title 28 / Subtitle 20 / Body Strong 14 SemiBold / Body 14 / Caption 12。
        const wchar_t* uiFace = PickFace(
            {L"Segoe UI Variable Text", L"Segoe UI"}, L"Microsoft YaHei UI");
        const wchar_t* displayFace = PickFace(
            {L"Segoe UI Variable Display", L"Segoe UI Variable Text", L"Segoe UI"},
            L"Microsoft YaHei UI");
        const wchar_t* iconFace = PickFace(
            {L"Segoe Fluent Icons", L"Segoe MDL2 Assets"}, L"Segoe MDL2 Assets");

        // Body Strong：正文尺寸下用 Text 光学尺寸（Display 是为 20px+ 设计的）
        g.fontTitle = CreateUiFont(14, FW_SEMIBOLD, uiFace);        // Body Strong
        g.fontBody = CreateUiFont(14, FW_NORMAL, uiFace);            // Body
        g.fontBodyBold = CreateUiFont(14, FW_SEMIBOLD, uiFace);      // Body Strong
        g.fontPercent = CreateUiFont(20, FW_SEMIBOLD, displayFace);  // Subtitle
        g.fontBadge = CreateUiFont(12, FW_NORMAL, uiFace);           // Caption
        g.fontCaption = CreateUiFont(12, FW_NORMAL, uiFace);         // Caption

        g.iconSmall = CreateUiFont(13, FW_NORMAL, iconFace);
        g.iconHeader = CreateUiFont(14, FW_NORMAL, iconFace);
        g.iconBrightness = CreateUiFont(15, FW_NORMAL, iconFace);
        g.iconEmpty = CreateUiFont(18, FW_NORMAL, iconFace);
        g.iconChevron = CreateUiFont(9, FW_NORMAL, iconFace);

        g.gdipTitle = CreateGdiplusFont(g.fontTitle);
        g.gdipBody = CreateGdiplusFont(g.fontBody);
        g.gdipBodyBold = CreateGdiplusFont(g.fontBodyBold);
        g.gdipPercent = CreateGdiplusFont(g.fontPercent);
        g.gdipBadge = CreateGdiplusFont(g.fontBadge);
        g.gdipCaption = CreateGdiplusFont(g.fontCaption);
        g.gdipIconSmall = CreateGdiplusFont(g.iconSmall);
        g.gdipIconHeader = CreateGdiplusFont(g.iconHeader);
        g.gdipIconBrightness = CreateGdiplusFont(g.iconBrightness);
        g.gdipIconEmpty = CreateGdiplusFont(g.iconEmpty);
        g.gdipIconChevron = CreateGdiplusFont(g.iconChevron);
    }

    Gdiplus::Font* GdiplusFontFor(HFONT font)
    {
        if (!font)
        {
            return nullptr;
        }
        if (font == g.fontTitle) return g.gdipTitle;
        if (font == g.fontBody) return g.gdipBody;
        if (font == g.fontBodyBold) return g.gdipBodyBold;
        if (font == g.fontPercent) return g.gdipPercent;
        if (font == g.fontBadge) return g.gdipBadge;
        if (font == g.fontCaption) return g.gdipCaption;
        if (font == g.iconSmall) return g.gdipIconSmall;
        if (font == g.iconHeader) return g.gdipIconHeader;
        if (font == g.iconBrightness) return g.gdipIconBrightness;
        if (font == g.iconEmpty) return g.gdipIconEmpty;
        if (font == g.iconChevron) return g.gdipIconChevron;
        return nullptr;
    }

    void DestroyFonts()
    {
        HFONT* fonts[] = {
            &g.fontTitle,
            &g.fontBody,
            &g.fontBodyBold,
            &g.fontPercent,
            &g.fontBadge,
            &g.fontCaption,
            &g.iconSmall,
            &g.iconHeader,
            &g.iconBrightness,
            &g.iconEmpty,
            &g.iconChevron};

        for (HFONT* font : fonts)
        {
            if (*font)
            {
                DeleteObject(*font);
                *font = nullptr;
            }
        }

        Gdiplus::Font** gdiFonts[] = {
            &g.gdipTitle,
            &g.gdipBody,
            &g.gdipBodyBold,
            &g.gdipPercent,
            &g.gdipBadge,
            &g.gdipIconSmall,
            &g.gdipIconHeader,
            &g.gdipIconBrightness,
            &g.gdipIconEmpty,
            &g.gdipIconChevron};

        for (Gdiplus::Font** font : gdiFonts)
        {
            delete *font;
            *font = nullptr;
        }
    }

    void DestroyMaskBuffer()
    {
        if (g.maskDc)
        {
            if (g.oldMaskBitmap)
            {
                SelectObject(g.maskDc, g.oldMaskBitmap);
                g.oldMaskBitmap = nullptr;
            }
            DeleteDC(g.maskDc);
            g.maskDc = nullptr;
        }
        if (g.maskBitmap)
        {
            DeleteObject(g.maskBitmap);
            g.maskBitmap = nullptr;
        }
        g.maskBits = nullptr;
    }

    void DestroyPaintBuffer()
    {
        if (g.paintDc)
        {
            if (g.oldPaintBitmap)
            {
                SelectObject(g.paintDc, g.oldPaintBitmap);
                g.oldPaintBitmap = nullptr;
            }
            DeleteDC(g.paintDc);
            g.paintDc = nullptr;
        }
        if (g.paintBitmap)
        {
            DeleteObject(g.paintBitmap);
            g.paintBitmap = nullptr;
        }
        g.paintBits = nullptr;
    }

    void DestroyPresentBuffer()
    {
        if (g.presentDc)
        {
            if (g.oldPresentBitmap)
            {
                SelectObject(g.presentDc, g.oldPresentBitmap);
                g.oldPresentBitmap = nullptr;
            }
            DeleteDC(g.presentDc);
            g.presentDc = nullptr;
        }
        if (g.presentBitmap)
        {
            DeleteObject(g.presentBitmap);
            g.presentBitmap = nullptr;
        }
        g.presentBits = nullptr;
        g.presentWidth = 0;
        g.presentHeight = 0;
    }

    void DestroyBackBuffer()
    {
        if (g.bufferDc)
        {
            if (g.oldBufferBitmap)
            {
                SelectObject(g.bufferDc, g.oldBufferBitmap);
            }
            DeleteDC(g.bufferDc);
            g.bufferDc = nullptr;
            g.oldBufferBitmap = nullptr;
        }

        if (g.bufferBitmap)
        {
            DeleteObject(g.bufferBitmap);
            g.bufferBitmap = nullptr;
        }

        g.bufferBits = nullptr;
        g.bufferWidth = 0;
        g.bufferHeight = 0;

        DestroyPresentBuffer();
        DestroyPaintBuffer();
        DestroyMaskBuffer();
    }

    void DestroyBackdrop()
    {
        if (g.backdropDc)
        {
            if (g.oldBackdropBitmap)
            {
                SelectObject(g.backdropDc, g.oldBackdropBitmap);
                g.oldBackdropBitmap = nullptr;
            }
            DeleteDC(g.backdropDc);
            g.backdropDc = nullptr;
        }
        if (g.backdropBitmap)
        {
            DeleteObject(g.backdropBitmap);
            g.backdropBitmap = nullptr;
        }
        g.backdropWidth = 0;
        g.backdropHeight = 0;
        g.backdropReady = false;
    }

    void BlurPixels(std::vector<uint32_t>& pixels, int width, int height, int radius, int passes)
    {
        if (pixels.empty() || width <= 0 || height <= 0)
        {
            return;
        }

        radius = std::clamp(radius, 1, std::min(width, height) / 2);
        std::vector<uint32_t> temp(pixels.size());
        for (int pass = 0; pass < passes; ++pass)
        {
            for (int y = 0; y < height; ++y)
            {
                for (int x = 0; x < width; ++x)
                {
                    int sumR = 0;
                    int sumG = 0;
                    int sumB = 0;
                    int count = 0;
                    for (int dx = -radius; dx <= radius; ++dx)
                    {
                        const int xx = std::clamp(x + dx, 0, width - 1);
                        const uint32_t value = pixels[static_cast<size_t>(y) * width + xx];
                        sumR += (value >> 16) & 0xFF;
                        sumG += (value >> 8) & 0xFF;
                        sumB += value & 0xFF;
                        ++count;
                    }
                    temp[static_cast<size_t>(y) * width + x] =
                        static_cast<uint32_t>((sumR / count) << 16 |
                                              (sumG / count) << 8 |
                                              (sumB / count));
                }
            }

            for (int y = 0; y < height; ++y)
            {
                for (int x = 0; x < width; ++x)
                {
                    int sumR = 0;
                    int sumG = 0;
                    int sumB = 0;
                    int count = 0;
                    for (int dy = -radius; dy <= radius; ++dy)
                    {
                        const int yy = std::clamp(y + dy, 0, height - 1);
                        const uint32_t value = temp[static_cast<size_t>(yy) * width + x];
                        sumR += (value >> 16) & 0xFF;
                        sumG += (value >> 8) & 0xFF;
                        sumB += value & 0xFF;
                        ++count;
                    }
                    pixels[static_cast<size_t>(y) * width + x] =
                        static_cast<uint32_t>((sumR / count) << 16 |
                                              (sumG / count) << 8 |
                                              (sumB / count));
                }
            }
        }
    }

    bool CaptureBackdrop(int screenX, int screenY, int width, int height)
    {
        DestroyBackdrop();
        if (width <= 0 || height <= 0)
        {
            return false;
        }

        const int smallWidth = std::max(48, width / 4);
        const int smallHeight = std::max(32, height / 4);

        BITMAPINFO smallInfo{};
        smallInfo.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        smallInfo.bmiHeader.biWidth = smallWidth;
        smallInfo.bmiHeader.biHeight = -smallHeight;
        smallInfo.bmiHeader.biPlanes = 1;
        smallInfo.bmiHeader.biBitCount = 32;
        smallInfo.bmiHeader.biCompression = BI_RGB;

        void* smallBits = nullptr;
        HBITMAP smallBitmap = CreateDIBSection(nullptr, &smallInfo, DIB_RGB_COLORS, &smallBits, nullptr, 0);
        if (!smallBitmap || !smallBits)
        {
            return false;
        }

        HDC smallDc = CreateCompatibleDC(nullptr);
        HGDIOBJ oldSmall = SelectObject(smallDc, smallBitmap);
        HDC screenDc = GetDC(nullptr);
        bool captured = false;
        if (screenDc)
        {
            SetStretchBltMode(smallDc, HALFTONE);
            SetBrushOrgEx(smallDc, 0, 0, nullptr);
            captured = StretchBlt(
                           smallDc,
                           0,
                           0,
                           smallWidth,
                           smallHeight,
                           screenDc,
                           screenX,
                           screenY,
                           width,
                           height,
                           SRCCOPY) != FALSE;
            ReleaseDC(nullptr, screenDc);
        }

        if (!captured)
        {
            SelectObject(smallDc, oldSmall);
            DeleteDC(smallDc);
            DeleteObject(smallBitmap);
            return false;
        }

        std::vector<uint32_t> pixels(static_cast<size_t>(smallWidth) * smallHeight);
        std::memcpy(pixels.data(), smallBits, pixels.size() * sizeof(uint32_t));
        SelectObject(smallDc, oldSmall);
        DeleteDC(smallDc);
        DeleteObject(smallBitmap);

        const int radius = std::max(2, std::min(smallWidth, smallHeight) / 24);
        BlurPixels(pixels, smallWidth, smallHeight, radius, 2);

        BITMAPINFO fullInfo{};
        fullInfo.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        fullInfo.bmiHeader.biWidth = width;
        fullInfo.bmiHeader.biHeight = -height;
        fullInfo.bmiHeader.biPlanes = 1;
        fullInfo.bmiHeader.biBitCount = 32;
        fullInfo.bmiHeader.biCompression = BI_RGB;

        void* fullBits = nullptr;
        HBITMAP fullBitmap = CreateDIBSection(nullptr, &fullInfo, DIB_RGB_COLORS, &fullBits, nullptr, 0);
        if (!fullBitmap)
        {
            return false;
        }

        HDC fullDc = CreateCompatibleDC(nullptr);
        HGDIOBJ oldFull = SelectObject(fullDc, fullBitmap);
        SetStretchBltMode(fullDc, HALFTONE);
        SetBrushOrgEx(fullDc, 0, 0, nullptr);
        StretchDIBits(
            fullDc,
            0,
            0,
            width,
            height,
            0,
            0,
            smallWidth,
            smallHeight,
            pixels.data(),
            &smallInfo,
            DIB_RGB_COLORS,
            SRCCOPY);

        BITMAPINFO tintInfo = smallInfo;
        tintInfo.bmiHeader.biWidth = 1;
        tintInfo.bmiHeader.biHeight = -1;
        void* tintBits = nullptr;
        HBITMAP tintBitmap = CreateDIBSection(nullptr, &tintInfo, DIB_RGB_COLORS, &tintBits, nullptr, 0);
        if (tintBitmap && tintBits)
        {
            const uint32_t tintPixel = (GetRValue(g.surface) << 16) |
                                       (GetGValue(g.surface) << 8) |
                                       GetBValue(g.surface);
            std::memcpy(tintBits, &tintPixel, sizeof(tintPixel));
            HDC tintDc = CreateCompatibleDC(nullptr);
            HGDIOBJ oldTint = SelectObject(tintDc, tintBitmap);
            BLENDFUNCTION blend{AC_SRC_OVER, 0, static_cast<BYTE>(g.lightTheme ? 212 : 228), 0};
            AlphaBlend(fullDc, 0, 0, width, height, tintDc, 0, 0, 1, 1, blend);
            SelectObject(tintDc, oldTint);
            DeleteDC(tintDc);
            DeleteObject(tintBitmap);
        }

        g.backdropDc = fullDc;
        g.backdropBitmap = fullBitmap;
        g.oldBackdropBitmap = oldFull;
        g.backdropWidth = width;
        g.backdropHeight = height;
        g.backdropReady = true;
        return true;
    }

    bool EnsurePresentBuffer(HDC referenceDc)
    {
        if (g.presentDc && g.presentBitmap && g.presentBits &&
            g.presentWidth == g.bufferWidth && g.presentHeight == g.bufferHeight)
        {
            return true;
        }
        DestroyPresentBuffer();
        if (g.bufferWidth <= 0 || g.bufferHeight <= 0)
        {
            return false;
        }

        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = g.bufferWidth;
        info.bmiHeader.biHeight = -g.bufferHeight;
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;

        g.presentDc = CreateCompatibleDC(referenceDc);
        g.presentBitmap = CreateDIBSection(referenceDc, &info, DIB_RGB_COLORS, &g.presentBits, nullptr, 0);
        if (!g.presentDc || !g.presentBitmap || !g.presentBits)
        {
            DestroyPresentBuffer();
            return false;
        }
        g.oldPresentBitmap = SelectObject(g.presentDc, g.presentBitmap);
        g.presentWidth = g.bufferWidth;
        g.presentHeight = g.bufferHeight;
        return true;
    }

    bool EnsureMaskBuffer(HDC referenceDc)
    {
        if (!g.liveAcrylic)
        {
            return false;
        }
        if (g.maskDc && g.maskBitmap && g.maskBits)
        {
            return true;
        }
        if (g.bufferWidth <= 0 || g.bufferHeight <= 0)
        {
            return false;
        }

        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = g.bufferWidth;
        info.bmiHeader.biHeight = -g.bufferHeight;
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 24;
        info.bmiHeader.biCompression = BI_RGB;

        g.maskDc = CreateCompatibleDC(referenceDc);
        g.maskBitmap = CreateDIBSection(referenceDc, &info, DIB_RGB_COLORS, &g.maskBits, nullptr, 0);
        if (!g.maskDc || !g.maskBitmap || !g.maskBits)
        {
            DestroyMaskBuffer();
            return false;
        }

        g.oldMaskBitmap = SelectObject(g.maskDc, g.maskBitmap);
        return true;
    }

    bool EnsurePaintBuffer(HDC referenceDc)
    {
        if (!g.liveAcrylic)
        {
            return false;
        }
        if (g.paintDc && g.paintBitmap && g.paintBits)
        {
            return true;
        }
        if (g.bufferWidth <= 0 || g.bufferHeight <= 0)
        {
            return false;
        }

        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = g.bufferWidth;
        info.bmiHeader.biHeight = -g.bufferHeight;
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 24;
        info.bmiHeader.biCompression = BI_RGB;

        g.paintDc = CreateCompatibleDC(referenceDc);
        g.paintBitmap = CreateDIBSection(referenceDc, &info, DIB_RGB_COLORS, &g.paintBits, nullptr, 0);
        if (!g.paintDc || !g.paintBitmap || !g.paintBits)
        {
            DestroyPaintBuffer();
            return false;
        }

        g.oldPaintBitmap = SelectObject(g.paintDc, g.paintBitmap);
        return true;
    }

    void EnsureBackBuffer(HDC referenceDc)
    {
        // 渲染面固定为目标尺寸。缩放动画时窗口比它小，由 WM_PAINT 整幅 StretchBlt
        // 呈现，这样得到的是等比缩放，而不是把布局挤窄、字不变小。
        const int width = Px(kWindowWidthDip);
        const int height = Px(kWindowHeightDip);
        if (width <= 0 || height <= 0)
        {
            return;
        }
        g.renderWidth = width;
        g.renderHeight = height;

        if (g.bufferDc && g.bufferBitmap && width == g.bufferWidth && height == g.bufferHeight)
        {
            if (g.liveAcrylic)
            {
                EnsurePaintBuffer(referenceDc);
                EnsureMaskBuffer(referenceDc);
            }
            return;
        }

        DestroyBackBuffer();
        g.bufferDc = CreateCompatibleDC(referenceDc);
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = width;
        info.bmiHeader.biHeight = -height;
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        g.bufferBitmap = CreateDIBSection(referenceDc, &info, DIB_RGB_COLORS, &g.bufferBits, nullptr, 0);
        if (!g.bufferDc || !g.bufferBitmap || !g.bufferBits)
        {
            DestroyBackBuffer();
            return;
        }

        g.oldBufferBitmap = SelectObject(g.bufferDc, g.bufferBitmap);
        g.bufferWidth = width;
        g.bufferHeight = height;
        g.sceneDirty = true;

        if (g.liveAcrylic)
        {
            EnsurePaintBuffer(referenceDc);
            EnsureMaskBuffer(referenceDc);
        }
    }

    void FillSolid(HDC dc, const RECT& rect, COLORREF color)
    {
        SetDCBrushColor(dc, color);
        FillRect(dc, &rect, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
    }

    void RoundFill(HDC dc, const RECT& rect, int radius, COLORREF fill, COLORREF border)
    {
        SetDCBrushColor(dc, fill);
        SetDCPenColor(dc, border);
        RoundRect(dc, rect.left, rect.top, rect.right, rect.bottom, radius * 2, radius * 2);
    }

    void RoundStroke(HDC dc, const RECT& rect, int radius, COLORREF color)
    {
        HGDIOBJ previous = SelectObject(dc, GetStockObject(HOLLOW_BRUSH));
        SetDCPenColor(dc, color);
        RoundRect(dc, rect.left, rect.top, rect.right, rect.bottom, radius * 2, radius * 2);
        SelectObject(dc, previous);
    }

    void CircleFill(HDC dc, int centerX, int centerY, int radius, COLORREF fill)
    {
        SetDCBrushColor(dc, fill);
        SetDCPenColor(dc, fill);
        Ellipse(dc, centerX - radius, centerY - radius, centerX + radius + 1, centerY + radius + 1);
    }

    void CircleStroke(HDC dc, int centerX, int centerY, int radius, COLORREF color)
    {
        HGDIOBJ previous = SelectObject(dc, GetStockObject(HOLLOW_BRUSH));
        SetDCPenColor(dc, color);
        Ellipse(dc, centerX - radius, centerY - radius, centerX + radius + 1, centerY + radius + 1);
        SelectObject(dc, previous);
    }

    int TextWidth(HDC dc, HFONT font, const wchar_t* text)
    {
        HGDIOBJ previous = SelectObject(dc, font);
        SIZE size{};
        GetTextExtentPoint32W(dc, text, static_cast<int>(wcslen(text)), &size);
        SelectObject(dc, previous);
        return size.cx;
    }

    void DrawTextIn(
        HDC dc,
        RECT rect,
        HFONT font,
        COLORREF color,
        const wchar_t* text,
        UINT flags = DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_HIDEPREFIX)
    {
        Gdiplus::Font* gdiFont = GdiplusFontFor(font);
        if (gdiFont)
        {
            Gdiplus::Graphics graphics(dc);
            if (graphics.GetLastStatus() == Gdiplus::Ok)
            {
                graphics.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAliasGridFit);
                Gdiplus::StringFormat format(Gdiplus::StringFormat::GenericTypographic());
                format.SetAlignment(
                    (flags & DT_CENTER) ? Gdiplus::StringAlignmentCenter :
                    (flags & DT_RIGHT) ? Gdiplus::StringAlignmentFar :
                                         Gdiplus::StringAlignmentNear);
                format.SetLineAlignment(
                    (flags & DT_VCENTER) ? Gdiplus::StringAlignmentCenter :
                    (flags & DT_BOTTOM) ? Gdiplus::StringAlignmentFar :
                                          Gdiplus::StringAlignmentNear);
                format.SetTrimming(
                    (flags & DT_END_ELLIPSIS)
                        ? Gdiplus::StringTrimmingEllipsisCharacter
                        : Gdiplus::StringTrimmingNone);
                if (flags & DT_SINGLELINE)
                {
                    format.SetFormatFlags(format.GetFormatFlags() | Gdiplus::StringFormatFlagsNoWrap);
                }
                Gdiplus::SolidBrush brush(Gdiplus::Color(
                    255,
                    GetRValue(color),
                    GetGValue(color),
                    GetBValue(color)));
                Gdiplus::RectF layoutRect(
                    static_cast<Gdiplus::REAL>(rect.left),
                    static_cast<Gdiplus::REAL>(rect.top),
                    static_cast<Gdiplus::REAL>(rect.right - rect.left),
                    static_cast<Gdiplus::REAL>(rect.bottom - rect.top));
                graphics.DrawString(text, -1, gdiFont, layoutRect, &format, &brush);
                if (graphics.GetLastStatus() == Gdiplus::Ok)
                {
                    return;
                }
            }
        }

        HGDIOBJ previous = SelectObject(dc, font);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, color);
        DrawTextW(dc, text, -1, &rect, flags);
        SelectObject(dc, previous);
    }

    std::wstring DisplayLabel(const SdrDisplay& display)
    {
        return display.friendlyName + L"  ·  显示器 " + std::to_wstring(display.displayNumber);
    }

    Layout BuildLayout()
    {
        Layout layout{};
        const int width = g.renderWidth > 0 ? g.renderWidth : Px(kWindowWidthDip);
        const int pad = Px(16);
        layout.padX = pad;

        // 栅格：外框 16，图标列 16..40（24 宽），正文列 48，右边界 304。
        // 无标题行：显示器 14..42、滑条 54..82、说明 86..102。
        // 上 14 / 下 18：文字墨迹在 12 DIP 行高里居中，视觉上上下才是等距的。
        const int textX = pad + Px(32);
        layout.textX = textX;

        layout.displayRow = {pad, Px(14), width - pad, Px(42)};
        layout.sliderRow = {pad, Px(54), width - pad, Px(82)};
        layout.percent = {width - pad - Px(48), Px(54), width - pad, Px(82)};

        const int trackRight = layout.percent.left - Px(12);
        const int trackCenter = Px(68);
        layout.track = {textX, trackCenter - Px(2), trackRight, trackCenter + Px(2)};
        layout.sliderHit = {textX - Px(12), Px(54), trackRight + Px(12), Px(82)};

        layout.caption = {textX, Px(86), width - pad, Px(102)};

        // 「同步全部」放在说明行右端，和其他内容同一行
        layout.sync = {width - pad - Px(56), Px(84), width - pad, Px(104)};
        layout.emptyCircle = {width / 2 - Px(16), Px(10), width / 2 + Px(16), Px(42)};
        layout.emptyButton = {
            (width - Px(108)) / 2,
            Px(86),
            (width + Px(108)) / 2,
            Px(110)};
        return layout;
    }

    Part HitTest(POINT point)
    {
        // 缩放动画期间窗口小于渲染面，先把客户区坐标映射回渲染坐标。
        RECT client{};
        GetClientRect(g.window, &client);
        if (client.right > 0 && client.bottom > 0 &&
            g.renderWidth > 0 && g.renderHeight > 0 &&
            (client.right != g.renderWidth || client.bottom != g.renderHeight))
        {
            point.x = MulDiv(point.x, g.renderWidth, client.right);
            point.y = MulDiv(point.y, g.renderHeight, client.bottom);
        }

        const Layout layout = BuildLayout();
        if (g.hasHdr)
        {
            if (g.displays.size() > 1 && PtInRect(&layout.displayRow, point))
            {
                return PartDisplay;
            }
            if (PtInRect(&layout.sliderHit, point))
            {
                return PartSlider;
            }
            if (g.displays.size() > 1 && PtInRect(&layout.sync, point))
            {
                return PartSync;
            }
        }
        else if (PtInRect(&layout.emptyButton, point))
        {
            return PartEmptySettings;
        }

        return PartNone;
    }

    void DrawFocusOutline(HDC dc, const RECT& rect, int part, int radius)
    {
        if (!g.keyboardFocusVisible || g.focusPart != part || GetFocus() != g.window)
        {
            return;
        }

        RECT focus = rect;
        InflateRect(&focus, -Px(2), -Px(2));
        RoundStroke(dc, focus, radius, g.accent);
    }

    COLORREF UiColor(COLORREF color, bool maskMode)
    {
        return maskMode ? RGB(255, 255, 255) : color;
    }

    void CompositeLiveSurface()
    {
        if (!g.paintBits || !g.maskBits || !g.bufferBits ||
            g.bufferWidth <= 0 || g.bufferHeight <= 0)
        {
            return;
        }

        const int width = g.bufferWidth;
        const int height = g.bufferHeight;
        const int stride = (width * 3 + 3) & ~3;
        auto* paint = static_cast<BYTE*>(g.paintBits);
        auto* mask = static_cast<BYTE*>(g.maskBits);
        auto* output = static_cast<BYTE*>(g.bufferBits);
        const int scrimAlpha = g.lightTheme ? 88 : 96;
        const COLORREF scrim = g.surface;
        const int scrimB = GetBValue(scrim) * scrimAlpha / 255;
        const int scrimG = GetGValue(scrim) * scrimAlpha / 255;
        const int scrimR = GetRValue(scrim) * scrimAlpha / 255;

        for (int y = 0; y < height; ++y)
        {
            const BYTE* paintRow = paint + y * stride;
            const BYTE* maskRow = mask + y * stride;
            BYTE* outputRow = output + y * width * 4;
            for (int x = 0; x < width; ++x)
            {
                const BYTE* paintPixel = paintRow + x * 3;
                const BYTE* maskPixel = maskRow + x * 3;
                BYTE* outputPixel = outputRow + x * 4;
                const int coverage = std::max<int>(
                    maskPixel[0],
                    std::max<int>(maskPixel[1], maskPixel[2]));
                const int inverse = 255 - coverage;
                outputPixel[0] = static_cast<BYTE>(std::min(255, paintPixel[0] + scrimB * inverse / 255));
                outputPixel[1] = static_cast<BYTE>(std::min(255, paintPixel[1] + scrimG * inverse / 255));
                outputPixel[2] = static_cast<BYTE>(std::min(255, paintPixel[2] + scrimR * inverse / 255));
                outputPixel[3] = static_cast<BYTE>(coverage + scrimAlpha * inverse / 255);
            }
        }
    }

    void MakeSurfaceOpaque()
    {
        if (!g.bufferBits || g.bufferWidth <= 0 || g.bufferHeight <= 0)
        {
            return;
        }
        auto* pixels = static_cast<BYTE*>(g.bufferBits);
        const int count = g.bufferWidth * g.bufferHeight;
        for (int i = 0; i < count; ++i)
        {
            pixels[i * 4 + 3] = 255;
        }
    }

    void DrawUi(HDC dc, bool maskMode)
    {
        const int width = g.renderWidth > 0 ? g.renderWidth : Px(kWindowWidthDip);
        const int height = g.renderHeight > 0 ? g.renderHeight : Px(kWindowHeightDip);
        const Layout layout = BuildLayout();

        const HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(DC_BRUSH));
        const HGDIOBJ oldPen = SelectObject(dc, GetStockObject(DC_PEN));
        SetBkMode(dc, TRANSPARENT);

        if (!maskMode && !g.liveAcrylic)
        {
            if (g.backdropReady && g.backdropDc)
            {
                BitBlt(dc, 0, 0, width, height, g.backdropDc, 0, 0, SRCCOPY);
            }
            else
            {
                constexpr int gradientBands = 32;
                for (int i = 0; i < gradientBands; ++i)
                {
                    const int top = height * i / gradientBands;
                    const int bottom = height * (i + 1) / gradientBands;
                    RECT band{0, top, width, bottom};
                    FillSolid(dc, band, BlendColor(g.surfaceTop, g.surfaceBottom, i * 100 / (gradientBands - 1)));
                }
            }
        }
        RECT highlight{0, 1, width, 2};
        FillSolid(dc, highlight, UiColor(g.surfaceHighlight, maskMode));
        RoundStroke(dc, RECT{0, 0, width, height}, Px(8), UiColor(g.border, maskMode));

        if (!g.hasHdr)
        {
            const int centerX = (layout.emptyCircle.left + layout.emptyCircle.right) / 2;
            const int circleRadius = (layout.emptyCircle.right - layout.emptyCircle.left) / 2;
            const int circleCenterY = (layout.emptyCircle.top + layout.emptyCircle.bottom) / 2;
            CircleFill(dc, centerX, circleCenterY, circleRadius, UiColor(g.card, maskMode));
            CircleStroke(dc, centerX, circleCenterY, circleRadius, UiColor(g.cardBorder, maskMode));
            RECT emptySun{centerX - Px(16), circleCenterY - Px(16), centerX + Px(16), circleCenterY + Px(16)};
            DrawTextIn(dc, emptySun, g.iconEmpty, UiColor(g.textSecondary, maskMode), L"\xE706", DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

            RECT emptyTitle{layout.padX, Px(46), width - layout.padX, Px(66)};
            DrawTextIn(dc, emptyTitle, g.fontTitle, UiColor(g.textPrimary, maskMode), L"未检测到 HDR 显示器", DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

            RECT emptyDescription{width / 2 - Px(140), Px(68), width / 2 + Px(140), Px(84)};
            DrawTextIn(
                dc,
                emptyDescription,
                g.fontCaption,
                UiColor(g.textSecondary, maskMode),
                L"请先在 Windows 设置中为显示器开启 HDR",
                DT_CENTER | DT_TOP | DT_SINGLELINE | DT_NOPREFIX);

            const COLORREF buttonFill = g.hoverPart == PartEmptySettings ? g.cardHover : g.card;
            RoundFill(dc, layout.emptyButton, Px(5), UiColor(buttonFill, maskMode), UiColor(g.cardBorder, maskMode));
            RECT emptyButtonText = layout.emptyButton;
            DrawTextIn(dc, emptyButtonText, g.fontBody, UiColor(g.textPrimary, maskMode), L"打开显示设置", DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
            DrawFocusOutline(dc, layout.emptyButton, PartEmptySettings, Px(5));

            SelectObject(dc, oldBrush);
            SelectObject(dc, oldPen);
            return;
        }

        SdrDisplay* display = nullptr;
        if (g.currentDisplay >= 0 && static_cast<size_t>(g.currentDisplay) < g.displays.size())
        {
            display = &g.displays[g.currentDisplay];
        }

        const bool multiple = g.displays.size() > 1;
        const bool displayActive =
            multiple && (g.hoverPart == PartDisplay ||
                         (g.keyboardFocusVisible && g.focusPart == PartDisplay));

        // 不再给这一行套边框卡片：它只是一行信息，悬停时才出现底色（多屏时可点）。
        if (displayActive)
        {
            RECT hover = layout.displayRow;
            InflateRect(&hover, Px(4), 0);
            RoundFill(dc, hover, Px(4), UiColor(g.hoverBackground, maskMode), UiColor(g.hoverBackground, maskMode));
        }

        RECT monitorIcon{layout.padX, layout.displayRow.top, layout.padX + Px(24), layout.displayRow.bottom};
        DrawTextIn(dc, monitorIcon, g.iconSmall, UiColor(g.textSecondary, maskMode), L"\xE7F4", DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

        const wchar_t* hdrText = L"HDR";
        const int hdrTextWidth = TextWidth(dc, g.fontBadge, hdrText);
        const int badgeWidth = Px(8) + Px(5) + Px(3) + hdrTextWidth + Px(7);
        const int badgeRight = layout.displayRow.right - (multiple ? Px(20) : 0);
        const int badgeTop = (layout.displayRow.top + layout.displayRow.bottom - Px(20)) / 2;
        RECT badge{badgeRight - badgeWidth, badgeTop, badgeRight, badgeTop + Px(20)};
        RoundFill(dc, badge, Px(4), UiColor(g.badgeBackground, maskMode), UiColor(g.badgeBorder, maskMode));
        const int dotRadius = Px(3);
        CircleFill(dc, badge.left + Px(8) + dotRadius, (badge.top + badge.bottom) / 2, dotRadius, UiColor(g.hdrDot, maskMode));
        RECT badgeText{badge.left + Px(17), badge.top, badge.right - Px(5), badge.bottom};
        DrawTextIn(dc, badgeText, g.fontBadge, UiColor(g.textSecondary, maskMode), hdrText, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

        if (multiple)
        {
            RECT chevron{badge.right + Px(2), layout.displayRow.top, layout.displayRow.right, layout.displayRow.bottom};
            DrawTextIn(dc, chevron, g.iconChevron, UiColor(g.textSecondary, maskMode), L"\xE70D", DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        }

        if (display)
        {
            std::wstring label = DisplayLabel(*display);
            RECT name{
                layout.textX,
                layout.displayRow.top,
                badge.left - Px(8),
                layout.displayRow.bottom};
            DrawTextIn(
                dc,
                name,
                g.fontBody,
                UiColor(g.textPrimary, maskMode),
                label.c_str(),
                DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
        }

        DrawFocusOutline(dc, layout.displayRow, PartDisplay, Px(4));

        RECT brightnessIcon{layout.padX, layout.sliderRow.top, layout.padX + Px(24), layout.sliderRow.bottom};
        DrawTextIn(dc, brightnessIcon, g.iconBrightness, UiColor(g.textSecondary, maskMode), L"\xE706", DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

        RoundFill(dc, layout.track, Px(2), UiColor(g.track, maskMode), UiColor(g.track, maskMode));

        const int trackWidth = layout.track.right - layout.track.left;
        const float visualPercent = std::clamp(g.sliderVisualPercent, 0.0f, 100.0f);
        const int thumbCenterX =
            layout.track.left + static_cast<int>(std::lround(trackWidth * visualPercent / 100.0));
        const int thumbCenterY = (layout.track.top + layout.track.bottom) / 2;

        if (visualPercent > 0.0f)
        {
            const int saved = SaveDC(dc);
            IntersectClipRect(dc, layout.track.left - Px(4), layout.track.top - Px(4), thumbCenterX, layout.track.bottom + Px(4));
            RoundFill(dc, layout.track, Px(2), UiColor(g.accent, maskMode), UiColor(g.accent, maskMode));
            RestoreDC(dc, saved);
        }

        const bool activeThumb = g.dragging || g.hoverPart == PartSlider;
        const int thumbRadius = activeThumb ? Px(10) : Px(9);
        CircleFill(dc, thumbCenterX, thumbCenterY, thumbRadius, UiColor(g.accent, maskMode));
        CircleFill(dc, thumbCenterX, thumbCenterY, Px(3), UiColor(g.surface, maskMode));

        if (g.keyboardFocusVisible && g.focusPart == PartSlider && GetFocus() == g.window)
        {
            CircleStroke(dc, thumbCenterX, thumbCenterY, thumbRadius + Px(3), UiColor(g.accent, maskMode));
        }

        wchar_t percentText[16]{};
        swprintf_s(percentText, L"%d", g.percent);
        DrawTextIn(
            dc,
            layout.percent,
            g.fontPercent,
            UiColor(g.textPrimary, maskMode),
            percentText,
            DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

        RECT captionRect = layout.caption;
        if (multiple)
        {
            captionRect.right = layout.sync.left - Px(8);
        }

        if (g.settingError)
        {
            DrawTextIn(
                dc,
                captionRect,
                g.fontCaption,
                UiColor(g.error, maskMode),
                L"调整失败，请确认 HDR 仍已开启",
                DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
        }
        else
        {
            const int nits = static_cast<int>(std::lround(SdrDisplayService::WhiteLevelToNits(g.whiteLevel)));
            const wchar_t* captionLabel = L"SDR 白点";
            wchar_t captionValue[48]{};
            swprintf_s(captionValue, L"  ·  %d 尼特", nits);

            RECT labelRect = captionRect;
            DrawTextIn(dc, labelRect, g.fontCaption, UiColor(g.textTertiary, maskMode), captionLabel,
                       DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);

            RECT valueRect = captionRect;
            valueRect.left += TextWidth(dc, g.fontCaption, captionLabel);
            DrawTextIn(dc, valueRect, g.fontCaption, UiColor(g.textSecondary, maskMode), captionValue,
                       DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
        }


        if (multiple)
        {
            const bool syncActive = g.hoverPart == PartSync ||
                                    (g.keyboardFocusVisible && g.focusPart == PartSync);
            RECT syncHover = layout.sync;
            InflateRect(&syncHover, Px(4), 0);
            if (syncActive)
            {
                RoundFill(dc, syncHover, Px(4), UiColor(g.hoverBackground, maskMode), UiColor(g.hoverBackground, maskMode));
            }
            DrawTextIn(dc, layout.sync, g.fontCaption, UiColor(g.accent, maskMode), L"同步全部",
                       DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
            DrawFocusOutline(dc, layout.sync, PartSync, Px(4));
        }

        SelectObject(dc, oldBrush);
        SelectObject(dc, oldPen);
    }

    void InvalidateUi()
    {
        g.sceneDirty = true;
        if (g.isOpen)
        {
            InvalidateRect(g.window, nullptr, FALSE);
        }
    }

    void UpdateTrayTooltip()
    {
        if (g.hasHdr && g.currentDisplay >= 0 &&
            static_cast<size_t>(g.currentDisplay) < g.displays.size())
        {
            const int nits = static_cast<int>(
                std::lround(SdrDisplayService::WhiteLevelToNits(g.whiteLevel)));
            wchar_t buffer[160]{};
            swprintf_s(
                buffer,
                L"SDR 亮度  %d%%  ·  %d 尼特  ·  %s",
                g.percent,
                nits,
                DisplayLabel(g.displays[g.currentDisplay]).c_str());
            g.trayTooltip = buffer;
        }
        else
        {
            g.trayTooltip = L"SDR 内容亮度";
        }

        if (g.trayTooltip.size() > 120)
        {
            g.trayTooltip.resize(120);
            g.trayTooltip += L"…";
        }

        if (g.trayAdded)
        {
            wcsncpy_s(g.notifyData.szTip, g.trayTooltip.c_str(), _TRUNCATE);
            g.notifyData.uFlags = NIF_TIP | NIF_SHOWTIP;
            Shell_NotifyIconW(NIM_MODIFY, &g.notifyData);
        }
    }

    std::wstring HotkeyHint()
    {
        std::wstring text;
        if (g.hotkeyToggle)
        {
            text = L"热键  Ctrl+Alt+B 呼出/收起";
        }
        if (g.hotkeyUp && g.hotkeyDown)
        {
            if (!text.empty())
            {
                text += L"  ·  ";
            }
            text += L"Ctrl+Alt+↑↓ 调节 ±5";
        }
        return text;
    }

    void RegisterHotkeys()
    {
        if (!g.window)
        {
            return;
        }

        g.hotkeyToggle = RegisterHotKey(g.window, kHotkeyToggle, kHotkeyModifiers, 'B') != FALSE;
        g.hotkeyUp = RegisterHotKey(g.window, kHotkeyUp, kHotkeyModifiers, VK_UP) != FALSE;
        g.hotkeyDown = RegisterHotKey(g.window, kHotkeyDown, kHotkeyModifiers, VK_DOWN) != FALSE;
    }

    void UnregisterHotkeys()
    {
        if (!g.window)
        {
            return;
        }

        if (g.hotkeyToggle)
        {
            UnregisterHotKey(g.window, kHotkeyToggle);
            g.hotkeyToggle = false;
        }
        if (g.hotkeyUp)
        {
            UnregisterHotKey(g.window, kHotkeyUp);
            g.hotkeyUp = false;
        }
        if (g.hotkeyDown)
        {
            UnregisterHotKey(g.window, kHotkeyDown);
            g.hotkeyDown = false;
        }
    }

    HICON LoadTrayIcon()
    {
        const int resourceId = g.lightTaskbar ? IDI_TRAY_ON_LIGHT : IDI_TRAY_ON_DARK;
        // 托盘图标由任务栏按“任务栏所在显示器”的 DPI 渲染。启动时 g.dpi 还是默认的
        // 96，会加载 16px 图标，任务栏在 200% 下把它放大到 32px —— 这就是"资源管理器
        // 里清楚、任务栏里糊"的原因。这里直接问任务栏要它的 DPI。
        UINT dpi = static_cast<UINT>(g.dpi > 0 ? g.dpi : 96);
        if (HWND taskbar = FindWindowW(L"Shell_TrayWnd", nullptr))
        {
            const UINT taskbarDpi = GetDpiForWindow(taskbar);
            if (taskbarDpi >= 96)
            {
                dpi = taskbarDpi;
            }
        }
        else
        {
            const UINT systemDpi = GetDpiForSystem();
            if (systemDpi >= 96)
            {
                dpi = systemDpi;
            }
        }
        const int size = GetSystemMetricsForDpi(SM_CXSMICON, dpi);
        HICON icon = static_cast<HICON>(LoadImageW(
            g.instance,
            MAKEINTRESOURCEW(resourceId),
            IMAGE_ICON,
            size,
            size,
            0));
        if (!icon)
        {
            icon = LoadIconW(g.instance, MAKEINTRESOURCEW(IDI_APP));
        }
        return icon;
    }

    bool AddTrayIcon()
    {
        if (g.trayAdded)
        {
            return true;
        }

        if (!g.trayIcon)
        {
            g.trayIcon = LoadTrayIcon();
        }

        g.notifyData = {};
        g.notifyData.cbSize = sizeof(g.notifyData);
        g.notifyData.hWnd = g.window;
        g.notifyData.uID = kTrayIconId;
        g.notifyData.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
        g.notifyData.uCallbackMessage = kTrayMessage;
        g.notifyData.hIcon = g.trayIcon;
        wcsncpy_s(g.notifyData.szTip, g.trayTooltip.c_str(), _TRUNCATE);

        if (!Shell_NotifyIconW(NIM_ADD, &g.notifyData))
        {
            return false;
        }

        g.notifyData.uVersion = NOTIFYICON_VERSION_4;
        Shell_NotifyIconW(NIM_SETVERSION, &g.notifyData);
        g.trayAdded = true;
        return true;
    }

    void RefreshTrayTheme()
    {
        HICON oldIcon = g.trayIcon;
        g.trayIcon = LoadTrayIcon();
        if (!g.trayIcon)
        {
            g.trayIcon = oldIcon;
            return;
        }

        if (g.trayAdded)
        {
            g.notifyData.uFlags = NIF_ICON | NIF_TIP | NIF_SHOWTIP;
            g.notifyData.hIcon = g.trayIcon;
            wcscpy_s(g.notifyData.szTip, g.trayTooltip.c_str());
            Shell_NotifyIconW(NIM_MODIFY, &g.notifyData);
        }

        if (oldIcon)
        {
            DestroyIcon(oldIcon);
        }
    }

    void RemoveTrayIcon()
    {
        if (g.trayAdded)
        {
            NOTIFYICONDATAW data{};
            data.cbSize = sizeof(data);
            data.hWnd = g.window;
            data.uID = kTrayIconId;
            Shell_NotifyIconW(NIM_DELETE, &data);
            g.trayAdded = false;
        }

        if (g.trayIcon)
        {
            DestroyIcon(g.trayIcon);
            g.trayIcon = nullptr;
        }
    }

    struct AccentPolicy
    {
        DWORD state;
        DWORD flags;
        DWORD gradientColor;
        DWORD animationId;
    };

    struct CompositionAttributeData
    {
        DWORD attribute;
        PVOID data;
        SIZE_T sizeOfData;
    };

    using SetWindowCompositionAttributeFn = BOOL(WINAPI*)(HWND, CompositionAttributeData*);

    bool IsWindows11OrGreater()
    {
        using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOW*);
        HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
        if (!ntdll)
        {
            return false;
        }
        auto rtlGetVersion = reinterpret_cast<RtlGetVersionFn>(GetProcAddress(ntdll, "RtlGetVersion"));
        if (!rtlGetVersion)
        {
            return false;
        }
        OSVERSIONINFOW info{};
        info.dwOSVersionInfoSize = sizeof(info);
        return rtlGetVersion(&info) == 0 && info.dwBuildNumber >= 22000;
    }

    bool TransparencyEnabled()
    {
        DWORD enabled = 1;
        DWORD size = sizeof(enabled);
        RegGetValueW(
            HKEY_CURRENT_USER,
            L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
            L"EnableTransparency",
            RRF_RT_REG_DWORD,
            nullptr,
            &enabled,
            &size);
        return enabled != 0;
    }

    bool ApplyAcrylicEffect(bool enable)
    {
        static SetWindowCompositionAttributeFn setAttribute = []() -> SetWindowCompositionAttributeFn
        {
            HMODULE user32 = GetModuleHandleW(L"user32.dll");
            return user32
                ? reinterpret_cast<SetWindowCompositionAttributeFn>(
                      GetProcAddress(user32, "SetWindowCompositionAttribute"))
                : nullptr;
        }();

        if (!setAttribute || !g.window)
        {
            return false;
        }

        AccentPolicy policy{};
        if (enable && IsWindows11OrGreater() && TransparencyEnabled())
        {
            const COLORREF tint = g.lightTheme
                ? BlendColor(RGB(242, 242, 242), g.accent, 4)
                : BlendColor(RGB(28, 28, 28), g.accent, 10);
            policy.state = 4;
            policy.flags = 0x20 | 0x40 | 0x80 | 0x100;
            policy.gradientColor =
                (0xCCu << 24) |
                (static_cast<DWORD>(GetBValue(tint)) << 16) |
                (static_cast<DWORD>(GetGValue(tint)) << 8) |
                static_cast<DWORD>(GetRValue(tint));
        }

        CompositionAttributeData data{19, &policy, sizeof(policy)};
        setAttribute(g.window, &data);
        return policy.state == 4;
    }

    void ApplyWindowAttributes()
    {
        int corner = 2;
        DwmSetWindowAttribute(g.window, 33, &corner, sizeof(corner));

        BOOL dark = g.lightTheme ? FALSE : TRUE;
        DwmSetWindowAttribute(g.window, 20, &dark, sizeof(dark));

        static HMODULE uxtheme = LoadLibraryExW(L"uxtheme.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (uxtheme)
        {
            using SetPreferredAppModeFn = int(WINAPI*)(int);
            using FlushMenuThemesFn = void(WINAPI*)();
            using AllowDarkModeForWindowFn = BOOL(WINAPI*)(HWND, BOOL);

            auto setPreferredAppMode = reinterpret_cast<SetPreferredAppModeFn>(
                GetProcAddress(uxtheme, MAKEINTRESOURCEA(135)));
            auto flushMenuThemes = reinterpret_cast<FlushMenuThemesFn>(
                GetProcAddress(uxtheme, MAKEINTRESOURCEA(136)));
            auto allowDarkModeForWindow = reinterpret_cast<AllowDarkModeForWindowFn>(
                GetProcAddress(uxtheme, MAKEINTRESOURCEA(104)));

            if (setPreferredAppMode)
            {
                setPreferredAppMode(g.lightTheme ? 3 : 2);
            }
            if (allowDarkModeForWindow)
            {
                allowDarkModeForWindow(g.window, g.lightTheme ? FALSE : TRUE);
            }
            if (flushMenuThemes)
            {
                flushMenuThemes();
            }
        }

        g.liveAcrylic = ApplyAcrylicEffect(true);
    }

    bool ReadSelectedDisplayKey(std::wstring& key)
    {
        key = ReadString(L"Software\\SdrBrightnessTray", L"LastDisplay");
        return !key.empty();
    }

    void SaveSelectedDisplayKey(const std::wstring& key)
    {
        if (key.empty())
        {
            return;
        }

        HKEY keyHandle = nullptr;
        if (RegCreateKeyExW(
                HKEY_CURRENT_USER,
                L"Software\\SdrBrightnessTray",
                0,
                nullptr,
                0,
                KEY_SET_VALUE,
                nullptr,
                &keyHandle,
                nullptr) != ERROR_SUCCESS)
        {
            return;
        }

        RegSetValueExW(
            keyHandle,
            L"LastDisplay",
            0,
            REG_SZ,
            reinterpret_cast<const BYTE*>(key.c_str()),
            static_cast<DWORD>((key.size() + 1) * sizeof(wchar_t)));
        RegCloseKey(keyHandle);
    }

    struct MonitorMatch
    {
        const std::wstring* gdiName;
        HMONITOR monitor;
    };

    BOOL CALLBACK FindMonitorCallback(HMONITOR monitor, HDC, LPRECT, LPARAM context)
    {
        auto* match = reinterpret_cast<MonitorMatch*>(context);
        MONITORINFOEXW info{};
        info.cbSize = sizeof(info);
        if (GetMonitorInfoW(monitor, &info) && _wcsicmp(info.szDevice, match->gdiName->c_str()) == 0)
        {
            match->monitor = monitor;
            return FALSE;
        }
        return TRUE;
    }

    HMONITOR FindDisplayMonitor(const std::wstring& gdiName)
    {
        if (gdiName.empty())
        {
            return nullptr;
        }

        MonitorMatch match{&gdiName, nullptr};
        EnumDisplayMonitors(nullptr, nullptr, FindMonitorCallback, reinterpret_cast<LPARAM>(&match));
        return match.monitor;
    }

    void UpdateCurrentMonitor()
    {
        g.currentMonitor = nullptr;
        if (g.hasHdr && g.currentDisplay >= 0 &&
            static_cast<size_t>(g.currentDisplay) < g.displays.size())
        {
            g.currentMonitor = FindDisplayMonitor(g.displays[g.currentDisplay].gdiName);
        }
    }

    bool ApplyLiveBoost(float visualPercent)
    {
        if (!g.dwmBoost || !g.currentMonitor)
        {
            return false;
        }

        const double boost = 1.0 + std::clamp(visualPercent, 0.0f, 100.0f) * 0.05;
        if (FAILED(g.dwmBoost(g.currentMonitor, boost)))
        {
            g.dwmBoost = nullptr;
            return false;
        }
        return true;
    }

    void InitializeLiveBoost()
    {
        HMODULE dwmapi = GetModuleHandleW(L"dwmapi.dll");
        if (!dwmapi)
        {
            dwmapi = LoadLibraryW(L"dwmapi.dll");
        }
        if (dwmapi)
        {
            g.dwmBoost = reinterpret_cast<DwmBoostFn>(
                GetProcAddress(dwmapi, MAKEINTRESOURCEA(171)));
        }
    }

    void RefreshDisplays()
    {
        std::wstring preferred = ReadString(L"Software\\SdrBrightnessTray", L"LastDisplay");
        if (preferred.empty() && g.currentDisplay >= 0 &&
            static_cast<size_t>(g.currentDisplay) < g.displays.size())
        {
            preferred = g.displays[g.currentDisplay].devicePath;
        }

        g.displays = g.displayService.GetHdrDisplays();
        g.hasHdr = !g.displays.empty();
        g.settingError = false;

        if (!g.hasHdr)
        {
            g.currentDisplay = -1;
            g.percent = 0;
            g.whiteLevel = 1000;
            g.sliderVisualPercent = 0.0f;
            g.currentMonitor = nullptr;
            UpdateTrayTooltip();
            return;
        }

        int selected = -1;
        if (!preferred.empty())
        {
            for (size_t i = 0; i < g.displays.size(); ++i)
            {
                if (_wcsicmp(g.displays[i].devicePath.c_str(), preferred.c_str()) == 0)
                {
                    selected = static_cast<int>(i);
                    break;
                }
            }
        }

        if (selected < 0)
        {
            for (size_t i = 0; i < g.displays.size(); ++i)
            {
                if (g.displays[i].isPrimary)
                {
                    selected = static_cast<int>(i);
                    break;
                }
            }
        }
        if (selected < 0)
        {
            selected = 0;
        }

        g.currentDisplay = selected;
        UINT32 whiteLevel = 1000;
        int percent = 0;
        if (g.displayService.ReadPercent(g.displays[selected], percent, whiteLevel))
        {
            g.percent = percent;
            g.whiteLevel = whiteLevel;
        }
        else
        {
            g.percent = 0;
            g.whiteLevel = 1000;
            g.settingError = true;
        }
        g.sliderVisualPercent = static_cast<float>(g.percent);
        UpdateCurrentMonitor();
        ApplyLiveBoost(g.sliderVisualPercent);

        SaveSelectedDisplayKey(g.displays[selected].devicePath);
        UpdateTrayTooltip();
    }

    void QueueBrightnessWrite(int percent, BYTE finalValue)
    {
        if (!g.hasHdr || g.currentDisplay < 0 ||
            static_cast<size_t>(g.currentDisplay) >= g.displays.size())
        {
            return;
        }

        const SdrDisplay display = g.displays[g.currentDisplay];
        {
            std::lock_guard<std::mutex> lock(g.writerMutex);
            g.writerDisplay = display;
            g.writerPercent = std::clamp(percent, 0, 100);
            g.writerFinalValue = finalValue;
            g.writerHasWork = true;
        }
        g.writerCv.notify_one();
    }

    void SetInteractivePercent(int percent, bool updateTooltip)
    {
        if (!g.hasHdr)
        {
            return;
        }

        percent = std::clamp(percent, 0, 100);
        g.sliderVisualPercent = static_cast<float>(percent);
        ApplyLiveBoost(g.sliderVisualPercent);
        if (percent != g.percent)
        {
            g.percent = percent;
            g.whiteLevel = SdrDisplayService::PercentToWhiteLevel(percent);
            g.settingError = false;
            QueueBrightnessWrite(percent, 1);
        }

        if (updateTooltip)
        {
            UpdateTrayTooltip();
        }
        InvalidateUi();
    }

    void BrightnessWriterLoop()
    {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
        for (;;)
        {
            SdrDisplay display;
            int percent = 0;
            BYTE finalValue = 1;
            {
                std::unique_lock<std::mutex> lock(g.writerMutex);
                g.writerCv.wait(lock, []
                {
                    return g.writerStop || g.writerHasWork;
                });

                if (g.writerStop && !g.writerHasWork)
                {
                    return;
                }

                display = g.writerDisplay;
                percent = g.writerPercent;
                finalValue = g.writerFinalValue;
                g.writerHasWork = false;
            }

            int appliedPercent = percent;
            UINT32 appliedWhiteLevel = SdrDisplayService::PercentToWhiteLevel(percent);
            const bool applied = g.displayService.SetPercent(
                display,
                percent,
                appliedPercent,
                appliedWhiteLevel,
                false,
                finalValue);

            if (g.window && !applied)
            {
                PostMessageW(
                    g.window,
                    kWriteResultMessage,
                    0,
                    static_cast<LPARAM>(percent));
            }
        }
    }

    void StartBrightnessWriter()
    {
        if (g.writerThread.joinable())
        {
            return;
        }

        {
            std::lock_guard<std::mutex> lock(g.writerMutex);
            g.writerStop = false;
        }
        g.writerThread = std::thread(BrightnessWriterLoop);
    }

    void StopBrightnessWriter()
    {
        if (!g.writerThread.joinable())
        {
            return;
        }

        {
            std::lock_guard<std::mutex> lock(g.writerMutex);
            g.writerStop = true;
        }
        g.writerCv.notify_all();
        g.writerThread.join();
    }

    void SyncAllDisplays()
    {
        if (!g.hasHdr)
        {
            return;
        }

        bool failed = false;
        for (const SdrDisplay& display : g.displays)
        {
            int appliedPercent = g.percent;
            UINT32 appliedLevel = g.whiteLevel;
            if (!g.displayService.SetPercent(display, g.percent, appliedPercent, appliedLevel, false, 1))
            {
                failed = true;
            }
        }

        g.settingError = failed;
        UpdateTrayTooltip();
        InvalidateUi();
    }

    void SelectDisplay(int index)
    {
        if (index < 0 || static_cast<size_t>(index) >= g.displays.size())
        {
            return;
        }

        g.currentDisplay = index;
        UINT32 whiteLevel = 1000;
        int percent = 0;
        if (g.displayService.ReadPercent(g.displays[index], percent, whiteLevel))
        {
            g.percent = percent;
            g.whiteLevel = whiteLevel;
            g.settingError = false;
        }
        else
        {
            g.percent = 0;
            g.whiteLevel = 1000;
            g.settingError = true;
        }
        g.sliderVisualPercent = static_cast<float>(g.percent);
        UpdateCurrentMonitor();
        ApplyLiveBoost(g.sliderVisualPercent);

        SaveSelectedDisplayKey(g.displays[index].devicePath);
        UpdateTrayTooltip();
        InvalidateUi();
    }

    bool StartupEnabled()
    {
        const std::wstring command = ReadString(
            L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
            L"SdrBrightnessTray");
        if (command.empty())
        {
            return false;
        }

        wchar_t executable[MAX_PATH]{};
        if (!GetModuleFileNameW(nullptr, executable, MAX_PATH))
        {
            return false;
        }

        std::wstring prefix = L"\"" + std::wstring(executable) + L"\"";
        return _wcsnicmp(command.c_str(), prefix.c_str(), prefix.size()) == 0;
    }

    void SetStartupEnabled(bool enabled)
    {
        HKEY key = nullptr;
        if (RegCreateKeyExW(
                HKEY_CURRENT_USER,
                L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
                0,
                nullptr,
                0,
                KEY_SET_VALUE,
                nullptr,
                &key,
                nullptr) != ERROR_SUCCESS)
        {
            return;
        }

        if (enabled)
        {
            wchar_t executable[MAX_PATH]{};
            GetModuleFileNameW(nullptr, executable, MAX_PATH);
            const std::wstring command = L"\"" + std::wstring(executable) + L"\" --background";
            RegSetValueExW(
                key,
                L"SdrBrightnessTray",
                0,
                REG_SZ,
                reinterpret_cast<const BYTE*>(command.c_str()),
                static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t)));
        }
        else
        {
            RegDeleteValueW(key, L"SdrBrightnessTray");
        }

        RegCloseKey(key);
    }

    void OpenDisplaySettings()
    {
        HideWindowForSettings();
        ShellExecuteW(nullptr, L"open", L"ms-settings:display", nullptr, nullptr, SW_SHOWNORMAL);
    }

    void StartHideTimer()
    {
        if (g.isOpen && !g.menuOpen)
        {
            SetTimer(g.window, kHideTimerId, kHideDelayMs, nullptr);
        }
    }

    void PerformAction(Part part);

    void RefreshHoverFromCursor()
    {
        POINT cursor{};
        if (!GetCursorPos(&cursor))
        {
            return;
        }
        ScreenToClient(g.window, &cursor);

        RECT client{};
        GetClientRect(g.window, &client);

        Part hover = PtInRect(&client, cursor) ? HitTest(cursor) : PartNone;
        if (hover == PartSlider && !g.hasHdr)
        {
            hover = PartNone;
        }
        g.hoverPart = hover;
    }

    void ShowDisplayMenu()
    {
        if (g.displays.size() < 2)
        {
            return;
        }

        HMENU menu = CreatePopupMenu();
        if (!menu)
        {
            return;
        }

        for (size_t i = 0; i < g.displays.size(); ++i)
        {
            const std::wstring label = DisplayLabel(g.displays[i]);
            UINT flags = MF_STRING;
            if (static_cast<int>(i) == g.currentDisplay)
            {
                flags |= MF_CHECKED;
            }
            AppendMenuW(menu, flags, kMenuDisplayBase + static_cast<int>(i), label.c_str());
        }

        RECT row = BuildLayout().displayRow;
        POINT anchor{row.left, row.bottom + Px(4)};
        ClientToScreen(g.window, &anchor);

        g.menuOpen = true;
        SetForegroundWindow(g.window);
        int command = TrackPopupMenuEx(
            menu,
            TPM_LEFTALIGN | TPM_TOPALIGN | TPM_RETURNCMD | TPM_RIGHTBUTTON,
            anchor.x,
            anchor.y,
            g.window,
            nullptr);
        g.menuOpen = false;
        DestroyMenu(menu);
        PostMessageW(g.window, WM_NULL, 0, 0);
        RefreshHoverFromCursor();

        if (command >= kMenuDisplayBase &&
            command < kMenuDisplayBase + static_cast<int>(g.displays.size()))
        {
            SelectDisplay(command - kMenuDisplayBase);
        }
        else if (command == 0 && g.isOpen)
        {
            HWND foreground = GetForegroundWindow();
            DWORD processId = 0;
            if (foreground)
            {
                GetWindowThreadProcessId(foreground, &processId);
            }
            if (processId != GetCurrentProcessId())
            {
                HideFlyout();
            }
        }

        InvalidateUi();
    }

    void ShowMainMenu(bool fromTray)
    {
        HMENU menu = CreatePopupMenu();
        if (!menu)
        {
            return;
        }

        // 托盘右键和面板右上角“…”用同一份菜单，逐项一致。
        AppendMenuW(menu, MF_STRING, kMenuOpen, L"打开面板");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);

        AppendMenuW(
            menu,
            MF_STRING | MF_GRAYED,
            0,
            g.liveAcrylic ? L"材质：DWM 实时亚克力" : L"材质：快照（系统透明效果关闭或不支持）");

        const std::wstring hotkeyHint = HotkeyHint();
        if (!hotkeyHint.empty())
        {
            AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, hotkeyHint.c_str());
        }

        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);

        UINT syncFlags = MF_STRING;
        if (!g.hasHdr || g.displays.size() < 2)
        {
            syncFlags |= MF_GRAYED;
        }
        AppendMenuW(menu, syncFlags, kMenuSync, L"同步所有显示器");

        UINT startupFlags = MF_STRING;
        if (StartupEnabled())
        {
            startupFlags |= MF_CHECKED;
        }
        AppendMenuW(menu, startupFlags, kMenuStartup, L"开机时启动");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, kMenuDisplaySettings, L"打开显示设置");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, kMenuExit, L"退出");

        int x = 0;
        int y = 0;
        UINT flags = TPM_RETURNCMD | TPM_RIGHTBUTTON;
        if (fromTray)
        {
            POINT cursor{};
            GetCursorPos(&cursor);
            x = cursor.x;
            y = cursor.y;
            flags |= TPM_RIGHTALIGN | TPM_BOTTOMALIGN;
        }
        else
        {
            // 没有“…”按钮了，键盘（Shift+F10 / 菜单键）唤起时贴着面板右上角
            RECT client{};
            GetClientRect(g.window, &client);
            POINT anchor{client.right - Px(12), Px(8)};
            ClientToScreen(g.window, &anchor);
            x = anchor.x;
            y = anchor.y;
            flags |= TPM_LEFTALIGN | TPM_TOPALIGN;
        }

        g.menuOpen = true;
        SetForegroundWindow(g.window);
        const int command = TrackPopupMenuEx(menu, flags, x, y, g.window, nullptr);
        g.menuOpen = false;
        DestroyMenu(menu);
        PostMessageW(g.window, WM_NULL, 0, 0);
        RefreshHoverFromCursor();

        switch (command)
        {
        case kMenuOpen:
            ShowFlyout();
            break;
        case kMenuSync:
            SyncAllDisplays();
            break;
        case kMenuStartup:
            SetStartupEnabled(!StartupEnabled());
            break;
        case kMenuDisplaySettings:
            OpenDisplaySettings();
            break;
        case kMenuExit:
            ExitApplication();
            return;
        default:
            break;
        }

        if (g.isOpen)
        {
            HWND foreground = GetForegroundWindow();
            DWORD processId = 0;
            if (foreground)
            {
                GetWindowThreadProcessId(foreground, &processId);
            }
            if (processId != GetCurrentProcessId())
            {
                HideFlyout();
            }
        }

        InvalidateUi();
    }

    void PerformAction(Part part)
    {
        switch (part)
        {
        case PartDisplay:
            ShowDisplayMenu();
            break;
        case PartSync:
            SyncAllDisplays();
            break;
        case PartEmptySettings:
            OpenDisplaySettings();
            break;
        default:
            break;
        }
    }

    void HideFlyoutImmediate();

    double BezierComponent(double t, double control1, double control2)
    {
        const double inverse = 1.0 - t;
        return 3.0 * inverse * inverse * t * control1 +
               3.0 * inverse * t * t * control2 +
               t * t * t;
    }

    double SolveBezier(double x1, double y1, double x2, double y2, double x)
    {
        double low = 0.0;
        double high = 1.0;
        double t = x;
        for (int i = 0; i < 10; ++i)
        {
            const double current = BezierComponent(t, x1, x2);
            if (std::abs(current - x) < 1e-4)
            {
                break;
            }
            if (current < x)
            {
                low = t;
            }
            else
            {
                high = t;
            }
            t = (low + high) * 0.5;
        }
        return BezierComponent(t, y1, y2);
    }

    // 进场 cubic-bezier(0.2, 0, 0, 1)：Material emphasized / 华为 EaseOut 的等价观感
    float EaseEnter(float t)
    {
        return static_cast<float>(SolveBezier(0.2, 0.0, 0.0, 1.0, t));
    }

    // 出场 cubic-bezier(0.3, 0, 1, 1)：Material standard accelerate / 华为 EaseIn
    float EaseExit(float t)
    {
        return static_cast<float>(SolveBezier(0.3, 0.0, 1.0, 1.0, t));
    }

    bool SystemAnimationsEnabled()
    {
        BOOL enabled = TRUE;
        if (!SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &enabled, 0))
        {
            return true;
        }
        return enabled != FALSE;
    }

    void ApplyFlyoutFrame(const RECT& target, int offset, int scaleMilli)
    {
        const int naturalWidth = target.right - target.left;
        const int naturalHeight = target.bottom - target.top;
        const int scale = std::clamp(scaleMilli, 200, 1000);
        const int width = std::max(1, naturalWidth * scale / 1000);
        const int height = std::max(1, naturalHeight * scale / 1000);

        // 锚点固定在底边中点：像从底部抽出来一样，左右对称展开。
        const int x = target.left + (naturalWidth - width) / 2;
        const int y = target.bottom - height + offset;
        SetWindowPos(
            g.window,
            nullptr,
            x,
            y,
            width,
            height,
            SWP_NOZORDER | SWP_NOACTIVATE);

        // 立刻同步重画。WM_PAINT 是队列里优先级最低的消息，动画期间每 6ms 就有
        // 一帧 WM_APP+4 投进来，它会被一直压到最后，表现就是「缩放全程空白、
        // 结束时内容才突然出现」。这里强制把这一帧画完再走下一帧。
        if (IsWindowVisible(g.window))
        {
            UpdateWindow(g.window);
        }
    }

    void MoveFlyoutTo(const RECT& target, int offset)
    {
        ApplyFlyoutFrame(target, offset, 1000);
    }

    int ScaleOfRect(const RECT& rect, const RECT& target)
    {
        const int naturalWidth = target.right - target.left;
        const int width = rect.right - rect.left;
        if (naturalWidth <= 0 || width <= 0)
        {
            return 1000;
        }
        return std::clamp(width * 1000 / naturalWidth, 200, 1000);
    }

    void AnimThreadLoop()
    {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
        LARGE_INTEGER frequency{};
        QueryPerformanceFrequency(&frequency);
        const double ticksToMs = 1000.0 / static_cast<double>(frequency.QuadPart);

        for (;;)
        {
            AnimJob job;
            {
                std::unique_lock<std::mutex> lock(g.animMutex);
                g.animCv.wait(lock, []
                {
                    return g.animStop || g.animPending;
                });
                if (g.animStop)
                {
                    return;
                }
                job = g.animJob;
                g.animPending = false;
            }

            LARGE_INTEGER started{};
            QueryPerformanceCounter(&started);
            for (;;)
            {
                if (g.animGeneration.load() != job.generation)
                {
                    break;
                }

                LARGE_INTEGER now{};
                QueryPerformanceCounter(&now);
                const double elapsedMs =
                    static_cast<double>(now.QuadPart - started.QuadPart) * ticksToMs;
                const double t = job.duration > 0
                                     ? std::min(1.0, elapsedMs / static_cast<double>(job.duration))
                                     : 1.0;
                const double eased = job.opening
                                         ? static_cast<double>(EaseEnter(static_cast<float>(t)))
                                         : static_cast<double>(EaseExit(static_cast<float>(t)));
                const int offset = static_cast<int>(
                    std::lround(job.from + (job.to - job.from) * eased));
                const int scale = static_cast<int>(
                    std::lround(job.scaleFrom + (job.scaleTo - job.scaleFrom) * eased));

                g.animLatestOffset.store(offset);
                g.animLatestScale.store(scale);
                if (!g.animFramePosted.exchange(true))
                {
                    PostMessageW(job.window, kAnimFrameMessage, job.generation, 0);
                }

                if (t >= 1.0)
                {
                    break;
                }

                // 等到下一个合成帧，动画自然跟随显示器刷新率。
                LARGE_INTEGER before{};
                QueryPerformanceCounter(&before);
                DwmFlush();
                LARGE_INTEGER after{};
                QueryPerformanceCounter(&after);
                const double frameMs =
                    static_cast<double>(after.QuadPart - before.QuadPart) * ticksToMs;
                if (frameMs < 1.0)
                {
                    Sleep(2);
                }
            }

            PostMessageW(job.window, kAnimDoneMessage, job.generation, 0);
        }
    }

    void StartAnimThread()
    {
        if (g.animThread.joinable())
        {
            return;
        }
        {
            std::lock_guard<std::mutex> lock(g.animMutex);
            g.animStop = false;
            g.animPending = false;
        }
        g.animThread = std::thread(AnimThreadLoop);
    }

    void StopAnimThread()
    {
        if (!g.animThread.joinable())
        {
            return;
        }
        ++g.animGeneration;
        {
            std::lock_guard<std::mutex> lock(g.animMutex);
            g.animStop = true;
            g.animPending = false;
        }
        g.animCv.notify_all();
        g.animThread.join();
    }

    void StartFlyoutAnimation(bool opening, const RECT& target, int fromOffset, int fromScale)
    {
        StartAnimThread();

        const int slide = Px(kOpenSlideDip);
        const int from = std::clamp(fromOffset, 0, slide);
        const int to = opening ? 0 : Px(kCloseSlideDip);
        const int scaleFrom = std::clamp(fromScale, 200, 1000);
        const int scaleTo = opening ? 1000 : kCloseScaleMilli;
        const int duration = opening ? kOpenAnimMs : kCloseAnimMs;
        const unsigned generation = ++g.animGeneration;

        g.animTarget = target;
        g.animating = true;
        g.animatingIn = opening;
        g.animFramePosted.store(false);

        {
            std::lock_guard<std::mutex> lock(g.animMutex);
            g.animJob = AnimJob{};
            g.animJob.window = g.window;
            g.animJob.target = target;
            g.animJob.from = from;
            g.animJob.to = to;
            g.animJob.scaleFrom = scaleFrom;
            g.animJob.scaleTo = scaleTo;
            g.animJob.duration = duration;
            g.animJob.opening = opening;
            g.animJob.generation = generation;
            g.animPending = true;
        }
        g.animCv.notify_one();

        ApplyFlyoutFrame(target, from, scaleFrom);
    }

    void SettleFlyoutEntrance()
    {
        if (!g.animating || !g.animatingIn)
        {
            return;
        }
        ++g.animGeneration;
        g.animating = false;
        g.animFramePosted.store(false);
        g.animLatestScale.store(1000);
        MoveFlyoutTo(g.animTarget, 0);
    }

    void CancelFlyoutClose()
    {
        if (!g.closing)
        {
            return;
        }
        ++g.animGeneration;
        g.animating = false;
        g.closing = false;
        g.animFramePosted.store(false);
        g.animLatestScale.store(1000);
        MoveFlyoutTo(g.animTarget, 0);
    }

    bool PrepareFlyout(bool refreshDisplays)
    {
        if (g.exitRequested)
        {
            return false;
        }

        POINT cursor{};
        if (GetCursorPos(&cursor))
        {
            g.showAnchor = cursor;
        }

        HMONITOR monitor = MonitorFromPoint(g.showAnchor, MONITOR_DEFAULTTONEAREST);
        MONITORINFO monitorInfo{};
        monitorInfo.cbSize = sizeof(monitorInfo);
        if (!GetMonitorInfoW(monitor, &monitorInfo))
        {
            monitor = MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
            GetMonitorInfoW(monitor, &monitorInfo);
        }

        UINT dpiX = 96;
        UINT dpiY = 96;
        if (FAILED(GetDpiForMonitor(monitor, MDT_EFFECTIVE_DPI, &dpiX, &dpiY)))
        {
            dpiX = 96;
        }

        if (g.dpi != static_cast<int>(dpiX))
        {
            g.dpi = static_cast<int>(dpiX);
            DestroyFonts();
            DestroyBackBuffer();
        }

        if (refreshDisplays)
        {
            RefreshDisplays();
        }
        CreateFonts();

        const int width = Px(kWindowWidthDip);
        const int height = Px(kWindowHeightDip);
        const int margin = Px(12);
        const int gap = Px(8);
        int x = monitorInfo.rcWork.right - width - margin;
        int y = monitorInfo.rcWork.bottom - height - gap;
        x = std::max(static_cast<int>(monitorInfo.rcWork.left) + margin, x);
        y = std::max(static_cast<int>(monitorInfo.rcWork.top) + margin, y);

        const bool wasVisible = IsWindowVisible(g.window) != FALSE;
        SetWindowPos(
            g.window,
            HWND_TOPMOST,
            x,
            y,
            width,
            height,
            SWP_NOACTIVATE);

        if (g.liveAcrylic)
        {
            ApplyAcrylicEffect(true);
        }

        if (!wasVisible)
        {
            if (g.liveAcrylic)
            {
                DestroyBackdrop();
            }
            else
            {
                CaptureBackdrop(x, y, width, height);
            }
        }

        g.flyoutRect = RECT{x, y, x + width, y + height};

        g.isOpen = true;
        g.hoverPart = PartNone;
        g.focusPart = g.hasHdr ? PartSlider : PartEmptySettings;
        g.keyboardFocusVisible = false;
        g.dragging = false;
        g.settingError = false;
        return true;
    }

    void ShowFlyout()
    {
        RECT before{};
        GetWindowRect(g.window, &before);
        const bool wasVisible = IsWindowVisible(g.window) != FALSE;

        if (!PrepareFlyout(true))
        {
            return;
        }

        g.flashing = false;
        g.closing = false;

        RECT target{};
        GetWindowRect(g.window, &target);

        // 系统关闭了「动画效果」时直接落位，尊重无障碍设置。
        const bool animate = SystemAnimationsEnabled();
        const int slide = Px(kOpenSlideDip);
        int from = slide;
        int scale = kOpenScaleMilli;
        if (wasVisible)
        {
            // 收起动画中途被打断：从当前位置、当前缩放续上，别跳回起点。
            from = std::clamp(static_cast<int>(before.top - target.top), 0, slide);
            scale = ScaleOfRect(before, target);
        }
        if (!animate)
        {
            from = 0;
            scale = 1000;
        }

        // 首帧就放在起点，避免先在终点闪一下。
        ApplyFlyoutFrame(target, from, scale);

        ShowWindow(g.window, SW_SHOW);
        SetForegroundWindow(g.window);
        SetFocus(g.window);
        UpdateWindow(g.window);
        if (animate)
        {
            // 等一帧合成：新窗口（尤其带亚克力背板）第一次上屏要几十毫秒，
            // 不等的话动画已经跑了一半窗口才出现，开头会"跳"一下。
            DwmFlush();
            StartFlyoutAnimation(true, target, from, scale);
        }
        g.sceneDirty = true;
        InvalidateRect(g.window, nullptr, FALSE);
        UpdateTrayTooltip();
    }

    void FlashFlyout()
    {
        if (g.isOpen)
        {
            SetTimer(g.window, kHideTimerId, kFlashHideMs, nullptr);
            InvalidateUi();
            return;
        }

        if (!PrepareFlyout(false))
        {
            return;
        }

        g.flashing = true;
        g.closing = false;

        RECT target{};
        GetWindowRect(g.window, &target);

        const bool animate = SystemAnimationsEnabled();
        const int from = animate ? Px(kOpenSlideDip) : 0;
        const int scale = animate ? kOpenScaleMilli : 1000;
        ApplyFlyoutFrame(target, from, scale);

        ShowWindow(g.window, SW_SHOWNOACTIVATE);
        UpdateWindow(g.window);
        if (animate)
        {
            DwmFlush();
            StartFlyoutAnimation(true, target, from, scale);
        }
        SetTimer(g.window, kHideTimerId, kFlashHideMs, nullptr);
        g.sceneDirty = true;
        InvalidateRect(g.window, nullptr, FALSE);
        UpdateTrayTooltip();
    }

    void HideFlyoutImmediate()
    {
        ++g.animGeneration;
        g.animFramePosted.store(false);
        g.animating = false;
        g.closing = false;
        KillTimer(g.window, kHideTimerId);
        if (g.dragging)
        {
            ReleaseCapture();
            g.dragging = false;
            QueueBrightnessWrite(g.percent, 1);
        }

        g.isOpen = false;
        g.flashing = false;
        g.hoverPart = PartNone;
        g.menuOpen = false;
        UpdateTrayTooltip();
        ShowWindow(g.window, SW_HIDE);
        DestroyFonts();
        DestroyBackBuffer();
        DestroyBackdrop();
        SetProcessWorkingSetSize(GetCurrentProcess(), static_cast<SIZE_T>(-1), static_cast<SIZE_T>(-1));
    }

    void HideFlyout()
    {
        if (g.exitRequested || !g.isOpen || g.closing || !IsWindowVisible(g.window) ||
            !SystemAnimationsEnabled())
        {
            HideFlyoutImmediate();
            return;
        }

        RECT current{};
        GetWindowRect(g.window, &current);
        const RECT target = g.flyoutRect;
        const int scale = ScaleOfRect(current, target);
        g.closing = true;
        g.flashing = false;
        StartFlyoutAnimation(false, target, 0, scale);
    }

    void PostHideIfInactive()
    {
        if (!g.isOpen || g.menuOpen || g.animating || g.closing)
        {
            return;
        }

        HWND foreground = GetForegroundWindow();
        DWORD processId = 0;
        if (foreground)
        {
            GetWindowThreadProcessId(foreground, &processId);
        }

        if (processId != GetCurrentProcessId())
        {
            HideFlyout();
        }
    }

    void HideWindowForSettings()
    {
        if (g.isOpen)
        {
            HideFlyout();
        }
    }

    void ExitApplication()
    {
        if (g.exitRequested)
        {
            return;
        }

        g.exitRequested = true;
        ++g.animGeneration;
        KillTimer(g.window, kHideTimerId);
        if (g.dragging)
        {
            ReleaseCapture();
            g.dragging = false;
        }
        if (g.hasHdr)
        {
            QueueBrightnessWrite(g.percent, 1);
        }
        UnregisterHotkeys();
        RemoveTrayIcon();
        DestroyFonts();
        DestroyBackBuffer();
        DestroyBackdrop();
        DestroyWindow(g.window);
    }

    void ToggleFlyout()
    {
        // 收起动画进行中再次触发，应当理解为“重新打开”，而不是立刻隐藏。
        if (g.isOpen && !g.flashing && !g.closing)
        {
            HideFlyout();
        }
        else
        {
            ShowFlyout();
        }
    }

    void OnHotkey(int id)
    {
        if (g.exitRequested)
        {
            return;
        }

        if (id == kHotkeyToggle)
        {
            ToggleFlyout();
            return;
        }

        if (id != kHotkeyUp && id != kHotkeyDown)
        {
            return;
        }

        if (!g.hasHdr)
        {
            FlashFlyout();
            return;
        }

        SetInteractivePercent(g.percent + (id == kHotkeyUp ? kHotkeyStep : -kHotkeyStep), true);
        FlashFlyout();
    }

    void SetSliderFromMouse(int mouseX)
    {
        if (!g.hasHdr)
        {
            return;
        }

        const Layout layout = BuildLayout();
        const int width = layout.track.right - layout.track.left;
        if (width <= 0)
        {
            return;
        }

        const int x = std::clamp(mouseX, static_cast<int>(layout.track.left), static_cast<int>(layout.track.right));
        const float visualPercent =
            static_cast<float>(x - layout.track.left) * 100.0f / static_cast<float>(width);
        const int percent = std::clamp(static_cast<int>(std::lround(visualPercent)), 0, 100);

        g.sliderVisualPercent = visualPercent;
        const bool liveBoosted = ApplyLiveBoost(visualPercent);
        if (percent != g.percent)
        {
            g.percent = percent;
            g.whiteLevel = SdrDisplayService::PercentToWhiteLevel(percent);
            g.settingError = false;
            if (!liveBoosted)
            {
                QueueBrightnessWrite(percent, 1);
            }
        }

        InvalidateUi();
    }

    void OnMouseMove(POINT point)
    {
        if (g.dragging)
        {
            SetSliderFromMouse(point.x);
            return;
        }

        Part hover = HitTest(point);
        if (hover == PartSlider && !g.hasHdr)
        {
            hover = PartNone;
        }

        if (hover != g.hoverPart)
        {
            g.hoverPart = hover;
            InvalidateUi();
        }

        if (!g.trackingMouse)
        {
            TRACKMOUSEEVENT track{};
            track.cbSize = sizeof(track);
            track.dwFlags = TME_LEAVE;
            track.hwndTrack = g.window;
            TrackMouseEvent(&track);
            g.trackingMouse = true;
        }
    }

    void OnTrayMessage(LPARAM lParam)
    {
        const UINT event = LOWORD(static_cast<DWORD_PTR>(lParam));
        if (event != NIN_SELECT &&
            event != NIN_KEYSELECT &&
            event != WM_LBUTTONUP &&
            event != WM_RBUTTONUP &&
            event != WM_CONTEXTMENU)
        {
            return;
        }

        const ULONGLONG now = GetTickCount64();
        if (now - g.lastTrayActivation < 250)
        {
            return;
        }
        g.lastTrayActivation = now;

        POINT cursor{};
        GetCursorPos(&cursor);
        g.showAnchor = cursor;

        if (event == WM_RBUTTONUP || event == WM_CONTEXTMENU)
        {
            ShowMainMenu(true);
        }
        else
        {
            ToggleFlyout();
        }
    }

    void MoveFocus(bool forward)
    {
        std::vector<Part> parts;
        if (g.hasHdr)
        {
            if (g.displays.size() > 1)
            {
                parts.push_back(PartDisplay);
            }
            parts.push_back(PartSlider);
            if (g.displays.size() > 1)
            {
                parts.push_back(PartSync);
            }
        }
        else
        {
            parts.push_back(PartEmptySettings);
        }

        auto iterator = std::find(parts.begin(), parts.end(), static_cast<Part>(g.focusPart));
        int index = iterator == parts.end() ? 0 : static_cast<int>(iterator - parts.begin());
        if (forward)
        {
            index = (index + 1) % static_cast<int>(parts.size());
        }
        else
        {
            index = (index - 1 + static_cast<int>(parts.size())) % static_cast<int>(parts.size());
        }

        g.focusPart = parts[index];
        g.keyboardFocusVisible = true;
        InvalidateUi();
    }

    void OnKeyDown(WPARAM key)
    {
        if (key == VK_ESCAPE)
        {
            HideFlyout();
            return;
        }

        if (key == VK_TAB)
        {
            MoveFocus((GetKeyState(VK_SHIFT) & 0x8000) == 0);
            return;
        }

        if (key == VK_APPS || (key == VK_F10 && (GetKeyState(VK_SHIFT) & 0x8000) != 0))
        {
            PostMessageW(g.window, kDeferredMenuMessage, 0, 0);
            return;
        }

        if (key == VK_RETURN || key == VK_SPACE)
        {
            if (g.focusPart != PartSlider)
            {
                PerformAction(static_cast<Part>(g.focusPart));
            }
            return;
        }

        if (!g.hasHdr)
        {
            return;
        }

        if (g.focusPart != PartSlider)
        {
            return;
        }

        int delta = 0;
        switch (key)
        {
        case VK_LEFT:
        case VK_DOWN:
            delta = (GetKeyState(VK_SHIFT) & 0x8000) ? -10 : -1;
            break;
        case VK_RIGHT:
        case VK_UP:
            delta = (GetKeyState(VK_SHIFT) & 0x8000) ? 10 : 1;
            break;
        case VK_PRIOR:
            delta = 10;
            break;
        case VK_NEXT:
            delta = -10;
            break;
        case VK_HOME:
            SetInteractivePercent(0, true);
            return;
        case VK_END:
            SetInteractivePercent(100, true);
            return;
        default:
            return;
        }

        if (delta != 0)
        {
            SetInteractivePercent(g.percent + delta, true);
        }
    }

    void OnLeftButtonDown(POINT point)
    {
        SettleFlyoutEntrance();
        SetFocus(g.window);
        const Part part = HitTest(point);
        if (part == PartNone)
        {
            return;
        }

        g.keyboardFocusVisible = false;
        g.focusPart = part;
        if (part == PartSlider)
        {
            g.dragging = true;
            SetCapture(g.window);
            SetSliderFromMouse(point.x);
        }
        else
        {
            PerformAction(part);
        }

        InvalidateUi();
    }

    void OnLeftButtonUp()
    {
        if (g.dragging)
        {
            g.dragging = false;
            ReleaseCapture();
            QueueBrightnessWrite(g.percent, 1);
            UpdateTrayTooltip();
            InvalidateUi();
        }
    }

    void OnMouseWheel(int delta)
    {
        if (!g.hasHdr || delta == 0)
        {
            return;
        }

        int steps = delta / WHEEL_DELTA;
        if (steps == 0)
        {
            steps = delta > 0 ? 1 : -1;
        }
        const bool fine = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
        const int step = (fine ? 1 : kWheelStep) * steps;
        SetInteractivePercent(g.percent + step, true);
    }

    LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
    {
        switch (message)
        {
        case WM_NCCALCSIZE:
            if (wParam)
            {
                return 0;
            }
            break;

        case WM_NCHITTEST:
            return HTCLIENT;

        case WM_NCACTIVATE:
            return TRUE;

        case WM_NCPAINT:
            return 0;

        case WM_ERASEBKGND:
            return 1;

        case WM_PAINT:
        {
            PAINTSTRUCT paint{};
            HDC dc = BeginPaint(window, &paint);
            EnsureBackBuffer(dc);
            if (g.bufferDc)
            {
                // 缩放动画只改窗口尺寸，画面内容没变；重画一次几百 KB 的
                // 双份 UI（live 亚克力还要合成）纯属浪费，直接复用后缓冲。
                if (g.sceneDirty)
                {
                    bool live =
                        g.liveAcrylic &&
                        EnsurePaintBuffer(dc) &&
                        EnsureMaskBuffer(dc) &&
                        g.paintBits &&
                        g.maskBits;
                    if (live)
                    {
                        const size_t bytes =
                            static_cast<size_t>((g.bufferWidth * 3 + 3) & ~3) * g.bufferHeight;
                        std::memset(g.paintBits, 0, bytes);
                        std::memset(g.maskBits, 0, bytes);
                        DrawUi(g.paintDc, false);
                        DrawUi(g.maskDc, true);
                        CompositeLiveSurface();
                    }
                    else
                    {
                        if (g.liveAcrylic)
                        {
                            g.liveAcrylic = false;
                            ApplyAcrylicEffect(false);
                        }
                        DrawUi(g.bufferDc, false);
                        MakeSurfaceOpaque();
                    }
                    g.sceneDirty = false;
                }
                RECT client{};
                GetClientRect(window, &client);
                const int clientWidth = client.right - client.left;
                const int clientHeight = client.bottom - client.top;
                if (clientWidth == g.bufferWidth && clientHeight == g.bufferHeight)
                {
                    BitBlt(dc, 0, 0, g.bufferWidth, g.bufferHeight, g.bufferDc, 0, 0, SRCCOPY);
                }
                else if (clientWidth > 0 && clientHeight > 0 && EnsurePresentBuffer(dc))
                {
                    // 先缩到中间缓冲、把 alpha 补成不透明，再 1:1 贴到窗口。
                    // StretchBlt 不保留 32bpp DIB 的 alpha（目标写成 0），而窗口带
                    // DWM 亚克力背板时 alpha=0 就是全透明 —— 之前"缩放期间只剩一块
                    // 空玻璃、结束时内容才出现"就是这个原因。
                    SetStretchBltMode(g.presentDc, HALFTONE);
                    SetBrushOrgEx(g.presentDc, 0, 0, nullptr);
                    StretchBlt(
                        g.presentDc,
                        0,
                        0,
                        clientWidth,
                        clientHeight,
                        g.bufferDc,
                        0,
                        0,
                        g.bufferWidth,
                        g.bufferHeight,
                        SRCCOPY);

                    // 把源图的 alpha 一并按同样比例搬过来（StretchBlt 只搬了颜色）。
                    // 直接填 255 会让动画期间失去亚克力透亮感，结束再"变亮"一下。
                    const auto* source = static_cast<const BYTE*>(g.bufferBits);
                    auto* pixels = static_cast<BYTE*>(g.presentBits);
                    const int sourceStride = g.bufferWidth * 4;
                    const int stride = g.presentWidth * 4;
                    const int sourceWidth = g.bufferWidth;
                    const int sourceHeight = g.bufferHeight;

                    const uint32_t stepX =
                        static_cast<uint32_t>((static_cast<uint64_t>(sourceWidth) << 16) / clientWidth);
                    const uint32_t stepY =
                        static_cast<uint32_t>((static_cast<uint64_t>(sourceHeight) << 16) / clientHeight);

                    uint32_t positionY = 0;
                    for (int y = 0; y < clientHeight; ++y)
                    {
                        const BYTE* sourceRow = source + static_cast<size_t>(positionY >> 16) * sourceStride;
                        BYTE* row = pixels + static_cast<size_t>(y) * stride;
                        uint32_t positionX = 0;
                        for (int x = 0; x < clientWidth; ++x)
                        {
                            row[x * 4 + 3] = sourceRow[(positionX >> 16) * 4 + 3];
                            positionX += stepX;
                            if ((positionX >> 16) >= static_cast<uint32_t>(sourceWidth))
                            {
                                positionX = static_cast<uint32_t>(sourceWidth - 1) << 16;
                            }
                        }
                        positionY += stepY;
                        if ((positionY >> 16) >= static_cast<uint32_t>(sourceHeight))
                        {
                            positionY = static_cast<uint32_t>(sourceHeight - 1) << 16;
                        }
                    }

                    BitBlt(dc, 0, 0, clientWidth, clientHeight, g.presentDc, 0, 0, SRCCOPY);
                }
            }
            EndPaint(window, &paint);
            return 0;
        }

        case WM_SIZE:
            // 渲染面尺寸固定，窗口尺寸变化（缩放动画）不需要重建缓冲。
            InvalidateRect(window, nullptr, FALSE);
            return 0;

        case WM_ACTIVATE:
            if (LOWORD(wParam) == WA_INACTIVE)
            {
                StartHideTimer();
            }
            else
            {
                KillTimer(window, kHideTimerId);
                CancelFlyoutClose();
            }
            return 0;

        case WM_HOTKEY:
            OnHotkey(static_cast<int>(wParam));
            return 0;

        case WM_TIMER:
            if (wParam == kHideTimerId)
            {
                KillTimer(window, kHideTimerId);
                PostHideIfInactive();
            }
            return 0;

        case kAnimFrameMessage:
            g.animFramePosted.store(false);
            if (static_cast<unsigned>(wParam) == g.animGeneration.load())
            {
                ApplyFlyoutFrame(
                    g.animTarget, g.animLatestOffset.load(), g.animLatestScale.load());
            }
            return 0;

        case kAnimDoneMessage:
            if (static_cast<unsigned>(wParam) == g.animGeneration.load())
            {
                g.animating = false;
                if (!g.animatingIn)
                {
                    g.closing = false;
                    HideFlyoutImmediate();
                }
            }
            return 0;

        case WM_MOUSEMOVE:
            OnMouseMove(POINT{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)});
            return 0;

        case WM_MOUSELEAVE:
            g.trackingMouse = false;
            if (!g.dragging && g.hoverPart != PartNone)
            {
                g.hoverPart = PartNone;
                InvalidateUi();
            }
            return 0;

        case WM_RBUTTONUP:
            // 延后一拍：直接在 WM_RBUTTONUP 里 TrackPopupMenu 会被残留的右键
            // 状态立刻取消（实测 13 ms 就返回 0，菜单闪一下就没）。
            PostMessageW(window, kDeferredMenuMessage, 1, 0);
            return 0;

        case kDeferredMenuMessage:
            ShowMainMenu(wParam != 0);
            return 0;

        case WM_LBUTTONDOWN:
            OnLeftButtonDown(POINT{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)});
            return 0;

        case WM_LBUTTONUP:
            OnLeftButtonUp();
            return 0;

        case WM_MOUSEWHEEL:
            OnMouseWheel(GET_WHEEL_DELTA_WPARAM(wParam));
            return 0;

        case WM_SETCURSOR:
        {
            POINT point{};
            GetCursorPos(&point);
            ScreenToClient(window, &point);
            const Part part = HitTest(point);
            LPCWSTR cursorName = IDC_ARROW;
            if (part == PartDisplay || part == PartSync || part == PartEmptySettings)
            {
                cursorName = IDC_HAND;
            }
            SetCursor(LoadCursorW(nullptr, cursorName));
            return TRUE;
        }

        case WM_KEYDOWN:
            OnKeyDown(wParam);
            return 0;

        case WM_GETDLGCODE:
            return DLGC_WANTARROWS | DLGC_WANTTAB | DLGC_WANTCHARS;

        case WM_SETTINGCHANGE:
        {
            const bool oldLightTaskbar = g.lightTaskbar;
            const bool wasLive = g.liveAcrylic;
            LoadTheme();
            ApplyWindowAttributes();
            if (wasLive != g.liveAcrylic)
            {
                DestroyMaskBuffer();
                DestroyBackdrop();
                g.backdropReady = false;
            }
            if (oldLightTaskbar != g.lightTaskbar)
            {
                RefreshTrayTheme();
            }
            InvalidateUi();
            return 0;
        }

        case WM_DWMCOMPOSITIONCHANGED:
            ApplyWindowAttributes();
            InvalidateUi();
            return 0;

        case WM_DISPLAYCHANGE:
            if (g.isOpen)
            {
                RefreshDisplays();
                InvalidateUi();
            }
            else
            {
                RefreshDisplays();
            }
            return 0;

        case WM_DPICHANGED:
            g.dpi = HIWORD(wParam);
            DestroyFonts();
            DestroyBackBuffer();
            RefreshTrayTheme();
            if (g.isOpen)
            {
                ShowFlyout();
            }
            return 0;

        case WM_CLOSE:
            if (g.exitRequested)
            {
                return DefWindowProcW(window, message, wParam, lParam);
            }
            HideFlyout();
            return 0;

        case WM_ENDSESSION:
            if (wParam)
            {
                ExitApplication();
            }
            return 0;

        case WM_DESTROY:
            RemoveTrayIcon();
            DestroyFonts();
            DestroyBackBuffer();
            DestroyBackdrop();
            PostQuitMessage(0);
            return 0;

        default:
            break;
        }

        if (message == g.taskbarCreatedMessage && g.taskbarCreatedMessage != 0)
        {
            g.trayAdded = false;
            AddTrayIcon();
            return 0;
        }

        if (message == kTrayMessage)
        {
            OnTrayMessage(lParam);
            return 0;
        }

        if (message == kWriteResultMessage)
        {
            const bool failed = wParam == 0;
            const int percent = static_cast<int>(lParam);
            if (g.hasHdr && percent == g.percent && failed != g.settingError)
            {
                g.settingError = failed;
                InvalidateUi();
            }
            if (!g.isOpen)
            {
                SetProcessWorkingSetSize(GetCurrentProcess(), static_cast<SIZE_T>(-1), static_cast<SIZE_T>(-1));
            }
            return 0;
        }

        if (message == kShowMessage)
        {
            ShowFlyout();
            return 0;
        }

        return DefWindowProcW(window, message, wParam, lParam);
    }

    bool IsBackgroundLaunch()
    {
        int argumentCount = 0;
        LPWSTR* arguments = CommandLineToArgvW(GetCommandLineW(), &argumentCount);
        if (!arguments)
        {
            return false;
        }

        bool background = false;
        for (int i = 1; i < argumentCount; ++i)
        {
            if (_wcsicmp(arguments[i], L"--background") == 0)
            {
                background = true;
                break;
            }
        }
        LocalFree(arguments);
        return background;
    }

    bool ActivateExistingInstance()
    {
        for (int attempt = 0; attempt < 20; ++attempt)
        {
            HWND existing = FindWindowW(kWindowClass, nullptr);
            if (existing)
            {
                PostMessageW(existing, kShowMessage, 0, 0);
                return true;
            }
            Sleep(50);
        }
        return false;
    }

    bool RegisterWindowClass()
    {
        WNDCLASSEXW windowClass{};
        windowClass.cbSize = sizeof(windowClass);
        windowClass.style = CS_HREDRAW | CS_VREDRAW;
        windowClass.lpfnWndProc = WindowProc;
        windowClass.hInstance = g.instance;
        windowClass.hIcon = LoadIconW(g.instance, MAKEINTRESOURCEW(IDI_APP));
        windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        windowClass.hbrBackground = nullptr;
        windowClass.lpszClassName = kWindowClass;
        windowClass.hIconSm = windowClass.hIcon;
        return RegisterClassExW(&windowClass) != 0;
    }
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int)
{
    g.instance = instance;
    Gdiplus::GdiplusStartupInput gdiplusInput;
    ULONG_PTR gdiplusToken = 0;
    g.gdiplusReady =
        Gdiplus::GdiplusStartup(&gdiplusToken, &gdiplusInput, nullptr) == Gdiplus::Ok;
    LoadTheme();

    HANDLE singleInstance = CreateMutexW(nullptr, FALSE, kMutexName);
    if (singleInstance && GetLastError() == ERROR_ALREADY_EXISTS)
    {
        ActivateExistingInstance();
        CloseHandle(singleInstance);
        return 0;
    }

    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    g.taskbarCreatedMessage = RegisterWindowMessageW(L"TaskbarCreated");

    if (!RegisterWindowClass())
    {
        if (singleInstance)
        {
            CloseHandle(singleInstance);
        }
        return 1;
    }

    g.window = CreateWindowExW(
        WS_EX_TOOLWINDOW,
        kWindowClass,
        kWindowTitle,
        WS_POPUP | WS_THICKFRAME,
        0,
        0,
        Px(kWindowWidthDip),
        Px(kWindowHeightDip),
        nullptr,
        nullptr,
        instance,
        nullptr);

    if (!g.window)
    {
        if (singleInstance)
        {
            CloseHandle(singleInstance);
        }
        return 1;
    }

    {
        const UINT windowDpi = GetDpiForWindow(g.window);
        if (windowDpi >= 96)
        {
            g.dpi = static_cast<int>(windowDpi);
        }
    }

    ApplyWindowAttributes();
    InitializeLiveBoost();
    StartBrightnessWriter();
    RefreshDisplays();
    AddTrayIcon();
    RegisterHotkeys();

    if (!IsBackgroundLaunch())
    {
        ShowFlyout();
    }
    else
    {
        SetProcessWorkingSetSize(GetCurrentProcess(), static_cast<SIZE_T>(-1), static_cast<SIZE_T>(-1));
    }

    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0)
    {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }

    UnregisterHotkeys();
    StopAnimThread();
    StopBrightnessWriter();
    RemoveTrayIcon();
    DestroyFonts();
    DestroyBackBuffer();
    DestroyBackdrop();

    if (singleInstance)
    {
        CloseHandle(singleInstance);
    }

    if (gdiplusToken)
    {
        Gdiplus::GdiplusShutdown(gdiplusToken);
    }

    return static_cast<int>(message.wParam);
}
