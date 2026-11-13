// FrameSmooth dashboard: a SteamVR dashboard overlay (tab) to control FrameSmooth.
// Runs in the background; waits for SteamVR, then adds a "FrameSmooth" tab to the dashboard.
// It does not register itself with SteamVR (no manifest), so nothing is written to SteamVR's config.
//
// It also detects the current VR game. OpenXR games are handled by the OpenXR layer automatically.
// For OpenVR games the user can opt in per game; the dashboard then loads FrameSmoothOVR.dll into
// that game's process (in memory only). Games shipping anti-cheat are refused.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <tlhelp32.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <openvr.h>

#include <algorithm>
#include <cstdio>
#include <iterator>
#include <map>
#include <mutex>
#include <set>
#include <thread>
#include <string>
#include <vector>

#include "shared.h"

static const int W = 1024, H = 850;

enum ButtonId {
    B_EN_ON = 1, B_EN_OFF, B_RATE_AUTO, B_RATE_HALF, B_RATE_NATIVE, B_SYN_MOTION, B_SYN_ROT, B_DBG_ON, B_DBG_OFF,
    B_GAME_ON, B_GAME_OFF, B_AUTO_ON, B_AUTO_OFF, B_EXIT
};

static HANDLE g_exitEvt = nullptr;  // signaled by the Exit button or "FrameSmoothDashboard.exe --exit/--toggle"
static bool ExitRequested() { return WaitForSingleObject(g_exitEvt, 0) == WAIT_OBJECT_0; }

// ---------------------------------------------------------------------------------------------
// "Start with SteamVR": a shortcut in the user's Windows Startup folder. FrameSmooth then waits
// quietly in the background and appears whenever SteamVR starts. SteamVR's config is not touched.
// ---------------------------------------------------------------------------------------------
static std::wstring StartupLinkPath() {
    PWSTR dir = nullptr;
    std::wstring p;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Startup, 0, nullptr, &dir))) p = std::wstring(dir) + L"\\FrameSmooth.lnk";
    CoTaskMemFree(dir);
    return p;
}

static bool AutostartEnabled() {
    std::wstring p = StartupLinkPath();
    return !p.empty() && GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES;
}

static void SetAutostart(bool on) {
    std::wstring link = StartupLinkPath();
    if (link.empty()) return;
    if (!on) { DeleteFileW(link.c_str()); return; }
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring dir(exe);
    dir = dir.substr(0, dir.find_last_of(L"\\/"));
    IShellLinkW* sl = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&sl)))) {
        sl->SetPath(exe);
        sl->SetWorkingDirectory(dir.c_str());
        sl->SetDescription(L"FrameSmooth (waits for SteamVR)");
        IPersistFile* pf = nullptr;
        if (SUCCEEDED(sl->QueryInterface(IID_PPV_ARGS(&pf)))) {
            pf->Save(link.c_str(), TRUE);
            pf->Release();
        }
        sl->Release();
    }
}

// ---------------------------------------------------------------------------------------------
// Game detection
// ---------------------------------------------------------------------------------------------
enum class GameApi { Unknown, OpenVR, OpenXR };

struct GameInfo {
    DWORD pid = 0;
    std::wstring path, exeLower;
    GameApi api = GameApi::Unknown;
    bool d3d12 = false, vulkan = false;
    bool antiCheat = false;
    std::wstring antiCheatWhat;
    bool hookLoaded = false;
    bool is32bit = false;
    ULONGLONG lastScan = 0;
    ULONGLONG sceneSince = 0;  // when this process became the VR scene app
};

static std::wstring Lower(std::wstring s) {
    for (auto& c : s) c = towlower(c);
    return s;
}

static void ScanModules(GameInfo& gi) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, gi.pid);
    if (snap == INVALID_HANDLE_VALUE) return;
    MODULEENTRY32W me{sizeof(me)};
    bool ovr = false, oxr = false;
    for (BOOL ok = Module32FirstW(snap, &me); ok; ok = Module32NextW(snap, &me)) {
        std::wstring n = Lower(me.szModule);
        if (n == L"openvr_api.dll") ovr = true;
        else if (n == L"openxr_loader.dll") oxr = true;
        else if (n == L"d3d12.dll") gi.d3d12 = true;
        else if (n == L"vulkan-1.dll") gi.vulkan = true;
        else if (n == L"framesmoothovr.dll") gi.hookLoaded = true;
        else if (n.find(L"easyanticheat") != std::wstring::npos || n.find(L"beclient") != std::wstring::npos) {
            gi.antiCheat = true;
            gi.antiCheatWhat = me.szModule;
        }
    }
    CloseHandle(snap);
    if (oxr) gi.api = GameApi::OpenXR;
    else if (ovr) gi.api = GameApi::OpenVR;
}

// Look for common anti-cheat components next to the game (exe folder and up to 3 parent folders).
static void ScanAntiCheat(GameInfo& gi) {
    if (fs_shared::HasAntiCheat(gi.path, &gi.antiCheatWhat)) gi.antiCheat = true;
}

// ---------------------------------------------------------------------------------------------
// Layer registration watcher: the OpenXR layer is registered only while this app runs and no
// program from an anti-cheat-protected folder is running (so it is never loaded into such games).
// ---------------------------------------------------------------------------------------------
static std::mutex g_acMtx;
static std::wstring g_acName;  // non-empty while an anti-cheat game runs

static std::wstring ManifestPath() {
    wchar_t buf[MAX_PATH];
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    std::wstring p(buf);
    return p.substr(0, p.find_last_of(L"\\/")) + L"\\FrameSmoothLayer.json";
}

static void WatchLog(const wchar_t* fmt, ...) {
    static FILE* f = nullptr;
    if (!f) f = _wfsopen((fs_shared::DataDir() + L"\\watch.log").c_str(), L"w", _SH_DENYNO);
    if (!f) return;
    SYSTEMTIME t;
    GetLocalTime(&t);
    fwprintf(f, L"%02d:%02d:%02d ", t.wHour, t.wMinute, t.wSecond);
    va_list ap;
    va_start(ap, fmt);
    vfwprintf(f, fmt, ap);
    va_end(ap);
    fputwc(L'\n', f);
    fflush(f);
}

static void WatchThread() {
    const std::wstring manifest = ManifestPath();
    WatchLog(L"watcher started, manifest=%s", manifest.c_str());
    std::wstring winDir(MAX_PATH, L'\0');
    winDir.resize(GetWindowsDirectoryW(&winDir[0], MAX_PATH));
    winDir = Lower(winDir);
    std::map<DWORD, std::wstring> acProcs;  // pid -> exe name, for running anti-cheat processes
    std::set<DWORD> seen;
    std::map<std::wstring, bool> dirCache;
    int state = -1;
    while (!ExitRequested()) {
        std::set<DWORD> alive;
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snap != INVALID_HANDLE_VALUE) {
            PROCESSENTRY32W pe{sizeof(pe)};
            for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe)) {
                DWORD pid = pe.th32ProcessID;
                alive.insert(pid);
                if (seen.count(pid)) continue;
                seen.insert(pid);
                HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
                if (!h) continue;
                wchar_t buf[MAX_PATH * 2];
                DWORD n = MAX_PATH * 2;
                std::wstring path;
                if (QueryFullProcessImageNameW(h, 0, buf, &n)) path.assign(buf, n);
                CloseHandle(h);
                if (path.empty()) continue;
                std::wstring low = Lower(path);
                if (low.compare(0, winDir.size(), winDir) == 0) continue;
                std::wstring dir = low.substr(0, low.find_last_of(L"\\/"));
                auto it = dirCache.find(dir);
                bool ac = it != dirCache.end() ? it->second : (dirCache[dir] = fs_shared::HasAntiCheat(path));
                if (ac) {
                    acProcs[pid] = pe.szExeFile;
                    WatchLog(L"anti-cheat program: %s (pid %lu)", path.c_str(), pid);
                }
            }
            CloseHandle(snap);
        }
        for (auto it = seen.begin(); it != seen.end();) it = alive.count(*it) ? std::next(it) : seen.erase(it);
        for (auto it = acProcs.begin(); it != acProcs.end();) it = alive.count(it->first) ? std::next(it) : acProcs.erase(it);
        {
            std::lock_guard<std::mutex> lk(g_acMtx);
            g_acName = acProcs.empty() ? L"" : acProcs.begin()->second;
        }
        int want = acProcs.empty() ? 1 : 0;
        if (want != state) {
            LONG r = fs_shared::SetLayerRegistered(manifest, want == 1);
            WatchLog(L"OpenXR layer %s (result %ld)", want == 1 ? L"ON" : L"OFF (anti-cheat program running)", r);
            state = want;
        }
        WaitForSingleObject(g_exitEvt, 400);
    }
    LONG r = fs_shared::SetLayerRegistered(manifest, false);
    WatchLog(L"exiting: OpenXR layer OFF (result %ld)", r);
}

static GameInfo DetectGame(DWORD pid, const GameInfo& prev) {
    if (pid == 0) return GameInfo{};
    GameInfo gi = prev.pid == pid ? prev : GameInfo{};
    if (gi.pid != pid) gi.sceneSince = GetTickCount64();
    gi.pid = pid;
    if (gi.path.empty()) {
        HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (!h) return gi;
        wchar_t buf[MAX_PATH * 2];
        DWORD n = MAX_PATH * 2;
        if (QueryFullProcessImageNameW(h, 0, buf, &n)) gi.path.assign(buf, n);
        BOOL wow = FALSE;
        IsWow64Process(h, &wow);
        gi.is32bit = wow != FALSE;
        CloseHandle(h);
        gi.exeLower = Lower(gi.path.substr(gi.path.find_last_of(L"\\/") + 1));
        ScanAntiCheat(gi);
    }
    if (GetTickCount64() - gi.lastScan > 3000) {
        gi.lastScan = GetTickCount64();
        ScanModules(gi);
    }
    return gi;
}

static bool Inject(DWORD pid, const std::wstring& dll) {
    HANDLE p = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION | PROCESS_VM_WRITE |
                               PROCESS_VM_READ, FALSE, pid);
    if (!p) return false;
    size_t bytes = (dll.size() + 1) * sizeof(wchar_t);
    void* mem = VirtualAllocEx(p, nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    bool ok = false;
    if (mem && WriteProcessMemory(p, mem, dll.c_str(), bytes, nullptr)) {
        auto loadLib = (LPTHREAD_START_ROUTINE)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW");
        HANDLE t = CreateRemoteThread(p, nullptr, 0, loadLib, mem, 0, nullptr);
        if (t) {
            WaitForSingleObject(t, 10000);
            DWORD code = 0;
            GetExitCodeThread(t, &code);
            ok = code != 0;
            CloseHandle(t);
        }
    }
    if (mem) VirtualFreeEx(p, mem, 0, MEM_RELEASE);
    CloseHandle(p);
    return ok;
}

// ---------------------------------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------------------------------
struct Canvas {
    HDC dc = nullptr;
    HBITMAP bmp = nullptr;
    uint32_t* px = nullptr;
    int w = 0, h = 0;
    HFONT fTitle = nullptr, fBody = nullptr, fSmall = nullptr;
    std::vector<std::pair<RECT, int>> buttons;
    std::vector<uint32_t> rgba;

    void Init(int width, int height) {
        w = width;
        h = height;
        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
        bi.bmiHeader.biWidth = w;
        bi.bmiHeader.biHeight = -h;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        dc = CreateCompatibleDC(nullptr);
        bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, reinterpret_cast<void**>(&px), nullptr, 0);
        SelectObject(dc, bmp);
        auto mk = [](int size, int weight) {
            return CreateFontW(-size, 0, 0, 0, weight, 0, 0, 0, DEFAULT_CHARSET, 0, 0, ANTIALIASED_QUALITY, 0, L"Segoe UI");
        };
        fTitle = mk(44, FW_SEMIBOLD);
        fBody = mk(28, FW_NORMAL);
        fSmall = mk(22, FW_NORMAL);
        SetBkMode(dc, TRANSPARENT);
        rgba.resize(size_t(w) * h);
    }
    void Fill(int x, int y, int ww, int hh, COLORREF c, int radius = 0) {
        HBRUSH br = CreateSolidBrush(c);
        HGDIOBJ ob = SelectObject(dc, br);
        HGDIOBJ op = SelectObject(dc, GetStockObject(NULL_PEN));
        if (radius) RoundRect(dc, x, y, x + ww + 1, y + hh + 1, radius, radius);
        else { RECT r{x, y, x + ww, y + hh}; FillRect(dc, &r, br); }
        SelectObject(dc, ob);
        SelectObject(dc, op);
        DeleteObject(br);
    }
    void Text(int x, int y, const std::wstring& t, HFONT f, COLORREF c) {
        SelectObject(dc, f);
        SetTextColor(dc, c);
        TextOutW(dc, x, y, t.c_str(), int(t.size()));
    }
    void Button(int x, int y, int ww, int hh, const wchar_t* label, bool selected, int id) {
        Fill(x, y, ww, hh, selected ? RGB(38, 132, 255) : RGB(60, 64, 74), 22);
        SelectObject(dc, fBody);
        SetTextColor(dc, RGB(255, 255, 255));
        RECT r{x, y, x + ww, y + hh};
        DrawTextW(dc, label, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        buttons.push_back({r, id});
    }
    const uint32_t* ToRGBA() {
        GdiFlush();
        for (size_t i = 0; i < rgba.size(); i++) {
            uint32_t p = px[i];
            rgba[i] = ((p >> 16) & 0xFF) | (p & 0xFF00) | ((p & 0xFF) << 16) | 0xFF000000u;
        }
        return rgba.data();
    }
};

static std::wstring Widen(const char* s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
    std::wstring w(n > 0 ? n - 1 : 0, L'\0');
    if (n > 1) MultiByteToWideChar(CP_UTF8, 0, s, -1, &w[0], n);
    return w;
}

static void DrawUi(Canvas& c, fs_shared::Shared* sh, const GameInfo& gi) {
    const auto& st = sh->settings;
    const auto& ss = sh->status;
    c.buttons.clear();
    c.Fill(0, 0, c.w, c.h, RGB(24, 26, 31));
    c.Text(40, 26, L"FrameSmooth", c.fTitle, RGB(255, 255, 255));
    c.Text(330, 42, L"half-rate rendering + motion reprojection", c.fSmall, RGB(150, 156, 170));
    c.Fill(c.w - 220, 24, 180, 56, RGB(150, 48, 48), 22);
    {
        SelectObject(c.dc, c.fBody);
        SetTextColor(c.dc, RGB(255, 255, 255));
        RECT r{c.w - 220, 24, c.w - 40, 80};
        DrawTextW(c.dc, L"Exit", -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        c.buttons.push_back({r, B_EXIT});
    }

    // status box
    c.Fill(40, 96, c.w - 80, 112, RGB(36, 39, 46), 20);
    bool live = ss.heartbeatMs != 0 && (GetTickCount64() - (ULONGLONG)ss.heartbeatMs) < 3000;
    wchar_t line[300];
    std::wstring acName;
    {
        std::lock_guard<std::mutex> lk(g_acMtx);
        acName = g_acName;
    }
    if (!acName.empty()) {
        swprintf_s(line, L"Paused: anti-cheat program running (%s)", acName.c_str());
        c.Text(64, 112, line, c.fBody, RGB(240, 180, 90));
        c.Text(64, 152, L"FrameSmooth stays out of all games until it closes.", c.fSmall, RGB(140, 146, 160));
    } else if (!live) {
        c.Text(64, 112, gi.pid ? L"FrameSmooth is not active in this game" : L"No VR game running", c.fBody, RGB(200, 204, 214));
        c.Text(64, 152, gi.pid ? L"See 'This game' below." : L"Start a game; status will appear here.", c.fSmall, RGB(140, 146, 160));
    } else {
        swprintf_s(line, L"Game: %s", Widen(ss.app).c_str());
        c.Text(64, 108, line, c.fBody, RGB(230, 232, 238));
        if (ss.active) {
            swprintf_s(line, L"%s  |  %.0f Hz  |  game %.1f fps  |  synthesized %.0f /s  |  %s",
                       ss.divisor == 2 ? L"HALF RATE" : L"NATIVE RATE", ss.displayHz, ss.appFps, ss.synthPerSec,
                       Widen(ss.reason).c_str() + (strncmp(ss.reason, "Active ", 7) == 0 ? 7 : 0));
            c.Text(64, 150, line, c.fSmall, RGB(110, 220, 140));
        } else {
            swprintf_s(line, L"Inactive: %s", Widen(ss.reason).c_str());
            c.Text(64, 150, line, c.fBody, RGB(240, 180, 90));
        }
    }

    // this game
    int y = 226, lx = 48, bx = 340, bh = 64;
    c.Text(lx, y + 16, L"This game", c.fBody, RGB(220, 222, 230));
    if (!gi.pid) {
        c.Text(bx, y + 18, L"none detected", c.fSmall, RGB(140, 146, 160));
    } else {
        const wchar_t* api = gi.api == GameApi::OpenXR ? L"OpenXR" : gi.api == GameApi::OpenVR ? L"OpenVR" : L"?";
        swprintf_s(line, L"%s  (%s)", gi.exeLower.c_str(), api);
        c.Text(bx, y - 4, line, c.fSmall, RGB(200, 204, 214));
        if (gi.api == GameApi::OpenXR) {
            c.Text(bx, y + 30, L"OpenXR games are handled automatically.", c.fSmall, RGB(110, 220, 140));
        } else if (gi.api == GameApi::OpenVR) {
            if (gi.antiCheat) {
                swprintf_s(line, L"Anti-cheat detected (%s) - not supported", gi.antiCheatWhat.c_str());
                c.Text(bx, y + 30, line, c.fSmall, RGB(240, 120, 100));
            } else if (gi.is32bit) {
                c.Text(bx, y + 30, L"32-bit game - not supported", c.fSmall, RGB(240, 180, 90));
            } else {
                bool on = fs_shared::IsGameEnabled(std::string(gi.exeLower.begin(), gi.exeLower.end()));
                c.Button(bx, y + 28, 300, 52, on ? L"Enabled for this game" : L"Enable for this game", on, B_GAME_ON);
                c.Button(bx + 316, y + 28, 150, 52, L"Off", !on, B_GAME_OFF);
                if (on && !gi.hookLoaded) c.Text(bx + 482, y + 40, L"loading...", c.fSmall, RGB(140, 146, 160));
            }
        } else {
            c.Text(bx, y + 30, L"Detecting VR API...", c.fSmall, RGB(140, 146, 160));
        }
    }

    y += 120;
    c.Text(lx, y + 16, L"Enabled", c.fBody, RGB(220, 222, 230));
    c.Button(bx, y, 190, bh, L"On", st.enabled != 0, B_EN_ON);
    c.Button(bx + 206, y, 190, bh, L"Off", st.enabled == 0, B_EN_OFF);

    y += 88;
    c.Text(lx, y + 16, L"Frame rate", c.fBody, RGB(220, 222, 230));
    c.Button(bx, y, 190, bh, L"Auto", st.rateMode == fs_shared::Rate_Auto, B_RATE_AUTO);
    c.Button(bx + 206, y, 220, bh, L"Always half", st.rateMode == fs_shared::Rate_AlwaysHalf, B_RATE_HALF);
    c.Button(bx + 442, y, 190, bh, L"Native", st.rateMode == fs_shared::Rate_Native, B_RATE_NATIVE);

    y += 88;
    c.Text(lx, y + 16, L"Reprojection", c.fBody, RGB(220, 222, 230));
    c.Button(bx, y, 300, bh, L"Motion (ASW-like)", st.synthMode == fs_shared::Synth_Motion, B_SYN_MOTION);
    c.Button(bx + 316, y, 316, bh, L"Rotation only", st.synthMode == fs_shared::Synth_Rotation, B_SYN_ROT);

    y += 88;
    c.Text(lx, y + 16, L"Tint fake frames", c.fBody, RGB(220, 222, 230));
    c.Button(bx, y, 190, bh, L"On", st.debugTint != 0, B_DBG_ON);
    c.Button(bx + 206, y, 190, bh, L"Off", st.debugTint == 0, B_DBG_OFF);

    y += 88;
    bool autoOn = AutostartEnabled();
    c.Text(lx, y + 16, L"Start with SteamVR", c.fBody, RGB(220, 222, 230));
    c.Button(bx, y, 190, bh, L"On", autoOn, B_AUTO_ON);
    c.Button(bx + 206, y, 190, bh, L"Off", !autoOn, B_AUTO_OFF);

    c.Text(48, c.h - 70,
           L"Auto halves the frame rate only when needed.  OpenXR: On/Off applies on next game start.",
           c.fSmall, RGB(130, 136, 150));
    c.Text(48, c.h - 42,
           L"Games are only affected while FrameSmooth runs. Exit = games render normally again.",
           c.fSmall, RGB(130, 136, 150));
}

static void DrawThumb(Canvas& c) {
    c.Fill(0, 0, c.w, c.h, RGB(38, 132, 255));
    c.Fill(10, 22, 20, 20, RGB(255, 255, 255), 6);
    c.Fill(34, 22, 20, 20, RGB(255, 255, 255), 6);
}

static void OnClick(fs_shared::Shared* sh, int id, const GameInfo& gi) {
    auto st = sh->settings;
    switch (id) {
        case B_EN_ON: st.enabled = 1; break;
        case B_EN_OFF: st.enabled = 0; break;
        case B_RATE_AUTO: st.rateMode = fs_shared::Rate_Auto; break;
        case B_RATE_HALF: st.rateMode = fs_shared::Rate_AlwaysHalf; break;
        case B_RATE_NATIVE: st.rateMode = fs_shared::Rate_Native; break;
        case B_SYN_MOTION: st.synthMode = fs_shared::Synth_Motion; break;
        case B_SYN_ROT: st.synthMode = fs_shared::Synth_Rotation; break;
        case B_DBG_ON: st.debugTint = 1; break;
        case B_DBG_OFF: st.debugTint = 0; break;
        case B_GAME_ON:
            if (gi.pid && !gi.antiCheat) fs_shared::SetGameEnabled(gi.exeLower, true);
            return;
        case B_GAME_OFF:
            if (gi.pid) fs_shared::SetGameEnabled(gi.exeLower, false);
            return;
        case B_AUTO_ON: SetAutostart(true); return;
        case B_AUTO_OFF: SetAutostart(false); return;
        case B_EXIT: SetEvent(g_exitEvt); return;
        default: return;
    }
    sh->settings = st;
    fs_shared::SaveSettings(st);
}

static bool SteamVRRunning() {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32W pe{sizeof(pe)};
    bool found = false;
    for (BOOL ok = Process32FirstW(snap, &pe); ok && !found; ok = Process32NextW(snap, &pe))
        if (_wcsicmp(pe.szExeFile, L"vrserver.exe") == 0) found = true;
    CloseHandle(snap);
    return found;
}

static std::wstring HookDllPath() {
    wchar_t buf[MAX_PATH];
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    std::wstring p(buf);
    return p.substr(0, p.find_last_of(L"\\/")) + L"\\FrameSmoothOVR.dll";
}

static void RunOverlay(fs_shared::Shared* sh) {
    vr::EVRInitError err = vr::VRInitError_None;
    vr::VR_Init(&err, vr::VRApplication_Overlay);
    if (err != vr::VRInitError_None) return;

    vr::VROverlayHandle_t main = vr::k_ulOverlayHandleInvalid, thumb = vr::k_ulOverlayHandleInvalid;
    if (vr::VROverlay()->CreateDashboardOverlay("framesmooth.dashboard", "FrameSmooth", &main, &thumb) != vr::VROverlayError_None) {
        vr::VR_Shutdown();
        return;
    }
    vr::VROverlay()->SetOverlayWidthInMeters(main, 2.2f);
    vr::VROverlay()->SetOverlayInputMethod(main, vr::VROverlayInputMethod_Mouse);
    vr::HmdVector2_t scale{float(W), float(H)};
    vr::VROverlay()->SetOverlayMouseScale(main, &scale);

    Canvas ui, th;
    ui.Init(W, H);
    th.Init(64, 64);
    DrawThumb(th);
    vr::VROverlay()->SetOverlayRaw(thumb, (void*)th.ToRGBA(), 64, 64, 4);

    const DWORD selfPid = GetCurrentProcessId();
    const std::wstring hookDll = HookDllPath();
    std::set<DWORD> injected;
    GameInfo game;
    bool dirty = true;
    ULONGLONG lastDraw = 0, lastDetect = 0;
    for (;;) {
        if (ExitRequested()) {
            vr::VROverlay()->DestroyOverlay(main);
            vr::VR_Shutdown();
            return;
        }
        vr::VREvent_t ev;
        while (vr::VRSystem()->PollNextEvent(&ev, sizeof(ev))) {
            if (ev.eventType == vr::VREvent_Quit) {
                vr::VRSystem()->AcknowledgeQuit_Exiting();
                vr::VR_Shutdown();
                return;
            }
        }
        while (vr::VROverlay()->PollNextOverlayEvent(main, &ev, sizeof(ev))) {
            if (ev.eventType == vr::VREvent_MouseButtonDown) {
                POINT p{int(ev.data.mouse.x), H - int(ev.data.mouse.y)};  // OpenVR mouse origin is bottom-left
                for (auto& b : ui.buttons)
                    if (PtInRect(&b.first, p)) { OnClick(sh, b.second, game); dirty = true; }
            }
        }
        ULONGLONG now = GetTickCount64();
        if (now - lastDetect > 1000 || dirty) {
            lastDetect = now;
            DWORD pid = vr::VRApplications()->GetCurrentSceneProcessId();
            if (pid == selfPid) pid = 0;
            game = DetectGame(pid, game);
            // opt-in injection of the OpenVR hook
            // wait until the game has been running in VR for a while (attaching during its start-up is unreliable)
            if (game.pid && game.api == GameApi::OpenVR && !game.antiCheat && !game.is32bit && !game.hookLoaded &&
                !injected.count(game.pid) && GetTickCount64() - game.sceneSince >= 10000 &&
                fs_shared::IsGameEnabled(std::string(game.exeLower.begin(), game.exeLower.end()))) {
                injected.insert(game.pid);
                bool ok = Inject(game.pid, hookDll);
                game.lastScan = 0;
                OutputDebugStringW(ok ? L"FrameSmooth: hook loaded\n" : L"FrameSmooth: hook load failed\n");
                dirty = true;
            }
        }
        bool visible = vr::VROverlay()->IsOverlayVisible(main);
        if (dirty || (visible && now - lastDraw > 500)) {
            DrawUi(ui, sh, game);
            vr::VROverlay()->SetOverlayRaw(main, (void*)ui.ToRGBA(), W, H, 4);
            lastDraw = now;
            dirty = false;
        }
        WaitForSingleObject(g_exitEvt, visible ? 30 : 200);
    }
}

// Usage: FrameSmoothDashboard.exe            start (no-op if already running)
//        FrameSmoothDashboard.exe --exit     close a running instance
//        FrameSmoothDashboard.exe --toggle   close it if running, otherwise start it
int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR cmd, int) {
    const bool argExit = cmd && wcsstr(cmd, L"--exit");
    const bool argToggle = cmd && wcsstr(cmd, L"--toggle");
    g_exitEvt = CreateEventW(nullptr, TRUE, FALSE, L"Local\\FrameSmoothDashboardExit");
    HANDLE mtx = CreateMutexW(nullptr, TRUE, L"Local\\FrameSmoothDashboard");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        if (argExit || argToggle) SetEvent(g_exitEvt);  // ask the running instance to close
        return 0;
    }
    if (argExit) return 0;  // nothing running

    // Started from Steam (e.g. as a non-Steam shortcut)? Then Steam would treat us as a running game and
    // SteamVR would add a "desktop game" tab for it. Hand off to Windows Explorer instead (the new process
    // is not part of Steam's process tree and has no Steam environment) and let this one exit at once.
    wchar_t steamId[32];
    if (GetEnvironmentVariableW(L"SteamAppId", steamId, 32) > 0 || GetEnvironmentVariableW(L"SteamGameId", steamId, 32) > 0) {
        wchar_t exe[MAX_PATH];
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        std::wstring quoted = L"\"" + std::wstring(exe) + L"\"";
        ReleaseMutex(mtx);
        CloseHandle(mtx);
        ShellExecuteW(nullptr, L"open", L"explorer.exe", quoted.c_str(), nullptr, SW_SHOWNORMAL);
        return 0;
    }
    ResetEvent(g_exitEvt);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    fs_shared::Shared* sh = fs_shared::Open();
    if (!sh) return 1;
    sh->settings = fs_shared::LoadSettings();
    std::thread watcher(WatchThread);
    while (!ExitRequested()) {
        if (SteamVRRunning()) {
            RunOverlay(sh);
            if (ExitRequested()) break;
            for (int i = 0; i < 20 && SteamVRRunning(); i++)
                if (WaitForSingleObject(g_exitEvt, 1000) == WAIT_OBJECT_0) break;
        }
        WaitForSingleObject(g_exitEvt, 3000);
    }
    watcher.join();  // switches the layer registration off before we exit
    ReleaseMutex(mtx);
    CloseHandle(mtx);
    return 0;
}
