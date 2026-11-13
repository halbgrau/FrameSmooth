// Shared settings / status between the OpenXR layer (inside the game) and the dashboard overlay.
#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <cstdint>
#include <cstring>
#include <cwctype>
#include <string>

namespace fs_shared {

constexpr uint32_t kMagic = 0x46534D32;  // 'FSM2'

enum RateMode : int32_t { Rate_Auto = 0, Rate_AlwaysHalf = 1, Rate_Native = 2 };
enum SynthMode : int32_t { Synth_Motion = 0, Synth_Rotation = 1 };

struct Settings {
    int32_t enabled = 1;            // applies when a game session starts
    int32_t rateMode = Rate_Auto;   // live
    int32_t synthMode = Synth_Motion;  // live
    int32_t debugTint = 0;          // live: tint synthesized frames green
};

struct Status {
    volatile LONG64 heartbeatMs;  // GetTickCount64() of the last layer update
    int32_t active;               // 1 = layer is driving the frame loop
    int32_t divisor;              // 1 = native rate, 2 = half rate
    float displayHz;
    float appFps;
    float synthPerSec;
    char app[64];
    char reason[96];
};

struct Shared {
    uint32_t magic;
    Settings settings;
    Status status;
};

inline std::wstring DataDir() {
    wchar_t buf[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH);
    std::wstring p = (n > 0 && n < MAX_PATH) ? std::wstring(buf, n) : std::wstring(L".");
    p += L"\\FrameSmooth";
    CreateDirectoryW(p.c_str(), nullptr);
    return p;
}

inline std::wstring SettingsPath() { return DataDir() + L"\\settings.ini"; }

inline Settings LoadSettings() {
    Settings s;
    std::wstring p = SettingsPath();
    s.enabled = GetPrivateProfileIntW(L"FrameSmooth", L"Enabled", s.enabled, p.c_str());
    s.rateMode = GetPrivateProfileIntW(L"FrameSmooth", L"RateMode", s.rateMode, p.c_str());
    s.synthMode = GetPrivateProfileIntW(L"FrameSmooth", L"SynthMode", s.synthMode, p.c_str());
    s.debugTint = GetPrivateProfileIntW(L"FrameSmooth", L"DebugTint", s.debugTint, p.c_str());
    return s;
}

inline void SaveSettings(const Settings& s) {
    std::wstring p = SettingsPath();
    auto w = [&](const wchar_t* k, int v) {
        wchar_t b[16];
        swprintf_s(b, L"%d", v);
        WritePrivateProfileStringW(L"FrameSmooth", k, b, p.c_str());
    };
    w(L"Enabled", s.enabled);
    w(L"RateMode", s.rateMode);
    w(L"SynthMode", s.synthMode);
    w(L"DebugTint", s.debugTint);
}

// Per-game opt-in for the OpenVR hook. Key = lower-case exe file name.
inline bool IsGameEnabled(const std::string& exeLower) {
    std::wstring w(exeLower.begin(), exeLower.end());
    return GetPrivateProfileIntW(L"Games", w.c_str(), 0, SettingsPath().c_str()) != 0;
}
inline void SetGameEnabled(const std::wstring& exeLower, bool on) {
    WritePrivateProfileStringW(L"Games", exeLower.c_str(), on ? L"1" : nullptr, SettingsPath().c_str());
}

// FrameSmooth only acts on games while the FrameSmooth app (dashboard) is running.
inline bool DashboardRunning() {
    HANDLE h = OpenMutexW(SYNCHRONIZE, FALSE, L"Local\\FrameSmoothDashboard");
    if (!h) return false;
    CloseHandle(h);
    return true;
}

// The OpenXR loader loads an implicit layer only while its registry value is 0. FrameSmooth keeps it at 1
// (disabled) and switches it to 0 only while the FrameSmooth app is running and no anti-cheat game runs.
// Only an existing value (created by the installer) is changed.
// Returns a Win32 error code (0 = success) for diagnostics.
inline LONG SetLayerRegistered(const std::wstring& manifestPath, bool on) {
    HKEY k;
    LONG r = RegOpenKeyExW(HKEY_CURRENT_USER, L"SOFTWARE\\Khronos\\OpenXR\\1\\ApiLayers\\Implicit", 0,
                           KEY_QUERY_VALUE | KEY_SET_VALUE, &k);
    if (r != ERROR_SUCCESS) return r;
    DWORD cur = 0, sz = sizeof(cur), type = 0;
    r = RegQueryValueExW(k, manifestPath.c_str(), nullptr, &type, reinterpret_cast<BYTE*>(&cur), &sz);
    if (r == ERROR_SUCCESS) {
        DWORD v = on ? 0 : 1;
        if (cur != v) r = RegSetValueExW(k, manifestPath.c_str(), 0, REG_DWORD, reinterpret_cast<const BYTE*>(&v), sizeof(v));
    }
    RegCloseKey(k);
    return r;
}

// Folders that contain many unrelated programs; the anti-cheat search never looks at these.
inline bool IsSharedFolder(const std::wstring& lowDir) {
    auto endsWith = [&](const wchar_t* s) {
        size_t n = wcslen(s);
        return lowDir.size() >= n && lowDir.compare(lowDir.size() - n, n, s) == 0;
    };
    if (lowDir.size() <= 3) return true;  // drive root
    if (endsWith(L"\\program files") || endsWith(L"\\program files (x86)") || endsWith(L"\\steamapps\\common") ||
        endsWith(L"\\steamapps") || endsWith(L"\\programdata") || endsWith(L"\\users") || endsWith(L"\\appdata\\local") ||
        endsWith(L"\\appdata\\roaming") || endsWith(L"\\appdata\\local\\programs") || endsWith(L"\\games"))
        return true;
    wchar_t prof[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"USERPROFILE", prof, MAX_PATH);
    if (n > 0 && n < MAX_PATH) {
        std::wstring p(prof, n);
        for (auto& c : p) c = towlower(c);
        if (lowDir == p) return true;
    }
    return false;
}

// Anti-cheat detection: looks for common anti-cheat components in an executable's own folder tree
// (its folder and up to 3 parent folders, never entering shared folders like Program Files).
inline bool HasAntiCheat(const std::wstring& exePath, std::wstring* what = nullptr) {
    static const wchar_t* patterns[] = {L"*EasyAntiCheat*", L"*BattlEye*", L"BEService*", L"start_protected_game*",
                                        L"*PnkBstr*", L"*xhunter*", L"*nProtect*", L"*vgk*", L"*EAAntiCheat*"};
    size_t cut = exePath.find_last_of(L"\\/");
    if (cut == std::wstring::npos) return false;
    std::wstring dir = exePath.substr(0, cut);
    if (dir.compare(0, 4, L"\\\\?\\") == 0) dir = dir.substr(4);
    for (int level = 0; level < 4 && !dir.empty(); level++) {
        std::wstring low = dir;
        for (auto& c : low) c = towlower(c);
        if (IsSharedFolder(low)) break;
        for (auto* p : patterns) {
            WIN32_FIND_DATAW fd;
            HANDLE h = FindFirstFileW((dir + L"\\" + p).c_str(), &fd);
            if (h != INVALID_HANDLE_VALUE) {
                if (what) *what = fd.cFileName;
                FindClose(h);
                return true;
            }
        }
        cut = dir.find_last_of(L"\\/");
        if (cut == std::wstring::npos) break;
        dir = dir.substr(0, cut);
    }
    return false;
}

// Opens (or creates) the shared block. The handle is intentionally never closed.
inline Shared* Open() {
    HANDLE h = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(Shared),
                                  L"Local\\FrameSmoothShared_v2");
    if (!h) return nullptr;
    bool existed = GetLastError() == ERROR_ALREADY_EXISTS;
    auto* s = static_cast<Shared*>(MapViewOfFile(h, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(Shared)));
    if (!s) return nullptr;
    if (!existed || s->magic != kMagic) {
        memset(s, 0, sizeof(Shared));
        s->settings = LoadSettings();
        s->magic = kMagic;
    }
    return s;
}

}  // namespace fs_shared
