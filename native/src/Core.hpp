#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN

#include <windows.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <queue>
#include <string>
#include <thread>
#include <vector>

#include "../third_party/json.hpp"

namespace rm {

namespace fs = std::filesystem;
using json = nlohmann::json;

std::string WideToUtf8(const std::wstring& value);
std::wstring Utf8ToWide(const std::string& value);
std::wstring LastErrorMessage(DWORD code = GetLastError());
std::string NewRandomHex(size_t bytes);
std::string Sha256Prefix(const std::string& value, size_t chars = 16);
std::string HmacSha256(const std::string& key, const std::string& value);
fs::path ExecutableDirectory();
fs::path DefaultDataDirectory();
double IdleSeconds();
std::optional<std::string> DirectedBroadcastAddress(
    const std::string& address, unsigned long prefixLength);
std::wstring LocalEndpointSummary(int port);

struct Config {
    std::string nodeId;
    std::string nodeName;
    std::string clusterToken;
    bool shareEnabled = false;
    int activeCpuPercent = 15;
    int idleAfterSeconds = 180;
    int httpPort = 8391;
    int discoveryPort = 8390;
    fs::path blenderPath;
    std::string manualPeerEndpoint;
};

class ConfigStore {
public:
    explicit ConfigStore(fs::path dataDir);
    Config Load();
    bool Save(const Config& config, std::wstring& error);
    const fs::path& DataDir() const { return dataDir_; }

private:
    fs::path dataDir_;
    fs::path configPath_;
};

struct DependencyItem {
    std::string kind;
    std::string owner;
    std::string path;
};

struct PreflightReport {
    bool passed = false;
    std::string blenderVersion;
    std::string renderEngine;
    bool cpuSafeWhileActive = false;
    bool requiresGpu = false;
    std::vector<std::string> addons;
    std::vector<DependencyItem> missing;
    std::vector<DependencyItem> unpacked;
    std::vector<std::string> warnings;
    fs::path preparedBlend;
    std::wstring summary;
};

fs::path FindBlender(const Config& config);
PreflightReport PreflightBlend(
    const Config& config,
    const fs::path& dataDir,
    const fs::path& source,
    const fs::path& prepared,
    int validationFrameStart = 1,
    int validationFrameEnd = 1,
    int validationFrameStep = 1,
    std::atomic<bool>* stopRequested = nullptr);
bool CheckWorkerCompatibility(
    const Config& config,
    const fs::path& dataDir,
    const fs::path& blend,
    const std::string& expectedVersion,
    const std::string& expectedEngine,
    bool requiresGpu,
    const std::vector<std::string>& requiredAddons,
    std::atomic<bool>& stopRequested,
    std::wstring& summary);
bool RenderBlenderFrame(
    const Config& config,
    const fs::path& blend,
    const fs::path& outputDir,
    int frame,
    const std::string& outputFormat,
    bool applySharingLimit,
    std::atomic<bool>& stopRequested,
    fs::path& output,
    std::wstring& error);

enum class TaskState { Pending, Dispatching, Assigned, Rendering, Completed, Failed };

std::wstring TaskStateLabel(TaskState state);

struct FrameTask {
    std::string taskId;
    int frame = 1;
    TaskState state = TaskState::Pending;
    std::string assignedNodeId;
    std::string assignedNodeName;
    std::chrono::steady_clock::time_point assignedAt{};
    int dispatchAttempts = 0;
    std::string attemptToken;
    fs::path output;
    std::string error;
};

struct RenderJob {
    std::string id;
    std::string name;
    fs::path preparedBlend;
    std::string outputFormat = "PNG";
    std::string blenderVersion;
    std::string renderEngine;
    bool cpuSafeWhileActive = false;
    bool requiresGpu = false;
    std::vector<std::string> requiredAddons;
    std::vector<FrameTask> frames;
    std::chrono::system_clock::time_point createdAt;
};

struct Peer {
    std::string nodeId;
    std::string name;
    std::string address;
    int httpPort = 8391;
    bool shareEnabled = false;
    bool trusted = false;
    int cpuPercent = 15;
    int assigned = 0;
    std::string currentTaskId;
    bool currentTaskKnown = true;
    bool manualRoute = false;
    double idleSeconds = 0;
    std::chrono::steady_clock::time_point lastSeen;
};

struct WorkItem {
    std::string taskId;
    std::string jobId;
    int frame = 1;
    fs::path blend;
    fs::path outputDir;
    std::string outputFormat;
    std::string blenderVersion;
    std::string renderEngine;
    bool cpuSafeWhileActive = false;
    bool requiresGpu = false;
    std::vector<std::string> requiredAddons;
    std::string attemptToken;
    bool remote = false;
    std::string originAddress;
    int originPort = 8391;
};

class WindowsJobLimiter {
public:
    WindowsJobLimiter();
    ~WindowsJobLimiter();
    WindowsJobLimiter(const WindowsJobLimiter&) = delete;
    WindowsJobLimiter& operator=(const WindowsJobLimiter&) = delete;
    bool Assign(HANDLE process, std::wstring& error);
    bool SetCpuPercent(int percent, std::wstring& error);

private:
    HANDLE handle_ = nullptr;
};

class NodeApp {
public:
    NodeApp(ConfigStore store, Config config, bool mockRenderer = false,
            std::chrono::milliseconds assignmentGrace = std::chrono::seconds(20));
    ~NodeApp();

    bool Start(std::wstring& error);
    void Stop();
    void SetChangedCallback(std::function<void()> callback);

    Config GetConfig() const;
    bool UpdateConfig(const Config& config, std::wstring& error);
    bool AddManualPeer(const std::string& endpoint, std::wstring& result);
    std::wstring ManualPeerStatus() const;
    std::vector<Peer> Peers() const;
    std::vector<RenderJob> Jobs() const;
    int AssignedCount() const;
    bool Running() const { return running_; }

    // In-process transport test seam. Production peers still come from UDP discovery.
    void InjectPeerForTest(Peer peer);
    void RemovePeerForTest(const std::string& nodeId);

    std::string SubmitPrepared(
        std::string name,
        const PreflightReport& report,
        int frameStart,
        int frameEnd,
        int frameStep,
        std::string outputFormat,
        std::wstring& error);

    fs::path DataDir() const { return store_.DataDir(); }

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    ConfigStore store_;
    mutable std::mutex configMutex_;
    Config config_;
    bool mockRenderer_ = false;
    std::atomic<bool> running_ = false;
};

bool RunSelfTest(std::wstring& report);

}  // namespace rm
