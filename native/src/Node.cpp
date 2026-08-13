#include <winsock2.h>
#include <ws2tcpip.h>

#include "Core.hpp"
#include "../third_party/httplib.h"

#include <algorithm>
#include <fstream>
#include <regex>

namespace rm {

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

httplib::Result PostFile(
    const std::string& host,
    int port,
    const std::string& route,
    const httplib::Headers& headers,
    const fs::path& file) {
    std::error_code ec;
    size_t total = static_cast<size_t>(fs::file_size(file, ec));
    httplib::Client client(host, port);
    client.set_connection_timeout(3, 0);
    client.set_read_timeout(600, 0);
    client.set_write_timeout(600, 0);
    auto provider = [file](size_t offset, size_t length, httplib::DataSink& sink) {
        std::ifstream input(file, std::ios::binary);
        if (!input) return false;
        input.seekg(static_cast<std::streamoff>(offset));
        std::vector<char> buffer(std::min<size_t>(length, 1024 * 1024));
        size_t remaining = length;
        while (remaining > 0 && input) {
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

struct NodeApp::Impl {
    explicit Impl(NodeApp* app, std::chrono::milliseconds grace)
        : owner(app), assignmentGrace(grace) {}

    NodeApp* owner;
    std::chrono::milliseconds assignmentGrace;
    mutable std::mutex stateMutex;
    std::mutex persistMutex;
    std::map<std::string, Peer> peers;
    std::vector<RenderJob> jobs;
    std::mutex callbackMutex;
    std::function<void()> changed;
    std::mutex workMutex;
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
                        {"output", WideToUtf8(frame.output.wstring())},
                        {"error", frame.error},
                    });
                }
                rows.push_back({
                    {"id", job.id}, {"name", job.name},
                    {"blend", WideToUtf8(job.preparedBlend.wstring())},
                    {"format", job.outputFormat}, {"version", job.blenderVersion},
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
                job.blenderVersion = row.value("version", "");
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
                    frame.output = Utf8ToWide(value.value("output", ""));
                    frame.error = value.value("error", "");
                    job.frames.push_back(std::move(frame));
                }
                if (!job.id.empty()) jobs.push_back(std::move(job));
            }
        } catch (...) {
        }
    }

    void ConfigureServer() {
        server.set_payload_max_length(100ull * 1024 * 1024 * 1024);
        server.Get("/health", [&](const httplib::Request&, httplib::Response& response) {
            Config config = owner->GetConfig();
            response.set_content(
                json{{"status", "ok"}, {"node_id", config.nodeId},
                     {"share", config.shareEnabled}, {"assigned", workerAssigned.load()}}
                    .dump(),
                "application/json");
        });
        server.Post(
            "/v1/task",
            [&](const httplib::Request& request, httplib::Response& response,
                const httplib::ContentReader& reader) {
                Config config = owner->GetConfig();
                if (!ConstantTimeEqual(request.get_header_value("X-RM-Token"),
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
                std::string version = request.get_header_value("X-RM-Blender");
                auto addons = HeaderStringList(request, "X-RM-Addons");
                if (!SafeId(taskId) || !SafeId(jobId) || frame < 1 || frame > 1000000 || originPort < 1024 ||
                    originPort > 65535 ||
                    (format != "PNG" && format != "JPEG" && format != "OPEN_EXR")) {
                    response.status = 422;
                    response.set_content("invalid task metadata", "text/plain");
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
                std::wstring compatibility;
                if (!owner->mockRenderer_ && !CheckWorkerCompatibility(
                        config, owner->DataDir(), blend, version, addons, compatibility)) {
                    response.status = 409;
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
                item.blenderVersion = version;
                item.requiredAddons = std::move(addons);
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
            });
        server.Post(
            "/v1/result",
            [&](const httplib::Request& request, httplib::Response& response,
                const httplib::ContentReader& reader) {
                Config config = owner->GetConfig();
                if (!ConstantTimeEqual(request.get_header_value("X-RM-Token"),
                                       config.clusterToken)) {
                    response.status = 401;
                    return;
                }
                std::string taskId = request.get_header_value("X-RM-Task");
                std::string status = request.get_header_value("X-RM-Status");
                if (!SafeId(taskId) || (status != "completed" && status != "failed")) {
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
                        found = true;
                        destination = owner->DataDir() / L"jobs" / Utf8ToWide(job.id) /
                            L"outputs" /
                            (L"frame_" + std::to_wstring(it->frame) +
                             Utf8ToWide(FrameExtension(job.outputFormat)));
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
                std::wstring receiveError;
                bool ok = status == "failed" || StreamRequestToFile(reader, destination, receiveError);
                {
                    std::scoped_lock lock(stateMutex);
                    if (targetJob >= jobs.size() || targetFrame >= jobs[targetJob].frames.size() ||
                        jobs[targetJob].frames[targetFrame].taskId != taskId) {
                        response.status = 409;
                        response.set_content("task changed while receiving result", "text/plain");
                        return;
                    }
                    auto& target = jobs[targetJob].frames[targetFrame];
                    if (status == "completed" && ok) {
                        target.state = TaskState::Completed;
                        target.output = destination;
                        target.error.clear();
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
            });
    }

    void DiscoveryLoop() {
        Config initial = owner->GetConfig();
        SOCKET activeSocket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (activeSocket == INVALID_SOCKET) return;
        discoverySocket.store(activeSocket);
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
            SOCKET expected = activeSocket;
            if (discoverySocket.compare_exchange_strong(expected, INVALID_SOCKET))
                closesocket(activeSocket);
            return;
        }

        auto nextBroadcast = std::chrono::steady_clock::now();
        while (!stopRequested.load()) {
            Config config = owner->GetConfig();
            if (std::chrono::steady_clock::now() >= nextBroadcast) {
                json message = {
                    {"protocol", 1}, {"node_id", config.nodeId}, {"name", config.nodeName},
                    {"fingerprint", Sha256Prefix(config.clusterToken)},
                    {"port", config.httpPort}, {"share", config.shareEnabled},
                    {"cpu", config.activeCpuPercent}, {"assigned", workerAssigned.load()},
                    {"task_id", CurrentTaskId()},
                    {"idle", static_cast<int>(IdleSeconds())},
                };
                std::string payload = message.dump();
                sockaddr_in broadcast{};
                broadcast.sin_family = AF_INET;
                broadcast.sin_addr.s_addr = INADDR_BROADCAST;
                broadcast.sin_port = htons(static_cast<u_short>(config.discoveryPort));
                sendto(activeSocket, payload.data(), static_cast<int>(payload.size()), 0,
                       reinterpret_cast<sockaddr*>(&broadcast), sizeof(broadcast));
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
                if (message.value("protocol", 0) != 1) continue;
                Peer peer;
                peer.nodeId = message.value("node_id", "");
                if (!SafeId(peer.nodeId) || peer.nodeId == config.nodeId) continue;
                char address[INET_ADDRSTRLEN]{};
                inet_ntop(AF_INET, &remote.sin_addr, address, sizeof(address));
                peer.address = address;
                peer.name = message.value("name", peer.nodeId);
                peer.httpPort = message.value("port", 8391);
                peer.shareEnabled = message.value("share", false);
                peer.cpuPercent = message.value("cpu", 15);
                peer.assigned = message.value("assigned", 0);
                peer.currentTaskId = message.value("task_id", "");
                peer.idleSeconds = message.value("idle", 0.0);
                peer.trusted = message.value("fingerprint", "") ==
                    Sha256Prefix(config.clusterToken);
                peer.lastSeen = std::chrono::steady_clock::now();
                {
                    std::scoped_lock lock(stateMutex);
                    peers[peer.nodeId] = std::move(peer);
                }
                Notify();
            } catch (...) {
            }
        }
        SOCKET expected = activeSocket;
        if (discoverySocket.compare_exchange_strong(expected, INVALID_SOCKET))
            closesocket(activeSocket);
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
                        std::string& error) {
        Config config = owner->GetConfig();
        httplib::Headers headers = {
            {"X-RM-Token", config.clusterToken}, {"X-RM-Task", frame.taskId},
            {"X-RM-Job", job.id}, {"X-RM-Frame", std::to_string(frame.frame)},
            {"X-RM-Format", job.outputFormat}, {"X-RM-Blender", job.blenderVersion},
            {"X-RM-Addons", json(job.requiredAddons).dump()},
            {"X-RM-Origin-Port", std::to_string(config.httpPort)},
        };
        auto result = PostFile(peer.address, peer.httpPort, "/v1/task", headers,
                               job.preparedBlend);
        if (!result) {
            error = "network error: " + httplib::to_string(result.error());
            return false;
        }
        if (result->status != 202) {
            error = result->body.empty() ? "worker rejected task" : result->body;
            return false;
        }
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
                    peer->second.currentTaskId != frame.taskId;
                if (!peerGone && !peerNoLongerWorking) continue;
                frame.state = TaskState::Pending;
                frame.error = peerGone ? "worker went offline; frame requeued"
                                       : "worker stopped without returning a result; frame requeued";
                frame.assignedNodeId.clear();
                frame.assignedNodeName.clear();
                frame.assignedAt = {};
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
                if (DispatchRemote(*peer, jobCopy, frameCopy, dispatchError)) {
                    std::scoped_lock lock(stateMutex);
                    if (jobIndex < jobs.size() && frameIndex < jobs[jobIndex].frames.size()) {
                        auto& frame = jobs[jobIndex].frames[frameIndex];
                        // A fast worker can return the result before POST /v1/task finishes.
                        // Do not regress that completed state back to Assigned.
                        if (frame.state == TaskState::Dispatching)
                            frame.state = TaskState::Assigned;
                        frame.assignedNodeId = peer->nodeId;
                        frame.assignedNodeName = peer->name;
                        if (frame.state == TaskState::Assigned) {
                            frame.assignedAt = std::chrono::steady_clock::now();
                            ++frame.dispatchAttempts;
                        }
                        if (frame.state != TaskState::Failed) frame.error.clear();
                    }
                    handled = true;
                } else {
                    std::scoped_lock lock(stateMutex);
                    if (jobIndex < jobs.size() && frameIndex < jobs[jobIndex].frames.size())
                        jobs[jobIndex].frames[frameIndex].error = dispatchError;
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
                item.blenderVersion = jobCopy.blenderVersion;
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
                }
                workCv.notify_one();
                handled = true;
            }
            if (!handled) {
                {
                    std::scoped_lock lock(stateMutex);
                    if (jobIndex < jobs.size() && frameIndex < jobs[jobIndex].frames.size())
                        jobs[jobIndex].frames[frameIndex].state = TaskState::Pending;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(250));
            }
            PersistJobs();
            Notify();
        }
    }

    bool SendRemoteResult(const WorkItem& item, bool success, const fs::path& output,
                          const std::wstring& renderError) {
        Config config = owner->GetConfig();
        httplib::Headers headers = {
            {"X-RM-Token", config.clusterToken}, {"X-RM-Task", item.taskId},
            {"X-RM-Status", success ? "completed" : "failed"},
            {"X-RM-Error", WideToUtf8(renderError)},
        };
        for (int attempt = 0; attempt < 4 && !stopRequested.load(); ++attempt) {
            if (attempt) std::this_thread::sleep_for(std::chrono::seconds(attempt * 2));
            httplib::Result result;
            if (success) {
                result = PostFile(item.originAddress, item.originPort, "/v1/result", headers, output);
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
            fs::path output;
            std::wstring renderError;
            bool success = false;
            if (owner->mockRenderer_) {
                fs::create_directories(item.outputDir);
                output = item.outputDir /
                    (L"frame_" + std::to_wstring(item.frame) + Utf8ToWide(FrameExtension(item.outputFormat)));
                std::ofstream mock(output, std::ios::binary);
                mock << "RenderMesh mock frame " << item.frame;
                success = static_cast<bool>(mock);
            } else {
                success = RenderBlenderFrame(owner->GetConfig(), item.blend, item.outputDir,
                                             item.frame, item.outputFormat, item.remote, stopRequested,
                                             output, renderError);
            }
            if (item.remote) {
                bool delivered = SendRemoteResult(item, success, output, renderError);
                if (delivered) {
                    std::error_code ignored;
                    fs::remove_all(item.blend.parent_path(), ignored);
                }
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
        return false;
    }
    impl_->serverThread = std::thread([this] { impl_->server.listen_after_bind(); });
    impl_->discoveryThread = std::thread([this] { impl_->DiscoveryLoop(); });
    impl_->schedulerThread = std::thread([this] { impl_->SchedulerLoop(); });
    impl_->workerThread = std::thread([this] { impl_->WorkerLoop(); });
    return true;
}

void NodeApp::Stop() {
    if (!running_.exchange(false)) return;
    impl_->stopRequested = true;
    impl_->server.stop();
    SOCKET discovery = impl_->discoverySocket.exchange(INVALID_SOCKET);
    if (discovery != INVALID_SOCKET) closesocket(discovery);
    impl_->workCv.notify_all();
    if (impl_->serverThread.joinable()) impl_->serverThread.join();
    if (impl_->discoveryThread.joinable()) impl_->discoveryThread.join();
    if (impl_->schedulerThread.joinable()) impl_->schedulerThread.join();
    if (impl_->workerThread.joinable()) impl_->workerThread.join();
    WSACleanup();
}

void NodeApp::SetChangedCallback(std::function<void()> callback) {
    std::scoped_lock lock(impl_->callbackMutex);
    impl_->changed = std::move(callback);
}

Config NodeApp::GetConfig() const {
    std::scoped_lock lock(configMutex_);
    return config_;
}

bool NodeApp::UpdateConfig(const Config& config, std::wstring& error) {
    if (config.nodeName.empty() || config.clusterToken.size() < 16 ||
        config.activeCpuPercent < 1 || config.activeCpuPercent > 100 ||
        config.idleAfterSeconds < 30) {
        error = L"设置值无效";
        return false;
    }
    if (!store_.Save(config, error)) return false;
    {
        std::scoped_lock lock(configMutex_);
        config_ = config;
    }
    impl_->Notify();
    return true;
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

int NodeApp::AssignedCount() const {
    int count = impl_->workerAssigned.load();
    std::scoped_lock lock(impl_->stateMutex);
    for (const auto& job : impl_->jobs)
        for (const auto& frame : job.frames)
            if (frame.state == TaskState::Assigned) ++count;
    return count;
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
    std::string outputFormat,
    std::wstring& error) {
    if (!report.passed || !fs::exists(report.preparedBlend)) {
        error = L"工程未通过预检";
        return {};
    }
    if (name.empty() || frameStart < 1 || frameEnd < frameStart || frameEnd > 1000000 ||
        frameStep < 1 ||
        (outputFormat != "PNG" && outputFormat != "JPEG" && outputFormat != "OPEN_EXR")) {
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
    job.outputFormat = std::move(outputFormat);
    job.blenderVersion = report.blenderVersion;
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
    return id;
}

bool RunSelfTest(std::wstring& report) {
    try {
        fs::path root = fs::temp_directory_path() / (L"RenderMesh-SelfTest-" +
            Utf8ToWide(NewRandomHex(4)));
        fs::create_directories(root);
        ConfigStore store(root);
        Config config = store.Load();
        config.nodeName = "SELFTEST";
        config.activeCpuPercent = 17;
        std::wstring error;
        if (!store.Save(config, error) || store.Load().activeCpuPercent != 17) {
            report = L"配置持久化失败：" + error;
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
            originConfig.clusterToken = sharedToken;
            originConfig.httpPort = 18531;
            originConfig.discoveryPort = 18530;
            workerConfig.nodeName = "SELFTEST-WORKER";
            workerConfig.nodeId = "selftest-worker";
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
                Peer testPeer;
                testPeer.nodeId = workerConfig.nodeId;
                testPeer.name = workerConfig.nodeName;
                testPeer.address = "127.0.0.1";
                testPeer.httpPort = workerConfig.httpPort;
                testPeer.shareEnabled = true;
                testPeer.trusted = true;
                origin.InjectPeerForTest(std::move(testPeer));

                fs::path prepared = root / L"prepared.blend";
                {
                    std::ofstream blend(prepared, std::ios::binary);
                    blend << "RenderMesh mock blend payload";
                }
                PreflightReport preflight;
                preflight.passed = true;
                preflight.preparedBlend = prepared;
                preflight.blenderVersion = "4.5";
                std::wstring submitError;
                std::string jobId = origin.SubmitPrepared(
                    "two-node-self-test", preflight, 1, 3, 1, "PNG", submitError);
                bool clusterOk = !jobId.empty();
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
                        "lost-worker-self-test", preflight, 4, 4, 1, "PNG", submitError);
                    bool reachedAssigned = false;
                    auto assignDeadline = std::chrono::steady_clock::now() +
                        std::chrono::seconds(3);
                    while (!recoveryJob.empty() &&
                           std::chrono::steady_clock::now() < assignDeadline) {
                        auto current = origin.Jobs();
                        if (!current.empty() && current.back().id == recoveryJob &&
                            current.back().frames.front().state == TaskState::Assigned) {
                            reachedAssigned = true;
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
                    clusterOk = reachedAssigned && recoveredLocally;
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
