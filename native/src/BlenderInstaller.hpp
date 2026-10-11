#pragma once

#include "Core.hpp"

namespace rm {

struct BlenderRelease {
    std::string branch;
    std::string version;
    std::wstring displayName;
    bool lts = false;
    std::wstring msiUrl;
    std::wstring sha256Url;
};

struct BlenderInstallProgress {
    int percent = 0;
    std::wstring detail;
};

struct BlenderInstallResult {
    bool ok = false;
    BlenderRelease release;
    fs::path installer;
    fs::path executable;
    bool preferencesCreated = false;
    bool optixConfigured = false;
    std::wstring warning;
    std::wstring error;
};

using BlenderInstallProgressCallback =
    std::function<void(const BlenderInstallProgress&)>;

fs::path DefaultBlenderDownloadDirectory();
bool HasNvidiaGpu(const HardwareInfo& hardware);
bool ValidateBlenderDownloadDirectory(const fs::path& directory,
                                      std::wstring& error);
bool FetchOfficialBlenderCatalog(std::vector<BlenderRelease>& releases,
                                 std::atomic<bool>& cancel,
                                 std::wstring& error);
bool ProbeOfficialBlenderRelease(const BlenderRelease& release,
                                 std::atomic<bool>& cancel,
                                 uint64_t& sizeBytes,
                                 std::string& sha256,
                                 std::wstring& error);
BlenderInstallResult InstallOfficialBlender(
    const BlenderRelease& release,
    const fs::path& dataDir,
    const fs::path& downloadDirectory,
    bool configureOptix,
    std::atomic<bool>& cancel,
    const BlenderInstallProgressCallback& progress = {});
#ifdef RM_DEV_TOOLS
bool RunBlenderInstallerSelfTest(std::wstring& report);
#endif  // RM_DEV_TOOLS


}  // namespace rm
