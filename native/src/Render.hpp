#pragma once

// Render application adapters.
//
// The farm (scheduling, project transfer, worker admission, UI and CLI) never
// talks to a specific application. Each supported application implements
// RenderAdapter; Blender is the first implementation. A Cinema 4D adapter
// would add its own project type (.c4d), executable discovery and render
// engines (for example Octane or Redshift) without changing the farm code.

#include "Core.hpp"

namespace rm {

struct RenderPackageRequest {
    Config config;
    fs::path project;
    fs::path outputDir;
    std::vector<int> frames;
    std::string outputFormat;
    std::string outputExtension;
    std::vector<OutputTarget> outputs;
    // Remote work stops after the current frame when the user returns.
    bool stopAfterCurrentOnActivity = false;
    // Application exit: terminate immediately.
    std::atomic<bool>* stop = nullptr;
    // Set by the adapter when the package stops after the current frame.
    std::atomic<bool>* finishCurrentThenStop = nullptr;
    // Set by the origin while no frame has started yet: abort the launch.
    std::atomic<bool>* abortBeforeFirstFrame = nullptr;
    FrameStartedCallback frameStarted;
    FrameCompletedCallback frameCompleted;
    FrameLogCallback frameLogUpdated;
};

struct RenderPackageResult {
    std::map<int, std::vector<RenderArtifact>> completed;
    std::map<int, fs::path> frameLogs;
    std::wstring error;
};

class RenderAdapter {
public:
    virtual ~RenderAdapter() = default;

    // Stable identifier stored in jobs, profiles and the protocol ("blender").
    virtual std::string_view Id() const = 0;
    virtual std::wstring_view DisplayName() const = 0;
    // Lowercase project file extension including the dot (".blend").
    virtual std::wstring_view ProjectExtension() const = 0;

    // Resolves the application executable from the active software profile,
    // the folder of RenderMesh.exe, PATH and the standard install locations.
    virtual fs::path LocateExecutable(const Config& config) const = 0;
    // Validates a user-selected executable and stores it in the active profile.
    virtual bool ConfigureExecutable(
        Config& config, const fs::path& selected, std::wstring& error) const = 0;

    // Opens the project once, reads its render settings and outputs, checks
    // external files and, only when needed, writes a self-contained copy into
    // `workDir`. Never renders project frames on the submitting computer.
    // `inspectOnly` reports what preparation would do without writing a copy.
    virtual PreflightReport Prepare(
        const Config& config, const fs::path& dataDir, const fs::path& project,
        const fs::path& workDir, bool ignoreAssetWarnings,
        std::atomic<bool>* cancel, bool inspectOnly = false) const = 0;

    // Checks that this computer can render a project with the given
    // application version, render engine and GPU requirement.
    virtual bool CheckCompatibility(
        const Config& config, const fs::path& dataDir, const fs::path& workDir,
        const std::string& version, const std::string& engine, bool requiresGpu,
        std::atomic<bool>& stop, std::wstring& summary) const = 0;

    // Renders the frames of one package in one application process and
    // reports every frame as soon as it is written.
    virtual bool RenderPackage(
        const RenderPackageRequest& request, RenderPackageResult& result) const = 0;

    // Converts a rendered image (for example OpenEXR) to a PNG preview.
    virtual bool ConvertPreview(
        const Config& config, const fs::path& dataDir, const fs::path& image,
        const fs::path& png, std::atomic<bool>& stop, std::wstring& error) const = 0;
};

const RenderAdapter& BlenderRenderAdapter();
// Adapter for a stored software identifier; empty means Blender (jobs and
// peers from releases before adapters existed).
const RenderAdapter* FindRenderAdapter(std::string_view id);
// Adapter whose project extension matches `project`, or nullptr.
const RenderAdapter* RenderAdapterForProject(const fs::path& project);
// Lists every adapter, for file dialogs and the command line help.
std::vector<const RenderAdapter*> RenderAdapters();

}  // namespace rm
