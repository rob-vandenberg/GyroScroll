/*
 * GyroScroll - Lightweight dependency-free chiral touchpad scrolling for Windows
 *
 * Uses Windows Raw HID input to read touchpad finger coordinates directly.
 * No wxWidgets, no abseil — only the Windows SDK (user32, hid, setupapi, shell32).
 *
 * ─── BUILD ────────────────────────────────────────────────────────────────────
 *
 * MSVC (from a Developer Command Prompt):
 *   rc.exe GyroScroll.rc
 *   cl /O2 /EHsc GyroScroll.cpp GyroScroll.res /link user32.lib hid.lib setupapi.lib shell32.lib comctl32.lib gdi32.lib advapi32.lib
 *
 * MinGW-w64:
 *   windres GyroScroll.rc -o GyroScroll.res
 *   g++ -O2 -mwindows -o GyroScroll.exe GyroScroll.cpp GyroScroll.res \
 *       -luser32 -lhid -lsetupapi -lshell32 -lcomctl32 -lgdi32
 *
 * ─── HOW CHIRAL SCROLLING WORKS ──────────────────────────────────────────────
 *
 * Instead of tracking raw pixel deltas (which requires reversal at the top/bottom),
 * we track the ANGLE of the finger relative to the touchpad centre.
 *
 *   ┌──────────────────────────┐
 *   │                    │░░░│  ← right edge zone (vertical scroll)
 *   │         centre     │░░░│
 *   │           ×        │░░░│
 *   │                    │░░░│
 *   │░░░░░░░░░░░░░░░░░░░░░░░░│  ← bottom edge zone (horizontal scroll)
 *   └──────────────────────────┘
 *
 * When a touch begins in an edge zone we record the angle atan2(y-½, x-½).
 * Each subsequent report gives a new angle; the angular delta drives scrolling.
 * Because we track angle, the user can circle the finger continuously without
 * ever needing to lift — that is the chiral part.
 *
 * ─── SETTINGS (GyroScroll.ini, beside the exe) ─────────────────────────────
 *
 *   [GyroScroll]
 *   EdgeRight=0.08        ; right edge zone width as fraction of pad width
 *   EdgeBottom=0.08       ; bottom edge zone height as fraction of pad height
 *   SpeedV=20.0           ; vertical scroll clicks per full (360°) rotation
 *   SpeedH=20.0           ; horizontal scroll clicks per full rotation
 *   NaturalV=0            ; 1 = natural (reverse) vertical scrolling
 *   NaturalH=0            ; 1 = natural (reverse) horizontal scrolling
 *
 * ─── KNOWN LIMITATIONS ───────────────────────────────────────────────────────
 *
 * - Targets Windows Precision Touchpad (WPT) devices. Older Synaptics HID
 *   touchpads may use a different report descriptor layout.
 * - Logical coordinate ranges are assumed unsigned (LogicalMin = 0). Devices
 *   that report signed logical ranges will need the sign-extension workaround
 *   described in the ParseContacts() comments below.
 * - Tested structure against the HID descriptor in the WPT spec (v1.0).
 *   Some third-party firmware quirks may require minor adjustments.
 *
 * MIT License 2025
 */

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>
#include <commctrl.h>
#include <hidsdi.h>  // also pulls in hidpi.h on most SDK versions
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

// ─── Version ──────────────────────────────────────────────────────────────────
#define VERSION_MAJOR  0
#define VERSION_MINOR  4
#define VERSION_PATCH  5
#define VERSION_STRING "0.4.5"
#ifdef _WIN64
    #define BITNESS_STRING "64-bit"
#else
    #define BITNESS_STRING "32-bit"
#endif


#define IDI_APPICON 1


// ═════════════════════════════════════════════════════════════════════════════
// HID usage constants (Digitizer usage page 0x0D + Generic Desktop 0x01)
// ═════════════════════════════════════════════════════════════════════════════

static constexpr USAGE UP_GENERIC   = 0x01;   // Generic Desktop Controls
static constexpr USAGE UP_DIGITIZER = 0x0D;   // Digitizer Devices
static constexpr USAGE U_TOUCHPAD   = 0x05;   // Touchpad (application collection)
static constexpr USAGE U_TIP_SWITCH = 0x42;   // Tip Switch (button: finger touching)
static constexpr USAGE U_CONTACT_ID = 0x51;   // Contact Identifier
static constexpr USAGE U_CONTACT_CNT= 0x54;   // Contact Count
static constexpr USAGE U_X          = 0x30;   // X coordinate
static constexpr USAGE U_Y          = 0x31;   // Y coordinate

// ═════════════════════════════════════════════════════════════════════════════
// Settings  (loaded from / saved to GyroScroll.ini beside the .exe)
// ═════════════════════════════════════════════════════════════════════════════

static float g_edgeRight  = 0.08f;   // right edge zone, fraction of pad width
static float g_edgeBottom = 0.08f;   // bottom edge zone, fraction of pad height
static float g_speedV     = 20.0f;   // wheel clicks per full (360°) rotation, vertical
static float g_speedH     = 20.0f;   // wheel clicks per full rotation, horizontal
static bool  g_naturalV   = false;   // reverse vertical scroll direction
static bool  g_naturalH   = false;   // reverse horizontal scroll direction

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
    // Edge zones stored as integers in INI (e.g. 8 = 0.08)
    g_edgeRight  = rf(L"EdgeRight",  g_edgeRight  * 100.f) / 100.f;
    g_edgeBottom = rf(L"EdgeBottom", g_edgeBottom * 100.f) / 100.f;
    g_speedV     = rf(L"SpeedV",     g_speedV);
    g_speedH     = rf(L"SpeedH",     g_speedH);
    g_naturalV   = rb(L"NaturalV",   g_naturalV);
    g_naturalH   = rb(L"NaturalH",   g_naturalH);
}

static void SaveSettings()
{
    const wchar_t* S = L"GyroScroll";
    auto wf = [&](const wchar_t* k, float v) {
        WritePrivateProfileStringW(S, k, std::to_wstring(v).c_str(), g_iniPath.c_str());
    };
    auto wb = [&](const wchar_t* k, bool v) {
        WritePrivateProfileStringW(S, k, v ? L"1" : L"0", g_iniPath.c_str());
    };
    // Edge zones stored as integers in INI (e.g. 0.08 → 8)
    wf(L"EdgeRight",  (float)(int)(g_edgeRight  * 100.f + 0.5f));
    wf(L"EdgeBottom", (float)(int)(g_edgeBottom * 100.f + 0.5f));
    wf(L"SpeedV",     g_speedV);
    wf(L"SpeedH",     g_speedH);
    wb(L"NaturalV",   g_naturalV);
    wb(L"NaturalH",   g_naturalH);
}

// ═════════════════════════════════════════════════════════════════════════════
// HID Touchpad discovery
//
// A Windows Precision Touchpad HID report has roughly this structure:
//
//   Collection(Application, UsagePage=Digitizer, Usage=Touchpad)
//     Value: ContactCount  (UP=Digitizer, Usage=0x54)  — how many fingers active
//     Collection(Logical, Usage=Finger) × N  — one per finger slot
//       Button: TipSwitch  (UP=Digitizer, Usage=0x42) — 1 = finger down
//       Value:  ContactID  (UP=Digitizer, Usage=0x51) — finger identifier
//       Value:  X          (UP=GenericDesktop, Usage=0x30)
//       Value:  Y          (UP=GenericDesktop, Usage=0x31)
//
// Each "Collection(Logical)" becomes a distinct LinkCollection in the parsed
// HID caps. We enumerate all value caps, group by LinkCollection, and build
// one FingerSlot per group that contains both X and Y.
// ═════════════════════════════════════════════════════════════════════════════

struct FingerSlot {
    USHORT link;                    // HID link collection index
    LONG   xMin, xMax;             // logical coordinate range for X
    LONG   yMin, yMax;             // logical coordinate range for Y
};

struct Touchpad {
    HANDLE               devHandle  = nullptr;
    PHIDP_PREPARSED_DATA preparsed  = nullptr;
    HIDP_CAPS            hidCaps    = {};
    std::vector<FingerSlot> slots;  // finger link collections (max contacts)
    bool   hasContactCount = false;
    USHORT contactCountLink = 0;
    // Unified logical range (used for normalisation; typically all slots share these)
    LONG xMin = 0, xMax = 1;
    LONG yMin = 0, yMax = 1;
};

static std::vector<Touchpad> g_pads;

static void BuildTouchpadList(HWND hwnd)
{
    // Free old preparsed data buffers
    for (auto& tp : g_pads)
        if (tp.preparsed) HidD_FreePreparsedData(tp.preparsed);
    g_pads.clear();

    // Enumerate raw input devices
    UINT n = 0;
    GetRawInputDeviceList(nullptr, &n, sizeof(RAWINPUTDEVICELIST));
    if (!n) return;

    std::vector<RAWINPUTDEVICELIST> devs(n);
    GetRawInputDeviceList(devs.data(), &n, sizeof(RAWINPUTDEVICELIST));

    for (auto& d : devs) {
        if (d.dwType != RIM_TYPEHID) continue;

        // Quick usage-page/usage check
        RID_DEVICE_INFO info = {};
        info.cbSize = sizeof(info);
        UINT sz = sizeof(info);
        if (GetRawInputDeviceInfoW(d.hDevice, RIDI_DEVICEINFO, &info, &sz) == (UINT)-1)
            continue;
        if (info.hid.usUsagePage != UP_DIGITIZER || info.hid.usUsage != U_TOUCHPAD)
            continue;

        // Fetch preparsed data
        UINT psz = 0;
        GetRawInputDeviceInfoW(d.hDevice, RIDI_PREPARSEDDATA, nullptr, &psz);
        if (!psz) continue;

        std::vector<BYTE> pdata(psz);
        GetRawInputDeviceInfoW(d.hDevice, RIDI_PREPARSEDDATA, pdata.data(), &psz);

        Touchpad tp;
        tp.devHandle = d.hDevice;
        // Allocate persistent buffer (lives for the process lifetime)
        tp.preparsed = reinterpret_cast<PHIDP_PREPARSED_DATA>(new BYTE[psz]);
        std::memcpy(tp.preparsed, pdata.data(), psz);

        if (HidP_GetCaps(tp.preparsed, &tp.hidCaps) != HIDP_STATUS_SUCCESS) {
            delete[] reinterpret_cast<BYTE*>(tp.preparsed);
            continue;
        }

        // Parse value caps → discover finger slots
        USHORT nv = tp.hidCaps.NumberInputValueCaps;
        std::vector<HIDP_VALUE_CAPS> vc(nv);
        HidP_GetValueCaps(HidP_Input, vc.data(), &nv, tp.preparsed);

        struct SlotBuilder {
            bool hasX = false, hasY = false;
            LONG xMin = 0, xMax = 0, yMin = 0, yMax = 0;
        };
        std::map<USHORT, SlotBuilder> builders;

        for (auto& cap : vc) {
            // Some drivers pack multiple usages as a range; handle both cases.
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

    // Register for raw input from all touchpads, even when not in focus
    if (!g_pads.empty()) {
        RAWINPUTDEVICE rid = {};
        rid.usUsagePage = UP_DIGITIZER;
        rid.usUsage     = U_TOUCHPAD;
        rid.dwFlags     = RIDEV_INPUTSINK;
        rid.hwndTarget  = hwnd;
        RegisterRawInputDevices(&rid, 1, sizeof(rid));
    }
}

// ═════════════════════════════════════════════════════════════════════════════
// Contact parsing
//
// NOTE on sign extension: HidP_GetUsageValue always returns ULONG. If your
// device uses a signed logical range (LogicalMin < 0) the returned bits are
// the two's-complement representation in the HID bit-width.  You would need
// to sign-extend using the bit-width from the value cap's BitSize field.
// Standard WPT devices use LogicalMin = 0, so we skip that here.
// ═════════════════════════════════════════════════════════════════════════════

struct Contact {
    ULONG id;
    float x, y;  // normalised to [0, 1]
    bool  tip;   // true = finger is touching
};

static std::vector<Contact> ParseContacts(const Touchpad& tp,
                                           const BYTE* report, UINT len)
{
    std::vector<Contact> out;

    // Button caps for tip switch
    USHORT nb = tp.hidCaps.NumberInputButtonCaps;
    std::vector<HIDP_BUTTON_CAPS> bc(nb ? nb : 1);
    if (nb) HidP_GetButtonCaps(HidP_Input, bc.data(), &nb, tp.preparsed);

    // Active contact count
    int maxSlots    = (int)tp.slots.size();
    int activeCount = maxSlots;
    if (tp.hasContactCount) {
        ULONG cnt = 0;
        // Contact count lives at link collection 0 (root) on most WPT devices.
        // Use contactCountLink rather than hard-coding 0 in case it differs.
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

        // Clamp to [0, 1] in case of slightly out-of-range values
        c.x = std::max(0.f, std::min(1.f, c.x));
        c.y = std::max(0.f, std::min(1.f, c.y));

        if (HidP_GetUsageValue(HidP_Input, UP_DIGITIZER, slot.link,
                U_CONTACT_ID, &idv, tp.preparsed,
                reinterpret_cast<PCHAR>(const_cast<BYTE*>(report)), len)
                == HIDP_STATUS_SUCCESS)
            c.id = idv;

        // Tip switch (a Button cap, not a Value cap)
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

// ═════════════════════════════════════════════════════════════════════════════
// Chiral scroll state machine
//
//
//  • Scroll amount per report = distance_moved × sensitivity
//    (pure linear distance, not angular rotation)
//  • Scroll direction is a ±1 flag, not a geometry computation
//  • Three deadzones gate noise without any ring-buffer smoothing:
//      startDeadzone  — must move this far before scrolling begins
//      moveDeadzone   — minimum projected movement to continue scrolling
//      reverseDeadzone — must move this far backward to flip direction
//  • Cursor freeze via ClipCursor (replaces WH_MOUSE_LL to avoid W11 lag)
//
// Positions are stored normalised 0..1 (same units as g_edgeRight/Bottom).
// Deadzones are also in normalised units (original author used hw/1784;
// we use equivalent fractions derived from the same calibration).
//
// State:  IDLE → SCROLL_V / SCROLL_H on edge touch → back to IDLE on lift.
// ═════════════════════════════════════════════════════════════════════════════

enum class ScrollMode { IDLE, SCROLL_V, SCROLL_H };

static ScrollMode g_mode       = ScrollMode::IDLE;
static ULONG      g_trackId    = 0xFFFFFFFF;

// Per-session scroll state (reset on each new session)
static float g_posX        = 0.f;  // last known finger position (normalised)
static float g_posY        = 0.f;
static float g_dirX        = 0.f;  // current movement direction (unit vector)
static float g_dirY        = 0.f;
static float g_scrollDir   = 0.f;  // ±1 once established, 0 = not yet set
static float g_accumScroll = 0.f;  // fractional wheel-click accumulator

// Deadzone constants (normalised, derived from original's hw/1784 baseline).
// startDeadzone & moveDeadzone = 10/1784 ≈ 0.0056
// reverseDeadzone              = 20/1784 ≈ 0.0112
// startDeadzoneAngle           = π/4 (45°) — cone within which start is allowed
// reverseDeadzoneAngle         = π   (180°) — effectively any backward movement
static constexpr float DZ_START         = 0.0056f;
static constexpr float DZ_MOVE          = 0.0056f;
static constexpr float DZ_REVERSE       = 0.0112f;
static constexpr float DZ_START_ANGLE   = 3.14159f / 4.0f;  // 45°
static constexpr float DZ_REVERSE_ANGLE = 3.14159f;         // 180°

// Cursor freeze via ClipCursor — confines cursor to a 1×1 pixel rectangle
// at its current position while a scroll session is active, preventing drift.
// This avoids WH_MOUSE_LL which causes severe lag on Windows 11 due to its
// strict low-level hook timeout enforcement at high touchpad report rates.

static constexpr UINT SCROLL_TIMER_ID = 1;
static constexpr UINT SCROLL_TIMER_MS = 16;  // ~60Hz flush rate

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

// Unsigned angle between two vectors. Returns π/2 if either vector is near-zero
// (safe fallback: caller's condition won't trigger on a zero-length movement).
static float AngleBetween(float ax, float ay, float bx, float by)
{
    float normA = std::sqrt(ax*ax + ay*ay);
    float normB = std::sqrt(bx*bx + by*by);
    if (normA < 1e-7f || normB < 1e-7f) return 1.5707963f;  // π/2
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
    // ── IDLE: look for a new edge touch ───────────────────────────────────
    if (g_mode == ScrollMode::IDLE) {
        for (const auto& c : contacts) {
            if (!c.tip) continue;
            bool inRight  = c.x >= (1.f - g_edgeRight);
            bool inBottom = c.y >= (1.f - g_edgeBottom);
            if (inRight && !inBottom) {
                g_mode       = ScrollMode::SCROLL_V;
                g_trackId    = c.id;
                g_posX       = c.x;  g_posY = c.y;
                // Initial canonical direction: downward (0,+1); sens negated
                // so upward movement = positive scroll (scroll up).
                g_dirX       = 0.f;  g_dirY = 1.f;
                g_scrollDir  = 0.f;
                g_accumScroll= 0.f;
                StartScrollFreeze();
                return;
            }
            if (inBottom && !inRight) {
                g_mode       = ScrollMode::SCROLL_H;
                g_trackId    = c.id;
                g_posX       = c.x;  g_posY = c.y;
                // Initial canonical direction: rightward (+1,0)
                g_dirX       = 1.f;  g_dirY = 0.f;
                g_scrollDir  = 0.f;
                g_accumScroll= 0.f;
                StartScrollFreeze();
                return;
            }
        }
        return;
    }

    // ── SCROLLING: find our tracked finger ────────────────────────────────
    const Contact* tr = nullptr;
    for (const auto& c : contacts)
        if (c.id == g_trackId) { tr = &c; break; }

    if (!tr || !tr->tip) {
        // Finger lifted — flush accumulator immediately on release
        if (g_accumScroll != 0.f) {
            int units = (int)(g_accumScroll * WHEEL_DELTA);
            if (units) SendScrollUnits(units, g_mode == ScrollMode::SCROLL_V);
        }
        g_mode       = ScrollMode::IDLE;
        g_trackId    = 0xFFFFFFFF;
        g_accumScroll= 0.f;
        StopScrollFreeze();
        return;
    }

    // Movement vector from last position
    float nx = tr->x - g_posX;
    float ny = tr->y - g_posY;
    float dist = std::sqrt(nx*nx + ny*ny);

    if (g_scrollDir == 0.f) {
        // ── Phase 1: waiting for initial direction to be established ──────
        // The finger must move startDeadzone in the canonical direction (or
        // exactly opposite) within a ±startDeadzoneAngle/2 cone.
        float dot = nx*g_dirX + ny*g_dirY;
        if (AngleBetween(g_dirX, g_dirY, nx, ny) < DZ_START_ANGLE/2.f && dot > DZ_START) {
            g_scrollDir = 1.f;
        } else if (AngleBetween(g_dirX, g_dirY, -nx, -ny) < DZ_START_ANGLE/2.f && dot < -DZ_START) {
            g_scrollDir = -1.f;
        } else {
            return;  // still waiting
        }
        // CRITICAL: update direction to actual movement now, before Phase 2's
        // reversal check runs. Without this, Phase 2 sees the original canonical
        // direction (0,1) and immediately mis-detects the first upward stroke
        // as a "reversal", flipping scrollDir back and locking to one direction.
        if (dist > 1e-6f) { g_dirX = nx / dist; g_dirY = ny / dist; }
    }

    // ── Phase 2: scrolling is active ─────────────────────────────────────

    // Check for direction reversal: movement must be backward within the
    // reverseDeadzoneAngle cone AND exceed reverseDeadzone distance.
    if (AngleBetween(g_dirX, g_dirY, -nx, -ny) < DZ_REVERSE_ANGLE/2.f) {
        if (dist > DZ_REVERSE) {
            g_scrollDir *= -1.f;
            // fall through — scroll with new direction, then update state
        } else {
            // Moving backward but not far enough yet — ignore
            return;
        }
    } else if (dist <= DZ_MOVE && nx*g_dirX + ny*g_dirY <= DZ_MOVE) {
        // Not reversing AND not moving enough forward — ignore this report
        return;
    }

    // Scroll amount = distance × speed × scrollDir (matches original exactly).
    // g_speedV/H = clicks per full pad traversal; dist is normalised 0..1.
    // Vertical uses negative speed so that downward finger = downward scroll
    // (mirrors the original's -vSens passed to ScrollSession).
    bool isV = (g_mode == ScrollMode::SCROLL_V);
    float speed  = isV ? -g_speedV : g_speedH;
    bool natural = isV ? g_naturalV : g_naturalH;

    float clicks = g_scrollDir * dist * speed;
    if (natural) clicks = -clicks;

    // Accumulate — timer flushes at ~60Hz
    g_accumScroll += clicks;

    // Update position and direction (unit vector of actual movement)
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

    // Match to one of our registered touchpads
    Touchpad* tp = nullptr;
    for (auto& p : g_pads)
        if (p.devHandle == raw->header.hDevice) { tp = &p; break; }
    if (!tp) return;

    // A single WM_INPUT can pack multiple HID reports (dwCount > 1)
    UINT reportSize  = raw->data.hid.dwSizeHid;
    UINT reportCount = raw->data.hid.dwCount;
    const BYTE* base = raw->data.hid.bRawData;

    for (UINT r = 0; r < reportCount; ++r) {
        auto contacts = ParseContacts(*tp, base + r * reportSize, reportSize);
        if (!contacts.empty()) OnContacts(contacts);
    }
}

// ═════════════════════════════════════════════════════════════════════════════
// System tray
// ═════════════════════════════════════════════════════════════════════════════

static NOTIFYICONDATAW g_nid    = {};
static constexpr UINT  WM_TRAY      = WM_USER + 1;
static constexpr UINT  IDM_QUIT     = 1001;
static constexpr UINT  IDM_SETTINGS = 1002;
static constexpr UINT  IDM_ABOUT    = 1003;

static void ShowAboutBox(HWND hwnd)
{
    wchar_t header[64];
    swprintf(header, 64, L"GyroScroll %hs (%hs)", VERSION_STRING, BITNESS_STRING);
    std::wstring msg =
        std::wstring(header) +
        L"\n"
        L"\n"
        L"Chiral scrolling for Precision Touchpads in Windows 10/11\n"
        L"\n"
        L"Move your finger along the right or bottom edge of your touchpad and make a circular motion for continuous scrolling, without lifting.\n"
        L"\n"
        L"Copyright \u00A9 2025 Rob Vandenberg\n"
        L"\n"
        L"https://github.com/rob-vandenberg/gyroscroll";
    MessageBoxW(hwnd, msg.c_str(), L"About GyroScroll", MB_OK | MB_ICONINFORMATION);
}

// Settings dialog control IDs
static constexpr UINT IDC_EDGE_RIGHT    = 2001;  // edit
static constexpr UINT IDC_EDGE_BOTTOM   = 2002;  // edit
static constexpr UINT IDC_SPEED_V       = 2003;  // edit
static constexpr UINT IDC_SPEED_H       = 2004;  // edit
static constexpr UINT IDC_NATURAL_V     = 2005;  // checkbox
static constexpr UINT IDC_NATURAL_H     = 2006;  // checkbox
static constexpr UINT IDC_OK            = 2007;
static constexpr UINT IDC_CANCEL        = 2008;
static constexpr UINT IDC_PREVIEW       = 2009;  // owner-drawn preview panel
static constexpr UINT IDC_SLD_EDGE_R    = 2010;  // slider for right edge
static constexpr UINT IDC_SLD_EDGE_B    = 2011;  // slider for bottom edge
static constexpr UINT IDC_SLD_SPEED_V   = 2012;  // slider for vertical speed
static constexpr UINT IDC_SLD_SPEED_H   = 2013;  // slider for horizontal speed
static constexpr UINT IDC_AUTOSTART     = 2014;  // checkbox

static HWND g_settingsWnd = nullptr;

// Prevent re-entrant edit↔slider sync
static bool g_syncLock = false;

// ─── Autostart registry helpers ───────────────────────────────────────────────
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

static LRESULT CALLBACK SettingsWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
static void ShowSettingsWindow(HWND parent);

// ═════════════════════════════════════════════════════════════════════════════
// Settings window
//
// Layout:
//   ┌──────────────────────────────────────────────────────┐
//   │  [Large touchpad preview]   Edge zones               │
//   │                             Right:   [0.08][======]  │
//   │                             Bottom:  [0.08][======]  │
//   │                                                      │
//   │                             Scroll speed             │
//   │                             Vertical:  [20][======]  │
//   │                             Horizontal:[20][======]  │
//   │                                                      │
//   │                             Natural scroll           │
//   │                             [x] Reverse vertical     │
//   │                             [x] Reverse horizontal   │
//   │                                                      │
//   │  Blue  = vertical zone                  [OK][Cancel] │
//   │  Green = horizontal zone                             │
//   └──────────────────────────────────────────────────────┘
//
// Sliders and edit boxes are kept in sync bidirectionally.
// The preview redraws live whenever an edge zone value changes.
// ═════════════════════════════════════════════════════════════════════════════

// ── Helpers ───────────────────────────────────────────────────────────────────

// Edge zone: stored as fraction (0.01–0.30), displayed/slid as integer 1–30 (percent)
static int   EdgeToSlider(float v) { return std::max(1, std::min(30, (int)(v * 100.f + 0.5f))); }
static float SliderToEdge(int pos) { return pos / 100.f; }

// Speed: stored and displayed as integer 1–40
static int   SpeedToSlider(float v) { return std::max(1, std::min(40, (int)(v + 0.5f))); }
static float SliderToSpeed(int pos) { return (float)pos; }

static float GetEditFloat(HWND dlg, UINT id, float fallback)
{
    wchar_t buf[32] = {};
    GetDlgItemTextW(dlg, id, buf, 32);
    try { return std::stof(buf); } catch (...) { return fallback; }
}

// Format edge value as integer percent (e.g. 0.08 → "8")
static void SetEditEdge(HWND dlg, UINT id, float v)
{
    wchar_t buf[16];
    swprintf_s(buf, L"%d", EdgeToSlider(v));
    SetDlgItemTextW(dlg, id, buf);
}

// Format speed value as plain integer
static void SetEditSpeed(HWND dlg, UINT id, float v)
{
    wchar_t buf[16];
    swprintf_s(buf, L"%d", (int)(v + 0.5f));
    SetDlgItemTextW(dlg, id, buf);
}

static void SetSliderPos(HWND dlg, UINT id, int pos)
{
    SendDlgItemMessageW(dlg, id, TBM_SETPOS, TRUE, pos);
}

// Trigger a preview repaint with fresh values from the edge edits
static void RefreshPreview(HWND hwnd)
{
    // Edge edits contain integer percent values (e.g. "8" = 0.08)
    float r = GetEditFloat(hwnd, IDC_EDGE_RIGHT,  g_edgeRight  * 100.f) / 100.f;
    float b = GetEditFloat(hwnd, IDC_EDGE_BOTTOM, g_edgeBottom * 100.f) / 100.f;
    r = std::max(0.01f, std::min(0.30f, r));
    b = std::max(0.01f, std::min(0.30f, b));
    HWND prev = GetDlgItem(hwnd, IDC_PREVIEW);
    // Pack two 16-bit fixed-point values into LONG_PTR (×10000 for precision)
    SetWindowLongPtrW(prev, GWLP_USERDATA,
        (LONG_PTR)(((DWORD)(WORD)(int)(r * 10000.f)) |
                   ((DWORD)(WORD)(int)(b * 10000.f) << 16)));
    InvalidateRect(prev, nullptr, TRUE);
}

// Draw the touchpad preview panel
static void DrawPreview(HWND panel, float edgeRight, float edgeBottom)
{
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(panel, &ps);

    RECT rc;
    GetClientRect(panel, &rc);
    int W = rc.right  - rc.left;
    int H = rc.bottom - rc.top;

    // Rounded rectangle background (pad surface)
    HBRUSH padBrush = CreateSolidBrush(RGB(210, 210, 220));
    FillRect(dc, &rc, padBrush);
    DeleteObject(padBrush);

    // Use the same base (shorter dimension) for both zones so that equal
    // percentage values produce visually equal bar sizes in the preview.
    int base = std::min(W, H);

    // Right edge zone (blue — vertical scroll)
    {
        int zw = std::max(2, (int)(edgeRight * base));
        RECT zr = { W - zw, 0, W, H };
        HBRUSH zb = CreateSolidBrush(RGB(80, 140, 255));
        FillRect(dc, &zr, zb);
        DeleteObject(zb);
    }

    // Bottom edge zone (green — horizontal scroll)
    {
        int zh = std::max(2, (int)(edgeBottom * base));
        RECT zr = { 0, H - zh, W, H };
        HBRUSH zb = CreateSolidBrush(RGB(60, 200, 120));
        FillRect(dc, &zr, zb);
        DeleteObject(zb);
    }

    // Border
    HPEN pen = CreatePen(PS_SOLID, 1, RGB(140, 140, 155));
    SelectObject(dc, pen);
    SelectObject(dc, GetStockObject(NULL_BRUSH));
    RoundRect(dc, rc.left, rc.top, rc.right - 1, rc.bottom - 1, 10, 10);
    DeleteObject(pen);

    EndPaint(panel, &ps);
}

// Preview subclass: handle WM_PAINT
static LRESULT CALLBACK PreviewSubclassProc(HWND hwnd, UINT msg,
                                             WPARAM wp, LPARAM lp,
                                             UINT_PTR, DWORD_PTR)
{
    if (msg == WM_PAINT) {
        LONG_PTR packed = GetWindowLongPtrW(hwnd, GWLP_USERDATA);
        float r = (float)(LOWORD(packed)) / 10000.f;
        float b = (float)(HIWORD(packed)) / 10000.f;
        if (r < 0.01f) r = g_edgeRight;
        if (b < 0.01f) b = g_edgeBottom;
        DrawPreview(hwnd, r, b);
        return 0;
    }
    return DefSubclassProc(hwnd, msg, wp, lp);
}

// Settings window procedure
static LRESULT CALLBACK SettingsWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg)
    {
    // ── Slider moved ─────────────────────────────────────────────────────────
    case WM_HSCROLL:
    {
        if (g_syncLock) break;
        HWND sldr = (HWND)lp;
        int  pos  = (int)SendMessageW(sldr, TBM_GETPOS, 0, 0);
        int  id   = GetDlgCtrlID(sldr);
        g_syncLock = true;
        if (id == IDC_SLD_EDGE_R)  { SetEditEdge (hwnd, IDC_EDGE_RIGHT,  SliderToEdge (pos)); RefreshPreview(hwnd); }
        if (id == IDC_SLD_EDGE_B)  { SetEditEdge (hwnd, IDC_EDGE_BOTTOM, SliderToEdge (pos)); RefreshPreview(hwnd); }
        if (id == IDC_SLD_SPEED_V) { SetEditSpeed(hwnd, IDC_SPEED_V,     SliderToSpeed(pos)); }
        if (id == IDC_SLD_SPEED_H) { SetEditSpeed(hwnd, IDC_SPEED_H,     SliderToSpeed(pos)); }
        g_syncLock = false;
        break;
    }

    // ── Edit changed ─────────────────────────────────────────────────────────
    case WM_COMMAND:
    {
        UINT id  = LOWORD(wp);
        UINT evt = HIWORD(wp);

        if (evt == EN_CHANGE && !g_syncLock) {
            g_syncLock = true;
            if (id == IDC_EDGE_RIGHT)  { int pct = std::max(1, std::min(30, (int)GetEditFloat(hwnd, id, g_edgeRight  * 100.f))); SetSliderPos(hwnd, IDC_SLD_EDGE_R,  pct); RefreshPreview(hwnd); }
            if (id == IDC_EDGE_BOTTOM) { int pct = std::max(1, std::min(30, (int)GetEditFloat(hwnd, id, g_edgeBottom * 100.f))); SetSliderPos(hwnd, IDC_SLD_EDGE_B,  pct); RefreshPreview(hwnd); }
            if (id == IDC_SPEED_V)     { float v = GetEditFloat(hwnd, id, g_speedV);     SetSliderPos(hwnd, IDC_SLD_SPEED_V, SpeedToSlider(v)); }
            if (id == IDC_SPEED_H)     { float v = GetEditFloat(hwnd, id, g_speedH);     SetSliderPos(hwnd, IDC_SLD_SPEED_H, SpeedToSlider(v)); }
            g_syncLock = false;
        }

        if (id == IDC_OK) {
            // Edge values are entered as integers (percent × 100), e.g. "8" = 0.08
            float r  = GetEditFloat(hwnd, IDC_EDGE_RIGHT,  g_edgeRight  * 100.f) / 100.f;
            float b  = GetEditFloat(hwnd, IDC_EDGE_BOTTOM, g_edgeBottom * 100.f) / 100.f;
            float sv = GetEditFloat(hwnd, IDC_SPEED_V,     g_speedV);
            float sh = GetEditFloat(hwnd, IDC_SPEED_H,     g_speedH);
            r  = std::max(0.01f, std::min(0.30f,  r));
            b  = std::max(0.01f, std::min(0.30f,  b));
            sv = std::max(1.f,   std::min(40.f,  sv));
            sh = std::max(1.f,   std::min(40.f,  sh));
            g_edgeRight  = r;
            g_edgeBottom = b;
            g_speedV     = sv;
            g_speedH     = sh;
            g_naturalV   = (IsDlgButtonChecked(hwnd, IDC_NATURAL_V) == BST_CHECKED);
            g_naturalH   = (IsDlgButtonChecked(hwnd, IDC_NATURAL_H) == BST_CHECKED);
            SetAutostart(IsDlgButtonChecked(hwnd, IDC_AUTOSTART) == BST_CHECKED);
            SaveSettings();
            DestroyWindow(hwnd);
        }
        if (id == IDC_CANCEL) DestroyWindow(hwnd);
        break;
    }

    case WM_KEYDOWN:
        if (wp == VK_ESCAPE) DestroyWindow(hwnd);
        if (wp == VK_RETURN) SendMessageW(hwnd, WM_COMMAND, IDC_OK, 0);
        break;

    case WM_DESTROY:
        g_settingsWnd = nullptr;
        break;

    case WM_CLOSE:
        DestroyWindow(hwnd);
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ── Layout helpers ────────────────────────────────────────────────────────────

static HWND MakeLabel(HWND p, HINSTANCE hi, const wchar_t* txt, int x, int y, int w, int h)
{
    return CreateWindowW(L"STATIC", txt, WS_CHILD | WS_VISIBLE | SS_LEFT,
        x, y, w, h, p, nullptr, hi, nullptr);
}

// One row: label | edit box | trackbar
// Returns the trackbar HWND so the caller can configure its range.
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

    // Label
    CreateWindowW(L"STATIC", label, WS_CHILD | WS_VISIBLE | SS_RIGHT | SS_CENTERIMAGE,
        x, y, LBL_W, ROW_H, p, nullptr, hi, nullptr);

    // Edit
    HWND edit = CreateWindowW(L"EDIT", nullptr,
        WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL,
        x + LBL_W + GAP, y, EDT_W, ROW_H,
        p, reinterpret_cast<HMENU>((UINT_PTR)editId), hi, nullptr);
    (void)edit;

    // Trackbar
    HWND sld = CreateWindowW(TRACKBAR_CLASSW, nullptr,
        WS_CHILD | WS_VISIBLE | TBS_HORZ | TBS_NOTICKS,
        x + LBL_W + GAP + EDT_W + GAP, y - 2, SLD_W, ROW_H + 4,
        p, reinterpret_cast<HMENU>((UINT_PTR)sldId), hi, nullptr);
    SendMessageW(sld, TBM_SETRANGE, TRUE, MAKELPARAM(sldMin, sldMax));
    SendMessageW(sld, TBM_SETPOS,   TRUE, sldInit);

    return sld;
}

static void ShowSettingsWindow(HWND parent)
{
    if (g_settingsWnd) { SetForegroundWindow(g_settingsWnd); return; }

    HINSTANCE hi = GetModuleHandleW(nullptr);

    // Init common controls (needed for TrackBar / Slider)
    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_BAR_CLASSES };
    InitCommonControlsEx(&icc);

    // Register settings window class (once)
    static bool classReg = false;
    if (!classReg) {
        WNDCLASSW wc     = {};
        wc.lpfnWndProc   = SettingsWndProc;
        wc.hInstance     = hi;
        wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
        wc.lpszClassName = L"GyroScrollSettings_v1";
        wc.hCursor       = LoadCursorW(nullptr, reinterpret_cast<LPCWSTR>(IDC_ARROW));
        RegisterClassW(&wc);
        classReg = true;
    }

    // ── Dimensions ────────────────────────────────────────────────────────────
    // All measurements are in CLIENT area pixels.
    // We use AdjustWindowRect to derive the correct outer window size.
    const int PAD    = 14;
    const int PREVW  = 220;                    // 16:10 landscape touchpad shape
    const int PREVH  = 138;                    // 220 * 10/16
    const int COL1   = PAD;
    const int COL2   = COL1 + PREVW + PAD*2;  // right column x
    const int RCOLW  = 290;                    // right column width
    const int CLW    = COL2 + RCOLW + PAD;    // total client width
    const int LH     = 18;                     // section label height
    const int RH     = 30;                     // slider row height

    // Calculate total client height by simulating the layout
    int ry = PAD;
    ry += LH + 4;   // "Edge zones"
    ry += RH;       // right edge row
    ry += RH + 10;  // bottom edge row + gap
    ry += LH + 4;   // "Scroll speed"
    ry += RH;       // vertical row
    ry += RH + 10;  // horizontal row + gap
    ry += LH + 4;   // "Natural scroll"
    ry += LH + 6;   // checkbox V
    ry += LH + 6;   // checkbox H
    const int BTN_H  = 26;
    const int BTN_GAP = 14;
    const int CLH    = ry + BTN_GAP + BTN_H + PAD;  // total client height

    // Outer window size = client size + frame chrome
    RECT adj = { 0, 0, CLW, CLH };
    AdjustWindowRect(&adj, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, FALSE);
    const int WW = adj.right  - adj.left;
    const int WH = adj.bottom - adj.top;

    g_settingsWnd = CreateWindowW(
        L"GyroScrollSettings_v1", L"GyroScroll Settings (GyroScroll.ini)",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
        CW_USEDEFAULT, CW_USEDEFAULT, WW, WH,
        parent, nullptr, hi, nullptr);
    if (!g_settingsWnd) return;

    // ── Preview panel (left) ──────────────────────────────────────────────────
    ry = PAD;  // reset after height calculation above
    HWND prev = CreateWindowW(L"STATIC", nullptr,
        WS_CHILD | WS_VISIBLE | SS_OWNERDRAW,
        COL1, PAD, PREVW, PREVH,
        g_settingsWnd, reinterpret_cast<HMENU>((UINT_PTR)IDC_PREVIEW), hi, nullptr);
    SetWindowSubclass(prev, PreviewSubclassProc, 0, 0);

    // Legend below preview
    MakeLabel(g_settingsWnd, hi,
        L"Blue  = vertical scroll zone\nGreen = horizontal scroll zone",
        COL1, PAD + PREVH + 6, PREVW, LH * 2 + 4);

    // ── Right column ──────────────────────────────────────────────────────────

    // Section: Edge zones
    MakeLabel(g_settingsWnd, hi, L"Edge zones", COL2, ry, RCOLW, LH);
    ry += LH + 4;

    MakeSliderRow(g_settingsWnd, hi, L"Right edge:",
        IDC_EDGE_RIGHT, IDC_SLD_EDGE_R, COL2, ry, RCOLW,
        1, 30, EdgeToSlider(g_edgeRight));
    SetEditEdge(g_settingsWnd, IDC_EDGE_RIGHT, g_edgeRight);
    ry += RH;

    MakeSliderRow(g_settingsWnd, hi, L"Bottom edge:",
        IDC_EDGE_BOTTOM, IDC_SLD_EDGE_B, COL2, ry, RCOLW,
        1, 30, EdgeToSlider(g_edgeBottom));
    SetEditEdge(g_settingsWnd, IDC_EDGE_BOTTOM, g_edgeBottom);
    ry += RH + 10;

    // Section: Scroll speed
    MakeLabel(g_settingsWnd, hi, L"Scroll speed (clicks / swipe)", COL2, ry, RCOLW, LH);
    ry += LH + 4;

    MakeSliderRow(g_settingsWnd, hi, L"Vertical:",
        IDC_SPEED_V, IDC_SLD_SPEED_V, COL2, ry, RCOLW,
        1, 40, SpeedToSlider(g_speedV));
    SetEditSpeed(g_settingsWnd, IDC_SPEED_V, g_speedV);
    ry += RH;

    MakeSliderRow(g_settingsWnd, hi, L"Horizontal:",
        IDC_SPEED_H, IDC_SLD_SPEED_H, COL2, ry, RCOLW,
        1, 40, SpeedToSlider(g_speedH));
    SetEditSpeed(g_settingsWnd, IDC_SPEED_H, g_speedH);
    ry += RH + 10;

    // Section: Natural scroll
    MakeLabel(g_settingsWnd, hi, L"Natural (reversed) scroll", COL2, ry, RCOLW, LH);
    ry += LH + 4;

    CreateWindowW(L"BUTTON", L"Reverse vertical",
        WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
        COL2, ry, RCOLW, LH + 2,
        g_settingsWnd, reinterpret_cast<HMENU>((UINT_PTR)IDC_NATURAL_V), hi, nullptr);
    CheckDlgButton(g_settingsWnd, IDC_NATURAL_V, g_naturalV ? BST_CHECKED : BST_UNCHECKED);
    ry += LH + 6;

    CreateWindowW(L"BUTTON", L"Reverse horizontal",
        WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
        COL2, ry, RCOLW, LH + 2,
        g_settingsWnd, reinterpret_cast<HMENU>((UINT_PTR)IDC_NATURAL_H), hi, nullptr);
    CheckDlgButton(g_settingsWnd, IDC_NATURAL_H, g_naturalH ? BST_CHECKED : BST_UNCHECKED);

    // Section: Startup
    ry += LH + 10;
    MakeLabel(g_settingsWnd, hi, L"Startup", COL2, ry, RCOLW, LH);
    ry += LH + 4;

    CreateWindowW(L"BUTTON", L"Start automatically with Windows",
        WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
        COL2, ry, RCOLW, LH + 2,
        g_settingsWnd, reinterpret_cast<HMENU>((UINT_PTR)IDC_AUTOSTART), hi, nullptr);
    CheckDlgButton(g_settingsWnd, IDC_AUTOSTART, GetAutostart() ? BST_CHECKED : BST_UNCHECKED);

    // ── OK / Cancel ───────────────────────────────────────────────────────────
    const int BTN_W = 82;
    const int BTN_Y = CLH - PAD - BTN_H;
    const int BTN_X = CLW - PAD - BTN_W * 2 - 8;

    CreateWindowW(L"BUTTON", L"OK",
        WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
        BTN_X, BTN_Y, BTN_W, BTN_H,
        g_settingsWnd, reinterpret_cast<HMENU>((UINT_PTR)IDC_OK), hi, nullptr);
    CreateWindowW(L"BUTTON", L"Cancel",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        BTN_X + BTN_W + 8, BTN_Y, BTN_W, BTN_H,
        g_settingsWnd, reinterpret_cast<HMENU>((UINT_PTR)IDC_CANCEL), hi, nullptr);

    // Apply system dialog font to all children
    HFONT font = reinterpret_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
    EnumChildWindows(g_settingsWnd, [](HWND child, LPARAM lp) -> BOOL {
        SendMessageW(child, WM_SETFONT, (WPARAM)lp, TRUE);
        return TRUE;
    }, reinterpret_cast<LPARAM>(font));

    // Centre on screen
    RECT wr;
    GetWindowRect(g_settingsWnd, &wr);
    int sw = GetSystemMetrics(SM_CXSCREEN);
    int sh = GetSystemMetrics(SM_CYSCREEN);
    SetWindowPos(g_settingsWnd, nullptr,
        (sw - (wr.right - wr.left)) / 2,
        (sh - (wr.bottom - wr.top)) / 2,
        0, 0, SWP_NOZORDER | SWP_NOSIZE);

    ShowWindow(g_settingsWnd, SW_SHOW);
    UpdateWindow(g_settingsWnd);
    RefreshPreview(g_settingsWnd);
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

    wcscpy_s(g_nid.szTip, L"GyroScroll — right/bottom edge to scroll");
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
    SetForegroundWindow(hwnd);   // required for TrackPopupMenu to close correctly
    TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN,
                   pt.x, pt.y, 0, hwnd, nullptr);
    DestroyMenu(menu);
}

// ═════════════════════════════════════════════════════════════════════════════
// Window procedure and entry point
// ═════════════════════════════════════════════════════════════════════════════

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
        // Process every message so direction tracking sees the full finger path.
        // Scroll output is throttled separately via WM_TIMER.
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
    // Prevent multiple instances
    HANDLE mutex = CreateMutexW(nullptr, TRUE, L"GyroScrollMutex_v1");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        MessageBoxW(nullptr, L"GyroScroll is already running.", L"GyroScroll",
                    MB_ICONINFORMATION | MB_OK);
        return 0;
    }

    // INI file lives beside the executable
    wchar_t exePath[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    g_iniPath = exePath;
    auto slash = g_iniPath.find_last_of(L"\\/");
    g_iniPath  = (slash != std::wstring::npos ? g_iniPath.substr(0, slash + 1) : L"")
                 + L"GyroScroll.ini";
    LoadSettings();

    WNDCLASSW wc     = {};
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInst;
    wc.hIcon         = LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDI_APPICON));
    wc.lpszClassName = L"GyroScrollWnd_v1";
    if (!RegisterClassW(&wc)) {
        MessageBoxW(nullptr, L"Failed to register window class.", L"GyroScroll", MB_ICONERROR);
        return 1;
    }

    // Message-only window — no visible window needed
    HWND hwnd = CreateWindowW(L"GyroScrollWnd_v1", L"GyroScroll",
                               0, 0, 0, 0, 0,
                               HWND_MESSAGE, nullptr, hInst, nullptr);
    if (!hwnd) {
        MessageBoxW(nullptr, L"Failed to create message window.", L"GyroScroll", MB_ICONERROR);
        return 1;
    }

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    CloseHandle(mutex);
    return (int)msg.wParam;
}
