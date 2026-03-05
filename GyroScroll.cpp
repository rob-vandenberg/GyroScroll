
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>
#include <commctrl.h>
#include <hidsdi.h>  
#include <hidpi.h>
#include <setupapi.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "hid.lib")
#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "advapi32.lib")

#define VERSION_MAJOR  1
#define VERSION_MINOR  0
#define VERSION_PATCH  1
#define VERSION_STRING "1.0.1"
#ifdef _WIN64
    #define BITNESS_STRING "64-bit"
#else
    #define BITNESS_STRING "32-bit"
#endif

#define IDI_APPICON   1
#define IDD_SETTINGS  101
#define IDD_ABOUT     102
#define IDC_VERSION   200
#define IDC_LINK_URL  201

static constexpr USAGE UP_GENERIC   = 0x01;   
static constexpr USAGE UP_DIGITIZER = 0x0D;   
static constexpr USAGE U_TOUCHPAD   = 0x05;   
static constexpr USAGE U_TIP_SWITCH = 0x42;   
static constexpr USAGE U_CONTACT_ID = 0x51;   
static constexpr USAGE U_CONTACT_CNT= 0x54;   
static constexpr USAGE U_X          = 0x30;   
static constexpr USAGE U_Y          = 0x31;   

static float g_sideEdge        = 0.06f;   
static float g_edgeBottom       = 0.06f;   
static float g_speedV           = 16.0f;   
static float g_speedH           = 16.0f;   
static bool  g_naturalV         = false;   
static bool  g_naturalH         = false;   
static float g_reverseThreshold = 0.012f;  
static bool  g_leftHanded       = false;   

static std::wstring g_iniPath;

static void LoadSettings()
{
    const wchar_t* S = L"GyroScroll";
    auto rf = [&](const wchar_t* k, float d) -> float {
        wchar_t b[32] = {};
        if (!GetPrivateProfileStringW(S, k, nullptr, b, 32, g_iniPath.c_str())) return d;
        try { return std::stof(b); } catch (...) { return d; }
    };
    auto rb = [&](const wchar_t* k, bool d) -> bool {
        wchar_t b[4] = {};
        GetPrivateProfileStringW(S, k, d ? L"1" : L"0", b, 4, g_iniPath.c_str());
        return b[0] == L'1';
    };
    
    g_sideEdge  = rf(L"SideEdge",  g_sideEdge  * 100.f) / 100.f;
    g_edgeBottom = rf(L"BottomEdge", g_edgeBottom * 100.f) / 100.f;
    g_speedV     = rf(L"SpeedV",     g_speedV);
    g_speedH     = rf(L"SpeedH",     g_speedH);
    g_naturalV   = rb(L"NaturalV",   g_naturalV);
    g_naturalH   = rb(L"NaturalH",   g_naturalH);
    g_reverseThreshold = rf(L"Sensitivity", g_reverseThreshold * 1000.f) / 1000.f;
    g_leftHanded = rb(L"LeftHanded", g_leftHanded);
}

static void SaveSettings()
{
    const wchar_t* S = L"GyroScroll";
    auto wi = [&](const wchar_t* k, int v) {
        wchar_t buf[16];
        swprintf_s(buf, L"%d", v);
        WritePrivateProfileStringW(S, k, buf, g_iniPath.c_str());
    };
    auto wb = [&](const wchar_t* k, bool v) {
        WritePrivateProfileStringW(S, k, v ? L"1" : L"0", g_iniPath.c_str());
    };
    
    wi(L"SideEdge",   (int)(g_sideEdge        * 100.f  + 0.5f));
    wi(L"BottomEdge",  (int)(g_edgeBottom        * 100.f  + 0.5f));
    wi(L"SpeedV",      (int)(g_speedV                     + 0.5f));
    wi(L"SpeedH",      (int)(g_speedH                     + 0.5f));
    wb(L"NaturalV",    g_naturalV);
    wb(L"NaturalH",    g_naturalH);
    wi(L"Sensitivity", (int)(g_reverseThreshold  * 1000.f + 0.5f));
    wb(L"LeftHanded",  g_leftHanded);
}

struct FingerSlot {
    USHORT link;                    
    LONG   xMin, xMax;             
    LONG   yMin, yMax;             
};

struct Touchpad {
    HANDLE               devHandle  = nullptr;
    PHIDP_PREPARSED_DATA preparsed  = nullptr;
    HIDP_CAPS            hidCaps    = {};
    std::vector<FingerSlot> slots;  
    bool   hasContactCount = false;
    USHORT contactCountLink = 0;
    
    LONG xMin = 0, xMax = 1;
    LONG yMin = 0, yMax = 1;
};

static std::vector<Touchpad> g_pads;

static void BuildTouchpadList(HWND hwnd)
{
    
    for (auto& tp : g_pads)
        if (tp.preparsed) HidD_FreePreparsedData(tp.preparsed);
    g_pads.clear();

    
    UINT n = 0;
    GetRawInputDeviceList(nullptr, &n, sizeof(RAWINPUTDEVICELIST));
    if (!n) return;

    std::vector<RAWINPUTDEVICELIST> devs(n);
    GetRawInputDeviceList(devs.data(), &n, sizeof(RAWINPUTDEVICELIST));

    for (auto& d : devs) {
        if (d.dwType != RIM_TYPEHID) continue;

        
        RID_DEVICE_INFO info = {};
        info.cbSize = sizeof(info);
        UINT sz = sizeof(info);
        if (GetRawInputDeviceInfoW(d.hDevice, RIDI_DEVICEINFO, &info, &sz) == (UINT)-1)
            continue;
        if (info.hid.usUsagePage != UP_DIGITIZER || info.hid.usUsage != U_TOUCHPAD)
            continue;

        
        UINT psz = 0;
        GetRawInputDeviceInfoW(d.hDevice, RIDI_PREPARSEDDATA, nullptr, &psz);
        if (!psz) continue;

        std::vector<BYTE> pdata(psz);
        GetRawInputDeviceInfoW(d.hDevice, RIDI_PREPARSEDDATA, pdata.data(), &psz);

        Touchpad tp;
        tp.devHandle = d.hDevice;
        
        tp.preparsed = reinterpret_cast<PHIDP_PREPARSED_DATA>(new BYTE[psz]);
        std::memcpy(tp.preparsed, pdata.data(), psz);

        if (HidP_GetCaps(tp.preparsed, &tp.hidCaps) != HIDP_STATUS_SUCCESS) {
            delete[] reinterpret_cast<BYTE*>(tp.preparsed);
            continue;
        }

        
        USHORT nv = tp.hidCaps.NumberInputValueCaps;
        std::vector<HIDP_VALUE_CAPS> vc(nv);
        HidP_GetValueCaps(HidP_Input, vc.data(), &nv, tp.preparsed);

        struct SlotBuilder {
            bool hasX = false, hasY = false;
            LONG xMin = 0, xMax = 0, yMin = 0, yMax = 0;
        };
        std::map<USHORT, SlotBuilder> builders;

        for (auto& cap : vc) {
            
            USAGE uFirst = cap.IsRange ? cap.Range.UsageMin  : cap.NotRange.Usage;
            USAGE uLast  = cap.IsRange ? cap.Range.UsageMax  : cap.NotRange.Usage;
            USHORT lc    = cap.LinkCollection;

            for (USAGE u = uFirst; u <= uLast; ++u) {
                if (cap.UsagePage == UP_GENERIC) {
                    if (u == U_X) {
                        builders[lc].hasX = true;
                        builders[lc].xMin = cap.LogicalMin;
                        builders[lc].xMax = cap.LogicalMax;
                        tp.xMin = cap.LogicalMin;
                        tp.xMax = cap.LogicalMax;
                    } else if (u == U_Y) {
                        builders[lc].hasY = true;
                        builders[lc].yMin = cap.LogicalMin;
                        builders[lc].yMax = cap.LogicalMax;
                        tp.yMin = cap.LogicalMin;
                        tp.yMax = cap.LogicalMax;
                    }
                } else if (cap.UsagePage == UP_DIGITIZER && u == U_CONTACT_CNT) {
                    tp.hasContactCount  = true;
                    tp.contactCountLink = lc;
                }
            }
        }

        for (auto it = builders.begin(); it != builders.end(); ++it) {
            USHORT lc      = it->first;
            SlotBuilder& b = it->second;
            if (b.hasX && b.hasY && b.xMax > b.xMin && b.yMax > b.yMin) {
                FingerSlot slot;
                slot.link = lc;
                slot.xMin = b.xMin; slot.xMax = b.xMax;
                slot.yMin = b.yMin; slot.yMax = b.yMax;
                tp.slots.push_back(slot);
            }
        }

        if (tp.slots.empty()) {
            delete[] reinterpret_cast<BYTE*>(tp.preparsed);
            continue;
        }

        g_pads.push_back(std::move(tp));
    }

    
    if (!g_pads.empty()) {
        RAWINPUTDEVICE rid = {};
        rid.usUsagePage = UP_DIGITIZER;
        rid.usUsage     = U_TOUCHPAD;
        rid.dwFlags     = RIDEV_INPUTSINK;
        rid.hwndTarget  = hwnd;
        RegisterRawInputDevices(&rid, 1, sizeof(rid));
    }
}

struct Contact {
    ULONG id;
    float x, y;  
    bool  tip;   
};

static std::vector<Contact> ParseContacts(const Touchpad& tp,
                                           const BYTE* report, UINT len)
{
    std::vector<Contact> out;

    
    USHORT nb = tp.hidCaps.NumberInputButtonCaps;
    std::vector<HIDP_BUTTON_CAPS> bc(nb ? nb : 1);
    if (nb) HidP_GetButtonCaps(HidP_Input, bc.data(), &nb, tp.preparsed);

    
    int maxSlots    = (int)tp.slots.size();
    int activeCount = maxSlots;
    if (tp.hasContactCount) {
        ULONG cnt = 0;
        
        
        if (HidP_GetUsageValue(HidP_Input, UP_DIGITIZER, tp.contactCountLink,
                U_CONTACT_CNT, &cnt,
                tp.preparsed, reinterpret_cast<PCHAR>(const_cast<BYTE*>(report)), len)
                == HIDP_STATUS_SUCCESS)
            activeCount = (int)std::min((ULONG)maxSlots, cnt);
    }

    for (int i = 0; i < activeCount; ++i) {
        const FingerSlot& slot = tp.slots[i];
        Contact c = { (ULONG)i, 0.f, 0.f, false };

        ULONG xv = 0, yv = 0, idv = 0;

        bool hx = HidP_GetUsageValue(HidP_Input, UP_GENERIC, slot.link,
            U_X, &xv, tp.preparsed,
            reinterpret_cast<PCHAR>(const_cast<BYTE*>(report)), len)
            == HIDP_STATUS_SUCCESS;
        bool hy = HidP_GetUsageValue(HidP_Input, UP_GENERIC, slot.link,
            U_Y, &yv, tp.preparsed,
            reinterpret_cast<PCHAR>(const_cast<BYTE*>(report)), len)
            == HIDP_STATUS_SUCCESS;

        if (!hx || !hy) continue;

        c.x = (float)((LONG)xv - slot.xMin) / (float)(slot.xMax - slot.xMin);
        c.y = (float)((LONG)yv - slot.yMin) / (float)(slot.yMax - slot.yMin);

        
        c.x = std::max(0.f, std::min(1.f, c.x));
        c.y = std::max(0.f, std::min(1.f, c.y));

        if (HidP_GetUsageValue(HidP_Input, UP_DIGITIZER, slot.link,
                U_CONTACT_ID, &idv, tp.preparsed,
                reinterpret_cast<PCHAR>(const_cast<BYTE*>(report)), len)
                == HIDP_STATUS_SUCCESS)
            c.id = idv;

        
        for (USHORT j = 0; j < nb; ++j) {
            if (bc[j].UsagePage != UP_DIGITIZER) continue;
            if (bc[j].LinkCollection != slot.link) continue;
            USAGE usages[4] = {};
            ULONG ucount    = 4;
            if (HidP_GetUsages(HidP_Input, UP_DIGITIZER, slot.link,
                    usages, &ucount, tp.preparsed,
                    reinterpret_cast<PCHAR>(const_cast<BYTE*>(report)), len)
                    == HIDP_STATUS_SUCCESS) {
                for (ULONG u = 0; u < ucount; ++u)
                    if (usages[u] == U_TIP_SWITCH) { c.tip = true; break; }
            }
            break;
        }

        out.push_back(c);
    }
    return out;
}

enum class ScrollMode { IDLE, SCROLL_V, SCROLL_H };

static ScrollMode g_mode       = ScrollMode::IDLE;
static ULONG      g_trackId    = 0xFFFFFFFF;
static int        g_touchZone  = 0;   // 0=undecided, +1=originated in zone, -1=outside zone

static float g_posX        = 0.f;  
static float g_posY        = 0.f;
static float g_dirX        = 0.f;  
static float g_dirY        = 0.f;
static float g_scrollDir   = 0.f;  
static float g_accumScroll = 0.f;  

static constexpr float DZ_START         = 0.0056f;
static constexpr float DZ_MOVE          = 0.0056f;
static constexpr float DZ_REVERSE       = 0.0112f;
static constexpr float DZ_START_ANGLE   = 3.14159f / 4.0f;  
static constexpr float DZ_REVERSE_ANGLE = 3.14159f;         

static constexpr UINT SCROLL_TIMER_ID = 1;
static constexpr UINT SCROLL_TIMER_MS = 16;  

static void StartScrollFreeze()
{
    POINT pt;
    GetCursorPos(&pt);
    RECT r = { pt.x, pt.y, pt.x + 1, pt.y + 1 };
    ClipCursor(&r);
}

static void StopScrollFreeze()
{
    ClipCursor(nullptr);
}

static float AngleBetween(float ax, float ay, float bx, float by)
{
    float normA = std::sqrt(ax*ax + ay*ay);
    float normB = std::sqrt(bx*bx + by*by);
    if (normA < 1e-7f || normB < 1e-7f) return 1.5707963f;  
    float cosA = (ax*bx + ay*by) / (normA * normB);
    cosA = std::max(-1.f, std::min(1.f, cosA));
    return std::acos(cosA);
}

static void SendScrollUnits(int units, bool vertical)
{
    INPUT in{};
    in.type         = INPUT_MOUSE;
    in.mi.dwFlags   = vertical ? MOUSEEVENTF_WHEEL : MOUSEEVENTF_HWHEEL;
    in.mi.mouseData = (DWORD)units;
    SendInput(1, &in, sizeof(INPUT));
}

static void OnContacts(const std::vector<Contact>& contacts)
{
    // Reset g_touchZone when no finger is on the pad
    bool anyTip = false;
    for (const auto& c : contacts)
        if (c.tip) { anyTip = true; break; }
    if (!anyTip)
        g_touchZone = 0;

    if (g_mode == ScrollMode::IDLE) {
        for (const auto& c : contacts) {
            if (!c.tip) continue;

            // Decide zone membership on the very first touch frame
            if (g_touchZone == 0) {
                bool inBottom = c.y >= (1.f - g_edgeBottom);
                bool inSide   = g_leftHanded ? (c.x <= g_sideEdge)
                                             : (c.x >= (1.f - g_sideEdge));
                g_touchZone = ((inSide && !inBottom) || (inBottom && !inSide)) ? +1 : -1;
            }

            // Ignore touches that did not originate in a zone
            if (g_touchZone < 0) return;

            bool inBottom = c.y >= (1.f - g_edgeBottom);
            bool inSide   = g_leftHanded ? (c.x <= g_sideEdge)
                                         : (c.x >= (1.f - g_sideEdge));
            if (inSide && !inBottom) {
                g_mode       = ScrollMode::SCROLL_V;
                g_trackId    = c.id;
                g_posX       = c.x;  g_posY = c.y;
                
                
                g_dirX       = 0.f;  g_dirY = 1.f;
                g_scrollDir  = 0.f;
                g_accumScroll= 0.f;
                StartScrollFreeze();
                return;
            }
            if (inBottom && !inSide) {
                g_mode       = ScrollMode::SCROLL_H;
                g_trackId    = c.id;
                g_posX       = c.x;  g_posY = c.y;
                
                g_dirX       = 1.f;  g_dirY = 0.f;
                g_scrollDir  = 0.f;
                g_accumScroll= 0.f;
                StartScrollFreeze();
                return;
            }
        }
        return;
    }

    
    const Contact* tr = nullptr;
    for (const auto& c : contacts)
        if (c.id == g_trackId) { tr = &c; break; }

    if (!tr || !tr->tip) {
        
        if (g_accumScroll != 0.f) {
            int units = (int)(g_accumScroll * WHEEL_DELTA);
            if (units) SendScrollUnits(units, g_mode == ScrollMode::SCROLL_V);
        }
        g_mode       = ScrollMode::IDLE;
        g_trackId    = 0xFFFFFFFF;
        g_accumScroll= 0.f;
        g_touchZone  = 0;
        StopScrollFreeze();
        return;
    }

    
    float nx = tr->x - g_posX;
    float ny = tr->y - g_posY;
    float dist = std::sqrt(nx*nx + ny*ny);

    if (g_scrollDir == 0.f) {
        
        
        
        float dot = nx*g_dirX + ny*g_dirY;
        if (AngleBetween(g_dirX, g_dirY, nx, ny) < DZ_START_ANGLE/2.f && dot > DZ_START) {
            g_scrollDir = 1.f;
        } else if (AngleBetween(g_dirX, g_dirY, -nx, -ny) < DZ_START_ANGLE/2.f && dot < -DZ_START) {
            g_scrollDir = -1.f;
        } else {
            return;  
        }
        
        
        
        
        if (dist > 1e-6f) { g_dirX = nx / dist; g_dirY = ny / dist; }
    }

    

    
    
    if (AngleBetween(g_dirX, g_dirY, -nx, -ny) < DZ_REVERSE_ANGLE/2.f) {
        if (dist > g_reverseThreshold) {
            g_scrollDir *= -1.f;
            
        } else {
            
            return;
        }
    } else if (dist <= DZ_MOVE && nx*g_dirX + ny*g_dirY <= DZ_MOVE) {
        
        return;
    }

    
    
    
    
    bool isV = (g_mode == ScrollMode::SCROLL_V);
    float speed  = isV ? -g_speedV : g_speedH;
    bool natural = isV ? g_naturalV : g_naturalH;

    float clicks = g_scrollDir * dist * speed;
    if (natural) clicks = -clicks;

    
    g_accumScroll += clicks;

    
    g_posX = tr->x;
    g_posY = tr->y;
    if (dist > 1e-6f) {
        g_dirX = nx / dist;
        g_dirY = ny / dist;
    }
}

static void ProcessRawInput(HRAWINPUT hRaw)
{
    UINT sz = 0;
    GetRawInputData(hRaw, RID_INPUT, nullptr, &sz, sizeof(RAWINPUTHEADER));
    if (!sz) return;

    std::vector<BYTE> buf(sz);
    if (GetRawInputData(hRaw, RID_INPUT, buf.data(), &sz, sizeof(RAWINPUTHEADER)) != sz)
        return;

    const auto* raw = reinterpret_cast<const RAWINPUT*>(buf.data());
    if (raw->header.dwType != RIM_TYPEHID) return;

    
    Touchpad* tp = nullptr;
    for (auto& p : g_pads)
        if (p.devHandle == raw->header.hDevice) { tp = &p; break; }
    if (!tp) return;

    
    UINT reportSize  = raw->data.hid.dwSizeHid;
    UINT reportCount = raw->data.hid.dwCount;
    const BYTE* base = raw->data.hid.bRawData;

    for (UINT r = 0; r < reportCount; ++r) {
        auto contacts = ParseContacts(*tp, base + r * reportSize, reportSize);
        if (!contacts.empty()) OnContacts(contacts);
    }
}

static NOTIFYICONDATAW g_nid    = {};
static constexpr UINT  WM_TRAY      = WM_USER + 1;
static constexpr UINT  IDM_QUIT     = 1001;
static constexpr UINT  IDM_SETTINGS = 1002;
static constexpr UINT  IDM_ABOUT    = 1003;

static HWND g_aboutWnd = nullptr;

static LRESULT CALLBACK LinkCursorProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp,
    UINT_PTR, DWORD_PTR)
{
    if (msg == WM_SETCURSOR) { SetCursor(LoadCursorW(nullptr, (LPCWSTR)IDC_HAND)); return TRUE; }
    return DefSubclassProc(hwnd, msg, wp, lp);
}

static INT_PTR CALLBACK AboutDlgProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg)
    {
    case WM_INITDIALOG:
    {
        HINSTANCE hi  = GetModuleHandleW(nullptr);
        HFONT font    = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
        const int PAD = 14, ICON_W = 32, W = 400, LH = 16;
        const int TX  = PAD + ICON_W + PAD;   
        const int TW  = W - TX - PAD;         
        int y = PAD;

        
        HWND hIco = CreateWindowW(L"STATIC", nullptr,
            WS_CHILD | WS_VISIBLE | SS_ICON,
            PAD, PAD, ICON_W, ICON_W, hwnd, nullptr, hi, nullptr);
        SendMessageW(hIco, STM_SETICON,
            (WPARAM)LoadIconW(nullptr, (LPCWSTR)IDI_INFORMATION), 0);

        
        auto addLine = [&](const wchar_t* txt, int h) {
            HWND hw = CreateWindowW(L"STATIC", txt,
                WS_CHILD | WS_VISIBLE | SS_LEFT,
                TX, y, TW, h, hwnd, nullptr, hi, nullptr);
            SendMessageW(hw, WM_SETFONT, (WPARAM)font, TRUE);
            y += h + 6;
        };

        wchar_t hdr[64];
        swprintf_s(hdr, L"GyroScroll %hs (%hs)", VERSION_STRING, BITNESS_STRING);
        addLine(hdr, LH);
        addLine(L"Circular scrolling for Precision Touchpads in Windows 10/11", LH);
        addLine(L"Move your finger along the right or bottom edge of your touchpad "
                L"and make a circular motion for continuous scrolling, without lifting.",
                LH * 2 + 4);
        addLine(L"Copyright \u00A9 2025 Rob Vandenberg", LH);

        
        
        LOGFONTW lf = {};
        GetObjectW(font, sizeof(lf), &lf);
        lf.lfUnderline = TRUE;
        HFONT linkFont = CreateFontIndirectW(&lf);

        HWND hLink = CreateWindowW(L"STATIC",
            L"https://github.com/rob-vandenberg/gyroscroll",
            WS_CHILD | WS_VISIBLE | SS_NOTIFY | SS_LEFT,
            TX, y, TW, LH + 4, hwnd,
            reinterpret_cast<HMENU>((UINT_PTR)IDC_LINK_URL), hi, nullptr);
        SendMessageW(hLink, WM_SETFONT, (WPARAM)linkFont, TRUE);
        SetWindowLongPtrW(hLink, GWLP_USERDATA, (LONG_PTR)linkFont);
        SetWindowSubclass(hLink, LinkCursorProc, 0, 0);
        y += LH + 4 + PAD;

        const int BTN_W = 80, BTN_H = 24;
        HWND hOk = CreateWindowW(L"BUTTON", L"OK",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
            (W - BTN_W) / 2, y, BTN_W, BTN_H,
            hwnd, reinterpret_cast<HMENU>((UINT_PTR)IDOK), hi, nullptr);
        SendMessageW(hOk, WM_SETFONT, (WPARAM)font, TRUE);
        y += BTN_H + PAD;

        RECT adj = { 0, 0, W, y };
        AdjustWindowRect(&adj, WS_CAPTION | WS_SYSMENU, FALSE);
        int ww = adj.right - adj.left, wh = adj.bottom - adj.top;
        SetWindowPos(hwnd, nullptr,
            (GetSystemMetrics(SM_CXSCREEN) - ww) / 2,
            (GetSystemMetrics(SM_CYSCREEN) - wh) / 2,
            ww, wh, SWP_NOZORDER);
        return TRUE;
    }
    case WM_CTLCOLORSTATIC:
        
        if (GetDlgCtrlID((HWND)lp) == IDC_LINK_URL) {
            SetTextColor((HDC)wp, RGB(0, 102, 204));
            SetBkMode((HDC)wp, TRANSPARENT);
            return (INT_PTR)GetStockObject(NULL_BRUSH);
        }
        return FALSE;
    case WM_COMMAND:
        if (LOWORD(wp) == IDOK) DestroyWindow(hwnd);
        if (LOWORD(wp) == IDC_LINK_URL && HIWORD(wp) == STN_CLICKED)
            ShellExecuteW(hwnd, L"open",
                L"https://github.com/rob-vandenberg/gyroscroll",
                nullptr, nullptr, SW_SHOWNORMAL);
        return TRUE;
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return TRUE;
    case WM_DESTROY:
    {
        
        HWND hLink = GetDlgItem(hwnd, IDC_LINK_URL);
        if (hLink) {
            HFONT lf = (HFONT)GetWindowLongPtrW(hLink, GWLP_USERDATA);
            if (lf) DeleteObject(lf);
        }
        g_aboutWnd = nullptr;
        return TRUE;
    }
    }
    return FALSE;
}

static void ShowAboutBox(HWND hwnd)
{
    if (g_aboutWnd) { SetForegroundWindow(g_aboutWnd); return; }
    g_aboutWnd = CreateDialogW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDD_ABOUT), hwnd, AboutDlgProc);
    if (g_aboutWnd) { ShowWindow(g_aboutWnd, SW_SHOW); UpdateWindow(g_aboutWnd); }
}

static constexpr UINT IDC_EDGE_RIGHT    = 2001;  
static constexpr UINT IDC_EDGE_BOTTOM   = 2002;  
static constexpr UINT IDC_SPEED_V       = 2003;  
static constexpr UINT IDC_SPEED_H       = 2004;  
static constexpr UINT IDC_NATURAL_V     = 2005;  
static constexpr UINT IDC_NATURAL_H     = 2006;  
static constexpr UINT IDC_PREVIEW       = 2009;  
static constexpr UINT IDC_SLD_EDGE_R    = 2010;  
static constexpr UINT IDC_SLD_EDGE_B    = 2011;  
static constexpr UINT IDC_SLD_SPEED_V   = 2012;  
static constexpr UINT IDC_SLD_SPEED_H     = 2013;  
static constexpr UINT IDC_AUTOSTART       = 2014;  
static constexpr UINT IDC_REVERSE_THRESH  = 2015;  
static constexpr UINT IDC_SLD_REVERSE_T   = 2016;  
static constexpr UINT IDC_LEFTHANDED      = 2017;  
static constexpr UINT IDC_LABEL_SIDE_EDGE = 2018;  

static HWND g_settingsWnd = nullptr;

static bool g_syncLock = false;

static const wchar_t* RUN_KEY = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
static const wchar_t* RUN_VAL = L"GyroScroll";

static bool GetAutostart()
{
    HKEY hk;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, RUN_KEY, 0, KEY_READ, &hk) != ERROR_SUCCESS)
        return false;
    bool present = (RegQueryValueExW(hk, RUN_VAL, nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS);
    RegCloseKey(hk);
    return present;
}

static void SetAutostart(bool enable)
{
    HKEY hk;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, RUN_KEY, 0, KEY_SET_VALUE, &hk) != ERROR_SUCCESS)
        return;
    if (enable) {
        wchar_t path[MAX_PATH];
        GetModuleFileNameW(nullptr, path, MAX_PATH);
        RegSetValueExW(hk, RUN_VAL, 0, REG_SZ,
            reinterpret_cast<const BYTE*>(path),
            (DWORD)((wcslen(path) + 1) * sizeof(wchar_t)));
    } else {
        RegDeleteValueW(hk, RUN_VAL);
    }
    RegCloseKey(hk);
}

static INT_PTR CALLBACK SettingsDlgProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
static void ShowSettingsWindow(HWND parent);

static HWND MakeLabel(HWND p, HINSTANCE hi, const wchar_t* txt, int x, int y, int w, int h);
static HWND MakeSliderRow(HWND p, HINSTANCE hi, const wchar_t* label,
                           UINT editId, UINT sldId, int x, int y, int rowW,
                           int sldMin, int sldMax, int sldInit);

static int   EdgeToSlider(float v) { return std::max(1, std::min(30, (int)(v * 100.f + 0.5f))); }
static float SliderToEdge(int pos) { return pos / 100.f; }

static int   SpeedToSlider(float v) { return std::max(1, std::min(40, (int)(v + 0.5f))); }
static float SliderToSpeed(int pos) { return (float)pos; }

static int   ThreshToSlider(float v) { return std::max(1, std::min(30, (int)(v * 1000.f + 0.5f))); }
static float SliderToThresh(int pos) { return pos / 1000.f; }

static float GetEditFloat(HWND dlg, UINT id, float fallback)
{
    wchar_t buf[32] = {};
    GetDlgItemTextW(dlg, id, buf, 32);
    try { return std::stof(buf); } catch (...) { return fallback; }
}

static void SetEditEdge(HWND dlg, UINT id, float v)
{
    wchar_t buf[16];
    swprintf_s(buf, L"%d", EdgeToSlider(v));
    SetDlgItemTextW(dlg, id, buf);
}

static void SetEditSpeed(HWND dlg, UINT id, float v)
{
    wchar_t buf[16];
    swprintf_s(buf, L"%d", (int)(v + 0.5f));
    SetDlgItemTextW(dlg, id, buf);
}

static void SetEditThresh(HWND dlg, UINT id, float v)
{
    wchar_t buf[16];
    swprintf_s(buf, L"%d", ThreshToSlider(v));
    SetDlgItemTextW(dlg, id, buf);
}

static void SetSliderPos(HWND dlg, UINT id, int pos)
{
    SendDlgItemMessageW(dlg, id, TBM_SETPOS, TRUE, pos);
}

static void RefreshPreview(HWND hwnd)
{
    
    float r = GetEditFloat(hwnd, IDC_EDGE_RIGHT,  g_sideEdge  * 100.f) / 100.f;
    float b = GetEditFloat(hwnd, IDC_EDGE_BOTTOM, g_edgeBottom * 100.f) / 100.f;
    r = std::max(0.01f, std::min(0.30f, r));
    b = std::max(0.01f, std::min(0.30f, b));
    HWND prev = GetDlgItem(hwnd, IDC_PREVIEW);
    
    SetWindowLongPtrW(prev, GWLP_USERDATA,
        (LONG_PTR)(((DWORD)(WORD)(int)(r * 10000.f)) |
                   ((DWORD)(WORD)(int)(b * 10000.f) << 16)));
    InvalidateRect(prev, nullptr, TRUE);
}

static void DrawPreview(HWND panel, float sideEdge, float edgeBottom, bool leftHanded)
{
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(panel, &ps);

    RECT rc;
    GetClientRect(panel, &rc);
    int W = rc.right  - rc.left;
    int H = rc.bottom - rc.top;

    
    HBRUSH padBrush = CreateSolidBrush(RGB(210, 210, 220));
    FillRect(dc, &rc, padBrush);
    DeleteObject(padBrush);

    
    
    int base = std::min(W, H);

    
    {
        int zw = std::max(2, (int)(sideEdge * base));
        RECT zr = leftHanded ? RECT{ 0, 0, zw, H }
                             : RECT{ W - zw, 0, W, H };
        HBRUSH zb = CreateSolidBrush(RGB(80, 140, 255));
        FillRect(dc, &zr, zb);
        DeleteObject(zb);
    }

    
    {
        int zh = std::max(2, (int)(edgeBottom * base));
        RECT zr = { 0, H - zh, W, H };
        HBRUSH zb = CreateSolidBrush(RGB(60, 200, 120));
        FillRect(dc, &zr, zb);
        DeleteObject(zb);
    }

    
    HPEN pen = CreatePen(PS_SOLID, 1, RGB(140, 140, 155));
    SelectObject(dc, pen);
    SelectObject(dc, GetStockObject(NULL_BRUSH));
    RoundRect(dc, rc.left, rc.top, rc.right - 1, rc.bottom - 1, 10, 10);
    DeleteObject(pen);

    EndPaint(panel, &ps);
}

static LRESULT CALLBACK PreviewSubclassProc(HWND hwnd, UINT msg,
                                             WPARAM wp, LPARAM lp,
                                             UINT_PTR, DWORD_PTR)
{
    if (msg == WM_PAINT) {
        LONG_PTR packed = GetWindowLongPtrW(hwnd, GWLP_USERDATA);
        float r = (float)(LOWORD(packed)) / 10000.f;
        float b = (float)(HIWORD(packed)) / 10000.f;
        if (r < 0.01f) r = g_sideEdge;
        if (b < 0.01f) b = g_edgeBottom;
        bool lh = (IsDlgButtonChecked(GetParent(hwnd), IDC_LEFTHANDED) == BST_CHECKED);
        DrawPreview(hwnd, r, b, lh);
        return 0;
    }
    return DefSubclassProc(hwnd, msg, wp, lp);
}

static INT_PTR CALLBACK SettingsDlgProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg)
    {
    
    case WM_INITDIALOG:
    {
        HINSTANCE hi = GetModuleHandleW(nullptr);

        
        const int PAD    = 14;
        const int PREVW  = 220;                    
        const int PREVH  = 138;                    
        const int COL1   = PAD;
        const int COL2   = COL1 + PREVW + PAD*2;  
        const int RCOLW  = 290;                    
        const int CLW    = COL2 + RCOLW + PAD;    
        const int LH     = 18;                     
        const int RH     = 30;                     

        
        int ry = PAD;
        ry += LH + 4;   
        ry += RH;       
        ry += RH + 10;  
        ry += LH + 4;   
        ry += RH;       
        ry += RH;       
        ry += RH + 10;  
        ry += LH + 4;   
        ry += LH + 6;   
        ry += LH + 6;   
        const int BTN_H   = 26;
        const int BTN_GAP = 14;
        const int CLH     = ry + BTN_GAP + BTN_H + PAD;

        
        RECT adj = { 0, 0, CLW, CLH };
        AdjustWindowRect(&adj, WS_CAPTION | WS_SYSMENU, FALSE);
        SetWindowPos(hwnd, nullptr, 0, 0,
            adj.right - adj.left, adj.bottom - adj.top,
            SWP_NOMOVE | SWP_NOZORDER);

        
        ry = PAD;
        HWND prev = CreateWindowW(L"STATIC", nullptr,
            WS_CHILD | WS_VISIBLE | SS_OWNERDRAW,
            COL1, PAD, PREVW, PREVH,
            hwnd, reinterpret_cast<HMENU>((UINT_PTR)IDC_PREVIEW), hi, nullptr);
        SetWindowSubclass(prev, PreviewSubclassProc, 0, 0);

        
        MakeLabel(hwnd, hi,
            L"Blue  = vertical scroll zone\nGreen = horizontal scroll zone",
            COL1, PAD + PREVH + 6, PREVW, LH * 2 + 4);

        
        int leftColY = PAD + PREVH + 6 + LH * 2 + 4 + 8;
        CreateWindowW(L"BUTTON", L"Left handed operation",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
            COL1, leftColY, PREVW, LH + 2,
            hwnd, reinterpret_cast<HMENU>((UINT_PTR)IDC_LEFTHANDED), hi, nullptr);
        CheckDlgButton(hwnd, IDC_LEFTHANDED, g_leftHanded ? BST_CHECKED : BST_UNCHECKED);
        leftColY += LH + 2 + 6;

        
        CreateWindowW(L"BUTTON", L"Start with Windows",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
            COL1, leftColY, PREVW, LH + 2,
            hwnd, reinterpret_cast<HMENU>((UINT_PTR)IDC_AUTOSTART), hi, nullptr);
        CheckDlgButton(hwnd, IDC_AUTOSTART, GetAutostart() ? BST_CHECKED : BST_UNCHECKED);

        

        
        MakeLabel(hwnd, hi, L"Edge zones", COL2, ry, RCOLW, LH);
        ry += LH + 4;

        
        {
            const wchar_t* sideLabel = g_leftHanded ? L"Left edge:" : L"Right edge:";
            HWND hw = CreateWindowW(L"STATIC", sideLabel,
                WS_CHILD | WS_VISIBLE | SS_RIGHT | SS_CENTERIMAGE,
                COL2, ry, 90, 22,
                hwnd, reinterpret_cast<HMENU>((UINT_PTR)IDC_LABEL_SIDE_EDGE), hi, nullptr);
            (void)hw;
        }
        
        {
            const int LBL_W = 90, EDT_W = 52, GAP = 6, ROW_H = 22;
            const int SLD_W = RCOLW - LBL_W - EDT_W - GAP;
            CreateWindowW(L"EDIT", nullptr,
                WS_CHILD | WS_VISIBLE | WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL,
                COL2 + LBL_W + GAP, ry, EDT_W, ROW_H,
                hwnd, reinterpret_cast<HMENU>((UINT_PTR)IDC_EDGE_RIGHT), hi, nullptr);
            HWND sld = CreateWindowW(TRACKBAR_CLASSW, nullptr,
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | TBS_HORZ | TBS_NOTICKS,
                COL2 + LBL_W + GAP + EDT_W + GAP, ry - 2, SLD_W, ROW_H + 4,
                hwnd, reinterpret_cast<HMENU>((UINT_PTR)IDC_SLD_EDGE_R), hi, nullptr);
            SendMessageW(sld, TBM_SETRANGE, TRUE, MAKELPARAM(1, 30));
            SendMessageW(sld, TBM_SETPOS,   TRUE, EdgeToSlider(g_sideEdge));
        }
        SetEditEdge(hwnd, IDC_EDGE_RIGHT, g_sideEdge);
        ry += RH;

        MakeSliderRow(hwnd, hi, L"Bottom edge:",
            IDC_EDGE_BOTTOM, IDC_SLD_EDGE_B, COL2, ry, RCOLW,
            1, 30, EdgeToSlider(g_edgeBottom));
        SetEditEdge(hwnd, IDC_EDGE_BOTTOM, g_edgeBottom);
        ry += RH + 10;

        
        MakeLabel(hwnd, hi, L"Scroll speed (clicks / swipe)", COL2, ry, RCOLW, LH);
        ry += LH + 4;

        MakeSliderRow(hwnd, hi, L"Vertical:",
            IDC_SPEED_V, IDC_SLD_SPEED_V, COL2, ry, RCOLW,
            1, 40, SpeedToSlider(g_speedV));
        SetEditSpeed(hwnd, IDC_SPEED_V, g_speedV);
        ry += RH;

        MakeSliderRow(hwnd, hi, L"Horizontal:",
            IDC_SPEED_H, IDC_SLD_SPEED_H, COL2, ry, RCOLW,
            1, 40, SpeedToSlider(g_speedH));
        SetEditSpeed(hwnd, IDC_SPEED_H, g_speedH);
        ry += RH;

        MakeSliderRow(hwnd, hi, L"Sensitivity:",
            IDC_REVERSE_THRESH, IDC_SLD_REVERSE_T, COL2, ry, RCOLW,
            1, 30, ThreshToSlider(g_reverseThreshold));
        SetEditThresh(hwnd, IDC_REVERSE_THRESH, g_reverseThreshold);
        ry += RH + 10;

        
        MakeLabel(hwnd, hi, L"Natural (reversed) scroll", COL2, ry, RCOLW, LH);
        ry += LH + 4;

        CreateWindowW(L"BUTTON", L"Reverse vertical",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
            COL2, ry, RCOLW, LH + 2,
            hwnd, reinterpret_cast<HMENU>((UINT_PTR)IDC_NATURAL_V), hi, nullptr);
        CheckDlgButton(hwnd, IDC_NATURAL_V, g_naturalV ? BST_CHECKED : BST_UNCHECKED);
        ry += LH + 6;

        CreateWindowW(L"BUTTON", L"Reverse horizontal",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
            COL2, ry, RCOLW, LH + 2,
            hwnd, reinterpret_cast<HMENU>((UINT_PTR)IDC_NATURAL_H), hi, nullptr);
        CheckDlgButton(hwnd, IDC_NATURAL_H, g_naturalH ? BST_CHECKED : BST_UNCHECKED);

        
        const int BTN_W = 82;
        const int BTN_Y = CLH - PAD - BTN_H;
        const int BTN_X = CLW - PAD - BTN_W * 2 - 8;

        
        
        CreateWindowW(L"BUTTON", L"OK",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
            BTN_X, BTN_Y, BTN_W, BTN_H,
            hwnd, reinterpret_cast<HMENU>((UINT_PTR)IDOK), hi, nullptr);
        CreateWindowW(L"BUTTON", L"Cancel",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
            BTN_X + BTN_W + 8, BTN_Y, BTN_W, BTN_H,
            hwnd, reinterpret_cast<HMENU>((UINT_PTR)IDCANCEL), hi, nullptr);

        
        HFONT font = reinterpret_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
        EnumChildWindows(hwnd, [](HWND child, LPARAM lp) -> BOOL {
            SendMessageW(child, WM_SETFONT, (WPARAM)lp, TRUE);
            return TRUE;
        }, reinterpret_cast<LPARAM>(font));

        
        RECT wr;
        GetWindowRect(hwnd, &wr);
        int sw = GetSystemMetrics(SM_CXSCREEN);
        int sh = GetSystemMetrics(SM_CYSCREEN);
        SetWindowPos(hwnd, nullptr,
            (sw - (wr.right - wr.left)) / 2,
            (sh - (wr.bottom - wr.top)) / 2,
            0, 0, SWP_NOZORDER | SWP_NOSIZE);

        RefreshPreview(hwnd);
        return TRUE;  
    }

    
    case WM_HSCROLL:
    {
        if (g_syncLock) break;
        HWND sldr = (HWND)lp;
        int  pos  = (int)SendMessageW(sldr, TBM_GETPOS, 0, 0);
        int  id   = GetDlgCtrlID(sldr);
        g_syncLock = true;
        if (id == IDC_SLD_EDGE_R)    { SetEditEdge (hwnd, IDC_EDGE_RIGHT,  SliderToEdge (pos)); RefreshPreview(hwnd); }
        if (id == IDC_SLD_EDGE_B)    { SetEditEdge (hwnd, IDC_EDGE_BOTTOM, SliderToEdge (pos)); RefreshPreview(hwnd); }
        if (id == IDC_SLD_SPEED_V)   { SetEditSpeed(hwnd, IDC_SPEED_V,     SliderToSpeed(pos)); }
        if (id == IDC_SLD_SPEED_H)   { SetEditSpeed(hwnd, IDC_SPEED_H,     SliderToSpeed(pos)); }
        if (id == IDC_SLD_REVERSE_T) { SetEditThresh(hwnd, IDC_REVERSE_THRESH, SliderToThresh(pos)); }
        g_syncLock = false;
        return TRUE;
    }

    
    case WM_COMMAND:
    {
        UINT id  = LOWORD(wp);
        UINT evt = HIWORD(wp);

        if (id == IDC_LEFTHANDED && evt == BN_CLICKED) {
            bool lh = (IsDlgButtonChecked(hwnd, IDC_LEFTHANDED) == BST_CHECKED);
            SetDlgItemTextW(hwnd, IDC_LABEL_SIDE_EDGE, lh ? L"Left edge:" : L"Right edge:");
            RefreshPreview(hwnd);
        }

        if (evt == EN_CHANGE && !g_syncLock) {
            g_syncLock = true;
            if (id == IDC_EDGE_RIGHT)     { int pct = std::max(1, std::min(30, (int)GetEditFloat(hwnd, id, g_sideEdge  * 100.f))); SetSliderPos(hwnd, IDC_SLD_EDGE_R,  pct); RefreshPreview(hwnd); }
            if (id == IDC_EDGE_BOTTOM)    { int pct = std::max(1, std::min(30, (int)GetEditFloat(hwnd, id, g_edgeBottom * 100.f))); SetSliderPos(hwnd, IDC_SLD_EDGE_B,  pct); RefreshPreview(hwnd); }
            if (id == IDC_SPEED_V)        { float v = GetEditFloat(hwnd, id, g_speedV);     SetSliderPos(hwnd, IDC_SLD_SPEED_V, SpeedToSlider(v)); }
            if (id == IDC_SPEED_H)        { float v = GetEditFloat(hwnd, id, g_speedH);     SetSliderPos(hwnd, IDC_SLD_SPEED_H, SpeedToSlider(v)); }
            if (id == IDC_REVERSE_THRESH) { int t = std::max(1, std::min(30, (int)GetEditFloat(hwnd, id, g_reverseThreshold * 1000.f))); SetSliderPos(hwnd, IDC_SLD_REVERSE_T, t); }
            g_syncLock = false;
        }

        if (id == IDOK) {
            
            float r  = GetEditFloat(hwnd, IDC_EDGE_RIGHT,  g_sideEdge  * 100.f) / 100.f;
            float b  = GetEditFloat(hwnd, IDC_EDGE_BOTTOM, g_edgeBottom * 100.f) / 100.f;
            float sv = GetEditFloat(hwnd, IDC_SPEED_V,     g_speedV);
            float sh = GetEditFloat(hwnd, IDC_SPEED_H,     g_speedH);
            float rt = GetEditFloat(hwnd, IDC_REVERSE_THRESH, g_reverseThreshold * 1000.f) / 1000.f;
            r  = std::max(0.01f, std::min(0.30f, r));
            b  = std::max(0.01f, std::min(0.30f, b));
            sv = std::max(1.f,   std::min(40.f,  sv));
            sh = std::max(1.f,   std::min(40.f,  sh));
            
            rt = std::max(0.001f, rt);
            g_sideEdge        = r;
            g_edgeBottom       = b;
            g_speedV           = sv;
            g_speedH           = sh;
            g_reverseThreshold = rt;
            g_naturalV   = (IsDlgButtonChecked(hwnd, IDC_NATURAL_V) == BST_CHECKED);
            g_naturalH   = (IsDlgButtonChecked(hwnd, IDC_NATURAL_H) == BST_CHECKED);
            g_leftHanded = (IsDlgButtonChecked(hwnd, IDC_LEFTHANDED) == BST_CHECKED);
            SetAutostart(IsDlgButtonChecked(hwnd, IDC_AUTOSTART) == BST_CHECKED);
            SaveSettings();
            DestroyWindow(hwnd);
        }
        if (id == IDCANCEL) DestroyWindow(hwnd);
        return TRUE;
    }

    case WM_DESTROY:
        g_settingsWnd = nullptr;
        return TRUE;

    case WM_CLOSE:
        DestroyWindow(hwnd);
        return TRUE;
    }
    return FALSE;  
}

static HWND MakeLabel(HWND p, HINSTANCE hi, const wchar_t* txt, int x, int y, int w, int h)
{
    return CreateWindowW(L"STATIC", txt, WS_CHILD | WS_VISIBLE | SS_LEFT,
        x, y, w, h, p, nullptr, hi, nullptr);
}

static HWND MakeSliderRow(HWND p, HINSTANCE hi,
                           const wchar_t* label,
                           UINT editId, UINT sldId,
                           int x, int y, int rowW,
                           int sldMin, int sldMax, int sldInit)
{
    const int LBL_W = 90;
    const int EDT_W = 52;
    const int GAP   = 6;
    const int ROW_H = 22;
    const int SLD_W = rowW - LBL_W - EDT_W - GAP;

    
    CreateWindowW(L"STATIC", label, WS_CHILD | WS_VISIBLE | SS_RIGHT | SS_CENTERIMAGE,
        x, y, LBL_W, ROW_H, p, nullptr, hi, nullptr);

    
    HWND edit = CreateWindowW(L"EDIT", nullptr,
        WS_CHILD | WS_VISIBLE | WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL,
        x + LBL_W + GAP, y, EDT_W, ROW_H,
        p, reinterpret_cast<HMENU>((UINT_PTR)editId), hi, nullptr);
    (void)edit;

    
    HWND sld = CreateWindowW(TRACKBAR_CLASSW, nullptr,
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | TBS_HORZ | TBS_NOTICKS,
        x + LBL_W + GAP + EDT_W + GAP, y - 2, SLD_W, ROW_H + 4,
        p, reinterpret_cast<HMENU>((UINT_PTR)sldId), hi, nullptr);
    SendMessageW(sld, TBM_SETRANGE, TRUE, MAKELPARAM(sldMin, sldMax));
    SendMessageW(sld, TBM_SETPOS,   TRUE, sldInit);

    return sld;
}

static void ShowSettingsWindow(HWND parent)
{
    if (g_settingsWnd) { SetForegroundWindow(g_settingsWnd); return; }

    
    
    
    g_settingsWnd = CreateDialogW(
        GetModuleHandleW(nullptr),
        MAKEINTRESOURCEW(IDD_SETTINGS),
        parent,
        SettingsDlgProc);
    if (!g_settingsWnd) return;

    ShowWindow(g_settingsWnd, SW_SHOW);
    UpdateWindow(g_settingsWnd);
}

static void AddTrayIcon(HWND hwnd)
{
    g_nid.cbSize           = sizeof(g_nid);
    g_nid.hWnd             = hwnd;
    g_nid.uID              = 1;
    g_nid.uFlags           = NIF_ICON | NIF_TIP | NIF_MESSAGE;
    g_nid.uCallbackMessage = WM_TRAY;
    g_nid.hIcon            = (HICON)LoadImageW(GetModuleHandleW(nullptr),
                                 MAKEINTRESOURCEW(IDI_APPICON), IMAGE_ICON,
                                 GetSystemMetrics(SM_CXSMICON),
                                 GetSystemMetrics(SM_CYSMICON),
                                 LR_DEFAULTCOLOR);

    wcscpy_s(g_nid.szTip, L"GyroScroll");
    Shell_NotifyIconW(NIM_ADD, &g_nid);
}

static void RemoveTrayIcon()
{
    Shell_NotifyIconW(NIM_DELETE, &g_nid);
}

static void ShowContextMenu(HWND hwnd)
{
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, IDM_ABOUT,    L"&GyroScroll...");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, IDM_SETTINGS, L"&Settings...");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, IDM_QUIT,     L"&Quit");
    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(hwnd);   
    TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN,
                   pt.x, pt.y, 0, hwnd, nullptr);
    DestroyMenu(menu);
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE:
        BuildTouchpadList(hwnd);
        AddTrayIcon(hwnd);
        SetTimer(hwnd, SCROLL_TIMER_ID, SCROLL_TIMER_MS, nullptr);
        return 0;

    case WM_TIMER:
        if (wp == SCROLL_TIMER_ID && g_mode != ScrollMode::IDLE) {
            bool isV = (g_mode == ScrollMode::SCROLL_V);
            int units = (int)(g_accumScroll * WHEEL_DELTA);
            if (units != 0) {
                SendScrollUnits(units, isV);
                g_accumScroll -= (float)units / WHEEL_DELTA;
            }
        }
        return 0;

    case WM_INPUT:
        
        
        ProcessRawInput(reinterpret_cast<HRAWINPUT>(lp));
        return DefWindowProcW(hwnd, msg, wp, lp);

    case WM_TRAY:
        if (lp == WM_RBUTTONUP || lp == WM_CONTEXTMENU)
            ShowContextMenu(hwnd);
        if (lp == WM_LBUTTONDBLCLK)
            ShowSettingsWindow(hwnd);
        return 0;

    case WM_COMMAND:
        if (LOWORD(wp) == IDM_ABOUT)    ShowAboutBox(hwnd);
        if (LOWORD(wp) == IDM_SETTINGS) ShowSettingsWindow(hwnd);
        if (LOWORD(wp) == IDM_QUIT) {
            SaveSettings();
            RemoveTrayIcon();
            PostQuitMessage(0);
        }
        return 0;

    case WM_DESTROY:
        KillTimer(hwnd, SCROLL_TIMER_ID);
        StopScrollFreeze();
        SaveSettings();
        RemoveTrayIcon();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPSTR, int)
{
    
    HANDLE mutex = CreateMutexW(nullptr, TRUE, L"GyroScrollMutex_v1");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        MessageBoxW(nullptr, L"GyroScroll is already running.", L"GyroScroll",
                    MB_ICONINFORMATION | MB_OK);
        return 0;
    }

    
    wchar_t exePath[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    g_iniPath = exePath;
    auto slash = g_iniPath.find_last_of(L"\\/");
    g_iniPath  = (slash != std::wstring::npos ? g_iniPath.substr(0, slash + 1) : L"")
                 + L"GyroScroll.ini";
    LoadSettings();

    
    
    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_BAR_CLASSES | ICC_LINK_CLASS };
    InitCommonControlsEx(&icc);

    WNDCLASSW wc     = {};
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInst;
    wc.hIcon         = LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDI_APPICON));
    wc.lpszClassName = L"GyroScrollWnd_v1";
    if (!RegisterClassW(&wc)) {
        MessageBoxW(nullptr, L"Failed to register window class.", L"GyroScroll", MB_ICONERROR);
        return 1;
    }

    
    HWND hwnd = CreateWindowW(L"GyroScrollWnd_v1", L"GyroScroll",
                               0, 0, 0, 0, 0,
                               HWND_MESSAGE, nullptr, hInst, nullptr);
    if (!hwnd) {
        MessageBoxW(nullptr, L"Failed to create message window.", L"GyroScroll", MB_ICONERROR);
        return 1;
    }

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        
        
        if (g_settingsWnd && IsDialogMessageW(g_settingsWnd, &msg))
            continue;
        if (g_aboutWnd && IsDialogMessageW(g_aboutWnd, &msg))
            continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    CloseHandle(mutex);
    return (int)msg.wParam;
}