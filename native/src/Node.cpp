#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>

#include "Render.hpp"
#include "UiModel.hpp"
#include "Http.hpp"

#include <algorithm>
#include <cwctype>
#include <deque>
#include <fstream>
#include <set>
#include <shared_mutex>

namespace rm {

std::optional<std::string> DirectedBroadcastAddress(
    const std::string& address, unsigned long prefixLength) {
    if (prefixLength >= 31) return std::nullopt;
    in_addr parsed{};
    if (inet_pton(AF_INET, address.c_str(), &parsed) != 1) return std::nullopt;
    uint32_t hostAddress = ntohl(parsed.s_addr);
    uint32_t mask = prefixLength == 0 ? 0 : 0xffffffffu << (32 - prefixLength);
    in_addr broadcast{};
    broadcast.s_addr = htonl(hostAddress | ~mask);
    char text[INET_ADDRSTRLEN]{};
    if (!inet_ntop(AF_INET, &broadcast, text, sizeof(text))) return std::nullopt;
    return std::string(text);
}

namespace {

constexpr auto kPeerTimeout = std::chrono::seconds(8);
constexpr uint64_t kMaxTransferBytes = 100ull * 1024 * 1024 * 1024;
constexpr uint64_t kMaxUpdateBytes = 128ull * 1024 * 1024;
constexpr size_t kProjectUploadChunkBytes = 8ull * 1024 * 1024;
constexpr time_t kProjectChunkTimeoutSeconds = 60;
constexpr time_t kProjectCommitTimeoutSeconds = 60 * 60;
constexpr int kProtocolVersion = 15;
constexpr int kMatchingIdBindingProtocol = 10;
constexpr int kBlenderReadinessProtocol = 11;
constexpr int kPeerExchangeProtocol = 12;
constexpr int kDirectStatusProtocol = 13;
constexpr int kIdleReadinessProtocol = 14;
constexpr int kFrameStartProtocol = 15;
constexpr char kDiscoveryMulticastGroup[] = "239.255.83.90";

bool IsTransientFileContention(DWORD error) {
    return error == ERROR_SHARING_VIOLATION || error == ERROR_LOCK_VIOLATION ||
        error == ERROR_ACCESS_DENIED || error == ERROR_BUSY ||
        error == ERROR_USER_MAPPED_FILE;
}

DWORD CALLBACK CopyJobProgress(
    LARGE_INTEGER, LARGE_INTEGER, LARGE_INTEGER, LARGE_INTEGER, DWORD, DWORD,
    HANDLE, HANDLE, LPVOID context) {
    const auto* cancel = static_cast<const std::atomic<bool>*>(context);
    return cancel && cancel->load() ? PROGRESS_CANCEL : PROGRESS_CONTINUE;
}

bool CopyJobFile(
    const fs::path& source, const fs::path& destination,
    std::atomic<bool>* cancel) {
    return CopyFileExW(source.c_str(), destination.c_str(), CopyJobProgress, cancel, nullptr,
                       COPY_FILE_FAIL_IF_EXISTS) != FALSE;
}

bool ShouldRetryQuickEmptyPackage(
    bool remote,
    bool rendered,
    bool outputsEmpty,
    bool stoppedAfterCurrent,
    bool appStopping,
    bool packageCancelled,
    std::chrono::steady_clock::duration renderDuration,
    const std::wstring& error) {
    return remote && !rendered && outputsEmpty && !stoppedAfterCurrent && !appStopping &&
        !packageCancelled && renderDuration < std::chrono::seconds(10) &&
        error.starts_with(L"Blender 已退出，但没有完成任何请求帧");
}

bool ConstantTimeEqual(const std::string& left, const std::string& right) {
    size_t size = std::max(left.size(), right.size());
    unsigned char diff = static_cast<unsigned char>(left.size() ^ right.size());
    for (size_t i = 0; i < size; ++i) {
        unsigned char a = i < left.size() ? static_cast<unsigned char>(left[i]) : 0;
        unsigned char b = i < right.size() ? static_cast<unsigned char>(right[i]) : 0;
        diff |= static_cast<unsigned char>(a ^ b);
    }
    return diff == 0;
}

bool SafeId(const std::string& value) {
    if (value.empty() || value.size() > 80) return false;
    return std::all_of(value.begin(), value.end(), [](unsigned char ch) {
        return std::isalnum(ch) || ch == '-' || ch == '_';
    });
}

bool SafeSha256(const std::string& value) {
    return value.size() == 64 &&
        std::all_of(value.begin(), value.end(), [](unsigned char ch) {
            return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f');
        });
}

bool ReadUpdatePackage(
    const fs::path& executable, uint64_t expectedSize, std::string& contents) {
    contents.clear();
    if (expectedSize == 0 || expectedSize > kMaxUpdateBytes ||
        !ReadFileText(executable, contents) || contents.size() != expectedSize) {
        contents.clear();
        return false;
    }
    return true;
}

std::wstring SafeExportFolderComponent(std::wstring value) {
    constexpr std::wstring_view invalid = L"<>:\"/\\|?*";
    for (auto& ch : value) {
        if (ch < 32 || invalid.find(ch) != std::wstring_view::npos) ch = L'_';
    }
    for (auto& ch : value) {
        if (ch != L'.') break;
        ch = L'_';
    }
    while (!value.empty() && (value.back() == L' ' || value.back() == L'.'))
        value.pop_back();
    if (value.empty() || value == L"." || value == L"..") value = L"RenderMesh";
    if (value.size() > 80) value.resize(80);
    return value;
}

std::optional<fs::path> ManagedJobDirectory(
    const fs::path& dataDirectory, const std::string& jobId) {
    if (!SafeId(jobId)) return std::nullopt;
    std::error_code ec;
    const fs::path root = fs::absolute(dataDirectory / L"jobs", ec).lexically_normal();
    if (ec) return std::nullopt;
    const fs::path candidate = (root / Utf8ToWide(jobId)).lexically_normal();
    if (candidate.parent_path() != root) return std::nullopt;
    return candidate;
}

constexpr std::array<std::wstring_view, 7> kManagedStorageDirectories = {
    L"jobs", L"cache", L"incoming", L"preflight", L"inspection",
    L"previews", L"failed-results",
};

fs::path AbsoluteNormalPath(const fs::path& value, std::error_code& error) {
    error.clear();
    fs::path absolute = fs::absolute(value, error);
    return error ? fs::path{} : absolute.lexically_normal();
}

bool SamePathComponent(const fs::path& left, const fs::path& right) {
    return _wcsicmp(left.c_str(), right.c_str()) == 0;
}

bool PathContains(const fs::path& root, const fs::path& candidate) {
    auto rootPart = root.begin();
    auto candidatePart = candidate.begin();
    for (; rootPart != root.end(); ++rootPart, ++candidatePart) {
        if (candidatePart == candidate.end() ||
            !SamePathComponent(*rootPart, *candidatePart))
            return false;
    }
    return true;
}

bool SameAbsolutePath(const fs::path& left, const fs::path& right) {
    return PathContains(left, right) && PathContains(right, left);
}

std::optional<fs::path> RelativeManagedPath(
    const fs::path& storageRoot, const fs::path& path) {
    if (path.empty() || !path.is_absolute()) return std::nullopt;
    std::error_code error;
    const fs::path root = AbsoluteNormalPath(storageRoot, error);
    const fs::path candidate = AbsoluteNormalPath(path, error);
    if (error || root.empty() || candidate.empty() ||
        !PathContains(root, candidate) || root == candidate)
        return std::nullopt;
    fs::path relative;
    auto rootPart = root.begin();
    auto candidatePart = candidate.begin();
    while (rootPart != root.end()) {
        ++rootPart;
        ++candidatePart;
    }
    for (; candidatePart != candidate.end(); ++candidatePart)
        relative /= *candidatePart;
    return relative.empty() ? std::nullopt : std::optional<fs::path>(relative);
}

std::string StoredPathText(const fs::path& path, const fs::path& storageRoot) {
    if (const auto relative = RelativeManagedPath(storageRoot, path))
        return "storage://" + WideToUtf8(relative->generic_wstring());
    return WideToUtf8(path.wstring());
}

fs::path LoadStoredPath(const std::string& text, const fs::path& storageRoot) {
    constexpr std::string_view prefix = "storage://";
    if (!text.starts_with(prefix)) return Utf8ToWide(text);
    const fs::path relative = Utf8ToWide(text.substr(prefix.size()));
    if (relative.empty() || !relative.is_relative() || relative.has_root_name() ||
        relative.has_root_directory())
        return {};
    for (const auto& component : relative)
        if (component == L"." || component == L"..") return {};
    return (storageRoot / relative).lexically_normal();
}

fs::path RebaseManagedPath(
    const fs::path& path, const fs::path& oldRoot, const fs::path& newRoot) {
    const auto relative = RelativeManagedPath(oldRoot, path);
    return relative ? (newRoot / *relative).lexically_normal() : path;
}

void RebaseJobStoragePaths(
    RenderJob& job, const fs::path& oldRoot, const fs::path& newRoot) {
    job.preparedProject = RebaseManagedPath(job.preparedProject, oldRoot, newRoot);
    job.projectPayload = RebaseManagedPath(job.projectPayload, oldRoot, newRoot);
    for (auto& [frame, path] : job.primaryOutputs)
        path = RebaseManagedPath(path, oldRoot, newRoot);
    for (auto& target : job.compositorOutputs)
        target.directory = RebaseManagedPath(target.directory, oldRoot, newRoot);
    for (auto& frame : job.frames) {
        frame.output = RebaseManagedPath(frame.output, oldRoot, newRoot);
        frame.log = RebaseManagedPath(frame.log, oldRoot, newRoot);
        for (auto& [logical, path] : frame.artifacts)
            path = RebaseManagedPath(path, oldRoot, newRoot);
    }
}

struct StorageCopyProgressContext {
    std::atomic<bool>* cancel = nullptr;
    const std::function<void(int)>* report = nullptr;
    uint64_t completedBytes = 0;
    uint64_t totalBytes = 0;
};

DWORD CALLBACK CopyStorageProgress(
    LARGE_INTEGER totalFileSize, LARGE_INTEGER totalBytesTransferred,
    LARGE_INTEGER, LARGE_INTEGER, DWORD, DWORD, HANDLE, HANDLE, LPVOID context) {
    auto* state = static_cast<StorageCopyProgressContext*>(context);
    if (state->cancel && state->cancel->load()) return PROGRESS_CANCEL;
    if (state->report && *state->report) {
        const uint64_t fileTotal = std::max<int64_t>(0, totalFileSize.QuadPart);
        const uint64_t transferred = std::min<uint64_t>(
            fileTotal, std::max<int64_t>(0, totalBytesTransferred.QuadPart));
        const uint64_t denominator = std::max<uint64_t>(1, state->totalBytes);
        const int percent = static_cast<int>(
            std::min<uint64_t>(95, (state->completedBytes + transferred) * 95 /
                                      denominator));
        (*state->report)(percent);
    }
    return PROGRESS_CONTINUE;
}

struct ManagedStorageEntry {
    fs::path source;
    fs::path relative;
    uint64_t size = 0;
    bool directory = false;
};

bool CopyManagedStorageToNewRoot(
    const fs::path& oldRootValue, const fs::path& destinationValue,
    const std::function<void(int)>& progress, std::atomic<bool>* cancel,
    fs::path& oldRoot, fs::path& newRoot, std::wstring& error) {
    std::error_code pathError;
    oldRoot = AbsoluteNormalPath(oldRootValue, pathError);
    if (pathError || oldRoot.empty()) {
        error = L"当前工程存储目录无效";
        return false;
    }
    newRoot = AbsoluteNormalPath(destinationValue, pathError);
    if (pathError || newRoot.empty() || !newRoot.is_absolute()) {
        error = L"请选择有效的绝对工程存储目录";
        return false;
    }
    const bool sameRoot = SameAbsolutePath(oldRoot, newRoot);
    if (PathContains(oldRoot, newRoot) || PathContains(newRoot, oldRoot)) {
        error = sameRoot
            ? L"选择的目录就是当前工程存储目录"
            : L"新旧工程存储目录不能互相包含";
        return false;
    }
    const DWORD targetAttributes = GetFileAttributesW(newRoot.c_str());
    if (targetAttributes != INVALID_FILE_ATTRIBUTES) {
        if ((targetAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0 ||
            (targetAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
            error = L"目标位置必须是普通文件夹";
            return false;
        }
        if (!fs::is_empty(newRoot, pathError) || pathError) {
            error = L"请选择一个空文件夹作为新的工程存储目录";
            return false;
        }
    } else {
        const DWORD attributesError = GetLastError();
        if (attributesError != ERROR_FILE_NOT_FOUND &&
            attributesError != ERROR_PATH_NOT_FOUND) {
            error = L"无法读取目标目录：" + LastErrorMessage(attributesError);
            return false;
        }
    }
    if (SameAbsolutePath(newRoot, newRoot.root_path())) {
        error = L"不能把磁盘根目录直接设为工程存储目录";
        return false;
    }

    std::vector<ManagedStorageEntry> entries;
    uint64_t totalBytes = 0;
    try {
        for (const auto name : kManagedStorageDirectories) {
            const fs::path sourceRoot = oldRoot / name;
            if (!fs::exists(sourceRoot)) continue;
            const DWORD attributes = GetFileAttributesW(sourceRoot.c_str());
            if (attributes == INVALID_FILE_ATTRIBUTES ||
                (attributes & FILE_ATTRIBUTE_REPARSE_POINT) ||
                (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
                error = L"工程存储包含不安全的链接或异常目录：" +
                    sourceRoot.wstring();
                return false;
            }
            entries.push_back({sourceRoot, fs::path(name), 0, true});
            for (const auto& entry : fs::recursive_directory_iterator(sourceRoot)) {
                if (cancel && cancel->load()) {
                    error = L"工程存储迁移已取消";
                    return false;
                }
                const DWORD childAttributes = GetFileAttributesW(entry.path().c_str());
                if (childAttributes == INVALID_FILE_ATTRIBUTES ||
                    (childAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
                    error = L"工程存储包含无法迁移的链接或重解析点：" +
                        entry.path().wstring();
                    return false;
                }
                const fs::path relative = fs::relative(entry.path(), oldRoot);
                if (entry.is_directory()) {
                    entries.push_back({entry.path(), relative, 0, true});
                } else if (entry.is_regular_file()) {
                    const uint64_t size = entry.file_size();
                    if (totalBytes > std::numeric_limits<uint64_t>::max() - size) {
                        error = L"工程存储数据量超过可迁移范围";
                        return false;
                    }
                    totalBytes += size;
                    entries.push_back({entry.path(), relative, size, false});
                } else {
                    error = L"工程存储包含不支持的文件类型：" +
                        entry.path().wstring();
                    return false;
                }
            }
        }
    } catch (const std::exception& exception) {
        error = L"无法枚举当前工程存储：" + Utf8ToWide(exception.what());
        return false;
    }

    fs::create_directories(newRoot.parent_path(), pathError);
    if (pathError) {
        error = L"无法创建目标目录：" + Utf8ToWide(pathError.message());
        return false;
    }
    ULARGE_INTEGER freeBytes{};
    constexpr uint64_t reserve = 64ull * 1024 * 1024;
    const uint64_t requiredBytes = totalBytes >
            std::numeric_limits<uint64_t>::max() - reserve
        ? std::numeric_limits<uint64_t>::max()
        : totalBytes + reserve;
    if (!GetDiskFreeSpaceExW(newRoot.parent_path().c_str(),
                             &freeBytes, nullptr, nullptr) ||
        freeBytes.QuadPart < requiredBytes) {
        error = L"目标磁盘空间不足，无法安全复制并校验现有工程";
        return false;
    }

    const fs::path staging = newRoot.parent_path() /
        (L"." + newRoot.filename().wstring() + L".migration-" +
         Utf8ToWide(NewRandomHex(6)));
    fs::remove_all(staging, pathError);
    pathError.clear();
    fs::create_directories(staging, pathError);
    if (pathError) {
        error = L"无法创建迁移临时目录：" + Utf8ToWide(pathError.message());
        return false;
    }
    auto discardStaging = [&] {
        std::error_code ignored;
        fs::remove_all(staging, ignored);
    };

    uint64_t copiedBytes = 0;
    for (const auto& entry : entries) {
        if (cancel && cancel->load()) {
            discardStaging();
            error = L"工程存储迁移已取消，原目录保持不变";
            return false;
        }
        const fs::path destination = staging / entry.relative;
        if (entry.directory) {
            fs::create_directories(destination, pathError);
            if (pathError) {
                discardStaging();
                error = L"无法创建目标工程目录：" + Utf8ToWide(pathError.message());
                return false;
            }
            continue;
        }
        fs::create_directories(destination.parent_path(), pathError);
        if (pathError) {
            discardStaging();
            error = L"无法创建目标工程目录：" + Utf8ToWide(pathError.message());
            return false;
        }
        StorageCopyProgressContext copyProgress{
            cancel, &progress, copiedBytes, totalBytes};
        if (!CopyFileExW(
                entry.source.c_str(), destination.c_str(), CopyStorageProgress,
                &copyProgress, nullptr, COPY_FILE_FAIL_IF_EXISTS)) {
            const DWORD copyError = GetLastError();
            discardStaging();
            error = (cancel && cancel->load()) || copyError == ERROR_REQUEST_ABORTED
                ? L"工程存储迁移已取消，原目录保持不变"
                : L"复制工程存储失败：" + LastErrorMessage(copyError);
            return false;
        }
        copiedBytes += entry.size;
        // CopyFileExW reports read and write failures itself; a size check
        // catches a file that changed while it was copied.
        if (fs::file_size(destination, pathError) != entry.size || pathError) {
            discardStaging();
            error = L"目标工程文件大小不一致：" + destination.wstring();
            return false;
        }
        if (progress) progress(totalBytes == 0 ? 95 : static_cast<int>(
            std::min<uint64_t>(95, copiedBytes * 95 / totalBytes)));
    }

    if (fs::exists(newRoot, pathError)) {
        fs::remove(newRoot, pathError);
        if (pathError) {
            discardStaging();
            error = L"无法使用已选择的空目录：" + Utf8ToWide(pathError.message());
            return false;
        }
    }
    if (!MoveFileExW(staging.c_str(), newRoot.c_str(), MOVEFILE_WRITE_THROUGH)) {
        const DWORD moveError = GetLastError();
        discardStaging();
        error = L"无法发布新的工程存储目录：" + LastErrorMessage(moveError);
        return false;
    }
    if (progress) progress(95);
    return true;
}

std::string HeaderDisplayText(std::string value, size_t limit = 256) {
    if (value.size() > limit) {
        size_t cut = limit;
        while (cut > 0 &&
               (static_cast<unsigned char>(value[cut]) & 0xc0) == 0x80)
            --cut;
        value.resize(cut);
    }
    value.erase(std::remove_if(value.begin(), value.end(), [](unsigned char ch) {
        return ch < 0x20 || ch == 0x7f;
    }), value.end());
    return value;
}

std::string ResolveBlendNameToken(std::string pattern, const RenderJob& job) {
    if (pattern.empty()) return pattern;
    std::string blendName = job.sourceProject.empty()
        ? job.name : WideToUtf8(job.sourceProject.stem().wstring());
    if (blendName.empty()) return pattern;
    constexpr std::string_view token = "{blend_name}";
    for (size_t offset = 0; (offset = pattern.find(token, offset)) != std::string::npos;
         offset += blendName.size())
        pattern.replace(offset, token.size(), blendName);
    return pattern;
}

int64_t UnixMilliseconds(std::chrono::system_clock::time_point value) {
    if (value.time_since_epoch() == std::chrono::system_clock::duration::zero()) return 0;
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        value.time_since_epoch()).count();
}

std::chrono::system_clock::time_point TimeFromUnixMilliseconds(int64_t value) {
    if (value <= 0) return {};
    return std::chrono::system_clock::time_point(std::chrono::milliseconds(value));
}

bool IsActiveFrameLease(TaskState state) {
    return state == TaskState::Assigned || state == TaskState::Rendering;
}

std::string UpdateManifestSignature(
    const Config& config, const std::string& version, uint64_t size,
    const std::string& sha256) {
    return HmacSha256(config.clusterToken,
        "render-mesh-update|" + version + "|" + std::to_string(size) + "|" + sha256);
}

bool ShouldAbortPackageBeforeFirstFrame(const std::string& currentPackageId,
                                        const std::string& requestedPackageId,
                                        bool currentPackageHasStartedFrame) {
    return currentPackageId != requestedPackageId || !currentPackageHasStartedFrame;
}

int HeaderInt(const http::Request& request, const char* name, int fallback = 0) {
    try {
        return std::stoi(request.get_header_value(name));
    } catch (...) {
        return fallback;
    }
}

uint64_t HeaderUint64(
    const http::Request& request, const char* name, uint64_t fallback = 0) {
    try {
        return std::stoull(request.get_header_value(name));
    } catch (...) {
        return fallback;
    }
}

bool ParseProjectUploadStatus(
    const std::string& body, bool& cached, uint64_t& offset,
    std::string& error) {
    cached = false;
    offset = 0;
    json value = json::parse(body, nullptr, false);
    if (value.is_discarded() || !value.is_object()) {
        error = "upload status response was invalid";
        return false;
    }
    if (value.contains("cached")) {
        if (!value["cached"].is_boolean()) {
            error = "upload status cached flag was invalid";
            return false;
        }
        cached = value["cached"].get<bool>();
    }
    if (cached) return true;
    if (!value.contains("offset") || !value["offset"].is_number_unsigned()) {
        error = "upload status offset was invalid";
        return false;
    }
    offset = value["offset"].get<uint64_t>();
    return true;
}

bool HeaderOutputTargets(
    const http::Request& request, const char* name,
    std::vector<OutputTarget>& targets) {
    targets.clear();
    std::string value = request.get_header_value(name);
    if (value.empty()) return true;
    if (value.size() > 4096) return false;
    try {
        json rows = json::parse(value);
        if (!rows.is_array() || rows.size() > 128) return false;
        std::set<std::string> ids;
        for (const auto& row : rows) {
            OutputTarget target;
            if (!ParseOutputTargetMetadata(row, target) ||
                !ids.insert(target.id).second)
                return false;
            targets.push_back(std::move(target));
        }
        return true;
    } catch (...) {
        return false;
    }
}

bool StreamRequestToFile(
    const http::ContentReader& reader,
    const fs::path& destination,
    std::wstring& error,
    std::atomic<bool>* stopRequested = nullptr) {
    fs::create_directories(destination.parent_path());
    fs::path temporary = destination;
    temporary += L".receiving";
    File output = File::Create(temporary);
    if (!output) {
        error = L"无法创建接收文件";
        return false;
    }
    uint64_t receivedBytes = 0;
    bool written = true;
    bool received = reader([&](const char* data, size_t size) {
        if (stopRequested && stopRequested->load()) return false;
        if (size > kMaxTransferBytes - receivedBytes) return false;
        receivedBytes += size;
        written = output.Write(data, size);
        return written;
    });
    output.Close();
    if (!received || !written) {
        error = stopRequested && stopRequested->load() ? L"文件接收已取消"
                                                       : L"文件接收中断";
        DeleteFileW(temporary.c_str());
        return false;
    }
    if (!MoveFileExW(temporary.c_str(), destination.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        error = LastErrorMessage();
        DeleteFileW(temporary.c_str());
        return false;
    }
    return true;
}


bool WriteDurableFile(const fs::path& path, const std::string& contents,
                      std::wstring& error) {
    return WriteFileAtomic(path, contents, error, true);
}

bool CancellationRequested(
    const std::atomic<bool>* stopRequested,
    const std::atomic<bool>* operationCancelled = nullptr) {
    return (stopRequested && stopRequested->load()) ||
           (operationCancelled && operationCancelled->load());
}

bool HasTransferCapacity(
    const http::Request& request, const fs::path& directory,
    uint64_t stagingCopies = 1) {
    std::string value = request.get_header_value("Content-Length");
    // A preflight disk-space guarantee is impossible for an unbounded chunked
    // body. RenderMesh clients always send a known file size, so reject other
    // transfer forms before accepting bytes.
    if (value.empty()) return false;
    uint64_t requested = 0;
    try {
        requested = std::stoull(value);
    } catch (...) {
        return false;
    }
    if (!requested || requested > kMaxTransferBytes) return false;
    std::error_code ignored;
    fs::create_directories(directory, ignored);
    ULARGE_INTEGER freeBytes{};
    if (!GetDiskFreeSpaceExW(directory.c_str(), &freeBytes, nullptr, nullptr)) return false;
    constexpr uint64_t reserve = 512ull * 1024 * 1024;
    if (!stagingCopies || requested > (UINT64_MAX - reserve) / stagingCopies) return false;
    uint64_t required = requested * stagingCopies + reserve;
    return freeBytes.QuadPart >= required;
}

bool HasFileCapacity(const fs::path& directory, uint64_t requested) {
    std::error_code ignored;
    fs::create_directories(directory, ignored);
    ULARGE_INTEGER freeBytes{};
    if (!GetDiskFreeSpaceExW(directory.c_str(), &freeBytes, nullptr, nullptr)) return false;
    constexpr uint64_t reserve = 512ull * 1024 * 1024;
    return requested <= UINT64_MAX - reserve && freeBytes.QuadPart >= requested + reserve;
}

bool CopyFileCancellable(
    const fs::path& source, const fs::path& destination,
    std::atomic<bool>* stopRequested, std::wstring& error) {
    File input = File::OpenRead(source);
    File output = File::Create(destination);
    std::vector<char> buffer(4u << 20);
    bool ok = input && output;
    while (ok) {
        if (stopRequested && stopRequested->load()) {
            error = L"结果发布已取消";
            ok = false;
            break;
        }
        size_t read = 0;
        ok = input.Read(buffer.data(), buffer.size(), read);
        if (!ok || read == 0) break;
        ok = output.Write(buffer.data(), read);
    }
    output.Close();
    if (!ok) {
        if (error.empty()) error = L"结果文件复制不完整";
        std::error_code ignored;
        fs::remove(destination, ignored);
    }
    return ok;
}

std::string MatchingIdBinding(const Config& config) {
    if (!IsInviteCode(config.inviteCode) || config.clusterToken.size() < 16) return {};
    return HmacSha256(
        config.clusterToken, "matching-id-binding-v1|" + config.inviteCode);
}

std::string PairingDiscoveryTag(const Config& config) {
    if (!IsInviteCode(config.inviteCode)) return {};
    return HmacSha256(config.inviteCode, "pairing-discovery-v1");
}

std::string DiscoverySignature(const Config& config, const json& message) {
    std::string signedValue =
        message.value("node_id", "") + "|" + message.value("nonce", "") + "|" +
        std::to_string(message.value("port", 0)) + "|" +
        (message.value("share", false) ? "1" : "0") + "|" +
        std::to_string(message.value("assigned", 0)) + "|" +
        message.value("task_id", "") + "|" +
        json(message.value("addresses", std::vector<std::string>{})).dump();
    if (message.value("protocol", 1) >= 2) {
        signedValue += "|" + message.value("name", "") + "|" +
            std::to_string(message.value("cpu", 0)) + "|" +
            std::to_string(message.value("idle", 0)) + "|" +
            message.value("cpu_model", "") + "|" +
            json(message.value("gpu_models", std::vector<std::string>{})).dump();
    }
    if (message.value("protocol", 1) >= 3)
        signedValue += "|" + std::string(message.value("project_cache", false) ? "1" : "0");
    if (message.value("protocol", 1) >= 4)
        signedValue += "|" + std::string(message.value("frame_packages", false) ? "1" : "0");
    if (message.value("protocol", 1) >= 5)
        signedValue += "|" + std::string(message.value("result_bundles", false) ? "1" : "0");
    if (message.value("protocol", 1) >= 6)
        signedValue += "|" + std::string(message.value("package_cancel", false) ? "1" : "0");
    if (message.value("protocol", 1) >= 7) {
        signedValue += "|" + message.value("activity_job", "") + "|" +
            json(message.value("activity_frames", std::vector<int>{})).dump() + "|" +
            std::to_string(message.value("activity_completed", size_t{0})) + "|" +
            std::string(message.value("activity_rendering", false) ? "1" : "0");
    }
    if (message.value("protocol", 1) >= 8)
        signedValue += "|" + std::string(message.value("job_cleanup", false) ? "1" : "0");
    if (message.value("protocol", 1) >= 9)
        signedValue += "|" +
            std::string(message.value("resumable_project_upload", false) ? "1" : "0") +
            "|" + std::to_string(message.value("max_package_frames", 1));
    // Protocol 10 introduces the matching-ID binding. Older protocol values
    // have an already-deployed signature layout and must not be silently
    // redefined, otherwise two binaries can both claim protocol 9 but be
    // unable to authenticate one another.
    if (message.value("protocol", 1) >= kMatchingIdBindingProtocol)
        signedValue += "|" + message.value("matching_id_binding", "");
    if (message.value("protocol", 1) >= kBlenderReadinessProtocol)
        signedValue += "|" + std::string(message.value("blender_ready", false) ? "1" : "0");
    if (message.value("protocol", 1) >= kPeerExchangeProtocol)
        signedValue += "|" + message.value("app_version", "") + "|" +
            std::string(message.value("peer_exchange", false) ? "1" : "0");
    if (message.value("protocol", 1) >= kDirectStatusProtocol)
        signedValue += "|" +
            std::string(message.value("direct_status", false) ? "1" : "0");
    if (message.value("protocol", 1) >= kIdleReadinessProtocol)
        signedValue += "|" +
            std::string(message.value("idle_ready", false) ? "1" : "0");
    if (message.value("protocol", 1) >= kFrameStartProtocol)
        signedValue += "|" +
            std::string(message.value("frame_start", false) ? "1" : "0");
    return HmacSha256(config.clusterToken, signedValue);
}

std::string ActivitySnapshotProof(
    const Config& config, const std::string& challenge,
    const std::string& nodeId, const WorkerActivity& activity) {
    return HmacSha256(
        config.clusterToken,
        "activity-proof|" + nodeId + "|" + challenge + "|" +
            HeaderDisplayText(activity.jobName, 128) + "|" +
            json(activity.frames).dump() + "|" +
            std::to_string(activity.completedFrames) + "|" +
            (activity.rendering ? "1" : "0"));
}

std::string CleanupCapabilityProof(
    const Config& config, const std::string& challenge,
    const std::string& nodeId, bool supportsJobCleanup) {
    return HmacSha256(
        config.clusterToken, "cleanup-capability-proof|" + nodeId + "|" + challenge + "|" +
            (supportsJobCleanup ? "1" : "0"));
}

std::string BlenderReadinessProof(
    const Config& config, const std::string& challenge,
    const std::string& nodeId, bool rendererReady) {
    return HmacSha256(
        config.clusterToken, "blender-readiness-proof|" + nodeId + "|" + challenge + "|" +
            (rendererReady ? "1" : "0"));
}

std::string PeerExchangeCapabilityProof(
    const Config& config, const std::string& challenge, const std::string& nodeId,
    int protocol, const std::string& appVersion, bool supportsPeerExchange) {
    return HmacSha256(
        config.clusterToken, "peer-exchange-capability-proof|" + nodeId + "|" + challenge +
            "|" + std::to_string(protocol) + "|" + HeaderDisplayText(appVersion, 32) + "|" +
            (supportsPeerExchange ? "1" : "0"));
}

std::string PeerDirectoryProof(
    const Config& config, const std::string& nodeId,
    const std::string& challenge, const json& peers) {
    return HmacSha256(
        config.clusterToken,
        "peer-directory-v1|" + nodeId + "|" + challenge + "|" + peers.dump());
}

std::string DirectStatusProof(
    const Config& config, const std::string& nodeId,
    const std::string& challenge, const json& snapshot) {
    return HmacSha256(
        config.clusterToken,
        "direct-status-v1|" + nodeId + "|" + challenge + "|" + snapshot.dump());
}

struct LocalIPv4Interface {
    std::string address;
    std::optional<std::string> broadcast;
};

std::vector<LocalIPv4Interface> BroadcastCapableInterfaces(
    const std::vector<LocalIPv4Interface>& interfaces) {
    std::vector<LocalIPv4Interface> result;
    for (const auto& network : interfaces)
        if (network.broadcast) result.push_back(network);
    return result;
}

std::vector<LocalIPv4Interface> LocalIPv4Interfaces() {
    ULONG size = 15 * 1024;
    std::vector<unsigned char> storage(size);
    auto* adapters = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(storage.data());
    ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST |
                  GAA_FLAG_SKIP_DNS_SERVER;
    ULONG status = GetAdaptersAddresses(AF_INET, flags, nullptr, adapters, &size);
    if (status == ERROR_BUFFER_OVERFLOW) {
        storage.resize(size);
        adapters = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(storage.data());
        status = GetAdaptersAddresses(AF_INET, flags, nullptr, adapters, &size);
    }
    std::vector<LocalIPv4Interface> interfaces;
    if (status != NO_ERROR) return interfaces;
    for (auto* adapter = adapters; adapter; adapter = adapter->Next) {
        if (adapter->OperStatus != IfOperStatusUp ||
            adapter->IfType == IF_TYPE_SOFTWARE_LOOPBACK)
            continue;
        for (auto* unicast = adapter->FirstUnicastAddress; unicast; unicast = unicast->Next) {
            if (!unicast->Address.lpSockaddr ||
                unicast->Address.lpSockaddr->sa_family != AF_INET)
                continue;
            auto* socketAddress = reinterpret_cast<sockaddr_in*>(unicast->Address.lpSockaddr);
            char text[INET_ADDRSTRLEN]{};
            if (!inet_ntop(AF_INET, &socketAddress->sin_addr, text, sizeof(text))) continue;
            std::string address(text);
            if (address.rfind("169.254.", 0) == 0) continue;
            interfaces.push_back({address,
                DirectedBroadcastAddress(address, unicast->OnLinkPrefixLength)});
        }
    }
    std::sort(interfaces.begin(), interfaces.end(),
              [](const auto& left, const auto& right) { return left.address < right.address; });
    interfaces.erase(std::unique(interfaces.begin(), interfaces.end(),
        [](const auto& left, const auto& right) { return left.address == right.address; }),
        interfaces.end());
    return interfaces;
}

std::vector<std::string> LocalIPv4Addresses() {
    std::vector<std::string> addresses;
    for (const auto& value : LocalIPv4Interfaces()) addresses.push_back(value.address);
    return addresses;
}

bool ParseIPv4Endpoint(const std::string& value, std::string& address, int& port) {
    std::string normalized = value;
    const std::string fullWidthColon = "\xef\xbc\x9a";
    for (size_t position = normalized.find(fullWidthColon); position != std::string::npos;
         position = normalized.find(fullWidthColon))
        normalized.replace(position, fullWidthColon.size(), ":");
    normalized.erase(std::remove_if(normalized.begin(), normalized.end(),
        [](unsigned char ch) { return std::isspace(ch); }), normalized.end());
    size_t colon = normalized.rfind(':');
    if (colon == std::string::npos || colon == 0 || colon + 1 >= normalized.size()) return false;
    address = normalized.substr(0, colon);
    in_addr parsed{};
    if (inet_pton(AF_INET, address.c_str(), &parsed) != 1) return false;
    try {
        size_t consumed = 0;
        port = std::stoi(normalized.substr(colon + 1), &consumed);
        return consumed == normalized.size() - colon - 1 && port >= 1024 && port <= 65535;
    } catch (...) {
        return false;
    }
}

bool PeerExplicitlyStoppedTask(const Peer& peer, const std::string& taskId) {
    return peer.currentTaskKnown ? peer.currentTaskId != taskId : peer.assigned == 0;
}

size_t EffectiveRemotePackageLimit(const Peer& peer) {
    if (!peer.supportsFramePackages || !peer.supportsPackageCancellation ||
        !peer.supportsFrameStart)
        return 1;
    return std::clamp(peer.maxPackageFrames, size_t{1}, size_t{20});
}

void PreserveAuthenticatedV2Metadata(Peer& candidate, const Peer& existing) {
    if (candidate.protocolVersion >= existing.protocolVersion) return;
    candidate.protocolVersion = existing.protocolVersion;
    candidate.supportsProjectCache = existing.supportsProjectCache;
    candidate.supportsFramePackages = existing.supportsFramePackages;
    candidate.supportsResultBundles = existing.supportsResultBundles;
    candidate.supportsPackageCancellation = existing.supportsPackageCancellation;
    candidate.supportsJobCleanup = existing.supportsJobCleanup;
    candidate.supportsResumableProjectUpload = existing.supportsResumableProjectUpload;
    candidate.supportsPeerExchange = existing.supportsPeerExchange;
    candidate.supportsDirectStatus = existing.supportsDirectStatus;
    candidate.supportsFrameStart = existing.supportsFrameStart;
    candidate.rendererReady = existing.rendererReady;
    candidate.idleReadyKnown = existing.idleReadyKnown;
    candidate.idleReady = existing.idleReady;
    candidate.maxPackageFrames = existing.maxPackageFrames;
    candidate.name = existing.name;
    candidate.appVersion = existing.appVersion;
    candidate.cpuPercent = existing.cpuPercent;
    candidate.idleSeconds = existing.idleSeconds;
    candidate.cpuModel = existing.cpuModel;
    candidate.gpuModels = existing.gpuModels;
    candidate.activity = existing.activity;
}

bool IsSameVersionCompatibilityBeacon(const Peer& candidate, const Peer& existing) {
    return candidate.protocolVersion == kBlenderReadinessProtocol &&
        candidate.protocolVersion < existing.protocolVersion &&
        !candidate.appVersion.empty() && candidate.appVersion == existing.appVersion;
}

fs::path ProjectCacheDirectory(const fs::path& dataDir, const std::string& hash) {
    return dataDir / L"cache" / L"projects" / Utf8ToWide(hash);
}

fs::path ProjectUploadDirectory(const fs::path& dataDir, const std::string& hash) {
    return dataDir / L"cache" / L"uploads" / Utf8ToWide(hash);
}

fs::path ProjectUploadPartial(const fs::path& dataDir, const std::string& hash) {
    return ProjectUploadDirectory(dataDir, hash) / L"source.blend.partial";
}

constexpr uint32_t kMaxProjectBundleEntries = 100001;

fs::path ProjectPayloadPath(const RenderJob& job) {
    return job.projectPayload.empty() ? job.preparedProject : job.projectPayload;
}

// tex project bundles need resumable upload (every v11+ node supports it).
bool PeerSupportsProjectBundle(const Peer& peer) {
    return peer.supportsResumableProjectUpload;
}

bool SafeWindowsProjectBundleComponent(const std::wstring& component) {
    if (component.empty() || component == L"." || component == L".." ||
        component.back() == L'.' || component.back() == L' ')
        return false;
    for (const wchar_t value : component) {
        if (value < 32 || value == L':' || value == L'<' || value == L'>' ||
            value == L'"' || value == L'|' || value == L'?' || value == L'*')
            return false;
    }
    std::wstring device = component.substr(0, component.find(L'.'));
    std::transform(device.begin(), device.end(), device.begin(),
                   [](wchar_t value) { return std::towupper(value); });
    if (device == L"CON" || device == L"PRN" || device == L"AUX" ||
        device == L"NUL" || device == L"CLOCK$" || device == L"CONIN$" ||
        device == L"CONOUT$")
        return false;
    const bool numberedDevice = device.size() == 4 &&
        (device.starts_with(L"COM") || device.starts_with(L"LPT")) &&
        ((device[3] >= L'1' && device[3] <= L'9') ||
         device[3] == L'\u00b9' || device[3] == L'\u00b2' ||
         device[3] == L'\u00b3');
    return !numberedDevice;
}

bool SafeProjectBundlePath(const std::string& logical) {
    if (logical.empty() || logical.size() > 4096 ||
        logical.find('\0') != std::string::npos ||
        logical.find('\\') != std::string::npos ||
        logical.find("//") != std::string::npos || logical.back() == '/')
        return false;
    const std::wstring wide = Utf8ToWide(logical);
    if (wide.empty()) return false;
    fs::path relative = wide;
    if (!relative.is_relative() || relative.has_root_name() ||
        relative.has_root_directory())
        return false;
    for (const auto& component : relative)
        if (!SafeWindowsProjectBundleComponent(component.native())) return false;
    return logical == "source.blend" || logical.starts_with("tex/");
}

bool ProjectBundleDestinationWithin(
    const fs::path& stagingDirectory, const fs::path& destination) {
    std::error_code pathError;
    const fs::path root = fs::absolute(stagingDirectory, pathError).lexically_normal();
    if (pathError) return false;
    const fs::path candidate = fs::absolute(destination, pathError).lexically_normal();
    if (pathError) return false;
    auto rootPart = root.begin();
    auto candidatePart = candidate.begin();
    for (; rootPart != root.end(); ++rootPart, ++candidatePart) {
        if (candidatePart == candidate.end() ||
            _wcsicmp(rootPart->c_str(), candidatePart->c_str()) != 0)
            return false;
    }
    return candidatePart != candidate.end();
}

// Writes a file while hashing every byte, so a snapshot is read only once.
struct HashingWriter {
    File file;
    Sha256 hash;
    uint64_t written = 0;
    std::function<void(uint64_t)> progress;

    bool Write(const void* data, size_t size) {
        if (!hash.Update(data, size) || !file.Write(data, size)) return false;
        written += size;
        if (progress) progress(written);
        return true;
    }
};

// Opens `path` for reading. Windows Defender, sync clients, indexers or
// Blender itself can hold a freshly written file for a moment, so sharing
// conflicts are retried for up to five seconds.
File OpenReadAfterTransientLocks(const fs::path& path, std::atomic<bool>* cancel) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    for (;;) {
        File file = File::OpenRead(path);
        if (file) return file;
        const DWORD code = GetLastError();
        if ((cancel && cancel->load()) || !IsTransientFileContention(code) ||
            std::chrono::steady_clock::now() >= deadline) {
            SetLastError(code);
            return file;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
}

bool CopyIntoWriter(const fs::path& source, HashingWriter& output, uint64_t& size,
                    std::atomic<bool>* cancel, std::wstring& error) {
    File input = OpenReadAfterTransientLocks(source, cancel);
    if (!input) {
        error = L"无法读取：" + source.wstring() + L"（" + LastErrorMessage() + L"）";
        return false;
    }
    size = input.Size();
    std::vector<char> buffer(static_cast<size_t>(std::min<uint64_t>(size, 4u << 20)) + 1);
    uint64_t remaining = size;
    while (remaining) {
        if (cancel && cancel->load()) {
            error = L"工程准备已取消";
            return false;
        }
        size_t read = 0;
        if (!input.Read(buffer.data(),
                        static_cast<size_t>(std::min<uint64_t>(buffer.size(), remaining)), read) ||
            !read) {
            error = L"文件在读取时发生变化：" + source.wstring();
            return false;
        }
        if (!output.Write(buffer.data(), read)) {
            error = L"写入工程快照失败：" + LastErrorMessage();
            return false;
        }
        remaining -= read;
    }
    return true;
}

// RMPKG1 bundle: magic, entry count, then per entry the path length, file
// size, UTF-8 logical path and the file bytes. Returns the bundle SHA-256.
std::string WriteProjectBundle(
    const fs::path& destination,
    const std::vector<std::pair<std::string, fs::path>>& files,
    std::wstring& error, std::atomic<bool>* cancel,
    const std::function<void(uint64_t, uint64_t)>& progress = {}) {
    if (files.empty() || files.size() > kMaxProjectBundleEntries) {
        error = L"tex 工程包文件数量超过 100000 个";
        return {};
    }
    std::vector<uint64_t> sizes;
    uint64_t total = 0;
    std::set<std::string> logicalPaths;
    for (const auto& [logical, source] : files) {
        std::error_code sizeError;
        const uint64_t size = fs::file_size(source, sizeError);
        if (sizeError || size > kMaxTransferBytes || total > kMaxTransferBytes - size ||
            !SafeProjectBundlePath(logical) || !logicalPaths.insert(logical).second) {
            error = L"tex 工程包包含无效、重复或过大的文件";
            return {};
        }
        sizes.push_back(size);
        total += size;
    }
    if (!HasFileCapacity(destination.parent_path(), total)) {
        error = L"工程存储磁盘空间不足，无法保存 .blend 与 tex 工程包";
        return {};
    }
    fs::path temporary = destination;
    temporary += L".tmp";
    HashingWriter output;
    output.file = File::Create(temporary);
    if (progress) output.progress = [&](uint64_t written) { progress(written, total); };
    auto fail = [&](std::wstring message) {
        error = std::move(message);
        output.file.Close();
        DeleteFileW(temporary.c_str());
        return std::string{};
    };
    if (!output.file) return fail(L"无法创建 tex 工程包：" + LastErrorMessage());
    const char magic[8] = {'R', 'M', 'P', 'K', 'G', '1', '\0', '\0'};
    const uint32_t count = static_cast<uint32_t>(files.size());
    if (!output.Write(magic, sizeof(magic)) || !output.Write(&count, sizeof(count)))
        return fail(L"tex 工程包写入不完整");
    for (size_t index = 0; index < files.size(); ++index) {
        const auto& [logical, source] = files[index];
        const uint32_t pathSize = static_cast<uint32_t>(logical.size());
        uint64_t copied = 0;
        std::wstring copyError;
        if (!output.Write(&pathSize, sizeof(pathSize)) ||
            !output.Write(&sizes[index], sizeof(uint64_t)) ||
            !output.Write(logical.data(), pathSize) ||
            !CopyIntoWriter(source, output, copied, cancel, copyError) || copied != sizes[index])
            return fail(copyError.empty() ? L"文件在打包时发生变化：" + source.wstring()
                                          : copyError);
    }
    std::string hash = output.hash.Finish();
    output.file.Close();
    if (hash.empty() || !MoveFileExW(temporary.c_str(), destination.c_str(),
                                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        return fail(L"无法发布 tex 工程包：" + LastErrorMessage());
    return hash;
}

bool ExtractProjectBundle(
    const fs::path& bundle, const fs::path& stagingDirectory,
    std::wstring& error, std::atomic<bool>* cancel = nullptr) {
    File input = File::OpenRead(bundle);
    char magic[8]{};
    uint32_t count = 0;
    const char expected[8] = {'R', 'M', 'P', 'K', 'G', '1', '\0', '\0'};
    if (!input || !input.ReadExact(magic, sizeof(magic)) ||
        !input.ReadExact(&count, sizeof(count)) ||
        std::memcmp(magic, expected, sizeof(expected)) != 0 ||
        !count || count > kMaxProjectBundleEntries) {
        error = L"tex 工程包头无效";
        return false;
    }
    std::error_code fileError;
    fs::remove_all(stagingDirectory, fileError);
    fileError.clear();
    fs::create_directories(stagingDirectory / L"tex", fileError);
    auto fail = [&](const wchar_t* message) {
        error = message;
        std::error_code ignored;
        fs::remove_all(stagingDirectory, ignored);
        return false;
    };
    if (fileError) return fail(L"无法还原 tex 文件夹");
    std::set<std::wstring> caseInsensitivePaths;
    bool hasBlend = false;
    uint64_t total = 0;
    std::vector<char> buffer(4u << 20);
    for (uint32_t index = 0; index < count; ++index) {
        if (cancel && cancel->load()) return fail(L"tex 工程包解包已取消");
        uint32_t pathSize = 0;
        uint64_t size = 0;
        if (!input.ReadExact(&pathSize, sizeof(pathSize)) ||
            !input.ReadExact(&size, sizeof(size)) || !pathSize || pathSize > 4096 ||
            size > kMaxTransferBytes || total > kMaxTransferBytes - size)
            return fail(L"tex 工程包条目大小无效");
        std::string logical(pathSize, '\0');
        if (!input.ReadExact(logical.data(), pathSize)) return fail(L"tex 工程包条目大小无效");
        std::wstring folded = Utf8ToWide(logical);
        std::transform(folded.begin(), folded.end(), folded.begin(),
                       [](wchar_t value) { return std::towlower(value); });
        if (!SafeProjectBundlePath(logical) || !caseInsensitivePaths.insert(folded).second)
            return fail(L"tex 工程包包含不安全或重复路径");
        hasBlend = hasBlend || logical == "source.blend";
        const fs::path destination = (stagingDirectory / Utf8ToWide(logical)).lexically_normal();
        if (!ProjectBundleDestinationWithin(stagingDirectory, destination))
            return fail(L"tex 工程包路径超出解包目录");
        fs::create_directories(destination.parent_path(), fileError);
        File output = File::Create(destination);
        if (fileError || !output) return fail(L"无法创建 tex 工程包条目");
        for (uint64_t remaining = size; remaining;) {
            if (cancel && cancel->load()) return fail(L"tex 工程包解包已取消");
            const size_t wanted = static_cast<size_t>(std::min<uint64_t>(buffer.size(), remaining));
            if (!input.ReadExact(buffer.data(), wanted) || !output.Write(buffer.data(), wanted))
                return fail(L"tex 工程包内容不完整");
            remaining -= wanted;
        }
        total += size;
    }
    char trailing = 0;
    size_t extra = 0;
    if (!hasBlend || (input.Read(&trailing, 1, extra) && extra) ||
        !fs::is_regular_file(stagingDirectory / L"source.blend"))
        return fail(L"tex 工程包缺少 source.blend 或包含多余数据");
    return true;
}

// Copies the prepared project into <storage>\jobs\<id>\ and computes the
// payload SHA-256 in the same pass. A prepared copy in the storage work
// folder is moved instead (same volume, no data copy). A sibling `tex`
// folder is streamed straight into the RMPKG1 bundle; the local renderer
// extracts it only if this computer renders the job itself.
struct ProjectSnapshot {
    fs::path project;
    fs::path payload;
    bool bundle = false;
    std::string hash;
};

bool SnapshotProject(const PreflightReport& report, const fs::path& jobDirectory,
                     ProjectSnapshot& snapshot, std::wstring& error,
                     std::atomic<bool>* cancel,
                     const std::function<void(uint64_t, uint64_t)>& progress) {
    const fs::path texSource = report.sourceProject.parent_path() / L"tex";
    const DWORD texAttributes = GetFileAttributesW(texSource.c_str());
    snapshot.bundle = texAttributes != INVALID_FILE_ATTRIBUTES &&
        (texAttributes & FILE_ATTRIBUTE_DIRECTORY);
    std::error_code ignored;
    fs::create_directories(jobDirectory, ignored);
    if (!snapshot.bundle) {
        snapshot.project = snapshot.payload = jobDirectory / L"source.blend";
        if (report.preparedCopy &&
            MoveFileExW(report.preparedProject.c_str(), snapshot.payload.c_str(),
                        MOVEFILE_REPLACE_EXISTING)) {
            snapshot.hash = Sha256File(snapshot.payload, cancel);
        } else {
            std::error_code sizeError;
            const uint64_t total = fs::file_size(report.preparedProject, sizeError);
            HashingWriter output;
            output.file = File::Create(snapshot.payload);
            if (progress) output.progress = [&](uint64_t written) { progress(written, total); };
            uint64_t copied = 0;
            if (!output.file) {
                error = L"无法写入工程快照：" + LastErrorMessage();
                return false;
            }
            if (!CopyIntoWriter(report.preparedProject, output, copied, cancel, error))
                return false;
            snapshot.hash = output.hash.Finish();
        }
        if (!SafeSha256(snapshot.hash)) {
            if (error.empty())
                error = cancel && cancel->load() ? L"工程准备已取消"
                                                 : L"无法计算工程快照的 SHA-256";
            return false;
        }
        return true;
    }
    if (texAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
        error = L"tex 文件夹不能是链接或重解析点：" + texSource.wstring();
        return false;
    }
    std::vector<std::pair<std::string, fs::path>> files{{"source.blend", report.preparedProject}};
    try {
        for (const auto& entry : fs::recursive_directory_iterator(texSource)) {
            if (cancel && cancel->load()) {
                error = L"tex 工程素材收集已取消";
                return false;
            }
            const DWORD attributes = GetFileAttributesW(entry.path().c_str());
            if (attributes == INVALID_FILE_ATTRIBUTES ||
                (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
                error = L"tex 文件夹包含无法安全分发的链接或重解析点：" +
                    entry.path().wstring();
                return false;
            }
            if (!entry.is_regular_file()) continue;
            const std::string logical = "tex/" +
                WideToUtf8(fs::relative(entry.path(), texSource).generic_wstring());
            if (!SafeProjectBundlePath(logical) || files.size() >= kMaxProjectBundleEntries) {
                error = L"tex 文件路径或文件数量超过工程包限制";
                return false;
            }
            files.emplace_back(logical, entry.path());
        }
    } catch (const std::exception& exception) {
        error = L"无法读取 tex 文件夹：" + Utf8ToWide(exception.what());
        return false;
    }
    std::sort(files.begin(), files.end(), [](const auto& left, const auto& right) {
        return left.first < right.first;
    });
    snapshot.payload = jobDirectory / L"project.rmp";
    snapshot.project = jobDirectory / L"project" / L"source.blend";
    snapshot.hash = WriteProjectBundle(snapshot.payload, files, error, cancel, progress);
    if (snapshot.hash.empty()) return false;
    LogInfo("included sibling tex directory in project payload; files=" +
            std::to_string(files.size() - 1));
    return true;
}

json ReadCacheMetadata(const fs::path& dataDir, const std::string& hash) {
    if (!SafeSha256(hash)) return json(json::value_t::discarded);
    const fs::path directory = ProjectCacheDirectory(dataDir, hash);
    if (!fs::is_regular_file(directory / L"source.blend"))
        return json(json::value_t::discarded);
    json value = json::parse(ReadFileText(directory / L"cache-entry.json"), nullptr, false);
    return value.is_object() && value.value("project_hash", "") == hash
        ? value : json(json::value_t::discarded);
}

#ifdef RM_DEV_TOOLS
bool CacheEntryValid(const fs::path& dataDir, const std::string& hash) {
    return ReadCacheMetadata(dataDir, hash).is_object();
}
#endif

bool CacheMetadataMatches(
    const fs::path& dataDir, const std::string& hash, const std::string& version,
    const std::string& engine, bool requiresGpu, bool projectBundle = false) {
    const json value = ReadCacheMetadata(dataDir, hash);
    return value.is_object() && value.value("blender_version", "") == version &&
        value.value("render_engine", "") == engine &&
        value.value("requires_gpu", false) == requiresGpu &&
        value.value("project_bundle", false) == projectBundle;
}

bool WriteCacheMetadata(
    const fs::path& dataDir, const std::string& hash, const std::string& version,
    const std::string& engine, bool requiresGpu, std::wstring& error,
    bool projectBundle = false) {
    return WriteFileAtomic(
        ProjectCacheDirectory(dataDir, hash) / L"cache-entry.json",
        json{{"project_hash", hash}, {"blender_version", version},
             {"render_engine", engine}, {"requires_gpu", requiresGpu},
             {"project_bundle", projectBundle}}.dump(2),
        error);
}

void TouchCacheEntry(const fs::path& dataDir, const std::string& hash) {
    WriteFileText(ProjectCacheDirectory(dataDir, hash) / L"last-used", "used");
}

size_t CleanupExpiredProjectCaches(
    const fs::path& dataDir,
    fs::file_time_type now = fs::file_time_type::clock::now(),
    std::chrono::hours maxAge = std::chrono::hours(24 * 7)) {
    fs::path root = dataDir / L"cache" / L"projects";
    std::error_code ec;
    if (!fs::is_directory(root, ec)) return 0;
    size_t removed = 0;
    for (const auto& entry : fs::directory_iterator(root, ec)) {
        if (ec || !entry.is_directory(ec)) continue;
        std::string hash = WideToUtf8(entry.path().filename().wstring());
        if (!SafeSha256(hash)) continue;
        fs::path marker = entry.path() / L"last-used";
        fs::path source = entry.path() / L"source.blend";
        fs::file_time_type lastUsed = fs::last_write_time(
            fs::exists(marker, ec) ? marker : source, ec);
        if (ec || now - lastUsed <= maxAge) {
            ec.clear();
            continue;
        }
        uintmax_t count = fs::remove_all(entry.path(), ec);
        if (!ec && count) ++removed;
        ec.clear();
    }
    return removed;
}

size_t CleanupExpiredProjectUploads(
    const fs::path& dataDir,
    fs::file_time_type now = fs::file_time_type::clock::now(),
    std::chrono::hours maxAge = std::chrono::hours(24 * 7)) {
    const fs::path root = dataDir / L"cache" / L"uploads";
    std::error_code ec;
    if (!fs::is_directory(root, ec)) return 0;
    size_t removed = 0;
    for (const auto& entry : fs::directory_iterator(root, ec)) {
        if (ec || !entry.is_directory(ec)) continue;
        const std::string hash = WideToUtf8(entry.path().filename().wstring());
        if (!SafeSha256(hash)) continue;
        const fs::path partial = entry.path() / L"source.blend.partial";
        const fs::file_time_type lastWrite = fs::last_write_time(
            fs::exists(partial, ec) ? partial : entry.path(), ec);
        if (ec || now - lastWrite <= maxAge) {
            ec.clear();
            continue;
        }
        const uintmax_t count = fs::remove_all(entry.path(), ec);
        if (!ec && count) ++removed;
        ec.clear();
    }
    return removed;
}

fs::path FrameLogDestination(const fs::path& dataDir, const std::string& jobId, int frame) {
    return dataDir / L"jobs" / Utf8ToWide(jobId) / L"logs" /
        (L"frame-" + std::to_wstring(frame) + L".log");
}

std::optional<fs::path> ResultDestination(
    const fs::path& dataDir, const RenderJob& job, int frame,
    const std::string& logicalPath) {
    if (logicalPath == "primary") {
        auto configured = job.primaryOutputs.find(frame);
        if (configured != job.primaryOutputs.end() && configured->second.is_absolute())
            return configured->second;
        std::wstring frameText = std::to_wstring(frame);
        return dataDir / L"jobs" / Utf8ToWide(job.id) / L"outputs" /
            (L"frame_" + std::wstring(6 - std::min<size_t>(6, frameText.size()), L'0') +
             frameText + Utf8ToWide(job.outputExtension));
    }
    constexpr std::string_view prefix = "compositor/";
    if (!logicalPath.starts_with(prefix)) return std::nullopt;
    std::string remainder = logicalPath.substr(prefix.size());
    size_t slash = remainder.find('/');
    if (slash == std::string::npos) return std::nullopt;
    std::string id = remainder.substr(0, slash);
    std::string relativeText = remainder.substr(slash + 1);
    auto target = std::find_if(job.compositorOutputs.begin(), job.compositorOutputs.end(),
                               [&](const OutputTarget& value) { return value.id == id; });
    fs::path relative = Utf8ToWide(relativeText);
    bool safe = target != job.compositorOutputs.end() && target->directory.is_absolute() &&
        !relativeText.empty() && relativeText.find('\\') == std::string::npos &&
        relative.is_relative() && !relative.has_root_name() && !relative.has_root_directory();
    for (const auto& component : relative)
        safe = safe && component != L".." && component != L".";
    if (!safe) return std::nullopt;
    return target->directory / relative;
}

std::map<std::string, fs::path> PublishedArtifactPaths(
    const fs::path& dataDir, const RenderJob& job, int frame,
    const std::map<std::string, fs::path>& artifacts) {
    std::map<std::string, fs::path> published;
    for (const auto& [logical, source] : artifacts) {
        (void)source;
        if (auto destination = ResultDestination(dataDir, job, frame, logical))
            published[logical] = *destination;
    }
    return published;
}

bool PublishResultArtifacts(
    const fs::path& dataDir, const RenderJob& job, int frame,
    const std::map<std::string, fs::path>& artifacts,
    const std::string& attemptToken, fs::path& primary, std::wstring& error,
    std::atomic<bool>* stopRequested = nullptr) {
    struct Publication {
        fs::path destination;
        fs::path temporary;
        fs::path backup;
        bool hadBackup = false;
        bool published = false;
    };
    error.clear();
    primary.clear();
    if (artifacts.empty() || !artifacts.contains("primary") || !SafeId(attemptToken)) {
        error = L"结果文件清单无效";
        return false;
    }
    std::vector<Publication> publications;
    std::set<std::wstring> destinations;
    std::error_code ec;
    for (const auto& [logical, source] : artifacts) {
        if (stopRequested && stopRequested->load()) {
            error = L"结果发布已取消";
            return false;
        }
        auto destination = ResultDestination(dataDir, job, frame, logical);
        std::wstring destinationKey = destination
            ? fs::absolute(*destination, ec).lexically_normal().wstring() : std::wstring{};
        std::transform(destinationKey.begin(), destinationKey.end(), destinationKey.begin(),
                       [](wchar_t ch) { return static_cast<wchar_t>(std::towlower(ch)); });
        if (!destination || ec || !fs::is_regular_file(source, ec) ||
            !destinations.insert(std::move(destinationKey)).second) {
            error = L"结果文件目标与工程输出设置不匹配";
            return false;
        }
        fs::create_directories(destination->parent_path(), ec);
        if (ec) {
            error = L"无法创建工程输出目录：" + destination->parent_path().wstring();
            return false;
        }
        uint64_t sourceSize = fs::file_size(source, ec);
        if (ec || !HasFileCapacity(destination->parent_path(), sourceSize)) {
            error = L"工程输出盘空间不足：" + destination->parent_path().wstring();
            for (const auto& staged : publications) fs::remove(staged.temporary, ec);
            return false;
        }
        Publication publication;
        publication.destination = *destination;
        publication.temporary = *destination;
        publication.temporary += Utf8ToWide(".rendermesh-" + attemptToken + ".tmp");
        publication.backup = *destination;
        publication.backup += Utf8ToWide(".rendermesh-" + attemptToken + ".bak");
        fs::remove(publication.temporary, ec);
        fs::remove(publication.backup, ec);
        if (!CopyFileCancellable(source, publication.temporary, stopRequested, error)) {
            error = L"无法暂存结果到工程输出目录：" + destination->wstring();
            fs::remove(publication.temporary, ec);
            for (const auto& staged : publications) fs::remove(staged.temporary, ec);
            return false;
        }
        publications.push_back(std::move(publication));
    }
    for (auto& publication : publications) {
        if (fs::exists(publication.destination, ec)) {
            if (!MoveFileExW(publication.destination.c_str(), publication.backup.c_str(),
                             MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
                error = L"无法备份已有输出：" + LastErrorMessage();
                break;
            }
            publication.hadBackup = true;
        }
        if (!MoveFileExW(publication.temporary.c_str(), publication.destination.c_str(),
                         MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            error = L"无法发布渲染结果：" + LastErrorMessage();
            break;
        }
        publication.published = true;
    }
    if (!error.empty()) {
        for (auto it = publications.rbegin(); it != publications.rend(); ++it) {
            if (it->published) fs::remove(it->destination, ec);
            if (it->hadBackup)
                MoveFileExW(it->backup.c_str(), it->destination.c_str(),
                            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
            fs::remove(it->temporary, ec);
        }
        return false;
    }
    for (auto& publication : publications) fs::remove(publication.backup, ec);
    auto primaryDestination = ResultDestination(dataDir, job, frame, "primary");
    if (!primaryDestination) {
        error = L"结果文件包缺少主输出";
        return false;
    }
    primary = *primaryDestination;
    return true;
}

void ConfigureClient(http::Client& client, time_t readTimeoutSeconds,
                     const std::atomic<bool>* stopRequested,
                     const std::atomic<bool>* operationCancelled) {
    client.set_connection_timeout(3);
    // Bounded socket waits keep application shutdown responsive even if a peer
    // accepts a connection and then stalls indefinitely.
    client.set_read_timeout(readTimeoutSeconds);
    client.set_write_timeout(std::max<time_t>(5, std::min<time_t>(readTimeoutSeconds, 60)));
    client.set_cancellation(stopRequested, operationCancelled);
}

http::Result PostFile(
    const std::string& host,
    int port,
    const std::string& route,
    const http::Headers& headers,
    const fs::path& file,
    std::atomic<bool>* stopRequested = nullptr,
    time_t responseTimeoutSeconds = 5,
    std::atomic<bool>* operationCancelled = nullptr) {
    File input = File::OpenRead(file);
    const size_t total = input ? static_cast<size_t>(input.Size()) : 0;
    std::vector<char> buffer;
    auto provider = [&](size_t offset, size_t length, http::DataSink& sink) {
        if (!input || !input.Seek(offset)) return false;
        buffer.resize(std::min<size_t>(length, 1u << 20));
        while (length) {
            size_t read = 0;
            if (!input.Read(buffer.data(), std::min(buffer.size(), length), read) || !read)
                return false;
            if (!sink.write(buffer.data(), read)) return false;
            length -= read;
        }
        return true;
    };
    http::Client client(host, port);
    ConfigureClient(client, responseTimeoutSeconds, stopRequested, operationCancelled);
    return client.Post(route, headers, total, provider, "application/octet-stream");
}

http::Result PostBytes(
    const std::string& host,
    int port,
    const std::string& route,
    const http::Headers& headers,
    const std::string& body,
    std::atomic<bool>* stopRequested = nullptr,
    std::atomic<bool>* operationCancelled = nullptr,
    time_t responseTimeoutSeconds = 60) {
    http::Client client(host, port);
    ConfigureClient(client, responseTimeoutSeconds, stopRequested, operationCancelled);
    return client.Post(route, headers, body, "application/octet-stream");
}

http::Result PostEmpty(
    const std::string& host,
    int port,
    const std::string& route,
    const http::Headers& headers,
    std::atomic<bool>* stopRequested = nullptr,
    std::atomic<bool>* operationCancelled = nullptr,
    time_t responseTimeoutSeconds = 5) {
    return PostBytes(host, port, route, headers, {}, stopRequested, operationCancelled,
                     responseTimeoutSeconds);
}

std::string FrameExtension(const std::string& format) {
    return format == "PNG" ? ".png" : format == "JPEG" ? ".jpg" : ".exr";
}

// Machine-readable names used by the command line and the local control API.
const char* JobStageName(JobStage stage) {
    switch (stage) {
        case JobStage::Rendering: return "rendering";
        case JobStage::Uploading: return "uploading";
        case JobStage::UploadFailed: return "upload_failed";
        case JobStage::ReadyToConfigure: return "ready";
        case JobStage::Testing: return "testing";
        case JobStage::TestCompleted: return "test_completed";
        case JobStage::TestFailed: return "test_failed";
        case JobStage::Completed: return "completed";
        case JobStage::Failed: return "failed";
        case JobStage::Stopped: return "stopped";
    }
    return "unknown";
}

const char* TaskStateName(TaskState state) {
    switch (state) {
        case TaskState::Pending: return "pending";
        case TaskState::Dispatching: return "dispatching";
        case TaskState::Assigned: return "queued";
        case TaskState::Rendering: return "rendering";
        case TaskState::Completed: return "completed";
        case TaskState::Failed: return "failed";
        case TaskState::Stopped: return "stopped";
    }
    return "unknown";
}

const char* TransferStateName(ProjectTransferState state) {
    switch (state) {
        case ProjectTransferState::Pending: return "pending";
        case ProjectTransferState::Checking: return "checking";
        case ProjectTransferState::Uploading: return "uploading";
        case ProjectTransferState::Cached: return "cached";
        case ProjectTransferState::Completed: return "completed";
        case ProjectTransferState::Failed: return "failed";
        case ProjectTransferState::Offline: return "offline";
        case ProjectTransferState::Excluded: return "excluded";
    }
    return "unknown";
}

json JobJson(const RenderJob& job, bool withFrames) {
    int completed = 0, failed = 0, active = 0, stopped = 0;
    for (const auto& frame : job.frames) {
        completed += frame.state == TaskState::Completed;
        failed += frame.state == TaskState::Failed;
        stopped += frame.state == TaskState::Stopped;
        active += frame.state == TaskState::Assigned || frame.state == TaskState::Rendering ||
            frame.state == TaskState::Dispatching;
    }
    json transfers = json::array();
    for (const auto& transfer : job.projectTransfers)
        transfers.push_back({
            {"node_id", transfer.nodeId}, {"node_name", transfer.nodeName},
            {"state", TransferStateName(transfer.state)},
            {"transferred_bytes", transfer.transferredBytes},
            {"total_bytes", transfer.totalBytes}, {"error", transfer.error},
        });
    json value = {
        {"id", job.id}, {"name", job.name}, {"software", job.software},
        {"scene", job.sceneName}, {"stage", JobStageName(job.stage)},
        {"source", WideToUtf8(job.sourceProject.wstring())},
        {"version", job.softwareVersion}, {"render_engine", job.renderEngine},
        {"output_format", job.outputFormat}, {"priority", job.priority},
        {"package_size", job.packageSize}, {"test_frames", job.testFrames},
        {"frame_first", job.frames.empty() ? 0 : job.frames.front().frame},
        {"frame_last", job.frames.empty() ? 0 : job.frames.back().frame},
        {"frames_total", job.frames.size()}, {"frames_completed", completed},
        {"frames_failed", failed}, {"frames_active", active}, {"frames_stopped", stopped},
        {"created_at", UnixMilliseconds(job.createdAt)},
        {"finished_at", UnixMilliseconds(job.finishedAt)},
        {"transfers", std::move(transfers)},
    };
    if (withFrames) {
        json frames = json::array();
        for (const auto& frame : job.frames)
            frames.push_back({
                {"frame", frame.frame}, {"state", TaskStateName(frame.state)},
                {"node", frame.assignedNodeName}, {"error", frame.error},
                {"started_at", UnixMilliseconds(frame.startedAt)},
                {"ended_at", UnixMilliseconds(frame.endedAt)},
                {"output", WideToUtf8(frame.output.wstring())},
            });
        value["frames"] = std::move(frames);
    }
    return value;
}

}  // namespace

std::wstring LocalEndpointSummary(int port) {
    auto addresses = LocalIPv4Addresses();
    if (addresses.empty()) return L"127.0.0.1:" + std::to_wstring(port);
    std::wstring summary;
    for (const auto& address : addresses) {
        if (!summary.empty()) summary += L" / ";
        summary += Utf8ToWide(address) + L":" + std::to_wstring(port);
    }
    return summary;
}

struct NodeApp::Impl {
    explicit Impl(NodeApp* app, std::chrono::milliseconds grace)
        : owner(app), assignmentGrace(grace), hardware(QueryHardwareInfo()) {}

    NodeApp* owner;
    std::chrono::milliseconds assignmentGrace;
    HardwareInfo hardware;
    mutable std::mutex stateMutex;
    std::mutex persistMutex;
    std::unordered_map<std::string, uint64_t> persistedFingerprints;
    std::map<std::string, Peer> peers;
    std::set<std::pair<std::string, std::string>> rejectedJobPeers;
    std::map<std::pair<std::string, std::string>, std::chrono::steady_clock::time_point>
        jobPeerBackoff;
    std::vector<RenderJob> jobs;
    std::atomic<uint64_t> jobRevision{1};
    mutable std::mutex jobStatsMutex;
    mutable uint64_t jobStatsRevision = 0;
    mutable JobAggregate cachedJobStats;
    std::vector<ContributionRecord> contributions;
    struct PendingCleanup {
        std::string operationId;
        std::string nodeId;
        std::string jobId;
        std::string projectHash;
    };
    std::mutex cleanupMutex;
    std::mutex cleanupPersistMutex;
    std::vector<PendingCleanup> pendingCleanups;
    std::mutex callbackMutex;
    std::function<void()> changed;
    mutable std::mutex manualStatusMutex;
    std::wstring manualStatus;
    std::mutex workMutex;
    std::shared_mutex workAdmissionMutex;
    std::map<std::string, std::chrono::steady_clock::time_point> cancelledPackages;
    std::map<std::string, std::shared_ptr<std::atomic<bool>>> packageAbortSignals;
    std::mutex cancellationMutex;
    std::mutex jobStageGateMutex;
    std::mutex cacheMutex;
    std::mutex cacheOperationMapMutex;
    std::unordered_map<std::string, std::shared_ptr<std::mutex>> cacheOperationMutexes;
    // Leading bytes of each partial upload verified chunk-by-chunk by this process.
    std::map<std::string, uint64_t> verifiedUploadBytes;
    std::mutex pairingMutex;
    std::map<std::string, std::deque<std::chrono::steady_clock::time_point>> pairingAttempts;
    std::condition_variable workCv;
    std::queue<WorkItem> work;
    std::mutex currentTaskMutex;
    std::string currentTaskId;
    std::string currentJobId;
    bool currentPackageHasStartedFrame = false;
    std::optional<int> currentRenderingFrame;
    WorkerActivity currentActivity;
    std::atomic<int> workerAssigned{0};
    std::atomic<bool> stopRequested{false};
    std::map<std::string, std::shared_ptr<std::atomic<bool>>> jobCancellations;
    http::Server server;
    std::atomic<SOCKET> discoverySocket{INVALID_SOCKET};
    std::thread serverThread;
    std::thread discoveryThread;
    std::thread manualPeerThread;
    std::thread peerExchangeThread;
    std::thread schedulerThread;
    std::array<std::thread, 2> projectSyncThreads;
    std::thread workerThread;
    std::thread cacheThread;

    // Executable discovery scans PATH and Program Files. /health and every
    // discovery beacon need the answer, so it is cached for a few seconds.
    std::mutex rendererCacheMutex;
    fs::path rendererCacheProfile;
    std::chrono::steady_clock::time_point rendererCheckedAt{};
    bool rendererCacheReady = false;

    bool RendererReady(const Config& config) {
        if (owner->mockRenderer_) return true;
        std::scoped_lock lock(rendererCacheMutex);
        const auto now = std::chrono::steady_clock::now();
        if (config.blenderPath != rendererCacheProfile ||
            now - rendererCheckedAt > std::chrono::seconds(5)) {
            rendererCacheReady = !BlenderRenderAdapter().LocateExecutable(config).empty();
            rendererCacheProfile = config.blenderPath;
            rendererCheckedAt = now;
        }
        return rendererCacheReady;
    }

    void Notify() {
        std::function<void()> callback;
        {
            std::scoped_lock lock(callbackMutex);
            callback = changed;
        }
        if (callback) callback();
    }

    fs::path ContributionsPath() const { return owner->DataDir() / L"contributions.json"; }
    fs::path PendingCleanupsPath() const {
        return owner->DataDir() / L"cleanup-pending.json";
    }

    void PersistPendingCleanups() {
        std::scoped_lock persistLock(cleanupPersistMutex);
        json rows = json::array();
        {
            std::scoped_lock lock(cleanupMutex);
            for (const auto& request : pendingCleanups) {
                rows.push_back({{"operation_id", request.operationId},
                                {"node_id", request.nodeId},
                                {"job_id", request.jobId},
                                {"project_hash", request.projectHash}});
            }
        }
        std::wstring error;
        if (!WriteDurableFile(PendingCleanupsPath(), rows.dump(2), error))
            LogError("cleanup queue was not persisted; " + WideToUtf8(error));
    }

    void LoadPendingCleanups() {
        if (!fs::exists(PendingCleanupsPath())) return;
        try {
            const json rows = json::parse(ReadFileText(PendingCleanupsPath()));
            std::scoped_lock lock(cleanupMutex);
            for (const auto& row : rows) {
                PendingCleanup request{
                    row.value("operation_id", ""), row.value("node_id", ""),
                    row.value("job_id", ""), row.value("project_hash", "")};
                if (SafeId(request.operationId) && SafeId(request.nodeId) &&
                    SafeId(request.jobId) && SafeSha256(request.projectHash))
                    pendingCleanups.push_back(std::move(request));
            }
        } catch (...) {
        }
    }

    void QueueRemoteCleanup(const RenderJob& job) {
        if (!SafeId(job.id) || !SafeSha256(job.projectHash)) return;
        std::set<std::string> nodes;
        for (const auto& nodeId : job.remoteNodeIds)
            if (SafeId(nodeId) && nodeId != owner->GetConfig().nodeId)
                nodes.insert(nodeId);
        for (const auto& frame : job.frames)
            if (SafeId(frame.assignedNodeId) && frame.assignedNodeId != owner->GetConfig().nodeId)
                nodes.insert(frame.assignedNodeId);
        bool changed = false;
        {
            std::scoped_lock lock(cleanupMutex);
            for (const auto& nodeId : nodes) {
                const bool exists = std::any_of(
                    pendingCleanups.begin(), pendingCleanups.end(),
                    [&](const PendingCleanup& request) {
                        return request.nodeId == nodeId && request.jobId == job.id;
                    });
                if (!exists) {
                    pendingCleanups.push_back(
                        {NewRandomHex(12), nodeId, job.id, job.projectHash});
                    changed = true;
                }
            }
        }
        if (changed) PersistPendingCleanups();
    }

    void ProcessPendingCleanups() {
        std::vector<PendingCleanup> snapshot;
        {
            std::scoped_lock lock(cleanupMutex);
            snapshot = pendingCleanups;
        }
        if (snapshot.empty()) return;
        Config config = owner->GetConfig();
        std::set<std::string> completedOperations;
        for (const auto& request : snapshot) {
            std::optional<Peer> target;
            {
                std::scoped_lock lock(stateMutex);
                auto found = peers.find(request.nodeId);
                if (found != peers.end() && found->second.supportsJobCleanup &&
                    std::chrono::steady_clock::now() - found->second.lastSeen <= kPeerTimeout)
                    target = found->second;
            }
            if (!target) continue;
            http::Headers headers = {
                {"X-RM-Token", config.clusterToken}, {"X-RM-Job", request.jobId},
                {"X-RM-Project", request.projectHash},
                {"X-RM-Operation", request.operationId},
            };
            auto response = PostEmpty(target->address, target->httpPort,
                                      "/v4/job-cleanup", headers, &stopRequested);
            if (response && response->status == 200)
                completedOperations.insert(request.operationId);
        }
        if (completedOperations.empty()) return;
        {
            std::scoped_lock lock(cleanupMutex);
            std::erase_if(pendingCleanups, [&](const PendingCleanup& request) {
                return completedOperations.contains(request.operationId);
            });
        }
        PersistPendingCleanups();
    }

    std::shared_ptr<std::atomic<bool>> JobCancellation(const std::string& jobId) {
        std::scoped_lock lock(cancellationMutex);
        auto& signal = jobCancellations[jobId];
        if (!signal) signal = std::make_shared<std::atomic<bool>>(false);
        return signal;
    }

    void BeginNewJobExecution(const std::string& jobId) {
        std::scoped_lock lock(cancellationMutex);
        // Never clear the old signal in place. In-flight work from the stopped
        // execution must keep observing true, while newly scheduled packages
        // receive a fresh false signal.
        jobCancellations[jobId] =
            std::make_shared<std::atomic<bool>>(false);
    }

    bool PackageCancelled(const std::string& packageId) {
        std::scoped_lock lock(workMutex);
        auto now = std::chrono::steady_clock::now();
        for (auto it = cancelledPackages.begin(); it != cancelledPackages.end();) {
            if (now - it->second > std::chrono::hours(1)) {
                packageAbortSignals.erase(it->first);
                it = cancelledPackages.erase(it);
            }
            else ++it;
        }
        return cancelledPackages.contains(packageId);
    }

    std::shared_ptr<std::atomic<bool>> PackageAbortSignal(const std::string& packageId) {
        std::scoped_lock lock(workMutex);
        auto& signal = packageAbortSignals[packageId];
        if (!signal) signal = std::make_shared<std::atomic<bool>>(false);
        return signal;
    }

    void CancelPackage(const std::string& packageId, bool abortBeforeFirstFrame) {
        std::scoped_lock lock(workMutex);
        if (cancelledPackages.size() >= 4096) {
            const auto oldest = std::min_element(
                cancelledPackages.begin(), cancelledPackages.end(),
                [](const auto& left, const auto& right) {
                    return left.second < right.second;
                });
            const std::string oldestPackageId = oldest->first;
            cancelledPackages.erase(oldest);
            packageAbortSignals.erase(oldestPackageId);
        }
        cancelledPackages[packageId] = std::chrono::steady_clock::now();
        auto& signal = packageAbortSignals[packageId];
        if (!signal) signal = std::make_shared<std::atomic<bool>>(false);
        if (abortBeforeFirstFrame) signal->store(true);
    }

    void ForgetCancelledPackage(const std::string& packageId) {
        std::scoped_lock lock(workMutex);
        cancelledPackages.erase(packageId);
        packageAbortSignals.erase(packageId);
    }

    std::string CurrentTaskId() {
        std::scoped_lock lock(currentTaskMutex);
        return currentTaskId;
    }

    bool IsJobRendering(const std::string& jobId) {
        std::scoped_lock lock(currentTaskMutex);
        return currentJobId == jobId;
    }

    WorkerActivity CurrentActivity() {
        std::scoped_lock lock(currentTaskMutex);
        return currentActivity;
    }

    std::shared_ptr<std::mutex> CacheOperationMutex(const std::string& hash) {
        std::scoped_lock lock(cacheOperationMapMutex);
        auto& operation = cacheOperationMutexes[hash];
        if (!operation) operation = std::make_shared<std::mutex>();
        return operation;
    }

    void SetManualStatus(std::wstring value) {
        std::scoped_lock lock(manualStatusMutex);
        manualStatus = std::move(value);
    }

    void RefreshExecutionStageLocked(RenderJob& job) {
        if (job.stage == JobStage::Testing) {
            bool allComplete = !job.testFrames.empty();
            bool anyFailed = false;
            for (int testFrame : job.testFrames) {
                auto frame = std::find_if(job.frames.begin(), job.frames.end(),
                    [&](const FrameTask& value) { return value.frame == testFrame; });
                if (frame == job.frames.end()) {
                    anyFailed = true;
                    allComplete = false;
                    continue;
                }
                allComplete = allComplete && frame->state == TaskState::Completed;
                anyFailed = anyFailed || frame->state == TaskState::Failed;
            }
            if (anyFailed) job.stage = JobStage::TestFailed;
            else if (allComplete) job.stage = JobStage::TestCompleted;
            return;
        }
        if (job.stage != JobStage::Rendering) return;
        if (job.frames.empty()) return;
        bool allTerminal = true;
        bool anyFailed = false;
        bool anyStopped = false;
        for (const auto& frame : job.frames) {
            allTerminal = allTerminal && (frame.state == TaskState::Completed ||
                frame.state == TaskState::Failed || frame.state == TaskState::Stopped);
            anyFailed = anyFailed || frame.state == TaskState::Failed;
            anyStopped = anyStopped || frame.state == TaskState::Stopped;
        }
        if (allTerminal)
            job.stage = anyFailed ? JobStage::Failed
                : anyStopped ? JobStage::Stopped : JobStage::Completed;
    }

    void MaintainJobHistory() {
        const auto now = std::chrono::system_clock::now();
        std::vector<RenderJob> purge;
        bool changed = false;
        {
            std::scoped_lock lock(stateMutex);
            for (auto& job : jobs) {
                RefreshExecutionStageLocked(job);
                const bool terminal = !job.frames.empty() && std::all_of(
                    job.frames.begin(), job.frames.end(), [](const FrameTask& frame) {
                        return frame.state == TaskState::Completed ||
                            frame.state == TaskState::Failed ||
                            frame.state == TaskState::Stopped;
                    });
                if (!terminal) {
                    if (job.finishedAt.time_since_epoch() !=
                        std::chrono::system_clock::duration::zero()) {
                        job.finishedAt = {};
                        changed = true;
                    }
                    continue;
                }
                if (job.finishedAt.time_since_epoch() ==
                    std::chrono::system_clock::duration::zero()) {
                    for (const auto& frame : job.frames)
                        job.finishedAt = std::max(job.finishedAt, frame.endedAt);
                    if (job.finishedAt.time_since_epoch() ==
                        std::chrono::system_clock::duration::zero())
                        job.finishedAt = now;
                    changed = true;
                }
                if (job.artifactsPurgedAt.time_since_epoch() ==
                        std::chrono::system_clock::duration::zero() &&
                    ui::IsExpiredJob(
                        {Utf8ToWide(job.id), Utf8ToWide(job.name), job.priority,
                         job.createdAt, job.finishedAt}, now))
                    purge.push_back(job);
            }
        }
        for (const auto& job : purge) {
            std::error_code removeError;
            const auto managedDirectory = ManagedJobDirectory(
                owner->ProjectStorageDir(), job.id);
            if (!managedDirectory) {
                LogError("expired job had an unsafe managed path; job=" + job.id);
                continue;
            }
            fs::remove_all(*managedDirectory, removeError);
            if (removeError) {
                LogWarning("expired job artifacts could not be removed; job=" + job.id);
                continue;
            }
            QueueRemoteCleanup(job);
            std::scoped_lock lock(stateMutex);
            auto found = std::find_if(jobs.begin(), jobs.end(), [&](const RenderJob& value) {
                return value.id == job.id;
            });
            if (found != jobs.end() && found->artifactsPurgedAt.time_since_epoch() ==
                    std::chrono::system_clock::duration::zero()) {
                found->artifactsPurgedAt = now;
                changed = true;
            }
        }
        if (changed) {
            PersistJobs();
            Notify();
        }
        ProcessPendingCleanups();
        std::error_code directoryError;
        const fs::path jobsRoot = owner->ProjectStorageDir() / L"jobs";
        if (fs::is_directory(jobsRoot, directoryError)) {
            for (const auto& entry : fs::directory_iterator(jobsRoot, directoryError)) {
                const std::wstring name = entry.path().filename().wstring();
                if (!entry.is_directory(directoryError) || !name.starts_with(L".deleting-"))
                    continue;
                std::error_code removeError;
                fs::remove_all(entry.path(), removeError);
                if (removeError) LogWarning("pending local job cleanup remains busy");
            }
        }
    }

    void CacheMaintenanceLoop() {
        while (!stopRequested.load()) {
            MaintainJobHistory();
            if (workerAssigned.load() == 0) {
                std::scoped_lock lock(cacheMutex);
                size_t removed = CleanupExpiredProjectCaches(
                    owner->ProjectStorageDir());
                removed += CleanupExpiredProjectUploads(
                    owner->ProjectStorageDir());
                if (removed)
                    LogInfo("expired project caches removed=" + std::to_string(removed));
            }
            for (int step = 0; step < 600 && !stopRequested.load(); ++step) Sleep(100);
        }
    }

    json SerializeJob(const RenderJob& job) const {
                const fs::path storageRoot = owner->ProjectStorageDir();
                json frames = json::array();
                json primaryOutputs = json::object();
                json compositorOutputs = json::array();
                for (const auto& [frame, path] : job.primaryOutputs)
                    primaryOutputs[std::to_string(frame)] =
                        StoredPathText(path, storageRoot);
                for (const auto& target : job.compositorOutputs) {
                    json metadata = OutputTargetMetadata(target);
                    metadata["directory"] =
                        StoredPathText(target.directory, storageRoot);
                    compositorOutputs.push_back(std::move(metadata));
                }
                json transfers = json::array();
                for (const auto& transfer : job.projectTransfers) {
                    transfers.push_back({
                        {"node_id", transfer.nodeId}, {"node_name", transfer.nodeName},
                        {"address", transfer.address}, {"port", transfer.httpPort},
                        {"cpu_model", transfer.cpuModel}, {"gpu_models", transfer.gpuModels},
                        {"state", static_cast<int>(transfer.state)},
                        {"transferred_bytes", transfer.transferredBytes},
                        {"total_bytes", transfer.totalBytes},
                        {"bytes_per_second", transfer.bytesPerSecond},
                        {"attempts", transfer.attempts},
                        {"error", transfer.error},
                    });
                }
                for (const auto& frame : job.frames) {
                    frames.push_back({
                        {"task_id", frame.taskId},
                        {"frame", frame.frame},
                        {"state", static_cast<int>(frame.state)},
                        {"assigned_node_id", frame.assignedNodeId},
                        {"assigned_node_name", frame.assignedNodeName},
                        {"assigned_package_id", frame.assignedPackageId},
                        {"dispatch_attempts", frame.dispatchAttempts},
                        {"attempt_token", frame.attemptToken},
                        {"output", StoredPathText(frame.output, storageRoot)},
                        {"log", StoredPathText(frame.log, storageRoot)},
                        {"artifacts", [&] {
                            json values = json::object();
                            for (const auto& [logical, path] : frame.artifacts)
                                values[logical] = StoredPathText(path, storageRoot);
                            return values;
                        }()},
                        {"error", frame.error},
                        {"started_at", UnixMilliseconds(frame.startedAt)},
                        {"ended_at", UnixMilliseconds(frame.endedAt)},
                    });
                }
                return {
                    {"id", job.id}, {"name", job.name}, {"scene_name", job.sceneName},
                    {"source_blend", WideToUtf8(job.sourceProject.wstring())},
                    {"blend", StoredPathText(job.preparedProject, storageRoot)},
                    {"project_payload", StoredPathText(ProjectPayloadPath(job), storageRoot)},
                    {"project_bundle", job.projectBundle},
                    {"project_hash", job.projectHash},
                    {"format", job.outputFormat}, {"extension", job.outputExtension},
                    {"version", job.softwareVersion},
                    {"render_engine", job.renderEngine},
                    {"render_software_profile_id", job.renderSoftwareProfileId},
                    {"render_software_profile_name", job.renderSoftwareProfileName},
                    {"blender_executable", WideToUtf8(job.softwareExecutable.wstring())},
                    {"requires_gpu", job.requiresGpu},
                    {"primary_outputs", primaryOutputs},
                    {"compositor_outputs", compositorOutputs},
                    {"remote_node_ids", job.remoteNodeIds},
                    {"stage", static_cast<int>(job.stage)},
                    {"package_size", job.packageSize},
                    {"test_frames", job.testFrames},
                    {"project_transfers", transfers},
                    {"frames", frames},
                    {"created_at", UnixMilliseconds(job.createdAt)},
                    {"finished_at", UnixMilliseconds(job.finishedAt)},
                    {"artifacts_purged_at", UnixMilliseconds(job.artifactsPurgedAt)},
                    {"priority", job.priority},
                };
    }

    std::optional<RenderJob> DeserializeJob(const json& row) const {
        try {
                const fs::path storageRoot = owner->ProjectStorageDir();
                RenderJob job;
                job.id = row.value("id", "");
                job.name = row.value("name", "");
                job.sceneName = row.value("scene_name", job.name);
                job.sourceProject = Utf8ToWide(row.value("source_blend", ""));
                job.preparedProject = LoadStoredPath(
                    row.value("blend", ""), storageRoot);
                job.projectPayload = LoadStoredPath(
                    row.value("project_payload", ""), storageRoot);
                job.projectBundle = row.value("project_bundle", false);
                if (job.projectPayload.empty()) job.projectPayload = job.preparedProject;
                job.projectHash = row.value("project_hash", "");
                if (!SafeSha256(job.projectHash) &&
                    fs::is_regular_file(ProjectPayloadPath(job)))
                    job.projectHash = Sha256File(ProjectPayloadPath(job));
                job.outputFormat = row.value("format", "PNG");
                job.outputExtension = row.value("extension", FrameExtension(job.outputFormat));
                job.softwareVersion = row.value("version", "");
                job.renderEngine = row.value("render_engine", "");
                job.renderSoftwareProfileId =
                    row.value("render_software_profile_id", "");
                job.renderSoftwareProfileName =
                    row.value("render_software_profile_name", "");
                job.softwareExecutable = Utf8ToWide(
                    row.value("blender_executable", ""));
                job.requiresGpu = row.value("requires_gpu", false);
                json primaryOutputRows = row.value("primary_outputs", json::object());
                for (const auto& [frameText, pathValue] : primaryOutputRows.items()) {
                    try {
                        job.primaryOutputs[std::stoi(frameText)] =
                            LoadStoredPath(pathValue.get<std::string>(), storageRoot);
                    } catch (...) {
                    }
                }
                for (const auto& value : row.value("compositor_outputs", json::array())) {
                    OutputTarget target;
                    if (!ParseOutputTargetMetadata(value, target)) continue;
                    target.directory = LoadStoredPath(
                        value.value("directory", ""), storageRoot);
                    if (target.directory.is_absolute())
                        job.compositorOutputs.push_back(std::move(target));
                }
                for (const auto& nodeId : row.value(
                         "remote_node_ids", std::vector<std::string>{})) {
                    if (SafeId(nodeId) && std::find(job.remoteNodeIds.begin(),
                            job.remoteNodeIds.end(), nodeId) == job.remoteNodeIds.end())
                        job.remoteNodeIds.push_back(nodeId);
                }
                int stage = row.value("stage", static_cast<int>(JobStage::Rendering));
                job.stage = stage >= static_cast<int>(JobStage::Rendering) &&
                        stage <= static_cast<int>(JobStage::Stopped)
                    ? static_cast<JobStage>(stage) : JobStage::Rendering;
                job.packageSize = std::clamp(
                    row.value("package_size", size_t{5}), size_t{1}, size_t{20});
                for (int frame : row.value("test_frames", std::vector<int>{})) {
                    if (frame >= kBlenderFrameMin && frame <= kBlenderFrameMax &&
                        std::find(job.testFrames.begin(), job.testFrames.end(), frame) ==
                            job.testFrames.end())
                        job.testFrames.push_back(frame);
                }
                for (const auto& value : row.value("project_transfers", json::array())) {
                    ProjectTransfer transfer;
                    transfer.nodeId = value.value("node_id", "");
                    transfer.nodeName = value.value("node_name", "");
                    transfer.address = value.value("address", "");
                    transfer.httpPort = value.value("port", 8391);
                    transfer.cpuModel = value.value("cpu_model", "");
                    transfer.gpuModels = value.value(
                        "gpu_models", std::vector<std::string>{});
                    int transferState = value.value(
                        "state", static_cast<int>(ProjectTransferState::Pending));
                    transfer.state = transferState >= static_cast<int>(ProjectTransferState::Pending) &&
                            transferState <= static_cast<int>(ProjectTransferState::Excluded)
                        ? static_cast<ProjectTransferState>(transferState)
                        : ProjectTransferState::Pending;
                    if (transfer.state == ProjectTransferState::Checking ||
                        transfer.state == ProjectTransferState::Uploading)
                        transfer.state = ProjectTransferState::Pending;
                    transfer.transferredBytes = value.value("transferred_bytes", uint64_t{0});
                    transfer.totalBytes = value.value("total_bytes", uint64_t{0});
                    transfer.bytesPerSecond = 0;
                    transfer.attempts = std::clamp(value.value("attempts", 0), 0, 1000);
                    transfer.error = value.value("error", "");
                    if (SafeId(transfer.nodeId) && !transfer.address.empty())
                        job.projectTransfers.push_back(std::move(transfer));
                }
                for (const auto& value : row.value("frames", json::array())) {
                    FrameTask frame;
                    frame.taskId = value.value("task_id", "");
                    frame.frame = value.value("frame", 1);
                    int state = value.value("state", 0);
                    frame.state = state >= static_cast<int>(TaskState::Pending) &&
                            state <= static_cast<int>(TaskState::Stopped)
                        ? static_cast<TaskState>(state) : TaskState::Pending;
                    if (frame.state == TaskState::Assigned || frame.state == TaskState::Dispatching ||
                        frame.state == TaskState::Rendering)
                        frame.state = TaskState::Pending;
                    frame.assignedNodeId = value.value("assigned_node_id", "");
                    frame.assignedNodeName = value.value("assigned_node_name", "");
                    frame.assignedPackageId = value.value("assigned_package_id", "");
                    frame.dispatchAttempts = value.value("dispatch_attempts", 0);
                    frame.attemptToken = value.value("attempt_token", "");
                    frame.output = LoadStoredPath(
                        value.value("output", ""), storageRoot);
                    frame.log = LoadStoredPath(
                        value.value("log", ""), storageRoot);
                    // Keep the object alive for the whole loop: items() only
                    // references it, and a temporary would dangle.
                    const json artifactRows = value.value("artifacts", json::object());
                    for (const auto& [logical, path] : artifactRows.items()) {
                        if (logical == "primary" || logical.starts_with("compositor/"))
                            frame.artifacts[logical] =
                                LoadStoredPath(path.get<std::string>(), storageRoot);
                    }
                    frame.error = value.value("error", "");
                    frame.startedAt = TimeFromUnixMilliseconds(value.value("started_at", int64_t{0}));
                    frame.endedAt = TimeFromUnixMilliseconds(value.value("ended_at", int64_t{0}));
                    job.frames.push_back(std::move(frame));
                }
                job.createdAt = TimeFromUnixMilliseconds(row.value("created_at", int64_t{0}));
                job.finishedAt = TimeFromUnixMilliseconds(row.value("finished_at", int64_t{0}));
                job.artifactsPurgedAt = TimeFromUnixMilliseconds(
                    row.value("artifacts_purged_at", int64_t{0}));
                job.priority = std::clamp(row.value("priority", 0), 0, 100);
                if (!SafeId(job.id)) return std::nullopt;
                return job;
        } catch (...) {
            return std::nullopt;
        }
    }

    // Each job is one JSON record: <data>\jobdb\<id>.json, replaced atomically.
    // A record is rewritten only when its serialised form changes.
    fs::path JobRecordsDir() const { return owner->DataDir() / L"jobdb"; }
    fs::path JobRecordPath(const std::string& id) const {
        return JobRecordsDir() / Utf8ToWide(id + ".json");
    }

    static uint64_t RecordFingerprint(std::string_view text) {
        uint64_t value = 1469598103934665603ull;  // FNV-1a
        for (unsigned char ch : text) value = (value ^ ch) * 1099511628211ull;
        return value ^ text.size();
    }

    // Requires persistMutex.
    bool WriteJobRecord(const std::string& id, const std::string& text,
                        std::wstring* failure) {
        const uint64_t fingerprint = RecordFingerprint(text);
        auto stored = persistedFingerprints.find(id);
        if (stored != persistedFingerprints.end() && stored->second == fingerprint)
            return true;
        std::wstring error;
        if (!WriteFileAtomic(JobRecordPath(id), text, error, true)) {
            LogError("job record was not persisted; job=" + id + "; " + WideToUtf8(error));
            if (failure) *failure = L"无法保存任务状态：" + error;
            return false;
        }
        persistedFingerprints[id] = fingerprint;
        return true;
    }

    bool PersistJobs(const std::string& onlyJobId = {}, std::wstring* failure = nullptr) {
        std::scoped_lock persistLock(persistMutex);
        std::vector<std::pair<std::string, std::string>> records;
        {
            std::scoped_lock lock(stateMutex);
            for (const auto& job : jobs)
                if (onlyJobId.empty() || job.id == onlyJobId)
                    records.emplace_back(job.id, SerializeJob(job).dump());
        }
        bool ok = true;
        for (const auto& [id, text] : records)
            ok = WriteJobRecord(id, text, failure) && ok;
        ++jobRevision;
        return ok;
    }

    bool PersistDetachedJob(const RenderJob& job, std::wstring* failure = nullptr) {
        std::scoped_lock persistLock(persistMutex);
        const bool ok = WriteJobRecord(job.id, SerializeJob(job).dump(), failure);
        ++jobRevision;
        return ok;
    }

    bool DeletePersistedJob(const std::string& jobId, std::wstring& error) {
        std::scoped_lock persistLock(persistMutex);
        if (!DeleteFileW(JobRecordPath(jobId).c_str()) &&
            GetLastError() != ERROR_FILE_NOT_FOUND) {
            error = L"无法删除任务记录，请稍后重试：" + LastErrorMessage();
            return false;
        }
        persistedFingerprints.erase(jobId);
        ++jobRevision;
        return true;
    }

    // Converts the history of earlier releases (jobs.db, or the even older
    // jobs.json) into job records once. The records are staged in a separate
    // folder and published with one rename, so an interrupted import is redone.
    void ImportLegacyJobs() {
        const fs::path dataDir = owner->DataDir();
        std::vector<std::string> payloads;
        std::string error;
        fs::path imported;
        if (fs::exists(dataDir / L"jobs.db")) {
            imported = dataDir / L"jobs.db";
            if (!ReadLegacyJobPayloads(imported, payloads, error)) {
                LogError("legacy jobs.db could not be imported: " + error);
                return;
            }
        } else if (fs::exists(dataDir / L"jobs.json")) {
            imported = dataDir / L"jobs.json";
            const json rows = json::parse(ReadFileText(imported), nullptr, false);
            if (rows.is_array())
                for (const auto& row : rows) payloads.push_back(row.dump());
        }
        const fs::path staging = dataDir / L"jobdb.importing";
        std::error_code ignored;
        fs::remove_all(staging, ignored);
        fs::create_directories(staging, ignored);
        size_t count = 0;
        for (const auto& payload : payloads) {
            const json row = json::parse(payload, nullptr, false);
            const std::string id = row.is_object() ? row.value("id", "") : std::string{};
            if (!SafeId(id)) continue;
            std::wstring writeError;
            if (!WriteFileAtomic(staging / Utf8ToWide(id + ".json"), payload, writeError, true)) {
                LogError("legacy job import failed: " + WideToUtf8(writeError));
                return;
            }
            ++count;
        }
        if (!MoveFileExW(staging.c_str(), JobRecordsDir().c_str(), MOVEFILE_WRITE_THROUGH)) {
            LogError("job records could not be published: " + WideToUtf8(LastErrorMessage()));
            return;
        }
        if (!imported.empty()) {
            for (const wchar_t* suffix : {L"", L"-wal", L"-shm"}) {
                fs::path legacy = imported;
                legacy += suffix;
                fs::path kept = legacy;
                kept += L".imported";
                MoveFileExW(legacy.c_str(), kept.c_str(), MOVEFILE_REPLACE_EXISTING);
            }
            LogInfo("legacy job history imported; jobs=" + std::to_string(count));
        }
    }

    void LoadJobs() {
        std::error_code ec;
        if (!fs::is_directory(JobRecordsDir(), ec)) ImportLegacyJobs();
        std::vector<RenderJob> loaded;
        std::scoped_lock persistLock(persistMutex);
        for (const auto& entry : fs::directory_iterator(JobRecordsDir(), ec)) {
            if (entry.path().extension() != L".json") continue;
            const std::string text = ReadFileText(entry.path());
            const json row = json::parse(text, nullptr, false);
            if (row.is_discarded()) {
                LogWarning("unreadable job record skipped: " + WideToUtf8(entry.path().wstring()));
                continue;
            }
            if (auto job = DeserializeJob(row)) {
                persistedFingerprints[job->id] = RecordFingerprint(text);
                loaded.push_back(std::move(*job));
            }
        }
        std::sort(loaded.begin(), loaded.end(), [](const RenderJob& left, const RenderJob& right) {
            return left.createdAt != right.createdAt ? left.createdAt < right.createdAt
                                                     : left.id < right.id;
        });
        std::scoped_lock lock(stateMutex);
        jobs = std::move(loaded);
    }

    bool PersistContributions(std::wstring* failure = nullptr) {
        std::scoped_lock persistLock(persistMutex);
        json rows = json::array();
        {
            std::scoped_lock lock(stateMutex);
            for (const auto& record : contributions) {
                rows.push_back({
                    {"package_id", record.packageId}, {"job_id", record.jobId},
                    {"job_name", record.jobName}, {"origin_name", record.originName},
                    {"origin_address", record.originAddress}, {"frames", record.frames},
                    {"completed_frames", record.completedFrames}, {"state", record.state},
                    {"error", record.error},
                    {"started_at", UnixMilliseconds(record.startedAt)},
                    {"ended_at", UnixMilliseconds(record.endedAt)},
                });
            }
        }
        std::wstring error;
        if (!WriteDurableFile(ContributionsPath(), rows.dump(2), error)) {
            LogError("contribution history was not replaced; " + WideToUtf8(error));
            if (failure) *failure = error;
            return false;
        }
        return true;
    }

    void LoadContributions() {
        if (!fs::exists(ContributionsPath())) return;
        try {
            json rows = json::parse(ReadFileText(ContributionsPath()));
            if (!rows.is_array()) return;
            std::vector<ContributionRecord> loaded;
            size_t first = rows.size() > 5000 ? rows.size() - 5000 : 0;
            for (size_t index = first; index < rows.size(); ++index) {
                const auto& row = rows[index];
                ContributionRecord record;
                record.packageId = row.value("package_id", "");
                record.jobId = row.value("job_id", "");
                record.jobName = row.value("job_name", "");
                record.originName = row.value("origin_name", "");
                record.originAddress = row.value("origin_address", "");
                record.frames = row.value("frames", std::vector<int>{});
                record.completedFrames = row.value("completed_frames", std::vector<int>{});
                record.state = row.value("state", "");
                record.error = row.value("error", "");
                record.startedAt = TimeFromUnixMilliseconds(row.value("started_at", int64_t{0}));
                record.endedAt = TimeFromUnixMilliseconds(row.value("ended_at", int64_t{0}));
                if (SafeId(record.packageId) && SafeId(record.jobId) &&
                    !record.frames.empty() && record.frames.size() <= 5)
                    loaded.push_back(std::move(record));
            }
            std::scoped_lock lock(stateMutex);
            contributions = std::move(loaded);
        } catch (...) {
        }
    }

    bool AllowPairingAttempt(const std::string& address) {
        std::scoped_lock lock(pairingMutex);
        auto now = std::chrono::steady_clock::now();
        auto& attempts = pairingAttempts[address];
        while (!attempts.empty() && now - attempts.front() > std::chrono::minutes(1))
            attempts.pop_front();
        if (attempts.size() >= 6) return false;
        attempts.push_back(now);
        return true;
    }

    bool TryPair(const std::string& address, int port, const std::string& serverNodeId,
                 std::wstring& result) {
        Config pending = owner->GetConfig();
        if (!pending.pairingPending || !IsInviteCode(pending.inviteCode) ||
            !SafeId(serverNodeId))
            return false;
        std::string nonce = NewRandomHex(16);
        std::string requestProof = HmacSha256(
            pending.inviteCode,
            "pair-request|" + pending.nodeId + "|" + serverNodeId + "|" + nonce);
        http::Client client(address, port);
        client.set_connection_timeout(2, 0);
        client.set_read_timeout(2, 0);
        client.set_write_timeout(2, 0);
        http::Headers headers = {
            {"X-RM-Node", pending.nodeId}, {"X-RM-Nonce", nonce},
            {"X-RM-Proof", requestProof},
        };
        auto response = client.Post("/v2/pair", headers, "", "application/octet-stream");
        if (!response || response->status != 200) {
            result = L"找到邀请码相同的电脑，但配对请求被拒绝";
            return false;
        }
        json body = json::parse(response->body, nullptr, false);
        std::string credential = body.value("cluster_token", "");
        std::string responseNode = body.value("node_id", "");
        std::string expected = HmacSha256(
            pending.inviteCode,
            "pair-response|" + serverNodeId + "|" + pending.nodeId + "|" + nonce + "|" +
                credential);
        if (body.is_discarded() || responseNode != serverNodeId || credential.size() < 16 ||
            !ConstantTimeEqual(body.value("proof", ""), expected)) {
            result = L"配对响应无效";
            return false;
        }
        Config current = owner->GetConfig();
        if (!current.pairingPending || current.inviteCode != pending.inviteCode) {
            result = L"邀请码已在配对期间发生变化";
            return false;
        }
        current.clusterToken = std::move(credential);
        current.pairingPending = false;
        current.manualPeerEndpoint = address + ":" + std::to_string(port);
        std::wstring saveError;
        if (!owner->UpdateConfig(std::move(current), saveError)) {
            result = L"配对成功，但无法保存内部凭证：" + saveError;
            return false;
        }
        result = L"邀请码配对成功";
        LogInfo("node pairing completed with " + serverNodeId);
        return true;
    }

    void ConfigureServer() {
        server.set_read_timeout(5, 0);
        server.set_write_timeout(5, 0);
        server.Get("/health", [&](const http::Request& request, http::Response& response) {
            Config config = owner->GetConfig();
            WorkerActivity activity = CurrentActivity();
            const bool rendererReady = RendererReady(config);
            const double idleSeconds = IdleSeconds();
            const bool idleReady = owner->mockRenderer_ ||
                IsComputerIdleForSharedWork(idleSeconds, config.idleAfterSeconds);
            json snapshot{{"status", "ok"}, {"protocol", kProtocolVersion},
                     {"app_version", kRenderMeshAppVersion}, {"project_cache", true},
                     {"frame_packages", true}, {"result_bundles", true},
                     {"package_cancel", true}, {"job_cleanup", true},
                     {"resumable_project_upload", true}, {"max_package_frames", 20},
                      {"peer_exchange", true}, {"direct_status", true},
                      {"frame_start", true},
                      {"idle_ready", idleReady},
                     {"node_id", config.nodeId},
                     {"name", config.nodeName}, {"share", config.shareEnabled},
                     {"pairing_pending", config.pairingPending},
                     {"matching_id_binding", MatchingIdBinding(config)},
                     {"blender_ready", rendererReady},
                     {"assigned", workerAssigned.load()}, {"cpu", 100},
                     {"cpu_model", hardware.cpuModel}, {"gpu_models", hardware.gpuModels},
                     {"pairing_available", IsInviteCode(config.inviteCode) &&
                         config.clusterToken.size() >= 16},
                     {"task_id", CurrentTaskId()},
                     {"activity_job", HeaderDisplayText(activity.jobName, 128)},
                     {"activity_frames", activity.frames},
                     {"activity_completed", activity.completedFrames},
                     {"activity_rendering", activity.rendering},
                     {"idle", static_cast<int>(idleSeconds)}};
            std::string challenge = request.has_param("challenge")
                ? request.get_param_value("challenge") : "";
            if (SafeId(challenge) && challenge.size() >= 16 &&
                !config.pairingPending && config.clusterToken.size() >= 16) {
                snapshot["activity_proof"] = ActivitySnapshotProof(
                    config, challenge, config.nodeId, activity);
                snapshot["job_cleanup_proof"] = CleanupCapabilityProof(
                    config, challenge, config.nodeId, true);
                snapshot["blender_readiness_proof"] = BlenderReadinessProof(
                    config, challenge, config.nodeId, rendererReady);
                snapshot["peer_exchange_proof"] = PeerExchangeCapabilityProof(
                    config, challenge, config.nodeId, kProtocolVersion,
                    kRenderMeshAppVersion, true);
                snapshot["direct_status_proof"] = DirectStatusProof(
                    config, config.nodeId, challenge, snapshot);
            }
            response.set_content(snapshot.dump(), "application/json");
        });
        // Local control API for `RenderMesh.exe <command>`. Only processes on
        // this computer that can read config.json (the node secret) may use it.
        auto localControl = [this](const http::Request& request, http::Response& response) {
            if (request.remote_addr == "127.0.0.1" &&
                ConstantTimeEqual(request.get_header_value("X-RM-Local-Token"),
                                  owner->GetConfig().clusterToken))
                return true;
            response.status = 401;
            return false;
        };
        auto reply = [](http::Response& response, const json& body, int status = 200) {
            response.status = status;
            response.set_content(body.dump(), "application/json; charset=utf-8");
        };
        auto failure = [reply](http::Response& response, const std::wstring& error,
                               int status = 409) {
            reply(response, json{{"ok", false}, {"error", WideToUtf8(error)}}, status);
        };
        server.Get("/v5/local/status", [=, this](const http::Request& request,
                                                   http::Response& response) {
            if (!localControl(request, response)) return;
            const Config config = owner->GetConfig();
            json peerRows = json::array();
            for (const auto& peer : owner->Peers())
                peerRows.push_back({
                    {"node_id", peer.nodeId}, {"name", peer.name},
                    {"address", peer.address + ":" + std::to_string(peer.httpPort)},
                    {"app_version", peer.appVersion}, {"trusted", peer.trusted},
                    {"share", peer.shareEnabled}, {"renderer_ready", peer.rendererReady},
                    {"idle_ready", peer.idleReadyKnown ? json(peer.idleReady) : json()},
                    {"assigned", peer.assigned}, {"activity_job", peer.activity.jobName},
                    {"cpu_model", peer.cpuModel}, {"gpu_models", peer.gpuModels},
                });
            const fs::path renderer = BlenderRenderAdapter().LocateExecutable(config);
            reply(response, {
                {"ok", true},
                {"node", {
                    {"id", config.nodeId}, {"name", config.nodeName},
                    {"version", kRenderMeshAppVersion}, {"http_port", config.httpPort},
                    {"matching_id", config.inviteCode},
                    {"pairing_pending", config.pairingPending},
                    {"share", config.shareEnabled},
                    {"idle_seconds", static_cast<int>(IdleSeconds())},
                    {"idle_after_seconds", config.idleAfterSeconds},
                    {"renderer_ready", !renderer.empty()},
                    {"renderer_path", WideToUtf8(renderer.wstring())},
                    {"project_storage", WideToUtf8(owner->ProjectStorageDir().wstring())},
                    {"cpu_model", hardware.cpuModel}, {"gpu_models", hardware.gpuModels},
                    {"assigned", workerAssigned.load()},
                }},
                {"peers", std::move(peerRows)},
            });
        });
        server.Get("/v5/local/jobs", [=, this](const http::Request& request,
                                                 http::Response& response) {
            if (!localControl(request, response)) return;
            auto values = owner->Jobs();
            std::sort(values.begin(), values.end(), [](const RenderJob& left,
                                                       const RenderJob& right) {
                return left.createdAt > right.createdAt;
            });
            json rows = json::array();
            for (const auto& job : values) rows.push_back(JobJson(job, false));
            reply(response, {{"ok", true}, {"jobs", std::move(rows)}});
        });
        server.Get("/v5/local/job", [=, this](const http::Request& request,
                                                http::Response& response) {
            if (!localControl(request, response)) return;
            const auto job = owner->FindJob(request.get_param_value("id"));
            if (!job) return failure(response, L"未找到任务", 404);
            reply(response, {{"ok", true}, {"job", JobJson(*job, true)}});
        });
        server.Post("/v5/local/jobs", [=, this](const http::Request& request,
                                                  http::Response& response) {
            if (!localControl(request, response)) return;
            const json body = json::parse(request.body, nullptr, false);
            PreflightReport report;
            if (!body.is_object() || !PreflightReportFromJson(body.value("report", json()), report))
                return failure(response, L"工程检查报告无效", 422);
            // Only a copy inside this node's preflight folder may be moved.
            const auto relative = RelativeManagedPath(
                owner->ProjectStorageDir() / L"preflight", report.preparedProject);
            if (!relative) report.preparedCopy = false;
            std::wstring error;
            const std::string id = owner->CreatePreparedDraft(
                body.value("name", ""), report, error);
            if (id.empty()) return failure(response, error);
            reply(response, {{"ok", true}, {"job", JobJson(*owner->FindJob(id), false)}});
        });
        server.Post("/v5/local/job-action", [=, this](const http::Request& request,
                                                        http::Response& response) {
            if (!localControl(request, response)) return;
            const json body = json::parse(request.body, nullptr, false);
            if (!body.is_object()) return failure(response, L"请求无效", 422);
            const std::string action = body.value("action", "");
            const std::string id = body.value("id", "");
            const auto job = owner->FindJob(id);
            if (!job) return failure(response, L"未找到任务", 404);
            std::wstring error;
            bool ok = false;
            if (action == "test") {
                const int first = job->frames.empty() ? 0 : job->frames.front().frame;
                const int last = job->frames.empty() ? 0 : job->frames.back().frame;
                const int step = job->frames.size() > 1
                    ? job->frames[1].frame - job->frames[0].frame : 1;
                ok = owner->ConfigureAndStartTest(
                    id, body.value("frame_start", first), body.value("frame_end", last),
                    body.value("frame_step", step),
                    body.value("package_size", size_t{5}),
                    Utf8ToWide(body.value("test_frames", std::to_string(first))), error);
            } else if (action == "start") {
                ok = owner->StartFullRender(id, error);
            } else if (action == "stop") {
                ok = owner->StopJob(id, error);
            } else if (action == "resume") {
                ok = owner->ResumeStoppedJob(id, error);
            } else if (action == "retry") {
                ok = owner->RetryFailedFrames(id, error);
            } else if (action == "delete") {
                ok = owner->DeleteJob(id, error);
                if (ok) return reply(response, {{"ok", true}, {"deleted", id}});
            } else {
                return failure(response, L"未知操作：" + Utf8ToWide(action), 422);
            }
            if (!ok) return failure(response, error);
            reply(response, {{"ok", true}, {"job", JobJson(*owner->FindJob(id), false)}});
        });
        server.Get("/v1/update/manifest", [&](const http::Request& request,
                                                  http::Response& response) {
            const Config config = owner->GetConfig();
            if (config.pairingPending || config.clusterToken.size() < 16 ||
                !ConstantTimeEqual(request.get_header_value("X-RM-Token"),
                                   config.clusterToken)) {
                response.status = 401;
                return;
            }
            const fs::path executable = ExecutablePath();
            std::error_code sizeError;
            const uint64_t size = fs::file_size(executable, sizeError);
            const std::string sha256 = sizeError || size == 0 || size > kMaxUpdateBytes
                ? std::string{} : Sha256File(executable, &stopRequested);
            if (!SafeSha256(sha256)) {
                response.status = 503;
                response.set_content("update executable unavailable", "text/plain");
                return;
            }
            response.set_header("Cache-Control", "no-store");
            response.set_content(json{
                {"version", kRenderMeshAppVersion}, {"size", size}, {"sha256", sha256},
                {"signature", UpdateManifestSignature(
                    config, kRenderMeshAppVersion, size, sha256)},
            }.dump(), "application/json");
        });
        server.Get("/v1/update/package", [&](const http::Request& request,
                                                 http::Response& response) {
            const Config config = owner->GetConfig();
            if (config.pairingPending || config.clusterToken.size() < 16 ||
                !ConstantTimeEqual(request.get_header_value("X-RM-Token"),
                                   config.clusterToken)) {
                response.status = 401;
                return;
            }
            const fs::path executable = ExecutablePath();
            std::error_code sizeError;
            const uint64_t size = fs::file_size(executable, sizeError);
            const std::string sha256 = sizeError || size == 0 || size > kMaxUpdateBytes
                ? std::string{} : Sha256File(executable, &stopRequested);
            if (!SafeSha256(sha256) ||
                !ConstantTimeEqual(request.get_header_value("X-RM-Sha256"), sha256)) {
                response.status = 409;
                response.set_content("update package changed", "text/plain");
                return;
            }
            std::string contents;
            if (!ReadUpdatePackage(executable, size, contents)) {
                response.status = 503;
                response.set_content("unable to read update package", "text/plain");
                return;
            }
            response.set_header("Cache-Control", "no-store");
            response.set_header("X-RM-Sha256", sha256);
            response.set_header("Content-Disposition", "attachment; filename=RenderMesh.exe");
            response.set_content(std::move(contents), "application/octet-stream");
        });
        server.Get("/v1/peers", [&](const http::Request& request,
                                       http::Response& response) {
            Config config = owner->GetConfig();
            const std::string challenge = request.has_param("challenge")
                ? request.get_param_value("challenge") : "";
            if (config.pairingPending || config.clusterToken.size() < 16 ||
                !SafeId(challenge) || challenge.size() < 16 ||
                !ConstantTimeEqual(request.get_header_value("X-RM-Token"),
                                   config.clusterToken)) {
                response.status = 401;
                return;
            }
            json directory = json::array();
            {
                std::scoped_lock lock(stateMutex);
                const auto now = std::chrono::steady_clock::now();
                for (const auto& [nodeId, peer] : peers) {
                    if (directory.size() >= 128 || !peer.trusted ||
                        now - peer.lastSeen > kPeerTimeout || peer.address.empty() ||
                        peer.httpPort < 1024 || peer.httpPort > 65535)
                        continue;
                    directory.push_back({
                        {"node_id", nodeId}, {"address", peer.address},
                        {"port", peer.httpPort},
                    });
                }
            }
            response.set_content(json{
                {"node_id", config.nodeId}, {"challenge", challenge},
                {"peers", directory},
                {"proof", PeerDirectoryProof(config, config.nodeId, challenge, directory)},
            }.dump(), "application/json");
        });
        server.Post("/v2/pair", [&](const http::Request& request,
                                      http::Response& response) {
            if (!AllowPairingAttempt(request.remote_addr)) {
                response.status = 429;
                response.set_content("too many pairing attempts", "text/plain");
                return;
            }
            Config config = owner->GetConfig();
            std::string clientNodeId = request.get_header_value("X-RM-Node");
            std::string nonce = request.get_header_value("X-RM-Nonce");
            std::string claimed = request.get_header_value("X-RM-Proof");
            if (config.clusterToken.size() < 16 ||
                !SafeId(clientNodeId) || !SafeId(nonce) || nonce.size() < 16) {
                response.status = 409;
                return;
            }
            std::string expected = HmacSha256(
                config.inviteCode,
                "pair-request|" + clientNodeId + "|" + config.nodeId + "|" + nonce);
            if (!ConstantTimeEqual(claimed, expected)) {
                response.status = 401;
                return;
            }
            Config current = owner->GetConfig();
            if (current.inviteCode != config.inviteCode ||
                current.clusterToken != config.clusterToken) {
                response.status = 409;
                return;
            }
            if (current.pairingPending) {
                current.pairingPending = false;
                std::wstring saveError;
                if (!owner->UpdateConfig(current, saveError)) {
                    response.status = 503;
                    response.set_content("unable to persist pairing state", "text/plain");
                    return;
                }
                config = owner->GetConfig();
            }
            response.set_content(json{
                {"node_id", config.nodeId},
                {"cluster_token", config.clusterToken},
                {"proof", HmacSha256(
                    config.inviteCode,
                    "pair-response|" + config.nodeId + "|" + clientNodeId + "|" + nonce +
                        "|" + config.clusterToken)},
            }.dump(), "application/json");
            LogInfo("approved LAN pairing for node " + clientNodeId);
        });
        server.Post("/v3/cancel", [&](const http::Request& request,
                                        http::Response& response) {
            Config config = owner->GetConfig();
            if (config.pairingPending || config.clusterToken.size() < 16 ||
                !ConstantTimeEqual(request.get_header_value("X-RM-Token"),
                                   config.clusterToken)) {
                response.status = 401;
                return;
            }
            std::string packageId = request.get_header_value("X-RM-Package");
            if (!SafeId(packageId)) {
                response.status = 422;
                return;
            }
            bool abortBeforeFirstFrame = true;
            {
                std::scoped_lock lock(currentTaskMutex);
                abortBeforeFirstFrame = ShouldAbortPackageBeforeFirstFrame(
                    currentTaskId, packageId, currentPackageHasStartedFrame);
            }
            CancelPackage(packageId, abortBeforeFirstFrame);
            fs::path marker = owner->ProjectStorageDir() / L"incoming" /
                Utf8ToWide(packageId) /
                              L"outputs" / L"stop-after-current";
            std::error_code ignored;
            fs::create_directories(marker.parent_path(), ignored);
            WriteFileText(marker, "stop");
            workCv.notify_all();
            response.status = 202;
            response.set_content("cancellation recorded", "text/plain");
            LogInfo("remote package cancellation recorded; package=" + packageId);
        });
        server.Post("/v4/job-cleanup", [&](const http::Request& request,
                                               http::Response& response) {
            std::unique_lock admissionLock(workAdmissionMutex);
            Config config = owner->GetConfig();
            if (config.pairingPending || config.clusterToken.size() < 16 ||
                !ConstantTimeEqual(request.get_header_value("X-RM-Token"),
                                   config.clusterToken)) {
                response.status = 401;
                return;
            }
            const std::string jobId = request.get_header_value("X-RM-Job");
            const std::string hash = request.get_header_value("X-RM-Project");
            const std::string operationId = request.get_header_value("X-RM-Operation");
            if (!SafeId(jobId) || !SafeSha256(hash) || !SafeId(operationId)) {
                response.status = 422;
                return;
            }
            auto operationMutex = CacheOperationMutex(hash);
            std::scoped_lock operationLock(*operationMutex);
            if (workerAssigned.load() != 0) {
                response.status = 409;
                response.set_content("worker is rendering; cleanup will be retried", "text/plain");
                return;
            }
            bool hashStillQueued = false;
            bool cleanupFailed = false;
            {
                std::scoped_lock lock(workMutex);
                std::queue<WorkItem> retained;
                while (!work.empty()) {
                    WorkItem item = std::move(work.front());
                    work.pop();
                    if (item.jobId == jobId) {
                        std::error_code removeError;
                        if (!item.temporaryTaskDir.empty())
                            fs::remove_all(item.temporaryTaskDir, removeError);
                        if (!removeError) continue;
                        cleanupFailed = true;
                    }
                    hashStillQueued = hashStillQueued || item.projectHash == hash;
                    retained.push(std::move(item));
                }
                work.swap(retained);
            }
            if (!cleanupFailed && !hashStillQueued) {
                std::scoped_lock lock(cacheMutex);
                std::error_code removeError;
                fs::remove_all(ProjectCacheDirectory(
                    owner->ProjectStorageDir(), hash), removeError);
                cleanupFailed = static_cast<bool>(removeError);
            }
            if (cleanupFailed) {
                response.status = 503;
                response.set_content("job artifacts are busy; cleanup will be retried", "text/plain");
                return;
            }
            response.status = 200;
            response.set_content("job artifacts removed", "text/plain");
            LogInfo("remote job cleanup completed; job=" + jobId +
                    "; operation=" + operationId);
        });
        server.Get("/v3/cache/upload-status", [&](const http::Request& request,
                                                     http::Response& response) {
            Config config = owner->GetConfig();
            if (config.pairingPending || config.clusterToken.size() < 16 ||
                !ConstantTimeEqual(request.get_header_value("X-RM-Token"),
                                   config.clusterToken)) {
                response.status = 401;
                return;
            }
            const std::string hash = request.has_param("hash")
                ? request.get_param_value("hash") : "";
            const uint64_t total = HeaderUint64(request, "X-RM-Total");
            const std::string version = request.get_header_value("X-RM-Blender");
            const std::string engine = request.get_header_value("X-RM-Engine");
            const bool requiresGpu = request.get_header_value("X-RM-Requires-GPU") == "1";
            const bool projectBundle =
                request.get_header_value("X-RM-Project-Bundle") == "1";
            if (!SafeSha256(hash) || !total || total > kMaxTransferBytes ||
                version.empty() || engine.empty()) {
                response.status = 422;
                return;
            }
            auto operationMutex = CacheOperationMutex(hash);
            std::scoped_lock operationLock(*operationMutex);
            std::scoped_lock lock(cacheMutex);
            if (CacheMetadataMatches(owner->ProjectStorageDir(), hash, version, engine,
                                     requiresGpu, projectBundle)) {
                TouchCacheEntry(owner->ProjectStorageDir(), hash);
                response.set_content(json{{"cached", true}, {"offset", total},
                                          {"total", total}}.dump(), "application/json");
                return;
            }
            const fs::path partial = ProjectUploadPartial(
                owner->ProjectStorageDir(), hash);
            std::error_code sizeError;
            uint64_t offset = fs::is_regular_file(partial, sizeError)
                ? fs::file_size(partial, sizeError) : 0;
            if (sizeError || offset > total) {
                std::error_code ignored;
                fs::remove_all(ProjectUploadDirectory(
                    owner->ProjectStorageDir(), hash), ignored);
                offset = 0;
            }
            response.set_content(json{{"cached", false}, {"offset", offset},
                                      {"total", total}}.dump(), "application/json");
        });
        server.Post("/v3/cache/chunk", [&](const http::Request& request,
                                              http::Response& response,
                                              const http::ContentReader& reader) {
            Config config = owner->GetConfig();
            if (config.pairingPending || config.clusterToken.size() < 16 ||
                !ConstantTimeEqual(request.get_header_value("X-RM-Token"),
                                   config.clusterToken)) {
                response.status = 401;
                return;
            }
            if (!config.shareEnabled) {
                response.status = 409;
                response.set_content("compute sharing is disabled", "text/plain");
                return;
            }
            std::string body;
            body.reserve(kProjectUploadChunkBytes);
            bool oversized = false;
            const bool received = reader([&](const char* data, size_t size) {
                if (stopRequested.load()) return false;
                if (size > kProjectUploadChunkBytes - body.size()) {
                    oversized = true;
                    return false;
                }
                body.append(data, size);
                return true;
            });
            if (!received || stopRequested.load()) {
                response.status = oversized ? 413 : 499;
                response.set_content(oversized ? "project chunk exceeds 8 MiB"
                                               : "project chunk receive cancelled",
                                     "text/plain");
                return;
            }
            const std::string hash = request.get_header_value("X-RM-Project");
            const uint64_t total = HeaderUint64(request, "X-RM-Total");
            const uint64_t offset = HeaderUint64(request, "X-RM-Offset", UINT64_MAX);
            const std::string chunkHash = request.get_header_value("X-RM-Chunk-SHA256");
            const bool projectBundle =
                request.get_header_value("X-RM-Project-Bundle") == "1";
            if (!SafeSha256(hash) || !total || total > kMaxTransferBytes ||
                offset == UINT64_MAX || body.empty() ||
                offset > total || body.size() > total - offset ||
                !ConstantTimeEqual(chunkHash, Sha256Prefix(body, 64))) {
                response.status = 422;
                response.set_content("invalid project upload chunk", "text/plain");
                return;
            }
            // The per-project operation lock serialises writers of this partial
            // file; other projects and task admission are not blocked.
            auto operationMutex = CacheOperationMutex(hash);
            std::scoped_lock operationLock(*operationMutex);
            const fs::path directory = ProjectUploadDirectory(
                owner->ProjectStorageDir(), hash);
            const fs::path partial = ProjectUploadPartial(
                owner->ProjectStorageDir(), hash);
            std::error_code ignored;
            fs::create_directories(directory, ignored);
            const uint64_t required = projectBundle && total <= kMaxTransferBytes / 2
                ? total * 2 : projectBundle ? UINT64_MAX : total;
            if (ignored || (offset == 0 &&
                (required == UINT64_MAX || !HasFileCapacity(directory, required)))) {
                response.status = 507;
                return;
            }
            File file = File::OpenWrite(partial);
            if (!file) {
                response.status = 500;
                return;
            }
            const uint64_t size = file.Size();
            if (size != offset) {
                response.status = 409;
                response.set_content(json{{"offset", size}}.dump(), "application/json");
                return;
            }
            // No per-chunk flush: the commit flushes once, and bytes from an
            // interrupted earlier session are re-verified before publication.
            if (!file.Seek(offset) || !file.Write(body)) {
                response.status = 500;
                return;
            }
            {
                std::scoped_lock lock(cacheOperationMapMutex);
                auto& verified = verifiedUploadBytes[hash];
                if (offset == 0) verified = 0;
                if (verified == offset) verified = offset + body.size();
            }
            response.set_content(json{{"offset", offset + body.size()},
                                      {"total", total}}.dump(), "application/json");
        });
        server.Post("/v3/cache/commit", [&](const http::Request& request,
                                               http::Response& response) {
            std::shared_lock admissionLock(workAdmissionMutex);
            Config config = owner->GetConfig();
            if (config.pairingPending || config.clusterToken.size() < 16 ||
                !ConstantTimeEqual(request.get_header_value("X-RM-Token"),
                                   config.clusterToken)) {
                response.status = 401;
                return;
            }
            if (!config.shareEnabled) {
                response.status = 409;
                response.set_content("compute sharing is disabled", "text/plain");
                return;
            }
            const std::string hash = request.get_header_value("X-RM-Project");
            const uint64_t total = HeaderUint64(request, "X-RM-Total");
            const std::string version = request.get_header_value("X-RM-Blender");
            const std::string engine = request.get_header_value("X-RM-Engine");
            const bool requiresGpu = request.get_header_value("X-RM-Requires-GPU") == "1";
            const bool projectBundle =
                request.get_header_value("X-RM-Project-Bundle") == "1";
            if (!SafeSha256(hash) || !total || total > kMaxTransferBytes ||
                version.empty() || engine.empty()) {
                response.status = 422;
                return;
            }
            auto operationMutex = CacheOperationMutex(hash);
            std::scoped_lock operationLock(*operationMutex);
            const fs::path partial = ProjectUploadPartial(
                owner->ProjectStorageDir(), hash);
            const fs::path cacheDirectory = ProjectCacheDirectory(
                owner->ProjectStorageDir(), hash);
            const fs::path committing =
                ProjectUploadDirectory(owner->ProjectStorageDir(), hash) /
                L"project.payload.committing";
            std::error_code fileError;
            {
                std::scoped_lock cacheLock(cacheMutex);
                if (CacheMetadataMatches(
                        owner->ProjectStorageDir(), hash, version, engine, requiresGpu,
                        projectBundle)) {
                    TouchCacheEntry(owner->ProjectStorageDir(), hash);
                    response.status = 200;
                    response.set_content("already cached", "text/plain");
                    return;
                }
                if (!fs::is_regular_file(partial, fileError) ||
                    fs::file_size(partial, fileError) != total) {
                    response.status = 409;
                    response.set_content("project upload is incomplete", "text/plain");
                    return;
                }
                fs::create_directories(ProjectUploadDirectory(
                    owner->ProjectStorageDir(), hash),
                                       fileError);
                if (fileError || !MoveFileExW(partial.c_str(), committing.c_str(),
                        MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
                    response.status = 500;
                    return;
                }
            }
            bool verifiedThisSession = false;
            {
                std::scoped_lock lock(cacheOperationMapMutex);
                auto verified = verifiedUploadBytes.find(hash);
                verifiedThisSession = verified != verifiedUploadBytes.end() &&
                    verified->second == total;
                if (verified != verifiedUploadBytes.end()) verifiedUploadBytes.erase(verified);
            }
            // Every chunk written by this process was checked against its own
            // SHA-256 on arrival. Only bytes that survived from an earlier,
            // possibly interrupted session need a full-file verification.
            const std::string receivedHash = [&] {
                if (!verifiedThisSession) return Sha256File(committing, &stopRequested);
                File written = File::OpenWrite(committing);
                return written && written.Flush() ? hash : std::string{};
            }();
            if (stopRequested.load()) {
                std::scoped_lock cacheLock(cacheMutex);
                fs::create_directories(ProjectUploadDirectory(
                    owner->ProjectStorageDir(), hash),
                                       fileError);
                const bool restored = !fileError && MoveFileExW(
                    committing.c_str(), partial.c_str(),
                    MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
                response.status = restored ? 499 : 500;
                response.set_content(restored
                    ? "project commit cancelled; upload preserved"
                    : "project commit cancelled; upload restore failed", "text/plain");
                return;
            }
            if (receivedHash != hash) {
                fs::remove(committing, fileError);
                fs::remove_all(ProjectUploadDirectory(
                    owner->ProjectStorageDir(), hash), fileError);
                response.status = 422;
                response.set_content("project hash mismatch", "text/plain");
                return;
            }
            auto restoreUpload = [&] {
                fileError.clear();
                fs::create_directories(ProjectUploadDirectory(
                    owner->ProjectStorageDir(), hash),
                                       fileError);
                return !fileError && MoveFileExW(
                    committing.c_str(), partial.c_str(),
                    MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
            };
            if (projectBundle) {
                if (!HasFileCapacity(cacheDirectory.parent_path(), total)) {
                    restoreUpload();
                    response.status = 507;
                    response.set_content("insufficient space to extract tex project bundle",
                                         "text/plain");
                    return;
                }
                {
                    std::scoped_lock cacheLock(cacheMutex);
                    fileError.clear();
                    fs::remove_all(cacheDirectory, fileError);
                }
                std::wstring extractError;
                if (!ExtractProjectBundle(
                        committing, cacheDirectory, extractError, &stopRequested)) {
                    const bool cancelled = stopRequested.load();
                    if (cancelled) restoreUpload();
                    else {
                        fs::remove(committing, fileError);
                        fs::remove_all(ProjectUploadDirectory(
                            owner->ProjectStorageDir(), hash),
                                       fileError);
                    }
                    response.status = cancelled ? 499 : 422;
                    response.set_content(WideToUtf8(extractError),
                                         "text/plain; charset=utf-8");
                    return;
                }
                fs::remove(committing, fileError);
            } else {
                const fs::path blend = cacheDirectory / L"source.blend";
                std::scoped_lock cacheLock(cacheMutex);
                fs::create_directories(cacheDirectory, fileError);
                if (fileError || !MoveFileExW(
                        committing.c_str(), blend.c_str(),
                        MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
                    restoreUpload();
                    response.status = 500;
                    return;
                }
            }
            {
                std::scoped_lock cacheLock(cacheMutex);
                // Project synchronisation is file I/O, not a render admission.
                // The actual package remains fenced by idle-only admission.
                std::wstring metadataError;
                if (!WriteCacheMetadata(owner->ProjectStorageDir(), hash, version, engine,
                                        requiresGpu, metadataError, projectBundle)) {
                    response.status = 500;
                    response.set_content(WideToUtf8(metadataError),
                                         "text/plain; charset=utf-8");
                    return;
                }
                TouchCacheEntry(owner->ProjectStorageDir(), hash);
                fs::remove_all(ProjectUploadDirectory(
                    owner->ProjectStorageDir(), hash), fileError);
            }
            response.status = 201;
            response.set_content("cached", "text/plain");
            LogInfo("resumable project upload committed; hash=" + hash.substr(0, 16));
        });
        server.Get("/v2/cache/status", [&](const http::Request& request,
                                             http::Response& response) {
            Config config = owner->GetConfig();
            if (config.pairingPending || config.clusterToken.size() < 16 ||
                !ConstantTimeEqual(request.get_header_value("X-RM-Token"),
                                   config.clusterToken)) {
                response.status = 401;
                return;
            }
            std::string hash = request.has_param("hash") ? request.get_param_value("hash") : "";
            std::string version = request.get_header_value("X-RM-Blender");
            std::string engine = request.get_header_value("X-RM-Engine");
            bool requiresGpu = request.get_header_value("X-RM-Requires-GPU") == "1";
            const bool projectBundle =
                request.get_header_value("X-RM-Project-Bundle") == "1";
            if (!SafeSha256(hash) || version.empty() || engine.empty()) {
                response.status = 422;
                return;
            }
            std::scoped_lock lock(cacheMutex);
            if (!CacheMetadataMatches(owner->ProjectStorageDir(), hash, version, engine,
                                      requiresGpu, projectBundle)) {
                response.status = 404;
                return;
            }
            response.set_content(json{{"cached", true}, {"project_hash", hash}}.dump(),
                                 "application/json");
        });
        server.Post("/v3/package", [&](const http::Request& request,
                                          http::Response& response) {
            std::shared_lock admissionLock(workAdmissionMutex);
            Config config = owner->GetConfig();
            if (config.pairingPending || config.clusterToken.size() < 16 ||
                !ConstantTimeEqual(request.get_header_value("X-RM-Token"),
                                   config.clusterToken)) {
                response.status = 401;
                return;
            }
            if (!config.shareEnabled) {
                response.status = 409;
                response.set_content("compute sharing is disabled", "text/plain");
                return;
            }
            std::string packageId = request.get_header_value("X-RM-Package");
            std::string jobId = request.get_header_value("X-RM-Job");
            std::string jobName = HeaderDisplayText(request.get_header_value("X-RM-Job-Name"));
            std::string originName = HeaderDisplayText(request.get_header_value("X-RM-Origin-Name"));
            std::string hash = request.get_header_value("X-RM-Project");
            int originPort = HeaderInt(request, "X-RM-Origin-Port", 0);
            std::string format = request.get_header_value("X-RM-Format");
            std::string extension = request.get_header_value("X-RM-Extension");
            std::string version = request.get_header_value("X-RM-Blender");
            std::string engine = request.get_header_value("X-RM-Engine");
            bool requiresGpu = request.get_header_value("X-RM-Requires-GPU") == "1";
            const bool projectBundle =
                request.get_header_value("X-RM-Project-Bundle") == "1";
            // Origins before 0.3 omit the header; they only submit Blender work.
            const std::string software = request.has_header("X-RM-Software")
                ? request.get_header_value("X-RM-Software") : std::string("blender");
            if (!FindRenderAdapter(software)) {
                response.status = 409;
                response.set_content("render software is not supported: " +
                                     HeaderDisplayText(software, 32), "text/plain");
                return;
            }
            std::vector<OutputTarget> compositorOutputs;
            if (!HeaderOutputTargets(request, "X-RM-Compositor", compositorOutputs)) {
                response.status = 422;
                return;
            }
            std::vector<WorkItem::Frame> packageFrames;
            try {
                json values = json::parse(request.get_header_value("X-RM-Frames"));
                if (!values.is_array() || values.empty() || values.size() > 20)
                    throw std::runtime_error("invalid frame count");
                std::set<std::string> taskIds;
                std::set<int> frameNumbers;
                for (const auto& value : values) {
                    WorkItem::Frame frame;
                    frame.taskId = value.value("task", "");
                    frame.frame = value.value("frame", kBlenderFrameMin - 1);
                    frame.attemptToken = value.value("attempt", "");
                    if (!SafeId(frame.taskId) || !SafeId(frame.attemptToken) ||
                        frame.frame < kBlenderFrameMin || frame.frame > kBlenderFrameMax ||
                        !taskIds.insert(frame.taskId).second ||
                        !frameNumbers.insert(frame.frame).second)
                        throw std::runtime_error("invalid frame metadata");
                    packageFrames.push_back(std::move(frame));
                }
            } catch (...) {
                response.status = 422;
                response.set_content("invalid frame package", "text/plain");
                return;
            }
            const size_t packageFrameCount = packageFrames.size();
            if (!SafeId(packageId) || !SafeId(jobId) || !SafeSha256(hash) ||
                originPort < 1024 || originPort > 65535 || format.empty() ||
                version.empty() || engine.empty() || !IsSafeOutputExtension(extension)) {
                response.status = 422;
                return;
            }
            if (PackageCancelled(packageId)) {
                response.status = 409;
                response.set_content("package was cancelled before acceptance", "text/plain");
                return;
            }
            if (!owner->mockRenderer_ &&
                !IsComputerIdleForSharedWork(IdleSeconds(), config.idleAfterSeconds)) {
                response.status = 425;
                response.set_content("package waits until this computer is idle",
                                     "text/plain");
                return;
            }
            std::unique_lock cacheLock(cacheMutex);
            if (!CacheMetadataMatches(owner->ProjectStorageDir(), hash, version, engine,
                                      requiresGpu, projectBundle)) {
                response.status = 404;
                response.set_content("project cache miss", "text/plain");
                return;
            }
            int expectedAssigned = 0;
            if (!workerAssigned.compare_exchange_strong(expectedAssigned, 1)) {
                response.status = 409;
                response.set_content("worker is busy", "text/plain");
                return;
            }
            struct AssignmentReservation {
                std::atomic<int>& count;
                bool handedOff = false;
                ~AssignmentReservation() { if (!handedOff) --count; }
            } reservation{workerAssigned};
            TouchCacheEntry(owner->ProjectStorageDir(), hash);
            fs::path taskDir = owner->ProjectStorageDir() / L"incoming" /
                Utf8ToWide(packageId);
            WorkItem item;
            item.taskId = packageFrames.front().taskId;
            item.jobId = jobId;
            item.jobName = jobName.empty() ? jobId : std::move(jobName);
            item.frame = packageFrames.front().frame;
            item.attemptToken = packageFrames.front().attemptToken;
            item.packageId = packageId;
            item.frames = std::move(packageFrames);
            item.projectHash = hash;
            item.software = software;
            item.project = ProjectCacheDirectory(
                owner->ProjectStorageDir(), hash) / L"source.blend";
            item.temporaryTaskDir = taskDir;
            item.outputDir = taskDir / L"outputs";
            item.outputFormat = format;
            item.outputExtension = extension;
            item.softwareVersion = version;
            item.renderEngine = engine;
            item.requiresGpu = requiresGpu;
            item.compositorOutputs = std::move(compositorOutputs);
            item.remote = true;
            item.resultBundles = request.get_header_value("X-RM-Result-Bundles") == "1";
            item.frameLogs = request.get_header_value("X-RM-Frame-Logs") == "1";
            item.originName = originName.empty() ? request.remote_addr : std::move(originName);
            item.originAddress = request.remote_addr;
            item.originPort = originPort;
            {
                std::scoped_lock lock(workMutex);
                work.push(std::move(item));
            }
            reservation.handedOff = true;
            cacheLock.unlock();
            workCv.notify_one();
            Notify();
            response.status = 202;
            response.set_content("accepted frame package", "text/plain");
            LogInfo("accepted package " + packageId + "; frames=" +
                    std::to_string(packageFrameCount));
        });
        server.Get("/v1/prove", [&](const http::Request& request,
                                     http::Response& response) {
            std::string challenge = request.has_param("challenge")
                ? request.get_param_value("challenge") : "";
            if (!SafeId(challenge) || challenge.size() < 16) {
                response.status = 422;
                return;
            }
            Config config = owner->GetConfig();
            if (config.pairingPending || config.clusterToken.size() < 16) {
                response.status = 409;
                return;
            }
            response.set_content(json{
                {"node_id", config.nodeId},
                {"matching_id_binding", MatchingIdBinding(config)},
                {"proof", HmacSha256(config.clusterToken,
                    "worker-proof|" + config.nodeId + "|" + challenge)},
            }.dump(), "application/json");
        });
        server.Post("/v1/frame-start", [&](const http::Request& request,
                                             http::Response& response) {
            Config config = owner->GetConfig();
            if (config.pairingPending || config.clusterToken.size() < 16 ||
                !ConstantTimeEqual(request.get_header_value("X-RM-Token"),
                                   config.clusterToken)) {
                response.status = 401;
                return;
            }
            const std::string taskId = request.get_header_value("X-RM-Task");
            const std::string attemptToken = request.get_header_value("X-RM-Attempt");
            const uint64_t startedMilliseconds =
                HeaderUint64(request, "X-RM-Started-At");
            if (!SafeId(taskId) || !SafeId(attemptToken) || startedMilliseconds == 0 ||
                startedMilliseconds > static_cast<uint64_t>(INT64_MAX)) {
                response.status = 422;
                return;
            }
            std::string jobId;
            bool changed = false;
            {
                std::scoped_lock lock(stateMutex);
                for (auto& job : jobs) {
                    auto frame = std::find_if(job.frames.begin(), job.frames.end(),
                        [&](const FrameTask& value) { return value.taskId == taskId; });
                    if (frame == job.frames.end()) continue;
                    if (frame->attemptToken != attemptToken ||
                        !IsActiveFrameLease(frame->state)) {
                        response.status = 409;
                        response.set_content("stale or superseded assignment", "text/plain");
                        return;
                    }
                    jobId = job.id;
                    if (frame->state == TaskState::Assigned) {
                        frame->state = TaskState::Rendering;
                        frame->startedAt = TimeFromUnixMilliseconds(
                            static_cast<int64_t>(startedMilliseconds));
                        frame->endedAt = {};
                        changed = true;
                    } else if (frame->startedAt.time_since_epoch() ==
                               std::chrono::system_clock::duration::zero()) {
                        frame->startedAt = TimeFromUnixMilliseconds(
                            static_cast<int64_t>(startedMilliseconds));
                        changed = true;
                    }
                    break;
                }
            }
            if (jobId.empty()) {
                response.status = 404;
                return;
            }
            if (changed) {
                PersistJobs(jobId);
                Notify();
                LogInfo("worker started frame; task=" + taskId);
            }
            response.status = 200;
            response.set_content("started", "text/plain");
        });
        server.Post(
            "/v1/frame-log",
            [&](const http::Request& request, http::Response& response,
                const http::ContentReader& reader) {
                Config config = owner->GetConfig();
                if (config.pairingPending || config.clusterToken.size() < 16 ||
                    !ConstantTimeEqual(request.get_header_value("X-RM-Token"),
                                       config.clusterToken)) {
                    response.status = 401;
                    return;
                }
                std::string taskId = request.get_header_value("X-RM-Task");
                std::string attemptToken = request.get_header_value("X-RM-Attempt");
                const bool finalLog =
                    request.get_header_value("X-RM-Complete") == "1";
                int contentLength = HeaderInt(request, "Content-Length", -1);
                if (!SafeId(taskId) || !SafeId(attemptToken) || contentLength < 0 ||
                    contentLength > 4 * 1024 * 1024) {
                    response.status = 422;
                    return;
                }
                size_t targetJob = 0;
                size_t targetFrame = 0;
                std::string targetJobId;
                fs::path staging;
                fs::path destination;
                bool found = false;
                {
                    std::scoped_lock lock(stateMutex);
                    for (size_t jobIndex = 0; jobIndex < jobs.size(); ++jobIndex) {
                        auto it = std::find_if(jobs[jobIndex].frames.begin(),
                            jobs[jobIndex].frames.end(), [&](const FrameTask& frame) {
                                return frame.taskId == taskId;
                            });
                        if (it == jobs[jobIndex].frames.end()) continue;
                        const bool activeAttempt = IsActiveFrameLease(it->state);
                        const bool completedAttempt = finalLog &&
                            (it->state == TaskState::Completed ||
                             it->state == TaskState::Failed);
                        if (it->attemptToken != attemptToken ||
                            (!activeAttempt && !completedAttempt)) {
                            response.status = 409;
                            response.set_content("stale or superseded assignment", "text/plain");
                            return;
                        }
                        targetJob = jobIndex;
                        targetJobId = jobs[jobIndex].id;
                        targetFrame = static_cast<size_t>(
                            std::distance(jobs[jobIndex].frames.begin(), it));
                        destination = FrameLogDestination(
                            owner->ProjectStorageDir(), jobs[jobIndex].id, it->frame);
                        // Keep log uploads outside the result staging tree. A result
                        // request may finish first and remove its entire staging
                        // directory while this request is still being streamed.
                        staging = owner->ProjectStorageDir() / L"jobs" /
                            Utf8ToWide(jobs[jobIndex].id) / L"log-receiving" /
                            Utf8ToWide(taskId + "-" + attemptToken + ".log");
                        found = true;
                        break;
                    }
                }
                if (!found) {
                    response.status = 404;
                    return;
                }
                std::wstring receiveError;
                if (!StreamRequestToFile(reader, staging, receiveError, &stopRequested)) {
                    response.status = stopRequested.load() ? 503 : 500;
                    response.set_content(WideToUtf8(receiveError), "text/plain; charset=utf-8");
                    return;
                }
                bool pathChanged = false;
                {
                    std::scoped_lock lock(stateMutex);
                    if (targetJob >= jobs.size() ||
                        targetFrame >= jobs[targetJob].frames.size()) {
                        response.status = 409;
                    } else {
                        auto& frame = jobs[targetJob].frames[targetFrame];
                        const bool activeAttempt = IsActiveFrameLease(frame.state);
                        const bool completedAttempt = finalLog &&
                            (frame.state == TaskState::Completed ||
                             frame.state == TaskState::Failed);
                        if (frame.taskId != taskId || frame.attemptToken != attemptToken ||
                            (!activeAttempt && !completedAttempt)) {
                            response.status = 409;
                        } else {
                            fs::create_directories(destination.parent_path());
                            if (!MoveFileExW(staging.c_str(), destination.c_str(),
                                    MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
                                response.status = 500;
                            } else {
                                pathChanged = frame.log != destination;
                                frame.log = destination;
                                response.status = 200;
                            }
                        }
                    }
                }
                if (response.status != 200) {
                    std::error_code ignored;
                    fs::remove(staging, ignored);
                    response.set_content("frame log assignment changed", "text/plain");
                    return;
                }
                if (pathChanged) PersistJobs(targetJobId);
                Notify();
                response.set_content("received", "text/plain");
            });
        server.Post(
            "/v1/result",
            [&](const http::Request& request, http::Response& response,
                const http::ContentReader& reader) {
                Config config = owner->GetConfig();
                if (config.pairingPending || config.clusterToken.size() < 16 ||
                    !ConstantTimeEqual(request.get_header_value("X-RM-Token"),
                                       config.clusterToken)) {
                    response.status = 401;
                    return;
                }
                std::string taskId = request.get_header_value("X-RM-Task");
                std::string attemptToken = request.get_header_value("X-RM-Attempt");
                std::string status = request.get_header_value("X-RM-Status");
                if (!SafeId(taskId) || !SafeId(attemptToken) ||
                    (status != "completed" && status != "failed" && status != "retry")) {
                    response.status = 422;
                    return;
                }
                if (status != "completed") {
                    LogWarning("worker reported " + status + " result; task=" + taskId +
                               "; error=" + HeaderDisplayText(
                                   request.get_header_value("X-RM-Error")));
                }
                bool resultBundle = request.get_header_value("X-RM-Result-Bundle") == "1";
                const uint64_t startedMilliseconds =
                    HeaderUint64(request, "X-RM-Started-At");
                const uint64_t endedMilliseconds =
                    HeaderUint64(request, "X-RM-Ended-At");
                const bool resultTimingValid = startedMilliseconds > 0 &&
                    endedMilliseconds >= startedMilliseconds &&
                    endedMilliseconds - startedMilliseconds <=
                        static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::hours(24 * 30)).count()) &&
                    endedMilliseconds <= static_cast<uint64_t>(INT64_MAX);
                fs::path stagingRoot;
                std::string targetJobId;
                size_t targetJob = 0;
                size_t targetFrame = 0;
                bool found = false;
                {
                    std::scoped_lock lock(stateMutex);
                    for (size_t jobIndex = 0; jobIndex < jobs.size(); ++jobIndex) {
                        auto& job = jobs[jobIndex];
                        auto it = std::find_if(job.frames.begin(), job.frames.end(),
                                               [&](const FrameTask& frame) {
                                                   return frame.taskId == taskId;
                                               });
                        if (it == job.frames.end()) continue;
                        targetJob = jobIndex;
                        targetFrame = static_cast<size_t>(std::distance(job.frames.begin(), it));
                        if (it->attemptToken != attemptToken ||
                            !IsActiveFrameLease(it->state)) {
                            response.status = 409;
                            response.set_content("stale or superseded assignment", "text/plain");
                            return;
                        }
                        found = true;
                        targetJobId = job.id;
                        stagingRoot = owner->ProjectStorageDir() / L"jobs" /
                            Utf8ToWide(job.id) /
                            L"receiving" / Utf8ToWide(taskId + "-" + attemptToken);
                        break;
                    }
                }
                if (!found) {
                    response.status = 404;
                    return;
                }
                auto jobCancelled = JobCancellation(targetJobId);
                if (status == "completed" && !HasTransferCapacity(
                        request, stagingRoot, resultBundle ? 2 : 1)) {
                    response.status = 507;
                    response.set_content("result exceeds transfer limit or available disk space", "text/plain");
                    return;
                }
                fs::path stagedResult;
                if (status == "completed") {
                    stagedResult = stagingRoot /
                        (resultBundle ? L"result.rmbundle" : L"primary.result");
                }
                std::wstring receiveError;
                bool ok = status != "completed" ||
                    StreamRequestToFile(reader, stagedResult, receiveError, &stopRequested);
                std::map<std::string, fs::path> receivedArtifacts;
                if (status == "completed" && ok) {
                    if (resultBundle) {
                        ok = ExtractResultBundle(stagedResult, stagingRoot / L"extracted",
                                                 receivedArtifacts, receiveError,
                                                 &stopRequested);
                    } else {
                        receivedArtifacts["primary"] = stagedResult;
                    }
                }
                if (stopRequested.load()) {
                    std::error_code ignored;
                    if (!stagingRoot.empty()) fs::remove_all(stagingRoot, ignored);
                    response.status = 503;
                    response.set_content("node is stopping", "text/plain");
                    return;
                }
                {
                    std::scoped_lock lock(stateMutex);
                    if (targetJob >= jobs.size() || targetFrame >= jobs[targetJob].frames.size() ||
                        jobs[targetJob].frames[targetFrame].taskId != taskId) {
                        std::error_code ignored;
                        if (!stagingRoot.empty()) fs::remove_all(stagingRoot, ignored);
                        response.status = 409;
                        response.set_content("task changed while receiving result", "text/plain");
                        return;
                    }
                    auto& target = jobs[targetJob].frames[targetFrame];
                    if (target.attemptToken != attemptToken ||
                        !IsActiveFrameLease(target.state)) {
                        std::error_code ignored;
                        if (!stagingRoot.empty()) fs::remove_all(stagingRoot, ignored);
                        response.status = 409;
                        response.set_content("assignment changed while receiving result", "text/plain");
                        return;
                    }
                    fs::path publishedPrimary;
                    if (status == "completed" && ok)
                        ok = PublishResultArtifacts(
                            owner->ProjectStorageDir(), jobs[targetJob], target.frame,
                            receivedArtifacts, attemptToken, publishedPrimary, receiveError,
                            jobCancelled.get());
                    if (stopRequested.load() && !ok) {
                        std::error_code ignored;
                        if (!stagingRoot.empty()) fs::remove_all(stagingRoot, ignored);
                        response.status = 503;
                        response.set_content("node is stopping", "text/plain");
                        return;
                    }
                    if (status == "completed" && ok) {
                        target.state = TaskState::Completed;
                        target.output = publishedPrimary;
                        target.artifacts = PublishedArtifactPaths(
                            owner->ProjectStorageDir(), jobs[targetJob], target.frame,
                            receivedArtifacts);
                        target.error.clear();
                        if (resultTimingValid) {
                            target.startedAt = TimeFromUnixMilliseconds(
                                static_cast<int64_t>(startedMilliseconds));
                            target.endedAt = TimeFromUnixMilliseconds(
                                static_cast<int64_t>(endedMilliseconds));
                        } else {
                            target.endedAt = std::chrono::system_clock::now();
                        }
                    } else if (status == "retry" && ok) {
                        target.state = TaskState::Pending;
                        target.error = request.get_header_value("X-RM-Error");
                        target.assignedNodeId.clear();
                        target.assignedNodeName.clear();
                        target.attemptToken.clear();
                        target.startedAt = {};
                        target.endedAt = {};
                    } else {
                        target.state = TaskState::Failed;
                        target.error = request.get_header_value("X-RM-Error");
                        if (!receiveError.empty()) target.error = WideToUtf8(receiveError);
                        target.endedAt = std::chrono::system_clock::now();
                    }
                    target.assignedAt = {};
                    target.assignedPackageId.clear();
                    RefreshExecutionStageLocked(jobs[targetJob]);
                }
                {
                    std::error_code ignored;
                    if (!stagingRoot.empty()) fs::remove_all(stagingRoot, ignored);
                }
                PersistJobs(targetJobId);
                Notify();
                response.status = ok ? 200 : 500;
                response.set_content(ok ? "received" : "receive failed", "text/plain");
                if (ok)
                    LogInfo("received task result " + taskId + "; status=" + status);
                else
                    LogWarning("failed receiving task result " + taskId);
            });
    }

    bool ProbeManualPeer(const std::string& endpoint, Peer& peer, std::wstring& result) {
        try {
        std::string address;
        int port = 0;
        if (!ParseIPv4Endpoint(endpoint, address, port)) {
            result = L"请输入局域网 IPv4 地址和端口，例如 192.168.28.28:8391";
            return false;
        }
        http::Client client(address, port);
        client.set_connection_timeout(2, 0);
        client.set_read_timeout(2, 0);
        client.set_write_timeout(2, 0);
        client.set_max_timeout(4000);
        auto healthResponse = client.Get("/health");
        if (!healthResponse || healthResponse->status != 200) {
            result = L"无法访问 " + Utf8ToWide(address) + L":" + std::to_wstring(port) +
                     L"；请检查对方软件、防火墙和局域网地址";
            return false;
        }
        json health = json::parse(healthResponse->body, nullptr, false);
        Config config = owner->GetConfig();
        if (health.is_discarded() || !health.is_object()) {
            result = L"目标响应不是有效的 RenderMesh 节点";
            return false;
        }
        std::string nodeId = health.value("node_id", "");
        if (!SafeId(nodeId)) {
            result = L"目标响应不是有效的 RenderMesh 节点";
            return false;
        }
        if (nodeId == config.nodeId) {
            result = L"该地址指向本机。请输入另一台电脑的局域网 IP，而不是 127.0.0.1";
            return false;
        }
        const int remoteProtocol = health.value("protocol", 1);
        if (remoteProtocol < kBlenderReadinessProtocol ||
            !health.contains("matching_id_binding")) {
            std::string remoteVersion = HeaderDisplayText(
                health.value("app_version", "旧版"), 32);
            result = L"连接被拒绝：对方 RenderMesh " + Utf8ToWide(remoteVersion) +
                L" 的握手协议过旧，请将两台电脑都更新到 " +
                Utf8ToWide(kRenderMeshAppVersion) + L" 或更高版本";
            return false;
        }
        if (config.pairingPending) {
            if (!health.value("pairing_available", false) ||
                (health.value("pairing_pending", false) && config.nodeId > nodeId)) {
                result = L"等待匹配 ID 相同的对方节点完成配对";
                return false;
            }
            if (!TryPair(address, port, nodeId, result)) {
                if (result.empty()) result = L"目标电脑暂时不能接受邀请码配对";
                return false;
            }
            config = owner->GetConfig();
        }
        std::string challenge = NewRandomHex(16);
        healthResponse = client.Get("/health?challenge=" + challenge);
        if (!healthResponse || healthResponse->status != 200) {
            result = L"目标未能返回经过验证的节点状态";
            return false;
        }
        health = json::parse(healthResponse->body, nullptr, false);
        if (health.is_discarded() || !health.is_object() ||
            health.value("node_id", "") != nodeId) {
            result = L"目标返回了无法识别的 RenderMesh 节点信息";
            return false;
        }
        if (remoteProtocol >= kDirectStatusProtocol) {
            const std::string directProof = health.value("direct_status_proof", "");
            health.erase("direct_status_proof");
            if (!health.value("direct_status", false) ||
                !ConstantTimeEqual(directProof, DirectStatusProof(
                    config, nodeId, challenge, health))) {
                result = L"连接被拒绝：节点实时状态未通过邀请码验证";
                return false;
            }
        }
        if (!ConstantTimeEqual(health.value("matching_id_binding", ""),
                               MatchingIdBinding(config))) {
            result = L"连接被拒绝：两台电脑的匹配 ID 不一致";
            return false;
        }
        WorkerActivity signedActivity;
        signedActivity.jobName = HeaderDisplayText(health.value("activity_job", ""), 128);
        signedActivity.frames = health.value("activity_frames", std::vector<int>{});
        if (signedActivity.frames.size() > 20) signedActivity.frames.resize(20);
        signedActivity.completedFrames = std::min(
            health.value("activity_completed", size_t{0}), signedActivity.frames.size());
        signedActivity.rendering = health.value("activity_rendering", false);
        std::string expectedActivityProof = ActivitySnapshotProof(
            config, challenge, nodeId, signedActivity);
        if (!ConstantTimeEqual(
                health.value("activity_proof", ""), expectedActivityProof)) {
            result = L"连接被拒绝：节点活动状态未通过邀请码验证";
            return false;
        }
        if (remoteProtocol >= 8 && !ConstantTimeEqual(
                health.value("job_cleanup_proof", ""), CleanupCapabilityProof(
                    config, challenge, nodeId, health.value("job_cleanup", false)))) {
            result = L"连接被拒绝：节点清理能力未通过邀请码验证";
            return false;
        }
        const bool remoteBlenderReady = health.value("blender_ready", false);
        if (!ConstantTimeEqual(
                health.value("blender_readiness_proof", ""), BlenderReadinessProof(
                    config, challenge, nodeId, remoteBlenderReady))) {
            result = L"连接被拒绝：节点 Blender 可用状态未通过邀请码验证";
            return false;
        }
        const std::string remoteAppVersion = HeaderDisplayText(
            health.value("app_version", ""), 32);
        const bool remotePeerExchange = health.value("peer_exchange", false);
        if (remoteProtocol >= kPeerExchangeProtocol && !ConstantTimeEqual(
                health.value("peer_exchange_proof", ""), PeerExchangeCapabilityProof(
                    config, challenge, nodeId, remoteProtocol, remoteAppVersion,
                    remotePeerExchange))) {
            result = L"连接被拒绝：节点交换能力未通过邀请码验证";
            return false;
        }
        auto proofResponse = client.Get("/v1/prove?challenge=" + challenge);
        if (!proofResponse || proofResponse->status != 200) {
            result = L"目标未能完成邀请码验证";
            return false;
        }
        json proof = json::parse(proofResponse->body, nullptr, false);
        std::string expected = HmacSha256(config.clusterToken,
            "worker-proof|" + nodeId + "|" + challenge);
        if (proof.is_discarded() || proof.value("node_id", "") != nodeId ||
            !ConstantTimeEqual(proof.value("proof", ""), expected)) {
            result = L"连接被拒绝：两台电脑的邀请码不一致";
            return false;
        }
        peer.nodeId = nodeId;
        peer.protocolVersion = health.value("protocol", 1);
        peer.supportsProjectCache = health.value("project_cache", false);
        peer.supportsFramePackages = health.value("frame_packages", false);
        peer.supportsResultBundles = health.value("result_bundles", false);
        peer.supportsPackageCancellation = health.value("package_cancel", false);
        peer.supportsJobCleanup = health.value("job_cleanup", false);
        peer.supportsResumableProjectUpload = health.value(
            "resumable_project_upload", false);
        peer.supportsPeerExchange = remoteProtocol >= kPeerExchangeProtocol &&
            remotePeerExchange;
        peer.supportsDirectStatus = remoteProtocol >= kDirectStatusProtocol &&
            health.value("direct_status", false);
        peer.supportsFrameStart = remoteProtocol >= kFrameStartProtocol &&
            health.value("frame_start", false);
        peer.rendererReady = remoteBlenderReady;
        peer.idleReadyKnown = remoteProtocol >= kIdleReadinessProtocol &&
            health.contains("idle_ready");
        peer.idleReady = peer.idleReadyKnown && health.value("idle_ready", false);
        peer.maxPackageFrames = std::clamp<size_t>(
            health.value("max_package_frames",
                         peer.protocolVersion >= 6 ? size_t{5} : size_t{1}),
            size_t{1}, size_t{20});
        peer.name = health.value("name", "");
        peer.appVersion = remoteAppVersion;
        if (peer.name.empty()) peer.name = "手动节点-" + nodeId.substr(0, 8);
        peer.address = address;
        peer.httpPort = port;
        peer.shareEnabled = health.value("share", false);
        peer.trusted = true;
        peer.cpuPercent = health.value("cpu", 15);
        peer.assigned = health.value("assigned", 0);
        peer.cpuModel = health.value("cpu_model", "");
        peer.gpuModels = health.value("gpu_models", std::vector<std::string>{});
        peer.currentTaskKnown = health.contains("task_id");
        peer.currentTaskId = health.value("task_id", "");
        peer.activity = std::move(signedActivity);
        peer.manualRoute = true;
        peer.manualValidatedAt = std::chrono::steady_clock::now();
        peer.idleSeconds = health.value("idle", 0.0);
        peer.lastSeen = std::chrono::steady_clock::now();
        result = L"已连接 " + Utf8ToWide(peer.name) + L"（" + Utf8ToWide(address) + L":" +
                 std::to_wstring(port) + L"）";
        return true;
        } catch (const std::exception&) {
            result = L"目标返回了无法识别的 RenderMesh 节点信息";
            return false;
        }
    }

    bool RefreshKnownPeer(const Peer& route, Peer& refreshed) {
        try {
            Config config = owner->GetConfig();
            if (config.pairingPending || config.clusterToken.size() < 16 ||
                !route.trusted || route.address.empty() ||
                route.httpPort < 1024 || route.httpPort > 65535)
                return false;
            const std::string challenge = NewRandomHex(16);
            http::Client client(route.address, route.httpPort);
            client.set_connection_timeout(0, 500000);
            client.set_read_timeout(1, 0);
            client.set_write_timeout(1, 0);
            client.set_max_timeout(1500);
            auto response = client.Get("/health?challenge=" + challenge);
            if (!response || response->status != 200) return false;
            json health = json::parse(response->body, nullptr, false);
            if (health.is_discarded() || !health.is_object() ||
                health.value("node_id", "") != route.nodeId ||
                health.value("protocol", 0) < kDirectStatusProtocol ||
                !health.value("direct_status", false))
                return false;
            const std::string proof = health.value("direct_status_proof", "");
            health.erase("direct_status_proof");
            if (!ConstantTimeEqual(proof, DirectStatusProof(
                    config, route.nodeId, challenge, health)) ||
                !ConstantTimeEqual(health.value("matching_id_binding", ""),
                                   MatchingIdBinding(config)))
                return false;

            refreshed = route;
            refreshed.protocolVersion = health.value("protocol", route.protocolVersion);
            refreshed.appVersion = HeaderDisplayText(health.value("app_version", ""), 32);
            refreshed.supportsProjectCache = health.value("project_cache", false);
            refreshed.supportsFramePackages = health.value("frame_packages", false);
            refreshed.supportsResultBundles = health.value("result_bundles", false);
            refreshed.supportsPackageCancellation = health.value("package_cancel", false);
            refreshed.supportsJobCleanup = health.value("job_cleanup", false);
            refreshed.supportsResumableProjectUpload = health.value(
                "resumable_project_upload", false);
            refreshed.supportsPeerExchange = health.value("peer_exchange", false);
            refreshed.supportsDirectStatus = true;
            refreshed.supportsFrameStart =
                refreshed.protocolVersion >= kFrameStartProtocol &&
                health.value("frame_start", false);
            refreshed.rendererReady = health.value("blender_ready", false);
            refreshed.idleReadyKnown =
                refreshed.protocolVersion >= kIdleReadinessProtocol &&
                health.contains("idle_ready");
            refreshed.idleReady = refreshed.idleReadyKnown &&
                health.value("idle_ready", false);
            refreshed.maxPackageFrames = std::clamp<size_t>(
                health.value("max_package_frames", size_t{1}), size_t{1}, size_t{20});
            refreshed.name = health.value("name", route.name);
            refreshed.shareEnabled = health.value("share", false);
            refreshed.cpuPercent = health.value("cpu", 0);
            refreshed.assigned = health.value("assigned", 0);
            refreshed.cpuModel = health.value("cpu_model", "");
            refreshed.gpuModels = health.value("gpu_models", std::vector<std::string>{});
            refreshed.currentTaskKnown = health.contains("task_id");
            refreshed.currentTaskId = health.value("task_id", "");
            refreshed.activity.jobName = HeaderDisplayText(
                health.value("activity_job", ""), 128);
            refreshed.activity.frames = health.value(
                "activity_frames", std::vector<int>{});
            if (refreshed.activity.frames.size() > 20)
                refreshed.activity.frames.resize(20);
            refreshed.activity.completedFrames = std::min(
                health.value("activity_completed", size_t{0}),
                refreshed.activity.frames.size());
            refreshed.activity.rendering = health.value("activity_rendering", false);
            refreshed.idleSeconds = health.value("idle", 0.0);
            refreshed.lastSeen = std::chrono::steady_clock::now();
            refreshed.trusted = true;
            return true;
        } catch (...) {
            return false;
        }
    }

    void ManualPeerLoop() {
        while (!stopRequested.load()) {
            std::string endpoint = owner->GetConfig().manualPeerEndpoint;
            bool active = false;
            if (!endpoint.empty()) {
                Peer peer;
                std::wstring status;
                if (ProbeManualPeer(endpoint, peer, status)) {
                    active = peer.activity.Active();
                    bool committed = false;
                    {
                        std::scoped_lock lock(stateMutex);
                        if (owner->GetConfig().manualPeerEndpoint == endpoint) {
                            peers[peer.nodeId] = std::move(peer);
                            committed = true;
                        }
                    }
                    if (!committed) continue;
                }
                if (owner->GetConfig().manualPeerEndpoint != endpoint) continue;
                SetManualStatus(std::move(status));
                Notify();
            }
            const int delaySteps = active ? 3 : 10;
            for (int step = 0; step < delaySteps && !stopRequested.load(); ++step) Sleep(100);
        }
    }

    void PeerExchangeLoop() {
        size_t pollCursor = 0;
        size_t directoryCursor = 0;
        while (!stopRequested.load()) {
            Config config = owner->GetConfig();
            std::optional<Peer> seed;
            std::vector<Peer> directRoutes;
            if (!config.pairingPending && config.clusterToken.size() >= 16) {
                std::scoped_lock lock(stateMutex);
                const auto now = std::chrono::steady_clock::now();
                for (const auto& [nodeId, peer] : peers) {
                    if (!peer.trusted || now - peer.lastSeen > kPeerTimeout) continue;
                    if (peer.supportsPeerExchange &&
                        (!seed || peer.nodeId < seed->nodeId))
                        seed = peer;
                    // UDP broadcast/multicast already carries fresh signed state.
                    // Routes learned through a manual address or peer directory must
                    // be polled at the endpoint itself; a third party directory is
                    // never allowed to refresh liveness or task state.
                    if (peer.manualRoute) directRoutes.push_back(peer);
                }
            }
            bool changed = false;
            std::sort(directRoutes.begin(), directRoutes.end(),
                      [](const Peer& left, const Peer& right) {
                          return left.nodeId < right.nodeId;
                      });
            if (!directRoutes.empty()) {
                const size_t count = std::min<size_t>(8, directRoutes.size());
                for (size_t offset = 0; offset < count && !stopRequested.load(); ++offset) {
                    const Peer& route = directRoutes[(pollCursor + offset) % directRoutes.size()];
                    Peer refreshed;
                    bool ok = route.supportsDirectStatus
                        ? RefreshKnownPeer(route, refreshed)
                        : [&] {
                            std::wstring ignored;
                            return ProbeManualPeer(
                                route.address + ":" + std::to_string(route.httpPort),
                                refreshed, ignored);
                        }();
                    if (!ok || refreshed.nodeId != route.nodeId) continue;
                    Config current = owner->GetConfig();
                    if (current.pairingPending ||
                        current.clusterToken != config.clusterToken)
                        break;
                    std::scoped_lock lock(stateMutex);
                    auto existing = peers.find(route.nodeId);
                    if (existing == peers.end() ||
                        existing->second.address != route.address ||
                        existing->second.httpPort != route.httpPort)
                        continue;
                    refreshed.manualRoute = true;
                    peers[route.nodeId] = std::move(refreshed);
                    changed = true;
                }
                pollCursor = (pollCursor + count) % directRoutes.size();
            }
            if (seed) {
                const std::string challenge = NewRandomHex(16);
                http::Client client(seed->address, seed->httpPort);
                client.set_connection_timeout(2, 0);
                client.set_read_timeout(2, 0);
                client.set_write_timeout(2, 0);
                client.set_max_timeout(4000);
                http::Headers headers = {{"X-RM-Token", config.clusterToken}};
                auto response = client.Get("/v1/peers?challenge=" + challenge, headers);
                if (response && response->status == 200) {
                    json body = json::parse(response->body, nullptr, false);
                    if (body.is_discarded() || !body.is_object()) {
                        for (int step = 0; step < 10 && !stopRequested.load(); ++step)
                            Sleep(100);
                        continue;
                    }
                    json directory = body.value("peers", json::array());
                    if (directory.is_array() &&
                        directory.size() <= 128 && body.value("node_id", "") == seed->nodeId &&
                        body.value("challenge", "") == challenge &&
                        ConstantTimeEqual(body.value("proof", ""), PeerDirectoryProof(
                            config, seed->nodeId, challenge, directory))) {
                        const size_t count = std::min<size_t>(2, directory.size());
                        for (size_t offset = 0; offset < count; ++offset) {
                            const auto& row = directory[
                                (directoryCursor + offset) % directory.size()];
                            if (stopRequested.load() || !row.is_object()) break;
                            const std::string nodeId = row.value("node_id", "");
                            const std::string address = row.value("address", "");
                            const int port = row.value("port", 0);
                            std::string parsedAddress;
                            int parsedPort = 0;
                            if (!SafeId(nodeId) || nodeId == config.nodeId ||
                                !ParseIPv4Endpoint(address + ":" + std::to_string(port),
                                                   parsedAddress, parsedPort))
                                continue;
                            bool alreadyKnown = false;
                            {
                                std::scoped_lock lock(stateMutex);
                                auto existing = peers.find(nodeId);
                                alreadyKnown = existing != peers.end() &&
                                    existing->second.trusted;
                            }
                            if (alreadyKnown) continue;
                            Peer introduced;
                            std::wstring ignoredStatus;
                            if (!ProbeManualPeer(
                                    parsedAddress + ":" + std::to_string(parsedPort),
                                    introduced, ignoredStatus))
                                continue;
                            if (introduced.nodeId != nodeId) continue;
                            Config current = owner->GetConfig();
                            if (current.pairingPending ||
                                current.clusterToken != config.clusterToken)
                                break;
                            {
                                std::scoped_lock lock(stateMutex);
                                peers[introduced.nodeId] = introduced;
                            }
                            changed = true;
                            LogInfo("authenticated peer introduced by " + seed->nodeId +
                                    "; peer=" + introduced.nodeId);
                        }
                        if (!directory.empty())
                            directoryCursor = (directoryCursor + count) % directory.size();
                    }
                }
            }
            if (changed) Notify();
            for (int step = 0; step < 10 && !stopRequested.load(); ++step) Sleep(100);
        }
    }

    void DiscoveryLoop() {
        Config initial = owner->GetConfig();
        SOCKET activeSocket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (activeSocket == INVALID_SOCKET) return;
        discoverySocket.store(activeSocket);
        struct SocketOwner {
            SOCKET value;
            ~SocketOwner() { if (value != INVALID_SOCKET) closesocket(value); }
        } socketOwner{activeSocket};
        BOOL enabled = TRUE;
        setsockopt(activeSocket, SOL_SOCKET, SO_BROADCAST,
                   reinterpret_cast<const char*>(&enabled), sizeof(enabled));
        setsockopt(activeSocket, SOL_SOCKET, SO_REUSEADDR,
                   reinterpret_cast<const char*>(&enabled), sizeof(enabled));
        DWORD timeout = 400;
        setsockopt(activeSocket, SOL_SOCKET, SO_RCVTIMEO,
                   reinterpret_cast<const char*>(&timeout), sizeof(timeout));
        sockaddr_in local{};
        local.sin_family = AF_INET;
        local.sin_addr.s_addr = INADDR_ANY;
        local.sin_port = htons(static_cast<u_short>(initial.discoveryPort));
        if (bind(activeSocket, reinterpret_cast<sockaddr*>(&local), sizeof(local)) == SOCKET_ERROR) {
            discoverySocket.store(INVALID_SOCKET);
            return;
        }
        in_addr multicastAddress{};
        inet_pton(AF_INET, kDiscoveryMulticastGroup, &multicastAddress);
        for (const auto& network : LocalIPv4Interfaces()) {
            ip_mreq membership{};
            membership.imr_multiaddr = multicastAddress;
            inet_pton(AF_INET, network.address.c_str(), &membership.imr_interface);
            setsockopt(activeSocket, IPPROTO_IP, IP_ADD_MEMBERSHIP,
                       reinterpret_cast<const char*>(&membership), sizeof(membership));
        }

        auto nextBroadcast = std::chrono::steady_clock::now();
        while (!stopRequested.load()) {
            Config config = owner->GetConfig();
            if (std::chrono::steady_clock::now() >= nextBroadcast) {
                auto networks = LocalIPv4Interfaces();
                std::vector<std::string> localAddresses;
                for (const auto& network : networks)
                    localAddresses.push_back(network.address);
                std::vector<std::string> payloads;
                // A fresh install does not yet share the internal cluster credential,
                // so it cannot emit or authenticate the normal discovery beacon. This
                // deliberately untrusted candidate beacon only provides an endpoint
                // for the rate-limited six-digit pairing exchange below. It never
                // enters the peer/task pool by itself.
                if (IsInviteCode(config.inviteCode) && config.clusterToken.size() >= 16) {
                    json candidate = {
                        {"kind", "pairing_candidate"},
                        {"protocol", kProtocolVersion},
                        {"app_version", kRenderMeshAppVersion},
                        {"node_id", config.nodeId}, {"nonce", NewRandomHex(8)},
                        {"addresses", localAddresses}, {"port", config.httpPort},
                        {"pairing_pending", config.pairingPending},
                        {"pairing_tag", PairingDiscoveryTag(config)},
                    };
                    payloads.push_back(candidate.dump());
                }
                if (!config.pairingPending && config.clusterToken.size() >= 16) {
                    WorkerActivity activity = CurrentActivity();
                    const bool rendererReady = RendererReady(config);
                    const double idleSeconds = IdleSeconds();
                    const bool idleReady = owner->mockRenderer_ ||
                        IsComputerIdleForSharedWork(idleSeconds, config.idleAfterSeconds);
                    json messageV7 = {
                        {"protocol", kProtocolVersion},
                        {"app_version", kRenderMeshAppVersion},
                        {"project_cache", true}, {"frame_packages", true},
                        {"result_bundles", true}, {"package_cancel", true},
                        {"job_cleanup", true},
                        {"resumable_project_upload", true}, {"max_package_frames", 20},
                         {"peer_exchange", true}, {"direct_status", true},
                         {"frame_start", true},
                         {"idle_ready", idleReady},
                        {"matching_id_binding", MatchingIdBinding(config)},
                        {"blender_ready", rendererReady},
                        {"node_id", config.nodeId}, {"name", config.nodeName},
                        {"nonce", NewRandomHex(8)}, {"addresses", localAddresses},
                        {"port", config.httpPort}, {"share", config.shareEnabled},
                        {"cpu", 100}, {"assigned", workerAssigned.load()},
                        {"cpu_model", hardware.cpuModel}, {"gpu_models", hardware.gpuModels},
                        {"task_id", CurrentTaskId()},
                        {"activity_job", HeaderDisplayText(activity.jobName, 128)},
                        {"activity_frames", activity.frames},
                        {"activity_completed", activity.completedFrames},
                        {"activity_rendering", activity.rendering},
                        {"idle", static_cast<int>(idleSeconds)},
                    };
                    messageV7["signature"] = DiscoverySignature(config, messageV7);
                    payloads.push_back(messageV7.dump());
                    // Protocol 11 nodes reject a higher protocol number before
                    // signature verification. Keep a fully authenticated v11
                    // compatibility beacon during rolling upgrades.
                    json messageV11 = messageV7;
                    messageV11["protocol"] = 11;
                    messageV11["nonce"] = NewRandomHex(8);
                    messageV11.erase("peer_exchange");
                    messageV11.erase("direct_status");
                    messageV11.erase("idle_ready");
                    messageV11.erase("frame_start");
                    messageV11.erase("signature");
                    messageV11["signature"] = DiscoverySignature(config, messageV11);
                    payloads.push_back(messageV11.dump());
                }
                for (const auto& network : BroadcastCapableInterfaces(networks)) {
                    SOCKET sendSocket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
                    if (sendSocket == INVALID_SOCKET) continue;
                    setsockopt(sendSocket, SOL_SOCKET, SO_BROADCAST,
                               reinterpret_cast<const char*>(&enabled), sizeof(enabled));
                    sockaddr_in source{};
                    source.sin_family = AF_INET;
                    inet_pton(AF_INET, network.address.c_str(), &source.sin_addr);
                    source.sin_port = 0;
                    if (bind(sendSocket, reinterpret_cast<sockaddr*>(&source), sizeof(source)) ==
                        SOCKET_ERROR) {
                        closesocket(sendSocket);
                        continue;
                    }
                    sockaddr_in broadcast{};
                    broadcast.sin_family = AF_INET;
                    inet_pton(AF_INET, network.broadcast->c_str(), &broadcast.sin_addr);
                    broadcast.sin_port = htons(static_cast<u_short>(config.discoveryPort));
                    for (const auto& payload : payloads)
                        sendto(sendSocket, payload.data(), static_cast<int>(payload.size()), 0,
                               reinterpret_cast<sockaddr*>(&broadcast), sizeof(broadcast));
                    closesocket(sendSocket);
                }
                // Multicast is a second, fully decentralised discovery path for
                // office networks that suppress directed broadcast. Every node
                // announces itself directly; no coordinator or directory is needed.
                for (const auto& network : networks) {
                    SOCKET sendSocket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
                    if (sendSocket == INVALID_SOCKET) continue;
                    in_addr interfaceAddress{};
                    inet_pton(AF_INET, network.address.c_str(), &interfaceAddress);
                    setsockopt(sendSocket, IPPROTO_IP, IP_MULTICAST_IF,
                               reinterpret_cast<const char*>(&interfaceAddress),
                               sizeof(interfaceAddress));
                    DWORD ttl = 1;
                    setsockopt(sendSocket, IPPROTO_IP, IP_MULTICAST_TTL,
                               reinterpret_cast<const char*>(&ttl), sizeof(ttl));
                    sockaddr_in source{};
                    source.sin_family = AF_INET;
                    source.sin_addr = interfaceAddress;
                    source.sin_port = 0;
                    if (bind(sendSocket, reinterpret_cast<sockaddr*>(&source), sizeof(source)) ==
                        SOCKET_ERROR) {
                        closesocket(sendSocket);
                        continue;
                    }
                    sockaddr_in multicast{};
                    multicast.sin_family = AF_INET;
                    multicast.sin_addr = multicastAddress;
                    multicast.sin_port = htons(static_cast<u_short>(config.discoveryPort));
                    for (const auto& payload : payloads)
                        sendto(sendSocket, payload.data(), static_cast<int>(payload.size()), 0,
                               reinterpret_cast<sockaddr*>(&multicast), sizeof(multicast));
                    closesocket(sendSocket);
                }
                // Directory-learned peers may sit on a network where broadcast
                // and multicast are filtered. Send the same signed beacon
                // directly to every trusted route so already-deployed clients
                // receive the full protocol/app-version metadata and can perform
                // rolling LAN updates without a coordinator.
                std::vector<std::string> trustedRoutes;
                {
                    std::scoped_lock lock(stateMutex);
                    const auto now = std::chrono::steady_clock::now();
                    for (const auto& [nodeId, peer] : peers) {
                        in_addr parsed{};
                        if (peer.trusted && now - peer.lastSeen <= kPeerTimeout &&
                            !peer.address.empty() &&
                            inet_pton(AF_INET, peer.address.c_str(), &parsed) == 1)
                            trustedRoutes.push_back(peer.address);
                    }
                }
                std::sort(trustedRoutes.begin(), trustedRoutes.end());
                trustedRoutes.erase(
                    std::unique(trustedRoutes.begin(), trustedRoutes.end()),
                    trustedRoutes.end());
                for (const auto& address : trustedRoutes) {
                    SOCKET sendSocket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
                    if (sendSocket == INVALID_SOCKET) continue;
                    sockaddr_in destination{};
                    destination.sin_family = AF_INET;
                    destination.sin_port = htons(
                        static_cast<u_short>(config.discoveryPort));
                    if (inet_pton(AF_INET, address.c_str(),
                                  &destination.sin_addr) == 1) {
                        for (const auto& payload : payloads)
                            sendto(sendSocket, payload.data(),
                                   static_cast<int>(payload.size()), 0,
                                   reinterpret_cast<sockaddr*>(&destination),
                                   sizeof(destination));
                    }
                    closesocket(sendSocket);
                }
                nextBroadcast = std::chrono::steady_clock::now() + std::chrono::seconds(2);
            }
            char buffer[8192];
            sockaddr_in remote{};
            int remoteSize = sizeof(remote);
            int received = recvfrom(activeSocket, buffer, sizeof(buffer), 0,
                                    reinterpret_cast<sockaddr*>(&remote), &remoteSize);
            if (received <= 0) continue;
            try {
                json message = json::parse(std::string(buffer, received));
                int protocol = message.value("protocol", 0);
                char remoteAddress[INET_ADDRSTRLEN]{};
                inet_ntop(AF_INET, &remote.sin_addr, remoteAddress, sizeof(remoteAddress));
                if (message.value("kind", "") == "pairing_candidate") {
                    Config pending = owner->GetConfig();
                    const std::string remoteNodeId = message.value("node_id", "");
                    const int remotePort = message.value("port", 0);
                    const bool remotePending = message.value("pairing_pending", true);
                    const auto advertisedAddresses = message.value(
                        "addresses", std::vector<std::string>{});
                    if (protocol < kBlenderReadinessProtocol ||
                        protocol > kProtocolVersion || !pending.pairingPending ||
                        !SafeId(remoteNodeId) || remoteNodeId == pending.nodeId ||
                        remotePort < 1024 || remotePort > 65535 ||
                        std::find(advertisedAddresses.begin(), advertisedAddresses.end(),
                                  remoteAddress) == advertisedAddresses.end() ||
                        !ConstantTimeEqual(message.value("pairing_tag", ""),
                                           PairingDiscoveryTag(pending)) ||
                        (remotePending && protocol == kProtocolVersion &&
                         pending.nodeId > remoteNodeId))
                        continue;
                    std::wstring pairingResult;
                    if (TryPair(remoteAddress, remotePort, remoteNodeId, pairingResult)) {
                        SetManualStatus(pairingResult + L"；已自动发现 " +
                            Utf8ToWide(std::string(remoteAddress)) + L":" +
                            std::to_wstring(remotePort));
                        Notify();
                    }
                    continue;
                }
                if (protocol < kBlenderReadinessProtocol ||
                    protocol > kProtocolVersion)
                    continue;
                if (!ConstantTimeEqual(message.value("matching_id_binding", ""),
                                       MatchingIdBinding(config)))
                    continue;
                Peer peer;
                peer.protocolVersion = protocol;
                peer.supportsProjectCache = message.value("project_cache", false);
                peer.supportsFramePackages = message.value("frame_packages", false);
                peer.supportsResultBundles = message.value("result_bundles", false);
                peer.supportsPackageCancellation = message.value("package_cancel", false);
                peer.supportsJobCleanup = message.value("job_cleanup", false);
                peer.supportsResumableProjectUpload = message.value(
                    "resumable_project_upload", false);
                peer.supportsPeerExchange = protocol >= kPeerExchangeProtocol &&
                    message.value("peer_exchange", false);
                peer.supportsDirectStatus = protocol >= kDirectStatusProtocol &&
                    message.value("direct_status", false);
                peer.supportsFrameStart = protocol >= kFrameStartProtocol &&
                    message.value("frame_start", false);
                peer.rendererReady = message.value("blender_ready", false);
                peer.idleReadyKnown = protocol >= kIdleReadinessProtocol &&
                    message.contains("idle_ready");
                peer.idleReady = peer.idleReadyKnown &&
                    message.value("idle_ready", false);
                peer.maxPackageFrames = std::clamp<size_t>(
                    message.value("max_package_frames",
                                  protocol >= 6 ? size_t{5} : size_t{1}),
                    size_t{1}, size_t{20});
                peer.nodeId = message.value("node_id", "");
                if (!SafeId(peer.nodeId) || peer.nodeId == config.nodeId) continue;
                peer.address = remoteAddress;
                peer.name = message.value("name", peer.nodeId);
                peer.appVersion = HeaderDisplayText(message.value("app_version", ""), 32);
                peer.httpPort = message.value("port", 8391);
                peer.shareEnabled = message.value("share", false);
                peer.cpuPercent = message.value("cpu", 15);
                peer.assigned = message.value("assigned", 0);
                peer.cpuModel = message.value("cpu_model", "");
                peer.gpuModels = message.value("gpu_models", std::vector<std::string>{});
                peer.currentTaskId = message.value("task_id", "");
                peer.currentTaskKnown = true;
                if (protocol >= 7) {
                    peer.activity.jobName = HeaderDisplayText(
                        message.value("activity_job", ""), 128);
                    peer.activity.frames = message.value(
                        "activity_frames", std::vector<int>{});
                    if (peer.activity.frames.size() > 20) peer.activity.frames.resize(20);
                    peer.activity.completedFrames = std::min(
                        message.value("activity_completed", size_t{0}),
                        peer.activity.frames.size());
                    peer.activity.rendering = message.value("activity_rendering", false);
                }
                peer.idleSeconds = message.value("idle", 0.0);
                std::string claimedSignature = message.value("signature", "");
                auto signedAddresses = message.value("addresses", std::vector<std::string>{});
                peer.trusted = claimedSignature.size() == 64 &&
                    std::find(signedAddresses.begin(), signedAddresses.end(), peer.address) !=
                        signedAddresses.end() &&
                    ConstantTimeEqual(claimedSignature, DiscoverySignature(config, message));
                // Never let an invalid datagram replace a previously authenticated
                // endpoint for the same node ID.
                if (!peer.trusted) continue;
                peer.lastSeen = std::chrono::steady_clock::now();
                {
                    std::scoped_lock lock(stateMutex);
                    auto existing = peers.find(peer.nodeId);
                    std::string configuredEndpoint = config.manualPeerEndpoint;
                    std::string existingEndpoint = existing == peers.end() ? "" :
                        existing->second.address + ":" +
                        std::to_string(existing->second.httpPort);
                    bool manualRouteStillValid = existing != peers.end() &&
                        existing->second.manualRoute &&
                        configuredEndpoint == existingEndpoint &&
                        std::chrono::steady_clock::now() -
                            existing->second.manualValidatedAt <= kPeerTimeout;
                    if (existing != peers.end() &&
                        IsSameVersionCompatibilityBeacon(peer, existing->second)) {
                        PreserveAuthenticatedV2Metadata(peer, existing->second);
                    }
                    if (manualRouteStillValid) {
                        peer.address = existing->second.address;
                        peer.httpPort = existing->second.httpPort;
                        peer.manualRoute = true;
                        peer.manualValidatedAt = existing->second.manualValidatedAt;
                    }
                    peers[peer.nodeId] = std::move(peer);
                }
                Notify();
            } catch (...) {
            }
        }
        discoverySocket.store(INVALID_SOCKET);
    }

    std::optional<Peer> AvailablePeer(const std::string& jobId = {}) {
        std::scoped_lock lock(stateMutex);
        auto now = std::chrono::steady_clock::now();
        const RenderJob* targetJob = nullptr;
        if (!jobId.empty()) {
            auto found = std::find_if(jobs.begin(), jobs.end(), [&](const RenderJob& job) {
                return job.id == jobId;
            });
            if (found != jobs.end()) targetJob = &*found;
        }
        for (const auto& [id, peer] : peers) {
            if (!jobId.empty() && rejectedJobPeers.contains({jobId, peer.nodeId})) continue;
            auto backoff = jobPeerBackoff.find({jobId, peer.nodeId});
            if (!jobId.empty() && backoff != jobPeerBackoff.end() && now < backoff->second)
                continue;
            if (targetJob && !targetJob->projectTransfers.empty()) {
                auto participant = std::find_if(
                    targetJob->projectTransfers.begin(), targetJob->projectTransfers.end(),
                    [&](const ProjectTransfer& transfer) {
                        return transfer.nodeId == peer.nodeId &&
                            (transfer.state == ProjectTransferState::Completed ||
                             transfer.state == ProjectTransferState::Cached);
                    });
                if (participant == targetJob->projectTransfers.end()) continue;
            }
            bool locallyLeased = std::any_of(jobs.begin(), jobs.end(), [&](const RenderJob& job) {
                return std::any_of(job.frames.begin(), job.frames.end(),
                    [&](const FrameTask& frame) {
                        return IsActiveFrameLease(frame.state) &&
                            frame.assignedNodeId == peer.nodeId;
                    });
            });
            if (now - peer.lastSeen <= kPeerTimeout && peer.trusted && peer.shareEnabled &&
                peer.rendererReady &&
                peer.assigned == 0 && !locallyLeased &&
                (!peer.idleReadyKnown || peer.idleReady))
                return peer;
        }
        return std::nullopt;
    }

    bool HasNonRejectedProjectParticipant(const std::string& jobId) {
        std::scoped_lock lock(stateMutex);
        const auto job = std::find_if(jobs.begin(), jobs.end(), [&](const RenderJob& candidate) {
            return candidate.id == jobId;
        });
        if (job != jobs.end()) {
            if (job->projectTransfers.empty()) return false;
            return std::any_of(
                job->projectTransfers.begin(), job->projectTransfers.end(),
                [&](const ProjectTransfer& transfer) {
                    const bool ready = transfer.state == ProjectTransferState::Completed ||
                                       transfer.state == ProjectTransferState::Cached;
                    return ready && !rejectedJobPeers.contains({jobId, transfer.nodeId});
                });
        }
        return false;
    }

    bool VerifyRemotePeer(const Peer& peer, const Config& config, std::string& error,
                          bool requireBlender = true) {
        if (peer.protocolVersion < kBlenderReadinessProtocol) {
            error = "permanent: worker handshake protocol is too old";
            return false;
        }
        if (requireBlender && !peer.rendererReady) {
            error = "permanent: worker has no available Blender executable";
            return false;
        }
        std::string lastFailure = "no response";
        constexpr int kOwnershipProofAttempts = 3;
        for (int attempt = 0; attempt < kOwnershipProofAttempts; ++attempt) {
            const std::string challenge = NewRandomHex(16);
            http::Client verifier(peer.address, peer.httpPort);
            verifier.set_connection_timeout(3, 0);
            verifier.set_read_timeout(5, 0);
            verifier.set_write_timeout(5, 0);
            verifier.set_max_timeout(10000);
            auto proofResponse = verifier.Get("/v1/prove?challenge=" + challenge);
            if (proofResponse && proofResponse->status == 200) {
                json proof = json::parse(proofResponse->body, nullptr, false);
                const std::string expectedProof = HmacSha256(
                    config.clusterToken,
                    "worker-proof|" + peer.nodeId + "|" + challenge);
                if (proof.is_discarded() || proof.value("node_id", "") != peer.nodeId ||
                    !ConstantTimeEqual(proof.value("matching_id_binding", ""),
                                       MatchingIdBinding(config)) ||
                    !ConstantTimeEqual(proof.value("proof", ""), expectedProof)) {
                    error = "permanent: worker ownership proof was invalid";
                    return false;
                }
                return true;
            }
            lastFailure = proofResponse
                ? "HTTP " + std::to_string(proofResponse->status)
                : http::to_string(proofResponse.error());
            if (attempt + 1 < kOwnershipProofAttempts)
                std::this_thread::sleep_for(
                    std::chrono::milliseconds(120 * (attempt + 1)));
        }
        error = "retry: worker failed fresh ownership proof after " +
            std::to_string(kOwnershipProofAttempts) + " attempts (" +
            lastFailure + ")";
        return false;
    }

    bool EnsureRemoteProjectCache(
        const Peer& peer, const Config& config, const RenderJob& job, std::string& error,
        std::atomic<bool>* jobCancelled) {
        if (!SafeSha256(job.projectHash)) return true;
        if (CancellationRequested(&stopRequested, jobCancelled)) {
            error = "job was stopped";
            return false;
        }
        return UploadProjectResumable(
            peer, config, job,
            [](ProjectTransferState, uint64_t, double, const std::string&) {},
            error, jobCancelled);
    }

    bool UploadProjectResumable(
        const Peer& peer, const Config& config, const RenderJob& job,
        const std::function<void(ProjectTransferState, uint64_t, double,
                                 const std::string&)>& progress,
        std::string& error, std::atomic<bool>* jobCancelled) {
        File input = File::OpenRead(ProjectPayloadPath(job));
        const uint64_t total = input ? input.Size() : 0;
        if (!total || total > kMaxTransferBytes) {
            error = "project size is invalid";
            return false;
        }
        // One keep-alive connection carries the status query, every chunk and
        // the commit, so a large project does not pay a TCP handshake and slow
        // start per 8 MiB chunk.
        http::Client client(peer.address, peer.httpPort);
        ConfigureClient(client, 5, &stopRequested, jobCancelled);
        client.set_keep_alive(true);
        const http::Headers common = {
            {"X-RM-Token", config.clusterToken}, {"X-RM-Project", job.projectHash},
            {"X-RM-Total", std::to_string(total)}, {"X-RM-Blender", job.softwareVersion},
            {"X-RM-Engine", job.renderEngine},
            {"X-RM-Requires-GPU", job.requiresGpu ? "1" : "0"},
            {"X-RM-Project-Bundle", job.projectBundle ? "1" : "0"},
        };
        progress(ProjectTransferState::Checking, 0, 0, {});
        auto status = client.Get("/v3/cache/upload-status?hash=" + job.projectHash, common);
        if (!status) {
            error = "retry: upload status query failed: " + http::to_string(status.error());
            return false;
        }
        if (status->status != 200) {
            error = (status->status >= 500 ? "retry: " : "") +
                (status->body.empty() ? "upload status query rejected" : status->body);
            return false;
        }
        bool cached = false;
        uint64_t offset = 0;
        std::string statusError;
        if (!ParseProjectUploadStatus(status->body, cached, offset, statusError)) {
            error = "retry: " + statusError;
            return false;
        }
        if (cached) {
            progress(ProjectTransferState::Cached, total, 0, {});
            return true;
        }
        if (offset > total) {
            error = "remote upload offset exceeds project size";
            return false;
        }
        client.set_read_timeout(kProjectChunkTimeoutSeconds);
        client.set_write_timeout(kProjectChunkTimeoutSeconds);
        std::string chunk;
        auto sampleAt = std::chrono::steady_clock::now();
        uint64_t sampleBytes = offset;
        progress(ProjectTransferState::Uploading, offset, 0, {});
        while (offset < total) {
            if (CancellationRequested(&stopRequested, jobCancelled)) {
                error = "retry: project upload was cancelled";
                return false;
            }
            chunk.resize(static_cast<size_t>(std::min<uint64_t>(
                kProjectUploadChunkBytes, total - offset)));
            if (!input.Seek(offset) || !input.ReadExact(chunk.data(), chunk.size())) {
                error = "project file changed or became unreadable during upload";
                return false;
            }
            http::Headers headers = common;
            headers.emplace("X-RM-Offset", std::to_string(offset));
            headers.emplace("X-RM-Chunk-SHA256", Sha256Prefix(chunk, 64));
            auto response = client.Post("/v3/cache/chunk", headers, chunk,
                                        "application/octet-stream");
            if (!response) {
                error = "retry: project chunk upload failed: " +
                    http::to_string(response.error());
                return false;
            }
            if (response->status == 409) {
                json conflict = json::parse(response->body, nullptr, false);
                if (conflict.is_discarded() || !conflict.contains("offset") ||
                    !conflict["offset"].is_number_unsigned() ||
                    conflict["offset"].get<uint64_t>() > total) {
                    error = "retry: remote upload offset could not be recovered";
                    return false;
                }
                offset = conflict["offset"].get<uint64_t>();
                continue;
            }
            if (response->status != 200) {
                error = (response->status >= 500 ? "retry: " : "") +
                    (response->body.empty() ? "project chunk was rejected" : response->body);
                return false;
            }
            offset += chunk.size();
            const auto now = std::chrono::steady_clock::now();
            const double seconds = std::chrono::duration<double>(now - sampleAt).count();
            const double rate = seconds > 0.05 ? (offset - sampleBytes) / seconds : 0;
            if (seconds > 0.25) {
                sampleAt = now;
                sampleBytes = offset;
            }
            progress(ProjectTransferState::Uploading, offset, rate, {});
        }
        client.set_read_timeout(kProjectCommitTimeoutSeconds);
        auto commit = client.Post("/v3/cache/commit", common, std::string{},
                                  "application/octet-stream");
        if (!commit || (commit->status != 200 && commit->status != 201)) {
            if (!commit) error = "retry: project commit failed: " +
                http::to_string(commit.error());
            else error = (commit->status == 425 || commit->status >= 500
                    ? "retry: " : "") +
                (commit->body.empty() ? "project commit was rejected" : commit->body);
            return false;
        }
        progress(ProjectTransferState::Completed, total, 0, {});
        return true;
    }

    void UpdateProjectTransfer(
        const std::string& jobId, const std::string& nodeId,
        ProjectTransferState state, uint64_t transferred, double rate,
        const std::string& error) {
        std::scoped_lock lock(stateMutex);
        auto job = std::find_if(jobs.begin(), jobs.end(),
            [&](const RenderJob& value) { return value.id == jobId; });
        if (job == jobs.end()) return;
        auto transfer = std::find_if(job->projectTransfers.begin(), job->projectTransfers.end(),
            [&](const ProjectTransfer& value) { return value.nodeId == nodeId; });
        if (transfer == job->projectTransfers.end() ||
            transfer->state == ProjectTransferState::Excluded)
            return;
        transfer->state = state;
        transfer->transferredBytes = transferred;
        transfer->bytesPerSecond = rate;
        transfer->error = error;
    }

    void RefreshUploadStageLocked(RenderJob& job) {
        if (job.stage != JobStage::Uploading && job.stage != JobStage::UploadFailed) return;
        size_t participants = 0;
        bool allReady = true;
        bool anyFailed = false;
        for (const auto& transfer : job.projectTransfers) {
            if (transfer.state == ProjectTransferState::Excluded) continue;
            ++participants;
            allReady = allReady && (transfer.state == ProjectTransferState::Completed ||
                                     transfer.state == ProjectTransferState::Cached);
            anyFailed = anyFailed || transfer.state == ProjectTransferState::Failed ||
                transfer.state == ProjectTransferState::Offline;
        }
        if (participants == 0 || allReady) job.stage = JobStage::ReadyToConfigure;
        else if (anyFailed) job.stage = JobStage::UploadFailed;
        else job.stage = JobStage::Uploading;
    }

    void ProjectSyncLoop() {
        while (!stopRequested.load()) {
            RenderJob jobCopy;
            ProjectTransfer transferCopy;
            Peer peerCopy;
            bool found = false;
            bool uploadStateChanged = false;
            {
                std::scoped_lock lock(stateMutex);
                const auto now = std::chrono::steady_clock::now();
                for (auto& job : jobs) {
                    const bool acceptsParticipantSync =
                        job.stage == JobStage::Uploading ||
                        job.stage == JobStage::UploadFailed ||
                        job.stage == JobStage::ReadyToConfigure ||
                        job.stage == JobStage::Testing ||
                        job.stage == JobStage::TestCompleted ||
                        job.stage == JobStage::TestFailed ||
                        job.stage == JobStage::Rendering ||
                        job.stage == JobStage::Failed;
                    if (!acceptsParticipantSync)
                        continue;
                    for (auto& transfer : job.projectTransfers) {
                        if (transfer.state != ProjectTransferState::Pending)
                            continue;
                        auto peer = peers.find(transfer.nodeId);
                        if (peer == peers.end() || now - peer->second.lastSeen > kPeerTimeout ||
                            !peer->second.trusted || !peer->second.shareEnabled ||
                            !peer->second.rendererReady) {
                            transfer.state = ProjectTransferState::Offline;
                            transfer.error = peer != peers.end() && !peer->second.rendererReady
                                ? "节点未检测到 Blender"
                                : "节点离线或未开启共享";
                            RefreshUploadStageLocked(job);
                            uploadStateChanged = true;
                            continue;
                        }
                        transfer.state = ProjectTransferState::Checking;
                        transfer.error.clear();
                        jobCopy = job;
                        transferCopy = transfer;
                        peerCopy = peer->second;
                        found = true;
                        break;
                    }
                    if (found) break;
                }
            }
            if (uploadStateChanged) {
                PersistJobs();
                Notify();
            }
            if (!found) {
                Sleep(200);
                continue;
            }
            auto jobCancelled = JobCancellation(jobCopy.id);
            std::string uploadError;
            auto lastProgressPublish = std::chrono::steady_clock::now() -
                std::chrono::seconds(1);
            auto progress = [&](ProjectTransferState state, uint64_t bytes,
                                double rate, const std::string& detail) {
                UpdateProjectTransfer(jobCopy.id, transferCopy.nodeId, state, bytes, rate, detail);
                const auto now = std::chrono::steady_clock::now();
                const bool terminal = state == ProjectTransferState::Cached ||
                    state == ProjectTransferState::Completed ||
                    state == ProjectTransferState::Failed ||
                    state == ProjectTransferState::Offline ||
                    state == ProjectTransferState::Excluded;
                if (terminal || now - lastProgressPublish >= std::chrono::milliseconds(500)) {
                    PersistJobs(jobCopy.id);
                    Notify();
                    lastProgressPublish = now;
                }
            };
            bool uploaded = peerCopy.supportsResumableProjectUpload
                ? UploadProjectResumable(peerCopy, owner->GetConfig(), jobCopy, progress,
                                         uploadError, jobCancelled.get())
                : EnsureRemoteProjectCache(peerCopy, owner->GetConfig(), jobCopy,
                                           uploadError, jobCancelled.get());
            {
                std::scoped_lock lock(stateMutex);
                auto job = std::find_if(jobs.begin(), jobs.end(),
                    [&](const RenderJob& value) { return value.id == jobCopy.id; });
                if (job != jobs.end()) {
                    auto transfer = std::find_if(job->projectTransfers.begin(),
                        job->projectTransfers.end(), [&](const ProjectTransfer& value) {
                            return value.nodeId == transferCopy.nodeId;
                        });
                    if (transfer != job->projectTransfers.end() &&
                        transfer->state != ProjectTransferState::Excluded) {
                        if (uploaded) {
                            transfer->state = peerCopy.supportsResumableProjectUpload
                                ? (transfer->state == ProjectTransferState::Cached
                                    ? ProjectTransferState::Cached
                                    : ProjectTransferState::Completed)
                                : ProjectTransferState::Completed;
                            transfer->transferredBytes = transfer->totalBytes;
                            transfer->bytesPerSecond = 0;
                            transfer->attempts = 0;
                            transfer->error.clear();
                        } else if (uploadError.starts_with("retry: ") &&
                                   ++transfer->attempts < 3) {
                            transfer->state = ProjectTransferState::Pending;
                            transfer->error = uploadError.substr(7);
                        } else {
                            transfer->state = ProjectTransferState::Failed;
                            transfer->error = uploadError.starts_with("retry: ")
                                ? uploadError.substr(7) : uploadError;
                        }
                    }
                    RefreshUploadStageLocked(*job);
                }
            }
            PersistJobs(jobCopy.id);
            Notify();
            if (!uploaded) Sleep(500);
        }
    }

    bool DispatchRemotePackage(
        const Peer& peer, const RenderJob& job, const std::vector<FrameTask>& frames,
        const std::vector<std::string>& attemptTokens, const std::string& packageId,
        std::string& error, std::atomic<bool>* jobCancelled) {
        if (!peer.supportsFramePackages || !peer.supportsProjectCache || frames.empty() ||
            frames.size() > std::clamp(peer.maxPackageFrames, size_t{1}, size_t{20}) ||
            frames.size() != attemptTokens.size()) {
            error = "permanent: worker does not support frame packages";
            return false;
        }
        if (std::any_of(frames.begin(), frames.end(), [](const FrameTask& frame) {
                return frame.frame == 0;
            })) {
            error = "permanent: protocol v6 workers do not advertise frame-zero support";
            return false;
        }
        if (!job.compositorOutputs.empty() && !peer.supportsResultBundles) {
            error = "permanent: worker does not support compositor result bundles";
            return false;
        }
        Config config = owner->GetConfig();
        if (CancellationRequested(&stopRequested, jobCancelled)) {
            error = "job was stopped";
            return false;
        }
        if (!VerifyRemotePeer(peer, config, error) ||
            !EnsureRemoteProjectCache(peer, config, job, error, jobCancelled))
            return false;
        json frameMetadata = json::array();
        json compositorMetadata = json::array();
        for (size_t index = 0; index < frames.size(); ++index) {
            frameMetadata.push_back({{"task", frames[index].taskId},
                                     {"frame", frames[index].frame},
                                     {"attempt", attemptTokens[index]}});
        }
        for (const auto& target : job.compositorOutputs) {
            OutputTarget executionTarget = target;
            executionTarget.filePattern = ResolveBlendNameToken(target.filePattern, job);
            compositorMetadata.push_back(OutputTargetLocatorMetadata(executionTarget));
        }
        const std::string compositorHeader = compositorMetadata.dump();
        if (compositorHeader.size() > 4096) {
            error = "permanent: compositor locator metadata exceeds protocol limit";
            return false;
        }
        http::Headers headers = {
            {"X-RM-Token", config.clusterToken}, {"X-RM-Package", packageId},
            {"X-RM-Job", job.id}, {"X-RM-Frames", frameMetadata.dump()},
            {"X-RM-Job-Name", HeaderDisplayText(job.name)},
            {"X-RM-Origin-Name", HeaderDisplayText(config.nodeName)},
            {"X-RM-Compositor", compositorHeader},
            {"X-RM-Result-Bundles", peer.supportsResultBundles ? "1" : "0"},
            {"X-RM-Project", job.projectHash}, {"X-RM-Format", job.outputFormat},
            {"X-RM-Project-Bundle", job.projectBundle ? "1" : "0"},
            {"X-RM-Software", job.software},
            {"X-RM-Extension", job.outputExtension},
            {"X-RM-Blender", job.softwareVersion}, {"X-RM-Engine", job.renderEngine},
            {"X-RM-CPU-Safe", "0"},
            {"X-RM-Requires-GPU", job.requiresGpu ? "1" : "0"},
            {"X-RM-Addons", "[]"},
            {"X-RM-Origin-Port", std::to_string(config.httpPort)},
            {"X-RM-Frame-Logs", "1"},
        };
        auto result = PostEmpty(peer.address, peer.httpPort, "/v3/package", headers,
                                &stopRequested, jobCancelled);
        if (!result) {
            error = "retry: package dispatch network error: " +
                http::to_string(result.error());
            return false;
        }
        if (result->status != 202) {
            std::string detail = result->body.empty() ? "worker rejected package" : result->body;
            bool transient = (result->status >= 500 && result->status != 507) ||
                result->status == 404 ||
                result->status == 409 || result->status == 425;
            error = (transient ? "retry: " : "permanent: ") + detail;
            LogWarning("worker rejected package " + packageId + "; node=" + peer.nodeId);
            return false;
        }
        LogInfo("dispatched package " + packageId + "; frames=" +
                std::to_string(frames.size()) + "; node=" + peer.nodeId);
        return true;
    }

    bool RecoverLostAssignments() {
        bool recovered = false;
        auto nowSteady = std::chrono::steady_clock::now();
        std::scoped_lock lock(stateMutex);
        for (auto& job : jobs) {
            for (auto& frame : job.frames) {
                if (!IsActiveFrameLease(frame.state) ||
                    frame.assignedAt == std::chrono::steady_clock::time_point{} ||
                    nowSteady - frame.assignedAt < assignmentGrace)
                    continue;
                auto peer = peers.find(frame.assignedNodeId);
                bool peerGone = peer == peers.end() || nowSteady - peer->second.lastSeen > kPeerTimeout;
                bool peerNoLongerWorking = !peerGone &&
                    PeerExplicitlyStoppedTask(peer->second,
                        frame.assignedPackageId.empty() ? frame.taskId
                                                        : frame.assignedPackageId);
                if (!peerGone && !peerNoLongerWorking) continue;
                frame.state = TaskState::Pending;
                frame.error = peerGone ? "worker went offline; frame requeued"
                                       : "worker stopped without returning a result; frame requeued";
                frame.assignedNodeId.clear();
                frame.assignedNodeName.clear();
                frame.assignedPackageId.clear();
                frame.assignedAt = {};
                frame.attemptToken.clear();
                frame.startedAt = {};
                frame.endedAt = {};
                recovered = true;
            }
        }
        return recovered;
    }

    void SchedulerLoop() {
        while (!stopRequested.load()) {
            if (RecoverLostAssignments()) {
                PersistJobs();
                Notify();
            }
            // Reserve the v6/local package size first. Once the job is known we
            // select a peer that has not rejected this job, then shrink to one
            // frame only when that actual peer lacks package cancellation.
            size_t jobIndex = 0;
            std::vector<size_t> frameIndices;
            RenderJob jobCopy;
            std::vector<FrameTask> frameCopies;
            bool found = false;
            std::unique_lock stageGate(jobStageGateMutex);
            {
                std::scoped_lock lock(stateMutex);
                size_t selected = jobs.size();
                for (size_t j = 0; j < jobs.size(); ++j) {
                    if (jobs[j].stage != JobStage::Rendering &&
                        jobs[j].stage != JobStage::Testing)
                        continue;
                    const bool pending = std::any_of(
                        jobs[j].frames.begin(), jobs[j].frames.end(),
                        [&](const FrameTask& frame) {
                            return frame.state == TaskState::Pending &&
                                (jobs[j].stage != JobStage::Testing ||
                                 std::find(jobs[j].testFrames.begin(), jobs[j].testFrames.end(),
                                           frame.frame) != jobs[j].testFrames.end());
                        });
                    if (!pending) continue;
                    if (selected == jobs.size() || ui::DispatchesBefore(
                            {Utf8ToWide(jobs[j].id), Utf8ToWide(jobs[j].name),
                             jobs[j].priority, jobs[j].createdAt, jobs[j].finishedAt},
                            {Utf8ToWide(jobs[selected].id), Utf8ToWide(jobs[selected].name),
                             jobs[selected].priority, jobs[selected].createdAt,
                             jobs[selected].finishedAt}))
                        selected = j;
                }
                if (selected != jobs.size()) {
                    jobIndex = selected;
                    jobCopy = jobs[selected];
                    const size_t wantedFrames = std::clamp(
                        jobs[selected].packageSize, size_t{1}, size_t{20});
                    for (size_t f = 0; f < jobs[selected].frames.size() &&
                         frameIndices.size() < wantedFrames; ++f) {
                        if (jobs[selected].frames[f].state != TaskState::Pending) continue;
                        if (jobs[selected].stage == JobStage::Testing &&
                            std::find(jobs[selected].testFrames.begin(),
                                      jobs[selected].testFrames.end(),
                                      jobs[selected].frames[f].frame) ==
                                jobs[selected].testFrames.end())
                            continue;
                        jobs[selected].frames[f].state = TaskState::Dispatching;
                        frameIndices.push_back(f);
                        frameCopies.push_back(jobs[selected].frames[f]);
                    }
                    found = !frameIndices.empty();
                }
            }
            stageGate.unlock();
            if (!found) {
                std::this_thread::sleep_for(std::chrono::milliseconds(150));
                continue;
            }
            // The first peer lookup determines the preferred package size before
            // a pending job is selected. Re-evaluate it now that the job is known
            // so a deterministic rejection from this peer is fenced for the rest
            // of this job while other compatible peers remain eligible.
            const bool forceLocalFrameZero = frameCopies.front().frame == 0;
            if (forceLocalFrameZero && frameIndices.size() > 1) {
                std::scoped_lock lock(stateMutex);
                for (size_t index = 1; index < frameIndices.size(); ++index) {
                    if (jobIndex < jobs.size() &&
                        frameIndices[index] < jobs[jobIndex].frames.size() &&
                        jobs[jobIndex].frames[frameIndices[index]].state ==
                            TaskState::Dispatching)
                        jobs[jobIndex].frames[frameIndices[index]].state = TaskState::Pending;
                }
                frameIndices.resize(1);
                frameCopies.resize(1);
            }
            auto peer = forceLocalFrameZero ? std::optional<Peer>{}
                                            : AvailablePeer(jobCopy.id);
            if (peer && frameIndices.size() > EffectiveRemotePackageLimit(*peer)) {
                std::scoped_lock lock(stateMutex);
                const size_t retained = std::min(
                    frameIndices.size(), EffectiveRemotePackageLimit(*peer));
                for (size_t index = retained; index < frameIndices.size(); ++index) {
                    if (jobIndex < jobs.size() && frameIndices[index] < jobs[jobIndex].frames.size() &&
                        jobs[jobIndex].frames[frameIndices[index]].state == TaskState::Dispatching)
                        jobs[jobIndex].frames[frameIndices[index]].state = TaskState::Pending;
                }
                frameIndices.resize(retained);
                frameCopies.resize(retained);
            }
            auto jobCancelled = JobCancellation(jobCopy.id);
            bool handled = false;
            // Persist and notify only when an assignment really changed; a
            // reservation that goes back to Pending leaves the stored job as is.
            bool stateChanged = false;
            if (peer) {
                std::string dispatchError;
                const std::string packageId = NewRandomHex(12);
                std::vector<std::string> attemptTokens;
                for (size_t index = 0; index < frameCopies.size(); ++index)
                    attemptTokens.push_back(NewRandomHex(12));
                bool assignmentReserved = true;
                {
                    std::scoped_lock lock(stateMutex);
                    assignmentReserved = jobIndex < jobs.size() && !jobCancelled->load();
                    for (size_t index = 0; assignmentReserved && index < frameIndices.size(); ++index)
                        assignmentReserved = frameIndices[index] < jobs[jobIndex].frames.size() &&
                            jobs[jobIndex].frames[frameIndices[index]].state == TaskState::Dispatching;
                    if (!assignmentReserved) handled = true;
                    if (assignmentReserved && std::find(
                            jobs[jobIndex].remoteNodeIds.begin(),
                            jobs[jobIndex].remoteNodeIds.end(), peer->nodeId) ==
                            jobs[jobIndex].remoteNodeIds.end())
                        jobs[jobIndex].remoteNodeIds.push_back(peer->nodeId);
                    for (size_t index = 0; index < frameIndices.size(); ++index) {
                        if (!assignmentReserved) break;
                        if (jobIndex >= jobs.size() ||
                            frameIndices[index] >= jobs[jobIndex].frames.size())
                            continue;
                        auto& frame = jobs[jobIndex].frames[frameIndices[index]];
                        frame.state = peer->supportsFrameStart
                            ? TaskState::Assigned : TaskState::Rendering;
                        frame.assignedNodeId = peer->nodeId;
                        frame.assignedNodeName = peer->name;
                        frame.assignedPackageId = packageId;
                        frame.assignedAt = {};
                        frame.attemptToken = attemptTokens[index];
                        frame.log = FrameLogDestination(
                            owner->ProjectStorageDir(), jobs[jobIndex].id, frame.frame);
                        frame.startedAt = peer->supportsFrameStart
                            ? std::chrono::system_clock::time_point{}
                            : std::chrono::system_clock::now();
                        frame.endedAt = {};
                        ++frame.dispatchAttempts;
                    }
                }
                stateChanged = stateChanged || assignmentReserved;
                const bool dispatched = assignmentReserved && DispatchRemotePackage(
                    *peer, jobCopy, frameCopies, attemptTokens, packageId, dispatchError,
                    jobCancelled.get());
                if (dispatched) {
                    std::scoped_lock lock(stateMutex);
                    for (size_t index = 0; index < frameIndices.size(); ++index) {
                        if (jobIndex >= jobs.size() ||
                            frameIndices[index] >= jobs[jobIndex].frames.size())
                            continue;
                        auto& frame = jobs[jobIndex].frames[frameIndices[index]];
                        if (IsActiveFrameLease(frame.state) &&
                            frame.attemptToken == attemptTokens[index]) {
                            frame.assignedAt = std::chrono::steady_clock::now();
                            frame.error.clear();
                        }
                    }
                    handled = true;
                } else {
                    bool retryable = dispatchError.starts_with("retry: ");
                    bool explicitlyPermanent = dispatchError.starts_with("permanent: ");
                    bool permanentlyRejected = !retryable && !jobCancelled->load();
                    std::string visibleError = dispatchError;
                    if (explicitlyPermanent) visibleError.erase(0, 11);
                    else if (visibleError.starts_with("retry: ")) visibleError.erase(0, 7);
                    std::scoped_lock lock(stateMutex);
                    if (!jobCancelled->load()) {
                        auto decision = std::make_pair(jobCopy.id, peer->nodeId);
                        if (permanentlyRejected) rejectedJobPeers.emplace(decision);
                        else if (retryable)
                            jobPeerBackoff[decision] = std::chrono::steady_clock::now() +
                                std::chrono::seconds(5);
                    }
                    LogWarning("worker unavailable for job " + jobCopy.id + "; node=" +
                               peer->nodeId + "; " + visibleError.substr(0, 2048));
                    size_t restored = 0;
                    for (size_t index = 0; index < frameIndices.size(); ++index) {
                        if (jobIndex >= jobs.size() ||
                            frameIndices[index] >= jobs[jobIndex].frames.size())
                            continue;
                        auto& frame = jobs[jobIndex].frames[frameIndices[index]];
                        if (IsActiveFrameLease(frame.state) &&
                            frame.attemptToken == attemptTokens[index]) {
                            frame.state = TaskState::Dispatching;
                            frame.assignedNodeId.clear();
                            frame.assignedNodeName.clear();
                            frame.assignedPackageId.clear();
                            frame.assignedAt = {};
                            frame.attemptToken.clear();
                            frame.startedAt = {};
                            frame.endedAt = {};
                            frame.error = visibleError;
                            ++restored;
                        }
                    }
                    // A fast worker may return some package results before the 202
                    // response reaches the client. Never locally duplicate those.
                    if (restored != frameIndices.size()) handled = true;
                }
            }
            // A rejected/backed-off peer is not the same as having no remote
            // capacity. Put the reservation back so the next eligible peer gets
            // the package before the local worker is considered.
            if (!handled && peer && !jobCancelled->load() &&
                AvailablePeer(jobCopy.id).has_value()) {
                std::scoped_lock lock(stateMutex);
                for (size_t frameIndex : frameIndices) {
                    if (jobIndex >= jobs.size() ||
                        frameIndex >= jobs[jobIndex].frames.size())
                        continue;
                    auto& frame = jobs[jobIndex].frames[frameIndex];
                    if (frame.state == TaskState::Dispatching)
                        frame.state = TaskState::Pending;
                }
                handled = true;
            }
            if (!handled && jobCopy.stage == JobStage::Testing && !jobCancelled->load() &&
                HasNonRejectedProjectParticipant(jobCopy.id)) {
                {
                    std::scoped_lock lock(stateMutex);
                    for (size_t frameIndex : frameIndices) {
                        if (jobIndex < jobs.size() && frameIndex < jobs[jobIndex].frames.size() &&
                            jobs[jobIndex].frames[frameIndex].state == TaskState::Dispatching)
                            jobs[jobIndex].frames[frameIndex].state = TaskState::Pending;
                    }
                }
                handled = true;
                Sleep(250);
            }
            int expectedAssigned = 0;
            if (!handled && !jobCancelled->load() &&
                workerAssigned.compare_exchange_strong(expectedAssigned, 1)) {
                WorkItem item;
                item.taskId = frameCopies.front().taskId;
                item.jobId = jobCopy.id;
                item.jobName = jobCopy.name;
                item.frame = frameCopies.front().frame;
                item.packageId = NewRandomHex(12);
                for (const auto& frame : frameCopies)
                    item.frames.push_back({frame.taskId, frame.frame, {}});
                item.project = jobCopy.preparedProject;
                item.software = jobCopy.software;
                if (jobCopy.projectBundle) item.bundlePayload = ProjectPayloadPath(jobCopy);
                item.outputDir = owner->ProjectStorageDir() / L"jobs" /
                    Utf8ToWide(jobCopy.id) /
                    L"packages" / Utf8ToWide(item.packageId);
                item.outputFormat = jobCopy.outputFormat;
                item.outputExtension = jobCopy.outputExtension;
                item.softwareVersion = jobCopy.softwareVersion;
                item.renderEngine = jobCopy.renderEngine;
                item.softwareExecutable = jobCopy.softwareExecutable;
                item.requiresGpu = jobCopy.requiresGpu;
                item.compositorOutputs = jobCopy.compositorOutputs;
                for (auto& target : item.compositorOutputs)
                    target.filePattern = ResolveBlendNameToken(target.filePattern, jobCopy);
                std::error_code markerError;
                fs::remove(item.outputDir / L"stop-after-current", markerError);
                bool localReserved = true;
                {
                    std::scoped_lock lock(stateMutex);
                    localReserved = jobIndex < jobs.size() && !jobCancelled->load();
                    for (size_t frameIndex : frameIndices)
                        localReserved = localReserved && frameIndex < jobs[jobIndex].frames.size() &&
                            jobs[jobIndex].frames[frameIndex].state == TaskState::Dispatching;
                    Config config = owner->GetConfig();
                    for (size_t frameIndex : frameIndices) {
                        if (!localReserved) break;
                        auto& frame = jobs[jobIndex].frames[frameIndex];
                        frame.state = TaskState::Assigned;
                        frame.assignedNodeId = config.nodeId;
                        frame.assignedNodeName = config.nodeName;
                        frame.assignedPackageId = item.packageId;
                        frame.assignedAt = {};
                        frame.attemptToken.clear();
                        frame.log = FrameLogDestination(
                            owner->ProjectStorageDir(), jobs[jobIndex].id, frame.frame);
                        frame.startedAt = {};
                        frame.endedAt = {};
                    }
                }
                stateChanged = stateChanged || localReserved;
                if (localReserved) {
                    std::scoped_lock workLock(workMutex);
                    work.push(std::move(item));
                    workCv.notify_one();
                } else {
                    --workerAssigned;
                }
                handled = true;
            }
            if (!handled) {
                {
                    std::scoped_lock lock(stateMutex);
                    if (jobIndex < jobs.size()) {
                        for (size_t frameIndex : frameIndices) {
                            if (frameIndex >= jobs[jobIndex].frames.size()) continue;
                            auto& frame = jobs[jobIndex].frames[frameIndex];
                            if (frame.state != TaskState::Dispatching) continue;
                            frame.state = TaskState::Pending;
                            frame.assignedNodeId.clear();
                            frame.assignedNodeName.clear();
                            frame.assignedPackageId.clear();
                            frame.assignedAt = {};
                            frame.attemptToken.clear();
                            frame.startedAt = {};
                            frame.endedAt = {};
                        }
                    }
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(250));
            }
            if (stateChanged) {
                PersistJobs(jobCopy.id);
                Notify();
            }
        }
    }

    bool SendRemoteResult(const WorkItem& item, const WorkItem::Frame& frame,
                          bool success, bool retry,
                          const fs::path& output, const std::wstring& renderError,
                          const FrameRenderTiming* timing = nullptr) {
        Config config = owner->GetConfig();
        http::Headers headers = {
            {"X-RM-Token", config.clusterToken}, {"X-RM-Task", frame.taskId},
            {"X-RM-Attempt", frame.attemptToken},
            {"X-RM-Status", retry ? "retry" : success ? "completed" : "failed"},
            {"X-RM-Error", HeaderDisplayText(WideToUtf8(renderError), 2048)},
        };
        if (success && item.resultBundles)
            headers.emplace("X-RM-Result-Bundle", "1");
        if (success && timing && timing->Valid()) {
            headers.emplace("X-RM-Started-At",
                            std::to_string(UnixMilliseconds(timing->startedAt)));
            headers.emplace("X-RM-Ended-At",
                            std::to_string(UnixMilliseconds(timing->endedAt)));
        }
        for (int attempt = 0; attempt < 4 && !stopRequested.load(); ++attempt) {
            if (attempt) {
                for (int step = 0; step < attempt * 20 && !stopRequested.load(); ++step)
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                if (stopRequested.load()) break;
            }
            http::Result result;
            if (success && !retry) {
                result = PostFile(item.originAddress, item.originPort, "/v1/result", headers,
                                  output, &stopRequested, 3600);
            } else {
                result = PostEmpty(item.originAddress, item.originPort, "/v1/result",
                                   headers, &stopRequested);
            }
            if (result && result->status == 200) return true;
            // Authentication, lease fencing, and stopped-job responses are
            // terminal for this attempt. Retrying them only delays cleanup.
            if (result && result->status >= 400 && result->status < 500) return false;
        }
        return false;
    }

    bool SendRemoteFrameStarted(
        const WorkItem& item, const WorkItem::Frame& frame,
        std::chrono::system_clock::time_point startedAt) {
        if (!item.remote || startedAt.time_since_epoch() ==
                                std::chrono::system_clock::duration::zero())
            return false;
        Config config = owner->GetConfig();
        http::Headers headers = {
            {"X-RM-Token", config.clusterToken},
            {"X-RM-Task", frame.taskId},
            {"X-RM-Attempt", frame.attemptToken},
            {"X-RM-Started-At", std::to_string(UnixMilliseconds(startedAt))},
        };
        for (int attempt = 0; attempt < 2 && !stopRequested.load(); ++attempt) {
            auto result = PostEmpty(item.originAddress, item.originPort,
                                    "/v1/frame-start", headers, &stopRequested);
            if (result && result->status == 200) return true;
            if (result && result->status >= 400 && result->status < 500) return false;
            if (attempt == 0)
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
        return false;
    }

    bool SendRemoteFrameLog(
        const WorkItem& item, const WorkItem::Frame& frame,
        const fs::path& log, bool complete) {
        if (!item.remote || !item.frameLogs || !fs::is_regular_file(log)) return false;
        Config config = owner->GetConfig();
        http::Headers headers = {
            {"X-RM-Token", config.clusterToken},
            {"X-RM-Task", frame.taskId},
            {"X-RM-Attempt", frame.attemptToken},
            {"X-RM-Complete", complete ? "1" : "0"},
        };
        const int attempts = complete ? 2 : 1;
        for (int attempt = 0; attempt < attempts && !stopRequested.load(); ++attempt) {
            auto result = PostFile(item.originAddress, item.originPort, "/v1/frame-log",
                                   headers, log, &stopRequested, 30);
            if (result && result->status == 200) return true;
            if (result && result->status >= 400 && result->status < 500) return false;
            if (attempt + 1 < attempts)
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
        return false;
    }

    // Local renders of tex-bundle jobs extract the bundle on first use, the
    // same way remote workers fill their project cache. Job creation therefore
    // writes the project data only once.
    bool PrepareLocalProject(const WorkItem& item, std::wstring& error) {
        if (item.remote || item.bundlePayload.empty() || fs::is_regular_file(item.project))
            return true;
        if (ExtractProjectBundle(item.bundlePayload, item.project.parent_path(), error,
                                 &stopRequested))
            return true;
        error = L"无法展开本机工程包：" + error;
        return false;
    }

    // Small diagnostic note kept when a result cannot reach its origin.
    static std::string FailedResultRecord(const WorkItem& item, const std::string& attempt,
                                          const char* status, const std::wstring& error) {
        return "origin=" + item.originAddress + ":" + std::to_string(item.originPort) +
            "\nattempt=" + attempt + "\nstatus=" + status + "\nerror=" + WideToUtf8(error) +
            "\n";
    }

    void WorkerLoop() {
        while (!stopRequested.load()) {
            WorkItem item;
            {
                std::unique_lock lock(workMutex);
                workCv.wait_for(lock, std::chrono::milliseconds(500), [&] {
                    return stopRequested.load() || !work.empty();
                });
                if (stopRequested.load()) break;
                if (work.empty()) continue;
                item = std::move(work.front());
                work.pop();
            }
            if (item.frames.empty())
                item.frames.push_back({item.taskId, item.frame, item.attemptToken});
            if (!item.remote) {
                bool allStopped = true;
                {
                    std::scoped_lock lock(stateMutex);
                    auto job = std::find_if(jobs.begin(), jobs.end(), [&](const RenderJob& value) {
                        return value.id == item.jobId;
                    });
                    if (job != jobs.end()) {
                        for (const auto& packagedFrame : item.frames) {
                            auto frame = std::find_if(job->frames.begin(), job->frames.end(),
                                [&](const FrameTask& value) {
                                    return value.taskId == packagedFrame.taskId;
                                });
                            allStopped = allStopped && frame != job->frames.end() &&
                                frame->state == TaskState::Stopped;
                        }
                    }
                }
                if (allStopped) {
                    --workerAssigned;
                    PersistJobs(item.jobId);
                    Notify();
                    continue;
                }
            }
            {
                std::scoped_lock lock(currentTaskMutex);
                currentTaskId = item.packageId.empty() ? item.taskId : item.packageId;
                currentJobId = item.jobId;
                currentPackageHasStartedFrame = false;
                currentRenderingFrame.reset();
                currentActivity.jobName = HeaderDisplayText(
                    item.jobName.empty() ? item.jobId : item.jobName, 128);
                currentActivity.frames.clear();
                currentActivity.frames.reserve(item.frames.size());
                for (const auto& frame : item.frames)
                    currentActivity.frames.push_back(frame.frame);
                currentActivity.completedFrames = 0;
                currentActivity.rendering = false;
            }
            Notify();
            LogInfo("render package started; package=" +
                    (item.packageId.empty() ? item.taskId : item.packageId) + "; frames=" +
                    std::to_string(item.frames.size()));
            const std::string cancellationId = item.packageId.empty() ? item.taskId : item.packageId;
            auto abortBeforeFirstFrame = item.remote
                ? PackageAbortSignal(cancellationId)
                : std::shared_ptr<std::atomic<bool>>{};
            const auto packageStartedAt = std::chrono::system_clock::now();
            std::vector<int> deliveredFrames;
            std::map<int, std::vector<RenderArtifact>> outputs;
            std::map<int, fs::path> frameLogs;
            std::wstring renderError;
            bool retryAfterIdle = false;
            Config renderConfig = owner->GetConfig();
            if (!item.remote && !item.softwareExecutable.empty())
                renderConfig.blenderPath = item.softwareExecutable;
            std::set<int> completedCallbacks;
            std::set<int> startedCallbacks;
            std::set<int> reportedFrames;
            std::map<int, std::chrono::steady_clock::time_point> lastLogSentAt;
            auto reportFrameStarted = [&](int frameNumber,
                                           std::chrono::system_clock::time_point startedAt) {
                auto packagedFrame = std::find_if(
                    item.frames.begin(), item.frames.end(), [&](const WorkItem::Frame& value) {
                        return value.frame == frameNumber;
                    });
                if (packagedFrame == item.frames.end() ||
                    !startedCallbacks.insert(frameNumber).second)
                    return;
                {
                    std::scoped_lock lock(currentTaskMutex);
                    currentPackageHasStartedFrame = true;
                    currentRenderingFrame = frameNumber;
                    currentActivity.rendering = true;
                }
                Notify();
                if (item.remote) {
                    SendRemoteFrameStarted(item, *packagedFrame, startedAt);
                    return;
                }
                bool changed = false;
                {
                    std::scoped_lock lock(stateMutex);
                    auto job = std::find_if(jobs.begin(), jobs.end(), [&](const RenderJob& value) {
                        return value.id == item.jobId;
                    });
                    if (job == jobs.end()) return;
                    auto frame = std::find_if(job->frames.begin(), job->frames.end(),
                        [&](const FrameTask& value) {
                            return value.taskId == packagedFrame->taskId;
                        });
                    if (frame != job->frames.end() &&
                        frame->assignedPackageId == item.packageId &&
                        IsActiveFrameLease(frame->state)) {
                        changed = frame->state != TaskState::Rendering ||
                            frame->startedAt.time_since_epoch() ==
                                std::chrono::system_clock::duration::zero();
                        frame->state = TaskState::Rendering;
                        if (frame->startedAt.time_since_epoch() ==
                            std::chrono::system_clock::duration::zero())
                            frame->startedAt = startedAt;
                        frame->endedAt = {};
                    }
                }
                if (changed) {
                    PersistJobs(item.jobId);
                    Notify();
                }
            };
            auto reportFrameLog = [&](int frameNumber, const fs::path& source, bool complete) {
                auto packagedFrame = std::find_if(
                    item.frames.begin(), item.frames.end(), [&](const WorkItem::Frame& value) {
                        return value.frame == frameNumber;
                    });
                if (packagedFrame == item.frames.end() || !fs::is_regular_file(source)) return;
                const auto now = std::chrono::steady_clock::now();
                auto previous = lastLogSentAt.find(frameNumber);
                if (!complete && previous != lastLogSentAt.end() &&
                    now - previous->second < std::chrono::seconds(1))
                    return;
                lastLogSentAt[frameNumber] = now;
                if (item.remote) {
                    if (item.frameLogs)
                        SendRemoteFrameLog(item, *packagedFrame, source, complete);
                    return;
                }
                const fs::path destination = FrameLogDestination(
                    owner->ProjectStorageDir(), item.jobId, frameNumber);
                std::wstring logError;
                const std::string logContents = ReadFileText(source);
                if (!logContents.empty() &&
                    WriteDurableFile(destination, logContents, logError)) {
                    std::scoped_lock lock(stateMutex);
                    auto job = std::find_if(jobs.begin(), jobs.end(), [&](const RenderJob& value) {
                        return value.id == item.jobId;
                    });
                    if (job == jobs.end()) return;
                    auto frame = std::find_if(job->frames.begin(), job->frames.end(),
                        [&](const FrameTask& value) {
                            return value.taskId == packagedFrame->taskId;
                        });
                    if (frame != job->frames.end()) frame->log = destination;
                }
                Notify();
            };
            std::map<int, FrameRenderTiming> frameTimings;
            auto reportCompletedFrame = [&](int frameNumber,
                                             const std::vector<RenderArtifact>& artifacts,
                                             const FrameRenderTiming& timing) {
                auto packagedFrame = std::find_if(
                    item.frames.begin(), item.frames.end(), [&](const WorkItem::Frame& value) {
                        return value.frame == frameNumber;
                    });
                if (packagedFrame == item.frames.end() ||
                    !completedCallbacks.insert(frameNumber).second)
                    return;
                {
                    std::scoped_lock lock(currentTaskMutex);
                    currentActivity.completedFrames = completedCallbacks.size();
                    if (currentRenderingFrame == frameNumber) {
                        currentRenderingFrame.reset();
                        currentActivity.rendering = false;
                    }
                }
                if (timing.Valid()) frameTimings[frameNumber] = timing;
                Notify();
                if (item.remote) {
                    bool frameSuccess = true;
                    fs::path transferFile;
                    std::wstring frameError;
                    if (item.resultBundles) {
                        transferFile = item.outputDir /
                            (L"result-" + std::to_wstring(frameNumber) + L".rmbundle");
                        frameSuccess = WriteResultBundle(
                            transferFile, artifacts, frameError, &stopRequested);
                    } else {
                        auto primary = std::find_if(
                            artifacts.begin(), artifacts.end(), [](const RenderArtifact& artifact) {
                                return artifact.logicalPath == "primary";
                            });
                        frameSuccess = primary != artifacts.end();
                        if (frameSuccess) transferFile = primary->source;
                        else frameError = L"Rendered frame has no primary output";
                    }
                    bool delivered = SendRemoteResult(
                        item, *packagedFrame, frameSuccess, false,
                        frameSuccess ? transferFile : fs::path{}, frameError,
                        timing.Valid() ? &timing : nullptr);
                    if (delivered) reportedFrames.insert(frameNumber);
                    if (delivered && frameSuccess) {
                        deliveredFrames.push_back(frameNumber);
                        LogInfo("render frame delivered; package=" + cancellationId +
                                "; frame=" + std::to_string(frameNumber));
                    } else if (!delivered) {
                        std::error_code ignored;
                        fs::path failures = owner->ProjectStorageDir() /
                            L"failed-results";
                        fs::create_directories(failures, ignored);
                        WriteFileText(
                            failures / (Utf8ToWide(packagedFrame->taskId) + L".txt"),
                            FailedResultRecord(item, packagedFrame->attemptToken,
                                               frameSuccess ? "completed" : "failed",
                                               frameError));
                    }
                    return;
                }

                auto jobCancelled = JobCancellation(item.jobId);
                RenderJob jobSnapshot;
                bool canPublish = false;
                bool stateUpdated = false;
                {
                    std::scoped_lock lock(stateMutex);
                    auto job = std::find_if(jobs.begin(), jobs.end(), [&](const RenderJob& value) {
                        return value.id == item.jobId;
                    });
                    if (job != jobs.end()) {
                        auto frame = std::find_if(job->frames.begin(), job->frames.end(),
                            [&](const FrameTask& value) {
                                return value.taskId == packagedFrame->taskId;
                            });
                        canPublish = frame != job->frames.end() &&
                            IsActiveFrameLease(frame->state) &&
                            frame->assignedPackageId == item.packageId;
                        if (canPublish) jobSnapshot = *job;
                    }
                }
                if (!canPublish) return;
                std::map<std::string, fs::path> artifactMap;
                for (const auto& artifact : artifacts)
                    artifactMap[artifact.logicalPath] = artifact.source;
                fs::path publishedPrimary;
                std::wstring publishError;
                bool published = PublishResultArtifacts(
                    owner->ProjectStorageDir(), jobSnapshot, frameNumber, artifactMap,
                    item.packageId, publishedPrimary, publishError, jobCancelled.get());
                {
                    std::scoped_lock lock(stateMutex);
                    auto job = std::find_if(jobs.begin(), jobs.end(), [&](const RenderJob& value) {
                        return value.id == item.jobId;
                    });
                    if (job != jobs.end()) {
                        auto frame = std::find_if(job->frames.begin(), job->frames.end(),
                            [&](const FrameTask& value) {
                                return value.taskId == packagedFrame->taskId;
                            });
                        if (frame != job->frames.end() && IsActiveFrameLease(frame->state) &&
                            frame->assignedPackageId == item.packageId) {
                            stateUpdated = true;
                            frame->state = published ? TaskState::Completed : TaskState::Failed;
                            frame->assignedPackageId.clear();
                            if (published) {
                                frame->output = publishedPrimary;
                                frame->artifacts = PublishedArtifactPaths(
                                    owner->ProjectStorageDir(), jobSnapshot,
                                    frameNumber, artifactMap);
                            }
                            frame->error = published ? "" : WideToUtf8(publishError);
                            if (timing.Valid()) {
                                frame->startedAt = timing.startedAt;
                                frame->endedAt = timing.endedAt;
                            } else {
                                frame->endedAt = std::chrono::system_clock::now();
                            }
                        }
                    }
                }
                if (stateUpdated) reportedFrames.insert(frameNumber);
                PersistJobs(item.jobId);
                Notify();
            };
            if (item.remote && PackageCancelled(cancellationId)) {
                renderError = L"任务包已由提交方停止";
                retryAfterIdle = true;
            } else if (!owner->mockRenderer_ && item.remote &&
                !IsComputerIdleForSharedWork(IdleSeconds(), renderConfig.idleAfterSeconds)) {
                renderError = L"电脑恢复使用后已停止，将在空闲时重新分配";
                retryAfterIdle = true;
#ifdef RM_DEV_TOOLS
            } else if (owner->mockRenderer_) {
                fs::create_directories(item.outputDir);
                // Model the real interval where Blender has accepted the package
                // but is still opening the .blend and has not started frame one.
                // Cancellation during this interval must release the Worker
                // without manufacturing a completed/current frame.
                if (item.remote) {
                    for (int step = 0; step < 50 && !stopRequested.load(); ++step) {
                        if (PackageCancelled(cancellationId)) {
                            retryAfterIdle = true;
                            renderError = L"任务包在首帧开始前已停止";
                            break;
                        }
                        std::this_thread::sleep_for(std::chrono::milliseconds(10));
                    }
                }
                for (const auto& frame : item.frames) {
                    if (retryAfterIdle) break;
                    if (item.remote && PackageCancelled(cancellationId)) {
                        retryAfterIdle = true;
                        renderError = L"任务包已由提交方停止";
                        break;
                    }
                    // Keep the per-frame streaming interval observable even on
                    // a busy CI or office workstation. This path is used only
                    // by the in-process mock renderer; real Blender timing is
                    // unaffected.
                    FrameRenderTiming timing;
                    timing.startedAt = std::chrono::system_clock::now();
                    reportFrameStarted(frame.frame, timing.startedAt);
                    std::this_thread::sleep_for(std::chrono::milliseconds(150));
                    fs::path output = item.outputDir /
                        (L"frame_" + std::to_wstring(frame.frame) +
                         Utf8ToWide(item.outputExtension));
                    std::ofstream mock(output, std::ios::binary);
                    mock << "RenderMesh mock frame " << frame.frame;
                    if (mock) outputs[frame.frame].push_back({"primary", output});
                    for (const auto& target : item.compositorOutputs) {
                        fs::path pass = item.outputDir / L"mock-compositor" /
                            Utf8ToWide(target.id) /
                            (L"pass_" + std::to_wstring(frame.frame) + L".exr");
                        fs::create_directories(pass.parent_path());
                        std::ofstream mockPass(pass, std::ios::binary);
                        mockPass << "RenderMesh mock pass " << frame.frame;
                        if (mockPass)
                            outputs[frame.frame].push_back({
                                "compositor/" + target.id + "/ViewLayer/pass_" +
                                    std::to_string(frame.frame) + ".exr",
                                pass});
                    }
                    fs::path mockLog = item.outputDir /
                        (L"frame-" + std::to_wstring(frame.frame) + L".log");
                    std::ofstream mockLogOutput(mockLog, std::ios::binary | std::ios::trunc);
                    mockLogOutput << "RenderMesh Blender log - frame " << frame.frame <<
                        " - complete\n\nBlender mock renderer started\nFra:" << frame.frame <<
                        " Mem:16.00M Time:00:00.08 | Rendering 64 / 64 samples\n"
                        "Saved: frame_" << frame.frame << item.outputExtension << "\n";
                    mockLogOutput.close();
                    timing.endedAt = std::chrono::system_clock::now();
                    if (fs::is_regular_file(mockLog)) {
                        frameLogs[frame.frame] = mockLog;
                    }
                    auto completed = outputs.find(frame.frame);
                    if (completed != outputs.end())
                        reportCompletedFrame(frame.frame, completed->second, timing);
                    // Exercise the valid network ordering where the result is
                    // committed before the final frame-log request arrives.
                    if (fs::is_regular_file(mockLog))
                        reportFrameLog(frame.frame, mockLog, true);
                }
#endif  // RM_DEV_TOOLS
            } else if (const RenderAdapter* adapter = FindRenderAdapter(item.software); !adapter) {
                renderError = L"本机不支持该渲染软件：" + Utf8ToWide(item.software);
            } else if (!PrepareLocalProject(item, renderError)) {
                // renderError explains why the local project copy is unavailable.
            } else {
                std::atomic<bool> finishCurrentThenStop = false;
                RenderPackageRequest request;
                request.config = renderConfig;
                request.project = item.project;
                request.outputDir = item.outputDir;
                for (const auto& frame : item.frames) request.frames.push_back(frame.frame);
                request.outputFormat = item.outputFormat;
                request.outputExtension = item.outputExtension;
                request.outputs = item.compositorOutputs;
                request.stopAfterCurrentOnActivity = item.remote;
                request.stop = &stopRequested;
                request.finishCurrentThenStop = &finishCurrentThenStop;
                request.abortBeforeFirstFrame = abortBeforeFirstFrame.get();
                request.frameStarted = reportFrameStarted;
                request.frameCompleted = reportCompletedFrame;
                request.frameLogUpdated = reportFrameLog;
                RenderPackageResult result;
                const auto renderStartedAt = std::chrono::steady_clock::now();
                bool rendered = adapter->RenderPackage(request, result);
                if (ShouldRetryQuickEmptyPackage(
                        item.remote, rendered, result.completed.empty(),
                        finishCurrentThenStop.load(), stopRequested.load(),
                        PackageCancelled(cancellationId),
                        std::chrono::steady_clock::now() - renderStartedAt, result.error)) {
                    LogWarning("renderer exited without a frame; retrying package once; package=" +
                               cancellationId);
                    std::this_thread::sleep_for(std::chrono::milliseconds(500));
                    result = {};
                    adapter->RenderPackage(request, result);
                }
                outputs = std::move(result.completed);
                frameLogs = std::move(result.frameLogs);
                renderError = std::move(result.error);
                retryAfterIdle = finishCurrentThenStop.load();
            }
            if (item.remote) {
                for (const auto& frame : item.frames) {
                    if (reportedFrames.contains(frame.frame)) continue;
                    auto output = outputs.find(frame.frame);
                    bool frameSuccess = output != outputs.end();
                    bool frameRetry = !frameSuccess && retryAfterIdle;
                    fs::path transferFile;
                    std::wstring bundleError;
                    if (frameSuccess && item.resultBundles) {
                        transferFile = item.outputDir /
                            (L"result-" + std::to_wstring(frame.frame) + L".rmbundle");
                        frameSuccess = WriteResultBundle(transferFile, output->second,
                                                         bundleError, &stopRequested);
                        if (!frameSuccess) renderError = std::move(bundleError);
                    } else if (frameSuccess) {
                        auto primary = std::find_if(
                            output->second.begin(), output->second.end(),
                            [](const RenderArtifact& artifact) {
                                return artifact.logicalPath == "primary";
                            });
                        frameSuccess = primary != output->second.end();
                        if (frameSuccess) transferFile = primary->source;
                        else renderError = L"渲染完成但缺少主输出文件";
                    }
                    frameRetry = !frameSuccess && retryAfterIdle;
                    bool delivered = SendRemoteResult(
                        item, frame, frameSuccess, frameRetry,
                        frameSuccess ? transferFile : fs::path{}, renderError,
                        frameTimings.contains(frame.frame)
                            ? &frameTimings.at(frame.frame) : nullptr);
                    if (delivered && frameSuccess) deliveredFrames.push_back(frame.frame);
                    if (!delivered) {
                        std::error_code ignored;
                        fs::path failures = owner->ProjectStorageDir() /
                            L"failed-results";
                        fs::create_directories(failures, ignored);
                        WriteFileText(
                            failures / (Utf8ToWide(frame.taskId) + L".txt"),
                            FailedResultRecord(item, frame.attemptToken,
                                               frameRetry ? "retry"
                                                   : frameSuccess ? "completed" : "failed",
                                               renderError));
                    }
                }
                // The origin observes the worker's current task ID and requeues a
                // lost delivery. Keep only a small diagnostic record, never an
                // unbounded copy of the received project and frame output.
                std::error_code ignored;
                fs::remove_all(item.temporaryTaskDir.empty()
                                   ? item.project.parent_path() : item.temporaryTaskDir,
                               ignored);
                if (!item.projectHash.empty()) {
                    std::scoped_lock lock(cacheMutex);
                    TouchCacheEntry(owner->ProjectStorageDir(), item.projectHash);
                }
                ContributionRecord record;
                record.packageId = item.packageId.empty() ? item.taskId : item.packageId;
                record.jobId = item.jobId;
                record.jobName = item.jobName.empty() ? item.jobId : item.jobName;
                record.originName = item.originName.empty() ? item.originAddress : item.originName;
                record.originAddress = item.originAddress + ":" + std::to_string(item.originPort);
                for (const auto& frame : item.frames) record.frames.push_back(frame.frame);
                record.completedFrames = std::move(deliveredFrames);
                record.state = record.completedFrames.size() == record.frames.size()
                    ? "completed" : retryAfterIdle ? "stopped"
                    : record.completedFrames.empty() ? "failed" : "partial";
                record.error = record.state == "completed" ? "" : WideToUtf8(renderError);
                record.startedAt = packageStartedAt;
                record.endedAt = std::chrono::system_clock::now();
                {
                    std::scoped_lock lock(stateMutex);
                    contributions.push_back(std::move(record));
                    if (contributions.size() > 5000)
                        contributions.erase(contributions.begin(),
                                            contributions.begin() +
                                                static_cast<ptrdiff_t>(contributions.size() - 5000));
                }
                PersistContributions();
                ForgetCancelledPackage(cancellationId);
            } else {
                auto jobCancelled = JobCancellation(item.jobId);
                std::scoped_lock lock(stateMutex);
                for (auto& job : jobs) {
                    if (job.id != item.jobId) continue;
                    for (const auto& packagedFrame : item.frames) {
                        if (reportedFrames.contains(packagedFrame.frame)) continue;
                        auto frame = std::find_if(job.frames.begin(), job.frames.end(),
                            [&](const FrameTask& candidate) {
                                return candidate.taskId == packagedFrame.taskId;
                            });
                        if (frame == job.frames.end()) continue;
                        auto output = outputs.find(packagedFrame.frame);
                        bool frameSuccess = output != outputs.end();
                        fs::path publishedPrimary;
                        std::wstring publishError;
                        std::map<std::string, fs::path> artifacts;
                        if (frameSuccess) {
                            for (const auto& artifact : output->second)
                                artifacts[artifact.logicalPath] = artifact.source;
                            frameSuccess = PublishResultArtifacts(
                                owner->ProjectStorageDir(), job,
                                packagedFrame.frame, artifacts,
                                item.packageId, publishedPrimary, publishError,
                                jobCancelled.get());
                        }
                        if (frame->state == TaskState::Stopped) continue;
                        frame->state = frameSuccess ? TaskState::Completed : TaskState::Failed;
                        frame->assignedPackageId.clear();
                        if (frameSuccess) {
                            frame->output = publishedPrimary;
                            frame->artifacts = PublishedArtifactPaths(
                                owner->ProjectStorageDir(), job,
                                packagedFrame.frame, artifacts);
                        }
                        frame->error = frameSuccess ? "" : WideToUtf8(
                            publishError.empty() ? renderError : publishError);
                        frame->endedAt = std::chrono::system_clock::now();
                    }
                    RefreshExecutionStageLocked(job);
                    break;
                }
                std::error_code ignored;
                fs::remove_all(item.outputDir, ignored);
            }
            {
                std::scoped_lock lock(currentTaskMutex);
                currentTaskId.clear();
                currentJobId.clear();
                currentPackageHasStartedFrame = false;
                currentRenderingFrame.reset();
                currentActivity = {};
            }
            --workerAssigned;
            if (outputs.size() == item.frames.size())
                LogInfo("render package completed; package=" +
                        (item.packageId.empty() ? item.taskId : item.packageId));
            else if (retryAfterIdle)
                LogInfo("render package stopped after current frame; package=" +
                        (item.packageId.empty() ? item.taskId : item.packageId));
            else
                LogError("render package failed; package=" +
                         (item.packageId.empty() ? item.taskId : item.packageId) + "; error=" +
                         WideToUtf8(renderError));
            PersistJobs(item.jobId);
            Notify();
        }
    }
};

NodeApp::NodeApp(ConfigStore store, Config config, bool mockRenderer,
                 std::chrono::milliseconds assignmentGrace)
    : impl_(std::make_unique<Impl>(this, assignmentGrace)), store_(std::move(store)),
      config_(std::move(config)), mockRenderer_(mockRenderer) {
    impl_->LoadJobs();
    impl_->LoadContributions();
    impl_->LoadPendingCleanups();
}

NodeApp::~NodeApp() { Stop(); }

bool NodeApp::Start(std::wstring& error) {
    if (running_.exchange(true)) return true;
    WSADATA winsock{};
    if (WSAStartup(MAKEWORD(2, 2), &winsock) != 0) {
        running_ = false;
        error = L"无法启动 Windows 网络组件";
        return false;
    }
    impl_->stopRequested = false;
    // No import runs while the node is down, so every work folder left in
    // preflight\ (and inspection\ from 0.2.x) is an abandoned project copy.
    // Releases before 0.3 never removed them; each held a full packed copy.
    for (const wchar_t* folder : {L"preflight", L"inspection"}) {
        std::error_code ignored;
        const uintmax_t removed = fs::remove_all(ProjectStorageDir() / folder, ignored);
        if (removed > 0)
            LogInfo("removed abandoned import work files; folder=" + WideToUtf8(folder) +
                    "; entries=" + std::to_string(removed));
    }
    impl_->ConfigureServer();
    Config config = GetConfig();
    if (!impl_->server.bind_to_port("0.0.0.0", config.httpPort)) {
        running_ = false;
        WSACleanup();
        error = L"TCP 端口 " + std::to_wstring(config.httpPort) + L" 已被占用";
        LogError("failed to bind HTTP port " + std::to_string(config.httpPort));
        return false;
    }
    impl_->serverThread = std::thread([this] { impl_->server.listen_after_bind(); });
    impl_->discoveryThread = std::thread([this] { impl_->DiscoveryLoop(); });
    impl_->manualPeerThread = std::thread([this] { impl_->ManualPeerLoop(); });
    impl_->peerExchangeThread = std::thread([this] { impl_->PeerExchangeLoop(); });
    for (auto& thread : impl_->projectSyncThreads)
        thread = std::thread([this] { impl_->ProjectSyncLoop(); });
    impl_->schedulerThread = std::thread([this] { impl_->SchedulerLoop(); });
    impl_->workerThread = std::thread([this] { impl_->WorkerLoop(); });
    impl_->cacheThread = std::thread([this] { impl_->CacheMaintenanceLoop(); });
    LogInfo("node started; id=" + config.nodeId + "; port=" +
            std::to_string(config.httpPort));
    return true;
}

void NodeApp::Stop() {
    if (!running_.exchange(false)) return;
    impl_->stopRequested = true;
    impl_->server.stop();
    impl_->workCv.notify_all();
    if (impl_->serverThread.joinable()) impl_->serverThread.join();
    if (impl_->discoveryThread.joinable()) impl_->discoveryThread.join();
    if (impl_->manualPeerThread.joinable()) impl_->manualPeerThread.join();
    if (impl_->peerExchangeThread.joinable()) impl_->peerExchangeThread.join();
    for (auto& thread : impl_->projectSyncThreads)
        if (thread.joinable()) thread.join();
    if (impl_->schedulerThread.joinable()) impl_->schedulerThread.join();
    if (impl_->workerThread.joinable()) impl_->workerThread.join();
    if (impl_->cacheThread.joinable()) impl_->cacheThread.join();
    WSACleanup();
    LogInfo("node stopped");
}

void NodeApp::SetChangedCallback(std::function<void()> callback) {
    std::scoped_lock lock(impl_->callbackMutex);
    impl_->changed = std::move(callback);
}

Config NodeApp::GetConfig() const {
    std::scoped_lock lock(configMutex_);
    return config_;
}

fs::path NodeApp::ProjectStorageDir() const {
    std::scoped_lock lock(configMutex_);
    return config_.projectStoragePath.empty()
        ? store_.DataDir() : config_.projectStoragePath;
}

bool NodeApp::UpdateConfig(Config config, std::wstring& error) {
    Config current = GetConfig();
    if (config.projectStoragePath.empty())
        config.projectStoragePath = store_.DataDir();
    std::error_code storagePathError;
    config.projectStoragePath = fs::absolute(
        config.projectStoragePath, storagePathError).lexically_normal();
    if (storagePathError || !config.projectStoragePath.is_absolute() ||
        !fs::is_directory(config.projectStoragePath, storagePathError) ||
        storagePathError) {
        error = L"工程存储目录无效或无法访问";
        return false;
    }
    const bool matchingIdChanged = config.inviteCode != current.inviteCode;
    if (matchingIdChanged) {
        std::string reconnectEndpoint = current.manualPeerEndpoint;
        if (reconnectEndpoint.empty()) {
            for (const auto& peer : Peers()) {
                if (!peer.trusted || peer.address.empty() || peer.httpPort <= 0) continue;
                reconnectEndpoint = peer.address + ":" + std::to_string(peer.httpPort);
                break;
            }
        }
        // A visible matching-ID change defines a new trust group. Rotate the
        // internal credential immediately so nodes with different visible IDs
        // cannot remain authenticated, while retaining a known LAN route so two
        // nodes changed to the same new ID can pair again automatically.
        config.clusterToken = NewRandomHex(16);
        config.manualPeerEndpoint = std::move(reconnectEndpoint);
        config.pairingPending = true;
    }
    if (config.pairingPending && config.clusterToken.size() < 16)
        config.clusterToken = NewRandomHex(16);
    if (config.renderSoftwareProfiles.empty()) {
        config.renderSoftwareProfiles.push_back({
            "default-blender", "Blender", "Windows", "Blender", config.blenderPath});
    }
    std::set<std::string> profileIds;
    for (const auto& profile : config.renderSoftwareProfiles) {
        if (!SafeId(profile.id) || profile.name.empty() || profile.renderSystem.empty() ||
            profile.renderSoftware.empty() || !profileIds.insert(profile.id).second) {
            error = L"渲染软件配置无效";
            return false;
        }
    }
    auto selectedProfile = std::find_if(
        config.renderSoftwareProfiles.begin(), config.renderSoftwareProfiles.end(),
        [&](const RenderSoftwareProfile& profile) {
            return profile.id == config.lastRenderSoftwareProfileId;
        });
    if (selectedProfile == config.renderSoftwareProfiles.end()) {
        selectedProfile = config.renderSoftwareProfiles.begin();
        config.lastRenderSoftwareProfileId = selectedProfile->id;
    }
    config.blenderPath = selectedProfile->installPath;
    if (!config.autoStart) config.silentAutoStart = false;
    if (config.nodeName.empty() || !IsInviteCode(config.inviteCode) ||
        (!config.pairingPending && config.clusterToken.size() < 16) ||
        config.idleAfterSeconds < 1 || config.idleAfterSeconds > 86400 ||
        (config.theme != "system" && config.theme != "light" && config.theme != "dark") ||
        (config.closeBehavior != "ask" && config.closeBehavior != "exit" &&
         config.closeBehavior != "tray")) {
        error = L"设置值无效";
        return false;
    }
    if (!store_.Save(config, error)) return false;
    {
        std::scoped_lock lock(configMutex_);
        config_ = config;
    }
    if (matchingIdChanged) {
        std::scoped_lock lock(impl_->stateMutex);
        impl_->peers.clear();
    }
    impl_->Notify();
    LogInfo("settings updated for node " + config.nodeId);
    return true;
}

bool NodeApp::MigrateProjectStorage(
    const fs::path& destination,
    const std::function<void(int)>& progress,
    std::atomic<bool>* cancel,
    std::wstring& result) {
    if (Running()) {
        result = L"工程存储迁移前需要暂停节点服务";
        return false;
    }
    const fs::path currentRoot = ProjectStorageDir();
    fs::path oldRoot;
    fs::path newRoot;
    if (progress) progress(0);
    if (!CopyManagedStorageToNewRoot(
            currentRoot, destination, progress, cancel,
            oldRoot, newRoot, result))
        return false;

    std::wstring persistenceError;
    if (!impl_->PersistJobs({}, &persistenceError)) {
        std::error_code ignored;
        fs::remove_all(newRoot, ignored);
        result = L"无法把作业路径转换为可迁移格式：" + persistenceError;
        return false;
    }

    {
        std::scoped_lock lock(impl_->stateMutex);
        for (auto& job : impl_->jobs)
            RebaseJobStoragePaths(job, oldRoot, newRoot);
    }
    Config updated = GetConfig();
    updated.projectStoragePath = newRoot;
    if (!UpdateConfig(std::move(updated), persistenceError)) {
        {
            std::scoped_lock lock(impl_->stateMutex);
            for (auto& job : impl_->jobs)
                RebaseJobStoragePaths(job, newRoot, oldRoot);
        }
        std::error_code ignored;
        fs::remove_all(newRoot, ignored);
        result = L"无法切换工程存储目录：" + persistenceError;
        return false;
    }
    if (progress) progress(97);

    std::vector<fs::path> retained;
    for (const auto name : kManagedStorageDirectories) {
        const fs::path previous = oldRoot / name;
        std::error_code removeError;
        if (!fs::exists(previous, removeError)) continue;
        fs::remove_all(previous, removeError);
        if (removeError) retained.push_back(previous);
    }
    std::error_code rootError;
    const fs::path localDataRoot = fs::absolute(store_.DataDir(), rootError).lexically_normal();
    if (!rootError && !SameAbsolutePath(oldRoot, localDataRoot) &&
        fs::is_empty(oldRoot, rootError)) {
        rootError.clear();
        fs::remove(oldRoot, rootError);
    }
    if (progress) progress(100);
    if (retained.empty()) {
        result = L"工程存储已迁移到：" + newRoot.wstring();
    } else {
        result = L"工程存储已切换到新目录；以下旧目录仍被占用，可在关闭占用程序后手动删除：\n";
        for (const auto& path : retained) result += L"• " + path.wstring() + L"\n";
    }
    LogInfo("project storage migrated; from=" + WideToUtf8(oldRoot.wstring()) +
            "; to=" + WideToUtf8(newRoot.wstring()) +
            "; retained=" + std::to_string(retained.size()));
    impl_->Notify();
    return true;
}

bool NodeApp::AddManualPeer(const std::string& endpoint, std::wstring& result) {
    Peer peer;
    if (!impl_->ProbeManualPeer(endpoint, peer, result)) {
        impl_->SetManualStatus(result);
        impl_->Notify();
        return false;
    }
    Config config = GetConfig();
    config.manualPeerEndpoint = peer.address + ":" + std::to_string(peer.httpPort);
    std::wstring saveError;
    if (!UpdateConfig(config, saveError)) {
        result = L"节点验证成功，但无法保存地址：" + saveError;
        impl_->SetManualStatus(result);
        impl_->Notify();
        return false;
    }
    {
        std::scoped_lock lock(impl_->stateMutex);
        impl_->peers[peer.nodeId] = std::move(peer);
    }
    impl_->SetManualStatus(result);
    impl_->Notify();
    return true;
}

std::wstring NodeApp::ManualPeerStatus() const {
    std::scoped_lock lock(impl_->manualStatusMutex);
    return impl_->manualStatus;
}

std::vector<Peer> NodeApp::Peers() const {
    std::vector<Peer> result;
    auto now = std::chrono::steady_clock::now();
    std::scoped_lock lock(impl_->stateMutex);
    for (const auto& [id, peer] : impl_->peers)
        if (now - peer.lastSeen <= kPeerTimeout) result.push_back(peer);
    return result;
}

std::vector<RenderJob> NodeApp::Jobs() const {
    std::scoped_lock lock(impl_->stateMutex);
    return impl_->jobs;
}

JobAggregate NodeApp::JobStats() const {
    const uint64_t revision = impl_->jobRevision.load();
    std::scoped_lock cacheLock(impl_->jobStatsMutex);
    if (impl_->jobStatsRevision == revision) return impl_->cachedJobStats;
    JobAggregate aggregate;
    {
        std::scoped_lock stateLock(impl_->stateMutex);
        for (const auto& job : impl_->jobs) {
            bool active = false;
            for (const auto& frame : job.frames) {
                ++aggregate.totalFrames;
                aggregate.completedFrames += frame.state == TaskState::Completed;
                active = active || frame.state == TaskState::Assigned ||
                    frame.state == TaskState::Dispatching ||
                    frame.state == TaskState::Rendering;
            }
            aggregate.activeJobs += active;
        }
    }
    impl_->cachedJobStats = aggregate;
    impl_->jobStatsRevision = revision;
    return aggregate;
}

std::optional<RenderJob> NodeApp::FindJob(const std::string& jobId) const {
    if (!SafeId(jobId)) return std::nullopt;
    std::scoped_lock lock(impl_->stateMutex);
    auto found = std::find_if(impl_->jobs.begin(), impl_->jobs.end(),
                              [&](const RenderJob& job) { return job.id == jobId; });
    if (found == impl_->jobs.end()) return std::nullopt;
    return *found;
}

std::vector<ContributionRecord> NodeApp::Contributions() const {
    std::scoped_lock lock(impl_->stateMutex);
    return impl_->contributions;
}

bool NodeApp::DeleteContributionHistory(
    const std::vector<std::string>& packageIds, std::wstring& error) {
    std::set<std::string> selected;
    for (const auto& packageId : packageIds) {
        if (!SafeId(packageId)) {
            error = L"贡献记录 ID 无效";
            return false;
        }
        selected.insert(packageId);
    }
    if (selected.empty()) {
        error = L"没有选择要清理的贡献记录";
        return false;
    }
    std::vector<ContributionRecord> previous;
    size_t removed = 0;
    {
        std::scoped_lock lock(impl_->stateMutex);
        previous = impl_->contributions;
        const size_t before = impl_->contributions.size();
        std::erase_if(impl_->contributions, [&](const ContributionRecord& record) {
            return selected.contains(record.packageId);
        });
        removed = before - impl_->contributions.size();
    }
    if (!removed) {
        error = L"未找到选中的贡献记录";
        return false;
    }
    if (!impl_->PersistContributions(&error)) {
        std::scoped_lock lock(impl_->stateMutex);
        impl_->contributions = std::move(previous);
        return false;
    }
    impl_->Notify();
    LogInfo("contribution history removed=" + std::to_string(removed));
    return true;
}

bool NodeApp::StopJob(const std::string& jobId, std::wstring& error) {
    if (!SafeId(jobId)) {
        error = L"任务 ID 无效";
        return false;
    }
    std::vector<fs::path> stopMarkers;
    struct RemoteCancellation {
        std::string address;
        int port = 0;
        std::string packageId;
    };
    std::vector<RemoteCancellation> remoteCancellations;
    std::set<std::string> remoteKeys;
    bool found = false;
    auto now = std::chrono::system_clock::now();
    std::string localNodeId = GetConfig().nodeId;
    // Publish/copy operations may hold stateMutex to fence a lease. Signal them
    // first through the independent cancellation map so StopJob never waits for
    // a multi-gigabyte result copy to finish.
    impl_->JobCancellation(jobId)->store(true);
    {
        std::scoped_lock lock(impl_->stateMutex);
        for (auto& job : impl_->jobs) {
            if (job.id != jobId) continue;
            found = true;
            for (auto& frame : job.frames) {
                if (frame.state == TaskState::Completed || frame.state == TaskState::Failed ||
                    frame.state == TaskState::Stopped)
                    continue;
                if (!frame.assignedPackageId.empty() &&
                    frame.assignedNodeId == localNodeId)
                    stopMarkers.push_back(ProjectStorageDir() / L"jobs" /
                        Utf8ToWide(job.id) / L"packages" /
                                          Utf8ToWide(frame.assignedPackageId) /
                                          L"stop-after-current");
                if (!frame.assignedPackageId.empty() &&
                    !frame.assignedNodeId.empty() && frame.assignedNodeId != localNodeId) {
                    auto peer = impl_->peers.find(frame.assignedNodeId);
                    if (peer != impl_->peers.end()) {
                        std::string key = peer->second.address + ":" +
                            std::to_string(peer->second.httpPort) + "/" +
                            frame.assignedPackageId;
                        if (peer->second.supportsPackageCancellation &&
                            remoteKeys.insert(key).second)
                            remoteCancellations.push_back({peer->second.address,
                                peer->second.httpPort, frame.assignedPackageId});
                    }
                }
                frame.state = TaskState::Stopped;
                frame.endedAt = now;
                frame.assignedAt = {};
                frame.attemptToken.clear();
                frame.error.clear();
            }
            job.stage = JobStage::Stopped;
            break;
        }
    }
    if (!found) {
        error = L"未找到任务";
        return false;
    }
    for (const auto& marker : stopMarkers) {
        std::error_code ignored;
        fs::create_directories(marker.parent_path(), ignored);
        WriteFileText(marker, "stop");
    }
    Config config = GetConfig();
    for (const auto& cancellation : remoteCancellations) {
        http::Headers headers = {
            {"X-RM-Token", config.clusterToken},
            {"X-RM-Package", cancellation.packageId},
        };
        auto result = PostEmpty(cancellation.address, cancellation.port, "/v3/cancel",
                                headers, &impl_->stopRequested);
        if (!result || result->status != 202)
            LogWarning("worker cancellation was not acknowledged; package=" +
                       cancellation.packageId);
    }
    if (!impl_->PersistJobs(jobId, &error)) return false;
    impl_->Notify();
    LogInfo("job stopped; id=" + jobId);
    return true;
}

bool NodeApp::ResumeStoppedJob(const std::string& jobId, std::wstring& error) {
    if (!SafeId(jobId)) {
        error = L"任务 ID 无效";
        return false;
    }
    std::unique_lock stageGate(impl_->jobStageGateMutex);
    RenderJob previous;
    bool resumed = false;
    {
        std::scoped_lock lock(impl_->stateMutex);
        auto job = std::find_if(impl_->jobs.begin(), impl_->jobs.end(),
            [&](const RenderJob& value) { return value.id == jobId; });
        if (job == impl_->jobs.end()) {
            error = L"未找到任务";
            return false;
        }
        if (job->stage != JobStage::Stopped) {
            error = L"只有已停止的任务可以恢复渲染";
            return false;
        }
        if (job->artifactsPurgedAt.time_since_epoch() !=
                std::chrono::system_clock::duration::zero() ||
            !fs::is_regular_file(ProjectPayloadPath(*job))) {
            error = L"任务工程缓存已经清理，无法恢复渲染";
            return false;
        }
        previous = *job;
        for (auto& frame : job->frames) {
            if (frame.state != TaskState::Stopped) continue;
            resumed = true;
            frame.state = TaskState::Pending;
            frame.assignedNodeId.clear();
            frame.assignedNodeName.clear();
            frame.assignedPackageId.clear();
            frame.assignedAt = {};
            frame.attemptToken.clear();
            frame.output.clear();
            frame.log.clear();
            frame.artifacts.clear();
            frame.error.clear();
            frame.startedAt = {};
            frame.endedAt = {};
        }
        if (!resumed) {
            error = L"任务中没有可恢复的已停止帧";
            return false;
        }
        const bool testsComplete = job->testFrames.empty() ||
            std::all_of(job->testFrames.begin(), job->testFrames.end(),
                [&](int testFrame) {
                    auto frame = std::find_if(job->frames.begin(), job->frames.end(),
                        [&](const FrameTask& value) {
                            return value.frame == testFrame;
                        });
                    return frame != job->frames.end() &&
                        frame->state == TaskState::Completed;
                });
        job->stage = testsComplete ? JobStage::Rendering : JobStage::Testing;
        job->finishedAt = {};
    }
    if (!impl_->PersistJobs(jobId, &error)) {
        std::scoped_lock lock(impl_->stateMutex);
        auto job = std::find_if(impl_->jobs.begin(), impl_->jobs.end(),
            [&](const RenderJob& value) { return value.id == jobId; });
        if (job != impl_->jobs.end()) *job = std::move(previous);
        return false;
    }
    {
        std::scoped_lock lock(impl_->stateMutex);
        std::erase_if(impl_->rejectedJobPeers,
            [&](const auto& entry) { return entry.first == jobId; });
        std::erase_if(impl_->jobPeerBackoff,
            [&](const auto& entry) { return entry.first.first == jobId; });
    }
    impl_->BeginNewJobExecution(jobId);
    impl_->Notify();
    LogInfo("stopped frames requeued; job=" + jobId);
    return true;
}

bool NodeApp::RetryFailedFrames(const std::string& jobId, std::wstring& error) {
    if (!SafeId(jobId)) {
        error = L"任务 ID 无效";
        return false;
    }
    RenderJob previous;
    bool retried = false;
    {
        std::scoped_lock lock(impl_->stateMutex);
        auto job = std::find_if(impl_->jobs.begin(), impl_->jobs.end(),
            [&](const RenderJob& value) { return value.id == jobId; });
        if (job == impl_->jobs.end()) {
            error = L"未找到任务";
            return false;
        }
        if (job->artifactsPurgedAt.time_since_epoch() !=
                std::chrono::system_clock::duration::zero() ||
            !fs::is_regular_file(ProjectPayloadPath(*job))) {
            error = L"任务工程缓存已经清理，无法重新渲染";
            return false;
        }
        previous = *job;
        for (auto& frame : job->frames) {
            if (frame.state != TaskState::Failed) continue;
            retried = true;
            frame.state = TaskState::Pending;
            frame.assignedNodeId.clear();
            frame.assignedNodeName.clear();
            frame.assignedPackageId.clear();
            frame.assignedAt = {};
            frame.attemptToken.clear();
            frame.output.clear();
            frame.log.clear();
            frame.artifacts.clear();
            frame.error.clear();
            frame.startedAt = {};
            frame.endedAt = {};
        }
        if (!retried) {
            error = L"任务中没有失败帧";
            return false;
        }
        job->stage = previous.stage == JobStage::TestFailed
            ? JobStage::Testing : JobStage::Rendering;
        job->finishedAt = {};
    }
    if (!impl_->PersistJobs(jobId, &error)) {
        std::scoped_lock lock(impl_->stateMutex);
        auto job = std::find_if(impl_->jobs.begin(), impl_->jobs.end(),
            [&](const RenderJob& value) { return value.id == jobId; });
        if (job != impl_->jobs.end()) *job = std::move(previous);
        return false;
    }
    impl_->JobCancellation(jobId)->store(false);
    impl_->Notify();
    LogInfo("failed frames requeued; job=" + jobId);
    return true;
}

bool NodeApp::RetryFrame(
    const std::string& jobId, const std::string& taskId, std::wstring& error) {
    if (!SafeId(jobId) || !SafeId(taskId)) {
        error = L"任务或帧 ID 无效";
        return false;
    }
    RenderJob previous;
    {
        std::scoped_lock lock(impl_->stateMutex);
        auto job = std::find_if(impl_->jobs.begin(), impl_->jobs.end(),
            [&](const RenderJob& value) { return value.id == jobId; });
        if (job == impl_->jobs.end()) {
            error = L"未找到任务";
            return false;
        }
        if (job->artifactsPurgedAt.time_since_epoch() !=
                std::chrono::system_clock::duration::zero() ||
            !fs::is_regular_file(ProjectPayloadPath(*job))) {
            error = L"任务工程缓存已经清理，无法重新渲染";
            return false;
        }
        auto frame = std::find_if(job->frames.begin(), job->frames.end(),
            [&](const FrameTask& value) { return value.taskId == taskId; });
        if (frame == job->frames.end()) {
            error = L"未找到帧";
            return false;
        }
        if (frame->state != TaskState::Failed) {
            error = L"只有失败帧可以重新渲染";
            return false;
        }
        previous = *job;
        frame->state = TaskState::Pending;
        frame->assignedNodeId.clear();
        frame->assignedNodeName.clear();
        frame->assignedPackageId.clear();
        frame->assignedAt = {};
        frame->attemptToken.clear();
        frame->output.clear();
        frame->log.clear();
        frame->artifacts.clear();
        frame->error.clear();
        frame->startedAt = {};
        frame->endedAt = {};
        job->stage = previous.stage == JobStage::TestFailed
            ? JobStage::Testing : JobStage::Rendering;
        job->finishedAt = {};
    }
    if (!impl_->PersistJobs(jobId, &error)) {
        std::scoped_lock lock(impl_->stateMutex);
        auto job = std::find_if(impl_->jobs.begin(), impl_->jobs.end(),
            [&](const RenderJob& value) { return value.id == jobId; });
        if (job != impl_->jobs.end()) *job = std::move(previous);
        return false;
    }
    impl_->JobCancellation(jobId)->store(false);
    impl_->Notify();
    LogInfo("failed frame requeued; job=" + jobId + "; task=" + taskId);
    return true;
}

bool NodeApp::SetJobPriority(
    const std::string& jobId, bool prioritised, std::wstring& error) {
    if (!SafeId(jobId)) {
        error = L"任务 ID 无效";
        return false;
    }
    bool found = false;
    int previousPriority = 0;
    {
        std::scoped_lock lock(impl_->stateMutex);
        for (auto& job : impl_->jobs) {
            if (job.id != jobId) continue;
            previousPriority = job.priority;
            job.priority = prioritised ? 100 : 0;
            found = true;
            break;
        }
    }
    if (!found) {
        error = L"未找到任务";
        return false;
    }
    if (!impl_->PersistJobs(jobId, &error)) {
        std::scoped_lock lock(impl_->stateMutex);
        for (auto& job : impl_->jobs)
            if (job.id == jobId) job.priority = previousPriority;
        return false;
    }
    impl_->Notify();
    return true;
}

bool NodeApp::SaveJobAs(
    const std::string& jobId, const fs::path& destination,
    std::wstring& error, std::atomic<bool>* cancel) const {
    if (!SafeId(jobId) || destination.empty()) {
        error = L"另存位置无效";
        return false;
    }
    RenderJob job;
    bool found = false;
    {
        std::scoped_lock lock(impl_->stateMutex);
        auto current = std::find_if(impl_->jobs.begin(), impl_->jobs.end(),
                                    [&](const RenderJob& value) {
                                        return value.id == jobId;
                                    });
        if (current != impl_->jobs.end()) {
            job = *current;
            found = true;
        }
    }
    if (!found) {
        error = L"未找到任务";
        return false;
    }
    if (job.artifactsPurgedAt.time_since_epoch() !=
        std::chrono::system_clock::duration::zero()) {
        error = L"该作业已经过期，托管结果已清理";
        return false;
    }
    std::error_code ec;
    fs::path absoluteDestination = fs::absolute(destination, ec).lexically_normal();
    if (ec) {
        error = L"无法解析另存位置";
        return false;
    }
    fs::create_directories(absoluteDestination, ec);
    if (ec) {
        error = L"无法创建另存目录";
        return false;
    }
    const std::string operation = NewRandomHex(8);
    fs::path staging = absoluteDestination /
        Utf8ToWide(".rendermesh-save-" + job.id + "-" + operation);
    fs::path final = absoluteDestination /
        (SafeExportFolderComponent(Utf8ToWide(job.name)) + L"_" + Utf8ToWide(job.id));
    final = final.lexically_normal();
    if (final.parent_path() != absoluteDestination) {
        error = L"另存目标超出所选目录，已拒绝操作";
        return false;
    }
    if (fs::exists(final, ec)) {
        error = L"目标位置已经存在同名作业目录";
        return false;
    }
    fs::create_directories(staging / L"primary", ec);
    if (ec) {
        error = L"无法创建另存暂存目录";
        return false;
    }
    const bool completeSequence = !job.frames.empty() &&
        std::all_of(job.frames.begin(), job.frames.end(), [&](const FrameTask& frame) {
            std::error_code fileError;
            return frame.state == TaskState::Completed && !frame.output.empty() &&
                fs::is_regular_file(frame.output, fileError);
        });
    if (!completeSequence) {
        error = L"作业尚未完整渲染，不能另存为完整序列";
        fs::remove_all(staging, ec);
        return false;
    }
    size_t copied = 0;
    json manifest{{"job_id", job.id},
                  {"scene_name", job.sceneName.empty() ? job.name : job.sceneName},
                  {"created_at", UnixMilliseconds(job.createdAt)},
                  {"files", json::array()}};
    for (const auto& frame : job.frames) {
        fs::path target = staging / L"primary" / frame.output.filename();
        if (!CopyJobFile(frame.output, target, cancel)) {
            if (cancel && cancel->load()) error = L"另存操作已取消";
            else
            error = L"无法复制渲染序列：" + frame.output.wstring();
            fs::remove_all(staging, ec);
            return false;
        }
        manifest["files"].push_back(WideToUtf8(
            (fs::path(L"primary") / target.filename()).generic_wstring()));
        ++copied;
    }
    std::set<std::wstring> copiedArtifacts;
    for (const auto& frame : job.frames) {
        for (const auto& [logical, source] : frame.artifacts) {
            if (logical == "primary" || !logical.starts_with("compositor/") ||
                !fs::is_regular_file(source, ec)) {
                ec.clear();
                continue;
            }
            if (logical.find('\\') != std::string::npos) continue;
            fs::path relative = Utf8ToWide(logical);
            bool safe = relative.is_relative() && !relative.has_root_name() &&
                !relative.has_root_directory();
            for (const auto& component : relative)
                safe = safe && component != L".." && component != L".";
            if (!safe || !copiedArtifacts.insert(relative.wstring()).second) continue;
            fs::path targetPath = staging / relative;
            fs::create_directories(targetPath.parent_path(), ec);
            if (ec || !CopyJobFile(source, targetPath, cancel)) {
                if (cancel && cancel->load()) error = L"另存操作已取消";
                else
                error = L"无法复制合成器输出：" + source.wstring();
                fs::remove_all(staging, ec);
                return false;
            }
            manifest["files"].push_back(WideToUtf8(relative.generic_wstring()));
        }
    }
    if (!copied) {
        error = L"该作业没有可另存的已完成序列";
        fs::remove_all(staging, ec);
        return false;
    }
    std::wstring writeError;
    if (!WriteDurableFile(staging / L"manifest.json", manifest.dump(2), writeError)) {
        error = writeError;
        fs::remove_all(staging, ec);
        return false;
    }
    if (!MoveFileExW(staging.c_str(), final.c_str(), MOVEFILE_WRITE_THROUGH)) {
        error = L"无法发布另存结果：" + LastErrorMessage();
        fs::remove_all(staging, ec);
        return false;
    }
    return true;
}

bool NodeApp::DeleteJob(const std::string& jobId, std::wstring& error) {
    if (!SafeId(jobId)) {
        error = L"任务 ID 无效";
        return false;
    }
    bool active = false;
    {
        std::scoped_lock lock(impl_->stateMutex);
        auto found = std::find_if(impl_->jobs.begin(), impl_->jobs.end(),
                                  [&](const RenderJob& job) { return job.id == jobId; });
        if (found == impl_->jobs.end()) {
            error = L"未找到任务";
            return false;
        }
        active = std::any_of(found->frames.begin(), found->frames.end(),
                             [](const FrameTask& frame) {
                                 return frame.state == TaskState::Pending ||
                                     frame.state == TaskState::Dispatching ||
                                     frame.state == TaskState::Assigned ||
                                     frame.state == TaskState::Rendering;
                             });
    }
    if (active && !StopJob(jobId, error)) return false;
    if (active || impl_->IsJobRendering(jobId)) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (impl_->IsJobRendering(jobId) && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
        if (impl_->IsJobRendering(jobId)) {
            error = L"作业已停止；当前帧结束后请再次删除，托管文件暂时保留";
            return false;
        }
    }
    RenderJob removed;
    {
        std::scoped_lock lock(impl_->stateMutex);
        auto found = std::find_if(impl_->jobs.begin(), impl_->jobs.end(),
                                  [&](const RenderJob& job) { return job.id == jobId; });
        if (found == impl_->jobs.end()) return true;
        removed = *found;
    }
    const auto managedDirectory = ManagedJobDirectory(ProjectStorageDir(), jobId);
    if (!managedDirectory) {
        error = L"作业托管路径无效，已拒绝删除";
        return false;
    }
    const fs::path stagedDirectory = managedDirectory->parent_path() /
        Utf8ToWide(".deleting-" + jobId + "-" + NewRandomHex(6));
    std::error_code moveError;
    const bool hadManagedDirectory = fs::exists(*managedDirectory, moveError);
    if (hadManagedDirectory) {
        fs::rename(*managedDirectory, stagedDirectory, moveError);
        if (moveError) {
            error = L"本地托管文件正被占用，作业已停止；请关闭占用程序后重试删除";
            return false;
        }
    }
    if (!impl_->DeletePersistedJob(jobId, error)) {
        if (hadManagedDirectory) {
            std::error_code restoreError;
            fs::rename(stagedDirectory, *managedDirectory, restoreError);
            if (restoreError)
                LogError("job delete rollback could not restore managed directory; job=" + jobId);
        }
        return false;
    }
    {
        std::scoped_lock lock(impl_->stateMutex);
        auto found = std::find_if(impl_->jobs.begin(), impl_->jobs.end(),
                                  [&](const RenderJob& job) { return job.id == jobId; });
        if (found == impl_->jobs.end()) return true;
        impl_->jobs.erase(found);
    }
    impl_->QueueRemoteCleanup(removed);
    impl_->ProcessPendingCleanups();
    if (hadManagedDirectory) {
        std::error_code removeError;
        fs::remove_all(stagedDirectory, removeError);
        if (removeError)
            LogWarning("local job deletion staged for retry; job=" + jobId);
    }
    impl_->Notify();
    LogInfo("job deleted; id=" + jobId);
    return true;
}

HardwareInfo NodeApp::Hardware() const { return impl_->hardware; }

int NodeApp::AssignedCount() const {
    return impl_->workerAssigned.load();
}

WorkerActivity NodeApp::CurrentActivity() const {
    return impl_->CurrentActivity();
}

uint64_t NodeApp::JobRevision() const { return impl_->jobRevision.load(); }

void NodeApp::InjectPeerForTest(Peer peer) {
    peer.lastSeen = std::chrono::steady_clock::now();
    std::scoped_lock lock(impl_->stateMutex);
    impl_->peers[peer.nodeId] = std::move(peer);
}

void NodeApp::RemovePeerForTest(const std::string& nodeId) {
    std::scoped_lock lock(impl_->stateMutex);
    impl_->peers.erase(nodeId);
}

namespace {

bool ValidPreparedReport(const PreflightReport& report, std::wstring& error) {
    if (!report.passed || !fs::is_regular_file(report.preparedProject) ||
        report.frameStart < kBlenderFrameMin || report.frameEnd < report.frameStart ||
        report.frameEnd > kBlenderFrameMax || report.frameStep < 1 ||
        report.outputFormat.empty() || !IsSafeOutputExtension(report.outputExtension) ||
        !FindRenderAdapter(report.software)) {
        error = L"工程未通过预检或工程参数无效";
        return false;
    }
    return true;
}

// Fills a new job from a prepared report and snapshots its project.
bool InitializeJob(RenderJob& job, std::string name, const PreflightReport& report,
                   const fs::path& storageRoot, int frameStart, int frameEnd, int frameStep,
                   std::wstring& error, std::atomic<bool>* cancel,
                   const std::function<void(uint64_t, uint64_t)>& progress) {
    job.id = NewRandomHex(8);
    job.name = std::move(name);
    job.software = report.software.empty() ? std::string("blender") : report.software;
    job.sceneName = report.sceneName.empty()
        ? WideToUtf8(report.sourceProject.filename().wstring()) : report.sceneName;
    job.sourceProject = report.sourceProject;
    job.outputFormat = report.outputFormat;
    job.outputExtension = report.outputExtension;
    job.softwareVersion = report.softwareVersion;
    job.renderEngine = report.renderEngine;
    job.requiresGpu = report.requiresGpu;
    job.primaryOutputs = report.primaryOutputs;
    job.compositorOutputs = report.compositorOutputs;
    job.createdAt = std::chrono::system_clock::now();
    for (int frame = frameStart; frame <= frameEnd; frame += frameStep) {
        FrameTask task;
        task.taskId = job.id + "-" + std::to_string(frame);
        task.frame = frame;
        job.frames.push_back(std::move(task));
        if (job.frames.size() > 100000) {
            error = L"帧数超过 100000，已拒绝";
            return false;
        }
        if (frame > frameEnd - frameStep) break;
    }
    const fs::path jobDirectory = storageRoot / L"jobs" / Utf8ToWide(job.id);
    std::error_code ignored;
    fs::create_directories(jobDirectory / L"outputs", ignored);
    ProjectSnapshot snapshot;
    if (!SnapshotProject(report, jobDirectory, snapshot, error, cancel, progress)) {
        fs::remove_all(jobDirectory, ignored);
        return false;
    }
    job.preparedProject = snapshot.project;
    job.projectPayload = snapshot.payload;
    job.projectBundle = snapshot.bundle;
    job.projectHash = snapshot.hash;
    return true;
}

}  // namespace

std::string NodeApp::SubmitPrepared(
    std::string name,
    const PreflightReport& report,
    int frameStart,
    int frameEnd,
    int frameStep,
    std::wstring& error) {
    if (!ValidPreparedReport(report, error)) return {};
    if (name.empty() || frameStart < kBlenderFrameMin || frameEnd < frameStart ||
        frameEnd > kBlenderFrameMax || frameStep < 1) {
        error = L"任务参数无效";
        return {};
    }
    RenderJob job;
    if (!InitializeJob(job, std::move(name), report, ProjectStorageDir(), frameStart, frameEnd,
                       frameStep, error, nullptr, {}))
        return {};
    {
        std::scoped_lock lock(impl_->stateMutex);
        impl_->jobs.push_back(job);
    }
    if (!impl_->PersistJobs(job.id, &error)) {
        {
            std::scoped_lock lock(impl_->stateMutex);
            std::erase_if(impl_->jobs, [&](const RenderJob& value) { return value.id == job.id; });
        }
        std::error_code ignored;
        fs::remove_all(ProjectStorageDir() / L"jobs" / Utf8ToWide(job.id), ignored);
        return {};
    }
    impl_->Notify();
    LogInfo("job submitted; id=" + job.id + "; frames=" +
            std::to_string(job.frames.size()) + "; format=" + job.outputFormat);
    return job.id;
}

std::string NodeApp::CreatePreparedDraft(
    std::string name,
    const PreflightReport& report,
    std::wstring& error,
    std::atomic<bool>* cancel,
    const std::function<void(uint64_t, uint64_t)>& progress) {
    if (!ValidPreparedReport(report, error)) return {};
    if (name.empty()) {
        error = L"任务名称不能为空";
        return {};
    }
    RenderJob job;
    if (!InitializeJob(job, std::move(name), report, ProjectStorageDir(), report.frameStart,
                       report.frameEnd, report.frameStep, error, cancel, progress))
        return {};
    const std::string id = job.id;
    const fs::path jobDir = ProjectStorageDir() / L"jobs" / Utf8ToWide(id);
    job.stage = JobStage::Uploading;
    job.packageSize = 5;
    std::error_code sizeError;
    const uint64_t total = fs::file_size(ProjectPayloadPath(job), sizeError);
    {
        std::scoped_lock lock(impl_->stateMutex);
        const auto now = std::chrono::steady_clock::now();
        for (const auto& [nodeId, peer] : impl_->peers) {
            if (now - peer.lastSeen > kPeerTimeout || !peer.trusted ||
                !peer.shareEnabled || !peer.supportsProjectCache || !peer.rendererReady ||
                (job.projectBundle && !PeerSupportsProjectBundle(peer)))
                continue;
            ProjectTransfer transfer;
            transfer.nodeId = nodeId;
            transfer.nodeName = peer.name;
            transfer.address = peer.address;
            transfer.httpPort = peer.httpPort;
            transfer.cpuModel = peer.cpuModel;
            transfer.gpuModels = peer.gpuModels;
            transfer.totalBytes = sizeError ? 0 : total;
            job.projectTransfers.push_back(std::move(transfer));
        }
    }
    if (job.projectTransfers.empty()) job.stage = JobStage::ReadyToConfigure;
    auto discard = [&](const wchar_t* message) {
        if (message) error = message;
        std::error_code ignored;
        fs::remove_all(jobDir, ignored);
        return std::string{};
    };
    if (cancel && cancel->load()) return discard(L"工程准备已取消");
    if (!impl_->PersistDetachedJob(job, &error)) return discard(nullptr);
    bool cancelledBeforeCommit = false;
    {
        std::scoped_lock lock(impl_->stateMutex);
        if (cancel && cancel->load()) {
            cancelledBeforeCommit = true;
        } else {
            // This insertion is the commit point. ProjectSyncLoop cannot see the
            // job, and therefore cannot start a remote upload, until the durable
            // record exists and the final cancellation check has passed.
            impl_->jobs.push_back(job);
        }
    }
    if (cancelledBeforeCommit) {
        std::wstring rollbackError;
        if (impl_->DeletePersistedJob(id, rollbackError)) return discard(L"工程准备已取消");
        // The durable record could not be removed. Publish the job instead of
        // leaving an invisible task that would reappear after restart.
        {
            std::scoped_lock lock(impl_->stateMutex);
            impl_->jobs.push_back(job);
        }
        LogWarning("task cancellation rollback failed; durable job was retained; job=" +
                   id + "; error=" + WideToUtf8(rollbackError));
    }
    impl_->Notify();
    LogInfo("staged job created; id=" + id + "; participants=" +
            std::to_string(job.projectTransfers.size()) + "; bytes=" + std::to_string(total));
    return id;
}

bool NodeApp::ConfigureAndStartTest(
    const std::string& jobId,
    int frameStart,
    int frameEnd,
    int frameStep,
    size_t packageSize,
    const std::wstring& testFrameExpression,
    std::wstring& error) {
    if (!SafeId(jobId) || frameStart < kBlenderFrameMin || frameEnd < frameStart ||
        frameEnd > kBlenderFrameMax || frameStep < 1 || packageSize < 1 ||
        packageSize > 20) {
        error = L"渲染配置无效";
        return false;
    }
    std::vector<int> allowed;
    for (int frame = frameStart; frame <= frameEnd; frame += frameStep) {
        allowed.push_back(frame);
        if (allowed.size() > 100000) {
            error = L"帧数超过 100000，已拒绝";
            return false;
        }
        if (frame > frameEnd - frameStep) break;
    }
    std::vector<int> tests;
    if (!ParseFrameSelection(testFrameExpression, allowed, tests, error)) return false;
    std::unique_lock stageGate(impl_->jobStageGateMutex);
    RenderJob previous;
    {
        std::scoped_lock lock(impl_->stateMutex);
        auto job = std::find_if(impl_->jobs.begin(), impl_->jobs.end(),
            [&](const RenderJob& value) { return value.id == jobId; });
        if (job == impl_->jobs.end()) {
            error = L"未找到任务";
            return false;
        }
        if (job->stage != JobStage::ReadyToConfigure) {
            error = L"工程尚未在所有参与电脑上传完成";
            return false;
        }
        const int detectedMin = job->frames.empty() ? frameStart : job->frames.front().frame;
        const int detectedMax = job->frames.empty() ? frameEnd : job->frames.back().frame;
        if (frameStart < detectedMin || frameEnd > detectedMax) {
            error = L"手动帧范围不能超出工程检测范围";
            return false;
        }
        previous = *job;
        job->frames.clear();
        for (int frame : allowed) {
            FrameTask task;
            task.taskId = jobId + "-" + std::to_string(frame);
            task.frame = frame;
            job->frames.push_back(std::move(task));
        }
        job->packageSize = packageSize;
        job->testFrames = std::move(tests);
        Config config = GetConfig();
        auto profile = std::find_if(
            config.renderSoftwareProfiles.begin(), config.renderSoftwareProfiles.end(),
            [&](const RenderSoftwareProfile& value) {
                return value.id == config.lastRenderSoftwareProfileId;
            });
        if (profile != config.renderSoftwareProfiles.end()) {
            job->renderSoftwareProfileId = profile->id;
            job->renderSoftwareProfileName = profile->name;
            job->softwareExecutable = profile->installPath;
        } else {
            job->softwareExecutable = config.blenderPath;
        }
        job->stage = JobStage::Testing;
        job->finishedAt = {};
    }
    if (!impl_->PersistJobs(jobId, &error)) {
        std::scoped_lock lock(impl_->stateMutex);
        auto job = std::find_if(impl_->jobs.begin(), impl_->jobs.end(),
            [&](const RenderJob& value) { return value.id == jobId; });
        if (job != impl_->jobs.end()) *job = std::move(previous);
        return false;
    }
    impl_->Notify();
    LogInfo("test render started; job=" + jobId);
    return true;
}

bool NodeApp::StartFullRender(const std::string& jobId, std::wstring& error) {
    if (!SafeId(jobId)) {
        error = L"任务 ID 无效";
        return false;
    }
    std::unique_lock stageGate(impl_->jobStageGateMutex);
    JobStage previousStage = JobStage::TestCompleted;
    {
        std::scoped_lock lock(impl_->stateMutex);
        auto job = std::find_if(impl_->jobs.begin(), impl_->jobs.end(),
            [&](const RenderJob& value) { return value.id == jobId; });
        if (job == impl_->jobs.end()) {
            error = L"未找到任务";
            return false;
        }
        if (job->stage != JobStage::TestCompleted) {
            error = L"只有测试完成的任务才能开始全速渲染";
            return false;
        }
        previousStage = job->stage;
        job->stage = JobStage::Rendering;
    }
    if (!impl_->PersistJobs(jobId, &error)) {
        std::scoped_lock lock(impl_->stateMutex);
        auto job = std::find_if(impl_->jobs.begin(), impl_->jobs.end(),
            [&](const RenderJob& value) { return value.id == jobId; });
        if (job != impl_->jobs.end()) job->stage = previousStage;
        return false;
    }
    impl_->Notify();
    LogInfo("full render released; job=" + jobId);
    return true;
}

bool NodeApp::ExcludeProjectTransfer(
    const std::string& jobId, const std::string& nodeId, std::wstring& error) {
    {
        std::scoped_lock lock(impl_->stateMutex);
        auto job = std::find_if(impl_->jobs.begin(), impl_->jobs.end(),
            [&](const RenderJob& value) { return value.id == jobId; });
        if (job == impl_->jobs.end()) {
            error = L"未找到任务";
            return false;
        }
        if (job->stage != JobStage::Uploading && job->stage != JobStage::UploadFailed) {
            error = L"当前阶段不能移出参与电脑";
            return false;
        }
        auto transfer = std::find_if(job->projectTransfers.begin(), job->projectTransfers.end(),
            [&](const ProjectTransfer& value) { return value.nodeId == nodeId; });
        if (transfer == job->projectTransfers.end()) {
            error = L"未找到该上传节点";
            return false;
        }
        const size_t remaining = std::count_if(
            job->projectTransfers.begin(), job->projectTransfers.end(),
            [](const ProjectTransfer& value) {
                return value.state != ProjectTransferState::Excluded;
            });
        if (remaining <= 1) {
            error = L"测试帧必须保留至少一台远程电脑";
            return false;
        }
        transfer->state = ProjectTransferState::Excluded;
        transfer->error.clear();
        impl_->RefreshUploadStageLocked(*job);
    }
    if (!impl_->PersistJobs(jobId, &error)) return false;
    impl_->Notify();
    return true;
}

bool NodeApp::RetryProjectTransfer(
    const std::string& jobId, const std::string& nodeId, std::wstring& error) {
    {
        std::scoped_lock lock(impl_->stateMutex);
        auto job = std::find_if(impl_->jobs.begin(), impl_->jobs.end(),
            [&](const RenderJob& value) { return value.id == jobId; });
        if (job == impl_->jobs.end()) {
            error = L"未找到任务";
            return false;
        }
        auto transfer = std::find_if(job->projectTransfers.begin(), job->projectTransfers.end(),
            [&](const ProjectTransfer& value) { return value.nodeId == nodeId; });
        if (transfer == job->projectTransfers.end() ||
            (transfer->state != ProjectTransferState::Failed &&
             transfer->state != ProjectTransferState::Offline)) {
            error = L"该节点当前不需要重试";
            return false;
        }
        transfer->state = ProjectTransferState::Pending;
        transfer->attempts = 0;
        transfer->error.clear();
        job->stage = JobStage::Uploading;
    }
    if (!impl_->PersistJobs(jobId, &error)) return false;
    impl_->Notify();
    return true;
}

bool NodeApp::SetJobParticipants(
    const std::string& jobId, const std::vector<std::string>& nodeIds,
    std::wstring& error) {
    if (!SafeId(jobId)) {
        error = L"任务 ID 无效";
        return false;
    }
    if (nodeIds.size() > 1024) {
        error = L"渲染主机数量超过限制";
        return false;
    }
    std::set<std::string> selected;
    const std::string localNodeId = GetConfig().nodeId;
    for (const auto& nodeId : nodeIds) {
        if (!SafeId(nodeId) || nodeId == localNodeId) {
            error = L"渲染主机 ID 无效";
            return false;
        }
        selected.insert(nodeId);
    }

    std::unique_lock stageGate(impl_->jobStageGateMutex);
    RenderJob previous;
    bool changed = false;
    {
        std::scoped_lock lock(impl_->stateMutex);
        auto job = std::find_if(impl_->jobs.begin(), impl_->jobs.end(),
            [&](const RenderJob& value) { return value.id == jobId; });
        if (job == impl_->jobs.end()) {
            error = L"未找到任务";
            return false;
        }
        if (job->stage == JobStage::Completed ||
            job->artifactsPurgedAt.time_since_epoch() !=
                std::chrono::system_clock::duration::zero()) {
            error = L"已完成或已清理的任务不能修改渲染主机";
            return false;
        }
        previous = *job;
        const JobStage previousStage = job->stage;
        std::error_code sizeError;
        const uint64_t totalBytes = fs::file_size(ProjectPayloadPath(*job), sizeError);
        const auto now = std::chrono::steady_clock::now();

        // Validate every newly selected node before mutating the live job so a
        // single stale/offline selection cannot leave a partial in-memory edit.
        for (const auto& nodeId : selected) {
            const auto transfer = std::find_if(
                job->projectTransfers.begin(), job->projectTransfers.end(),
                [&](const ProjectTransfer& value) { return value.nodeId == nodeId; });
            if (transfer != job->projectTransfers.end()) continue;
            const auto peer = impl_->peers.find(nodeId);
            const bool peerEligible = peer != impl_->peers.end() &&
                now - peer->second.lastSeen <= kPeerTimeout &&
                peer->second.trusted && peer->second.shareEnabled &&
                peer->second.supportsProjectCache && peer->second.rendererReady &&
                (!job->projectBundle || PeerSupportsProjectBundle(peer->second));
            if (!peerEligible) {
                error = L"所选的新渲染主机当前离线、未共享算力或没有可用的 Blender";
                return false;
            }
        }
        for (const auto& nodeId : selected) {
            auto transfer = std::find_if(
                job->projectTransfers.begin(), job->projectTransfers.end(),
                [&](const ProjectTransfer& value) { return value.nodeId == nodeId; });
            auto peer = impl_->peers.find(nodeId);
            const bool peerEligible = peer != impl_->peers.end() &&
                now - peer->second.lastSeen <= kPeerTimeout &&
                peer->second.trusted && peer->second.shareEnabled &&
                peer->second.supportsProjectCache && peer->second.rendererReady &&
                (!job->projectBundle || PeerSupportsProjectBundle(peer->second));
            if (transfer == job->projectTransfers.end()) {
                if (!peerEligible) {
                    error = L"所选的新渲染主机当前离线、未共享算力或没有可用的 Blender";
                    return false;
                }
                ProjectTransfer added;
                added.nodeId = nodeId;
                added.nodeName = peer->second.name;
                added.address = peer->second.address;
                added.httpPort = peer->second.httpPort;
                added.cpuModel = peer->second.cpuModel;
                added.gpuModels = peer->second.gpuModels;
                added.totalBytes = sizeError ? 0 : totalBytes;
                job->projectTransfers.push_back(std::move(added));
                changed = true;
                continue;
            }
            if (peer != impl_->peers.end()) {
                transfer->nodeName = peer->second.name;
                transfer->address = peer->second.address;
                transfer->httpPort = peer->second.httpPort;
                transfer->cpuModel = peer->second.cpuModel;
                transfer->gpuModels = peer->second.gpuModels;
            }
            if (!sizeError) transfer->totalBytes = totalBytes;
            if (transfer->state == ProjectTransferState::Excluded) {
                transfer->state = peerEligible
                    ? ProjectTransferState::Pending
                    : ProjectTransferState::Offline;
                transfer->transferredBytes = 0;
                transfer->bytesPerSecond = 0;
                transfer->attempts = 0;
                transfer->error = peerEligible ? "" : "节点离线或未开启共享";
                changed = true;
            }
        }
        for (auto& transfer : job->projectTransfers) {
            if (selected.contains(transfer.nodeId)) continue;
            if (transfer.state != ProjectTransferState::Excluded) changed = true;
            transfer.state = ProjectTransferState::Excluded;
            transfer.bytesPerSecond = 0;
            transfer.error.clear();
        }

        const bool uploadWorkflow = previousStage == JobStage::Uploading ||
            previousStage == JobStage::UploadFailed ||
            previousStage == JobStage::ReadyToConfigure;
        if (uploadWorkflow) {
            job->stage = JobStage::Uploading;
            impl_->RefreshUploadStageLocked(*job);
        }
        changed = changed || job->stage != previous.stage;
    }
    if (!changed) return true;
    if (!impl_->PersistJobs(jobId, &error)) {
        std::scoped_lock lock(impl_->stateMutex);
        auto job = std::find_if(impl_->jobs.begin(), impl_->jobs.end(),
            [&](const RenderJob& value) { return value.id == jobId; });
        if (job != impl_->jobs.end()) *job = std::move(previous);
        return false;
    }
    {
        std::scoped_lock lock(impl_->stateMutex);
        std::erase_if(impl_->rejectedJobPeers, [&](const auto& entry) {
            return entry.first == jobId && selected.contains(entry.second);
        });
        std::erase_if(impl_->jobPeerBackoff, [&](const auto& entry) {
            return entry.first.first == jobId && selected.contains(entry.first.second);
        });
    }
    impl_->Notify();
    LogInfo("job participants updated; id=" + jobId + "; selected=" +
            std::to_string(selected.size()));
    return true;
}

std::optional<UpdateOffer> NodeApp::CheckForLanUpdate(
    std::wstring& error, std::atomic<bool>* cancel) {
    error.clear();
    if (cancel && cancel->load()) {
        error = L"更新检查已取消";
        return std::nullopt;
    }
    Config config = GetConfig();
    auto peers = Peers();
    // Peers learned through authenticated peer exchange can initially contain
    // only a node id and endpoint. Probe their signed direct status before the
    // version filter; otherwise those valid routes can never advertise an
    // update until a direct discovery packet happens to arrive.
    for (auto& peer : peers) {
        if (!peer.trusted || (!peer.appVersion.empty() &&
                              peer.protocolVersion >= kDirectStatusProtocol &&
                              peer.supportsDirectStatus))
            continue;
        Peer refreshed;
        if (!impl_->RefreshKnownPeer(peer, refreshed)) continue;
        peer = refreshed;
        std::scoped_lock lock(impl_->stateMutex);
        auto stored = impl_->peers.find(peer.nodeId);
        if (stored != impl_->peers.end() &&
            stored->second.address == peer.address &&
            stored->second.httpPort == peer.httpPort)
            stored->second = peer;
    }
    std::stable_sort(peers.begin(), peers.end(), [](const Peer& left, const Peer& right) {
        if (IsNewerAppVersion(left.appVersion, right.appVersion)) return true;
        if (IsNewerAppVersion(right.appVersion, left.appVersion)) return false;
        return left.nodeId < right.nodeId;
    });
    std::wstring lastError;
    for (const auto& peer : peers) {
        if (cancel && cancel->load()) {
            error = L"更新检查已取消";
            return std::nullopt;
        }
        if (!peer.trusted || !IsNewerAppVersion(
                peer.appVersion, kRenderMeshAppVersion))
            continue;
        std::string verifyError;
        if (!impl_->VerifyRemotePeer(peer, config, verifyError, false)) {
            lastError = L"新版节点身份验证失败：" + Utf8ToWide(verifyError);
            continue;
        }
        if (cancel && cancel->load()) {
            error = L"更新检查已取消";
            return std::nullopt;
        }
        http::Client client(peer.address, peer.httpPort);
        client.set_connection_timeout(3, 0);
        client.set_read_timeout(5, 0);
        client.set_write_timeout(5, 0);
        client.set_max_timeout(10000);
        auto response = client.Get("/v1/update/manifest",
            http::Headers{{"X-RM-Token", config.clusterToken}});
        if (!response || response->status != 200) {
            lastError = L"无法从新版节点读取更新清单";
            continue;
        }
        json manifest = json::parse(response->body, nullptr, false);
        if (manifest.is_discarded() || !manifest.is_object() ||
            !manifest.contains("version") || !manifest["version"].is_string() ||
            !manifest.contains("size") || !manifest["size"].is_number_unsigned() ||
            !manifest.contains("sha256") || !manifest["sha256"].is_string() ||
            !manifest.contains("signature") || !manifest["signature"].is_string()) {
            lastError = L"新版节点返回的更新清单格式无效";
            continue;
        }
        const std::string version = manifest["version"].get<std::string>();
        const uint64_t size = manifest["size"].get<uint64_t>();
        const std::string sha256 = manifest["sha256"].get<std::string>();
        const std::string signature = manifest["signature"].get<std::string>();
        const bool valid = version == peer.appVersion &&
            IsNewerAppVersion(version, kRenderMeshAppVersion) &&
            size > 0 && size <= kMaxUpdateBytes && SafeSha256(sha256) &&
            ConstantTimeEqual(signature,
                UpdateManifestSignature(config, version, size, sha256));
        if (!valid) {
            lastError = L"新版节点返回的更新清单无效";
            continue;
        }
        return UpdateOffer{
            version, peer.nodeId, peer.name, peer.address, peer.httpPort, size, sha256};
    }
    error = std::move(lastError);
    return std::nullopt;
}

bool NodeApp::DownloadLanUpdate(
    const UpdateOffer& offer, fs::path& pendingExecutable, std::wstring& error,
    std::atomic<bool>* cancel) {
    pendingExecutable.clear();
    if (!SafeId(offer.nodeId) || offer.address.empty() || offer.httpPort < 1024 ||
        offer.httpPort > 65535 || offer.sizeBytes == 0 ||
        offer.sizeBytes > kMaxUpdateBytes || !SafeSha256(offer.sha256) ||
        !IsNewerAppVersion(offer.version, kRenderMeshAppVersion)) {
        error = L"更新信息无效";
        return false;
    }
    Config config = GetConfig();
    std::optional<Peer> trustedPeer;
    for (const auto& current : Peers()) {
        if (current.nodeId == offer.nodeId && current.trusted) {
            trustedPeer = current;
            break;
        }
    }
    if (!trustedPeer) {
        error = L"提供更新的电脑已离线或不再受信任";
        return false;
    }
    // The peer table stores one preferred route. On a multi-homed Windows node
    // that route may change between showing the offer and the user's click.
    // Keep the endpoint whose signed manifest the user accepted, but only while
    // the same node ID remains trusted, and prove ownership again below.
    Peer offeredRoute = *trustedPeer;
    offeredRoute.address = offer.address;
    offeredRoute.httpPort = offer.httpPort;
    offeredRoute.appVersion = offer.version;
    std::string verifyError;
    if (!impl_->VerifyRemotePeer(offeredRoute, config, verifyError, false)) {
        error = L"原更新地址已不可用或身份验证失败（" +
            Utf8ToWide(offer.address) + L":" + std::to_wstring(offer.httpPort) +
            L"）：" + Utf8ToWide(verifyError);
        return false;
    }

    const fs::path updateDir = DataDir() / L"updates";
    std::error_code directoryError;
    fs::create_directories(updateDir, directoryError);
    if (directoryError) {
        error = L"无法创建更新缓存目录";
        return false;
    }
    const fs::path partial = updateDir /
        (L"RenderMesh-" + Utf8ToWide(offer.version) + L"-" +
         Utf8ToWide(NewRandomHex(6)) + L".exe.pending");
    HANDLE output = CreateFileW(
        partial.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (output == INVALID_HANDLE_VALUE) {
        error = L"无法创建更新文件：" + LastErrorMessage();
        return false;
    }
    uint64_t received = 0;
    bool headersObserved = false;
    bool headersAccepted = false;
    bool writeFailed = false;
    DWORD writeError = ERROR_SUCCESS;
    DWORD flushError = ERROR_SUCCESS;
    int responseStatus = 0;
    uint64_t responseLength = 0;
    std::string responseHash;
    http::Client client(offer.address, offer.httpPort);
    client.set_connection_timeout(3, 0);
    client.set_read_timeout(5, 0);
    client.set_write_timeout(5, 0);
    client.set_max_timeout(120000);
    auto response = client.Get(
        "/v1/update/package",
        http::Headers{{"X-RM-Token", config.clusterToken},
                         {"X-RM-Sha256", offer.sha256}},
        [&](const http::Response& headers) {
            headersObserved = true;
            responseStatus = headers.status;
            responseLength = headers.get_header_value_u64("Content-Length");
            responseHash = headers.get_header_value("X-RM-Sha256");
            headersAccepted = responseStatus == 200 &&
                responseLength == offer.sizeBytes &&
                ConstantTimeEqual(responseHash, offer.sha256);
            return headersAccepted;
        },
        [&](const char* data, size_t length) {
            if ((cancel && cancel->load()) ||
                length > static_cast<size_t>(MAXDWORD) ||
                received + length > offer.sizeBytes)
                return false;
            DWORD written = 0;
            if (!WriteFile(output, data, static_cast<DWORD>(length), &written, nullptr) ||
                written != length) {
                writeFailed = true;
                writeError = GetLastError();
                if (writeError == ERROR_SUCCESS) writeError = ERROR_WRITE_FAULT;
                return false;
            }
            received += length;
            return true;
        });
    bool flushed = false;
    if (!writeFailed && received == offer.sizeBytes) {
        flushed = FlushFileBuffers(output) != FALSE;
        if (!flushed) flushError = GetLastError();
    }
    CloseHandle(output);
    const bool cancelled = cancel && cancel->load();
    if (!response || !headersAccepted || response->status != 200 || !flushed || cancelled) {
        std::error_code ignored;
        fs::remove(partial, ignored);
        if (cancelled) {
            error = L"更新下载已取消";
        } else if (writeFailed) {
            SetLastError(writeError);
            error = L"更新文件写入失败：" + LastErrorMessage();
        } else if (!headersObserved && !response) {
            error = L"无法连接更新地址或下载超时（" +
                Utf8ToWide(offer.address) + L":" +
                std::to_wstring(offer.httpPort) + L"）";
        } else if (headersObserved && responseStatus != 200) {
            error = L"更新节点拒绝下载（HTTP " +
                std::to_wstring(responseStatus) + L"）";
        } else if (headersObserved && !headersAccepted) {
            error = L"更新包响应头不匹配（声明大小 " +
                std::to_wstring(responseLength) + L" 字节，期望 " +
                std::to_wstring(offer.sizeBytes) + L" 字节）";
        } else if (received != offer.sizeBytes) {
            error = L"更新包接收不完整（已接收 " + std::to_wstring(received) +
                L" / " + std::to_wstring(offer.sizeBytes) + L" 字节）";
        } else if (!flushed) {
            SetLastError(flushError);
            error = L"更新文件刷新失败：" + LastErrorMessage();
        } else {
            error = L"更新下载连接意外中断";
        }
        return false;
    }
    const std::string actualHash = Sha256File(partial, cancel);
    DWORD binaryType = 0;
    if ((cancel && cancel->load()) ||
        !ConstantTimeEqual(actualHash, offer.sha256) ||
        !GetBinaryTypeW(partial.c_str(), &binaryType) ||
        (binaryType != SCS_64BIT_BINARY && binaryType != SCS_32BIT_BINARY)) {
        std::error_code ignored;
        fs::remove(partial, ignored);
        error = cancel && cancel->load() ? L"更新验证已取消"
            : L"更新文件校验失败，现有版本未被修改";
        return false;
    }
    pendingExecutable = partial;
    LogInfo("LAN update downloaded and verified; version=" + offer.version +
            "; node=" + offer.nodeId);
    return true;
}

#ifdef RM_DEV_TOOLS
bool RunSelfTest(std::wstring& report) {
    try {
        auto fileContains = [](const fs::path& path, std::string_view needle) {
            std::ifstream input(path, std::ios::binary);
            std::string text((std::istreambuf_iterator<char>(input)), {});
            return input.good() || input.eof() ? text.find(needle) != std::string::npos : false;
        };
        if (!IsNewerAppVersion("0.2.29", "0.2.28") ||
            !IsNewerAppVersion("0.3", "0.2.99") ||
            IsNewerAppVersion("0.2.29", "0.2.29") ||
            IsNewerAppVersion("0.2.28", "0.2.29") ||
            IsNewerAppVersion("0.2.beta", "0.2.28")) {
            report = L"LAN update version ordering self-test failed";
            return false;
        }
        Peer modernUploadPeer;
        modernUploadPeer.supportsResumableProjectUpload = true;
        if (!PeerSupportsProjectBundle(modernUploadPeer) || PeerSupportsProjectBundle(Peer{})) {
            report = L"Large project resumable-upload policy self-test failed";
            return false;
        }
        bool cachedUpload = false;
        uint64_t uploadOffset = 0;
        std::string uploadStatusError;
        if (!ParseProjectUploadStatus(
                R"({"cached":false,"offset":4294967297})", cachedUpload,
                uploadOffset, uploadStatusError) || cachedUpload ||
            uploadOffset != 4294967297ULL ||
            ParseProjectUploadStatus(
                R"({"cached":"no","offset":0})", cachedUpload,
                uploadOffset, uploadStatusError) ||
            ParseProjectUploadStatus(
                R"({"cached":false,"offset":-1})", cachedUpload,
                uploadOffset, uploadStatusError) ||
            ParseProjectUploadStatus(
                "[]", cachedUpload, uploadOffset, uploadStatusError)) {
            report = L"Large project upload-status validation self-test failed";
            return false;
        }
        Config updateSignatureConfig;
        updateSignatureConfig.clusterToken = "self-test-update-cluster-token";
        const std::string updateSignature = UpdateManifestSignature(
            updateSignatureConfig, "0.2.29", 123456, std::string(64, 'a'));
        if (updateSignature.size() != 64 || updateSignature == UpdateManifestSignature(
                updateSignatureConfig, "0.2.30", 123456, std::string(64, 'a')) ||
            updateSignature == UpdateManifestSignature(
                updateSignatureConfig, "0.2.29", 123457, std::string(64, 'a')) ||
            updateSignature == UpdateManifestSignature(
                updateSignatureConfig, "0.2.29", 123456, std::string(64, 'b'))) {
            report = L"LAN update manifest authentication self-test failed";
            return false;
        }
        if (SafeExportFolderComponent(L"../shot:01* ") != L"___shot_01_") {
            report = L"Export folder sanitisation self-test failed";
            return false;
        }
        std::vector<int> parsedTestFrames;
        std::wstring frameSelectionError;
        if (!ParseFrameSelection(L"1,4,6-10,30-35",
                std::vector<int>{1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15,
                                 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28,
                                 29, 30, 31, 32, 33, 34, 35, 36},
                parsedTestFrames, frameSelectionError) ||
            parsedTestFrames != std::vector<int>({1, 4, 6, 7, 8, 9, 10, 30, 31, 32, 33, 34, 35}) ||
            ParseFrameSelection(L"1，4", std::vector<int>{1, 2, 3, 4},
                                parsedTestFrames, frameSelectionError) ||
            ParseFrameSelection(L"2", std::vector<int>{1, 3, 5, 7, 9},
                                parsedTestFrames, frameSelectionError)) {
            report = L"Test-frame expression self-test failed";
            return false;
        }
        if (JobStageLabel(JobStage::TestCompleted) != L"测试完成") {
            report = L"Job-stage label self-test failed";
            return false;
        }
        if (IsComputerIdleForSharedWork(179.9, 180) ||
            !IsComputerIdleForSharedWork(180.0, 180) ||
            !IsComputerIdleForSharedWork(600.0, 180)) {
            report = L"Idle-only sharing policy self-test failed";
            return false;
        }
        const auto packages = BuildFramePackages({1, 3, 5, 7, 9, 11, 13});
        if (packages.size() != 2 || packages[0] != std::vector<int>({1, 3, 5, 7, 9}) ||
            packages[1] != std::vector<int>({11, 13}) ||
            !BuildFramePackages({1, 2}, 0).empty()) {
            report = L"Five-frame package partition self-test failed";
            return false;
        }
        std::wstring missingFrameError;
        std::wstring stoppedPackageError;
        if (ValidateFramePackageCompletion(1, 0, false, missingFrameError) ||
            missingFrameError.empty() ||
            !ValidateFramePackageCompletion(5, 1, true, stoppedPackageError)) {
            report = L"Zero-output Blender package validation self-test failed";
            return false;
        }
        if (!ShouldRetryQuickEmptyPackage(
                true, false, true, false, false, false,
                std::chrono::seconds(1), missingFrameError) ||
            ShouldRetryQuickEmptyPackage(
                true, false, true, false, false, false,
                std::chrono::seconds(10), missingFrameError) ||
            ShouldRetryQuickEmptyPackage(
                true, false, true, true, false, false,
                std::chrono::seconds(1), missingFrameError)) {
            report = L"Quick zero-output Blender retry self-test failed";
            return false;
        }
        OutputTarget metadataSource;
        metadataSource.id = "output-7";
        metadataSource.name = "AOV File Output";
        metadataSource.outputs = {
            "Output", "Alpha", "Mist", "Emission", "CryptoObject00", "Noisy Image"};
        metadataSource.filePattern = "{blend_name}";
        metadataSource.sceneName = "Scene";
        metadataSource.nodePath = {4, 2};
        metadataSource.required = true;
        OutputTarget metadataParsed;
        json metadataJson = OutputTargetMetadata(metadataSource);
        json locatorJson = OutputTargetLocatorMetadata(metadataSource);
        OutputTarget manyAovSource = metadataSource;
        manyAovSource.outputs.assign(256, std::string(128, 'A'));
        const std::string manyAovMetadata = OutputTargetMetadata(manyAovSource).dump();
        const std::string manyAovLocator = OutputTargetLocatorMetadata(manyAovSource).dump();
        if (!ParseOutputTargetMetadata(metadataJson, metadataParsed) ||
            metadataParsed.id != metadataSource.id ||
            metadataParsed.outputs != metadataSource.outputs ||
            metadataParsed.filePattern != metadataSource.filePattern ||
            metadataParsed.sceneName != metadataSource.sceneName ||
            metadataParsed.nodePath != metadataSource.nodePath ||
            metadataParsed.required != metadataSource.required ||
            locatorJson.contains("outputs") ||
            locatorJson.value("file_pattern", "") != metadataSource.filePattern ||
            locatorJson.value("id", "") != metadataSource.id ||
            locatorJson.value("node_path", std::vector<int>{}) != metadataSource.nodePath ||
            !locatorJson.value("required", false) ||
            manyAovMetadata.size() <= 8192 || manyAovLocator.size() > 4096 ||
            ParseOutputTargetMetadata(
                json{{"id", "../escape"}, {"name", "bad"}}, metadataParsed)) {
            report = L"Compositor File Output metadata self-test failed";
            return false;
        }
        if (!RunBlenderReadinessSelfTest(report)) return false;
        fs::path root = fs::temp_directory_path() / (L"RenderMesh-SelfTest-" +
            Utf8ToWide(NewRandomHex(4)));
        fs::create_directories(root);
        const fs::path texFixtureRoot = root / L"tex-project-source";
        fs::create_directories(texFixtureRoot / L"tex" / L"sequence");
        const fs::path texSourceBlend = texFixtureRoot / L"shot.blend";
        const fs::path texPreparedBlend = root / L"tex-prepared.blend";
        std::ofstream(texSourceBlend, std::ios::binary) << "original blend";
        std::ofstream(texPreparedBlend, std::ios::binary) << "prepared blend";
        std::ofstream(texFixtureRoot / L"tex" / L"albedo.png", std::ios::binary)
            << "albedo";
        std::ofstream(texFixtureRoot / L"tex" / L"sequence" / L"0001.exr",
                      std::ios::binary) << "sequence-frame";
        PreflightReport texReport;
        texReport.sourceProject = texSourceBlend;
        texReport.preparedProject = texPreparedBlend;
        std::wstring texError;
        ProjectSnapshot texSnapshot;
        if (!SnapshotProject(texReport, root / L"tex-job", texSnapshot, texError, nullptr, {}) ||
            !texSnapshot.bundle || !fs::is_regular_file(texSnapshot.payload) ||
            texSnapshot.hash != Sha256File(texSnapshot.payload) ||
            fs::exists(texSnapshot.project) ||
            !ExtractProjectBundle(texSnapshot.payload, root / L"tex-extracted", texError) ||
            !fs::is_regular_file(root / L"tex-extracted" / L"source.blend") ||
            !fileContains(root / L"tex-extracted" / L"tex" / L"albedo.png",
                          "albedo") ||
            !fileContains(root / L"tex-extracted" / L"tex" / L"sequence" /
                              L"0001.exr", "sequence-frame") ||
            SafeProjectBundlePath("../escape") ||
            SafeProjectBundlePath("other/file.png") ||
            SafeProjectBundlePath("tex/.. /.. /escaped.txt") ||
            SafeProjectBundlePath("tex/NUL.txt") ||
            SafeProjectBundlePath("tex/name. ") ||
            SafeProjectBundlePath("tex/file.txt:payload")) {
            report = L"Sibling tex project bundle round-trip self-test failed: " + texError;
            return false;
        }
        // A prepared copy in the storage work folder is moved, not copied, and
        // its hash is the hash of the snapshot.
        const fs::path plainSource = root / L"plain-project" / L"plain.blend";
        const fs::path plainPrepared = root / L"plain-work" / L"prepared.blend";
        fs::create_directories(plainSource.parent_path());
        fs::create_directories(plainPrepared.parent_path());
        std::ofstream(plainSource, std::ios::binary) << "plain source";
        std::ofstream(plainPrepared, std::ios::binary) << "plain prepared copy";
        PreflightReport plainReport;
        plainReport.sourceProject = plainSource;
        plainReport.preparedProject = plainPrepared;
        plainReport.preparedCopy = true;
        ProjectSnapshot plainSnapshot;
        std::wstring plainError;
        const bool movedSnapshot =
            SnapshotProject(plainReport, root / L"plain-job", plainSnapshot, plainError,
                            nullptr, {}) &&
            !plainSnapshot.bundle && !fs::exists(plainPrepared) &&
            fileContains(plainSnapshot.payload, "plain prepared copy") &&
            plainSnapshot.hash == Sha256File(plainSnapshot.payload);
        plainReport.preparedProject = plainSource;
        plainReport.preparedCopy = false;
        uint64_t copiedBytes = 0;
        const bool copiedSnapshot = movedSnapshot &&
            SnapshotProject(plainReport, root / L"plain-copy-job", plainSnapshot, plainError,
                            nullptr, [&](uint64_t done, uint64_t) { copiedBytes = done; }) &&
            fs::exists(plainSource) && copiedBytes == fs::file_size(plainSource) &&
            plainSnapshot.hash == Sha256File(plainSource);
        if (!copiedSnapshot) {
            report = L"Single-pass project snapshot self-test failed: " + plainError;
            return false;
        }
        const fs::path traversalBundle = root / L"unsafe-project.rmpkg";
        const fs::path traversalTarget = root / L"escaped.txt";
        std::ofstream(traversalTarget, std::ios::binary) << "sentinel";
        {
            std::ofstream unsafe(traversalBundle, std::ios::binary | std::ios::trunc);
            const char magic[8] = {'R', 'M', 'P', 'K', 'G', '1', '\0', '\0'};
            const uint32_t count = 2;
            unsafe.write(magic, sizeof(magic));
            unsafe.write(reinterpret_cast<const char*>(&count), sizeof(count));
            const auto writeEntry = [&unsafe](
                const std::string& path, const std::string& contents) {
                const uint32_t pathSize = static_cast<uint32_t>(path.size());
                const uint64_t size = static_cast<uint64_t>(contents.size());
                unsafe.write(reinterpret_cast<const char*>(&pathSize), sizeof(pathSize));
                unsafe.write(reinterpret_cast<const char*>(&size), sizeof(size));
                unsafe.write(path.data(), static_cast<std::streamsize>(path.size()));
                unsafe.write(contents.data(),
                             static_cast<std::streamsize>(contents.size()));
            };
            writeEntry("source.blend", "blend");
            writeEntry("tex/.. /.. /escaped.txt", "overwritten");
        }
        std::wstring traversalError;
        if (ExtractProjectBundle(
                traversalBundle, root / L"unsafe-extracted", traversalError) ||
            !fileContains(traversalTarget, "sentinel")) {
            report = L"Windows-normalized project bundle traversal self-test failed";
            return false;
        }
        const fs::path emptyTexRoot = root / L"empty-tex-source";
        fs::create_directories(emptyTexRoot / L"tex");
        const fs::path emptyTexBlend = emptyTexRoot / L"empty.blend";
        const fs::path emptyTexPrepared = root / L"empty-tex-prepared.blend";
        std::ofstream(emptyTexBlend, std::ios::binary) << "empty tex source";
        std::ofstream(emptyTexPrepared, std::ios::binary) << "empty tex prepared";
        PreflightReport emptyTexReport;
        emptyTexReport.sourceProject = emptyTexBlend;
        emptyTexReport.preparedProject = emptyTexPrepared;
        ProjectSnapshot emptySnapshot;
        if (!SnapshotProject(emptyTexReport, root / L"empty-tex-job", emptySnapshot, texError,
                             nullptr, {}) ||
            !emptySnapshot.bundle ||
            !ExtractProjectBundle(
                emptySnapshot.payload, root / L"empty-tex-extracted", texError) ||
            !fs::is_directory(root / L"empty-tex-extracted" / L"tex")) {
            report = L"Empty sibling tex directory self-test failed: " + texError;
            return false;
        }
        ConfigStore store(root);
        Config config = store.Load();
        if (!IsInviteCode(config.inviteCode) || config.clusterToken.size() < 16) {
            report = L"Default invite-code or internal credential generation failed";
            return false;
        }
        config.nodeName = "SELFTEST";
        config.idleAfterSeconds = 7;
        config.inviteCode = "042731";
        config.clusterToken = "legacy-token-123";
        config.manualPeerEndpoint = "192.168.28.28:8391";
        config.theme = "dark";
        config.closeBehavior = "tray";
        config.autoStart = true;
        config.silentAutoStart = true;
        config.projectStoragePath = root / L"configured-project-storage";
        fs::create_directories(config.projectStoragePath);
        config.renderSoftwareProfiles = {
            {"blender-52", "Blender 5.2", "Windows", "Blender",
             root / L"Blender-5.2" / L"blender.exe"},
            {"blender-41", "Blender 4.1", "Windows", "Blender",
             root / L"Blender-4.1" / L"blender.exe"},
        };
        config.lastRenderSoftwareProfileId = "blender-41";
        config.blenderPath = config.renderSoftwareProfiles.back().installPath;
        std::wstring error;
        Config persisted;
        if (!store.Save(config, error) ||
            (persisted = store.Load()).idleAfterSeconds != 7 ||
            persisted.inviteCode != "042731" ||
            persisted.clusterToken != "legacy-token-123" ||
            persisted.manualPeerEndpoint != config.manualPeerEndpoint ||
            persisted.theme != "dark" ||
            persisted.closeBehavior != "tray" ||
            !persisted.autoStart ||
            !persisted.silentAutoStart ||
            persisted.projectStoragePath != config.projectStoragePath ||
            persisted.renderSoftwareProfiles.size() != 2 ||
            persisted.renderSoftwareProfiles[0].name != "Blender 5.2" ||
            persisted.renderSoftwareProfiles[1].renderSystem != "Windows" ||
            persisted.renderSoftwareProfiles[1].renderSoftware != "Blender" ||
            persisted.renderSoftwareProfiles[1].installPath !=
                config.renderSoftwareProfiles[1].installPath ||
            persisted.lastRenderSoftwareProfileId != "blender-41" ||
            persisted.blenderPath != config.renderSoftwareProfiles[1].installPath) {
            report = L"配置持久化失败：" + error;
            return false;
        }
        const fs::path manualBlenderRoot = root / L"manual-blender-config";
        const fs::path manualBlender =
            manualBlenderRoot / L"Blender Custom" / L"blender.exe";
        fs::create_directories(manualBlender.parent_path());
        std::ofstream(manualBlender, std::ios::binary) << "fixture";
        ConfigStore manualBlenderStore(manualBlenderRoot);
        Config manualBlenderConfig = manualBlenderStore.Load();
        std::wstring manualBlenderError;
        const RenderAdapter& blenderAdapter = BlenderRenderAdapter();
        if (!blenderAdapter.ConfigureExecutable(
                manualBlenderConfig, manualBlender, manualBlenderError) ||
            blenderAdapter.ConfigureExecutable(
                manualBlenderConfig, manualBlenderRoot / L"missing.exe",
                manualBlenderError) ||
            !manualBlenderStore.Save(manualBlenderConfig, manualBlenderError)) {
            report = L"Manual Blender configuration self-test failed: " +
                manualBlenderError;
            return false;
        }
        const Config manualBlenderPersisted = manualBlenderStore.Load();
        const fs::path expectedManualBlender =
            fs::absolute(manualBlender).lexically_normal();
        if (manualBlenderPersisted.blenderPath != expectedManualBlender ||
            blenderAdapter.LocateExecutable(manualBlenderPersisted) != expectedManualBlender ||
            manualBlenderPersisted.renderSoftwareProfiles.empty() ||
            manualBlenderPersisted.renderSoftwareProfiles.front().installPath !=
                expectedManualBlender) {
            report = L"Manual Blender configuration persistence self-test failed";
            return false;
        }
        const fs::path migrationConfigRoot = root / L"storage-migration-config";
        const fs::path oldStorageRoot = root / L"storage-old";
        const fs::path newStorageRoot = root / L"storage-new";
        fs::create_directories(oldStorageRoot);
        fs::create_directories(newStorageRoot);
        ConfigStore migrationStore(migrationConfigRoot);
        Config migrationConfig = migrationStore.Load();
        migrationConfig.projectStoragePath = oldStorageRoot;
        if (!migrationStore.Save(migrationConfig, error)) {
            report = L"Project-storage migration setup failed: " + error;
            return false;
        }
        NodeApp migrationApp(
            std::move(migrationStore), migrationConfig, true,
            std::chrono::milliseconds(10));
        const fs::path migrationSource = root / L"migration-source.blend";
        const fs::path migrationPrepared = root / L"migration-prepared.blend";
        std::ofstream(migrationSource, std::ios::binary) << "source";
        std::ofstream(migrationPrepared, std::ios::binary) << "prepared";
        PreflightReport migrationReport;
        migrationReport.passed = true;
        migrationReport.sourceProject = migrationSource;
        migrationReport.preparedProject = migrationPrepared;
        migrationReport.sceneName = "StorageMigration";
        migrationReport.frameStart = 1;
        migrationReport.frameEnd = 2;
        migrationReport.frameStep = 1;
        migrationReport.outputFormat = "PNG";
        migrationReport.outputExtension = ".png";
        std::string migrationJobId = migrationApp.CreatePreparedDraft(
            "storage-migration", migrationReport, error);
        fs::create_directories(oldStorageRoot / L"cache" / L"projects" / L"fixture");
        std::ofstream(oldStorageRoot / L"cache" / L"projects" / L"fixture" /
                      L"source.blend", std::ios::binary) << "cached";
        fs::create_directories(oldStorageRoot / L"incoming" / L"fixture");
        std::ofstream(oldStorageRoot / L"incoming" / L"fixture" / L"chunk.part",
                      std::ios::binary) << "partial";
        int migrationPercent = 0;
        std::atomic<bool> migrationCancel = false;
        std::wstring migrationResult;
        if (migrationJobId.empty() ||
            !migrationApp.MigrateProjectStorage(
                newStorageRoot,
                [&](int percent) { migrationPercent = percent; },
                &migrationCancel, migrationResult) ||
            migrationPercent != 100 ||
            migrationApp.ProjectStorageDir() != fs::absolute(newStorageRoot) ||
            fs::exists(oldStorageRoot / L"jobs") ||
            fs::exists(oldStorageRoot / L"cache") ||
            fs::exists(oldStorageRoot / L"incoming") ||
            !fileContains(newStorageRoot / L"jobs" /
                              Utf8ToWide(migrationJobId) / L"source.blend",
                          "prepared") ||
            !fileContains(newStorageRoot / L"cache" / L"projects" / L"fixture" /
                              L"source.blend",
                          "cached") ||
            !fileContains(newStorageRoot / L"incoming" / L"fixture" / L"chunk.part",
                          "partial")) {
            report = L"Project-storage copy/cutover self-test failed: " + migrationResult;
            return false;
        }
        auto migratedJob = migrationApp.FindJob(migrationJobId);
        if (!migratedJob ||
            !RelativeManagedPath(newStorageRoot, migratedJob->preparedProject)) {
            report = L"Project-storage in-memory path rebasing self-test failed";
            return false;
        }
        ConfigStore reopenedMigrationStore(migrationConfigRoot);
        Config reopenedMigrationConfig = reopenedMigrationStore.Load();
        NodeApp reopenedMigrationApp(
            std::move(reopenedMigrationStore), reopenedMigrationConfig, true,
            std::chrono::milliseconds(10));
        auto reopenedMigrationJob = reopenedMigrationApp.FindJob(migrationJobId);
        if (reopenedMigrationConfig.projectStoragePath != fs::absolute(newStorageRoot) ||
            !reopenedMigrationJob ||
            !RelativeManagedPath(newStorageRoot, reopenedMigrationJob->preparedProject) ||
            !fs::is_regular_file(reopenedMigrationJob->preparedProject)) {
            report = L"Project-storage relative persistence self-test failed";
            return false;
        }
        const fs::path cancelledStorageRoot = root / L"storage-cancelled";
        fs::create_directories(cancelledStorageRoot);
        migrationCancel = true;
        if (migrationApp.MigrateProjectStorage(
                cancelledStorageRoot, {}, &migrationCancel, migrationResult) ||
            migrationApp.ProjectStorageDir() != fs::absolute(newStorageRoot) ||
            !fs::is_regular_file(newStorageRoot / L"jobs" /
                                 Utf8ToWide(migrationJobId) / L"source.blend")) {
            report = L"Project-storage cancellation self-test failed";
            return false;
        }
        const fs::path updaterRoot = root / L"lan-update-client";
        ConfigStore updaterStore(updaterRoot);
        Config updaterConfig = updaterStore.Load();
        updaterConfig.nodeId = "selftest-update-client";
        updaterConfig.inviteCode = "246810";
        updaterConfig.clusterToken = NewRandomHex(16);
        updaterConfig.pairingPending = false;
        NodeApp updaterClient(std::move(updaterStore), updaterConfig, true);
        const fs::path runningExecutable = ExecutablePath();
        const uint64_t runningSize = fs::file_size(runningExecutable);
        const std::string runningHash = Sha256File(runningExecutable);
        std::string runningPackage;
        const bool runningPackageReadable =
            ReadUpdatePackage(runningExecutable, runningSize, runningPackage) &&
            runningPackage.size() == runningSize;
        // Keep the update fixture newer than the executable under test so a
        // normal release version bump cannot silently invalidate this path.
        const std::string offeredVersion = "999.0.0";
        std::atomic<bool> serveMalformedManifest = false;
        std::atomic<int> transientProofFailures = 1;
        http::Server updateServer;
        updateServer.Get("/health", [&](const http::Request& request,
                                           http::Response& response) {
            const std::string challenge = request.has_param("challenge")
                ? request.get_param_value("challenge") : "";
            json snapshot{
                {"status", "ok"}, {"protocol", kProtocolVersion},
                {"app_version", offeredVersion}, {"direct_status", true},
                {"node_id", "selftest-update-peer"},
                {"name", "SELFTEST-UPDATE-SOURCE"},
                {"matching_id_binding", MatchingIdBinding(updaterConfig)},
            };
            if (SafeId(challenge) && challenge.size() >= 16) {
                snapshot["direct_status_proof"] = DirectStatusProof(
                    updaterConfig, "selftest-update-peer", challenge, snapshot);
            }
            response.set_content(snapshot.dump(), "application/json");
        });
        updateServer.Get("/v1/prove", [&](const http::Request& request,
                                             http::Response& response) {
            if (transientProofFailures.fetch_sub(1) > 0) {
                response.status = 503;
                response.set_content("transient ownership probe failure", "text/plain");
                return;
            }
            const std::string challenge = request.has_param("challenge")
                ? request.get_param_value("challenge") : "";
            response.set_content(json{
                {"node_id", "selftest-update-peer"},
                {"matching_id_binding", MatchingIdBinding(updaterConfig)},
                {"proof", HmacSha256(
                    updaterConfig.clusterToken,
                    "worker-proof|selftest-update-peer|" + challenge)},
            }.dump(), "application/json");
        });
        updateServer.Get("/v1/update/manifest", [&](const http::Request& request,
                                                       http::Response& response) {
            if (!ConstantTimeEqual(request.get_header_value("X-RM-Token"),
                                   updaterConfig.clusterToken)) {
                response.status = 401;
                return;
            }
            if (serveMalformedManifest.load()) {
                response.set_content("[]", "application/json");
                return;
            }
            response.set_content(json{
                {"version", offeredVersion}, {"size", runningSize},
                {"sha256", runningHash},
                {"signature", UpdateManifestSignature(
                    updaterConfig, offeredVersion, runningSize, runningHash)},
            }.dump(), "application/json");
        });
        updateServer.Get("/v1/update/package", [&](const http::Request& request,
                                                      http::Response& response) {
            if (!ConstantTimeEqual(request.get_header_value("X-RM-Token"),
                                   updaterConfig.clusterToken) ||
                !ConstantTimeEqual(request.get_header_value("X-RM-Sha256"),
                                   runningHash)) {
                response.status = 401;
                return;
            }
            response.set_header("X-RM-Sha256", runningHash);
            response.set_content(runningPackage, "application/octet-stream");
        });
        const int updatePort = updateServer.bind_to_any_port("127.0.0.1");
        std::thread updateThread;
        if (updatePort > 0)
            updateThread = std::thread([&] { updateServer.listen_after_bind(); });
        Peer updatePeer;
        updatePeer.nodeId = "selftest-update-peer";
        updatePeer.address = "127.0.0.1";
        updatePeer.httpPort = updatePort;
        updatePeer.trusted = true;
        updaterClient.InjectPeerForTest(updatePeer);
        std::wstring updateError;
        auto updateOffer = updatePort > 0
            ? updaterClient.CheckForLanUpdate(updateError) : std::nullopt;
        // A multi-homed node can advertise another valid route between the
        // update offer and the user's click. The offered endpoint is still
        // online and authenticated, so downloading from it must remain valid.
        Peer flappedUpdatePeer = updatePeer;
        flappedUpdatePeer.address = "127.0.0.2";
        flappedUpdatePeer.protocolVersion = kProtocolVersion;
        flappedUpdatePeer.appVersion = offeredVersion;
        flappedUpdatePeer.supportsDirectStatus = true;
        updaterClient.InjectPeerForTest(flappedUpdatePeer);
        fs::path downloadedUpdate;
        const bool updateDownloaded = updateOffer &&
            updaterClient.DownloadLanUpdate(
                *updateOffer, downloadedUpdate, updateError) &&
            fs::is_regular_file(downloadedUpdate) &&
            Sha256File(downloadedUpdate) == runningHash;
        const std::wstring updateDownloadError = updateError;
        updaterClient.InjectPeerForTest(updatePeer);
        serveMalformedManifest = true;
        updateError.clear();
        const auto malformedOffer = updaterClient.CheckForLanUpdate(updateError);
        std::atomic<bool> cancelledUpdateCheck = true;
        std::wstring cancelledUpdateError;
        const auto cancelledOffer = updaterClient.CheckForLanUpdate(
            cancelledUpdateError, &cancelledUpdateCheck);
        updateServer.stop();
        if (updateThread.joinable()) updateThread.join();
        if (!runningPackageReadable || !updateOffer ||
            updateOffer->version != offeredVersion ||
            !updateDownloaded || malformedOffer || updateError.empty() ||
            cancelledOffer || cancelledUpdateError != L"更新检查已取消") {
            report = !updateDownloaded
                ? L"Authenticated LAN update route-change self-test failed: " +
                    updateDownloadError
                : L"Authenticated LAN update download self-test failed: " + updateError;
            return false;
        }
        fs::path brokenPairingRoot = root / L"broken-pairing-config";
        ConfigStore brokenPairingStore(brokenPairingRoot);
        Config brokenPairingConfig = brokenPairingStore.Load();
        brokenPairingConfig.inviteCode = "314159";
        brokenPairingConfig.clusterToken.clear();
        brokenPairingConfig.pairingPending = true;
        if (!brokenPairingStore.Save(brokenPairingConfig, error)) {
            report = L"Unable to persist broken matching-ID fixture: " + error;
            return false;
        }
        Config recoveredPairingConfig = brokenPairingStore.Load();
        if (!recoveredPairingConfig.pairingPending ||
            recoveredPairingConfig.inviteCode != "314159" ||
            recoveredPairingConfig.clusterToken.size() < 16) {
            report = L"Broken matching-ID credential recovery self-test failed";
            return false;
        }
        if (!IsInviteCode("000000") || !IsInviteCode("999999") ||
            IsInviteCode("12345") || IsInviteCode("1234567") ||
            IsInviteCode("12A456") || !IsSafeOutputExtension("") ||
            !IsSafeOutputExtension(".png") || IsSafeOutputExtension("png") ||
            IsSafeOutputExtension(".bad/path")) {
            report = L"Six-digit invite-code validation failed";
            return false;
        }
        fs::path legacyRoot = root / L"legacy-job-migration";
        fs::create_directories(legacyRoot);
        const int64_t legacyCreated = UnixMilliseconds(
            std::chrono::system_clock::now() - std::chrono::hours(2));
        const int64_t legacyEnded = UnixMilliseconds(
            std::chrono::system_clock::now() - std::chrono::hours(1));
        std::ofstream(legacyRoot / L"jobs.json", std::ios::binary) << json::array({
            {{"id", "legacy-job"}, {"name", "Legacy Scene"},
             {"format", "PNG"}, {"extension", ".png"},
             {"frames", json::array({
                 {{"task_id", "legacy-job-1"}, {"frame", 1},
                  {"state", static_cast<int>(TaskState::Completed)},
                  {"ended_at", legacyEnded}}
             })},
             {"created_at", legacyCreated}},
            {{"id", ".."}, {"name", "Unsafe history row"},
             {"frames", json::array({
                 {{"task_id", "unsafe-1"}, {"frame", 1},
                  {"state", static_cast<int>(TaskState::Completed)},
                  {"ended_at", legacyEnded}}
             })},
             {"created_at", legacyCreated}}
        }).dump(2);
        ConfigStore legacyStore(legacyRoot);
        Config legacyConfig = legacyStore.Load();
        NodeApp legacyApp(std::move(legacyStore), legacyConfig, true);
        bool legacyImported = legacyApp.Jobs().size() == 1 &&
            legacyApp.Jobs().front().id == "legacy-job" &&
            fs::exists(legacyRoot / L"jobdb" / L"legacy-job.json") &&
            fs::exists(legacyRoot / L"jobs.json.imported");
        std::wstring legacyDeleteError;
        bool legacyDeleted = legacyImported &&
            legacyApp.DeleteJob("legacy-job", legacyDeleteError);
        NodeApp legacyReloaded(ConfigStore(legacyRoot), legacyConfig, true);
        if (!legacyDeleted || !legacyReloaded.Jobs().empty()) {
            report = L"Legacy jobs.json migration or deletion persistence failed";
            return false;
        }
        fs::path retryAllRoot = root / L"retry-failed-frames";
        fs::create_directories(retryAllRoot);
        fs::path retryAllPrepared = retryAllRoot / L"source.blend";
        std::ofstream(retryAllPrepared, std::ios::binary) << "RenderMesh retry self-test";
        std::ofstream(retryAllRoot / L"jobs.json", std::ios::binary) << json::array({
            {{"id", "retry-all-job"}, {"name", "Retry Failed Frames"},
             {"blend", WideToUtf8(retryAllPrepared.wstring())},
             {"stage", static_cast<int>(JobStage::Failed)},
             {"frames", json::array({
                 {{"task_id", "retry-all-job-1"}, {"frame", 1},
                  {"state", static_cast<int>(TaskState::Completed)},
                  {"ended_at", legacyEnded}},
                 {{"task_id", "retry-all-job-2"}, {"frame", 2},
                  {"state", static_cast<int>(TaskState::Failed)},
                  {"assigned_node_id", "stale-node"},
                  {"assigned_node_name", "STALE"},
                  {"assigned_package_id", "stale-package"},
                  {"attempt_token", "stale-attempt"},
                  {"error", "render failed"}, {"ended_at", legacyEnded}},
                 {{"task_id", "retry-all-job-3"}, {"frame", 3},
                  {"state", static_cast<int>(TaskState::Failed)},
                  {"error", "publish failed"}, {"ended_at", legacyEnded}}
             })},
             {"created_at", legacyCreated}, {"finished_at", legacyEnded}},
            {{"id", "retry-one-job"}, {"name", "Retry One Frame"},
             {"blend", WideToUtf8(retryAllPrepared.wstring())},
             {"stage", static_cast<int>(JobStage::Failed)},
             {"frames", json::array({
                 {{"task_id", "retry-one-job-10"}, {"frame", 10},
                  {"state", static_cast<int>(TaskState::Completed)},
                  {"ended_at", legacyEnded}},
                 {{"task_id", "retry-one-job-11"}, {"frame", 11},
                  {"state", static_cast<int>(TaskState::Failed)},
                  {"assigned_node_id", "stale-node"},
                  {"assigned_package_id", "stale-package"},
                  {"attempt_token", "stale-attempt"},
                  {"error", "frame failed"}, {"ended_at", legacyEnded}}
             })},
             {"created_at", legacyCreated}, {"finished_at", legacyEnded}},
            {{"id", "resume-stopped-job"}, {"name", "Resume Stopped Frames"},
             {"blend", WideToUtf8(retryAllPrepared.wstring())},
             {"stage", static_cast<int>(JobStage::Stopped)},
             {"test_frames", json::array({1, 2})},
             {"frames", json::array({
                 {{"task_id", "resume-stopped-job-1"}, {"frame", 1},
                  {"state", static_cast<int>(TaskState::Completed)},
                  {"assigned_node_id", "completed-node"},
                  {"assigned_node_name", "COMPLETED"},
                  {"output", "keep-completed.png"}, {"ended_at", legacyEnded}},
                 {{"task_id", "resume-stopped-job-2"}, {"frame", 2},
                  {"state", static_cast<int>(TaskState::Stopped)},
                  {"assigned_node_id", "stale-node"},
                  {"assigned_node_name", "STALE"},
                  {"assigned_package_id", "stale-package"},
                  {"attempt_token", "stale-attempt"},
                  {"output", "partial.png"}, {"log", "partial.log"},
                  {"error", "stopped"}, {"started_at", legacyCreated},
                  {"ended_at", legacyEnded}},
                 {{"task_id", "resume-stopped-job-3"}, {"frame", 3},
                  {"state", static_cast<int>(TaskState::Stopped)},
                  {"error", "stopped"}, {"ended_at", legacyEnded}}
             })},
             {"created_at", legacyCreated}, {"finished_at", legacyEnded}}
        }).dump(2);
        ConfigStore retryAllStore(retryAllRoot);
        Config retryAllConfig = retryAllStore.Load();
        NodeApp retryAllApp(std::move(retryAllStore), retryAllConfig, true);
        std::wstring retryAllError;
        const bool retryAllAccepted =
            retryAllApp.RetryFailedFrames("retry-all-job", retryAllError);
        auto retryAllJob = retryAllApp.FindJob("retry-all-job");
        bool retryAllResetOnlyFailures = retryAllAccepted && retryAllJob &&
            retryAllJob->stage == JobStage::Rendering &&
            retryAllJob->finishedAt.time_since_epoch() ==
                std::chrono::system_clock::duration::zero() &&
            retryAllJob->frames.size() == 3 &&
            retryAllJob->frames[0].state == TaskState::Completed;
        if (retryAllResetOnlyFailures) {
            for (size_t index : {size_t{1}, size_t{2}}) {
                const auto& frame = retryAllJob->frames[index];
                retryAllResetOnlyFailures = retryAllResetOnlyFailures &&
                    frame.state == TaskState::Pending && frame.error.empty() &&
                    frame.assignedNodeId.empty() && frame.assignedNodeName.empty() &&
                    frame.assignedPackageId.empty() && frame.attemptToken.empty() &&
                    frame.startedAt.time_since_epoch() ==
                        std::chrono::system_clock::duration::zero() &&
                    frame.endedAt.time_since_epoch() ==
                        std::chrono::system_clock::duration::zero();
            }
        }
        NodeApp retryAllReloaded(
            ConfigStore(retryAllRoot), retryAllConfig, true);
        auto retryAllPersisted = retryAllReloaded.FindJob("retry-all-job");
        const bool retryAllWasPersisted = retryAllPersisted &&
            retryAllPersisted->stage == JobStage::Rendering &&
            retryAllPersisted->frames.size() == 3 &&
            retryAllPersisted->frames[0].state == TaskState::Completed &&
            retryAllPersisted->frames[1].state == TaskState::Pending &&
            retryAllPersisted->frames[2].state == TaskState::Pending;
        if (!retryAllResetOnlyFailures || !retryAllWasPersisted) {
            report = L"Retry-all failed frames self-test failed: " + retryAllError;
            return false;
        }
        std::wstring retryOneError;
        const bool completedFrameRejected = !retryAllApp.RetryFrame(
            "retry-one-job", "retry-one-job-10", retryOneError);
        const bool failedFrameAccepted = retryAllApp.RetryFrame(
            "retry-one-job", "retry-one-job-11", retryOneError);
        auto retryOneJob = retryAllApp.FindJob("retry-one-job");
        const bool retryOneResetOnlyTarget = completedFrameRejected &&
            failedFrameAccepted && retryOneJob && retryOneJob->frames.size() == 2 &&
            retryOneJob->stage == JobStage::Rendering &&
            retryOneJob->frames[0].state == TaskState::Completed &&
            retryOneJob->frames[1].state == TaskState::Pending &&
            retryOneJob->frames[1].error.empty() &&
            retryOneJob->frames[1].assignedNodeId.empty() &&
            retryOneJob->frames[1].assignedPackageId.empty() &&
            retryOneJob->frames[1].attemptToken.empty() &&
            retryOneJob->frames[1].endedAt.time_since_epoch() ==
                std::chrono::system_clock::duration::zero();
        NodeApp retryOneReloaded(ConfigStore(retryAllRoot), retryAllConfig, true);
        auto retryOnePersisted = retryOneReloaded.FindJob("retry-one-job");
        if (!retryOneResetOnlyTarget || !retryOnePersisted ||
            retryOnePersisted->frames[0].state != TaskState::Completed ||
            retryOnePersisted->frames[1].state != TaskState::Pending) {
            report = L"Retry-one failed frame self-test failed: " + retryOneError;
            return false;
        }
        std::wstring resumeError;
        const bool nonStoppedRejected = !retryAllApp.ResumeStoppedJob(
            "retry-all-job", resumeError);
        const bool resumeAccepted = retryAllApp.ResumeStoppedJob(
            "resume-stopped-job", resumeError);
        auto resumedJob = retryAllApp.FindJob("resume-stopped-job");
        bool resumePreservedCompleted = nonStoppedRejected && resumeAccepted &&
            resumedJob && resumedJob->stage == JobStage::Testing &&
            resumedJob->finishedAt.time_since_epoch() ==
                std::chrono::system_clock::duration::zero() &&
            resumedJob->frames.size() == 3 &&
            resumedJob->frames[0].state == TaskState::Completed &&
            resumedJob->frames[0].assignedNodeId == "completed-node" &&
            resumedJob->frames[0].output == L"keep-completed.png";
        if (resumePreservedCompleted) {
            for (size_t index : {size_t{1}, size_t{2}}) {
                const auto& frame = resumedJob->frames[index];
                resumePreservedCompleted = resumePreservedCompleted &&
                    frame.state == TaskState::Pending && frame.error.empty() &&
                    frame.assignedNodeId.empty() && frame.assignedNodeName.empty() &&
                    frame.assignedPackageId.empty() && frame.attemptToken.empty() &&
                    frame.output.empty() && frame.log.empty() &&
                    frame.artifacts.empty() &&
                    frame.startedAt.time_since_epoch() ==
                        std::chrono::system_clock::duration::zero() &&
                    frame.endedAt.time_since_epoch() ==
                        std::chrono::system_clock::duration::zero();
            }
        }
        NodeApp resumeReloaded(ConfigStore(retryAllRoot), retryAllConfig, true);
        auto resumePersisted = resumeReloaded.FindJob("resume-stopped-job");
        if (!resumePreservedCompleted || !resumePersisted ||
            resumePersisted->stage != JobStage::Testing ||
            resumePersisted->frames.size() != 3 ||
            resumePersisted->frames[0].state != TaskState::Completed ||
            resumePersisted->frames[1].state != TaskState::Pending ||
            resumePersisted->frames[2].state != TaskState::Pending) {
            report = L"Resume stopped frames self-test failed: " + resumeError;
            return false;
        }
        fs::path joinRoot = root / L"join-config";
        ConfigStore joinStore(joinRoot);
        Config joinConfig = joinStore.Load();
        std::string previousCredential = joinConfig.clusterToken;
        NodeApp joinApp(std::move(joinStore), joinConfig, true);
        Config joinUpdate = joinApp.GetConfig();
        joinUpdate.inviteCode = joinUpdate.inviteCode == "654321" ? "123456" : "654321";
        if (!joinApp.UpdateConfig(joinUpdate, error)) {
            report = L"Entering a six-digit invite code was rejected: " + error;
            return false;
        }
        Config pendingJoin = joinApp.GetConfig();
        if (!pendingJoin.pairingPending ||
            pendingJoin.clusterToken == previousCredential ||
            pendingJoin.inviteCode != joinUpdate.inviteCode || previousCredential.empty()) {
            report = L"Six-digit matching ID did not rotate trust state";
            return false;
        }
        InitializeLogging(root);
        LogInfo("self-test-log-marker");
        fs::path logPath = CurrentLogPath();
        std::ifstream logInput(logPath, std::ios::binary);
        std::string logText((std::istreambuf_iterator<char>(logInput)), {});
        if (!fs::is_regular_file(logPath) ||
            logText.find("self-test-log-marker") == std::string::npos) {
            report = L"Persistent logging self-test failed";
            return false;
        }
        HardwareInfo hardware = QueryHardwareInfo();
        if (hardware.cpuModel.empty()) {
            report = L"CPU model detection self-test failed";
            return false;
        }
        if (HmacSha256("key", "The quick brown fox jumps over the lazy dog") !=
            "f7bc83f430538424b13298e6aa6fb143ef4d59a14946175997479dbc2d1a3cd8") {
            report = L"HMAC-SHA256 自检失败";
            return false;
        }
        fs::path hashFixture = root / L"sha256-fixture.bin";
        {
            std::ofstream fixture(hashFixture, std::ios::binary | std::ios::trunc);
            fixture << "abc";
        }
        if (Sha256File(hashFixture) !=
            "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad") {
            report = L"Streaming file SHA-256 self-test failed";
            return false;
        }
        fs::path bundlePrimary = root / L"bundle-primary.bin";
        fs::path bundlePass = root / L"bundle-pass.exr";
        std::ofstream(bundlePrimary, std::ios::binary) << "primary";
        std::ofstream(bundlePass, std::ios::binary) << "pass";
        fs::path bundle = root / L"result.rmbundle";
        std::map<std::string, fs::path> extracted;
        std::atomic<bool> bundleCancelled = true;
        fs::path cancelledBundle = root / L"cancelled.rmbundle";
        std::wstring cancelledError;
        if (WriteResultBundle(cancelledBundle, {{"primary", bundlePrimary}},
                              cancelledError, &bundleCancelled) ||
            fs::exists(cancelledBundle) || fs::exists(cancelledBundle.wstring() + L".tmp")) {
            report = L"Cancelled result bundle write self-test failed";
            return false;
        }
        if (!WriteResultBundle(
                bundle,
                {{"primary", bundlePrimary},
                 {"compositor/output-0/ViewLayer/Depth0001.exr", bundlePass}},
                error) ||
            !ExtractResultBundle(bundle, root / L"bundle-extracted", extracted, error) ||
            extracted.size() != 2 || !extracted.contains("primary") ||
            !extracted.contains("compositor/output-0/ViewLayer/Depth0001.exr")) {
            report = L"Multi-output result bundle self-test failed: " + error;
            return false;
        }
        std::map<std::string, fs::path> cancelledExtraction;
        if (ExtractResultBundle(bundle, root / L"cancelled-extracted", cancelledExtraction,
                                cancelledError, &bundleCancelled) ||
            fs::exists(root / L"cancelled-extracted")) {
            report = L"Cancelled result bundle extraction self-test failed";
            return false;
        }
        fs::path unsafeBundle = root / L"unsafe.rmbundle";
        {
            std::ofstream unsafe(unsafeBundle, std::ios::binary | std::ios::trunc);
            const char magic[8] = {'R', 'M', 'B', 'N', 'D', 'L', '1', '\0'};
            uint32_t count = 1, pathSize = 8;
            uint64_t fileSize = 1;
            unsafe.write(magic, sizeof(magic));
            unsafe.write(reinterpret_cast<const char*>(&count), sizeof(count));
            unsafe.write(reinterpret_cast<const char*>(&pathSize), sizeof(pathSize));
            unsafe.write(reinterpret_cast<const char*>(&fileSize), sizeof(fileSize));
            unsafe.write("../x.exr", pathSize);
            unsafe.put('x');
        }
        extracted.clear();
        std::wstring unsafeError;
        if (ExtractResultBundle(unsafeBundle, root / L"unsafe-extracted", extracted,
                                unsafeError) || fs::exists(root / L"x.exr")) {
            report = L"Unsafe result bundle path was accepted";
            return false;
        }
        const std::string staleHash(64, 'a');
        fs::path staleCache = ProjectCacheDirectory(root, staleHash);
        fs::create_directories(staleCache);
        {
            std::ofstream(staleCache / L"source.blend", std::ios::binary) << "cached";
            std::ofstream(staleCache / L"cache-entry.json", std::ios::binary)
                << json{{"project_hash", staleHash}}.dump();
        }
        TouchCacheEntry(root, staleHash);
        auto cacheNow = fs::file_time_type::clock::now();
        fs::last_write_time(staleCache / L"last-used", cacheNow - std::chrono::hours(24 * 8));
        if (!CacheEntryValid(root, staleHash) ||
            CleanupExpiredProjectCaches(root, cacheNow) != 1 || fs::exists(staleCache)) {
            report = L"Seven-day project cache cleanup self-test failed";
            return false;
        }
        const std::string staleUploadHash(64, 'c');
        fs::path staleUpload = ProjectUploadDirectory(root, staleUploadHash);
        fs::create_directories(staleUpload);
        fs::path stalePartial = staleUpload / L"source.blend.partial";
        std::ofstream(stalePartial, std::ios::binary) << "partial";
        fs::last_write_time(stalePartial, cacheNow - std::chrono::hours(24 * 8));
        if (CleanupExpiredProjectUploads(root, cacheNow) != 1 || fs::exists(staleUpload)) {
            report = L"Seven-day partial project upload cleanup self-test failed";
            return false;
        }
        const std::string compatibilityHash(64, 'b');
        fs::path compatibilityCache = ProjectCacheDirectory(root, compatibilityHash);
        fs::create_directories(compatibilityCache);
        std::ofstream(compatibilityCache / L"source.blend", std::ios::binary) << "cached";
        std::wstring compatibilityMetadataError;
        if (!WriteCacheMetadata(root, compatibilityHash, "5.2", "CYCLES", true,
                                compatibilityMetadataError)) {
            report = L"Disabled add-on compatibility review self-test setup failed";
            return false;
        }
        {
            fs::path metadata = compatibilityCache / L"cache-entry.json";
            json legacyMetadata = json::parse(std::ifstream(metadata, std::ios::binary));
            legacyMetadata["addons"] = {"legacy_missing_addon@999.0"};
            std::ofstream(metadata, std::ios::binary | std::ios::trunc)
                << legacyMetadata.dump(2);
        }
        if (!CacheMetadataMatches(root, compatibilityHash, "5.2", "CYCLES", true)) {
            report = L"Disabled add-on compatibility review self-test failed";
            return false;
        }
        Config signatureConfig;
        signatureConfig.inviteCode = "123456";
        signatureConfig.clusterToken = "legacy-token-123";
        json discoveryV2 = {
            {"protocol", 2}, {"node_id", "signed-node"}, {"nonce", "0011223344556677"},
            {"port", 8391}, {"share", true}, {"assigned", 0}, {"task_id", ""},
            {"addresses", std::vector<std::string>{"192.168.1.10"}},
            {"name", "WORKSTATION"}, {"cpu", 15}, {"idle", 180},
            {"cpu_model", "Test CPU"},
            {"gpu_models", std::vector<std::string>{"Test GPU"}},
        };
        std::string discoveryProof = DiscoverySignature(signatureConfig, discoveryV2);
        discoveryV2["idle"] = 181;
        if (ConstantTimeEqual(discoveryProof,
                              DiscoverySignature(signatureConfig, discoveryV2))) {
            report = L"Protocol-v2 discovery state was not authenticated";
            return false;
        }
        json discoveryV5 = discoveryV2;
        discoveryV5["protocol"] = 5;
        discoveryV5["project_cache"] = true;
        discoveryV5["frame_packages"] = true;
        discoveryV5["result_bundles"] = true;
        std::string discoveryV5Proof = DiscoverySignature(signatureConfig, discoveryV5);
        discoveryV5["result_bundles"] = false;
        if (ConstantTimeEqual(discoveryV5Proof,
                              DiscoverySignature(signatureConfig, discoveryV5))) {
            report = L"Protocol-v5 result-bundle capability was not authenticated";
            return false;
        }
        json discoveryV6 = discoveryV5;
        discoveryV6["protocol"] = 6;
        discoveryV6["result_bundles"] = true;
        discoveryV6["package_cancel"] = true;
        std::string discoveryV6Proof = DiscoverySignature(signatureConfig, discoveryV6);
        discoveryV6["package_cancel"] = false;
        if (ConstantTimeEqual(discoveryV6Proof,
                              DiscoverySignature(signatureConfig, discoveryV6))) {
            report = L"Protocol-v6 cancellation capability was not authenticated";
            return false;
        }
        json discoveryV7 = discoveryV6;
        discoveryV7["protocol"] = 7;
        discoveryV7["package_cancel"] = true;
        discoveryV7["activity_job"] = "signed render";
        discoveryV7["activity_frames"] = {1, 2, 3};
        discoveryV7["activity_completed"] = 1;
        discoveryV7["activity_rendering"] = true;
        std::string discoveryV7Proof = DiscoverySignature(signatureConfig, discoveryV7);
        WorkerActivity signedActivity;
        signedActivity.jobName = "signed render";
        signedActivity.frames = {1, 2, 3};
        signedActivity.completedFrames = 1;
        signedActivity.rendering = true;
        std::string activityProof = ActivitySnapshotProof(
            signatureConfig, "0011223344556677", "signed-node", signedActivity);
        signedActivity.completedFrames = 2;
        if (ConstantTimeEqual(activityProof, ActivitySnapshotProof(
                signatureConfig, "0011223344556677", "signed-node", signedActivity))) {
            report = L"Manual-peer activity snapshot was not authenticated";
            return false;
        }
        signedActivity.completedFrames = 1;
        const std::string cleanupProof = CleanupCapabilityProof(
            signatureConfig, "0011223344556677", "signed-node", true);
        if (ConstantTimeEqual(cleanupProof, CleanupCapabilityProof(
                signatureConfig, "0011223344556677", "signed-node", false))) {
            report = L"Manual-peer cleanup capability was not authenticated";
            return false;
        }
        discoveryV7["activity_job"] = "tampered render";
        if (ConstantTimeEqual(discoveryV7Proof,
                              DiscoverySignature(signatureConfig, discoveryV7))) {
            report = L"Protocol-v7 worker activity was not authenticated";
            return false;
        }
        json discoveryV8 = discoveryV7;
        discoveryV8["activity_job"] = "signed render";
        discoveryV8["protocol"] = 8;
        discoveryV8["job_cleanup"] = true;
        std::string discoveryV8Proof = DiscoverySignature(signatureConfig, discoveryV8);
        discoveryV8["job_cleanup"] = false;
        if (ConstantTimeEqual(discoveryV8Proof,
                              DiscoverySignature(signatureConfig, discoveryV8))) {
            report = L"Protocol-v8 cleanup capability was not authenticated";
            return false;
        }
        json discoveryV9 = discoveryV8;
        discoveryV9["job_cleanup"] = true;
        discoveryV9["protocol"] = 9;
        discoveryV9["resumable_project_upload"] = true;
        discoveryV9["max_package_frames"] = 20;
        std::string discoveryV9Proof = DiscoverySignature(signatureConfig, discoveryV9);
        discoveryV9["max_package_frames"] = 1;
        if (ConstantTimeEqual(discoveryV9Proof,
                              DiscoverySignature(signatureConfig, discoveryV9))) {
            report = L"Protocol-v9 upload/package capability was not authenticated";
            return false;
        }
        json discoveryV10 = discoveryV9;
        discoveryV10["max_package_frames"] = 20;
        discoveryV10["protocol"] = 10;
        discoveryV10["matching_id_binding"] = MatchingIdBinding(signatureConfig);
        std::string discoveryV10Proof = DiscoverySignature(signatureConfig, discoveryV10);
        discoveryV10["matching_id_binding"] = "tampered";
        if (ConstantTimeEqual(discoveryV10Proof,
                              DiscoverySignature(signatureConfig, discoveryV10))) {
            report = L"Protocol-v10 matching-ID binding was not authenticated";
            return false;
        }
        json discoveryV11 = discoveryV10;
        discoveryV11["matching_id_binding"] = MatchingIdBinding(signatureConfig);
        discoveryV11["protocol"] = 11;
        discoveryV11["blender_ready"] = true;
        std::string discoveryV11Proof = DiscoverySignature(signatureConfig, discoveryV11);
        discoveryV11["blender_ready"] = false;
        if (ConstantTimeEqual(discoveryV11Proof,
                              DiscoverySignature(signatureConfig, discoveryV11))) {
            report = L"Protocol-v11 Blender readiness was not authenticated";
            return false;
        }
        json discoveryV12 = discoveryV11;
        discoveryV12["blender_ready"] = true;
        discoveryV12["protocol"] = 12;
        discoveryV12["app_version"] = "0.2.19";
        discoveryV12["peer_exchange"] = true;
        std::string discoveryV12Proof = DiscoverySignature(signatureConfig, discoveryV12);
        discoveryV12["peer_exchange"] = false;
        if (ConstantTimeEqual(discoveryV12Proof,
                              DiscoverySignature(signatureConfig, discoveryV12))) {
            report = L"Protocol-v12 peer exchange was not authenticated";
            return false;
        }
        const std::string readinessProof = BlenderReadinessProof(
            signatureConfig, "0011223344556677", "signed-node", true);
        if (ConstantTimeEqual(readinessProof, BlenderReadinessProof(
                signatureConfig, "0011223344556677", "signed-node", false))) {
            report = L"Manual-peer Blender readiness was not authenticated";
            return false;
        }
        const std::string peerExchangeProof = PeerExchangeCapabilityProof(
            signatureConfig, "0011223344556677", "signed-node", 12, "0.2.19", true);
        if (ConstantTimeEqual(peerExchangeProof, PeerExchangeCapabilityProof(
                signatureConfig, "0011223344556677", "signed-node", 12, "0.2.19", false))) {
            report = L"Manual-peer exchange capability was not authenticated";
            return false;
        }
        json discoveryV13 = discoveryV12;
        discoveryV13["protocol"] = 13;
        discoveryV13["app_version"] = "0.2.20";
        discoveryV13["peer_exchange"] = true;
        discoveryV13["direct_status"] = true;
        std::string discoveryV13Proof = DiscoverySignature(signatureConfig, discoveryV13);
        discoveryV13["direct_status"] = false;
        if (ConstantTimeEqual(discoveryV13Proof,
                              DiscoverySignature(signatureConfig, discoveryV13))) {
            report = L"Protocol-v13 direct status capability was not authenticated";
            return false;
        }
        json discoveryV14 = discoveryV13;
        discoveryV14["direct_status"] = true;
        discoveryV14["protocol"] = 14;
        discoveryV14["app_version"] = "0.2.21";
        discoveryV14["idle_ready"] = true;
        std::string discoveryV14Proof = DiscoverySignature(signatureConfig, discoveryV14);
        discoveryV14["idle_ready"] = false;
        if (ConstantTimeEqual(discoveryV14Proof,
                              DiscoverySignature(signatureConfig, discoveryV14))) {
            report = L"Protocol-v14 idle readiness was not authenticated";
            return false;
        }
        json discoveryV15 = discoveryV14;
        discoveryV15["idle_ready"] = true;
        discoveryV15["protocol"] = 15;
        discoveryV15["app_version"] = "0.2.28";
        discoveryV15["frame_start"] = true;
        std::string discoveryV15Proof = DiscoverySignature(signatureConfig, discoveryV15);
        discoveryV15["frame_start"] = false;
        if (ConstantTimeEqual(discoveryV15Proof,
                              DiscoverySignature(signatureConfig, discoveryV15))) {
            report = L"Protocol-v15 frame-start capability was not authenticated";
            return false;
        }
        json directSnapshot = {
            {"node_id", "signed-node"}, {"protocol", 13},
            {"share", true}, {"assigned", 1}, {"task_id", "signed-package"},
        };
        const std::string directStatusProof = DirectStatusProof(
            signatureConfig, "signed-node", "0011223344556677", directSnapshot);
        directSnapshot["assigned"] = 0;
        if (ConstantTimeEqual(directStatusProof, DirectStatusProof(
                signatureConfig, "signed-node", "0011223344556677", directSnapshot))) {
            report = L"Direct worker status was not authenticated";
            return false;
        }
        Config mismatchedIdConfig = signatureConfig;
        mismatchedIdConfig.inviteCode = "777777";
        if (ConstantTimeEqual(MatchingIdBinding(signatureConfig),
                              MatchingIdBinding(mismatchedIdConfig))) {
            report = L"Matching ID was not bound to the authenticated cluster";
            return false;
        }
        Peer authenticatedPeer;
        authenticatedPeer.protocolVersion = 15;
        authenticatedPeer.supportsProjectCache = true;
        authenticatedPeer.supportsFramePackages = true;
        authenticatedPeer.supportsResultBundles = true;
        authenticatedPeer.supportsPackageCancellation = true;
        authenticatedPeer.supportsJobCleanup = true;
        authenticatedPeer.supportsResumableProjectUpload = true;
        authenticatedPeer.supportsPeerExchange = true;
        authenticatedPeer.supportsDirectStatus = true;
        authenticatedPeer.supportsFrameStart = true;
        authenticatedPeer.rendererReady = true;
        authenticatedPeer.idleReadyKnown = true;
        authenticatedPeer.idleReady = true;
        authenticatedPeer.maxPackageFrames = 20;
        authenticatedPeer.appVersion = "0.2.28";
        authenticatedPeer.name = "SIGNED-NAME";
        authenticatedPeer.cpuPercent = 17;
        authenticatedPeer.idleSeconds = 181;
        authenticatedPeer.cpuModel = "SIGNED-CPU";
        authenticatedPeer.gpuModels = {"SIGNED-GPU"};
        authenticatedPeer.activity = {"signed render", {1, 2, 3}, 1, true};
        Peer legacyUpdate;
        legacyUpdate.protocolVersion = 1;
        legacyUpdate.name = "tampered-name";
        legacyUpdate.cpuPercent = 99;
        legacyUpdate.idleSeconds = 999;
        PreserveAuthenticatedV2Metadata(legacyUpdate, authenticatedPeer);
        if (legacyUpdate.protocolVersion != 15 || !legacyUpdate.supportsProjectCache ||
            !legacyUpdate.supportsFramePackages || !legacyUpdate.supportsResultBundles ||
            !legacyUpdate.supportsPackageCancellation || !legacyUpdate.supportsJobCleanup ||
            !legacyUpdate.supportsResumableProjectUpload ||
            !legacyUpdate.supportsPeerExchange || !legacyUpdate.supportsDirectStatus ||
            !legacyUpdate.supportsFrameStart ||
            !legacyUpdate.idleReadyKnown || !legacyUpdate.idleReady ||
            legacyUpdate.maxPackageFrames != 20 ||
            legacyUpdate.name != "SIGNED-NAME" ||
            legacyUpdate.cpuPercent != 17 || legacyUpdate.idleSeconds != 181 ||
            legacyUpdate.cpuModel != "SIGNED-CPU" ||
            legacyUpdate.gpuModels != std::vector<std::string>{"SIGNED-GPU"} ||
            legacyUpdate.activity.jobName != "signed render" ||
            legacyUpdate.activity.frames != std::vector<int>{1, 2, 3} ||
            legacyUpdate.activity.completedFrames != 1 ||
            !legacyUpdate.activity.rendering) {
            report = L"Legacy discovery downgraded authenticated v2 metadata";
            return false;
        }
        Peer legacyTimingPeer = authenticatedPeer;
        legacyTimingPeer.protocolVersion = 14;
        legacyTimingPeer.supportsFrameStart = false;
        if (EffectiveRemotePackageLimit(legacyTimingPeer) != 1 ||
            EffectiveRemotePackageLimit(authenticatedPeer) != 20) {
            report = L"Legacy frame-start capability did not force single-frame fallback";
            return false;
        }
        Peer compatibilityBeacon;
        compatibilityBeacon.protocolVersion = kBlenderReadinessProtocol;
        compatibilityBeacon.appVersion = authenticatedPeer.appVersion;
        Peer actualDowngrade;
        actualDowngrade.protocolVersion = 14;
        actualDowngrade.appVersion = "0.2.25";
        if (!IsSameVersionCompatibilityBeacon(compatibilityBeacon, authenticatedPeer) ||
            IsSameVersionCompatibilityBeacon(actualDowngrade, authenticatedPeer)) {
            report = L"Discovery compatibility beacon downgrade classification failed";
            return false;
        }
        if (!ShouldAbortPackageBeforeFirstFrame("package-a", "package-a", false) ||
            ShouldAbortPackageBeforeFirstFrame("package-a", "package-a", true) ||
            !ShouldAbortPackageBeforeFirstFrame("package-b", "package-a", true)) {
            report = L"Package pre-frame cancellation classification failed";
            return false;
        }
        if (DirectedBroadcastAddress("192.168.29.75", 23) != "192.168.29.255" ||
            DirectedBroadcastAddress("10.20.30.40", 24) != "10.20.30.255" ||
            DirectedBroadcastAddress("100.64.1.2", 32).has_value() ||
            DirectedBroadcastAddress("not-an-ip", 24).has_value()) {
            report = L"IPv4 directed-broadcast calculation failed";
            return false;
        }
        auto routes = BroadcastCapableInterfaces({
            {"192.168.29.10", std::string("192.168.29.255")},
            {"192.168.29.20", std::string("192.168.29.255")},
            {"100.64.1.2", std::nullopt},
        });
        if (routes.size() != 2 || routes[0].address == routes[1].address ||
            routes[0].broadcast != routes[1].broadcast) {
            report = L"Per-interface broadcast route planning failed";
            return false;
        }
        Peer legacyPeer;
        legacyPeer.currentTaskKnown = false;
        legacyPeer.currentTaskId.clear();
        legacyPeer.assigned = 1;
        if (PeerExplicitlyStoppedTask(legacyPeer, "long-running-frame")) {
            report = L"Legacy worker task-state compatibility failed";
            return false;
        }
        legacyPeer.assigned = 0;
        if (!PeerExplicitlyStoppedTask(legacyPeer, "long-running-frame")) {
            report = L"Legacy worker stopped-task recovery failed";
            return false;
        }
        legacyPeer.currentTaskKnown = true;
        if (!PeerExplicitlyStoppedTask(legacyPeer, "long-running-frame")) {
            report = L"Known worker task-state recovery failed";
            return false;
        }
        http::Server server;
        fs::path received = root / L"received.bin";
        fs::path cancelledReceive = root / L"cancelled.bin";
        std::atomic<bool> cancelInbound = true;
        server.Post("/stream", [&](const http::Request&, http::Response& response,
                                   const http::ContentReader& reader) {
            std::wstring streamError;
            bool ok = StreamRequestToFile(reader, received, streamError);
            response.status = ok ? 200 : 500;
        });
        server.Post("/cancelled-stream", [&](const http::Request&,
                                              http::Response& response,
                                              const http::ContentReader& reader) {
            std::wstring streamError;
            bool ok = StreamRequestToFile(
                reader, cancelledReceive, streamError, &cancelInbound);
            response.status = ok ? 200 : 499;
        });
        server.Post("/slow-empty", [&](const http::Request&,
                                         http::Response& response) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1200));
            response.status = 200;
        });
        server.Post("/slow-bytes", [&](const http::Request& request,
                                         http::Response& response) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1200));
            response.status = request.body == "chunk-probe" ? 200 : 400;
        });
        server.Post("/cancel-bytes", [&](const http::Request&,
                                           http::Response& response) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2000));
            response.status = 200;
        });
        int port = server.bind_to_any_port("127.0.0.1");
        if (port <= 0) {
            report = L"回环端口绑定失败";
            return false;
        }
        std::thread thread([&] { server.listen_after_bind(); });
        fs::path source = root / L"source.bin";
        {
            std::ofstream output(source, std::ios::binary);
            output << std::string(2 * 1024 * 1024, 'R');
        }
        auto result = PostFile("127.0.0.1", port, "/stream", {}, source);
        auto cancelledResult = PostFile(
            "127.0.0.1", port, "/cancelled-stream", {}, source);
        std::atomic<bool> preCancelledRequest = true;
        const auto preCancelledStarted = std::chrono::steady_clock::now();
        auto preCancelledResult = PostEmpty(
            "127.0.0.1", port, "/slow-empty", {}, &preCancelledRequest,
            nullptr, 2);
        const auto preCancelledElapsed =
            std::chrono::steady_clock::now() - preCancelledStarted;
        const auto slowChunkStarted = std::chrono::steady_clock::now();
        auto slowChunkResult = PostBytes(
            "127.0.0.1", port, "/slow-bytes", {}, "chunk-probe",
            nullptr, nullptr, 2);
        const auto slowChunkElapsed =
            std::chrono::steady_clock::now() - slowChunkStarted;
        std::atomic<bool> cancelChunk = false;
        std::thread cancelChunkThread([&] {
            std::this_thread::sleep_for(std::chrono::milliseconds(150));
            cancelChunk = true;
        });
        const auto cancelledChunkStarted = std::chrono::steady_clock::now();
        auto cancelledChunkResult = PostBytes(
            "127.0.0.1", port, "/cancel-bytes", {}, "chunk-probe",
            nullptr, &cancelChunk, 5);
        const auto cancelledChunkElapsed =
            std::chrono::steady_clock::now() - cancelledChunkStarted;
        cancelChunkThread.join();
        const auto slowCommitStarted = std::chrono::steady_clock::now();
        auto slowCommitResult = PostEmpty(
            "127.0.0.1", port, "/slow-empty", {}, nullptr, nullptr, 2);
        const auto slowCommitElapsed =
            std::chrono::steady_clock::now() - slowCommitStarted;
        server.stop();
        thread.join();
        if (!result || result->status != 200 || fs::file_size(received) != fs::file_size(source) ||
            (cancelledResult && cancelledResult->status == 200) || fs::exists(cancelledReceive)) {
            report = L"流式任务传输失败";
            return false;
        }
        if (!slowCommitResult || slowCommitResult->status != 200 ||
            slowCommitElapsed < std::chrono::seconds(1)) {
            report = L"Large project commit timeout self-test failed";
            return false;
        }
        if (preCancelledResult ||
            preCancelledResult.error() != http::Error::Canceled ||
            preCancelledElapsed >= std::chrono::milliseconds(250)) {
            report = L"Pre-cancelled large project request self-test failed";
            return false;
        }
        if (!slowChunkResult || slowChunkResult->status != 200 ||
            slowChunkElapsed < std::chrono::seconds(1)) {
            report = L"Large project chunk timeout self-test failed";
            return false;
        }
        if (cancelledChunkResult ||
            cancelledChunkElapsed >= std::chrono::milliseconds(1500)) {
            const auto elapsedMilliseconds =
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    cancelledChunkElapsed).count();
            report = L"Large project chunk cancellation self-test failed; response=" +
                std::to_wstring(cancelledChunkResult ? 1 : 0) + L"; status=" +
                std::to_wstring(cancelledChunkResult ? cancelledChunkResult->status : 0) +
                L"; error=" + Utf8ToWide(http::to_string(cancelledChunkResult.error())) +
                L"; elapsed_ms=" + std::to_wstring(elapsedMilliseconds);
            return false;
        }
        bool clusterSuitePassed = true;
        {
            fs::path originRoot = root / L"origin";
            fs::path workerRoot = root / L"worker";
            ConfigStore originStore(originRoot);
            ConfigStore workerStore(workerRoot);
            Config originConfig = originStore.Load();
            Config workerConfig = workerStore.Load();
            std::string sharedToken = NewRandomHex(16);
            http::Server originPortProbe;
            http::Server workerPortProbe;
            int originPort = originPortProbe.bind_to_any_port("127.0.0.1");
            int workerPort = workerPortProbe.bind_to_any_port("127.0.0.1");
            originPortProbe.stop();
            workerPortProbe.stop();
            if (originPort <= 0 || workerPort <= 0 || originPort == workerPort) {
                report = L"自检无法分配本机动态端口";
                return false;
            }
            originConfig.nodeName = "SELFTEST-ORIGIN";
            originConfig.nodeId = "selftest-origin";
            originConfig.inviteCode = "123456";
            originConfig.clusterToken = NewRandomHex(16);
            originConfig.httpPort = originPort;
            originConfig.discoveryPort = 0;
            workerConfig.nodeName = "SELFTEST-WORKER";
            workerConfig.nodeId = "selftest-worker";
            workerConfig.inviteCode = "654321";
            workerConfig.clusterToken = sharedToken;
            workerConfig.httpPort = workerPort;
            workerConfig.discoveryPort = 0;
            workerConfig.shareEnabled = true;
            workerConfig.idleAfterSeconds = 1;

            NodeApp origin(std::move(originStore), originConfig, true,
                           std::chrono::milliseconds(2500));
            NodeApp worker(std::move(workerStore), workerConfig, true);
            std::wstring startError;
            bool started = worker.Start(startError) && origin.Start(startError);
            if (started) {
                http::Server malformedServer;
                malformedServer.Get("/health", [](const http::Request&,
                                                    http::Response& response) {
                    response.set_content(R"({"node_id":123})", "application/json");
                });
                int malformedPort = malformedServer.bind_to_any_port("127.0.0.1");
                std::thread malformedThread;
                if (malformedPort > 0)
                    malformedThread = std::thread([&] { malformedServer.listen_after_bind(); });
                std::wstring malformedResult;
                bool malformedRejected = malformedPort > 0 && !origin.AddManualPeer(
                    "127.0.0.1:" + std::to_string(malformedPort), malformedResult);
                malformedServer.stop();
                if (malformedThread.joinable()) malformedThread.join();
                Config pairingConfig = origin.GetConfig();
                pairingConfig.inviteCode = workerConfig.inviteCode;
                bool pairingStarted = origin.UpdateConfig(pairingConfig, error) &&
                    origin.GetConfig().pairingPending;
                std::wstring connectResult;
                bool manuallyConnected = origin.AddManualPeer(
                    "127.0.0.1:" + std::to_string(workerConfig.httpPort), connectResult);
                Config pairedConfig = origin.GetConfig();
                bool paired = !pairedConfig.pairingPending &&
                    pairedConfig.clusterToken == sharedToken;
                std::wstring reverseConnectResult;
                bool reverseConnected = paired && worker.AddManualPeer(
                    "127.0.0.1:" + std::to_string(originConfig.httpPort),
                    reverseConnectResult);
                Config originMatchIdConfig = origin.GetConfig();
                Config workerMatchIdConfig = worker.GetConfig();
                const std::string originTokenBeforeIdChange =
                    originMatchIdConfig.clusterToken;
                const std::string workerTokenBeforeIdChange =
                    workerMatchIdConfig.clusterToken;
                const std::string originEndpointBeforeIdChange =
                    originMatchIdConfig.manualPeerEndpoint;
                const std::string workerEndpointBeforeIdChange =
                    workerMatchIdConfig.manualPeerEndpoint;
                originMatchIdConfig.inviteCode = "777777";
                workerMatchIdConfig.inviteCode = "777777";
                std::wstring matchIdError;
                bool originMatchIdUpdated = reverseConnected &&
                    origin.UpdateConfig(originMatchIdConfig, matchIdError);
                Config isolatedOriginConfig = origin.GetConfig();
                bool mismatchedIdDisconnectsImmediately = originMatchIdUpdated &&
                    isolatedOriginConfig.pairingPending &&
                    isolatedOriginConfig.clusterToken != originTokenBeforeIdChange &&
                    isolatedOriginConfig.manualPeerEndpoint ==
                        originEndpointBeforeIdChange &&
                    origin.Peers().empty();
                if (!mismatchedIdDisconnectsImmediately) {
                    report = L"Mismatched matching ID isolation self-test failed";
                    origin.Stop();
                    worker.Stop();
                    return false;
                }
                bool workerMatchIdUpdated = worker.UpdateConfig(
                    workerMatchIdConfig, matchIdError);
                bool automaticallyRepaired = false;
                for (int attempt = 0; attempt < 40 && workerMatchIdUpdated; ++attempt) {
                    Config currentOrigin = origin.GetConfig();
                    Config currentWorker = worker.GetConfig();
                    if (!currentOrigin.pairingPending && !currentWorker.pairingPending &&
                        currentOrigin.clusterToken == currentWorker.clusterToken &&
                        !origin.Peers().empty() && !worker.Peers().empty()) {
                        automaticallyRepaired = true;
                        break;
                    }
                    Sleep(100);
                }
                Config reconnectedOriginConfig = origin.GetConfig();
                Config reconnectedWorkerConfig = worker.GetConfig();
                bool matchingIdsReconnect = automaticallyRepaired &&
                    !reconnectedOriginConfig.pairingPending &&
                    !reconnectedWorkerConfig.pairingPending &&
                    reconnectedOriginConfig.inviteCode == "777777" &&
                    reconnectedWorkerConfig.inviteCode == "777777" &&
                    reconnectedOriginConfig.clusterToken ==
                        reconnectedWorkerConfig.clusterToken &&
                    reconnectedOriginConfig.clusterToken != originTokenBeforeIdChange &&
                    reconnectedWorkerConfig.clusterToken != workerTokenBeforeIdChange &&
                    reconnectedOriginConfig.manualPeerEndpoint ==
                        originEndpointBeforeIdChange &&
                    reconnectedWorkerConfig.manualPeerEndpoint ==
                        workerEndpointBeforeIdChange;
                if (!matchingIdsReconnect) {
                    report = L"Matching ID automatic reconnection self-test failed: " +
                        (matchIdError.empty() ? reverseConnectResult : matchIdError);
                    origin.Stop();
                    worker.Stop();
                    return false;
                }
                const Config activeClusterConfig = reconnectedOriginConfig;
                auto seedResumablePrefix = [&] (
                    const fs::path& payload, bool projectBundle,
                    const std::string& softwareVersion, const std::string& renderEngine,
                    bool requiresGpu,
                    std::string& payloadHash, uint64_t& seededOffset,
                    std::wstring& seedError) {
                    std::error_code sizeError;
                    const uint64_t total = fs::file_size(payload, sizeError);
                    payloadHash = sizeError ? std::string{} : Sha256File(payload);
                    if (sizeError || total < 2 || !SafeSha256(payloadHash)) {
                        seedError = L"resumable upload fixture is invalid";
                        return false;
                    }
                    const size_t prefixSize = static_cast<size_t>(
                        std::min<uint64_t>(17, total - 1));
                    std::string prefix(prefixSize, '\0');
                    std::ifstream input(payload, std::ios::binary);
                    input.read(prefix.data(), static_cast<std::streamsize>(prefix.size()));
                    if (static_cast<size_t>(input.gcount()) != prefix.size()) {
                        seedError = L"resumable upload fixture could not be read";
                        return false;
                    }
                    const http::Headers common = {
                        {"X-RM-Token", activeClusterConfig.clusterToken},
                        {"X-RM-Project", payloadHash},
                        {"X-RM-Total", std::to_string(total)},
                        {"X-RM-Blender", softwareVersion},
                        {"X-RM-Engine", renderEngine},
                        {"X-RM-Requires-GPU", requiresGpu ? "1" : "0"},
                        {"X-RM-Project-Bundle", projectBundle ? "1" : "0"},
                    };
                    http::Headers chunkHeaders = common;
                    chunkHeaders.emplace("X-RM-Offset", "0");
                    chunkHeaders.emplace("X-RM-Chunk-SHA256", Sha256Prefix(prefix, 64));
                    auto chunk = PostBytes(
                        "127.0.0.1", workerConfig.httpPort, "/v3/cache/chunk",
                        chunkHeaders, prefix, nullptr, nullptr, 5);
                    if (!chunk || chunk->status != 200) {
                        seedError = L"initial resumable project chunk was rejected";
                        return false;
                    }
                    http::Client statusClient("127.0.0.1", workerConfig.httpPort);
                    statusClient.set_connection_timeout(2, 0);
                    statusClient.set_read_timeout(2, 0);
                    auto status = statusClient.Get(
                        "/v3/cache/upload-status?hash=" + payloadHash, common);
                    bool cached = false;
                    std::string parseError;
                    if (!status || status->status != 200 ||
                        !ParseProjectUploadStatus(
                            status->body, cached, seededOffset, parseError) || cached ||
                        seededOffset != prefix.size()) {
                        seedError = L"resumable upload status did not preserve the prefix";
                        return false;
                    }
                    std::error_code partialError;
                    const fs::path partial = ProjectUploadPartial(workerRoot, payloadHash);
                    const bool preserved = fs::is_regular_file(partial, partialError) &&
                        !partialError && fs::file_size(partial, partialError) == seededOffset &&
                        !partialError;
                    if (!preserved)
                        seedError = L"resumable upload prefix was not preserved on disk";
                    return preserved;
                };
                auto verifyResumableCommit = [&] (
                    const std::string& payloadHash, uint64_t total, bool projectBundle,
                    const std::string& softwareVersion, const std::string& renderEngine,
                    bool requiresGpu, std::wstring& verifyError) {
                    const http::Headers common = {
                        {"X-RM-Token", activeClusterConfig.clusterToken},
                        {"X-RM-Project", payloadHash},
                        {"X-RM-Total", std::to_string(total)},
                        {"X-RM-Blender", softwareVersion},
                        {"X-RM-Engine", renderEngine},
                        {"X-RM-Requires-GPU", requiresGpu ? "1" : "0"},
                        {"X-RM-Project-Bundle", projectBundle ? "1" : "0"},
                    };
                    http::Client statusClient("127.0.0.1", workerConfig.httpPort);
                    statusClient.set_connection_timeout(2, 0);
                    statusClient.set_read_timeout(2, 0);
                    auto status = statusClient.Get(
                        "/v3/cache/upload-status?hash=" + payloadHash, common);
                    const json body = status
                        ? json::parse(status->body, nullptr, false) : json{};
                    if (!status || status->status != 200 || body.is_discarded() ||
                        !body.is_object() || !body.value("cached", false) ||
                        !body.contains("offset") || !body["offset"].is_number_unsigned() ||
                        !body.contains("total") || !body["total"].is_number_unsigned() ||
                        body["offset"].get<uint64_t>() != total ||
                        body["total"].get<uint64_t>() != total ||
                        fs::exists(ProjectUploadDirectory(workerRoot, payloadHash)) ||
                        !CacheMetadataMatches(workerRoot, payloadHash, softwareVersion,
                                              renderEngine, requiresGpu, projectBundle)) {
                        verifyError = L"resumable upload did not commit a complete cache entry";
                        return false;
                    }
                    return true;
                };
                bool hardwareReported = false;
                bool activityProjected = false;
                std::optional<Peer> connectedPeer;
                for (const auto& peer : origin.Peers()) {
                    if (peer.nodeId == workerConfig.nodeId && !peer.cpuModel.empty()) {
                        hardwareReported = true;
                        connectedPeer = peer;
                        break;
                    }
                }
                if (connectedPeer) {
                    if (!connectedPeer->supportsResumableProjectUpload) {
                        report = L"Connected worker lacks resumable project upload capability";
                        origin.Stop();
                        worker.Stop();
                        return false;
                    }
                    Peer activityPeer = *connectedPeer;
                    activityPeer.activity = {
                        "activity-projection-self-test", {10, 11, 12}, 1, true};
                    activityPeer.lastSeen = std::chrono::steady_clock::now();
                    origin.InjectPeerForTest(activityPeer);
                    for (const auto& visible : origin.Peers()) {
                        activityProjected = activityProjected ||
                            (visible.nodeId == workerConfig.nodeId &&
                             visible.activity.jobName == "activity-projection-self-test" &&
                             visible.activity.frames == std::vector<int>{10, 11, 12} &&
                             visible.activity.completedFrames == 1 &&
                             visible.activity.rendering);
                    }
                    connectedPeer->lastSeen = std::chrono::steady_clock::now();
                    origin.InjectPeerForTest(*connectedPeer);
                }

                fs::path prepared = root / L"prepared.blend";
                {
                    std::ofstream blend(prepared, std::ios::binary);
                    blend << "RenderMesh mock blend payload";
                }
                PreflightReport preflight;
                preflight.passed = true;
                preflight.sourceProject = prepared;
                preflight.preparedProject = prepared;
                preflight.softwareVersion = "4.5";
                preflight.renderEngine = "CYCLES";
                preflight.outputFormat = "PNG";
                preflight.outputExtension = ".png";
                fs::path finalOutputRoot = root / L"origin-final";
                for (int frame = 0; frame <= 7; ++frame)
                    preflight.primaryOutputs[frame] = finalOutputRoot /
                        (L"primary-" + std::to_wstring(frame) + L".png");
                OutputTarget selfTestOutput;
                selfTestOutput.id = "output-0";
                selfTestOutput.name = "SelfTest Multi Output";
                selfTestOutput.directory = finalOutputRoot / L"compositor";
                preflight.compositorOutputs.push_back(std::move(selfTestOutput));
                std::wstring submitError;
                const fs::path networkTexRoot = root / L"network-tex-project";
                fs::create_directories(networkTexRoot / L"tex" / L"sequence");
                const fs::path networkTexBlend = networkTexRoot / L"shot.blend";
                std::ofstream(networkTexBlend, std::ios::binary)
                    << "RenderMesh network tex mock blend";
                std::ofstream(networkTexRoot / L"tex" / L"sequence" / L"0001.exr",
                              std::ios::binary) << "network-tex-frame";
                const fs::path expectedNetworkBundle = root / L"network-tex-expected.rmp";
                std::wstring resumeSeedError;
                std::wstring expectedBundleError;
                std::vector<std::pair<std::string, fs::path>> expectedBundleFiles = {
                    {"source.blend", networkTexBlend},
                    {"tex/sequence/0001.exr",
                     networkTexRoot / L"tex" / L"sequence" / L"0001.exr"},
                };
                std::sort(expectedBundleFiles.begin(), expectedBundleFiles.end(),
                    [](const auto& left, const auto& right) {
                        return left.first < right.first;
                    });
                std::string expectedBundleHash;
                uint64_t bundleResumeOffset = 0;
                const bool bundleResumeSeeded = !WriteProjectBundle(
                        expectedNetworkBundle, expectedBundleFiles, expectedBundleError,
                        nullptr).empty() &&
                    seedResumablePrefix(
                        expectedNetworkBundle, true, preflight.softwareVersion,
                        preflight.renderEngine, preflight.requiresGpu, expectedBundleHash,
                        bundleResumeOffset, resumeSeedError);
                if (!bundleResumeSeeded) {
                    report = L"Bundled resumable upload prefix self-test failed: " +
                        (resumeSeedError.empty() ? expectedBundleError : resumeSeedError);
                    origin.Stop();
                    worker.Stop();
                    return false;
                }
                PreflightReport networkTexPreflight = preflight;
                networkTexPreflight.sourceProject = networkTexBlend;
                networkTexPreflight.preparedProject = networkTexBlend;
                networkTexPreflight.frameStart = 1;
                networkTexPreflight.frameEnd = 1;
                networkTexPreflight.frameStep = 1;
                networkTexPreflight.primaryOutputs = {
                    {1, finalOutputRoot / L"network-tex-1.png"}};
                const std::string networkTexJobId = origin.CreatePreparedDraft(
                    "Network tex bundle", networkTexPreflight, submitError);
                const uint64_t expectedBundleBytes = fs::file_size(expectedNetworkBundle);
                bool networkTexReady = false;
                for (int attempt = 0; attempt < 240 && !networkTexReady; ++attempt) {
                    auto current = origin.FindJob(networkTexJobId);
                    networkTexReady = current &&
                        current->stage == JobStage::ReadyToConfigure &&
                        current->projectBundle &&
                        current->projectHash == expectedBundleHash &&
                        !fs::exists(ProjectUploadPartial(
                            workerRoot, current->projectHash)) &&
                        std::any_of(current->projectTransfers.begin(),
                                    current->projectTransfers.end(),
                                    [&](const ProjectTransfer& transfer) {
                                        return transfer.state ==
                                                ProjectTransferState::Completed &&
                                            transfer.nodeId == workerConfig.nodeId &&
                                            transfer.transferredBytes == expectedBundleBytes;
                                    });
                    if (!networkTexReady) Sleep(25);
                }
                bool networkTexRendered = networkTexReady &&
                    origin.ConfigureAndStartTest(
                        networkTexJobId, 1, 1, 1, 1, L"1", submitError);
                for (int attempt = 0; attempt < 240 && networkTexRendered; ++attempt) {
                    auto current = origin.FindJob(networkTexJobId);
                    if (current && current->stage == JobStage::TestCompleted &&
                        current->frames.size() == 1 &&
                        current->frames.front().state == TaskState::Completed) {
                        const fs::path workerSource = ProjectCacheDirectory(
                            workerRoot, current->projectHash) / L"source.blend";
                        const fs::path workerTex = ProjectCacheDirectory(
                            workerRoot, current->projectHash) /
                            L"tex" / L"sequence" / L"0001.exr";
                        networkTexRendered =
                            Sha256File(workerSource) == Sha256File(networkTexBlend) &&
                            fileContains(workerTex, "network-tex-frame") &&
                            verifyResumableCommit(
                                current->projectHash, expectedBundleBytes, true,
                                current->softwareVersion, current->renderEngine,
                                current->requiresGpu, resumeSeedError);
                        break;
                    }
                    if (attempt == 239) networkTexRendered = false;
                    else Sleep(25);
                }
                if (!networkTexReady || !networkTexRendered) {
                    report = L"Network tex project synchronisation self-test failed: " +
                        submitError;
                    origin.Stop();
                    worker.Stop();
                    return false;
                }
                fs::path localOnlyRoot = root / L"local-only";
                ConfigStore localOnlyStore(localOnlyRoot);
                Config localOnlyConfig = localOnlyStore.Load();
                http::Server localOnlyPortProbe;
                int localOnlyPort = localOnlyPortProbe.bind_to_any_port("127.0.0.1");
                localOnlyPortProbe.stop();
                localOnlyConfig.nodeName = "SELFTEST-LOCAL";
                localOnlyConfig.nodeId = "selftest-local";
                localOnlyConfig.httpPort = localOnlyPort;
                localOnlyConfig.discoveryPort = 0;
                NodeApp localOnly(std::move(localOnlyStore), localOnlyConfig, true);
                std::wstring localOnlyError;
                bool localOnlyStarted = localOnlyPort > 0 && localOnly.Start(localOnlyError);
                Peer unavailableLocalPeer;
                unavailableLocalPeer.nodeId = "selftest-unavailable-local-peer";
                unavailableLocalPeer.name = "SELFTEST-UNAVAILABLE";
                unavailableLocalPeer.address = "127.0.0.1";
                unavailableLocalPeer.httpPort = 65534;
                unavailableLocalPeer.protocolVersion = kProtocolVersion;
                unavailableLocalPeer.supportsProjectCache = true;
                unavailableLocalPeer.rendererReady = false;
                unavailableLocalPeer.trusted = true;
                unavailableLocalPeer.shareEnabled = true;
                unavailableLocalPeer.lastSeen = std::chrono::steady_clock::now();
                localOnly.InjectPeerForTest(std::move(unavailableLocalPeer));
                PreflightReport localOnlyPreflight = preflight;
                localOnlyPreflight.frameStart = 1;
                localOnlyPreflight.frameEnd = 3;
                localOnlyPreflight.frameStep = 1;
                std::string localOnlyJob = localOnlyStarted
                    ? localOnly.CreatePreparedDraft(
                        "local-only-self-test", localOnlyPreflight, localOnlyError)
                    : std::string{};
                auto localOnlyDraft = localOnly.FindJob(localOnlyJob);
                bool localOnlyConfigured = localOnlyDraft &&
                    localOnlyDraft->projectTransfers.empty() &&
                    localOnlyDraft->stage == JobStage::ReadyToConfigure &&
                    localOnly.ConfigureAndStartTest(
                        localOnlyJob, 1, 3, 1, 3, L"1-3", localOnlyError);
                bool localOnlyCompleted = false;
                auto localOnlyDeadline = std::chrono::steady_clock::now() +
                    std::chrono::seconds(4);
                while (localOnlyConfigured &&
                       std::chrono::steady_clock::now() < localOnlyDeadline) {
                    auto current = localOnly.FindJob(localOnlyJob);
                    if (current && current->stage == JobStage::TestCompleted) {
                        localOnlyCompleted = current->frames.size() == 3;
                        for (const auto& frame : current->frames)
                            localOnlyCompleted = localOnlyCompleted &&
                                frame.state == TaskState::Completed &&
                                frame.assignedNodeId == localOnlyConfig.nodeId;
                        if (localOnlyCompleted) break;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(20));
                }
                localOnly.Stop();
                if (!localOnlyCompleted) {
                    report = L"Local-only prepared submission self-test failed: " +
                        localOnlyError;
                    return false;
                }
                fs::path lockedSubmitRoot = root / L"locked-submit";
                ConfigStore lockedSubmitStore(lockedSubmitRoot);
                Config lockedSubmitConfig = lockedSubmitStore.Load();
                NodeApp lockedSubmitApp(
                    std::move(lockedSubmitStore), lockedSubmitConfig, true);
                HANDLE preparedLock = CreateFileW(
                    prepared.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING,
                    FILE_ATTRIBUTE_NORMAL, nullptr);
                if (preparedLock == INVALID_HANDLE_VALUE) {
                    report = L"Unable to create locked-source submission self-test";
                    return false;
                }
                std::thread releasePreparedLock([preparedLock] {
                    Sleep(250);
                    CloseHandle(preparedLock);
                });
                std::string lockedSubmitJob = lockedSubmitApp.SubmitPrepared(
                    "locked-source-self-test", preflight, 1, 1, 1, submitError);
                releasePreparedLock.join();
                if (lockedSubmitJob.empty()) {
                    report = L"Transient locked-source submission self-test failed: " +
                        submitError;
                    return false;
                }
                fs::path stoppedRoot = root / L"stopped-job";
                ConfigStore stoppedStore(stoppedRoot);
                Config stoppedConfig = stoppedStore.Load();
                NodeApp stoppedApp(std::move(stoppedStore), stoppedConfig, true);
                std::string stoppedJobId = stoppedApp.SubmitPrepared(
                    "stopped-self-test", preflight, 0, 2, 1, submitError);
                bool stoppedOk = !stoppedJobId.empty() &&
                    stoppedApp.StopJob(stoppedJobId, submitError);
                for (const auto& stoppedJob : stoppedApp.Jobs())
                    if (stoppedJob.id == stoppedJobId)
                        for (const auto& frame : stoppedJob.frames)
                            stoppedOk = stoppedOk && frame.state == TaskState::Stopped &&
                                frame.endedAt.time_since_epoch() !=
                                    std::chrono::system_clock::duration::zero();
                NodeApp reloadedStopped(ConfigStore(stoppedRoot), stoppedConfig, true);
                bool stoppedReloaded = false;
                for (const auto& stoppedJob : reloadedStopped.Jobs())
                    if (stoppedJob.id == stoppedJobId) {
                        stoppedReloaded = !stoppedJob.frames.empty();
                        for (const auto& frame : stoppedJob.frames)
                             stoppedReloaded = stoppedReloaded &&
                                 frame.state == TaskState::Stopped;
                     }
                fs::path participantRoot = root / L"participant-edit";
                ConfigStore participantStore(participantRoot);
                Config participantConfig = participantStore.Load();
                participantConfig.nodeId = "selftest-participant-origin";
                participantConfig.nodeName = "SELFTEST-PARTICIPANT-ORIGIN";
                NodeApp participantApp(
                    std::move(participantStore), participantConfig, true);
                auto makeParticipantPeer = [](std::string id, std::string name, int port) {
                    Peer peer;
                    peer.nodeId = std::move(id);
                    peer.name = std::move(name);
                    peer.address = "127.0.0.1";
                    peer.httpPort = port;
                    peer.protocolVersion = kProtocolVersion;
                    peer.shareEnabled = true;
                    peer.trusted = true;
                    peer.supportsProjectCache = true;
                    peer.supportsResumableProjectUpload = true;
                    peer.rendererReady = true;
                    peer.lastSeen = std::chrono::steady_clock::now();
                    return peer;
                };
                const std::string participantA = "selftest-participant-a";
                const std::string participantB = "selftest-participant-b";
                participantApp.InjectPeerForTest(
                    makeParticipantPeer(participantA, "PARTICIPANT-A", 65431));
                std::string participantJob = participantApp.CreatePreparedDraft(
                    "participant-edit-self-test", preflight, submitError);
                participantApp.InjectPeerForTest(
                    makeParticipantPeer(participantB, "PARTICIPANT-B", 65432));
                bool participantEditOk = !participantJob.empty() &&
                    participantApp.SetJobParticipants(
                        participantJob, {participantA, participantB}, submitError);
                auto participantDraft = participantApp.FindJob(participantJob);
                participantEditOk = participantEditOk && participantDraft &&
                    participantDraft->stage == JobStage::Uploading &&
                    participantDraft->projectTransfers.size() == 2;
                if (participantDraft) {
                    for (const auto& transfer : participantDraft->projectTransfers)
                        participantEditOk = participantEditOk &&
                            transfer.state == ProjectTransferState::Pending;
                }
                participantEditOk = participantEditOk &&
                    participantApp.SetJobParticipants(
                        participantJob, {participantB}, submitError);
                participantDraft = participantApp.FindJob(participantJob);
                bool participantAExcluded = false;
                bool participantBSelected = false;
                if (participantDraft) {
                    for (const auto& transfer : participantDraft->projectTransfers) {
                        if (transfer.nodeId == participantA)
                            participantAExcluded =
                                transfer.state == ProjectTransferState::Excluded;
                        if (transfer.nodeId == participantB)
                            participantBSelected =
                                transfer.state == ProjectTransferState::Pending;
                    }
                }
                participantEditOk = participantEditOk && participantAExcluded &&
                    participantBSelected && participantApp.SetJobParticipants(
                        participantJob, {participantA, participantB}, submitError);
                participantDraft = participantApp.FindJob(participantJob);
                bool participantAReselected = false;
                if (participantDraft) {
                    for (const auto& transfer : participantDraft->projectTransfers)
                        if (transfer.nodeId == participantA)
                            participantAReselected =
                                transfer.state == ProjectTransferState::Pending;
                }
                if (!participantEditOk || !participantAReselected) {
                    report = L"Dynamic job participant editing self-test failed: " +
                        submitError;
                    return false;
                }
                fs::path stagedPrepared = root / L"prepared-staged.blend";
                {
                    std::ofstream blend(stagedPrepared, std::ios::binary);
                    blend << "RenderMesh staged resumable payload";
                }
                std::string expectedPlainHash;
                uint64_t plainResumeOffset = 0;
                const bool plainResumeSeeded = seedResumablePrefix(
                    stagedPrepared, false, preflight.softwareVersion,
                    preflight.renderEngine, preflight.requiresGpu, expectedPlainHash,
                    plainResumeOffset, resumeSeedError);
                if (!plainResumeSeeded) {
                    report = L"Plain resumable upload prefix self-test failed: " +
                        resumeSeedError;
                    origin.Stop();
                    worker.Stop();
                    return false;
                }
                PreflightReport stagedPreflight = preflight;
                stagedPreflight.sourceProject = stagedPrepared;
                stagedPreflight.preparedProject = stagedPrepared;
                stagedPreflight.frameStart = 1;
                stagedPreflight.frameEnd = 7;
                stagedPreflight.frameStep = 1;
                fs::path stagedOutputRoot = root / L"staged-final";
                stagedPreflight.primaryOutputs.clear();
                for (int frame = 1; frame <= 7; ++frame)
                    stagedPreflight.primaryOutputs[frame] = stagedOutputRoot /
                        (L"primary-" + std::to_wstring(frame) + L".png");
                for (auto& output : stagedPreflight.compositorOutputs)
                    output.directory = stagedOutputRoot / L"compositor";
                std::string stagedJob = origin.CreatePreparedDraft(
                    "staged-workflow-self-test", stagedPreflight, submitError);
                const uint64_t expectedPlainBytes = fs::file_size(stagedPrepared);
                bool stagedUploadReady = false;
                auto stagedUploadDeadline = std::chrono::steady_clock::now() +
                    std::chrono::seconds(5);
                while (!stagedJob.empty() &&
                       std::chrono::steady_clock::now() < stagedUploadDeadline) {
                    auto current = origin.FindJob(stagedJob);
                    if (current && current->stage == JobStage::ReadyToConfigure &&
                        current->projectTransfers.size() == 1 &&
                        current->projectHash == expectedPlainHash &&
                        !fs::exists(ProjectUploadPartial(
                            workerRoot, current->projectHash)) &&
                        current->projectTransfers.front().state ==
                            ProjectTransferState::Completed &&
                        current->projectTransfers.front().transferredBytes ==
                            expectedPlainBytes &&
                        CacheEntryValid(workerRoot, current->projectHash) &&
                        Sha256File(ProjectCacheDirectory(
                            workerRoot, current->projectHash) / L"source.blend") ==
                            expectedPlainHash &&
                        verifyResumableCommit(
                            current->projectHash, expectedPlainBytes, false,
                            current->softwareVersion, current->renderEngine,
                            current->requiresGpu, resumeSeedError)) {
                        stagedUploadReady = true;
                        break;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(20));
                }
                Peer unlistedPeer;
                unlistedPeer.nodeId = "selftest-a-unlisted-worker";
                unlistedPeer.name = "SELFTEST-UNLISTED-WORKER";
                unlistedPeer.address = "127.0.0.1";
                unlistedPeer.httpPort = 65534;
                unlistedPeer.protocolVersion = kProtocolVersion;
                unlistedPeer.shareEnabled = true;
                unlistedPeer.trusted = true;
                unlistedPeer.supportsProjectCache = true;
                unlistedPeer.supportsFramePackages = true;
                unlistedPeer.supportsResultBundles = true;
                unlistedPeer.supportsPackageCancellation = true;
                unlistedPeer.supportsResumableProjectUpload = true;
                unlistedPeer.rendererReady = true;
                unlistedPeer.maxPackageFrames = 20;
                unlistedPeer.lastSeen = std::chrono::steady_clock::now();
                origin.InjectPeerForTest(std::move(unlistedPeer));
                bool stagedTestStarted = stagedUploadReady && origin.ConfigureAndStartTest(
                    stagedJob, 1, 7, 1, 3, L"1,4,7", submitError);
                bool stagedTestCompleted = false;
                auto stagedTestDeadline = std::chrono::steady_clock::now() +
                    std::chrono::seconds(6);
                while (stagedTestStarted &&
                       std::chrono::steady_clock::now() < stagedTestDeadline) {
                    auto current = origin.FindJob(stagedJob);
                    if (current && current->stage == JobStage::TestCompleted) {
                        stagedTestCompleted = current->packageSize == 3;
                        for (const auto& frame : current->frames) {
                            const bool isTest = frame.frame == 1 || frame.frame == 4 ||
                                                frame.frame == 7;
                            stagedTestCompleted = stagedTestCompleted &&
                                (isTest
                                    ? frame.state == TaskState::Completed &&
                                      frame.assignedNodeId == workerConfig.nodeId &&
                                      frame.dispatchAttempts == 1
                                    : frame.state == TaskState::Pending);
                        }
                        if (stagedTestCompleted) break;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(20));
                }
                bool stagedFullStarted = stagedTestCompleted &&
                    origin.StartFullRender(stagedJob, submitError);
                bool stagedFullCompleted = false;
                auto stagedFullDeadline = std::chrono::steady_clock::now() +
                    std::chrono::seconds(8);
                while (stagedFullStarted &&
                       std::chrono::steady_clock::now() < stagedFullDeadline) {
                    auto current = origin.FindJob(stagedJob);
                    if (current && current->stage == JobStage::Completed) {
                        stagedFullCompleted = current->frames.size() == 7;
                        for (const auto& frame : current->frames)
                            stagedFullCompleted = stagedFullCompleted &&
                                frame.state == TaskState::Completed;
                        if (stagedFullCompleted) break;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(20));
                }
                origin.RemovePeerForTest("selftest-a-unlisted-worker");
                auto workerReadyDeadline = std::chrono::steady_clock::now() +
                    std::chrono::seconds(2);
                while (worker.AssignedCount() != 0 &&
                       std::chrono::steady_clock::now() < workerReadyDeadline)
                    std::this_thread::sleep_for(std::chrono::milliseconds(20));
                if (connectedPeer) {
                    connectedPeer->assigned = 0;
                    connectedPeer->lastSeen = std::chrono::steady_clock::now();
                    origin.InjectPeerForTest(*connectedPeer);
                }
                std::string jobId = origin.SubmitPrepared(
                    "two-node-self-test", preflight, 1, 7, 1, submitError);
                bool clusterOk = malformedRejected && pairingStarted && paired &&
                    manuallyConnected && hardwareReported && activityProjected &&
                    stoppedOk && stoppedReloaded && stagedUploadReady &&
                    stagedTestCompleted && stagedFullCompleted &&
                    !jobId.empty();
                bool usedRemote = false;
                bool streamedFirstFrame = false;
                std::wstring streamingDiagnostics = L"job not observed";
                auto streamingDeadline = std::chrono::steady_clock::now() +
                    std::chrono::seconds(3);
                while (clusterOk && std::chrono::steady_clock::now() < streamingDeadline) {
                    auto current = origin.Jobs();
                    if (!current.empty() && current.back().id == jobId &&
                        current.back().frames.size() >= 5) {
                        bool firstComplete =
                            current.back().frames[0].state == TaskState::Completed;
                        bool laterIncomplete = false;
                        int renderingFrames = 0;
                        bool queuedFrameTimerFrozen = false;
                        for (size_t index = 1; index < 5; ++index)
                        {
                            const auto& frame = current.back().frames[index];
                            laterIncomplete = laterIncomplete ||
                                frame.state != TaskState::Completed;
                            if (frame.state == TaskState::Rendering) ++renderingFrames;
                            queuedFrameTimerFrozen = queuedFrameTimerFrozen ||
                                (frame.state == TaskState::Assigned &&
                                 frame.startedAt.time_since_epoch() ==
                                     std::chrono::system_clock::duration::zero());
                        }
                        WorkerActivity workerProgress = worker.CurrentActivity();
                        const bool outputExists = fs::exists(current.back().frames[0].output);
                        const bool logExists = fs::is_regular_file(current.back().frames[0].log);
                        const bool logReady = logExists && fileContains(
                            current.back().frames[0].log, "Blender mock renderer started");
                        const bool compositorExists = fs::exists(
                            finalOutputRoot / L"compositor" / L"ViewLayer" / L"pass_1.exr");
                        const bool observedLiveProgress = laterIncomplete &&
                            workerProgress.rendering &&
                            workerProgress.jobName == "two-node-self-test" &&
                            workerProgress.frames == std::vector<int>{1, 2, 3, 4, 5} &&
                            workerProgress.completedFrames > 0 &&
                            workerProgress.completedFrames < workerProgress.frames.size();
                        const bool observedCommittedSequence =
                            current.back().frames[4].state == TaskState::Completed &&
                            current.back().frames[0].endedAt < current.back().frames[4].endedAt &&
                            current.back().frames[0].assignedNodeId == workerConfig.nodeId &&
                            current.back().frames[4].assignedNodeId == workerConfig.nodeId;
                        streamingDiagnostics = L"first=" + std::to_wstring(firstComplete) +
                            L" later=" + std::to_wstring(laterIncomplete) +
                            L" rendering=" + std::to_wstring(workerProgress.rendering) +
                            L" name=" + Utf8ToWide(workerProgress.jobName) +
                            L" frames=" + std::to_wstring(workerProgress.frames.size()) +
                            L" completed=" + std::to_wstring(workerProgress.completedFrames) +
                            L" active_frames=" + std::to_wstring(renderingFrames) +
                            L" queued_frozen=" + std::to_wstring(queuedFrameTimerFrozen) +
                            L" output=" + std::to_wstring(outputExists) +
                            L" log=" + std::to_wstring(logReady) +
                            L" compositor=" + std::to_wstring(compositorExists) +
                            L" sequence=" + std::to_wstring(observedCommittedSequence);
                        streamedFirstFrame = firstComplete && outputExists && logReady &&
                            compositorExists && queuedFrameTimerFrozen &&
                            renderingFrames == 1 && observedLiveProgress;
                        if (streamedFirstFrame) break;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                }
                clusterOk = clusterOk && streamedFirstFrame;
                if (!streamedFirstFrame)
                    error = L"per-frame result streaming self-test failed; " +
                        streamingDiagnostics;
                auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(12);
                while (clusterOk && std::chrono::steady_clock::now() < deadline) {
                    auto jobs = origin.Jobs();
                    if (!jobs.empty() && jobs.back().id == jobId) {
                        bool allDone = !jobs.back().frames.empty();
                        for (const auto& frame : jobs.back().frames) {
                            allDone = allDone && frame.state == TaskState::Completed &&
                                fs::exists(frame.output) && fs::is_regular_file(frame.log) &&
                                fileContains(frame.log, "Blender mock renderer started");
                            usedRemote = usedRemote || frame.assignedNodeId == workerConfig.nodeId;
                        }
                        if (allDone) break;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                }
                auto jobs = origin.Jobs();
                clusterOk = clusterOk && !jobs.empty() && jobs.back().id == jobId && usedRemote;
                if (clusterOk) {
                    for (const auto& frame : jobs.back().frames)
                        clusterOk = clusterOk && frame.state == TaskState::Completed &&
                            frame.startedAt.time_since_epoch() !=
                                std::chrono::system_clock::duration::zero() &&
                            frame.endedAt >= frame.startedAt &&
                            fs::exists(frame.output) &&
                            fs::exists(finalOutputRoot / L"compositor" / L"ViewLayer" /
                                (L"pass_" + std::to_wstring(frame.frame) + L".exr"));
                    for (size_t index = 1; index < 5; ++index)
                        clusterOk = clusterOk &&
                            jobs.back().frames[index].startedAt -
                                jobs.back().frames[index - 1].startedAt >=
                                    std::chrono::milliseconds(100);
                    for (size_t index = 0; index < 5; ++index)
                        clusterOk = clusterOk &&
                            jobs.back().frames[index].assignedNodeId == workerConfig.nodeId;
                    clusterOk = clusterOk && SafeSha256(jobs.back().projectHash) &&
                        CacheEntryValid(workerRoot, jobs.back().projectHash);
                }
                if (clusterOk) {
                    bool historyRecorded = false;
                    auto historyDeadline = std::chrono::steady_clock::now() +
                        std::chrono::seconds(2);
                    while (std::chrono::steady_clock::now() < historyDeadline) {
                        for (const auto& record : worker.Contributions()) {
                            if (record.jobId == jobId && record.jobName == "two-node-self-test" &&
                                record.frames.size() == 5 && record.completedFrames.size() == 5 &&
                                record.state == "completed" &&
                                record.endedAt >= record.startedAt) {
                                historyRecorded = true;
                                break;
                            }
                        }
                        if (historyRecorded) break;
                        std::this_thread::sleep_for(std::chrono::milliseconds(25));
                    }
                    clusterOk = historyRecorded;
                    if (!historyRecorded) error = L"worker contribution history self-test failed";
                }
                if (clusterOk) {
                    auto zeroWorkerReadyDeadline = std::chrono::steady_clock::now() +
                        std::chrono::seconds(3);
                    while (worker.AssignedCount() != 0 &&
                           std::chrono::steady_clock::now() < zeroWorkerReadyDeadline)
                        std::this_thread::sleep_for(std::chrono::milliseconds(20));
                    if (connectedPeer && worker.AssignedCount() == 0) {
                        Peer zeroFramePeer = *connectedPeer;
                        zeroFramePeer.lastSeen = std::chrono::steady_clock::now();
                        zeroFramePeer.assigned = 0;
                        zeroFramePeer.currentTaskKnown = true;
                        zeroFramePeer.currentTaskId.clear();
                        zeroFramePeer.activity = {};
                        origin.InjectPeerForTest(std::move(zeroFramePeer));
                    }
                    std::string zeroFrameJob = worker.AssignedCount() == 0
                        ? origin.SubmitPrepared(
                            "frame-zero-local-fence-self-test", preflight, 0, 5, 1,
                            submitError)
                        : std::string{};
                    bool zeroStayedLocalAndOthersRemote = false;
                    auto zeroDeadline = std::chrono::steady_clock::now() +
                        std::chrono::seconds(12);
                    while (!zeroFrameJob.empty() &&
                           std::chrono::steady_clock::now() < zeroDeadline) {
                        auto current = origin.Jobs();
                        if (!current.empty() && current.back().id == zeroFrameJob &&
                            current.back().frames.size() == 6) {
                            bool allDone = true;
                            for (const auto& frame : current.back().frames)
                                allDone = allDone && frame.state == TaskState::Completed;
                            if (allDone) {
                                zeroStayedLocalAndOthersRemote =
                                    current.back().frames[0].assignedNodeId == originConfig.nodeId;
                                for (size_t index = 1; index < current.back().frames.size(); ++index)
                                    zeroStayedLocalAndOthersRemote =
                                        zeroStayedLocalAndOthersRemote &&
                                        current.back().frames[index].assignedNodeId ==
                                            workerConfig.nodeId;
                                break;
                            }
                        }
                        std::this_thread::sleep_for(std::chrono::milliseconds(25));
                    }
                    clusterOk = zeroStayedLocalAndOthersRemote;
                    if (!clusterOk)
                        error = L"frame zero local-only scheduling fence self-test failed";
                }
                if (clusterOk) {
                    // Frame completion reaches the Origin slightly before the
                    // Worker finishes contribution bookkeeping and releases its
                    // single execution slot. Wait for that public readiness
                    // signal so this test measures cancellation, not the tail of
                    // the preceding package.
                    auto workerReadyDeadline = std::chrono::steady_clock::now() +
                        std::chrono::seconds(3);
                    while (worker.AssignedCount() != 0 &&
                           std::chrono::steady_clock::now() < workerReadyDeadline)
                        std::this_thread::sleep_for(std::chrono::milliseconds(20));
                    bool workerReadyForCancel = worker.AssignedCount() == 0;
                    if (connectedPeer) {
                        Peer cancellationPeer = *connectedPeer;
                        cancellationPeer.lastSeen = std::chrono::steady_clock::now();
                        cancellationPeer.assigned = 0;
                        cancellationPeer.currentTaskKnown = true;
                        cancellationPeer.currentTaskId.clear();
                        cancellationPeer.activity = {};
                        origin.InjectPeerForTest(std::move(cancellationPeer));
                    }
                    std::string cancelledJob = workerReadyForCancel ? origin.SubmitPrepared(
                        "remote-cancel-self-test", preflight, 30, 34, 1, submitError)
                        : std::string{};
                    bool remotelyAssigned = false;
                    auto assignmentDeadline = std::chrono::steady_clock::now() +
                        std::chrono::seconds(8);
                    while (!cancelledJob.empty() &&
                           std::chrono::steady_clock::now() < assignmentDeadline) {
                        auto current = origin.Jobs();
                        if (!current.empty() && current.back().id == cancelledJob &&
                            IsActiveFrameLease(current.back().frames.front().state) &&
                            current.back().frames.front().assignedNodeId == workerConfig.nodeId) {
                            remotelyAssigned = true;
                            break;
                        }
                        std::this_thread::sleep_for(std::chrono::milliseconds(10));
                    }
                    bool workerEnteredLoading = false;
                    auto loadingDeadline = std::chrono::steady_clock::now() +
                        std::chrono::seconds(2);
                    while (remotelyAssigned &&
                           std::chrono::steady_clock::now() < loadingDeadline) {
                        const WorkerActivity activity = worker.CurrentActivity();
                        if (worker.AssignedCount() != 0 && activity.Active() &&
                            !activity.rendering) {
                            workerEnteredLoading = true;
                            break;
                        }
                        std::this_thread::sleep_for(std::chrono::milliseconds(5));
                    }
                    bool stopAccepted = workerEnteredLoading &&
                        origin.StopJob(cancelledJob, submitError);
                    bool workerStoppedPackage = false;
                    std::wstring workerCancellationState;
                    auto stopDeadline = std::chrono::steady_clock::now() +
                        std::chrono::seconds(3);
                    while (stopAccepted && std::chrono::steady_clock::now() < stopDeadline) {
                        for (const auto& record : worker.Contributions()) {
                            if (record.jobId == cancelledJob) {
                                workerCancellationState = Utf8ToWide(record.state) + L":" +
                                    std::to_wstring(record.completedFrames.size());
                                if (record.state == "stopped" &&
                                    record.completedFrames.empty()) {
                                    workerStoppedPackage = true;
                                    break;
                                }
                            }
                        }
                        if (workerStoppedPackage) break;
                        std::this_thread::sleep_for(std::chrono::milliseconds(20));
                    }
                    bool originStayedStopped = false;
                    for (const auto& current : origin.Jobs()) {
                        if (current.id != cancelledJob) continue;
                        originStayedStopped = !current.frames.empty();
                        for (const auto& frame : current.frames)
                            originStayedStopped = originStayedStopped &&
                                frame.state == TaskState::Stopped;
                    }
                    auto releaseDeadline = std::chrono::steady_clock::now() +
                        std::chrono::seconds(3);
                    while ((worker.AssignedCount() != 0 ||
                            worker.CurrentActivity().Active()) &&
                           std::chrono::steady_clock::now() < releaseDeadline)
                        std::this_thread::sleep_for(std::chrono::milliseconds(20));
                    const bool workerReleased = worker.AssignedCount() == 0 &&
                        !worker.CurrentActivity().Active();
                    clusterOk = stopAccepted && workerStoppedPackage && originStayedStopped &&
                        workerReleased;
                    if (!clusterOk) error = L"remote package cancellation self-test failed; assigned=" +
                        std::to_wstring(remotelyAssigned) + L"; accepted=" +
                        std::to_wstring(stopAccepted) + L"; entered_loading=" +
                        std::to_wstring(workerEnteredLoading) + L"; worker_stopped=" +
                        std::to_wstring(workerStoppedPackage) + L"; origin_stopped=" +
                        std::to_wstring(originStayedStopped) + L"; worker_state=" +
                        workerCancellationState + L"; worker_released=" +
                        std::to_wstring(workerReleased);
                }
                if (clusterOk) {
                    std::wstring lifecycleError;
                    const fs::path savedRoot = root / L"saved-job";
                    bool priorityUpdated = origin.SetJobPriority(jobId, true, lifecycleError);
                    bool priorityVisible = false;
                    for (const auto& current : origin.Jobs())
                        if (current.id == jobId) priorityVisible = current.priority == 100;
                    bool saved = priorityUpdated && priorityVisible &&
                        origin.SaveJobAs(jobId, savedRoot, lifecycleError);
                    bool manifestFound = false;
                    if (saved && fs::exists(savedRoot)) {
                        for (const auto& entry : fs::directory_iterator(savedRoot)) {
                            fs::path manifestPath = entry.path() / L"manifest.json";
                            if (!fs::exists(manifestPath)) continue;
                            json savedManifest = json::parse(
                                std::ifstream(manifestPath, std::ios::binary), nullptr, false);
                            bool hasPrimary = false;
                            bool hasCompositor = false;
                            bool everyFileExists = !savedManifest.is_discarded();
                            for (const auto& file : savedManifest.value(
                                     "files", std::vector<std::string>{})) {
                                hasPrimary = hasPrimary || file.starts_with("primary/");
                                hasCompositor = hasCompositor || file.starts_with("compositor/");
                                everyFileExists = everyFileExists &&
                                    fs::is_regular_file(entry.path() / Utf8ToWide(file));
                            }
                            manifestFound = everyFileExists && hasPrimary && hasCompositor;
                        }
                    }
                    const std::string cleanupHash = jobs.back().projectHash;
                    fs::path lockPath = origin.ProjectStorageDir() / L"jobs" /
                        Utf8ToWide(jobId) /
                        L"deletion-lock.bin";
                    HANDLE lockedFile = CreateFileW(
                        lockPath.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
                    std::wstring expectedBusyError;
                    bool firstDeleteSucceeded = lockedFile != INVALID_HANDLE_VALUE &&
                        origin.DeleteJob(jobId, expectedBusyError);
                    if (lockedFile != INVALID_HANDLE_VALUE) CloseHandle(lockedFile);
                    bool metadataPresent = false;
                    for (const auto& current : origin.Jobs())
                        metadataPresent = metadataPresent || current.id == jobId;
                    const bool busyDeleteSafe = firstDeleteSucceeded ? !metadataPresent
                                                                     : metadataPresent;
                    bool deleted = manifestFound && busyDeleteSafe &&
                        (firstDeleteSucceeded || origin.DeleteJob(jobId, lifecycleError));
                    bool metadataRemoved = true;
                    for (const auto& current : origin.Jobs())
                        metadataRemoved = metadataRemoved && current.id != jobId;
                    bool remoteCacheRemoved = false;
                    auto cleanupDeadline = std::chrono::steady_clock::now() +
                        std::chrono::seconds(2);
                    while (deleted && std::chrono::steady_clock::now() < cleanupDeadline) {
                        remoteCacheRemoved = !CacheEntryValid(workerRoot, cleanupHash);
                        if (remoteCacheRemoved) break;
                        std::this_thread::sleep_for(std::chrono::milliseconds(25));
                    }
                    clusterOk = deleted && metadataRemoved && remoteCacheRemoved;
                    if (!clusterOk)
                        error = L"job lifecycle cleanup self-test failed: " + lifecycleError;
                }
                if (clusterOk) {
                    worker.Stop();
                    NodeApp reloadedWorker(ConfigStore(workerRoot), workerConfig, true);
                    bool historyReloaded = false;
                    for (const auto& record : reloadedWorker.Contributions())
                        historyReloaded = historyReloaded ||
                            (record.jobId == jobId && record.completedFrames.size() == 5);
                    std::vector<std::string> contributionIds;
                    for (const auto& record : reloadedWorker.Contributions())
                        contributionIds.push_back(record.packageId);
                    std::wstring clearHistoryError;
                    bool selectedHistoryCleared = historyReloaded &&
                        reloadedWorker.DeleteContributionHistory(
                            contributionIds, clearHistoryError);
                    NodeApp clearedHistoryReloaded(
                        ConfigStore(workerRoot), workerConfig, true);
                    clusterOk = selectedHistoryCleared &&
                        clearedHistoryReloaded.Contributions().empty();
                    if (!clusterOk)
                        error = L"selected contribution history cleanup self-test failed: " +
                            clearHistoryError;
                    origin.RemovePeerForTest(workerConfig.nodeId);
                    http::Server incompatibleServer;
                    std::atomic<int> incompatibleUploads{0};
                    std::atomic<int> cacheStatusQueries{0};
                    std::atomic<bool> transientBusy{false};
                    incompatibleServer.Get("/v1/prove", [&](const http::Request& request,
                                                              http::Response& response) {
                        std::string challenge = request.has_param("challenge")
                            ? request.get_param_value("challenge") : "";
                        response.set_content(json{
                            {"node_id", "selftest-incompatible-worker"},
                            {"matching_id_binding", MatchingIdBinding(activeClusterConfig)},
                            {"proof", HmacSha256(activeClusterConfig.clusterToken,
                                "worker-proof|selftest-incompatible-worker|" + challenge)},
                        }.dump(), "application/json");
                    });
                    // A busy worker answers the cache query with a retryable
                    // error; an incompatible one rejects the upload permanently.
                    incompatibleServer.Get("/v3/cache/upload-status", [&] (
                        const http::Request&, http::Response& response) {
                        ++cacheStatusQueries;
                        if (transientBusy.load()) {
                            response.status = 503;
                            response.set_content("computer is active", "text/plain");
                            return;
                        }
                        ++incompatibleUploads;
                        response.status = 422;
                        response.set_content("Blender version mismatch", "text/plain");
                    });
                    int incompatiblePort = incompatibleServer.bind_to_any_port("127.0.0.1");
                    std::thread incompatibleThread;
                    if (incompatiblePort > 0)
                        incompatibleThread = std::thread(
                            [&] { incompatibleServer.listen_after_bind(); });
                    Peer incompatiblePeer;
                    incompatiblePeer.nodeId = "selftest-incompatible-worker";
                    incompatiblePeer.name = "SELFTEST-INCOMPATIBLE-WORKER";
                    incompatiblePeer.address = "127.0.0.1";
                    incompatiblePeer.httpPort = incompatiblePort;
                    incompatiblePeer.protocolVersion = kProtocolVersion;
                    incompatiblePeer.shareEnabled = true;
                    incompatiblePeer.trusted = true;
                    incompatiblePeer.supportsProjectCache = true;
                    incompatiblePeer.supportsFramePackages = true;
                    incompatiblePeer.supportsResultBundles = true;
                    incompatiblePeer.supportsPackageCancellation = true;
                    incompatiblePeer.supportsFrameStart = true;
                    incompatiblePeer.rendererReady = true;
                    origin.InjectPeerForTest(std::move(incompatiblePeer));
                    std::string incompatibleJob = origin.SubmitPrepared(
                        "incompatible-worker-self-test", preflight, 40, 46, 1, submitError);
                    bool fellBackWithoutRetryStorm = false;
                    auto incompatibleDeadline = std::chrono::steady_clock::now() +
                        std::chrono::seconds(4);
                    while (!incompatibleJob.empty() &&
                           std::chrono::steady_clock::now() < incompatibleDeadline) {
                        auto current = origin.Jobs();
                        if (!current.empty() && current.back().id == incompatibleJob) {
                            bool completeLocally = current.back().frames.size() == 7;
                            for (const auto& frame : current.back().frames)
                                completeLocally = completeLocally &&
                                    frame.state == TaskState::Completed &&
                                    frame.assignedNodeId == originConfig.nodeId;
                            if (completeLocally) {
                                fellBackWithoutRetryStorm = incompatibleUploads.load() == 1 &&
                                    current.back().frames[0].dispatchAttempts == 1 &&
                                    current.back().frames[5].dispatchAttempts == 0 &&
                                    current.back().frames[6].dispatchAttempts == 0;
                                break;
                            }
                        }
                        std::this_thread::sleep_for(std::chrono::milliseconds(25));
                    }
                    int queriesBeforeBusyJob = cacheStatusQueries.load();
                    transientBusy = true;
                    std::string busyJob = origin.SubmitPrepared(
                        "busy-worker-self-test", preflight, 50, 56, 1, submitError);
                    bool busyPeerFellBackLocally = false;
                    auto busyDeadline = std::chrono::steady_clock::now() +
                        std::chrono::seconds(4);
                    while (!busyJob.empty() &&
                           std::chrono::steady_clock::now() < busyDeadline) {
                        auto current = origin.Jobs();
                        if (!current.empty() && current.back().id == busyJob) {
                            bool completeLocally = current.back().frames.size() == 7;
                            for (const auto& frame : current.back().frames)
                                completeLocally = completeLocally &&
                                    frame.state == TaskState::Completed &&
                                    frame.assignedNodeId == originConfig.nodeId;
                            if (completeLocally) {
                                busyPeerFellBackLocally =
                                    cacheStatusQueries.load() - queriesBeforeBusyJob == 1 &&
                                    incompatibleUploads.load() == 1 &&
                                    current.back().frames[0].dispatchAttempts == 1 &&
                                    current.back().frames[5].dispatchAttempts == 0 &&
                                    current.back().frames[6].dispatchAttempts == 0;
                                break;
                            }
                        }
                        std::this_thread::sleep_for(std::chrono::milliseconds(25));
                    }
                    fs::path alternateRoot = root / L"alternate-worker";
                    ConfigStore alternateStore(alternateRoot);
                    Config alternateConfig = alternateStore.Load();
                    http::Server alternatePortProbe;
                    int alternatePort = alternatePortProbe.bind_to_any_port("127.0.0.1");
                    alternatePortProbe.stop();
                    alternateConfig.nodeName = "SELFTEST-COMPATIBLE-WORKER";
                    alternateConfig.nodeId = "selftest-z-compatible-worker";
                    alternateConfig.inviteCode = activeClusterConfig.inviteCode;
                    alternateConfig.clusterToken = activeClusterConfig.clusterToken;
                    alternateConfig.httpPort = alternatePort;
                    alternateConfig.discoveryPort = 0;
                    alternateConfig.shareEnabled = true;
                    NodeApp alternateWorker(
                        std::move(alternateStore), alternateConfig, true);
                    std::wstring alternateStartError;
                    bool alternateStarted = alternatePort > 0 &&
                        alternateWorker.Start(alternateStartError);
                    bool triedSecondWorkerBeforeLocal = false;
                    if (alternateStarted) {
                        Peer alternatePeer;
                        alternatePeer.nodeId = alternateConfig.nodeId;
                        alternatePeer.name = alternateConfig.nodeName;
                        alternatePeer.address = "127.0.0.1";
                        alternatePeer.httpPort = alternatePort;
                        alternatePeer.protocolVersion = kProtocolVersion;
                        alternatePeer.shareEnabled = true;
                        alternatePeer.trusted = true;
                        alternatePeer.supportsProjectCache = true;
                        alternatePeer.supportsFramePackages = true;
                        alternatePeer.supportsResultBundles = true;
                        alternatePeer.supportsPackageCancellation = true;
                        alternatePeer.supportsFrameStart = true;
                        alternatePeer.rendererReady = true;
                        origin.InjectPeerForTest(std::move(alternatePeer));
                        transientBusy = false;
                        int uploadsBeforeMultiPeer = incompatibleUploads.load();
                        std::string multiPeerJob = origin.SubmitPrepared(
                            "second-worker-self-test", preflight, 60, 66, 1, submitError);
                        auto multiPeerDeadline = std::chrono::steady_clock::now() +
                            std::chrono::seconds(4);
                        while (!multiPeerJob.empty() &&
                               std::chrono::steady_clock::now() < multiPeerDeadline) {
                            auto current = origin.Jobs();
                            if (!current.empty() && current.back().id == multiPeerJob) {
                                bool allComplete = current.back().frames.size() == 7;
                                for (const auto& frame : current.back().frames)
                                    allComplete = allComplete &&
                                        frame.state == TaskState::Completed;
                                if (allComplete) {
                                    triedSecondWorkerBeforeLocal =
                                        incompatibleUploads.load() - uploadsBeforeMultiPeer == 1;
                                    for (size_t index = 0; index < 5; ++index)
                                        triedSecondWorkerBeforeLocal =
                                            triedSecondWorkerBeforeLocal &&
                                            current.back().frames[index].assignedNodeId ==
                                                alternateConfig.nodeId;
                                    break;
                                }
                            }
                            std::this_thread::sleep_for(std::chrono::milliseconds(25));
                        }
                        origin.RemovePeerForTest(alternateConfig.nodeId);
                    }
                    alternateWorker.Stop();
                    incompatibleServer.stop();
                    if (incompatibleThread.joinable()) incompatibleThread.join();
                    origin.RemovePeerForTest("selftest-incompatible-worker");
                    clusterOk = clusterOk && fellBackWithoutRetryStorm &&
                        busyPeerFellBackLocally && triedSecondWorkerBeforeLocal;
                    if (!fellBackWithoutRetryStorm || !busyPeerFellBackLocally ||
                        !triedSecondWorkerBeforeLocal)
                        error = L"worker rejection fallback self-test failed; uploads=" +
                            std::to_wstring(incompatibleUploads.load()) + L"; busy_queries=" +
                            std::to_wstring(cacheStatusQueries.load() - queriesBeforeBusyJob) +
                            L"; second_worker=" +
                            std::to_wstring(triedSecondWorkerBeforeLocal);
                }
                if (clusterOk) {
                    http::Server lostServer;
                    lostServer.Get("/v1/prove", [&](const http::Request& request,
                                                     http::Response& response) {
                        std::string challenge = request.has_param("challenge")
                            ? request.get_param_value("challenge") : "";
                        response.set_content(json{
                            {"node_id", "selftest-lost-worker"},
                            {"matching_id_binding", MatchingIdBinding(activeClusterConfig)},
                            {"proof", HmacSha256(activeClusterConfig.clusterToken,
                                "worker-proof|selftest-lost-worker|" + challenge)},
                        }.dump(), "application/json");
                    });
                    lostServer.Get("/v3/cache/upload-status", [](const http::Request&,
                                                                  http::Response& response) {
                        response.set_content(R"({"cached":true,"offset":1})",
                                             "application/json");
                    });
                    lostServer.Post("/v3/package", [](const http::Request&,
                                                       http::Response& response) {
                        response.status = 202;
                        response.set_content("accepted then lost", "text/plain");
                    });
                    int lostPort = lostServer.bind_to_any_port("127.0.0.1");
                    std::thread lostThread;
                    if (lostPort > 0)
                        lostThread = std::thread([&] { lostServer.listen_after_bind(); });
                    Peer lostPeer;
                    lostPeer.nodeId = "selftest-lost-worker";
                    lostPeer.name = "SELFTEST-LOST-WORKER";
                    lostPeer.address = "127.0.0.1";
                    lostPeer.httpPort = lostPort;
                    lostPeer.protocolVersion = kProtocolVersion;
                    lostPeer.rendererReady = true;
                    lostPeer.shareEnabled = true;
                    lostPeer.trusted = true;
                    lostPeer.supportsProjectCache = true;
                    lostPeer.supportsFramePackages = true;
                    lostPeer.supportsResultBundles = true;
                    lostPeer.supportsPackageCancellation = true;
                    lostPeer.supportsResumableProjectUpload = true;
                    lostPeer.supportsFrameStart = true;
                    origin.InjectPeerForTest(std::move(lostPeer));
                    PreflightReport legacyRecovery = preflight;
                    legacyRecovery.compositorOutputs.clear();
                    std::string recoveryJob = origin.SubmitPrepared(
                        "lost-worker-self-test", legacyRecovery, 4, 4, 1, submitError);
                    bool reachedAssigned = false;
                    std::string expiredAttempt;
                    auto assignDeadline = std::chrono::steady_clock::now() +
                        std::chrono::seconds(3);
                    while (!recoveryJob.empty() &&
                           std::chrono::steady_clock::now() < assignDeadline) {
                        auto current = origin.Jobs();
                        if (!current.empty() && current.back().id == recoveryJob &&
                            IsActiveFrameLease(current.back().frames.front().state)) {
                            reachedAssigned = true;
                            expiredAttempt = current.back().frames.front().attemptToken;
                            break;
                        }
                        std::this_thread::sleep_for(std::chrono::milliseconds(50));
                    }
                    Peer switchedPeer;
                    switchedPeer.nodeId = "selftest-lost-worker";
                    switchedPeer.name = "SELFTEST-LOST-WORKER";
                    switchedPeer.address = "127.0.0.1";
                    switchedPeer.httpPort = lostPort;
                    switchedPeer.protocolVersion = kProtocolVersion;
                    switchedPeer.rendererReady = true;
                    switchedPeer.shareEnabled = true;
                    switchedPeer.trusted = true;
                    switchedPeer.assigned = 1;
                    switchedPeer.currentTaskId = "unrelated-task";
                    origin.InjectPeerForTest(std::move(switchedPeer));
                    lostServer.stop();
                    if (lostThread.joinable()) lostThread.join();
                    bool recoveredLocally = false;
                    // The scheduler can already be inside one bounded remote
                    // probe when this synthetic worker disappears. Allow that
                    // 3-second connection timeout plus the 2.5-second lease
                    // grace before judging recovery; four seconds was shorter
                    // than the production bounds and made this test flaky.
                    auto recoveryDeadline = std::chrono::steady_clock::now() +
                        std::chrono::seconds(8);
                    while (reachedAssigned &&
                           std::chrono::steady_clock::now() < recoveryDeadline) {
                        auto current = origin.Jobs();
                        if (!current.empty() && current.back().id == recoveryJob) {
                            const auto& frame = current.back().frames.front();
                            if (frame.state == TaskState::Completed && fs::exists(frame.output) &&
                                frame.assignedNodeId == originConfig.nodeId &&
                                frame.dispatchAttempts >= 1) {
                                recoveredLocally = true;
                                break;
                            }
                        }
                        std::this_thread::sleep_for(std::chrono::milliseconds(50));
                    }
                    http::Client staleClient("127.0.0.1", originConfig.httpPort);
                    http::Headers staleHeaders = {
                        {"X-RM-Token", activeClusterConfig.clusterToken},
                        {"X-RM-Task", recoveryJob + "-4"},
                        {"X-RM-Attempt", expiredAttempt},
                        {"X-RM-Status", "failed"},
                        {"X-RM-Error", "late result from expired worker"},
                    };
                    auto staleResult = staleClient.Post(
                        "/v1/result", staleHeaders, "", "application/octet-stream");
                    bool staleRejected = staleResult && staleResult->status == 409;
                    clusterOk = reachedAssigned && recoveredLocally && staleRejected;
                    if (!clusterOk) {
                        error = L"lost-worker recovery self-test failed; assigned=" +
                            std::to_wstring(reachedAssigned) + L"; recovered=" +
                            std::to_wstring(recoveredLocally) + L"; stale_status=" +
                            (staleResult ? std::to_wstring(staleResult->status) : L"no-response") +
                            L"; attempt=" + std::to_wstring(!expiredAttempt.empty());
                    }
                }
                if (!clusterOk && error.empty()) error = L"two-node HTTP dispatch/result test failed";
                clusterSuitePassed = clusterOk;
            } else {
                clusterSuitePassed = false;
                error = startError;
            }
            origin.Stop();
            worker.Stop();
        }
        std::error_code ignored;
        fs::remove_all(root, ignored);
        if (!clusterSuitePassed) {
            report = L"双节点调度自检失败：" + error;
            return false;
        }
        report = L"SELF_TEST_OK：配置、手动 Blender 路径、工程存储迁移、可取消流式传输、仅空闲接单、双节点五帧分发、停止栅栏和掉线重派均通过";
        return true;
    } catch (const std::exception& exception) {
        report = L"自检异常：" + Utf8ToWide(exception.what());
        return false;
    }
}

#endif  // RM_DEV_TOOLS

}  // namespace rm
