#include "Core.hpp"

#include <bcrypt.h>
#include <dxgi1_2.h>
#include <shlobj.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <cwctype>
#include <mutex>
#include <set>

// libc++abi's default terminate handler demangles the type of an uncaught
// exception before printing it. A GUI process has nowhere to print it, and the
// demangler is ~125 KB, so report "invalid name" (-2) instead of linking it.
extern "C" char* __cxa_demangle(const char*, char*, size_t*, int* status) {
    if (status) *status = -2;
    return nullptr;
}

namespace rm {

namespace {

std::mutex gLogMutex;
fs::path gLogDirectory;
fs::path gLogPath;

std::string TrimAscii(std::string value) {
    auto whitespace = [](unsigned char ch) { return std::isspace(ch) != 0; };
    value.erase(value.begin(), std::find_if_not(value.begin(), value.end(), whitespace));
    value.erase(std::find_if_not(value.rbegin(), value.rend(), whitespace).base(), value.end());
    return value;
}

std::string LogDayStamp() {
    SYSTEMTIME time{};
    GetLocalTime(&time);
    char value[16]{};
    std::snprintf(value, sizeof(value), "%04u-%02u-%02u",
                  time.wYear, time.wMonth, time.wDay);
    return value;
}

std::string LogTimestamp() {
    SYSTEMTIME time{};
    GetLocalTime(&time);
    char value[32]{};
    std::snprintf(value, sizeof(value), "%04u-%02u-%02u %02u:%02u:%02u.%03u",
                  time.wYear, time.wMonth, time.wDay,
                  time.wHour, time.wMinute, time.wSecond, time.wMilliseconds);
    return value;
}

void WriteLogLine(const char* level, const std::string& message) {
    std::scoped_lock lock(gLogMutex);
    if (gLogDirectory.empty()) return;
    fs::path expected = gLogDirectory / Utf8ToWide("RenderMesh-" + LogDayStamp() + ".log");
    if (expected != gLogPath) gLogPath = std::move(expected);
    std::string clean = message;
    std::replace(clean.begin(), clean.end(), '\r', ' ');
    std::replace(clean.begin(), clean.end(), '\n', ' ');
    const std::string line = LogTimestamp() + " [" + level + "] " + clean + "\r\n";
    HANDLE output = CreateFileW(gLogPath.c_str(), FILE_APPEND_DATA,
                                FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                                FILE_ATTRIBUTE_NORMAL, nullptr);
    if (output == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    WriteFile(output, line.data(), static_cast<DWORD>(line.size()), &written, nullptr);
    CloseHandle(output);
}

std::string InviteCodeFromToken(const std::string& token) {
    std::string prefix = Sha256Prefix(token, 8);
    unsigned long value = 0;
    try {
        value = std::stoul(prefix, nullptr, 16);
    } catch (...) {
        return NewInviteCode();
    }
    char code[7]{};
    std::snprintf(code, sizeof(code), "%06lu", value % 1000000ul);
    return code;
}

}  // namespace

std::wstring QuoteCommandLineArgument(const std::wstring& value) {
    // An empty argument must stay visible as "" or the arguments shift.
    if (!value.empty() && value.find_first_of(L" \t\"") == std::wstring::npos) return value;
    std::wstring out = L"\"";
    size_t slashes = 0;
    for (wchar_t ch : value) {
        if (ch == L'\\') {
            ++slashes;
        } else if (ch == L'\"') {
            out.append(slashes * 2 + 1, L'\\');
            out.push_back(ch);
            slashes = 0;
        } else {
            out.append(slashes, L'\\');
            slashes = 0;
            out.push_back(ch);
        }
    }
    out.append(slashes * 2, L'\\');
    out.push_back(L'\"');
    return out;
}

namespace {

class WindowsProcessTree {
public:
    WindowsProcessTree() {
        handle_ = CreateJobObjectW(nullptr, nullptr);
        if (!handle_) return;
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (!SetInformationJobObject(handle_, JobObjectExtendedLimitInformation,
                                     &limits, sizeof(limits))) {
            CloseHandle(handle_);
            handle_ = nullptr;
        }
    }

    ~WindowsProcessTree() {
        if (handle_) CloseHandle(handle_);
    }

    WindowsProcessTree(const WindowsProcessTree&) = delete;
    WindowsProcessTree& operator=(const WindowsProcessTree&) = delete;

    bool TryAssign(HANDLE process, std::wstring& error) {
        if (!handle_) {
            error = L"无法创建 Windows 进程树容器";
            return false;
        }
        if (!AssignProcessToJobObject(handle_, process)) {
            error = LastErrorMessage();
            return false;
        }
        assigned_ = true;
        return true;
    }

    bool Terminate(DWORD exitCode) const {
        return assigned_ && TerminateJobObject(handle_, exitCode);
    }

private:
    HANDLE handle_ = nullptr;
    bool assigned_ = false;
};

}  // namespace

ProcessResult RunProcess(
    const fs::path& executable,
    const std::vector<std::wstring>& arguments,
    const fs::path& logPath,
    std::atomic<bool>* stopRequested,
    std::chrono::milliseconds timeout,
    const std::function<void()>& progress) {
    ProcessResult result;
    fs::create_directories(logPath.parent_path());
    SECURITY_ATTRIBUTES logSecurity{};
    logSecurity.nLength = sizeof(logSecurity);
    logSecurity.bInheritHandle = TRUE;
    HANDLE log = CreateFileW(
        logPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &logSecurity, CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (log == INVALID_HANDLE_VALUE) {
        result.error = L"无法创建日志：" + LastErrorMessage();
        return result;
    }

    std::wstring command = QuoteCommandLineArgument(executable.wstring());
    for (const auto& arg : arguments) {
        command.push_back(L' ');
        command += QuoteCommandLineArgument(arg);
    }
    std::vector<wchar_t> buffer(command.begin(), command.end());
    buffer.push_back(L'\0');

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_HIDE;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    startup.hStdOutput = log;
    startup.hStdError = log;
    PROCESS_INFORMATION process{};
    DWORD flags = CREATE_NO_WINDOW | CREATE_SUSPENDED;
    BOOL created = CreateProcessW(
        executable.c_str(), buffer.data(), nullptr, nullptr, TRUE, flags, nullptr,
        logPath.parent_path().c_str(), &startup, &process);
    CloseHandle(log);
    if (!created) {
        result.error = L"无法启动进程：" + LastErrorMessage();
        return result;
    }
    result.started = true;

    WindowsProcessTree processTree;
    std::wstring containmentError;
    if (!processTree.TryAssign(process.hProcess, containmentError)) {
        LogWarning("Blender process-tree containment unavailable; continuing without it: " +
                   WideToUtf8(containmentError));
    }
    auto terminateAndWait = [&](DWORD exitCode) {
        bool terminated = processTree.Terminate(exitCode) ||
            TerminateProcess(process.hProcess, exitCode);
        if (!terminated) {
            std::wstring terminateError = LastErrorMessage();
            if (!result.error.empty()) result.error += L"；";
            result.error += L"无法终止 Blender：" + terminateError;
        }
        DWORD wait = WaitForSingleObject(process.hProcess, 5000);
        if (wait != WAIT_OBJECT_0 && result.error.empty())
            result.error = L"等待 Blender 退出超时";
    };
    if (ResumeThread(process.hThread) == static_cast<DWORD>(-1)) {
        result.error = L"无法恢复 Blender 进程：" + LastErrorMessage();
        terminateAndWait(90);
    }

    if (result.error.empty()) {
        auto deadline = timeout > std::chrono::milliseconds::zero()
            ? std::chrono::steady_clock::now() + timeout
            : std::chrono::steady_clock::time_point::max();
        for (;;) {
            if (stopRequested && stopRequested->load()) {
                result.error = L"任务因应用退出而中止";
                terminateAndWait(92);
                break;
            }
            DWORD wait = WaitForSingleObject(
                process.hProcess,
                progress ? 100 : timeout > std::chrono::milliseconds::zero() ? 250 :
                    stopRequested ? 500 : INFINITE);
            if (wait == WAIT_OBJECT_0) break;
            if (wait == WAIT_FAILED) {
                result.error = LastErrorMessage();
                terminateAndWait(93);
                break;
            }
            if (std::chrono::steady_clock::now() >= deadline) {
                result.timedOut = true;
                result.error = L"Blender 首次无界面初始化超时";
                terminateAndWait(94);
                break;
            }
            if (progress) progress();
        }
    }
    if (progress) progress();
    GetExitCodeProcess(process.hProcess, &result.exitCode);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return result;
}

std::string ReadFileTail(const fs::path& path, uint64_t maximumBytes) {
    File input = File::OpenRead(path);
    if (!input) return {};
    const uint64_t size = input.Size();
    const uint64_t start = size > maximumBytes ? size - maximumBytes : 0;
    std::string text(static_cast<size_t>(size - start), '\0');
    if (!input.Seek(start) || !input.ReadExact(text.data(), text.size())) return {};
    if (start > 0) {
        size_t firstLine = text.find('\n');
        if (firstLine != std::string::npos) text.erase(0, firstLine + 1);
    }
    return text;
}


std::string WideToUtf8(const std::wstring& value) {
    if (value.empty()) return {};
    int size = WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
                                   nullptr, 0, nullptr, nullptr);
    std::string output(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
                        output.data(), size, nullptr, nullptr);
    return output;
}

std::wstring Utf8ToWide(const std::string& value) {
    if (value.empty()) return {};
    int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                                   static_cast<int>(value.size()), nullptr, 0);
    if (size <= 0) return L"";
    std::wstring output(static_cast<size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                        static_cast<int>(value.size()), output.data(), size);
    return output;
}

std::wstring LastErrorMessage(DWORD code) {
    wchar_t* message = nullptr;
    DWORD size = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
            FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, code, 0, reinterpret_cast<wchar_t*>(&message), 0, nullptr);
    std::wstring output = size ? std::wstring(message, size) : L"Windows 错误 " + std::to_wstring(code);
    if (message) LocalFree(message);
    while (!output.empty() && (output.back() == L'\r' || output.back() == L'\n')) output.pop_back();
    return output;
}

std::string NewRandomHex(size_t bytes) {
    std::vector<unsigned char> data(bytes);
    if (BCryptGenRandom(nullptr, data.data(), static_cast<ULONG>(data.size()),
                        BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) {
        throw std::runtime_error("BCryptGenRandom failed");
    }
    static constexpr char kHex[] = "0123456789abcdef";
    std::string text;
    for (unsigned char value : data) {
        text.push_back(kHex[value >> 4]);
        text.push_back(kHex[value & 15]);
    }
    return text;
}

std::string NewInviteCode() {
    uint32_t value = 0;
    constexpr uint32_t range = 1000000;
    constexpr uint32_t limit = UINT32_MAX - (UINT32_MAX % range);
    do {
        if (BCryptGenRandom(nullptr, reinterpret_cast<PUCHAR>(&value), sizeof(value),
                            BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) {
            throw std::runtime_error("BCryptGenRandom failed");
        }
    } while (value >= limit);
    char code[7]{};
    std::snprintf(code, sizeof(code), "%06u", value % range);
    return code;
}

bool IsInviteCode(const std::string& value) {
    return value.size() == 6 &&
        std::all_of(value.begin(), value.end(), [](unsigned char ch) {
            return ch >= '0' && ch <= '9';
        });
}

bool IsSafeOutputExtension(const std::string& value) {
    return value.empty() || (value.size() >= 2 && value.size() <= 16 && value.front() == '.' &&
        std::all_of(value.begin() + 1, value.end(), [](unsigned char ch) {
            return std::isalnum(ch) != 0 || ch == '_';
        }));
}

json OutputTargetMetadata(const OutputTarget& target) {
    json metadata = {
        {"id", target.id},
        {"name", target.name},
        {"outputs", target.outputs},
        {"file_pattern", target.filePattern},
        {"scene", target.sceneName},
        {"required", target.required},
    };
    // Old protocol-v6 peers sent only id/name. Omitting the locator preserves
    // their top-level count/order fallback instead of treating [] as a locator.
    if (!target.nodePath.empty() || !target.sceneName.empty())
        metadata["node_path"] = target.nodePath;
    return metadata;
}

json OutputTargetLocatorMetadata(const OutputTarget& target) {
    json metadata = {{"id", target.id}};
    if (!target.nodePath.empty() || !target.sceneName.empty())
        metadata["node_path"] = target.nodePath;
    metadata["required"] = target.required;
    if (!target.filePattern.empty()) metadata["file_pattern"] = target.filePattern;
    return metadata;
}

bool ParseOutputTargetMetadata(const json& value, OutputTarget& target) {
    try {
        if (!value.is_object()) return false;
        OutputTarget parsed;
        parsed.id = value.value("id", "");
        parsed.name = value.value("name", "");
        parsed.filePattern = value.value("file_pattern", "");
        parsed.sceneName = value.value("scene", "");
        parsed.required = value.value("required", false);
        if (parsed.id.empty() || parsed.id.size() > 128 || parsed.name.size() > 512 ||
            parsed.filePattern.size() > 2048 || parsed.sceneName.size() > 512 ||
            !std::all_of(parsed.id.begin(), parsed.id.end(), [](unsigned char ch) {
                return std::isalnum(ch) != 0 || ch == '-' || ch == '_';
            }))
            return false;
        const json outputRows = value.value("outputs", json::array());
        const json pathRows = value.value("node_path", json::array());
        if (!outputRows.is_array() || outputRows.size() > 256 ||
            !pathRows.is_array() || pathRows.size() > 32)
            return false;
        for (const auto& row : outputRows) {
            if (!row.is_string()) return false;
            std::string label = row.get<std::string>();
            if (label.size() > 512) return false;
            parsed.outputs.push_back(std::move(label));
        }
        for (const auto& row : pathRows) {
            if (!row.is_number_integer()) return false;
            int index = row.get<int>();
            if (index < 0 || index > 100000) return false;
            parsed.nodePath.push_back(index);
        }
        target = std::move(parsed);
        return true;
    } catch (...) {
        return false;
    }
}

json PreflightReportToJson(const PreflightReport& report) {
    auto dependencies = [](const std::vector<DependencyItem>& values) {
        json rows = json::array();
        for (const auto& item : values)
            rows.push_back({{"kind", item.kind}, {"owner", item.owner}, {"path", item.path}});
        return rows;
    };
    json primary = json::object();
    for (const auto& [frame, path] : report.primaryOutputs)
        primary[std::to_string(frame)] = WideToUtf8(path.wstring());
    json outputs = json::array();
    for (const auto& target : report.compositorOutputs) {
        json metadata = OutputTargetMetadata(target);
        metadata["directory"] = WideToUtf8(target.directory.wstring());
        outputs.push_back(std::move(metadata));
    }
    return {
        {"pass", report.passed},
        {"can_ignore_asset_warnings", report.canIgnoreAssetWarnings},
        {"asset_warnings_ignored", report.assetWarningsIgnored},
        {"validation_skipped_by_user", report.validationSkippedByUser},
        {"software", report.software},
        {"scene_name", report.sceneName},
        {"version", report.softwareVersion},
        {"render_engine", report.renderEngine},
        {"render_format", report.outputFormat},
        {"file_extension", report.outputExtension},
        {"requires_gpu", report.requiresGpu},
        {"frame_start", report.frameStart},
        {"frame_end", report.frameEnd},
        {"frame_step", report.frameStep},
        {"packed", report.packedCount},
        {"bundled", report.bundledCount},
        {"pending_changes", report.pendingChanges},
        {"missing", dependencies(report.missing)},
        {"unpacked", dependencies(report.unpacked)},
        {"warnings", report.warnings},
        {"primary_outputs", std::move(primary)},
        {"compositor_outputs", std::move(outputs)},
        {"source", WideToUtf8(report.sourceProject.wstring())},
        {"prepared", WideToUtf8(report.preparedProject.wstring())},
        {"prepared_copy", report.preparedCopy},
        {"summary", WideToUtf8(report.summary)},
    };
}

bool PreflightReportFromJson(const json& value, PreflightReport& report) {
    if (!value.is_object()) return false;
    try {
        report = {};
        report.passed = value.value("pass", false);
        report.canIgnoreAssetWarnings = value.value("can_ignore_asset_warnings", false);
        report.assetWarningsIgnored = value.value("asset_warnings_ignored", false);
        report.validationSkippedByUser = value.value("validation_skipped_by_user", false);
        report.software = value.value("software", "blender");
        report.sceneName = value.value("scene_name", "");
        report.softwareVersion = value.value("version", "");
        report.renderEngine = value.value("render_engine", "");
        report.outputFormat = value.value("render_format", "");
        report.outputExtension = value.value("file_extension", "");
        report.requiresGpu = value.value("requires_gpu", false);
        report.frameStart = value.value("frame_start", 1);
        report.frameEnd = value.value("frame_end", 1);
        report.frameStep = value.value("frame_step", 1);
        report.packedCount = value.value("packed", 0);
        report.bundledCount = value.value("bundled", 0);
        report.pendingChanges = value.value("pending_changes", 0);
        for (const char* key : {"missing", "unpacked"}) {
            auto& rows = std::string_view(key) == "missing" ? report.missing : report.unpacked;
            for (const auto& item : value.value(key, json::array()))
                rows.push_back({item.value("kind", ""), item.value("owner", ""),
                                item.value("path", "")});
        }
        report.warnings = value.value("warnings", std::vector<std::string>{});
        const json primary = value.value("primary_outputs", json::object());
        for (const auto& [frame, path] : primary.items())
            if (path.is_string())
                report.primaryOutputs[std::atoi(frame.c_str())] =
                    Utf8ToWide(path.get<std::string>());
        for (const auto& row : value.value("compositor_outputs", json::array())) {
            OutputTarget target;
            if (!ParseOutputTargetMetadata(row, target)) return false;
            target.directory = Utf8ToWide(row.value("directory", ""));
            report.compositorOutputs.push_back(std::move(target));
        }
        report.sourceProject = Utf8ToWide(value.value("source", ""));
        report.preparedProject = Utf8ToWide(value.value("prepared", ""));
        report.preparedCopy = value.value("prepared_copy", false);
        report.summary = Utf8ToWide(value.value("summary", ""));
        return true;
    } catch (...) {
        return false;
    }
}

std::vector<std::vector<int>> BuildFramePackages(
    const std::vector<int>& frames, size_t packageSize) {
    std::vector<std::vector<int>> result;
    if (packageSize == 0) return result;
    for (size_t offset = 0; offset < frames.size(); offset += packageSize) {
        size_t end = std::min(frames.size(), offset + packageSize);
        result.emplace_back(frames.begin() + static_cast<std::ptrdiff_t>(offset),
                            frames.begin() + static_cast<std::ptrdiff_t>(end));
    }
    return result;
}

bool ParseFrameSelection(
    const std::wstring& expression,
    const std::vector<int>& allowedFrames,
    std::vector<int>& selectedFrames,
    std::wstring& error) {
    selectedFrames.clear();
    error.clear();
    if (expression.empty() || allowedFrames.empty()) {
        error = L"请填写至少一个测试帧";
        return false;
    }
    const std::set<int> allowed(allowedFrames.begin(), allowedFrames.end());
    std::set<int> selected;
    auto trim = [](std::wstring value) {
        value.erase(value.begin(), std::find_if_not(value.begin(), value.end(),
            [](wchar_t ch) { return std::iswspace(ch) != 0; }));
        value.erase(std::find_if_not(value.rbegin(), value.rend(),
            [](wchar_t ch) { return std::iswspace(ch) != 0; }).base(), value.end());
        return value;
    };
    auto parseNumber = [&](const std::wstring& value, int& parsed) {
        if (value.empty() || !std::all_of(value.begin(), value.end(),
                [](wchar_t ch) { return ch >= L'0' && ch <= L'9'; }))
            return false;
        try {
            size_t consumed = 0;
            long long number = std::stoll(value, &consumed, 10);
            if (consumed != value.size() || number < kBlenderFrameMin ||
                number > kBlenderFrameMax)
                return false;
            parsed = static_cast<int>(number);
            return true;
        } catch (...) {
            return false;
        }
    };
    size_t begin = 0;
    while (begin <= expression.size()) {
        size_t comma = expression.find(L',', begin);
        std::wstring token = trim(expression.substr(
            begin, comma == std::wstring::npos ? std::wstring::npos : comma - begin));
        if (token.empty()) {
            error = L"测试帧格式无效，请使用半角逗号分隔";
            return false;
        }
        size_t dash = token.find(L'-');
        int first = 0;
        int last = 0;
        if (dash == std::wstring::npos) {
            if (!parseNumber(token, first)) {
                error = L"测试帧包含无效数字";
                return false;
            }
            last = first;
        } else {
            if (token.find(L'-', dash + 1) != std::wstring::npos ||
                !parseNumber(trim(token.substr(0, dash)), first) ||
                !parseNumber(trim(token.substr(dash + 1)), last) || last < first) {
                error = L"测试帧范围格式无效";
                return false;
            }
        }
        for (int frame = first; frame <= last; ++frame) {
            if (!allowed.contains(frame)) {
                error = L"测试帧必须位于渲染范围内并符合步长";
                return false;
            }
            selected.insert(frame);
            if (selected.size() > 100000) {
                error = L"测试帧数量超过 100000，已拒绝";
                return false;
            }
            if (frame == kBlenderFrameMax) break;
        }
        if (comma == std::wstring::npos) break;
        begin = comma + 1;
    }
    selectedFrames.assign(selected.begin(), selected.end());
    return !selectedFrames.empty();
}

File File::OpenRead(const fs::path& path) {
    return File(CreateFileW(path.c_str(), GENERIC_READ,
                            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                            OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
}

File File::Create(const fs::path& path) {
    return File(CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
}

File File::OpenWrite(const fs::path& path) {
    return File(CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ,
                            nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
}

bool File::Read(void* data, size_t size, size_t& read) {
    DWORD count = 0;
    const BOOL ok = ReadFile(handle_, data,
                             static_cast<DWORD>(std::min<size_t>(size, 64u << 20)), &count,
                             nullptr);
    read = count;
    return ok != FALSE;
}

bool File::ReadExact(void* data, size_t size) {
    auto* bytes = static_cast<char*>(data);
    while (size) {
        size_t read = 0;
        if (!Read(bytes, size, read) || read == 0) return false;
        bytes += read;
        size -= read;
    }
    return true;
}

bool File::Write(const void* data, size_t size) {
    const auto* bytes = static_cast<const char*>(data);
    while (size) {
        DWORD written = 0;
        const DWORD wanted = static_cast<DWORD>(std::min<size_t>(size, 64u << 20));
        if (!WriteFile(handle_, bytes, wanted, &written, nullptr) || written == 0) return false;
        bytes += written;
        size -= written;
    }
    return true;
}

bool File::Seek(uint64_t offset) {
    LARGE_INTEGER position{};
    position.QuadPart = static_cast<LONGLONG>(offset);
    return SetFilePointerEx(handle_, position, nullptr, FILE_BEGIN) != FALSE;
}

uint64_t File::Size() const {
    LARGE_INTEGER size{};
    return GetFileSizeEx(handle_, &size) ? static_cast<uint64_t>(size.QuadPart) : 0;
}

void File::Close() {
    if (handle_ != INVALID_HANDLE_VALUE) CloseHandle(handle_);
    handle_ = INVALID_HANDLE_VALUE;
}

bool ReadFileText(const fs::path& path, std::string& contents) {
    contents.clear();
    File input = File::OpenRead(path);
    if (!input) return false;
    contents.resize(static_cast<size_t>(input.Size()));
    if (!input.ReadExact(contents.data(), contents.size())) {
        contents.clear();
        return false;
    }
    return true;
}

std::string ReadFileText(const fs::path& path) {
    std::string contents;
    ReadFileText(path, contents);
    return contents;
}

bool WriteFileText(const fs::path& path, std::string_view contents) {
    File output = File::Create(path);
    return output && output.Write(contents);
}

bool WriteFileAtomic(const fs::path& path, std::string_view contents,
                     std::wstring& error, bool durable) {
    std::error_code ignored;
    fs::create_directories(path.parent_path(), ignored);
    fs::path temporary = path;
    temporary += L".tmp";
    {
        File output = File::Create(temporary);
        if (!output || !output.Write(contents) || (durable && !output.Flush())) {
            error = L"无法写入文件：" + path.wstring() + L"（" + LastErrorMessage() + L"）";
            output.Close();
            DeleteFileW(temporary.c_str());
            return false;
        }
    }
    if (!MoveFileExW(temporary.c_str(), path.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        error = L"无法替换文件：" + LastErrorMessage();
        DeleteFileW(temporary.c_str());
        return false;
    }
    return true;
}

namespace {

std::string HexDigest(const unsigned char* digest, size_t size) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string text(size * 2, '0');
    for (size_t index = 0; index < size; ++index) {
        text[index * 2] = kHex[digest[index] >> 4];
        text[index * 2 + 1] = kHex[digest[index] & 15];
    }
    return text;
}

std::string HashWithKey(const std::string* key, std::string_view value) {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr,
                                    key ? BCRYPT_ALG_HANDLE_HMAC_FLAG : 0) != 0)
        return {};
    BCRYPT_HASH_HANDLE hash = nullptr;
    std::array<unsigned char, 32> digest{};
    bool ok = BCryptCreateHash(
        algorithm, &hash, nullptr, 0,
        key ? reinterpret_cast<PUCHAR>(const_cast<char*>(key->data())) : nullptr,
        key ? static_cast<ULONG>(key->size()) : 0, 0) == 0;
    ok = ok && BCryptHashData(hash, reinterpret_cast<PUCHAR>(const_cast<char*>(value.data())),
                              static_cast<ULONG>(value.size()), 0) == 0;
    ok = ok && BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0) == 0;
    if (hash) BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(algorithm, 0);
    return ok ? HexDigest(digest.data(), digest.size()) : std::string{};
}

}  // namespace

Sha256::Sha256() {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    ok_ = BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) == 0 &&
        BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0) == 0;
    algorithm_ = algorithm;
    hash_ = hash;
}

Sha256::~Sha256() {
    if (hash_) BCryptDestroyHash(static_cast<BCRYPT_HASH_HANDLE>(hash_));
    if (algorithm_) BCryptCloseAlgorithmProvider(static_cast<BCRYPT_ALG_HANDLE>(algorithm_), 0);
}

bool Sha256::Update(const void* data, size_t size) {
    const auto* bytes = static_cast<const unsigned char*>(data);
    while (ok_ && size) {
        const ULONG chunk = static_cast<ULONG>(std::min<size_t>(size, 1u << 30));
        ok_ = BCryptHashData(static_cast<BCRYPT_HASH_HANDLE>(hash_),
                             const_cast<PUCHAR>(bytes), chunk, 0) == 0;
        bytes += chunk;
        size -= chunk;
    }
    return ok_;
}

std::string Sha256::Finish() {
    std::array<unsigned char, 32> digest{};
    ok_ = ok_ && BCryptFinishHash(static_cast<BCRYPT_HASH_HANDLE>(hash_), digest.data(),
                                  static_cast<ULONG>(digest.size()), 0) == 0;
    return ok_ ? HexDigest(digest.data(), digest.size()) : std::string{};
}

std::string Sha256Prefix(std::string_view value, size_t chars) {
    return HashWithKey(nullptr, value).substr(0, chars);
}

std::string HmacSha256(const std::string& key, const std::string& value) {
    return HashWithKey(&key, value);
}

std::string Sha256File(const fs::path& path, std::atomic<bool>* stopRequested) {
    File input = File::OpenRead(path);
    if (!input) return {};
    Sha256 hash;
    // Heap buffer: GUI threads have a 1 MiB stack.
    std::vector<char> buffer(4u << 20);
    for (;;) {
        if (stopRequested && stopRequested->load()) return {};
        size_t read = 0;
        if (!input.Read(buffer.data(), buffer.size(), read)) return {};
        if (read == 0) break;
        if (!hash.Update(buffer.data(), read)) return {};
    }
    return hash.Finish();
}

std::string CopyFileWithSha256(const fs::path& source, const fs::path& destination,
                               std::atomic<bool>* cancel, std::wstring& error) {
    File input = File::OpenRead(source);
    if (!input) {
        error = L"无法读取：" + source.wstring() + L"（" + LastErrorMessage() + L"）";
        return {};
    }
    std::error_code ignored;
    fs::create_directories(destination.parent_path(), ignored);
    File output = File::Create(destination);
    if (!output) {
        error = L"无法写入：" + destination.wstring() + L"（" + LastErrorMessage() + L"）";
        return {};
    }
    Sha256 hash;
    std::vector<char> buffer(4u << 20);
    for (;;) {
        if (cancel && cancel->load()) {
            error = L"操作已取消";
            break;
        }
        size_t read = 0;
        if (!input.Read(buffer.data(), buffer.size(), read)) {
            error = L"读取失败：" + LastErrorMessage();
            break;
        }
        if (read == 0) return hash.Finish();
        if (!hash.Update(buffer.data(), read) || !output.Write(buffer.data(), read)) {
            error = L"写入失败：" + LastErrorMessage();
            break;
        }
    }
    output.Close();
    DeleteFileW(destination.c_str());
    return {};
}

fs::path ExecutablePath() {
    std::vector<wchar_t> path(32768);
    DWORD size = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    return fs::path(std::wstring(path.data(), size));
}

fs::path ExecutableDirectory() {
    return ExecutablePath().parent_path();
}

bool IsNewerAppVersion(const std::string& candidate, const std::string& current) {
    auto parse = [](const std::string& value) -> std::optional<std::array<uint32_t, 4>> {
        if (value.empty() || value.size() > 32) return std::nullopt;
        std::array<uint32_t, 4> parts{};
        size_t part = 0;
        size_t start = 0;
        while (start < value.size() && part < parts.size()) {
            const size_t end = value.find('.', start);
            const size_t length = (end == std::string::npos ? value.size() : end) - start;
            if (length == 0 || length > 6) return std::nullopt;
            uint32_t number = 0;
            for (size_t index = start; index < start + length; ++index) {
                if (value[index] < '0' || value[index] > '9') return std::nullopt;
                number = number * 10 + static_cast<uint32_t>(value[index] - '0');
            }
            parts[part++] = number;
            if (end == std::string::npos) return parts;
            start = end + 1;
        }
        return std::nullopt;
    };
    const auto candidateParts = parse(candidate);
    const auto currentParts = parse(current);
    return candidateParts && currentParts && *candidateParts > *currentParts;
}

fs::path DefaultDataDirectory() {
    // FOLDERID_LocalAppData, defined locally instead of linking libuuid.
    constexpr KNOWNFOLDERID kLocalAppData = {
        0xf1b32785, 0x6fba, 0x4fcf, {0x9d, 0x55, 0x7b, 0x8e, 0x7f, 0x15, 0x70, 0x91}};
    PWSTR value = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(kLocalAppData, 0, nullptr, &value))) {
        fs::path result = fs::path(value) / L"RenderMesh";
        CoTaskMemFree(value);
        return result;
    }
    return ExecutableDirectory() / L"data";
}

double IdleSeconds() {
    LASTINPUTINFO info{};
    info.cbSize = sizeof(info);
    if (!GetLastInputInfo(&info)) return 0;
    // Both values deliberately use the 32-bit tick domain. Unsigned subtraction
    // remains correct across GetTickCount's 49.7-day rollover.
    DWORD elapsed = GetTickCount() - info.dwTime;
    return static_cast<double>(elapsed) / 1000.0;
}

bool IsComputerIdleForSharedWork(double idleSeconds, int idleAfterSeconds) {
    return idleAfterSeconds >= 1 && idleSeconds >= idleAfterSeconds;
}

void InitializeLogging(const fs::path& dataDir) {
    std::scoped_lock lock(gLogMutex);
    gLogDirectory = dataDir / L"logs";
    std::error_code ignored;
    fs::create_directories(gLogDirectory, ignored);
    gLogPath = gLogDirectory / Utf8ToWide("RenderMesh-" + LogDayStamp() + ".log");
}

void LogInfo(const std::string& message) { WriteLogLine("INFO", message); }
void LogWarning(const std::string& message) { WriteLogLine("WARN", message); }
void LogError(const std::string& message) { WriteLogLine("ERROR", message); }

fs::path CurrentLogPath() {
    std::scoped_lock lock(gLogMutex);
    return gLogPath;
}

HardwareInfo QueryHardwareInfo() {
    HardwareInfo result;
    wchar_t cpu[512]{};
    DWORD cpuBytes = sizeof(cpu);
    if (RegGetValueW(HKEY_LOCAL_MACHINE,
                     L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0",
                     L"ProcessorNameString", RRF_RT_REG_SZ, nullptr, cpu, &cpuBytes) ==
        ERROR_SUCCESS) {
        result.cpuModel = TrimAscii(WideToUtf8(cpu));
    }
    if (result.cpuModel.empty()) result.cpuModel = "Unknown CPU";

    IDXGIFactory1* factory = nullptr;
    if (SUCCEEDED(CreateDXGIFactory1(__uuidof(IDXGIFactory1),
                                     reinterpret_cast<void**>(&factory)))) {
        for (UINT index = 0;; ++index) {
            IDXGIAdapter1* adapter = nullptr;
            HRESULT status = factory->EnumAdapters1(index, &adapter);
            if (status == DXGI_ERROR_NOT_FOUND) break;
            if (FAILED(status)) break;
            DXGI_ADAPTER_DESC1 description{};
            if (SUCCEEDED(adapter->GetDesc1(&description)) &&
                !(description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) {
                std::string name = TrimAscii(WideToUtf8(description.Description));
                if (!name.empty() &&
                    std::find(result.gpuModels.begin(), result.gpuModels.end(), name) ==
                        result.gpuModels.end())
                    result.gpuModels.push_back(std::move(name));
            }
            adapter->Release();
        }
        factory->Release();
    }
    return result;
}

ConfigStore::ConfigStore(fs::path dataDir)
    : dataDir_(std::move(dataDir)), configPath_(dataDir_ / L"config.json") {}

namespace {

bool ValidRenderSoftwareProfileId(const std::string& value) {
    return !value.empty() && value.size() <= 128 &&
        std::all_of(value.begin(), value.end(), [](unsigned char ch) {
            return std::isalnum(ch) || ch == '-' || ch == '_';
        });
}

void NormalizeRenderSoftwareProfiles(Config& config) {
    std::erase_if(config.renderSoftwareProfiles, [](const RenderSoftwareProfile& profile) {
        return !ValidRenderSoftwareProfileId(profile.id) || profile.name.empty() ||
            profile.renderSystem.empty() || profile.renderSoftware.empty();
    });
    if (config.renderSoftwareProfiles.empty()) {
        config.renderSoftwareProfiles.push_back({
            "default-blender", "Blender", "Windows", "Blender", config.blenderPath});
    }
    auto selected = std::find_if(
        config.renderSoftwareProfiles.begin(), config.renderSoftwareProfiles.end(),
        [&](const RenderSoftwareProfile& profile) {
            return profile.id == config.lastRenderSoftwareProfileId;
        });
    if (selected == config.renderSoftwareProfiles.end()) {
        selected = config.renderSoftwareProfiles.begin();
        config.lastRenderSoftwareProfileId = selected->id;
    }
    config.blenderPath = selected->installPath;
}

} // namespace

bool SetActiveProfileExecutable(
    Config& config, const fs::path& executable, std::wstring& error) {
    NormalizeRenderSoftwareProfiles(config);
    auto selected = std::find_if(
        config.renderSoftwareProfiles.begin(), config.renderSoftwareProfiles.end(),
        [&](const RenderSoftwareProfile& profile) {
            return profile.id == config.lastRenderSoftwareProfileId;
        });
    if (selected == config.renderSoftwareProfiles.end()) {
        error = L"当前渲染软件配置无效";
        return false;
    }
    selected->installPath = executable;
    config.blenderPath = executable;
    return true;
}

Config ConfigStore::Load() {
    Config config;
    config.projectStoragePath = dataDir_;
    wchar_t hostname[256]{};
    DWORD hostnameSize = 256;
    GetComputerNameW(hostname, &hostnameSize);
    config.nodeName = WideToUtf8(std::wstring(hostname, hostnameSize));
    config.nodeId = NewRandomHex(16);
    config.inviteCode = NewInviteCode();
    config.clusterToken = NewRandomHex(16);
    if (!fs::exists(configPath_)) {
        NormalizeRenderSoftwareProfiles(config);
        std::wstring ignored;
        Save(config, ignored);
        return config;
    }
    try {
        json data = json::parse(ReadFileText(configPath_));
        config.nodeId = data.value("node_id", config.nodeId);
        config.nodeName = data.value("node_name", config.nodeName);
        config.clusterToken = data.value("cluster_token", config.clusterToken);
        config.inviteCode = data.value("invite_code", std::string{});
        if (!IsInviteCode(config.inviteCode))
            config.inviteCode = InviteCodeFromToken(config.clusterToken);
        config.pairingPending = data.value("pairing_pending", false);
        if (config.clusterToken.size() < 16)
            config.clusterToken = NewRandomHex(16);
        config.shareEnabled = data.value("share_enabled", false);
        config.idleAfterSeconds = std::clamp(data.value("idle_after_seconds", 180), 1, 86400);
        config.httpPort = std::clamp(data.value("http_port", 8391), 1024, 65535);
        config.discoveryPort = std::clamp(data.value("discovery_port", 8390), 1024, 65535);
        config.blenderPath = Utf8ToWide(data.value("blender_path", std::string{}));
        for (const auto& row : data.value("render_software_profiles", json::array())) {
            RenderSoftwareProfile profile;
            profile.id = row.value("id", "");
            profile.name = row.value("name", "");
            profile.renderSystem = row.value("render_system", "Windows");
            profile.renderSoftware = row.value("render_software", "Blender");
            profile.installPath = Utf8ToWide(row.value("install_path", ""));
            config.renderSoftwareProfiles.push_back(std::move(profile));
        }
        config.lastRenderSoftwareProfileId =
            data.value("last_render_software_profile_id", std::string{});
        config.manualPeerEndpoint = data.value("manual_peer_endpoint", std::string{});
        config.theme = data.value("theme", "system");
        if (config.theme != "system" && config.theme != "light" && config.theme != "dark")
            config.theme = "system";
        config.closeBehavior = data.value("close_behavior", "ask");
        if (config.closeBehavior != "ask" && config.closeBehavior != "exit" &&
            config.closeBehavior != "tray")
            config.closeBehavior = "ask";
        config.autoStart = data.value("auto_start", false);
        config.silentAutoStart = config.autoStart &&
            data.value("silent_auto_start", false);
        config.projectStoragePath = Utf8ToWide(
            data.value("project_storage_path",
                       WideToUtf8(dataDir_.wstring())));
        // Identity values generated above must be the same in every process
        // that reads this folder (desktop window, serve, command line).
        if (!data.contains("node_id") || !data.contains("cluster_token") ||
            !data.contains("invite_code")) {
            NormalizeRenderSoftwareProfiles(config);
            std::wstring ignored;
            Save(config, ignored);
        }
    } catch (...) {
        fs::path backup = configPath_;
        backup += L".broken";
        std::error_code ec;
        fs::rename(configPath_, backup, ec);
    }
    NormalizeRenderSoftwareProfiles(config);
    return config;
}

bool ConfigStore::Save(const Config& config, std::wstring& error) {
    Config normalized = config;
    if (normalized.projectStoragePath.empty())
        normalized.projectStoragePath = dataDir_;
    if (!normalized.autoStart) normalized.silentAutoStart = false;
    NormalizeRenderSoftwareProfiles(normalized);
    json profiles = json::array();
    for (const auto& profile : normalized.renderSoftwareProfiles) {
        profiles.push_back({
            {"id", profile.id}, {"name", profile.name},
            {"render_system", profile.renderSystem},
            {"render_software", profile.renderSoftware},
            {"install_path", WideToUtf8(profile.installPath.wstring())},
        });
    }
    json data = {
        {"node_id", normalized.nodeId},
        {"node_name", normalized.nodeName},
        {"invite_code", normalized.inviteCode},
        {"cluster_token", normalized.clusterToken},
        {"pairing_pending", normalized.pairingPending},
        {"share_enabled", normalized.shareEnabled},
        {"idle_after_seconds", normalized.idleAfterSeconds},
        {"http_port", normalized.httpPort},
        {"discovery_port", normalized.discoveryPort},
        {"blender_path", WideToUtf8(normalized.blenderPath.wstring())},
        {"render_software_profiles", profiles},
        {"last_render_software_profile_id", normalized.lastRenderSoftwareProfileId},
        {"manual_peer_endpoint", normalized.manualPeerEndpoint},
        {"theme", normalized.theme},
        {"close_behavior", normalized.closeBehavior},
        {"auto_start", normalized.autoStart},
        {"silent_auto_start", normalized.silentAutoStart},
        {"project_storage_path",
         WideToUtf8(normalized.projectStoragePath.wstring())},
    };
    return WriteFileAtomic(configPath_, data.dump(2), error);
}

bool ValidateFramePackageCompletion(
    size_t requestedFrames,
    size_t completedFrames,
    bool stoppedAfterCurrent,
    std::wstring& error) {
    if (!requestedFrames || completedFrames > requestedFrames) {
        if (error.empty()) error = L"渲染任务包完成帧计数无效";
        return false;
    }
    if (stoppedAfterCurrent) return true;
    if (completedFrames != requestedFrames) {
        if (error.empty()) {
            error = completedFrames == 0
                ? L"Blender 已退出，但没有完成任何请求帧"
                : L"Blender 只完成了 " + std::to_wstring(completedFrames) + L" / " +
                      std::to_wstring(requestedFrames) + L" 帧";
        }
        return false;
    }
    return true;
}


// RMBNDL1 result bundle: magic, entry count, then per entry the path length,
// file size, UTF-8 logical path and the file bytes.
bool WriteResultBundle(
    const fs::path& destination,
    const std::vector<RenderArtifact>& artifacts,
    std::wstring& error,
    std::atomic<bool>* stopRequested) {
    if (artifacts.empty() || artifacts.size() > 4096) {
        error = L"结果文件包条目数量无效";
        return false;
    }
    std::error_code ec;
    fs::create_directories(destination.parent_path(), ec);
    uint64_t requiredBytes = 512ull * 1024 * 1024;
    std::set<std::string> logicalPaths;
    for (const auto& artifact : artifacts) {
        const uint64_t size = fs::file_size(artifact.source, ec);
        if (ec || size > UINT64_MAX - requiredBytes || artifact.logicalPath.empty() ||
            artifact.logicalPath.size() > 4096 ||
            artifact.logicalPath.find('\0') != std::string::npos ||
            !logicalPaths.insert(artifact.logicalPath).second) {
            error = L"结果文件包包含无效或重复条目";
            return false;
        }
        requiredBytes += size;
    }
    ULARGE_INTEGER freeBytes{};
    if (!GetDiskFreeSpaceExW(destination.parent_path().c_str(), &freeBytes, nullptr, nullptr) ||
        freeBytes.QuadPart < requiredBytes) {
        error = L"结果文件包所在磁盘空间不足";
        return false;
    }
    fs::path temporary = destination;
    temporary += L".tmp";
    File output = File::Create(temporary);
    auto fail = [&](const wchar_t* message) {
        error = message;
        output.Close();
        DeleteFileW(temporary.c_str());
        return false;
    };
    if (!output) return fail(L"无法创建结果文件包");
    const char magic[8] = {'R', 'M', 'B', 'N', 'D', 'L', '1', '\0'};
    const uint32_t count = static_cast<uint32_t>(artifacts.size());
    if (!output.Write(magic, sizeof(magic)) || !output.Write(&count, sizeof(count)))
        return fail(L"结果文件包写入不完整");
    std::vector<char> buffer(1024 * 1024);
    for (const auto& artifact : artifacts) {
        if (stopRequested && stopRequested->load()) return fail(L"结果文件包已取消");
        File input = File::OpenRead(artifact.source);
        const uint64_t size = input ? input.Size() : 0;
        const uint32_t pathSize = static_cast<uint32_t>(artifact.logicalPath.size());
        if (!input || !output.Write(&pathSize, sizeof(pathSize)) ||
            !output.Write(&size, sizeof(size)) || !output.Write(artifact.logicalPath))
            return fail(L"结果文件包写入不完整");
        for (uint64_t remaining = size; remaining;) {
            if (stopRequested && stopRequested->load()) return fail(L"结果文件包已取消");
            const size_t wanted = static_cast<size_t>(std::min<uint64_t>(buffer.size(), remaining));
            if (!input.ReadExact(buffer.data(), wanted) || !output.Write(buffer.data(), wanted))
                return fail(L"结果文件包写入不完整");
            remaining -= wanted;
        }
    }
    output.Close();
    if (!MoveFileExW(temporary.c_str(), destination.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        error = LastErrorMessage();
        DeleteFileW(temporary.c_str());
        return false;
    }
    return true;
}

bool ExtractResultBundle(
    const fs::path& bundle,
    const fs::path& stagingDirectory,
    std::map<std::string, fs::path>& artifacts,
    std::wstring& error,
    std::atomic<bool>* stopRequested) {
    artifacts.clear();
    File input = File::OpenRead(bundle);
    char magic[8]{};
    uint32_t count = 0;
    if (!input || !input.ReadExact(magic, sizeof(magic)) ||
        !input.ReadExact(&count, sizeof(count)) ||
        std::memcmp(magic, "RMBNDL1\0", 8) != 0 || !count || count > 4096) {
        error = L"结果文件包头无效";
        return false;
    }
    std::error_code ec;
    fs::remove_all(stagingDirectory, ec);
    fs::create_directories(stagingDirectory, ec);
    if (ec) {
        error = L"无法创建结果暂存目录";
        return false;
    }
    auto fail = [&](const wchar_t* message) {
        error = message;
        artifacts.clear();
        std::error_code ignored;
        fs::remove_all(stagingDirectory, ignored);
        return false;
    };
    uint64_t total = 0;
    std::vector<char> buffer(1024 * 1024);
    for (uint32_t index = 0; index < count; ++index) {
        if (stopRequested && stopRequested->load()) return fail(L"结果文件包解包已取消");
        uint32_t pathSize = 0;
        uint64_t fileSize = 0;
        if (!input.ReadExact(&pathSize, sizeof(pathSize)) ||
            !input.ReadExact(&fileSize, sizeof(fileSize)) || !pathSize || pathSize > 4096 ||
            fileSize > 100ull * 1024 * 1024 * 1024 ||
            total > 100ull * 1024 * 1024 * 1024 - fileSize)
            return fail(L"结果文件包条目大小无效");
        std::string logical(pathSize, '\0');
        if (!input.ReadExact(logical.data(), pathSize)) return fail(L"结果文件包条目大小无效");
        const fs::path relative = Utf8ToWide(logical);
        bool safe = logical.find('\0') == std::string::npos &&
            logical.find('\\') == std::string::npos && relative.is_relative() &&
            !relative.has_root_name() && !relative.has_root_directory();
        for (const auto& component : relative)
            safe = safe && component != L".." && component != L".";
        if (!safe || artifacts.contains(logical)) return fail(L"结果文件包包含不安全路径");
        const fs::path destination = stagingDirectory / relative;
        fs::create_directories(destination.parent_path(), ec);
        File output = File::Create(destination);
        if (!output) return fail(L"结果文件包内容不完整");
        for (uint64_t remaining = fileSize; remaining;) {
            if (stopRequested && stopRequested->load()) return fail(L"结果文件包解包已取消");
            const size_t wanted = static_cast<size_t>(std::min<uint64_t>(buffer.size(), remaining));
            if (!input.ReadExact(buffer.data(), wanted) || !output.Write(buffer.data(), wanted))
                return fail(L"结果文件包内容不完整");
            remaining -= wanted;
        }
        total += fileSize;
        artifacts.emplace(std::move(logical), destination);
    }
    char trailing = 0;
    size_t extra = 0;
    if (input.Read(&trailing, 1, extra) && extra) return fail(L"结果文件包含有多余数据");
    return true;
}

std::wstring TaskStateLabel(TaskState state) {
    switch (state) {
        case TaskState::Pending: return L"待提交";
        case TaskState::Dispatching: return L"发送中";
        case TaskState::Assigned: return L"排队中";
        case TaskState::Rendering: return L"渲染中";
        case TaskState::Completed: return L"已完成";
        case TaskState::Failed: return L"失败";
        case TaskState::Stopped: return L"已停止";
    }
    return L"未知";
}

std::wstring JobStageLabel(JobStage stage) {
    switch (stage) {
        case JobStage::Rendering: return L"渲染中";
        case JobStage::Uploading: return L"上传中";
        case JobStage::UploadFailed: return L"上传失败";
        case JobStage::ReadyToConfigure: return L"上传完成";
        case JobStage::Testing: return L"测试渲染中";
        case JobStage::TestCompleted: return L"测试完成";
        case JobStage::TestFailed: return L"测试失败";
        case JobStage::Completed: return L"已完成";
        case JobStage::Failed: return L"有错误";
        case JobStage::Stopped: return L"已停止";
    }
    return L"未知";
}

}  // namespace rm
