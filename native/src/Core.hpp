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
std::string NewInviteCode();
bool IsInviteCode(const std::string& value);
bool IsSafeOutputExtension(const std::string& value);
std::vector<std::vector<int>> BuildFramePackages(
    const std::vector<int>& frames, size_t packageSize = 5);
std::string Sha256Prefix(const std::string& value, size_t chars = 16);
std::string Sha256File(const fs::path& path);
std::string HmacSha256(const std::string& key, const std::string& value);
fs::path ExecutableDirectory();
fs::path DefaultDataDirectory();
double IdleSeconds();
std::optional<std::string> DirectedBroadcastAddress(
    const std::string& address, unsigned long prefixLength);
std::wstring LocalEndpointSummary(int port);
void InitializeLogging(const fs::path& dataDir);
void LogInfo(const std::string& message);
void LogWarning(const std::string& message);
void LogError(const std::string& message);
fs::path CurrentLogPath();

struct HardwareInfo {
    std::string cpuModel;
    std::vector<std::string> gpuModels;
};

HardwareInfo QueryHardwareInfo();

struct Config {
    std::string nodeId;
    std::string nodeName;
    std::string inviteCode;
    std::string clusterToken;
    bool pairingPending = false;
    bool shareEnabled = false;
    int activeCpuPercent = 15;
    int idleAfterSeconds = 180;
    int httpPort = 8391;
    int discoveryPort = 8390;
    fs::path blenderPath;
    std::string manualPeerEndpoint;
    std::string theme = "system";
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

struct OutputTarget {
    std::string id;
    std::string name;
    fs::path directory;
};

struct RenderArtifact {
    std::string logicalPath;
    fs::path source;
};

struct PreflightReport {
    bool passed = false;
    std::string blenderVersion;
    std::string renderEngine;
    std::string outputFormat;
    std::string outputExtension;
    bool cpuSafeWhileActive = false;
    bool requiresGpu = false;
    std::vector<std::string> addons;
    std::vector<DependencyItem> missing;
    std::vector<DependencyItem> unpacked;
    std::vector<std::string> warnings;
    std::map<int, fs::path> primaryOutputs;
    std::vector<OutputTarget> compositorOutputs;
    fs::path sourceBlend;
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
    const std::string& outputExtension,
    bool applySharingLimit,
    std::atomic<bool>& stopRequested,
    fs::path& output,
    std::wstring& error);
bool RenderBlenderPackage(
    const Config& config,
    const fs::path& blend,
    const fs::path& outputDir,
    const std::vector<int>& frames,
    const std::string& outputFormat,
    const std::string& outputExtension,
    bool applySharingLimit,
    bool stopAfterCurrentOnActivity,
    std::atomic<bool>& stopRequested,
    std::atomic<bool>& finishCurrentThenStop,
    const std::vector<OutputTarget>& compositorOutputs,
    std::map<int, std::vector<RenderArtifact>>& completed,
    std::wstring& error);
bool WriteResultBundle(
    const fs::path& destination,
    const std::vector<RenderArtifact>& artifacts,
    std::wstring& error,
    std::atomic<bool>* stopRequested = nullptr);
bool ExtractResultBundle(
    const fs::path& bundle,
    const fs::path& stagingDirectory,
    std::map<std::string, fs::path>& artifacts,
    std::wstring& error,
    std::atomic<bool>* stopRequested = nullptr);

enum class TaskState { Pending, Dispatching, Assigned, Rendering, Completed, Failed, Stopped };

std::wstring TaskStateLabel(TaskState state);

struct FrameTask {
    std::string taskId;
    int frame = 1;
    TaskState state = TaskState::Pending;
    std::string assignedNodeId;
    std::string assignedNodeName;
    std::string assignedPackageId;
    std::chrono::steady_clock::time_point assignedAt{};
    int dispatchAttempts = 0;
    std::string attemptToken;
    fs::path output;
    std::string error;
    std::chrono::system_clock::time_point startedAt{};
    std::chrono::system_clock::time_point endedAt{};
};

struct RenderJob {
    std::string id;
    std::string name;
    fs::path sourceBlend;
    fs::path preparedBlend;
    std::string projectHash;
    std::string outputFormat = "PNG";
    std::string outputExtension = ".png";
    std::string blenderVersion;
    std::string renderEngine;
    bool cpuSafeWhileActive = false;
    bool requiresGpu = false;
    std::vector<std::string> requiredAddons;
    std::map<int, fs::path> primaryOutputs;
    std::vector<OutputTarget> compositorOutputs;
    std::vector<FrameTask> frames;
    std::chrono::system_clock::time_point createdAt;
};

struct ContributionRecord {
    std::string packageId;
    std::string jobId;
    std::string jobName;
    std::string originName;
    std::string originAddress;
    std::vector<int> frames;
    std::vector<int> completedFrames;
    std::string state;
    std::string error;
    std::chrono::system_clock::time_point startedAt{};
    std::chrono::system_clock::time_point endedAt{};
};

struct Peer {
    int protocolVersion = 1;
    bool supportsProjectCache = false;
    bool supportsFramePackages = false;
    bool supportsResultBundles = false;
    std::string nodeId;
    std::string name;
    std::string address;
    int httpPort = 8391;
    bool shareEnabled = false;
    bool trusted = false;
    int cpuPercent = 15;
    int assigned = 0;
    std::string cpuModel;
    std::vector<std::string> gpuModels;
    std::string currentTaskId;
    bool currentTaskKnown = true;
    bool manualRoute = false;
    std::chrono::steady_clock::time_point manualValidatedAt{};
    double idleSeconds = 0;
    std::chrono::steady_clock::time_point lastSeen;
};

struct WorkItem {
    struct Frame {
        std::string taskId;
        int frame = 1;
        std::string attemptToken;
    };
    std::string taskId;
    std::string jobId;
    std::string jobName;
    int frame = 1;
    std::string packageId;
    std::vector<Frame> frames;
    std::string projectHash;
    fs::path blend;
    fs::path temporaryTaskDir;
    fs::path outputDir;
    std::string outputFormat;
    std::string outputExtension;
    std::string blenderVersion;
    std::string renderEngine;
    bool cpuSafeWhileActive = false;
    bool requiresGpu = false;
    std::vector<std::string> requiredAddons;
    std::string attemptToken;
    std::vector<OutputTarget> compositorOutputs;
    bool remote = false;
    bool resultBundles = false;
    std::string originName;
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
    bool UpdateConfig(Config config, std::wstring& error);
    bool AddManualPeer(const std::string& endpoint, std::wstring& result);
    std::wstring ManualPeerStatus() const;
    std::vector<Peer> Peers() const;
    std::vector<RenderJob> Jobs() const;
    std::vector<ContributionRecord> Contributions() const;
    bool StopJob(const std::string& jobId, std::wstring& error);
    HardwareInfo Hardware() const;
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
