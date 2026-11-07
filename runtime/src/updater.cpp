// In-game updates (see updater.h).
#include <windows.h>
#include <shellapi.h>
#include <winhttp.h>
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <runtime.h>
#include <settings.h>
#include "updater.h"

#ifndef SCDA_VERSION
#define SCDA_VERSION "0.0.0"
#endif
#ifndef SCDA_UPDATE_REPO
#define SCDA_UPDATE_REPO "" // no release feed: update check off
#endif

namespace updater {

namespace {

std::mutex g_mutex;
Status g_status;
std::string g_installerUrl;  // the offered release's TCSCC_Setup.exe
std::wstring g_setupPath;    // the downloaded installer

std::wstring Wide(const std::string& s)
{
    std::wstring w(s.size() + 1, L'\0');
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), int(w.size()));
    w.resize(n > 0 ? size_t(n - 1) : 0);
    return w;
}

std::string Lower(std::string t)
{
    std::transform(t.begin(), t.end(), t.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return t;
}

bool IsExe(const std::string& t) { return t.size() > 4 && Lower(t.substr(t.size() - 4)) == ".exe"; }

// HTTPS GET through WinHTTP (redirects followed: permalink/latest answers 302); `sink` gets the body
// in chunks and returns false to stop. 404 -> `notFound`.
template <typename Sink>
bool HttpGet(const std::wstring& url, std::string& error, bool& notFound, uint64_t& length, Sink sink)
{
    notFound = false;
    URL_COMPONENTS u{ sizeof u };
    wchar_t host[256] = {}, path[2048] = {}, extra[2048] = {};
    u.lpszHostName = host;
    u.dwHostNameLength = 256;
    u.lpszUrlPath = path;
    u.dwUrlPathLength = 2048;
    u.lpszExtraInfo = extra;
    u.dwExtraInfoLength = 2048;
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &u))
    {
        error = "bad address";
        return false;
    }
    struct Handle
    {
        HINTERNET h;
        ~Handle()
        {
            if (h)
                WinHttpCloseHandle(h);
        }
    };
    Handle session{ WinHttpOpen(Wide(std::string("TCSCCRecomp-Updater/") + SCDA_VERSION).c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0) };
    if (!session.h)
    {
        error = "no internet access";
        return false;
    }
    WinHttpSetTimeouts(session.h, 10000, 10000, 15000, 30000);
    Handle connect{ WinHttpConnect(session.h, host, u.nPort, 0) };
    const std::wstring object = std::wstring(path) + extra;
    Handle request{ connect.h ? WinHttpOpenRequest(connect.h, L"GET", object.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                                   u.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0)
                              : nullptr };
    if (!request.h)
    {
        error = "no connection to GitLab";
        return false;
    }
    if (!WinHttpSendRequest(request.h, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(request.h, nullptr))
    {
        error = "no connection to GitLab (error " + std::to_string(GetLastError()) + ")";
        return false;
    }
    DWORD status = 0, size = sizeof status;
    WinHttpQueryHeaders(request.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &size,
                        WINHTTP_NO_HEADER_INDEX);
    if (status == 404)
    {
        notFound = true;
        error = "not found";
        return false;
    }
    if (status != 200)
    {
        error = "GitLab answered " + std::to_string(status) + (status == 403 || status == 429 ? " (rate limit, try later)" : "");
        return false;
    }
    wchar_t len[32] = {};
    size = sizeof len;
    length = WinHttpQueryHeaders(request.h, WINHTTP_QUERY_CONTENT_LENGTH, WINHTTP_HEADER_NAME_BY_INDEX, len, &size, WINHTTP_NO_HEADER_INDEX)
                 ? _wcstoui64(len, nullptr, 10)
                 : 0;
    std::vector<char> buffer(1 << 16);
    for (;;)
    {
        DWORD got = 0;
        if (!WinHttpReadData(request.h, buffer.data(), DWORD(buffer.size()), &got))
        {
            error = "the download broke off";
            return false;
        }
        if (!got)
            return true;
        if (!sink(buffer.data(), size_t(got)))
        {
            if (error.empty())
                error = "could not save the download";
            return false;
        }
    }
}

void Fail(const std::string& error, bool download)
{
    std::lock_guard lock(g_mutex);
    g_status.state = State::Failed;
    g_status.error = error;
    g_status.download = download;
}

void RunCheck()
{
    if (Repo().empty())
    {
        std::lock_guard lock(g_mutex);
        g_status.state = State::NoReleases; // no feed configured ([Update] Repo= or SCDA_UPDATE_REPO)
        return;
    }
    // GitLab wants the project path URL-encoded ("group/name" -> "group%2Fname").
    std::string project;
    for (const char c : Repo())
        project += c == '/' ? std::string("%2F") : std::string(1, c);
    std::string body, error;
    bool notFound = false;
    uint64_t length = 0;
    const bool ok = HttpGet(Wide("https://gitlab.com/api/v4/projects/" + project + "/releases/permalink/latest"), error, notFound, length,
                            [&](const char* d, size_t n) { body.append(d, n); return body.size() < (8u << 20); });
    std::string version, title, url;
    {
        std::lock_guard lock(g_mutex);
        Status& s = g_status;
        s.error.clear();
        s.download = false;
        g_installerUrl.clear();
        if (!ok && notFound)
            s.state = State::NoReleases; // no release yet (or the project is not public)
        else if (!ok)
            s.state = State::Failed, s.error = error;
        else if (!ParseRelease(body, version, title, url))
            s.state = State::Failed, s.error = "the latest release has no version tag";
        else
        {
            s.latest = version;
            s.title = title;
            if (CompareVersions(version, CurrentVersion()) <= 0)
                s.state = State::UpToDate;
            else if (url.empty())
                s.state = State::Failed, s.error = "release " + version + " has no TCSCC_Setup.exe";
            else
                s.state = State::Available, g_installerUrl = url;
        }
        RuntimeLog("[update] %s: state %d latest '%s' (this build %s)%s%s\n", Repo().c_str(), int(s.state), s.latest.c_str(), CurrentVersion(),
                   s.error.empty() ? "" : ": ", s.error.c_str());
    }
}

void RunDownload()
{
    std::string url, version;
    {
        std::lock_guard lock(g_mutex);
        url = g_installerUrl;
        version = g_status.latest;
    }
    const std::filesystem::path folder = SettingsPath().parent_path() / "update";
    std::error_code ec;
    std::filesystem::create_directories(folder, ec);
    const std::wstring path = (folder / (L"TCSCC_Setup_" + Wide(version) + L".exe")).wstring();
    const std::wstring part = path + L".part";
    HANDLE f = CreateFileW(part.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE)
        return Fail("could not write the download", true);
    std::string error;
    bool notFound = false, exe = true;
    uint64_t length = 0, done = 0;
    const bool ok = HttpGet(Wide(url), error, notFound, length, [&](const char* d, size_t n) {
        if (done == 0 && (n < 2 || d[0] != 'M' || d[1] != 'Z')) // GitLab links carry no checksum: at least a Windows program
        {
            exe = false;
            error = "the download is not an installer";
            return false;
        }
        DWORD put = 0;
        if (!WriteFile(f, d, DWORD(n), &put, nullptr) || put != n)
            return false;
        done += n;
        std::lock_guard lock(g_mutex);
        g_status.percent = length ? int(std::min<uint64_t>(done * 100 / length, 99)) : 0;
        return true;
    });
    CloseHandle(f);
    if (ok && length && done != length)
        error = "the download is incomplete";
    if (!ok || !error.empty())
    {
        DeleteFileW(part.c_str());
        RuntimeLog("[update] download failed: %s\n", error.c_str());
        return Fail(error.empty() ? "the download failed" : error, true);
    }
    DeleteFileW(path.c_str());
    if (!MoveFileW(part.c_str(), path.c_str()))
        return Fail("could not save the download", true);
    std::lock_guard lock(g_mutex);
    g_setupPath = path;
    g_status.percent = 100;
    g_status.state = State::Downloaded;
    RuntimeLog("[update] %s downloaded (%llu bytes)\n", version.c_str(), (unsigned long long)done);
}

std::wstring ExeDir(std::wstring* exe = nullptr)
{
    wchar_t self[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    std::wstring dir = self;
    if (exe)
        *exe = dir;
    dir.resize(dir.find_last_of(L'\\'));
    return dir;
}

bool Writable(const std::wstring& dir)
{
    const std::wstring probe = dir + L"\\.tcscc_update_probe";
    HANDLE h = CreateFileW(probe.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return false;
    CloseHandle(h);
    return true;
}

std::string Base64(const std::string& bytes)
{
    static const char* t = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    for (size_t i = 0; i < bytes.size(); i += 3)
    {
        const uint32_t n = (uint8_t(bytes[i]) << 16) | (i + 1 < bytes.size() ? uint8_t(bytes[i + 1]) << 8 : 0) |
                           (i + 2 < bytes.size() ? uint8_t(bytes[i + 2]) : 0);
        out += t[(n >> 18) & 63];
        out += t[(n >> 12) & 63];
        out += i + 1 < bytes.size() ? t[(n >> 6) & 63] : '=';
        out += i + 2 < bytes.size() ? t[n & 63] : '=';
    }
    return out;
}

std::wstring PsQuote(const std::wstring& s)
{
    std::wstring q = L"'";
    for (wchar_t c : s)
    {
        if (c == L'\'')
            q += L'\'';
        q += c;
    }
    return q + L"'";
}

} // namespace

const char* CurrentVersion()
{
    static const char* v = [] {
        const char* e = getenv("SCDA_VERSION_OVERRIDE");
        return e && *e ? e : SCDA_VERSION;
    }();
    return v;
}

const std::string& Repo()
{
    static const std::string repo = [] {
        std::string r = LoadUpdateSettings(SettingsPath()).repo;
        return r.empty() ? std::string(SCDA_UPDATE_REPO) : r;
    }();
    return repo;
}

std::string ReleasePageUrl() { return "https://gitlab.com/" + Repo() + "/-/releases"; }

Status Get()
{
    std::lock_guard lock(g_mutex);
    return g_status;
}

void Check()
{
    {
        std::lock_guard lock(g_mutex);
        if (g_status.state == State::Checking || g_status.state == State::Downloading)
            return;
        g_status.state = State::Checking;
        g_status.error.clear();
        g_status.download = false;
    }
    std::thread(RunCheck).detach();
}

void Download()
{
    {
        std::lock_guard lock(g_mutex);
        if (g_status.state != State::Available && g_status.state != State::Failed)
            return;
        if (!g_installerUrl.empty()) // a newer version found (Available, or Failed in its download)
        {
            g_status.state = State::Downloading;
            g_status.percent = 0;
            g_status.error.clear();
            g_status.download = false;
            std::thread(RunDownload).detach();
            return;
        }
    }
    Check(); // a failed check: nothing to download yet, so check again
}

void OpenReleasePage()
{
    const std::wstring url = Wide(ReleasePageUrl());
    ShellExecuteW(nullptr, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

bool CanInstall() { return GetFileAttributesW((ExeDir() + L"\\game\\default.img").c_str()) != INVALID_FILE_ATTRIBUTES; }

bool Install(std::string& error)
{
    std::wstring setup;
    std::string version;
    {
        std::lock_guard lock(g_mutex);
        if (g_status.state != State::Downloaded)
        {
            error = "nothing downloaded";
            return false;
        }
        setup = g_setupPath;
        version = g_status.latest;
    }
    if (!CanInstall())
    {
        error = "this copy is not an installed game";
        return false;
    }
    std::wstring exe;
    const std::wstring dir = ExeDir(&exe);
    // A hidden PowerShell waits for every tcscc.exe of this folder to close (the game, and its Steam
    // helper, a second tcscc.exe that dies with the game's job), runs the installer's --update
    // (elevated only when the folder is not writable; Windows asks), then starts the game again -
    // also when the update failed or was declined.
    const std::wstring runAs = Writable(dir) ? L"" : L" -Verb RunAs";
    const std::wstring script =
        L"$exe = " + PsQuote(exe) + L"\n"
        L"Wait-Process -Id " + std::to_wstring(GetCurrentProcessId()) + L" -ErrorAction SilentlyContinue\n"
        L"for ($i = 0; $i -lt 50 -and (Get-Process -ErrorAction SilentlyContinue | Where-Object { $_.Path -eq $exe }); $i++) { Start-Sleep -Milliseconds 200 }\n"
        L"$shell = New-Object -ComObject WScript.Shell\n"
        L"try {\n"
        L"  $p = Start-Process -FilePath " + PsQuote(setup) + L" -ArgumentList @('--update', '--dir', " + PsQuote(L"\"" + dir + L"\"") + L")" + runAs +
        L" -Wait -PassThru\n"
        L"  if ($p.ExitCode -ne 0) { [void]$shell.Popup('The update could not be installed. Run ' + " + PsQuote(setup) +
        L" + ' by hand to see why.', 0, 'TCSCC Recomp', 48) }\n"
        L"} catch { [void]$shell.Popup('The update was cancelled.', 0, 'TCSCC Recomp', 64) }\n"
        L"Start-Process -FilePath $exe -WorkingDirectory " + PsQuote(dir) + L"\n";
    const std::string utf16(reinterpret_cast<const char*>(script.data()), script.size() * sizeof(wchar_t));
    std::wstring command =
        L"powershell.exe -NoProfile -NonInteractive -WindowStyle Hidden -ExecutionPolicy Bypass -EncodedCommand " + Wide(Base64(utf16));
    STARTUPINFOW si{ sizeof si };
    PROCESS_INFORMATION pi{};
    // Out of the game's job (the Steam helper's job kills its members when the game exits), when allowed.
    if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW | CREATE_BREAKAWAY_FROM_JOB, nullptr, nullptr, &si, &pi) &&
        !CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
    {
        error = "could not start the installer (error " + std::to_string(GetLastError()) + ")";
        return false;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    RuntimeLog("[update] installing %s: the game closes and restarts\n", version.c_str());
    return true;
}

int CompareVersions(const std::string& a, const std::string& b)
{
    size_t i = !a.empty() && (a[0] == 'v' || a[0] == 'V'), j = !b.empty() && (b[0] == 'v' || b[0] == 'V');
    while (i < a.size() || j < b.size())
    {
        unsigned long x = 0, y = 0;
        while (i < a.size() && isdigit((unsigned char)a[i]))
            x = x * 10 + unsigned(a[i++] - '0');
        while (j < b.size() && isdigit((unsigned char)b[j]))
            y = y * 10 + unsigned(b[j++] - '0');
        if (x != y)
            return x < y ? -1 : 1;
        // Next field: past the '.', stop at anything else (a "-rc1" suffix is ignored).
        i = i < a.size() && a[i] == '.' ? i + 1 : a.size();
        j = j < b.size() && b[j] == '.' ? j + 1 : b.size();
    }
    return 0;
}

std::string JsonString(const std::string& json, const std::string& key)
{
    const std::string quoted = "\"" + key + "\"";
    size_t p = json.find(quoted);
    if (p == std::string::npos)
        return {};
    p = json.find_first_not_of(" \t\r\n", p + quoted.size());
    if (p == std::string::npos || json[p] != ':')
        return {};
    p = json.find_first_not_of(" \t\r\n", p + 1);
    if (p == std::string::npos || json[p] != '"')
        return {};
    std::string out;
    for (p++; p < json.size() && json[p] != '"'; p++)
    {
        if (json[p] == '\\' && p + 1 < json.size())
        {
            const char e = json[++p];
            out += e == 'n' ? '\n' : e == 't' ? '\t' : e == 'u' ? '?' : e;
            if (e == 'u')
                p += 4; // non-ASCII in a release name: shown as '?'
        }
        else
            out += json[p];
    }
    return out;
}

bool ParseRelease(const std::string& json, std::string& version, std::string& title, std::string& installerUrl)
{
    version = JsonString(json, "tag_name");
    if (!version.empty() && (version[0] == 'v' || version[0] == 'V'))
        version.erase(0, 1);
    title = JsonString(json, "name"); // GitLab sends the release's own name first
    installerUrl.clear();
    if (version.empty())
        return false;
    // assets.links: each object at depth 1 of the array (assets.sources are the source archives).
    size_t p = json.find("\"links\"");
    p = p == std::string::npos ? p : json.find('[', p);
    if (p == std::string::npos)
        return true;
    int depth = 0;
    bool inString = false;
    size_t start = 0;
    for (size_t i = p + 1; i < json.size(); i++)
    {
        const char c = json[i];
        if (inString)
        {
            if (c == '\\')
                i++;
            else if (c == '"')
                inString = false;
            continue;
        }
        if (c == '"')
            inString = true;
        else if (c == '{' && depth++ == 0)
            start = i;
        else if (c == '}' && --depth == 0)
        {
            const std::string link = json.substr(start, i + 1 - start);
            const std::string name = JsonString(link, "name"), url = JsonString(link, "url"), direct = JsonString(link, "direct_asset_url");
            const bool named = Lower(name) == "tcscc_setup.exe";
            if (named || (installerUrl.empty() && (IsExe(name) || IsExe(url) || IsExe(direct))))
                installerUrl = direct.empty() ? url : direct;
            if (named)
                break;
        }
        else if (c == ']' && depth == 0)
            break;
    }
    return true;
}

} // namespace updater
