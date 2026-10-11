#include "BlenderInstaller.hpp"

#include <winhttp.h>

#include <algorithm>
#include <array>
#include <cwctype>
#include <set>

namespace rm {
namespace {

constexpr uint64_t kMinimumDownloadFreeBytes = 1024ull * 1024 * 1024;
constexpr size_t kCatalogBranchLimit = 6;

class InternetHandle {
public:
    InternetHandle() = default;
    explicit InternetHandle(HINTERNET value) : value_(value) {}
    ~InternetHandle() { if (value_) WinHttpCloseHandle(value_); }
    InternetHandle(const InternetHandle&) = delete;
    InternetHandle& operator=(const InternetHandle&) = delete;
    InternetHandle(InternetHandle&& other) noexcept
        : value_(std::exchange(other.value_, nullptr)) {}
    InternetHandle& operator=(InternetHandle&& other) noexcept {
        if (this != &other) {
            if (value_) WinHttpCloseHandle(value_);
            value_ = std::exchange(other.value_, nullptr);
        }
        return *this;
    }
    HINTERNET Get() const { return value_; }
    explicit operator bool() const { return value_ != nullptr; }

private:
    HINTERNET value_ = nullptr;
};

struct ParsedUrl {
    std::wstring host;
    std::wstring path;
    INTERNET_PORT port = INTERNET_DEFAULT_HTTPS_PORT;
};

void ReportProgress(const BlenderInstallProgressCallback& callback,
                    int percent, std::wstring detail) {
    if (callback)
        callback({std::clamp(percent, 0, 100), std::move(detail)});
}

bool ParseUrl(const std::wstring& url, ParsedUrl& parsed,
              std::wstring& error) {
    URL_COMPONENTSW components{};
    components.dwStructSize = sizeof(components);
    components.dwHostNameLength = static_cast<DWORD>(-1);
    components.dwUrlPathLength = static_cast<DWORD>(-1);
    components.dwExtraInfoLength = static_cast<DWORD>(-1);
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &components)) {
        error = L"无法解析 Blender 官方地址：" + LastErrorMessage();
        return false;
    }
    parsed.host.assign(components.lpszHostName, components.dwHostNameLength);
    parsed.path.assign(components.lpszUrlPath, components.dwUrlPathLength);
    if (components.dwExtraInfoLength)
        parsed.path.append(components.lpszExtraInfo, components.dwExtraInfoLength);
    parsed.port = components.nPort;
    std::wstring lowerHost = parsed.host;
    std::transform(lowerHost.begin(), lowerHost.end(), lowerHost.begin(),
                   [](wchar_t ch) { return static_cast<wchar_t>(std::towlower(ch)); });
    if (components.nScheme != INTERNET_SCHEME_HTTPS ||
        (lowerHost != L"mirror.blender.org" &&
         lowerHost != L"www.blender.org") || parsed.path.empty()) {
        error = L"Blender 下载器只接受 HTTPS 官方地址";
        return false;
    }
    return true;
}

bool OpenRequest(const std::wstring& url, const wchar_t* method,
                 InternetHandle& session, InternetHandle& connection,
                 InternetHandle& request, std::wstring& error) {
    ParsedUrl parsed;
    if (!ParseUrl(url, parsed, error)) return false;
    session = InternetHandle(WinHttpOpen(
        L"RenderMesh-BlenderInstaller/2.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session) {
        error = L"无法初始化 Windows 下载组件：" + LastErrorMessage();
        return false;
    }
    WinHttpSetTimeouts(session.Get(), 5000, 5000, 5000, 5000);
    connection = InternetHandle(WinHttpConnect(
        session.Get(), parsed.host.c_str(), parsed.port, 0));
    if (!connection) {
        error = L"无法连接 Blender 官方镜像：" + LastErrorMessage();
        return false;
    }
    request = InternetHandle(WinHttpOpenRequest(
        connection.Get(), method, parsed.path.c_str(), nullptr,
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE));
    if (!request) {
        error = L"无法创建 Blender 官方请求：" + LastErrorMessage();
        return false;
    }
    // mirror.blender.org intentionally redirects large binaries to a nearby
    // community mirror. Keep that official distribution flow working, but
    // never permit the transport to be downgraded from HTTPS to HTTP. The MSI
    // is still accepted only after it matches the SHA-256 list fetched from
    // the whitelisted Blender origin.
    DWORD redirectPolicy = WINHTTP_OPTION_REDIRECT_POLICY_DISALLOW_HTTPS_TO_HTTP;
    if (!WinHttpSetOption(request.Get(), WINHTTP_OPTION_REDIRECT_POLICY,
                          &redirectPolicy, sizeof(redirectPolicy))) {
        error = L"无法锁定 Blender HTTPS 下载策略：" + LastErrorMessage();
        return false;
    }
    return true;
}

bool SendRequest(InternetHandle& request, DWORD& status,
                 std::wstring& error) {
    if (!WinHttpSendRequest(request.Get(), WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                            WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(request.Get(), nullptr)) {
        error = L"Blender 官方请求失败：" + LastErrorMessage();
        return false;
    }
    DWORD size = sizeof(status);
    if (!WinHttpQueryHeaders(
            request.Get(), WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX, &status, &size,
            WINHTTP_NO_HEADER_INDEX)) {
        error = L"无法读取 Blender 官方响应：" + LastErrorMessage();
        return false;
    }
    return true;
}

bool DownloadText(const std::wstring& url, std::atomic<bool>& cancel,
                  std::string& text, std::wstring& error) {
    InternetHandle session, connection, request;
    if (!OpenRequest(url, L"GET", session, connection, request, error))
        return false;
    DWORD status = 0;
    if (!SendRequest(request, status, error)) return false;
    if (status != 200) {
        error = L"Blender 官方目录返回 HTTP " + std::to_wstring(status);
        return false;
    }
    text.clear();
    std::array<char, 64 * 1024> buffer{};
    while (!cancel.load()) {
        DWORD read = 0;
        if (!WinHttpReadData(request.Get(), buffer.data(),
                             static_cast<DWORD>(buffer.size()), &read)) {
            error = L"读取 Blender 官方目录失败：" + LastErrorMessage();
            return false;
        }
        if (!read) return true;
        if (text.size() + read > 8 * 1024 * 1024) {
            error = L"Blender 官方目录响应异常大";
            return false;
        }
        text.append(buffer.data(), read);
    }
    error = L"Blender 下载已取消";
    return false;
}

std::array<int, 3> VersionParts(const std::string& version) {
    std::array<int, 3> parts{};
    size_t index = 0;
    for (size_t position = 0; index < parts.size() && position <= version.size(); ++index) {
        const size_t dot = std::min(version.find('.', position), version.size());
        parts[index] = std::atoi(version.substr(position, dot - position).c_str());
        position = dot + 1;
    }
    return parts;
}

bool VersionGreater(const std::string& left, const std::string& right) {
    return VersionParts(left) > VersionParts(right);
}

bool StartsWithIgnoreCase(const std::string& text, size_t position, std::string_view prefix) {
    if (position + prefix.size() > text.size()) return false;
    for (size_t index = 0; index < prefix.size(); ++index)
        if (std::tolower(static_cast<unsigned char>(text[position + index])) !=
            std::tolower(static_cast<unsigned char>(prefix[index])))
            return false;
    return true;
}

// Reads "<digits>.<digits>" (and ".<digits>" again when `parts` is 3) at
// `position`; advances `position` past it.
std::string ReadVersion(const std::string& text, size_t& position, int parts) {
    const size_t start = position;
    for (int part = 0; part < parts; ++part) {
        if (part && (position >= text.size() || text[position++] != '.')) return {};
        const size_t digits = position;
        while (position < text.size() && std::isdigit(static_cast<unsigned char>(text[position])))
            ++position;
        if (position == digits) return {};
    }
    return text.substr(start, position - start);
}

void SkipSpaces(const std::string& text, size_t& position) {
    while (position < text.size() && std::isspace(static_cast<unsigned char>(text[position])))
        ++position;
}

// "Blender 5.2 LTS" headings on blender.org/download/lts.
std::set<std::string> ParseLtsBranches(const std::string& html) {
    std::set<std::string> values;
    for (size_t position = 0; position < html.size(); ++position) {
        if (!StartsWithIgnoreCase(html, position, "blender")) continue;
        size_t cursor = position + 7;
        const size_t gap = cursor;
        SkipSpaces(html, cursor);
        if (cursor == gap) continue;
        const std::string branch = ReadVersion(html, cursor, 2);
        const size_t afterVersion = cursor;
        SkipSpaces(html, cursor);
        if (!branch.empty() && cursor > afterVersion && StartsWithIgnoreCase(html, cursor, "lts"))
            values.insert(branch);
    }
    return values;
}

// href="./Blender5.2/" directory links on mirror.blender.org/release.
std::vector<std::string> ParseBranches(const std::string& html) {
    std::set<std::string> unique;
    constexpr std::string_view prefix = "href=\"./Blender";
    for (size_t position = html.find(prefix); position != std::string::npos;
         position = html.find(prefix, position + 1)) {
        size_t cursor = position + prefix.size();
        const std::string branch = ReadVersion(html, cursor, 2);
        if (!branch.empty() && html.compare(cursor, 2, "/\"") == 0 &&
            VersionParts(branch)[0] >= 4)
            unique.insert(branch);
    }
    std::vector<std::string> values(unique.begin(), unique.end());
    std::sort(values.begin(), values.end(), VersionGreater);
    if (values.size() > kCatalogBranchLimit) values.resize(kCatalogBranchLimit);
    return values;
}

// Newest blender-X.Y.Z-windows-x64.msi of `branch` in a release directory.
std::string LatestMsiVersion(const std::string& html, const std::string& branch) {
    std::vector<std::string> versions;
    constexpr std::string_view suffix = "-windows-x64.msi";
    for (size_t position = 0; position < html.size(); ++position) {
        if (!StartsWithIgnoreCase(html, position, "blender-")) continue;
        size_t cursor = position + 8;
        const std::string version = ReadVersion(html, cursor, 3);
        if (version.empty() || !StartsWithIgnoreCase(html, cursor, suffix)) continue;
        cursor += suffix.size();
        if (cursor < html.size() && (html[cursor] == '"' || html[cursor] == '<') &&
            version.rfind(branch + ".", 0) == 0)
            versions.push_back(version);
    }
    if (versions.empty()) return {};
    std::sort(versions.begin(), versions.end(), VersionGreater);
    return versions.front();
}

// Finds "<sha256> [*]<filename>" in a sha256sum-style list.
std::string FindChecksum(const std::string& list, const std::string& filename) {
    size_t position = 0;
    while (position < list.size()) {
        size_t end = list.find('\n', position);
        if (end == std::string::npos) end = list.size();
        std::string line = list.substr(position, end - position);
        position = end + 1;
        while (!line.empty() && std::isspace(static_cast<unsigned char>(line.back())))
            line.pop_back();
        if (line.size() < 66 || line.find_first_not_of("0123456789abcdefABCDEF") != 64) continue;
        size_t name = 64;
        while (name < line.size() && (line[name] == ' ' || line[name] == '\t')) ++name;
        if (name == 64) continue;
        if (name < line.size() && line[name] == '*') ++name;
        if (line.size() - name == filename.size() && StartsWithIgnoreCase(line, name, filename)) {
            std::string sha = line.substr(0, 64);
            std::transform(sha.begin(), sha.end(), sha.begin(),
                           [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
            return sha;
        }
    }
    return {};
}

bool QueryRemoteSize(const std::wstring& url, uint64_t& size,
                     std::wstring& error) {
    InternetHandle session, connection, request;
    if (!OpenRequest(url, L"HEAD", session, connection, request, error))
        return false;
    DWORD status = 0;
    if (!SendRequest(request, status, error)) return false;
    if (status != 200) {
        error = L"Blender 官方安装包返回 HTTP " + std::to_wstring(status);
        return false;
    }
    wchar_t lengthText[64]{};
    DWORD lengthSize = sizeof(lengthText);
    if (!WinHttpQueryHeaders(
            request.Get(), WINHTTP_QUERY_CONTENT_LENGTH,
            WINHTTP_HEADER_NAME_BY_INDEX, lengthText, &lengthSize,
            WINHTTP_NO_HEADER_INDEX)) {
        error = L"无法读取 Blender 官方安装包大小";
        return false;
    }
    try {
        size = std::stoull(lengthText);
    } catch (...) {
        size = 0;
    }
    if (!size) {
        error = L"Blender 官方安装包大小无效";
        return false;
    }
    return true;
}

bool ExpectedMsiSha256(const BlenderRelease& release,
                       std::atomic<bool>& cancel, std::string& sha256,
                       std::wstring& error) {
    std::string checksums;
    if (!DownloadText(release.sha256Url, cancel, checksums, error)) return false;
    sha256 = FindChecksum(checksums, "blender-" + release.version + "-windows-x64.msi");
    if (sha256.empty()) {
        error = L"Blender 官方 SHA-256 清单中没有所选 MSI";
        return false;
    }
    return true;
}

bool DownloadMsi(const BlenderRelease& release, const fs::path& destination,
                 std::atomic<bool>& cancel,
                 const BlenderInstallProgressCallback& progress,
                 std::wstring& error) {
    std::string expectedSha;
    ReportProgress(progress, 12, L"正在读取 Blender 官方 SHA-256 清单");
    if (!ExpectedMsiSha256(release, cancel, expectedSha, error)) return false;
    uint64_t total = 0;
    if (!QueryRemoteSize(release.msiUrl, total, error)) return false;
    const fs::path partial(destination.wstring() + L".partial");
    std::error_code ignored;
    fs::create_directories(destination.parent_path(), ignored);
    if (fs::is_regular_file(destination, ignored) &&
        fs::file_size(destination, ignored) == total &&
        Sha256File(destination, &cancel) == expectedSha)
        return true;
    uint64_t existing = fs::is_regular_file(partial, ignored)
        ? fs::file_size(partial, ignored) : 0;
    if (existing > total) {
        fs::remove(partial, ignored);
        existing = 0;
    }
    if (existing == total) {
        ReportProgress(progress, 76, L"正在校验已下载的 Blender 官方 MSI");
        if (Sha256File(partial, &cancel) == expectedSha) {
            if (!MoveFileExW(partial.c_str(), destination.c_str(),
                             MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
                error = L"无法发布已校验的 Blender MSI：" + LastErrorMessage();
                return false;
            }
            return true;
        }
        fs::remove(partial, ignored);
        existing = 0;
    }
    InternetHandle session, connection, request;
    if (!OpenRequest(release.msiUrl, L"GET", session, connection, request, error))
        return false;
    if (existing) {
        const std::wstring range = L"Range: bytes=" + std::to_wstring(existing) + L"-";
        WinHttpAddRequestHeaders(
            request.Get(), range.c_str(), static_cast<DWORD>(-1L),
            WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE);
    }
    DWORD status = 0;
    if (!SendRequest(request, status, error)) return false;
    if (status != 200 && status != 206) {
        error = L"Blender 官方安装包返回 HTTP " + std::to_wstring(status);
        return false;
    }
    if (status == 200) existing = 0;
    HANDLE file = CreateFileW(
        partial.c_str(), GENERIC_WRITE, FILE_SHARE_READ,
        nullptr, existing ? OPEN_ALWAYS : CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        error = L"无法创建 Blender 下载文件：" + LastErrorMessage();
        return false;
    }
    LARGE_INTEGER offset{};
    offset.QuadPart = static_cast<LONGLONG>(existing);
    SetFilePointerEx(file, offset, nullptr, FILE_BEGIN);
    uint64_t received = existing;
    std::array<BYTE, 256 * 1024> buffer{};
    bool ok = true;
    while (!cancel.load()) {
        DWORD read = 0;
        if (!WinHttpReadData(request.Get(), buffer.data(),
                             static_cast<DWORD>(buffer.size()), &read)) {
            error = L"Blender MSI 下载中断：" + LastErrorMessage();
            ok = false;
            break;
        }
        if (!read) break;
        DWORD written = 0;
        if (!WriteFile(file, buffer.data(), read, &written, nullptr) ||
            written != read) {
            error = L"无法写入 Blender MSI：" + LastErrorMessage();
            ok = false;
            break;
        }
        received += written;
        ReportProgress(
            progress,
            15 + static_cast<int>(std::min(received, total) * 58 / total),
            L"正在下载 Blender " + Utf8ToWide(release.version) + L" · " +
                std::to_wstring(received / (1024 * 1024)) + L" / " +
                std::to_wstring(total / (1024 * 1024)) + L" MiB");
    }
    if (cancel.load()) {
        error = L"Blender 下载已取消";
        ok = false;
    }
    if (ok && !FlushFileBuffers(file)) {
        error = L"无法保存 Blender MSI：" + LastErrorMessage();
        ok = false;
    }
    CloseHandle(file);
    if (!ok) return false;
    if (received != total) {
        error = L"Blender MSI 下载不完整，已保留断点供下次继续";
        return false;
    }
    ReportProgress(progress, 76, L"正在校验 Blender 官方 MSI 的 SHA-256");
    if (Sha256File(partial, &cancel) != expectedSha) {
        if (!cancel.load()) fs::remove(partial, ignored);
        error = cancel.load() ? L"Blender 下载已取消"
                              : L"Blender MSI SHA-256 校验失败，已删除损坏文件";
        return false;
    }
    if (!MoveFileExW(partial.c_str(), destination.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        error = L"无法发布已校验的 Blender MSI：" + LastErrorMessage();
        return false;
    }
    return true;
}

bool RunInstallerStep(const fs::path& executable,
                const std::vector<std::wstring>& arguments,
                bool visible, const fs::path& logPath,
                std::atomic<bool>& cancel, DWORD& exitCode,
                std::wstring& error) {
    std::wstring command = QuoteCommandLineArgument(executable.wstring());
    for (const auto& argument : arguments)
        command += L" " + QuoteCommandLineArgument(argument);
    std::vector<wchar_t> mutableCommand(command.begin(), command.end());
    mutableCommand.push_back(L'\0');
    HANDLE log = INVALID_HANDLE_VALUE;
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    if (!visible) {
        std::error_code ignored;
        fs::create_directories(logPath.parent_path(), ignored);
        log = CreateFileW(logPath.c_str(), GENERIC_WRITE,
                          FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr,
                          CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (log == INVALID_HANDLE_VALUE) {
            error = L"无法创建 Blender 安装日志：" + LastErrorMessage();
            return false;
        }
        startup.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
        startup.wShowWindow = SW_HIDE;
        startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
        startup.hStdOutput = log;
        startup.hStdError = log;
    }
    PROCESS_INFORMATION process{};
    const BOOL started = CreateProcessW(
        executable.c_str(), mutableCommand.data(), nullptr, nullptr,
        visible ? FALSE : TRUE, visible ? 0 : CREATE_NO_WINDOW,
        nullptr, executable.parent_path().c_str(), &startup, &process);
    if (log != INVALID_HANDLE_VALUE) CloseHandle(log);
    if (!started) {
        error = L"无法启动 Blender 安装步骤：" + LastErrorMessage();
        return false;
    }
    CloseHandle(process.hThread);
    while (WaitForSingleObject(process.hProcess, 200) == WAIT_TIMEOUT) {
        if (cancel.load()) {
            TerminateProcess(process.hProcess, ERROR_CANCELLED);
            WaitForSingleObject(process.hProcess, 5000);
            CloseHandle(process.hProcess);
            error = L"Blender 安装已取消";
            return false;
        }
    }
    GetExitCodeProcess(process.hProcess, &exitCode);
    CloseHandle(process.hProcess);
    constexpr DWORD kSuccessRebootRequired = 3010;
    if (exitCode != ERROR_SUCCESS && exitCode != kSuccessRebootRequired) {
        error = L"Blender 安装程序退出码：" + std::to_wstring(exitCode);
        return false;
    }
    return true;
}

std::optional<std::wstring> ReadRegistryString(HKEY key,
                                                const wchar_t* name) {
    DWORD type = 0, bytes = 0;
    if (RegQueryValueExW(key, name, nullptr, &type, nullptr, &bytes) != ERROR_SUCCESS ||
        (type != REG_SZ && type != REG_EXPAND_SZ) || bytes < sizeof(wchar_t))
        return std::nullopt;
    std::wstring value(bytes / sizeof(wchar_t), L'\0');
    if (RegQueryValueExW(key, name, nullptr, nullptr,
                        reinterpret_cast<BYTE*>(value.data()), &bytes) != ERROR_SUCCESS)
        return std::nullopt;
    while (!value.empty() && value.back() == L'\0') value.pop_back();
    return value;
}

fs::path FindInstalledBlender(const BlenderRelease& release) {
    const std::wstring branch = Utf8ToWide(release.branch);
    std::vector<fs::path> candidates;
    for (const wchar_t* variable : {L"ProgramFiles", L"ProgramFiles(x86)"}) {
        wchar_t root[32768]{};
        if (GetEnvironmentVariableW(variable, root,
                                    static_cast<DWORD>(std::size(root)))) {
            candidates.push_back(fs::path(root) / L"Blender Foundation" /
                                 (L"Blender " + branch) / L"blender.exe");
        }
    }
    struct RegistryRoot { HKEY root; const wchar_t* path; REGSAM view; };
    const RegistryRoot roots[] = {
        {HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall", KEY_WOW64_64KEY},
        {HKEY_CURRENT_USER, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall", KEY_WOW64_64KEY},
        {HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall", KEY_WOW64_32KEY},
    };
    for (const auto& root : roots) {
        HKEY uninstall = nullptr;
        if (RegOpenKeyExW(root.root, root.path, 0, KEY_READ | root.view,
                          &uninstall) != ERROR_SUCCESS)
            continue;
        for (DWORD index = 0;; ++index) {
            wchar_t name[512]{};
            DWORD nameLength = static_cast<DWORD>(std::size(name));
            if (RegEnumKeyExW(uninstall, index, name, &nameLength, nullptr,
                              nullptr, nullptr, nullptr) != ERROR_SUCCESS)
                break;
            HKEY product = nullptr;
            if (RegOpenKeyExW(uninstall, name, 0, KEY_READ | root.view,
                              &product) != ERROR_SUCCESS)
                continue;
            const auto displayName = ReadRegistryString(product, L"DisplayName");
            const auto installLocation = ReadRegistryString(product, L"InstallLocation");
            if (displayName && installLocation &&
                displayName->find(L"Blender") != std::wstring::npos &&
                (displayName->find(branch) != std::wstring::npos ||
                 displayName->find(Utf8ToWide(release.version)) != std::wstring::npos))
                candidates.push_back(fs::path(*installLocation) / L"blender.exe");
            RegCloseKey(product);
        }
        RegCloseKey(uninstall);
    }
    std::error_code ignored;
    for (const auto& candidate : candidates)
        if (fs::is_regular_file(candidate, ignored))
            return fs::absolute(candidate, ignored);
    return {};
}

std::string PreferenceScript(bool configureOptix) {
    std::string script = "import bpy\n";
    if (configureOptix) {
        script +=
            "addon=bpy.context.preferences.addons.get('cycles')\n"
            "assert addon is not None, 'Cycles preferences unavailable'\n"
            "prefs=addon.preferences\n"
            "prefs.compute_device_type='OPTIX'\n"
            "prefs.get_devices()\n"
            "devices=[d for d in prefs.devices if d.type == 'OPTIX']\n"
            "assert devices, 'No OptiX device detected'\n"
            "for d in prefs.devices: d.use=(d.type == 'OPTIX')\n";
    }
    return script + "bpy.ops.wm.save_userpref()\n";
}

bool ConfigurePreferences(const fs::path& blender, const fs::path& dataDir,
                          bool configureOptix, std::atomic<bool>& cancel,
                          std::wstring& error) {
    const fs::path script = dataDir / L"blender-install" /
        (configureOptix ? L"configure-optix.py" : L"configure-defaults.py");
    std::error_code ignored;
    fs::create_directories(script.parent_path(), ignored);
    if (!WriteFileText(script, PreferenceScript(configureOptix))) {
        error = L"无法写入 Blender 默认配置脚本";
        return false;
    }
    DWORD exitCode = 0;
    return RunInstallerStep(
        blender,
        {L"--background", L"--factory-startup", L"--disable-autoexec",
         L"--python-exit-code", L"97", L"--python", script.wstring()},
        false, dataDir / L"logs" / L"blender-install-config.log",
        cancel, exitCode, error);
}

}  // namespace

fs::path DefaultBlenderDownloadDirectory() {
    wchar_t profile[32768]{};
    const DWORD length = GetEnvironmentVariableW(
        L"USERPROFILE", profile, static_cast<DWORD>(std::size(profile)));
    if (length > 0 && length < std::size(profile))
        return fs::path(profile) / L"Downloads";
    return ExecutableDirectory();
}

bool HasNvidiaGpu(const HardwareInfo& hardware) {
    for (std::string model : hardware.gpuModels) {
        std::transform(model.begin(), model.end(), model.begin(),
                       [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        if (model.find("nvidia") != std::string::npos ||
            model.find("geforce") != std::string::npos ||
            model.find("quadro") != std::string::npos ||
            model.find("rtx") != std::string::npos)
            return true;
    }
    return false;
}

bool ValidateBlenderDownloadDirectory(const fs::path& directory,
                                      std::wstring& error) {
    error.clear();
    if (directory.empty() || !directory.is_absolute()) {
        error = L"请选择一个绝对下载目录";
        return false;
    }
    std::error_code directoryError;
    fs::create_directories(directory, directoryError);
    if (directoryError || !fs::is_directory(directory, directoryError)) {
        error = L"无法创建选择的 Blender 下载目录";
        return false;
    }
    ULARGE_INTEGER freeBytes{};
    if (!GetDiskFreeSpaceExW(directory.c_str(), &freeBytes, nullptr, nullptr)) {
        error = L"无法读取 Blender 下载目录的剩余空间：" + LastErrorMessage();
        return false;
    }
    if (freeBytes.QuadPart < kMinimumDownloadFreeBytes) {
        error = L"下载并安装 Blender 至少需要 1 GiB 可用空间";
        return false;
    }
    return true;
}

bool FetchOfficialBlenderCatalog(std::vector<BlenderRelease>& releases,
                                 std::atomic<bool>& cancel,
                                 std::wstring& error) {
    releases.clear();
    std::string rootHtml;
    if (!DownloadText(L"https://mirror.blender.org/release/", cancel,
                      rootHtml, error))
        return false;
    std::set<std::string> ltsBranches;
    std::string ltsHtml;
    std::wstring ignoredError;
    if (DownloadText(L"https://www.blender.org/download/lts/", cancel,
                     ltsHtml, ignoredError)) {
        const auto parsed = ParseLtsBranches(ltsHtml);
        ltsBranches = parsed;
    } else {
        LogWarning("Blender LTS labels could not be refreshed: " +
                   WideToUtf8(ignoredError));
    }
    for (const std::string& branch : ParseBranches(rootHtml)) {
        if (cancel.load()) {
            error = L"Blender 版本读取已取消";
            return false;
        }
        const std::wstring directory =
            L"https://mirror.blender.org/release/Blender" +
            Utf8ToWide(branch) + L"/";
        std::string page;
        std::wstring pageError;
        if (!DownloadText(directory, cancel, page, pageError)) {
            LogWarning("Blender catalog skipped branch " + branch + ": " +
                       WideToUtf8(pageError));
            continue;
        }
        const std::string version = LatestMsiVersion(page, branch);
        if (version.empty()) continue;
        BlenderRelease release;
        release.branch = branch;
        release.version = version;
        release.lts = ltsBranches.contains(branch);
        release.displayName = L"Blender " + Utf8ToWide(version) +
            (release.lts ? L"  · LTS" : L"");
        const std::wstring base = directory + L"blender-" +
            Utf8ToWide(version);
        release.msiUrl = base + L"-windows-x64.msi";
        release.sha256Url = base + L".sha256";
        releases.push_back(std::move(release));
    }
    if (releases.empty()) {
        error = L"Blender 官方镜像当前没有可用的 Windows x64 MSI";
        return false;
    }
    return true;
}

bool ProbeOfficialBlenderRelease(const BlenderRelease& release,
                                 std::atomic<bool>& cancel,
                                 uint64_t& sizeBytes,
                                 std::string& sha256,
                                 std::wstring& error) {
    sizeBytes = 0;
    sha256.clear();
    return ExpectedMsiSha256(release, cancel, sha256, error) &&
        QueryRemoteSize(release.msiUrl, sizeBytes, error);
}

BlenderInstallResult InstallOfficialBlender(
    const BlenderRelease& release, const fs::path& dataDir,
    const fs::path& downloadDirectory, bool configureOptix,
    std::atomic<bool>& cancel,
    const BlenderInstallProgressCallback& progress) {
    BlenderInstallResult result;
    result.release = release;
    if (!ValidateBlenderDownloadDirectory(downloadDirectory, result.error))
        return result;
    result.installer = downloadDirectory /
        (L"blender-" + Utf8ToWide(release.version) + L"-windows-x64.msi");
    ReportProgress(progress, 8, L"正在连接 Blender 官方镜像");
    if (!DownloadMsi(release, result.installer, cancel, progress, result.error))
        return result;
    wchar_t systemDirectory[32768]{};
    if (!GetSystemDirectoryW(systemDirectory,
                             static_cast<UINT>(std::size(systemDirectory)))) {
        result.error = L"无法定位 Windows Installer";
        return result;
    }
    const fs::path msiexec = fs::path(systemDirectory) / L"msiexec.exe";
    ReportProgress(progress, 82, L"请在 Blender 官方安装程序中完成安装");
    DWORD exitCode = 0;
    if (!RunInstallerStep(msiexec, {L"/i", result.installer.wstring()}, true,
                    {}, cancel, exitCode, result.error))
        return result;
    result.executable = FindInstalledBlender(release);
    if (result.executable.empty()) {
        result.warning =
            L"Blender 安装程序已完成，但 RenderMesh 没有定位到 blender.exe。"
            L"请在渲染参数或设置中手动选择 blender.exe。";
        result.ok = true;
        ReportProgress(progress, 100, L"Blender 安装程序已完成");
        return result;
    }
    ReportProgress(
        progress, 94,
        configureOptix ? L"正在导入 NVIDIA OptiX 默认渲染配置"
                       : L"正在初始化 Blender 用户配置");
    std::wstring preferenceError;
    if (!ConfigurePreferences(result.executable, dataDir, configureOptix,
                              cancel, preferenceError)) {
        if (cancel.load()) {
            result.error = preferenceError;
            return result;
        }
        result.warning = L"Blender 已安装，但默认配置未能写入：" +
            preferenceError;
    } else {
        result.preferencesCreated = true;
        result.optixConfigured = configureOptix;
    }
    result.ok = true;
    ReportProgress(progress, 100, L"Blender " + Utf8ToWide(release.version) +
                   L" 安装完成");
    return result;
}

#ifdef RM_DEV_TOOLS
bool RunBlenderInstallerSelfTest(std::wstring& report) {
    const std::string rootFixture =
        R"(<a href="./Blender5.1/"></a><a href="./Blender5.2/"></a>)";
    const std::string releaseFixture =
        R"(<a href="./blender-5.2.0-windows-x64.msi"></a>)";
    const std::string ltsFixture = R"(<h2>Blender 5.2 LTS</h2>)";
    HardwareInfo nvidia;
    nvidia.gpuModels = {"NVIDIA GeForce RTX 5090"};
    HardwareInfo amd;
    amd.gpuModels = {"AMD Radeon RX 7900 XTX"};
    const auto branches = ParseBranches(rootFixture);
    const auto lts = ParseLtsBranches(ltsFixture);
    const std::string optixScript = PreferenceScript(true);
    if (branches.size() != 2 || branches.front() != "5.2" ||
        LatestMsiVersion(releaseFixture, "5.2") != "5.2.0" ||
        !lts.contains("5.2") || !HasNvidiaGpu(nvidia) || HasNvidiaGpu(amd) ||
        optixScript.find("compute_device_type='OPTIX'") == std::string::npos ||
        optixScript.find("bpy.ops.wm.save_userpref()") == std::string::npos) {
        report = L"Blender 官方 MSI 下载器自检失败";
        return false;
    }
    report = L"BLENDER_INSTALLER_OK";
    return true;
}

#endif  // RM_DEV_TOOLS

}  // namespace rm
