#include "Core.hpp"

#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>

#include <algorithm>
#include <fstream>
#include <memory>

namespace {

using namespace rm;

constexpr UINT WM_RM_REFRESH = WM_APP + 1;
constexpr UINT WM_RM_PREFLIGHT = WM_APP + 2;

enum ControlId {
    ID_SHARE = 100,
    ID_NODE_NAME,
    ID_CPU_SLIDER,
    ID_CPU_VALUE,
    ID_IDLE_MINUTES,
    ID_BLENDER_PATH,
    ID_BROWSE_BLENDER,
    ID_CLUSTER_TOKEN,
    ID_COPY_TOKEN,
    ID_SAVE_SETTINGS,
    ID_SOURCE_PATH,
    ID_BROWSE_SOURCE,
    ID_JOB_NAME,
    ID_FRAME_START,
    ID_FRAME_END,
    ID_FRAME_STEP,
    ID_OUTPUT_FORMAT,
    ID_SUBMIT,
    ID_PREFLIGHT,
    ID_NODES,
    ID_JOBS,
    ID_STATUS,
    ID_OPEN_DATA,
};

std::unique_ptr<NodeApp> gApp;
HWND gWindow = nullptr;
HFONT gFont = nullptr;
HFONT gTitleFont = nullptr;
std::thread gPreflight;
std::atomic<bool> gPreflightRunning = false;

HWND Item(int id) { return GetDlgItem(gWindow, id); }

std::wstring Text(HWND control) {
    int length = GetWindowTextLengthW(control);
    std::wstring value(static_cast<size_t>(length) + 1, L'\0');
    GetWindowTextW(control, value.data(), length + 1);
    value.resize(static_cast<size_t>(length));
    return value;
}

void SetText(int id, const std::wstring& value) { SetWindowTextW(Item(id), value.c_str()); }

int IntValue(int id, int fallback) {
    try {
        return std::stoi(Text(Item(id)));
    } catch (...) {
        return fallback;
    }
}

HWND AddControl(
    const wchar_t* type,
    const wchar_t* text,
    DWORD style,
    int x,
    int y,
    int width,
    int height,
    int id,
    HFONT font = nullptr) {
    HWND control = CreateWindowExW(
        wcscmp(type, WC_LISTVIEWW) == 0 ? WS_EX_CLIENTEDGE : 0, type, text,
        WS_CHILD | WS_VISIBLE | style, x, y, width, height, gWindow,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr), nullptr);
    SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font ? font : gFont), TRUE);
    return control;
}

HWND Label(const wchar_t* text, int x, int y, int width, int height, int id = 0,
           HFONT font = nullptr) {
    return AddControl(L"STATIC", text, SS_LEFT, x, y, width, height, id, font);
}

void AddColumn(HWND list, int index, int width, const wchar_t* title) {
    LVCOLUMNW column{};
    column.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
    column.pszText = const_cast<wchar_t*>(title);
    column.cx = width;
    column.iSubItem = index;
    ListView_InsertColumn(list, index, &column);
}

void SetCell(HWND list, int row, int column, const std::wstring& value) {
    if (column == 0) {
        LVITEMW item{};
        item.mask = LVIF_TEXT;
        item.iItem = row;
        item.pszText = const_cast<wchar_t*>(value.c_str());
        ListView_InsertItem(list, &item);
    } else {
        ListView_SetItemText(list, row, column, const_cast<wchar_t*>(value.c_str()));
    }
}

std::wstring ChooseFile(const wchar_t* filter, const wchar_t* title) {
    wchar_t path[32768]{};
    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = gWindow;
    dialog.lpstrFilter = filter;
    dialog.lpstrFile = path;
    dialog.nMaxFile = 32768;
    dialog.lpstrTitle = title;
    dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    return GetOpenFileNameW(&dialog) ? path : L"";
}

void CopyText(const std::wstring& text) {
    if (!OpenClipboard(gWindow)) return;
    EmptyClipboard();
    size_t bytes = (text.size() + 1) * sizeof(wchar_t);
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (memory) {
        void* target = GlobalLock(memory);
        memcpy(target, text.c_str(), bytes);
        GlobalUnlock(memory);
        SetClipboardData(CF_UNICODETEXT, memory);
    }
    CloseClipboard();
}

void LoadConfigIntoUi() {
    Config config = gApp->GetConfig();
    SetText(ID_NODE_NAME, Utf8ToWide(config.nodeName));
    SendMessageW(Item(ID_SHARE), BM_SETCHECK,
                 config.shareEnabled ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(Item(ID_CPU_SLIDER), TBM_SETPOS, TRUE, config.activeCpuPercent);
    SetText(ID_CPU_VALUE, std::to_wstring(config.activeCpuPercent) + L"%");
    SetText(ID_IDLE_MINUTES, std::to_wstring(config.idleAfterSeconds / 60.0));
    SetText(ID_BLENDER_PATH, config.blenderPath.wstring());
    SetText(ID_CLUSTER_TOKEN, Utf8ToWide(config.clusterToken));
}

bool SaveSettings() {
    Config config = gApp->GetConfig();
    config.nodeName = WideToUtf8(Text(Item(ID_NODE_NAME)));
    config.shareEnabled = SendMessageW(Item(ID_SHARE), BM_GETCHECK, 0, 0) == BST_CHECKED;
    config.activeCpuPercent = static_cast<int>(SendMessageW(Item(ID_CPU_SLIDER), TBM_GETPOS, 0, 0));
    config.idleAfterSeconds = std::max(30, static_cast<int>(IntValue(ID_IDLE_MINUTES, 3) * 60));
    config.blenderPath = Text(Item(ID_BLENDER_PATH));
    config.clusterToken = WideToUtf8(Text(Item(ID_CLUSTER_TOKEN)));
    std::wstring error;
    if (!gApp->UpdateConfig(config, error)) {
        MessageBoxW(gWindow, error.c_str(), L"设置未保存", MB_ICONERROR);
        return false;
    }
    MessageBoxW(gWindow, L"设置已保存。共享开关和 CPU 策略立即生效。", L"RenderMesh",
                MB_ICONINFORMATION);
    return true;
}

void RefreshLists() {
    if (!gApp) return;
    auto peers = gApp->Peers();
    Config config = gApp->GetConfig();
    HWND nodes = Item(ID_NODES);
    ListView_DeleteAllItems(nodes);
    int row = 0;
    SetCell(nodes, row, 0, Utf8ToWide(config.nodeName) + L"（本机）");
    SetCell(nodes, row, 1, L"已信任");
    SetCell(nodes, row, 2, config.shareEnabled
        ? (IdleSeconds() >= config.idleAfterSeconds ? L"空闲 · 全速" : L"活跃 · 限速")
        : L"未共享");
    SetCell(nodes, row, 3, std::to_wstring(config.activeCpuPercent) + L"%");
    SetCell(nodes, row, 4, std::to_wstring(gApp->AssignedCount()));
    SetCell(nodes, row, 5, L"127.0.0.1:" + std::to_wstring(config.httpPort));
    for (const auto& peer : peers) {
        ++row;
        SetCell(nodes, row, 0, Utf8ToWide(peer.name));
        SetCell(nodes, row, 1, peer.trusted ? L"已信任" : L"邀请码不同");
        SetCell(nodes, row, 2, peer.shareEnabled
            ? (peer.assigned ? L"正在渲染" : peer.idleSeconds >= config.idleAfterSeconds
                    ? L"空闲 · 全速" : L"活跃 · 限速")
            : L"未共享");
        SetCell(nodes, row, 3, std::to_wstring(peer.cpuPercent) + L"%");
        SetCell(nodes, row, 4, std::to_wstring(peer.assigned));
        SetCell(nodes, row, 5, Utf8ToWide(peer.address) + L":" + std::to_wstring(peer.httpPort));
    }

    HWND jobs = Item(ID_JOBS);
    ListView_DeleteAllItems(jobs);
    auto values = gApp->Jobs();
    for (int index = 0; index < static_cast<int>(values.size()); ++index) {
        const auto& job = values[static_cast<size_t>(index)];
        int completed = 0, failed = 0, active = 0;
        for (const auto& frame : job.frames) {
            completed += frame.state == TaskState::Completed;
            failed += frame.state == TaskState::Failed;
            active += frame.state == TaskState::Assigned || frame.state == TaskState::Rendering ||
                      frame.state == TaskState::Dispatching;
        }
        std::wstring state = completed == static_cast<int>(job.frames.size()) ? L"已完成"
            : failed + completed == static_cast<int>(job.frames.size()) && failed ? L"有失败"
            : active ? L"渲染中" : L"等待";
        SetCell(jobs, index, 0, Utf8ToWide(job.name));
        SetCell(jobs, index, 1, state);
        SetCell(jobs, index, 2, std::to_wstring(completed) + L" / " +
            std::to_wstring(job.frames.size()));
        SetCell(jobs, index, 3, std::to_wstring(active));
        SetCell(jobs, index, 4, Utf8ToWide(job.outputFormat));
        SetCell(jobs, index, 5,
                (gApp->DataDir() / L"jobs" / Utf8ToWide(job.id) / L"outputs").wstring());
    }
    SetText(ID_STATUS, L"在线电脑：" + std::to_wstring(peers.size() + 1) +
        L"    已分配渲染：" + std::to_wstring(gApp->AssignedCount()) +
        L"    Blender：" + (FindBlender(config).empty() ? L"未检测到" : L"已检测") +
        L"    本机空闲：" + std::to_wstring(static_cast<int>(IdleSeconds())) + L" 秒");
}

void StartSubmission() {
    if (gPreflightRunning.exchange(true)) {
        MessageBoxW(gWindow, L"已有工程正在预检。", L"RenderMesh", MB_ICONINFORMATION);
        return;
    }
    fs::path source = Text(Item(ID_SOURCE_PATH));
    std::string name = WideToUtf8(Text(Item(ID_JOB_NAME)));
    int frameStart = IntValue(ID_FRAME_START, 1);
    int frameEnd = IntValue(ID_FRAME_END, 1);
    int frameStep = IntValue(ID_FRAME_STEP, 1);
    wchar_t formatBuffer[32]{};
    GetWindowTextW(Item(ID_OUTPUT_FORMAT), formatBuffer, 32);
    std::string format = WideToUtf8(formatBuffer);
    if (!fs::exists(source) || source.extension() != L".blend" || name.empty() ||
        frameEnd < frameStart || frameStep < 1) {
        gPreflightRunning = false;
        MessageBoxW(gWindow, L"请选择 .blend 文件，并检查任务名称和帧范围。", L"无法提交",
                    MB_ICONWARNING);
        return;
    }
    EnableWindow(Item(ID_SUBMIT), FALSE);
    SetText(ID_PREFLIGHT, L"正在启动 Blender 检查插件和外部素材，并打包临时工程……");
    if (gPreflight.joinable()) gPreflight.join();
    gPreflight = std::thread([=] {
        fs::path work = gApp->DataDir() / L"preflight" / Utf8ToWide(NewRandomHex(6));
        fs::create_directories(work);
        PreflightReport preflight = PreflightBlend(
            gApp->GetConfig(), gApp->DataDir(), source, work / L"prepared.blend");
        std::wstring message = preflight.summary;
        if (preflight.passed) {
            std::wstring error;
            std::string id = gApp->SubmitPrepared(name, preflight, frameStart, frameEnd,
                                                   frameStep, format, error);
            if (id.empty()) message = L"预检通过，但任务创建失败：" + error;
            else message += L"\n任务已创建并开始分发。";
        }
        auto* result = new std::wstring(std::move(message));
        PostMessageW(gWindow, WM_RM_PREFLIGHT, preflight.passed ? 1 : 0,
                     reinterpret_cast<LPARAM>(result));
        gPreflightRunning = false;
    });
}

void BuildUi() {
    Label(L"RenderMesh", 24, 18, 240, 35, 0, gTitleFont);
    Label(L"原生 Windows · Blender 帧级渲染农场", 26, 52, 380, 22);
    Label(L"", 550, 25, 580, 26, ID_STATUS);

    AddControl(L"BUTTON", L"本机共享算力", BS_GROUPBOX, 20, 82, 360, 248, 0);
    AddControl(L"BUTTON", L"允许其他电脑使用本机算力", BS_AUTOCHECKBOX, 36, 110, 300, 24,
               ID_SHARE);
    Label(L"电脑名称", 36, 145, 100, 20);
    AddControl(L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL, 140, 142, 215, 25, ID_NODE_NAME);
    Label(L"活跃状态 CPU 硬上限", 36, 181, 190, 20);
    HWND slider = AddControl(TRACKBAR_CLASSW, L"", TBS_AUTOTICKS, 36, 205, 255, 36,
                             ID_CPU_SLIDER);
    SendMessageW(slider, TBM_SETRANGE, TRUE, MAKELONG(1, 100));
    Label(L"15%", 300, 208, 55, 24, ID_CPU_VALUE);
    Label(L"无键鼠输入后全速（分钟）", 36, 248, 210, 20);
    AddControl(L"EDIT", L"3", WS_BORDER | ES_NUMBER, 260, 245, 95, 25, ID_IDLE_MINUTES);
    Label(L"Blender 程序", 36, 285, 100, 20);
    AddControl(L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL, 140, 282, 150, 25, ID_BLENDER_PATH);
    AddControl(L"BUTTON", L"选择…", BS_PUSHBUTTON, 297, 281, 58, 27, ID_BROWSE_BLENDER);

    AddControl(L"BUTTON", L"集群与保存", BS_GROUPBOX, 20, 342, 360, 148, 0);
    Label(L"邀请码（所有电脑保持一致）", 36, 372, 260, 20);
    AddControl(L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL | ES_PASSWORD, 36, 396, 240, 25,
               ID_CLUSTER_TOKEN);
    AddControl(L"BUTTON", L"复制", BS_PUSHBUTTON, 284, 395, 70, 27, ID_COPY_TOKEN);
    AddControl(L"BUTTON", L"保存设置", BS_DEFPUSHBUTTON, 36, 444, 318, 30, ID_SAVE_SETTINGS);

    AddControl(L"BUTTON", L"提交 Blender 渲染", BS_GROUPBOX, 400, 82, 750, 248, 0);
    Label(L"工程文件", 418, 111, 80, 20);
    AddControl(L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL | ES_READONLY, 500, 108, 550, 25,
               ID_SOURCE_PATH);
    AddControl(L"BUTTON", L"选择…", BS_PUSHBUTTON, 1060, 107, 72, 27, ID_BROWSE_SOURCE);
    Label(L"任务名称", 418, 148, 80, 20);
    AddControl(L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL, 500, 145, 250, 25, ID_JOB_NAME);
    Label(L"起始帧", 770, 148, 55, 20);
    AddControl(L"EDIT", L"1", WS_BORDER | ES_NUMBER, 828, 145, 70, 25, ID_FRAME_START);
    Label(L"结束帧", 912, 148, 55, 20);
    AddControl(L"EDIT", L"100", WS_BORDER | ES_NUMBER, 970, 145, 70, 25, ID_FRAME_END);
    Label(L"步长", 1050, 148, 40, 20);
    AddControl(L"EDIT", L"1", WS_BORDER | ES_NUMBER, 1090, 145, 42, 25, ID_FRAME_STEP);
    Label(L"输出格式", 418, 185, 80, 20);
    HWND format = AddControl(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL, 500, 181, 160,
                             120, ID_OUTPUT_FORMAT);
    for (const wchar_t* value : {L"PNG", L"JPEG", L"OPEN_EXR"})
        SendMessageW(format, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(value));
    SendMessageW(format, CB_SETCURSEL, 0, 0);
    AddControl(L"BUTTON", L"预检并分发", BS_DEFPUSHBUTTON, 900, 181, 232, 31, ID_SUBMIT);
    Label(L"提交前会用 Blender 检查并打包链接素材，记录插件环境；不兼容 Worker 不会接单。",
          418, 229, 700, 22);
    AddControl(L"STATIC", L"等待选择工程", SS_LEFT, 418, 258, 714, 55, ID_PREFLIGHT);

    AddControl(L"BUTTON", L"服务器列表", BS_GROUPBOX, 20, 505, 1130, 180, 0);
    HWND nodes = AddControl(WC_LISTVIEWW, L"", LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
                            34, 530, 1102, 140, ID_NODES);
    ListView_SetExtendedListViewStyle(nodes, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);
    AddColumn(nodes, 0, 190, L"电脑"); AddColumn(nodes, 1, 100, L"连接");
    AddColumn(nodes, 2, 145, L"算力状态"); AddColumn(nodes, 3, 95, L"活动限额");
    AddColumn(nodes, 4, 90, L"已分配"); AddColumn(nodes, 5, 250, L"地址");

    AddControl(L"BUTTON", L"任务队列", BS_GROUPBOX, 20, 696, 1130, 184, 0);
    HWND jobs = AddControl(WC_LISTVIEWW, L"", LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
                           34, 721, 1102, 122, ID_JOBS);
    ListView_SetExtendedListViewStyle(jobs, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);
    AddColumn(jobs, 0, 220, L"任务"); AddColumn(jobs, 1, 95, L"状态");
    AddColumn(jobs, 2, 100, L"完成帧"); AddColumn(jobs, 3, 85, L"已分配");
    AddColumn(jobs, 4, 85, L"格式"); AddColumn(jobs, 5, 430, L"结果目录");
    AddControl(L"BUTTON", L"打开数据目录", BS_PUSHBUTTON, 986, 850, 150, 26, ID_OPEN_DATA);
}

LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
        case WM_CREATE:
            gWindow = window;
            BuildUi();
            LoadConfigIntoUi();
            SetTimer(window, 1, 1000, nullptr);
            RefreshLists();
            return 0;
        case WM_HSCROLL:
            if (reinterpret_cast<HWND>(lParam) == Item(ID_CPU_SLIDER)) {
                int value = static_cast<int>(SendMessageW(Item(ID_CPU_SLIDER), TBM_GETPOS, 0, 0));
                SetText(ID_CPU_VALUE, std::to_wstring(value) + L"%");
            }
            return 0;
        case WM_COMMAND:
            switch (LOWORD(wParam)) {
                case ID_BROWSE_BLENDER: {
                    auto path = ChooseFile(L"Blender 程序\0blender.exe\0所有程序\0*.exe\0\0",
                                           L"选择 blender.exe");
                    if (!path.empty()) SetText(ID_BLENDER_PATH, path);
                    break;
                }
                case ID_BROWSE_SOURCE: {
                    auto path = ChooseFile(L"Blender 工程\0*.blend\0\0", L"选择 Blender 工程");
                    if (!path.empty()) {
                        SetText(ID_SOURCE_PATH, path);
                        if (Text(Item(ID_JOB_NAME)).empty())
                            SetText(ID_JOB_NAME, fs::path(path).stem().wstring());
                    }
                    break;
                }
                case ID_COPY_TOKEN:
                    CopyText(Text(Item(ID_CLUSTER_TOKEN)));
                    MessageBoxW(window, L"邀请码已复制。", L"RenderMesh", MB_ICONINFORMATION);
                    break;
                case ID_SAVE_SETTINGS:
                    SaveSettings();
                    break;
                case ID_SUBMIT:
                    StartSubmission();
                    break;
                case ID_OPEN_DATA:
                    ShellExecuteW(window, L"open", gApp->DataDir().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                    break;
            }
            return 0;
        case WM_TIMER:
        case WM_RM_REFRESH:
            RefreshLists();
            return 0;
        case WM_RM_PREFLIGHT: {
            std::unique_ptr<std::wstring> result(reinterpret_cast<std::wstring*>(lParam));
            SetText(ID_PREFLIGHT, *result);
            EnableWindow(Item(ID_SUBMIT), TRUE);
            RefreshLists();
            if (!wParam) MessageBoxW(window, result->c_str(), L"工程预检未通过", MB_ICONWARNING);
            return 0;
        }
        case WM_CLOSE:
            if (gPreflightRunning.load()) {
                MessageBoxW(window, L"Blender 正在预检工程，请等待完成后再退出。", L"RenderMesh",
                            MB_ICONINFORMATION);
                return 0;
            }
            DestroyWindow(window);
            return 0;
        case WM_DESTROY:
            KillTimer(window, 1);
            PostQuitMessage(0);
            return 0;
        case WM_CTLCOLORSTATIC: {
            HDC dc = reinterpret_cast<HDC>(wParam);
            SetBkMode(dc, TRANSPARENT);
            return reinterpret_cast<LRESULT>(GetStockObject(WHITE_BRUSH));
        }
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

void PrintToParentConsole(const std::wstring& text) {
    if (!AttachConsole(ATTACH_PARENT_PROCESS)) return;
    HANDLE output = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD written = 0;
    WriteConsoleW(output, text.c_str(), static_cast<DWORD>(text.size()), &written, nullptr);
    WriteConsoleW(output, L"\r\n", 2, &written, nullptr);
    FreeConsole();
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_LISTVIEW_CLASSES | ICC_BAR_CLASSES};
    InitCommonControlsEx(&controls);
    int argumentCount = 0;
    LPWSTR* arguments = CommandLineToArgvW(GetCommandLineW(), &argumentCount);
    fs::path dataDir = DefaultDataDirectory();
    bool selfTest = false, mock = false, headless = false, noDialog = false;
    fs::path preflightSource, preflightPrepared, preflightReport, blenderOverride;
    fs::path renderSource, renderOutputDir, compatibilitySource;
    int renderFrame = 1;
    std::string renderFormat = "PNG";
    int overridePort = 0, runSeconds = 0;
    for (int i = 1; i < argumentCount; ++i) {
        std::wstring arg = arguments[i];
        if (arg == L"--self-test") selfTest = true;
        else if (arg == L"--no-dialog") noDialog = true;
        else if (arg == L"--mock-renderer") mock = true;
        else if (arg == L"--headless") headless = true;
        else if (arg == L"--data-dir" && i + 1 < argumentCount) dataDir = arguments[++i];
        else if (arg == L"--preflight" && i + 1 < argumentCount) preflightSource = arguments[++i];
        else if (arg == L"--prepared" && i + 1 < argumentCount) preflightPrepared = arguments[++i];
        else if (arg == L"--report" && i + 1 < argumentCount) preflightReport = arguments[++i];
        else if (arg == L"--blender" && i + 1 < argumentCount) blenderOverride = arguments[++i];
        else if (arg == L"--render" && i + 1 < argumentCount) renderSource = arguments[++i];
        else if (arg == L"--output-dir" && i + 1 < argumentCount) renderOutputDir = arguments[++i];
        else if (arg == L"--frame" && i + 1 < argumentCount) renderFrame = _wtoi(arguments[++i]);
        else if (arg == L"--format" && i + 1 < argumentCount) renderFormat = WideToUtf8(arguments[++i]);
        else if (arg == L"--compatibility" && i + 1 < argumentCount) compatibilitySource = arguments[++i];
        else if (arg == L"--port" && i + 1 < argumentCount) overridePort = _wtoi(arguments[++i]);
        else if (arg == L"--run-seconds" && i + 1 < argumentCount) runSeconds = _wtoi(arguments[++i]);
    }
    LocalFree(arguments);
    if (selfTest) {
        std::wstring result;
        bool ok = RunSelfTest(result);
        PrintToParentConsole(result);
        if (!noDialog)
            MessageBoxW(nullptr, result.c_str(),
                        ok ? L"RenderMesh 自检通过" : L"RenderMesh 自检失败",
                        ok ? MB_ICONINFORMATION : MB_ICONERROR);
        return ok ? 0 : 1;
    }

    if (!preflightSource.empty()) {
        ConfigStore preflightStore(dataDir);
        Config preflightConfig = preflightStore.Load();
        if (!blenderOverride.empty()) preflightConfig.blenderPath = blenderOverride;
        if (preflightPrepared.empty())
            preflightPrepared = dataDir / L"verification" / L"prepared.blend";
        if (preflightReport.empty())
            preflightReport = dataDir / L"verification" / L"preflight-result.json";
        fs::create_directories(preflightPrepared.parent_path());
        fs::create_directories(preflightReport.parent_path());
        PreflightReport result = PreflightBlend(preflightConfig, dataDir, preflightSource,
                                                preflightPrepared);
        json output = {
            {"pass", result.passed}, {"version", result.blenderVersion},
            {"render_engine", result.renderEngine},
            {"cpu_safe_while_active", result.cpuSafeWhileActive},
            {"addons", result.addons}, {"prepared", WideToUtf8(result.preparedBlend.wstring())},
            {"summary", WideToUtf8(result.summary)}, {"warnings", result.warnings},
        };
        auto dependencies = [](const std::vector<DependencyItem>& values) {
            json rows = json::array();
            for (const auto& item : values)
                rows.push_back({{"kind", item.kind}, {"owner", item.owner}, {"path", item.path}});
            return rows;
        };
        output["missing"] = dependencies(result.missing);
        output["unpacked"] = dependencies(result.unpacked);
        std::ofstream reportFile(preflightReport, std::ios::binary | std::ios::trunc);
        reportFile << output.dump(2);
        PrintToParentConsole(result.summary);
        return result.passed && reportFile ? 0 : 3;
    }

    if (!renderSource.empty()) {
        ConfigStore renderStore(dataDir);
        Config renderConfig = renderStore.Load();
        if (!blenderOverride.empty()) renderConfig.blenderPath = blenderOverride;
        if (renderOutputDir.empty()) renderOutputDir = dataDir / L"verification" / L"render";
        std::atomic<bool> stopRequested = false;
        fs::path output;
        std::wstring renderError;
        bool ok = RenderBlenderFrame(renderConfig, renderSource, renderOutputDir, renderFrame,
                                     renderFormat, false, stopRequested, output, renderError);
        PrintToParentConsole(ok ? L"渲染成功：" + output.wstring() : renderError);
        return ok ? 0 : 4;
    }

    if (!compatibilitySource.empty()) {
        if (preflightReport.empty() || !fs::exists(preflightReport)) return 5;
        ConfigStore compatibilityStore(dataDir);
        Config compatibilityConfig = compatibilityStore.Load();
        if (!blenderOverride.empty()) compatibilityConfig.blenderPath = blenderOverride;
        std::ifstream manifestFile(preflightReport, std::ios::binary);
        json manifest = json::parse(manifestFile, nullptr, false);
        if (manifest.is_discarded()) return 5;
        std::wstring summary;
        std::atomic<bool> compatibilityStop = false;
        bool ok = CheckWorkerCompatibility(
            compatibilityConfig, dataDir, compatibilitySource,
            manifest.value("version", std::string{}),
            manifest.value("render_engine", std::string{}),
            manifest.value("addons", std::vector<std::string>{}),
            compatibilityStop, summary);
        PrintToParentConsole(summary);
        return ok ? 0 : 5;
    }

    ConfigStore store(dataDir);
    Config config = store.Load();
    if (overridePort >= 1024 && overridePort <= 65535) config.httpPort = overridePort;
    gApp = std::make_unique<NodeApp>(std::move(store), config, mock);
    std::wstring error;
    if (!gApp->Start(error)) {
        MessageBoxW(nullptr, error.c_str(), L"RenderMesh 启动失败", MB_ICONERROR);
        return 2;
    }
    if (headless) {
        auto deadline = runSeconds > 0
            ? std::chrono::steady_clock::now() + std::chrono::seconds(runSeconds)
            : std::chrono::steady_clock::time_point::max();
        while (std::chrono::steady_clock::now() < deadline) Sleep(200);
        gApp->Stop();
        return 0;
    }

    gFont = CreateFontW(-16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                        DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    gTitleFont = CreateFontW(-28, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                             OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                             DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    WNDCLASSW windowClass{};
    windowClass.lpfnWndProc = WindowProc;
    windowClass.hInstance = instance;
    windowClass.lpszClassName = L"RenderMeshNativeWindow";
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    windowClass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    RegisterClassW(&windowClass);
    gWindow = CreateWindowExW(
        0, windowClass.lpszClassName, L"RenderMesh — Blender 分布式渲染农场",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        CW_USEDEFAULT, CW_USEDEFAULT, 1185, 930, nullptr, nullptr, instance, nullptr);
    gApp->SetChangedCallback([] {
        if (gWindow) PostMessageW(gWindow, WM_RM_REFRESH, 0, 0);
    });
    ShowWindow(gWindow, show);
    UpdateWindow(gWindow);
    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    gApp->SetChangedCallback({});
    gApp->Stop();
    if (gPreflight.joinable()) gPreflight.join();
    DeleteObject(gFont);
    DeleteObject(gTitleFont);
    return static_cast<int>(message.wParam);
}
