#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include <windows.h>
#include <windowsx.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <shellscalingapi.h>
#include <gdiplus.h>
#include <d2d1.h>
#include <dwrite.h>

#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "dwrite.lib")

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <ctime>
#include <cwchar>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "display_config.h"
#include "resource.h"

namespace
{
    constexpr wchar_t kWindowClass[] = L"SdrBrightnessTrayNative.Window";
    constexpr wchar_t kMenuClass[] = L"SdrBrightnessTrayNative.Menu";
    constexpr wchar_t kWindowTitle[] = L"SDR 内容亮度";
    constexpr wchar_t kMutexName[] = L"Local\\SdrBrightnessTray.Native.Instance";
    constexpr UINT kTrayMessage = WM_APP + 1;
    constexpr UINT kShowMessage = WM_APP + 2;
    constexpr UINT kWriteResultMessage = WM_APP + 3;
    constexpr UINT kTrayIconId = 1;
    constexpr UINT_PTR kHideTimerId = 1;
    constexpr UINT_PTR kSwitchTimerId = 3;
    constexpr int kSwitchAnimMs = 160;
    constexpr int kWindowWidthDip = 320;
    constexpr int kWindowHeightDip = 184;
    constexpr int kHideDelayMs = 180;

    // 面板/菜单的整体不透明度（0-100），可在右键菜单里调节。
    constexpr int kDefaultTransparency = 33;

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
    constexpr int kWheelStep = 5;
    constexpr UINT kFlashHideMs = 1400;

    constexpr int kMenuOpen = 1000;
    constexpr int kMenuSync = 1001;
    constexpr int kMenuStartup = 1002;

    constexpr int kMenuDisplaySettings = 1003;
    constexpr int kMenuExit = 1004;
    constexpr int kMenuDisplayBase = 2000;

    constexpr UINT_PTR kMenuSlideTimerId = 2;
    constexpr int kMenuSlideMs = 140;
    constexpr int kMenuItemHeightDip = 32;
    constexpr int kMenuInfoHeightDip = 21;
    constexpr int kMenuSeparatorHeightDip = 9;
    constexpr int kMenuPadYDip = 6;
    constexpr int kMenuPadXDip = 8;
    constexpr int kMenuIconColumnDip = 28;

    enum Part
    {
        PartNone = 0,
        PartDisplay = 1,
        PartSlider = 2,
        PartSync = 3,
        PartEye = 4,
        PartEmptySettings = 5,
        PartWarmth = 6
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
        RECT eyeRow{};
        RECT eyeSwitch{};
        RECT warmRow{};
        RECT warmTrack{};
        RECT warmHit{};
        RECT warmPercent{};
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
        int duration = 0;
        bool opening = false;
        unsigned generation = 0;
    };

    struct GammaBase
    {
        std::wstring device;
        WORD ramp[3][256]{};
        bool valid = false;
    };

    // 已实际写入显示器的暖色 Ramp，供亮度写入后的守护线程快速恢复用。
    struct AppliedRamp
    {
        std::wstring device;
        WORD ramp[3][256]{};
    };

    struct TextCmd
    {
        RECT rect{};
        HFONT font = nullptr;
        COLORREF color = 0;
        std::wstring text;
        UINT flags = 0;
    };

    // 把 DirectWrite 的 glyph 输出转交给位图渲染目标。
    class DWriteGlyphRenderer final : public IDWriteTextRenderer
    {
    public:
        DWriteGlyphRenderer(IDWriteBitmapRenderTarget* target, IDWriteRenderingParams* params)
            : target_(target), params_(params)
        {
        }

        void SetColor(COLORREF color)
        {
            color_ = color;
        }

        HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** object) override
        {
            if (!object)
            {
                return E_INVALIDARG;
            }
            if (riid == __uuidof(IUnknown) || riid == __uuidof(IDWriteTextRenderer))
            {
                *object = this;
                AddRef();
                return S_OK;
            }
            *object = nullptr;
            return E_NOINTERFACE;
        }

        ULONG STDMETHODCALLTYPE AddRef() override
        {
            return ++ref_;
        }

        ULONG STDMETHODCALLTYPE Release() override
        {
            const ULONG value = --ref_;
            if (value == 0)
            {
                delete this;
            }
            return value;
        }

        HRESULT STDMETHODCALLTYPE IsPixelSnappingDisabled(void*, BOOL* disabled) override
        {
            *disabled = FALSE;
            return S_OK;
        }

        HRESULT STDMETHODCALLTYPE GetCurrentTransform(void*, DWRITE_MATRIX* matrix) override
        {
            *matrix = DWRITE_MATRIX{1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f};
            return S_OK;
        }

        HRESULT STDMETHODCALLTYPE GetPixelsPerDip(void*, FLOAT* pixelsPerDip) override
        {
            *pixelsPerDip = 1.0f;
            return S_OK;
        }

        HRESULT STDMETHODCALLTYPE DrawGlyphRun(
            void*,
            FLOAT baselineX,
            FLOAT baselineY,
            DWRITE_MEASURING_MODE mode,
            const DWRITE_GLYPH_RUN* glyphRun,
            const DWRITE_GLYPH_RUN_DESCRIPTION*,
            IUnknown*) override
        {
            return target_->DrawGlyphRun(
                baselineX,
                baselineY,
                mode,
                glyphRun,
                params_,
                color_,
                nullptr);
        }

        HRESULT STDMETHODCALLTYPE DrawUnderline(void*, FLOAT, FLOAT, const DWRITE_UNDERLINE*, IUnknown*) override
        {
            return S_OK;
        }

        HRESULT STDMETHODCALLTYPE DrawStrikethrough(void*, FLOAT, FLOAT, const DWRITE_STRIKETHROUGH*, IUnknown*) override
        {
            return S_OK;
        }

        HRESULT STDMETHODCALLTYPE DrawInlineObject(void*, FLOAT, FLOAT, IDWriteInlineObject*, BOOL, BOOL, IUnknown*) override
        {
            return S_OK;
        }

    private:
        ULONG ref_ = 1;
        IDWriteBitmapRenderTarget* target_ = nullptr;
        IDWriteRenderingParams* params_ = nullptr;
        COLORREF color_ = RGB(255, 255, 255);
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
        int dragPart = PartNone;
        bool keyboardFocusVisible = false;
        bool dragging = false;
        bool menuOpen = false;

        bool eyeCare = false;
        int eyeCareLevel = 50;
        bool textMaskMode = false;

        IDWriteFactory* dwriteFactory = nullptr;
        IDWriteGdiInterop* dwriteGdi = nullptr;
        IDWriteBitmapRenderTarget* dwriteTarget = nullptr;
        IDWriteRenderingParams* dwriteParams = nullptr;
        IDWriteInlineObject* dwriteEllipsis = nullptr;
        DWriteGlyphRenderer* dwriteRenderer = nullptr;
        bool dwriteReady = false;
        std::map<HFONT, IDWriteTextFormat*> dwriteFormats;
        std::vector<TextCmd> textCmds;
        float eyeSwitchVisual = 0.0f;
        float eyeSwitchAnimFrom = 0.0f;
        float eyeSwitchAnimTo = 0.0f;
        ULONGLONG eyeSwitchAnimStart = 0;
        bool eyeSwitchAnimating = false;
        int transparency = kDefaultTransparency;
        std::vector<GammaBase> gammaBases;
        std::vector<AppliedRamp> appliedRamps;
        std::mutex rampMutex;
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
        bool writerGuard = false;
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

    // 自绘菜单：和主面板共用配色、字体与亚克力合成管线，避免系统原生菜单的灰底割裂感。
    struct MenuItem
    {
        enum class Kind
        {
            Action,
            Separator,
            Info,
            Slider
        };

        Kind kind = Kind::Action;
        int id = 0;
        std::wstring text;
        const wchar_t* glyph = nullptr;
        bool enabled = true;
        bool checked = false;
        int sliderValue = 0;
    };

    struct MenuFonts
    {
        int dpi = 0;
        HFONT body = nullptr;
        HFONT caption = nullptr;
        HFONT icon = nullptr;
        Gdiplus::Font* gdipBody = nullptr;
        Gdiplus::Font* gdipCaption = nullptr;
        Gdiplus::Font* gdipIcon = nullptr;
    };

    struct MenuState
    {
        HWND window = nullptr;
        std::vector<MenuItem> items;
        std::vector<RECT> rows;
        int width = 0;
        int height = 0;
        int padX = 0;
        int textX = 0;
        int iconX = 0;
        int hover = -1;
        int pressed = -1;
        bool shown = false;
        bool activated = false;
        bool sceneDirty = true;
        int result = 0;
        int dpi = 96;
        ULONGLONG lastClosed = 0;
        MenuFonts fonts;
        bool liveAcrylic = false;
        bool trackingMouse = false;
        bool draggingSlider = false;
        int sliderIndex = -1;
        RECT sliderTrack{};
        RECT sliderHit{};
        RECT target{};
        int slideFrom = 0;
        ULONGLONG slideStart = 0;

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
    };

    MenuState menu;

    int Px(int dip)
    {
        return MulDiv(dip, g.dpi, 96);
    }

    int SurfaceAlpha()
    {
        // transparency 是「透明度」：0 = 完全不透明，100 = 完全透明。
        return (100 - std::clamp(g.transparency, 0, 100)) * 255 / 100;
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
    bool SystemAnimationsEnabled();
    void ReleaseDWriteFormats();

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

    void WriteDword(const wchar_t* subkey, const wchar_t* name, DWORD value)
    {
        HKEY key = nullptr;
        if (RegCreateKeyExW(
                HKEY_CURRENT_USER,
                subkey,
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

        RegSetValueExW(
            key,
            name,
            0,
            REG_DWORD,
            reinterpret_cast<const BYTE*>(&value),
            sizeof(value));
        RegCloseKey(key);
    }

    bool WriteBinary(const wchar_t* subkey, const wchar_t* name, const std::vector<BYTE>& data)
    {
        HKEY key = nullptr;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, subkey, 0, KEY_SET_VALUE, &key) != ERROR_SUCCESS)
        {
            return false;
        }

        const LONG result = RegSetValueExW(
            key,
            name,
            0,
            REG_BINARY,
            data.data(),
            static_cast<DWORD>(data.size()));
        RegCloseKey(key);
        return result == ERROR_SUCCESS;
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
            g.surface = RGB(24, 24, 24);
            g.surfaceHighlight = RGB(43, 43, 43);
            g.border = RGB(64, 64, 64);
            g.card = RGB(43, 43, 43);
            g.cardHover = RGB(50, 50, 50);
            g.cardBorder = RGB(64, 64, 64);
            g.badgeBackground = RGB(53, 53, 53);
            g.badgeBorder = RGB(82, 82, 82);
            g.hoverBackground = RGB(53, 53, 53);
            g.textPrimary = RGB(255, 255, 255);
            g.textSecondary = RGB(198, 198, 198);
            g.textTertiary = RGB(160, 160, 160);
            g.track = RGB(90, 90, 90);
            g.hdrDot = RGB(108, 203, 95);
            g.error = RGB(255, 141, 141);
        }

        const int surfaceTint = g.lightTheme ? 14 : 48;
        const int cardTint = g.lightTheme ? 8 : 32;
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

    HFONT CreateUiFont(int dipHeight, int weight, const wchar_t* face, int dpi = 0)
    {
        if (dpi <= 0)
        {
            dpi = g.dpi;
        }
        LOGFONTW font{};
        font.lfHeight = -MulDiv(dipHeight, dpi, 96);
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

        // 字号阶梯：Body Strong 14 SemiBold / Body 14 / Caption 12 / Subtitle 20。
        // 中英文统一使用微软雅黑，图标仍走 Segoe Fluent Icons / MDL2。
        const wchar_t* uiFace = PickFace(
            {L"Microsoft YaHei UI", L"Microsoft YaHei"}, L"Microsoft YaHei UI");
        const wchar_t* displayFace = PickFace(
            {L"Microsoft YaHei UI", L"Microsoft YaHei"}, L"Microsoft YaHei UI");
        const wchar_t* iconFace = PickFace(
            {L"Segoe Fluent Icons", L"Segoe MDL2 Assets"}, L"Segoe MDL2 Assets");

        // Body Strong：正文尺寸下用 Text 光学尺寸（Display 是为 20px+ 设计的）
        g.fontTitle = CreateUiFont(14, FW_SEMIBOLD, uiFace);        // Body Strong
        g.fontBody = CreateUiFont(14, FW_NORMAL, uiFace);            // Body
        g.fontBodyBold = CreateUiFont(14, FW_SEMIBOLD, uiFace);      // Body Strong
        g.fontPercent = CreateUiFont(16, FW_SEMIBOLD, displayFace);  // 滑块数值
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
        ReleaseDWriteFormats();

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

    // 动画期间把固定渲染面按面积加权缩放到当前窗口尺寸（含 alpha）。
    // GDI 的 HALFTONE 拉伸带锐化滤波，会在文字边缘留下虚影，这里改为纯面积平均。
    void ScaleSurfaceToPresent(int destWidth, int destHeight)
    {
        if (!g.bufferBits || !g.presentBits || g.bufferWidth <= 0 || g.bufferHeight <= 0 ||
            g.presentWidth <= 0 || destWidth <= 0 || destHeight <= 0)
        {
            return;
        }

        const auto* source = static_cast<const BYTE*>(g.bufferBits);
        auto* dest = static_cast<BYTE*>(g.presentBits);
        const int sourceStride = g.bufferWidth * 4;
        const int destStride = g.presentWidth * 4;
        const double scaleX = static_cast<double>(g.bufferWidth) / destWidth;
        const double scaleY = static_cast<double>(g.bufferHeight) / destHeight;

        for (int y = 0; y < destHeight; ++y)
        {
            const double top = y * scaleY;
            const double bottom = (y + 1) * scaleY;
            const int firstY = static_cast<int>(top);
            const int lastY = std::min(g.bufferHeight, static_cast<int>(std::ceil(bottom)));
            BYTE* destRow = dest + static_cast<size_t>(y) * destStride;

            for (int x = 0; x < destWidth; ++x)
            {
                const double left = x * scaleX;
                const double right = (x + 1) * scaleX;
                const int firstX = static_cast<int>(left);
                const int lastX = std::min(g.bufferWidth, static_cast<int>(std::ceil(right)));

                double blue = 0.0;
                double green = 0.0;
                double red = 0.0;
                double alpha = 0.0;
                double weightSum = 0.0;
                for (int sy = firstY; sy < lastY; ++sy)
                {
                    const double wy =
                        std::min(bottom, static_cast<double>(sy + 1)) -
                        std::max(top, static_cast<double>(sy));
                    if (wy <= 0.0)
                    {
                        continue;
                    }
                    const BYTE* sourceRow = source + static_cast<size_t>(sy) * sourceStride;
                    for (int sx = firstX; sx < lastX; ++sx)
                    {
                        const double wx =
                            std::min(right, static_cast<double>(sx + 1)) -
                            std::max(left, static_cast<double>(sx));
                        if (wx <= 0.0)
                        {
                            continue;
                        }
                        const double weight = wx * wy;
                        const BYTE* pixel = sourceRow + static_cast<size_t>(sx) * 4;
                        blue += pixel[0] * weight;
                        green += pixel[1] * weight;
                        red += pixel[2] * weight;
                        alpha += pixel[3] * weight;
                        weightSum += weight;
                    }
                }

                BYTE* out = destRow + static_cast<size_t>(x) * 4;
                if (weightSum > 0.0)
                {
                    out[0] = static_cast<BYTE>(std::lround(blue / weightSum));
                    out[1] = static_cast<BYTE>(std::lround(green / weightSum));
                    out[2] = static_cast<BYTE>(std::lround(red / weightSum));
                    out[3] = static_cast<BYTE>(std::lround(alpha / weightSum));
                }
                else
                {
                    out[0] = 0;
                    out[1] = 0;
                    out[2] = 0;
                    out[3] = 0;
                }
            }
        }
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

    Gdiplus::Color GdiColor(COLORREF color)
    {
        return Gdiplus::Color(255, GetRValue(color), GetGValue(color), GetBValue(color));
    }

    // GDI 的 Ellipse/RoundRect 是硬边缘，开关和滑块用 GDI+ 做抗锯齿，失败时退回 GDI。
    void RoundFillAA(HDC dc, const RECT& rect, int radius, COLORREF fill, COLORREF border)
    {
        if (g.gdiplusReady)
        {
            Gdiplus::Graphics graphics(dc);
            if (graphics.GetLastStatus() == Gdiplus::Ok)
            {
                graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
                graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);

                const Gdiplus::REAL left = static_cast<Gdiplus::REAL>(rect.left);
                const Gdiplus::REAL top = static_cast<Gdiplus::REAL>(rect.top);
                const Gdiplus::REAL width = static_cast<Gdiplus::REAL>(rect.right - rect.left);
                const Gdiplus::REAL height = static_cast<Gdiplus::REAL>(rect.bottom - rect.top);
                const Gdiplus::REAL d = static_cast<Gdiplus::REAL>(radius) * 2.0f;

                Gdiplus::GraphicsPath path;
                if (d > 0.5f && width >= d && height >= d)
                {
                    path.AddArc(left, top, d, d, 180.0f, 90.0f);
                    path.AddArc(left + width - d, top, d, d, 270.0f, 90.0f);
                    path.AddArc(left + width - d, top + height - d, d, d, 0.0f, 90.0f);
                    path.AddArc(left, top + height - d, d, d, 90.0f, 90.0f);
                    path.CloseFigure();
                }
                else
                {
                    path.AddRectangle(Gdiplus::RectF(left, top, width, height));
                }

                Gdiplus::SolidBrush brush(GdiColor(fill));
                Gdiplus::Pen pen(GdiColor(border), 1.0f);
                if (graphics.FillPath(&brush, &path) == Gdiplus::Ok &&
                    graphics.DrawPath(&pen, &path) == Gdiplus::Ok)
                {
                    return;
                }
            }
        }
        RoundFill(dc, rect, radius, fill, border);
    }

    void CircleFillAA(HDC dc, int centerX, int centerY, int radius, COLORREF fill)
    {
        if (g.gdiplusReady && radius > 0)
        {
            Gdiplus::Graphics graphics(dc);
            if (graphics.GetLastStatus() == Gdiplus::Ok)
            {
                graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
                graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
                Gdiplus::SolidBrush brush(GdiColor(fill));
                const Gdiplus::REAL d = static_cast<Gdiplus::REAL>(radius * 2);
                if (graphics.FillEllipse(
                        &brush,
                        static_cast<Gdiplus::REAL>(centerX - radius),
                        static_cast<Gdiplus::REAL>(centerY - radius),
                        d,
                        d) == Gdiplus::Ok)
                {
                    return;
                }
            }
        }
        CircleFill(dc, centerX, centerY, radius, fill);
    }

    int TextWidth(HDC dc, HFONT font, const wchar_t* text)
    {
        HGDIOBJ previous = SelectObject(dc, font);
        SIZE size{};
        GetTextExtentPoint32W(dc, text, static_cast<int>(wcslen(text)), &size);
        SelectObject(dc, previous);
        return size.cx;
    }

    void DrawTextCore(
        HDC dc,
        RECT rect,
        HFONT font,
        Gdiplus::Font* gdiFont,
        COLORREF color,
        const wchar_t* text,
        UINT flags)
    {
        if (g.dwriteReady)
        {
            TextCmd cmd;
            cmd.rect = rect;
            cmd.font = font;
            cmd.color = color;
            cmd.text = text;
            cmd.flags = flags;
            g.textCmds.push_back(std::move(cmd));
            return;
        }

        if (gdiFont)
        {
            Gdiplus::Graphics graphics(dc);
            if (graphics.GetLastStatus() == Gdiplus::Ok)
            {
                // 不透明面板用 ClearType 网格拟合贴近系统文字；半透明时改用
                // 灰度抗锯齿，避免彩色亚像素在玻璃背景上出现描边。
                const bool clearType = !g.textMaskMode && SurfaceAlpha() > 200;
                graphics.SetTextRenderingHint(
                    clearType
                        ? Gdiplus::TextRenderingHintClearTypeGridFit
                        : Gdiplus::TextRenderingHintAntiAliasGridFit);
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

    void DrawTextIn(
        HDC dc,
        RECT rect,
        HFONT font,
        COLORREF color,
        const wchar_t* text,
        UINT flags = DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_HIDEPREFIX)
    {
        DrawTextCore(dc, rect, font, GdiplusFontFor(font), color, text, flags);
    }

    IDWriteTextFormat* DWriteFormatFor(HFONT font)
    {
        if (!g.dwriteReady || !g.dwriteFactory || !font)
        {
            return nullptr;
        }

        const auto found = g.dwriteFormats.find(font);
        if (found != g.dwriteFormats.end())
        {
            return found->second;
        }

        LOGFONTW logFont{};
        if (!GetObjectW(font, sizeof(logFont), &logFont))
        {
            return nullptr;
        }

        IDWriteTextFormat* format = nullptr;
        if (FAILED(g.dwriteFactory->CreateTextFormat(
                logFont.lfFaceName,
                nullptr,
                static_cast<DWRITE_FONT_WEIGHT>(logFont.lfWeight),
                DWRITE_FONT_STYLE_NORMAL,
                DWRITE_FONT_STRETCH_NORMAL,
                static_cast<FLOAT>(std::abs(logFont.lfHeight)),
                L"zh-CN",
                &format)) ||
            !format)
        {
            return nullptr;
        }

        g.dwriteFormats[font] = format;
        return format;
    }

    // 把本轮收集到的文字统一用 DirectWrite 画到透明位图，再按覆盖率混合进目标缓冲。
    void BlendTextPixels(
        const RECT& rect,
        COLORREF color,
        void* destBits,
        int destStride,
        int destBpp,
        const BYTE* sourceBits,
        int sourceStride,
        int targetHeight,
        bool targetTopDown)
    {
        if (!destBits || !sourceBits || destBpp < 3)
        {
            return;
        }

        auto* dest = static_cast<BYTE*>(destBits);
        const int destStep = destBpp / 8;
        const int r = GetRValue(color);
        const int green = GetGValue(color);
        const int b = GetBValue(color);

        const LONG left = std::max<LONG>(0, rect.left);
        const LONG top = std::max<LONG>(0, rect.top);
        const LONG right = std::min<LONG>(rect.right, 65535);
        const LONG bottom = std::min<LONG>(rect.bottom, 65535);
        if (right <= left || bottom <= top)
        {
            return;
        }

        for (LONG y = top; y < bottom; ++y)
        {
            const int sourceY = targetTopDown ? static_cast<int>(y) : (targetHeight - 1 - static_cast<int>(y));
            const BYTE* sourceRow = sourceBits + static_cast<size_t>(sourceY) * sourceStride + left * 4;
            BYTE* destRow = dest + static_cast<size_t>(y) * destStride;
            for (LONG x = left; x < right; ++x)
            {
                const BYTE* source = sourceRow + (x - left) * 4;
                const int coverage = std::max<int>(
                    source[0], std::max<int>(source[1], source[2]));
                if (coverage == 0)
                {
                    continue;
                }

                BYTE* pixel = destRow + x * destStep;
                const int inverse = 255 - coverage;
                pixel[0] = static_cast<BYTE>((pixel[0] * inverse + b * coverage) / 255);
                pixel[1] = static_cast<BYTE>((pixel[1] * inverse + green * coverage) / 255);
                pixel[2] = static_cast<BYTE>((pixel[2] * inverse + r * coverage) / 255);
            }
        }
    }

    void FlushText(
        HDC dc,
        int width,
        int height,
        bool maskMode,
        void* destBits,
        int destStride,
        int destBpp)
    {
        if (g.textCmds.empty())
        {
            return;
        }
        if (!g.dwriteReady || !g.dwriteFactory || !g.dwriteGdi || width <= 0 || height <= 0 ||
            !destBits)
        {
            g.textCmds.clear();
            return;
        }

        if (!g.dwriteTarget)
        {
            if (FAILED(g.dwriteGdi->CreateBitmapRenderTarget(
                    dc, static_cast<UINT32>(width), static_cast<UINT32>(height), &g.dwriteTarget)) ||
                !g.dwriteTarget)
            {
                g.textCmds.clear();
                return;
            }
            // 位图渲染目标默认继承 DC 的 DPI，而我们的坐标已经是物理像素，固定 1.0。
            g.dwriteTarget->SetPixelsPerDip(1.0f);
            g.dwriteRenderer = new DWriteGlyphRenderer(g.dwriteTarget, g.dwriteParams);
        }

        SIZE size{};
        g.dwriteTarget->GetSize(&size);
        if (size.cx != width || size.cy != height)
        {
            if (FAILED(g.dwriteTarget->Resize(
                    static_cast<UINT32>(width), static_cast<UINT32>(height))))
            {
                g.textCmds.clear();
                return;
            }
        }

        HDC targetDc = g.dwriteTarget->GetMemoryDC();
        HBITMAP targetBitmap = static_cast<HBITMAP>(GetCurrentObject(targetDc, OBJ_BITMAP));
        DIBSECTION targetInfo{};
        if (!targetBitmap ||
            GetObjectW(targetBitmap, sizeof(targetInfo), &targetInfo) != sizeof(targetInfo) ||
            !targetInfo.dsBm.bmBits)
        {
            g.textCmds.clear();
            return;
        }
        auto* targetBits = static_cast<const BYTE*>(targetInfo.dsBm.bmBits);
        const int targetStride = targetInfo.dsBm.bmWidthBytes;
        // DWrite 位图渲染目标的 scan0 始终是顶行（头里即使报正高度也按顶行读）。
        const bool targetTopDown = true;

        if (!g.dwriteEllipsis && !g.textCmds.empty())
        {
            IDWriteTextFormat* anyFormat = DWriteFormatFor(g.textCmds.front().font);
            if (anyFormat)
            {
                g.dwriteFactory->CreateEllipsisTrimmingSign(anyFormat, &g.dwriteEllipsis);
            }
        }

        for (const TextCmd& cmd : g.textCmds)
        {
            IDWriteTextFormat* format = DWriteFormatFor(cmd.font);
            if (!format)
            {
                continue;
            }

            format->SetTextAlignment(
                (cmd.flags & DT_CENTER) ? DWRITE_TEXT_ALIGNMENT_CENTER :
                (cmd.flags & DT_RIGHT) ? DWRITE_TEXT_ALIGNMENT_TRAILING :
                                         DWRITE_TEXT_ALIGNMENT_LEADING);
            format->SetParagraphAlignment(
                (cmd.flags & DT_VCENTER) ? DWRITE_PARAGRAPH_ALIGNMENT_CENTER :
                (cmd.flags & DT_BOTTOM) ? DWRITE_PARAGRAPH_ALIGNMENT_FAR :
                                          DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
            format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);

            if ((cmd.flags & DT_END_ELLIPSIS) && g.dwriteEllipsis)
            {
                DWRITE_TRIMMING trimming{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
                format->SetTrimming(&trimming, g.dwriteEllipsis);
            }
            else
            {
                format->SetTrimming(nullptr, nullptr);
            }

            const FLOAT layoutWidth = static_cast<FLOAT>(
                std::max<LONG>(1, cmd.rect.right - cmd.rect.left));
            const FLOAT layoutHeight = static_cast<FLOAT>(
                std::max<LONG>(1, cmd.rect.bottom - cmd.rect.top));

            IDWriteTextLayout* layout = nullptr;
            if (FAILED(g.dwriteFactory->CreateTextLayout(
                    cmd.text.c_str(),
                    static_cast<UINT32>(cmd.text.size()),
                    format,
                    layoutWidth,
                    layoutHeight,
                    &layout)) ||
                !layout)
            {
                continue;
            }

            RECT clearRect = cmd.rect;
            clearRect.left = std::max<LONG>(0, clearRect.left);
            clearRect.top = std::max<LONG>(0, clearRect.top);
            clearRect.right = std::min<LONG>(width, clearRect.right);
            clearRect.bottom = std::min<LONG>(height, clearRect.bottom);
            if (clearRect.right > clearRect.left && clearRect.bottom > clearRect.top)
            {
                FillSolid(targetDc, clearRect, RGB(0, 0, 0));

                g.dwriteRenderer->SetColor(RGB(255, 255, 255));
                layout->Draw(
                    nullptr,
                    g.dwriteRenderer,
                    static_cast<FLOAT>(cmd.rect.left),
                    static_cast<FLOAT>(cmd.rect.top));

                const COLORREF textColor = maskMode ? RGB(255, 255, 255) : cmd.color;
                BlendTextPixels(
                    clearRect,
                    textColor,
                    destBits,
                    destStride,
                    destBpp,
                    targetBits,
                    targetStride,
                    height,
                    targetTopDown);
            }
            layout->Release();
        }

        g.textCmds.clear();
    }

    void FlushUiText(HDC dc, int width, int height, bool maskMode)
    {
        void* bits = nullptr;
        int stride = 0;
        int bpp = 0;
        if (g.liveAcrylic && g.paintBits && g.maskBits)
        {
            bits = maskMode ? g.maskBits : g.paintBits;
            stride = (width * 3 + 3) & ~3;
            bpp = 24;
        }
        else
        {
            bits = g.bufferBits;
            stride = width * 4;
            bpp = 32;
        }
        FlushText(dc, width, height, maskMode, bits, stride, bpp);
    }

    void FlushMenuText(HDC dc, int width, int height, bool maskMode)
    {
        void* bits = nullptr;
        int stride = 0;
        int bpp = 0;
        if (menu.liveAcrylic && menu.paintBits && menu.maskBits)
        {
            bits = maskMode ? menu.maskBits : menu.paintBits;
            stride = (width * 3 + 3) & ~3;
            bpp = 24;
        }
        else
        {
            bits = menu.bufferBits;
            stride = width * 4;
            bpp = 32;
        }
        FlushText(dc, width, height, maskMode, bits, stride, bpp);
    }

    void ReleaseDWriteFormats()
    {
        for (auto& pair : g.dwriteFormats)
        {
            if (pair.second)
            {
                pair.second->Release();
            }
        }
        g.dwriteFormats.clear();
    }

    void InitializeDirectWrite()
    {
        g.dwriteReady = false;
        if (FAILED(DWriteCreateFactory(
                DWRITE_FACTORY_TYPE_SHARED,
                __uuidof(IDWriteFactory),
                reinterpret_cast<IUnknown**>(&g.dwriteFactory))) ||
            !g.dwriteFactory)
        {
            return;
        }

        if (FAILED(g.dwriteFactory->GetGdiInterop(&g.dwriteGdi)) || !g.dwriteGdi)
        {
            return;
        }

        // 灰度抗锯齿渲染参数：clearTypeLevel=0，适合透明位图叠加。
        if (FAILED(g.dwriteFactory->CreateCustomRenderingParams(
                1.8f,
                1.0f,
                0.0f,
                DWRITE_PIXEL_GEOMETRY_FLAT,
                DWRITE_RENDERING_MODE_NATURAL_SYMMETRIC,
                &g.dwriteParams)) ||
            !g.dwriteParams)
        {
            if (FAILED(g.dwriteFactory->CreateRenderingParams(&g.dwriteParams)))
            {
                return;
            }
        }

        g.dwriteReady = true;
    }

    void ShutdownDirectWrite()
    {
        ReleaseDWriteFormats();
        if (g.dwriteEllipsis)
        {
            g.dwriteEllipsis->Release();
            g.dwriteEllipsis = nullptr;
        }
        if (g.dwriteParams)
        {
            g.dwriteParams->Release();
            g.dwriteParams = nullptr;
        }
        if (g.dwriteRenderer)
        {
            g.dwriteRenderer->Release();
            g.dwriteRenderer = nullptr;
        }
        if (g.dwriteTarget)
        {
            g.dwriteTarget->Release();
            g.dwriteTarget = nullptr;
        }
        if (g.dwriteGdi)
        {
            g.dwriteGdi->Release();
            g.dwriteGdi = nullptr;
        }
        if (g.dwriteFactory)
        {
            g.dwriteFactory->Release();
            g.dwriteFactory = nullptr;
        }
        g.dwriteReady = false;
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
        // 显示器 10..38、护眼开关 46..74、暖色 82..106、亮度 114..142、说明 150..166。
        const int textX = pad + Px(32);
        layout.textX = textX;

        layout.displayRow = {pad, Px(10), width - pad, Px(38)};
        layout.eyeRow = {pad, Px(46), width - pad, Px(74)};
        // 「护眼模式」四字约 56 DIP，开关紧跟文字右侧，留 10 DIP 间距
        layout.eyeSwitch = {textX + Px(66), Px(52), textX + Px(98), Px(68)};

        layout.warmRow = {pad, Px(82), width - pad, Px(106)};
        // 数值列与亮度滑条同宽，保证两条滑轨等长
        layout.warmPercent = {width - pad - Px(48), Px(82), width - pad, Px(106)};
        layout.warmTrack = {textX, Px(92), layout.warmPercent.left - Px(12), Px(95)};
        layout.warmHit = {textX - Px(10), Px(82), layout.warmPercent.left, Px(106)};

        layout.sliderRow = {pad, Px(114), width - pad, Px(142)};
        layout.percent = {width - pad - Px(48), Px(114), width - pad, Px(142)};
        const int trackRight = layout.percent.left - Px(12);
        const int trackCenter = Px(128);
        layout.track = {textX, trackCenter - Px(2), trackRight, trackCenter + Px(1)};
        layout.sliderHit = {textX - Px(10), Px(114), trackRight + Px(10), Px(142)};

        layout.caption = {textX, Px(150), width - pad, Px(166)};

        // 「同步全部」放在说明行右端，和其他内容同一行
        layout.sync = {width - pad - Px(56), Px(146), width - pad, Px(166)};
        layout.emptyCircle = {width / 2 - Px(16), Px(38), width / 2 + Px(16), Px(70)};
        layout.emptyButton = {
            (width - Px(108)) / 2,
            Px(122),
            (width + Px(108)) / 2,
            Px(146)};
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
            RECT eyeHit = layout.eyeSwitch;
            InflateRect(&eyeHit, Px(4), Px(4));
            if (PtInRect(&eyeHit, point))
            {
                return PartEye;
            }
            if (g.eyeCare && PtInRect(&layout.warmHit, point))
            {
                return PartWarmth;
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

    void CompositeLiveSurface(void* paintBits, void* maskBits, void* outputBits, int width, int height)
    {
        if (!paintBits || !maskBits || !outputBits || width <= 0 || height <= 0)
        {
            return;
        }

        const int stride = (width * 3 + 3) & ~3;
        auto* paint = static_cast<BYTE*>(paintBits);
        auto* mask = static_cast<BYTE*>(maskBits);
        auto* output = static_cast<BYTE*>(outputBits);
        const int scrimAlpha = SurfaceAlpha();
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

    void DrawSwitch(HDC dc, const RECT& rect, float progress, bool hover, bool maskMode)
    {
        const float t = std::clamp(progress, 0.0f, 1.0f);
        const int tPercent = static_cast<int>(std::lround(t * 100.0f));
        const int radius = (rect.bottom - rect.top) / 2;

        const COLORREF offTrack = BlendColor(g.card, g.track, 55);
        COLORREF trackColor = BlendColor(offTrack, g.accent, tPercent);
        if (hover)
        {
            trackColor = BlendColor(trackColor, RGB(255, 255, 255), 8);
        }
        const COLORREF borderColor = t > 0.001f
            ? BlendColor(g.border, g.accent, tPercent)
            : g.border;

        RoundFillAA(
            dc,
            rect,
            radius,
            UiColor(trackColor, maskMode),
            UiColor(borderColor, maskMode));

        const int knobRadius = std::max(1, Px(4));
        const int centerY = (rect.top + rect.bottom) / 2;
        const int offCenter = rect.left + Px(4) + knobRadius;
        const int onCenter = rect.right - Px(4) - knobRadius;
        const int centerX = offCenter + static_cast<int>(
            std::lround(static_cast<float>(onCenter - offCenter) * t));

        const COLORREF offKnob = BlendColor(g.textPrimary, g.surface, 35);
        const COLORREF knobColor = BlendColor(offKnob, RGB(255, 255, 255), tPercent);
        CircleFillAA(dc, centerX, centerY, knobRadius, UiColor(knobColor, maskMode));
    }

    void DrawUi(HDC dc, bool maskMode)
    {
        g.textMaskMode = maskMode;
        g.textCmds.clear();
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

            RECT emptyTitle{layout.padX, Px(74), width - layout.padX, Px(94)};
            DrawTextIn(dc, emptyTitle, g.fontTitle, UiColor(g.textPrimary, maskMode), L"未检测到 HDR 显示器", DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

            RECT emptyDescription{width / 2 - Px(140), Px(96), width / 2 + Px(140), Px(112)};
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
            FlushUiText(dc, width, height, maskMode);
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
        const int thumbRadius = activeThumb ? Px(9) : Px(8);
        CircleFillAA(dc, thumbCenterX, thumbCenterY, thumbRadius, UiColor(g.accent, maskMode));
        CircleFillAA(dc, thumbCenterX, thumbCenterY, Px(2), UiColor(g.surface, maskMode));

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

        // 护眼模式：只有右侧开关本身可点、可悬停，整行不参与交互
        const bool eyeHover = g.hoverPart == PartEye;

        RECT eyeLabel{
            layout.textX,
            layout.eyeRow.top,
            layout.eyeSwitch.left - Px(8),
            layout.eyeRow.bottom};
        DrawTextIn(
            dc,
            eyeLabel,
            g.fontBody,
            UiColor(g.textPrimary, maskMode),
            L"护眼模式",
            DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

        DrawSwitch(dc, layout.eyeSwitch, g.eyeSwitchVisual, eyeHover, maskMode);
        DrawFocusOutline(dc, layout.eyeSwitch, PartEye, Px(8));

        // 护眼图标与暖色滑块同排对齐（和亮度滑块的太阳图标同一栅格）
        RECT eyeIcon{
            layout.padX,
            layout.warmRow.top,
            layout.padX + Px(24),
            layout.warmRow.bottom};
        DrawTextIn(
            dc,
            eyeIcon,
            g.iconBrightness,
            UiColor(g.eyeCare ? g.textSecondary : g.textTertiary, maskMode),
            L"\xE890",
            DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

        // 暖色强度：关闭时整行降级，且不响应鼠标
        const COLORREF warmTrackColor = g.eyeCare
            ? g.track
            : BlendColor(g.track, g.surface, 55);
        RoundFill(
            dc,
            layout.warmTrack,
            Px(2),
            UiColor(warmTrackColor, maskMode),
            UiColor(warmTrackColor, maskMode));

        const int warmTrackWidth = layout.warmTrack.right - layout.warmTrack.left;
        const int warmLevel = std::clamp(g.eyeCareLevel, 0, 100);
        const int warmThumbX = layout.warmTrack.left + warmTrackWidth * warmLevel / 100;
        const int warmCenterY = (layout.warmTrack.top + layout.warmTrack.bottom) / 2;

        if (g.eyeCare && warmLevel > 0)
        {
            const int saved = SaveDC(dc);
            IntersectClipRect(
                dc,
                layout.warmTrack.left - Px(4),
                layout.warmTrack.top - Px(4),
                warmThumbX,
                layout.warmTrack.bottom + Px(4));
            RoundFill(
                dc,
                layout.warmTrack,
                Px(2),
                UiColor(g.accent, maskMode),
                UiColor(g.accent, maskMode));
            RestoreDC(dc, saved);
        }

        const bool warmActive =
            g.eyeCare && ((g.dragging && g.dragPart == PartWarmth) || g.hoverPart == PartWarmth);
        const int warmThumbRadius = g.eyeCare ? (warmActive ? Px(8) : Px(7)) : Px(6);
        CircleFillAA(
            dc,
            warmThumbX,
            warmCenterY,
            warmThumbRadius,
            UiColor(g.eyeCare ? g.accent : g.textTertiary, maskMode));
        CircleFillAA(dc, warmThumbX, warmCenterY, Px(2), UiColor(g.surface, maskMode));

        if (g.keyboardFocusVisible && g.focusPart == PartWarmth && GetFocus() == g.window)
        {
            CircleStroke(
                dc,
                warmThumbX,
                warmCenterY,
                warmThumbRadius + Px(3),
                UiColor(g.accent, maskMode));
        }

        wchar_t warmText[16]{};
        swprintf_s(warmText, L"%d", warmLevel);
        DrawTextIn(
            dc,
            layout.warmPercent,
            g.fontBody,
            UiColor(g.eyeCare ? g.textPrimary : g.textTertiary, maskMode),
            warmText,
            DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

        // 护眼与亮度之间的分组分隔线
        RECT divider{layout.padX, Px(110), width - layout.padX, Px(111)};
        FillSolid(dc, divider, UiColor(g.border, maskMode));

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
        FlushUiText(dc, width, height, maskMode);
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
            if (g.eyeCare)
            {
                swprintf_s(
                    buffer,
                    L"SDR 亮度  %d%%  ·  %d 尼特  ·  护眼 %d%%  ·  %s",
                    g.percent,
                    nits,
                    g.eyeCareLevel,
                    DisplayLabel(g.displays[g.currentDisplay]).c_str());
            }
            else
            {
                swprintf_s(
                    buffer,
                    L"SDR 亮度  %d%%  ·  %d 尼特  ·  %s",
                    g.percent,
                    nits,
                    DisplayLabel(g.displays[g.currentDisplay]).c_str());
            }
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

    bool ApplyAcrylicEffect(bool enable, HWND target = nullptr)
    {
        static SetWindowCompositionAttributeFn setAttribute = []() -> SetWindowCompositionAttributeFn
        {
            HMODULE user32 = GetModuleHandleW(L"user32.dll");
            return user32
                ? reinterpret_cast<SetWindowCompositionAttributeFn>(
                      GetProcAddress(user32, "SetWindowCompositionAttribute"))
                : nullptr;
        }();

        if (!target)
        {
            target = g.window;
        }
        if (!setAttribute || !target)
        {
            return false;
        }

        AccentPolicy policy{};
        if (enable && IsWindows11OrGreater() && TransparencyEnabled())
        {
            const COLORREF tint = g.lightTheme
                ? BlendColor(RGB(242, 242, 242), g.accent, 10)
                : BlendColor(RGB(22, 22, 22), g.accent, 32);
            policy.state = 4;
            policy.flags = 0x20 | 0x40 | 0x80 | 0x100;
            policy.gradientColor =
                (static_cast<DWORD>(SurfaceAlpha()) << 24) |
                (static_cast<DWORD>(GetBValue(tint)) << 16) |
                (static_cast<DWORD>(GetGValue(tint)) << 8) |
                static_cast<DWORD>(GetRValue(tint));
        }

        CompositionAttributeData data{19, &policy, sizeof(policy)};
        setAttribute(target, &data);
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

    // ---- 护眼模式：用显示器 Gamma Ramp 做暖色校正 ----

    // 系统夜间模式（Night Light）的开关状态存在 CloudStore 的 Bond 二进制里。
    // 开启护眼模式时把它关掉，避免两套暖色叠加。
    constexpr wchar_t kNightLightStateKey[] =
        L"Software\\Microsoft\\Windows\\CurrentVersion\\CloudStore\\Store\\DefaultAccount\\Current\\"
        L"default$windows.data.bluelightreduction.bluelightreductionstate\\"
        L"windows.data.bluelightreduction.bluelightreductionstate";

    struct BondReader
    {
        const std::vector<BYTE>& data;
        size_t pos = 0;
        bool ok = true;

        int Byte()
        {
            if (!ok || pos >= data.size())
            {
                ok = false;
                return 0;
            }
            return data[pos++];
        }

        uint64_t Varint()
        {
            uint64_t value = 0;
            int shift = 0;
            for (int i = 0; i < 10; ++i)
            {
                const int byte = Byte();
                if (!ok)
                {
                    return 0;
                }
                value |= static_cast<uint64_t>(byte & 0x7F) << shift;
                if ((byte & 0x80) == 0)
                {
                    return value;
                }
                shift += 7;
            }
            ok = false;
            return 0;
        }

        void SkipBytes(size_t count)
        {
            if (!ok || pos + count > data.size())
            {
                ok = false;
                return;
            }
            pos += count;
        }

        // 返回 false 表示字段结束；用 ok 区分“正常结束”和“解析失败”。
        bool ReadField(uint16_t& id, int& type)
        {
            const int raw = Byte();
            if (!ok)
            {
                return false;
            }

            type = raw & 0x1F;
            const int idBits = raw >> 5;
            if (type == 0 || type == 1 || type < 2 || type > 18)
            {
                return false;
            }

            if (idBits == 7)
            {
                const int low = Byte();
                const int high = Byte();
                id = static_cast<uint16_t>(low | (high << 8));
            }
            else if (idBits == 6)
            {
                id = static_cast<uint16_t>(Byte());
            }
            else
            {
                id = static_cast<uint16_t>(idBits);
            }
            return ok;
        }

        bool SkipValue(int type, int depth)
        {
            if (depth > 32)
            {
                ok = false;
                return false;
            }

            switch (type)
            {
            case 2:
            case 3:
            case 14:
                Byte();
                break;
            case 4:
            case 5:
            case 6:
            case 15:
            case 16:
            case 17:
                Varint();
                break;
            case 7:
                SkipBytes(4);
                break;
            case 8:
                SkipBytes(8);
                break;
            case 9:
            {
                const uint64_t length = Varint();
                if (ok && length <= data.size())
                {
                    SkipBytes(static_cast<size_t>(length));
                }
                break;
            }
            case 18:
            {
                const uint64_t units = Varint();
                if (ok && units <= data.size())
                {
                    SkipBytes(static_cast<size_t>(units) * 2);
                }
                break;
            }
            case 10:
                for (;;)
                {
                    uint16_t id = 0;
                    int fieldType = 0;
                    if (!ReadField(id, fieldType))
                    {
                        break;
                    }
                    if (!SkipValue(fieldType, depth + 1))
                    {
                        break;
                    }
                }
                break;
            case 11:
            case 12:
            {
                const int elementType = Byte() & 0x1F;
                const uint64_t count = Varint();
                for (uint64_t i = 0; i < count && ok; ++i)
                {
                    SkipValue(elementType, depth + 1);
                }
                break;
            }
            case 13:
            {
                const int keyType = Byte() & 0x1F;
                const int valueType = Byte() & 0x1F;
                const uint64_t count = Varint();
                for (uint64_t i = 0; i < count && ok; ++i)
                {
                    SkipValue(keyType, depth + 1);
                    SkipValue(valueType, depth + 1);
                }
                break;
            }
            default:
                ok = false;
                break;
            }
            return ok;
        }
    };

    struct NightLightState
    {
        bool parsed = false;
        bool active = false;
        uint64_t modified = 0;
        size_t modifiedValueBegin = 0;
        size_t modifiedValueEnd = 0;
        size_t countBegin = 0;
        size_t payloadBegin = 0;
        size_t payloadEnd = 0;
        size_t statusBegin = 0;
        size_t statusEnd = 0;
        uint64_t payloadCount = 0;
    };

    void WriteBondVarint(std::vector<BYTE>& out, uint64_t value)
    {
        do
        {
            BYTE byte = static_cast<BYTE>(value & 0x7F);
            value >>= 7;
            if (value != 0)
            {
                byte |= 0x80;
            }
            out.push_back(byte);
        } while (value != 0);
    }

    NightLightState ParseNightLightState(const std::vector<BYTE>& data)
    {
        NightLightState state;
        BondReader reader{data};
        if (reader.Byte() != 0x43 || reader.Byte() != 0x42 ||
            reader.Byte() != 0x01 || reader.Byte() != 0x00)
        {
            return state;
        }

        uint16_t id = 0;
        int type = 0;
        while (reader.ReadField(id, type))
        {
            if (id != 1 || type != 10)
            {
                if (!reader.SkipValue(type, 0))
                {
                    return state;
                }
                continue;
            }

            uint16_t containerId = 0;
            int containerType = 0;
            while (reader.ReadField(containerId, containerType))
            {
                if (containerId == 0 && containerType == 6)
                {
                    state.modifiedValueBegin = reader.pos;
                    state.modified = reader.Varint();
                    state.modifiedValueEnd = reader.pos;
                    continue;
                }

                if (containerId != 1 || containerType != 10)
                {
                    if (!reader.SkipValue(containerType, 0))
                    {
                        return state;
                    }
                    continue;
                }

                uint16_t wrapperId = 0;
                int wrapperType = 0;
                while (reader.ReadField(wrapperId, wrapperType))
                {
                    if (wrapperId != 1 || wrapperType != 11)
                    {
                        if (!reader.SkipValue(wrapperType, 0))
                        {
                            return state;
                        }
                        continue;
                    }

                    const int elementType = reader.Byte() & 0x1F;
                    if (elementType != 14)
                    {
                        return state;
                    }
                    state.countBegin = reader.pos;
                    state.payloadCount = reader.Varint();
                    state.payloadBegin = reader.pos;
                    if (!reader.ok || state.payloadCount > data.size())
                    {
                        return state;
                    }
                    state.payloadEnd = state.payloadBegin + static_cast<size_t>(state.payloadCount);
                    if (state.payloadEnd > data.size())
                    {
                        return state;
                    }
                    reader.pos = state.payloadEnd;
                }
            }
            break;
        }

        if (!reader.ok || state.payloadBegin == 0)
        {
            return state;
        }

        BondReader payload{data};
        payload.pos = state.payloadBegin;
        if (payload.Byte() != 0x43 || payload.Byte() != 0x42 ||
            payload.Byte() != 0x01 || payload.Byte() != 0x00)
        {
            return state;
        }

        uint16_t payloadId = 0;
        int payloadType = 0;
        if (payload.ReadField(payloadId, payloadType) &&
            payloadId == 0 && payloadType == 16)
        {
            state.active = true;
            state.statusBegin = state.payloadBegin + 4;
            payload.Varint();
            state.statusEnd = payload.pos;
        }
        else if (!payload.ok)
        {
            return state;
        }

        state.parsed = true;
        return state;
    }

    bool DisableSystemNightLight()
    {
        std::vector<BYTE> data;
        if (!ReadBinary(kNightLightStateKey, L"Data", data) || data.size() < 20)
        {
            return false;
        }

        const NightLightState state = ParseNightLightState(data);
        if (!state.parsed || !state.active)
        {
            return false;
        }

        const size_t removed = state.statusEnd - state.statusBegin;
        if (removed == 0 || removed > state.payloadCount)
        {
            return false;
        }

        uint64_t modified = state.modified + 2;
        const uint64_t now = static_cast<uint64_t>(std::time(nullptr));
        if (now > modified)
        {
            modified = now;
        }

        std::vector<BYTE> output;
        output.reserve(data.size() - removed + 2);
        output.insert(output.end(), data.begin(), data.begin() + state.modifiedValueBegin);
        WriteBondVarint(output, modified);
        output.insert(output.end(), data.begin() + state.modifiedValueEnd, data.begin() + state.countBegin);
        WriteBondVarint(output, state.payloadCount - removed);
        output.insert(output.end(), data.begin() + state.payloadBegin, data.begin() + state.statusBegin);
        output.insert(output.end(), data.begin() + state.statusEnd, data.begin() + state.payloadEnd);
        output.insert(output.end(), data.begin() + state.payloadEnd, data.end());
        return WriteBinary(kNightLightStateKey, L"Data", output);
    }

    void BuildWarmRamp(const WORD* base, int level, WORD ramp[3][256])
    {
        const double t = std::clamp(level, 0, 100) / 100.0;
        // 6500K -> 约 3400K：蓝通道大幅收敛、绿通道略降，红通道基本保持。
        // Windows 会拒绝通道上限低于 32768 的 ramp，所以蓝通道最多收到 0.52。
        const double gains[3] = {
            1.0 - 0.01 * t,
            1.0 - 0.15 * t,
            1.0 - 0.48 * t};

        for (int channel = 0; channel < 3; ++channel)
        {
            for (int i = 0; i < 256; ++i)
            {
                const double source = base
                    ? base[channel * 256 + i] / 65535.0
                    : i / 255.0;
                const double value = std::clamp(source * gains[channel], 0.0, 1.0);
                ramp[channel][i] = static_cast<WORD>(std::lround(value * 65535.0));
            }
        }
    }

    void RestoreGammaRamps()
    {
        for (const GammaBase& base : g.gammaBases)
        {
            if (!base.valid)
            {
                continue;
            }
            HDC dc = CreateDCW(nullptr, base.device.c_str(), nullptr, nullptr);
            if (dc)
            {
                SetDeviceGammaRamp(dc, const_cast<WORD*>(&base.ramp[0][0]));
                DeleteDC(dc);
            }
        }
    }

    void ApplyEyeCareRamp()
    {
        const int level = g.eyeCare ? std::clamp(g.eyeCareLevel, 0, 100) : 0;
        std::vector<AppliedRamp> snapshot;
        snapshot.reserve(g.gammaBases.size());

        for (const GammaBase& base : g.gammaBases)
        {
            AppliedRamp applied;
            applied.device = base.device;

            if (level <= 0)
            {
                if (base.valid)
                {
                    std::memcpy(applied.ramp, base.ramp, sizeof(applied.ramp));
                }
                else
                {
                    for (int channel = 0; channel < 3; ++channel)
                    {
                        for (int i = 0; i < 256; ++i)
                        {
                            applied.ramp[channel][i] = static_cast<WORD>(i * 257);
                        }
                    }
                }
            }
            else
            {
                BuildWarmRamp(base.valid ? &base.ramp[0][0] : nullptr, level, applied.ramp);
            }

            HDC dc = CreateDCW(nullptr, applied.device.c_str(), nullptr, nullptr);
            if (dc)
            {
                SetDeviceGammaRamp(dc, &applied.ramp[0][0]);
                DeleteDC(dc);
            }
            snapshot.push_back(std::move(applied));
        }

        std::lock_guard<std::mutex> lock(g.rampMutex);
        g.appliedRamps = std::move(snapshot);
    }

    // 亮度写入后 Windows 会在几十毫秒内把 Gamma 重置回默认。这里以 1ms 间隔
    // 轮询，一旦发现被重置就立刻写回，闪烁只剩不到一帧。
    void GuardAppliedRamp()
    {
        if (!g.eyeCare)
        {
            return;
        }

        struct Target
        {
            HDC dc = nullptr;
            WORD ramp[3][256]{};
        };

        std::vector<Target> targets;
        {
            std::lock_guard<std::mutex> lock(g.rampMutex);
            for (const AppliedRamp& applied : g.appliedRamps)
            {
                HDC dc = CreateDCW(nullptr, applied.device.c_str(), nullptr, nullptr);
                if (!dc)
                {
                    continue;
                }
                Target target;
                target.dc = dc;
                std::memcpy(target.ramp, applied.ramp, sizeof(target.ramp));
                targets.push_back(target);
            }
        }

        const ULONGLONG deadline = GetTickCount64() + 300;
        ULONGLONG repairedAt = 0;
        while (GetTickCount64() < deadline)
        {
            if (!g.eyeCare)
            {
                break;
            }

            {
                std::lock_guard<std::mutex> lock(g.writerMutex);
                if (g.writerStop || g.writerHasWork || g.writerGuard)
                {
                    break;
                }
            }

            bool repaired = false;
            for (const Target& target : targets)
            {
                WORD current[3][256]{};
                if (!GetDeviceGammaRamp(target.dc, &current[0][0]) ||
                    std::memcmp(current, target.ramp, sizeof(current)) != 0)
                {
                    SetDeviceGammaRamp(
                        target.dc, const_cast<WORD*>(&target.ramp[0][0]));
                    repaired = true;
                }
            }

            const ULONGLONG now = GetTickCount64();
            if (repaired)
            {
                repairedAt = now;
            }
            else if (repairedAt != 0 && now - repairedAt > 60)
            {
                // 重置已抓到并写回，再多守 60ms 防二次重置即可。
                break;
            }
            // 高密度轮询，把“被重置”到“写回”的间隔压到 1ms 以内。
            SwitchToThread();
        }

        for (const Target& target : targets)
        {
            DeleteDC(target.dc);
        }
    }

    void CaptureGammaDevices()
    {
        g.gammaBases.clear();

        for (const std::wstring& name : g.displayService.GetActiveGdiNames())
        {
            GammaBase base;
            base.device = name;
            HDC dc = CreateDCW(nullptr, name.c_str(), nullptr, nullptr);
            if (dc)
            {
                base.valid = GetDeviceGammaRamp(dc, &base.ramp[0][0]) != FALSE;
                DeleteDC(dc);
            }
            g.gammaBases.push_back(std::move(base));
        }
    }

    void RefreshGammaDevices()
    {
        // 刷新前先把已改过的 Gamma 还原，避免把暖色当成本底叠加。
        RestoreGammaRamps();
        CaptureGammaDevices();
    }

    // 上次进程可能被强制结束、把暖色留在了屏幕上。启动时若护眼为开启状态且
    // 当前 Ramp 明显偏暖，按当前档位反解回原始 Ramp，避免重启后越叠越暖。
    void RepairLeftoverGamma()
    {
        constexpr const wchar_t* store = L"Software\\SdrBrightnessTray";
        if (ReadDword(store, L"EyeCare", 0) == 0)
        {
            return;
        }

        const int level = std::clamp<int>(
            static_cast<int>(ReadDword(store, L"EyeCareLevel", 50)),
            0,
            100);
        if (level <= 0)
        {
            return;
        }

        const double t = level / 100.0;
        const double gains[3] = {
            1.0 - 0.01 * t,
            1.0 - 0.15 * t,
            1.0 - 0.48 * t};

        for (const std::wstring& name : g.displayService.GetActiveGdiNames())
        {
            HDC dc = CreateDCW(nullptr, name.c_str(), nullptr, nullptr);
            if (!dc)
            {
                continue;
            }

            WORD ramp[3][256]{};
            if (GetDeviceGammaRamp(dc, &ramp[0][0]) &&
                ramp[0][255] > ramp[2][255] + 512)
            {
                for (int channel = 0; channel < 3; ++channel)
                {
                    for (int i = 0; i < 256; ++i)
                    {
                        const long value = std::lround(ramp[channel][i] / gains[channel]);
                        ramp[channel][i] = static_cast<WORD>(
                            std::clamp<long>(value, 0, 65535));
                    }
                }
                SetDeviceGammaRamp(dc, ramp);
            }
            DeleteDC(dc);
        }
    }

    // 打开护眼时先关掉系统夜间模式，等它撤掉自己的 Gamma 后再采样本底。
    void ActivateEyeCare()
    {
        if (DisableSystemNightLight())
        {
            Sleep(90);
            CaptureGammaDevices();
        }
        ApplyEyeCareRamp();
    }

    void SaveEyeCareSettings()
    {
        WriteDword(L"Software\\SdrBrightnessTray", L"EyeCare", g.eyeCare ? 1u : 0u);
        WriteDword(
            L"Software\\SdrBrightnessTray",
            L"EyeCareLevel",
            static_cast<DWORD>(std::clamp(g.eyeCareLevel, 0, 100)));
    }

    void LoadEyeCareSettings()
    {
        g.eyeCare = ReadDword(L"Software\\SdrBrightnessTray", L"EyeCare", 0) != 0;
        g.eyeCareLevel = std::clamp<int>(
            static_cast<int>(ReadDword(L"Software\\SdrBrightnessTray", L"EyeCareLevel", 50)),
            0,
            100);
        g.eyeSwitchVisual = g.eyeCare ? 1.0f : 0.0f;
        if (g.eyeCare)
        {
            ActivateEyeCare();
        }
        else
        {
            ApplyEyeCareRamp();
        }
    }

    void AnimateEyeSwitch()
    {
        const float target = g.eyeCare ? 1.0f : 0.0f;
        g.eyeSwitchAnimFrom = g.eyeSwitchVisual;
        g.eyeSwitchAnimTo = target;

        if (!SystemAnimationsEnabled() ||
            std::abs(g.eyeSwitchVisual - target) < 0.001f)
        {
            g.eyeSwitchVisual = target;
            g.eyeSwitchAnimating = false;
            KillTimer(g.window, kSwitchTimerId);
            InvalidateUi();
            return;
        }

        g.eyeSwitchAnimStart = GetTickCount64();
        g.eyeSwitchAnimating = true;
        SetTimer(g.window, kSwitchTimerId, 8, nullptr);
        InvalidateUi();
    }

    void ToggleEyeCare()
    {
        g.eyeCare = !g.eyeCare;
        if (g.eyeCare && g.eyeCareLevel < 10)
        {
            g.eyeCareLevel = 50;
        }
        SaveEyeCareSettings();
        if (g.eyeCare)
        {
            ActivateEyeCare();
        }
        else
        {
            ApplyEyeCareRamp();
        }
        UpdateTrayTooltip();
        AnimateEyeSwitch();
        InvalidateUi();
    }

    void SetEyeCareLevel(int percent)
    {
        percent = std::clamp(percent, 0, 100);
        if (percent == g.eyeCareLevel)
        {
            return;
        }

        g.eyeCareLevel = percent;
        ApplyEyeCareRamp();
        if (!g.dragging)
        {
            SaveEyeCareSettings();
        }
        UpdateTrayTooltip();
        InvalidateUi();
    }

    void SetWarmthFromMouse(int mouseX)
    {
        if (!g.eyeCare)
        {
            return;
        }

        const Layout layout = BuildLayout();
        const int width = layout.warmTrack.right - layout.warmTrack.left;
        if (width <= 0)
        {
            return;
        }

        const int x = std::clamp(
            mouseX,
            static_cast<int>(layout.warmTrack.left),
            static_cast<int>(layout.warmTrack.right));
        const int percent = std::clamp(
            static_cast<int>(std::lround(
                static_cast<float>(x - layout.warmTrack.left) * 100.0f /
                static_cast<float>(width))),
            0,
            100);
        SetEyeCareLevel(percent);
    }

    void BrightnessWriterLoop()
    {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
        for (;;)
        {
            SdrDisplay display;
            int percent = 0;
            BYTE finalValue = 1;
            bool guardOnly = false;
            {
                std::unique_lock<std::mutex> lock(g.writerMutex);
                g.writerCv.wait(lock, []
                {
                    return g.writerStop || g.writerHasWork || g.writerGuard;
                });

                if (g.writerStop && !g.writerHasWork && !g.writerGuard)
                {
                    return;
                }

                if (g.writerHasWork)
                {
                    display = g.writerDisplay;
                    percent = g.writerPercent;
                    finalValue = g.writerFinalValue;
                    g.writerHasWork = false;
                }
                else
                {
                    g.writerGuard = false;
                    guardOnly = true;
                }
            }

            if (guardOnly)
            {
                GuardAppliedRamp();
                continue;
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

            // 写 SDR 白点会顺带把显示器 Gamma 重置成系统默认，护眼暖色因此丢失。
            // 立即进入守护循环，重置一发生就写回，避免可感知的闪烁。
            if (applied && g.eyeCare)
            {
                GuardAppliedRamp();
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
        if (g.eyeCare)
        {
            {
                std::lock_guard<std::mutex> lock(g.writerMutex);
                g.writerGuard = true;
            }
            g.writerCv.notify_one();
        }
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

    // ---------------------------------------------------------------------
    // 自绘弹出菜单：与主面板共用配色、字体和亚克力合成管线
    // ---------------------------------------------------------------------

    int MenuDpi(int dip)
    {
        return MulDiv(dip, menu.dpi > 0 ? menu.dpi : 96, 96);
    }

    void SaveTransparency()
    {
        WriteDword(
            L"Software\\SdrBrightnessTray",
            L"Transparency",
            static_cast<DWORD>(std::clamp(g.transparency, 0, 100)));
    }

    void SetTransparency(int percent, bool persist)
    {
        percent = std::clamp(percent, 0, 100);
        if (percent == g.transparency)
        {
            if (persist)
            {
                SaveTransparency();
            }
            return;
        }

        g.transparency = percent;
        if (persist)
        {
            SaveTransparency();
        }

        // 面板与菜单共用同一透明度：重应用亚克力并重绘。
        if (g.window)
        {
            ApplyAcrylicEffect(true);
        }
        if (menu.window)
        {
            ApplyAcrylicEffect(true, menu.window);
        }

        g.sceneDirty = true;
        if (g.isOpen)
        {
            InvalidateRect(g.window, nullptr, FALSE);
        }

        if (menu.shown)
        {
            if (menu.sliderIndex >= 0 &&
                menu.sliderIndex < static_cast<int>(menu.items.size()))
            {
                menu.items[menu.sliderIndex].sliderValue = percent;
            }
            menu.sceneDirty = true;
            InvalidateRect(menu.window, nullptr, FALSE);
        }
    }

    int MenuSliderValueFromX(int x)
    {
        const int width = menu.sliderTrack.right - menu.sliderTrack.left;
        if (width <= 0)
        {
            return g.transparency;
        }
        const int clamped = std::clamp(
            x,
            static_cast<int>(menu.sliderTrack.left),
            static_cast<int>(menu.sliderTrack.right));
        return std::clamp((clamped - static_cast<int>(menu.sliderTrack.left)) * 100 / width, 0, 100);
    }

    void DestroyMenuLayers()
    {
        if (menu.paintDc)
        {
            if (menu.oldPaintBitmap)
            {
                SelectObject(menu.paintDc, menu.oldPaintBitmap);
                menu.oldPaintBitmap = nullptr;
            }
            DeleteDC(menu.paintDc);
            menu.paintDc = nullptr;
        }
        if (menu.paintBitmap)
        {
            DeleteObject(menu.paintBitmap);
            menu.paintBitmap = nullptr;
        }
        menu.paintBits = nullptr;

        if (menu.maskDc)
        {
            if (menu.oldMaskBitmap)
            {
                SelectObject(menu.maskDc, menu.oldMaskBitmap);
                menu.oldMaskBitmap = nullptr;
            }
            DeleteDC(menu.maskDc);
            menu.maskDc = nullptr;
        }
        if (menu.maskBitmap)
        {
            DeleteObject(menu.maskBitmap);
            menu.maskBitmap = nullptr;
        }
        menu.maskBits = nullptr;
    }

    void DestroyMenuBuffers()
    {
        if (menu.bufferDc)
        {
            if (menu.oldBufferBitmap)
            {
                SelectObject(menu.bufferDc, menu.oldBufferBitmap);
                menu.oldBufferBitmap = nullptr;
            }
            DeleteDC(menu.bufferDc);
            menu.bufferDc = nullptr;
        }
        if (menu.bufferBitmap)
        {
            DeleteObject(menu.bufferBitmap);
            menu.bufferBitmap = nullptr;
        }
        menu.bufferBits = nullptr;
        menu.bufferWidth = 0;
        menu.bufferHeight = 0;
        DestroyMenuLayers();
    }

    void DestroyMenuFonts()
    {
        ReleaseDWriteFormats();

        HFONT* fonts[] = {&menu.fonts.body, &menu.fonts.caption, &menu.fonts.icon};
        for (HFONT* font : fonts)
        {
            if (*font)
            {
                DeleteObject(*font);
                *font = nullptr;
            }
        }

        Gdiplus::Font** gdiFonts[] = {
            &menu.fonts.gdipBody,
            &menu.fonts.gdipCaption,
            &menu.fonts.gdipIcon};
        for (Gdiplus::Font** font : gdiFonts)
        {
            delete *font;
            *font = nullptr;
        }

        menu.fonts.dpi = 0;
    }

    void EnsureMenuFonts(int dpi)
    {
        if (menu.fonts.dpi == dpi &&
            menu.fonts.body && menu.fonts.caption && menu.fonts.icon)
        {
            return;
        }

        DestroyMenuFonts();
        menu.fonts.dpi = dpi;

        const wchar_t* uiFace = PickFace(
            {L"Microsoft YaHei UI", L"Microsoft YaHei"}, L"Microsoft YaHei UI");
        const wchar_t* iconFace = PickFace(
            {L"Segoe Fluent Icons", L"Segoe MDL2 Assets"}, L"Segoe MDL2 Assets");

        menu.fonts.body = CreateUiFont(14, FW_NORMAL, uiFace, dpi);
        menu.fonts.caption = CreateUiFont(12, FW_NORMAL, uiFace, dpi);
        menu.fonts.icon = CreateUiFont(14, FW_NORMAL, iconFace, dpi);
        menu.fonts.gdipBody = CreateGdiplusFont(menu.fonts.body);
        menu.fonts.gdipCaption = CreateGdiplusFont(menu.fonts.caption);
        menu.fonts.gdipIcon = CreateGdiplusFont(menu.fonts.icon);
    }

    void EnsureMenuBuffers(HDC referenceDc)
    {
        const int width = menu.width;
        const int height = menu.height;
        if (width <= 0 || height <= 0)
        {
            return;
        }
        if (menu.bufferDc && menu.bufferBitmap && menu.bufferBits &&
            menu.bufferWidth == width && menu.bufferHeight == height)
        {
            return;
        }

        DestroyMenuBuffers();

        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = width;
        info.bmiHeader.biHeight = -height;
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;

        menu.bufferDc = CreateCompatibleDC(referenceDc);
        menu.bufferBitmap = CreateDIBSection(
            referenceDc, &info, DIB_RGB_COLORS, &menu.bufferBits, nullptr, 0);
        if (!menu.bufferDc || !menu.bufferBitmap || !menu.bufferBits)
        {
            DestroyMenuBuffers();
            return;
        }
        menu.oldBufferBitmap = SelectObject(menu.bufferDc, menu.bufferBitmap);
        menu.bufferWidth = width;
        menu.bufferHeight = height;
        menu.sceneDirty = true;

        if (!menu.liveAcrylic)
        {
            return;
        }

        BITMAPINFO layer{};
        layer.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        layer.bmiHeader.biWidth = width;
        layer.bmiHeader.biHeight = -height;
        layer.bmiHeader.biPlanes = 1;
        layer.bmiHeader.biBitCount = 24;
        layer.bmiHeader.biCompression = BI_RGB;

        menu.paintDc = CreateCompatibleDC(referenceDc);
        menu.paintBitmap = CreateDIBSection(
            referenceDc, &layer, DIB_RGB_COLORS, &menu.paintBits, nullptr, 0);
        menu.maskDc = CreateCompatibleDC(referenceDc);
        menu.maskBitmap = CreateDIBSection(
            referenceDc, &layer, DIB_RGB_COLORS, &menu.maskBits, nullptr, 0);
        if (!menu.paintDc || !menu.paintBitmap || !menu.paintBits ||
            !menu.maskDc || !menu.maskBitmap || !menu.maskBits)
        {
            menu.liveAcrylic = false;
            DestroyMenuLayers();
            return;
        }

        menu.oldPaintBitmap = SelectObject(menu.paintDc, menu.paintBitmap);
        menu.oldMaskBitmap = SelectObject(menu.maskDc, menu.maskBitmap);
    }

    void MeasureMenu(HDC dc)
    {
        const int padX = MenuDpi(kMenuPadXDip);
        const int iconColumn = MenuDpi(kMenuIconColumnDip);
        const int itemHeight = MenuDpi(kMenuItemHeightDip);
        const int infoHeight = MenuDpi(kMenuInfoHeightDip);
        const int separatorHeight = MenuDpi(kMenuSeparatorHeightDip);
        const int rightPad = MenuDpi(18);

        int contentWidth = 0;
        int totalHeight = MenuDpi(kMenuPadYDip) * 2;
        menu.rows.clear();
        menu.rows.reserve(menu.items.size());

        for (const MenuItem& item : menu.items)
        {
            int height = itemHeight;
            if (item.kind == MenuItem::Kind::Separator)
            {
                height = separatorHeight;
            }
            else if (item.kind == MenuItem::Kind::Info)
            {
                height = infoHeight;
                contentWidth = std::max(
                    contentWidth,
                    TextWidth(dc, menu.fonts.caption, item.text.c_str()) + iconColumn);
            }
            else if (item.kind == MenuItem::Kind::Slider)
            {
                height = MenuDpi(48);
                contentWidth = std::max(
                    contentWidth,
                    TextWidth(dc, menu.fonts.body, item.text.c_str()) + iconColumn + MenuDpi(96));
            }
            else
            {
                contentWidth = std::max(
                    contentWidth,
                    TextWidth(dc, menu.fonts.body, item.text.c_str()) + iconColumn);
            }

            menu.rows.push_back(RECT{0, totalHeight, 0, totalHeight + height});
            totalHeight += height;
        }

        const int width = std::max(padX * 2 + contentWidth + rightPad, MenuDpi(216));
        menu.width = width;
        menu.height = totalHeight;
        menu.padX = padX;
        menu.iconX = padX + MenuDpi(2);
        menu.textX = padX + iconColumn;
        for (RECT& row : menu.rows)
        {
            row.right = width;
        }

        menu.sliderIndex = -1;
        for (size_t i = 0; i < menu.items.size(); ++i)
        {
            if (menu.items[i].kind != MenuItem::Kind::Slider)
            {
                continue;
            }

            const RECT row = menu.rows[i];
            menu.sliderIndex = static_cast<int>(i);
            menu.sliderTrack = RECT{
                menu.textX,
                row.top + MenuDpi(31),
                width - menu.padX - MenuDpi(4),
                row.top + MenuDpi(34)};
            menu.sliderHit = RECT{
                menu.textX - MenuDpi(6),
                row.top + MenuDpi(22),
                width - menu.padX,
                row.top + MenuDpi(44)};
            break;
        }
    }

    void DrawMenu(HDC dc, bool maskMode)
    {
        g.textMaskMode = maskMode;
        g.textCmds.clear();
        const int width = menu.width;
        const int height = menu.height;
        if (width <= 0 || height <= 0)
        {
            return;
        }

        const HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(DC_BRUSH));
        const HGDIOBJ oldPen = SelectObject(dc, GetStockObject(DC_PEN));
        SetBkMode(dc, TRANSPARENT);

        // 没有实时亚克力时退回不透明surface渐变，避免出现系统原生菜单的灰底。
        if (!maskMode && !menu.liveAcrylic)
        {
            constexpr int gradientBands = 28;
            for (int i = 0; i < gradientBands; ++i)
            {
                RECT band{0, height * i / gradientBands, width, height * (i + 1) / gradientBands};
                FillSolid(
                    dc,
                    band,
                    BlendColor(g.surfaceTop, g.surfaceBottom, i * 100 / (gradientBands - 1)));
            }
        }

        RECT highlight{0, 1, width, 2};
        FillSolid(dc, highlight, UiColor(g.surfaceHighlight, maskMode));
        RoundStroke(dc, RECT{0, 0, width, height}, MenuDpi(8), UiColor(g.border, maskMode));

        for (size_t i = 0; i < menu.items.size(); ++i)
        {
            const MenuItem& item = menu.items[i];
            const RECT row = menu.rows[i];

            if (item.kind == MenuItem::Kind::Separator)
            {
                const int y = (row.top + row.bottom) / 2;
                RECT line{menu.padX + MenuDpi(10), y, width - menu.padX - MenuDpi(10), y + 1};
                FillSolid(dc, line, UiColor(g.border, maskMode));
                continue;
            }

            if (item.kind == MenuItem::Kind::Info)
            {
                RECT textRect{
                    menu.textX,
                    row.top,
                    width - menu.padX - MenuDpi(4),
                    row.bottom};
                DrawTextCore(
                    dc,
                    textRect,
                    menu.fonts.caption,
                    menu.fonts.gdipCaption,
                    UiColor(g.textTertiary, maskMode),
                    item.text.c_str(),
                    DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
                continue;
            }

            if (item.kind == MenuItem::Kind::Slider)
            {
                RECT labelRect{
                    menu.textX,
                    row.top + MenuDpi(2),
                    width - menu.padX - MenuDpi(4),
                    row.top + MenuDpi(22)};
                DrawTextCore(
                    dc,
                    labelRect,
                    menu.fonts.body,
                    menu.fonts.gdipBody,
                    UiColor(g.textPrimary, maskMode),
                    item.text.c_str(),
                    DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

                wchar_t valueText[16]{};
                swprintf_s(valueText, L"%d%%", std::clamp(item.sliderValue, 0, 100));
                RECT valueRect = labelRect;
                DrawTextCore(
                    dc,
                    valueRect,
                    menu.fonts.body,
                    menu.fonts.gdipBody,
                    UiColor(g.textSecondary, maskMode),
                    valueText,
                    DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

                RoundFill(
                    dc,
                    menu.sliderTrack,
                    MenuDpi(2),
                    UiColor(g.track, maskMode),
                    UiColor(g.track, maskMode));

                const int trackWidth =
                    menu.sliderTrack.right - menu.sliderTrack.left;
                const int value = std::clamp(item.sliderValue, 0, 100);
                const int thumbX =
                    menu.sliderTrack.left + trackWidth * value / 100;
                const int centerY =
                    (menu.sliderTrack.top + menu.sliderTrack.bottom) / 2;

                if (value > 0)
                {
                    const int saved = SaveDC(dc);
                    IntersectClipRect(
                        dc,
                        menu.sliderTrack.left - MenuDpi(4),
                        menu.sliderTrack.top - MenuDpi(4),
                        thumbX,
                        menu.sliderTrack.bottom + MenuDpi(4));
                    RoundFill(
                        dc,
                        menu.sliderTrack,
                        MenuDpi(2),
                        UiColor(g.accent, maskMode),
                        UiColor(g.accent, maskMode));
                    RestoreDC(dc, saved);
                }

                const bool active = menu.draggingSlider ||
                                    menu.hover == static_cast<int>(i);
                const int thumbRadius = active ? MenuDpi(6) : MenuDpi(5);
                CircleFill(dc, thumbX, centerY, thumbRadius, UiColor(g.accent, maskMode));
                CircleFill(dc, thumbX, centerY, MenuDpi(2), UiColor(g.surface, maskMode));
                continue;
            }

            const bool hot = item.enabled &&
                             (menu.hover == static_cast<int>(i) ||
                              menu.pressed == static_cast<int>(i));
            if (hot)
            {
                RECT hoverRect = row;
                hoverRect.left += menu.padX;
                hoverRect.right -= menu.padX;
                hoverRect.top += MenuDpi(1);
                hoverRect.bottom -= MenuDpi(1);
                const COLORREF fill = menu.pressed == static_cast<int>(i)
                    ? BlendColor(g.hoverBackground, g.accent, 12)
                    : g.hoverBackground;
                RoundFill(
                    dc,
                    hoverRect,
                    MenuDpi(4),
                    UiColor(fill, maskMode),
                    UiColor(fill, maskMode));
            }

            RECT iconRect{
                menu.iconX,
                row.top,
                menu.iconX + MenuDpi(24),
                row.bottom};
            if (item.checked)
            {
                DrawTextCore(
                    dc,
                    iconRect,
                    menu.fonts.icon,
                    menu.fonts.gdipIcon,
                    UiColor(g.accent, maskMode),
                    L"\xE73E",
                    DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
            }
            else if (item.glyph)
            {
                DrawTextCore(
                    dc,
                    iconRect,
                    menu.fonts.icon,
                    menu.fonts.gdipIcon,
                    UiColor(item.enabled ? g.textSecondary : g.textTertiary, maskMode),
                    item.glyph,
                    DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
            }

            RECT textRect{
                menu.textX,
                row.top,
                width - menu.padX - MenuDpi(6),
                row.bottom};
            DrawTextCore(
                dc,
                textRect,
                menu.fonts.body,
                menu.fonts.gdipBody,
                UiColor(item.enabled ? g.textPrimary : g.textTertiary, maskMode),
                item.text.c_str(),
                DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
        }

        SelectObject(dc, oldBrush);
        SelectObject(dc, oldPen);
        FlushMenuText(dc, width, height, maskMode);
    }

    void MakeMenuOpaque()
    {
        if (!menu.bufferBits || menu.bufferWidth <= 0 || menu.bufferHeight <= 0)
        {
            return;
        }
        auto* pixels = static_cast<BYTE*>(menu.bufferBits);
        const int count = menu.bufferWidth * menu.bufferHeight;
        for (int i = 0; i < count; ++i)
        {
            pixels[i * 4 + 3] = 255;
        }
    }

    int MenuHitTest(POINT point)
    {
        for (size_t i = 0; i < menu.rows.size(); ++i)
        {
            if (PtInRect(&menu.rows[i], point))
            {
                return static_cast<int>(i);
            }
        }
        return -1;
    }

    int MenuNextAction(int from, int direction)
    {
        const int count = static_cast<int>(menu.items.size());
        if (count == 0 || direction == 0)
        {
            return -1;
        }

        int index = from;
        for (int step = 0; step < count; ++step)
        {
            index += direction;
            if (index < 0)
            {
                index = count - 1;
            }
            else if (index >= count)
            {
                index = 0;
            }

            const MenuItem& item = menu.items[index];
            if ((item.kind == MenuItem::Kind::Action ||
                 item.kind == MenuItem::Kind::Slider) &&
                item.enabled)
            {
                return index;
            }
        }
        return -1;
    }

    void CloseCustomMenu(int result)
    {
        if (!menu.shown)
        {
            return;
        }
        if (menu.draggingSlider)
        {
            ReleaseCapture();
            menu.draggingSlider = false;
            SaveTransparency();
        }
        menu.shown = false;
        menu.result = result;
        KillTimer(menu.window, kMenuSlideTimerId);
        ShowWindow(menu.window, SW_HIDE);
    }

    void HandleMenuKey(WPARAM key)
    {
        if ((key == VK_LEFT || key == VK_RIGHT) &&
            menu.sliderIndex >= 0 && menu.hover == menu.sliderIndex)
        {
            const int step = (GetKeyState(VK_SHIFT) & 0x8000) ? 10 : 1;
            SetTransparency(g.transparency + (key == VK_RIGHT ? step : -step), true);
            return;
        }

        if (key == VK_ESCAPE || key == VK_LEFT)
        {
            CloseCustomMenu(0);
            return;
        }

        if (key == VK_UP || key == VK_DOWN)
        {
            const int next = MenuNextAction(menu.hover, key == VK_DOWN ? 1 : -1);
            if (next >= 0)
            {
                menu.hover = next;
                menu.sceneDirty = true;
                InvalidateRect(menu.window, nullptr, FALSE);
            }
            return;
        }

        if (key == VK_HOME || key == VK_END)
        {
            const int next = MenuNextAction(
                key == VK_HOME ? static_cast<int>(menu.items.size()) : -1,
                key == VK_HOME ? 1 : -1);
            if (next >= 0)
            {
                menu.hover = next;
                menu.sceneDirty = true;
                InvalidateRect(menu.window, nullptr, FALSE);
            }
            return;
        }

        if ((key == VK_RETURN || key == VK_SPACE) &&
            menu.hover >= 0 &&
            menu.hover < static_cast<int>(menu.items.size()))
        {
            const MenuItem& item = menu.items[menu.hover];
            if (item.kind == MenuItem::Kind::Action && item.enabled)
            {
                CloseCustomMenu(item.id);
            }
        }
    }

    LRESULT CALLBACK MenuWindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
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
            if (menu.shown)
            {
                EnsureMenuBuffers(dc);
                if (menu.bufferDc)
                {
                    if (menu.sceneDirty)
                    {
                        const bool live = menu.liveAcrylic &&
                                          menu.paintDc && menu.maskDc &&
                                          menu.paintBits && menu.maskBits;
                        if (live)
                        {
                            const size_t bytes =
                                static_cast<size_t>((menu.bufferWidth * 3 + 3) & ~3) * menu.bufferHeight;
                            std::memset(menu.paintBits, 0, bytes);
                            std::memset(menu.maskBits, 0, bytes);
                            DrawMenu(menu.paintDc, false);
                            DrawMenu(menu.maskDc, true);
                            CompositeLiveSurface(
                                menu.paintBits,
                                menu.maskBits,
                                menu.bufferBits,
                                menu.bufferWidth,
                                menu.bufferHeight);
                        }
                        else
                        {
                            if (menu.liveAcrylic)
                            {
                                menu.liveAcrylic = false;
                                ApplyAcrylicEffect(false, window);
                            }
                            DrawMenu(menu.bufferDc, false);
                            MakeMenuOpaque();
                        }
                        menu.sceneDirty = false;
                    }
                    BitBlt(
                        dc,
                        0,
                        0,
                        menu.bufferWidth,
                        menu.bufferHeight,
                        menu.bufferDc,
                        0,
                        0,
                        SRCCOPY);
                }
            }
            EndPaint(window, &paint);
            return 0;
        }

        case WM_MOUSEMOVE:
        {
            const POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            if (menu.draggingSlider && menu.sliderIndex >= 0)
            {
                SetTransparency(MenuSliderValueFromX(point.x), false);
                menu.hover = menu.sliderIndex;
                return 0;
            }

            const int index = MenuHitTest(point);
            const bool interactive = index >= 0 &&
                                     (menu.items[index].kind == MenuItem::Kind::Action ||
                                      menu.items[index].kind == MenuItem::Kind::Slider) &&
                                     menu.items[index].enabled;
            const int hover = interactive ? index : -1;
            if (hover != menu.hover)
            {
                menu.hover = hover;
                menu.sceneDirty = true;
                InvalidateRect(window, nullptr, FALSE);
            }
            if (!menu.trackingMouse)
            {
                TRACKMOUSEEVENT track{};
                track.cbSize = sizeof(track);
                track.dwFlags = TME_LEAVE;
                track.hwndTrack = window;
                TrackMouseEvent(&track);
                menu.trackingMouse = true;
            }
            return 0;
        }

        case WM_MOUSELEAVE:
            menu.trackingMouse = false;
            if (!menu.draggingSlider && menu.hover >= 0)
            {
                menu.hover = -1;
                menu.sceneDirty = true;
                InvalidateRect(window, nullptr, FALSE);
            }
            return 0;

        case WM_LBUTTONDOWN:
        {
            const POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            if (menu.sliderIndex >= 0 && PtInRect(&menu.sliderHit, point))
            {
                menu.draggingSlider = true;
                menu.hover = menu.sliderIndex;
                SetCapture(window);
                SetTransparency(MenuSliderValueFromX(point.x), false);
                menu.sceneDirty = true;
                InvalidateRect(window, nullptr, FALSE);
                return 0;
            }

            const int index = MenuHitTest(point);
            menu.pressed = -1;
            if (index >= 0 &&
                menu.items[index].kind == MenuItem::Kind::Action &&
                menu.items[index].enabled)
            {
                menu.pressed = index;
                menu.sceneDirty = true;
                InvalidateRect(window, nullptr, FALSE);
            }
            return 0;
        }

        case WM_LBUTTONUP:
        {
            if (menu.draggingSlider)
            {
                ReleaseCapture();
                menu.draggingSlider = false;
                SaveTransparency();
                menu.sceneDirty = true;
                InvalidateRect(window, nullptr, FALSE);
                return 0;
            }

            const int index = MenuHitTest(POINT{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)});
            const int pressed = menu.pressed;
            menu.pressed = -1;
            menu.sceneDirty = true;
            if (index >= 0 && index == pressed &&
                menu.items[index].kind == MenuItem::Kind::Action &&
                menu.items[index].enabled)
            {
                CloseCustomMenu(menu.items[index].id);
                return 0;
            }
            InvalidateRect(window, nullptr, FALSE);
            return 0;
        }

        case WM_RBUTTONUP:
            CloseCustomMenu(0);
            return 0;

        case WM_ACTIVATE:
            if (LOWORD(wParam) == WA_INACTIVE)
            {
                // ShowWindow 首次上屏也会收到一次 WA_INACTIVE，只有真正激活过之后
                // 的失活才代表用户点到别处了。
                if (menu.activated)
                {
                    CloseCustomMenu(0);
                }
            }
            else
            {
                menu.activated = true;
            }
            return 0;

        case WM_KEYDOWN:
            HandleMenuKey(wParam);
            return 0;

        case WM_SETCURSOR:
        {
            POINT point{};
            GetCursorPos(&point);
            ScreenToClient(window, &point);
            const int index = MenuHitTest(point);
            const bool actionable = index >= 0 &&
                                    (menu.items[index].kind == MenuItem::Kind::Action ||
                                     menu.items[index].kind == MenuItem::Kind::Slider) &&
                                    menu.items[index].enabled;
            SetCursor(LoadCursorW(nullptr, actionable ? IDC_HAND : IDC_ARROW));
            return TRUE;
        }

        case WM_TIMER:
            if (wParam == kMenuSlideTimerId)
            {
                const ULONGLONG elapsed = GetTickCount64() - menu.slideStart;
                const double t = std::min(1.0, static_cast<double>(elapsed) / kMenuSlideMs);
                const double eased = 1.0 - std::pow(1.0 - t, 3.0);
                const int offset =
                    static_cast<int>(std::lround(menu.slideFrom * (1.0 - eased)));
                SetWindowPos(
                    window,
                    nullptr,
                    menu.target.left,
                    menu.target.top + offset,
                    menu.target.right - menu.target.left,
                    menu.target.bottom - menu.target.top,
                    SWP_NOZORDER | SWP_NOACTIVATE);
                if (t >= 1.0)
                {
                    KillTimer(window, kMenuSlideTimerId);
                }
                return 0;
            }
            break;

        case WM_CLOSE:
            CloseCustomMenu(0);
            return 0;

        default:
            break;
        }

        return DefWindowProcW(window, message, wParam, lParam);
    }

    bool EnsureMenuWindow()
    {
        if (menu.window)
        {
            return true;
        }

        WNDCLASSEXW windowClass{};
        windowClass.cbSize = sizeof(windowClass);
        windowClass.style = CS_HREDRAW | CS_VREDRAW;
        windowClass.lpfnWndProc = MenuWindowProc;
        windowClass.hInstance = g.instance;
        windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        windowClass.lpszClassName = kMenuClass;
        windowClass.hbrBackground = nullptr;
        if (!RegisterClassExW(&windowClass) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        {
            return false;
        }

        menu.window = CreateWindowExW(
            WS_EX_TOOLWINDOW,
            kMenuClass,
            L"SDR 亮度菜单",
            WS_POPUP | WS_THICKFRAME,
            0,
            0,
            1,
            1,
            nullptr,
            nullptr,
            g.instance,
            nullptr);
        return menu.window != nullptr;
    }

    void ApplyMenuWindowAttributes()
    {
        int corner = 2;
        DwmSetWindowAttribute(menu.window, 33, &corner, sizeof(corner));

        BOOL dark = g.lightTheme ? FALSE : TRUE;
        DwmSetWindowAttribute(menu.window, 20, &dark, sizeof(dark));
    }

    RECT GetTrayIconRect()
    {
        NOTIFYICONIDENTIFIER identifier{};
        identifier.cbSize = sizeof(identifier);
        identifier.hWnd = g.window;
        identifier.uID = kTrayIconId;
        RECT rect{};
        if (SUCCEEDED(Shell_NotifyIconGetRect(&identifier, &rect)))
        {
            return rect;
        }

        POINT cursor{};
        GetCursorPos(&cursor);
        const int half = Px(10);
        return RECT{cursor.x - half, cursor.y - half, cursor.x + half, cursor.y + half};
    }

    RECT ComputeMenuRect(const RECT& anchor, int width, int height, bool preferAbove)
    {
        MONITORINFO info{};
        info.cbSize = sizeof(info);
        HMONITOR monitor = MonitorFromRect(&anchor, MONITOR_DEFAULTTONEAREST);
        if (!GetMonitorInfoW(monitor, &info))
        {
            return RECT{anchor.left, anchor.top, anchor.left + width, anchor.top + height};
        }

        const int margin = MenuDpi(8);
        const int gap = MenuDpi(6);
        const int leftLimit = info.rcWork.left + margin;
        const int rightLimit = info.rcWork.right - margin;
        const int topLimit = info.rcWork.top + margin;
        const int bottomLimit = info.rcWork.bottom - margin;

        int right = std::min<int>(anchor.right, rightLimit);
        int left = right - width;
        if (left < leftLimit)
        {
            left = leftLimit;
            right = left + width;
        }

        int top = 0;
        if (preferAbove)
        {
            top = anchor.top - gap - height;
            if (top < topLimit)
            {
                top = anchor.bottom + gap;
            }
        }
        else
        {
            top = anchor.bottom + gap;
            if (top + height > bottomLimit)
            {
                top = anchor.top - gap - height;
            }
        }

        top = std::clamp(top, topLimit, std::max(topLimit, bottomLimit - height));
        return RECT{left, top, right, top + height};
    }

    // 以锚点矩形弹出菜单，返回被选中的命令 ID；0 表示取消。
    int ShowCustomMenu(std::vector<MenuItem> items, const RECT& anchor, bool preferAbove)
    {
        if (items.empty() || g.exitRequested)
        {
            return 0;
        }
        if (menu.shown)
        {
            CloseCustomMenu(0);
            return 0;
        }
        if (!EnsureMenuWindow())
        {
            return 0;
        }

        HMONITOR monitor = MonitorFromRect(&anchor, MONITOR_DEFAULTTONEAREST);
        UINT dpiX = 96;
        UINT dpiY = 96;
        if (FAILED(GetDpiForMonitor(monitor, MDT_EFFECTIVE_DPI, &dpiX, &dpiY)))
        {
            dpiX = 96;
        }
        menu.dpi = static_cast<int>(dpiX);
        EnsureMenuFonts(menu.dpi);

        menu.items = std::move(items);
        HDC measureDc = GetDC(nullptr);
        MeasureMenu(measureDc);
        ReleaseDC(nullptr, measureDc);

        menu.target = ComputeMenuRect(anchor, menu.width, menu.height, preferAbove);
        menu.hover = -1;
        menu.pressed = -1;
        menu.draggingSlider = false;
        menu.result = 0;
        menu.sceneDirty = true;
        menu.trackingMouse = false;
        menu.liveAcrylic = false;
        DestroyMenuBuffers();

        const int slide = MenuDpi(6);
        menu.slideFrom = preferAbove ? slide : -slide;
        if (!SystemAnimationsEnabled())
        {
            menu.slideFrom = 0;
        }
        menu.slideStart = GetTickCount64();

        SetWindowPos(
            menu.window,
            HWND_TOPMOST,
            menu.target.left,
            menu.target.top + menu.slideFrom,
            menu.width,
            menu.height,
            SWP_NOACTIVATE);
        ApplyMenuWindowAttributes();
        menu.liveAcrylic = ApplyAcrylicEffect(true, menu.window);

        menu.shown = true;
        menu.activated = false;
        g.menuOpen = true;

        ShowWindow(menu.window, SW_SHOW);
        SetForegroundWindow(menu.window);
        SetFocus(menu.window);
        InvalidateRect(menu.window, nullptr, FALSE);
        if (menu.slideFrom != 0)
        {
            SetTimer(menu.window, kMenuSlideTimerId, 8, nullptr);
        }

        for (;;)
        {
            MSG message{};
            const BOOL result = GetMessageW(&message, nullptr, 0, 0);
            if (result <= 0)
            {
                if (result == 0)
                {
                    PostQuitMessage(static_cast<int>(message.wParam));
                }
                break;
            }
            TranslateMessage(&message);
            DispatchMessageW(&message);
            if (!menu.shown)
            {
                break;
            }
        }

        g.menuOpen = false;
        KillTimer(menu.window, kMenuSlideTimerId);
        ShowWindow(menu.window, SW_HIDE);
        DestroyMenuBuffers();
        DestroyMenuFonts();
        menu.items.clear();
        menu.rows.clear();
        menu.hover = -1;
        menu.pressed = -1;
        menu.lastClosed = GetTickCount64();

        // 选中命令后把前台还给面板，面板才不会因为失焦而收起。
        if (menu.result != 0 && g.isOpen)
        {
            SetForegroundWindow(g.window);
            SetFocus(g.window);
        }

        return menu.result;
    }

    RECT PanelMenuAnchor()
    {
        RECT client{};
        GetClientRect(g.window, &client);
        POINT topRight{client.right, 0};
        ClientToScreen(g.window, &topRight);
        return RECT{topRight.x - 1, topRight.y, topRight.x, topRight.y + 1};
    }

    void ShowDisplayMenu()
    {
        if (g.displays.size() < 2)
        {
            return;
        }

        std::vector<MenuItem> items;
        items.reserve(g.displays.size());
        for (size_t i = 0; i < g.displays.size(); ++i)
        {
            items.push_back(MenuItem{
                MenuItem::Kind::Action,
                kMenuDisplayBase + static_cast<int>(i),
                DisplayLabel(g.displays[i]),
                L"\xE7F4",
                true,
                static_cast<int>(i) == g.currentDisplay});
        }

        const int command = ShowCustomMenu(std::move(items), PanelMenuAnchor(), true);
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
        std::vector<MenuItem> items;
        items.push_back(MenuItem{
            MenuItem::Kind::Action, kMenuOpen, L"打开面板", L"\xE8A7", true, false});
        items.push_back(MenuItem{MenuItem::Kind::Separator});

        items.push_back(MenuItem{
            MenuItem::Kind::Info,
            0,
            g.liveAcrylic ? L"材质：DWM 实时亚克力" : L"材质：静态快照"});
        if (g.hotkeyToggle)
        {
            items.push_back(MenuItem{
                MenuItem::Kind::Info, 0, L"Ctrl+Alt+B   呼出 / 收起面板"});
        }
        if (g.hotkeyUp && g.hotkeyDown)
        {
            items.push_back(MenuItem{
                MenuItem::Kind::Info, 0, L"Ctrl+Alt+↑↓   亮度 ±5%"});
        }

        items.push_back(MenuItem{MenuItem::Kind::Separator});
        items.push_back(MenuItem{
            MenuItem::Kind::Slider,
            0,
            L"透明度",
            nullptr,
            true,
            false,
            g.transparency});
        items.push_back(MenuItem{MenuItem::Kind::Separator});
        items.push_back(MenuItem{
            MenuItem::Kind::Action,
            kMenuSync,
            L"同步所有显示器",
            L"\xE895",
            g.hasHdr && g.displays.size() > 1,
            false});
        items.push_back(MenuItem{
            MenuItem::Kind::Action,
            kMenuStartup,
            L"开机时启动",
            L"\xE7E8",
            true,
            StartupEnabled()});
        items.push_back(MenuItem{MenuItem::Kind::Separator});
        items.push_back(MenuItem{
            MenuItem::Kind::Action,
            kMenuDisplaySettings,
            L"打开显示设置",
            L"\xE713",
            true,
            false});
        items.push_back(MenuItem{MenuItem::Kind::Separator});
        items.push_back(MenuItem{
            MenuItem::Kind::Action, kMenuExit, L"退出", L"\xE8BB", true, false});

        // 托盘右键：贴着任务栏图标上方弹出，和系统自家托盘菜单的位置一致。
        const RECT anchor = fromTray ? GetTrayIconRect() : PanelMenuAnchor();
        const int command = ShowCustomMenu(std::move(items), anchor, true);
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
        case PartEye:
            ToggleEyeCare();
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

    void ApplyFlyoutFrame(const RECT& target, int offset)
    {
        const int width = target.right - target.left;
        const int height = target.bottom - target.top;

        // 只做纵向位移，从底部向上滑出。
        const int x = target.left;
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
        // 一帧 WM_APP+4 投进来，它会被一直压到最后，表现就是「动画全程空白、
        // 结束时内容才突然出现」。这里强制把这一帧画完再走下一帧。
        if (IsWindowVisible(g.window))
        {
            UpdateWindow(g.window);
        }
    }

    void MoveFlyoutTo(const RECT& target, int offset)
    {
        ApplyFlyoutFrame(target, offset);
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

                g.animLatestOffset.store(offset);
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

    void StartFlyoutAnimation(bool opening, const RECT& target, int fromOffset)
    {
        StartAnimThread();

        const int slide = Px(kOpenSlideDip);
        const int from = std::clamp(fromOffset, 0, slide);
        const int to = opening ? 0 : Px(kCloseSlideDip);
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
            g.animJob.duration = duration;
            g.animJob.opening = opening;
            g.animJob.generation = generation;
            g.animPending = true;
        }
        g.animCv.notify_one();

        ApplyFlyoutFrame(target, from);
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
        if (wasVisible)
        {
            // 收起动画中途被打断：从当前位置续上，别跳回起点。
            from = std::clamp(static_cast<int>(before.top - target.top), 0, slide);
        }
        if (!animate)
        {
            from = 0;
        }

        // 首帧就放在起点，避免先在终点闪一下。
        ApplyFlyoutFrame(target, from);

        ShowWindow(g.window, SW_SHOW);
        SetForegroundWindow(g.window);
        SetFocus(g.window);
        UpdateWindow(g.window);
        if (animate)
        {
            // 等一帧合成：新窗口（尤其带亚克力背板）第一次上屏要几十毫秒，
            // 不等的话动画已经跑了一半窗口才出现，开头会"跳"一下。
            DwmFlush();
            StartFlyoutAnimation(true, target, from);
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
        ApplyFlyoutFrame(target, from);

        ShowWindow(g.window, SW_SHOWNOACTIVATE);
        UpdateWindow(g.window);
        if (animate)
        {
            DwmFlush();
            StartFlyoutAnimation(true, target, from);
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
        KillTimer(g.window, kSwitchTimerId);
        g.eyeSwitchAnimating = false;
        g.eyeSwitchVisual = g.eyeCare ? 1.0f : 0.0f;
        if (g.dragging)
        {
            ReleaseCapture();
            g.dragging = false;
            g.dragPart = PartNone;
            QueueBrightnessWrite(g.percent, 1);
            SaveEyeCareSettings();
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

        const RECT target = g.flyoutRect;
        g.closing = true;
        g.flashing = false;
        StartFlyoutAnimation(false, target, 0);
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
        RestoreGammaRamps();
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
            if (g.dragPart == PartWarmth)
            {
                SetWarmthFromMouse(point.x);
            }
            else
            {
                SetSliderFromMouse(point.x);
            }
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
        // 菜单刚刚被任务栏点击关掉时，同一次点击产生的托盘消息不应再把它弹回来。
        if (menu.lastClosed != 0 && now - menu.lastClosed < 300)
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
            parts.push_back(PartEye);
            if (g.eyeCare)
            {
                parts.push_back(PartWarmth);
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

        if (g.focusPart == PartWarmth)
        {
            if (!g.eyeCare)
            {
                return;
            }

            switch (key)
            {
            case VK_LEFT:
            case VK_DOWN:
                SetEyeCareLevel(g.eyeCareLevel - ((GetKeyState(VK_SHIFT) & 0x8000) ? 10 : 1));
                return;
            case VK_RIGHT:
            case VK_UP:
                SetEyeCareLevel(g.eyeCareLevel + ((GetKeyState(VK_SHIFT) & 0x8000) ? 10 : 1));
                return;
            case VK_PRIOR:
                SetEyeCareLevel(g.eyeCareLevel + 10);
                return;
            case VK_NEXT:
                SetEyeCareLevel(g.eyeCareLevel - 10);
                return;
            case VK_HOME:
                SetEyeCareLevel(0);
                return;
            case VK_END:
                SetEyeCareLevel(100);
                return;
            default:
                return;
            }
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
        if (part == PartSlider || part == PartWarmth)
        {
            g.dragging = true;
            g.dragPart = part;
            SetCapture(g.window);
            if (part == PartSlider)
            {
                SetSliderFromMouse(point.x);
            }
            else
            {
                SetWarmthFromMouse(point.x);
            }
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
            g.dragPart = PartNone;
            ReleaseCapture();
            QueueBrightnessWrite(g.percent, 1);
            SaveEyeCareSettings();
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
                        CompositeLiveSurface(g.paintBits, g.maskBits, g.bufferBits, g.bufferWidth, g.bufferHeight);
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
                    // 按面积加权重采样到当前窗口尺寸（颜色与 alpha 一起），再 1:1 贴到窗口。
                    ScaleSurfaceToPresent(clientWidth, clientHeight);
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
            else if (wParam == kSwitchTimerId)
            {
                const ULONGLONG elapsed = GetTickCount64() - g.eyeSwitchAnimStart;
                const float t = std::min(
                    1.0f,
                    static_cast<float>(elapsed) / kSwitchAnimMs);
                const float eased = 1.0f - std::pow(1.0f - t, 3.0f);
                g.eyeSwitchVisual =
                    g.eyeSwitchAnimFrom +
                    (g.eyeSwitchAnimTo - g.eyeSwitchAnimFrom) * eased;

                if (t >= 1.0f)
                {
                    g.eyeSwitchVisual = g.eyeSwitchAnimTo;
                    g.eyeSwitchAnimating = false;
                    KillTimer(window, kSwitchTimerId);
                }
                InvalidateUi();
            }
            return 0;

        case kAnimFrameMessage:
            g.animFramePosted.store(false);
            if (static_cast<unsigned>(wParam) == g.animGeneration.load())
            {
                ApplyFlyoutFrame(g.animTarget, g.animLatestOffset.load());
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
            // 延后一拍再弹菜单：避免在按键处理里直接进入模态消息循环，
            // 也避免残留的右键状态影响菜单的激活。
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
            if (part == PartDisplay || part == PartSync || part == PartEye ||
                part == PartEmptySettings)
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
            RefreshDisplays();
            RefreshGammaDevices();
            if (g.eyeCare)
            {
                ApplyEyeCareRamp();
            }
            if (g.isOpen)
            {
                InvalidateUi();
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
            RestoreGammaRamps();
            if (menu.window)
            {
                KillTimer(menu.window, kMenuSlideTimerId);
                DestroyWindow(menu.window);
                menu.window = nullptr;
            }
            DestroyMenuBuffers();
            DestroyMenuFonts();
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
    InitializeDirectWrite();
    g.transparency = std::clamp<int>(
        static_cast<int>(ReadDword(
            L"Software\\SdrBrightnessTray",
            L"Transparency",
            kDefaultTransparency)),
        0,
        100);

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
    RepairLeftoverGamma();
    RefreshGammaDevices();
    LoadEyeCareSettings();
    UpdateTrayTooltip();
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

    ShutdownDirectWrite();

    if (gdiplusToken)
    {
        Gdiplus::GdiplusShutdown(gdiplusToken);
    }

    return static_cast<int>(message.wParam);
}
