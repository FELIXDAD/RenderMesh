// Command line interface for people, scripts and agents.
//
// RenderMesh.exe <command> [arguments] [options]
//
// `inspect` works alone. The job commands send the operation to the RenderMesh
// process that runs on this computer (desktop window or `serve`) through its
// local control API on 127.0.0.1, authenticated with the node secret stored in
// config.json. The help text follows ASD-STE100 Simplified Technical English.

#include "Cli.hpp"

#include "Http.hpp"
#include "Render.hpp"

namespace rm {
namespace {

constexpr int kExitOk = 0;
constexpr int kExitFailed = 1;
constexpr int kExitUsage = 2;
constexpr int kExitNotRunning = 3;

// ------------------------------------------------------------------ output

// A GUI-subsystem program has no console of its own. Write to redirected
// handles (pipes and files from scripts or agents) directly; otherwise attach
// to the console of the parent process.
class Output {
public:
    Output() {
        out_ = Resolve(STD_OUTPUT_HANDLE);
        err_ = Resolve(STD_ERROR_HANDLE);
    }
    void Out(std::string_view text) { Write(out_, text); }
    void Err(std::string_view text) { Write(err_ ? err_ : out_, text); }

private:
    HANDLE Resolve(DWORD which) {
        HANDLE handle = GetStdHandle(which);
        if (handle && handle != INVALID_HANDLE_VALUE &&
            GetFileType(handle) != FILE_TYPE_UNKNOWN)
            return handle;
        if (!attached_) attached_ = AttachConsole(ATTACH_PARENT_PROCESS) != FALSE;
        if (!attached_) return nullptr;
        handle = CreateFileW(L"CONOUT$", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_WRITE,
                             nullptr, OPEN_EXISTING, 0, nullptr);
        return handle == INVALID_HANDLE_VALUE ? nullptr : handle;
    }

    static void Write(HANDLE handle, std::string_view text) {
        if (!handle) return;
        DWORD mode = 0;
        if (GetConsoleMode(handle, &mode)) {
            const std::wstring wide = Utf8ToWide(std::string(text));
            DWORD written = 0;
            WriteConsoleW(handle, wide.data(), static_cast<DWORD>(wide.size()), &written,
                          nullptr);
            return;
        }
        DWORD written = 0;
        WriteFile(handle, text.data(), static_cast<DWORD>(text.size()), &written, nullptr);
    }

    HANDLE out_ = nullptr;
    HANDLE err_ = nullptr;
    bool attached_ = false;
};

// ------------------------------------------------------------------ help

const char* kGeneralHelp = R"(RenderMesh %VERSION%
RenderMesh is a render farm for computers on a local network.
It sends the frames of a project to other computers and collects the images.

USAGE
  RenderMesh.exe
      Start the desktop application.
  RenderMesh.exe <command> [arguments] [options]
      Do one operation, write the result, and stop.

COMMANDS
  help [<command>]    Show this help, or the help for one command.
  version             Show the version of this program.
  status              Show this computer and the other computers in the farm.
  jobs                Show all jobs.
  job <job-id>        Show one job and the status of each frame.
  inspect <project>   Read a project and examine its external files.
                      This command does not create a job.
  submit <project>    Read a project, make a copy of it, and create a job.
  test <job-id>       Set the frame range of a job and render its test frames.
  start <job-id>      Render all frames of a job after the test frames are complete.
  stop <job-id>       Stop a job. Complete frames stay complete.
  resume <job-id>     Render the stopped frames of a job again.
  retry <job-id>      Render the failed frames of a job again.
  delete <job-id>     Stop a job and remove its files from the project storage.
  serve               Run this computer as a node without the desktop window.

OPTIONS FOR ALL COMMANDS
  --json              Write the result as one JSON object on one line.
                      Use this option in scripts and agents.
  --data-dir <folder> Use this data folder.
                      The default folder is %LOCALAPPDATA%\RenderMesh.

EXIT CODES
  0  The operation is complete.
  1  The operation failed. The output gives the cause.
  2  The command or an argument is not correct.
  3  RenderMesh is not running on this computer.
     Start the desktop application or run "RenderMesh.exe serve".

NOTES
  The commands status, jobs, job, submit, test, start, stop, resume, retry and
  delete send the operation to the RenderMesh process on this computer.
  With --json, the key "ok" tells if the operation is complete.
  If "ok" is false, the key "error" gives the cause.
  In cmd.exe, use "start /wait RenderMesh.exe <command>" to see the output
  before the next prompt. In PowerShell, use "RenderMesh.exe <command> | Out-Host".
  Use "RenderMesh.exe help <command>" to see the options of one command.
)";

struct CommandHelp {
    const char* name;
    const char* text;
};

const CommandHelp kCommandHelp[] = {
    {"status", R"(RenderMesh.exe status [--json]
  Show the status of this computer and of the other computers in the farm.
  The output gives the version, the shared state, the idle time, the
  renderer path and the project storage folder of this computer.
  For each other computer, the output gives the address, the version and the
  state.
)"},
    {"jobs", R"(RenderMesh.exe jobs [--json]
  Show all jobs, the newest job first.
  For each job, the output gives the job ID, the stage and the frame counts.
  Stages: uploading, upload_failed, ready, testing, test_completed,
  test_failed, rendering, completed, failed, stopped.
)"},
    {"job", R"(RenderMesh.exe job <job-id> [--json]
  Show one job and the status of each frame.
  Frame states: pending, dispatching, queued, rendering, completed, failed,
  stopped.
)"},
    {"inspect", R"(RenderMesh.exe inspect <project> [--ignore-warnings] [--json]
  Read a project and examine its external files.
  This command does not create a job and does not change the project.
  The output gives the frame range, the render engine, the output format, the
  missing files and the files that the job copy cannot contain.
  --ignore-warnings   Accept missing files and files that cannot be packed.
  Exit code 0 tells that "submit" can create a job from this project.
)"},
    {"submit", R"(RenderMesh.exe submit <project> [--name <name>] [--ignore-warnings]
                       [--wait] [--json]
  Read a project, make a copy of it, and create a job.
  RenderMesh opens the project one time. It packs external files into the copy
  only when necessary. Files in a "tex" folder next to the project go to the
  other computers in a project bundle.
  After this command, RenderMesh sends the copy to the participating
  computers. Then the job stage changes to "ready".
  --name <name>       Use this job name. The default is the project file name.
  --ignore-warnings   Create the job also when files are missing.
                      Make sure that the missing files are not necessary.
  --wait              Wait until the copy is on all participating computers.
)"},
    {"test", R"(RenderMesh.exe test <job-id> [--frames <first>-<last>] [--step <n>]
                     [--package <n>] [--test-frames <list>] [--json]
  Set the frame range of a job and render its test frames.
  Use this command when the job stage is "ready".
  --frames <first>-<last>  Render only these frames.
                           The default is the frame range of the project.
  --step <n>               Render each n-th frame. The default is the step of
                           the project.
  --package <n>            Send n frames to a computer in one package.
                           Use a value from 1 to 20. The default is 5.
  --test-frames <list>     Render these frames first, for example "1,50,100"
                           or "1-3". The default is the first frame.
)"},
    {"start", R"(RenderMesh.exe start <job-id> [--json]
  Render all frames of a job.
  Use this command when the job stage is "test_completed".
)"},
    {"stop", R"(RenderMesh.exe stop <job-id> [--json]
  Stop a job. Complete frames stay complete.
  A computer that renders a frame completes that frame and then stops.
)"},
    {"resume", R"(RenderMesh.exe resume <job-id> [--json]
  Render the stopped frames of a job again.
)"},
    {"retry", R"(RenderMesh.exe retry <job-id> [--json]
  Render the failed frames of a job again.
)"},
    {"delete", R"(RenderMesh.exe delete <job-id> [--json]
  Stop a job and remove its files from the project storage.
  This command does not remove the source project or the rendered images in
  the output folders of the project.
)"},
    {"serve", R"(RenderMesh.exe serve [--port <port>] [--data-dir <folder>]
  Run this computer as a node without the desktop window.
  The node finds other computers, receives jobs and renders frames.
  Press Ctrl+C or stop the process to stop the node.
  Only one RenderMesh process can use a data folder at one time.
  --port <port>       Use this TCP port. The default is 8391.
)"},
    {"version", R"(RenderMesh.exe version [--json]
  Show the version of this program.
)"},
};

std::string GeneralHelp() {
    std::string text = kGeneralHelp;
    const size_t token = text.find("%VERSION%");
    text.replace(token, 9, kRenderMeshAppVersion);
    return text;
}

// ------------------------------------------------------------------ arguments

struct Arguments {
    std::string command;
    std::vector<std::wstring> positional;
    std::map<std::string, std::wstring> options;
    std::set<std::string> flags;
    bool json = false;
    fs::path dataDir;
    std::wstring error;

    std::wstring Option(const char* name, std::wstring fallback = {}) const {
        auto found = options.find(name);
        return found == options.end() ? fallback : found->second;
    }
};

const std::set<std::string> kValueOptions = {
    "--data-dir", "--name", "--frames", "--step", "--package", "--test-frames", "--port"};
const std::set<std::string> kFlagOptions = {"--json", "--ignore-warnings", "--wait"};

Arguments Parse(int count, LPWSTR* values) {
    Arguments arguments;
    arguments.dataDir = DefaultDataDirectory();
    arguments.command = WideToUtf8(values[1]);
    for (int index = 2; index < count; ++index) {
        const std::string text = WideToUtf8(values[index]);
        if (kFlagOptions.contains(text)) {
            arguments.flags.insert(text);
        } else if (kValueOptions.contains(text)) {
            if (index + 1 >= count) {
                arguments.error = L"Option " + std::wstring(values[index]) + L" needs a value.";
                break;
            }
            arguments.options[text] = values[++index];
        } else if (text.starts_with("--")) {
            arguments.error = L"Unknown option: " + std::wstring(values[index]);
            break;
        } else {
            arguments.positional.push_back(values[index]);
        }
    }
    arguments.json = arguments.flags.contains("--json");
    if (auto dir = arguments.options.find("--data-dir"); dir != arguments.options.end())
        arguments.dataDir = dir->second;
    return arguments;
}

bool ParseInt(const std::wstring& text, int& value) {
    if (text.empty() || text.size() > 9 ||
        text.find_first_not_of(L"0123456789") != std::wstring::npos)
        return false;
    value = std::stoi(text);
    return true;
}

// ------------------------------------------------------------------ results

class Reporter {
public:
    Reporter(Output& output, bool json) : output_(output), json_(json) {}

    int Success(const json& value, const std::string& text) {
        if (json_) {
            json body = value;
            body["ok"] = true;
            output_.Out(body.dump() + "\n");
        } else {
            output_.Out(text);
        }
        return kExitOk;
    }

    int Failure(int code, const std::string& error, const json& details = json::object()) {
        if (json_) {
            json body = details;
            body["ok"] = false;
            body["error"] = error;
            body["exit_code"] = code;
            output_.Out(body.dump() + "\n");
        } else {
            output_.Err("Error: " + error + "\n");
        }
        return code;
    }

private:
    Output& output_;
    bool json_;
};

// ------------------------------------------------------------------ local node

class LocalNode {
public:
    explicit LocalNode(const fs::path& dataDir) {
        const fs::path configPath = dataDir / L"config.json";
        if (!fs::is_regular_file(configPath)) return;
        config_ = ConfigStore(dataDir).Load();
        available_ = true;
    }

    bool Configured() const { return available_; }
    const Config& Settings() const { return config_; }

    // Returns false and sets `error` when RenderMesh does not answer.
    bool Call(const char* method, const std::string& path, const json& body,
              json& reply, std::string& error, int& status, time_t timeoutSeconds = 30) {
        http::Client client("127.0.0.1", config_.httpPort);
        client.set_connection_timeout(2);
        client.set_read_timeout(timeoutSeconds);
        client.set_write_timeout(30);
        const http::Headers headers = {{"X-RM-Local-Token", config_.clusterToken}};
        const http::Result result = std::string_view(method) == "GET"
            ? client.Get(path, headers)
            : client.Post(path, headers, body.dump(), "application/json");
        if (!result) {
            error = "RenderMesh does not answer on 127.0.0.1:" +
                std::to_string(config_.httpPort) + " (" + http::to_string(result.error()) + ")";
            return false;
        }
        status = result->status;
        reply = json::parse(result->body, nullptr, false);
        if (status == 401) {
            error = "RenderMesh refused the local request. The data folder belongs to a "
                    "different RenderMesh process.";
            return false;
        }
        if (!reply.is_object()) {
            error = "RenderMesh sent a reply that is not correct (HTTP " +
                std::to_string(status) + "). Update RenderMesh to version " +
                std::string(kRenderMeshAppVersion) + " or later.";
            return false;
        }
        return true;
    }

private:
    Config config_;
    bool available_ = false;
};

std::string FormatTime(int64_t milliseconds) {
    if (milliseconds <= 0) return "-";
    const auto seconds = static_cast<time_t>(milliseconds / 1000);
    std::tm local{};
    localtime_s(&local, &seconds);
    char text[32]{};
    std::strftime(text, sizeof(text), "%Y-%m-%d %H:%M:%S", &local);
    return text;
}

std::string JobLine(const json& job) {
    return job.value("id", "") + "  " + job.value("stage", "") + "  " +
        std::to_string(job.value("frames_completed", 0)) + "/" +
        std::to_string(job.value("frames_total", 0)) + " frames" +
        (job.value("frames_failed", 0) ? "  failed " +
            std::to_string(job.value("frames_failed", 0)) : std::string{}) +
        "  " + job.value("name", "") + "\n";
}

std::string JobText(const json& job, bool frames) {
    std::string text =
        "Job:        " + job.value("id", "") + "\n" +
        "Name:       " + job.value("name", "") + "\n" +
        "Stage:      " + job.value("stage", "") + "\n" +
        "Software:   " + job.value("software", "") + " " + job.value("version", "") + " (" +
            job.value("render_engine", "") + ")\n" +
        "Source:     " + job.value("source", "") + "\n" +
        "Frames:     " + std::to_string(job.value("frame_first", 0)) + "-" +
            std::to_string(job.value("frame_last", 0)) + ", " +
            std::to_string(job.value("frames_completed", 0)) + " of " +
            std::to_string(job.value("frames_total", 0)) + " complete, " +
            std::to_string(job.value("frames_failed", 0)) + " failed\n" +
        "Created:    " + FormatTime(job.value("created_at", int64_t{0})) + "\n";
    for (const auto& transfer : job.value("transfers", json::array()))
        text += "Copy to:    " + transfer.value("node_name", "") + "  " +
            transfer.value("state", "") + "  " +
            std::to_string(transfer.value("transferred_bytes", uint64_t{0}) >> 20) + "/" +
            std::to_string(transfer.value("total_bytes", uint64_t{0}) >> 20) + " MiB" +
            (transfer.value("error", "").empty() ? "" : "  " + transfer.value("error", "")) +
            "\n";
    if (frames) {
        text += "\nFRAME  STATE        COMPUTER              ERROR\n";
        for (const auto& frame : job.value("frames", json::array())) {
            std::string row = std::to_string(frame.value("frame", 0));
            row.resize(7, ' ');
            std::string state = frame.value("state", "");
            state.resize(13, ' ');
            std::string node = frame.value("node", "");
            if (node.size() < 22) node.resize(22, ' ');
            text += row + state + node + frame.value("error", "") + "\n";
        }
    }
    return text;
}

// ------------------------------------------------------------------ commands

int Inspect(const Arguments& arguments, Reporter& reporter, const fs::path& project) {
    const RenderAdapter* adapter = RenderAdapterForProject(project);
    if (!adapter || !fs::is_regular_file(project))
        return reporter.Failure(kExitUsage, "The project file does not exist or its type is "
                                "not supported: " + WideToUtf8(project.wstring()));
    const Config config = fs::is_regular_file(arguments.dataDir / L"config.json")
        ? ConfigStore(arguments.dataDir).Load() : Config{};
    const fs::path work = fs::temp_directory_path() /
        Utf8ToWide("RenderMesh-inspect-" + NewRandomHex(6));
    PreflightReport report = adapter->Prepare(
        config, arguments.dataDir, project, work, arguments.flags.contains("--ignore-warnings"),
        nullptr, true);
    std::error_code ignored;
    fs::remove_all(work, ignored);
    json value = PreflightReportToJson(report);
    value.erase("prepared");
    value.erase("prepared_copy");
    if (!report.passed)
        return reporter.Failure(kExitFailed, WideToUtf8(report.summary), value);
    return reporter.Success(value, WideToUtf8(report.summary) + "\n");
}

int Submit(const Arguments& arguments, Reporter& reporter, LocalNode& node,
           const fs::path& project, Output& output) {
    const RenderAdapter* adapter = RenderAdapterForProject(project);
    if (!adapter || !fs::is_regular_file(project))
        return reporter.Failure(kExitUsage, "The project file does not exist or its type is "
                                "not supported: " + WideToUtf8(project.wstring()));
    json status;
    std::string error;
    int code = 0;
    if (!node.Call("GET", "/v5/local/status", {}, status, error, code))
        return reporter.Failure(kExitNotRunning, error);
    // The copy goes to the preflight folder of the node's project storage, so
    // the node can move it into the job folder without copying the data again.
    const fs::path storage = Utf8ToWide(status["node"].value("project_storage", ""));
    const fs::path work = storage / L"preflight" / Utf8ToWide("cli-" + NewRandomHex(6));
    if (!arguments.json) output.Out("Reading the project...\n");
    PreflightReport report = adapter->Prepare(
        node.Settings(), arguments.dataDir, project, work,
        arguments.flags.contains("--ignore-warnings"), nullptr);
    std::error_code ignored;
    if (!report.passed) {
        fs::remove_all(work, ignored);
        return reporter.Failure(kExitFailed, WideToUtf8(report.summary),
                                {{"report", PreflightReportToJson(report)}});
    }
    if (!arguments.json) output.Out("Creating the job...\n");
    json reply;
    const std::string name = WideToUtf8(arguments.Option("--name", project.stem().wstring()));
    const bool called = node.Call(
        "POST", "/v5/local/jobs", {{"name", name}, {"report", PreflightReportToJson(report)}},
        reply, error, code, 3600);
    fs::remove_all(work, ignored);
    if (!called) return reporter.Failure(kExitNotRunning, error);
    if (!reply.value("ok", false))
        return reporter.Failure(kExitFailed, reply.value("error", "The job was not created."));
    json job = reply["job"];
    const std::string id = job.value("id", "");
    if (arguments.flags.contains("--wait")) {
        // Wait while the copy goes to the participating computers.
        while (job.value("stage", "") == "uploading") {
            Sleep(1000);
            if (!node.Call("GET", "/v5/local/job?id=" + id, {}, reply, error, code))
                return reporter.Failure(kExitNotRunning, error);
            if (!reply.value("ok", false))
                return reporter.Failure(kExitFailed, reply.value("error", "The job is not found."));
            job = reply["job"];
            job.erase("frames");
        }
        if (job.value("stage", "") == "upload_failed")
            return reporter.Failure(kExitFailed, "The copy did not go to all computers.",
                                    {{"job", job}});
    }
    return reporter.Success({{"job", job}, {"report", PreflightReportToJson(report)}},
                            WideToUtf8(report.summary) + "\nJob " + id + " created. Stage: " +
                                job.value("stage", "") + "\n");
}

int JobAction(const Arguments& arguments, Reporter& reporter, LocalNode& node,
              const std::string& action, const std::string& id) {
    json body = {{"action", action}, {"id", id}};
    if (action == "test") {
        const std::wstring frames = arguments.Option("--frames");
        if (!frames.empty()) {
            const size_t dash = frames.find(L'-');
            int first = 0, last = 0;
            if (dash == std::wstring::npos || !ParseInt(frames.substr(0, dash), first) ||
                !ParseInt(frames.substr(dash + 1), last))
                return reporter.Failure(kExitUsage, "--frames needs the form <first>-<last>.");
            body["frame_start"] = first;
            body["frame_end"] = last;
        }
        int number = 0;
        if (const auto step = arguments.Option("--step"); !step.empty()) {
            if (!ParseInt(step, number) || number < 1)
                return reporter.Failure(kExitUsage, "--step needs a number of 1 or more.");
            body["frame_step"] = number;
        }
        if (const auto package = arguments.Option("--package"); !package.empty()) {
            if (!ParseInt(package, number) || number < 1 || number > 20)
                return reporter.Failure(kExitUsage, "--package needs a number from 1 to 20.");
            body["package_size"] = number;
        }
        if (const auto tests = arguments.Option("--test-frames"); !tests.empty())
            body["test_frames"] = WideToUtf8(tests);
    }
    json reply;
    std::string error;
    int code = 0;
    if (!node.Call("POST", "/v5/local/job-action", body, reply, error, code))
        return reporter.Failure(kExitNotRunning, error);
    if (!reply.value("ok", false))
        return reporter.Failure(kExitFailed, reply.value("error", "The operation failed."));
    reply.erase("ok");
    if (action == "delete") return reporter.Success(reply, "Job " + id + " deleted.\n");
    return reporter.Success(reply, JobText(reply["job"], false));
}

}  // namespace

bool IsCommandLineCommand(const std::wstring& argument) {
    // Every first argument that is not an option of the desktop application
    // is a command; unknown commands fail with exit code 2 instead of
    // silently opening a window.
    return argument == L"--help" || argument == L"-h" || argument == L"/?" ||
        argument == L"--version" || (!argument.empty() && argument[0] != L'-' &&
                                     argument != L"serve");
}

int RunCommandLine(int count, LPWSTR* values) {
    Output output;
    Arguments arguments = Parse(count, values);
    Reporter reporter(output, arguments.json);
    if (!arguments.error.empty())
        return reporter.Failure(kExitUsage, WideToUtf8(arguments.error));
    const std::string& command = arguments.command;
    if (command == "help" || command == "--help" || command == "-h" || command == "/?") {
        if (!arguments.positional.empty()) {
            const std::string topic = WideToUtf8(arguments.positional.front());
            for (const auto& entry : kCommandHelp)
                if (topic == entry.name) return reporter.Success({{"help", entry.text}}, entry.text);
            return reporter.Failure(kExitUsage, "There is no command \"" + topic + "\".");
        }
        const std::string text = GeneralHelp();
        return reporter.Success({{"help", text}}, text);
    }
    if (command == "version" || command == "--version")
        return reporter.Success({{"version", kRenderMeshAppVersion}},
                                std::string("RenderMesh ") + kRenderMeshAppVersion + "\n");
    static const std::set<std::string> known = {
        "status", "jobs", "job", "inspect", "submit", "test", "start", "stop",
        "resume", "retry", "delete"};
    if (!known.contains(command))
        return reporter.Failure(kExitUsage, "There is no command \"" + command +
                                "\". Run \"RenderMesh.exe help\".");

    const bool needsTarget = command != "status" && command != "jobs";
    if (needsTarget && arguments.positional.size() != 1)
        return reporter.Failure(kExitUsage, "The command \"" + command +
                                "\" needs one argument. Run \"RenderMesh.exe help " + command +
                                "\".");
    if (!needsTarget && !arguments.positional.empty())
        return reporter.Failure(kExitUsage, "The command \"" + command +
                                "\" does not use arguments.");
    InitializeLogging(arguments.dataDir);
    if (command == "inspect")
        return Inspect(arguments, reporter, fs::absolute(arguments.positional.front()));

    LocalNode node(arguments.dataDir);
    if (!node.Configured())
        return reporter.Failure(kExitNotRunning, "RenderMesh has no data in " +
                                WideToUtf8(arguments.dataDir.wstring()) +
                                ". Start RenderMesh one time on this computer.");
    if (command == "submit")
        return Submit(arguments, reporter, node, fs::absolute(arguments.positional.front()),
                      output);
    json reply;
    std::string error;
    int code = 0;
    if (command == "status") {
        if (!node.Call("GET", "/v5/local/status", {}, reply, error, code))
            return reporter.Failure(kExitNotRunning, error);
        reply.erase("ok");
        const json& self = reply["node"];
        std::string text =
            "Computer:   " + self.value("name", "") + " (" + self.value("id", "") + ")\n" +
            "Version:    " + self.value("version", "") + "\n" +
            "Matching ID " + self.value("matching_id", "") +
                (self.value("pairing_pending", false) ? " (waiting for pairing)" : "") + "\n" +
            "Sharing:    " + (self.value("share", false) ? "on" : "off") + ", idle " +
                std::to_string(self.value("idle_seconds", 0)) + " s of " +
                std::to_string(self.value("idle_after_seconds", 0)) + " s\n" +
            "Renderer:   " + (self.value("renderer_ready", false)
                ? self.value("renderer_path", "") : std::string("not found")) + "\n" +
            "Storage:    " + self.value("project_storage", "") + "\n";
        for (const auto& peer : reply["peers"])
            text += "Peer:       " + peer.value("name", "") + "  " + peer.value("address", "") +
                "  " + peer.value("app_version", "") + "  " +
                (peer.value("share", false) ? "sharing" : "not sharing") +
                (peer.value("renderer_ready", false) ? "" : ", no renderer") + "\n";
        if (reply["peers"].empty()) text += "Peer:       none found\n";
        return reporter.Success(reply, text);
    }
    if (command == "jobs") {
        if (!node.Call("GET", "/v5/local/jobs", {}, reply, error, code))
            return reporter.Failure(kExitNotRunning, error);
        reply.erase("ok");
        std::string text;
        for (const auto& job : reply["jobs"]) text += JobLine(job);
        if (text.empty()) text = "There are no jobs.\n";
        return reporter.Success(reply, text);
    }
    const std::string id = WideToUtf8(arguments.positional.front());
    if (command == "job") {
        if (!node.Call("GET", "/v5/local/job?id=" + id, {}, reply, error, code))
            return reporter.Failure(kExitNotRunning, error);
        if (!reply.value("ok", false))
            return reporter.Failure(kExitFailed, reply.value("error", "The job is not found."));
        reply.erase("ok");
        return reporter.Success(reply, JobText(reply["job"], true));
    }
    return JobAction(arguments, reporter, node, command, id);
}

}  // namespace rm
