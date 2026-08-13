#include "Core.hpp"

#include <bcrypt.h>
#include <shlobj.h>

#include <algorithm>
#include <array>
#include <fstream>
#include <iomanip>
#include <sstream>

namespace rm {

namespace {

std::wstring QuoteArg(const std::wstring& value) {
    if (value.find_first_of(L" \t\"") == std::wstring::npos) return value;
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

struct ProcessResult {
    bool started = false;
    DWORD exitCode = static_cast<DWORD>(-1);
    std::wstring error;
};

ProcessResult RunProcess(
    const fs::path& executable,
    const std::vector<std::wstring>& arguments,
    const fs::path& logPath,
    const Config* renderPolicy = nullptr,
    std::atomic<bool>* stopRequested = nullptr) {
    ProcessResult result;
    fs::create_directories(logPath.parent_path());
    HANDLE log = CreateFileW(
        logPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (log == INVALID_HANDLE_VALUE) {
        result.error = L"无法创建日志：" + LastErrorMessage();
        return result;
    }

    std::wstring command = QuoteArg(executable.wstring());
    for (const auto& arg : arguments) {
        command.push_back(L' ');
        command += QuoteArg(arg);
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
    DWORD flags = CREATE_NO_WINDOW;
    // Windows 10/11 support nested jobs. Keeping the renderer in the parent's
    // job avoids ERROR_ACCESS_DENIED under managed desktop/agent environments,
    // while the child job below still enforces the Blender process-tree cap.
    if (renderPolicy) flags |= CREATE_SUSPENDED;
    BOOL created = CreateProcessW(
        executable.c_str(), buffer.data(), nullptr, nullptr, TRUE, flags, nullptr,
        logPath.parent_path().c_str(), &startup, &process);
    CloseHandle(log);
    if (!created) {
        result.error = L"无法启动进程：" + LastErrorMessage();
        return result;
    }
    result.started = true;

    WindowsJobLimiter limiter;
    int lastPercent = -1;
    if (renderPolicy) {
        std::wstring policyError;
        if (!limiter.Assign(process.hProcess, policyError)) {
            TerminateProcess(process.hProcess, 90);
            result.error = L"无法限制 Blender 进程：" + policyError;
        } else {
            ResumeThread(process.hThread);
        }
    }

    if (result.error.empty()) {
        for (;;) {
            if (renderPolicy) {
                int wanted = IdleSeconds() >= renderPolicy->idleAfterSeconds
                    ? 100
                    : renderPolicy->activeCpuPercent;
                if (wanted != lastPercent) {
                    std::wstring policyError;
                    if (!limiter.SetCpuPercent(wanted, policyError)) {
                        result.error = policyError;
                        TerminateProcess(process.hProcess, 91);
                        break;
                    }
                    lastPercent = wanted;
                }
            }
            if (stopRequested && stopRequested->load()) {
                TerminateProcess(process.hProcess, 92);
                result.error = L"任务因应用退出而中止";
                break;
            }
            DWORD wait = WaitForSingleObject(process.hProcess, renderPolicy ? 500 : INFINITE);
            if (wait == WAIT_OBJECT_0) break;
            if (wait == WAIT_FAILED) {
                result.error = LastErrorMessage();
                break;
            }
        }
    }
    GetExitCodeProcess(process.hProcess, &result.exitCode);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return result;
}

std::string ReadText(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(input), {});
}

bool WriteTextAtomic(const fs::path& path, const std::string& text, std::wstring& error) {
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    fs::path temporary = path;
    temporary += L".tmp";
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output) {
            error = L"无法写入配置临时文件";
            return false;
        }
        output.write(text.data(), static_cast<std::streamsize>(text.size()));
        output.flush();
        if (!output) {
            error = L"配置写入不完整";
            return false;
        }
    }
    if (!MoveFileExW(
            temporary.c_str(), path.c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        error = L"无法原子替换配置：" + LastErrorMessage();
        DeleteFileW(temporary.c_str());
        return false;
    }
    return true;
}

const char* kPreflightScript = R"PY(import bpy, addon_utils, json, os, sys, glob, traceback

args = sys.argv[sys.argv.index('--') + 1:]
prepared, report_path = args[0], args[1]
warnings = []

def real_exists(path):
    if not path:
        return True
    path = bpy.path.abspath(path)
    patterns = [path.replace('<UDIM>', '*').replace('####', '*')]
    return any(os.path.exists(p) or glob.glob(p) for p in patterns)

def gather():
    rows = []
    groups = [
        ('image', bpy.data.images), ('font', bpy.data.fonts),
        ('sound', bpy.data.sounds), ('movie', bpy.data.movieclips),
        ('library', bpy.data.libraries), ('cache', bpy.data.cache_files),
        ('volume', getattr(bpy.data, 'volumes', [])),
    ]
    for kind, values in groups:
        for item in values:
            path = getattr(item, 'filepath', '') or ''
            if not path or path in {'<builtin>', '<generated>'}:
                continue
            packed = bool(getattr(item, 'packed_file', None) or getattr(item, 'packed_files', None))
            rows.append({
                'kind': kind, 'owner': getattr(item, 'name', ''),
                'path': bpy.path.abspath(path), 'packed': packed,
                'exists': real_exists(path),
            })
    return rows

report = {'pass': False, 'version': f'{bpy.app.version[0]}.{bpy.app.version[1]}',
          'addons': sorted(a.module for a in bpy.context.preferences.addons),
          'missing': [], 'unpacked': [], 'warnings': warnings}
try:
    before = gather()
    report['missing'] = [x for x in before if not x['exists']]
    if not report['missing']:
        try:
            bpy.ops.file.pack_all()
        except Exception as exc:
            warnings.append(f'pack_all: {exc}')
        bpy.ops.wm.save_as_mainfile(filepath=prepared, copy=True)
        after = gather()
        report['missing'] = [x for x in after if not x['exists']]
        report['unpacked'] = [x for x in after if x['exists'] and not x['packed']]
        report['pass'] = not report['missing'] and not report['unpacked'] and os.path.isfile(prepared)
except Exception:
    warnings.append(traceback.format_exc())
with open(report_path, 'w', encoding='utf-8') as f:
    json.dump(report, f, ensure_ascii=False, indent=2)
)PY";

const char* kCompatibilityScript = R"PY(import bpy, addon_utils, json, sys
args = sys.argv[sys.argv.index('--') + 1:]
manifest_path, report_path = args[0], args[1]
with open(manifest_path, 'r', encoding='utf-8') as f:
    manifest = json.load(f)
available = set()
for module in addon_utils.modules(refresh=True):
    available.add(module.__name__)
    available.add(module.__name__.split('.')[0])
enabled = set(addon.module for addon in bpy.context.preferences.addons)
missing_installed = [name for name in manifest.get('addons', [])
                     if name not in available and name.split('.')[0] not in available]
missing_enabled = [name for name in manifest.get('addons', [])
                   if name not in enabled]
missing = sorted(set(missing_installed + missing_enabled))
version = f'{bpy.app.version[0]}.{bpy.app.version[1]}'
result = {'pass': version == manifest.get('version') and not missing,
          'version': version, 'missing_addons': missing,
          'missing_installed': missing_installed, 'missing_enabled': missing_enabled}
with open(report_path, 'w', encoding='utf-8') as f:
    json.dump(result, f, ensure_ascii=False, indent=2)
)PY";

}  // namespace

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
    std::ostringstream output;
    output << std::hex << std::setfill('0');
    for (unsigned char value : data) output << std::setw(2) << static_cast<int>(value);
    return output.str();
}

std::string Sha256Prefix(const std::string& value, size_t chars) {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    DWORD objectSize = 0, resultSize = 0;
    std::array<unsigned char, 32> digest{};
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0)
        return {};
    BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH,
                      reinterpret_cast<PUCHAR>(&objectSize), sizeof(objectSize), &resultSize, 0);
    std::vector<unsigned char> object(objectSize);
    if (BCryptCreateHash(algorithm, &hash, object.data(), objectSize, nullptr, 0, 0) == 0) {
        BCryptHashData(hash, reinterpret_cast<PUCHAR>(const_cast<char*>(value.data())),
                       static_cast<ULONG>(value.size()), 0);
        BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0);
    }
    if (hash) BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(algorithm, 0);
    std::ostringstream output;
    output << std::hex << std::setfill('0');
    for (unsigned char byte : digest) output << std::setw(2) << static_cast<int>(byte);
    return output.str().substr(0, chars);
}

fs::path ExecutableDirectory() {
    std::vector<wchar_t> path(32768);
    DWORD size = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    return fs::path(std::wstring(path.data(), size)).parent_path();
}

fs::path DefaultDataDirectory() {
    PWSTR value = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &value))) {
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
    return static_cast<double>(GetTickCount64() - info.dwTime) / 1000.0;
}

ConfigStore::ConfigStore(fs::path dataDir)
    : dataDir_(std::move(dataDir)), configPath_(dataDir_ / L"config.json") {}

Config ConfigStore::Load() {
    Config config;
    wchar_t hostname[256]{};
    DWORD hostnameSize = 256;
    GetComputerNameW(hostname, &hostnameSize);
    config.nodeName = WideToUtf8(std::wstring(hostname, hostnameSize));
    config.nodeId = NewRandomHex(16);
    config.clusterToken = NewRandomHex(16);
    if (!fs::exists(configPath_)) {
        std::wstring ignored;
        Save(config, ignored);
        return config;
    }
    try {
        json data = json::parse(ReadText(configPath_));
        config.nodeId = data.value("node_id", config.nodeId);
        config.nodeName = data.value("node_name", config.nodeName);
        config.clusterToken = data.value("cluster_token", config.clusterToken);
        config.shareEnabled = data.value("share_enabled", false);
        config.activeCpuPercent = std::clamp(data.value("active_cpu_percent", 15), 1, 100);
        config.idleAfterSeconds = std::clamp(data.value("idle_after_seconds", 180), 30, 86400);
        config.httpPort = std::clamp(data.value("http_port", 8391), 1024, 65535);
        config.discoveryPort = std::clamp(data.value("discovery_port", 8390), 1024, 65535);
        config.blenderPath = Utf8ToWide(data.value("blender_path", std::string{}));
    } catch (...) {
        fs::path backup = configPath_;
        backup += L".broken";
        std::error_code ec;
        fs::rename(configPath_, backup, ec);
    }
    return config;
}

bool ConfigStore::Save(const Config& config, std::wstring& error) {
    json data = {
        {"node_id", config.nodeId},
        {"node_name", config.nodeName},
        {"cluster_token", config.clusterToken},
        {"share_enabled", config.shareEnabled},
        {"active_cpu_percent", config.activeCpuPercent},
        {"idle_after_seconds", config.idleAfterSeconds},
        {"http_port", config.httpPort},
        {"discovery_port", config.discoveryPort},
        {"blender_path", WideToUtf8(config.blenderPath.wstring())},
    };
    return WriteTextAtomic(configPath_, data.dump(2), error);
}

fs::path FindBlender(const Config& config) {
    std::vector<fs::path> candidates;
    if (!config.blenderPath.empty()) candidates.push_back(config.blenderPath);
    candidates.push_back(ExecutableDirectory() / L"Blender" / L"blender.exe");
    wchar_t found[32768]{};
    if (SearchPathW(nullptr, L"blender.exe", nullptr, 32768, found, nullptr) > 0)
        candidates.emplace_back(found);
    for (const wchar_t* variable : {L"ProgramFiles", L"ProgramFiles(x86)"}) {
        wchar_t root[32768]{};
        DWORD length = GetEnvironmentVariableW(variable, root, 32768);
        if (!length) continue;
        fs::path foundation = fs::path(root) / L"Blender Foundation";
        std::error_code ec;
        if (!fs::is_directory(foundation, ec)) continue;
        std::vector<fs::path> installs;
        for (const auto& entry : fs::directory_iterator(foundation, ec)) {
            fs::path executable = entry.path() / L"blender.exe";
            if (fs::is_regular_file(executable, ec)) installs.push_back(executable);
        }
        std::sort(installs.rbegin(), installs.rend());
        candidates.insert(candidates.end(), installs.begin(), installs.end());
    }
    std::error_code ec;
    for (const auto& candidate : candidates)
        if (fs::is_regular_file(candidate, ec)) return fs::absolute(candidate, ec);
    return {};
}

PreflightReport PreflightBlend(
    const Config& config,
    const fs::path& dataDir,
    const fs::path& source,
    const fs::path& prepared) {
    PreflightReport report;
    fs::path blender = FindBlender(config);
    if (blender.empty()) {
        report.summary = L"未找到 Blender。请先安装 Blender，或在设置中选择 blender.exe。";
        return report;
    }
    if (source.extension() != L".blend") {
        report.summary = L"只能提交 .blend 工程文件。";
        return report;
    }
    fs::path tools = dataDir / L"tools";
    fs::create_directories(tools);
    fs::path script = tools / L"preflight.py";
    fs::path reportPath = prepared.parent_path() / L"preflight.json";
    fs::path log = prepared.parent_path() / L"preflight.log";
    {
        std::ofstream output(script, std::ios::binary | std::ios::trunc);
        output << kPreflightScript;
    }
    ProcessResult process = RunProcess(
        blender,
        {L"--background", source.wstring(), L"--python", script.wstring(), L"--",
         prepared.wstring(), reportPath.wstring()},
        log);
    if (!process.started || process.exitCode != 0 || !fs::exists(reportPath)) {
        report.summary = L"Blender 预检失败。请查看：" + log.wstring();
        if (!process.error.empty()) report.summary += L"\n" + process.error;
        return report;
    }
    try {
        json data = json::parse(ReadText(reportPath));
        report.passed = data.value("pass", false);
        report.blenderVersion = data.value("version", std::string{});
        report.addons = data.value("addons", std::vector<std::string>{});
        report.warnings = data.value("warnings", std::vector<std::string>{});
        auto parseDependencies = [](const json& values) {
            std::vector<DependencyItem> result;
            for (const auto& item : values) {
                result.push_back({item.value("kind", "unknown"), item.value("owner", ""),
                                  item.value("path", "")});
            }
            return result;
        };
        report.missing = parseDependencies(data.value("missing", json::array()));
        report.unpacked = parseDependencies(data.value("unpacked", json::array()));
        report.preparedBlend = prepared;
        if (report.passed) {
            report.summary = L"预检通过：素材已打包，Blender " +
                Utf8ToWide(report.blenderVersion) + L"，插件环境 " +
                std::to_wstring(report.addons.size()) + L" 项。";
        } else {
            report.summary = L"预检未通过：缺失 " + std::to_wstring(report.missing.size()) +
                L" 项，无法打包 " + std::to_wstring(report.unpacked.size()) + L" 项。";
            const auto& rows = report.missing.empty() ? report.unpacked : report.missing;
            for (size_t i = 0; i < std::min<size_t>(rows.size(), 5); ++i)
                report.summary += L"\n• " + Utf8ToWide(rows[i].path);
        }
    } catch (const std::exception& exception) {
        report.summary = L"无法读取预检报告：" + Utf8ToWide(exception.what());
    }
    return report;
}

bool CheckWorkerCompatibility(
    const Config& config,
    const fs::path& dataDir,
    const fs::path& blend,
    const std::string& expectedVersion,
    const std::vector<std::string>& requiredAddons,
    std::wstring& summary) {
    fs::path blender = FindBlender(config);
    if (blender.empty()) {
        summary = L"Worker 未安装 Blender";
        return false;
    }
    fs::path tools = dataDir / L"tools";
    fs::create_directories(tools);
    fs::path script = tools / L"compatibility.py";
    fs::path manifest = blend.parent_path() / L"required-environment.json";
    fs::path report = blend.parent_path() / L"worker-environment.json";
    fs::path log = blend.parent_path() / L"compatibility.log";
    {
        std::ofstream output(script, std::ios::binary | std::ios::trunc);
        output << kCompatibilityScript;
    }
    {
        std::ofstream output(manifest, std::ios::binary | std::ios::trunc);
        output << json{{"version", expectedVersion}, {"addons", requiredAddons}}.dump(2);
    }
    ProcessResult process = RunProcess(
        blender,
        {L"--background", blend.wstring(), L"--python", script.wstring(),
         L"--", manifest.wstring(), report.wstring()},
        log);
    if (!process.started || process.exitCode != 0 || !fs::exists(report)) {
        summary = L"Worker 环境检查失败：" + log.wstring();
        return false;
    }
    try {
        json data = json::parse(ReadText(report));
        std::string version = data.value("version", std::string{});
        auto missing = data.value("missing_addons", std::vector<std::string>{});
        if (version != expectedVersion) {
            summary = L"Blender 版本不一致：需要 " + Utf8ToWide(expectedVersion) + L"，本机 " +
                Utf8ToWide(version);
            return false;
        }
        if (!missing.empty()) {
            summary = L"缺少插件：";
            for (size_t i = 0; i < std::min<size_t>(missing.size(), 8); ++i)
                summary += (i ? L"、" : L"") + Utf8ToWide(missing[i]);
            return false;
        }
        summary = L"Worker 环境兼容";
        return data.value("pass", false);
    } catch (const std::exception& exception) {
        summary = L"Worker 环境报告无效：" + Utf8ToWide(exception.what());
        return false;
    }
}

bool RenderBlenderFrame(
    const Config& config,
    const fs::path& blend,
    const fs::path& outputDir,
    int frame,
    const std::string& outputFormat,
    std::atomic<bool>& stopRequested,
    fs::path& output,
    std::wstring& error) {
    fs::path blender = FindBlender(config);
    if (blender.empty()) {
        error = L"未找到 Blender";
        return false;
    }
    fs::create_directories(outputDir);
    fs::path prefix = outputDir / L"frame_######";
    fs::path log = outputDir / (L"frame_" + std::to_wstring(frame) + L".log");
    ProcessResult process = RunProcess(
        blender,
        {L"--background", L"--disable-autoexec", blend.wstring(), L"--render-output",
         prefix.wstring(), L"--render-format", Utf8ToWide(outputFormat), L"--render-frame",
         std::to_wstring(frame)},
        log, &config, &stopRequested);
    if (!process.started || process.exitCode != 0) {
        error = process.error.empty()
            ? L"Blender 渲染失败，退出码 " + std::to_wstring(process.exitCode) + L"。日志：" +
                  log.wstring()
            : process.error;
        return false;
    }
    std::wstring extension = outputFormat == "PNG" ? L".png"
        : outputFormat == "JPEG"                    ? L".jpg"
                                                     : L".exr";
    output = outputDir / (L"frame_" + std::wstring(6 - std::min<size_t>(6, std::to_wstring(frame).size()), L'0') +
                          std::to_wstring(frame) + extension);
    if (!fs::exists(output)) {
        for (const auto& entry : fs::directory_iterator(outputDir)) {
            if (entry.path().filename().wstring().starts_with(L"frame_") &&
                entry.path().extension() != L".log") {
                output = entry.path();
                break;
            }
        }
    }
    if (!fs::exists(output)) {
        error = L"Blender 已退出，但没有生成帧文件。日志：" + log.wstring();
        return false;
    }
    return true;
}

std::wstring TaskStateLabel(TaskState state) {
    switch (state) {
        case TaskState::Pending: return L"等待";
        case TaskState::Dispatching: return L"发送中";
        case TaskState::Assigned: return L"已分配";
        case TaskState::Rendering: return L"渲染中";
        case TaskState::Completed: return L"已完成";
        case TaskState::Failed: return L"失败";
    }
    return L"未知";
}

WindowsJobLimiter::WindowsJobLimiter() {
    handle_ = CreateJobObjectW(nullptr, nullptr);
    if (!handle_) return;
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!SetInformationJobObject(handle_, JobObjectExtendedLimitInformation, &limits, sizeof(limits))) {
        CloseHandle(handle_);
        handle_ = nullptr;
    }
}

WindowsJobLimiter::~WindowsJobLimiter() {
    if (handle_) CloseHandle(handle_);
}

bool WindowsJobLimiter::Assign(HANDLE process, std::wstring& error) {
    if (!handle_) {
        error = L"无法创建 Windows Job Object";
        return false;
    }
    if (!AssignProcessToJobObject(handle_, process)) {
        error = LastErrorMessage();
        return false;
    }
    return true;
}

bool WindowsJobLimiter::SetCpuPercent(int percent, std::wstring& error) {
    if (!handle_ || percent < 1 || percent > 100) {
        error = L"CPU 百分比无效";
        return false;
    }
    JOBOBJECT_CPU_RATE_CONTROL_INFORMATION info{};
    if (percent < 100) {
        info.ControlFlags = JOB_OBJECT_CPU_RATE_CONTROL_ENABLE |
                            JOB_OBJECT_CPU_RATE_CONTROL_HARD_CAP;
        info.CpuRate = static_cast<DWORD>(percent * 100);
    }
    if (!SetInformationJobObject(handle_, JobObjectCpuRateControlInformation, &info, sizeof(info))) {
        error = LastErrorMessage();
        return false;
    }
    return true;
}

}  // namespace rm
