#include "Core.hpp"

#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <uxtheme.h>
#include <wincodec.h>

#include <algorithm>
#include <array>
#include <fstream>
#include <memory>

namespace {

using namespace rm;

constexpr UINT WM_RM_REFRESH = WM_APP + 1;
constexpr UINT WM_RM_PREFLIGHT = WM_APP + 2;
constexpr UINT WM_RM_PREVIEW_READY = WM_APP + 3;

enum ControlId {
    ID_SHARE = 100,
    ID_NODE_NAME,
    ID_CPU_SLIDER,
    ID_CPU_VALUE,
    ID_IDLE_SECONDS,
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
    ID_SUBMIT,
    ID_PREFLIGHT,
    ID_NODES,
    ID_JOBS,
    ID_STATUS,
    ID_OPEN_DATA,
    ID_MANUAL_ENDPOINT,
    ID_CONNECT_MANUAL,
    ID_MANUAL_STATUS,
    ID_THEME,
    ID_JOB_DETAILS,
    ID_CONTRIBUTIONS,
    ID_NAV_DASHBOARD,
    ID_NAV_TASKS,
    ID_NAV_CONTRIBUTIONS,
    ID_NAV_SETTINGS,
    ID_CONTRIBUTION_LIST,
    ID_DETAIL_FRAMES = 300,
    ID_DETAIL_SUMMARY,
    ID_DETAIL_PREVIEW,
    ID_DETAIL_OPEN_SOURCE,
    ID_DETAIL_OPEN_OUTPUT,
    ID_DETAIL_STOP,
    ID_DETAIL_PREVIEW_STATUS,
};

std::unique_ptr<NodeApp> gApp;
HWND gWindow = nullptr;
HFONT gFont = nullptr;
HFONT gTitleFont = nullptr;
std::thread gPreflight;
std::atomic<bool> gPreflightRunning = false;
std::atomic<bool> gPreflightStop = false;
HWND gDetailWindow = nullptr;
std::string gDetailJobId;
HBITMAP gPreviewBitmap = nullptr;
std::thread gPreviewThread;
std::atomic<bool> gPreviewStop = false;
std::atomic<uint64_t> gPreviewGeneration = 0;
fs::path gPreviewSource;
std::string gPreviewTaskId;
bool gRefreshingJobDetail = false;
int gCurrentPage = 0;
int gInitialPage = 0;
bool gOpenInitialDetail = false;
fs::path gUiMetricsReport;
bool gDarkTheme = false;
HBRUSH gBackgroundBrush = nullptr;
HBRUSH gSurfaceBrush = nullptr;
std::array<std::vector<HWND>, 4> gPageControls;

constexpr COLORREF kAccent = RGB(77, 69, 233);

std::wstring FormatTime(std::chrono::system_clock::time_point value) {
    if (value.time_since_epoch() == std::chrono::system_clock::duration::zero()) return L"-";
    std::time_t raw = std::chrono::system_clock::to_time_t(value);
    std::tm local{};
    localtime_s(&local, &raw);
    wchar_t text[32]{};
    wcsftime(text, std::size(text), L"%Y-%m-%d %H:%M:%S", &local);
    return text;
}

std::wstring FormatDuration(
    std::chrono::system_clock::time_point started,
    std::chrono::system_clock::time_point ended = {}) {
    if (started.time_since_epoch() == std::chrono::system_clock::duration::zero()) return L"-";
    if (ended.time_since_epoch() == std::chrono::system_clock::duration::zero())
        ended = std::chrono::system_clock::now();
    auto seconds = std::max<int64_t>(0,
        std::chrono::duration_cast<std::chrono::seconds>(ended - started).count());
    wchar_t text[40]{};
    swprintf_s(text, L"%02lld:%02lld:%02lld", seconds / 3600,
               (seconds / 60) % 60, seconds % 60);
    return text;
}

std::wstring FrameNumbers(const std::vector<int>& frames) {
    std::wstring result;
    for (int frame : frames) {
        if (!result.empty()) result += L", ";
        result += std::to_wstring(frame);
    }
    return result;
}

std::optional<RenderJob> FindJob(const std::string& id) {
    if (!gApp) return std::nullopt;
    for (auto& job : gApp->Jobs()) if (job.id == id) return job;
    return std::nullopt;
}

bool SystemUsesDarkTheme() {
    DWORD value = 1, size = sizeof(value);
    return RegGetValueW(HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
        L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &value, &size) == ERROR_SUCCESS &&
        value == 0;
}

bool ResolveDarkTheme(const Config& config) {
    return config.theme == "dark" || (config.theme == "system" && SystemUsesDarkTheme());
}

void ApplyTheme() {
    if (!gApp || !gWindow) return;
    gDarkTheme = ResolveDarkTheme(gApp->GetConfig());
    if (gBackgroundBrush) DeleteObject(gBackgroundBrush);
    if (gSurfaceBrush) DeleteObject(gSurfaceBrush);
    COLORREF background = gDarkTheme ? RGB(18, 18, 24) : RGB(246, 247, 251);
    COLORREF surface = gDarkTheme ? RGB(31, 31, 40) : RGB(255, 255, 255);
    COLORREF text = gDarkTheme ? RGB(239, 239, 247) : RGB(31, 31, 38);
    gBackgroundBrush = CreateSolidBrush(background);
    gSurfaceBrush = CreateSolidBrush(surface);
    for (int id : {ID_NODES, ID_JOBS, ID_CONTRIBUTION_LIST, ID_DETAIL_FRAMES}) {
        HWND list = id == ID_DETAIL_FRAMES && gDetailWindow
            ? GetDlgItem(gDetailWindow, id) : GetDlgItem(gWindow, id);
        if (!list) continue;
        ListView_SetBkColor(list, surface);
        ListView_SetTextBkColor(list, surface);
        ListView_SetTextColor(list, text);
    }
    BOOL dark = gDarkTheme;
    SetWindowTheme(GetDlgItem(gWindow, ID_NODES),
                   gDarkTheme ? L"DarkMode_Explorer" : L"Explorer", nullptr);
    SetWindowTheme(GetDlgItem(gWindow, ID_JOBS),
                   gDarkTheme ? L"DarkMode_Explorer" : L"Explorer", nullptr);
    SetWindowTheme(GetDlgItem(gWindow, ID_CONTRIBUTION_LIST),
                   gDarkTheme ? L"DarkMode_Explorer" : L"Explorer", nullptr);
    HMODULE dwm = LoadLibraryW(L"dwmapi.dll");
    if (dwm) {
        using SetAttribute = HRESULT(WINAPI*)(HWND, DWORD, LPCVOID, DWORD);
        auto setAttribute = reinterpret_cast<SetAttribute>(
            GetProcAddress(dwm, "DwmSetWindowAttribute"));
        if (setAttribute) {
            setAttribute(gWindow, 20, &dark, sizeof(dark));
            if (gDetailWindow) setAttribute(gDetailWindow, 20, &dark, sizeof(dark));
        }
        FreeLibrary(dwm);
    }
    InvalidateRect(gWindow, nullptr, TRUE);
    if (gDetailWindow) InvalidateRect(gDetailWindow, nullptr, TRUE);
}

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

HWND PageControl(int page, HWND control) {
    if (page >= 0 && page < static_cast<int>(gPageControls.size()))
        gPageControls[static_cast<size_t>(page)].push_back(control);
    return control;
}

void ShowPage(int page) {
    gCurrentPage = std::clamp(page, 0, 3);
    for (size_t index = 0; index < gPageControls.size(); ++index)
        for (HWND control : gPageControls[index])
            ShowWindow(control, static_cast<int>(index) == gCurrentPage ? SW_SHOW : SW_HIDE);
    for (int id : {ID_NAV_DASHBOARD, ID_NAV_TASKS, ID_NAV_CONTRIBUTIONS, ID_NAV_SETTINGS})
        InvalidateRect(Item(id), nullptr, TRUE);
}

void AddColumn(HWND list, int index, int width, const wchar_t* title) {
    LVCOLUMNW column{};
    column.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
    column.pszText = const_cast<wchar_t*>(title);
    column.cx = width;
    column.iSubItem = index;
    ListView_InsertColumn(list, index, &column);
}

void SizeListForVisibleRows(HWND list, int rows) {
    RECT bounds{};
    GetWindowRect(list, &bounds);
    int height = bounds.bottom - bounds.top;
    for (int attempt = 0; attempt < 256; ++attempt) {
        int visible = ListView_GetCountPerPage(list);
        if (visible == rows) break;
        height += visible > rows ? -1 : 1;
        SetWindowPos(list, nullptr, 0, 0, bounds.right - bounds.left, height,
                     SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
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

HBITMAP LoadPreviewBitmap(const fs::path& path, UINT maxWidth, UINT maxHeight) {
    IWICImagingFactory* factory = nullptr;
    IWICBitmapDecoder* decoder = nullptr;
    IWICBitmapFrameDecode* frame = nullptr;
    IWICBitmapScaler* scaler = nullptr;
    IWICFormatConverter* converter = nullptr;
    HBITMAP bitmap = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&factory));
    if (SUCCEEDED(hr)) hr = factory->CreateDecoderFromFilename(
        path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnLoad, &decoder);
    if (SUCCEEDED(hr)) hr = decoder->GetFrame(0, &frame);
    UINT width = 0, height = 0;
    if (SUCCEEDED(hr)) hr = frame->GetSize(&width, &height);
    if (SUCCEEDED(hr) && width && height) {
        double scale = std::min({1.0, static_cast<double>(maxWidth) / width,
                                 static_cast<double>(maxHeight) / height});
        width = std::max<UINT>(1, static_cast<UINT>(width * scale));
        height = std::max<UINT>(1, static_cast<UINT>(height * scale));
        hr = factory->CreateBitmapScaler(&scaler);
        if (SUCCEEDED(hr)) hr = scaler->Initialize(frame, width, height,
                                                   WICBitmapInterpolationModeFant);
        if (SUCCEEDED(hr)) hr = factory->CreateFormatConverter(&converter);
        if (SUCCEEDED(hr)) hr = converter->Initialize(
            scaler, GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone,
            nullptr, 0, WICBitmapPaletteTypeCustom);
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = static_cast<LONG>(width);
        info.bmiHeader.biHeight = -static_cast<LONG>(height);
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        void* pixels = nullptr;
        if (SUCCEEDED(hr)) bitmap = CreateDIBSection(nullptr, &info, DIB_RGB_COLORS,
                                                     &pixels, nullptr, 0);
        if (bitmap) {
            hr = converter->CopyPixels(nullptr, width * 4, width * height * 4,
                                       static_cast<BYTE*>(pixels));
            if (FAILED(hr)) {
                DeleteObject(bitmap);
                bitmap = nullptr;
            }
        }
    }
    if (converter) converter->Release();
    if (scaler) scaler->Release();
    if (frame) frame->Release();
    if (decoder) decoder->Release();
    if (factory) factory->Release();
    return bitmap;
}

void ReplacePreviewBitmap(HBITMAP next) {
    HWND preview = gDetailWindow ? GetDlgItem(gDetailWindow, ID_DETAIL_PREVIEW) : nullptr;
    if (!preview) {
        if (next) DeleteObject(next);
        if (gPreviewBitmap) DeleteObject(gPreviewBitmap);
        gPreviewBitmap = nullptr;
        return;
    }
    HBITMAP tracked = gPreviewBitmap;
    HBITMAP old = reinterpret_cast<HBITMAP>(
        SendMessageW(preview, STM_SETIMAGE, IMAGE_BITMAP, reinterpret_cast<LPARAM>(next)));
    gPreviewBitmap = next;
    if (old && old != next) DeleteObject(old);
    if (tracked && tracked != old && tracked != next) DeleteObject(tracked);
}

struct PreviewResult {
    uint64_t generation = 0;
    fs::path source;
    fs::path preview;
    bool ok = false;
    std::wstring error;
};

void StopPreviewGeneration() {
    ++gPreviewGeneration;
    gPreviewStop = true;
    if (gPreviewThread.joinable()) gPreviewThread.join();
    MSG stale{};
    while (gDetailWindow && PeekMessageW(
               &stale, gDetailWindow, WM_RM_PREVIEW_READY, WM_RM_PREVIEW_READY, PM_REMOVE))
        delete reinterpret_cast<PreviewResult*>(stale.lParam);
    gPreviewStop = false;
}

void ShowInExplorer(const fs::path& path) {
    if (path.empty()) return;
    fs::path target = fs::is_directory(path) ? path : path.parent_path();
    if (fs::is_regular_file(path)) {
        std::wstring parameters = L"/select,\"" + path.wstring() + L"\"";
        ShellExecuteW(gWindow, L"open", L"explorer.exe", parameters.c_str(), nullptr,
                      SW_SHOWNORMAL);
    } else if (!target.empty()) {
        ShellExecuteW(gWindow, L"open", target.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }
}

void UpdateFramePreview();

void RefreshJobDetail(bool preserveSelection = true) {
    if (!gDetailWindow) return;
    auto jobValue = FindJob(gDetailJobId);
    if (!jobValue) {
        DestroyWindow(gDetailWindow);
        return;
    }
    const auto& job = *jobValue;
    gRefreshingJobDetail = true;
    HWND list = GetDlgItem(gDetailWindow, ID_DETAIL_FRAMES);
    int selectedFrame = -1;
    if (preserveSelection) {
        int selected = ListView_GetNextItem(list, -1, LVNI_SELECTED);
        if (selected >= 0 && selected < static_cast<int>(job.frames.size()))
            selectedFrame = job.frames[static_cast<size_t>(selected)].frame;
    }
    int completed = 0, failed = 0, stopped = 0, active = 0;
    ListView_DeleteAllItems(list);
    for (int index = 0; index < static_cast<int>(job.frames.size()); ++index) {
        const auto& frame = job.frames[static_cast<size_t>(index)];
        completed += frame.state == TaskState::Completed;
        failed += frame.state == TaskState::Failed;
        stopped += frame.state == TaskState::Stopped;
        active += frame.state == TaskState::Assigned || frame.state == TaskState::Rendering ||
                  frame.state == TaskState::Dispatching;
        SetCell(list, index, 0, std::to_wstring(frame.frame));
        SetCell(list, index, 1, TaskStateLabel(frame.state));
        SetCell(list, index, 2, Utf8ToWide(frame.assignedNodeName));
        SetCell(list, index, 3, Utf8ToWide(frame.assignedPackageId));
        SetCell(list, index, 4, FormatDuration(frame.startedAt, frame.endedAt));
        SetCell(list, index, 5, FormatTime(frame.startedAt));
        SetCell(list, index, 6, FormatTime(frame.endedAt));
        SetCell(list, index, 7, frame.error.empty() ? frame.output.wstring()
                                                    : Utf8ToWide(frame.error));
        if (frame.frame == selectedFrame)
            ListView_SetItemState(list, index, LVIS_SELECTED | LVIS_FOCUSED,
                                  LVIS_SELECTED | LVIS_FOCUSED);
    }
    if (!preserveSelection && !job.frames.empty())
        ListView_SetItemState(list, 0, LVIS_SELECTED | LVIS_FOCUSED,
                              LVIS_SELECTED | LVIS_FOCUSED);
    std::wstring summary = Utf8ToWide(job.name) + L"    总帧数 " +
        std::to_wstring(job.frames.size()) + L"    已完成 " + std::to_wstring(completed) +
        L"    渲染中 " + std::to_wstring(active) + L"    错误 " +
        std::to_wstring(failed) + L"    已停止 " + std::to_wstring(stopped) +
        L"    提交时间 " + FormatTime(job.createdAt);
    SetWindowTextW(GetDlgItem(gDetailWindow, ID_DETAIL_SUMMARY), summary.c_str());
    gRefreshingJobDetail = false;
    UpdateFramePreview();
}

void UpdateFramePreview() {
    if (!gDetailWindow) return;
    auto job = FindJob(gDetailJobId);
    if (!job) return;
    HWND list = GetDlgItem(gDetailWindow, ID_DETAIL_FRAMES);
    int selected = ListView_GetNextItem(list, -1, LVNI_SELECTED);
    if (selected < 0 || selected >= static_cast<int>(job->frames.size())) {
        StopPreviewGeneration();
        gPreviewSource.clear();
        gPreviewTaskId.clear();
        ReplacePreviewBitmap(nullptr);
        SetWindowTextW(GetDlgItem(gDetailWindow, ID_DETAIL_PREVIEW_STATUS),
                       L"选择一帧查看预览");
        return;
    }
    const auto& frame = job->frames[static_cast<size_t>(selected)];
    if (frame.taskId == gPreviewTaskId && frame.output == gPreviewSource) return;
    StopPreviewGeneration();
    gPreviewSource.clear();
    gPreviewTaskId = frame.taskId;
    ReplacePreviewBitmap(nullptr);
    if (frame.output.empty() || !fs::is_regular_file(frame.output)) {
        SetWindowTextW(GetDlgItem(gDetailWindow, ID_DETAIL_PREVIEW_STATUS),
                       L"该帧尚未生成图像");
        return;
    }
    gPreviewSource = frame.output;
    if (HBITMAP bitmap = LoadPreviewBitmap(frame.output, 330, 220)) {
        ReplacePreviewBitmap(bitmap);
        SetWindowTextW(GetDlgItem(gDetailWindow, ID_DETAIL_PREVIEW_STATUS),
                       (L"帧 " + std::to_wstring(frame.frame)).c_str());
        return;
    }
    std::error_code ignored;
    uint64_t fileSize = fs::file_size(frame.output, ignored);
    auto modified = fs::last_write_time(frame.output, ignored).time_since_epoch().count();
    std::string cacheKey = HmacSha256(
        "RenderMesh-preview-cache-v1",
        WideToUtf8(frame.output.wstring()) + "|" + std::to_string(fileSize) + "|" +
            std::to_string(static_cast<long long>(modified)));
    fs::path previewPath = gApp->DataDir() / L"previews" / Utf8ToWide(cacheKey + ".png");
    if (fs::is_regular_file(previewPath)) {
        if (HBITMAP bitmap = LoadPreviewBitmap(previewPath, 330, 220)) {
            ReplacePreviewBitmap(bitmap);
            SetWindowTextW(GetDlgItem(gDetailWindow, ID_DETAIL_PREVIEW_STATUS),
                           (L"帧 " + std::to_wstring(frame.frame) + L" · Blender 预览").c_str());
            return;
        }
        fs::remove(previewPath, ignored);
    }
    SetWindowTextW(GetDlgItem(gDetailWindow, ID_DETAIL_PREVIEW_STATUS),
                   L"正在用 Blender 生成预览……");
    const uint64_t generation = ++gPreviewGeneration;
    const fs::path source = frame.output;
    const Config config = gApp->GetConfig();
    const fs::path dataDir = gApp->DataDir();
    const HWND detail = gDetailWindow;
    gPreviewThread = std::thread([=] {
        auto* result = new PreviewResult;
        result->generation = generation;
        result->source = source;
        result->preview = previewPath;
        result->ok = GeneratePreviewImage(
            config, dataDir, source, previewPath, gPreviewStop, result->error);
        if (gPreviewStop.load() || generation != gPreviewGeneration.load()) {
            delete result;
            return;
        }
        if (!PostMessageW(detail, WM_RM_PREVIEW_READY, 0,
                          reinterpret_cast<LPARAM>(result)))
            delete result;
    });
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
    SetText(ID_IDLE_SECONDS, std::to_wstring(config.idleAfterSeconds));
    SetText(ID_BLENDER_PATH, config.blenderPath.wstring());
    SetText(ID_CLUSTER_TOKEN, Utf8ToWide(config.inviteCode));
    SetText(ID_MANUAL_ENDPOINT, Utf8ToWide(config.manualPeerEndpoint));
    int themeSelection = config.theme == "light" ? 1 : config.theme == "dark" ? 2 : 0;
    SendMessageW(Item(ID_THEME), CB_SETCURSEL, themeSelection, 0);
}

Config ReadConfigFromUi() {
    Config config = gApp->GetConfig();
    config.nodeName = WideToUtf8(Text(Item(ID_NODE_NAME)));
    config.activeCpuPercent = static_cast<int>(SendMessageW(Item(ID_CPU_SLIDER), TBM_GETPOS, 0, 0));
    config.idleAfterSeconds = std::clamp(IntValue(ID_IDLE_SECONDS, 180), 1, 86400);
    config.blenderPath = Text(Item(ID_BLENDER_PATH));
    config.inviteCode = WideToUtf8(Text(Item(ID_CLUSTER_TOKEN)));
    int themeSelection = static_cast<int>(SendMessageW(Item(ID_THEME), CB_GETCURSEL, 0, 0));
    config.theme = themeSelection == 1 ? "light" : themeSelection == 2 ? "dark" : "system";
    return config;
}

bool SaveSettings() {
    Config config = ReadConfigFromUi();
    std::wstring error;
    if (!gApp->UpdateConfig(config, error)) {
        MessageBoxW(gWindow, error.c_str(), L"设置未保存", MB_ICONERROR);
        return false;
    }
    Config saved = gApp->GetConfig();
    ApplyTheme();
    MessageBoxW(gWindow,
                saved.pairingPending
                    ? L"邀请码已保存。请在“手动连接”中填写一台已加入该集群的电脑 IP 完成配对。"
                    : L"设置已保存。共享开关和空闲策略立即生效。",
                L"RenderMesh", MB_ICONINFORMATION);
    return true;
}

void ConnectManualPeer() {
    Config config = ReadConfigFromUi();
    std::wstring result;
    if (!gApp->UpdateConfig(config, result)) {
        SetText(ID_MANUAL_STATUS, result);
        MessageBoxW(gWindow, result.c_str(), L"设置无效", MB_ICONERROR);
        return;
    }
    bool connected = gApp->AddManualPeer(
        WideToUtf8(Text(Item(ID_MANUAL_ENDPOINT))), result);
    SetText(ID_MANUAL_STATUS, result);
    if (connected) {
        SetText(ID_MANUAL_ENDPOINT, Utf8ToWide(gApp->GetConfig().manualPeerEndpoint));
    } else {
        MessageBoxW(gWindow, result.c_str(), L"手动连接失败", MB_ICONWARNING);
    }
}

void RefreshLists() {
    if (!gApp) return;
    auto peers = gApp->Peers();
    HardwareInfo hardware = gApp->Hardware();
    Config config = gApp->GetConfig();
    std::wstring manualStatus = gApp->ManualPeerStatus();
    if (!manualStatus.empty()) SetText(ID_MANUAL_STATUS, manualStatus);
    HWND nodes = Item(ID_NODES);
    ListView_DeleteAllItems(nodes);
    int row = 0;
    SetCell(nodes, row, 0, Utf8ToWide(config.nodeName) + L"（本机）");
    SetCell(nodes, row, 1, L"已信任");
    SetCell(nodes, row, 2, config.shareEnabled
        ? (IdleSeconds() >= config.idleAfterSeconds ? L"空闲 · GPU可用" : L"活跃 · GPU等待")
        : L"未共享");
    SetCell(nodes, row, 3, std::to_wstring(config.activeCpuPercent) + L"%");
    SetCell(nodes, row, 4, std::to_wstring(gApp->AssignedCount()));
    SetCell(nodes, row, 5, Utf8ToWide(hardware.cpuModel));
    std::wstring localGpus;
    for (const auto& gpu : hardware.gpuModels) {
        if (!localGpus.empty()) localGpus += L" / ";
        localGpus += Utf8ToWide(gpu);
    }
    SetCell(nodes, row, 6, localGpus.empty() ? L"未检测到" : localGpus);
    SetCell(nodes, row, 7, LocalEndpointSummary(config.httpPort));
    for (const auto& peer : peers) {
        ++row;
        SetCell(nodes, row, 0, Utf8ToWide(peer.name));
        SetCell(nodes, row, 1, peer.trusted ? L"已信任" : L"邀请码不同");
        SetCell(nodes, row, 2, peer.shareEnabled
            ? (peer.assigned ? L"正在渲染" : peer.idleSeconds >= config.idleAfterSeconds
                    ? L"空闲 · GPU可用" : L"活跃 · GPU等待")
            : L"未共享");
        SetCell(nodes, row, 3, std::to_wstring(peer.cpuPercent) + L"%");
        SetCell(nodes, row, 4, std::to_wstring(peer.assigned));
        SetCell(nodes, row, 5, peer.cpuModel.empty() ? L"未报告" : Utf8ToWide(peer.cpuModel));
        std::wstring remoteGpus;
        for (const auto& gpu : peer.gpuModels) {
            if (!remoteGpus.empty()) remoteGpus += L" / ";
            remoteGpus += Utf8ToWide(gpu);
        }
        SetCell(nodes, row, 6, remoteGpus.empty() ? L"未报告" : remoteGpus);
        SetCell(nodes, row, 7, Utf8ToWide(peer.address) + L":" + std::to_wstring(peer.httpPort));
    }

    HWND jobs = Item(ID_JOBS);
    ListView_DeleteAllItems(jobs);
    auto values = gApp->Jobs();
    for (int index = 0; index < static_cast<int>(values.size()); ++index) {
        const auto& job = values[static_cast<size_t>(index)];
        int completed = 0, failed = 0, stopped = 0, active = 0;
        std::chrono::system_clock::time_point started{}, ended{};
        for (const auto& frame : job.frames) {
            completed += frame.state == TaskState::Completed;
            failed += frame.state == TaskState::Failed;
            stopped += frame.state == TaskState::Stopped;
            active += frame.state == TaskState::Assigned || frame.state == TaskState::Rendering ||
                      frame.state == TaskState::Dispatching;
            if (frame.startedAt.time_since_epoch() != std::chrono::system_clock::duration::zero() &&
                (started.time_since_epoch() == std::chrono::system_clock::duration::zero() ||
                 frame.startedAt < started))
                started = frame.startedAt;
            if (frame.endedAt > ended) ended = frame.endedAt;
        }
        std::wstring state = completed == static_cast<int>(job.frames.size()) ? L"已完成"
            : failed ? L"有错误" : stopped ? L"已停止"
            : active ? L"渲染中" : L"待提交";
        SetCell(jobs, index, 0, Utf8ToWide(job.name));
        SetCell(jobs, index, 1, state);
        SetCell(jobs, index, 2, std::to_wstring(job.frames.size()));
        SetCell(jobs, index, 3, std::to_wstring(completed) + L" / " +
            std::to_wstring(job.frames.size()));
        SetCell(jobs, index, 4, FormatDuration(started,
            active ? std::chrono::system_clock::time_point{} : ended));
        SetCell(jobs, index, 5, FormatTime(started));
        SetCell(jobs, index, 6, active ? L"-" : FormatTime(ended));
    }
    HWND contributions = Item(ID_CONTRIBUTION_LIST);
    ListView_DeleteAllItems(contributions);
    auto records = gApp->Contributions();
    for (int index = 0; index < static_cast<int>(records.size()); ++index) {
        const auto& record = records[records.size() - 1 - static_cast<size_t>(index)];
        std::wstring state = record.state == "completed" ? L"已完成"
            : record.state == "stopped" ? L"活跃后已停止"
            : record.state == "partial" ? L"部分完成" : L"失败";
        SetCell(contributions, index, 0, Utf8ToWide(record.jobName));
        SetCell(contributions, index, 1, Utf8ToWide(record.packageId));
        SetCell(contributions, index, 2, Utf8ToWide(record.originName));
        SetCell(contributions, index, 3, FrameNumbers(record.frames));
        SetCell(contributions, index, 4, FrameNumbers(record.completedFrames));
        SetCell(contributions, index, 5, state);
        SetCell(contributions, index, 6, FormatDuration(record.startedAt, record.endedAt));
        SetCell(contributions, index, 7, FormatTime(record.startedAt));
        SetCell(contributions, index, 8, FormatTime(record.endedAt));
        SetCell(contributions, index, 9, Utf8ToWide(record.error));
    }
    SetText(ID_STATUS, std::wstring(config.pairingPending ? L"正在邀请码配对    " : L"") +
        L"在线电脑：" + std::to_wstring(peers.size() + 1) +
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
    gPreflightStop = false;
    gPreflight = std::thread([=] {
        fs::path work = gApp->DataDir() / L"preflight" / Utf8ToWide(NewRandomHex(6));
        fs::create_directories(work);
        PreflightReport preflight = PreflightBlend(
            gApp->GetConfig(), gApp->DataDir(), source, work / L"prepared.blend",
            frameStart, frameEnd, frameStep, &gPreflightStop);
        std::wstring message = preflight.summary;
        if (preflight.passed) {
            std::wstring error;
            std::string id = gApp->SubmitPrepared(name, preflight, frameStart, frameEnd,
                                                   frameStep, error);
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
    for (auto& page : gPageControls) page.clear();
    Label(L"RenderMesh", 24, 20, 180, 32, 0, gTitleFont);
    Label(L"Blender 渲染网格", 26, 54, 170, 22);
    AddControl(L"BUTTON", L"共享本机算力", BS_OWNERDRAW,
               12, 88, 196, 76, ID_SHARE);
    AddControl(L"BUTTON", L"服务器", BS_OWNERDRAW, 18, 196, 184, 42, ID_NAV_DASHBOARD);
    AddControl(L"BUTTON", L"渲染任务", BS_OWNERDRAW, 18, 244, 184, 42, ID_NAV_TASKS);
    AddControl(L"BUTTON", L"本机贡献", BS_OWNERDRAW, 18, 292, 184, 42, ID_NAV_CONTRIBUTIONS);
    AddControl(L"BUTTON", L"设置", BS_OWNERDRAW, 18, 340, 184, 42, ID_NAV_SETTINGS);
    Label(L"主色  #4D45E9", 28, 744, 165, 22);

    // 服务器页
    PageControl(0, Label(L"服务器", 248, 24, 250, 36, 0, gTitleFont));
    PageControl(0, Label(L"", 250, 68, 980, 28, ID_STATUS));
    PageControl(0, Label(L"手动连接", 250, 116, 80, 24));
    PageControl(0, AddControl(L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL,
                              334, 112, 235, 28, ID_MANUAL_ENDPOINT));
    PageControl(0, AddControl(L"BUTTON", L"连接", BS_PUSHBUTTON,
                              579, 111, 82, 30, ID_CONNECT_MANUAL));
    PageControl(0, Label(L"填写另一台电脑的局域网 IP:端口，不要填 127.0.0.1",
                         680, 116, 550, 24, ID_MANUAL_STATUS));
    HWND nodes = PageControl(0, AddControl(
        WC_LISTVIEWW, L"", LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
        250, 162, 990, 530, ID_NODES));
    ListView_SetExtendedListViewStyle(nodes,
        LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES | LVS_EX_DOUBLEBUFFER);
    AddColumn(nodes, 0, 170, L"电脑"); AddColumn(nodes, 1, 85, L"连接");
    AddColumn(nodes, 2, 140, L"算力状态"); AddColumn(nodes, 3, 85, L"活动限额");
    AddColumn(nodes, 4, 75, L"已分配"); AddColumn(nodes, 5, 220, L"CPU 型号");
    AddColumn(nodes, 6, 240, L"GPU 型号"); AddColumn(nodes, 7, 210, L"地址");

    // 渲染任务页
    PageControl(1, Label(L"渲染任务", 248, 24, 300, 36, 0, gTitleFont));
    PageControl(1, Label(L"工程文件", 250, 86, 80, 24));
    PageControl(1, AddControl(L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL | ES_READONLY,
                              334, 82, 790, 28, ID_SOURCE_PATH));
    PageControl(1, AddControl(L"BUTTON", L"选择…", BS_PUSHBUTTON,
                              1134, 81, 92, 30, ID_BROWSE_SOURCE));
    PageControl(1, Label(L"任务名称", 250, 130, 80, 24));
    PageControl(1, AddControl(L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL,
                              334, 126, 300, 28, ID_JOB_NAME));
    PageControl(1, Label(L"起始帧", 656, 130, 55, 24));
    PageControl(1, AddControl(L"EDIT", L"1", WS_BORDER | ES_NUMBER,
                              714, 126, 65, 28, ID_FRAME_START));
    PageControl(1, Label(L"结束帧", 795, 130, 55, 24));
    PageControl(1, AddControl(L"EDIT", L"100", WS_BORDER | ES_NUMBER,
                              853, 126, 65, 28, ID_FRAME_END));
    PageControl(1, Label(L"步长", 934, 130, 42, 24));
    PageControl(1, AddControl(L"EDIT", L"1", WS_BORDER | ES_NUMBER,
                              978, 126, 55, 28, ID_FRAME_STEP));
    PageControl(1, AddControl(L"BUTTON", L"预检并提交渲染", BS_OWNERDRAW,
                              1049, 124, 177, 34, ID_SUBMIT));
    PageControl(1, Label(L"输出格式由 Blender 工程决定；提交前检查并打包链接素材、记录插件环境。",
                         250, 178, 976, 24));
    PageControl(1, AddControl(L"STATIC", L"等待选择工程", SS_LEFT,
                              250, 210, 976, 45, ID_PREFLIGHT));
    PageControl(1, Label(L"任务队列（可见 10 个槽位，超出后滚动）",
                         250, 274, 440, 26));
    HWND jobs = PageControl(1, AddControl(
        WC_LISTVIEWW, L"", LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
        250, 304, 976, 350, ID_JOBS));
    ListView_SetExtendedListViewStyle(jobs,
        LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES | LVS_EX_DOUBLEBUFFER);
    AddColumn(jobs, 0, 210, L"任务"); AddColumn(jobs, 1, 95, L"状态");
    AddColumn(jobs, 2, 75, L"总帧数"); AddColumn(jobs, 3, 95, L"完成");
    AddColumn(jobs, 4, 100, L"渲染时长"); AddColumn(jobs, 5, 155, L"开始时间");
    AddColumn(jobs, 6, 155, L"结束时间");
    SizeListForVisibleRows(jobs, 10);
    RECT jobBounds{};
    GetWindowRect(jobs, &jobBounds);
    MapWindowPoints(HWND_DESKTOP, gWindow, reinterpret_cast<POINT*>(&jobBounds), 2);
    int jobActionY = jobBounds.bottom + 18;
    PageControl(1, AddControl(L"BUTTON", L"打开任务详情", BS_PUSHBUTTON,
                              892, jobActionY, 160, 32, ID_JOB_DETAILS));
    PageControl(1, AddControl(L"BUTTON", L"打开数据目录", BS_PUSHBUTTON,
                              1062, jobActionY, 164, 32, ID_OPEN_DATA));

    // 本机贡献页
    PageControl(2, Label(L"本机贡献", 248, 24, 300, 36, 0, gTitleFont));
    PageControl(2, Label(L"这里只记录本机替其他电脑渲染的任务包和帧，重启后仍保留。",
                         250, 72, 920, 24));
    HWND contributionList = PageControl(2, AddControl(
        WC_LISTVIEWW, L"", LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
        250, 112, 976, 580, ID_CONTRIBUTION_LIST));
    ListView_SetExtendedListViewStyle(contributionList,
        LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES | LVS_EX_DOUBLEBUFFER);
    AddColumn(contributionList, 0, 180, L"任务");
    AddColumn(contributionList, 1, 125, L"任务包");
    AddColumn(contributionList, 2, 150, L"来源电脑");
    AddColumn(contributionList, 3, 140, L"分配帧");
    AddColumn(contributionList, 4, 140, L"已贡献帧");
    AddColumn(contributionList, 5, 105, L"状态");
    AddColumn(contributionList, 6, 95, L"时长");
    AddColumn(contributionList, 7, 155, L"开始时间");
    AddColumn(contributionList, 8, 155, L"结束时间");
    AddColumn(contributionList, 9, 300, L"日志 / 错误");

    // 设置页
    PageControl(3, Label(L"设置", 248, 24, 250, 36, 0, gTitleFont));
    PageControl(3, Label(L"电脑名称", 270, 100, 155, 24));
    PageControl(3, AddControl(L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL,
                              445, 96, 350, 30, ID_NODE_NAME));
    PageControl(3, Label(L"活跃状态 CPU 任务上限", 270, 154, 190, 24));
    HWND slider = PageControl(3, AddControl(TRACKBAR_CLASSW, L"", TBS_AUTOTICKS,
                                            445, 146, 285, 38, ID_CPU_SLIDER));
    SendMessageW(slider, TBM_SETRANGE, TRUE, MAKELONG(1, 100));
    PageControl(3, Label(L"15%", 742, 154, 55, 24, ID_CPU_VALUE));
    PageControl(3, Label(L"无键鼠输入后启用 GPU（秒）", 270, 208, 240, 24));
    PageControl(3, AddControl(L"EDIT", L"180", WS_BORDER | ES_NUMBER,
                              520, 204, 110, 30, ID_IDLE_SECONDS));
    PageControl(3, Label(L"Blender 程序", 270, 262, 155, 24));
    PageControl(3, AddControl(L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL,
                              445, 258, 650, 30, ID_BLENDER_PATH));
    PageControl(3, AddControl(L"BUTTON", L"选择…", BS_PUSHBUTTON,
                              1105, 257, 90, 32, ID_BROWSE_BLENDER));
    PageControl(3, Label(L"六位邀请码", 270, 316, 155, 24));
    PageControl(3, AddControl(L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL | ES_NUMBER,
                              445, 312, 240, 30, ID_CLUSTER_TOKEN));
    PageControl(3, AddControl(L"BUTTON", L"复制", BS_PUSHBUTTON,
                              695, 311, 90, 32, ID_COPY_TOKEN));
    PageControl(3, Label(L"外观", 270, 370, 155, 24));
    HWND theme = PageControl(3, AddControl(L"COMBOBOX", L"",
        CBS_DROPDOWNLIST | WS_VSCROLL, 445, 366, 240, 120, ID_THEME));
    SendMessageW(theme, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"跟随系统"));
    SendMessageW(theme, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"浅色"));
    SendMessageW(theme, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"深色"));
    PageControl(3, Label(L"端口 8391 · 后台日志按天保存在数据目录",
                         270, 430, 600, 24));
    PageControl(3, AddControl(L"BUTTON", L"保存设置", BS_OWNERDRAW,
                              445, 486, 240, 40, ID_SAVE_SETTINGS));
    ShowPage(gInitialPage);
}

void DrawOwnerButton(const DRAWITEMSTRUCT& item) {
    RECT rect = item.rcItem;
    int id = static_cast<int>(item.CtlID);
    bool share = id == ID_SHARE;
    bool primary = id == ID_SUBMIT || id == ID_SAVE_SETTINGS;
    int navPage = id == ID_NAV_DASHBOARD ? 0 : id == ID_NAV_TASKS ? 1
        : id == ID_NAV_CONTRIBUTIONS ? 2 : id == ID_NAV_SETTINGS ? 3 : -1;
    bool selected = navPage == gCurrentPage;
    bool pressed = (item.itemState & ODS_SELECTED) != 0;
    COLORREF surface = gDarkTheme ? RGB(31, 31, 40) : RGB(255, 255, 255);
    COLORREF hover = gDarkTheme ? RGB(47, 46, 64) : RGB(235, 234, 255);
    COLORREF fill = primary || selected ? kAccent : pressed ? hover : surface;
    HBRUSH brush = CreateSolidBrush(fill);
    HPEN pen = CreatePen(PS_SOLID, 1,
        primary || selected ? kAccent : gDarkTheme ? RGB(61, 61, 74) : RGB(222, 224, 232));
    HGDIOBJ oldBrush = SelectObject(item.hDC, brush);
    HGDIOBJ oldPen = SelectObject(item.hDC, pen);
    RoundRect(item.hDC, rect.left, rect.top, rect.right, rect.bottom, 12, 12);
    SelectObject(item.hDC, oldBrush);
    SelectObject(item.hDC, oldPen);
    DeleteObject(brush);
    DeleteObject(pen);
    SetBkMode(item.hDC, TRANSPARENT);
    if (share) {
        Config config = gApp->GetConfig();
        SetTextColor(item.hDC, gDarkTheme ? RGB(239, 239, 247) : RGB(31, 31, 38));
        RECT label = rect;
        label.left += 14;
        label.right -= 72;
        std::wstring shareText = config.shareEnabled ? L"共享本机算力\n已开启"
                                                     : L"共享本机算力\n已关闭";
        DrawTextW(item.hDC, shareText.c_str(), -1, &label,
                  DT_LEFT | DT_VCENTER | DT_WORDBREAK);
        RECT track{rect.right - 62, rect.top + 25, rect.right - 14, rect.top + 51};
        HBRUSH trackBrush = CreateSolidBrush(config.shareEnabled ? kAccent
            : gDarkTheme ? RGB(91, 91, 103) : RGB(186, 188, 198));
        HGDIOBJ old = SelectObject(item.hDC, trackBrush);
        HGDIOBJ oldTrackPen = SelectObject(item.hDC, GetStockObject(NULL_PEN));
        RoundRect(item.hDC, track.left, track.top, track.right, track.bottom, 26, 26);
        SelectObject(item.hDC, old);
        DeleteObject(trackBrush);
        HBRUSH knob = CreateSolidBrush(RGB(255, 255, 255));
        old = SelectObject(item.hDC, knob);
        int knobLeft = config.shareEnabled ? track.right - 24 : track.left + 3;
        Ellipse(item.hDC, knobLeft, track.top + 3, knobLeft + 20, track.top + 23);
        SelectObject(item.hDC, old);
        SelectObject(item.hDC, oldTrackPen);
        DeleteObject(knob);
        return;
    }
    wchar_t text[128]{};
    GetWindowTextW(item.hwndItem, text, static_cast<int>(std::size(text)));
    SetTextColor(item.hDC, primary || selected ? RGB(255, 255, 255)
        : gDarkTheme ? RGB(239, 239, 247) : RGB(31, 31, 38));
    DrawTextW(item.hDC, text, -1, &rect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

void WriteUiMetricsReport() {
    if (gUiMetricsReport.empty()) return;
    HWND jobs = Item(ID_JOBS);
    HWND share = Item(ID_SHARE);
    RECT shareRect{};
    GetClientRect(share, &shareRect);
    RECT label = shareRect;
    label.left += 14;
    label.right -= 72;
    Config config = gApp->GetConfig();
    std::wstring shareText = config.shareEnabled ? L"共享本机算力\n已开启"
                                                 : L"共享本机算力\n已关闭";
    HDC dc = GetDC(share);
    HGDIOBJ oldFont = SelectObject(dc, gFont);
    RECT measured{0, 0, label.right - label.left, 0};
    DrawTextW(dc, shareText.c_str(), -1, &measured,
              DT_LEFT | DT_WORDBREAK | DT_CALCRECT);
    SelectObject(dc, oldFont);
    ReleaseDC(share, dc);
    json metrics = {
        {"jobs_visible_rows", ListView_GetCountPerPage(jobs)},
        {"share_text_width", measured.right - measured.left},
        {"share_text_height", measured.bottom - measured.top},
        {"share_label_width", label.right - label.left},
        {"share_label_height", label.bottom - label.top},
        {"share_text_fits", measured.right - measured.left <= label.right - label.left &&
                            measured.bottom - measured.top <= label.bottom - label.top},
    };
    std::error_code ignored;
    fs::create_directories(gUiMetricsReport.parent_path(), ignored);
    std::ofstream output(gUiMetricsReport, std::ios::binary | std::ios::trunc);
    output << metrics.dump(2);
}

LRESULT CALLBACK DetailWindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
        case WM_CREATE: {
            gDetailWindow = window;
            HWND summary = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE,
                22, 18, 1130, 28, window,
                reinterpret_cast<HMENU>(ID_DETAIL_SUMMARY), GetModuleHandleW(nullptr), nullptr);
            SendMessageW(summary, WM_SETFONT, reinterpret_cast<WPARAM>(gFont), TRUE);
            HWND frames = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
                WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
                22, 55, 805, 520, window,
                reinterpret_cast<HMENU>(ID_DETAIL_FRAMES), GetModuleHandleW(nullptr), nullptr);
            SendMessageW(frames, WM_SETFONT, reinterpret_cast<WPARAM>(gFont), TRUE);
            ListView_SetExtendedListViewStyle(frames,
                LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES | LVS_EX_DOUBLEBUFFER);
            AddColumn(frames, 0, 55, L"帧");
            AddColumn(frames, 1, 82, L"状态");
            AddColumn(frames, 2, 135, L"渲染电脑");
            AddColumn(frames, 3, 115, L"任务包");
            AddColumn(frames, 4, 85, L"渲染时长");
            AddColumn(frames, 5, 145, L"开始时间");
            AddColumn(frames, 6, 145, L"结束时间");
            AddColumn(frames, 7, 330, L"输出 / 错误");
            HWND preview = CreateWindowExW(WS_EX_CLIENTEDGE, L"STATIC", L"",
                WS_CHILD | WS_VISIBLE | SS_BITMAP | SS_CENTERIMAGE,
                845, 55, 330, 230, window,
                reinterpret_cast<HMENU>(ID_DETAIL_PREVIEW), GetModuleHandleW(nullptr), nullptr);
            SendMessageW(preview, WM_SETFONT, reinterpret_cast<WPARAM>(gFont), TRUE);
            HWND previewStatus = CreateWindowExW(0, L"STATIC", L"选择一帧查看预览",
                WS_CHILD | WS_VISIBLE | SS_CENTER, 845, 289, 330, 20, window,
                reinterpret_cast<HMENU>(ID_DETAIL_PREVIEW_STATUS),
                GetModuleHandleW(nullptr), nullptr);
            SendMessageW(previewStatus, WM_SETFONT, reinterpret_cast<WPARAM>(gFont), TRUE);
            auto button = [&](const wchar_t* text, int y, int id) {
                HWND control = CreateWindowExW(0, L"BUTTON", text,
                    WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 845, y, 330, 34, window,
                    reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                    GetModuleHandleW(nullptr), nullptr);
                SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(gFont), TRUE);
            };
            button(L"在资源管理器中显示选中帧", 321, ID_DETAIL_OPEN_OUTPUT);
            button(L"在资源管理器中显示源 .blend", 365, ID_DETAIL_OPEN_SOURCE);
            button(L"停止本任务", 421, ID_DETAIL_STOP);
            SetTimer(window, 2, 1000, nullptr);
            RefreshJobDetail(false);
            return 0;
        }
        case WM_TIMER:
            RefreshJobDetail();
            return 0;
        case WM_NOTIFY: {
            auto* header = reinterpret_cast<NMHDR*>(lParam);
            if (!gRefreshingJobDetail && header->idFrom == ID_DETAIL_FRAMES &&
                header->code == LVN_ITEMCHANGED)
                UpdateFramePreview();
            return 0;
        }
        case WM_RM_PREVIEW_READY: {
            std::unique_ptr<PreviewResult> result(
                reinterpret_cast<PreviewResult*>(lParam));
            if (gPreviewThread.joinable()) gPreviewThread.join();
            if (!result || result->generation != gPreviewGeneration.load() ||
                result->source != gPreviewSource)
                return 0;
            HBITMAP bitmap = result->ok
                ? LoadPreviewBitmap(result->preview, 330, 220) : nullptr;
            ReplacePreviewBitmap(bitmap);
            std::wstring status = bitmap ? L"Blender 预览"
                : result->error.empty() ? L"无法生成该图片格式的预览" : result->error;
            SetWindowTextW(GetDlgItem(window, ID_DETAIL_PREVIEW_STATUS), status.c_str());
            return 0;
        }
        case WM_COMMAND:
            if (LOWORD(wParam) == ID_DETAIL_OPEN_SOURCE) {
                if (auto job = FindJob(gDetailJobId))
                    ShowInExplorer(job->sourceBlend.empty() ? job->preparedBlend : job->sourceBlend);
            } else if (LOWORD(wParam) == ID_DETAIL_OPEN_OUTPUT) {
                if (auto job = FindJob(gDetailJobId)) {
                    int selected = ListView_GetNextItem(
                        GetDlgItem(window, ID_DETAIL_FRAMES), -1, LVNI_SELECTED);
                    if (selected >= 0 && selected < static_cast<int>(job->frames.size()))
                        ShowInExplorer(job->frames[static_cast<size_t>(selected)].output);
                }
            } else if (LOWORD(wParam) == ID_DETAIL_STOP) {
                std::wstring error;
                if (!gApp->StopJob(gDetailJobId, error))
                    MessageBoxW(window, error.c_str(), L"RenderMesh", MB_ICONWARNING);
                RefreshJobDetail();
            }
            return 0;
        case WM_CLOSE:
            DestroyWindow(window);
            return 0;
        case WM_DESTROY:
            KillTimer(window, 2);
            StopPreviewGeneration();
            gPreviewSource.clear();
            gPreviewTaskId.clear();
            ReplacePreviewBitmap(nullptr);
            gDetailWindow = nullptr;
            gDetailJobId.clear();
            return 0;
        case WM_CTLCOLORSTATIC: {
            HDC dc = reinterpret_cast<HDC>(wParam);
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, gDarkTheme ? RGB(239, 239, 247) : RGB(31, 31, 38));
            return reinterpret_cast<LRESULT>(gBackgroundBrush);
        }
        case WM_ERASEBKGND: {
            RECT client{};
            GetClientRect(window, &client);
            FillRect(reinterpret_cast<HDC>(wParam), &client, gBackgroundBrush
                ? gBackgroundBrush : reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1));
            return TRUE;
        }
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

void OpenSelectedJobDetail() {
    HWND jobs = Item(ID_JOBS);
    int selected = ListView_GetNextItem(jobs, -1, LVNI_SELECTED);
    auto values = gApp->Jobs();
    if (selected < 0 || selected >= static_cast<int>(values.size())) {
        MessageBoxW(gWindow, L"请先选择一个渲染任务。", L"RenderMesh", MB_ICONINFORMATION);
        return;
    }
    gDetailJobId = values[static_cast<size_t>(selected)].id;
    if (gDetailWindow) {
        ShowWindow(gDetailWindow, SW_RESTORE);
        SetForegroundWindow(gDetailWindow);
        RefreshJobDetail(false);
        return;
    }
    gDetailWindow = CreateWindowExW(
        WS_EX_APPWINDOW, L"RenderMeshJobDetail", L"RenderMesh · 任务详情",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_SIZEBOX,
        CW_USEDEFAULT, CW_USEDEFAULT, 1220, 650, gWindow, nullptr,
        GetModuleHandleW(nullptr), nullptr);
    ShowWindow(gDetailWindow, SW_SHOW);
    UpdateWindow(gDetailWindow);
    ApplyTheme();
}

LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
        case WM_CREATE:
            gWindow = window;
            BuildUi();
            LoadConfigIntoUi();
            ApplyTheme();
            SetTimer(window, 1, 1000, nullptr);
            RefreshLists();
            if (!gUiMetricsReport.empty()) {
                WriteUiMetricsReport();
                PostMessageW(window, WM_CLOSE, 0, 0);
            }
            if (gOpenInitialDetail && ListView_GetItemCount(Item(ID_JOBS)) > 0) {
                ListView_SetItemState(Item(ID_JOBS), 0, LVIS_SELECTED | LVIS_FOCUSED,
                                      LVIS_SELECTED | LVIS_FOCUSED);
                OpenSelectedJobDetail();
            }
            return 0;
        case WM_HSCROLL:
            if (reinterpret_cast<HWND>(lParam) == Item(ID_CPU_SLIDER)) {
                int value = static_cast<int>(SendMessageW(Item(ID_CPU_SLIDER), TBM_GETPOS, 0, 0));
                SetText(ID_CPU_VALUE, std::to_wstring(value) + L"%");
            }
            return 0;
        case WM_COMMAND:
            switch (LOWORD(wParam)) {
                case ID_SHARE: {
                    Config config = gApp->GetConfig();
                    config.shareEnabled = !config.shareEnabled;
                    std::wstring error;
                    if (!gApp->UpdateConfig(config, error))
                        MessageBoxW(window, error.c_str(), L"RenderMesh", MB_ICONWARNING);
                    LoadConfigIntoUi();
                    InvalidateRect(Item(ID_SHARE), nullptr, TRUE);
                    RefreshLists();
                    break;
                }
                case ID_NAV_DASHBOARD: ShowPage(0); break;
                case ID_NAV_TASKS: ShowPage(1); break;
                case ID_NAV_CONTRIBUTIONS: ShowPage(2); break;
                case ID_NAV_SETTINGS: ShowPage(3); break;
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
                case ID_CONNECT_MANUAL:
                    ConnectManualPeer();
                    break;
                case ID_SUBMIT:
                    StartSubmission();
                    break;
                case ID_OPEN_DATA:
                    ShellExecuteW(window, L"open", gApp->DataDir().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                    break;
                case ID_JOB_DETAILS:
                    OpenSelectedJobDetail();
                    break;
            }
            return 0;
        case WM_DRAWITEM:
            DrawOwnerButton(*reinterpret_cast<DRAWITEMSTRUCT*>(lParam));
            return TRUE;
        case WM_NOTIFY: {
            auto* header = reinterpret_cast<NMHDR*>(lParam);
            if (header->idFrom == ID_JOBS && header->code == NM_DBLCLK)
                OpenSelectedJobDetail();
            return 0;
        }
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
                gPreflightStop = true;
                EnableWindow(window, FALSE);
                SetText(ID_PREFLIGHT, L"正在中止 Blender 预检并安全退出……");
                if (gPreflight.joinable()) gPreflight.join();
                EnableWindow(window, TRUE);
            }
            if (gDetailWindow) DestroyWindow(gDetailWindow);
            DestroyWindow(window);
            return 0;
        case WM_SETTINGCHANGE:
            if (gApp && gApp->GetConfig().theme == "system") ApplyTheme();
            return 0;
        case WM_DESTROY:
            KillTimer(window, 1);
            PostQuitMessage(0);
            return 0;
        case WM_CTLCOLORSTATIC: {
            HDC dc = reinterpret_cast<HDC>(wParam);
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, gDarkTheme ? RGB(239, 239, 247) : RGB(31, 31, 38));
            return reinterpret_cast<LRESULT>(gBackgroundBrush);
        }
        case WM_CTLCOLOREDIT:
        case WM_CTLCOLORLISTBOX: {
            HDC dc = reinterpret_cast<HDC>(wParam);
            SetTextColor(dc, gDarkTheme ? RGB(239, 239, 247) : RGB(31, 31, 38));
            SetBkColor(dc, gDarkTheme ? RGB(31, 31, 40) : RGB(255, 255, 255));
            return reinterpret_cast<LRESULT>(gSurfaceBrush);
        }
        case WM_ERASEBKGND: {
            RECT client{};
            GetClientRect(window, &client);
            FillRect(reinterpret_cast<HDC>(wParam), &client, gBackgroundBrush
                ? gBackgroundBrush : reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1));
            HPEN pen = CreatePen(PS_SOLID, 1,
                gDarkTheme ? RGB(50, 50, 62) : RGB(224, 225, 232));
            HGDIOBJ old = SelectObject(reinterpret_cast<HDC>(wParam), pen);
            MoveToEx(reinterpret_cast<HDC>(wParam), 220, 0, nullptr);
            LineTo(reinterpret_cast<HDC>(wParam), 220, client.bottom);
            SelectObject(reinterpret_cast<HDC>(wParam), old);
            DeleteObject(pen);
            return TRUE;
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
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_LISTVIEW_CLASSES | ICC_BAR_CLASSES};
    InitCommonControlsEx(&controls);
    int argumentCount = 0;
    LPWSTR* arguments = CommandLineToArgvW(GetCommandLineW(), &argumentCount);
    fs::path dataDir = DefaultDataDirectory();
    bool selfTest = false, mock = false, headless = false, noDialog = false;
    fs::path preflightSource, preflightPrepared, preflightReport, blenderOverride;
    fs::path renderSource, renderOutputDir, compatibilitySource;
    fs::path previewSource, previewOutput;
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
        else if (arg == L"--preview" && i + 1 < argumentCount) previewSource = arguments[++i];
        else if (arg == L"--preview-output" && i + 1 < argumentCount) previewOutput = arguments[++i];
        else if (arg == L"--port" && i + 1 < argumentCount) overridePort = _wtoi(arguments[++i]);
        else if (arg == L"--run-seconds" && i + 1 < argumentCount) runSeconds = _wtoi(arguments[++i]);
        else if (arg == L"--page" && i + 1 < argumentCount)
            gInitialPage = std::clamp(_wtoi(arguments[++i]), 0, 3);
        else if (arg == L"--detail") gOpenInitialDetail = true;
        else if (arg == L"--ui-metrics-report" && i + 1 < argumentCount)
            gUiMetricsReport = arguments[++i];
    }
    LocalFree(arguments);
    InitializeLogging(dataDir);
    if (selfTest) {
        std::wstring result;
        bool ok = RunSelfTest(result);
        if (!preflightReport.empty()) {
            std::error_code ignored;
            fs::create_directories(preflightReport.parent_path(), ignored);
            std::ofstream output(preflightReport, std::ios::binary | std::ios::trunc);
            output << WideToUtf8(result);
        }
        PrintToParentConsole(result);
        if (!noDialog)
            MessageBoxW(nullptr, result.c_str(),
                        ok ? L"RenderMesh 自检通过" : L"RenderMesh 自检失败",
                        ok ? MB_ICONINFORMATION : MB_ICONERROR);
        return ok ? 0 : 1;
    }

    if (!previewSource.empty()) {
        ConfigStore previewStore(dataDir);
        Config previewConfig = previewStore.Load();
        if (!blenderOverride.empty()) previewConfig.blenderPath = blenderOverride;
        if (previewOutput.empty()) previewOutput = dataDir / L"verification" / L"preview.png";
        std::atomic<bool> previewStop = false;
        std::wstring previewError;
        bool ok = GeneratePreviewImage(previewConfig, dataDir, previewSource, previewOutput,
                                       previewStop, previewError);
        PrintToParentConsole(ok ? L"预览生成成功：" + previewOutput.wstring() : previewError);
        return ok ? 0 : 6;
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
                                                preflightPrepared, renderFrame, renderFrame, 1);
        json output = {
            {"pass", result.passed}, {"version", result.blenderVersion},
            {"render_engine", result.renderEngine},
            {"render_format", result.outputFormat},
            {"file_extension", result.outputExtension},
            {"cpu_safe_while_active", result.cpuSafeWhileActive},
            {"requires_gpu", result.requiresGpu},
            {"addons", result.addons}, {"prepared", WideToUtf8(result.preparedBlend.wstring())},
            {"summary", WideToUtf8(result.summary)}, {"warnings", result.warnings},
        };
        output["primary_outputs"] = json::object();
        for (const auto& [frame, path] : result.primaryOutputs)
            output["primary_outputs"][std::to_string(frame)] = WideToUtf8(path.wstring());
        output["compositor_outputs"] = json::array();
        for (const auto& target : result.compositorOutputs)
            output["compositor_outputs"].push_back({
                {"id", target.id}, {"name", target.name},
                {"directory", WideToUtf8(target.directory.wstring())}});
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
        std::wstring renderError;
        bool ok = false;
        fs::path output;
        if (!preflightReport.empty() && fs::is_regular_file(preflightReport)) {
            std::ifstream input(preflightReport, std::ios::binary);
            json manifest = json::parse(input, nullptr, false);
            std::vector<OutputTarget> targets;
            if (!manifest.is_discarded()) {
                renderFormat = manifest.value("render_format", renderFormat);
                for (const auto& value : manifest.value("compositor_outputs", json::array()))
                    targets.push_back({value.value("id", ""), value.value("name", ""), {}});
            }
            std::atomic<bool> finishCurrentThenStop = false;
            std::map<int, std::vector<RenderArtifact>> artifacts;
            ok = !manifest.is_discarded() && RenderBlenderPackage(
                renderConfig, renderSource, renderOutputDir, {renderFrame}, renderFormat,
                manifest.value("file_extension", ".png"), false, false, stopRequested,
                finishCurrentThenStop, targets, artifacts, renderError);
            if (ok && artifacts.contains(renderFrame)) {
                auto primary = std::find_if(
                    artifacts[renderFrame].begin(), artifacts[renderFrame].end(),
                    [](const RenderArtifact& artifact) {
                        return artifact.logicalPath == "primary";
                    });
                if (primary != artifacts[renderFrame].end()) output = primary->source;
            }
        } else {
            ok = RenderBlenderFrame(renderConfig, renderSource, renderOutputDir, renderFrame,
                                    renderFormat,
                                    renderFormat == "PNG" ? ".png" :
                                        renderFormat == "JPEG" ? ".jpg" : ".exr",
                                    false, stopRequested, output, renderError);
        }
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
            manifest.value("requires_gpu", false),
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
    WNDCLASSW detailClass = windowClass;
    detailClass.lpfnWndProc = DetailWindowProc;
    detailClass.lpszClassName = L"RenderMeshJobDetail";
    RegisterClassW(&detailClass);
    gWindow = CreateWindowExW(
        0, windowClass.lpszClassName, L"RenderMesh · Blender 分布式渲染农场",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        CW_USEDEFAULT, CW_USEDEFAULT, 1280, 820, nullptr, nullptr, instance, nullptr);
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
    StopPreviewGeneration();
    gApp->Stop();
    if (gPreflight.joinable()) gPreflight.join();
    DeleteObject(gFont);
    DeleteObject(gTitleFont);
    if (gBackgroundBrush) DeleteObject(gBackgroundBrush);
    if (gSurfaceBrush) DeleteObject(gSurfaceBrush);
    CoUninitialize();
    return static_cast<int>(message.wParam);
}
