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
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "../third_party/json.hpp"

namespace rm {

namespace fs = std::filesystem;
using json = nlohmann::json;
inline constexpr char kRenderMeshAppVersion[] = "0.3.0";
inline constexpr int kBlenderFrameMin = 0;
inline constexpr int kBlenderFrameMax = 1048574;

std::string WideToUtf8(const std::wstring& value);
std::wstring Utf8ToWide(const std::string& value);
std::wstring LastErrorMessage(DWORD code = GetLastError());
std::string NewRandomHex(size_t bytes);
std::string NewInviteCode();
bool IsInviteCode(const std::string& value);
bool IsSafeOutputExtension(const std::string& value);
std::vector<std::vector<int>> BuildFramePackages(
    const std::vector<int>& frames, size_t packageSize = 5);
bool ParseFrameSelection(
    const std::wstring& expression,
    const std::vector<int>& allowedFrames,
    std::vector<int>& selectedFrames,
    std::wstring& error);
// RAII wrapper over a Win32 file handle; replaces iostreams for all file I/O.
class File {
public:
    File() = default;
    ~File() { Close(); }
    File(File&& other) noexcept : handle_(std::exchange(other.handle_, INVALID_HANDLE_VALUE)) {}
    File& operator=(File&& other) noexcept {
        if (this != &other) {
            Close();
            handle_ = std::exchange(other.handle_, INVALID_HANDLE_VALUE);
        }
        return *this;
    }
    File(const File&) = delete;
    File& operator=(const File&) = delete;

    // Shared read access; other processes may keep reading or delete the file.
    static File OpenRead(const fs::path& path);
    // Truncates or creates the file for exclusive writing.
    static File Create(const fs::path& path);
    // Opens or creates the file without truncation (for resumable writes).
    static File OpenWrite(const fs::path& path);

    explicit operator bool() const { return handle_ != INVALID_HANDLE_VALUE; }
    HANDLE Handle() const { return handle_; }
    // Reads up to `size` bytes; `read` is 0 at the end of the file.
    bool Read(void* data, size_t size, size_t& read);
    bool ReadExact(void* data, size_t size);
    bool Write(const void* data, size_t size);
    bool Write(std::string_view text) { return Write(text.data(), text.size()); }
    bool Seek(uint64_t offset);
    uint64_t Size() const;
    bool Flush() { return FlushFileBuffers(handle_) != FALSE; }
    void Close();

private:
    explicit File(HANDLE handle) : handle_(handle) {}
    HANDLE handle_ = INVALID_HANDLE_VALUE;
};

// Reads a whole file. Returns false when it cannot be opened or read.
bool ReadFileText(const fs::path& path, std::string& contents);
std::string ReadFileText(const fs::path& path);
// Writes `contents` to a temporary file and atomically replaces `path`.
// `durable` flushes the data to disk before the replacement.
bool WriteFileAtomic(const fs::path& path, std::string_view contents,
                     std::wstring& error, bool durable = false);
bool WriteFileText(const fs::path& path, std::string_view contents);

class Sha256 {
public:
    Sha256();
    ~Sha256();
    Sha256(const Sha256&) = delete;
    Sha256& operator=(const Sha256&) = delete;
    bool Update(const void* data, size_t size);
    // Lowercase hex digest; empty when hashing failed.
    std::string Finish();

private:
    void* algorithm_ = nullptr;
    void* hash_ = nullptr;
    std::vector<unsigned char> object_;
    bool ok_ = false;
};

std::string Sha256Prefix(std::string_view value, size_t chars = 16);
std::string Sha256File(const fs::path& path, std::atomic<bool>* stopRequested = nullptr);
// Copies `source` to `destination` (replacing it) and returns the SHA-256 of
// the copied bytes, reading the source only once. Empty on failure or cancel.
std::string CopyFileWithSha256(const fs::path& source, const fs::path& destination,
                               std::atomic<bool>* cancel, std::wstring& error);
std::string HmacSha256(const std::string& key, const std::string& value);
fs::path ExecutableDirectory();
fs::path ExecutablePath();
fs::path DefaultDataDirectory();
bool IsNewerAppVersion(const std::string& candidate, const std::string& current);
double IdleSeconds();
bool IsComputerIdleForSharedWork(double idleSeconds, int idleAfterSeconds);
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

struct RenderSoftwareProfile {
    std::string id;
    std::string name;
    std::string renderSystem = "Windows";
    std::string renderSoftware = "Blender";
    fs::path installPath;
};

struct Config {
    std::string nodeId;
    std::string nodeName;
    std::string inviteCode;
    std::string clusterToken;
    bool pairingPending = false;
    bool shareEnabled = false;
    int idleAfterSeconds = 180;
    int httpPort = 8391;
    int discoveryPort = 8390;
    fs::path blenderPath;
    std::vector<RenderSoftwareProfile> renderSoftwareProfiles;
    std::string lastRenderSoftwareProfileId;
    std::string manualPeerEndpoint;
    std::string theme = "system";
    std::string closeBehavior = "ask";
    bool autoStart = false;
    bool silentAutoStart = false;
    fs::path projectStoragePath;
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
    std::vector<std::string> outputs;
    std::string filePattern;
    std::string sceneName;
    std::vector<int> nodePath;
    bool required = false;
};
json OutputTargetMetadata(const OutputTarget& target);
json OutputTargetLocatorMetadata(const OutputTarget& target);
bool ParseOutputTargetMetadata(const json& value, OutputTarget& target);

struct RenderArtifact {
    std::string logicalPath;
    fs::path source;
};

struct FrameRenderTiming {
    std::chrono::system_clock::time_point startedAt{};
    std::chrono::system_clock::time_point endedAt{};

    bool Valid() const {
        return startedAt.time_since_epoch() !=
                   std::chrono::system_clock::duration::zero() &&
            endedAt >= startedAt;
    }
};

using FrameStartedCallback =
    std::function<void(int, std::chrono::system_clock::time_point)>;
using FrameCompletedCallback =
    std::function<void(int, const std::vector<RenderArtifact>&,
                       const FrameRenderTiming&)>;
using FrameLogCallback =
    std::function<void(int, const fs::path&, bool)>;

// Result of RenderAdapter::Prepare: everything the farm needs to know about a
// project before a job is created.
struct PreflightReport {
    bool passed = false;
    bool canIgnoreAssetWarnings = false;
    bool assetWarningsIgnored = false;
    bool validationSkippedByUser = false;
    std::string software;          // RenderAdapter::Id()
    std::string sceneName;
    std::string softwareVersion;   // major.minor; workers must match it
    std::string renderEngine;
    std::string outputFormat;
    std::string outputExtension;
    bool requiresGpu = false;
    int frameStart = 1;
    int frameEnd = 1;
    int frameStep = 1;
    int packedCount = 0;           // external files packed into the copy
    int bundledCount = 0;          // files shipped in the sibling tex folder
    int pendingChanges = 0;        // files to pack or paths to fix (inspect only)
    std::vector<DependencyItem> missing;
    std::vector<DependencyItem> unpacked;
    std::vector<std::string> warnings;
    std::map<int, fs::path> primaryOutputs;
    std::vector<OutputTarget> compositorOutputs;
    fs::path sourceProject;
    // The file to snapshot into the job: a prepared copy in the work folder
    // (preparedCopy) or the unchanged source project.
    fs::path preparedProject;
    bool preparedCopy = false;
    std::wstring summary;
};

// JSON form of a PreflightReport, used by the command line (`inspect --json`)
// and by the local control API (`submit`).
json PreflightReportToJson(const PreflightReport& report);
bool PreflightReportFromJson(const json& value, PreflightReport& report);

struct ProcessResult {
    bool started = false;
    bool timedOut = false;
    DWORD exitCode = static_cast<DWORD>(-1);
    std::wstring error;
};

// Starts `executable` hidden with stdout/stderr redirected to `logPath`, inside
// a kill-on-close job object. Polls `stopRequested`, an optional timeout and
// `progress` (every 100 ms when given) until the process exits.
ProcessResult RunProcess(
    const fs::path& executable,
    const std::vector<std::wstring>& arguments,
    const fs::path& logPath,
    std::atomic<bool>* stopRequested = nullptr,
    std::chrono::milliseconds timeout = std::chrono::milliseconds::zero(),
    const std::function<void()>& progress = {});
std::wstring QuoteCommandLineArgument(const std::wstring& value);
// Last `maximumBytes` of a text file, starting at a line boundary.
std::string ReadFileTail(const fs::path& path, uint64_t maximumBytes);
// Stores `executable` in the active render software profile.
bool SetActiveProfileExecutable(
    Config& config, const fs::path& executable, std::wstring& error);
bool ValidateFramePackageCompletion(
    size_t requestedFrames,
    size_t completedFrames,
    bool stoppedAfterCurrent,
    std::wstring& error);
#ifdef RM_DEV_TOOLS
bool RunBlenderReadinessSelfTest(std::wstring& report);
#endif  // RM_DEV_TOOLS

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

enum class JobStage {
    Rendering,
    Uploading,
    UploadFailed,
    ReadyToConfigure,
    Testing,
    TestCompleted,
    TestFailed,
    Completed,
    Failed,
    Stopped,
};

enum class ProjectTransferState {
    Pending,
    Checking,
    Uploading,
    Cached,
    Completed,
    Failed,
    Offline,
    Excluded,
};

std::wstring TaskStateLabel(TaskState state);
std::wstring JobStageLabel(JobStage stage);

struct ProjectTransfer {
    std::string nodeId;
    std::string nodeName;
    std::string address;
    int httpPort = 8391;
    std::string cpuModel;
    std::vector<std::string> gpuModels;
    ProjectTransferState state = ProjectTransferState::Pending;
    uint64_t transferredBytes = 0;
    uint64_t totalBytes = 0;
    double bytesPerSecond = 0;
    int attempts = 0;
    std::string error;
};

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
    fs::path log;
    std::map<std::string, fs::path> artifacts;
    std::string error;
    std::chrono::system_clock::time_point startedAt{};
    std::chrono::system_clock::time_point endedAt{};
};

struct RenderJob {
    std::string id;
    std::string name;
    std::string sceneName;
    std::string software = "blender";  // RenderAdapter::Id()
    fs::path sourceProject;
    fs::path preparedProject;
    fs::path projectPayload;
    bool projectBundle = false;
    std::string projectHash;
    std::string outputFormat = "PNG";
    std::string outputExtension = ".png";
    std::string softwareVersion;
    std::string renderEngine;
    std::string renderSoftwareProfileId;
    std::string renderSoftwareProfileName;
    fs::path softwareExecutable;
    bool requiresGpu = false;
    std::map<int, fs::path> primaryOutputs;
    std::vector<OutputTarget> compositorOutputs;
    std::vector<std::string> remoteNodeIds;
    JobStage stage = JobStage::Rendering;
    size_t packageSize = 5;
    std::vector<int> testFrames;
    std::vector<ProjectTransfer> projectTransfers;
    std::vector<FrameTask> frames;
    std::chrono::system_clock::time_point createdAt;
    std::chrono::system_clock::time_point finishedAt{};
    std::chrono::system_clock::time_point artifactsPurgedAt{};
    int priority = 0;
};

// Reads the job payloads of a RenderMesh 0.2.x jobs.db (SQLite) for the
// one-time import into per-job JSON records.
bool ReadLegacyJobPayloads(const fs::path& database, std::vector<std::string>& payloads,
                           std::string& error);

struct JobAggregate {
    int activeJobs = 0;
    int totalFrames = 0;
    int completedFrames = 0;
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

struct UpdateOffer {
    std::string version;
    std::string nodeId;
    std::string nodeName;
    std::string address;
    int httpPort = 0;
    uint64_t sizeBytes = 0;
    std::string sha256;
};

struct WorkerActivity {
    std::string jobName;
    std::vector<int> frames;
    size_t completedFrames = 0;
    bool rendering = false;

    bool Active() const { return !jobName.empty() || !frames.empty(); }
};

struct Peer {
    int protocolVersion = 1;
    bool supportsProjectCache = false;
    bool supportsFramePackages = false;
    bool supportsResultBundles = false;
    bool supportsPackageCancellation = false;
    bool supportsJobCleanup = false;
    bool supportsResumableProjectUpload = false;
    bool supportsPeerExchange = false;
    bool supportsDirectStatus = false;
    bool supportsFrameStart = false;
    bool rendererReady = false;
    bool idleReadyKnown = false;
    bool idleReady = false;
    size_t maxPackageFrames = 5;
    std::string nodeId;
    std::string name;
    std::string appVersion;
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
    WorkerActivity activity;
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
    std::string software = "blender";  // RenderAdapter::Id()
    fs::path project;
    // Local tex-bundle job: `project` is extracted from this bundle on first use.
    fs::path bundlePayload;
    fs::path temporaryTaskDir;
    fs::path outputDir;
    std::string outputFormat;
    std::string outputExtension;
    std::string softwareVersion;
    std::string renderEngine;
    fs::path softwareExecutable;
    bool requiresGpu = false;
    std::string attemptToken;
    std::vector<OutputTarget> compositorOutputs;
    bool remote = false;
    bool resultBundles = false;
    bool frameLogs = false;
    std::string originName;
    std::string originAddress;
    int originPort = 8391;
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
    JobAggregate JobStats() const;
    std::optional<RenderJob> FindJob(const std::string& jobId) const;
    std::vector<ContributionRecord> Contributions() const;
    bool DeleteContributionHistory(
        const std::vector<std::string>& packageIds, std::wstring& error);
    bool StopJob(const std::string& jobId, std::wstring& error);
    bool ResumeStoppedJob(const std::string& jobId, std::wstring& error);
    bool RetryFailedFrames(const std::string& jobId, std::wstring& error);
    bool RetryFrame(
        const std::string& jobId, const std::string& taskId, std::wstring& error);
    bool SetJobPriority(const std::string& jobId, bool prioritised, std::wstring& error);
    bool SaveJobAs(const std::string& jobId, const fs::path& destination,
                   std::wstring& error, std::atomic<bool>* cancel = nullptr) const;
    bool DeleteJob(const std::string& jobId, std::wstring& error);
    bool ConfigureAndStartTest(
        const std::string& jobId,
        int frameStart,
        int frameEnd,
        int frameStep,
        size_t packageSize,
        const std::wstring& testFrameExpression,
        std::wstring& error);
    bool StartFullRender(const std::string& jobId, std::wstring& error);
    bool ExcludeProjectTransfer(
        const std::string& jobId, const std::string& nodeId, std::wstring& error);
    bool RetryProjectTransfer(
        const std::string& jobId, const std::string& nodeId, std::wstring& error);
    bool SetJobParticipants(
        const std::string& jobId, const std::vector<std::string>& nodeIds,
        std::wstring& error);
    std::optional<UpdateOffer> CheckForLanUpdate(
        std::wstring& error, std::atomic<bool>* cancel = nullptr);
    bool DownloadLanUpdate(
        const UpdateOffer& offer, fs::path& pendingExecutable, std::wstring& error,
        std::atomic<bool>* cancel = nullptr);
    HardwareInfo Hardware() const;
    int AssignedCount() const;
    WorkerActivity CurrentActivity() const;
    uint64_t JobRevision() const;
    bool Running() const { return running_; }
    fs::path ProjectStorageDir() const;
    bool MigrateProjectStorage(
        const fs::path& destination,
        const std::function<void(int)>& progress,
        std::atomic<bool>* cancel,
        std::wstring& result);

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
    // Snapshots a prepared project into project storage and creates a job
    // that first synchronises the project to the participating computers.
    // `progress` receives (bytes written, total bytes) while copying.
    std::string CreatePreparedDraft(
        std::string name,
        const PreflightReport& report,
        std::wstring& error,
        std::atomic<bool>* cancel = nullptr,
        const std::function<void(uint64_t, uint64_t)>& progress = {});

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

#ifdef RM_DEV_TOOLS
bool RunSelfTest(std::wstring& report);
#endif  // RM_DEV_TOOLS


}  // namespace rm
