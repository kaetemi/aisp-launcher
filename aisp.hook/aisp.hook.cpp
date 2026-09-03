#define WIN32_LEAN_AND_MEAN
// C-style COM vtables: the screen hook patches IWebBrowser2's vtable in place and needs the
// slot layout from the SDK header rather than hand-counted offsets.
#define CINTERFACE
#include <windows.h>
#include <strsafe.h>
#include <tlhelp32.h>
#include <exdisp.h>
#include <mshtml.h>
#include <oleauto.h>
#include <servprov.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <cwctype>
#include <cstring>
#include <cwchar>

namespace
{
constexpr UINT kJapaneseCodePage = 932;
constexpr LCID kJapaneseLcid = 0x0411;
constexpr LANGID kJapaneseLangId = 0x0411;

using GetACP_t = UINT(WINAPI*)();
using GetOEMCP_t = UINT(WINAPI*)();
using GetThreadLocale_t = LCID(WINAPI*)();
using GetUserDefaultLCID_t = LCID(WINAPI*)();
using GetSystemDefaultLCID_t = LCID(WINAPI*)();
using GetUserDefaultLangID_t = LANGID(WINAPI*)();
using GetSystemDefaultLangID_t = LANGID(WINAPI*)();
using MultiByteToWideChar_t = int(WINAPI*)(UINT, DWORD, LPCCH, int, LPWSTR, int);
using WideCharToMultiByte_t = int(WINAPI*)(UINT, DWORD, LPCWCH, int, LPSTR, int, LPCCH, LPBOOL);
using LoadLibraryW_t = HMODULE(WINAPI*)(LPCWSTR);

GetACP_t g_originalGetACP = nullptr;
GetOEMCP_t g_originalGetOEMCP = nullptr;
GetThreadLocale_t g_originalGetThreadLocale = nullptr;
GetUserDefaultLCID_t g_originalGetUserDefaultLCID = nullptr;
GetSystemDefaultLCID_t g_originalGetSystemDefaultLCID = nullptr;
GetUserDefaultLangID_t g_originalGetUserDefaultLangID = nullptr;
GetSystemDefaultLangID_t g_originalGetSystemDefaultLangID = nullptr;
MultiByteToWideChar_t g_originalMultiByteToWideChar = nullptr;
WideCharToMultiByte_t g_originalWideCharToMultiByte = nullptr;
LoadLibraryW_t g_originalLoadLibraryW = nullptr;

UINT WINAPI HookGetACP()
{
    return kJapaneseCodePage;
}

UINT WINAPI HookGetOEMCP()
{
    return kJapaneseCodePage;
}

LCID WINAPI HookGetThreadLocale()
{
    return kJapaneseLcid;
}

LCID WINAPI HookGetUserDefaultLCID()
{
    return kJapaneseLcid;
}

LCID WINAPI HookGetSystemDefaultLCID()
{
    return kJapaneseLcid;
}

LANGID WINAPI HookGetUserDefaultLangID()
{
    return kJapaneseLangId;
}

LANGID WINAPI HookGetSystemDefaultLangID()
{
    return kJapaneseLangId;
}

int WINAPI HookMultiByteToWideChar(UINT codePage, DWORD dwFlags, LPCCH multiByteStr, int cbMultiByte, LPWSTR wideCharStr, int cchWideChar)
{
    if (codePage == CP_ACP || codePage == CP_THREAD_ACP)
        codePage = kJapaneseCodePage;
    return g_originalMultiByteToWideChar ? g_originalMultiByteToWideChar(codePage, dwFlags, multiByteStr, cbMultiByte, wideCharStr, cchWideChar) : 0;
}

int WINAPI HookWideCharToMultiByte(
    UINT codePage,
    DWORD dwFlags,
    LPCWCH wideCharStr,
    int cchWideChar,
    LPSTR multiByteStr,
    int cbMultiByte,
    LPCCH defaultChar,
    LPBOOL usedDefaultChar
)
{
    if (codePage == CP_ACP || codePage == CP_THREAD_ACP)
        codePage = kJapaneseCodePage;
    return g_originalWideCharToMultiByte
               ? g_originalWideCharToMultiByte(codePage, dwFlags, wideCharStr, cchWideChar, multiByteStr, cbMultiByte, defaultChar, usedDefaultChar)
               : 0;
}

bool IsD3d9LibraryPath(LPCWSTR path)
{
    if (!path || !*path)
        return false;

    const wchar_t* fileName = path;
    if (const wchar_t* slash = std::wcsrchr(path, L'\\'))
        fileName = slash + 1;
    if (const wchar_t* slash = std::wcsrchr(fileName, L'/'))
        fileName = slash + 1;

    return _wcsicmp(fileName, L"d3d9.dll") == 0 || _wcsicmp(fileName, L"d3d9") == 0;
}

bool BuildLocalD3d9Path(wchar_t* outPath, size_t outPathCount)
{
    if (!outPath || outPathCount == 0)
        return false;

    wchar_t processPath[MAX_PATH] = {};
    const DWORD processPathLen = GetModuleFileNameW(nullptr, processPath, MAX_PATH);
    if (processPathLen == 0 || processPathLen >= MAX_PATH)
        return false;

    wchar_t* lastSlash = std::wcsrchr(processPath, L'\\');
    if (!lastSlash)
        return false;
    *(lastSlash + 1) = L'\0';

    if (FAILED(StringCchCopyW(outPath, outPathCount, processPath)))
        return false;
    if (FAILED(StringCchCatW(outPath, outPathCount, L"d3d9.dll")))
        return false;

    return GetFileAttributesW(outPath) != INVALID_FILE_ATTRIBUTES;
}

HMODULE WINAPI HookLoadLibraryW(LPCWSTR lpLibFileName)
{
    if (!g_originalLoadLibraryW)
        return nullptr;

    if (IsD3d9LibraryPath(lpLibFileName))
    {
        wchar_t localD3d9Path[MAX_PATH] = {};
        if (BuildLocalD3d9Path(localD3d9Path, sizeof(localD3d9Path) / sizeof(localD3d9Path[0])))
            return g_originalLoadLibraryW(localD3d9Path);
    }

    return g_originalLoadLibraryW(lpLibFileName);
}

// ---------------------------------------------------------------------------------------------
// In-game screens.
//
// Every in-game display (room TVs, channel screens, the Nico Live billboard) is an IE WebBrowser
// control hosted by the client's statically linked ATL AxHost. The host creates it with
// CoCreateInstance(CLSID_WebBrowser) and calls IWebBrowser2::Navigate exactly once with a URL
// built from the client's own templates:
//   http://aisp.jp/player/jdfoiajwpefha/nicoplayer.php?movieid=<id>       a TV given a movie id
//   http://aisp.jp/player/jdfoiajwpefha/nicoplayer.php?tvid=<n>&chid=<n>  a channel screen
//   http://live.nicovideo.jp/watch/<id>?npwarn=false#player               the live billboard
// Neither host serves the game any more, so Navigate is patched to send them to the emulator,
// keeping the distinction:
//   <base>room-tv?movieid=<id>         <base>channel-screen?tvid=<n>&chid=<n>
//   <base>live-watch?liveid=<id>       <base>screen?url=<anything else on aisp.jp>
// <base> is http://<download host>/<directory of the download path>/ from connection.txt
// (lines 4 and 5, written by the launcher), e.g. http://host/ai-sp/ or http://host/ai-sp/dev/,
// or the AISP_SCREEN_BASE environment variable verbatim. javascript: navigations pass through.
//
// The client draws a screen by calling OleDraw on the browser's document every frame and
// copying a fixed rectangle out of the result. That import is hooked too: when the page names a
// stream source in its title, the page is left idle and the
// pixels come from ffmpeg instead: twitch:<channel> through streamlink, stream:<url> straight
// into ffmpeg. Video arrives as raw BGRA
// frames at the crop size on ffmpeg's stdout, audio as 32-bit float at the device's mix format
// on a named pipe, and both go through jitter buffers: the audio device is the clock and the
// presenter shows the frame matching the samples played. Volume and mute come from the page,
// which publishes "aisp:vol=<0-100>;mute=<0|1>" in its title when the client calls its
// ext_setVolume / ext_setMute script functions.
//
// Tools: a streamlink install in the game directory (streamlink\bin\streamlink.exe and its bundled
// streamlink\ffmpeg\ffmpeg.exe), or yt-dlp.exe there; AISP_STREAMLINK, AISP_YTDLP and AISP_FFMPEG
// override the paths. Child processes are attached to a job so they die with the game. stderr of
// every tool goes to aisp.screen.log next to the game executable.
// ---------------------------------------------------------------------------------------------

using CoCreateInstance_t = HRESULT(WINAPI*)(REFCLSID, LPUNKNOWN, DWORD, REFIID, LPVOID*);
using OleDraw_t = HRESULT(WINAPI*)(LPUNKNOWN, DWORD, HDC, LPCRECT);
using Navigate_t = HRESULT(STDMETHODCALLTYPE*)(IWebBrowser2*, BSTR, VARIANT*, VARIANT*, VARIANT*, VARIANT*);
using Navigate2_t = HRESULT(STDMETHODCALLTYPE*)(IWebBrowser2*, VARIANT*, VARIANT*, VARIANT*, VARIANT*, VARIANT*);
using OleClose_t = HRESULT(STDMETHODCALLTYPE*)(IOleObject*, DWORD);

CoCreateInstance_t g_originalCoCreateInstance = nullptr;
OleDraw_t g_originalOleDraw = nullptr;
Navigate_t g_originalNavigate = nullptr;
Navigate2_t g_originalNavigate2 = nullptr;
OleClose_t g_originalOleClose = nullptr;
bool g_webBrowserPatched = false;
wchar_t g_screenBase[1024] = {};

const GUID kClsidWebBrowser = {0x8856F961, 0x340A, 0x11D0, {0xA9, 0x6B, 0x00, 0xC0, 0x4F, 0xD7, 0x05, 0xA2}};
const GUID kIidWebBrowser2 = {0xD30C1661, 0xCDAF, 0x11D0, {0x8A, 0x3E, 0x00, 0xC0, 0x4F, 0xC9, 0xE2, 0x6E}};
const GUID kIidOleObject = {0x00000112, 0x0000, 0x0000, {0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
const GUID kIidHtmlDocument2 = {0x332C4425, 0x26CB, 0x11D0, {0xB4, 0x83, 0x00, 0xC0, 0x4F, 0xD9, 0x01, 0x19}};
const GUID kIidServiceProvider = {0x6D5140C1, 0x7436, 0x11CE, {0x80, 0x34, 0x00, 0xAA, 0x00, 0x60, 0x09, 0xFA}};
const GUID kSidWebBrowserApp = {0x0002DF05, 0x0000, 0x0000, {0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
const GUID kIidUnknown = {0x00000000, 0x0000, 0x0000, {0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
const GUID kClsidMMDeviceEnumerator = {0xBCDE0395, 0xE52F, 0x467C, {0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E}};
const GUID kIidMMDeviceEnumerator = {0xA95664D2, 0x9614, 0x4F35, {0xA7, 0x46, 0xDE, 0x8D, 0xB6, 0x36, 0x17, 0xE6}};
const GUID kIidAudioClient = {0x1CB9AD4C, 0xDBFA, 0x4C32, {0xB1, 0x78, 0xC2, 0xF5, 0x68, 0xA7, 0x03, 0xB2}};
const GUID kIidAudioRenderClient = {0xF294ACFC, 0x3146, 0x4483, {0xA7, 0xBF, 0xAD, 0xDC, 0xA7, 0xC2, 0x60, 0xE2}};

constexpr wchar_t kTvHost[] = L"http://aisp.jp";
constexpr wchar_t kLiveHost[] = L"http://live.nicovideo.jp/watch/";

void DebugLog(const wchar_t* format, const wchar_t* arg)
{
    wchar_t line[2048] = {};
    if (SUCCEEDED(StringCchPrintfW(line, 2048, format, arg)))
        OutputDebugStringW(line);
}

bool BuildGameFilePath(const wchar_t* fileName, wchar_t* outPath, size_t outPathCount)
{
    wchar_t processPath[MAX_PATH] = {};
    const DWORD processPathLen = GetModuleFileNameW(nullptr, processPath, MAX_PATH);
    if (processPathLen == 0 || processPathLen >= MAX_PATH)
        return false;

    wchar_t* lastSlash = std::wcsrchr(processPath, L'\\');
    if (!lastSlash)
        return false;
    *(lastSlash + 1) = L'\0';

    return SUCCEEDED(StringCchCopyW(outPath, outPathCount, processPath)) && SUCCEEDED(StringCchCatW(outPath, outPathCount, fileName));
}

// connection.txt lines are "<n>,<value>,# comment"; returns the value of line <n>.
bool ReadConnectionValue(char lineNumber, wchar_t* out, size_t outCount)
{
    wchar_t path[MAX_PATH] = {};
    if (!BuildGameFilePath(L"connection.txt", path, MAX_PATH))
        return false;

    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return false;

    char buffer[4096] = {};
    DWORD read = 0;
    const BOOL ok = ReadFile(file, buffer, sizeof(buffer) - 1, &read, nullptr);
    CloseHandle(file);
    if (!ok || read == 0)
        return false;
    buffer[read] = '\0';

    const char* line = buffer;
    while (line && *line)
    {
        const char* end = std::strchr(line, '\n');
        if (line[0] == lineNumber && line[1] == ',')
        {
            const char* value = line + 2;
            const char* comma = std::strchr(value, ',');
            const char* stop = comma ? comma : (end ? end : value + std::strlen(value));
            while (stop > value && (stop[-1] == '\r' || stop[-1] == ' '))
                --stop;
            const int length = static_cast<int>(stop - value);
            if (length <= 0 || static_cast<size_t>(length) >= outCount)
                return false;
            const int converted = MultiByteToWideChar(CP_ACP, 0, value, length, out, static_cast<int>(outCount) - 1);
            if (converted <= 0)
                return false;
            out[converted] = L'\0';
            return true;
        }
        line = end ? end + 1 : nullptr;
    }
    return false;
}

void InitScreenBase()
{
    if (GetEnvironmentVariableW(L"AISP_SCREEN_BASE", g_screenBase, 1024) > 0 && g_screenBase[0])
    {
        DebugLog(L"aisp.hook: screen base from environment: %s\n", g_screenBase);
        return;
    }

    wchar_t host[512] = {}, downloadPath[512] = {};
    if (!ReadConnectionValue('4', host, 512))
    {
        g_screenBase[0] = L'\0';
        OutputDebugStringW(L"aisp.hook: no download host in connection.txt; screens are not redirected\n");
        return;
    }
    // The download path names the environment's endpoint directory: ai-sp/download.php or
    // ai-sp/dev/download.php. The screen pages live next to it.
    if (!ReadConnectionValue('5', downloadPath, 512))
        StringCchCopyW(downloadPath, 512, L"ai-sp/download.php");
    if (wchar_t* slash = std::wcsrchr(downloadPath, L'/'))
        *slash = L'\0';
    else
        StringCchCopyW(downloadPath, 512, L"ai-sp");
    while (downloadPath[0] == L'/')
        std::wmemmove(downloadPath, downloadPath + 1, std::wcslen(downloadPath));

    if (FAILED(StringCchPrintfW(g_screenBase, 1024, L"http://%s/%s/", host, downloadPath)))
        g_screenBase[0] = L'\0';
    DebugLog(L"aisp.hook: screen base: %s\n", g_screenBase);
}

bool AppendW(wchar_t* buffer, size_t count, const wchar_t* text)
{
    return SUCCEEDED(StringCchCatW(buffer, count, text));
}

bool AppendPercentEncoded(wchar_t* buffer, size_t count, const wchar_t* text)
{
    for (const wchar_t* p = text; *p; ++p)
    {
        const wchar_t c = *p;
        const bool safe = (c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') || (c >= L'0' && c <= L'9') || c == L'-' || c == L'.' || c == L'_' || c == L'~';
        wchar_t piece[16] = {};
        if (safe)
        {
            piece[0] = c;
        }
        else if (c < 0x80)
        {
            StringCchPrintfW(piece, 16, L"%%%02X", static_cast<unsigned>(c));
        }
        else
        {
            char utf8[8] = {};
            const int n = WideCharToMultiByte(CP_UTF8, 0, &c, 1, utf8, 8, nullptr, nullptr);
            wchar_t* out = piece;
            for (int i = 0; i < n; ++i)
                out += swprintf(out, 4, L"%%%02X", static_cast<unsigned char>(utf8[i]));
        }
        if (!AppendW(buffer, count, piece))
            return false;
    }
    return true;
}

// The client keeps the current channel and map in two globals written by its notify_change_map
// handler ([0xA5DAE0] channel, [0xA5DAE4] map; verified in the binary, which loads unrelocated
// at 0x400000). The URL a screen asks for only carries the screen's own ids, so these tell the
// server which map's screens a page is for.
bool ReadClientMapContext(DWORD* mapId, DWORD* channelId)
{
    if (GetModuleHandleW(nullptr) != reinterpret_cast<HMODULE>(0x400000))
        return false;
    MEMORY_BASIC_INFORMATION info = {};
    if (!VirtualQuery(reinterpret_cast<LPCVOID>(0xA5DAE0), &info, sizeof(info)) || info.State != MEM_COMMIT || info.Protect == PAGE_NOACCESS)
        return false;
    *channelId = *reinterpret_cast<volatile DWORD*>(0xA5DAE0);
    *mapId = *reinterpret_cast<volatile DWORD*>(0xA5DAE4);
    return true;
}

// Appends map=<id>&ch=<id> to a screen URL, ahead of any #fragment.
void AppendClientContext(wchar_t* url, size_t count)
{
    DWORD mapId = 0, channelId = 0;
    if (!ReadClientMapContext(&mapId, &channelId))
        return;
    wchar_t fragment[512] = {};
    if (wchar_t* hash = std::wcschr(url, L'#'))
    {
        StringCchCopyW(fragment, 512, hash);
        *hash = L'\0';
    }
    wchar_t context[64] = {};
    StringCchPrintfW(context, 64, L"%smap=%lu&ch=%lu", std::wcschr(url, L'?') ? L"&" : L"?", mapId, channelId);
    AppendW(url, count, context);
    AppendW(url, count, fragment);
}

// Returns a new BSTR with the emulator URL for a screen URL, or nullptr for anything else.
BSTR RewriteScreenUrl(const wchar_t* url)
{
    if (!url || !g_screenBase[0])
        return nullptr;

    wchar_t rewritten[4096] = {};
    StringCchCopyW(rewritten, 4096, g_screenBase);

    const size_t tvHostLength = std::wcslen(kTvHost);
    const size_t liveHostLength = std::wcslen(kLiveHost);
    if (_wcsnicmp(url, kTvHost, tvHostLength) == 0 && (url[tvHostLength] == L'/' || url[tvHostLength] == L'?' || url[tvHostLength] == L'#' || url[tvHostLength] == L'\0'))
    {
        const wchar_t* query = std::wcschr(url, L'?');
        const wchar_t* fragment = std::wcschr(url, L'#');
        const wchar_t* tail = query ? query + 1 : (fragment ? fragment : L"");
        if (query && std::wcsstr(tail, L"movieid="))
        {
            AppendW(rewritten, 4096, L"room-tv?");
            AppendW(rewritten, 4096, tail);
        }
        else if (query && std::wcsstr(tail, L"tvid="))
        {
            AppendW(rewritten, 4096, L"channel-screen?");
            AppendW(rewritten, 4096, tail);
        }
        else
        {
            AppendW(rewritten, 4096, L"screen?url=");
            AppendPercentEncoded(rewritten, 4096, url);
        }
    }
    else if (_wcsnicmp(url, kLiveHost, liveHostLength) == 0)
    {
        const wchar_t* id = url + liveHostLength;
        size_t idLength = 0;
        while (id[idLength] && id[idLength] != L'?' && id[idLength] != L'#' && id[idLength] != L'/')
            ++idLength;
        AppendW(rewritten, 4096, L"live-watch?liveid=");
        wchar_t idCopy[256] = {};
        StringCchCopyNW(idCopy, 256, id, idLength);
        AppendPercentEncoded(rewritten, 4096, idCopy);
        const wchar_t* rest = id + idLength;
        if (*rest == L'?')
        {
            AppendW(rewritten, 4096, L"&");
            AppendW(rewritten, 4096, rest + 1);
        }
        else if (*rest)
        {
            AppendW(rewritten, 4096, rest);
        }
    }
    else
    {
        return nullptr;
    }

    AppendClientContext(rewritten, 4096);
    DebugLog(L"aisp.hook: screen navigate -> %s\n", rewritten);
    return SysAllocString(rewritten);
}

// --- streams ---------------------------------------------------------------------------------

constexpr int kStreamFps = 30;            // ffmpeg's fps filter makes the video constant-rate
constexpr int kBufferFrames = 90;          // 3 s ring
constexpr int kPrerollFrames = 45;         // start after 1.5 s queued
constexpr int kResumeFrames = 20;          // after an underrun, resume once this much is queued
// Full rings block their reader instead of dropping: ffmpeg then blocks on its pipe and
// streamlink's own buffer absorbs the segment bursts, so nothing is ever skipped and the
// latency is bounded by the ring. The audio ring is larger so it never blocks first.
constexpr int kTitlePollFrames = 15;       // read the page title for volume twice a second
constexpr ULONGLONG kAudioHoldMs = 300;    // no draw for this long: silence, nothing advances
constexpr ULONGLONG kIdleStopMs = 3000;    // no draw for this long: stop the decoder processes
constexpr ULONGLONG kIdleFreeMs = 60000;   // no draw for this long: release the rings too

struct ScreenStream
{
    IUnknown* browser = nullptr;         // identity of the WebBrowser this stream belongs to (held)
    IUnknown* document = nullptr;        // the document pointer OleDraw hands us for it (not held)
    IHTMLDocument2* html = nullptr;      // same document as IHTMLDocument2, for the title (held)
    int x = 0, y = 0, width = 0, height = 0; // crop rectangle inside the control
    int videoX = 0, videoY = 0, videoWidth = 0, videoHeight = 0; // where video goes inside the crop
    CRITICAL_SECTION lock;

    // Video ring: frame f lives at slot f % capacity for f in [videoPos, videoWritten).
    BYTE* ring = nullptr;
    DWORD frameBytes = 0;
    int capacity = 0;
    LONGLONG videoWritten = 0;
    LONGLONG videoPos = 0;               // next frame to present; videoPos - 1 is on screen
    bool playing = false;
    LONGLONG nextPresent = 0;            // video-only clock (QPC) when there is no audio
    int titlePoll = 0;
    wchar_t pageSource[512] = {};        // src=... from the page title, applied by OleDraw

    // Audio ring (float32 interleaved): sample s at s % audioCapacity for s in [audioPos, audioWritten).
    bool audioWanted = false;            // a device was found; ffmpeg produces audio
    bool audioActive = false;            // ffmpeg connected the audio pipe; audio is the clock
    UINT32 sampleRate = 0;
    UINT32 channels = 0;
    UINT32 samplesPerFrame = 0;
    float* audioRing = nullptr;
    LONGLONG audioCapacity = 0;          // in samples (frames of channels)
    LONGLONG audioWritten = 0;
    LONGLONG audioPos = 0;               // next sample the device takes
    LONGLONG audioPlayed = 0;            // samples actually out of the speaker (audioPos - device padding)
    float volume = 1.0f;                 // 0..1 from the page, already perceptual
    bool muted = false;
    HANDLE audioPipe = INVALID_HANDLE_VALUE;
    wchar_t audioPipeName[128] = {};
    WAVEFORMATEX* mixFormat = nullptr;
    IAudioClient* audioClient = nullptr;

    // Lifetime: the client draws a screen every frame while it exists, so the last draw time is
    // the lifetime signal. Audio pauses at kAudioHoldMs, the session is torn down at kIdleStopMs
    // and restarted when drawing resumes, and the rings are freed at kIdleFreeMs.
    bool sessionActive = false;
    ULONGLONG lastDraw = 0;
    LONG underruns = 0;                  // audio ran dry (playback held) since the session started
    LONG videoWaits = 0;                 // reader held because the frame ring was full
    LONG audioWaits = 0;                 // reader held because the sample ring was full
    wchar_t status[256] = {};
    wchar_t source[512] = {};
    HANDLE thread = nullptr;
    HANDLE audioReadThread = nullptr;
    HANDLE audioRenderThread = nullptr;
    HANDLE processes[2] = {};
    volatile LONG stop = 0;
    ScreenStream* next = nullptr;
};

ScreenStream* g_streams = nullptr;
CRITICAL_SECTION g_streamsLock;
HANDLE g_job = nullptr;
HANDLE g_toolLog = INVALID_HANDLE_VALUE;
bool g_screenVideoInitialised = false;
LONG g_streamCounter = 0;
HANDLE g_watchdog = nullptr;

void StopSession(ScreenStream* stream);
void FreeRings(ScreenStream* stream);

void LogLine(const char* text)
{
    if (g_toolLog == INVALID_HANDLE_VALUE)
        return;
    DWORD written = 0;
    WriteFile(g_toolLog, text, static_cast<DWORD>(std::strlen(text)), &written, nullptr);
}

// One line per second per running stream: queue depths and positions, to see where a stutter
// comes from (frame ring empty, sample ring empty, reader held on a full ring).
void LogStats(ScreenStream* stream)
{
    char line[512] = {};
    EnterCriticalSection(&stream->lock);
    const LONGLONG queuedFrames = stream->videoWritten - stream->videoPos;
    const LONGLONG queuedSamples = stream->audioWritten - stream->audioPos;
    const int queuedAudioMs = stream->sampleRate ? static_cast<int>(queuedSamples * 1000 / stream->sampleRate) : -1;
    const LONGLONG avDriftMs = (stream->audioActive && stream->samplesPerFrame) ? (stream->audioPlayed / stream->samplesPerFrame - (stream->videoPos - 1)) * 1000 / kStreamFps : 0;
    StringCchPrintfA(
        line,
        512,
        "stats %lu: vq=%lld aq=%dms playing=%d audio=%d vpos=%lld vwritten=%lld aplayed=%lld drift=%lldms underruns=%ld vwaits=%ld awaits=%ld held=%d\r\n",
        static_cast<unsigned long>(GetTickCount64() / 1000),
        queuedFrames,
        queuedAudioMs,
        stream->playing ? 1 : 0,
        stream->audioActive ? 1 : 0,
        stream->videoPos,
        stream->videoWritten,
        stream->audioPlayed,
        avDriftMs,
        stream->underruns,
        stream->videoWaits,
        stream->audioWaits,
        (GetTickCount64() - stream->lastDraw > kAudioHoldMs) ? 1 : 0
    );
    LeaveCriticalSection(&stream->lock);
    LogLine(line);
}

// Tears down sessions nobody draws any more (TV switched off, room left, game minimised).
DWORD WINAPI WatchdogThread(LPVOID)
{
    int ticks = 0;
    for (;;)
    {
        Sleep(500);
        const ULONGLONG now = GetTickCount64();
        const bool logNow = (++ticks % 2) == 0;
        EnterCriticalSection(&g_streamsLock);
        for (ScreenStream* stream = g_streams; stream; stream = stream->next)
        {
            if (logNow && stream->sessionActive)
                LogStats(stream);
            const ULONGLONG idle = now - stream->lastDraw;
            if (stream->sessionActive && idle > kIdleStopMs)
            {
                StopSession(stream);
                DebugLog(L"aisp.hook: screen idle, stream stopped: %s\n", stream->source);
            }
            if (!stream->sessionActive && stream->ring && idle > kIdleFreeMs)
                FreeRings(stream);
        }
        LeaveCriticalSection(&g_streamsLock);
    }
    return 0;
}

void SetStatus(ScreenStream* stream, const wchar_t* text)
{
    EnterCriticalSection(&stream->lock);
    StringCchCopyW(stream->status, 256, text);
    LeaveCriticalSection(&stream->lock);
    DebugLog(L"aisp.hook: screen: %s\n", text);
}

void InitScreenVideo()
{
    if (g_screenVideoInitialised)
        return;
    g_screenVideoInitialised = true;
    InitializeCriticalSection(&g_streamsLock);
    g_watchdog = CreateThread(nullptr, 0, WatchdogThread, nullptr, 0, nullptr);

    g_job = CreateJobObjectW(nullptr, nullptr);
    if (g_job)
    {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits = {};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(g_job, JobObjectExtendedLimitInformation, &limits, sizeof(limits));
    }

    wchar_t logPath[MAX_PATH] = {};
    if (BuildGameFilePath(L"aisp.screen.log", logPath, MAX_PATH))
    {
        SECURITY_ATTRIBUTES inheritable = {sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
        g_toolLog = CreateFileW(logPath, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, &inheritable, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    }
}

// The tool path from the environment variable, or `fallback` relative to the game directory.
bool ToolPath(const wchar_t* variable, const wchar_t* fallback, wchar_t* out, size_t outCount)
{
    if (GetEnvironmentVariableW(variable, out, static_cast<DWORD>(outCount)) == 0 || !out[0])
    {
        if (!BuildGameFilePath(fallback, out, outCount))
            return false;
    }
    return GetFileAttributesW(out) != INVALID_FILE_ATTRIBUTES;
}

// Starts a child with the given standard handles (nullptr = the log file / nothing) and puts it
// in the job. The command line buffer is modified by CreateProcessW.
HANDLE LaunchTool(wchar_t* commandLine, HANDLE stdIn, HANDLE stdOut)
{
    STARTUPINFOW startup = {};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = stdIn ? stdIn : GetStdHandle(STD_INPUT_HANDLE);
    startup.hStdOutput = stdOut ? stdOut : g_toolLog;
    startup.hStdError = g_toolLog;

    PROCESS_INFORMATION info = {};
    if (!CreateProcessW(nullptr, commandLine, nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &info))
        return nullptr;
    if (g_job)
        AssignProcessToJobObject(g_job, info.hProcess);
    CloseHandle(info.hThread);
    return info.hProcess;
}

bool CreateInheritablePipe(HANDLE* readEnd, HANDLE* writeEnd, bool inheritRead)
{
    SECURITY_ATTRIBUTES inheritable = {sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    if (!CreatePipe(readEnd, writeEnd, &inheritable, 1 << 20))
        return false;
    // Only the end the child uses may be inherited, or the pipe never reports EOF.
    SetHandleInformation(inheritRead ? *writeEnd : *readEnd, HANDLE_FLAG_INHERIT, 0);
    return true;
}

// Runs a tool to completion and returns its first stdout line (trimmed) in `out`.
bool RunToolForLine(wchar_t* commandLine, wchar_t* out, size_t outCount)
{
    HANDLE readEnd = nullptr, writeEnd = nullptr;
    if (!CreateInheritablePipe(&readEnd, &writeEnd, false))
        return false;
    HANDLE process = LaunchTool(commandLine, nullptr, writeEnd);
    CloseHandle(writeEnd);
    if (!process)
    {
        CloseHandle(readEnd);
        return false;
    }

    char buffer[8192] = {};
    DWORD total = 0, read = 0;
    while (total < sizeof(buffer) - 1 && ReadFile(readEnd, buffer + total, sizeof(buffer) - 1 - total, &read, nullptr) && read > 0)
        total += read;
    CloseHandle(readEnd);
    WaitForSingleObject(process, 30000);
    CloseHandle(process);

    buffer[total] = '\0';
    char* newline = std::strpbrk(buffer, "\r\n");
    if (newline)
        *newline = '\0';
    if (!buffer[0])
        return false;
    return MultiByteToWideChar(CP_UTF8, 0, buffer, -1, out, static_cast<int>(outCount)) > 0;
}

bool ReadFully(HANDLE pipe, BYTE* buffer, DWORD size, volatile LONG* stop)
{
    DWORD total = 0;
    while (total < size)
    {
        if (*stop)
            return false;
        DWORD read = 0;
        if (!ReadFile(pipe, buffer + total, size - total, &read, nullptr) || read == 0)
            return false;
        total += read;
    }
    return true;
}

// --- audio -----------------------------------------------------------------------------------

// Opens the default render device in shared mode at its mix format so ffmpeg can be told the
// exact sample rate and channel count. Runs before ffmpeg starts.
bool PrepareAudio(ScreenStream* stream)
{
    IMMDeviceEnumerator* enumerator = nullptr;
    IMMDevice* device = nullptr;
    bool ok = false;
    if (SUCCEEDED(CoCreateInstance(kClsidMMDeviceEnumerator, nullptr, CLSCTX_ALL, kIidMMDeviceEnumerator, reinterpret_cast<void**>(&enumerator))) && enumerator)
    {
        if (SUCCEEDED(enumerator->lpVtbl->GetDefaultAudioEndpoint(enumerator, eRender, eConsole, &device)) && device)
        {
            IAudioClient* client = nullptr;
            if (SUCCEEDED(device->lpVtbl->Activate(device, kIidAudioClient, CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&client))) && client)
            {
                WAVEFORMATEX* format = nullptr;
                if (SUCCEEDED(client->lpVtbl->GetMixFormat(client, &format)) && format && format->nChannels > 0 && format->nSamplesPerSec > 0)
                {
                    stream->audioClient = client;
                    stream->mixFormat = format;
                    stream->sampleRate = format->nSamplesPerSec;
                    stream->channels = format->nChannels;
                    stream->samplesPerFrame = stream->sampleRate / kStreamFps;
                    ok = true;
                }
                else
                {
                    if (format)
                        CoTaskMemFree(format);
                    client->lpVtbl->Release(client);
                }
            }
            device->lpVtbl->Release(device);
        }
        enumerator->lpVtbl->Release(enumerator);
    }
    return ok;
}

float GainOf(const ScreenStream* stream)
{
    return stream->muted ? 0.0f : stream->volume;
}

// Pipe -> audio ring. Audio only becomes the clock once ffmpeg has actually opened the pipe.
DWORD WINAPI AudioReadThread(LPVOID parameter)
{
    ScreenStream* stream = static_cast<ScreenStream*>(parameter);
    if (!ConnectNamedPipe(stream->audioPipe, nullptr) && GetLastError() != ERROR_PIPE_CONNECTED)
        return 0;
    if (stream->stop)
        return 0;

    EnterCriticalSection(&stream->lock);
    stream->audioActive = true;
    LeaveCriticalSection(&stream->lock);

    const DWORD chunkSamples = stream->sampleRate / 100; // 10 ms
    const DWORD chunkBytes = chunkSamples * stream->channels * sizeof(float);
    float* chunk = new float[chunkSamples * stream->channels];
    while (ReadFully(stream->audioPipe, reinterpret_cast<BYTE*>(chunk), chunkBytes, &stream->stop))
    {
        EnterCriticalSection(&stream->lock);
        // Never overwrite samples the device has not taken yet: wait for room (back pressure).
        while (!stream->stop && stream->audioWritten + chunkSamples - stream->audioPos > stream->audioCapacity - chunkSamples)
        {
            ++stream->audioWaits;
            LeaveCriticalSection(&stream->lock);
            Sleep(5);
            EnterCriticalSection(&stream->lock);
        }
        for (DWORD i = 0; i < chunkSamples; ++i)
        {
            const LONGLONG slot = (stream->audioWritten + i) % stream->audioCapacity;
            std::memcpy(stream->audioRing + slot * stream->channels, chunk + static_cast<size_t>(i) * stream->channels, stream->channels * sizeof(float));
        }
        stream->audioWritten += chunkSamples;
        LeaveCriticalSection(&stream->lock);
    }
    delete[] chunk;
    EnterCriticalSection(&stream->lock);
    stream->audioActive = false;
    LeaveCriticalSection(&stream->lock);
    return 0;
}

// Audio ring -> WASAPI. Keeps audioPlayed current so the presenter can pick the matching frame.
DWORD WINAPI AudioRenderThread(LPVOID parameter)
{
    ScreenStream* stream = static_cast<ScreenStream*>(parameter);
    IAudioClient* client = stream->audioClient;
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!event)
        return 0;

    const REFERENCE_TIME bufferDuration = 2000000; // 200 ms
    if (FAILED(client->lpVtbl->Initialize(client, AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, bufferDuration, 0, stream->mixFormat, nullptr)))
    {
        SetStatus(stream, L"audio: device initialisation failed; video only");
        CloseHandle(event);
        return 0;
    }
    UINT32 bufferFrames = 0;
    IAudioRenderClient* render = nullptr;
    if (FAILED(client->lpVtbl->SetEventHandle(client, event)) || FAILED(client->lpVtbl->GetBufferSize(client, &bufferFrames)) || FAILED(client->lpVtbl->GetService(client, kIidAudioRenderClient, reinterpret_cast<void**>(&render))) || !render)
    {
        SetStatus(stream, L"audio: render client unavailable; video only");
        CloseHandle(event);
        return 0;
    }
    client->lpVtbl->Start(client);

    const bool isFloat = stream->mixFormat->wBitsPerSample == 32;
    while (!stream->stop)
    {
        if (WaitForSingleObject(event, 200) != WAIT_OBJECT_0)
            continue;
        UINT32 padding = 0;
        if (FAILED(client->lpVtbl->GetCurrentPadding(client, &padding)))
            break;
        const UINT32 want = bufferFrames - padding;
        if (want == 0)
            continue;
        BYTE* out = nullptr;
        if (FAILED(render->lpVtbl->GetBuffer(render, want, &out)) || !out)
            break;

        EnterCriticalSection(&stream->lock);
        const bool held = GetTickCount64() - stream->lastDraw > kAudioHoldMs;
        const bool feed = stream->playing && stream->audioActive && !held;
        const LONGLONG available = stream->audioWritten - stream->audioPos;
        if (feed && available >= static_cast<LONGLONG>(want))
        {
            const float gain = GainOf(stream);
            for (UINT32 i = 0; i < want; ++i)
            {
                const float* src = stream->audioRing + ((stream->audioPos + i) % stream->audioCapacity) * stream->channels;
                for (UINT32 c = 0; c < stream->channels; ++c)
                {
                    const float sample = src[c] * gain;
                    if (isFloat)
                        reinterpret_cast<float*>(out)[i * stream->channels + c] = sample;
                    else
                        reinterpret_cast<short*>(out)[i * stream->channels + c] = static_cast<short>(sample * 32767.0f);
                }
            }
            stream->audioPos += want;
            stream->audioPlayed = stream->audioPos - padding - want;
            LeaveCriticalSection(&stream->lock);
            render->lpVtbl->ReleaseBuffer(render, want, 0);
        }
        else
        {
            if (feed)
            {
                stream->playing = false; // underrun: everything holds until the ring refills
                ++stream->underruns;
            }
            // While held (no draws) nothing advances, so picture and sound resume together.
            LeaveCriticalSection(&stream->lock);
            render->lpVtbl->ReleaseBuffer(render, want, AUDCLNT_BUFFERFLAGS_SILENT);
        }
    }

    client->lpVtbl->Stop(client);
    render->lpVtbl->Release(render);
    CloseHandle(event);
    return 0;
}

// --- decode ----------------------------------------------------------------------------------

DWORD WINAPI StreamThread(LPVOID parameter)
{
    ScreenStream* stream = static_cast<ScreenStream*>(parameter);
    wchar_t ffmpeg[MAX_PATH] = {}, streamlink[MAX_PATH] = {}, ytdlp[MAX_PATH] = {};
    wchar_t command[4096] = {};
    wchar_t message[512] = {};

    if (!ToolPath(L"AISP_FFMPEG", L"streamlink\\ffmpeg\\ffmpeg.exe", ffmpeg, MAX_PATH))
    {
        StringCchPrintfW(message, 512, L"ffmpeg not found: %s", ffmpeg);
        SetStatus(stream, message);
        return 0;
    }

    // Decide what ffmpeg reads: a piped transport stream from streamlink, or a URL.
    HANDLE ffmpegInput = nullptr;
    wchar_t input[2048] = {};
    const bool isTwitch = _wcsnicmp(stream->source, L"twitch:", 7) == 0;
    if (isTwitch)
    {
        const wchar_t* channel = stream->source + 7;
        if (ToolPath(L"AISP_STREAMLINK", L"streamlink\\bin\\streamlink.exe", streamlink, MAX_PATH))
        {
            StringCchPrintfW(message, 512, L"streamlink: twitch.tv/%s", channel);
            SetStatus(stream, message);
            HANDLE readEnd = nullptr, writeEnd = nullptr;
            if (!CreateInheritablePipe(&readEnd, &writeEnd, false))
            {
                SetStatus(stream, L"pipe creation failed");
                return 0;
            }
            // Ads are skipped by default in current streamlink; cap the quality below 720p. No
            // low-latency mode: it chases the live edge with a two segment window and delivers
            // in stalls and bursts, which is what the screen shows; the default edge and a large
            // ring buffer on streamlink's side keep the delivery steady at a few seconds' delay.
            StringCchPrintfW(command, 4096, L"\"%s\" --stdout --ringbuffer-size 64M --stream-sorting-excludes \">=720p\" https://twitch.tv/%s best", streamlink, channel);
            stream->processes[1] = LaunchTool(command, nullptr, writeEnd);
            CloseHandle(writeEnd);
            if (!stream->processes[1])
            {
                CloseHandle(readEnd);
                SetStatus(stream, L"streamlink failed to start (see aisp.screen.log)");
                return 0;
            }
            // streamlink was started while the read end was not inheritable, so it holds only
            // its write end; ffmpeg must now inherit the read end as its stdin.
            SetHandleInformation(readEnd, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
            ffmpegInput = readEnd;
            StringCchCopyW(input, 2048, L"pipe:0");
        }
        else if (ToolPath(L"AISP_YTDLP", L"yt-dlp.exe", ytdlp, MAX_PATH))
        {
            StringCchPrintfW(message, 512, L"yt-dlp: resolving twitch.tv/%s", channel);
            SetStatus(stream, message);
            StringCchPrintfW(command, 4096, L"\"%s\" -g --no-warnings -f \"best[height<=480]/best\" https://twitch.tv/%s", ytdlp, channel);
            if (!RunToolForLine(command, input, 2048) || _wcsnicmp(input, L"http", 4) != 0)
            {
                SetStatus(stream, L"yt-dlp returned no playlist URL (offline? see aisp.screen.log)");
                return 0;
            }
        }
        else
        {
            StringCchPrintfW(message, 512, L"neither %s nor %s found", streamlink, ytdlp);
            SetStatus(stream, message);
            return 0;
        }
    }
    else if (_wcsnicmp(stream->source, L"stream:", 7) == 0)
    {
        StringCchCopyW(input, 2048, stream->source + 7);
    }
    else
    {
        SetStatus(stream, L"unknown source; use twitch:<channel> or stream:<url>");
        return 0;
    }

    if (stream->stop)
        return 0;

    // Audio: a named pipe ffmpeg writes float samples into, at the device's own format.
    wchar_t audioOutput[512] = {};
    if (stream->audioWanted)
    {
        StringCchPrintfW(stream->audioPipeName, 128, L"\\\\.\\pipe\\aisp-screen-%lu-%ld", GetCurrentProcessId(), InterlockedIncrement(&g_streamCounter));
        stream->audioPipe = CreateNamedPipeW(stream->audioPipeName, PIPE_ACCESS_INBOUND, PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT, 1, 1 << 16, 1 << 20, 0, nullptr);
        if (stream->audioPipe != INVALID_HANDLE_VALUE)
        {
            stream->audioReadThread = CreateThread(nullptr, 0, AudioReadThread, stream, 0, nullptr);
            stream->audioRenderThread = CreateThread(nullptr, 0, AudioRenderThread, stream, 0, nullptr);
            // The '?' keeps ffmpeg going for sources without an audio track; the pipe then simply
            // never connects and the video clock takes over.
            StringCchPrintfW(audioOutput, 512, L" -map 0:a:0? -af \"aresample=async=1000:first_pts=0\" -ac %u -ar %u -f f32le \"%s\"", stream->channels, stream->sampleRate, stream->audioPipeName);
        }
    }

    SetStatus(stream, L"ffmpeg: starting");
    HANDLE frameRead = nullptr, frameWrite = nullptr;
    if (!CreateInheritablePipe(&frameRead, &frameWrite, false))
    {
        if (ffmpegInput)
            CloseHandle(ffmpegInput);
        SetStatus(stream, L"pipe creation failed");
        return 0;
    }
    // -y: the audio output is a named pipe that already exists on our side, and ffmpeg would
    // otherwise stop to ask about overwriting it.
    // Letterbox into the video box (the part of the crop the panel actually shows) at a constant
    // frame rate; -re makes ffmpeg consume its input at real time so streamlink's buffer and the
    // pipe, not our frame ring, absorb the source's bursts (a Twitch playlist refresh delivers
    // about two segments at once). Both outputs start at the
    // stream's own zero so frame f pairs with sample f * samplesPerFrame. ffmpeg runs as fast as
    // data arrives; the rings do the pacing.
    StringCchPrintfW(
        command,
        4096,
        L"\"%s\" -hide_banner -loglevel warning -nostdin -y -fflags nobuffer -re -i \"%s\" -map 0:v:0 -vf \"scale=%d:%d:force_original_aspect_ratio=decrease,pad=%d:%d:(ow-iw)/2:(oh-ih)/2,fps=%d:start_time=0\" -f rawvideo -pix_fmt bgra pipe:1%s",
        ffmpeg,
        input,
        stream->videoWidth,
        stream->videoHeight,
        stream->videoWidth,
        stream->videoHeight,
        kStreamFps,
        audioOutput
    );
    stream->processes[0] = LaunchTool(command, ffmpegInput, frameWrite);
    CloseHandle(frameWrite);
    if (ffmpegInput)
        CloseHandle(ffmpegInput);
    if (!stream->processes[0])
    {
        CloseHandle(frameRead);
        SetStatus(stream, L"ffmpeg failed to start (see aisp.screen.log)");
        return 0;
    }

    SetStatus(stream, L"ffmpeg: buffering");
    BYTE* frame = new BYTE[stream->frameBytes];
    while (ReadFully(frameRead, frame, stream->frameBytes, &stream->stop))
    {
        EnterCriticalSection(&stream->lock);
        // Keep the frame on screen (videoPos - 1) intact: wait for room (back pressure).
        while (!stream->stop && stream->videoWritten - stream->videoPos >= stream->capacity - 2)
        {
            ++stream->videoWaits;
            LeaveCriticalSection(&stream->lock);
            Sleep(5);
            EnterCriticalSection(&stream->lock);
        }
        std::memcpy(stream->ring + static_cast<size_t>(stream->videoWritten % stream->capacity) * stream->frameBytes, frame, stream->frameBytes);
        ++stream->videoWritten;
        LeaveCriticalSection(&stream->lock);
    }
    delete[] frame;
    CloseHandle(frameRead);
    if (!stream->stop)
        SetStatus(stream, L"stream ended (see aisp.screen.log)");
    return 0;
}

void StopSession(ScreenStream* stream)
{
    if (!stream->sessionActive)
        return;
    stream->sessionActive = false;
    InterlockedExchange(&stream->stop, 1);
    for (HANDLE& process : stream->processes)
    {
        if (process)
        {
            TerminateProcess(process, 0);
            CloseHandle(process);
            process = nullptr;
        }
    }
    if (stream->audioPipe != INVALID_HANDLE_VALUE)
    {
        // Unblocks a reader stuck in ConnectNamedPipe/ReadFile.
        DisconnectNamedPipe(stream->audioPipe);
        CloseHandle(stream->audioPipe);
        stream->audioPipe = INVALID_HANDLE_VALUE;
    }
    HANDLE threads[3] = {stream->thread, stream->audioReadThread, stream->audioRenderThread};
    for (HANDLE& thread : threads)
    {
        if (thread)
        {
            WaitForSingleObject(thread, 5000);
            CloseHandle(thread);
        }
    }
    stream->thread = stream->audioReadThread = stream->audioRenderThread = nullptr;
    EnterCriticalSection(&stream->lock);
    stream->playing = false;
    stream->audioActive = false;
    LeaveCriticalSection(&stream->lock);
    if (stream->audioClient)
    {
        stream->audioClient->lpVtbl->Release(stream->audioClient);
        stream->audioClient = nullptr;
    }
    if (stream->mixFormat)
    {
        CoTaskMemFree(stream->mixFormat);
        stream->mixFormat = nullptr;
    }
}

void FreeRings(ScreenStream* stream)
{
    EnterCriticalSection(&stream->lock);
    delete[] stream->ring;
    delete[] stream->audioRing;
    stream->ring = nullptr;
    stream->audioRing = nullptr;
    stream->videoWritten = stream->videoPos = 0;
    LeaveCriticalSection(&stream->lock);
}

// Starts (or restarts) decoding the stream's source into its rings.
void StartSession(ScreenStream* stream)
{
    if (stream->sessionActive || !stream->source[0])
        return;
    EnterCriticalSection(&stream->lock);
    stream->videoWritten = stream->videoPos = 0;
    stream->audioWritten = stream->audioPos = stream->audioPlayed = 0;
    stream->playing = false;
    stream->audioActive = false;
    stream->titlePoll = 0;
    stream->underruns = stream->videoWaits = stream->audioWaits = 0;
    if (!stream->ring)
        stream->ring = new BYTE[static_cast<size_t>(stream->frameBytes) * static_cast<size_t>(stream->capacity)]();
    LeaveCriticalSection(&stream->lock);

    stream->audioWanted = PrepareAudio(stream);
    if (stream->audioWanted)
    {
        const LONGLONG audioCapacity = static_cast<LONGLONG>(stream->sampleRate) * kBufferFrames * 3 / 2 / kStreamFps;
        EnterCriticalSection(&stream->lock);
        if (!stream->audioRing || stream->audioCapacity != audioCapacity)
        {
            delete[] stream->audioRing;
            stream->audioCapacity = audioCapacity;
            stream->audioRing = new float[static_cast<size_t>(audioCapacity) * stream->channels]();
        }
        LeaveCriticalSection(&stream->lock);
    }

    StringCchCopyW(stream->status, 256, L"starting");
    stream->lastDraw = GetTickCount64();
    InterlockedExchange(&stream->stop, 0);
    stream->sessionActive = true;
    DebugLog(L"aisp.hook: screen stream: %s\n", stream->source);
    stream->thread = CreateThread(nullptr, 0, StreamThread, stream, 0, nullptr);
    if (!stream->thread)
    {
        stream->sessionActive = false;
        SetStatus(stream, L"thread creation failed");
    }
}

IUnknown* IdentityOf(IUnknown* object)
{
    IUnknown* identity = nullptr;
    if (object && SUCCEEDED(object->lpVtbl->QueryInterface(object, kIidUnknown, reinterpret_cast<void**>(&identity))))
        return identity;
    return nullptr;
}

// Called from the Navigate hook with the URL the client built, before the rewrite. Every screen
// gets an entry; what it plays, if anything, is decided by the page the server serves, which
// publishes the source in its title (read by the OleDraw hook).
void OnScreenNavigate(IWebBrowser2* browser, const wchar_t* url)
{
    if (!url || !browser)
        return;
    InitScreenVideo();

    IUnknown* identity = IdentityOf(reinterpret_cast<IUnknown*>(browser));
    if (!identity)
        return;

    // One entry per browser: a new navigation replaces whatever it streamed before.
    EnterCriticalSection(&g_streamsLock);
    ScreenStream* stream = nullptr;
    for (ScreenStream* existing = g_streams; existing && !stream; existing = existing->next)
        if (existing->browser == identity)
            stream = existing;
    if (stream)
    {
        StopSession(stream);
        identity->lpVtbl->Release(identity);
    }
    else
    {
        stream = new ScreenStream();
        InitializeCriticalSection(&stream->lock);
        stream->browser = identity;
        stream->capacity = kBufferFrames;
        stream->next = g_streams;
        g_streams = stream;
    }
    LeaveCriticalSection(&g_streamsLock);

    // Crop rectangles the client copies out of the control (see the frame routine): live pages
    // (URL with /lv) 950x520 placed at the page's flvplayer_container, which the emulator's
    // page puts at the origin; everything else 486x343 at (9,15). The video box is what the
    // panel shows of that crop: a TV shows all of it, the Stage wall's main LED shows page
    // (7,65)-(550,447) (measured in game with the page's c: outlines; the banner above it,
    // (7,7)-(550,60), is left to the page).
    int x = 9, y = 15, width = 486, height = 343;
    int videoX = 0, videoY = 0, videoWidth = 486, videoHeight = 343;
    if (std::wcsstr(url, L"/lv"))
    {
        x = 0;
        y = 0;
        width = 950;
        height = 520;
        videoX = 7;
        videoY = 65;
        videoWidth = 543;
        videoHeight = 382;
    }
    // Ring frames hold the video box only (a Stage frame would otherwise be 2 MB of mostly black).
    const DWORD frameBytes = static_cast<DWORD>(videoWidth) * static_cast<DWORD>(videoHeight) * 4;
    if (frameBytes != stream->frameBytes)
        FreeRings(stream);
    EnterCriticalSection(&stream->lock);
    stream->x = x;
    stream->y = y;
    stream->width = width;
    stream->height = height;
    stream->videoX = videoX;
    stream->videoY = videoY;
    stream->videoWidth = videoWidth;
    stream->videoHeight = videoHeight;
    stream->frameBytes = frameBytes;
    stream->source[0] = L'\0';
    stream->pageSource[0] = L'\0';
    if (stream->html)
    {
        stream->html->lpVtbl->Release(stream->html);
        stream->html = nullptr;
    }
    stream->document = nullptr; // the navigation brings a new document
    stream->lastDraw = GetTickCount64();
    LeaveCriticalSection(&stream->lock);
}

// The document OleDraw hands us belongs to some WebBrowser; ask it which through its service
// provider and match against the streams. Cached per document pointer afterwards.
ScreenStream* FindStream(IUnknown* document)
{
    if (!document)
        return nullptr;

    EnterCriticalSection(&g_streamsLock);
    ScreenStream* found = nullptr;
    for (ScreenStream* stream = g_streams; stream && !found; stream = stream->next)
        if (stream->document == document)
            found = stream;
    LeaveCriticalSection(&g_streamsLock);
    if (found)
        return found;

    IServiceProvider* provider = nullptr;
    if (FAILED(document->lpVtbl->QueryInterface(document, kIidServiceProvider, reinterpret_cast<void**>(&provider))) || !provider)
        return nullptr;
    IUnknown* browser = nullptr;
    provider->lpVtbl->QueryService(provider, kSidWebBrowserApp, kIidWebBrowser2, reinterpret_cast<void**>(&browser));
    provider->lpVtbl->Release(provider);
    if (!browser)
        return nullptr;
    IUnknown* identity = IdentityOf(browser);
    browser->lpVtbl->Release(browser);
    if (!identity)
        return nullptr;

    EnterCriticalSection(&g_streamsLock);
    for (ScreenStream* stream = g_streams; stream && !found; stream = stream->next)
    {
        if (stream->browser == identity)
        {
            stream->document = document;
            IHTMLDocument2* html = nullptr;
            if (SUCCEEDED(document->lpVtbl->QueryInterface(document, kIidHtmlDocument2, reinterpret_cast<void**>(&html))))
            {
                EnterCriticalSection(&stream->lock);
                if (stream->html)
                    stream->html->lpVtbl->Release(stream->html);
                stream->html = html;
                LeaveCriticalSection(&stream->lock);
            }
            found = stream;
        }
    }
    LeaveCriticalSection(&g_streamsLock);
    identity->lpVtbl->Release(identity);
    return found;
}

// The page publishes "aisp:vol=<0-100>;mute=<0|1>[;src=<source>]" in its title: the volume and
// mute whenever the client's ext_setVolume / ext_setMute reach it, the source as the server
// decided when it served the page. Call under the stream lock.
void PollTitle(ScreenStream* stream)
{
    if (!stream->html || ++stream->titlePoll < kTitlePollFrames)
        return;
    stream->titlePoll = 0;
    BSTR title = nullptr;
    if (FAILED(stream->html->lpVtbl->get_title(stream->html, &title)) || !title)
        return;
    if (std::wcsncmp(title, L"aisp:", 5) == 0)
    {
        // Only sources the hook decodes count; the page frames web pages itself.
        const wchar_t* src = std::wcsstr(title, L";src=");
        const bool stream_ = src && (_wcsnicmp(src + 5, L"twitch:", 7) == 0 || _wcsnicmp(src + 5, L"stream:", 7) == 0);
        StringCchCopyW(stream->pageSource, 512, stream_ ? src + 5 : L"");
    }
    if (const wchar_t* vol = std::wcsstr(title, L"vol="))
    {
        const float percent = static_cast<float>(wcstol(vol + 4, nullptr, 10));
        const float linear = percent < 0 ? 0.0f : (percent > 100 ? 1.0f : percent / 100.0f);
        stream->volume = linear * linear; // perceptual taper
    }
    if (const wchar_t* mute = std::wcsstr(title, L"mute="))
        stream->muted = mute[5] == L'1';
    SysFreeString(title);
}

HRESULT WINAPI HookOleDraw(LPUNKNOWN unknown, DWORD aspect, HDC hdc, LPCRECT bounds)
{
    ScreenStream* stream = g_screenVideoInitialised ? FindStream(unknown) : nullptr;
    if (!stream || !bounds || !hdc)
        return g_originalOleDraw(unknown, aspect, hdc, bounds);
    stream->lastDraw = GetTickCount64();

    // What the page says to play, applied when it changes: start, replace, or stop.
    wchar_t wanted[512] = {};
    EnterCriticalSection(&stream->lock);
    PollTitle(stream);
    StringCchCopyW(wanted, 512, stream->pageSource);
    const bool changed = std::wcscmp(wanted, stream->source) != 0;
    LeaveCriticalSection(&stream->lock);
    if (changed)
    {
        StopSession(stream);
        StringCchCopyW(stream->source, 512, wanted);
    }
    if (!stream->sessionActive)
    {
        // Also the restart after an idle stop (game minimised, or the TV came back).
        if (stream->source[0])
            StartSession(stream);
        else
            return g_originalOleDraw(unknown, aspect, hdc, bounds); // the page is the content
    }

    // When the video box is only part of the crop (the Stage wall's main LED), let the page draw
    // itself first so the rest of the crop, the banner, still shows the page; a TV's video box is
    // the whole crop, so the page draw is skipped there.
    const bool videoFillsCrop = stream->videoX == 0 && stream->videoY == 0 && stream->videoWidth == stream->width && stream->videoHeight == stream->height;
    if (videoFillsCrop)
        FillRect(hdc, bounds, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
    else
        g_originalOleDraw(unknown, aspect, hdc, bounds);
    const int x = bounds->left + stream->x;
    const int y = bounds->top + stream->y;

    EnterCriticalSection(&stream->lock);

    // Pre-roll, then present: with audio, the frame that matches what the speaker has played;
    // without, one frame per interval on the performance counter. Underruns hold the last
    // frame (the audio thread clears `playing`); a full ring holds the reader instead.
    const LONGLONG queued = stream->videoWritten - stream->videoPos;
    const bool hasAudio = stream->audioActive;
    if (!stream->playing)
    {
        const int needed = stream->videoPos == 0 ? kPrerollFrames : kResumeFrames;
        const bool audioReady = !hasAudio || stream->audioWritten - stream->audioPos >= static_cast<LONGLONG>(needed) * stream->samplesPerFrame;
        if (queued >= needed && audioReady)
        {
            stream->playing = true;
            LARGE_INTEGER now = {};
            QueryPerformanceCounter(&now);
            stream->nextPresent = now.QuadPart;
        }
    }
    if (stream->playing)
    {
        if (hasAudio)
        {
            const LONGLONG target = stream->audioPlayed / stream->samplesPerFrame + 1;
            if (target > stream->videoPos)
                stream->videoPos = target < stream->videoWritten ? target : stream->videoWritten;
        }
        else
        {
            LARGE_INTEGER now = {}, frequency = {};
            QueryPerformanceCounter(&now);
            QueryPerformanceFrequency(&frequency);
            const LONGLONG interval = frequency.QuadPart / kStreamFps;
            while (now.QuadPart >= stream->nextPresent)
            {
                if (stream->videoPos >= stream->videoWritten)
                {
                    stream->playing = false;
                    break;
                }
                ++stream->videoPos;
                stream->nextPresent += interval;
            }
        }
    }

    if (stream->videoPos > 0)
    {
        BITMAPINFO info = {};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = stream->videoWidth;
        info.bmiHeader.biHeight = -stream->videoHeight; // top-down, as ffmpeg writes it
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        const BYTE* shown = stream->ring + static_cast<size_t>((stream->videoPos - 1) % stream->capacity) * stream->frameBytes;
        SetDIBitsToDevice(hdc, x + stream->videoX, y + stream->videoY, stream->videoWidth, stream->videoHeight, 0, 0, 0, stream->videoHeight, shown, &info, DIB_RGB_COLORS);
    }
    else
    {
        RECT videoBox = {x + stream->videoX, y + stream->videoY, x + stream->videoX + stream->videoWidth, y + stream->videoY + stream->videoHeight};
        FillRect(hdc, &videoBox, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, RGB(255, 255, 255));
        TextOutW(hdc, videoBox.left + 10, videoBox.top + 10, stream->source, static_cast<int>(std::wcslen(stream->source)));
        TextOutW(hdc, videoBox.left + 10, videoBox.top + 30, stream->status, static_cast<int>(std::wcslen(stream->status)));
    }
    LeaveCriticalSection(&stream->lock);
    return S_OK;
}

// The ATL host calls IOleObject::Close on the control when the client destroys a screen's
// window: the TV switched off or closed, the map unloaded. Stop that screen's stream at once and
// drop the entry; the draw watchdog stays for pauses that are not teardowns.
HRESULT STDMETHODCALLTYPE HookOleClose(IOleObject* self, DWORD saveOption)
{
    if (g_screenVideoInitialised)
    {
        IUnknown* identity = IdentityOf(reinterpret_cast<IUnknown*>(self));
        if (identity)
        {
            ScreenStream* stream = nullptr;
            EnterCriticalSection(&g_streamsLock);
            ScreenStream** link = &g_streams;
            while (*link && !stream)
            {
                if ((*link)->browser == identity)
                {
                    stream = *link;
                    *link = stream->next; // unlinked: the watchdog will not see it again
                }
                else
                {
                    link = &(*link)->next;
                }
            }
            LeaveCriticalSection(&g_streamsLock);
            identity->lpVtbl->Release(identity);

            if (stream)
            {
                DebugLog(L"aisp.hook: screen closed: %s\n", stream->source);
                StopSession(stream);
                FreeRings(stream);
                EnterCriticalSection(&stream->lock);
                if (stream->html)
                {
                    stream->html->lpVtbl->Release(stream->html);
                    stream->html = nullptr;
                }
                stream->document = nullptr;
                stream->source[0] = L'\0';
                LeaveCriticalSection(&stream->lock);
                stream->browser->lpVtbl->Release(stream->browser);
                stream->browser = nullptr;
                // The struct itself is kept: a worker that outlived the 5 s join would still hold it.
            }
        }
    }
    return g_originalOleClose ? g_originalOleClose(self, saveOption) : S_OK;
}

// --- browser patch -----------------------------------------------------------------------------

HRESULT STDMETHODCALLTYPE HookNavigate(IWebBrowser2* self, BSTR url, VARIANT* flags, VARIANT* targetFrameName, VARIANT* postData, VARIANT* headers)
{
    BSTR rewritten = RewriteScreenUrl(url);
    if (rewritten)
        OnScreenNavigate(self, url);
    const HRESULT hr = g_originalNavigate(self, rewritten ? rewritten : url, flags, targetFrameName, postData, headers);
    if (rewritten)
        SysFreeString(rewritten);
    return hr;
}

HRESULT STDMETHODCALLTYPE HookNavigate2(IWebBrowser2* self, VARIANT* url, VARIANT* flags, VARIANT* targetFrameName, VARIANT* postData, VARIANT* headers)
{
    if (url && V_VT(url) == VT_BSTR)
    {
        BSTR rewritten = RewriteScreenUrl(V_BSTR(url));
        if (rewritten)
        {
            OnScreenNavigate(self, V_BSTR(url));
            VARIANT replaced;
            VariantInit(&replaced);
            V_VT(&replaced) = VT_BSTR;
            V_BSTR(&replaced) = rewritten;
            const HRESULT hr = g_originalNavigate2(self, &replaced, flags, targetFrameName, postData, headers);
            SysFreeString(rewritten);
            return hr;
        }
    }
    return g_originalNavigate2(self, url, flags, targetFrameName, postData, headers);
}

// All WebBrowser instances share ieframe's vtable, so one patch covers every screen.
void PatchWebBrowserVtable(IUnknown* unknown)
{
    if (g_webBrowserPatched || !unknown)
        return;

    IWebBrowser2* browser = nullptr;
    if (FAILED(unknown->lpVtbl->QueryInterface(unknown, kIidWebBrowser2, reinterpret_cast<void**>(&browser))) || !browser)
        return;

    IWebBrowser2Vtbl* vtable = browser->lpVtbl;
    DWORD oldProtect = 0;
    if (VirtualProtect(vtable, sizeof(*vtable), PAGE_READWRITE, &oldProtect))
    {
        g_originalNavigate = vtable->Navigate;
        g_originalNavigate2 = vtable->Navigate2;
        vtable->Navigate = HookNavigate;
        vtable->Navigate2 = HookNavigate2;
        DWORD ignored = 0;
        VirtualProtect(vtable, sizeof(*vtable), oldProtect, &ignored);
        g_webBrowserPatched = true;
        OutputDebugStringW(L"aisp.hook: IWebBrowser2::Navigate patched\n");
    }

    browser->lpVtbl->Release(browser);

    IOleObject* oleObject = nullptr;
    if (SUCCEEDED(unknown->lpVtbl->QueryInterface(unknown, kIidOleObject, reinterpret_cast<void**>(&oleObject))) && oleObject)
    {
        IOleObjectVtbl* oleVtable = oleObject->lpVtbl;
        if (VirtualProtect(oleVtable, sizeof(*oleVtable), PAGE_READWRITE, &oldProtect))
        {
            g_originalOleClose = oleVtable->Close;
            oleVtable->Close = HookOleClose;
            DWORD ignored = 0;
            VirtualProtect(oleVtable, sizeof(*oleVtable), oldProtect, &ignored);
            OutputDebugStringW(L"aisp.hook: IOleObject::Close patched\n");
        }
        oleObject->lpVtbl->Release(oleObject);
    }
}

HRESULT WINAPI HookCoCreateInstance(REFCLSID clsid, LPUNKNOWN outer, DWORD context, REFIID iid, LPVOID* out)
{
    if (!g_originalCoCreateInstance)
        return E_FAIL;

    const HRESULT hr = g_originalCoCreateInstance(clsid, outer, context, iid, out);
    if (SUCCEEDED(hr) && out && *out && IsEqualGUID(clsid, kClsidWebBrowser))
        PatchWebBrowserVtable(static_cast<IUnknown*>(*out));
    return hr;
}

template <typename T>
bool PatchSingleImport(HMODULE module, const char* importedModuleName, const char* importName, void* replacement, T* original)
{
    if (!module)
        return false;

    auto base = reinterpret_cast<BYTE*>(module);
    auto dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    if (!dos || dos->e_magic != IMAGE_DOS_SIGNATURE)
        return false;

    auto nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (!nt || nt->Signature != IMAGE_NT_SIGNATURE)
        return false;

    auto importDir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!importDir.VirtualAddress || !importDir.Size)
        return false;

    auto importDesc = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + importDir.VirtualAddress);
    while (importDesc->Name)
    {
        auto moduleName = reinterpret_cast<const char*>(base + importDesc->Name);
        if (_stricmp(moduleName, importedModuleName) == 0)
        {
            auto originalThunk = importDesc->OriginalFirstThunk
                                     ? reinterpret_cast<IMAGE_THUNK_DATA*>(base + importDesc->OriginalFirstThunk)
                                     : reinterpret_cast<IMAGE_THUNK_DATA*>(base + importDesc->FirstThunk);
            auto thunk = reinterpret_cast<IMAGE_THUNK_DATA*>(base + importDesc->FirstThunk);

            while (originalThunk->u1.AddressOfData)
            {
                if ((originalThunk->u1.Ordinal & IMAGE_ORDINAL_FLAG) == 0)
                {
                    auto byName = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + originalThunk->u1.AddressOfData);
                    if (std::strcmp(reinterpret_cast<const char*>(byName->Name), importName) == 0)
                    {
                        DWORD oldProtect = 0;
                        if (!VirtualProtect(&thunk->u1.Function, sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProtect))
                            return false;

                        if (original && !*original)
                            *original = reinterpret_cast<T>(thunk->u1.Function);
                        thunk->u1.Function = reinterpret_cast<ULONG_PTR>(replacement);

                        FlushInstructionCache(GetCurrentProcess(), &thunk->u1.Function, sizeof(void*));

                        DWORD ignored = 0;
                        VirtualProtect(&thunk->u1.Function, sizeof(void*), oldProtect, &ignored);
                        return true;
                    }
                }

                ++originalThunk;
                ++thunk;
            }
        }
        ++importDesc;
    }

    return false;
}

template <typename T>
void PatchImport(HMODULE module, const char* importName, void* replacement, T* original)
{
    PatchSingleImport(module, "KERNEL32.dll", importName, replacement, original);
    PatchSingleImport(module, "KERNELBASE.dll", importName, replacement, original);
}

void PatchModule(HMODULE module)
{
    PatchImport(module, "GetACP", reinterpret_cast<void*>(HookGetACP), &g_originalGetACP);
    PatchImport(module, "GetOEMCP", reinterpret_cast<void*>(HookGetOEMCP), &g_originalGetOEMCP);
    PatchImport(module, "GetThreadLocale", reinterpret_cast<void*>(HookGetThreadLocale), &g_originalGetThreadLocale);
    PatchImport(module, "GetUserDefaultLCID", reinterpret_cast<void*>(HookGetUserDefaultLCID), &g_originalGetUserDefaultLCID);
    PatchImport(module, "GetSystemDefaultLCID", reinterpret_cast<void*>(HookGetSystemDefaultLCID), &g_originalGetSystemDefaultLCID);
    PatchImport(module, "GetUserDefaultLangID", reinterpret_cast<void*>(HookGetUserDefaultLangID), &g_originalGetUserDefaultLangID);
    PatchImport(module, "GetSystemDefaultLangID", reinterpret_cast<void*>(HookGetSystemDefaultLangID), &g_originalGetSystemDefaultLangID);
    PatchImport(module, "MultiByteToWideChar", reinterpret_cast<void*>(HookMultiByteToWideChar), &g_originalMultiByteToWideChar);
    PatchImport(module, "WideCharToMultiByte", reinterpret_cast<void*>(HookWideCharToMultiByte), &g_originalWideCharToMultiByte);
    PatchImport(module, "LoadLibraryW", reinterpret_cast<void*>(HookLoadLibraryW), &g_originalLoadLibraryW);
}

void PatchLoadedModules()
{
    const DWORD processId = GetCurrentProcessId();
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, processId);
    if (snapshot == INVALID_HANDLE_VALUE)
        return;

    MODULEENTRY32 moduleEntry = {};
    moduleEntry.dwSize = sizeof(moduleEntry);

    if (Module32First(snapshot, &moduleEntry))
    {
        do
        {
            PatchModule(moduleEntry.hModule);
        } while (Module32Next(snapshot, &moduleEntry));
    }

    CloseHandle(snapshot);
}
} // namespace

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(instance);
        PatchLoadedModules();
        // The screen hooks only concern the game executable's own imports (the ATL host is
        // linked into it); other modules keep the real functions.
        InitScreenBase();
        PatchSingleImport(GetModuleHandleW(nullptr), "ole32.dll", "CoCreateInstance", reinterpret_cast<void*>(HookCoCreateInstance), &g_originalCoCreateInstance);
        PatchSingleImport(GetModuleHandleW(nullptr), "ole32.dll", "OleDraw", reinterpret_cast<void*>(HookOleDraw), &g_originalOleDraw);
    }
    return TRUE;
}
