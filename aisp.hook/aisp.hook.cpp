#define WIN32_LEAN_AND_MEAN
// C-style COM vtables: the screen hook patches IWebBrowser2's vtable in place and needs the
// slot layout from the SDK header rather than hand-counted offsets.
#define CINTERFACE
#include <windows.h>
#include <strsafe.h>
#include <tlhelp32.h>
#include <exdisp.h>
#include <oleauto.h>
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
// In-game screen redirect.
//
// Every in-game display (room TVs, channel screens, the Nico Live billboard) is a WebBrowser
// control hosted by the client's statically linked ATL AxHost. The host creates it with
// CoCreateInstance(CLSID_WebBrowser) and calls IWebBrowser2::Navigate exactly once with a URL
// built from the client's own templates, e.g.
//   http://aisp.jp/player/jdfoiajwpefha/nicoplayer.php?movieid=<id>
// aisp.jp no longer belongs to the game, so Navigate is patched to send that host to the
// emulator instead:
//   <base>/aisp.jp/player/jdfoiajwpefha/nicoplayer.php?movieid=<id>
// <base> is "http://" + the download host from connection.txt (line 4), or the
// AISP_SCREEN_BASE environment variable verbatim when set. Other hosts (live.nicovideo.jp)
// are left alone; javascript: navigations pass through untouched.
// ---------------------------------------------------------------------------------------------

using CoCreateInstance_t = HRESULT(WINAPI*)(REFCLSID, LPUNKNOWN, DWORD, REFIID, LPVOID*);
using Navigate_t = HRESULT(STDMETHODCALLTYPE*)(IWebBrowser2*, BSTR, VARIANT*, VARIANT*, VARIANT*, VARIANT*);
using Navigate2_t = HRESULT(STDMETHODCALLTYPE*)(IWebBrowser2*, VARIANT*, VARIANT*, VARIANT*, VARIANT*, VARIANT*);

CoCreateInstance_t g_originalCoCreateInstance = nullptr;
Navigate_t g_originalNavigate = nullptr;
Navigate2_t g_originalNavigate2 = nullptr;
bool g_webBrowserPatched = false;
wchar_t g_screenBase[1024] = {};

const GUID kClsidWebBrowser = {0x8856F961, 0x340A, 0x11D0, {0xA9, 0x6B, 0x00, 0xC0, 0x4F, 0xD7, 0x05, 0xA2}};
const GUID kIidWebBrowser2 = {0xD30C1661, 0xCDAF, 0x11D0, {0x8A, 0x3E, 0x00, 0xC0, 0x4F, 0xC9, 0xE2, 0x6E}};

constexpr wchar_t kScreenHost[] = L"http://aisp.jp";
constexpr wchar_t kScreenSubdir[] = L"/aisp.jp";

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

// connection.txt line 4 is "4,<download host>,# download ip" (written by the launcher).
bool ReadDownloadHost(wchar_t* outHost, size_t outHostCount)
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
        if (line[0] == '4' && line[1] == ',')
        {
            const char* host = line + 2;
            const char* comma = std::strchr(host, ',');
            const char* stop = comma ? comma : (end ? end : host + std::strlen(host));
            while (stop > host && (stop[-1] == '\r' || stop[-1] == ' '))
                --stop;
            const int length = static_cast<int>(stop - host);
            if (length <= 0 || static_cast<size_t>(length) >= outHostCount)
                return false;
            const int converted = MultiByteToWideChar(CP_ACP, 0, host, length, outHost, static_cast<int>(outHostCount) - 1);
            if (converted <= 0)
                return false;
            outHost[converted] = L'\0';
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

    wchar_t host[512] = {};
    if (!ReadDownloadHost(host, 512))
    {
        g_screenBase[0] = L'\0';
        OutputDebugStringW(L"aisp.hook: no download host in connection.txt; screens are not redirected\n");
        return;
    }

    if (FAILED(StringCchPrintfW(g_screenBase, 1024, L"http://%s", host)))
        g_screenBase[0] = L'\0';
    DebugLog(L"aisp.hook: screen base: %s\n", g_screenBase);
}

// Returns a new BSTR with the redirected URL, or nullptr when the URL is not on the screen host.
BSTR RewriteScreenUrl(const wchar_t* url)
{
    if (!url || !g_screenBase[0])
        return nullptr;

    const size_t hostLength = std::wcslen(kScreenHost);
    if (_wcsnicmp(url, kScreenHost, hostLength) != 0)
        return nullptr;

    const wchar_t next = url[hostLength];
    if (next != L'/' && next != L'?' && next != L'#' && next != L'\0')
        return nullptr; // a longer host name such as aisp.jp.example

    const wchar_t* rest = url + hostLength;
    wchar_t rewritten[4096] = {};
    const HRESULT hr = StringCchPrintfW(rewritten, 4096, L"%s%s%s%s", g_screenBase, kScreenSubdir, (*rest == L'/' || *rest == L'\0') ? L"" : L"/", rest);
    if (FAILED(hr))
        return nullptr;

    DebugLog(L"aisp.hook: screen navigate -> %s\n", rewritten);
    return SysAllocString(rewritten);
}

HRESULT STDMETHODCALLTYPE HookNavigate(IWebBrowser2* self, BSTR url, VARIANT* flags, VARIANT* targetFrameName, VARIANT* postData, VARIANT* headers)
{
    BSTR rewritten = RewriteScreenUrl(url);
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
        // The screen hook only concerns the game executable's own import of CoCreateInstance
        // (the ATL host is linked into it); other modules keep the real one.
        InitScreenBase();
        PatchSingleImport(GetModuleHandleW(nullptr), "ole32.dll", "CoCreateInstance", reinterpret_cast<void*>(HookCoCreateInstance), &g_originalCoCreateInstance);
    }
    return TRUE;
}
