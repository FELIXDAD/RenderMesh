#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>

#include "Core.hpp"
#include "../third_party/httplib.h"

#include <algorithm>
#include <cwctype>
#include <deque>
#include <fstream>
#include <regex>
#include <set>

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

bool IsTransientFileContention(DWORD error) {
    return error == ERROR_SHARING_VIOLATION || error == ERROR_LOCK_VIOLATION ||
        error == ERROR_ACCESS_DENIED || error == ERROR_BUSY ||
        error == ERROR_USER_MAPPED_FILE;
}

bool CopyFileAfterTransientLocks(
    const fs::path& source, const fs::path& destination,
    std::wstring& error,
    std::chrono::milliseconds timeout = std::chrono::seconds(5)) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    unsigned retries = 0;
    for (;;) {
        if (CopyFileW(source.c_str(), destination.c_str(), FALSE)) {
            if (retries)
                LogInfo("submitted packed project after transient file-lock retries=" +
                        std::to_string(retries));
            return true;
        }
        DWORD code = GetLastError();
        if (!IsTransientFileContention(code) ||
            std::chrono::steady_clock::now() >= deadline) {
            error = L"无法复制已打包工程：" + LastErrorMessage(code);
            return false;
        }
        ++retries;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
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
    std::string blendName = job.sourceBlend.empty()
        ? job.name : WideToUtf8(job.sourceBlend.stem().wstring());
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

int HeaderInt(const httplib::Request& request, const char* name, int fallback = 0) {
    try {
        return std::stoi(request.get_header_value(name));
    } catch (...) {
        return fallback;
    }
}

bool HeaderOutputTargets(
    const httplib::Request& request, const char* name,
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
    const httplib::ContentReader& reader,
    const fs::path& destination,
    std::wstring& error,
    std::atomic<bool>* stopRequested = nullptr) {
    fs::create_directories(destination.parent_path());
    fs::path temporary = destination;
    temporary += L".receiving";
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    if (!output) {
        error = L"无法创建接收文件";
        return false;
    }
    uint64_t receivedBytes = 0;
    bool received = reader([&](const char* data, size_t size) {
        if (stopRequested && stopRequested->load()) return false;
        if (size > kMaxTransferBytes - receivedBytes) return false;
        receivedBytes += size;
        output.write(data, static_cast<std::streamsize>(size));
        return static_cast<bool>(output);
    });
    output.close();
    if (!received || !output) {
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

bool WriteDurableFileAtomic(
    const fs::path& destination, const std::string& contents, std::wstring& error) {
    std::error_code ignored;
    fs::create_directories(destination.parent_path(), ignored);
    fs::path temporary = destination;
    temporary += L".tmp";
    HANDLE file = CreateFileW(
        temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        error = L"无法创建状态临时文件：" + LastErrorMessage();
        return false;
    }
    bool ok = true;
    size_t offset = 0;
    while (offset < contents.size()) {
        DWORD wanted = static_cast<DWORD>(std::min<size_t>(
            contents.size() - offset, static_cast<size_t>(32 * 1024 * 1024)));
        DWORD written = 0;
        if (!WriteFile(file, contents.data() + offset, wanted, &written, nullptr) ||
            written != wanted) {
            error = L"状态文件写入不完整：" + LastErrorMessage();
            ok = false;
            break;
        }
        offset += written;
    }
    if (ok && !FlushFileBuffers(file)) {
        error = L"状态文件无法刷新到磁盘：" + LastErrorMessage();
        ok = false;
    }
    if (!CloseHandle(file) && ok) {
        error = L"状态文件无法关闭：" + LastErrorMessage();
        ok = false;
    }
    if (!ok) {
        DeleteFileW(temporary.c_str());
        return false;
    }
    if (!MoveFileExW(temporary.c_str(), destination.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        error = L"无法发布状态文件：" + LastErrorMessage();
        DeleteFileW(temporary.c_str());
        return false;
    }
    return true;
}

bool CancellationRequested(
    const std::atomic<bool>* stopRequested,
    const std::atomic<bool>* operationCancelled = nullptr) {
    return (stopRequested && stopRequested->load()) ||
           (operationCancelled && operationCancelled->load());
}

bool HasTransferCapacity(
    const httplib::Request& request, const fs::path& directory,
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
    std::ifstream input(source, std::ios::binary);
    std::ofstream output(destination, std::ios::binary | std::ios::trunc);
    std::vector<char> buffer(1024 * 1024);
    while (input && output) {
        if (stopRequested && stopRequested->load()) {
            error = L"结果发布已取消";
            break;
        }
        input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        auto read = input.gcount();
        if (read > 0) output.write(buffer.data(), read);
        if (read == 0) break;
    }
    bool ok = input.eof() && output;
    output.close();
    if (!ok) {
        if (error.empty()) error = L"结果文件复制不完整";
        std::error_code ignored;
        fs::remove(destination, ignored);
    }
    return ok;
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

void PreserveAuthenticatedV2Metadata(Peer& candidate, const Peer& existing) {
    if (candidate.protocolVersion != 1 || existing.protocolVersion < 2) return;
    candidate.protocolVersion = existing.protocolVersion;
    candidate.supportsProjectCache = existing.supportsProjectCache;
    candidate.supportsFramePackages = existing.supportsFramePackages;
    candidate.supportsResultBundles = existing.supportsResultBundles;
    candidate.supportsPackageCancellation = existing.supportsPackageCancellation;
    candidate.name = existing.name;
    candidate.cpuPercent = existing.cpuPercent;
    candidate.idleSeconds = existing.idleSeconds;
    candidate.cpuModel = existing.cpuModel;
    candidate.gpuModels = existing.gpuModels;
    candidate.activity = existing.activity;
}

fs::path ProjectCacheDirectory(const fs::path& dataDir, const std::string& hash) {
    return dataDir / L"cache" / L"projects" / Utf8ToWide(hash);
}

bool CacheEntryValid(const fs::path& dataDir, const std::string& hash) {
    if (!SafeSha256(hash)) return false;
    fs::path directory = ProjectCacheDirectory(dataDir, hash);
    fs::path source = directory / L"source.blend";
    fs::path metadata = directory / L"cache-entry.json";
    if (!fs::is_regular_file(source) || !fs::is_regular_file(metadata)) return false;
    try {
        std::ifstream input(metadata, std::ios::binary);
        json value = json::parse(input);
        return value.value("project_hash", "") == hash;
    } catch (...) {
        return false;
    }
}

bool CacheMetadataMatches(
    const fs::path& dataDir, const std::string& hash, const std::string& version,
    const std::string& engine, bool requiresGpu) {
    if (!CacheEntryValid(dataDir, hash)) return false;
    try {
        std::ifstream input(ProjectCacheDirectory(dataDir, hash) / L"cache-entry.json",
                            std::ios::binary);
        json value = json::parse(input);
        return value.value("blender_version", "") == version &&
            value.value("render_engine", "") == engine &&
            value.value("requires_gpu", false) == requiresGpu;
    } catch (...) {
        return false;
    }
}

bool WriteCacheMetadata(
    const fs::path& dataDir, const std::string& hash, const std::string& version,
    const std::string& engine, bool requiresGpu, std::wstring& error) {
    fs::path destination = ProjectCacheDirectory(dataDir, hash) / L"cache-entry.json";
    fs::path temporary = destination;
    temporary += L".tmp";
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        output << json{{"project_hash", hash}, {"blender_version", version},
                       {"render_engine", engine}, {"requires_gpu", requiresGpu}}.dump(2);
        output.flush();
        if (!output) {
            error = L"无法写入工程缓存元数据";
            return false;
        }
    }
    if (!MoveFileExW(temporary.c_str(), destination.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        error = LastErrorMessage();
        DeleteFileW(temporary.c_str());
        return false;
    }
    return true;
}

void TouchCacheEntry(const fs::path& dataDir, const std::string& hash) {
    fs::path marker = ProjectCacheDirectory(dataDir, hash) / L"last-used";
    std::ofstream output(marker, std::ios::binary | std::ios::trunc);
    output << "used";
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

httplib::Result PostFile(
    const std::string& host,
    int port,
    const std::string& route,
    const httplib::Headers& headers,
    const fs::path& file,
    std::atomic<bool>* stopRequested = nullptr,
    time_t responseTimeoutSeconds = 5,
    std::atomic<bool>* operationCancelled = nullptr) {
    std::error_code ec;
    size_t total = static_cast<size_t>(fs::file_size(file, ec));
    httplib::Client client(host, port);
    client.set_connection_timeout(3, 0);
    // Bounded socket waits keep application shutdown responsive even if a peer
    // accepts a connection and then stalls indefinitely.
    client.set_read_timeout(responseTimeoutSeconds, 0);
    client.set_write_timeout(5, 0);
    auto provider = [file, stopRequested, operationCancelled](
                        size_t offset, size_t length, httplib::DataSink& sink) {
        if (CancellationRequested(stopRequested, operationCancelled)) return false;
        std::ifstream input(file, std::ios::binary);
        if (!input) return false;
        input.seekg(static_cast<std::streamoff>(offset));
        std::vector<char> buffer(std::min<size_t>(length, 1024 * 1024));
        size_t remaining = length;
        while (remaining > 0 && input) {
            if (CancellationRequested(stopRequested, operationCancelled)) return false;
            size_t wanted = std::min(buffer.size(), remaining);
            input.read(buffer.data(), static_cast<std::streamsize>(wanted));
            size_t read = static_cast<size_t>(input.gcount());
            if (!read) break;
            if (!sink.write(buffer.data(), read)) return false;
            remaining -= read;
        }
        return remaining == 0;
    };
    std::mutex cancellationMutex;
    std::condition_variable cancellationChanged;
    bool finished = false;
    std::thread cancellationThread;
    if (stopRequested || operationCancelled) {
        cancellationThread = std::thread([&] {
            std::unique_lock lock(cancellationMutex);
            while (!finished && !CancellationRequested(stopRequested, operationCancelled))
                cancellationChanged.wait_for(lock, std::chrono::milliseconds(100));
            if (!finished) client.stop();
        });
    }
    auto result = client.Post(route, headers, total, provider, "application/octet-stream");
    {
        std::scoped_lock lock(cancellationMutex);
        finished = true;
    }
    cancellationChanged.notify_all();
    if (cancellationThread.joinable()) cancellationThread.join();
    return result;
}

httplib::Result PostEmpty(
    const std::string& host,
    int port,
    const std::string& route,
    const httplib::Headers& headers,
    std::atomic<bool>* stopRequested = nullptr,
    std::atomic<bool>* operationCancelled = nullptr) {
    httplib::Client client(host, port);
    client.set_connection_timeout(3, 0);
    client.set_read_timeout(5, 0);
    client.set_write_timeout(5, 0);
    std::mutex cancellationMutex;
    std::condition_variable cancellationChanged;
    bool finished = false;
    std::thread cancellationThread;
    if (stopRequested || operationCancelled) {
        cancellationThread = std::thread([&] {
            std::unique_lock lock(cancellationMutex);
            while (!finished && !CancellationRequested(stopRequested, operationCancelled))
                cancellationChanged.wait_for(lock, std::chrono::milliseconds(100));
            if (!finished) client.stop();
        });
    }
    auto result = client.Post(route, headers, "", "application/octet-stream");
    {
        std::scoped_lock lock(cancellationMutex);
        finished = true;
    }
    cancellationChanged.notify_all();
    if (cancellationThread.joinable()) cancellationThread.join();
    return result;
}

std::string FrameExtension(const std::string& format) {
    return format == "PNG" ? ".png" : format == "JPEG" ? ".jpg" : ".exr";
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
    std::map<std::string, Peer> peers;
    std::set<std::pair<std::string, std::string>> rejectedJobPeers;
    std::map<std::pair<std::string, std::string>, std::chrono::steady_clock::time_point>
        jobPeerBackoff;
    std::vector<RenderJob> jobs;
    std::vector<ContributionRecord> contributions;
    std::mutex callbackMutex;
    std::function<void()> changed;
    mutable std::mutex manualStatusMutex;
    std::wstring manualStatus;
    std::mutex workMutex;
    std::map<std::string, std::chrono::steady_clock::time_point> cancelledPackages;
    std::mutex cancellationMutex;
    std::mutex cacheMutex;
    std::mutex pairingMutex;
    std::map<std::string, std::deque<std::chrono::steady_clock::time_point>> pairingAttempts;
    std::condition_variable workCv;
    std::queue<WorkItem> work;
    std::mutex currentTaskMutex;
    std::string currentTaskId;
    WorkerActivity currentActivity;
    std::atomic<int> workerAssigned{0};
    std::atomic<bool> stopRequested{false};
    std::map<std::string, std::shared_ptr<std::atomic<bool>>> jobCancellations;
    httplib::Server server;
    std::atomic<SOCKET> discoverySocket{INVALID_SOCKET};
    std::thread serverThread;
    std::thread discoveryThread;
    std::thread manualPeerThread;
    std::thread schedulerThread;
    std::thread workerThread;
    std::thread cacheThread;

    void Notify() {
        std::function<void()> callback;
        {
            std::scoped_lock lock(callbackMutex);
            callback = changed;
        }
        if (callback) callback();
    }

    fs::path JobsPath() const { return owner->DataDir() / L"jobs.json"; }
    fs::path ContributionsPath() const { return owner->DataDir() / L"contributions.json"; }

    std::shared_ptr<std::atomic<bool>> JobCancellation(const std::string& jobId) {
        std::scoped_lock lock(cancellationMutex);
        auto& signal = jobCancellations[jobId];
        if (!signal) signal = std::make_shared<std::atomic<bool>>(false);
        return signal;
    }

    bool PackageCancelled(const std::string& packageId) {
        std::scoped_lock lock(workMutex);
        auto now = std::chrono::steady_clock::now();
        for (auto it = cancelledPackages.begin(); it != cancelledPackages.end();) {
            if (now - it->second > std::chrono::hours(1)) it = cancelledPackages.erase(it);
            else ++it;
        }
        return cancelledPackages.contains(packageId);
    }

    void CancelPackage(const std::string& packageId) {
        std::scoped_lock lock(workMutex);
        if (cancelledPackages.size() >= 4096) cancelledPackages.erase(cancelledPackages.begin());
        cancelledPackages[packageId] = std::chrono::steady_clock::now();
    }

    void ForgetCancelledPackage(const std::string& packageId) {
        std::scoped_lock lock(workMutex);
        cancelledPackages.erase(packageId);
    }

    std::string CurrentTaskId() {
        std::scoped_lock lock(currentTaskMutex);
        return currentTaskId;
    }

    WorkerActivity CurrentActivity() {
        std::scoped_lock lock(currentTaskMutex);
        return currentActivity;
    }

    void SetManualStatus(std::wstring value) {
        std::scoped_lock lock(manualStatusMutex);
        manualStatus = std::move(value);
    }

    void CacheMaintenanceLoop() {
        while (!stopRequested.load()) {
            if (workerAssigned.load() == 0) {
                std::scoped_lock lock(cacheMutex);
                size_t removed = CleanupExpiredProjectCaches(owner->DataDir());
                if (removed)
                    LogInfo("expired project caches removed=" + std::to_string(removed));
            }
            for (int step = 0; step < 600 && !stopRequested.load(); ++step) Sleep(100);
        }
    }

    void PersistJobs() {
        std::scoped_lock persistLock(persistMutex);
        json rows = json::array();
        {
            std::scoped_lock lock(stateMutex);
            for (const auto& job : jobs) {
                json frames = json::array();
                json primaryOutputs = json::object();
                json compositorOutputs = json::array();
                for (const auto& [frame, path] : job.primaryOutputs)
                    primaryOutputs[std::to_string(frame)] = WideToUtf8(path.wstring());
                for (const auto& target : job.compositorOutputs) {
                    json metadata = OutputTargetMetadata(target);
                    metadata["directory"] = WideToUtf8(target.directory.wstring());
                    compositorOutputs.push_back(std::move(metadata));
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
                        {"output", WideToUtf8(frame.output.wstring())},
                        {"error", frame.error},
                        {"started_at", UnixMilliseconds(frame.startedAt)},
                        {"ended_at", UnixMilliseconds(frame.endedAt)},
                    });
                }
                rows.push_back({
                    {"id", job.id}, {"name", job.name},
                    {"source_blend", WideToUtf8(job.sourceBlend.wstring())},
                    {"blend", WideToUtf8(job.preparedBlend.wstring())},
                    {"project_hash", job.projectHash},
                    {"format", job.outputFormat}, {"extension", job.outputExtension},
                    {"version", job.blenderVersion},
                    {"render_engine", job.renderEngine},
                    {"requires_gpu", job.requiresGpu},
                    {"primary_outputs", primaryOutputs},
                    {"compositor_outputs", compositorOutputs},
                    {"frames", frames},
                    {"created_at", UnixMilliseconds(job.createdAt)},
                });
            }
        }
        std::wstring error;
        if (!WriteDurableFileAtomic(JobsPath(), rows.dump(2), error))
            LogError("jobs state was not replaced; " + WideToUtf8(error));
    }

    void LoadJobs() {
        if (!fs::exists(JobsPath())) return;
        try {
            std::ifstream input(JobsPath(), std::ios::binary);
            json rows = json::parse(input);
            std::scoped_lock lock(stateMutex);
            for (const auto& row : rows) {
                RenderJob job;
                job.id = row.value("id", "");
                job.name = row.value("name", "");
                job.sourceBlend = Utf8ToWide(row.value("source_blend", ""));
                job.preparedBlend = Utf8ToWide(row.value("blend", ""));
                job.projectHash = row.value("project_hash", "");
                if (!SafeSha256(job.projectHash) && fs::is_regular_file(job.preparedBlend))
                    job.projectHash = Sha256File(job.preparedBlend);
                job.outputFormat = row.value("format", "PNG");
                job.outputExtension = row.value("extension", FrameExtension(job.outputFormat));
                job.blenderVersion = row.value("version", "");
                job.renderEngine = row.value("render_engine", "");
                job.requiresGpu = row.value("requires_gpu", false);
                json primaryOutputRows = row.value("primary_outputs", json::object());
                for (const auto& [frameText, pathValue] : primaryOutputRows.items()) {
                    try {
                        job.primaryOutputs[std::stoi(frameText)] =
                            Utf8ToWide(pathValue.get<std::string>());
                    } catch (...) {
                    }
                }
                for (const auto& value : row.value("compositor_outputs", json::array())) {
                    OutputTarget target;
                    if (!ParseOutputTargetMetadata(value, target)) continue;
                    target.directory = Utf8ToWide(value.value("directory", ""));
                    if (target.directory.is_absolute())
                        job.compositorOutputs.push_back(std::move(target));
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
                    frame.output = Utf8ToWide(value.value("output", ""));
                    frame.error = value.value("error", "");
                    frame.startedAt = TimeFromUnixMilliseconds(value.value("started_at", int64_t{0}));
                    frame.endedAt = TimeFromUnixMilliseconds(value.value("ended_at", int64_t{0}));
                    job.frames.push_back(std::move(frame));
                }
                job.createdAt = TimeFromUnixMilliseconds(row.value("created_at", int64_t{0}));
                if (!job.id.empty()) jobs.push_back(std::move(job));
            }
        } catch (...) {
        }
    }

    void PersistContributions() {
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
        if (!WriteDurableFileAtomic(ContributionsPath(), rows.dump(2), error))
            LogError("contribution history was not replaced; " + WideToUtf8(error));
    }

    void LoadContributions() {
        if (!fs::exists(ContributionsPath())) return;
        try {
            std::ifstream input(ContributionsPath(), std::ios::binary);
            json rows = json::parse(input);
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
        httplib::Client client(address, port);
        client.set_connection_timeout(2, 0);
        client.set_read_timeout(2, 0);
        client.set_write_timeout(2, 0);
        httplib::Headers headers = {
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
        server.set_payload_max_length(100ull * 1024 * 1024 * 1024);
        server.set_read_timeout(5, 0);
        server.set_write_timeout(5, 0);
        server.Get("/health", [&](const httplib::Request& request, httplib::Response& response) {
            Config config = owner->GetConfig();
            WorkerActivity activity = CurrentActivity();
            json snapshot{{"status", "ok"}, {"protocol", 7}, {"project_cache", true},
                     {"frame_packages", true}, {"result_bundles", true},
                     {"package_cancel", true},
                     {"node_id", config.nodeId},
                     {"name", config.nodeName}, {"share", config.shareEnabled},
                     {"assigned", workerAssigned.load()}, {"cpu", 100},
                     {"cpu_model", hardware.cpuModel}, {"gpu_models", hardware.gpuModels},
                     {"pairing_available", !config.pairingPending},
                     {"task_id", CurrentTaskId()},
                     {"activity_job", HeaderDisplayText(activity.jobName, 128)},
                     {"activity_frames", activity.frames},
                     {"activity_completed", activity.completedFrames},
                     {"activity_rendering", activity.rendering},
                     {"idle", static_cast<int>(IdleSeconds())}};
            std::string challenge = request.has_param("challenge")
                ? request.get_param_value("challenge") : "";
            if (SafeId(challenge) && challenge.size() >= 16 &&
                !config.pairingPending && config.clusterToken.size() >= 16) {
                snapshot["activity_proof"] = ActivitySnapshotProof(
                    config, challenge, config.nodeId, activity);
            }
            response.set_content(snapshot.dump(), "application/json");
        });
        server.Post("/v2/pair", [&](const httplib::Request& request,
                                      httplib::Response& response) {
            if (!AllowPairingAttempt(request.remote_addr)) {
                response.status = 429;
                response.set_content("too many pairing attempts", "text/plain");
                return;
            }
            Config config = owner->GetConfig();
            std::string clientNodeId = request.get_header_value("X-RM-Node");
            std::string nonce = request.get_header_value("X-RM-Nonce");
            std::string claimed = request.get_header_value("X-RM-Proof");
            if (config.pairingPending || config.clusterToken.size() < 16 ||
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
        server.Post("/v3/cancel", [&](const httplib::Request& request,
                                        httplib::Response& response) {
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
            CancelPackage(packageId);
            fs::path marker = owner->DataDir() / L"incoming" / Utf8ToWide(packageId) /
                              L"outputs" / L"stop-after-current";
            std::error_code ignored;
            fs::create_directories(marker.parent_path(), ignored);
            std::ofstream(marker, std::ios::binary | std::ios::trunc) << "stop";
            workCv.notify_all();
            response.status = 202;
            response.set_content("cancellation recorded", "text/plain");
            LogInfo("remote package cancellation recorded; package=" + packageId);
        });
        server.Get("/v2/cache/status", [&](const httplib::Request& request,
                                             httplib::Response& response) {
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
            if (!SafeSha256(hash) || version.empty() || engine.empty()) {
                response.status = 422;
                return;
            }
            std::scoped_lock lock(cacheMutex);
            if (!CacheMetadataMatches(owner->DataDir(), hash, version, engine, requiresGpu)) {
                response.status = 404;
                return;
            }
            response.set_content(json{{"cached", true}, {"project_hash", hash}}.dump(),
                                 "application/json");
        });
        server.Post(
            "/v2/cache",
            [&](const httplib::Request& request, httplib::Response& response,
                const httplib::ContentReader& reader) {
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
                std::string hash = request.get_header_value("X-RM-Project");
                std::string version = request.get_header_value("X-RM-Blender");
                std::string engine = request.get_header_value("X-RM-Engine");
                bool requiresGpu = request.get_header_value("X-RM-Requires-GPU") == "1";
                if (!SafeSha256(hash) || version.empty() || engine.empty()) {
                    response.status = 422;
                    return;
                }
                if (!owner->mockRenderer_ &&
                    !IsComputerIdleForSharedWork(IdleSeconds(), config.idleAfterSeconds)) {
                    response.status = 425;
                    response.set_content("project waits until this computer is idle",
                                         "text/plain");
                    return;
                }
                if (!HasTransferCapacity(request, owner->DataDir())) {
                    response.status = 507;
                    return;
                }
                std::unique_lock cacheLock(cacheMutex);
                if (CacheMetadataMatches(owner->DataDir(), hash, version, engine, requiresGpu)) {
                    TouchCacheEntry(owner->DataDir(), hash);
                    response.status = 200;
                    response.set_content("already cached", "text/plain");
                    return;
                }
                int expectedAssigned = 0;
                if (!workerAssigned.compare_exchange_strong(expectedAssigned, 1)) {
                    response.status = 409;
                    response.set_content("worker is busy", "text/plain");
                    return;
                }
                struct CacheReservation {
                    std::atomic<int>& count;
                    ~CacheReservation() { --count; }
                } reservation{workerAssigned};
                fs::path directory = ProjectCacheDirectory(owner->DataDir(), hash);
                fs::path blend = directory / L"source.blend";
                std::wstring receiveError;
                if (!StreamRequestToFile(reader, blend, receiveError, &stopRequested)) {
                    response.status = 500;
                    response.set_content(WideToUtf8(receiveError), "text/plain");
                    return;
                }
                std::string receivedHash = Sha256File(blend, &stopRequested);
                if (stopRequested.load()) {
                    std::error_code ignored;
                    fs::remove_all(directory, ignored);
                    response.status = 503;
                    response.set_content("node is stopping", "text/plain");
                    return;
                }
                if (receivedHash != hash) {
                    std::error_code ignored;
                    fs::remove_all(directory, ignored);
                    response.status = 422;
                    response.set_content("project hash mismatch", "text/plain");
                    return;
                }
                if (!owner->mockRenderer_ &&
                    !IsComputerIdleForSharedWork(IdleSeconds(), config.idleAfterSeconds)) {
                    std::error_code ignored;
                    fs::remove_all(directory, ignored);
                    response.status = 425;
                    response.set_content("computer became active during project upload",
                                         "text/plain");
                    return;
                }
                std::wstring compatibility;
                std::atomic<bool> compatibilityStop = false;
                std::atomic<bool> resumedInput = false;
                std::atomic<bool>* effectiveStop = &stopRequested;
                std::thread compatibilityGuard;
                if (!owner->mockRenderer_) {
                    effectiveStop = &compatibilityStop;
                    compatibilityGuard = std::thread([&] {
                        while (!compatibilityStop.load()) {
                            if (stopRequested.load()) {
                                compatibilityStop = true;
                                break;
                            }
                            if (!IsComputerIdleForSharedWork(IdleSeconds(), config.idleAfterSeconds)) {
                                resumedInput = true;
                                compatibilityStop = true;
                                break;
                            }
                            Sleep(50);
                        }
                    });
                }
                bool compatible = owner->mockRenderer_ || CheckWorkerCompatibility(
                    config, owner->DataDir(), blend, version, engine, requiresGpu,
                    *effectiveStop, compatibility);
                compatibilityStop = true;
                if (compatibilityGuard.joinable()) compatibilityGuard.join();
                if (!compatible) {
                    std::error_code ignored;
                    fs::remove_all(directory, ignored);
                    response.status = resumedInput.load() ? 425 : 409;
                    if (resumedInput.load())
                        compatibility = L"电脑恢复使用，已中止工程缓存兼容性检查";
                    response.set_content(WideToUtf8(compatibility),
                                         "text/plain; charset=utf-8");
                    return;
                }
                if (!WriteCacheMetadata(owner->DataDir(), hash, version, engine,
                                        requiresGpu, receiveError)) {
                    response.status = 500;
                    response.set_content(WideToUtf8(receiveError), "text/plain; charset=utf-8");
                    return;
                }
                TouchCacheEntry(owner->DataDir(), hash);
                response.status = 201;
                response.set_content("cached", "text/plain");
                LogInfo("project cached; hash=" + hash.substr(0, 16));
            });
        server.Post("/v2/task", [&](const httplib::Request& request,
                                      httplib::Response& response) {
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
            std::string taskId = request.get_header_value("X-RM-Task");
            std::string jobId = request.get_header_value("X-RM-Job");
            std::string jobName = HeaderDisplayText(request.get_header_value("X-RM-Job-Name"));
            std::string originName = HeaderDisplayText(request.get_header_value("X-RM-Origin-Name"));
            std::string hash = request.get_header_value("X-RM-Project");
            int frame = HeaderInt(request, "X-RM-Frame", kBlenderFrameMin - 1);
            int originPort = HeaderInt(request, "X-RM-Origin-Port", 0);
            std::string format = request.get_header_value("X-RM-Format");
            std::string extension = request.get_header_value("X-RM-Extension");
            std::string version = request.get_header_value("X-RM-Blender");
            std::string engine = request.get_header_value("X-RM-Engine");
            bool requiresGpu = request.get_header_value("X-RM-Requires-GPU") == "1";
            std::string attemptToken = request.get_header_value("X-RM-Attempt");
            if (!SafeId(taskId) || !SafeId(jobId) || !SafeSha256(hash) ||
                frame < kBlenderFrameMin || frame > kBlenderFrameMax ||
                originPort < 1024 || originPort > 65535 ||
                !SafeId(attemptToken) || format.empty() ||
                !IsSafeOutputExtension(extension)) {
                response.status = 422;
                return;
            }
            if (PackageCancelled(taskId)) {
                response.status = 409;
                response.set_content("task was cancelled before acceptance", "text/plain");
                return;
            }
            if (!owner->mockRenderer_ &&
                !IsComputerIdleForSharedWork(IdleSeconds(), config.idleAfterSeconds)) {
                response.status = 425;
                response.set_content("task waits until this computer is idle", "text/plain");
                return;
            }
            std::unique_lock cacheLock(cacheMutex);
            if (!CacheMetadataMatches(owner->DataDir(), hash, version, engine, requiresGpu)) {
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
            TouchCacheEntry(owner->DataDir(), hash);
            fs::path taskDir = owner->DataDir() / L"incoming" / Utf8ToWide(taskId);
            WorkItem item;
            item.taskId = taskId;
            item.jobId = jobId;
            item.jobName = jobName.empty() ? jobId : std::move(jobName);
            item.frame = frame;
            item.projectHash = hash;
            item.blend = ProjectCacheDirectory(owner->DataDir(), hash) / L"source.blend";
            item.temporaryTaskDir = taskDir;
            item.outputDir = taskDir / L"outputs";
            item.outputFormat = format;
            item.outputExtension = extension;
            item.blenderVersion = version;
            item.renderEngine = engine;
            item.requiresGpu = requiresGpu;
            item.attemptToken = attemptToken;
            item.remote = true;
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
            response.set_content("accepted cached project", "text/plain");
            LogInfo("accepted cached task " + taskId + "; project=" + hash.substr(0, 16));
        });
        server.Post("/v3/package", [&](const httplib::Request& request,
                                         httplib::Response& response) {
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
            std::vector<OutputTarget> compositorOutputs;
            if (!HeaderOutputTargets(request, "X-RM-Compositor", compositorOutputs)) {
                response.status = 422;
                return;
            }
            std::vector<WorkItem::Frame> packageFrames;
            try {
                json values = json::parse(request.get_header_value("X-RM-Frames"));
                if (!values.is_array() || values.empty() || values.size() > 5)
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
            if (!CacheMetadataMatches(owner->DataDir(), hash, version, engine, requiresGpu)) {
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
            TouchCacheEntry(owner->DataDir(), hash);
            fs::path taskDir = owner->DataDir() / L"incoming" / Utf8ToWide(packageId);
            WorkItem item;
            item.taskId = packageFrames.front().taskId;
            item.jobId = jobId;
            item.jobName = jobName.empty() ? jobId : std::move(jobName);
            item.frame = packageFrames.front().frame;
            item.attemptToken = packageFrames.front().attemptToken;
            item.packageId = packageId;
            item.frames = std::move(packageFrames);
            item.projectHash = hash;
            item.blend = ProjectCacheDirectory(owner->DataDir(), hash) / L"source.blend";
            item.temporaryTaskDir = taskDir;
            item.outputDir = taskDir / L"outputs";
            item.outputFormat = format;
            item.outputExtension = extension;
            item.blenderVersion = version;
            item.renderEngine = engine;
            item.requiresGpu = requiresGpu;
            item.compositorOutputs = std::move(compositorOutputs);
            item.remote = true;
            item.resultBundles = request.get_header_value("X-RM-Result-Bundles") == "1";
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
        server.Get("/v1/prove", [&](const httplib::Request& request,
                                     httplib::Response& response) {
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
                {"proof", HmacSha256(config.clusterToken,
                    "worker-proof|" + config.nodeId + "|" + challenge)},
            }.dump(), "application/json");
        });
        server.Post(
            "/v1/task",
            [&](const httplib::Request& request, httplib::Response& response,
                const httplib::ContentReader& reader) {
                Config config = owner->GetConfig();
                if (config.pairingPending || config.clusterToken.size() < 16 ||
                    !ConstantTimeEqual(request.get_header_value("X-RM-Token"),
                                       config.clusterToken)) {
                    response.status = 401;
                    response.set_content("invalid cluster token", "text/plain");
                    return;
                }
                if (!config.shareEnabled) {
                    response.status = 409;
                    response.set_content("compute sharing is disabled", "text/plain");
                    return;
                }
                std::string taskId = request.get_header_value("X-RM-Task");
                std::string jobId = request.get_header_value("X-RM-Job");
                std::string jobName = HeaderDisplayText(request.get_header_value("X-RM-Job-Name"));
                std::string originName = HeaderDisplayText(request.get_header_value("X-RM-Origin-Name"));
                int frame = HeaderInt(request, "X-RM-Frame", kBlenderFrameMin - 1);
                int originPort = HeaderInt(request, "X-RM-Origin-Port", 0);
                std::string format = request.get_header_value("X-RM-Format");
                std::string extension = request.get_header_value("X-RM-Extension");
                if (!request.has_header("X-RM-Extension")) extension = FrameExtension(format);
                std::string version = request.get_header_value("X-RM-Blender");
                std::string renderEngine = request.get_header_value("X-RM-Engine");
                bool requiresGpu = request.get_header_value("X-RM-Requires-GPU") == "1";
                std::string attemptToken = request.get_header_value("X-RM-Attempt");
                if (!SafeId(taskId) || !SafeId(jobId) ||
                    frame < kBlenderFrameMin || frame > kBlenderFrameMax ||
                    originPort < 1024 ||
                    originPort > 65535 ||
                    !SafeId(attemptToken) || format.empty() ||
                    !IsSafeOutputExtension(extension)) {
                    response.status = 422;
                    response.set_content("invalid task metadata", "text/plain");
                    return;
                }
                if (!owner->mockRenderer_ &&
                    !IsComputerIdleForSharedWork(IdleSeconds(), config.idleAfterSeconds)) {
                    response.status = 425;
                    response.set_content("task waits until this computer is idle", "text/plain");
                    return;
                }
                if (!HasTransferCapacity(request, owner->DataDir())) {
                    response.status = 507;
                    response.set_content("project exceeds transfer limit or available disk space", "text/plain");
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
                fs::path taskDir = owner->DataDir() / L"incoming" / Utf8ToWide(taskId);
                fs::path blend = taskDir / L"source.blend";
                std::wstring receiveError;
                if (!StreamRequestToFile(reader, blend, receiveError, &stopRequested)) {
                    response.status = 500;
                    response.set_content(WideToUtf8(receiveError), "text/plain");
                    return;
                }
                if (PackageCancelled(taskId)) {
                    response.status = 409;
                    response.set_content("task was cancelled during upload", "text/plain");
                    std::error_code ignored;
                    fs::remove_all(taskDir, ignored);
                    return;
                }
                if (!owner->mockRenderer_ &&
                    !IsComputerIdleForSharedWork(IdleSeconds(), config.idleAfterSeconds)) {
                    response.status = 425;
                    response.set_content("computer became active during upload", "text/plain");
                    std::error_code ignored;
                    fs::remove_all(taskDir, ignored);
                    return;
                }
                std::wstring compatibility;
                bool compatible = true;
                std::atomic<bool> compatibilityStop = false;
                std::atomic<bool> resumedInput = false;
                std::atomic<bool>* effectiveStop = &stopRequested;
                std::thread compatibilityGuard;
                if (!owner->mockRenderer_) {
                    effectiveStop = &compatibilityStop;
                    compatibilityGuard = std::thread([&] {
                        while (!compatibilityStop.load()) {
                            if (stopRequested.load()) {
                                compatibilityStop = true;
                                break;
                            }
                            if (!IsComputerIdleForSharedWork(IdleSeconds(), config.idleAfterSeconds)) {
                                resumedInput = true;
                                compatibilityStop = true;
                                break;
                            }
                            Sleep(250);
                        }
                    });
                }
                if (!owner->mockRenderer_)
                    compatible = CheckWorkerCompatibility(
                        config, owner->DataDir(), blend, version, renderEngine, requiresGpu,
                        *effectiveStop, compatibility);
                compatibilityStop = true;
                if (compatibilityGuard.joinable()) compatibilityGuard.join();
                if (!compatible) {
                    if (resumedInput.load())
                        compatibility = L"电脑恢复使用，已中止 Blender 兼容性检查";
                    response.status = resumedInput.load() ? 425 : 409;
                    response.set_content(WideToUtf8(compatibility), "text/plain; charset=utf-8");
                    std::error_code ignored;
                    fs::remove_all(taskDir, ignored);
                    return;
                }
                WorkItem item;
                item.taskId = taskId;
                item.jobId = jobId;
                item.jobName = jobName.empty() ? jobId : std::move(jobName);
                item.frame = frame;
                item.blend = blend;
                item.temporaryTaskDir = taskDir;
                item.outputDir = taskDir / L"outputs";
                item.outputFormat = format;
                item.outputExtension = extension;
                item.blenderVersion = version;
                item.renderEngine = renderEngine;
                item.requiresGpu = requiresGpu;
                item.attemptToken = attemptToken;
                item.remote = true;
                item.originName = originName.empty() ? request.remote_addr : std::move(originName);
                item.originAddress = request.remote_addr;
                item.originPort = originPort;
                {
                    std::scoped_lock lock(workMutex);
                    work.push(std::move(item));
                }
                reservation.handedOff = true;
                workCv.notify_one();
                Notify();
                response.status = 202;
                response.set_content("accepted", "text/plain");
                LogInfo("accepted remote task " + taskId + "; frame=" +
                        std::to_string(frame) + "; origin=" + request.remote_addr);
            });
        server.Post(
            "/v1/result",
            [&](const httplib::Request& request, httplib::Response& response,
                const httplib::ContentReader& reader) {
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
                            it->state != TaskState::Assigned) {
                            response.status = 409;
                            response.set_content("stale or superseded assignment", "text/plain");
                            return;
                        }
                        found = true;
                        targetJobId = job.id;
                        stagingRoot = owner->DataDir() / L"jobs" / Utf8ToWide(job.id) /
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
                    if (target.attemptToken != attemptToken || target.state != TaskState::Assigned) {
                        std::error_code ignored;
                        if (!stagingRoot.empty()) fs::remove_all(stagingRoot, ignored);
                        response.status = 409;
                        response.set_content("assignment changed while receiving result", "text/plain");
                        return;
                    }
                    fs::path publishedPrimary;
                    if (status == "completed" && ok)
                        ok = PublishResultArtifacts(
                            owner->DataDir(), jobs[targetJob], target.frame,
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
                        target.error.clear();
                        target.endedAt = std::chrono::system_clock::now();
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
                }
                {
                    std::error_code ignored;
                    if (!stagingRoot.empty()) fs::remove_all(stagingRoot, ignored);
                }
                PersistJobs();
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
        httplib::Client client(address, port);
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
        if (config.pairingPending) {
            if (!health.value("pairing_available", false) ||
                !TryPair(address, port, nodeId, result)) {
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
        WorkerActivity signedActivity;
        signedActivity.jobName = HeaderDisplayText(health.value("activity_job", ""), 128);
        signedActivity.frames = health.value("activity_frames", std::vector<int>{});
        if (signedActivity.frames.size() > 5) signedActivity.frames.resize(5);
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
        peer.name = health.value("name", "");
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

        auto nextBroadcast = std::chrono::steady_clock::now();
        while (!stopRequested.load()) {
            Config config = owner->GetConfig();
            if (std::chrono::steady_clock::now() >= nextBroadcast) {
                auto networks = LocalIPv4Interfaces();
                std::vector<std::string> localAddresses;
                for (const auto& network : networks)
                    localAddresses.push_back(network.address);
                std::vector<std::string> payloads;
                if (!config.pairingPending && config.clusterToken.size() >= 16) {
                    WorkerActivity activity = CurrentActivity();
                    json messageV7 = {
                        {"protocol", 7}, {"project_cache", true}, {"frame_packages", true},
                        {"result_bundles", true}, {"package_cancel", true},
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
                        {"idle", static_cast<int>(IdleSeconds())},
                    };
                    messageV7["signature"] = DiscoverySignature(config, messageV7);
                    payloads.push_back(messageV7.dump());
                    // Keep a v1 beacon during the protocol-v7 rollout so an older
                    // installed node can still discover this endpoint.
                    json messageV1 = {
                        {"protocol", 1}, {"node_id", config.nodeId}, {"name", config.nodeName},
                        {"nonce", NewRandomHex(8)}, {"addresses", localAddresses},
                        {"port", config.httpPort}, {"share", config.shareEnabled},
                        {"cpu", 100}, {"assigned", workerAssigned.load()},
                        {"task_id", CurrentTaskId()},
                        {"idle", static_cast<int>(IdleSeconds())},
                    };
                    messageV1["signature"] = DiscoverySignature(config, messageV1);
                    payloads.push_back(messageV1.dump());
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
                if (protocol < 1 || protocol > 7) continue;
                char remoteAddress[INET_ADDRSTRLEN]{};
                inet_ntop(AF_INET, &remote.sin_addr, remoteAddress, sizeof(remoteAddress));
                Peer peer;
                peer.protocolVersion = protocol;
                peer.supportsProjectCache = message.value("project_cache", false);
                peer.supportsFramePackages = message.value("frame_packages", false);
                peer.supportsResultBundles = message.value("result_bundles", false);
                peer.supportsPackageCancellation = message.value("package_cancel", false);
                peer.nodeId = message.value("node_id", "");
                if (!SafeId(peer.nodeId) || peer.nodeId == config.nodeId) continue;
                peer.address = remoteAddress;
                peer.name = message.value("name", peer.nodeId);
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
                    if (peer.activity.frames.size() > 5) peer.activity.frames.resize(5);
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
                    if (protocol == 1 && existing != peers.end() &&
                        existing->second.protocolVersion >= 2) {
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
        for (const auto& [id, peer] : peers) {
            if (!jobId.empty() && rejectedJobPeers.contains({jobId, peer.nodeId})) continue;
            auto backoff = jobPeerBackoff.find({jobId, peer.nodeId});
            if (!jobId.empty() && backoff != jobPeerBackoff.end() && now < backoff->second)
                continue;
            bool locallyLeased = std::any_of(jobs.begin(), jobs.end(), [&](const RenderJob& job) {
                return std::any_of(job.frames.begin(), job.frames.end(),
                    [&](const FrameTask& frame) {
                        return frame.state == TaskState::Assigned &&
                            frame.assignedNodeId == peer.nodeId;
                    });
            });
            if (now - peer.lastSeen <= kPeerTimeout && peer.trusted && peer.shareEnabled &&
                peer.assigned == 0 && !locallyLeased)
                return peer;
        }
        return std::nullopt;
    }

    bool VerifyRemotePeer(const Peer& peer, const Config& config, std::string& error) {
        std::string challenge = NewRandomHex(16);
        httplib::Client verifier(peer.address, peer.httpPort);
        verifier.set_connection_timeout(3, 0);
        verifier.set_read_timeout(5, 0);
        auto proofResponse = verifier.Get("/v1/prove?challenge=" + challenge);
        if (!proofResponse || proofResponse->status != 200) {
            error = "retry: worker failed fresh ownership proof";
            return false;
        }
        json proof = json::parse(proofResponse->body, nullptr, false);
        std::string expectedProof = HmacSha256(config.clusterToken,
            "worker-proof|" + peer.nodeId + "|" + challenge);
        if (proof.is_discarded() || proof.value("node_id", "") != peer.nodeId ||
            !ConstantTimeEqual(proof.value("proof", ""), expectedProof)) {
            error = "permanent: worker ownership proof was invalid";
            return false;
        }
        return true;
    }

    bool EnsureRemoteProjectCache(
        const Peer& peer, const Config& config, const RenderJob& job, std::string& error,
        std::atomic<bool>* jobCancelled) {
        if (!peer.supportsProjectCache || !SafeSha256(job.projectHash)) return true;
        if (CancellationRequested(&stopRequested, jobCancelled)) {
            error = "job was stopped";
            return false;
        }
        httplib::Client cacheClient(peer.address, peer.httpPort);
        cacheClient.set_connection_timeout(3, 0);
        cacheClient.set_read_timeout(5, 0);
        cacheClient.set_write_timeout(5, 0);
        httplib::Headers statusHeaders = {
            {"X-RM-Token", config.clusterToken},
            {"X-RM-Blender", job.blenderVersion},
            {"X-RM-Engine", job.renderEngine},
            {"X-RM-Requires-GPU", job.requiresGpu ? "1" : "0"},
            // Protocol-v6 compatibility: older workers still parse this header,
            // but an empty list prevents legacy strict add-on matching.
            {"X-RM-Addons", "[]"},
        };
        auto status = cacheClient.Get("/v2/cache/status?hash=" + job.projectHash,
                                      statusHeaders);
        if (CancellationRequested(&stopRequested, jobCancelled)) {
            error = "job was stopped";
            return false;
        }
        if (!status) {
            error = "retry: project cache query failed: " +
                httplib::to_string(status.error());
            return false;
        }
        if (status->status == 200) return true;
        if (status->status != 404) {
            std::string detail = status->body.empty() ? "project cache query failed" : status->body;
            error = (status->status >= 500 && status->status != 507) ||
                    status->status == 409 || status->status == 425
                ? "retry: " + detail : "permanent: " + detail;
            return false;
        }
        httplib::Headers uploadHeaders = {
            {"X-RM-Token", config.clusterToken},
            {"X-RM-Project", job.projectHash},
            {"X-RM-Blender", job.blenderVersion},
            {"X-RM-Engine", job.renderEngine},
            {"X-RM-CPU-Safe", "0"},
            {"X-RM-Requires-GPU", job.requiresGpu ? "1" : "0"},
            {"X-RM-Addons", "[]"},
        };
        auto upload = PostFile(peer.address, peer.httpPort, "/v2/cache", uploadHeaders,
                               job.preparedBlend, &stopRequested, 3600, jobCancelled);
        if (!upload || (upload->status != 200 && upload->status != 201)) {
            if (!upload) {
                error = "retry: project cache upload failed: " +
                    httplib::to_string(upload.error());
                return false;
            }
            std::string detail = upload->body.empty()
                ? "project cache upload failed" : upload->body;
            bool transient = (upload->status >= 500 && upload->status != 507) ||
                upload->status == 425 ||
                (upload->status == 409 && detail == "worker is busy");
            error = (transient ? "retry: " : "permanent: ") + detail;
            return false;
        }
        return true;
    }

    bool DispatchRemote(const Peer& peer, const RenderJob& job, const FrameTask& frame,
                         const std::string& attemptToken,
        std::string& error, std::atomic<bool>* jobCancelled) {
        if (frame.frame == 0) {
            error = "permanent: protocol v6 workers do not advertise frame-zero support";
            return false;
        }
        // Legacy single-frame transport returns only the primary image. Never
        // silently mark a compositor job complete without its File Output files.
        if (!job.compositorOutputs.empty()) {
            error = "permanent: worker single-frame protocol cannot return compositor outputs";
            return false;
        }
        Config config = owner->GetConfig();
        if (CancellationRequested(&stopRequested, jobCancelled)) {
            error = "job was stopped";
            return false;
        }
        if (!VerifyRemotePeer(peer, config, error)) return false;
        httplib::Headers headers = {
            {"X-RM-Token", config.clusterToken}, {"X-RM-Task", frame.taskId},
            {"X-RM-Job", job.id}, {"X-RM-Frame", std::to_string(frame.frame)},
            {"X-RM-Job-Name", HeaderDisplayText(job.name)},
            {"X-RM-Origin-Name", HeaderDisplayText(config.nodeName)},
            {"X-RM-Project", job.projectHash},
            {"X-RM-Format", job.outputFormat}, {"X-RM-Blender", job.blenderVersion},
            {"X-RM-Extension", job.outputExtension},
            {"X-RM-Engine", job.renderEngine},
            {"X-RM-CPU-Safe", "0"},
            {"X-RM-Requires-GPU", job.requiresGpu ? "1" : "0"},
            {"X-RM-Attempt", attemptToken},
            {"X-RM-Addons", "[]"},
            {"X-RM-Origin-Port", std::to_string(config.httpPort)},
        };
        httplib::Result result;
        if (peer.supportsProjectCache && SafeSha256(job.projectHash)) {
            if (!EnsureRemoteProjectCache(peer, config, job, error, jobCancelled)) return false;
            result = PostEmpty(peer.address, peer.httpPort, "/v2/task", headers,
                               &stopRequested, jobCancelled);
        } else {
            result = PostFile(peer.address, peer.httpPort, "/v1/task", headers,
                              job.preparedBlend, &stopRequested, 3600, jobCancelled);
        }
        if (!result) {
            error = "retry: network error: " + httplib::to_string(result.error());
            return false;
        }
        if (result->status != 202) {
            std::string detail = result->body.empty() ? "worker rejected task" : result->body;
            bool transient = (result->status >= 500 && result->status != 507) ||
                result->status == 404 || result->status == 409 || result->status == 425;
            error = (transient ? "retry: " : "permanent: ") + detail;
            LogWarning("worker rejected task " + frame.taskId + "; node=" + peer.nodeId);
            return false;
        }
        LogInfo("dispatched task " + frame.taskId + "; node=" + peer.nodeId);
        return true;
    }

    bool DispatchRemotePackage(
        const Peer& peer, const RenderJob& job, const std::vector<FrameTask>& frames,
        const std::vector<std::string>& attemptTokens, const std::string& packageId,
        std::string& error, std::atomic<bool>* jobCancelled) {
        if (!peer.supportsFramePackages || !peer.supportsProjectCache || frames.empty() ||
            frames.size() > 5 || frames.size() != attemptTokens.size()) {
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
        httplib::Headers headers = {
            {"X-RM-Token", config.clusterToken}, {"X-RM-Package", packageId},
            {"X-RM-Job", job.id}, {"X-RM-Frames", frameMetadata.dump()},
            {"X-RM-Job-Name", HeaderDisplayText(job.name)},
            {"X-RM-Origin-Name", HeaderDisplayText(config.nodeName)},
            {"X-RM-Compositor", compositorHeader},
            {"X-RM-Result-Bundles", peer.supportsResultBundles ? "1" : "0"},
            {"X-RM-Project", job.projectHash}, {"X-RM-Format", job.outputFormat},
            {"X-RM-Extension", job.outputExtension},
            {"X-RM-Blender", job.blenderVersion}, {"X-RM-Engine", job.renderEngine},
            {"X-RM-CPU-Safe", "0"},
            {"X-RM-Requires-GPU", job.requiresGpu ? "1" : "0"},
            {"X-RM-Addons", "[]"},
            {"X-RM-Origin-Port", std::to_string(config.httpPort)},
        };
        auto result = PostEmpty(peer.address, peer.httpPort, "/v3/package", headers,
                                &stopRequested, jobCancelled);
        if (!result) {
            error = "retry: package dispatch network error: " +
                httplib::to_string(result.error());
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
                if (frame.state != TaskState::Assigned ||
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
            constexpr size_t wantedFrames = 5;
            size_t jobIndex = 0;
            std::vector<size_t> frameIndices;
            RenderJob jobCopy;
            std::vector<FrameTask> frameCopies;
            bool found = false;
            {
                std::scoped_lock lock(stateMutex);
                for (size_t j = 0; j < jobs.size() && !found; ++j) {
                    for (size_t f = 0; f < jobs[j].frames.size(); ++f) {
                        if (jobs[j].frames[f].state == TaskState::Pending) {
                            jobIndex = j;
                            jobCopy = jobs[j];
                            for (size_t candidate = f;
                                 candidate < jobs[j].frames.size() &&
                                 frameIndices.size() < wantedFrames;
                                 ++candidate) {
                                if (jobs[j].frames[candidate].state != TaskState::Pending)
                                    continue;
                                jobs[j].frames[candidate].state = TaskState::Dispatching;
                                frameIndices.push_back(candidate);
                                frameCopies.push_back(jobs[j].frames[candidate]);
                            }
                            found = true;
                            break;
                        }
                    }
                }
            }
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
            if (peer && frameIndices.size() > 1 &&
                (!peer->supportsFramePackages || !peer->supportsPackageCancellation)) {
                std::scoped_lock lock(stateMutex);
                for (size_t index = 1; index < frameIndices.size(); ++index) {
                    if (jobIndex < jobs.size() && frameIndices[index] < jobs[jobIndex].frames.size() &&
                        jobs[jobIndex].frames[frameIndices[index]].state == TaskState::Dispatching)
                        jobs[jobIndex].frames[frameIndices[index]].state = TaskState::Pending;
                }
                frameIndices.resize(1);
                frameCopies.resize(1);
            }
            auto jobCancelled = JobCancellation(jobCopy.id);
            bool handled = false;
            if (peer) {
                std::string dispatchError;
                std::string packageId = peer->supportsFramePackages
                    ? NewRandomHex(12) : frameCopies.front().taskId;
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
                    for (size_t index = 0; index < frameIndices.size(); ++index) {
                        if (!assignmentReserved) break;
                        if (jobIndex >= jobs.size() ||
                            frameIndices[index] >= jobs[jobIndex].frames.size())
                            continue;
                        auto& frame = jobs[jobIndex].frames[frameIndices[index]];
                        frame.state = TaskState::Assigned;
                        frame.assignedNodeId = peer->nodeId;
                        frame.assignedNodeName = peer->name;
                        frame.assignedPackageId = packageId;
                        frame.assignedAt = {};
                        frame.attemptToken = attemptTokens[index];
                        frame.startedAt = std::chrono::system_clock::now();
                        frame.endedAt = {};
                        ++frame.dispatchAttempts;
                    }
                }
                bool dispatched = assignmentReserved && (peer->supportsFramePackages
                    ? DispatchRemotePackage(*peer, jobCopy, frameCopies, attemptTokens,
                                            packageId, dispatchError, jobCancelled.get())
                    : DispatchRemote(*peer, jobCopy, frameCopies.front(),
                                     attemptTokens.front(), dispatchError,
                                     jobCancelled.get()));
                if (dispatched) {
                    std::scoped_lock lock(stateMutex);
                    for (size_t index = 0; index < frameIndices.size(); ++index) {
                        if (jobIndex >= jobs.size() ||
                            frameIndices[index] >= jobs[jobIndex].frames.size())
                            continue;
                        auto& frame = jobs[jobIndex].frames[frameIndices[index]];
                        if (frame.state == TaskState::Assigned &&
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
                        if (frame.state == TaskState::Assigned &&
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
                item.blend = jobCopy.preparedBlend;
                item.outputDir = owner->DataDir() / L"jobs" / Utf8ToWide(jobCopy.id) /
                    L"packages" / Utf8ToWide(item.packageId);
                item.outputFormat = jobCopy.outputFormat;
                item.outputExtension = jobCopy.outputExtension;
                item.blenderVersion = jobCopy.blenderVersion;
                item.renderEngine = jobCopy.renderEngine;
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
                        frame.state = TaskState::Rendering;
                        frame.assignedNodeId = config.nodeId;
                        frame.assignedNodeName = config.nodeName;
                        frame.assignedPackageId = item.packageId;
                        frame.assignedAt = {};
                        frame.attemptToken.clear();
                        frame.startedAt = std::chrono::system_clock::now();
                        frame.endedAt = {};
                    }
                }
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
            PersistJobs();
            Notify();
        }
    }

    bool SendRemoteResult(const WorkItem& item, const WorkItem::Frame& frame,
                          bool success, bool retry,
                          const fs::path& output, const std::wstring& renderError) {
        Config config = owner->GetConfig();
        httplib::Headers headers = {
            {"X-RM-Token", config.clusterToken}, {"X-RM-Task", frame.taskId},
            {"X-RM-Attempt", frame.attemptToken},
            {"X-RM-Status", retry ? "retry" : success ? "completed" : "failed"},
            {"X-RM-Error", HeaderDisplayText(WideToUtf8(renderError), 2048)},
        };
        if (success && item.resultBundles)
            headers.emplace("X-RM-Result-Bundle", "1");
        for (int attempt = 0; attempt < 4 && !stopRequested.load(); ++attempt) {
            if (attempt) {
                for (int step = 0; step < attempt * 20 && !stopRequested.load(); ++step)
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                if (stopRequested.load()) break;
            }
            httplib::Result result;
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
                    PersistJobs();
                    Notify();
                    continue;
                }
            }
            {
                std::scoped_lock lock(currentTaskMutex);
                currentTaskId = item.packageId.empty() ? item.taskId : item.packageId;
                currentActivity.jobName = HeaderDisplayText(
                    item.jobName.empty() ? item.jobId : item.jobName, 128);
                currentActivity.frames.clear();
                currentActivity.frames.reserve(item.frames.size());
                for (const auto& frame : item.frames)
                    currentActivity.frames.push_back(frame.frame);
                currentActivity.completedFrames = 0;
                currentActivity.rendering = true;
            }
            Notify();
            LogInfo("render package started; package=" +
                    (item.packageId.empty() ? item.taskId : item.packageId) + "; frames=" +
                    std::to_string(item.frames.size()));
            const std::string cancellationId = item.packageId.empty() ? item.taskId : item.packageId;
            const auto packageStartedAt = std::chrono::system_clock::now();
            std::vector<int> deliveredFrames;
            std::map<int, std::vector<RenderArtifact>> outputs;
            std::wstring renderError;
            bool retryAfterIdle = false;
            Config renderConfig = owner->GetConfig();
            std::set<int> completedCallbacks;
            std::set<int> reportedFrames;
            auto reportCompletedFrame = [&](int frameNumber,
                                             const std::vector<RenderArtifact>& artifacts) {
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
                }
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
                        frameSuccess ? transferFile : fs::path{}, frameError);
                    if (delivered) reportedFrames.insert(frameNumber);
                    if (delivered && frameSuccess) {
                        deliveredFrames.push_back(frameNumber);
                        LogInfo("render frame delivered; package=" + cancellationId +
                                "; frame=" + std::to_string(frameNumber));
                    } else if (!delivered) {
                        std::error_code ignored;
                        fs::path failures = owner->DataDir() / L"failed-results";
                        fs::create_directories(failures, ignored);
                        std::ofstream record(
                            failures / (Utf8ToWide(packagedFrame->taskId) + L".txt"),
                            std::ios::binary | std::ios::trunc);
                        record << "origin=" << item.originAddress << ':' << item.originPort << '\n'
                               << "attempt=" << packagedFrame->attemptToken << '\n'
                               << "status=" << (frameSuccess ? "completed" : "failed") << '\n'
                               << "error=" << WideToUtf8(frameError) << '\n';
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
                            frame->state == TaskState::Rendering &&
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
                    owner->DataDir(), jobSnapshot, frameNumber, artifactMap,
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
                        if (frame != job->frames.end() && frame->state == TaskState::Rendering &&
                            frame->assignedPackageId == item.packageId) {
                            stateUpdated = true;
                            frame->state = published ? TaskState::Completed : TaskState::Failed;
                            frame->assignedPackageId.clear();
                            if (published) frame->output = publishedPrimary;
                            frame->error = published ? "" : WideToUtf8(publishError);
                            frame->endedAt = std::chrono::system_clock::now();
                        }
                    }
                }
                if (stateUpdated) reportedFrames.insert(frameNumber);
                PersistJobs();
                Notify();
            };
            if (item.remote && PackageCancelled(cancellationId)) {
                renderError = L"任务包已由提交方停止";
                retryAfterIdle = true;
            } else if (!owner->mockRenderer_ && item.remote &&
                !IsComputerIdleForSharedWork(IdleSeconds(), renderConfig.idleAfterSeconds)) {
                renderError = L"电脑恢复使用后已停止，将在空闲时重新分配";
                retryAfterIdle = true;
            } else if (owner->mockRenderer_) {
                fs::create_directories(item.outputDir);
                for (const auto& frame : item.frames) {
                    if (item.remote && PackageCancelled(cancellationId)) {
                        retryAfterIdle = true;
                        renderError = L"任务包已由提交方停止";
                        break;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(80));
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
                    auto completed = outputs.find(frame.frame);
                    if (completed != outputs.end())
                        reportCompletedFrame(frame.frame, completed->second);
                }
            } else {
                std::atomic<bool> finishCurrentThenStop = false;
                std::vector<int> frameNumbers;
                for (const auto& frame : item.frames) frameNumbers.push_back(frame.frame);
                auto renderStartedAt = std::chrono::steady_clock::now();
                bool rendered = RenderBlenderPackage(
                    renderConfig, item.blend, item.outputDir, frameNumbers,
                    item.outputFormat, item.outputExtension, item.remote, stopRequested,
                    finishCurrentThenStop, item.compositorOutputs, reportCompletedFrame,
                    outputs, renderError);
                auto renderDuration = std::chrono::steady_clock::now() - renderStartedAt;
                bool quickEmptyExit = ShouldRetryQuickEmptyPackage(
                    item.remote, rendered, outputs.empty(), finishCurrentThenStop.load(),
                    stopRequested.load(), PackageCancelled(cancellationId), renderDuration,
                    renderError);
                if (quickEmptyExit) {
                    LogWarning("Blender exited without a frame; retrying package once; package=" +
                               cancellationId);
                    std::this_thread::sleep_for(std::chrono::milliseconds(500));
                    renderError.clear();
                    outputs.clear();
                    RenderBlenderPackage(
                        renderConfig, item.blend, item.outputDir, frameNumbers,
                        item.outputFormat, item.outputExtension, item.remote, stopRequested,
                        finishCurrentThenStop, item.compositorOutputs, reportCompletedFrame,
                        outputs, renderError);
                }
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
                        frameSuccess ? transferFile : fs::path{}, renderError);
                    if (delivered && frameSuccess) deliveredFrames.push_back(frame.frame);
                    if (!delivered) {
                        std::error_code ignored;
                        fs::path failures = owner->DataDir() / L"failed-results";
                        fs::create_directories(failures, ignored);
                        std::ofstream record(
                            failures / (Utf8ToWide(frame.taskId) + L".txt"),
                            std::ios::binary | std::ios::trunc);
                        record << "origin=" << item.originAddress << ':' << item.originPort << '\n'
                               << "attempt=" << frame.attemptToken << '\n'
                               << "status=" << (frameRetry ? "retry" :
                                                   frameSuccess ? "completed" : "failed") << '\n'
                               << "error=" << WideToUtf8(renderError) << '\n';
                    }
                }
                // The origin observes the worker's current task ID and requeues a
                // lost delivery. Keep only a small diagnostic record, never an
                // unbounded copy of the received project and frame output.
                std::error_code ignored;
                fs::remove_all(item.temporaryTaskDir.empty()
                                   ? item.blend.parent_path() : item.temporaryTaskDir,
                               ignored);
                if (!item.projectHash.empty()) {
                    std::scoped_lock lock(cacheMutex);
                    TouchCacheEntry(owner->DataDir(), item.projectHash);
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
                        if (frameSuccess) {
                            std::map<std::string, fs::path> artifacts;
                            for (const auto& artifact : output->second)
                                artifacts[artifact.logicalPath] = artifact.source;
                            frameSuccess = PublishResultArtifacts(
                                owner->DataDir(), job, packagedFrame.frame, artifacts,
                                item.packageId, publishedPrimary, publishError,
                                jobCancelled.get());
                        }
                        if (frame->state == TaskState::Stopped) continue;
                        frame->state = frameSuccess ? TaskState::Completed : TaskState::Failed;
                        frame->assignedPackageId.clear();
                        if (frameSuccess) frame->output = publishedPrimary;
                        frame->error = frameSuccess ? "" : WideToUtf8(
                            publishError.empty() ? renderError : publishError);
                        frame->endedAt = std::chrono::system_clock::now();
                    }
                    break;
                }
                std::error_code ignored;
                fs::remove_all(item.outputDir, ignored);
            }
            {
                std::scoped_lock lock(currentTaskMutex);
                currentTaskId.clear();
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
            PersistJobs();
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

bool NodeApp::UpdateConfig(Config config, std::wstring& error) {
    Config current = GetConfig();
    if (config.inviteCode != current.inviteCode) {
        config.clusterToken.clear();
        config.pairingPending = true;
        config.manualPeerEndpoint.clear();
    }
    if (config.nodeName.empty() || !IsInviteCode(config.inviteCode) ||
        (!config.pairingPending && config.clusterToken.size() < 16) ||
        config.idleAfterSeconds < 1 || config.idleAfterSeconds > 86400 ||
        (config.theme != "system" && config.theme != "light" && config.theme != "dark")) {
        error = L"设置值无效";
        return false;
    }
    if (!store_.Save(config, error)) return false;
    {
        std::scoped_lock lock(configMutex_);
        config_ = config;
    }
    impl_->Notify();
    LogInfo("settings updated for node " + config.nodeId);
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

std::vector<ContributionRecord> NodeApp::Contributions() const {
    std::scoped_lock lock(impl_->stateMutex);
    return impl_->contributions;
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
                    stopMarkers.push_back(DataDir() / L"jobs" / Utf8ToWide(job.id) / L"packages" /
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
        std::ofstream(marker, std::ios::binary | std::ios::trunc) << "stop";
    }
    Config config = GetConfig();
    for (const auto& cancellation : remoteCancellations) {
        httplib::Headers headers = {
            {"X-RM-Token", config.clusterToken},
            {"X-RM-Package", cancellation.packageId},
        };
        auto result = PostEmpty(cancellation.address, cancellation.port, "/v3/cancel",
                                headers, &impl_->stopRequested);
        if (!result || result->status != 202)
            LogWarning("worker cancellation was not acknowledged; package=" +
                       cancellation.packageId);
    }
    impl_->PersistJobs();
    impl_->Notify();
    LogInfo("job stopped; id=" + jobId);
    return true;
}

HardwareInfo NodeApp::Hardware() const { return impl_->hardware; }

int NodeApp::AssignedCount() const {
    return impl_->workerAssigned.load();
}

WorkerActivity NodeApp::CurrentActivity() const {
    return impl_->CurrentActivity();
}

void NodeApp::InjectPeerForTest(Peer peer) {
    peer.lastSeen = std::chrono::steady_clock::now();
    std::scoped_lock lock(impl_->stateMutex);
    impl_->peers[peer.nodeId] = std::move(peer);
}

void NodeApp::RemovePeerForTest(const std::string& nodeId) {
    std::scoped_lock lock(impl_->stateMutex);
    impl_->peers.erase(nodeId);
}

std::string NodeApp::SubmitPrepared(
    std::string name,
    const PreflightReport& report,
    int frameStart,
    int frameEnd,
    int frameStep,
    std::wstring& error) {
    if (!report.passed || !fs::exists(report.preparedBlend)) {
        error = L"工程未通过预检";
        return {};
    }
    if (name.empty() || frameStart < kBlenderFrameMin || frameEnd < frameStart ||
        frameEnd > kBlenderFrameMax ||
        frameStep < 1 || report.outputFormat.empty() ||
        !IsSafeOutputExtension(report.outputExtension)) {
        error = L"任务参数无效";
        return {};
    }
    std::string id = NewRandomHex(8);
    fs::path jobDir = DataDir() / L"jobs" / Utf8ToWide(id);
    fs::create_directories(jobDir / L"outputs");
    fs::path source = jobDir / L"source.blend";
    if (!CopyFileAfterTransientLocks(report.preparedBlend, source, error)) {
        std::error_code ignored;
        fs::remove_all(jobDir, ignored);
        return {};
    }
    RenderJob job;
    job.id = id;
    job.name = std::move(name);
    job.sourceBlend = report.sourceBlend;
    job.preparedBlend = source;
    job.projectHash = Sha256File(source);
    if (!SafeSha256(job.projectHash)) {
        error = L"无法计算已打包工程的 SHA-256";
        return {};
    }
    job.outputFormat = report.outputFormat;
    job.outputExtension = report.outputExtension;
    job.blenderVersion = report.blenderVersion;
    job.renderEngine = report.renderEngine;
    job.requiresGpu = report.requiresGpu;
    job.primaryOutputs = report.primaryOutputs;
    job.compositorOutputs = report.compositorOutputs;
    job.createdAt = std::chrono::system_clock::now();
    for (int frame = frameStart; frame <= frameEnd; frame += frameStep) {
        FrameTask task;
        task.taskId = id + "-" + std::to_string(frame);
        task.frame = frame;
        job.frames.push_back(std::move(task));
        if (job.frames.size() > 100000) {
            error = L"帧数超过 100000，已拒绝";
            return {};
        }
    }
    {
        std::scoped_lock lock(impl_->stateMutex);
        impl_->jobs.push_back(job);
    }
    impl_->PersistJobs();
    impl_->Notify();
    LogInfo("job submitted; id=" + id + "; frames=" +
            std::to_string(job.frames.size()) + "; format=" + job.outputFormat);
    return id;
}

bool RunSelfTest(std::wstring& report) {
    try {
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
        std::wstring error;
        Config persisted;
        if (!store.Save(config, error) ||
            (persisted = store.Load()).idleAfterSeconds != 7 ||
            persisted.inviteCode != "042731" ||
            persisted.clusterToken != "legacy-token-123" ||
            persisted.manualPeerEndpoint != config.manualPeerEndpoint ||
            persisted.theme != "dark") {
            report = L"配置持久化失败：" + error;
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
        if (!pendingJoin.pairingPending || !pendingJoin.clusterToken.empty() ||
            pendingJoin.inviteCode != joinUpdate.inviteCode || previousCredential.empty()) {
            report = L"Six-digit invite code did not enter pairing state";
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
        discoveryV7["activity_job"] = "tampered render";
        if (ConstantTimeEqual(discoveryV7Proof,
                              DiscoverySignature(signatureConfig, discoveryV7))) {
            report = L"Protocol-v7 worker activity was not authenticated";
            return false;
        }
        Peer authenticatedPeer;
        authenticatedPeer.protocolVersion = 7;
        authenticatedPeer.supportsProjectCache = true;
        authenticatedPeer.supportsFramePackages = true;
        authenticatedPeer.supportsResultBundles = true;
        authenticatedPeer.supportsPackageCancellation = true;
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
        if (legacyUpdate.protocolVersion != 7 || !legacyUpdate.supportsProjectCache ||
            !legacyUpdate.supportsFramePackages || !legacyUpdate.supportsResultBundles ||
            !legacyUpdate.supportsPackageCancellation ||
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
        httplib::Server server;
        fs::path received = root / L"received.bin";
        fs::path cancelledReceive = root / L"cancelled.bin";
        std::atomic<bool> cancelInbound = true;
        server.Post("/stream", [&](const httplib::Request&, httplib::Response& response,
                                   const httplib::ContentReader& reader) {
            std::wstring streamError;
            bool ok = StreamRequestToFile(reader, received, streamError);
            response.status = ok ? 200 : 500;
        });
        server.Post("/cancelled-stream", [&](const httplib::Request&,
                                              httplib::Response& response,
                                              const httplib::ContentReader& reader) {
            std::wstring streamError;
            bool ok = StreamRequestToFile(
                reader, cancelledReceive, streamError, &cancelInbound);
            response.status = ok ? 200 : 499;
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
        server.stop();
        thread.join();
        if (!result || result->status != 200 || fs::file_size(received) != fs::file_size(source) ||
            (cancelledResult && cancelledResult->status == 200) || fs::exists(cancelledReceive)) {
            report = L"流式任务传输失败";
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
            httplib::Server originPortProbe;
            httplib::Server workerPortProbe;
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
                httplib::Server malformedServer;
                malformedServer.Get("/health", [](const httplib::Request&,
                                                    httplib::Response& response) {
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
                bool hardwareReported = false;
                for (const auto& peer : origin.Peers()) {
                    if (peer.nodeId == workerConfig.nodeId && !peer.cpuModel.empty()) {
                        hardwareReported = true;
                        break;
                    }
                }

                fs::path prepared = root / L"prepared.blend";
                {
                    std::ofstream blend(prepared, std::ios::binary);
                    blend << "RenderMesh mock blend payload";
                }
                PreflightReport preflight;
                preflight.passed = true;
                preflight.sourceBlend = prepared;
                preflight.preparedBlend = prepared;
                preflight.blenderVersion = "4.5";
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
                std::string jobId = origin.SubmitPrepared(
                    "two-node-self-test", preflight, 1, 7, 1, submitError);
                bool clusterOk = malformedRejected && pairingStarted && paired &&
                    manuallyConnected && hardwareReported && stoppedOk && stoppedReloaded &&
                    !jobId.empty();
                bool usedRemote = false;
                bool streamedFirstFrame = false;
                auto streamingDeadline = std::chrono::steady_clock::now() +
                    std::chrono::seconds(3);
                while (clusterOk && std::chrono::steady_clock::now() < streamingDeadline) {
                    auto current = origin.Jobs();
                    if (!current.empty() && current.back().id == jobId &&
                        current.back().frames.size() >= 5) {
                        bool firstComplete =
                            current.back().frames[0].state == TaskState::Completed;
                        bool laterIncomplete = false;
                        for (size_t index = 1; index < 5; ++index)
                            laterIncomplete = laterIncomplete ||
                                current.back().frames[index].state != TaskState::Completed;
                        WorkerActivity workerProgress = worker.CurrentActivity();
                        streamedFirstFrame = firstComplete && laterIncomplete &&
                            workerProgress.rendering &&
                            workerProgress.jobName == "two-node-self-test" &&
                            workerProgress.frames == std::vector<int>{1, 2, 3, 4, 5} &&
                            workerProgress.completedFrames > 0 &&
                            workerProgress.completedFrames < workerProgress.frames.size() &&
                            fs::exists(current.back().frames[0].output) &&
                            fs::exists(finalOutputRoot / L"compositor" / L"ViewLayer" /
                                L"pass_1.exr");
                        if (streamedFirstFrame) break;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                }
                clusterOk = clusterOk && streamedFirstFrame;
                if (!streamedFirstFrame)
                    error = L"per-frame result streaming self-test failed";
                auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(12);
                while (clusterOk && std::chrono::steady_clock::now() < deadline) {
                    auto jobs = origin.Jobs();
                    if (!jobs.empty() && jobs.back().id == jobId) {
                        bool allDone = !jobs.back().frames.empty();
                        for (const auto& frame : jobs.back().frames) {
                            allDone = allDone && frame.state == TaskState::Completed &&
                                fs::exists(frame.output);
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
                    std::string zeroFrameJob = origin.SubmitPrepared(
                        "frame-zero-local-fence-self-test", preflight, 0, 5, 1, submitError);
                    bool zeroStayedLocalAndOthersRemote = false;
                    auto zeroDeadline = std::chrono::steady_clock::now() +
                        std::chrono::seconds(6);
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
                    std::string cancelledJob = origin.SubmitPrepared(
                        "remote-cancel-self-test", preflight, 30, 34, 1, submitError);
                    bool remotelyAssigned = false;
                    auto assignmentDeadline = std::chrono::steady_clock::now() +
                        std::chrono::seconds(3);
                    while (!cancelledJob.empty() &&
                           std::chrono::steady_clock::now() < assignmentDeadline) {
                        auto current = origin.Jobs();
                        if (!current.empty() && current.back().id == cancelledJob &&
                            current.back().frames.front().state == TaskState::Assigned &&
                            current.back().frames.front().assignedNodeId == workerConfig.nodeId) {
                            remotelyAssigned = true;
                            break;
                        }
                        std::this_thread::sleep_for(std::chrono::milliseconds(10));
                    }
                    bool stopAccepted = remotelyAssigned && origin.StopJob(cancelledJob, submitError);
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
                                    record.completedFrames.size() <= 1) {
                                    workerStoppedPackage = true;
                                    break;
                                }
                            }
                        }
                        if (workerStoppedPackage) break;
                        std::this_thread::sleep_for(std::chrono::milliseconds(20));
                    }
                    // If cancellation wins the dispatch race, the worker rejects
                    // the package before queueing it and therefore has no history
                    // row. That is also a successful stop fence.
                    if (stopAccepted && workerCancellationState.empty() &&
                        worker.AssignedCount() == 0)
                        workerStoppedPackage = true;
                    bool originStayedStopped = false;
                    for (const auto& current : origin.Jobs()) {
                        if (current.id != cancelledJob) continue;
                        originStayedStopped = !current.frames.empty();
                        for (const auto& frame : current.frames)
                            originStayedStopped = originStayedStopped &&
                                frame.state == TaskState::Stopped;
                    }
                    clusterOk = stopAccepted && workerStoppedPackage && originStayedStopped;
                    if (!clusterOk) error = L"remote package cancellation self-test failed; assigned=" +
                        std::to_wstring(remotelyAssigned) + L"; accepted=" +
                        std::to_wstring(stopAccepted) + L"; worker_stopped=" +
                        std::to_wstring(workerStoppedPackage) + L"; origin_stopped=" +
                        std::to_wstring(originStayedStopped) + L"; worker_state=" +
                        workerCancellationState;
                }
                if (clusterOk) {
                    worker.Stop();
                    NodeApp reloadedWorker(ConfigStore(workerRoot), workerConfig, true);
                    bool historyReloaded = false;
                    for (const auto& record : reloadedWorker.Contributions())
                        historyReloaded = historyReloaded ||
                            (record.jobId == jobId && record.completedFrames.size() == 5);
                    clusterOk = historyReloaded;
                    origin.RemovePeerForTest(workerConfig.nodeId);
                    httplib::Server incompatibleServer;
                    std::atomic<int> incompatibleUploads{0};
                    std::atomic<int> cacheStatusQueries{0};
                    std::atomic<bool> transientBusy{false};
                    incompatibleServer.Get("/v1/prove", [&](const httplib::Request& request,
                                                              httplib::Response& response) {
                        std::string challenge = request.has_param("challenge")
                            ? request.get_param_value("challenge") : "";
                        response.set_content(json{
                            {"node_id", "selftest-incompatible-worker"},
                            {"proof", HmacSha256(sharedToken,
                                "worker-proof|selftest-incompatible-worker|" + challenge)},
                        }.dump(), "application/json");
                    });
                    incompatibleServer.Get("/v2/cache/status", [&] (
                        const httplib::Request&, httplib::Response& response) {
                        ++cacheStatusQueries;
                        response.status = transientBusy.load() ? 425 : 404;
                        if (transientBusy.load())
                            response.set_content("computer is active", "text/plain");
                    });
                    incompatibleServer.Post("/v2/cache", [&](const httplib::Request&,
                                                               httplib::Response& response) {
                        ++incompatibleUploads;
                        response.status = 409;
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
                    incompatiblePeer.protocolVersion = 6;
                    incompatiblePeer.shareEnabled = true;
                    incompatiblePeer.trusted = true;
                    incompatiblePeer.supportsProjectCache = true;
                    incompatiblePeer.supportsFramePackages = true;
                    incompatiblePeer.supportsResultBundles = true;
                    incompatiblePeer.supportsPackageCancellation = true;
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
                    httplib::Server alternatePortProbe;
                    int alternatePort = alternatePortProbe.bind_to_any_port("127.0.0.1");
                    alternatePortProbe.stop();
                    alternateConfig.nodeName = "SELFTEST-COMPATIBLE-WORKER";
                    alternateConfig.nodeId = "selftest-z-compatible-worker";
                    alternateConfig.clusterToken = sharedToken;
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
                        alternatePeer.protocolVersion = 6;
                        alternatePeer.shareEnabled = true;
                        alternatePeer.trusted = true;
                        alternatePeer.supportsProjectCache = true;
                        alternatePeer.supportsFramePackages = true;
                        alternatePeer.supportsResultBundles = true;
                        alternatePeer.supportsPackageCancellation = true;
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
                    httplib::Server lostServer;
                    lostServer.Get("/v1/prove", [&](const httplib::Request& request,
                                                     httplib::Response& response) {
                        std::string challenge = request.has_param("challenge")
                            ? request.get_param_value("challenge") : "";
                        response.set_content(json{
                            {"node_id", "selftest-lost-worker"},
                            {"proof", HmacSha256(sharedToken,
                                "worker-proof|selftest-lost-worker|" + challenge)},
                        }.dump(), "application/json");
                    });
                    lostServer.Post("/v1/task", [](const httplib::Request&,
                                                    httplib::Response& response) {
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
                    lostPeer.shareEnabled = true;
                    lostPeer.trusted = true;
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
                            current.back().frames.front().state == TaskState::Assigned) {
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
                    switchedPeer.shareEnabled = true;
                    switchedPeer.trusted = true;
                    switchedPeer.assigned = 1;
                    switchedPeer.currentTaskId = "unrelated-task";
                    origin.InjectPeerForTest(std::move(switchedPeer));
                    lostServer.stop();
                    if (lostThread.joinable()) lostThread.join();
                    bool recoveredLocally = false;
                    auto recoveryDeadline = std::chrono::steady_clock::now() +
                        std::chrono::seconds(4);
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
                    httplib::Client staleClient("127.0.0.1", originConfig.httpPort);
                    httplib::Headers staleHeaders = {
                        {"X-RM-Token", sharedToken},
                        {"X-RM-Task", recoveryJob + "-4"},
                        {"X-RM-Attempt", expiredAttempt},
                        {"X-RM-Status", "failed"},
                        {"X-RM-Error", "late result from expired worker"},
                    };
                    auto staleResult = staleClient.Post(
                        "/v1/result", staleHeaders, "", "application/octet-stream");
                    bool staleRejected = staleResult && staleResult->status == 409;
                    clusterOk = reachedAssigned && recoveredLocally && staleRejected;
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
        report = L"SELF_TEST_OK：配置、可取消流式传输、仅空闲接单、双节点五帧分发、停止栅栏和掉线重派均通过";
        return true;
    } catch (const std::exception& exception) {
        report = L"自检异常：" + Utf8ToWide(exception.what());
        return false;
    }
}

}  // namespace rm
