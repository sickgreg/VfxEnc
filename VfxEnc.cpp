// VfxEnc.cpp
// Win32 + libmpv preview, drag&drop video + mpv .hook GLSL shaders,
// reorder shaders by drag inside list, and re-encode via ffmpeg+libplacebo.
//
// Build: link against mpv.lib, ensure mpv-2.dll is available at runtime.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <commdlg.h>
#include <commctrl.h>
#include <ShlObj.h>

#include <mpv/client.h>

#include <string>
#include <vector>
#include <algorithm>
#include <thread>
#include <fstream>
#include <sstream>

#pragma comment(lib, "Comdlg32.lib")
#pragma comment(lib, "Shell32.lib")
#pragma comment(lib, "Ole32.lib")
#pragma comment(lib, "Comctl32.lib")

#ifndef LOAD_LIBRARY_SEARCH_DEFAULT_DIRS
#define LOAD_LIBRARY_SEARCH_DEFAULT_DIRS 0x00001000
#endif
#ifndef LOAD_LIBRARY_SEARCH_USER_DIRS
#define LOAD_LIBRARY_SEARCH_USER_DIRS 0x00000400
#endif

// ----------------------------
// Globals (small app, simple)
// ----------------------------
static HINSTANCE g_hInst = nullptr;
static HWND g_hwndMain = nullptr;
static HWND g_hwndVideo = nullptr;
static HWND g_hwndList = nullptr;
static HWND g_hwndStatus = nullptr;
static HWND g_hwndBitrate = nullptr;
static HWND g_hwndBitrateLabel = nullptr;
static HWND g_hwndPlayPause = nullptr;
static HWND g_hwndAddVideo = nullptr;
static HWND g_hwndAddShader = nullptr;
static HWND g_hwndFiltersLabel = nullptr;
static HWND g_hwndEncoder = nullptr;
static HWND g_hwndEncoderLabel = nullptr;

static mpv_handle* g_mpv = nullptr;

static std::wstring g_loadedVideo;
static std::vector<std::wstring> g_shaders;
static std::vector<bool> g_shaderBypass;
static int g_bitrateMbps = 0; // 0 = same as input
static std::wstring g_encoderChoice = L"auto";
static bool g_isPlaying = false;
static std::wstring g_lastVideoDir;
static std::wstring g_lastShaderDir;

// (no custom brushes)

// listbox drag reorder state
static WNDPROC g_listOrigProc = nullptr;
static bool g_dragging = false;
static int  g_dragIndex = -1;

// ----------------------------
// Helpers
// ----------------------------
static std::wstring JoinPath(const std::wstring& a, const std::wstring& b);
static void ListRefresh();
static void MpvApplyShaderList();
static void AddShaderPath(const std::wstring& path);

static std::wstring GetExeDir()
{
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring s = path;
    auto pos = s.find_last_of(L"\\/");
    return (pos == std::wstring::npos) ? L"." : s.substr(0, pos);
}

static void SetupDllSearchPath()
{
    std::wstring deps = JoinPath(GetExeDir(), L"deps");
    HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
    if (!k32) {
        SetDllDirectoryW(deps.c_str());
        return;
    }

    typedef BOOL (WINAPI *SetDefaultDllDirectoriesFn)(DWORD);
    typedef PVOID (WINAPI *AddDllDirectoryFn)(PCWSTR);

    auto pSetDefault = (SetDefaultDllDirectoriesFn)GetProcAddress(k32, "SetDefaultDllDirectories");
    auto pAdd = (AddDllDirectoryFn)GetProcAddress(k32, "AddDllDirectory");
    if (pSetDefault && pAdd) {
        pSetDefault(LOAD_LIBRARY_SEARCH_DEFAULT_DIRS | LOAD_LIBRARY_SEARCH_USER_DIRS);
        pAdd(deps.c_str());
    } else {
        SetDllDirectoryW(deps.c_str());
    }
}

static std::wstring GetAppDataDir()
{
    PWSTR path = nullptr;
    std::wstring dir;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &path)) && path) {
        dir = JoinPath(path, L"VfxEnc");
        CoTaskMemFree(path);
    } else {
        dir = GetExeDir();
    }
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir;
}

static std::wstring GetShadersSavePath()
{
    return JoinPath(GetAppDataDir(), L"shaders.txt");
}

static std::wstring GetSettingsPath()
{
    return JoinPath(GetAppDataDir(), L"settings.txt");
}

static std::string WideToUtf8(const std::wstring& s)
{
    if (s.empty()) return {};
    int len = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string out(len > 0 ? len - 1 : 0, '\0');
    if (len > 1) WideCharToMultiByte(CP_UTF8, 0, s.c_str(), -1, out.data(), len, nullptr, nullptr);
    return out;
}

static std::wstring Utf8ToWide(const std::string& s)
{
    if (s.empty()) return {};
    int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring out(len > 0 ? len - 1 : 0, L'\0');
    if (len > 1) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, out.data(), len);
    return out;
}

static void WriteLogLine(HANDLE h, const std::wstring& line)
{
    if (h == INVALID_HANDLE_VALUE) return;
    std::string utf8 = WideToUtf8(line);
    if (utf8.empty()) return;
    DWORD written = 0;
    WriteFile(h, utf8.data(), (DWORD)utf8.size(), &written, nullptr);
}

static bool EndsWithI(const std::wstring& s, const std::wstring& suf)
{
    if (s.size() < suf.size()) return false;
    auto a = s.substr(s.size() - suf.size());
    auto lower = [](wchar_t c){ return (wchar_t)CharLowerW((LPWSTR)(ULONG_PTR)c); };
    for (size_t i=0;i<suf.size();++i) {
        wchar_t ca = towlower(a[i]);
        wchar_t cb = towlower(suf[i]);
        if (ca != cb) return false;
    }
    return true;
}

static bool IsShaderFile(const std::wstring& p)
{
    // accept common mpv/libplacebo shader suffixes
    return EndsWithI(p, L".glsl") || EndsWithI(p, L".hook.glsl") || EndsWithI(p, L".hook") || EndsWithI(p, L".frag") || EndsWithI(p, L".fs");
}

static bool IsLikelyVideo(const std::wstring& p)
{
    // crude but practical; mpv will accept many containers
    return EndsWithI(p,L".mp4")||EndsWithI(p,L".mkv")||EndsWithI(p,L".mov")||EndsWithI(p,L".avi")||
           EndsWithI(p,L".ts") ||EndsWithI(p,L".m2ts")||EndsWithI(p,L".webm")||EndsWithI(p,L".m4v");
}

static void SetStatus(const std::wstring& msg)
{
    if (g_hwndStatus) SetWindowTextW(g_hwndStatus, msg.c_str());
}

static void UpdatePlayPauseLabel()
{
    if (!g_hwndPlayPause) return;
    int pause = 1;
    if (g_mpv) {
        mpv_get_property(g_mpv, "pause", MPV_FORMAT_FLAG, &pause);
    }
    g_isPlaying = (pause == 0);
    SetWindowTextW(g_hwndPlayPause, L"Play");
    InvalidateRect(g_hwndPlayPause, nullptr, TRUE);
}

static std::wstring Quote(const std::wstring& s)
{
    std::wstring out = L"\"";
    out += s;
    out += L"\"";
    return out;
}

static std::wstring BasenameNoExt(const std::wstring& path)
{
    auto slash = path.find_last_of(L"\\/");
    std::wstring file = (slash == std::wstring::npos) ? path : path.substr(slash+1);
    auto dot = file.find_last_of(L'.');
    if (dot != std::wstring::npos) file = file.substr(0, dot);
    return file;
}

static std::wstring FilenameOnly(const std::wstring& path)
{
    auto slash = path.find_last_of(L"\\/");
    return (slash == std::wstring::npos) ? path : path.substr(slash+1);
}

static std::wstring Dirname(const std::wstring& path)
{
    auto slash = path.find_last_of(L"\\/");
    return (slash == std::wstring::npos) ? L"." : path.substr(0, slash);
}

static std::wstring JoinPath(const std::wstring& a, const std::wstring& b)
{
    if (a.empty()) return b;
    wchar_t last = a.back();
    if (last == L'\\' || last == L'/') return a + b;
    return a + L"\\" + b;
}

static double GetMpvDurationSeconds()
{
    if (!g_mpv) return 0.0;
    double duration = 0.0;
    if (mpv_get_property(g_mpv, "duration", MPV_FORMAT_DOUBLE, &duration) >= 0 && duration > 0.0) {
        return duration;
    }
    return 0.0;
}

static std::wstring BuildEncoderArgs(const std::wstring& enc, int targetMbps)
{
    wchar_t rate[64];
    wchar_t buf[64];
    swprintf_s(rate, L"%dM", targetMbps);
    swprintf_s(buf, L"%dM", targetMbps * 2);

    if (enc == L"hevc_amf") {
        return L"-c:v hevc_amf -rc cbr -b:v " + std::wstring(rate) + L" -maxrate " + rate + L" -bufsize " + buf;
    }
    if (enc == L"hevc_nvenc") {
        return L"-c:v hevc_nvenc -preset p5 -rc vbr -cq 23 -b:v " + std::wstring(rate) + L" -maxrate " + rate + L" -bufsize " + buf;
    }
    if (enc == L"hevc_qsv") {
        return L"-c:v hevc_qsv -b:v " + std::wstring(rate) + L" -maxrate " + rate + L" -bufsize " + buf;
    }
    if (enc == L"hevc_mf") {
        return L"-c:v hevc_mf -b:v " + std::wstring(rate);
    }
    // software fallback
    return L"-c:v libx265 -b:v " + std::wstring(rate) + L" -maxrate " + rate + L" -bufsize " + buf;
}

static int ParseBitrateKbps(const std::string& text)
{
    size_t pos = text.find("bitrate:");
    while (pos != std::string::npos) {
        pos += 8;
        while (pos < text.size() && (text[pos] == ' ' || text[pos] == '\t')) pos++;
        if (pos + 2 < text.size() && text.compare(pos, 3, "N/A") == 0) return 0;
        int val = 0;
        bool any = false;
        while (pos < text.size() && text[pos] >= '0' && text[pos] <= '9') {
            any = true;
            val = val * 10 + (text[pos] - '0');
            pos++;
        }
        if (any && val > 0) return val;
        pos = text.find("bitrate:", pos);
    }
    return 0;
}

static int64_t ParseOutTimeMs(const std::string& line)
{
    const char* key = "out_time_ms=";
    if (line.rfind(key, 0) != 0) return -1;
    int64_t v = 0;
    bool any = false;
    for (size_t i = strlen(key); i < line.size(); ++i) {
        char c = line[i];
        if (c < '0' || c > '9') break;
        any = true;
        v = v * 10 + (c - '0');
    }
    return any ? v : -1;
}

static int ProbeBitrateKbpsWithFfmpeg(const std::wstring& ffmpeg, const std::wstring& file)
{
    std::wstring cmd = Quote(ffmpeg) + L" -hide_banner -i " + Quote(file);

    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    sa.lpSecurityDescriptor = nullptr;

    HANDLE hRead = nullptr;
    HANDLE hWrite = nullptr;
    if (!CreatePipe(&hRead, &hWrite, &sa, 0)) return 0;
    SetHandleInformation(hRead, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags |= STARTF_USESTDHANDLES;
    si.hStdOutput = hWrite;
    si.hStdError = hWrite;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);

    PROCESS_INFORMATION pi{};
    std::wstring mutableCmd = cmd;
    BOOL ok = CreateProcessW(
        nullptr,
        mutableCmd.data(),
        nullptr, nullptr,
        TRUE,
        CREATE_NO_WINDOW,
        nullptr,
        nullptr,
        &si, &pi
    );

    CloseHandle(hWrite);
    if (!ok) {
        CloseHandle(hRead);
        return 0;
    }

    std::string output;
    char buf[4096];
    DWORD read = 0;
    while (ReadFile(hRead, buf, sizeof(buf), &read, nullptr) && read > 0) {
        output.append(buf, buf + read);
    }
    CloseHandle(hRead);

    WaitForSingleObject(pi.hProcess, INFINITE);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);

    return ParseBitrateKbps(output);
}

static std::wstring FfmpegEscapeFilterValue(const std::wstring& value)
{
    // ffmpeg filter args use ':' as option separators; escape special chars.
    std::wstring out;
    out.reserve(value.size() * 2);
    for (wchar_t c : value) {
        if (c == L'\\') {
            out.push_back(L'/');
            continue;
        }
        switch (c) {
        case L':': out += L"\\:"; break;
        case L',': out += L"\\,"; break;
        case L'=': out += L"\\="; break;
        default:   out.push_back(c); break;
        }
    }
    return out;
}

static bool ReadTextFile(const std::wstring& path, std::string& out)
{
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::ostringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    return true;
}

static std::wstring WriteCombinedShaderTemp(const std::vector<std::wstring>& shaders, std::wstring* outFilename)
{
    std::wstring outDir = GetExeDir();

    SYSTEMTIME st{};
    GetSystemTime(&st);

    wchar_t name[256];
    swprintf_s(name, L"combined_shaders_%04u%02u%02u_%02u%02u%02u.glsl",
               st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);

    if (outFilename) *outFilename = name;

    std::wstring outPath = JoinPath(outDir, name);

    std::ofstream o(outPath, std::ios::binary);
    if (!o) return L"";

    for (const auto& s : shaders) {
        std::string text;
        if (ReadTextFile(s, text)) {
            o << "\n// ---- BEGIN: " << std::string(s.begin(), s.end()) << "\n";
            o << text;
            o << "\n// ---- END\n";
        }
    }

    return outPath;
}

static void SaveSettings()
{
    std::ofstream o(GetSettingsPath(), std::ios::binary);
    if (!o) return;
    if (!g_lastVideoDir.empty()) {
        o << "video=" << WideToUtf8(g_lastVideoDir) << "\n";
    }
    if (!g_lastShaderDir.empty()) {
        o << "shader=" << WideToUtf8(g_lastShaderDir) << "\n";
    }
}

static void LoadSettings()
{
    g_lastVideoDir.clear();
    g_lastShaderDir.clear();
    std::ifstream f(GetSettingsPath(), std::ios::binary);
    if (!f) return;
    std::string line;
    while (std::getline(f, line)) {
        if (line.rfind("video=", 0) == 0) {
            g_lastVideoDir = Utf8ToWide(line.substr(6));
        } else if (line.rfind("shader=", 0) == 0) {
            g_lastShaderDir = Utf8ToWide(line.substr(7));
        }
    }
}

static void UpdateLastVideoDir(const std::wstring& path)
{
    std::wstring dir = Dirname(path);
    if (!dir.empty()) {
        g_lastVideoDir = dir;
        SaveSettings();
    }
}

static void UpdateLastShaderDir(const std::wstring& path)
{
    std::wstring dir = Dirname(path);
    if (!dir.empty()) {
        g_lastShaderDir = dir;
        SaveSettings();
    }
}

static void SaveShaders()
{
    std::ofstream o(GetShadersSavePath(), std::ios::binary);
    if (!o) return;
    for (size_t i = 0; i < g_shaders.size(); ++i) {
        bool bypass = (i < g_shaderBypass.size()) ? g_shaderBypass[i] : false;
        o << (bypass ? "1|" : "0|") << WideToUtf8(g_shaders[i]) << "\n";
    }
}

static void LoadShaders()
{
    g_shaders.clear();
    g_shaderBypass.clear();
    std::ifstream f(GetShadersSavePath(), std::ios::binary);
    if (!f) return;
    std::string line;
    while (std::getline(f, line)) {
        if (line.empty()) continue;
        bool bypass = false;
        if (line.size() > 2 && (line[0] == '0' || line[0] == '1') && line[1] == '|') {
            bypass = (line[0] == '1');
            line = line.substr(2);
        }
        std::wstring w = Utf8ToWide(line);
        if (w.empty()) continue;
        if (IsShaderFile(w)) {
            g_shaders.push_back(w);
            g_shaderBypass.push_back(bypass);
        }
    }
}

static void MoveShader(int from, int to)
{
    if (from < 0 || to < 0 || from >= (int)g_shaders.size() || to >= (int)g_shaders.size()) return;
    auto item = g_shaders[from];
    g_shaders.erase(g_shaders.begin() + from);
    g_shaders.insert(g_shaders.begin() + to, item);
    bool b = g_shaderBypass[from];
    g_shaderBypass.erase(g_shaderBypass.begin() + from);
    g_shaderBypass.insert(g_shaderBypass.begin() + to, b);
    ListRefresh();
    SendMessageW(g_hwndList, LB_SETCURSEL, to, 0);
    MpvApplyShaderList();
    SaveShaders();
}

static void EditShaderInNotepad(const std::wstring& path)
{
    std::wstring npp1 = L"C:\\Program Files\\Notepad++\\notepad++.exe";
    std::wstring npp2 = L"C:\\Program Files (x86)\\Notepad++\\notepad++.exe";
    std::wstring npp;
    if (GetFileAttributesW(npp1.c_str()) != INVALID_FILE_ATTRIBUTES) npp = npp1;
    else if (GetFileAttributesW(npp2.c_str()) != INVALID_FILE_ATTRIBUTES) npp = npp2;

    if (npp.empty()) {
        const wchar_t* url = L"https://notepad-plus-plus.org/downloads/";
        MessageBoxW(g_hwndMain, L"Notepad++ not found.\nDownload: https://notepad-plus-plus.org/downloads/", L"Notepad++", MB_OK | MB_ICONINFORMATION);
        ShellExecuteW(nullptr, L"open", url, nullptr, nullptr, SW_SHOWNORMAL);
        return;
    }
    ShellExecuteW(nullptr, L"open", npp.c_str(), Quote(path).c_str(), nullptr, SW_SHOWNORMAL);
}

static bool FindFfmpeg(std::wstring& outFfmpeg)
{
    // Prefer ffmpeg.exe in dist\deps; then next to exe; else rely on PATH.
    std::wstring exeDir = GetExeDir();
    std::wstring deps = JoinPath(exeDir, L"deps");
    std::wstring depsFfmpeg = JoinPath(deps, L"ffmpeg.exe");
    DWORD attrsDeps = GetFileAttributesW(depsFfmpeg.c_str());
    if (attrsDeps != INVALID_FILE_ATTRIBUTES && !(attrsDeps & FILE_ATTRIBUTE_DIRECTORY)) {
        outFfmpeg = depsFfmpeg;
        return true;
    }
    std::wstring local = JoinPath(exeDir, L"ffmpeg.exe");
    DWORD attrs = GetFileAttributesW(local.c_str());
    if (attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY)) {
        outFfmpeg = local;
        return true;
    }
    outFfmpeg = L"ffmpeg.exe";
    return true;
}

// ----------------------------
// mpv integration
// ----------------------------
static void MpvApplyShaderList()
{
    if (!g_mpv) return;

    // Build mpv_node array of strings for "glsl-shaders"
    mpv_node node{};
    node.format = MPV_FORMAT_NODE_ARRAY;

    mpv_node_list list{};
    std::vector<mpv_node> elems;
    size_t activeCount = 0;
    for (size_t i = 0; i < g_shaders.size(); ++i) {
        if (i < g_shaderBypass.size() && g_shaderBypass[i]) continue;
        activeCount++;
    }
    elems.resize(activeCount);

    // mpv expects UTF-8 strings
    std::vector<std::string> utf8;
    utf8.reserve(activeCount);

    size_t outIdx = 0;
    for (size_t i = 0; i < g_shaders.size(); ++i) {
        if (i < g_shaderBypass.size() && g_shaderBypass[i]) continue;
        int len = WideCharToMultiByte(CP_UTF8, 0, g_shaders[i].c_str(), -1, nullptr, 0, nullptr, nullptr);
        std::string s(len > 0 ? len-1 : 0, '\0');
        if (len > 1) WideCharToMultiByte(CP_UTF8, 0, g_shaders[i].c_str(), -1, s.data(), len, nullptr, nullptr);
        utf8.push_back(s);

        elems[outIdx].format = MPV_FORMAT_STRING;
        elems[outIdx].u.string = (char*)utf8[outIdx].c_str();
        outIdx++;
    }

    list.num = (int)elems.size();
    list.values = elems.data();
    node.u.list = &list;

    // This overwrites the shader list with our current vector
    mpv_set_property(g_mpv, "glsl-shaders", MPV_FORMAT_NODE, &node);

    mpv_command_string(g_mpv, "show-text \"Shaders updated\"");
}

static void MpvLoadVideo(const std::wstring& path)
{
    if (!g_mpv) return;

    g_loadedVideo = path;
    UpdateLastVideoDir(path);

    int pause = 1;
    mpv_set_property(g_mpv, "pause", MPV_FORMAT_FLAG, &pause);

    int len = WideCharToMultiByte(CP_UTF8, 0, path.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string u8(len > 0 ? len-1 : 0, '\0');
    if (len > 1) WideCharToMultiByte(CP_UTF8, 0, path.c_str(), -1, u8.data(), len, nullptr, nullptr);

    const char* cmd[] = {"loadfile", u8.c_str(), "replace", nullptr};
    mpv_command(g_mpv, cmd);

    pause = 1;
    mpv_set_property(g_mpv, "pause", MPV_FORMAT_FLAG, &pause);
    UpdatePlayPauseLabel();
    SetStatus(L"Loaded: " + path);
}

static void MpvTogglePause()
{
    if (!g_mpv) return;
    int pause = 0;
    mpv_get_property(g_mpv, "pause", MPV_FORMAT_FLAG, &pause);
    pause = !pause;
    mpv_set_property(g_mpv, "pause", MPV_FORMAT_FLAG, &pause);
    UpdatePlayPauseLabel();
}

static void MpvFrameStep(bool backwards)
{
    if (!g_mpv) return;
    mpv_command_string(g_mpv, backwards ? "frame-back-step" : "frame-step");
}

static void OpenVideoDialog()
{
    wchar_t file[MAX_PATH] = {};
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = g_hwndMain;
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    std::wstring initialDir = g_lastVideoDir.empty() ? GetExeDir() : g_lastVideoDir;
    ofn.lpstrInitialDir = initialDir.c_str();
    ofn.lpstrFilter = L"Video Files\0*.mp4;*.mkv;*.mov;*.avi;*.ts;*.m2ts;*.webm;*.m4v\0All Files\0*.*\0";
    ofn.nFilterIndex = 1;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    if (GetOpenFileNameW(&ofn)) {
        MpvLoadVideo(file);
    }
}

static void OpenShaderDialog()
{
    wchar_t buffer[4096] = {};
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = g_hwndMain;
    ofn.lpstrFile = buffer;
    ofn.nMaxFile = (DWORD)sizeof(buffer) / sizeof(wchar_t);
    std::wstring initialDir = g_lastShaderDir.empty() ? GetExeDir() : g_lastShaderDir;
    ofn.lpstrInitialDir = initialDir.c_str();
    ofn.lpstrFilter = L"Shader Files\0*.glsl;*.hook.glsl;*.hook;*.frag;*.fs\0All Files\0*.*\0";
    ofn.nFilterIndex = 1;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_ALLOWMULTISELECT | OFN_EXPLORER;
    if (!GetOpenFileNameW(&ofn)) return;

    std::wstring dir = buffer;
    const wchar_t* p = buffer + dir.size() + 1;
    if (*p == L'\0') {
        AddShaderPath(dir);
        return;
    }
    while (*p) {
        std::wstring name = p;
        AddShaderPath(JoinPath(dir, name));
        p += name.size() + 1;
    }
}

static bool MpvInit(HWND hwndVideo)
{
    g_mpv = mpv_create();
    if (!g_mpv) return false;

    // Keep UI snappy / no console noise
    mpv_set_option_string(g_mpv, "terminal", "no");
    mpv_set_option_string(g_mpv, "msg-level", "all=no");

    // Tell mpv to render inside our child window
    int64_t wid = (int64_t)(intptr_t)hwndVideo;
    mpv_set_option(g_mpv, "wid", MPV_FORMAT_INT64, &wid);

    // Useful defaults
    mpv_set_option_string(g_mpv, "keep-open", "yes");
    mpv_set_option_string(g_mpv, "loop-file", "inf");

    if (mpv_initialize(g_mpv) < 0) return false;

    return true;
}

static void MpvShutdown()
{
    if (g_mpv) {
        mpv_terminate_destroy(g_mpv);
        g_mpv = nullptr;
    }
}

// ----------------------------
// UI list helpers
// ----------------------------
static void ListRefresh()
{
    SendMessageW(g_hwndList, LB_RESETCONTENT, 0, 0);
    for (size_t i = 0; i < g_shaders.size(); ++i) {
        std::wstring name = FilenameOnly(g_shaders[i]);
        if (i < g_shaderBypass.size() && g_shaderBypass[i]) {
            name += L" (Bypassed)";
        }
        SendMessageW(g_hwndList, LB_ADDSTRING, 0, (LPARAM)name.c_str());
    }
}

static void AddShaderPath(const std::wstring& path)
{
    if (std::find(g_shaders.begin(), g_shaders.end(), path) != g_shaders.end())
        return;

    g_shaders.push_back(path);
    g_shaderBypass.push_back(false);
    UpdateLastShaderDir(path);
    ListRefresh();
    MpvApplyShaderList();
    SaveShaders();
}

static void RemoveSelectedShader()
{
    int sel = (int)SendMessageW(g_hwndList, LB_GETCURSEL, 0, 0);
    if (sel == LB_ERR) return;

    g_shaders.erase(g_shaders.begin() + sel);
    if (sel >= 0 && sel < (int)g_shaderBypass.size()) {
        g_shaderBypass.erase(g_shaderBypass.begin() + sel);
    }
    ListRefresh();
    MpvApplyShaderList();
    SaveShaders();
}

static void ClearShaders()
{
    g_shaders.clear();
    g_shaderBypass.clear();
    ListRefresh();
    MpvApplyShaderList();
    SaveShaders();
}

static std::vector<std::wstring> GetActiveShaders()
{
    std::vector<std::wstring> out;
    out.reserve(g_shaders.size());
    for (size_t i = 0; i < g_shaders.size(); ++i) {
        if (i < g_shaderBypass.size() && g_shaderBypass[i]) continue;
        out.push_back(g_shaders[i]);
    }
    return out;
}

// ----------------------------
// Encoding (ffmpeg + libplacebo)
// ----------------------------
static bool GetMpvVideoSize(int& w, int& h)
{
    w = h = 0;
    if (!g_mpv) return false;

    // mpv properties "width"/"height" refer to current video size
    mpv_get_property(g_mpv, "width", MPV_FORMAT_INT64, &w);
    mpv_get_property(g_mpv, "height", MPV_FORMAT_INT64, &h);
    return (w > 0 && h > 0);
}

static int GetInputBitrateMbps()
{
    if (!g_mpv) return 0;
    int64_t bps = 0;
    if (mpv_get_property(g_mpv, "video-bitrate", MPV_FORMAT_INT64, &bps) >= 0 && bps > 0) {
        int mbps = (int)((bps + 500000) / 1000000);
        if (mbps < 1) mbps = 1;
        return mbps;
    }

    // Fallback: estimate from file size and duration
    double duration = 0.0;
    if (mpv_get_property(g_mpv, "duration", MPV_FORMAT_DOUBLE, &duration) >= 0 && duration > 0.0) {
        WIN32_FILE_ATTRIBUTE_DATA fad{};
        if (GetFileAttributesExW(g_loadedVideo.c_str(), GetFileExInfoStandard, &fad)) {
            ULARGE_INTEGER sz{};
            sz.HighPart = fad.nFileSizeHigh;
            sz.LowPart = fad.nFileSizeLow;
            double bits = (double)sz.QuadPart * 8.0;
            int mbps = (int)((bits / duration) / 1000000.0 + 0.5);
            if (mbps < 1) mbps = 1;
            return mbps;
        }
    }

    // Final fallback: ask ffmpeg for container bitrate
    std::wstring ffmpeg;
    if (FindFfmpeg(ffmpeg)) {
        int kbps = ProbeBitrateKbpsWithFfmpeg(ffmpeg, g_loadedVideo);
        if (kbps > 0) {
            int mbps = (kbps + 500) / 1000;
            if (mbps < 1) mbps = 1;
            return mbps;
        }
    }

    return 0;
}

static void RunEncode(bool to1440p)
{
    if (g_loadedVideo.empty()) {
        SetStatus(L"No video loaded.");
        return;
    }

    std::wstring ffmpeg;
    FindFfmpeg(ffmpeg);

    // Combine shaders into one file for libplacebo custom_shader_path
    std::wstring combinedName;
    std::vector<std::wstring> activeShaders = GetActiveShaders();
    std::wstring combined = WriteCombinedShaderTemp(activeShaders, &combinedName);

    // Output file
    std::wstring dir = Dirname(g_loadedVideo);
    std::wstring base = BasenameNoExt(g_loadedVideo);
    std::wstring out = JoinPath(dir, base + (to1440p ? L"_shaded_1440p.mp4" : L"_shaded.mp4"));

    // Build libplacebo filter string
    std::wstringstream vf;
    if (!combined.empty()) {
        // libplacebo supports mpv .hook shaders via custom_shader_path :contentReference[oaicite:3]{index=3}
        const std::wstring& shaderArg = combinedName.empty() ? combined : combinedName;
        vf << L"libplacebo=custom_shader_path=" << FfmpegEscapeFilterValue(shaderArg);
    } else {
        vf << L"libplacebo";
    }

    if (to1440p) {
        // Compute aspect-correct width for 1440p, keep even width
        int iw=0, ih=0;
        if (!GetMpvVideoSize(iw, ih) || ih <= 0) {
            iw = 1920; ih = 1080; // fallback
        }
        int outH = 1440;
        int outW = (int)((double)iw * (double)outH / (double)ih + 0.5);
        outW &= ~1; // make even

        // libplacebo can scale using w/h parameters; see docs/examples :contentReference[oaicite:4]{index=4}
        // We just append another libplacebo stage to scale (clean and GPU-friendly).
        vf << L",libplacebo=w=" << outW << L":h=" << outH;
    }

    std::vector<std::wstring> encoders;
    if (g_encoderChoice != L"auto") {
        encoders.push_back(g_encoderChoice);
    } else {
        encoders = {
            L"hevc_amf",
            L"hevc_nvenc",
            L"hevc_qsv",
            L"hevc_mf",
            L"libx265"
        };
    }

    int targetMbps = g_bitrateMbps;
    if (targetMbps <= 0) {
        targetMbps = GetInputBitrateMbps();
        if (targetMbps <= 0) targetMbps = 20;
    }

    // Log path next to exe (helps troubleshooting ffmpeg failures).
    std::wstring logPath = JoinPath(GetExeDir(), BasenameNoExt(out) + L".log");

    SetStatus(L"Encoding...");

    double durationSec = GetMpvDurationSeconds();

    // Run in background thread
    std::thread([ffmpeg, vfStr = vf.str(), out, logPath, encoders, targetMbps, combined, durationSec]() {
        STARTUPINFOW si{};
        si.cb = sizeof(si);
        PROCESS_INFORMATION pi{};

        SECURITY_ATTRIBUTES sa{};
        sa.nLength = sizeof(sa);
        sa.bInheritHandle = TRUE;
        sa.lpSecurityDescriptor = nullptr;

        HANDLE hLog = CreateFileW(
            logPath.c_str(),
            GENERIC_WRITE,
            FILE_SHARE_READ,
            &sa,
            CREATE_ALWAYS,
            FILE_ATTRIBUTE_NORMAL,
            nullptr
        );

        if (hLog != INVALID_HANDLE_VALUE) {
            si.dwFlags |= STARTF_USESTDHANDLES;
            si.hStdOutput = hLog;
            si.hStdError = hLog;
            si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
        }

        bool success = false;
        std::wstring workDir = GetExeDir();

        for (const auto& enc : encoders) {
            std::wstring cmd =
                Quote(ffmpeg) + L" -hide_banner -y -i " + Quote(g_loadedVideo) +
                L" -vf " + Quote(vfStr) + L" " + BuildEncoderArgs(enc, targetMbps) +
                L" -c:a copy ";
            if (durationSec > 0.0) {
                cmd += L"-progress pipe:1 -nostats ";
            }
            cmd += Quote(out);

            if (hLog != INVALID_HANDLE_VALUE) {
                SetFilePointer(hLog, 0, nullptr, FILE_END);
                WriteLogLine(hLog, L"\r\n=== Attempt encoder: " + enc + L" ===\r\n");
                WriteLogLine(hLog, cmd + L"\r\n");
            }

            std::wstring status = L"Encoding (" + enc + L")...";
            PostMessageW(g_hwndMain, WM_APP + 1, 0, (LPARAM)new std::wstring(status));

            HANDLE hRead = nullptr;
            HANDLE hWrite = nullptr;
            if (!CreatePipe(&hRead, &hWrite, &sa, 0)) {
                continue;
            }
            SetHandleInformation(hRead, HANDLE_FLAG_INHERIT, 0);
            si.hStdOutput = hWrite;
            si.hStdError = hWrite;

            // CreateProcess wants mutable buffer
            std::wstring mutableCmd = cmd;

            BOOL ok = CreateProcessW(
                nullptr,
                mutableCmd.data(),
                nullptr, nullptr,
                TRUE,
                CREATE_NO_WINDOW,
                nullptr,
                workDir.c_str(),
                &si, &pi
            );

            CloseHandle(hWrite);
            if (!ok) {
                CloseHandle(hRead);
                continue;
            }

            std::string buffer;
            buffer.reserve(8192);
            char chunk[4096];
            DWORD read = 0;
            double lastPct = -1.0;

            while (ReadFile(hRead, chunk, sizeof(chunk), &read, nullptr) && read > 0) {
                if (hLog != INVALID_HANDLE_VALUE) {
                    DWORD written = 0;
                    WriteFile(hLog, chunk, read, &written, nullptr);
                }
                buffer.append(chunk, chunk + read);
                size_t pos = 0;
                while (true) {
                    size_t nl = buffer.find('\n', pos);
                    if (nl == std::string::npos) break;
                    std::string line = buffer.substr(pos, nl - pos);
                    if (!line.empty() && line.back() == '\r') line.pop_back();
                    int64_t outMs = ParseOutTimeMs(line);
                    if (outMs >= 0 && durationSec > 0.0) {
                        double pct = (outMs / (durationSec * 1000000.0)) * 100.0;
                        if (pct > 100.0) pct = 100.0;
                        if (pct - lastPct >= 0.5 || lastPct < 0.0) {
                            wchar_t buf[128];
                            swprintf_s(buf, L"Encoding (%s)... %.1f%%", enc.c_str(), pct);
                            PostMessageW(g_hwndMain, WM_APP + 1, 0, (LPARAM)new std::wstring(buf));
                            lastPct = pct;
                        }
                    }
                    pos = nl + 1;
                }
                if (pos > 0) {
                    buffer.erase(0, pos);
                }
            }
            CloseHandle(hRead);

            WaitForSingleObject(pi.hProcess, INFINITE);

            DWORD exitCode = 1;
            GetExitCodeProcess(pi.hProcess, &exitCode);
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);

            if (exitCode == 0) {
                success = true;
                break;
            }
        }

        if (hLog != INVALID_HANDLE_VALUE) CloseHandle(hLog);

        if (!combined.empty()) {
            DeleteFileW(combined.c_str());
        }

        if (success) {
            PostMessageW(g_hwndMain, WM_APP + 1, 0, (LPARAM)new std::wstring(L"Done: " + out));
        } else {
            PostMessageW(g_hwndMain, WM_APP + 1, 0, (LPARAM)new std::wstring(L"Encode failed. See log: " + logPath));
        }
    }).detach();
}

// ----------------------------
// Drag reorder listbox subclass
// ----------------------------
static int ListItemFromPoint(HWND hList, POINT ptClient)
{
    // LB_ITEMFROMPOINT returns (index | (outside<<16))
    LRESULT r = SendMessageW(hList, LB_ITEMFROMPOINT, 0, MAKELPARAM(ptClient.x, ptClient.y));
    int idx = (int)LOWORD(r);
    int outside = (int)HIWORD(r);
    if (outside) return -1;
    return idx;
}

static LRESULT CALLBACK ListSubclassProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_LBUTTONDOWN: {
        POINT pt{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        int idx = ListItemFromPoint(hwnd, pt);
        if (idx >= 0 && idx < (int)g_shaders.size()) {
            g_dragging = true;
            g_dragIndex = idx;
            SetCapture(hwnd);
        }
        break;
    }
    case WM_MOUSEMOVE:
        break;
    case WM_RBUTTONDOWN:
        SetFocus(hwnd);
        break;

    case WM_LBUTTONUP: {
        if (g_dragging) {
            ReleaseCapture();
            g_dragging = false;

            POINT pt{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            int drop = ListItemFromPoint(hwnd, pt);
            if (drop >= 0 && drop < (int)g_shaders.size() && g_dragIndex >= 0 && g_dragIndex < (int)g_shaders.size() && drop != g_dragIndex) {
                auto item = g_shaders[g_dragIndex];
                g_shaders.erase(g_shaders.begin() + g_dragIndex);
                g_shaders.insert(g_shaders.begin() + drop, item);

                ListRefresh();
                SendMessageW(hwnd, LB_SETCURSEL, drop, 0);
                MpvApplyShaderList();
                SaveShaders();
            }
            g_dragIndex = -1;
        }
        break;
    }
    }
    return CallWindowProcW(g_listOrigProc, hwnd, msg, wParam, lParam);
}

// ----------------------------
// Drag&Drop handler
// ----------------------------
static void HandleDrop(HDROP hDrop)
{
    UINT count = DragQueryFileW(hDrop, 0xFFFFFFFF, nullptr, 0);
    for (UINT i = 0; i < count; ++i) {
        wchar_t path[MAX_PATH];
        DragQueryFileW(hDrop, i, path, MAX_PATH);

        std::wstring p = path;
        if (IsShaderFile(p)) {
            AddShaderPath(p);
        } else if (IsLikelyVideo(p) || count == 1) {
            // If only one file dropped, try loading as video even if extension isn't in our list
            MpvLoadVideo(p);
        }
    }
    DragFinish(hDrop);
}

// ----------------------------
// UI creation
// ----------------------------
enum {
    ID_BTN_PLAYPAUSE = 1001,
    ID_BTN_ADDVIDEO,
    ID_BTN_ADDSHADER,
    ID_BTN_REMOVE,
    ID_BTN_CLEAR,
    ID_BTN_ENCODE_SAME,
    ID_BTN_ENCODE_1440,
    ID_CB_BITRATE,
    ID_CB_ENCODER,
    ID_CTX_REMOVE = 2001,
    ID_CTX_MOVEUP,
    ID_CTX_MOVEDOWN,
    ID_CTX_EDIT,
    ID_CTX_BYPASS,
};

static void Layout(HWND hwnd)
{
    RECT rc{};
    GetClientRect(hwnd, &rc);

    int pad = 8;
    int rightW = 360;
    int statusH = 22;

    RECT rcStatus = { rc.left + pad, rc.bottom - statusH - pad, rc.right - pad, rc.bottom - pad };
    MoveWindow(g_hwndStatus, rcStatus.left, rcStatus.top, rcStatus.right - rcStatus.left, rcStatus.bottom - rcStatus.top, TRUE);

    int playH = 32;
    int playGap = 6;
    int addVideoH = 28;
    int addGap = 6;
    RECT rcVideo = { rc.left + pad, rc.top + pad, rc.right - rightW - pad*2, rc.bottom - statusH - pad*2 - playH - playGap - addVideoH - addGap };
    MoveWindow(g_hwndVideo, rcVideo.left, rcVideo.top, rcVideo.right - rcVideo.left, rcVideo.bottom - rcVideo.top, TRUE);

    int playW = rcVideo.right - rcVideo.left;
    if (playW > 200) playW = 200;
    int playX = rcVideo.left;
    int playY = rcVideo.bottom + playGap;
    MoveWindow(g_hwndPlayPause, playX, playY, playW, playH, TRUE);

    int addW = playW;
    int addX = playX;
    int addY = playY + playH + addGap;
    MoveWindow(g_hwndAddVideo, addX, addY, addW, addVideoH, TRUE);

    int x = rcVideo.right + pad;
    int y = rc.top + pad;

    int btnH = 28;
    int btnW = rightW - pad;
    int labelH = 18;
    int comboH = 28;
    int encoderH = 28;

    // Buttons
    auto placeBtn = [&](int id, const wchar_t* text) {
        HWND h = GetDlgItem(hwnd, id);
        MoveWindow(h, x, y, btnW, btnH, TRUE);
        y += btnH + 6;
    };

    // right panel starts with Filters label + Add Shader
    MoveWindow(g_hwndFiltersLabel, x, y, btnW - 110, labelH, TRUE);
    MoveWindow(g_hwndAddShader, x + (btnW - 110), y - 2, 110, btnH, TRUE);
    y += labelH + 6;

    // Listbox
    int listH = (rc.bottom - statusH - pad*3) - y - (btnH + 6)*2 - (labelH + 6 + comboH + 8) - (labelH + 6 + encoderH + 8) - 10;
    if (listH < 120) listH = 120;
    MoveWindow(g_hwndList, x, y, btnW, listH, TRUE);
    y += listH + 8;

    // Bitrate label + combo
    MoveWindow(g_hwndBitrateLabel, x, y, btnW, labelH, TRUE);
    y += labelH + 6;
    int comboDropH = comboH * 7;
    MoveWindow(g_hwndBitrate, x, y, btnW, comboDropH, TRUE);
    y += comboH + 8;

    // Encoder label + combo
    MoveWindow(g_hwndEncoderLabel, x, y, btnW, labelH, TRUE);
    y += labelH + 6;
    MoveWindow(g_hwndEncoder, x, y, btnW, comboH * 7, TRUE);
    y += comboH + 8;

    y += 6;
    placeBtn(ID_BTN_ENCODE_SAME, L"Re-encode (same res)");
    placeBtn(ID_BTN_ENCODE_1440, L"Re-encode (1440p)");
}

static void CreateUi(HWND hwnd)
{
    // video child (mpv renders here)
    g_hwndVideo = CreateWindowExW(WS_EX_CLIENTEDGE, L"STATIC", L"",
        WS_CHILD | WS_VISIBLE,
        0, 0, 100, 100, hwnd, nullptr, g_hInst, nullptr);

    g_hwndPlayPause = CreateWindowExW(0, L"BUTTON", L"Play",
        WS_CHILD | WS_VISIBLE,
        0, 0, 100, 28, hwnd, (HMENU)(INT_PTR)ID_BTN_PLAYPAUSE, g_hInst, nullptr);

    g_hwndAddVideo = CreateWindowExW(0, L"BUTTON", L"Add Video",
        WS_CHILD | WS_VISIBLE,
        0, 0, 100, 28, hwnd, (HMENU)(INT_PTR)ID_BTN_ADDVIDEO, g_hInst, nullptr);

    // filters label + listbox for shaders
    g_hwndFiltersLabel = CreateWindowExW(0, L"STATIC", L"Video Filters",
        WS_CHILD | WS_VISIBLE,
        0, 0, 100, 18, hwnd, nullptr, g_hInst, nullptr);

    g_hwndList = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", L"",
        WS_CHILD | WS_VISIBLE | LBS_NOTIFY | WS_VSCROLL | LBS_OWNERDRAWFIXED | LBS_HASSTRINGS,
        0, 0, 100, 100, hwnd, nullptr, g_hInst, nullptr);
    SendMessageW(g_hwndList, LB_SETITEMHEIGHT, 0, 20);

    g_hwndAddShader = CreateWindowExW(0, L"BUTTON", L"Add glsl",
        WS_CHILD | WS_VISIBLE,
        0, 0, 100, 28, hwnd, (HMENU)(INT_PTR)ID_BTN_ADDSHADER, g_hInst, nullptr);

    // subclass for drag reorder
    g_listOrigProc = (WNDPROC)SetWindowLongPtrW(g_hwndList, GWLP_WNDPROC, (LONG_PTR)ListSubclassProc);

    // status
    g_hwndStatus = CreateWindowExW(0, L"STATIC", L"Drop a video file to start.",
        WS_CHILD | WS_VISIBLE,
        0, 0, 100, 22, hwnd, nullptr, g_hInst, nullptr);

    // bitrate label + combo
    g_hwndBitrateLabel = CreateWindowExW(0, L"STATIC", L"Output bitrate",
        WS_CHILD | WS_VISIBLE,
        0, 0, 100, 18, hwnd, nullptr, g_hInst, nullptr);

    g_hwndBitrate = CreateWindowExW(0, L"COMBOBOX", L"",
        WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
        0, 0, 100, 200, hwnd, (HMENU)(INT_PTR)ID_CB_BITRATE, g_hInst, nullptr);

    SendMessageW(g_hwndBitrate, CB_ADDSTRING, 0, (LPARAM)L"Auto");
    SendMessageW(g_hwndBitrate, CB_ADDSTRING, 0, (LPARAM)L"5 Mbps");
    SendMessageW(g_hwndBitrate, CB_ADDSTRING, 0, (LPARAM)L"10 Mbps");
    SendMessageW(g_hwndBitrate, CB_ADDSTRING, 0, (LPARAM)L"20 Mbps");
    SendMessageW(g_hwndBitrate, CB_ADDSTRING, 0, (LPARAM)L"30 Mbps");
    SendMessageW(g_hwndBitrate, CB_ADDSTRING, 0, (LPARAM)L"40 Mbps");
    SendMessageW(g_hwndBitrate, CB_ADDSTRING, 0, (LPARAM)L"50 Mbps");
    SendMessageW(g_hwndBitrate, CB_ADDSTRING, 0, (LPARAM)L"60 Mbps");
    SendMessageW(g_hwndBitrate, CB_ADDSTRING, 0, (LPARAM)L"70 Mbps");
    SendMessageW(g_hwndBitrate, CB_ADDSTRING, 0, (LPARAM)L"80 Mbps");
    SendMessageW(g_hwndBitrate, CB_ADDSTRING, 0, (LPARAM)L"90 Mbps");
    SendMessageW(g_hwndBitrate, CB_ADDSTRING, 0, (LPARAM)L"100 Mbps");
    SendMessageW(g_hwndBitrate, CB_SETCURSEL, 0, 0);
    SendMessageW(g_hwndBitrate, CB_SETITEMHEIGHT, (WPARAM)-1, (LPARAM)22);
    SendMessageW(g_hwndBitrate, CB_SETITEMHEIGHT, 0, (LPARAM)20);

    g_hwndEncoderLabel = CreateWindowExW(0, L"STATIC", L"Encoder",
        WS_CHILD | WS_VISIBLE,
        0, 0, 100, 18, hwnd, nullptr, g_hInst, nullptr);

    g_hwndEncoder = CreateWindowExW(0, L"COMBOBOX", L"",
        WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
        0, 0, 100, 200, hwnd, (HMENU)(INT_PTR)ID_CB_ENCODER, g_hInst, nullptr);

    SendMessageW(g_hwndEncoder, CB_ADDSTRING, 0, (LPARAM)L"Auto");
    SendMessageW(g_hwndEncoder, CB_ADDSTRING, 0, (LPARAM)L"hevc_amf");
    SendMessageW(g_hwndEncoder, CB_ADDSTRING, 0, (LPARAM)L"hevc_nvenc");
    SendMessageW(g_hwndEncoder, CB_ADDSTRING, 0, (LPARAM)L"hevc_qsv");
    SendMessageW(g_hwndEncoder, CB_ADDSTRING, 0, (LPARAM)L"hevc_mf");
    SendMessageW(g_hwndEncoder, CB_ADDSTRING, 0, (LPARAM)L"libx265");
    SendMessageW(g_hwndEncoder, CB_SETCURSEL, 0, 0);
    SendMessageW(g_hwndEncoder, CB_SETITEMHEIGHT, (WPARAM)-1, (LPARAM)22);
    SendMessageW(g_hwndEncoder, CB_SETITEMHEIGHT, 0, (LPARAM)20);

    auto mkBtn = [&](int id, const wchar_t* text) {
        CreateWindowExW(0, L"BUTTON", text,
            WS_CHILD | WS_VISIBLE,
            0, 0, 100, 28, hwnd, (HMENU)(INT_PTR)id, g_hInst, nullptr);
    };

    mkBtn(ID_BTN_ENCODE_SAME, L"Re-encode (same res)");
    mkBtn(ID_BTN_ENCODE_1440, L"Re-encode (1440p)");

    DragAcceptFiles(hwnd, TRUE);
}

// ----------------------------
// WndProc
// ----------------------------
static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_CREATE:
        CreateUi(hwnd);
        Layout(hwnd);
        if (!MpvInit(g_hwndVideo)) {
            MessageBoxW(hwnd, L"Failed to initialize libmpv.\nMake sure mpv-2.dll is present.", L"Error", MB_ICONERROR);
            return -1;
        }
        LoadSettings();
        LoadShaders();
        if (!g_shaders.empty()) {
            ListRefresh();
            MpvApplyShaderList();
        }
        UpdatePlayPauseLabel();
        SetStatus(L"Drop a video file to start. Drop .glsl shader files to add filters.");
        return 0;

    case WM_SIZE:
        Layout(hwnd);
        return 0;

    case WM_DROPFILES:
        HandleDrop((HDROP)wParam);
        return 0;

    case WM_CONTEXTMENU: {
        HWND src = (HWND)wParam;
        if (src == g_hwndList) {
            POINT pt{};
            if (lParam == (LPARAM)-1) {
                int sel = (int)SendMessageW(g_hwndList, LB_GETCURSEL, 0, 0);
                if (sel == LB_ERR) return 0;
                RECT rc{};
                SendMessageW(g_hwndList, LB_GETITEMRECT, sel, (LPARAM)&rc);
                pt.x = rc.left;
                pt.y = rc.bottom;
                ClientToScreen(g_hwndList, &pt);
            } else {
                pt.x = GET_X_LPARAM(lParam);
                pt.y = GET_Y_LPARAM(lParam);
                POINT ptClient = pt;
                ScreenToClient(g_hwndList, &ptClient);
                int idx = ListItemFromPoint(g_hwndList, ptClient);
                if (idx >= 0 && idx < (int)g_shaders.size()) {
                    SendMessageW(g_hwndList, LB_SETCURSEL, idx, 0);
                } else {
                    return 0;
                }
            }

            int sel = (int)SendMessageW(g_hwndList, LB_GETCURSEL, 0, 0);
            std::wstring fullPath;
            bool bypassed = false;
            if (sel >= 0 && sel < (int)g_shaders.size()) {
                fullPath = g_shaders[sel];
                if (sel < (int)g_shaderBypass.size()) bypassed = g_shaderBypass[sel];
            }

            HMENU menu = CreatePopupMenu();
            AppendMenuW(menu, MF_STRING, ID_CTX_REMOVE, L"Remove");
            AppendMenuW(menu, MF_STRING, ID_CTX_MOVEUP, L"Move Up");
            AppendMenuW(menu, MF_STRING, ID_CTX_MOVEDOWN, L"Move Down");
            AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(menu, MF_STRING, ID_CTX_EDIT, L"Edit with Notepad++");
            AppendMenuW(menu, MF_STRING | (bypassed ? MF_CHECKED : 0), ID_CTX_BYPASS, L"Bypass");
            if (!fullPath.empty()) {
                AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
                std::wstring label = L"Path: " + fullPath;
                AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, label.c_str());
            }
            TrackPopupMenu(menu, TPM_LEFTALIGN | TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, nullptr);
            DestroyMenu(menu);
            return 0;
        }
        break;
    }

    case WM_MEASUREITEM: {
        auto* mi = (MEASUREITEMSTRUCT*)lParam;
        if (mi && mi->CtlType == ODT_LISTBOX && mi->CtlID == (UINT)GetDlgCtrlID(g_hwndList)) {
            mi->itemHeight = 20;
            return TRUE;
        }
        break;
    }

    case WM_DRAWITEM: {
        auto* dis = (DRAWITEMSTRUCT*)lParam;
        if (!dis || dis->CtlType != ODT_LISTBOX || dis->hwndItem != g_hwndList) break;
        if (dis->itemID == (UINT)-1) break;

        HDC hdc = dis->hDC;
        RECT rc = dis->rcItem;

        bool bypassed = false;
        if (dis->itemID < g_shaderBypass.size()) bypassed = g_shaderBypass[dis->itemID];

        COLORREF bg = GetSysColor(COLOR_WINDOW);
        COLORREF fg = GetSysColor(COLOR_WINDOWTEXT);
        if (dis->itemState & ODS_SELECTED) {
            bg = GetSysColor(COLOR_HIGHLIGHT);
            fg = GetSysColor(COLOR_HIGHLIGHTTEXT);
        } else if (bypassed) {
            fg = RGB(96, 96, 96);
        }

        HBRUSH bgBrush = CreateSolidBrush(bg);
        FillRect(hdc, &rc, bgBrush);
        DeleteObject(bgBrush);

        int len = (int)SendMessageW(g_hwndList, LB_GETTEXTLEN, dis->itemID, 0);
        std::wstring text;
        if (len > 0) {
            text.resize(len + 1);
            SendMessageW(g_hwndList, LB_GETTEXT, dis->itemID, (LPARAM)text.data());
            text.resize(len);
        }

        SetTextColor(hdc, fg);
        SetBkMode(hdc, TRANSPARENT);
        rc.left += 6;
        DrawTextW(hdc, text.c_str(), (int)text.size(), &rc, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

        if (dis->itemState & ODS_FOCUS) {
            DrawFocusRect(hdc, &dis->rcItem);
        }
        return TRUE;
    }


    case WM_COMMAND: {
        int id = LOWORD(wParam);
        if (id == ID_CB_BITRATE && HIWORD(wParam) == CBN_SELCHANGE) {
            int sel = (int)SendMessageW(g_hwndBitrate, CB_GETCURSEL, 0, 0);
            switch (sel) {
            case 0: g_bitrateMbps = 0; break;
            case 1: g_bitrateMbps = 5; break;
            case 2: g_bitrateMbps = 10; break;
            case 3: g_bitrateMbps = 20; break;
            case 4: g_bitrateMbps = 30; break;
            case 5: g_bitrateMbps = 40; break;
            case 6: g_bitrateMbps = 50; break;
            case 7: g_bitrateMbps = 60; break;
            case 8: g_bitrateMbps = 70; break;
            case 9: g_bitrateMbps = 80; break;
            case 10: g_bitrateMbps = 90; break;
            case 11: g_bitrateMbps = 100; break;
            default: g_bitrateMbps = 0; break;
            }
            return 0;
        }
        if (id == ID_CB_ENCODER && HIWORD(wParam) == CBN_SELCHANGE) {
            int sel = (int)SendMessageW(g_hwndEncoder, CB_GETCURSEL, 0, 0);
            switch (sel) {
            case 0: g_encoderChoice = L"auto"; break;
            case 1: g_encoderChoice = L"hevc_amf"; break;
            case 2: g_encoderChoice = L"hevc_nvenc"; break;
            case 3: g_encoderChoice = L"hevc_qsv"; break;
            case 4: g_encoderChoice = L"hevc_mf"; break;
            case 5: g_encoderChoice = L"libx265"; break;
            default: g_encoderChoice = L"auto"; break;
            }
            return 0;
        }
        switch (id) {
        case ID_BTN_PLAYPAUSE: MpvTogglePause(); break;
        case ID_BTN_ADDVIDEO: OpenVideoDialog(); break;
        case ID_BTN_ADDSHADER: OpenShaderDialog(); break;
        case ID_BTN_ENCODE_SAME: RunEncode(false); break;
        case ID_BTN_ENCODE_1440: RunEncode(true); break;
        case ID_CTX_REMOVE:    RemoveSelectedShader(); break;
        case ID_CTX_MOVEUP: {
            int sel = (int)SendMessageW(g_hwndList, LB_GETCURSEL, 0, 0);
            MoveShader(sel, sel - 1);
            break;
        }
        case ID_CTX_MOVEDOWN: {
            int sel = (int)SendMessageW(g_hwndList, LB_GETCURSEL, 0, 0);
            MoveShader(sel, sel + 1);
            break;
        }
        case ID_CTX_EDIT: {
            int sel = (int)SendMessageW(g_hwndList, LB_GETCURSEL, 0, 0);
            if (sel >= 0 && sel < (int)g_shaders.size()) EditShaderInNotepad(g_shaders[sel]);
            break;
        }
        case ID_CTX_BYPASS: {
            int sel = (int)SendMessageW(g_hwndList, LB_GETCURSEL, 0, 0);
            if (sel >= 0 && sel < (int)g_shaderBypass.size()) {
                g_shaderBypass[sel] = !g_shaderBypass[sel];
                ListRefresh();
                MpvApplyShaderList();
                SaveShaders();
            }
            break;
        }
        }
        return 0;
    }

    case WM_APP + 1: {
        // status update from encode thread
        auto* p = (std::wstring*)lParam;
        if (p) {
            SetStatus(*p);
            delete p;
        }
        return 0;
    }

    case WM_DESTROY:
        MpvShutdown();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR, int nCmdShow)
{
    g_hInst = hInst;

    SetupDllSearchPath();

    INITCOMMONCONTROLSEX icc{};
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&icc);

    const wchar_t* cls = L"VfxEncWnd";
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.lpszClassName = cls;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);

    if (!RegisterClassExW(&wc)) return 0;

    g_hwndMain = CreateWindowExW(
        0, cls, L"VfxEnc - GPU Shader Encoder",
        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
        CW_USEDEFAULT, CW_USEDEFAULT, 1200, 720,
        nullptr, nullptr, hInst, nullptr);

    if (!g_hwndMain) return 0;

    ShowWindow(g_hwndMain, nCmdShow);
    UpdateWindow(g_hwndMain);

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}
