#include "Core.hpp"
#include "UiModel.hpp"

#include <commctrl.h>
#include <commdlg.h>
#include <d2d1.h>
#include <d2d1helper.h>
#include <dwrite.h>
#include <shellapi.h>
#include <uxtheme.h>
#include <wincodec.h>

#include <algorithm>
#include <array>
#include <fstream>
#include <memory>
#include <unordered_map>

namespace {

using namespace rm;

constexpr UINT WM_RM_REFRESH = WM_APP + 1;
constexpr UINT WM_RM_PREFLIGHT = WM_APP + 2;
constexpr UINT WM_RM_PREVIEW_READY = WM_APP + 3;
constexpr UINT_PTR TIMER_HEARTBEAT = 1;
constexpr UINT_PTR TIMER_BURST_REFRESH = 3;
constexpr UINT_PTR TIMER_UI_TEST_REPORT = 4;
constexpr UINT IDLE_HEARTBEAT_INTERVAL_MS = 1000;
constexpr int UI_TEST_BURST_NOTIFICATIONS = 100;

enum ControlId {
    ID_SHARE = 100,
    ID_NODE_NAME,
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
int gUiTestRows = 0;
bool gDarkTheme = false;
HBRUSH gBackgroundBrush = nullptr;
HBRUSH gSurfaceBrush = nullptr;
std::array<std::vector<HWND>, 4> gPageControls;
ui::StableTableModel gNodeRows;
ui::StableTableModel gJobRows;
ui::StableTableModel gContributionRows;
ui::StableTableModel gFrameRows;
std::atomic<bool> gRefreshQueued = false;
std::chrono::steady_clock::time_point gLastRefresh{};
uint64_t gListMutationCount = 0;
uint64_t gRefreshCount = 0;
uint64_t gHeartbeatTimerRefreshCount = 0;
uint64_t gBurstTimerRefreshCount = 0;
ID2D1Factory* gD2dFactory = nullptr;
IDWriteFactory* gDwriteFactory = nullptr;
ID2D1HwndRenderTarget* gRenderTarget = nullptr;
IDWriteTextFormat* gCardValueFormat = nullptr;
IDWriteTextFormat* gCardLabelFormat = nullptr;
HIMAGELIST gRowHeightImages = nullptr;
SIZE gLayoutBaseClient{};
std::unordered_map<HWND, RECT> gBaseControlRects;

struct DashboardView {
    int onlineNodes = 0;
    int activeJobs = 0;
    int completedFrames = 0;
    int totalFrames = 0;
    int assignedFrames = 0;
    int idleSeconds = 0;
    bool blenderReady = false;
    bool sharing = false;

    bool operator==(const DashboardView&) const = default;
};

DashboardView gDashboardView;

constexpr COLORREF kAccent = RGB(77, 69, 233);

template <typename T>
void SafeRelease(T*& value) {
    if (!value) return;
    value->Release();
    value = nullptr;
}

D2D1_COLOR_F D2dColor(COLORREF value, float alpha = 1.0f) {
    return D2D1::ColorF(GetRValue(value) / 255.0f, GetGValue(value) / 255.0f,
                        GetBValue(value) / 255.0f, alpha);
}

void ReleaseRenderTarget() {
    SafeRelease(gRenderTarget);
}

void ReleaseGraphics() {
    ReleaseRenderTarget();
    SafeRelease(gCardValueFormat);
    SafeRelease(gCardLabelFormat);
    SafeRelease(gDwriteFactory);
    SafeRelease(gD2dFactory);
}

bool EnsureGraphics(HWND window) {
    if (!gD2dFactory && FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,
                                                 &gD2dFactory)))
        return false;
    if (!gDwriteFactory && FAILED(DWriteCreateFactory(
            DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
            reinterpret_cast<IUnknown**>(&gDwriteFactory))))
        return false;
    if (!gCardValueFormat) {
        HRESULT hr = gDwriteFactory->CreateTextFormat(
            L"Segoe UI Variable Display", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD,
            DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 24.0f, L"zh-CN",
            &gCardValueFormat);
        if (FAILED(hr)) hr = gDwriteFactory->CreateTextFormat(
            L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL, 24.0f, L"zh-CN", &gCardValueFormat);
        if (FAILED(hr)) return false;
        gCardValueFormat->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    }
    if (!gCardLabelFormat) {
        HRESULT hr = gDwriteFactory->CreateTextFormat(
            L"Segoe UI Variable Text", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
            DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 12.0f, L"zh-CN",
            &gCardLabelFormat);
        if (FAILED(hr)) hr = gDwriteFactory->CreateTextFormat(
            L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL, 12.0f, L"zh-CN", &gCardLabelFormat);
        if (FAILED(hr)) return false;
        gCardLabelFormat->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    }
    if (!gRenderTarget) {
        RECT client{};
        GetClientRect(window, &client);
        const D2D1_SIZE_U size = D2D1::SizeU(
            static_cast<UINT>(std::max(1L, client.right)),
            static_cast<UINT>(std::max(1L, client.bottom)));
        if (FAILED(gD2dFactory->CreateHwndRenderTarget(
                D2D1::RenderTargetProperties(),
                D2D1::HwndRenderTargetProperties(window, size), &gRenderTarget)))
            return false;
        gRenderTarget->SetDpi(96.0f, 96.0f);
    }
    return true;
}

void DrawD2dText(const std::wstring& text, IDWriteTextFormat* format,
                 const D2D1_RECT_F& rect, ID2D1Brush* brush) {
    if (!text.empty())
        gRenderTarget->DrawTextW(text.c_str(), static_cast<UINT32>(text.size()), format,
                                 rect, brush, D2D1_DRAW_TEXT_OPTIONS_CLIP);
}

void PaintChrome(HWND window) {
    PAINTSTRUCT paint{};
    BeginPaint(window, &paint);
    if (!EnsureGraphics(window)) {
        FillRect(paint.hdc, &paint.rcPaint,
                 gBackgroundBrush ? gBackgroundBrush
                                  : reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1));
        EndPaint(window, &paint);
        return;
    }

    RECT client{};
    GetClientRect(window, &client);
    gRenderTarget->Resize(D2D1::SizeU(static_cast<UINT>(std::max(1L, client.right)),
                                      static_cast<UINT>(std::max(1L, client.bottom))));
    const COLORREF background = gDarkTheme ? RGB(15, 16, 22) : RGB(244, 245, 249);
    const COLORREF sidebar = gDarkTheme ? RGB(23, 24, 32) : RGB(252, 252, 254);
    const COLORREF surface = gDarkTheme ? RGB(31, 32, 42) : RGB(255, 255, 255);
    const COLORREF stroke = gDarkTheme ? RGB(51, 52, 66) : RGB(224, 226, 234);
    const COLORREF text = gDarkTheme ? RGB(244, 244, 249) : RGB(28, 29, 36);
    const COLORREF secondary = gDarkTheme ? RGB(164, 166, 183) : RGB(103, 105, 119);
    ID2D1SolidColorBrush* backgroundBrush = nullptr;
    ID2D1SolidColorBrush* sidebarBrush = nullptr;
    ID2D1SolidColorBrush* surfaceBrush = nullptr;
    ID2D1SolidColorBrush* strokeBrush = nullptr;
    ID2D1SolidColorBrush* textBrush = nullptr;
    ID2D1SolidColorBrush* secondaryBrush = nullptr;
    ID2D1SolidColorBrush* accentBrush = nullptr;
    gRenderTarget->CreateSolidColorBrush(D2dColor(background), &backgroundBrush);
    gRenderTarget->CreateSolidColorBrush(D2dColor(sidebar), &sidebarBrush);
    gRenderTarget->CreateSolidColorBrush(D2dColor(surface), &surfaceBrush);
    gRenderTarget->CreateSolidColorBrush(D2dColor(stroke), &strokeBrush);
    gRenderTarget->CreateSolidColorBrush(D2dColor(text), &textBrush);
    gRenderTarget->CreateSolidColorBrush(D2dColor(secondary), &secondaryBrush);
    gRenderTarget->CreateSolidColorBrush(D2dColor(kAccent), &accentBrush);

    gRenderTarget->BeginDraw();
    gRenderTarget->Clear(D2dColor(background));
    gRenderTarget->FillRectangle(D2D1::RectF(0, 0, 220.0f,
                                             static_cast<float>(client.bottom)), sidebarBrush);
    gRenderTarget->DrawLine(D2D1::Point2F(220.0f, 0),
                            D2D1::Point2F(220.0f, static_cast<float>(client.bottom)),
                            strokeBrush, 1.0f);
    gRenderTarget->FillEllipse(D2D1::Ellipse(D2D1::Point2F(31.0f, 31.0f), 5.0f, 5.0f),
                               accentBrush);

    const float contentLeft = 236.0f;
    const float contentRight = static_cast<float>(client.right) - 18.0f;
    if (gCurrentPage == 0) {
        constexpr float gap = 12.0f;
        const float cardWidth = std::max(160.0f,
            (contentRight - contentLeft - gap * 3.0f) / 4.0f);
        const std::array<std::wstring, 4> labels = {
            L"在线节点", L"活动任务", L"完成帧", L"本机共享",
        };
        const std::array<std::wstring, 4> values = {
            std::to_wstring(gDashboardView.onlineNodes),
            std::to_wstring(gDashboardView.activeJobs),
            std::to_wstring(gDashboardView.completedFrames) + L" / " +
                std::to_wstring(gDashboardView.totalFrames),
            gDashboardView.sharing ? L"已开启" : L"已关闭",
        };
        for (size_t index = 0; index < labels.size(); ++index) {
            const float left = contentLeft + static_cast<float>(index) * (cardWidth + gap);
            const D2D1_ROUNDED_RECT card = D2D1::RoundedRect(
                D2D1::RectF(left, 86.0f, left + cardWidth, 176.0f), 14.0f, 14.0f);
            gRenderTarget->FillRoundedRectangle(card, surfaceBrush);
            gRenderTarget->DrawRoundedRectangle(card, strokeBrush, 1.0f);
            DrawD2dText(labels[index], gCardLabelFormat,
                        D2D1::RectF(left + 16.0f, 102.0f, left + cardWidth - 12.0f, 124.0f),
                        secondaryBrush);
            DrawD2dText(values[index], gCardValueFormat,
                        D2D1::RectF(left + 16.0f, 128.0f, left + cardWidth - 12.0f, 163.0f),
                        index == 3 && gDashboardView.sharing ? accentBrush : textBrush);
        }
    } else {
        const float top = gCurrentPage == 1 ? 72.0f : gCurrentPage == 2 ? 82.0f : 74.0f;
        const D2D1_ROUNDED_RECT panel = D2D1::RoundedRect(
            D2D1::RectF(contentLeft, top, contentRight,
                        static_cast<float>(client.bottom) - 18.0f), 16.0f, 16.0f);
        gRenderTarget->FillRoundedRectangle(panel, surfaceBrush);
        gRenderTarget->DrawRoundedRectangle(panel, strokeBrush, 1.0f);
    }

    HRESULT result = gRenderTarget->EndDraw();
    SafeRelease(accentBrush);
    SafeRelease(secondaryBrush);
    SafeRelease(textBrush);
    SafeRelease(strokeBrush);
    SafeRelease(surfaceBrush);
    SafeRelease(sidebarBrush);
    SafeRelease(backgroundBrush);
    if (result == D2DERR_RECREATE_TARGET) ReleaseRenderTarget();
    EndPaint(window, &paint);
}

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
    COLORREF background = gDarkTheme ? RGB(15, 16, 22) : RGB(244, 245, 249);
    COLORREF surface = gDarkTheme ? RGB(31, 32, 42) : RGB(255, 255, 255);
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
        SetWindowTheme(list, gDarkTheme ? L"DarkMode_Explorer" : L"Explorer", nullptr);
        if (HWND header = ListView_GetHeader(list))
            SetWindowTheme(header, gDarkTheme ? L"DarkMode_ItemsView" : L"Explorer", nullptr);
    }
    BOOL dark = gDarkTheme;
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

bool SetWindowTextIfChanged(HWND control, const std::wstring& value) {
    if (!control || Text(control) == value) return false;
    SetWindowTextW(control, value.c_str());
    return true;
}

void SetText(int id, const std::wstring& value) {
    SetWindowTextIfChanged(Item(id), value);
}

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
        0, type, text,
        WS_CHILD | WS_VISIBLE | style, x, y, width, height, gWindow,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr), nullptr);
    SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font ? font : gFont), TRUE);
    return control;
}

HWND Label(const wchar_t* text, int x, int y, int width, int height, int id = 0,
           HFONT font = nullptr) {
    return AddControl(L"STATIC", text, SS_LEFT | SS_NOPREFIX | SS_ENDELLIPSIS,
                      x, y, width, height, id, font);
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

void ConfigureList(HWND list) {
    ListView_SetExtendedListViewStyle(list,
        LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_HEADERDRAGDROP);
    if (!gRowHeightImages) {
        constexpr int rowHeight = 40;
        gRowHeightImages = ImageList_Create(1, rowHeight, ILC_COLOR32, 1, 1);
        if (gRowHeightImages) {
            HBITMAP spacer = CreateBitmap(1, rowHeight, 1, 32, nullptr);
            ImageList_Add(gRowHeightImages, spacer, nullptr);
            DeleteObject(spacer);
        }
    }
    if (gRowHeightImages) ListView_SetImageList(list, gRowHeightImages, LVSIL_SMALL);
}

void InsertRow(HWND list, int index, const ui::Row& row) {
    if (row.cells.empty()) return;
    LVITEMW item{};
    item.mask = LVIF_TEXT;
    item.iItem = index;
    item.pszText = const_cast<wchar_t*>(row.cells[0].c_str());
    int inserted = ListView_InsertItem(list, &item);
    if (inserted < 0) return;
    for (size_t column = 1; column < row.cells.size(); ++column)
        ListView_SetItemText(list, inserted, static_cast<int>(column),
                             const_cast<wchar_t*>(row.cells[column].c_str()));
}

struct TableViewState {
    std::vector<std::wstring> selectedKeys;
    std::optional<std::wstring> focusedKey;
    std::optional<std::wstring> topKey;
    int topIndex = 0;
    int topOffset = 0;
    int horizontalScroll = 0;
};

TableViewState CaptureTableView(HWND list, const ui::StableTableModel& model) {
    TableViewState state;
    const auto& rows = model.Rows();
    for (int selected = ListView_GetNextItem(list, -1, LVNI_SELECTED); selected >= 0;
         selected = ListView_GetNextItem(list, selected, LVNI_SELECTED)) {
        if (selected < static_cast<int>(rows.size()))
            state.selectedKeys.push_back(rows[static_cast<size_t>(selected)].key);
    }
    int focused = ListView_GetNextItem(list, -1, LVNI_FOCUSED);
    if (focused >= 0 && focused < static_cast<int>(rows.size()))
        state.focusedKey = rows[static_cast<size_t>(focused)].key;
    state.topIndex = ListView_GetTopIndex(list);
    if (state.topIndex >= 0 && state.topIndex < static_cast<int>(rows.size())) {
        state.topKey = rows[static_cast<size_t>(state.topIndex)].key;
        RECT item{};
        if (ListView_GetItemRect(list, state.topIndex, &item, LVIR_BOUNDS))
            state.topOffset = item.top;
    }
    state.horizontalScroll = GetScrollPos(list, SB_HORZ);
    return state;
}

void RestoreTableView(HWND list, const TableViewState& state,
                      const ui::StableTableModel& model) {
    const auto& rows = model.Rows();
    ListView_SetItemState(list, -1, 0, LVIS_SELECTED | LVIS_FOCUSED);
    for (const auto& key : state.selectedKeys) {
        if (auto index = model.IndexOf(key))
            ListView_SetItemState(list, static_cast<int>(*index), LVIS_SELECTED,
                                  LVIS_SELECTED);
    }
    if (state.focusedKey) {
        if (auto index = model.IndexOf(*state.focusedKey))
            ListView_SetItemState(list, static_cast<int>(*index), LVIS_FOCUSED,
                                  LVIS_FOCUSED);
    }

    std::optional<size_t> anchor;
    if (state.topKey) anchor = model.IndexOf(*state.topKey);
    if (!anchor && !rows.empty())
        anchor = static_cast<size_t>(std::min<int>(state.topIndex,
                                                   static_cast<int>(rows.size()) - 1));
    if (anchor) {
        ListView_EnsureVisible(list, static_cast<int>(*anchor), FALSE);
        RECT item{};
        if (ListView_GetItemRect(list, static_cast<int>(*anchor), &item, LVIR_BOUNDS))
            ListView_Scroll(list, 0, item.top - state.topOffset);
    }
    const int horizontalNow = GetScrollPos(list, SB_HORZ);
    if (horizontalNow != state.horizontalScroll)
        ListView_Scroll(list, state.horizontalScroll - horizontalNow, 0);
}

bool SyncTable(HWND list, ui::StableTableModel& model, std::vector<ui::Row> rows) {
    if (!list) return false;
    TableViewState view = CaptureTableView(list, model);
    auto operations = model.Reconcile(std::move(rows));
    if (operations.empty()) return false;

    const bool structural = std::any_of(operations.begin(), operations.end(),
        [](const ui::Operation& operation) {
            return operation.type != ui::OperationType::Update;
        });
    if (structural) SendMessageW(list, WM_SETREDRAW, FALSE, 0);
    for (const auto& operation : operations) {
        switch (operation.type) {
            case ui::OperationType::Insert:
                InsertRow(list, static_cast<int>(operation.index), operation.row);
                break;
            case ui::OperationType::Erase:
                ListView_DeleteItem(list, static_cast<int>(operation.index));
                break;
            case ui::OperationType::Move:
                ListView_DeleteItem(list, static_cast<int>(operation.fromIndex));
                InsertRow(list, static_cast<int>(operation.index), operation.row);
                break;
            case ui::OperationType::Update:
                for (const auto& cell : operation.cells)
                    ListView_SetItemText(list, static_cast<int>(operation.index),
                        static_cast<int>(cell.column), const_cast<wchar_t*>(cell.value.c_str()));
                ListView_RedrawItems(list, static_cast<int>(operation.index),
                                     static_cast<int>(operation.index));
                break;
        }
    }
    gListMutationCount += operations.size();
    if (structural) {
        RestoreTableView(list, view, model);
        SendMessageW(list, WM_SETREDRAW, TRUE, 0);
        RedrawWindow(list, nullptr, nullptr,
                     RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_UPDATENOW);
    }
    return true;
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
ui::VisualState TaskVisualState(TaskState state);

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
    int completed = 0, failed = 0, stopped = 0, active = 0;
    std::vector<ui::Row> frameRows;
    frameRows.reserve(job.frames.size());
    for (int index = 0; index < static_cast<int>(job.frames.size()); ++index) {
        const auto& frame = job.frames[static_cast<size_t>(index)];
        completed += frame.state == TaskState::Completed;
        failed += frame.state == TaskState::Failed;
        stopped += frame.state == TaskState::Stopped;
        active += frame.state == TaskState::Assigned || frame.state == TaskState::Rendering ||
                  frame.state == TaskState::Dispatching;
        ui::Row row;
        row.key = Utf8ToWide(frame.taskId.empty()
            ? job.id + ":" + std::to_string(frame.frame) : frame.taskId);
        row.visualState = TaskVisualState(frame.state);
        row.cells = {
            std::to_wstring(frame.frame),
            TaskStateLabel(frame.state),
            Utf8ToWide(frame.assignedNodeName),
            Utf8ToWide(frame.assignedPackageId),
            FormatDuration(frame.startedAt, frame.endedAt),
            FormatTime(frame.startedAt),
            FormatTime(frame.endedAt),
            frame.error.empty() ? frame.output.wstring() : Utf8ToWide(frame.error),
        };
        frameRows.push_back(std::move(row));
    }
    SyncTable(list, gFrameRows, std::move(frameRows));
    if (!preserveSelection && !job.frames.empty())
        ListView_SetItemState(list, 0, LVIS_SELECTED | LVIS_FOCUSED,
                              LVIS_SELECTED | LVIS_FOCUSED);
    std::wstring summary = Utf8ToWide(job.name) + L"    总帧数 " +
        std::to_wstring(job.frames.size()) + L"    已完成 " + std::to_wstring(completed) +
        L"    渲染中 " + std::to_wstring(active) + L"    错误 " +
        std::to_wstring(failed) + L"    已停止 " + std::to_wstring(stopped) +
        L"    提交时间 " + FormatTime(job.createdAt);
    SetWindowTextIfChanged(GetDlgItem(gDetailWindow, ID_DETAIL_SUMMARY), summary);
    gRefreshingJobDetail = false;
    UpdateFramePreview();
}

void UpdateFramePreview() {
    if (!gDetailWindow) return;
    auto job = FindJob(gDetailJobId);
    if (!job) return;
    HWND list = GetDlgItem(gDetailWindow, ID_DETAIL_FRAMES);
    int selected = ListView_GetNextItem(list, -1, LVNI_SELECTED);
    if (selected < 0 || selected >= static_cast<int>(gFrameRows.Rows().size())) {
        StopPreviewGeneration();
        gPreviewSource.clear();
        gPreviewTaskId.clear();
        ReplacePreviewBitmap(nullptr);
        SetWindowTextW(GetDlgItem(gDetailWindow, ID_DETAIL_PREVIEW_STATUS),
                       L"选择一帧查看预览");
        return;
    }
    const std::wstring selectedKey = gFrameRows.Rows()[static_cast<size_t>(selected)].key;
    auto selectedFrame = std::find_if(job->frames.begin(), job->frames.end(),
        [&](const FrameTask& frame) {
            return Utf8ToWide(frame.taskId.empty()
                ? job->id + ":" + std::to_string(frame.frame) : frame.taskId) == selectedKey;
        });
    if (selectedFrame == job->frames.end()) return;
    const auto& frame = *selectedFrame;
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

ui::VisualState TaskVisualState(TaskState state) {
    switch (state) {
        case TaskState::Completed: return ui::VisualState::Success;
        case TaskState::Assigned:
        case TaskState::Dispatching:
        case TaskState::Rendering: return ui::VisualState::Active;
        case TaskState::Pending: return ui::VisualState::Warning;
        case TaskState::Failed: return ui::VisualState::Error;
        case TaskState::Stopped: return ui::VisualState::Muted;
    }
    return ui::VisualState::Neutral;
}

void RefreshLists() {
    if (!gApp) return;
    ++gRefreshCount;
    const Config config = gApp->GetConfig();

    if (gCurrentPage == 0) {
        const auto peers = gApp->Peers();
        const auto jobs = gApp->Jobs();
        const HardwareInfo hardware = gApp->Hardware();
        const int idleSeconds = static_cast<int>(IdleSeconds());
        const int assigned = gApp->AssignedCount();
        std::wstring manualStatus = gApp->ManualPeerStatus();
        if (!manualStatus.empty()) SetText(ID_MANUAL_STATUS, manualStatus);

        std::vector<ui::Row> nodeRows;
        nodeRows.reserve(peers.size() + 1);
        std::wstring localGpus;
        for (const auto& gpu : hardware.gpuModels) {
            if (!localGpus.empty()) localGpus += L" / ";
            localGpus += Utf8ToWide(gpu);
        }
        const bool localIdle = IsComputerIdleForSharedWork(IdleSeconds(),
                                                            config.idleAfterSeconds);
        nodeRows.push_back({
            Utf8ToWide("local:" + config.nodeId),
            {
                Utf8ToWide(config.nodeName) + L"（本机）",
                L"已信任",
                config.shareEnabled ? (localIdle ? L"空闲 · 可接单" : L"活跃 · 等待")
                                    : L"未共享",
                L"仅空闲",
                std::to_wstring(assigned),
                Utf8ToWide(hardware.cpuModel),
                localGpus.empty() ? L"未检测到" : localGpus,
                LocalEndpointSummary(config.httpPort),
            },
            config.shareEnabled
                ? (localIdle ? ui::VisualState::Success : ui::VisualState::Warning)
                : ui::VisualState::Muted,
        });
        for (const auto& peer : peers) {
            std::wstring remoteGpus;
            for (const auto& gpu : peer.gpuModels) {
                if (!remoteGpus.empty()) remoteGpus += L" / ";
                remoteGpus += Utf8ToWide(gpu);
            }
            nodeRows.push_back({
                Utf8ToWide(peer.nodeId),
                {
                    Utf8ToWide(peer.name),
                    peer.trusted ? L"已信任" : L"邀请码不同",
                    peer.shareEnabled
                        ? (peer.assigned ? L"正在渲染"
                                         : L"已空闲 " +
                                               std::to_wstring(static_cast<int>(peer.idleSeconds)) +
                                               L" 秒")
                        : L"未共享",
                    L"仅空闲",
                    std::to_wstring(peer.assigned),
                    peer.cpuModel.empty() ? L"未报告" : Utf8ToWide(peer.cpuModel),
                    remoteGpus.empty() ? L"未报告" : remoteGpus,
                    Utf8ToWide(peer.address) + L":" + std::to_wstring(peer.httpPort),
                },
                !peer.trusted ? ui::VisualState::Error
                    : peer.assigned ? ui::VisualState::Active
                    : peer.shareEnabled ? ui::VisualState::Success
                    : ui::VisualState::Muted,
            });
        }
        SyncTable(Item(ID_NODES), gNodeRows, std::move(nodeRows));

        DashboardView nextDashboard;
        nextDashboard.onlineNodes = static_cast<int>(peers.size()) + 1;
        nextDashboard.assignedFrames = assigned;
        nextDashboard.idleSeconds = idleSeconds;
        nextDashboard.blenderReady = !FindBlender(config).empty();
        nextDashboard.sharing = config.shareEnabled;
        for (const auto& job : jobs) {
            bool jobActive = false;
            for (const auto& frame : job.frames) {
                ++nextDashboard.totalFrames;
                nextDashboard.completedFrames += frame.state == TaskState::Completed;
                jobActive = jobActive || frame.state == TaskState::Assigned ||
                    frame.state == TaskState::Dispatching || frame.state == TaskState::Rendering;
            }
            nextDashboard.activeJobs += jobActive;
        }
        if (!(nextDashboard == gDashboardView)) {
            gDashboardView = nextDashboard;
            RECT client{};
            GetClientRect(gWindow, &client);
            RECT cards{236, 86, std::max(237L, client.right - 18), 176};
            InvalidateRect(gWindow, &cards, FALSE);
        }
        SetText(ID_STATUS, std::wstring(config.pairingPending ? L"正在邀请码配对  ·  " : L"") +
            L"本机空闲 " + std::to_wstring(idleSeconds) + L" 秒  ·  " +
            (nextDashboard.blenderReady ? L"Blender 已就绪" : L"Blender 未检测到"));
    } else if (gCurrentPage == 1) {
        const auto values = gApp->Jobs();
        std::vector<ui::Row> jobRows;
        jobRows.reserve(values.size());
        for (const auto& job : values) {
            int completed = 0, failed = 0, stopped = 0, active = 0;
            std::chrono::system_clock::time_point started{}, ended{};
            for (const auto& frame : job.frames) {
                completed += frame.state == TaskState::Completed;
                failed += frame.state == TaskState::Failed;
                stopped += frame.state == TaskState::Stopped;
                active += frame.state == TaskState::Assigned ||
                    frame.state == TaskState::Rendering || frame.state == TaskState::Dispatching;
                if (frame.startedAt.time_since_epoch() !=
                        std::chrono::system_clock::duration::zero() &&
                    (started.time_since_epoch() ==
                         std::chrono::system_clock::duration::zero() ||
                     frame.startedAt < started))
                    started = frame.startedAt;
                if (frame.endedAt > ended) ended = frame.endedAt;
            }
            const TaskState aggregate = completed == static_cast<int>(job.frames.size())
                ? TaskState::Completed : failed ? TaskState::Failed
                : stopped ? TaskState::Stopped : active ? TaskState::Rendering
                : TaskState::Pending;
            const std::wstring state = aggregate == TaskState::Completed ? L"已完成"
                : aggregate == TaskState::Failed ? L"有错误"
                : aggregate == TaskState::Stopped ? L"已停止"
                : aggregate == TaskState::Rendering ? L"渲染中" : L"待提交";
            jobRows.push_back({
                Utf8ToWide(job.id),
                {
                    Utf8ToWide(job.name), state,
                    std::to_wstring(job.frames.size()),
                    std::to_wstring(completed) + L" / " +
                        std::to_wstring(job.frames.size()),
                    FormatDuration(started,
                        active ? std::chrono::system_clock::time_point{} : ended),
                    FormatTime(started), active ? L"-" : FormatTime(ended),
                },
                TaskVisualState(aggregate),
            });
        }
        SyncTable(Item(ID_JOBS), gJobRows, std::move(jobRows));
    } else if (gCurrentPage == 2) {
        const auto records = gApp->Contributions();
        std::vector<ui::Row> contributionRows;
        contributionRows.reserve(records.size());
        for (auto record = records.rbegin(); record != records.rend(); ++record) {
            const std::wstring state = record->state == "completed" ? L"已完成"
                : record->state == "stopped" ? L"活跃后已停止"
                : record->state == "partial" ? L"部分完成" : L"失败";
            contributionRows.push_back({
                Utf8ToWide(record->jobId + ":" + record->packageId),
                {
                    Utf8ToWide(record->jobName), Utf8ToWide(record->packageId),
                    Utf8ToWide(record->originName), FrameNumbers(record->frames),
                    FrameNumbers(record->completedFrames), state,
                    FormatDuration(record->startedAt, record->endedAt),
                    FormatTime(record->startedAt), FormatTime(record->endedAt),
                    Utf8ToWide(record->error),
                },
                record->state == "completed" ? ui::VisualState::Success
                    : record->state == "partial" ? ui::VisualState::Warning
                    : record->state == "stopped" ? ui::VisualState::Muted
                    : ui::VisualState::Error,
            });
        }
        SyncTable(Item(ID_CONTRIBUTION_LIST), gContributionRows,
                  std::move(contributionRows));
    }

    if (gDetailWindow) RefreshJobDetail();
    gLastRefresh = std::chrono::steady_clock::now();
}

void QueueModelRefresh() {
    if (!gWindow || gRefreshQueued.exchange(true)) return;
    if (!PostMessageW(gWindow, WM_RM_REFRESH, 0, 0)) gRefreshQueued = false;
}

void HandleQueuedRefresh() {
    if (!gRefreshQueued.load()) return;
    const auto now = std::chrono::steady_clock::now();
    const auto elapsed = now - gLastRefresh;
    if (gLastRefresh.time_since_epoch() != std::chrono::steady_clock::duration::zero() &&
        elapsed < ui::MinimumRefreshInterval()) {
        const auto remaining = ui::RefreshDelayFor(elapsed);
        SetTimer(gWindow, TIMER_BURST_REFRESH,
                 static_cast<UINT>(std::max<int64_t>(1, remaining.count())), nullptr);
        return;
    }
    gRefreshQueued = false;
    RefreshLists();
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
    SetText(ID_PREFLIGHT, L"正在启动 Blender 检查外部素材，并打包临时工程……");
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
    Label(L"RenderMesh", 24, 20, 190, 32, 0, gTitleFont);
    Label(L"Blender 渲染网格", 26, 54, 170, 22);
    AddControl(L"BUTTON", L"共享本机算力", BS_OWNERDRAW,
               12, 88, 196, 76, ID_SHARE);
    AddControl(L"BUTTON", L"服务器", BS_OWNERDRAW, 18, 196, 184, 42, ID_NAV_DASHBOARD);
    AddControl(L"BUTTON", L"渲染任务", BS_OWNERDRAW, 18, 244, 184, 42, ID_NAV_TASKS);
    AddControl(L"BUTTON", L"本机贡献", BS_OWNERDRAW, 18, 292, 184, 42, ID_NAV_CONTRIBUTIONS);
    AddControl(L"BUTTON", L"设置", BS_OWNERDRAW, 18, 340, 184, 42, ID_NAV_SETTINGS);
    Label(L"原生客户端  ·  本地优先", 28, 744, 165, 22);

    // 服务器页
    PageControl(0, Label(L"服务器", 248, 24, 250, 36, 0, gTitleFont));
    PageControl(0, Label(L"", 250, 62, 980, 22, ID_STATUS));
    PageControl(0, Label(L"手动连接", 250, 202, 80, 24));
    PageControl(0, AddControl(L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL,
                              334, 198, 235, 28, ID_MANUAL_ENDPOINT));
    PageControl(0, AddControl(L"BUTTON", L"连接", BS_PUSHBUTTON,
                              579, 197, 82, 30, ID_CONNECT_MANUAL));
    PageControl(0, Label(L"填写另一台电脑的局域网 IP:端口，不要填 127.0.0.1",
                         680, 202, 550, 24, ID_MANUAL_STATUS));
    HWND nodes = PageControl(0, AddControl(
        WC_LISTVIEWW, L"", LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
        250, 238, 990, 454, ID_NODES));
    ConfigureList(nodes);
    AddColumn(nodes, 0, 170, L"电脑"); AddColumn(nodes, 1, 85, L"连接");
    AddColumn(nodes, 2, 170, L"算力状态"); AddColumn(nodes, 3, 85, L"调度策略");
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
    PageControl(1, Label(L"输出格式由 Blender 工程决定；提交前检查并打包链接素材，同时执行低分辨率验证渲染。",
                         250, 178, 976, 24));
    PageControl(1, AddControl(L"STATIC", L"等待选择工程", SS_LEFT,
                              250, 210, 976, 45, ID_PREFLIGHT));
    PageControl(1, Label(L"任务队列（可见 10 个槽位，超出后滚动）",
                         250, 274, 440, 26));
    HWND jobs = PageControl(1, AddControl(
        WC_LISTVIEWW, L"", LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
        250, 304, 976, 350, ID_JOBS));
    ConfigureList(jobs);
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
    ConfigureList(contributionList);
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
    PageControl(3, Label(L"无键鼠输入后进入闲置（秒）", 270, 154, 240, 24));
    PageControl(3, AddControl(L"EDIT", L"180", WS_BORDER | ES_NUMBER,
                              520, 150, 110, 30, ID_IDLE_SECONDS));
    PageControl(3, Label(L"Blender 程序", 270, 208, 155, 24));
    PageControl(3, AddControl(L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL,
                              445, 204, 650, 30, ID_BLENDER_PATH));
    PageControl(3, AddControl(L"BUTTON", L"选择…", BS_PUSHBUTTON,
                              1105, 203, 90, 32, ID_BROWSE_BLENDER));
    PageControl(3, Label(L"六位邀请码", 270, 262, 155, 24));
    PageControl(3, AddControl(L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL | ES_NUMBER,
                              445, 258, 240, 30, ID_CLUSTER_TOKEN));
    PageControl(3, AddControl(L"BUTTON", L"复制", BS_PUSHBUTTON,
                              695, 257, 90, 32, ID_COPY_TOKEN));
    PageControl(3, Label(L"外观", 270, 316, 155, 24));
    HWND theme = PageControl(3, AddControl(L"COMBOBOX", L"",
        CBS_DROPDOWNLIST | WS_VSCROLL, 445, 312, 240, 120, ID_THEME));
    SendMessageW(theme, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"跟随系统"));
    SendMessageW(theme, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"浅色"));
    SendMessageW(theme, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"深色"));
    PageControl(3, Label(L"端口 8391 · 后台日志按天保存在数据目录",
                         270, 376, 600, 24));
    PageControl(3, AddControl(L"BUTTON", L"保存设置", BS_OWNERDRAW,
                              445, 432, 240, 40, ID_SAVE_SETTINGS));
    ShowPage(gInitialPage);
}

void CaptureBaseLayout() {
    RECT client{};
    GetClientRect(gWindow, &client);
    gLayoutBaseClient = {client.right, client.bottom};
    gBaseControlRects.clear();
    for (HWND control = GetWindow(gWindow, GW_CHILD); control;
         control = GetWindow(control, GW_HWNDNEXT)) {
        RECT rect{};
        GetWindowRect(control, &rect);
        MapWindowPoints(HWND_DESKTOP, gWindow, reinterpret_cast<POINT*>(&rect), 2);
        gBaseControlRects.emplace(control, rect);
    }
}

void LayoutUi() {
    if (gBaseControlRects.empty() || gLayoutBaseClient.cx <= 0) return;
    RECT client{};
    GetClientRect(gWindow, &client);
    constexpr int contentAnchor = 232;
    constexpr int rightPadding = 18;
    const int baseContent = std::max(1, static_cast<int>(
        gLayoutBaseClient.cx - contentAnchor - rightPadding));
    const int nextContent = std::max(1, static_cast<int>(
        client.right - contentAnchor - rightPadding));
    const double scale = static_cast<double>(nextContent) / baseContent;
    const int heightDelta = client.bottom - gLayoutBaseClient.cy;
    for (const auto& [control, base] : gBaseControlRects) {
        if (!IsWindow(control)) continue;
        RECT next = base;
        if (base.left >= 220) {
            next.left = contentAnchor + static_cast<int>((base.left - contentAnchor) * scale);
            next.right = contentAnchor + static_cast<int>((base.right - contentAnchor) * scale);
        }
        const int height = base.bottom - base.top;
        if (height >= 300) next.bottom += heightDelta;
        else if (base.top >= 650) {
            next.top += heightDelta;
            next.bottom += heightDelta;
        }
        MoveWindow(control, next.left, next.top,
                   std::max(24, static_cast<int>(next.right - next.left)),
                   std::max(20, static_cast<int>(next.bottom - next.top)), FALSE);
    }
    InvalidateRect(gWindow, nullptr, FALSE);
}

void LayoutDetailWindow(HWND window) {
    RECT client{};
    GetClientRect(window, &client);
    const int rightWidth = 330;
    const int gap = 18;
    const int margin = 22;
    const int rightX = std::max(650, static_cast<int>(client.right - margin - rightWidth));
    const int listWidth = std::max(560, rightX - margin - gap);
    if (HWND summary = GetDlgItem(window, ID_DETAIL_SUMMARY))
        MoveWindow(summary, margin, 18, client.right - margin * 2, 28, TRUE);
    if (HWND frames = GetDlgItem(window, ID_DETAIL_FRAMES))
        MoveWindow(frames, margin, 55, listWidth,
                   std::max(360, static_cast<int>(client.bottom - 77)), TRUE);
    if (HWND preview = GetDlgItem(window, ID_DETAIL_PREVIEW))
        MoveWindow(preview, rightX, 55, rightWidth, 230, TRUE);
    if (HWND status = GetDlgItem(window, ID_DETAIL_PREVIEW_STATUS))
        MoveWindow(status, rightX, 289, rightWidth, 20, TRUE);
    for (const auto& [id, y] : std::array<std::pair<int, int>, 3>{
             std::pair{ID_DETAIL_OPEN_OUTPUT, 321},
             std::pair{ID_DETAIL_OPEN_SOURCE, 365},
             std::pair{ID_DETAIL_STOP, 421}})
        if (HWND button = GetDlgItem(window, id))
            MoveWindow(button, rightX, y, rightWidth, 34, TRUE);
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
    COLORREF parentSurface = (share || navPage >= 0)
        ? (gDarkTheme ? RGB(23, 24, 32) : RGB(252, 252, 254))
        : (gDarkTheme ? RGB(15, 16, 22) : RGB(244, 245, 249));
    HBRUSH parentBrush = CreateSolidBrush(parentSurface);
    FillRect(item.hDC, &rect, parentBrush);
    DeleteObject(parentBrush);
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
        std::wstring shareText = config.shareEnabled ? L"共享算力\n已开启"
                                                     : L"共享算力\n已关闭";
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

const ui::StableTableModel* ModelForList(UINT_PTR id) {
    switch (id) {
        case ID_NODES: return &gNodeRows;
        case ID_JOBS: return &gJobRows;
        case ID_CONTRIBUTION_LIST: return &gContributionRows;
        case ID_DETAIL_FRAMES: return &gFrameRows;
        default: return nullptr;
    }
}

bool IsKnownListHeader(HWND header) {
    for (int id : {ID_NODES, ID_JOBS, ID_CONTRIBUTION_LIST, ID_DETAIL_FRAMES}) {
        HWND list = id == ID_DETAIL_FRAMES && gDetailWindow
            ? GetDlgItem(gDetailWindow, id) : GetDlgItem(gWindow, id);
        if (list && ListView_GetHeader(list) == header) return true;
    }
    return false;
}

LRESULT HandleHeaderCustomDraw(NMCUSTOMDRAW* draw) {
    if (draw->dwDrawStage == CDDS_PREPAINT) return CDRF_NOTIFYITEMDRAW;
    if (draw->dwDrawStage != CDDS_ITEMPREPAINT) return CDRF_DODEFAULT;
    const COLORREF fill = gDarkTheme ? RGB(38, 39, 50) : RGB(247, 248, 251);
    const COLORREF stroke = gDarkTheme ? RGB(58, 59, 73) : RGB(222, 224, 232);
    const COLORREF text = gDarkTheme ? RGB(205, 207, 220) : RGB(75, 77, 89);
    HBRUSH brush = CreateSolidBrush(fill);
    FillRect(draw->hdc, &draw->rc, brush);
    DeleteObject(brush);
    HPEN pen = CreatePen(PS_SOLID, 1, stroke);
    HGDIOBJ oldPen = SelectObject(draw->hdc, pen);
    MoveToEx(draw->hdc, draw->rc.left, draw->rc.bottom - 1, nullptr);
    LineTo(draw->hdc, draw->rc.right, draw->rc.bottom - 1);
    LineTo(draw->hdc, draw->rc.right, draw->rc.top);
    SelectObject(draw->hdc, oldPen);
    DeleteObject(pen);
    wchar_t buffer[256]{};
    HDITEMW item{};
    item.mask = HDI_TEXT;
    item.pszText = buffer;
    item.cchTextMax = static_cast<int>(std::size(buffer));
    Header_GetItem(draw->hdr.hwndFrom, static_cast<int>(draw->dwItemSpec), &item);
    RECT textRect = draw->rc;
    textRect.left += 9;
    textRect.right -= 6;
    SetBkMode(draw->hdc, TRANSPARENT);
    SetTextColor(draw->hdc, text);
    HGDIOBJ oldFont = SelectObject(draw->hdc, gFont);
    DrawTextW(draw->hdc, buffer, -1, &textRect,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    SelectObject(draw->hdc, oldFont);
    return CDRF_SKIPDEFAULT;
}

int StatusColumnForList(UINT_PTR id) {
    switch (id) {
        case ID_NODES: return 2;
        case ID_JOBS: return 1;
        case ID_CONTRIBUTION_LIST: return 5;
        case ID_DETAIL_FRAMES: return 1;
        default: return -1;
    }
}

COLORREF ListRowBackground(bool selected, size_t row) {
    if (selected) return gDarkTheme ? RGB(64, 59, 132) : RGB(229, 226, 255);
    if ((row & 1U) != 0) return gDarkTheme ? RGB(34, 35, 46) : RGB(249, 249, 252);
    return gDarkTheme ? RGB(31, 32, 42) : RGB(255, 255, 255);
}

void VisualColors(ui::VisualState state, bool selected, COLORREF& fill, COLORREF& text) {
    if (selected) {
        fill = gDarkTheme ? RGB(89, 82, 171) : RGB(208, 203, 255);
        text = gDarkTheme ? RGB(255, 255, 255) : RGB(47, 40, 128);
        return;
    }
    switch (state) {
        case ui::VisualState::Success:
            fill = gDarkTheme ? RGB(27, 72, 59) : RGB(222, 247, 237);
            text = gDarkTheme ? RGB(110, 231, 184) : RGB(0, 111, 76);
            break;
        case ui::VisualState::Active:
            fill = gDarkTheme ? RGB(59, 54, 120) : RGB(235, 232, 255);
            text = gDarkTheme ? RGB(188, 182, 255) : RGB(70, 60, 194);
            break;
        case ui::VisualState::Warning:
            fill = gDarkTheme ? RGB(79, 62, 29) : RGB(255, 244, 214);
            text = gDarkTheme ? RGB(245, 199, 100) : RGB(139, 91, 0);
            break;
        case ui::VisualState::Error:
            fill = gDarkTheme ? RGB(82, 42, 48) : RGB(255, 231, 234);
            text = gDarkTheme ? RGB(255, 158, 168) : RGB(177, 36, 55);
            break;
        default:
            fill = gDarkTheme ? RGB(55, 56, 69) : RGB(236, 237, 242);
            text = gDarkTheme ? RGB(190, 192, 207) : RGB(92, 94, 108);
            break;
    }
}

void DrawStatusPill(const NMLVCUSTOMDRAW& draw, const ui::Row& row, int column,
                    bool selected) {
    RECT cell{};
    ListView_GetSubItemRect(draw.nmcd.hdr.hwndFrom, static_cast<int>(draw.nmcd.dwItemSpec),
                            column, LVIR_BOUNDS, &cell);
    const COLORREF background = ListRowBackground(
        selected, static_cast<size_t>(draw.nmcd.dwItemSpec));
    HBRUSH backgroundBrush = CreateSolidBrush(background);
    FillRect(draw.nmcd.hdc, &cell, backgroundBrush);
    DeleteObject(backgroundBrush);

    std::wstring value = column < static_cast<int>(row.cells.size())
        ? row.cells[static_cast<size_t>(column)] : L"";
    HGDIOBJ oldFont = SelectObject(draw.nmcd.hdc, gFont);
    SIZE measured{};
    GetTextExtentPoint32W(draw.nmcd.hdc, value.c_str(), static_cast<int>(value.size()),
                          &measured);
    const int width = std::min(
        static_cast<int>(cell.right - cell.left - 14),
        std::max(56, static_cast<int>(measured.cx + 20)));
    RECT pill{cell.left + 8, cell.top + 7, cell.left + 8 + width, cell.bottom - 7};
    COLORREF fill{}, text{};
    VisualColors(row.visualState, selected, fill, text);
    HBRUSH fillBrush = CreateSolidBrush(fill);
    HGDIOBJ oldBrush = SelectObject(draw.nmcd.hdc, fillBrush);
    HGDIOBJ oldPen = SelectObject(draw.nmcd.hdc, GetStockObject(NULL_PEN));
    RoundRect(draw.nmcd.hdc, pill.left, pill.top, pill.right, pill.bottom, 16, 16);
    SelectObject(draw.nmcd.hdc, oldBrush);
    SelectObject(draw.nmcd.hdc, oldPen);
    DeleteObject(fillBrush);
    SetBkMode(draw.nmcd.hdc, TRANSPARENT);
    SetTextColor(draw.nmcd.hdc, text);
    DrawTextW(draw.nmcd.hdc, value.c_str(), -1, &pill,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    SelectObject(draw.nmcd.hdc, oldFont);
}

void DrawProgressCell(const NMLVCUSTOMDRAW& draw, const ui::Row& row, bool selected) {
    RECT cell{};
    ListView_GetSubItemRect(draw.nmcd.hdr.hwndFrom, static_cast<int>(draw.nmcd.dwItemSpec),
                            3, LVIR_BOUNDS, &cell);
    const COLORREF background = ListRowBackground(
        selected, static_cast<size_t>(draw.nmcd.dwItemSpec));
    HBRUSH backgroundBrush = CreateSolidBrush(background);
    FillRect(draw.nmcd.hdc, &cell, backgroundBrush);
    DeleteObject(backgroundBrush);
    int completed = 0, total = 0;
    const std::wstring value = row.cells.size() > 3 ? row.cells[3] : L"0 / 0";
    swscanf_s(value.c_str(), L"%d / %d", &completed, &total);
    RECT track{cell.left + 8, cell.top + 14, cell.right - 8, cell.bottom - 14};
    HBRUSH trackBrush = CreateSolidBrush(gDarkTheme ? RGB(55, 56, 69) : RGB(230, 231, 237));
    FillRect(draw.nmcd.hdc, &track, trackBrush);
    DeleteObject(trackBrush);
    if (total > 0 && completed > 0) {
        RECT progress = track;
        progress.right = progress.left + MulDiv(track.right - track.left, completed, total);
        HBRUSH progressBrush = CreateSolidBrush(selected && gDarkTheme
            ? RGB(202, 198, 255) : kAccent);
        FillRect(draw.nmcd.hdc, &progress, progressBrush);
        DeleteObject(progressBrush);
    }
    SetBkMode(draw.nmcd.hdc, TRANSPARENT);
    SetTextColor(draw.nmcd.hdc, selected && gDarkTheme ? RGB(255, 255, 255)
        : gDarkTheme ? RGB(238, 239, 246) : RGB(40, 41, 49));
    HGDIOBJ oldFont = SelectObject(draw.nmcd.hdc, gFont);
    DrawTextW(draw.nmcd.hdc, value.c_str(), -1, &cell,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    SelectObject(draw.nmcd.hdc, oldFont);
}

LRESULT HandleListCustomDraw(NMLVCUSTOMDRAW* draw) {
    const ui::StableTableModel* model = ModelForList(draw->nmcd.hdr.idFrom);
    if (!model) {
        if (IsKnownListHeader(draw->nmcd.hdr.hwndFrom))
            return HandleHeaderCustomDraw(&draw->nmcd);
        return CDRF_DODEFAULT;
    }
    if (draw->nmcd.dwDrawStage == CDDS_PREPAINT) return CDRF_NOTIFYITEMDRAW;
    if (draw->nmcd.dwDrawStage == CDDS_ITEMPREPAINT) {
        const bool selected = (ListView_GetItemState(
            draw->nmcd.hdr.hwndFrom, static_cast<int>(draw->nmcd.dwItemSpec),
            LVIS_SELECTED) & LVIS_SELECTED) != 0;
        draw->clrTextBk = ListRowBackground(
            selected, static_cast<size_t>(draw->nmcd.dwItemSpec));
        draw->clrText = selected && gDarkTheme ? RGB(255, 255, 255)
            : gDarkTheme ? RGB(235, 236, 243) : RGB(39, 40, 48);
        return CDRF_NOTIFYSUBITEMDRAW;
    }
    if (draw->nmcd.dwDrawStage == (CDDS_ITEMPREPAINT | CDDS_SUBITEM)) {
        const size_t index = static_cast<size_t>(draw->nmcd.dwItemSpec);
        if (index >= model->Rows().size()) return CDRF_DODEFAULT;
        const bool selected = (ListView_GetItemState(
            draw->nmcd.hdr.hwndFrom, static_cast<int>(draw->nmcd.dwItemSpec),
            LVIS_SELECTED) & LVIS_SELECTED) != 0;
        const int statusColumn = StatusColumnForList(draw->nmcd.hdr.idFrom);
        if (draw->iSubItem == statusColumn) {
            DrawStatusPill(*draw, model->Rows()[index], statusColumn, selected);
            return CDRF_SKIPDEFAULT;
        }
        if (draw->nmcd.hdr.idFrom == ID_JOBS && draw->iSubItem == 3) {
            DrawProgressCell(*draw, model->Rows()[index], selected);
            return CDRF_SKIPDEFAULT;
        }
    }
    return CDRF_DODEFAULT;
}

void WriteUiMetricsReport() {
    if (gUiMetricsReport.empty()) return;
    HWND jobs = Item(ID_JOBS);
    HWND share = Item(ID_SHARE);
    const int jobsVisibleRows = ListView_GetCountPerPage(jobs);
    RECT shareRect{};
    GetClientRect(share, &shareRect);
    RECT label = shareRect;
    label.left += 14;
    label.right -= 72;
    Config config = gApp->GetConfig();
    std::wstring shareText = config.shareEnabled ? L"共享算力\n已开启"
                                                 : L"共享算力\n已关闭";
    HDC dc = GetDC(share);
    HGDIOBJ oldFont = SelectObject(dc, gFont);
    RECT measured{0, 0, label.right - label.left, 0};
    DrawTextW(dc, shareText.c_str(), -1, &measured,
              DT_LEFT | DT_WORDBREAK | DT_CALCRECT);
    SelectObject(dc, oldFont);
    ReleaseDC(share, dc);
    const uint64_t mutationCountBeforeNoOp = gListMutationCount;
    const bool noOpReportedMutation = SyncTable(Item(ID_NODES), gNodeRows, gNodeRows.Rows());
    const uint64_t stableSnapshotMutationDelta =
        gListMutationCount - mutationCountBeforeNoOp;
    bool scrollAnchorPreserved = true;
    bool selectionPreserved = true;
    bool focusPreserved = true;
    bool horizontalScrollPreserved = true;
    bool horizontalScrollWasNonzero = true;
    bool topPixelOffsetPreserved = true;
    bool controlOrderMatchesModel = true;
    bool structuralUpdateApplied = true;
    uint64_t incrementalMutationDelta = 0;
    if (gUiTestRows > 0) {
        std::vector<ui::Row> syntheticRows;
        syntheticRows.reserve(static_cast<size_t>(gUiTestRows));
        for (int index = 0; index < gUiTestRows; ++index) {
            syntheticRows.push_back({
                L"synthetic-job-" + std::to_wstring(index),
                {L"性能测试 " + std::to_wstring(index), L"待提交", L"100", L"0 / 100",
                 L"-", L"-", L"-"},
                ui::VisualState::Warning,
            });
        }
        SyncTable(jobs, gJobRows, syntheticRows);
        const int anchorTarget = std::min(50, gUiTestRows - 1);
        ListView_EnsureVisible(jobs, anchorTarget, FALSE);
        const int topBefore = ListView_GetTopIndex(jobs);
        const int selectedBefore = std::min(gUiTestRows - 1, topBefore + 2);
        ListView_SetItemState(jobs, selectedBefore, LVIS_SELECTED | LVIS_FOCUSED,
                              LVIS_SELECTED | LVIS_FOCUSED);
        RECT topRectBefore{};
        ListView_GetItemRect(jobs, topBefore, &topRectBefore, LVIR_BOUNDS);
        ListView_SetColumnWidth(jobs, 6, 600);
        ListView_Scroll(jobs, 120, 0);
        const int horizontalBefore = GetScrollPos(jobs, SB_HORZ);
        horizontalScrollWasNonzero = horizontalBefore > 0;
        const std::wstring topKeyBefore = gJobRows.Rows()[static_cast<size_t>(topBefore)].key;
        const std::wstring selectedKeyBefore =
            gJobRows.Rows()[static_cast<size_t>(selectedBefore)].key;
        auto changedRows = syntheticRows;
        const int changedIndex = std::min(gUiTestRows - 1, topBefore + 5);
        changedRows[static_cast<size_t>(changedIndex)].cells[1] = L"渲染中";
        changedRows[static_cast<size_t>(changedIndex)].visualState =
            ui::VisualState::Active;
        const uint64_t beforeIncrementalUpdate = gListMutationCount;
        SyncTable(jobs, gJobRows, changedRows);
        incrementalMutationDelta = gListMutationCount - beforeIncrementalUpdate;

        auto structuralRows = changedRows;
        structuralRows.insert(structuralRows.begin(), {
            L"synthetic-job-new",
            {L"新到任务", L"待提交", L"1", L"0 / 1", L"-", L"-", L"-"},
            ui::VisualState::Warning,
        });
        if (structuralRows.size() > 10)
            std::swap(structuralRows[8], structuralRows[9]);
        if (structuralRows.size() > 2) structuralRows.pop_back();
        const uint64_t beforeStructuralUpdate = gListMutationCount;
        SyncTable(jobs, gJobRows, std::move(structuralRows));
        const uint64_t structuralMutationDelta =
            gListMutationCount - beforeStructuralUpdate;
        const int topAfter = ListView_GetTopIndex(jobs);
        const int selectedAfter = ListView_GetNextItem(jobs, -1, LVNI_SELECTED);
        const int focusedAfter = ListView_GetNextItem(jobs, -1, LVNI_FOCUSED);
        RECT topRectAfter{};
        ListView_GetItemRect(jobs, topAfter, &topRectAfter, LVIR_BOUNDS);
        scrollAnchorPreserved = topAfter >= 0 &&
            gJobRows.Rows()[static_cast<size_t>(topAfter)].key == topKeyBefore;
        selectionPreserved = selectedAfter >= 0 &&
            gJobRows.Rows()[static_cast<size_t>(selectedAfter)].key == selectedKeyBefore;
        focusPreserved = focusedAfter >= 0 &&
            gJobRows.Rows()[static_cast<size_t>(focusedAfter)].key == selectedKeyBefore;
        horizontalScrollPreserved = GetScrollPos(jobs, SB_HORZ) == horizontalBefore;
        topPixelOffsetPreserved = topRectAfter.top == topRectBefore.top;
        controlOrderMatchesModel =
            ListView_GetItemCount(jobs) == static_cast<int>(gJobRows.Rows().size());
        wchar_t controlText[512]{};
        for (int row = 0; controlOrderMatchesModel &&
             row < ListView_GetItemCount(jobs); ++row) {
            ListView_GetItemText(jobs, row, 0, controlText,
                                 static_cast<int>(std::size(controlText)));
            controlOrderMatchesModel =
                gJobRows.Rows()[static_cast<size_t>(row)].cells[0] == controlText;
        }
        structuralUpdateApplied = structuralMutationDelta >= 2;
        ListView_SetColumnWidth(jobs, 6, 155);
    }
    json metrics = {
        {"jobs_visible_rows", jobsVisibleRows},
        {"share_text_width", measured.right - measured.left},
        {"share_text_height", measured.bottom - measured.top},
        {"share_label_width", label.right - label.left},
        {"share_label_height", label.bottom - label.top},
        {"share_text_fits", measured.right - measured.left <= label.right - label.left &&
                             measured.bottom - measured.top <= label.bottom - label.top},
        {"stable_snapshot_mutated", noOpReportedMutation},
        {"stable_snapshot_mutation_delta", stableSnapshotMutationDelta},
        {"refresh_count", gRefreshCount},
        {"list_mutation_count", gListMutationCount},
        {"idle_heartbeat_interval_ms", IDLE_HEARTBEAT_INTERVAL_MS},
        {"burst_refresh_min_interval_ms", ui::MinimumRefreshInterval().count()},
        {"ui_test_burst_notifications", UI_TEST_BURST_NOTIFICATIONS},
        {"observed_burst_refreshes", gBurstTimerRefreshCount},
        {"observed_heartbeat_refreshes", gHeartbeatTimerRefreshCount},
        {"scroll_anchor_preserved", scrollAnchorPreserved},
        {"selection_preserved", selectionPreserved},
        {"focus_preserved", focusPreserved},
        {"horizontal_scroll_preserved", horizontalScrollPreserved},
        {"horizontal_scroll_was_nonzero", horizontalScrollWasNonzero},
        {"top_pixel_offset_preserved", topPixelOffsetPreserved},
        {"control_order_matches_model", controlOrderMatchesModel},
        {"incremental_mutation_delta", incrementalMutationDelta},
        {"structural_update_applied", structuralUpdateApplied},
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
            HWND frames = CreateWindowExW(0, WC_LISTVIEWW, L"",
                WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
                22, 55, 805, 520, window,
                reinterpret_cast<HMENU>(ID_DETAIL_FRAMES), GetModuleHandleW(nullptr), nullptr);
            SendMessageW(frames, WM_SETFONT, reinterpret_cast<WPARAM>(gFont), TRUE);
            ConfigureList(frames);
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
            LayoutDetailWindow(window);
            RefreshJobDetail(false);
            return 0;
        }
        case WM_SIZE:
            if (wParam == SIZE_MINIMIZED) return 0;
            LayoutDetailWindow(window);
            return 0;
        case WM_GETMINMAXINFO: {
            auto* limits = reinterpret_cast<MINMAXINFO*>(lParam);
            limits->ptMinTrackSize = {1000, 600};
            return 0;
        }
        case WM_NOTIFY: {
            auto* header = reinterpret_cast<NMHDR*>(lParam);
            if (header->code == NM_CUSTOMDRAW)
                return HandleListCustomDraw(reinterpret_cast<NMLVCUSTOMDRAW*>(lParam));
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
                    if (selected >= 0 && selected < static_cast<int>(gFrameRows.Rows().size())) {
                        const auto& key = gFrameRows.Rows()[static_cast<size_t>(selected)].key;
                        auto frame = std::find_if(job->frames.begin(), job->frames.end(),
                            [&](const FrameTask& value) {
                                return Utf8ToWide(value.taskId.empty()
                                    ? job->id + ":" + std::to_string(value.frame)
                                    : value.taskId) == key;
                            });
                        if (frame != job->frames.end()) ShowInExplorer(frame->output);
                    }
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
            StopPreviewGeneration();
            gPreviewSource.clear();
            gPreviewTaskId.clear();
            ReplacePreviewBitmap(nullptr);
            gDetailWindow = nullptr;
            gDetailJobId.clear();
            gFrameRows = ui::StableTableModel{};
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
    if (selected < 0 || selected >= static_cast<int>(gJobRows.Rows().size())) {
        MessageBoxW(gWindow, L"请先选择一个渲染任务。", L"RenderMesh", MB_ICONINFORMATION);
        return;
    }
    gDetailJobId = WideToUtf8(gJobRows.Rows()[static_cast<size_t>(selected)].key);
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
            CaptureBaseLayout();
            LayoutUi();
            LoadConfigIntoUi();
            ApplyTheme();
            SetTimer(window, TIMER_HEARTBEAT, IDLE_HEARTBEAT_INTERVAL_MS, nullptr);
            RefreshLists();
            if (!gUiMetricsReport.empty()) {
                for (int index = 0; index < UI_TEST_BURST_NOTIFICATIONS; ++index)
                    QueueModelRefresh();
                SetTimer(window, TIMER_UI_TEST_REPORT, 1200, nullptr);
            }
            if (gOpenInitialDetail && ListView_GetItemCount(Item(ID_JOBS)) > 0) {
                ListView_SetItemState(Item(ID_JOBS), 0, LVIS_SELECTED | LVIS_FOCUSED,
                                      LVIS_SELECTED | LVIS_FOCUSED);
                OpenSelectedJobDetail();
            }
            return 0;
        case WM_SIZE:
            if (wParam == SIZE_MINIMIZED) return 0;
            if (gRenderTarget)
                gRenderTarget->Resize(D2D1::SizeU(
                    static_cast<UINT>(std::max(1, static_cast<int>(LOWORD(lParam)))),
                    static_cast<UINT>(std::max(1, static_cast<int>(HIWORD(lParam))))));
            LayoutUi();
            return 0;
        case WM_GETMINMAXINFO: {
            auto* limits = reinterpret_cast<MINMAXINFO*>(lParam);
            limits->ptMinTrackSize = {1120, 720};
            return 0;
        }
        case WM_PAINT:
            PaintChrome(window);
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
                case ID_NAV_DASHBOARD: ShowPage(0); RefreshLists(); break;
                case ID_NAV_TASKS: ShowPage(1); RefreshLists(); break;
                case ID_NAV_CONTRIBUTIONS: ShowPage(2); RefreshLists(); break;
                case ID_NAV_SETTINGS: ShowPage(3); RefreshLists(); break;
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
            if (header->code == NM_CUSTOMDRAW)
                return HandleListCustomDraw(reinterpret_cast<NMLVCUSTOMDRAW*>(lParam));
            if (header->idFrom == ID_JOBS && header->code == NM_DBLCLK)
                OpenSelectedJobDetail();
            return 0;
        }
        case WM_TIMER:
            if (wParam == TIMER_HEARTBEAT) {
                if (gRefreshQueued.exchange(false)) KillTimer(window, TIMER_BURST_REFRESH);
                ++gHeartbeatTimerRefreshCount;
                RefreshLists();
            } else if (wParam == TIMER_BURST_REFRESH) {
                KillTimer(window, TIMER_BURST_REFRESH);
                gRefreshQueued = false;
                ++gBurstTimerRefreshCount;
                RefreshLists();
            } else if (wParam == TIMER_UI_TEST_REPORT) {
                KillTimer(window, TIMER_UI_TEST_REPORT);
                WriteUiMetricsReport();
                PostMessageW(window, WM_CLOSE, 0, 0);
            }
            return 0;
        case WM_RM_REFRESH:
            HandleQueuedRefresh();
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
            KillTimer(window, TIMER_HEARTBEAT);
            KillTimer(window, TIMER_BURST_REFRESH);
            KillTimer(window, TIMER_UI_TEST_REPORT);
            gWindow = nullptr;
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
        case WM_ERASEBKGND:
            return TRUE;
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
    bool selfTest = false, headless = false, noDialog = false;
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
        else if (arg == L"--ui-test-rows" && i + 1 < argumentCount)
            gUiTestRows = std::clamp(_wtoi(arguments[++i]), 0, 10000);
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
            {"requires_gpu", result.requiresGpu},
            {"prepared", WideToUtf8(result.preparedBlend.wstring())},
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
                manifest.value("file_extension", ".png"), false, stopRequested,
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
                                    stopRequested, output, renderError);
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
            compatibilityStop, summary);
        PrintToParentConsole(summary);
        return ok ? 0 : 5;
    }

    ConfigStore store(dataDir);
    Config config = store.Load();
    if (overridePort >= 1024 && overridePort <= 65535) config.httpPort = overridePort;
    gApp = std::make_unique<NodeApp>(std::move(store), config);
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
                        DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI Variable Text");
    gTitleFont = CreateFontW(-28, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                             OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                             DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI Variable Display");
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
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX |
            WS_MAXIMIZEBOX | WS_THICKFRAME | WS_CLIPCHILDREN,
        CW_USEDEFAULT, CW_USEDEFAULT, 1280, 820, nullptr, nullptr, instance, nullptr);
    gApp->SetChangedCallback([] {
        QueueModelRefresh();
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
    ReleaseGraphics();
    if (gRowHeightImages) ImageList_Destroy(gRowHeightImages);
    DeleteObject(gFont);
    DeleteObject(gTitleFont);
    if (gBackgroundBrush) DeleteObject(gBackgroundBrush);
    if (gSurfaceBrush) DeleteObject(gSurfaceBrush);
    CoUninitialize();
    return static_cast<int>(message.wParam);
}
