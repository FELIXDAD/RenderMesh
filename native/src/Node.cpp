#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>

#include "Core.hpp"
#include "../third_party/httplib.h"

#include <algorithm>
#include <deque>
#include <fstream>
#include <regex>

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

int HeaderInt(const httplib::Request& request, const char* name, int fallback = 0) {
    try {
        return std::stoi(request.get_header_value(name));
    } catch (...) {
        return fallback;
    }
}

std::vector<std::string> HeaderStringList(const httplib::Request& request, const char* name) {
    try {
        return json::parse(request.get_header_value(name)).get<std::vector<std::string>>();
    } catch (...) {
        return {};
    }
}

bool StreamRequestToFile(
    const httplib::ContentReader& reader,
    const fs::path& destination,
    std::wstring& error) {
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
        if (size > kMaxTransferBytes - receivedBytes) return false;
        receivedBytes += size;
        output.write(data, static_cast<std::streamsize>(size));
        return static_cast<bool>(output);
    });
    output.close();
    if (!received || !output) {
        error = L"文件接收中断";
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

bool HasTransferCapacity(const httplib::Request& request, const fs::path& directory) {
    std::string value = request.get_header_value("Content-Length");
    if (value.empty()) return true;  // The streaming hard limit still applies.
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
    return freeBytes.QuadPart > requested && freeBytes.QuadPart - requested >= reserve;
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
        signedValue += "|" + message.value("cpu_model", "") + "|" +
            json(message.value("gpu_models", std::vector<std::string>{})).dump();
    }
    return HmacSha256(config.clusterToken, signedValue);
}

std::string PairingBeaconProof(const std::string& inviteCode, const json& message) {
    return HmacSha256(inviteCode,
        "pairing-beacon|" + message.value("node_id", "") + "|" +
        message.value("nonce", "") + "|" + std::to_string(message.value("port", 0)) + "|" +
        json(message.value("addresses", std::vector<std::string>{})).dump());
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

httplib::Result PostFile(
    const std::string& host,
    int port,
    const std::string& route,
    const httplib::Headers& headers,
    const fs::path& file,
    std::atomic<bool>* stopRequested = nullptr) {
    std::error_code ec;
    size_t total = static_cast<size_t>(fs::file_size(file, ec));
    httplib::Client client(host, port);
    client.set_connection_timeout(3, 0);
    // Bounded socket waits keep application shutdown responsive even if a peer
    // accepts a connection and then stalls indefinitely.
    client.set_read_timeout(5, 0);
    client.set_write_timeout(5, 0);
    auto provider = [file, stopRequested](size_t offset, size_t length, httplib::DataSink& sink) {
        if (stopRequested && stopRequested->load()) return false;
        std::ifstream input(file, std::ios::binary);
        if (!input) return false;
        input.seekg(static_cast<std::streamoff>(offset));
        std::vector<char> buffer(std::min<size_t>(length, 1024 * 1024));
        size_t remaining = length;
        while (remaining > 0 && input) {
            if (stopRequested && stopRequested->load()) return false;
            size_t wanted = std::min(buffer.size(), remaining);
            input.read(buffer.data(), static_cast<std::streamsize>(wanted));
            size_t read = static_cast<size_t>(input.gcount());
            if (!read) break;
            if (!sink.write(buffer.data(), read)) return false;
            remaining -= read;
        }
        return remaining == 0;
    };
    return client.Post(route, headers, total, provider, "application/octet-stream");
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
    std::vector<RenderJob> jobs;
    std::mutex callbackMutex;
    std::function<void()> changed;
    mutable std::mutex manualStatusMutex;
    std::wstring manualStatus;
    std::mutex workMutex;
    std::mutex pairingMutex;
    std::map<std::string, std::deque<std::chrono::steady_clock::time_point>> pairingAttempts;
    std::condition_variable workCv;
    std::queue<WorkItem> work;
    std::mutex currentTaskMutex;
    std::string currentTaskId;
    std::atomic<int> workerAssigned{0};
    std::atomic<bool> stopRequested{false};
    httplib::Server server;
    std::atomic<SOCKET> discoverySocket{INVALID_SOCKET};
    std::thread serverThread;
    std::thread discoveryThread;
    std::thread manualPeerThread;
    std::thread schedulerThread;
    std::thread workerThread;

    void Notify() {
        std::function<void()> callback;
        {
            std::scoped_lock lock(callbackMutex);
            callback = changed;
        }
        if (callback) callback();
    }

    fs::path JobsPath() const { return owner->DataDir() / L"jobs.json"; }

    std::string CurrentTaskId() {
        std::scoped_lock lock(currentTaskMutex);
        return currentTaskId;
    }

    void SetManualStatus(std::wstring value) {
        std::scoped_lock lock(manualStatusMutex);
        manualStatus = std::move(value);
    }

    void PersistJobs() {
        std::scoped_lock persistLock(persistMutex);
        json rows = json::array();
        {
            std::scoped_lock lock(stateMutex);
            for (const auto& job : jobs) {
                json frames = json::array();
                for (const auto& frame : job.frames) {
                    frames.push_back({
                        {"task_id", frame.taskId},
                        {"frame", frame.frame},
                        {"state", static_cast<int>(frame.state)},
                        {"assigned_node_id", frame.assignedNodeId},
                        {"assigned_node_name", frame.assignedNodeName},
                        {"dispatch_attempts", frame.dispatchAttempts},
                        {"attempt_token", frame.attemptToken},
                        {"output", WideToUtf8(frame.output.wstring())},
                        {"error", frame.error},
                    });
                }
                rows.push_back({
                    {"id", job.id}, {"name", job.name},
                    {"blend", WideToUtf8(job.preparedBlend.wstring())},
                    {"format", job.outputFormat}, {"extension", job.outputExtension},
                    {"version", job.blenderVersion},
                    {"render_engine", job.renderEngine},
                    {"cpu_safe_while_active", job.cpuSafeWhileActive},
                    {"requires_gpu", job.requiresGpu},
                    {"addons", job.requiredAddons}, {"frames", frames},
                });
            }
        }
        fs::create_directories(owner->DataDir());
        fs::path temporary = JobsPath();
        temporary += L".tmp";
        {
            std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
            output << rows.dump(2);
        }
        for (int attempt = 0; attempt < 5; ++attempt) {
            if (MoveFileExW(temporary.c_str(), JobsPath().c_str(),
                            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
                return;
            Sleep(static_cast<DWORD>((attempt + 1) * 20));
        }
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
                job.preparedBlend = Utf8ToWide(row.value("blend", ""));
                job.outputFormat = row.value("format", "PNG");
                job.outputExtension = row.value("extension", FrameExtension(job.outputFormat));
                job.blenderVersion = row.value("version", "");
                job.renderEngine = row.value("render_engine", "");
                job.cpuSafeWhileActive = row.value("cpu_safe_while_active", false);
                job.requiresGpu = row.value("requires_gpu", false);
                job.requiredAddons = row.value("addons", std::vector<std::string>{});
                for (const auto& value : row.value("frames", json::array())) {
                    FrameTask frame;
                    frame.taskId = value.value("task_id", "");
                    frame.frame = value.value("frame", 1);
                    frame.state = static_cast<TaskState>(value.value("state", 0));
                    if (frame.state == TaskState::Assigned || frame.state == TaskState::Dispatching ||
                        frame.state == TaskState::Rendering)
                        frame.state = TaskState::Pending;
                    frame.assignedNodeId = value.value("assigned_node_id", "");
                    frame.assignedNodeName = value.value("assigned_node_name", "");
                    frame.dispatchAttempts = value.value("dispatch_attempts", 0);
                    frame.attemptToken = value.value("attempt_token", "");
                    frame.output = Utf8ToWide(value.value("output", ""));
                    frame.error = value.value("error", "");
                    job.frames.push_back(std::move(frame));
                }
                if (!job.id.empty()) jobs.push_back(std::move(job));
            }
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
        if (body.is_discarded() || responseNode != serverNodeId || credential.size() < 32 ||
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
        server.Get("/health", [&](const httplib::Request&, httplib::Response& response) {
            Config config = owner->GetConfig();
            response.set_content(
                json{{"status", "ok"}, {"node_id", config.nodeId},
                     {"name", config.nodeName}, {"share", config.shareEnabled},
                     {"assigned", workerAssigned.load()}, {"cpu", config.activeCpuPercent},
                     {"cpu_model", hardware.cpuModel}, {"gpu_models", hardware.gpuModels},
                     {"pairing_available", !config.pairingPending},
                     {"task_id", CurrentTaskId()}, {"idle", static_cast<int>(IdleSeconds())}}
                    .dump(),
                "application/json");
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
            if (config.pairingPending || config.clusterToken.size() < 32 ||
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
        server.Get("/v1/prove", [&](const httplib::Request& request,
                                     httplib::Response& response) {
            std::string challenge = request.has_param("challenge")
                ? request.get_param_value("challenge") : "";
            if (!SafeId(challenge) || challenge.size() < 16) {
                response.status = 422;
                return;
            }
            Config config = owner->GetConfig();
            if (config.pairingPending || config.clusterToken.size() < 32) {
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
                if (config.pairingPending || config.clusterToken.size() < 32 ||
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
                int frame = HeaderInt(request, "X-RM-Frame", 0);
                int originPort = HeaderInt(request, "X-RM-Origin-Port", 0);
                std::string format = request.get_header_value("X-RM-Format");
                std::string extension = request.get_header_value("X-RM-Extension");
                if (extension.empty()) extension = FrameExtension(format);
                std::string version = request.get_header_value("X-RM-Blender");
                std::string renderEngine = request.get_header_value("X-RM-Engine");
                bool cpuSafeWhileActive = request.get_header_value("X-RM-CPU-Safe") == "1";
                bool requiresGpu = request.get_header_value("X-RM-Requires-GPU") == "1";
                std::string attemptToken = request.get_header_value("X-RM-Attempt");
                auto addons = HeaderStringList(request, "X-RM-Addons");
                if (!SafeId(taskId) || !SafeId(jobId) || frame < 1 || frame > 1000000 || originPort < 1024 ||
                    originPort > 65535 ||
                    !SafeId(attemptToken) || format.empty() ||
                    !IsSafeOutputExtension(extension)) {
                    response.status = 422;
                    response.set_content("invalid task metadata", "text/plain");
                    return;
                }
                if (!cpuSafeWhileActive && IdleSeconds() < config.idleAfterSeconds) {
                    response.status = 425;
                    response.set_content("GPU or unknown renderer waits until this computer is idle", "text/plain");
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
                if (!StreamRequestToFile(reader, blend, receiveError)) {
                    response.status = 500;
                    response.set_content(WideToUtf8(receiveError), "text/plain");
                    return;
                }
                if (!cpuSafeWhileActive && IdleSeconds() < config.idleAfterSeconds) {
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
                if (!owner->mockRenderer_ && !cpuSafeWhileActive) {
                    effectiveStop = &compatibilityStop;
                    compatibilityGuard = std::thread([&] {
                        while (!compatibilityStop.load()) {
                            if (stopRequested.load()) {
                                compatibilityStop = true;
                                break;
                            }
                            if (IdleSeconds() < config.idleAfterSeconds) {
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
                        config, owner->DataDir(), blend, version, renderEngine, requiresGpu, addons,
                        *effectiveStop, compatibility);
                compatibilityStop = true;
                if (compatibilityGuard.joinable()) compatibilityGuard.join();
                if (!compatible) {
                    if (resumedInput.load())
                        compatibility = L"电脑恢复使用，已中止 GPU/未知引擎兼容性检查";
                    response.status = resumedInput.load() ? 425 : 409;
                    response.set_content(WideToUtf8(compatibility), "text/plain; charset=utf-8");
                    std::error_code ignored;
                    fs::remove_all(taskDir, ignored);
                    return;
                }
                WorkItem item;
                item.taskId = taskId;
                item.jobId = jobId;
                item.frame = frame;
                item.blend = blend;
                item.outputDir = taskDir / L"outputs";
                item.outputFormat = format;
                item.outputExtension = extension;
                item.blenderVersion = version;
                item.renderEngine = renderEngine;
                item.cpuSafeWhileActive = cpuSafeWhileActive;
                item.requiresGpu = requiresGpu;
                item.requiredAddons = std::move(addons);
                item.attemptToken = attemptToken;
                item.remote = true;
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
                if (config.pairingPending || config.clusterToken.size() < 32 ||
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
                fs::path destination;
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
                        destination = owner->DataDir() / L"jobs" / Utf8ToWide(job.id) /
                            L"outputs" /
                            (L"frame_" + std::to_wstring(it->frame) +
                             Utf8ToWide(job.outputExtension));
                        break;
                    }
                }
                if (!found) {
                    response.status = 404;
                    return;
                }
                if (status == "completed" && !HasTransferCapacity(request, destination.parent_path())) {
                    response.status = 507;
                    response.set_content("result exceeds transfer limit or available disk space", "text/plain");
                    return;
                }
                fs::path stagedResult;
                if (status == "completed") {
                    stagedResult = destination;
                    stagedResult += Utf8ToWide("." + attemptToken + ".result");
                }
                std::wstring receiveError;
                bool ok = status != "completed" ||
                    StreamRequestToFile(reader, stagedResult, receiveError);
                {
                    std::scoped_lock lock(stateMutex);
                    if (targetJob >= jobs.size() || targetFrame >= jobs[targetJob].frames.size() ||
                        jobs[targetJob].frames[targetFrame].taskId != taskId) {
                        std::error_code ignored;
                        if (!stagedResult.empty()) fs::remove(stagedResult, ignored);
                        response.status = 409;
                        response.set_content("task changed while receiving result", "text/plain");
                        return;
                    }
                    auto& target = jobs[targetJob].frames[targetFrame];
                    if (target.attemptToken != attemptToken || target.state != TaskState::Assigned) {
                        std::error_code ignored;
                        if (!stagedResult.empty()) fs::remove(stagedResult, ignored);
                        response.status = 409;
                        response.set_content("assignment changed while receiving result", "text/plain");
                        return;
                    }
                    if (status == "completed" && ok &&
                        !MoveFileExW(stagedResult.c_str(), destination.c_str(),
                                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
                        ok = false;
                        receiveError = LastErrorMessage();
                        std::error_code ignored;
                        fs::remove(stagedResult, ignored);
                    }
                    if (status == "completed" && ok) {
                        target.state = TaskState::Completed;
                        target.output = destination;
                        target.error.clear();
                    } else if (status == "retry" && ok) {
                        target.state = TaskState::Pending;
                        target.error = request.get_header_value("X-RM-Error");
                        target.assignedNodeId.clear();
                        target.assignedNodeName.clear();
                        target.attemptToken.clear();
                    } else {
                        target.state = TaskState::Failed;
                        target.error = request.get_header_value("X-RM-Error");
                        if (!receiveError.empty()) target.error = WideToUtf8(receiveError);
                    }
                    target.assignedAt = {};
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
            if (!endpoint.empty()) {
                Peer peer;
                std::wstring status;
                if (ProbeManualPeer(endpoint, peer, status)) {
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
            for (int step = 0; step < 20 && !stopRequested.load(); ++step) Sleep(100);
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
                if (!config.pairingPending && config.clusterToken.size() >= 32) {
                    json message = {
                        {"protocol", 2}, {"node_id", config.nodeId}, {"name", config.nodeName},
                        {"nonce", NewRandomHex(8)}, {"addresses", localAddresses},
                        {"port", config.httpPort}, {"share", config.shareEnabled},
                        {"cpu", config.activeCpuPercent}, {"assigned", workerAssigned.load()},
                        {"cpu_model", hardware.cpuModel}, {"gpu_models", hardware.gpuModels},
                        {"task_id", CurrentTaskId()},
                        {"idle", static_cast<int>(IdleSeconds())},
                    };
                    message["signature"] = DiscoverySignature(config, message);
                    payloads.push_back(message.dump());
                    json pairing = {
                        {"protocol", 2}, {"kind", "pairing"}, {"node_id", config.nodeId},
                        {"name", config.nodeName}, {"nonce", NewRandomHex(8)},
                        {"addresses", localAddresses}, {"port", config.httpPort},
                    };
                    pairing["proof"] = PairingBeaconProof(config.inviteCode, pairing);
                    payloads.push_back(pairing.dump());
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
                if (protocol != 1 && protocol != 2) continue;
                char remoteAddress[INET_ADDRSTRLEN]{};
                inet_ntop(AF_INET, &remote.sin_addr, remoteAddress, sizeof(remoteAddress));
                if (protocol == 2 && message.value("kind", "") == "pairing") {
                    Config pending = owner->GetConfig();
                    std::string serverNodeId = message.value("node_id", "");
                    int serverPort = message.value("port", 0);
                    auto addresses = message.value("addresses", std::vector<std::string>{});
                    bool valid = pending.pairingPending && SafeId(serverNodeId) &&
                        serverNodeId != pending.nodeId && serverPort >= 1024 && serverPort <= 65535 &&
                        std::find(addresses.begin(), addresses.end(), std::string(remoteAddress)) !=
                            addresses.end() &&
                        ConstantTimeEqual(message.value("proof", ""),
                                          PairingBeaconProof(pending.inviteCode, message));
                    if (valid) {
                        std::wstring pairingStatus;
                        if (TryPair(remoteAddress, serverPort, serverNodeId, pairingStatus))
                            SetManualStatus(std::move(pairingStatus));
                        Notify();
                    }
                    continue;
                }
                Peer peer;
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

    std::optional<Peer> AvailablePeer() {
        std::scoped_lock lock(stateMutex);
        auto now = std::chrono::steady_clock::now();
        for (const auto& [id, peer] : peers) {
            if (now - peer.lastSeen <= kPeerTimeout && peer.trusted && peer.shareEnabled &&
                peer.assigned == 0)
                return peer;
        }
        return std::nullopt;
    }

    bool DispatchRemote(const Peer& peer, const RenderJob& job, const FrameTask& frame,
                        const std::string& attemptToken,
                        std::string& error) {
        Config config = owner->GetConfig();
        std::string challenge = NewRandomHex(16);
        httplib::Client verifier(peer.address, peer.httpPort);
        verifier.set_connection_timeout(3, 0);
        auto proofResponse = verifier.Get("/v1/prove?challenge=" + challenge);
        if (!proofResponse || proofResponse->status != 200) {
            error = "worker failed fresh ownership proof";
            return false;
        }
        json proof = json::parse(proofResponse->body, nullptr, false);
        std::string expectedProof = HmacSha256(config.clusterToken,
            "worker-proof|" + peer.nodeId + "|" + challenge);
        if (proof.is_discarded() || proof.value("node_id", "") != peer.nodeId ||
            !ConstantTimeEqual(proof.value("proof", ""), expectedProof)) {
            error = "worker ownership proof was invalid";
            return false;
        }
        httplib::Headers headers = {
            {"X-RM-Token", config.clusterToken}, {"X-RM-Task", frame.taskId},
            {"X-RM-Job", job.id}, {"X-RM-Frame", std::to_string(frame.frame)},
            {"X-RM-Format", job.outputFormat}, {"X-RM-Blender", job.blenderVersion},
            {"X-RM-Extension", job.outputExtension},
            {"X-RM-Engine", job.renderEngine},
            {"X-RM-CPU-Safe", job.cpuSafeWhileActive ? "1" : "0"},
            {"X-RM-Requires-GPU", job.requiresGpu ? "1" : "0"},
            {"X-RM-Attempt", attemptToken},
            {"X-RM-Addons", json(job.requiredAddons).dump()},
            {"X-RM-Origin-Port", std::to_string(config.httpPort)},
        };
        auto result = PostFile(peer.address, peer.httpPort, "/v1/task", headers,
                               job.preparedBlend, &stopRequested);
        if (!result) {
            error = "network error: " + httplib::to_string(result.error());
            return false;
        }
        if (result->status != 202) {
            error = result->body.empty() ? "worker rejected task" : result->body;
            LogWarning("worker rejected task " + frame.taskId + "; node=" + peer.nodeId);
            return false;
        }
        LogInfo("dispatched task " + frame.taskId + "; node=" + peer.nodeId);
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
                    PeerExplicitlyStoppedTask(peer->second, frame.taskId);
                if (!peerGone && !peerNoLongerWorking) continue;
                frame.state = TaskState::Pending;
                frame.error = peerGone ? "worker went offline; frame requeued"
                                       : "worker stopped without returning a result; frame requeued";
                frame.assignedNodeId.clear();
                frame.assignedNodeName.clear();
                frame.assignedAt = {};
                frame.attemptToken.clear();
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
            size_t jobIndex = 0, frameIndex = 0;
            RenderJob jobCopy;
            FrameTask frameCopy;
            bool found = false;
            {
                std::scoped_lock lock(stateMutex);
                for (size_t j = 0; j < jobs.size() && !found; ++j) {
                    for (size_t f = 0; f < jobs[j].frames.size(); ++f) {
                        if (jobs[j].frames[f].state == TaskState::Pending) {
                            jobIndex = j;
                            frameIndex = f;
                            jobs[j].frames[f].state = TaskState::Dispatching;
                            jobCopy = jobs[j];
                            frameCopy = jobs[j].frames[f];
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
            bool handled = false;
            auto peer = AvailablePeer();
            if (peer) {
                std::string dispatchError;
                std::string attemptToken = NewRandomHex(12);
                {
                    std::scoped_lock lock(stateMutex);
                    if (jobIndex < jobs.size() && frameIndex < jobs[jobIndex].frames.size()) {
                        auto& frame = jobs[jobIndex].frames[frameIndex];
                        // Publish the lease before uploading. A very fast worker can
                        // return its result before POST /v1/task itself returns.
                        frame.state = TaskState::Assigned;
                        frame.assignedNodeId = peer->nodeId;
                        frame.assignedNodeName = peer->name;
                        frame.assignedAt = {};
                        frame.attemptToken = attemptToken;
                        ++frame.dispatchAttempts;
                    }
                }
                if (DispatchRemote(*peer, jobCopy, frameCopy, attemptToken, dispatchError)) {
                    std::scoped_lock lock(stateMutex);
                    if (jobIndex < jobs.size() && frameIndex < jobs[jobIndex].frames.size()) {
                        auto& frame = jobs[jobIndex].frames[frameIndex];
                        if (frame.state == TaskState::Assigned &&
                            frame.attemptToken == attemptToken) {
                            frame.assignedAt = std::chrono::steady_clock::now();
                            frame.error.clear();
                        }
                    }
                    handled = true;
                } else {
                    std::scoped_lock lock(stateMutex);
                    if (jobIndex < jobs.size() && frameIndex < jobs[jobIndex].frames.size()) {
                        auto& frame = jobs[jobIndex].frames[frameIndex];
                        if (frame.state == TaskState::Assigned &&
                            frame.attemptToken == attemptToken) {
                            frame.state = TaskState::Dispatching;
                            frame.assignedNodeId.clear();
                            frame.assignedNodeName.clear();
                            frame.assignedAt = {};
                            frame.attemptToken.clear();
                            frame.error = dispatchError;
                        } else {
                            // The worker may have completed or requested a retry
                            // before the task-upload client observed its 202 reply.
                            // Never enqueue a duplicate local execution in that case.
                            handled = true;
                        }
                    }
                }
            }
            int expectedAssigned = 0;
            if (!handled && workerAssigned.compare_exchange_strong(expectedAssigned, 1)) {
                WorkItem item;
                item.taskId = frameCopy.taskId;
                item.jobId = jobCopy.id;
                item.frame = frameCopy.frame;
                item.blend = jobCopy.preparedBlend;
                item.outputDir = owner->DataDir() / L"jobs" / Utf8ToWide(jobCopy.id) / L"outputs";
                item.outputFormat = jobCopy.outputFormat;
                item.outputExtension = jobCopy.outputExtension;
                item.blenderVersion = jobCopy.blenderVersion;
                item.renderEngine = jobCopy.renderEngine;
                item.cpuSafeWhileActive = jobCopy.cpuSafeWhileActive;
                item.requiresGpu = jobCopy.requiresGpu;
                item.requiredAddons = jobCopy.requiredAddons;
                {
                    std::scoped_lock workLock(workMutex);
                    work.push(item);
                }
                {
                    std::scoped_lock lock(stateMutex);
                    auto& frame = jobs[jobIndex].frames[frameIndex];
                    frame.state = TaskState::Rendering;
                    Config config = owner->GetConfig();
                    frame.assignedNodeId = config.nodeId;
                    frame.assignedNodeName = config.nodeName;
                    frame.assignedAt = {};
                    frame.attemptToken.clear();
                }
                workCv.notify_one();
                handled = true;
            }
            if (!handled) {
                {
                    std::scoped_lock lock(stateMutex);
                    if (jobIndex < jobs.size() && frameIndex < jobs[jobIndex].frames.size()) {
                        auto& frame = jobs[jobIndex].frames[frameIndex];
                        frame.state = TaskState::Pending;
                        frame.assignedNodeId.clear();
                        frame.assignedNodeName.clear();
                        frame.assignedAt = {};
                        frame.attemptToken.clear();
                    }
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(250));
            }
            PersistJobs();
            Notify();
        }
    }

    bool SendRemoteResult(const WorkItem& item, bool success, bool retry,
                          const fs::path& output, const std::wstring& renderError) {
        Config config = owner->GetConfig();
        httplib::Headers headers = {
            {"X-RM-Token", config.clusterToken}, {"X-RM-Task", item.taskId},
            {"X-RM-Attempt", item.attemptToken},
            {"X-RM-Status", retry ? "retry" : success ? "completed" : "failed"},
            {"X-RM-Error", WideToUtf8(renderError)},
        };
        for (int attempt = 0; attempt < 4 && !stopRequested.load(); ++attempt) {
            if (attempt) std::this_thread::sleep_for(std::chrono::seconds(attempt * 2));
            httplib::Result result;
            if (success && !retry) {
                result = PostFile(item.originAddress, item.originPort, "/v1/result", headers,
                                  output, &stopRequested);
            } else {
                httplib::Client client(item.originAddress, item.originPort);
                client.set_connection_timeout(3, 0);
                result = client.Post("/v1/result", headers, "", "application/octet-stream");
            }
            if (result && result->status == 200) return true;
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
            {
                std::scoped_lock lock(currentTaskMutex);
                currentTaskId = item.taskId;
            }
            LogInfo("render started; task=" + item.taskId + "; frame=" +
                    std::to_string(item.frame));
            fs::path output;
            std::wstring renderError;
            bool success = false;
            bool retryAfterIdle = false;
            Config renderConfig = owner->GetConfig();
            if (item.remote && !item.cpuSafeWhileActive &&
                IdleSeconds() < renderConfig.idleAfterSeconds) {
                renderError = L"本任务可能使用 GPU；电脑恢复使用后已停止，将在空闲时重新分配";
                retryAfterIdle = true;
            } else if (owner->mockRenderer_) {
                fs::create_directories(item.outputDir);
                output = item.outputDir /
                    (L"frame_" + std::to_wstring(item.frame) +
                     Utf8ToWide(item.outputExtension));
                std::ofstream mock(output, std::ios::binary);
                mock << "RenderMesh mock frame " << item.frame;
                success = static_cast<bool>(mock);
            } else {
                std::atomic<bool> policyStop = false;
                std::atomic<bool> resumedInput = false;
                std::atomic<bool>* effectiveStop = &stopRequested;
                std::thread inputGuard;
                if (item.remote && !item.cpuSafeWhileActive) {
                    effectiveStop = &policyStop;
                    inputGuard = std::thread([&] {
                        while (!policyStop.load()) {
                            if (stopRequested.load()) {
                                policyStop = true;
                                break;
                            }
                            if (IdleSeconds() < renderConfig.idleAfterSeconds) {
                                resumedInput = true;
                                policyStop = true;
                                break;
                            }
                            Sleep(250);
                        }
                    });
                }
                success = RenderBlenderFrame(renderConfig, item.blend, item.outputDir,
                                             item.frame, item.outputFormat, item.outputExtension,
                                             item.remote, *effectiveStop, output, renderError);
                policyStop = true;
                if (inputGuard.joinable()) inputGuard.join();
                retryAfterIdle = resumedInput.load();
            }
            if (item.remote) {
                bool delivered = SendRemoteResult(item, success, retryAfterIdle, output, renderError);
                if (!delivered) {
                    std::error_code ignored;
                    fs::path failures = owner->DataDir() / L"failed-results";
                    fs::create_directories(failures, ignored);
                    std::ofstream record(failures / (Utf8ToWide(item.taskId) + L".txt"),
                                         std::ios::binary | std::ios::trunc);
                    record << "origin=" << item.originAddress << ':' << item.originPort << '\n'
                           << "attempt=" << item.attemptToken << '\n'
                           << "status=" << (retryAfterIdle ? "retry" : success ? "completed" : "failed") << '\n'
                           << "error=" << WideToUtf8(renderError) << '\n';
                }
                // The origin observes the worker's current task ID and requeues a
                // lost delivery. Keep only a small diagnostic record, never an
                // unbounded copy of the received project and frame output.
                std::error_code ignored;
                fs::remove_all(item.blend.parent_path(), ignored);
            } else {
                std::scoped_lock lock(stateMutex);
                for (auto& job : jobs) {
                    auto frame = std::find_if(job.frames.begin(), job.frames.end(),
                                              [&](const FrameTask& candidate) {
                                                  return candidate.taskId == item.taskId;
                                              });
                    if (frame == job.frames.end()) continue;
                    frame->state = success ? TaskState::Completed : TaskState::Failed;
                    frame->output = output;
                    frame->error = WideToUtf8(renderError);
                    break;
                }
            }
            {
                std::scoped_lock lock(currentTaskMutex);
                currentTaskId.clear();
            }
            --workerAssigned;
            if (success)
                LogInfo("render completed; task=" + item.taskId + "; frame=" +
                        std::to_string(item.frame));
            else if (retryAfterIdle)
                LogInfo("render deferred after user activity; task=" + item.taskId);
            else
                LogError("render failed; task=" + item.taskId + "; error=" +
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
        (!config.pairingPending && config.clusterToken.size() < 32) ||
        config.activeCpuPercent < 1 || config.activeCpuPercent > 100 ||
        config.idleAfterSeconds < 1 || config.idleAfterSeconds > 86400) {
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

HardwareInfo NodeApp::Hardware() const { return impl_->hardware; }

int NodeApp::AssignedCount() const {
    return impl_->workerAssigned.load();
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
    if (name.empty() || frameStart < 1 || frameEnd < frameStart || frameEnd > 1000000 ||
        frameStep < 1 || report.outputFormat.empty() ||
        !IsSafeOutputExtension(report.outputExtension)) {
        error = L"任务参数无效";
        return {};
    }
    std::string id = NewRandomHex(8);
    fs::path jobDir = DataDir() / L"jobs" / Utf8ToWide(id);
    fs::create_directories(jobDir / L"outputs");
    fs::path source = jobDir / L"source.blend";
    std::error_code ec;
    fs::copy_file(report.preparedBlend, source, fs::copy_options::overwrite_existing, ec);
    if (ec) {
        error = L"无法复制已打包工程：" + Utf8ToWide(ec.message());
        return {};
    }
    RenderJob job;
    job.id = id;
    job.name = std::move(name);
    job.preparedBlend = source;
    job.outputFormat = report.outputFormat;
    job.outputExtension = report.outputExtension;
    job.blenderVersion = report.blenderVersion;
    job.renderEngine = report.renderEngine;
    job.cpuSafeWhileActive = report.cpuSafeWhileActive;
    job.requiresGpu = report.requiresGpu;
    job.requiredAddons = report.addons;
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
        fs::path root = fs::temp_directory_path() / (L"RenderMesh-SelfTest-" +
            Utf8ToWide(NewRandomHex(4)));
        fs::create_directories(root);
        ConfigStore store(root);
        Config config = store.Load();
        if (!IsInviteCode(config.inviteCode) || config.clusterToken.size() < 32) {
            report = L"Default invite-code or internal credential generation failed";
            return false;
        }
        config.nodeName = "SELFTEST";
        config.activeCpuPercent = 17;
        config.idleAfterSeconds = 7;
        config.inviteCode = "042731";
        config.manualPeerEndpoint = "192.168.28.28:8391";
        std::wstring error;
        Config persisted;
        if (!store.Save(config, error) ||
            (persisted = store.Load()).activeCpuPercent != 17 ||
            persisted.idleAfterSeconds != 7 ||
            persisted.inviteCode != "042731" ||
            persisted.clusterToken != config.clusterToken ||
            persisted.manualPeerEndpoint != config.manualPeerEndpoint) {
            report = L"配置持久化失败：" + error;
            return false;
        }
        if (!IsInviteCode("000000") || !IsInviteCode("999999") ||
            IsInviteCode("12345") || IsInviteCode("1234567") ||
            IsInviteCode("12A456")) {
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
        server.Post("/stream", [&](const httplib::Request&, httplib::Response& response,
                                   const httplib::ContentReader& reader) {
            std::wstring streamError;
            bool ok = StreamRequestToFile(reader, received, streamError);
            response.status = ok ? 200 : 500;
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
        server.stop();
        thread.join();
        if (!result || result->status != 200 || fs::file_size(received) != fs::file_size(source)) {
            report = L"流式任务传输失败";
            return false;
        }
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION process{};
        std::wstring command = L"cmd.exe /c ping 127.0.0.1 -n 2 > nul";
        std::vector<wchar_t> buffer(command.begin(), command.end());
        buffer.push_back(0);
        if (!CreateProcessW(nullptr, buffer.data(), nullptr, nullptr, FALSE,
                            CREATE_SUSPENDED | CREATE_NO_WINDOW, nullptr, nullptr,
                            &startup, &process)) {
            report = L"测试子进程启动失败";
            return false;
        }
        WindowsJobLimiter limiter;
        bool limited = limiter.Assign(process.hProcess, error) && limiter.SetCpuPercent(15, error) &&
            limiter.SetCpuPercent(100, error);
        ResumeThread(process.hThread);
        WaitForSingleObject(process.hProcess, 5000);
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);

        if (limited) {
            fs::path originRoot = root / L"origin";
            fs::path workerRoot = root / L"worker";
            ConfigStore originStore(originRoot);
            ConfigStore workerStore(workerRoot);
            Config originConfig = originStore.Load();
            Config workerConfig = workerStore.Load();
            std::string sharedToken = NewRandomHex(16);
            originConfig.nodeName = "SELFTEST-ORIGIN";
            originConfig.nodeId = "selftest-origin";
            originConfig.inviteCode = "123456";
            originConfig.clusterToken = NewRandomHex(16);
            originConfig.httpPort = 18531;
            originConfig.discoveryPort = 18530;
            workerConfig.nodeName = "SELFTEST-WORKER";
            workerConfig.nodeId = "selftest-worker";
            workerConfig.inviteCode = "654321";
            workerConfig.clusterToken = sharedToken;
            workerConfig.httpPort = 18532;
            workerConfig.discoveryPort = 18533;
            workerConfig.shareEnabled = true;

            NodeApp origin(std::move(originStore), originConfig, true,
                           std::chrono::milliseconds(600));
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
                preflight.preparedBlend = prepared;
                preflight.blenderVersion = "4.5";
                preflight.renderEngine = "CYCLES";
                preflight.outputFormat = "PNG";
                preflight.outputExtension = ".png";
                preflight.cpuSafeWhileActive = true;
                std::wstring submitError;
                std::string jobId = origin.SubmitPrepared(
                    "two-node-self-test", preflight, 1, 3, 1, submitError);
                bool clusterOk = malformedRejected && pairingStarted && paired &&
                    manuallyConnected && hardwareReported && !jobId.empty();
                bool usedRemote = false;
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
                            fs::exists(frame.output);
                }
                if (clusterOk) {
                    worker.Stop();
                    origin.RemovePeerForTest(workerConfig.nodeId);
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
                    std::string recoveryJob = origin.SubmitPrepared(
                        "lost-worker-self-test", preflight, 4, 4, 1, submitError);
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
                if (!clusterOk) error = L"two-node HTTP dispatch/result test failed";
                limited = limited && clusterOk;
            } else {
                limited = false;
                error = startError;
            }
            origin.Stop();
            worker.Stop();
        }
        std::error_code ignored;
        fs::remove_all(root, ignored);
        if (!limited) {
            report = L"Windows CPU Job Object 测试失败：" + error;
            return false;
        }
        report = L"SELF_TEST_OK：配置、2 MiB 流式传输、CPU 15%→100%、双节点分发和掉线重派均通过";
        return true;
    } catch (const std::exception& exception) {
        report = L"自检异常：" + Utf8ToWide(exception.what());
        return false;
    }
}

}  // namespace rm
