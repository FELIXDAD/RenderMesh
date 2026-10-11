#include "Render.hpp"
#include "BlenderInstaller.hpp"
#include "Cli.hpp"
#include "UiModel.hpp"

#include <commctrl.h>
#include <commdlg.h>
#include <d2d1.h>
#include <d2d1helper.h>
#include <dwrite.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <uxtheme.h>
#include <wincodec.h>
#include <windowsx.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cwctype>
#include <functional>
#include <memory>
#include <limits>
#include <set>
#include <unordered_map>
#include <utility>

namespace {

using namespace rm;

constexpr UINT WM_RM_REFRESH = WM_APP + 1;
constexpr UINT WM_RM_PREFLIGHT = WM_APP + 2;
constexpr UINT WM_RM_PREVIEW_READY = WM_APP + 3;
constexpr UINT WM_RM_JOB_OPERATION = WM_APP + 5;
constexpr UINT WM_RM_WORKFLOW_PROGRESS = WM_APP + 6;
constexpr UINT WM_RM_OPEN_CONFIG_DRAWER = WM_APP + 7;
constexpr UINT WM_RM_BLEND_DROP = WM_APP + 8;
constexpr UINT WM_RM_TRAY = WM_APP + 9;
constexpr UINT WM_RM_UPDATE_CHECKED = WM_APP + 10;
constexpr UINT WM_RM_UPDATE_DOWNLOADED = WM_APP + 11;
constexpr UINT WM_RM_BLENDER_INSTALL_PROGRESS = WM_APP + 12;
constexpr UINT WM_RM_BLENDER_INSTALLED = WM_APP + 13;
constexpr UINT WM_RM_BLENDER_CATALOG = WM_APP + 14;
constexpr UINT WM_RM_UPDATE_APPLIED = WM_APP + 15;
constexpr UINT_PTR TIMER_HEARTBEAT = 1;
constexpr UINT_PTR TIMER_BURST_REFRESH = 3;
constexpr UINT_PTR TIMER_UI_TEST_REPORT = 4;
constexpr UINT_PTR TIMER_FRAME_LOG = 5;
constexpr UINT_PTR TIMER_UPLOAD_PROGRESS = 6;
constexpr UINT_PTR TIMER_SETTINGS_AUTOSAVE = 7;
constexpr UINT_PTR TIMER_CONFIG_DRAWER_PREVIEW = 8;
constexpr UINT_PTR TIMER_MENU_ACTION = 9;
constexpr UINT_PTR TIMER_CONTEXT_MENU_OPEN = 10;
constexpr UINT_PTR TIMER_UPDATE_CHECK = 11;
constexpr UINT_PTR TIMER_WORKFLOW_PROGRESS = 12;
constexpr UINT IDLE_HEARTBEAT_INTERVAL_MS = 1000;
constexpr int UI_TEST_BURST_NOTIFICATIONS = 100;
constexpr int IDI_RENDERMESH = 101;
// Defined locally so the executable does not link libuuid's full GUID table.
constexpr GUID kClsidFileOpenDialog = {
    0xdc1c5a9c, 0xe88a, 0x4dde, {0xa5, 0xa1, 0x60, 0xf8, 0x2a, 0x20, 0xae, 0xf7}};
constexpr GUID kClsidWicImagingFactory = {
    0xcacaf262, 0x9370, 0x4615, {0xa1, 0x3b, 0x9f, 0x55, 0x39, 0xda, 0x4c, 0x0a}};
constexpr GUID kWicPixelFormat32bppBGRA = {
    0x6fddc324, 0x4e03, 0x4bfe, {0xb1, 0x85, 0x3d, 0x77, 0x76, 0x8d, 0xc9, 0x0f}};

enum ControlId {
    ID_SHARE = 100,
    ID_NODE_NAME,
    ID_IDLE_SECONDS,
    ID_BLENDER_PATH,
    ID_BROWSE_BLENDER,
    ID_CLUSTER_TOKEN,
    ID_COPY_TOKEN,
    ID_SETTINGS_STATUS,
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
    ID_LABEL_SOURCE,
    ID_LABEL_JOB_NAME,
    ID_LABEL_FRAME_START,
    ID_LABEL_FRAME_END,
    ID_LABEL_FRAME_STEP,
    ID_TASK_HELP,
    ID_TASK_QUEUE_LABEL,
    ID_DROP_OVERLAY,
    ID_DETAIL_FRAMES = 300,
    ID_DETAIL_SUMMARY,
    ID_DETAIL_PREVIEW,
    ID_DETAIL_OPEN_SOURCE,
    ID_DETAIL_OPEN_OUTPUT,
    ID_DETAIL_STOP,
    ID_DETAIL_FULL_RENDER,
    ID_DETAIL_RETRY_FAILED,
    ID_DETAIL_RETRY_FRAME,
    ID_DETAIL_PREVIEW_STATUS,
    ID_LOG_SUMMARY = 350,
    ID_LOG_VIEW,
    ID_LOG_SEARCH,
    ID_LOG_FIND,
    ID_LOG_COPY,
    ID_LOG_OPEN,
    ID_LOG_RETRY,
    ID_JOB_PRIORITY = 400,
    ID_JOB_SAVE_AS,
    ID_JOB_OPEN_OUTPUT,
    ID_JOB_CONTEXT_DETAILS,
    ID_JOB_COPY_ID,
    ID_JOB_COPY_NAME,
    ID_JOB_COPY_BOTH,
    ID_JOB_DELETE,
    ID_JOB_START_FULL,
    ID_ADD_JOB,
    ID_JOB_RETRY_FAILED,
    ID_JOB_RESUME,
    ID_JOB_EDIT_PARTICIPANTS,
    ID_JOB_BULK_START_FULL,
    ID_JOB_BULK_DELETE,
    ID_CONTRIBUTION_CLEAR_SELECTED,
    ID_INTERNAL_DIALOG = 600,
    ID_INTERNAL_CANCEL,
    ID_INTERNAL_CONFIRM,
    ID_INTERNAL_REMEMBER,
    ID_INTERNAL_VERSION,
    ID_WORKFLOW_OVERLAY,
    ID_WORKFLOW_PROGRESS,
    ID_WORKFLOW_CANCEL,
    ID_UPLOAD_LIST = 500,
    ID_UPLOAD_SUMMARY,
    ID_UPLOAD_TITLE,
    ID_UPLOAD_HINT,
    ID_UPLOAD_RETRY,
    ID_UPLOAD_ADD,
    ID_UPLOAD_NEXT,
    ID_UPLOAD_CLOSE,
    ID_UPLOAD_SELECT_ALL,
    ID_UPLOAD_INVERT_SELECTION,
    ID_UPLOAD_DISCONNECT,
    ID_UPLOAD_CANCEL,
    ID_CONFIG_START,
    ID_CONFIG_END,
    ID_CONFIG_STEP,
    ID_CONFIG_PACKAGE,
    ID_CONFIG_TEST_FRAMES,
    ID_CONFIG_SUBMIT,
    ID_CONFIG_CANCEL,
    ID_CONFIG_PROFILE_SELECT,
    ID_CONFIG_PROFILE_DRAWER,
    ID_CONFIG_PROFILE_EDIT,
    ID_CONFIG_PROFILE_NEW,
    ID_CONFIG_PROFILE_NAME,
    ID_CONFIG_PROFILE_SYSTEM,
    ID_CONFIG_PROFILE_SOFTWARE,
    ID_CONFIG_PROFILE_PATH,
    ID_CONFIG_PROFILE_BROWSE,
    ID_CONFIG_PROFILE_SAVE,
    ID_CONFIG_PROFILE_DELETE,
    ID_CLOSE_BEHAVIOR = 720,
    ID_AUTO_START,
    ID_SILENT_AUTO_START,
    ID_SILENT_AUTO_START_INFO,
    ID_CURRENT_VERSION,
    ID_CHECK_UPDATE,
    ID_INSTALL_BLENDER,
    ID_PROJECT_STORAGE_PATH,
    ID_CHANGE_PROJECT_STORAGE,
    ID_TRAY_STATUS,
    ID_TRAY_TOGGLE_SHARE,
    ID_TRAY_EXIT,
};

std::unique_ptr<NodeApp> gApp;
HWND gWindow = nullptr;
HANDLE gSingleInstanceMutex = nullptr;
HANDLE gSingleInstanceReadyEvent = nullptr;
UINT gRestoreInstanceMessage = 0;
uint64_t gSingleInstanceKey = 0;
bool gAutoStartRegistrationEnabled = true;
std::wstring gAutoStartStartupWarning;
HFONT gFont = nullptr;
HFONT gTitleFont = nullptr;
HFONT gIconFont = nullptr;
HFONT gSemiboldFont = nullptr;
HFONT gMatchIdFont = nullptr;
std::thread gPreflight;
std::atomic<bool> gPreflightRunning = false;
std::atomic<bool> gPreflightStop = false;
std::atomic<int> gImportPermille = 0;
std::thread gJobOperation;
std::atomic<bool> gJobOperationRunning = false;
std::atomic<bool> gJobOperationCancel = false;
std::atomic<int> gStorageMigrationPercent = 0;
std::atomic<bool> gUploadCancelRunning = false;
std::atomic<bool> gUiShuttingDown = false;
std::mutex gJobOperationResultMutex;
struct JobOperationResult;
JobOperationResult* gPendingJobOperationResult = nullptr;
std::thread gUpdateCheckThread;
std::thread gUpdateDownloadThread;
std::thread gUpdateApplyThread;
std::atomic<bool> gUpdateCheckRunning = false;
std::atomic<bool> gUpdateDownloadRunning = false;
std::atomic<bool> gUpdateApplyRunning = false;
std::atomic<bool> gUpdateCancel = false;
std::thread gBlenderInstallThread;
std::atomic<bool> gBlenderInstallRunning = false;
std::atomic<bool> gBlenderInstallCancel = false;
bool gBlenderInstallPromptShown = false;
bool gBlenderInstallNvidiaAvailable = false;
bool gBlenderRequestedOptix = false;
std::vector<BlenderRelease> gBlenderCatalog;
bool gDisableUpdateChecks = false;
bool gUpdateApplying = false;
bool gManualUpdateCheck = false;
HWND gSilentAutoStartTooltip = nullptr;
HWND gDetailWindow = nullptr;
std::string gDetailJobId;
HWND gFrameLogWindow = nullptr;
std::string gFrameLogJobId;
std::string gFrameLogTaskId;
std::wstring gFrameLogDisplayedText;
HFONT gLogFont = nullptr;
HBITMAP gPreviewBitmap = nullptr;
std::thread gPreviewThread;
std::atomic<bool> gPreviewStop = false;
std::atomic<uint64_t> gPreviewGeneration = 0;
fs::path gPreviewSource;
std::string gPreviewTaskId;
HWND gUploadWindow = nullptr;
HWND gConfigWindow = nullptr;
HWND gConfigProfileDrawer = nullptr;
int gConfigProfileDrawerHover = -1;
bool gConfigProfileEditing = false;
bool gConfigProfileDeleteArmed = false;
HWND gInternalDialog = nullptr;
HBITMAP gInternalDialogBackdrop = nullptr;
HWND gWorkflowProgressOverlay = nullptr;
HBITMAP gWorkflowProgressBackdrop = nullptr;
std::wstring gWorkflowProgressTitle;
std::wstring gWorkflowProgressDetail;
int gWorkflowProgressPercent = 0;
std::chrono::steady_clock::time_point gWorkflowProgressStartedAt{};
bool gWorkflowProgressCancelVisible = false;
bool gWorkflowProgressCancelRequested = false;
enum class InternalDialogMode {
    DeleteConfirmation,
    ContributionDeleteConfirmation,
    PreflightWarningOverride,
    UpdateAvailable,
    BlenderMissing,
    BlenderVersionPicker,
    CloseBehavior,
    Notice,
};
InternalDialogMode gInternalDialogMode = InternalDialogMode::DeleteConfirmation;
std::vector<std::string> gPendingDeleteJobIds;
std::vector<std::string> gPendingContributionPackageIds;
std::optional<UpdateOffer> gPendingUpdateOffer;
std::string gSkippedUpdateVersion;
std::wstring gInternalDialogTitle;
std::wstring gInternalDialogSubject;
std::wstring gInternalDialogBody;
std::string gUploadJobId;
bool gUploadEditMode = false;
bool gUploadSyncingChecks = false;
std::set<std::string> gUploadSelectableNodeIds;
bool gMainRefreshDeferredByModal = false;
std::string gConfigJobId;
std::string gConfigProfileId;
bool gNewJobFlow = false;
bool gRefreshingJobDetail = false;
int gCurrentPage = 0;
int gInitialPage = 0;
int gPageBeforeSettings = 0;
bool gOpenInitialDetail = false;
bool gPreviewDeleteConfirmation = false;
bool gPreviewConfigWindow = false;
bool gPreviewConfigDrawer = false;
bool gPreviewDropOverlay = false;
bool gPreviewBlenderMissing = false;
bool gDropOverlayVisible = false;
class MainDropTarget;
MainDropTarget* gMainDropTarget = nullptr;
fs::path gUiMetricsReport;
#ifdef RM_DEV_TOOLS
int gUiTestRows = 0;
#endif
bool gDarkTheme = false;
bool gLoadingSettings = false;
bool gSettingsDirty = false;
bool gForceExit = false;
bool gTrayIconAdded = false;
UINT gTaskbarCreatedMessage = RegisterWindowMessageW(L"TaskbarCreated");
NOTIFYICONDATAW gTrayIconData{};
std::array<HICON, 3> gTrayIcons{};
std::vector<std::function<void()>> gDeferredMenuActions;
UINT gPendingContextMenuControl = 0;
POINT gPendingContextMenuPoint{};
HBRUSH gBackgroundBrush = nullptr;
HBRUSH gSidebarBrush = nullptr;
HBRUSH gSurfaceBrush = nullptr;
std::array<std::vector<HWND>, 4> gPageControls;
ui::StableTableModel gNodeRows;
ui::StableTableModel gJobRows;
ui::StableTableModel gContributionRows;
ui::StableTableModel gFrameRows;
ui::StableTableModel gUploadRows;
std::atomic<bool> gRefreshQueued = false;
std::chrono::steady_clock::time_point gLastRefresh{};
uint64_t gListMutationCount = 0;
uint64_t gRefreshCount = 0;
uint64_t gHeartbeatTimerRefreshCount = 0;
uint64_t gBurstTimerRefreshCount = 0;
uint64_t gSelfFramedFieldPaintCount = 0;
uint64_t gLastJobRevision = std::numeric_limits<uint64_t>::max();
ID2D1Factory* gD2dFactory = nullptr;
IDWriteFactory* gDwriteFactory = nullptr;
ID2D1HwndRenderTarget* gRenderTarget = nullptr;
IDWriteTextFormat* gCardValueFormat = nullptr;
IDWriteTextFormat* gCardLabelFormat = nullptr;
IDWriteTextFormat* gChromeTitleFormat = nullptr;
UINT gChromeTitleFormatDpi = 0;
HIMAGELIST gRowHeightImages = nullptr;
SIZE gLayoutBaseClient{};
std::unordered_map<HWND, RECT> gBaseControlRects;

enum class StaticSurfaceRole : ULONG_PTR {
    Background = 1,
    Sidebar = 2,
    Surface = 3,
};

constexpr wchar_t kStaticSurfaceRoleProperty[] = L"RM_STATIC_SURFACE_ROLE";
constexpr wchar_t kFieldHoverProperty[] = L"RM_FIELD_HOVER";
constexpr wchar_t kFieldSelfFrameProperty[] = L"RM_FIELD_SELF_FRAME";
constexpr wchar_t kThemeSelectionProperty[] = L"RM_THEME_SELECTION";
constexpr wchar_t kCloseBehaviorSelectionProperty[] = L"RM_CLOSE_BEHAVIOR_SELECTION";
constexpr wchar_t kDropdownAffordanceProperty[] = L"RM_DROPDOWN_AFFORDANCE";
constexpr wchar_t kIconActionProperty[] = L"RM_ICON_ACTION";
constexpr wchar_t kInternalDialogButtonProperty[] = L"RM_INTERNAL_DIALOG_BUTTON";
constexpr wchar_t kSettingToggleCheckedProperty[] = L"RM_SETTING_TOGGLE_CHECKED";
constexpr wchar_t kSingleInstanceKeyProperty[] = L"RM_SINGLE_INSTANCE_KEY";
constexpr UINT_PTR kFieldSubclassId = 0x524D4649;
constexpr UINT_PTR kStaticLabelSubclassId = 0x524D5354;
struct ChromeState {
    int hot = HTNOWHERE;
    int pressed = HTNOWHERE;
    bool active = true;
};

std::unordered_map<HWND, ChromeState> gChromeStates;

struct ListScrollbarState {
    bool hovered = false;
    bool dragging = false;
    int dragOffset = 0;
    int wheelDeltaRemainder = 0;
};

std::unordered_map<HWND, ListScrollbarState> gListScrollbarStates;

enum class JobOperationKind {
    Generic,
    CancelUpload,
    StorageMigration,
};

struct JobOperationResult {
    bool ok = false;
    JobOperationKind kind = JobOperationKind::Generic;
    std::string jobId;
    std::wstring message;
};

struct UpdateCheckResult {
    std::optional<UpdateOffer> offer;
    std::wstring error;
};

struct UpdateDownloadResult {
    bool ok = false;
    UpdateOffer offer;
    fs::path pendingExecutable;
    std::wstring error;
};

struct UpdateApplyResult {
    bool ok = false;
    std::wstring error;
};

struct BlenderCatalogResult {
    std::vector<BlenderRelease> releases;
    std::wstring error;
};

struct DashboardView {
    int onlineNodes = 0;
    int activeJobs = 0;
    int completedFrames = 0;
    int totalFrames = 0;
    int assignedFrames = 0;
    int idleSeconds = 0;
    bool rendererReady = false;
    std::wstring matchId;

    bool operator==(const DashboardView&) const = default;
};

DashboardView gDashboardView;

void ApplyWorkflowTheme(HWND window, HWND list);
HWND Item(int id);
void LayoutUi(bool updateColumns = true);
LRESULT CALLBACK WorkflowProgressProc(HWND window, UINT message,
                                      WPARAM wParam, LPARAM lParam);

enum class WorkflowProgressStage : WPARAM {
    Inspecting = 1,
    CreatingTask = 3,
    DownloadingUpdate = 4,
    InstallingBlender = 5,
    FetchingBlenderCatalog = 6,
    ApplyingUpdate = 7,
    MigratingStorage = 8,
};

WorkflowProgressStage gWorkflowProgressStage = WorkflowProgressStage::Inspecting;

void ShowWorkflowProgress(WorkflowProgressStage stage);
void CloseWorkflowProgress();

struct PreflightUiResult {
    bool ok = false;
    bool cancelled = false;
    bool canIgnoreAssetWarnings = false;
    int frameStart = 1;
    int frameEnd = 1;
    int frameStep = 1;
    std::wstring message;
    std::string jobId;
};

constexpr COLORREF kAccent = RGB(77, 69, 233);
constexpr COLORREF kChromeAccent = RGB(53, 226, 122);
constexpr int kChromeHeightDip = 36;
constexpr int kChromeButtonWidthDip = 46;
constexpr DWORD kDwmUseImmersiveDarkMode = 20;
constexpr DWORD kDwmWindowCornerPreference = 33;
constexpr DWORD kDwmBorderColor = 34;
constexpr UINT_PTR kButtonSubclassId = 1;
constexpr UINT_PTR kListSubclassId = 2;
constexpr UINT_PTR kUiResizeTestSubclassId = 3;

bool gUiResizeTestTracking = false;
std::unordered_map<HWND, int> gUiResizeLayoutCounts;
std::unordered_map<HWND, int> gUiResizePaintCounts;

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

int DpiValue(HWND window, int dip) {
    return MulDiv(dip, static_cast<int>(GetDpiForWindow(window)), 96);
}

int ChromeHeight(HWND window) { return DpiValue(window, kChromeHeightDip); }

std::array<RECT, 3> ChromeButtonRects(HWND window) {
    RECT client{};
    GetClientRect(window, &client);
    const int width = DpiValue(window, kChromeButtonWidthDip);
    const int height = ChromeHeight(window);
    return {
        RECT{client.right - width * 3, 0, client.right - width * 2, height},
        RECT{client.right - width * 2, 0, client.right - width, height},
        RECT{client.right - width, 0, client.right, height},
    };
}

void InvalidateChromePart(HWND window, int part) {
    const auto rects = ChromeButtonRects(window);
    const RECT* rect = part == HTMINBUTTON ? &rects[0]
        : part == HTMAXBUTTON ? &rects[1] : part == HTCLOSE ? &rects[2] : nullptr;
    if (rect) InvalidateRect(window, rect, FALSE);
}

void ApplyWindowChromeTheme(HWND window) {
    HMODULE dwm = LoadLibraryW(L"dwmapi.dll");
    if (!dwm) return;
    using SetAttribute = HRESULT(WINAPI*)(HWND, DWORD, LPCVOID, DWORD);
    auto setAttribute = reinterpret_cast<SetAttribute>(
        GetProcAddress(dwm, "DwmSetWindowAttribute"));
    if (setAttribute) {
        BOOL dark = gDarkTheme;
        setAttribute(window, kDwmUseImmersiveDarkMode, &dark, sizeof(dark));
        const DWORD roundCorners = 2;
        setAttribute(window, kDwmWindowCornerPreference, &roundCorners, sizeof(roundCorners));
        const DWORD noBorder = 0xFFFFFFFEu;
        setAttribute(window, kDwmBorderColor, &noBorder, sizeof(noBorder));
    }
    FreeLibrary(dwm);
}

int ChromeHitTest(HWND window, POINT point) {
    RECT client{};
    GetClientRect(window, &client);
    const int frameX = GetSystemMetricsForDpi(SM_CXFRAME, GetDpiForWindow(window)) +
        GetSystemMetricsForDpi(SM_CXPADDEDBORDER, GetDpiForWindow(window));
    const int frameY = GetSystemMetricsForDpi(SM_CYFRAME, GetDpiForWindow(window)) +
        GetSystemMetricsForDpi(SM_CXPADDEDBORDER, GetDpiForWindow(window));
    if (!IsZoomed(window)) {
        const bool left = point.x < frameX;
        const bool right = point.x >= client.right - frameX;
        const bool top = point.y < frameY;
        const bool bottom = point.y >= client.bottom - frameY;
        if (top && left) return HTTOPLEFT;
        if (top && right) return HTTOPRIGHT;
        if (bottom && left) return HTBOTTOMLEFT;
        if (bottom && right) return HTBOTTOMRIGHT;
        if (left) return HTLEFT;
        if (right) return HTRIGHT;
        if (top) return HTTOP;
        if (bottom) return HTBOTTOM;
    }
    const auto rects = ChromeButtonRects(window);
    if (PtInRect(&rects[2], point)) return HTCLOSE;
    if (PtInRect(&rects[1], point)) return HTMAXBUTTON;
    if (PtInRect(&rects[0], point)) return HTMINBUTTON;
    if (point.x < DpiValue(window, 42) && point.y < ChromeHeight(window)) return HTSYSMENU;
    if (point.y < ChromeHeight(window)) return HTCAPTION;
    return HTCLIENT;
}

void UpdateChromeHot(HWND window, int next) {
    auto& state = gChromeStates[window];
    if (next != HTMINBUTTON && next != HTMAXBUTTON && next != HTCLOSE)
        next = HTNOWHERE;
    if (state.hot == next) return;
    InvalidateChromePart(window, state.hot);
    state.hot = next;
    InvalidateChromePart(window, state.hot);
}

bool HandleWindowChrome(HWND window, UINT message, WPARAM wParam, LPARAM lParam,
                        LRESULT& result) {
    auto& state = gChromeStates[window];
    switch (message) {
        case WM_NCCALCSIZE:
            result = 0;
            return true;
        case WM_NCHITTEST: {
            POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            ScreenToClient(window, &point);
            result = ChromeHitTest(window, point);
            return true;
        }
        case WM_NCMOUSEMOVE: {
            UpdateChromeHot(window, static_cast<int>(wParam));
            TRACKMOUSEEVENT tracking{sizeof(tracking), TME_NONCLIENT | TME_LEAVE, window, 0};
            TrackMouseEvent(&tracking);
            return false;
        }
        case WM_NCMOUSELEAVE:
            UpdateChromeHot(window, HTNOWHERE);
            result = 0;
            return true;
        case WM_NCLBUTTONDOWN:
            if (wParam == HTMINBUTTON || wParam == HTMAXBUTTON || wParam == HTCLOSE) {
                state.pressed = static_cast<int>(wParam);
                SetCapture(window);
                InvalidateChromePart(window, state.pressed);
                result = 0;
                return true;
            }
            return false;
        case WM_MOUSEMOVE:
            if (state.pressed != HTNOWHERE) {
                POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
                UpdateChromeHot(window, ChromeHitTest(window, point));
                result = 0;
                return true;
            }
            return false;
        case WM_LBUTTONUP:
            if (state.pressed != HTNOWHERE) {
                POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
                const int releasedOver = ChromeHitTest(window, point);
                const int pressed = state.pressed;
                state.pressed = HTNOWHERE;
                ReleaseCapture();
                InvalidateChromePart(window, pressed);
                if (releasedOver == pressed) {
                    if (pressed == HTMINBUTTON)
                        SendMessageW(window, WM_SYSCOMMAND, SC_MINIMIZE, 0);
                    else if (pressed == HTMAXBUTTON)
                        SendMessageW(window, WM_SYSCOMMAND,
                                     IsZoomed(window) ? SC_RESTORE : SC_MAXIMIZE, 0);
                    else if (pressed == HTCLOSE)
                        SendMessageW(window, WM_SYSCOMMAND, SC_CLOSE, 0);
                }
                result = 0;
                return true;
            }
            return false;
        case WM_CANCELMODE:
        case WM_CAPTURECHANGED:
            if (state.pressed != HTNOWHERE) {
                const int pressed = state.pressed;
                state.pressed = HTNOWHERE;
                InvalidateChromePart(window, pressed);
            }
            if (message == WM_CANCELMODE && GetCapture() == window) ReleaseCapture();
            return false;
        case WM_NCACTIVATE:
        case WM_ACTIVATE:
            state.active = message == WM_NCACTIVATE ? wParam != FALSE
                : LOWORD(wParam) != WA_INACTIVE;
            InvalidateRect(window, nullptr, FALSE);
            if (message == WM_NCACTIVATE) {
                result = TRUE;
                return true;
            }
            return false;
        case WM_DPICHANGED: {
            const RECT* suggested = reinterpret_cast<const RECT*>(lParam);
            SetWindowPos(window, nullptr, suggested->left, suggested->top,
                         suggested->right - suggested->left, suggested->bottom - suggested->top,
                         SWP_NOACTIVATE | SWP_NOZORDER);
            InvalidateRect(window, nullptr, TRUE);
            result = 0;
            return true;
        }
        case WM_THEMECHANGED:
            ApplyWindowChromeTheme(window);
            InvalidateRect(window, nullptr, TRUE);
            return false;
        case WM_EXITSIZEMOVE:
            RedrawWindow(window, nullptr, nullptr,
                         RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_UPDATENOW | RDW_NOERASE);
            return false;
        case WM_DESTROY:
            gChromeStates.erase(window);
            return false;
    }
    return false;
}

void ApplyMonitorWorkArea(HWND window, MINMAXINFO* limits) {
    MONITORINFO monitor{};
    monitor.cbSize = sizeof(monitor);
    HMONITOR handle = MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST);
    if (!GetMonitorInfoW(handle, &monitor)) return;
    limits->ptMaxPosition = {monitor.rcWork.left - monitor.rcMonitor.left,
                             monitor.rcWork.top - monitor.rcMonitor.top};
    limits->ptMaxSize = {monitor.rcWork.right - monitor.rcWork.left,
                         monitor.rcWork.bottom - monitor.rcWork.top};
}

void ReleaseRenderTarget() {
    SafeRelease(gRenderTarget);
}

void ReleaseGraphics() {
    ReleaseRenderTarget();
    SafeRelease(gCardValueFormat);
    SafeRelease(gCardLabelFormat);
    SafeRelease(gChromeTitleFormat);
    gChromeTitleFormatDpi = 0;
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
    const UINT chromeDpi = GetDpiForWindow(window);
    if (gChromeTitleFormat && gChromeTitleFormatDpi != chromeDpi)
        SafeRelease(gChromeTitleFormat);
    if (!gChromeTitleFormat) {
        const float chromeFontSize = 16.0f * static_cast<float>(chromeDpi) / 96.0f;
        HRESULT hr = gDwriteFactory->CreateTextFormat(
            L"Segoe UI Variable Text", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD,
            DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, chromeFontSize, L"zh-CN",
            &gChromeTitleFormat);
        if (FAILED(hr)) hr = gDwriteFactory->CreateTextFormat(
            L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL, chromeFontSize, L"zh-CN", &gChromeTitleFormat);
        if (FAILED(hr)) return false;
        gChromeTitleFormatDpi = chromeDpi;
        gChromeTitleFormat->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        gChromeTitleFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        gChromeTitleFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
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
                 const D2D1_RECT_F& rect, ID2D1Brush* brush);

void DrawMainChrome(HWND window, ID2D1SolidColorBrush* textBrush,
                    ID2D1SolidColorBrush* strokeBrush) {
    const auto rects = ChromeButtonRects(window);
    const auto state = gChromeStates[window];
    const float height = static_cast<float>(ChromeHeight(window));
    ID2D1SolidColorBrush* titleBrush = nullptr;
    ID2D1SolidColorBrush* hoverBrush = nullptr;
    ID2D1SolidColorBrush* pressedBrush = nullptr;
    ID2D1SolidColorBrush* closeBrush = nullptr;
    gRenderTarget->CreateSolidColorBrush(
        D2dColor(gDarkTheme ? RGB(24, 25, 31) : RGB(252, 252, 254)), &titleBrush);
    gRenderTarget->CreateSolidColorBrush(
        D2dColor(gDarkTheme ? RGB(48, 49, 60) : RGB(232, 234, 240)), &hoverBrush);
    gRenderTarget->CreateSolidColorBrush(
        D2dColor(gDarkTheme ? RGB(62, 63, 76) : RGB(217, 220, 228)), &pressedBrush);
    gRenderTarget->CreateSolidColorBrush(D2dColor(RGB(196, 43, 50)), &closeBrush);
    RECT client{};
    GetClientRect(window, &client);
    gRenderTarget->FillRectangle(D2D1::RectF(0, 0, static_cast<float>(client.right), height),
                                 titleBrush);
    gRenderTarget->DrawLine(D2D1::Point2F(0, height - 1),
                            D2D1::Point2F(static_cast<float>(client.right), height - 1),
                            strokeBrush, 1.0f);
    const float iconSize = static_cast<float>(DpiValue(window, 20));
    const float iconLeft = static_cast<float>(DpiValue(window, 12));
    const float iconTop = (height - iconSize) / 2.0f;
    ID2D1SolidColorBrush* iconSurface = nullptr;
    ID2D1SolidColorBrush* iconAccent = nullptr;
    gRenderTarget->CreateSolidColorBrush(D2dColor(RGB(5, 42, 25)), &iconSurface);
    gRenderTarget->CreateSolidColorBrush(D2dColor(kChromeAccent), &iconAccent);
    const auto iconRect = D2D1::RoundedRect(
        D2D1::RectF(iconLeft, iconTop, iconLeft + iconSize, iconTop + iconSize),
        DpiValue(window, 4), DpiValue(window, 4));
    gRenderTarget->FillRoundedRectangle(iconRect, iconSurface);
    gRenderTarget->DrawRoundedRectangle(iconRect, iconAccent, 1.0f);
    const std::array<D2D1_POINT_2F, 5> nodes = {
        D2D1::Point2F(iconLeft + iconSize * .22f, iconTop + iconSize * .25f),
        D2D1::Point2F(iconLeft + iconSize * .78f, iconTop + iconSize * .22f),
        D2D1::Point2F(iconLeft + iconSize * .50f, iconTop + iconSize * .50f),
        D2D1::Point2F(iconLeft + iconSize * .22f, iconTop + iconSize * .76f),
        D2D1::Point2F(iconLeft + iconSize * .78f, iconTop + iconSize * .78f),
    };
    for (size_t index : {size_t{0}, size_t{1}, size_t{3}, size_t{4}})
        gRenderTarget->DrawLine(nodes[index], nodes[2], iconAccent, 1.0f);
    for (const auto& node : nodes)
        gRenderTarget->FillEllipse(D2D1::Ellipse(node, 1.4f, 1.4f), iconAccent);
    wchar_t title[256]{};
    GetWindowTextW(window, title, static_cast<int>(std::size(title)));
    DrawD2dText(title, gChromeTitleFormat,
                D2D1::RectF(static_cast<float>(DpiValue(window, 42)),
                            0.0f,
                            static_cast<float>(rects[0].left - DpiValue(window, 8)), height),
                textBrush);

    for (size_t index = 0; index < rects.size(); ++index) {
        const int part = index == 0 ? HTMINBUTTON : index == 1 ? HTMAXBUTTON : HTCLOSE;
        ID2D1SolidColorBrush* fill = nullptr;
        if (state.pressed == part && state.hot == part)
            fill = part == HTCLOSE ? closeBrush : pressedBrush;
        else if (state.hot == part) fill = part == HTCLOSE ? closeBrush : hoverBrush;
        if (fill)
            gRenderTarget->FillRectangle(D2D1::RectF(
                static_cast<float>(rects[index].left), 0,
                static_cast<float>(rects[index].right), height), fill);
        const float cx = (rects[index].left + rects[index].right) / 2.0f;
        const float cy = height / 2.0f;
        const float radius = static_cast<float>(DpiValue(window, 5));
        if (part == HTMINBUTTON) {
            gRenderTarget->DrawLine(D2D1::Point2F(cx - radius, cy + 2),
                                    D2D1::Point2F(cx + radius, cy + 2), textBrush, 1.2f);
        } else if (part == HTCLOSE) {
            gRenderTarget->DrawLine(D2D1::Point2F(cx - radius, cy - radius),
                                    D2D1::Point2F(cx + radius, cy + radius), textBrush, 1.2f);
            gRenderTarget->DrawLine(D2D1::Point2F(cx + radius, cy - radius),
                                    D2D1::Point2F(cx - radius, cy + radius), textBrush, 1.2f);
        } else if (IsZoomed(window)) {
            gRenderTarget->DrawRectangle(D2D1::RectF(cx - radius + 2, cy - radius - 2,
                                                     cx + radius + 2, cy + radius - 2),
                                         textBrush, 1.0f);
            gRenderTarget->DrawRectangle(D2D1::RectF(cx - radius - 2, cy - radius + 2,
                                                     cx + radius - 2, cy + radius + 2),
                                         textBrush, 1.0f);
        } else {
            gRenderTarget->DrawRectangle(D2D1::RectF(cx - radius, cy - radius,
                                                     cx + radius, cy + radius),
                                         textBrush, 1.0f);
        }
    }
    SafeRelease(closeBrush);
    SafeRelease(pressedBrush);
    SafeRelease(hoverBrush);
    SafeRelease(titleBrush);
    SafeRelease(iconSurface);
    SafeRelease(iconAccent);
}

void DrawChromeGdi(HWND window, HDC dc) {
    RECT client{};
    GetClientRect(window, &client);
    const int height = ChromeHeight(window);
    const auto rects = ChromeButtonRects(window);
    const auto state = gChromeStates[window];
    const COLORREF titleColor = gDarkTheme ? RGB(24, 25, 31) : RGB(252, 252, 254);
    const COLORREF textColor = gDarkTheme ? RGB(244, 244, 249) : RGB(28, 29, 36);
    HBRUSH titleBrush = CreateSolidBrush(titleColor);
    RECT titleRect{0, 0, client.right, height};
    FillRect(dc, &titleRect, titleBrush);
    DeleteObject(titleBrush);
    HPEN separator = CreatePen(PS_SOLID, 1, gDarkTheme ? RGB(51, 52, 66) : RGB(224, 226, 234));
    HGDIOBJ oldPen = SelectObject(dc, separator);
    MoveToEx(dc, 0, height - 1, nullptr);
    LineTo(dc, client.right, height - 1);
    SelectObject(dc, oldPen);
    DeleteObject(separator);
    const int iconSize = DpiValue(window, 20);
    const int iconLeft = DpiValue(window, 12);
    const int iconTop = (height - iconSize) / 2;
    HBRUSH iconSurface = CreateSolidBrush(RGB(5, 42, 25));
    HPEN iconPen = CreatePen(PS_SOLID, 1, kChromeAccent);
    HGDIOBJ oldIconBrush = SelectObject(dc, iconSurface);
    HGDIOBJ oldIconPen = SelectObject(dc, iconPen);
    RoundRect(dc, iconLeft, iconTop, iconLeft + iconSize, iconTop + iconSize,
              DpiValue(window, 7), DpiValue(window, 7));
    const std::array<POINT, 5> nodes = {
        POINT{iconLeft + iconSize * 22 / 100, iconTop + iconSize * 25 / 100},
        POINT{iconLeft + iconSize * 78 / 100, iconTop + iconSize * 22 / 100},
        POINT{iconLeft + iconSize / 2, iconTop + iconSize / 2},
        POINT{iconLeft + iconSize * 22 / 100, iconTop + iconSize * 76 / 100},
        POINT{iconLeft + iconSize * 78 / 100, iconTop + iconSize * 78 / 100},
    };
    for (size_t index : {size_t{0}, size_t{1}, size_t{3}, size_t{4}}) {
        MoveToEx(dc, nodes[index].x, nodes[index].y, nullptr);
        LineTo(dc, nodes[2].x, nodes[2].y);
    }
    SelectObject(dc, iconPen);
    for (const POINT& node : nodes)
        Ellipse(dc, node.x - 1, node.y - 1, node.x + 2, node.y + 2);
    SelectObject(dc, oldIconPen);
    SelectObject(dc, oldIconBrush);
    DeleteObject(iconPen);
    DeleteObject(iconSurface);
    wchar_t title[256]{};
    GetWindowTextW(window, title, static_cast<int>(std::size(title)));
    RECT textRect{DpiValue(window, 42), 0, rects[0].left - DpiValue(window, 8), height};
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, state.active ? textColor
                                  : (gDarkTheme ? RGB(160, 162, 174) : RGB(112, 114, 124)));
    HFONT chromeFont = CreateFontW(-DpiValue(window, 16), 0, 0, 0, FW_SEMIBOLD,
                                   FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                   OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                   CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
                                   L"Segoe UI Variable Text");
    HGDIOBJ oldFont = SelectObject(dc, chromeFont ? chromeFont : gFont);
    HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(NULL_BRUSH));
    DrawTextW(dc, title, -1, &textRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    for (size_t index = 0; index < rects.size(); ++index) {
        const int part = index == 0 ? HTMINBUTTON : index == 1 ? HTMAXBUTTON : HTCLOSE;
        if (state.hot == part || state.pressed == part) {
            const COLORREF fill = part == HTCLOSE ? RGB(196, 43, 50)
                : state.pressed == part && state.hot == part
                    ? (gDarkTheme ? RGB(62, 63, 76) : RGB(217, 220, 228))
                : (gDarkTheme ? RGB(48, 49, 60) : RGB(232, 234, 240));
            HBRUSH brush = CreateSolidBrush(fill);
            FillRect(dc, &rects[index], brush);
            DeleteObject(brush);
        }
        const int cx = (rects[index].left + rects[index].right) / 2;
        const int cy = height / 2;
        const int r = DpiValue(window, 5);
        HPEN glyph = CreatePen(PS_SOLID, 1, textColor);
        oldPen = SelectObject(dc, glyph);
        if (part == HTMINBUTTON) {
            MoveToEx(dc, cx - r, cy + 2, nullptr); LineTo(dc, cx + r, cy + 2);
        } else if (part == HTCLOSE) {
            MoveToEx(dc, cx - r, cy - r, nullptr); LineTo(dc, cx + r, cy + r);
            MoveToEx(dc, cx + r, cy - r, nullptr); LineTo(dc, cx - r, cy + r);
        } else if (IsZoomed(window)) {
            Rectangle(dc, cx - r + 2, cy - r - 2, cx + r + 2, cy + r - 2);
            Rectangle(dc, cx - r - 2, cy - r + 2, cx + r - 2, cy + r + 2);
        } else {
            Rectangle(dc, cx - r, cy - r, cx + r, cy + r);
        }
        SelectObject(dc, oldPen);
        DeleteObject(glyph);
    }
    SelectObject(dc, oldBrush);
    SelectObject(dc, oldFont);
    if (chromeFont) DeleteObject(chromeFont);
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
    if (FAILED(gRenderTarget->Resize(D2D1::SizeU(
            static_cast<UINT>(std::max(1L, client.right)),
            static_cast<UINT>(std::max(1L, client.bottom)))))) {
        ReleaseRenderTarget();
        EndPaint(window, &paint);
        InvalidateRect(window, nullptr, FALSE);
        return;
    }
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
    const float chromeTop = static_cast<float>(ChromeHeight(window));
    gRenderTarget->FillRectangle(D2D1::RectF(0, chromeTop, 220.0f,
                                             static_cast<float>(client.bottom)), sidebarBrush);
    gRenderTarget->DrawLine(D2D1::Point2F(220.0f, chromeTop),
                            D2D1::Point2F(220.0f, static_cast<float>(client.bottom)),
                            strokeBrush, 1.0f);

    const float contentLeft = 236.0f;
    const float contentRight = static_cast<float>(client.right) - 18.0f;
    if (gCurrentPage == 0) {
        constexpr float gap = 12.0f;
        const float cardWidth = std::max(160.0f,
            (contentRight - contentLeft - gap * 3.0f) / 4.0f);
        const std::array<std::wstring, 4> labels = {
            L"在线节点", L"活动任务", L"完成帧", L"匹配 ID · 可编辑",
        };
        const std::array<std::wstring, 4> values = {
            std::to_wstring(gDashboardView.onlineNodes),
            std::to_wstring(gDashboardView.activeJobs),
            std::to_wstring(gDashboardView.completedFrames) + L" / " +
                std::to_wstring(gDashboardView.totalFrames),
            gDashboardView.matchId.empty() ? L"未生成" : gDashboardView.matchId,
        };
        for (size_t index = 0; index < labels.size(); ++index) {
            const float left = contentLeft + static_cast<float>(index) * (cardWidth + gap);
            const D2D1_ROUNDED_RECT card = D2D1::RoundedRect(
                D2D1::RectF(left, chromeTop + 86.0f, left + cardWidth,
                            chromeTop + 176.0f), 14.0f, 14.0f);
            gRenderTarget->FillRoundedRectangle(card, surfaceBrush);
            gRenderTarget->DrawRoundedRectangle(card, strokeBrush, 1.0f);
            DrawD2dText(labels[index], gCardLabelFormat,
                        D2D1::RectF(left + 16.0f, chromeTop + 102.0f,
                                   left + cardWidth - 12.0f, chromeTop + 124.0f),
                        secondaryBrush);
            if (index != 3)
                DrawD2dText(values[index], gCardValueFormat,
                            D2D1::RectF(left + 16.0f, chromeTop + 128.0f,
                                       left + cardWidth - 12.0f, chromeTop + 163.0f),
                            textBrush);
        }
    }

    auto controlRect = [&](int id, float horizontalPadding, float verticalPadding)
        -> std::optional<D2D1_RECT_F> {
        HWND control = Item(id);
        if (!control || !IsWindowVisible(control)) return std::nullopt;
        RECT rect{};
        GetWindowRect(control, &rect);
        MapWindowPoints(HWND_DESKTOP, window, reinterpret_cast<POINT*>(&rect), 2);
        return D2D1::RectF(static_cast<float>(rect.left) - horizontalPadding,
                           static_cast<float>(rect.top) - verticalPadding,
                           static_cast<float>(rect.right) + horizontalPadding,
                           static_cast<float>(rect.bottom) + verticalPadding);
    };
    const int activeListId = gCurrentPage == 0 ? ID_NODES
        : gCurrentPage == 1 ? ID_JOBS
        : gCurrentPage == 2 ? ID_CONTRIBUTION_LIST : 0;
    if (activeListId) {
        if (auto rect = controlRect(activeListId, 8.0f, 8.0f)) {
            const auto card = D2D1::RoundedRect(*rect, 12.0f, 12.0f);
            gRenderTarget->FillRoundedRectangle(card, surfaceBrush);
            gRenderTarget->DrawRoundedRectangle(card, strokeBrush, 1.0f);
        }
    }
    if (auto rect = controlRect(ID_PREFLIGHT, 8.0f, 4.0f)) {
        const auto status = D2D1::RoundedRect(*rect, 9.0f, 9.0f);
        gRenderTarget->FillRoundedRectangle(status, surfaceBrush);
        gRenderTarget->DrawRoundedRectangle(status, strokeBrush, 1.0f);
    }

    const std::array<int, 5> fieldIds = {
        ID_MANUAL_ENDPOINT, ID_NODE_NAME, ID_IDLE_SECONDS,
        ID_BLENDER_PATH, ID_CLUSTER_TOKEN,
    };
    for (int id : fieldIds) {
        HWND field = Item(id);
        auto rect = controlRect(id, 5.0f, 4.0f);
        if (!field || !rect) continue;
        const auto frame = D2D1::RoundedRect(*rect, 8.0f, 8.0f);
        gRenderTarget->FillRoundedRectangle(frame, surfaceBrush);
        ID2D1SolidColorBrush* border = GetFocus() == field ? accentBrush : strokeBrush;
        const float borderWidth = GetFocus() == field ? 2.0f
            : GetPropW(field, kFieldHoverProperty) ? 1.5f : 1.0f;
        gRenderTarget->DrawRoundedRectangle(frame, border, borderWidth);
    }

    DrawMainChrome(window, textBrush, strokeBrush);

    HRESULT result = gRenderTarget->EndDraw();
    SafeRelease(accentBrush);
    SafeRelease(secondaryBrush);
    SafeRelease(textBrush);
    SafeRelease(strokeBrush);
    SafeRelease(surfaceBrush);
    SafeRelease(sidebarBrush);
    SafeRelease(backgroundBrush);
    if (result == D2DERR_RECREATE_TARGET) {
        ReleaseRenderTarget();
        InvalidateRect(window, nullptr, FALSE);
    }
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

std::wstring WorkerActivityText(const WorkerActivity& activity, int assigned) {
    if (!activity.Active()) return assigned > 0 ? L"排队中" : L"—";
    ui::WorkerActivityView view;
    view.jobName = Utf8ToWide(activity.jobName);
    view.frames = activity.frames;
    view.completedFrames = activity.completedFrames;
    view.rendering = activity.rendering;
    return ui::FormatWorkerActivity(view);
}

std::optional<RenderJob> FindJob(const std::string& id) {
    if (!gApp) return std::nullopt;
    return gApp->FindJob(id);
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

void ApplyListTheme(HWND list) {
    if (!list) return;
    const COLORREF surface = gDarkTheme ? RGB(31, 32, 42) : RGB(255, 255, 255);
    const COLORREF text = gDarkTheme ? RGB(239, 239, 247) : RGB(31, 31, 38);
    SendMessageW(list, WM_SETREDRAW, FALSE, 0);
    SetWindowTheme(list, gDarkTheme ? L"DarkMode_Explorer" : L"Explorer", nullptr);
    if (HWND header = ListView_GetHeader(list))
        SetWindowTheme(header, gDarkTheme ? L"" : L"Explorer", nullptr);
    ListView_SetBkColor(list, surface);
    ListView_SetTextBkColor(list, surface);
    ListView_SetTextColor(list, text);
    SendMessageW(list, WM_SETREDRAW, TRUE, 0);
    RedrawWindow(list, nullptr, nullptr,
                 RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
}

void ApplyTheme() {
    if (!gApp || !gWindow) return;
    gDarkTheme = ResolveDarkTheme(gApp->GetConfig());
    if (gBackgroundBrush) DeleteObject(gBackgroundBrush);
    if (gSidebarBrush) DeleteObject(gSidebarBrush);
    if (gSurfaceBrush) DeleteObject(gSurfaceBrush);
    COLORREF background = gDarkTheme ? RGB(15, 16, 22) : RGB(244, 245, 249);
    COLORREF sidebar = gDarkTheme ? RGB(23, 24, 32) : RGB(252, 252, 254);
    COLORREF surface = gDarkTheme ? RGB(31, 32, 42) : RGB(255, 255, 255);
    gBackgroundBrush = CreateSolidBrush(background);
    gSidebarBrush = CreateSolidBrush(sidebar);
    gSurfaceBrush = CreateSolidBrush(surface);
    for (int id : {ID_NODES, ID_JOBS, ID_CONTRIBUTION_LIST, ID_DETAIL_FRAMES}) {
        HWND list = id == ID_DETAIL_FRAMES && gDetailWindow
            ? GetDlgItem(gDetailWindow, id) : GetDlgItem(gWindow, id);
        if (!list) continue;
        // ApplyListTheme sets the native theme before RenderMesh colours so the
        // area beyond the last column cannot fall back to system white.
        ApplyListTheme(list);
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
            if (gFrameLogWindow) setAttribute(gFrameLogWindow, 20, &dark, sizeof(dark));
        }
        FreeLibrary(dwm);
    }
    auto redrawTheme = [](HWND window) {
        if (!window) return;
        RedrawWindow(window, nullptr, nullptr,
                     RDW_INVALIDATE | RDW_NOERASE | RDW_ALLCHILDREN | RDW_UPDATENOW);
    };
    redrawTheme(gWindow);
    redrawTheme(gDetailWindow);
    redrawTheme(gFrameLogWindow);
    redrawTheme(gInternalDialog);
    if (gUploadWindow)
        ApplyWorkflowTheme(gUploadWindow, GetDlgItem(gUploadWindow, ID_UPLOAD_LIST));
    if (gConfigWindow) ApplyWorkflowTheme(gConfigWindow, nullptr);
    ApplyWindowChromeTheme(gWindow);
    if (gDetailWindow) ApplyWindowChromeTheme(gDetailWindow);
    if (gFrameLogWindow) ApplyWindowChromeTheme(gFrameLogWindow);
}

HWND Item(int id) { return GetDlgItem(gWindow, id); }

void PaintRememberCheckbox(HWND window) {
    PAINTSTRUCT paint{};
    HDC dc = BeginPaint(window, &paint);
    RECT rect{};
    GetClientRect(window, &rect);
    const COLORREF surface = gDarkTheme ? RGB(31, 32, 42) : RGB(255, 255, 255);
    const COLORREF text = gDarkTheme ? RGB(218, 220, 232) : RGB(52, 54, 64);
    const COLORREF border = gDarkTheme ? RGB(92, 94, 110) : RGB(166, 170, 184);
    HBRUSH background = CreateSolidBrush(surface);
    FillRect(dc, &rect, background);
    DeleteObject(background);

    RECT box{rect.left + 2, rect.top + (rect.bottom - rect.top - 20) / 2,
             rect.left + 22, rect.top + (rect.bottom - rect.top + 20) / 2};
    const bool checked = SendMessageW(window, BM_GETCHECK, 0, 0) == BST_CHECKED;
    HBRUSH boxBrush = CreateSolidBrush(checked ? kChromeAccent : surface);
    HPEN boxPen = CreatePen(PS_SOLID, 1, checked ? kChromeAccent : border);
    HGDIOBJ oldBrush = SelectObject(dc, boxBrush);
    HGDIOBJ oldPen = SelectObject(dc, boxPen);
    RoundRect(dc, box.left, box.top, box.right, box.bottom, 5, 5);
    SelectObject(dc, oldBrush);
    SelectObject(dc, oldPen);
    DeleteObject(boxBrush);
    DeleteObject(boxPen);
    if (checked) {
        HPEN checkPen = CreatePen(PS_SOLID, 2, RGB(255, 255, 255));
        oldPen = SelectObject(dc, checkPen);
        MoveToEx(dc, box.left + 5, box.top + 10, nullptr);
        LineTo(dc, box.left + 9, box.top + 14);
        LineTo(dc, box.right - 4, box.top + 6);
        SelectObject(dc, oldPen);
        DeleteObject(checkPen);
    }
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, text);
    HGDIOBJ oldFont = SelectObject(dc, gFont);
    RECT label{box.right + 10, rect.top, rect.right, rect.bottom};
    DrawTextW(dc, L"记住本次选择", -1, &label,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    if (GetFocus() == window) DrawFocusRect(dc, &label);
    SelectObject(dc, oldFont);
    EndPaint(window, &paint);
}

LRESULT CALLBACK ButtonSubclassProc(HWND window, UINT message, WPARAM wParam,
                                    LPARAM lParam, UINT_PTR, DWORD_PTR) {
    const int controlId = GetDlgCtrlID(window);
    const bool settingToggle =
        controlId == ID_AUTO_START || controlId == ID_SILENT_AUTO_START;
    if (settingToggle && message == BM_GETCHECK)
        return GetPropW(window, kSettingToggleCheckedProperty)
            ? BST_CHECKED : BST_UNCHECKED;
    if (settingToggle && message == BM_SETCHECK) {
        const bool checked = wParam == BST_CHECKED;
        const bool wasChecked =
            GetPropW(window, kSettingToggleCheckedProperty) != nullptr;
        if (checked)
            SetPropW(window, kSettingToggleCheckedProperty,
                     reinterpret_cast<HANDLE>(1));
        else
            RemovePropW(window, kSettingToggleCheckedProperty);
        if (checked != wasChecked) InvalidateRect(window, nullptr, FALSE);
        return 0;
    }
    if (message == WM_PAINT && controlId == ID_INTERNAL_REMEMBER) {
        PaintRememberCheckbox(window);
        return 0;
    }
    if (message == WM_KEYDOWN &&
        GetPropW(window, kInternalDialogButtonProperty) &&
        (wParam == VK_ESCAPE || wParam == VK_RETURN || wParam == VK_TAB)) {
        SendMessageW(GetParent(window), message, wParam, lParam);
        return 0;
    }
    if ((message == WM_SETFOCUS || message == WM_KILLFOCUS) &&
        controlId == ID_INTERNAL_REMEMBER) {
        InvalidateRect(window, nullptr, TRUE);
    }
    if (message == WM_MOUSEMOVE && !GetPropW(window, L"RM_HOVER")) {
        SetPropW(window, L"RM_HOVER", reinterpret_cast<HANDLE>(1));
        TRACKMOUSEEVENT tracking{sizeof(tracking), TME_LEAVE, window, 0};
        TrackMouseEvent(&tracking);
        InvalidateRect(window, nullptr, FALSE);
    } else if (message == WM_MOUSELEAVE) {
        RemovePropW(window, L"RM_HOVER");
        InvalidateRect(window, nullptr, FALSE);
    } else if (message == WM_NCDESTROY) {
        RemovePropW(window, L"RM_HOVER");
        RemovePropW(window, kThemeSelectionProperty);
        RemovePropW(window, kCloseBehaviorSelectionProperty);
        RemovePropW(window, kDropdownAffordanceProperty);
        RemovePropW(window, kIconActionProperty);
        RemovePropW(window, kInternalDialogButtonProperty);
        RemovePropW(window, kSettingToggleCheckedProperty);
        RemoveWindowSubclass(window, ButtonSubclassProc, kButtonSubclassId);
    }
    return DefSubclassProc(window, message, wParam, lParam);
}

void ConfigureOwnerButton(HWND button) {
    if (button)
        SetWindowSubclass(button, ButtonSubclassProc, kButtonSubclassId, 0);
}

HWND CreateTooltip(HWND parent, HWND control, const wchar_t* text) {
    if (!parent || !control || !text) return nullptr;
    HWND tooltip = CreateWindowExW(
        WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr,
        WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX,
        CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
        parent, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!tooltip) return nullptr;
    TOOLINFOW info{};
    info.cbSize = sizeof(info);
    info.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
    info.hwnd = parent;
    info.uId = reinterpret_cast<UINT_PTR>(control);
    info.lpszText = const_cast<wchar_t*>(text);
    SendMessageW(tooltip, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&info));
    SendMessageW(tooltip, TTM_SETMAXTIPWIDTH, 0, 360);
    SetWindowPos(tooltip, HWND_TOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    return tooltip;
}

bool SetControlVisibleIfChanged(HWND control, bool visible) {
    if (!control) return false;
    const bool current = IsWindowVisible(control) != FALSE;
    if (current == visible) return false;
    ShowWindow(control, visible ? SW_SHOWNA : SW_HIDE);
    return true;
}

void UpdateModernFieldRegion(HWND field) {
    RECT client{};
    GetClientRect(field, &client);
    if (client.right <= 0 || client.bottom <= 0) return;
    HRGN region = CreateRoundRectRgn(0, 0, client.right + 1, client.bottom + 1,
                                     12, 12);
    if (region && !SetWindowRgn(field, region, TRUE)) DeleteObject(region);
}

void DrawSelfFramedField(HWND field) {
    HDC dc = GetDC(field);
    if (!dc) return;
    RECT client{};
    GetClientRect(field, &client);
    client.right -= 1;
    client.bottom -= 1;
    const bool focused = GetFocus() == field;
    const bool hovered = GetPropW(field, kFieldHoverProperty) != nullptr;
    const COLORREF color = focused ? kAccent
        : hovered ? (gDarkTheme ? RGB(92, 94, 116) : RGB(164, 167, 181))
        : (gDarkTheme ? RGB(51, 52, 66) : RGB(210, 212, 222));
    HPEN pen = CreatePen(PS_SOLID, focused ? 2 : 1, color);
    HGDIOBJ oldPen = SelectObject(dc, pen);
    HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(HOLLOW_BRUSH));
    RoundRect(dc, client.left, client.top, client.right, client.bottom, 10, 10);
    SelectObject(dc, oldBrush);
    SelectObject(dc, oldPen);
    DeleteObject(pen);
    ReleaseDC(field, dc);
    ++gSelfFramedFieldPaintCount;
}

LRESULT CALLBACK FieldSubclassProc(HWND window, UINT message, WPARAM wParam,
                                   LPARAM lParam, UINT_PTR, DWORD_PTR) {
    auto invalidateFrame = [&] {
        if (GetPropW(window, kFieldSelfFrameProperty))
            InvalidateRect(window, nullptr, FALSE);
        else if (HWND parent = GetParent(window))
            InvalidateRect(parent, nullptr, FALSE);
    };
    if (message == WM_LBUTTONDOWN && GetDlgCtrlID(window) == ID_CLUSTER_TOKEN) {
        // A matching ID is an atomic six-digit value, not free-form text. Select
        // the complete old value on click so typing a new ID cannot silently
        // replace just one digit while the six-character limit blocks the rest.
        SetFocus(window);
        SendMessageW(window, EM_SETSEL, 0, -1);
        invalidateFrame();
        return 0;
    }
    if (message == WM_MOUSEMOVE && !GetPropW(window, kFieldHoverProperty)) {
        SetPropW(window, kFieldHoverProperty, reinterpret_cast<HANDLE>(1));
        TRACKMOUSEEVENT tracking{sizeof(tracking), TME_LEAVE, window, 0};
        TrackMouseEvent(&tracking);
        invalidateFrame();
    } else if (message == WM_MOUSELEAVE) {
        RemovePropW(window, kFieldHoverProperty);
        invalidateFrame();
    } else if (message == WM_SETFOCUS || message == WM_KILLFOCUS) {
        invalidateFrame();
    } else if (message == WM_SIZE) {
        UpdateModernFieldRegion(window);
    } else if (message == WM_NCDESTROY) {
        RemovePropW(window, kFieldHoverProperty);
        RemovePropW(window, kFieldSelfFrameProperty);
        RemoveWindowSubclass(window, FieldSubclassProc, kFieldSubclassId);
    }
    if (message == WM_PAINT && GetPropW(window, kFieldSelfFrameProperty)) {
        const LRESULT result = DefSubclassProc(window, message, wParam, lParam);
        DrawSelfFramedField(window);
        return result;
    }
    return DefSubclassProc(window, message, wParam, lParam);
}

void ConfigureModernField(HWND field, const wchar_t* type) {
    if (!field) return;
    const LONG_PTR style = GetWindowLongPtrW(field, GWL_STYLE);
    const LONG_PTR exStyle = GetWindowLongPtrW(field, GWL_EXSTYLE);
    SetWindowLongPtrW(field, GWL_STYLE,
                      style & ~static_cast<LONG_PTR>(WS_BORDER));
    SetWindowLongPtrW(field, GWL_EXSTYLE,
                      exStyle & ~static_cast<LONG_PTR>(WS_EX_CLIENTEDGE));
    SetWindowPos(field, nullptr, 0, 0, 0, 0,
                 SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE |
                     SWP_NOACTIVATE | SWP_NOZORDER);
    if (wcscmp(type, L"EDIT") == 0)
        SendMessageW(field, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN,
                     MAKELPARAM(8, 8));
    SetWindowSubclass(field, FieldSubclassProc, kFieldSubclassId, 0);
    UpdateModernFieldRegion(field);
}

LRESULT CALLBACK StaticLabelSubclassProc(HWND window, UINT message, WPARAM wParam,
                                         LPARAM lParam, UINT_PTR, DWORD_PTR) {
    if (message == WM_NCDESTROY) {
        RemovePropW(window, kStaticSurfaceRoleProperty);
        RemoveWindowSubclass(window, StaticLabelSubclassProc,
                             kStaticLabelSubclassId);
    }
    return DefSubclassProc(window, message, wParam, lParam);
}

LRESULT CALLBACK UiResizeTestSubclassProc(HWND window, UINT message, WPARAM wParam,
                                          LPARAM lParam, UINT_PTR, DWORD_PTR) {
    if (message == WM_PAINT && gUiResizeTestTracking) ++gUiResizePaintCounts[window];
    if (message == WM_NCDESTROY)
        RemoveWindowSubclass(window, UiResizeTestSubclassProc, kUiResizeTestSubclassId);
    return DefSubclassProc(window, message, wParam, lParam);
}

void MoveUiControl(HWND control, int x, int y, int width, int height, BOOL repaint) {
    if (!control) return;
    if (gUiResizeTestTracking) ++gUiResizeLayoutCounts[control];
    MoveWindow(control, x, y, width, height, repaint);
}

void ApplyChildLayout(HWND parent, const std::vector<std::pair<HWND, RECT>>& targets) {
    HDWP batch = BeginDeferWindowPos(static_cast<int>(targets.size()));
    bool deferred = batch != nullptr;
    if (batch) {
        for (const auto& [control, rect] : targets) {
            if (!control) continue;
            if (gUiResizeTestTracking) ++gUiResizeLayoutCounts[control];
            batch = DeferWindowPos(batch, control, nullptr, rect.left, rect.top,
                                   rect.right - rect.left, rect.bottom - rect.top,
                                   SWP_NOACTIVATE | SWP_NOZORDER | SWP_NOCOPYBITS);
            if (!batch) {
                deferred = false;
                break;
            }
        }
        if (deferred && !EndDeferWindowPos(batch)) deferred = false;
    }
    if (!deferred) {
        for (const auto& [control, rect] : targets)
            if (control) MoveUiControl(control, rect.left, rect.top,
                                       rect.right - rect.left, rect.bottom - rect.top, FALSE);
    }
    UINT redrawFlags = RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_NOERASE;
    if (gUiResizeTestTracking) redrawFlags |= RDW_UPDATENOW;
    RedrawWindow(parent, nullptr, nullptr, redrawFlags);
}

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

void SetStatusText(const std::wstring& value) {
    std::wstring displayed = value.empty() ? L"就绪" : value;
    if (!gAutoStartStartupWarning.empty())
        displayed += L" · 开机自启未生效：" + gAutoStartStartupWarning;
    SetText(ID_PREFLIGHT, displayed);
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
        WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | style,
        x, y, width, height, gWindow,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr), nullptr);
    SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font ? font : gFont), TRUE);
    if (wcscmp(type, L"BUTTON") == 0 && (style & BS_OWNERDRAW) == BS_OWNERDRAW)
        ConfigureOwnerButton(control);
    if (wcscmp(type, L"EDIT") == 0 || wcscmp(type, L"COMBOBOX") == 0)
        ConfigureModernField(control, type);
    return control;
}

HWND Label(const wchar_t* text, int x, int y, int width, int height, int id = 0,
           HFONT font = nullptr,
           StaticSurfaceRole role = StaticSurfaceRole::Background) {
    HWND label = AddControl(L"STATIC", text,
                            SS_LEFT | SS_NOPREFIX | SS_ENDELLIPSIS,
                            x, y, width, height, id, font);
    if (SetPropW(label, kStaticSurfaceRoleProperty,
                 reinterpret_cast<HANDLE>(static_cast<ULONG_PTR>(role)))) {
        if (!SetWindowSubclass(label, StaticLabelSubclassProc,
                               kStaticLabelSubclassId, 0))
            RemovePropW(label, kStaticSurfaceRoleProperty);
    }
    return label;
}

HWND PageControl(int page, HWND control) {
    if (page >= 0 && page < static_cast<int>(gPageControls.size()))
        gPageControls[static_cast<size_t>(page)].push_back(control);
    return control;
}

void ShowPage(int page) {
    gCurrentPage = std::clamp(page, 0, 3);
    if (gCurrentPage != 1) {
        gDropOverlayVisible = false;
        if (HWND overlay = Item(ID_DROP_OVERLAY)) ShowWindow(overlay, SW_HIDE);
    }
    for (size_t index = 0; index < gPageControls.size(); ++index)
        for (HWND control : gPageControls[index])
            ShowWindow(control, static_cast<int>(index) == gCurrentPage ? SW_SHOW : SW_HIDE);
    for (int id : {ID_NAV_DASHBOARD, ID_NAV_TASKS, ID_NAV_CONTRIBUTIONS, ID_NAV_SETTINGS})
        InvalidateRect(Item(id), nullptr, TRUE);
    if (gCurrentPage == 1)
        SetStatusText(L"拖入 .blend 文件开始新任务");
    else if (gCurrentPage == 2)
        SetStatusText(L"这里只记录本机替其他电脑渲染的任务包和帧，重启后仍保留。");
    else if (gCurrentPage == 3)
        SetStatusText(L"已就绪");
}

void NavigateSidebarPage(int page) {
    page = std::clamp(page, 0, 3);
    if (page == 3) {
        if (gCurrentPage == 3) {
            ShowPage(std::clamp(gPageBeforeSettings, 0, 2));
            return;
        }
        gPageBeforeSettings = std::clamp(gCurrentPage, 0, 2);
    }
    ShowPage(page);
}

void AddColumn(HWND list, int index, int width, const wchar_t* title) {
    LVCOLUMNW column{};
    column.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
    column.pszText = const_cast<wchar_t*>(title);
    column.cx = width;
    column.iSubItem = index;
    ListView_InsertColumn(list, index, &column);
}

void HideNativeListScrollbars(HWND list);

void FitListColumns(HWND list, std::initializer_list<int> weights,
                    std::initializer_list<int> minimumWidths) {
    if (!list || weights.size() == 0 || weights.size() != minimumWidths.size() ||
        Header_GetItemCount(ListView_GetHeader(list)) !=
            static_cast<int>(weights.size()))
        return;
    // Column changes and list mutations can make comctl32 recreate its native
    // non-client scrollbars. Remove them before calculating the usable width
    // and again after the last column has been committed.
    HideNativeListScrollbars(list);
    RECT client{};
    GetClientRect(list, &client);
    const int available = std::max(
        static_cast<int>(weights.size()),
        static_cast<int>(client.right - client.left));
    std::vector<int> layoutMinimums;
    std::vector<int> layoutWeights;
    layoutMinimums.reserve(minimumWidths.size());
    layoutWeights.reserve(weights.size());
    int minimumTotal = 0;
    int weightTotal = 0;
    for (const int value : minimumWidths) {
        const int width = value > 0 ? DpiValue(list, value) : 0;
        layoutMinimums.push_back(width);
        minimumTotal += width;
    }
    for (const int value : weights) {
        const int weight = std::max(0, value);
        layoutWeights.push_back(weight);
        weightTotal += weight;
    }
    std::vector<int> widths(layoutMinimums.size(), 0);
    if (minimumTotal > available) {
        for (size_t index = 0; index < widths.size(); ++index)
            widths[index] = minimumTotal > 0
                ? layoutMinimums[index] * available / minimumTotal : 0;
    } else {
        widths = layoutMinimums;
        const int extra = available - minimumTotal;
        for (size_t index = 0; index < widths.size(); ++index)
            if (weightTotal > 0)
                widths[index] += extra * layoutWeights[index] / weightTotal;
    }
    int used = 0;
    size_t lastVisible = 0;
    for (size_t index = 0; index < widths.size(); ++index) {
        used += widths[index];
        if (layoutMinimums[index] > 0 || layoutWeights[index] > 0)
            lastVisible = index;
    }
    widths[lastVisible] += available - used;
    for (size_t index = 0; index < widths.size(); ++index) {
        if (ListView_GetColumnWidth(list, static_cast<int>(index)) != widths[index])
            ListView_SetColumnWidth(list, static_cast<int>(index), widths[index]);
    }
    HideNativeListScrollbars(list);
}

#ifdef RM_DEV_TOOLS
bool ListColumnsFitClient(HWND list) {
    if (!list) return false;
    HWND header = ListView_GetHeader(list);
    const int columns = header ? Header_GetItemCount(header) : 0;
    RECT client{};
    GetClientRect(list, &client);
    int total = 0;
    for (int index = 0; index < columns; ++index)
        total += ListView_GetColumnWidth(list, index);
    return columns > 0 && total <= client.right - client.left;
}
#endif

void FitMainListColumns() {
    FitListColumns(Item(ID_NODES),
        {14, 9, 12, 20, 15, 16, 14},
        {110, 76, 100, 120, 110, 120, 100});
    FitListColumns(Item(ID_JOBS),
        {24, 11, 8, 12, 12, 17, 17},
        {145, 82, 58, 76, 86, 120, 120});
    FitListColumns(Item(ID_CONTRIBUTION_LIST),
        {16, 10, 12, 11, 11, 9, 7, 12, 12, 18},
        {86, 66, 80, 70, 70, 66, 56, 84, 84, 104});
}

RECT CenteredOwnedWindowRect(const RECT& ownerRect, const RECT& workArea,
                             int width, int height) {
    const int workWidth = std::max(
        1, static_cast<int>(workArea.right - workArea.left));
    const int workHeight = std::max(
        1, static_cast<int>(workArea.bottom - workArea.top));
    width = std::min(std::max(1, width), workWidth);
    height = std::min(std::max(1, height), workHeight);
    const int centeredX = ownerRect.left +
        ((ownerRect.right - ownerRect.left) - width) / 2;
    const int centeredY = ownerRect.top +
        ((ownerRect.bottom - ownerRect.top) - height) / 2;
    const int x = std::clamp(centeredX, static_cast<int>(workArea.left),
                             static_cast<int>(workArea.right) - width);
    const int y = std::clamp(centeredY, static_cast<int>(workArea.top),
                             static_cast<int>(workArea.bottom) - height);
    return {x, y, x + width, y + height};
}

void CenterOwnedWindow(HWND window, HWND owner) {
    if (!window) return;
    RECT current{};
    GetWindowRect(window, &current);
    RECT ownerRect = current;
    if (owner && IsWindow(owner)) GetWindowRect(owner, &ownerRect);
    MONITORINFO monitor{};
    monitor.cbSize = sizeof(monitor);
    const HMONITOR handle = MonitorFromRect(
        owner && IsWindow(owner) ? &ownerRect : &current,
        MONITOR_DEFAULTTONEAREST);
    if (!GetMonitorInfoW(handle, &monitor)) return;
    const RECT centered = CenteredOwnedWindowRect(
        ownerRect, monitor.rcWork, current.right - current.left,
        current.bottom - current.top);
    SetWindowPos(window, nullptr, centered.left, centered.top,
                 centered.right - centered.left, centered.bottom - centered.top,
                 SWP_NOACTIVATE | SWP_NOZORDER);
}

LRESULT HandleHeaderCustomDraw(NMCUSTOMDRAW* draw);
LRESULT HandleListCustomDraw(NMLVCUSTOMDRAW* draw);
void DrawOwnerButton(const DRAWITEMSTRUCT& item);

void HideNativeListScrollbars(HWND list) {
    if (!list) return;
    const LONG_PTR style = GetWindowLongPtrW(list, GWL_STYLE);
    const LONG_PTR nextStyle = style & ~(WS_VSCROLL | WS_HSCROLL);
    if (style != nextStyle) {
        SetWindowLongPtrW(list, GWL_STYLE, nextStyle);
        SetWindowPos(list, nullptr, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER |
                         SWP_NOACTIVATE | SWP_FRAMECHANGED);
    }
    ShowScrollBar(list, SB_BOTH, FALSE);
}

RECT CustomListScrollbarTrack(HWND list) {
    RECT client{};
    GetClientRect(list, &client);
    int headerHeight = 0;
    if (HWND header = ListView_GetHeader(list)) {
        RECT headerRect{};
        GetWindowRect(header, &headerRect);
        headerHeight = static_cast<int>(headerRect.bottom - headerRect.top);
    }
    const int rightMargin = DpiValue(list, 4);
    const int width = DpiValue(list, 7);
    const int verticalMargin = DpiValue(list, 5);
    return {
        std::max(0L, client.right - rightMargin - width),
        std::min(static_cast<int>(client.bottom), headerHeight + verticalMargin),
        std::max(0L, client.right - rightMargin),
        std::max(0L, client.bottom - verticalMargin),
    };
}

std::optional<RECT> CustomListScrollbarThumb(HWND list) {
    const int itemCount = ListView_GetItemCount(list);
    const int visibleCount = std::max(1, ListView_GetCountPerPage(list));
    if (itemCount <= visibleCount) return std::nullopt;
    const RECT track = CustomListScrollbarTrack(list);
    const int trackHeight = std::max(0L, track.bottom - track.top);
    if (trackHeight <= 0) return std::nullopt;
    const int minimumThumb = DpiValue(list, 30);
    const int thumbHeight = std::min(
        trackHeight,
        std::max(minimumThumb,
                 MulDiv(trackHeight, visibleCount, std::max(1, itemCount))));
    const int maxTopIndex = std::max(1, itemCount - visibleCount);
    const int topIndex = std::clamp(ListView_GetTopIndex(list), 0, maxTopIndex);
    const int travel = std::max(0, trackHeight - thumbHeight);
    const int thumbTop = track.top + MulDiv(travel, topIndex, maxTopIndex);
    return RECT{track.left, thumbTop, track.right, thumbTop + thumbHeight};
}

void InvalidateCustomListScrollbar(HWND list) {
    if (!list) return;
    RECT track = CustomListScrollbarTrack(list);
    InflateRect(&track, DpiValue(list, 2), DpiValue(list, 2));
    InvalidateRect(list, &track, FALSE);
}

void DrawCustomListScrollbar(HWND list) {
    const auto thumb = CustomListScrollbarThumb(list);
    if (!thumb) return;
    HDC dc = GetDC(list);
    if (!dc) return;
    const RECT track = CustomListScrollbarTrack(list);
    const bool hovered = gListScrollbarStates[list].hovered ||
        gListScrollbarStates[list].dragging;
    const COLORREF trackColor = gDarkTheme
        ? RGB(47, 48, 60) : RGB(226, 228, 235);
    const COLORREF thumbColor = hovered
        ? kAccent
        : gDarkTheme ? RGB(105, 103, 137) : RGB(125, 120, 186);
    HGDIOBJ oldPen = SelectObject(dc, GetStockObject(NULL_PEN));
    HBRUSH trackBrush = CreateSolidBrush(trackColor);
    HGDIOBJ oldBrush = SelectObject(dc, trackBrush);
    const int radius = DpiValue(list, 7);
    RoundRect(dc, track.left, track.top, track.right, track.bottom,
              radius, radius);
    SelectObject(dc, oldBrush);
    DeleteObject(trackBrush);
    HBRUSH thumbBrush = CreateSolidBrush(thumbColor);
    oldBrush = SelectObject(dc, thumbBrush);
    RoundRect(dc, thumb->left, thumb->top, thumb->right, thumb->bottom,
              radius, radius);
    SelectObject(dc, oldBrush);
    SelectObject(dc, oldPen);
    DeleteObject(thumbBrush);
    ReleaseDC(list, dc);
}

void ScrollListToTopIndex(HWND list, int targetTopIndex) {
    const int itemCount = ListView_GetItemCount(list);
    const int visibleCount = std::max(1, ListView_GetCountPerPage(list));
    const int maxTopIndex = std::max(0, itemCount - visibleCount);
    targetTopIndex = std::clamp(targetTopIndex, 0, maxTopIndex);
    const int currentTopIndex = std::max(0, ListView_GetTopIndex(list));
    if (targetTopIndex == currentTopIndex) return;
    RECT itemRect{};
    int rowHeight = DpiValue(list, 40);
    if (currentTopIndex < itemCount &&
        ListView_GetItemRect(list, currentTopIndex, &itemRect, LVIR_BOUNDS))
        rowHeight = std::max(1L, itemRect.bottom - itemRect.top);
    ListView_Scroll(list, 0, (targetTopIndex - currentTopIndex) * rowHeight);
    HideNativeListScrollbars(list);
    InvalidateCustomListScrollbar(list);
}

LRESULT CALLBACK ListViewSubclassProc(HWND window, UINT message, WPARAM wParam,
                                      LPARAM lParam, UINT_PTR, DWORD_PTR) {
    auto& scrollState = gListScrollbarStates[window];
    switch (message) {
        case WM_NOTIFY: {
            auto* header = reinterpret_cast<NMHDR*>(lParam);
            if (header && header->hwndFrom == ListView_GetHeader(window) &&
                header->code == NM_CUSTOMDRAW)
                return HandleHeaderCustomDraw(
                    reinterpret_cast<NMCUSTOMDRAW*>(lParam));
            break;
        }
        case WM_PAINT: {
            const LRESULT result = DefSubclassProc(window, message, wParam, lParam);
            DrawCustomListScrollbar(window);
            return result;
        }
        case WM_STYLECHANGING:
            if (wParam == static_cast<WPARAM>(GWL_STYLE)) {
                auto* styles = reinterpret_cast<STYLESTRUCT*>(lParam);
                if (styles) styles->styleNew &= ~(WS_VSCROLL | WS_HSCROLL);
            }
            break;
        case WM_MOUSEMOVE: {
            POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            const auto thumb = CustomListScrollbarThumb(window);
            if (scrollState.dragging && thumb) {
                const RECT track = CustomListScrollbarTrack(window);
                const int thumbHeight = thumb->bottom - thumb->top;
                const int travel = std::max(
                    1, static_cast<int>(track.bottom - track.top) - thumbHeight);
                const int thumbTop = std::clamp(
                    point.y - scrollState.dragOffset, track.top,
                    track.bottom - thumbHeight);
                const int itemCount = ListView_GetItemCount(window);
                const int visibleCount = std::max(1, ListView_GetCountPerPage(window));
                const int maxTopIndex = std::max(0, itemCount - visibleCount);
                ScrollListToTopIndex(
                    window, MulDiv(thumbTop - track.top, maxTopIndex, travel));
                return 0;
            }
            const bool hovered = thumb && PtInRect(&*thumb, point);
            if (hovered != scrollState.hovered) {
                scrollState.hovered = hovered;
                InvalidateCustomListScrollbar(window);
            }
            TRACKMOUSEEVENT tracking{sizeof(tracking), TME_LEAVE, window, 0};
            TrackMouseEvent(&tracking);
            break;
        }
        case WM_MOUSELEAVE:
            if (scrollState.hovered) {
                scrollState.hovered = false;
                InvalidateCustomListScrollbar(window);
            }
            break;
        case WM_LBUTTONDOWN: {
            POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            const auto thumb = CustomListScrollbarThumb(window);
            if (thumb && PtInRect(&*thumb, point)) {
                scrollState.dragging = true;
                scrollState.dragOffset = point.y - thumb->top;
                SetCapture(window);
                InvalidateCustomListScrollbar(window);
                return 0;
            }
            const RECT track = CustomListScrollbarTrack(window);
            if (thumb && PtInRect(&track, point)) {
                const int page = std::max(1, ListView_GetCountPerPage(window) - 1);
                const int direction = point.y < thumb->top ? -1 : 1;
                ScrollListToTopIndex(
                    window, ListView_GetTopIndex(window) + direction * page);
                return 0;
            }
            break;
        }
        case WM_LBUTTONUP:
            if (scrollState.dragging) {
                scrollState.dragging = false;
                if (GetCapture() == window) ReleaseCapture();
                InvalidateCustomListScrollbar(window);
                return 0;
            }
            break;
        case WM_CAPTURECHANGED:
            if (scrollState.dragging) {
                scrollState.dragging = false;
                InvalidateCustomListScrollbar(window);
            }
            break;
        case WM_MOUSEWHEEL: {
            scrollState.wheelDeltaRemainder += GET_WHEEL_DELTA_WPARAM(wParam);
            const int wheelSteps = scrollState.wheelDeltaRemainder / WHEEL_DELTA;
            scrollState.wheelDeltaRemainder %= WHEEL_DELTA;
            if (wheelSteps != 0) {
                UINT wheelLines = 3;
                SystemParametersInfoW(SPI_GETWHEELSCROLLLINES, 0, &wheelLines, 0);
                const int rowsPerStep = wheelLines == WHEEL_PAGESCROLL
                    ? std::max(1, ListView_GetCountPerPage(window) - 1)
                    : std::max(1, static_cast<int>(wheelLines));
                ScrollListToTopIndex(
                    window, ListView_GetTopIndex(window) - wheelSteps * rowsPerStep);
            }
            return 0;
        }
        case WM_VSCROLL:
        case WM_KEYDOWN: {
            const LRESULT result = DefSubclassProc(window, message, wParam, lParam);
            HideNativeListScrollbars(window);
            InvalidateCustomListScrollbar(window);
            return result;
        }
        case WM_SIZE: {
            const LRESULT result = DefSubclassProc(window, message, wParam, lParam);
            HideNativeListScrollbars(window);
            InvalidateCustomListScrollbar(window);
            return result;
        }
        case WM_NCDESTROY:
            gListScrollbarStates.erase(window);
            RemoveWindowSubclass(window, ListViewSubclassProc, kListSubclassId);
            break;
    }
    return DefSubclassProc(window, message, wParam, lParam);
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
    // List views are heavyweight sibling windows. Without WS_CLIPSIBLINGS a
    // delayed ListView repaint can draw across the full-window internal modal
    // layer even while that layer is first in the sibling Z order.
    const LONG_PTR style = GetWindowLongPtrW(list, GWL_STYLE);
    if ((style & WS_CLIPSIBLINGS) == 0) {
        SetWindowLongPtrW(list, GWL_STYLE, style | WS_CLIPSIBLINGS);
        SetWindowPos(list, nullptr, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER |
                     SWP_NOACTIVATE | SWP_FRAMECHANGED);
    }
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
    if (HWND header = ListView_GetHeader(list))
        SendMessageW(header, WM_SETFONT, reinterpret_cast<WPARAM>(gFont), TRUE);
    SetWindowSubclass(list, ListViewSubclassProc, kListSubclassId, 0);
    HideNativeListScrollbars(list);
}

void InsertRow(HWND list, int index, const ui::Row& row) {
    if (row.cells.empty()) return;
    LVITEMW item{};
    item.mask = LVIF_TEXT | LVIF_IMAGE;
    item.iItem = index;
    item.iImage = 0;
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
                     RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
        HideNativeListScrollbars(list);
        InvalidateCustomListScrollbar(list);
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
    HRESULT hr = CoCreateInstance(kClsidWicImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
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
            scaler, kWicPixelFormat32bppBGRA, WICBitmapDitherTypeNone,
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
    HBITMAP old = gPreviewBitmap;
    gPreviewBitmap = next;
    if (old && old != next) DeleteObject(old);
    InvalidateRect(preview, nullptr, TRUE);
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
void ShowDeleteConfirmation(const RenderJob& job);
void ShowInternalDialog(InternalDialogMode mode, std::wstring title,
                        std::wstring subject, std::wstring body);
void ShowInternalNotice(std::wstring title, std::wstring body);
void StartLanUpdateCheck(bool manual = false);
void MaybeShowLanUpdateOffer();
void StartLanUpdateDownload(const UpdateOffer& offer);
void MaybeShowBlenderInstallPrompt();
void StartBlenderCatalogFetch(bool configureOptix);
void StartBlenderInstall(const BlenderRelease& release, bool configureOptix);
void StopBlenderInstallThread();
bool LaunchUpdaterHelper(const UpdateDownloadResult& result, std::wstring& error);
void LayoutDetailWindow(HWND window);
LRESULT CALLBACK InternalDialogProc(HWND window, UINT message, WPARAM wParam,
                                    LPARAM lParam);

bool IsFrameInProgress(TaskState state) {
    return state == TaskState::Pending || state == TaskState::Dispatching ||
        state == TaskState::Assigned || state == TaskState::Rendering;
}

bool IsJobInProgress(const RenderJob& job) {
    if (job.stage != JobStage::Rendering && job.stage != JobStage::Testing)
        return false;
    return std::any_of(job.frames.begin(), job.frames.end(), [&](const FrameTask& frame) {
        if (job.stage == JobStage::Testing &&
            std::find(job.testFrames.begin(), job.testFrames.end(), frame.frame) ==
                job.testFrames.end())
            return false;
        return IsFrameInProgress(frame.state);
    });
}

bool HasFailedFrames(const RenderJob& job) {
    return std::any_of(job.frames.begin(), job.frames.end(), [](const FrameTask& frame) {
        return frame.state == TaskState::Failed;
    });
}

bool CanRetryFrame(const FrameTask& frame) {
    return frame.state == TaskState::Failed;
}

enum class DetailPrimaryAction { None, Stop, FullRender };

DetailPrimaryAction DetailPrimaryActionFor(const RenderJob& job) {
    if (IsJobInProgress(job)) return DetailPrimaryAction::Stop;
    if (job.stage == JobStage::TestCompleted)
        return DetailPrimaryAction::FullRender;
    return DetailPrimaryAction::None;
}

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
    int completed = 0, failed = 0, stopped = 0, queued = 0, rendering = 0;
    std::vector<ui::Row> frameRows;
    frameRows.reserve(job.frames.size());
    for (int index = 0; index < static_cast<int>(job.frames.size()); ++index) {
        const auto& frame = job.frames[static_cast<size_t>(index)];
        completed += frame.state == TaskState::Completed;
        failed += frame.state == TaskState::Failed;
        stopped += frame.state == TaskState::Stopped;
        queued += frame.state == TaskState::Assigned;
        rendering += frame.state == TaskState::Rendering;
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
    std::wstring summary = Utf8ToWide(job.name) + L"\r\n总帧数 " +
        std::to_wstring(job.frames.size()) + L"    已完成 " + std::to_wstring(completed) +
        L"    排队中 " + std::to_wstring(queued) + L"    渲染中 " +
        std::to_wstring(rendering) + L"    错误 " +
        std::to_wstring(failed) + L"    已停止 " + std::to_wstring(stopped) +
        L"    提交时间 " + FormatTime(job.createdAt);
    SetWindowTextIfChanged(GetDlgItem(gDetailWindow, ID_DETAIL_SUMMARY), summary);
    const DetailPrimaryAction primaryAction = DetailPrimaryActionFor(job);
    const bool stopVisibilityChanged =
        SetControlVisibleIfChanged(GetDlgItem(gDetailWindow, ID_DETAIL_STOP),
                                   primaryAction == DetailPrimaryAction::Stop);
    const bool fullRenderVisibilityChanged =
        SetControlVisibleIfChanged(GetDlgItem(gDetailWindow, ID_DETAIL_FULL_RENDER),
                                   primaryAction == DetailPrimaryAction::FullRender);
    const bool retryVisibilityChanged =
        SetControlVisibleIfChanged(GetDlgItem(gDetailWindow, ID_DETAIL_RETRY_FAILED),
                                   HasFailedFrames(job));
    const bool visibilityChanged = stopVisibilityChanged ||
        fullRenderVisibilityChanged || retryVisibilityChanged;
    // The detail window is refreshed on the 1 Hz heartbeat. Re-laying out and
    // invalidating every child when only text changed caused the visible flash.
    if (visibilityChanged) LayoutDetailWindow(gDetailWindow);
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
        // ListView reports the old row as deselected before it reports the next
        // row as selected. Keeping the last frame here avoids a one-frame empty
        // placeholder during ordinary keyboard and mouse navigation.
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
    if (frame.output.empty() || !fs::is_regular_file(frame.output)) {
        ReplacePreviewBitmap(nullptr);
        SetWindowTextW(GetDlgItem(gDetailWindow, ID_DETAIL_PREVIEW_STATUS),
                       L"该帧尚未生成图像");
        return;
    }
    gPreviewSource = frame.output;
    if (HBITMAP bitmap = LoadPreviewBitmap(frame.output, 1280, 800)) {
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
    fs::path previewPath = gApp->ProjectStorageDir() / L"previews" /
        Utf8ToWide(cacheKey + ".png");
    if (fs::is_regular_file(previewPath)) {
        if (HBITMAP bitmap = LoadPreviewBitmap(previewPath, 1280, 800)) {
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
    const fs::path dataDir = gApp->ProjectStorageDir();
    const HWND detail = gDetailWindow;
    gPreviewThread = std::thread([=] {
        auto* result = new PreviewResult;
        result->generation = generation;
        result->source = source;
        result->preview = previewPath;
        result->ok = BlenderRenderAdapter().ConvertPreview(
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

// Executable discovery scans PATH and Program Files; the UI asks every second,
// so the answer is cached briefly (UI thread only).
fs::path LocalRendererPath(const Config& config) {
    static fs::path cachedProfile;
    static fs::path cachedPath;
    static std::chrono::steady_clock::time_point checkedAt{};
    const auto now = std::chrono::steady_clock::now();
    if (checkedAt == std::chrono::steady_clock::time_point{} ||
        config.blenderPath != cachedProfile || now - checkedAt > std::chrono::seconds(3)) {
        cachedPath = BlenderRenderAdapter().LocateExecutable(config);
        cachedProfile = config.blenderPath;
        checkedAt = now;
    }
    return cachedPath;
}

void CopyText(const std::wstring& text);
void OpenSelectedJobDetail();
void RefreshLists();
void ScheduleSettingsSave();
void ShowUploadWindow(const std::string& jobId, bool editMode = false);
void LayoutUploadWindow(HWND window);
void ShowConfigWindow(const std::string& jobId);

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

std::wstring ChooseFolder(const wchar_t* title, const fs::path& suggested = {}) {
    IFileOpenDialog* dialog = nullptr;
    if (FAILED(CoCreateInstance(kClsidFileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&dialog))))
        return {};
    dialog->SetTitle(title);
    DWORD options = 0;
    dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM |
                       FOS_PATHMUSTEXIST | FOS_NOCHANGEDIR);
    IShellItem* suggestedItem = nullptr;
    if (!suggested.empty() && SUCCEEDED(SHCreateItemFromParsingName(
            suggested.c_str(), nullptr, IID_PPV_ARGS(&suggestedItem)))) {
        dialog->SetFolder(suggestedItem);
        suggestedItem->Release();
    }
    std::wstring selected;
    if (SUCCEEDED(dialog->Show(gWindow))) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dialog->GetResult(&item))) {
            PWSTR path = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
                selected = path;
                CoTaskMemFree(path);
            }
            item->Release();
        }
    }
    dialog->Release();
    return selected;
}

void StartProjectStorageMigration() {
    if (!gApp || gJobOperationRunning.load() ||
        gPreflightRunning.load()) {
        ShowInternalNotice(
            L"暂时无法更改工程存储",
            L"请等待当前工程读取、上传或文件操作完成后再更改位置。");
        return;
    }
    if (gApp->AssignedCount() > 0) {
        ShowInternalNotice(
            L"本机正在渲染",
            L"请等待当前帧完成或停止对应任务后再迁移工程存储。");
        return;
    }
    const std::wstring selected = ChooseFolder(L"选择新的 RenderMesh 工程存储目录");
    if (selected.empty()) return;
    StopPreviewGeneration();
    if (gJobOperation.joinable()) gJobOperation.join();
    gStorageMigrationPercent = 0;
    gJobOperationCancel = false;
    gJobOperationRunning = true;
    ShowWorkflowProgress(WorkflowProgressStage::MigratingStorage);
    SetStatusText(L"正在后台复制并校验工程存储；节点服务会短暂停止。");
    gJobOperation = std::thread([destination = fs::path(selected)] {
        auto* result = new JobOperationResult;
        result->kind = JobOperationKind::StorageMigration;
        gApp->Stop();
        result->ok = gApp->MigrateProjectStorage(
            destination,
            [](int percent) {
                gStorageMigrationPercent = std::clamp(percent, 0, 100);
            },
            &gJobOperationCancel, result->message);
        std::wstring startError;
        if (!gApp->Start(startError)) {
            result->ok = false;
            result->message += result->message.empty() ? L"" : L"\n\n";
            result->message += L"节点服务重新启动失败：" + startError;
        }
        {
            std::scoped_lock lock(gJobOperationResultMutex);
            delete gPendingJobOperationResult;
            gPendingJobOperationResult = result;
        }
        if (!gUiShuttingDown.load())
            PostMessageW(gWindow, WM_RM_JOB_OPERATION, 0, 0);
    });
}

std::optional<RenderJob> SelectedJob() {
    HWND jobs = Item(ID_JOBS);
    const int selected = ListView_GetNextItem(jobs, -1, LVNI_SELECTED);
    if (selected < 0 || selected >= static_cast<int>(gJobRows.Rows().size()))
        return std::nullopt;
    return FindJob(WideToUtf8(gJobRows.Rows()[static_cast<size_t>(selected)].key));
}

std::vector<RenderJob> SelectedJobs() {
    std::vector<RenderJob> selectedJobs;
    HWND jobs = Item(ID_JOBS);
    for (int selected = ListView_GetNextItem(jobs, -1, LVNI_SELECTED); selected >= 0;
         selected = ListView_GetNextItem(jobs, selected, LVNI_SELECTED)) {
        if (selected >= static_cast<int>(gJobRows.Rows().size())) continue;
        if (auto job = FindJob(
                WideToUtf8(gJobRows.Rows()[static_cast<size_t>(selected)].key)))
            selectedJobs.push_back(std::move(*job));
    }
    return selectedJobs;
}

std::vector<std::string> SelectedContributionPackageIds() {
    std::vector<std::string> packageIds;
    HWND list = Item(ID_CONTRIBUTION_LIST);
    for (int selected = ListView_GetNextItem(list, -1, LVNI_SELECTED); selected >= 0;
         selected = ListView_GetNextItem(list, selected, LVNI_SELECTED)) {
        if (selected < static_cast<int>(gContributionRows.Rows().size()))
            packageIds.push_back(WideToUtf8(
                gContributionRows.Rows()[static_cast<size_t>(selected)].key));
    }
    return packageIds;
}

struct MenuItemData {
    const wchar_t* text;
    bool danger = false;
    bool submenu = false;
};

void MeasureContextMenuItem(MEASUREITEMSTRUCT& item) {
    auto* data = reinterpret_cast<const MenuItemData*>(item.itemData);
    HDC dc = GetDC(gWindow);
    HFONT previous = static_cast<HFONT>(SelectObject(dc, gFont));
    SIZE text{};
    if (data && data->text)
        GetTextExtentPoint32W(dc, data->text, static_cast<int>(wcslen(data->text)), &text);
    SelectObject(dc, previous);
    ReleaseDC(gWindow, dc);
    item.itemWidth = std::max<UINT>(210, static_cast<UINT>(text.cx + 64));
    item.itemHeight = 34;
}

void DrawContextMenuItem(const DRAWITEMSTRUCT& item) {
    auto* data = reinterpret_cast<const MenuItemData*>(item.itemData);
    const bool selected = (item.itemState & ODS_SELECTED) != 0;
    const bool disabled = (item.itemState & ODS_DISABLED) != 0;
    COLORREF background = selected ? RGB(50, 46, 104)
        : gDarkTheme ? RGB(31, 31, 40) : RGB(250, 250, 252);
    HBRUSH brush = CreateSolidBrush(background);
    FillRect(item.hDC, &item.rcItem, brush);
    DeleteObject(brush);
    SetBkMode(item.hDC, TRANSPARENT);
    SetTextColor(item.hDC, disabled ? RGB(112, 114, 124)
        : data && data->danger ? RGB(255, 116, 122)
        : gDarkTheme ? RGB(239, 239, 247) : RGB(31, 31, 38));
    HFONT previous = static_cast<HFONT>(SelectObject(item.hDC, gFont));
    RECT textRect = item.rcItem;
    textRect.left += 18;
    DrawTextW(item.hDC, data && data->text ? data->text : L"", -1, &textRect,
              DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_NOPREFIX);
    if (data && data->submenu) {
        RECT arrowRect = item.rcItem;
        arrowRect.right -= 14;
        DrawTextW(item.hDC, L"›", 1, &arrowRect,
                  DT_SINGLELINE | DT_VCENTER | DT_RIGHT | DT_NOPREFIX);
    }
    SelectObject(item.hDC, previous);
    if (item.itemState & ODS_FOCUS) {
        RECT focus = item.rcItem;
        InflateRect(&focus, -3, -3);
        DrawFocusRect(item.hDC, &focus);
    }
}

void StartSaveJobAs(const RenderJob& job) {
    if (gJobOperationRunning.load()) {
        MessageBoxW(gWindow, L"已有另存操作正在进行。", L"RenderMesh", MB_ICONINFORMATION);
        return;
    }
    const std::wstring destination = ChooseFolder(L"选择作业序列的另存位置");
    if (destination.empty()) return;
    if (gJobOperation.joinable()) gJobOperation.join();
    gJobOperationRunning = true;
    gJobOperationCancel = false;
    const std::string jobId = job.id;
    gJobOperation = std::thread([jobId, destination] {
        auto* result = new JobOperationResult;
        result->ok = gApp->SaveJobAs(
            jobId, destination, result->message, &gJobOperationCancel);
        if (result->ok) result->message = L"作业序列已另存完成。";
        {
            std::scoped_lock lock(gJobOperationResultMutex);
            delete gPendingJobOperationResult;
            gPendingJobOperationResult = result;
        }
        if (!gUiShuttingDown.load()) PostMessageW(gWindow, WM_RM_JOB_OPERATION, 0, 0);
    });
}

void StartCancelUploadJob() {
    if (!gApp || gUploadJobId.empty()) return;
    if (gJobOperationRunning.load()) {
        SetStatusText(L"已有作业操作正在执行，请稍后再取消上传。");
        return;
    }
    if (gJobOperation.joinable()) gJobOperation.join();
    const std::string jobId = gUploadJobId;
    gJobOperationRunning = true;
    gUploadCancelRunning = true;
    if (gUploadWindow) {
        HWND button = GetDlgItem(gUploadWindow, ID_UPLOAD_CANCEL);
        SetWindowTextW(button, L"正在取消…");
        EnableWindow(button, FALSE);
    }
    SetStatusText(L"正在后台取消工程上传；窗口仍可移动和操作。");
    gJobOperation = std::thread([jobId] {
        auto* result = new JobOperationResult;
        result->kind = JobOperationKind::CancelUpload;
        result->jobId = jobId;
        result->ok = gApp->StopJob(jobId, result->message);
        if (result->ok)
            result->message = L"已取消整个工程上传任务；远端已接收的分块缓存可供后续恢复。";
        {
            std::scoped_lock lock(gJobOperationResultMutex);
            delete gPendingJobOperationResult;
            gPendingJobOperationResult = result;
        }
        if (!gUiShuttingDown.load())
            PostMessageW(gWindow, WM_RM_JOB_OPERATION, 0, 0);
    });
}

void ExecuteJobContextCommand(UINT command, const RenderJob& job) {
    std::wstring error;
    switch (command) {
        case ID_JOB_PRIORITY:
            if (!gApp->SetJobPriority(job.id, job.priority == 0, error))
                ShowInternalNotice(L"无法调整优先级", error);
            break;
        case ID_JOB_SAVE_AS:
            StartSaveJobAs(job);
            break;
        case ID_JOB_OPEN_OUTPUT: {
            fs::path directory;
            for (const auto& frame : job.frames) {
                if (!frame.output.empty() && fs::exists(frame.output)) {
                    directory = frame.output.parent_path();
                    break;
                }
            }
            if (directory.empty())
                directory = gApp->ProjectStorageDir() / L"jobs" /
                    Utf8ToWide(job.id) / L"outputs";
            ShellExecuteW(gWindow, L"open", directory.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            break;
        }
        case ID_JOB_CONTEXT_DETAILS:
            OpenSelectedJobDetail();
            break;
        case ID_JOB_START_FULL:
            if (!gApp->StartFullRender(job.id, error))
                ShowInternalNotice(L"无法开始全速渲染", error);
            else
                SetStatusText(L"测试已确认，正式帧已进入全速渲染。");
            break;
        case ID_JOB_RESUME:
            if (!gApp->ResumeStoppedJob(job.id, error))
                ShowInternalNotice(L"无法恢复渲染", error);
            else
                SetStatusText(L"已完成帧保持不变，已停止帧重新进入渲染队列。");
            break;
        case ID_JOB_RETRY_FAILED:
            if (!gApp->RetryFailedFrames(job.id, error))
                ShowInternalNotice(L"无法重新渲染失败帧", error);
            else
                SetStatusText(L"失败帧已重新进入渲染队列。");
            break;
        case ID_JOB_EDIT_PARTICIPANTS:
            ShowUploadWindow(job.id, true);
            break;
        case ID_JOB_COPY_ID:
            CopyText(Utf8ToWide(job.id));
            break;
        case ID_JOB_COPY_NAME:
            CopyText(Utf8ToWide(job.sceneName.empty() ? job.name : job.sceneName));
            break;
        case ID_JOB_COPY_BOTH:
            CopyText(Utf8ToWide(job.id + "  " +
                (job.sceneName.empty() ? job.name : job.sceneName)));
            break;
        case ID_JOB_DELETE: {
            ShowDeleteConfirmation(job);
            break;
        }
    }
    RefreshLists();
}

void ExecuteBulkJobContextCommand(UINT command, const std::vector<RenderJob>& jobs) {
    if (jobs.empty()) return;
    if (command == ID_JOB_BULK_DELETE) {
        gPendingDeleteJobIds.clear();
        for (const auto& job : jobs) gPendingDeleteJobIds.push_back(job.id);
        ShowInternalDialog(
            InternalDialogMode::DeleteConfirmation,
            L"批量删除作业",
            std::to_wstring(jobs.size()) + L" 个作业",
            L"将停止所有未完成帧，并删除所选作业的 RenderMesh 托管工程副本、"
            L"渲染序列与远端缓存。原始 .blend 和已另存结果不会被删除。");
        return;
    }
    if (command != ID_JOB_BULK_START_FULL) return;
    size_t started = 0;
    size_t skipped = 0;
    std::wstring lastError;
    for (const auto& job : jobs) {
        if (job.stage != JobStage::TestCompleted) {
            ++skipped;
            continue;
        }
        std::wstring error;
        if (gApp->StartFullRender(job.id, error)) ++started;
        else {
            ++skipped;
            lastError = std::move(error);
        }
    }
    std::wstring status = L"已将 " + std::to_wstring(started) +
        L" 个作业切换为全速渲染";
    if (skipped) status += L"；跳过 " + std::to_wstring(skipped) + L" 个未完成测试的作业";
    SetStatusText(status + L"。");
    if (!started && !lastError.empty())
        ShowInternalNotice(L"无法批量开始渲染", lastError);
    RefreshLists();
}

void HideThreadMenuWindows() {
    EnumThreadWindows(GetCurrentThreadId(), [](HWND window, LPARAM) -> BOOL {
        wchar_t className[32]{};
        if (GetClassNameW(window, className, static_cast<int>(std::size(className))) > 0 &&
            wcscmp(className, L"#32768") == 0) {
            SendMessageW(window, WM_CANCELMODE, 0, 0);
            ShowWindow(window, SW_HIDE);
        }
        return TRUE;
    }, 0);
}

UINT TrackAndCloseContextMenu(HWND owner, HMENU menu, int x, int y) {
    if (!owner || !IsWindow(owner) || !menu) return 0;
    SetForegroundWindow(owner);
    const UINT command = TrackPopupMenuEx(
        menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_NOANIMATION |
            TPM_RIGHTBUTTON | TPM_LEFTALIGN,
        x, y, owner, nullptr);
    // TrackPopupMenuEx can return while the composited popup is still in its
    // dismissal animation. End menu mode explicitly, destroy the complete menu
    // tree, repaint the owner synchronously, then wait for one DWM composition
    // boundary before a command opens another window or captures a backdrop.
    EndMenu();
    HideThreadMenuWindows();
    SendMessageW(owner, WM_CANCELMODE, 0, 0);
    DestroyMenu(menu);
    RedrawWindow(owner, nullptr, nullptr,
                 RDW_INVALIDATE | RDW_ERASE | RDW_FRAME |
                     RDW_ALLCHILDREN | RDW_UPDATENOW);
    HMODULE dwm = LoadLibraryW(L"dwmapi.dll");
    if (dwm) {
        using Flush = HRESULT(WINAPI*)();
        if (auto flush = reinterpret_cast<Flush>(GetProcAddress(dwm, "DwmFlush")))
            flush();
        FreeLibrary(dwm);
    }
    return command;
}

void DeferMenuAction(std::function<void()> action) {
    if (!action || !gWindow || !IsWindow(gWindow)) return;
    gDeferredMenuActions.push_back(std::move(action));
    KillTimer(gWindow, TIMER_MENU_ACTION);
    SetTimer(gWindow, TIMER_MENU_ACTION, 60, nullptr);
}

void QueueContextMenu(UINT controlId, POINT screenPoint) {
    if (!gWindow || !IsWindow(gWindow)) return;
    gPendingContextMenuControl = controlId;
    gPendingContextMenuPoint = screenPoint;
    KillTimer(gWindow, TIMER_CONTEXT_MENU_OPEN);
    // A ListView emits NM_RCLICK before its full right-button notification
    // cycle has drained. Opening a popup on that stack lets the trailing input
    // keep the native #32768 menu alive after a command. Open it only after the
    // input cycle is complete.
    SetTimer(gWindow, TIMER_CONTEXT_MENU_OPEN, 60, nullptr);
}

void ShowJobContextMenu(POINT screenPoint) {
    auto jobs = SelectedJobs();
    if (jobs.empty()) return;
    static const MenuItemData priorityOn{L"优先渲染"};
    static const MenuItemData priorityOff{L"取消优先"};
    static const MenuItemData saveAs{L"另存为…"};
    static const MenuItemData openOutput{L"打开本地下载目录"};
    static const MenuItemData details{L"查看作业详情"};
    static const MenuItemData fullRender{L"全速渲染"};
    static const MenuItemData resumeRender{L"恢复渲染"};
    static const MenuItemData retryFailed{L"重新渲染失败帧"};
    static const MenuItemData editParticipants{L"编辑渲染主机"};
    static const MenuItemData copy{L"复制", false, true};
    static const MenuItemData copyId{L"作业 ID"};
    static const MenuItemData copyName{L"场景名"};
    static const MenuItemData copyBoth{L"作业 ID + 场景名"};
    static const MenuItemData remove{L"删除作业", true};
    static const MenuItemData bulkFullRender{L"批量全速渲染"};
    static const MenuItemData bulkRemove{L"批量删除所选作业", true};
    HMENU menu = CreatePopupMenu();
    HMENU copyMenu = CreatePopupMenu();
    if (jobs.size() > 1) {
        const bool anyTestCompleted = std::any_of(
            jobs.begin(), jobs.end(), [](const RenderJob& value) {
                return value.stage == JobStage::TestCompleted;
            });
        AppendMenuW(menu, MF_OWNERDRAW | (anyTestCompleted ? 0 : MF_GRAYED),
                    ID_JOB_BULK_START_FULL,
                    reinterpret_cast<LPCWSTR>(&bulkFullRender));
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_OWNERDRAW, ID_JOB_BULK_DELETE,
                    reinterpret_cast<LPCWSTR>(&bulkRemove));
        MENUINFO info{};
        info.cbSize = sizeof(info);
        info.fMask = MIM_BACKGROUND | MIM_STYLE;
        info.hbrBack = gSurfaceBrush;
        info.dwStyle = MNS_CHECKORBMP;
        SetMenuInfo(menu, &info);
        const UINT command = TrackAndCloseContextMenu(
            gWindow, menu, screenPoint.x, screenPoint.y);
        if (command) {
            DeferMenuAction([command, jobs = std::move(jobs)] {
                ExecuteBulkJobContextCommand(command, jobs);
            });
        }
        return;
    }
    const RenderJob& job = jobs.front();
    const bool hasPending = std::any_of(job.frames.begin(), job.frames.end(),
        [](const FrameTask& frame) {
            return frame.state == TaskState::Pending ||
                frame.state == TaskState::Dispatching ||
                frame.state == TaskState::Assigned ||
                frame.state == TaskState::Rendering;
        });
    const bool hasCompleted = !job.frames.empty() &&
        std::all_of(job.frames.begin(), job.frames.end(),
        [](const FrameTask& frame) {
            return frame.state == TaskState::Completed && !frame.output.empty();
        });
    const bool hasFailed = HasFailedFrames(job);
    if (job.stage == JobStage::Stopped)
        AppendMenuW(menu, MF_OWNERDRAW, ID_JOB_RESUME,
                    reinterpret_cast<LPCWSTR>(&resumeRender));
    else
        AppendMenuW(menu, MF_OWNERDRAW |
                    (job.stage == JobStage::TestCompleted ? 0 : MF_GRAYED),
                    ID_JOB_START_FULL, reinterpret_cast<LPCWSTR>(&fullRender));
    if (hasFailed)
        AppendMenuW(menu, MF_OWNERDRAW, ID_JOB_RETRY_FAILED,
                    reinterpret_cast<LPCWSTR>(&retryFailed));
    AppendMenuW(menu, MF_OWNERDRAW |
                (job.stage == JobStage::Completed ||
                 job.artifactsPurgedAt.time_since_epoch() !=
                     std::chrono::system_clock::duration::zero()
                    ? MF_GRAYED : 0),
                ID_JOB_EDIT_PARTICIPANTS,
                reinterpret_cast<LPCWSTR>(&editParticipants));
    AppendMenuW(menu, MF_OWNERDRAW | (hasPending ? 0 : MF_GRAYED), ID_JOB_PRIORITY,
                reinterpret_cast<LPCWSTR>(job.priority > 0 ? &priorityOff : &priorityOn));
    AppendMenuW(menu, MF_OWNERDRAW | (hasCompleted &&
                job.artifactsPurgedAt.time_since_epoch() ==
                    std::chrono::system_clock::duration::zero() ? 0 : MF_GRAYED), ID_JOB_SAVE_AS,
                reinterpret_cast<LPCWSTR>(&saveAs));
    AppendMenuW(menu, MF_OWNERDRAW, ID_JOB_OPEN_OUTPUT,
                reinterpret_cast<LPCWSTR>(&openOutput));
    AppendMenuW(menu, MF_OWNERDRAW, ID_JOB_CONTEXT_DETAILS,
                reinterpret_cast<LPCWSTR>(&details));
    AppendMenuW(copyMenu, MF_OWNERDRAW, ID_JOB_COPY_ID,
                reinterpret_cast<LPCWSTR>(&copyId));
    AppendMenuW(copyMenu, MF_OWNERDRAW, ID_JOB_COPY_NAME,
                reinterpret_cast<LPCWSTR>(&copyName));
    AppendMenuW(copyMenu, MF_OWNERDRAW, ID_JOB_COPY_BOTH,
                reinterpret_cast<LPCWSTR>(&copyBoth));
    AppendMenuW(menu, MF_POPUP | MF_OWNERDRAW,
                reinterpret_cast<UINT_PTR>(copyMenu), reinterpret_cast<LPCWSTR>(&copy));
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_OWNERDRAW, ID_JOB_DELETE,
                reinterpret_cast<LPCWSTR>(&remove));
    MENUINFO info{};
    info.cbSize = sizeof(info);
    info.fMask = MIM_BACKGROUND | MIM_STYLE;
    info.hbrBack = gSurfaceBrush;
    info.dwStyle = MNS_CHECKORBMP;
    SetMenuInfo(menu, &info);
    SetMenuInfo(copyMenu, &info);
    const UINT command = TrackAndCloseContextMenu(
        gWindow, menu, screenPoint.x, screenPoint.y);
    if (command) {
        DeferMenuAction([command, job] {
            ExecuteJobContextCommand(command, job);
        });
    }
}

void ShowContributionContextMenu(POINT screenPoint) {
    auto packageIds = SelectedContributionPackageIds();
    if (packageIds.empty()) return;
    static const MenuItemData clearSelected{L"清理选中历史记录", true};
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_OWNERDRAW, ID_CONTRIBUTION_CLEAR_SELECTED,
                reinterpret_cast<LPCWSTR>(&clearSelected));
    MENUINFO info{};
    info.cbSize = sizeof(info);
    info.fMask = MIM_BACKGROUND | MIM_STYLE;
    info.hbrBack = gSurfaceBrush;
    info.dwStyle = MNS_CHECKORBMP;
    SetMenuInfo(menu, &info);
    const UINT command = TrackAndCloseContextMenu(
        gWindow, menu, screenPoint.x, screenPoint.y);
    if (command != ID_CONTRIBUTION_CLEAR_SELECTED) return;
    DeferMenuAction([packageIds = std::move(packageIds)] {
        gPendingContributionPackageIds = packageIds;
        ShowInternalDialog(
            InternalDialogMode::ContributionDeleteConfirmation,
            L"清理贡献历史",
            std::to_wstring(gPendingContributionPackageIds.size()) + L" 条本机记录",
            L"只清理所选的本机贡献历史元数据，不会删除渲染任务、工程文件或输出结果。");
    });
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

const wchar_t* ThemeSelectionText(int selection) {
    switch (selection) {
        case 1: return L"浅色";
        case 2: return L"深色";
        default: return L"跟随系统";
    }
}

void SetThemeSelection(int selection) {
    HWND button = Item(ID_THEME);
    if (!button) return;
    selection = std::clamp(selection, 0, 2);
    SetPropW(button, kThemeSelectionProperty,
             reinterpret_cast<HANDLE>(static_cast<INT_PTR>(selection + 1)));
    SetWindowTextW(button, ThemeSelectionText(selection));
    InvalidateRect(button, nullptr, FALSE);
}

int ThemeSelection() {
    HWND button = Item(ID_THEME);
    const INT_PTR stored = reinterpret_cast<INT_PTR>(
        GetPropW(button, kThemeSelectionProperty));
    return stored >= 1 && stored <= 3 ? static_cast<int>(stored - 1) : 0;
}

void ShowThemeSelectionMenu() {
    HWND button = Item(ID_THEME);
    if (!button) return;
    HMENU menu = CreatePopupMenu();
    const int current = ThemeSelection();
    for (int selection = 0; selection < 3; ++selection) {
        AppendMenuW(menu, MF_STRING | (selection == current ? MF_CHECKED : 0),
                    static_cast<UINT_PTR>(selection + 1),
                    ThemeSelectionText(selection));
    }
    RECT rect{};
    GetWindowRect(button, &rect);
    const UINT command = TrackAndCloseContextMenu(
        gWindow, menu, rect.left, rect.bottom + 4);
    if (command >= 1 && command <= 3) {
        DeferMenuAction([selection = static_cast<int>(command - 1)] {
            SetThemeSelection(selection);
            gSettingsDirty = true;
            SetStatusText(L"正在自动保存外观设置…");
            SetTimer(gWindow, TIMER_SETTINGS_AUTOSAVE, 300, nullptr);
        });
    }
}

const wchar_t* CloseBehaviorSelectionText(int selection) {
    switch (selection) {
        case 1: return L"直接关闭";
        case 2: return L"最小化到任务托盘";
        default: return L"每次询问";
    }
}

void SetCloseBehaviorSelection(int selection) {
    HWND button = Item(ID_CLOSE_BEHAVIOR);
    if (!button) return;
    selection = std::clamp(selection, 0, 2);
    SetPropW(button, kCloseBehaviorSelectionProperty,
             reinterpret_cast<HANDLE>(static_cast<INT_PTR>(selection + 1)));
    SetWindowTextW(button, CloseBehaviorSelectionText(selection));
    InvalidateRect(button, nullptr, FALSE);
}

std::wstring AutoStartCommand(const fs::path& executable) {
    return L"\"" + executable.wstring() + L"\" --startup";
}

bool ShouldStartSilently(const Config& config, bool startupInvocation) {
    return startupInvocation && config.autoStart && config.silentAutoStart;
}

bool UpdateAutoStartRegistration(
    const Config& config, const wchar_t* valueName,
    const fs::path& executable, std::wstring& error) {
    constexpr wchar_t runKey[] =
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
    HKEY key = nullptr;
    LSTATUS status = ERROR_SUCCESS;
    if (config.autoStart) {
        status = RegCreateKeyExW(
            HKEY_CURRENT_USER, runKey, 0, nullptr, REG_OPTION_NON_VOLATILE,
            KEY_SET_VALUE, nullptr, &key, nullptr);
        if (status == ERROR_SUCCESS) {
            const std::wstring command = AutoStartCommand(executable);
            status = RegSetValueExW(
                key, valueName, 0, REG_SZ,
                reinterpret_cast<const BYTE*>(command.c_str()),
                static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t)));
        }
    } else {
        status = RegOpenKeyExW(HKEY_CURRENT_USER, runKey, 0, KEY_SET_VALUE, &key);
        if (status == ERROR_FILE_NOT_FOUND) return true;
        if (status == ERROR_SUCCESS) {
            status = RegDeleteValueW(key, valueName);
            if (status == ERROR_FILE_NOT_FOUND) status = ERROR_SUCCESS;
        }
    }
    if (key) RegCloseKey(key);
    if (status == ERROR_SUCCESS) return true;
    SetLastError(static_cast<DWORD>(status));
    error = config.autoStart
        ? L"无法启用开机自动启动：" + LastErrorMessage()
        : L"无法关闭开机自动启动：" + LastErrorMessage();
    return false;
}

bool SyncAutoStartRegistration(const Config& config, std::wstring& error) {
    return UpdateAutoStartRegistration(
        config, L"RenderMesh", ExecutablePath(), error);
}

#ifdef RM_DEV_TOOLS
bool RunAutoStartLogicSelfTest(std::wstring& error) {
    const fs::path probeExecutable =
        L"C:\\Program Files\\RenderMesh Self Test\\RenderMesh.exe";
    Config enabled;
    enabled.autoStart = true;
    enabled.silentAutoStart = true;
    if (AutoStartCommand(probeExecutable) !=
            L"\"C:\\Program Files\\RenderMesh Self Test\\RenderMesh.exe\" --startup" ||
        !ShouldStartSilently(enabled, true) ||
        ShouldStartSilently(enabled, false)) {
        error = L"开机自启命令或静默启动判定失败";
        return false;
    }
    enabled.autoStart = false;
    if (ShouldStartSilently(enabled, true)) {
        error = L"关闭开机自启后仍会静默启动";
        return false;
    }
    return true;
}

#endif  // RM_DEV_TOOLS

std::wstring NormalizedPathIdentity(const fs::path& path) {
    std::error_code ignored;
    fs::path normalized = fs::absolute(path, ignored);
    if (ignored) normalized = path;
    normalized = normalized.lexically_normal();
    std::wstring value = normalized.wstring();
    std::transform(value.begin(), value.end(), value.begin(), [](wchar_t character) {
        return static_cast<wchar_t>(std::towlower(character));
    });
    return value;
}

uint64_t SingleInstanceKey(const fs::path& dataDir) {
    uint64_t hash = 1469598103934665603ULL;
    for (wchar_t character : NormalizedPathIdentity(dataDir)) {
        hash ^= static_cast<uint64_t>(character);
        hash *= 1099511628211ULL;
    }
    return hash == 0 ? 1 : hash;
}

struct ExistingInstanceSearch {
    uint64_t key = 0;
    HWND window = nullptr;
};

BOOL CALLBACK FindExistingInstanceWindow(HWND window, LPARAM parameter) {
    auto* search = reinterpret_cast<ExistingInstanceSearch*>(parameter);
    wchar_t className[64]{};
    if (!GetClassNameW(window, className, static_cast<int>(std::size(className))) ||
        wcscmp(className, L"RenderMeshNativeWindow") != 0)
        return TRUE;
    const uint64_t key = static_cast<uint64_t>(reinterpret_cast<ULONG_PTR>(
        GetPropW(window, kSingleInstanceKeyProperty)));
    if (key != search->key) return TRUE;
    search->window = window;
    return FALSE;
}

bool AcquireSingleInstance(const fs::path& dataDir, bool restoreExisting,
                           bool& existing,
                           std::wstring& error) {
    existing = false;
    gSingleInstanceKey = SingleInstanceKey(dataDir);
    wchar_t suffix[17]{};
    swprintf_s(suffix, L"%016llx", static_cast<unsigned long long>(gSingleInstanceKey));
    const std::wstring identity = suffix;
    const std::wstring messageName = L"RenderMesh.Restore." + identity;
    gRestoreInstanceMessage = RegisterWindowMessageW(messageName.c_str());
    if (!gRestoreInstanceMessage) {
        error = L"无法注册 RenderMesh 实例唤醒消息：" + LastErrorMessage();
        return false;
    }
    const std::wstring readyEventName =
        L"Local\\RenderMesh.Native.Ready." + identity;
    gSingleInstanceReadyEvent = CreateEventW(
        nullptr, TRUE, FALSE, readyEventName.c_str());
    if (!gSingleInstanceReadyEvent) {
        error = L"无法建立 RenderMesh 实例就绪信号：" + LastErrorMessage();
        return false;
    }
    const std::wstring mutexName = L"Local\\RenderMesh.Native." + identity;
    gSingleInstanceMutex = CreateMutexW(nullptr, FALSE, mutexName.c_str());
    if (!gSingleInstanceMutex) {
        error = L"无法建立 RenderMesh 单实例锁：" + LastErrorMessage();
        CloseHandle(gSingleInstanceReadyEvent);
        gSingleInstanceReadyEvent = nullptr;
        return false;
    }
    if (GetLastError() != ERROR_ALREADY_EXISTS) return true;

    CloseHandle(gSingleInstanceMutex);
    gSingleInstanceMutex = nullptr;
    existing = true;
    if (!restoreExisting) {
        CloseHandle(gSingleInstanceReadyEvent);
        gSingleInstanceReadyEvent = nullptr;
        return true;
    }
    const DWORD ready = WaitForSingleObject(gSingleInstanceReadyEvent, 30000);
    if (ready != WAIT_OBJECT_0) {
        error = ready == WAIT_TIMEOUT
            ? L"已有 RenderMesh 正在启动，但 30 秒内没有就绪。请稍后重试。"
            : L"等待已有 RenderMesh 就绪失败：" + LastErrorMessage();
        CloseHandle(gSingleInstanceReadyEvent);
        gSingleInstanceReadyEvent = nullptr;
        return false;
    }
    for (int attempt = 0; attempt < 20; ++attempt) {
        ExistingInstanceSearch search{gSingleInstanceKey, nullptr};
        EnumWindows(FindExistingInstanceWindow,
                    reinterpret_cast<LPARAM>(&search));
        if (search.window) {
            PostMessageW(search.window, gRestoreInstanceMessage, 0, 0);
            CloseHandle(gSingleInstanceReadyEvent);
            gSingleInstanceReadyEvent = nullptr;
            return true;
        }
        Sleep(50);
    }
    error = L"已有 RenderMesh 已启动，但无法定位它的主窗口。";
    CloseHandle(gSingleInstanceReadyEvent);
    gSingleInstanceReadyEvent = nullptr;
    return false;
}

void ReleaseSingleInstance() {
    if (gSingleInstanceMutex) CloseHandle(gSingleInstanceMutex);
    if (gSingleInstanceReadyEvent) CloseHandle(gSingleInstanceReadyEvent);
    gSingleInstanceMutex = nullptr;
    gSingleInstanceReadyEvent = nullptr;
}

int CloseBehaviorSelection() {
    HWND button = Item(ID_CLOSE_BEHAVIOR);
    const INT_PTR stored = reinterpret_cast<INT_PTR>(
        button ? GetPropW(button, kCloseBehaviorSelectionProperty) : nullptr);
    return stored >= 1 && stored <= 3 ? static_cast<int>(stored - 1) : 0;
}

void ShowCloseBehaviorSelectionMenu() {
    HWND button = Item(ID_CLOSE_BEHAVIOR);
    if (!button) return;
    HMENU menu = CreatePopupMenu();
    const int current = CloseBehaviorSelection();
    for (int selection = 0; selection < 3; ++selection) {
        AppendMenuW(menu, MF_STRING | (selection == current ? MF_CHECKED : 0),
                    static_cast<UINT_PTR>(selection + 1),
                    CloseBehaviorSelectionText(selection));
    }
    RECT rect{};
    GetWindowRect(button, &rect);
    const UINT command = TrackAndCloseContextMenu(
        gWindow, menu, rect.left, rect.bottom + 4);
    if (command >= 1 && command <= 3) {
        DeferMenuAction([selection = static_cast<int>(command - 1)] {
            SetCloseBehaviorSelection(selection);
            ScheduleSettingsSave();
        });
    }
}

void LoadConfigIntoUi() {
    gLoadingSettings = true;
    Config config = gApp->GetConfig();
    SetText(ID_NODE_NAME, Utf8ToWide(config.nodeName));
    SendMessageW(Item(ID_SHARE), BM_SETCHECK,
                 config.shareEnabled ? BST_CHECKED : BST_UNCHECKED, 0);
    SetText(ID_IDLE_SECONDS, std::to_wstring(config.idleAfterSeconds));
    SetText(ID_CLUSTER_TOKEN, Utf8ToWide(config.inviteCode));
    SetText(ID_MANUAL_ENDPOINT, Utf8ToWide(config.manualPeerEndpoint));
    int themeSelection = config.theme == "light" ? 1 : config.theme == "dark" ? 2 : 0;
    SetThemeSelection(themeSelection);
    SetCloseBehaviorSelection(config.closeBehavior == "exit" ? 1
        : config.closeBehavior == "tray" ? 2 : 0);
    SendMessageW(Item(ID_AUTO_START), BM_SETCHECK,
                 config.autoStart ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(Item(ID_SILENT_AUTO_START), BM_SETCHECK,
                 config.silentAutoStart ? BST_CHECKED : BST_UNCHECKED, 0);
    const fs::path blender = LocalRendererPath(config);
    SetText(ID_BLENDER_PATH, blender.empty()
        ? L"未检测到 Blender" : blender.wstring());
    SetText(ID_PROJECT_STORAGE_PATH, config.projectStoragePath.wstring());
    EnableWindow(Item(ID_SILENT_AUTO_START), config.autoStart ? TRUE : FALSE);
    InvalidateRect(Item(ID_AUTO_START), nullptr, FALSE);
    InvalidateRect(Item(ID_SILENT_AUTO_START), nullptr, FALSE);
    gLoadingSettings = false;
    gSettingsDirty = false;
}

Config ReadConfigFromUi() {
    Config config = gApp->GetConfig();
    config.nodeName = WideToUtf8(Text(Item(ID_NODE_NAME)));
    config.idleAfterSeconds = std::clamp(IntValue(ID_IDLE_SECONDS, 180), 1, 86400);
    if (Item(ID_CLUSTER_TOKEN))
        config.inviteCode = WideToUtf8(Text(Item(ID_CLUSTER_TOKEN)));
    int themeSelection = ThemeSelection();
    config.theme = themeSelection == 1 ? "light" : themeSelection == 2 ? "dark" : "system";
    const int closeSelection = CloseBehaviorSelection();
    config.closeBehavior = closeSelection == 1 ? "exit"
        : closeSelection == 2 ? "tray" : "ask";
    config.autoStart = SendMessageW(
        Item(ID_AUTO_START), BM_GETCHECK, 0, 0) == BST_CHECKED;
    config.silentAutoStart = config.autoStart && SendMessageW(
        Item(ID_SILENT_AUTO_START), BM_GETCHECK, 0, 0) == BST_CHECKED;
    return config;
}

bool SaveSettings() {
    if (!gApp || gLoadingSettings) return false;
    const Config previous = gApp->GetConfig();
    Config config = ReadConfigFromUi();
    std::wstring error;
    const bool autoStartRegistrationChanged =
        gAutoStartRegistrationEnabled && previous.autoStart != config.autoStart;
    if (autoStartRegistrationChanged &&
        !SyncAutoStartRegistration(config, error)) {
        SetStatusText(L"设置未保存：" + error);
        return false;
    }
    if (!gApp->UpdateConfig(config, error)) {
        if (autoStartRegistrationChanged) {
            const std::wstring persistenceError = error;
            std::wstring rollbackError;
            if (!SyncAutoStartRegistration(previous, rollbackError)) {
                error = persistenceError +
                    L"；开机自启注册表回滚失败，当前启动状态可能与界面不一致：" +
                    rollbackError;
            }
        }
        SetStatusText(L"设置未保存：" + error);
        return false;
    }
    if (autoStartRegistrationChanged) gAutoStartStartupWarning.clear();
    gSettingsDirty = false;
    SetStatusText(L"设置已自动保存");
    if (previous.theme != config.theme) ApplyTheme();
    return true;
}

enum class TrayVisualState { Idle, Error, Rendering };

HICON CreateBrandedTrayIcon(TrayVisualState state) {
    constexpr int size = 32;
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = size;
    info.bmiHeader.biHeight = -size;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void* raw = nullptr;
    HDC screen = GetDC(nullptr);
    HBITMAP colourBitmap = CreateDIBSection(
        screen, &info, DIB_RGB_COLORS, &raw, nullptr, 0);
    HDC memory = CreateCompatibleDC(screen);
    HGDIOBJ previous = SelectObject(memory, colourBitmap);
    if (raw) memset(raw, 0, size * size * 4);
    const COLORREF panel = RGB(16, 20, 27);
    const COLORREF mesh = RGB(23, 201, 130);
    const COLORREF badge = state == TrayVisualState::Error
        ? RGB(239, 68, 68)
        : state == TrayVisualState::Rendering
            ? RGB(77, 69, 233) : RGB(23, 201, 130);
    HBRUSH panelBrush = CreateSolidBrush(panel);
    HPEN meshPen = CreatePen(PS_SOLID, 2, mesh);
    HGDIOBJ oldBrush = SelectObject(memory, panelBrush);
    HGDIOBJ oldPen = SelectObject(memory, meshPen);
    RoundRect(memory, 2, 2, 30, 30, 8, 8);
    const std::array<POINT, 4> nodes = {
        POINT{16, 7}, POINT{8, 16}, POINT{16, 24}, POINT{24, 16},
    };
    const std::array<std::pair<int, int>, 6> edges = {
        std::pair{0, 1}, std::pair{0, 3}, std::pair{1, 2},
        std::pair{3, 2}, std::pair{1, 3}, std::pair{0, 2},
    };
    for (const auto& [from, to] : edges) {
        MoveToEx(memory, nodes[from].x, nodes[from].y, nullptr);
        LineTo(memory, nodes[to].x, nodes[to].y);
    }
    HBRUSH meshBrush = CreateSolidBrush(mesh);
    SelectObject(memory, meshBrush);
    for (const POINT node : nodes)
        Ellipse(memory, node.x - 2, node.y - 2, node.x + 3, node.y + 3);

    HPEN badgeBorder = CreatePen(PS_SOLID, 2, RGB(244, 246, 250));
    HBRUSH badgeBrush = CreateSolidBrush(badge);
    SelectObject(memory, badgeBorder);
    SelectObject(memory, badgeBrush);
    Ellipse(memory, 17, 17, 31, 31);
    HPEN glyphPen = CreatePen(PS_SOLID, 2, RGB(255, 255, 255));
    SelectObject(memory, glyphPen);
    if (state == TrayVisualState::Idle) {
        MoveToEx(memory, 20, 24, nullptr);
        LineTo(memory, 23, 27);
        LineTo(memory, 28, 21);
    } else if (state == TrayVisualState::Rendering) {
        MoveToEx(memory, 20, 22, nullptr);
        LineTo(memory, 24, 20);
        LineTo(memory, 28, 22);
        MoveToEx(memory, 28, 26, nullptr);
        LineTo(memory, 24, 28);
        LineTo(memory, 20, 26);
    } else {
        MoveToEx(memory, 24, 20, nullptr);
        LineTo(memory, 24, 25);
        Ellipse(memory, 23, 27, 26, 30);
    }
    SelectObject(memory, oldBrush);
    SelectObject(memory, oldPen);
    DeleteObject(glyphPen);
    DeleteObject(badgeBrush);
    DeleteObject(badgeBorder);
    DeleteObject(meshBrush);
    DeleteObject(meshPen);
    DeleteObject(panelBrush);
    SelectObject(memory, previous);
    DeleteDC(memory);
    ReleaseDC(nullptr, screen);
    if (!colourBitmap || !raw) {
        if (colourBitmap) DeleteObject(colourBitmap);
        return nullptr;
    }
    auto* pixels = static_cast<unsigned char*>(raw);
    for (int index = 0; index < size * size; ++index) {
        unsigned char* pixel = pixels + index * 4;
        if (pixel[0] || pixel[1] || pixel[2]) pixel[3] = 255;
    }
    HBITMAP mask = CreateBitmap(size, size, 1, 1, nullptr);
    ICONINFO iconInfo{};
    iconInfo.fIcon = TRUE;
    iconInfo.hbmMask = mask;
    iconInfo.hbmColor = colourBitmap;
    HICON result = CreateIconIndirect(&iconInfo);
    DeleteObject(mask);
    DeleteObject(colourBitmap);
    return result;
}

TrayVisualState CurrentTrayVisualState() {
    if (!gApp) return TrayVisualState::Idle;
    bool rendering = gApp->AssignedCount() > 0 || gApp->CurrentActivity().Active();
    for (const auto& job : gApp->Jobs()) {
        if (job.stage == JobStage::UploadFailed || job.stage == JobStage::TestFailed ||
            job.stage == JobStage::Failed)
            return TrayVisualState::Error;
        rendering = rendering || job.stage == JobStage::Testing ||
            job.stage == JobStage::Rendering;
    }
    return rendering ? TrayVisualState::Rendering : TrayVisualState::Idle;
}

std::wstring CurrentTrayStatusText() {
    if (!gApp) return L"RenderMesh";
    const WorkerActivity activity = gApp->CurrentActivity();
    if (activity.Active()) {
        return L"正在渲染：" + Utf8ToWide(activity.jobName) + L" · " +
            std::to_wstring(activity.completedFrames) + L" / " +
            std::to_wstring(activity.frames.size()) + L" 帧";
    }
    for (const auto& job : gApp->Jobs()) {
        if (job.stage != JobStage::Testing && job.stage != JobStage::Rendering &&
            job.stage != JobStage::UploadFailed && job.stage != JobStage::TestFailed &&
            job.stage != JobStage::Failed)
            continue;
        const size_t completed = static_cast<size_t>(std::count_if(
            job.frames.begin(), job.frames.end(), [](const FrameTask& frame) {
                return frame.state == TaskState::Completed;
            }));
        if (job.stage == JobStage::UploadFailed || job.stage == JobStage::TestFailed ||
            job.stage == JobStage::Failed)
            return L"任务有错误：" + Utf8ToWide(job.name);
        return L"任务进度：" + Utf8ToWide(job.name) + L" · " +
            std::to_wstring(completed) + L" / " + std::to_wstring(job.frames.size()) + L" 帧";
    }
    const Config config = gApp->GetConfig();
    return config.shareEnabled ? L"已就绪 · 等待空闲任务" : L"已就绪 · 算力共享已关闭";
}

bool UpdateTrayIcon() {
    if (!gWindow || !IsWindow(gWindow)) return false;
    if (!gTrayIcons[0]) {
        gTrayIcons[0] = CreateBrandedTrayIcon(TrayVisualState::Idle);
        gTrayIcons[1] = CreateBrandedTrayIcon(TrayVisualState::Error);
        gTrayIcons[2] = CreateBrandedTrayIcon(TrayVisualState::Rendering);
    }
    const TrayVisualState state = CurrentTrayVisualState();
    const size_t iconIndex = state == TrayVisualState::Error ? 1
        : state == TrayVisualState::Rendering ? 2 : 0;
    const std::wstring tip = L"RenderMesh · " + CurrentTrayStatusText();
    if (!gTrayIconAdded) {
        gTrayIconData = {};
        gTrayIconData.cbSize = sizeof(gTrayIconData);
        gTrayIconData.hWnd = gWindow;
        gTrayIconData.uID = 1;
        gTrayIconData.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
        gTrayIconData.uCallbackMessage = WM_RM_TRAY;
    }
    gTrayIconData.hIcon = gTrayIcons[iconIndex];
    wcsncpy_s(gTrayIconData.szTip, tip.c_str(), _TRUNCATE);
    auto addIcon = [&] {
        gTrayIconAdded = Shell_NotifyIconW(NIM_ADD, &gTrayIconData) != FALSE;
        if (!gTrayIconAdded) return false;
        gTrayIconData.uVersion = NOTIFYICON_VERSION_4;
        Shell_NotifyIconW(NIM_SETVERSION, &gTrayIconData);
        return true;
    };
    if (!gTrayIconAdded) return addIcon();
    if (Shell_NotifyIconW(NIM_MODIFY, &gTrayIconData) != FALSE) return true;
    gTrayIconAdded = false;
    return addIcon();
}

void RemoveTrayIcon() {
    if (gTrayIconAdded) Shell_NotifyIconW(NIM_DELETE, &gTrayIconData);
    gTrayIconAdded = false;
    for (auto& icon : gTrayIcons) {
        if (icon) DestroyIcon(icon);
        icon = nullptr;
    }
}

void RestoreMainWindowFromTray() {
    if (!gWindow) return;
    ShowWindow(gWindow, SW_RESTORE);
    SetForegroundWindow(gWindow);
}

bool ToggleSharingFromTray(std::wstring& error) {
    if (!gApp) {
        error = L"RenderMesh 服务尚未就绪";
        return false;
    }
    Config config = gApp->GetConfig();
    config.shareEnabled = !config.shareEnabled;
    if (!gApp->UpdateConfig(config, error)) return false;
    SendMessageW(Item(ID_SHARE), BM_SETCHECK,
                 config.shareEnabled ? BST_CHECKED : BST_UNCHECKED, 0);
    InvalidateRect(Item(ID_SHARE), nullptr, FALSE);
    RefreshLists();
    return true;
}

void ShowTrayContextMenu() {
    if (!gWindow || !gApp) return;
    HMENU menu = CreatePopupMenu();
    std::wstring status = CurrentTrayStatusText();
    if (status.size() > 92) status.resize(92);
    AppendMenuW(menu, MF_STRING | MF_GRAYED, ID_TRAY_STATUS, status.c_str());
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    const bool sharing = gApp->GetConfig().shareEnabled;
    AppendMenuW(menu, MF_STRING | (sharing ? MF_CHECKED : 0),
                ID_TRAY_TOGGLE_SHARE,
                sharing ? L"关闭算力共享" : L"开启算力共享");
    AppendMenuW(menu, MF_STRING, ID_TRAY_EXIT, L"退出 RenderMesh");
    POINT point{};
    GetCursorPos(&point);
    SetForegroundWindow(gWindow);
    const UINT command = TrackPopupMenu(
        menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON,
        point.x, point.y, 0, gWindow, nullptr);
    DestroyMenu(menu);
    PostMessageW(gWindow, WM_NULL, 0, 0);
    if (command == ID_TRAY_TOGGLE_SHARE) {
        std::wstring error;
        if (!ToggleSharingFromTray(error))
            MessageBoxW(gWindow, error.c_str(), L"RenderMesh", MB_ICONWARNING);
    } else if (command == ID_TRAY_EXIT) {
        gForceExit = true;
        PostMessageW(gWindow, WM_CLOSE, 0, 0);
    }
}

enum class CloseRequestResult { Cancel, Minimize, Exit };

CloseRequestResult ResolveCloseRequest(HWND window) {
    if (gForceExit || !gUiMetricsReport.empty()) return CloseRequestResult::Exit;
    const Config config = gApp->GetConfig();
    if (config.closeBehavior == "exit") return CloseRequestResult::Exit;
    if (config.closeBehavior == "tray") return CloseRequestResult::Minimize;
    if (!gInternalDialog || !IsWindow(gInternalDialog) ||
        gInternalDialogMode != InternalDialogMode::CloseBehavior) {
        ShowInternalDialog(
            InternalDialogMode::CloseBehavior,
            L"关闭 RenderMesh",
            L"",
            L"直接退出会停止本机发现、同步和渲染服务。\n"
            L"最小化到系统托盘后，RenderMesh 会继续在后台运行。");
    }
    (void)window;
    return CloseRequestResult::Cancel;
}

void ScheduleSettingsSave() {
    if (gLoadingSettings || !gWindow) return;
    gSettingsDirty = true;
    SetStatusText(L"正在自动保存设置…");
    KillTimer(gWindow, TIMER_SETTINGS_AUTOSAVE);
    SetTimer(gWindow, TIMER_SETTINGS_AUTOSAVE, 300, nullptr);
}

void ConnectManualPeer() {
    Config config = ReadConfigFromUi();
    std::wstring result;
    if (!gApp->UpdateConfig(config, result)) {
        SetStatusText(L"连接设置无效：" + result);
        ShowInternalNotice(L"连接设置无效", result);
        return;
    }
    bool connected = gApp->AddManualPeer(
        WideToUtf8(Text(Item(ID_MANUAL_ENDPOINT))), result);
    SetStatusText(result);
    if (connected) {
        SetText(ID_MANUAL_ENDPOINT, Utf8ToWide(gApp->GetConfig().manualPeerEndpoint));
    } else {
        ShowInternalNotice(L"手动连接失败", result);
    }
}

ui::VisualState TaskVisualState(TaskState state) {
    switch (state) {
        case TaskState::Completed: return ui::VisualState::Success;
        case TaskState::Dispatching:
        case TaskState::Rendering: return ui::VisualState::Active;
        case TaskState::Assigned:
        case TaskState::Pending: return ui::VisualState::Warning;
        case TaskState::Failed: return ui::VisualState::Error;
        case TaskState::Stopped: return ui::VisualState::Muted;
    }
    return ui::VisualState::Neutral;
}

ui::VisualState JobStageVisualState(JobStage stage) {
    switch (stage) {
        case JobStage::ReadyToConfigure:
        case JobStage::TestCompleted:
        case JobStage::Completed: return ui::VisualState::Success;
        case JobStage::Uploading:
        case JobStage::Testing:
        case JobStage::Rendering: return ui::VisualState::Active;
        case JobStage::UploadFailed:
        case JobStage::TestFailed:
        case JobStage::Failed: return ui::VisualState::Error;
        case JobStage::Stopped: return ui::VisualState::Muted;
    }
    return ui::VisualState::Neutral;
}

std::wstring LocalComputeStatusText(bool rendererReady, bool shareEnabled,
                                    bool idleReady, int assigned, bool frameRendering) {
    if (!rendererReady) return L"未检测到 Blender · 不接单";
    if (!shareEnabled) return L"未共享";
    if (assigned > 0) return frameRendering ? L"正在渲染" : L"排队中";
    return idleReady ? L"可接单" : L"使用中";
}

std::wstring ReadyStatusText(bool pairingPending, bool rendererReady) {
    if (pairingPending) return L"正在邀请码配对";
    if (!rendererReady) return L"未检测到 Blender";
    return L"已就绪";
}

std::wstring DashboardStatusText(bool pairingPending, bool rendererReady,
                                 const std::wstring& manualPeerStatus) {
    if (manualPeerStatus.find(L"握手协议过旧") != std::wstring::npos)
        return manualPeerStatus;
    return ReadyStatusText(pairingPending, rendererReady);
}

std::wstring RemoteComputeStatusText(const Peer& peer) {
    if (!peer.rendererReady) return L"未检测到 Blender · 不接单";
    if (!peer.shareEnabled) return L"未共享";
    if (peer.assigned) return peer.activity.rendering ? L"正在渲染" : L"排队中";
    if (peer.idleReadyKnown) return peer.idleReady ? L"可接单" : L"使用中";
    return L"使用中";
}

void RefreshLists() {
    if (!gApp) return;
    UpdateTrayIcon();
    HWND modalLayer = nullptr;
    if (gWorkflowProgressOverlay && IsWindow(gWorkflowProgressOverlay))
        modalLayer = gWorkflowProgressOverlay;
    else if (gInternalDialog && IsWindow(gInternalDialog))
        modalLayer = gInternalDialog;
    if (modalLayer) {
        gMainRefreshDeferredByModal = true;
        SetWindowPos(modalLayer, HWND_TOP, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE |
                         SWP_SHOWWINDOW);
        return;
    }
    ++gRefreshCount;
    const Config config = gApp->GetConfig();

    if (gCurrentPage == 0) {
        const auto peers = gApp->Peers();
        const JobAggregate jobStats = gApp->JobStats();
        const HardwareInfo hardware = gApp->Hardware();
        const int idleSeconds = static_cast<int>(IdleSeconds());
        const int assigned = gApp->AssignedCount();
        const WorkerActivity localActivity = gApp->CurrentActivity();
        std::wstring manualStatus = gApp->ManualPeerStatus();

        std::vector<ui::Row> nodeRows;
        nodeRows.reserve(peers.size() + 1);
        std::wstring localGpus;
        for (const auto& gpu : hardware.gpuModels) {
            if (!localGpus.empty()) localGpus += L" / ";
            localGpus += Utf8ToWide(gpu);
        }
        const bool localIdle = IsComputerIdleForSharedWork(IdleSeconds(),
                                                            config.idleAfterSeconds);
        const bool localBlenderReady = !LocalRendererPath(config).empty();
        nodeRows.push_back({
            Utf8ToWide("local:" + config.nodeId),
            {
                Utf8ToWide(config.nodeName) + L"（本机）",
                L"已信任",
                LocalComputeStatusText(localBlenderReady, config.shareEnabled,
                                       localIdle, assigned, localActivity.rendering),
                WorkerActivityText(localActivity, assigned),
                Utf8ToWide(hardware.cpuModel),
                localGpus.empty() ? L"未检测到" : localGpus,
                LocalEndpointSummary(config.httpPort),
            },
            !localBlenderReady ? ui::VisualState::Error
                : config.shareEnabled
                ? (assigned > 0 ? (localActivity.rendering
                        ? ui::VisualState::Active : ui::VisualState::Warning)
                    : localIdle ? ui::VisualState::Success : ui::VisualState::Warning)
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
                    peer.trusted
                        ? L"已信任" + (peer.appVersion.empty() ? std::wstring{} :
                            L" · " + Utf8ToWide(peer.appVersion))
                        : L"邀请码不同",
                    RemoteComputeStatusText(peer),
                    WorkerActivityText(peer.activity, peer.assigned),
                    peer.cpuModel.empty() ? L"未报告" : Utf8ToWide(peer.cpuModel),
                    remoteGpus.empty() ? L"未报告" : remoteGpus,
                    Utf8ToWide(peer.address) + L":" + std::to_wstring(peer.httpPort),
                },
                !peer.trusted || !peer.rendererReady ? ui::VisualState::Error
                    : peer.assigned ? (peer.activity.rendering
                            ? ui::VisualState::Active : ui::VisualState::Warning)
                    : peer.shareEnabled
                        ? (peer.idleReadyKnown && peer.idleReady
                            ? ui::VisualState::Success
                            : ui::VisualState::Warning)
                    : ui::VisualState::Muted,
            });
        }
        SyncTable(Item(ID_NODES), gNodeRows, std::move(nodeRows));
        FitMainListColumns();

        DashboardView nextDashboard;
        nextDashboard.onlineNodes = static_cast<int>(peers.size()) + 1;
        nextDashboard.assignedFrames = assigned;
        nextDashboard.idleSeconds = idleSeconds;
        nextDashboard.rendererReady = !LocalRendererPath(config).empty();
        nextDashboard.matchId = Utf8ToWide(config.inviteCode);
        nextDashboard.totalFrames = jobStats.totalFrames;
        nextDashboard.completedFrames = jobStats.completedFrames;
        nextDashboard.activeJobs = jobStats.activeJobs;
        if (!(nextDashboard == gDashboardView)) {
            gDashboardView = nextDashboard;
            RECT client{};
            GetClientRect(gWindow, &client);
            const int chromeTop = ChromeHeight(gWindow);
            RECT cards{236, 86 + chromeTop, std::max(237L, client.right - 18),
                       176 + chromeTop};
            InvalidateRect(gWindow, &cards, FALSE);
        }
        SetStatusText(DashboardStatusText(config.pairingPending,
                                          nextDashboard.rendererReady,
                                          manualStatus));
    } else if (gCurrentPage == 1) {
        const uint64_t revision = gApp->JobRevision();
        if (revision == gLastJobRevision) {
            if (gDetailWindow) RefreshJobDetail();
            gLastRefresh = std::chrono::steady_clock::now();
            return;
        }
        gLastJobRevision = revision;
        auto values = gApp->Jobs();
        std::stable_sort(values.begin(), values.end(), [](const RenderJob& left,
                                                          const RenderJob& right) {
            if (left.createdAt != right.createdAt) return left.createdAt > right.createdAt;
            return left.id > right.id;
        });
        std::vector<ui::Row> jobRows;
        jobRows.reserve(values.size());
        const auto now = std::chrono::system_clock::now();
        for (const auto& job : values) {
            int completed = 0, active = 0;
            std::chrono::system_clock::time_point started{}, ended{};
            for (const auto& frame : job.frames) {
                completed += frame.state == TaskState::Completed;
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
            const std::wstring state = JobStageLabel(job.stage);
            jobRows.push_back({
                Utf8ToWide(job.id),
                {
                    ui::DisplayJobName({Utf8ToWide(job.id), Utf8ToWide(job.name),
                                        job.priority, job.createdAt, job.finishedAt}, now),
                    job.priority > 0 ? L"优先 · " + state : state,
                    std::to_wstring(job.frames.size()),
                    std::to_wstring(completed) + L" / " +
                        std::to_wstring(job.frames.size()),
                    FormatDuration(started,
                        active ? std::chrono::system_clock::time_point{} : ended),
                    FormatTime(started), active ? L"-" : FormatTime(ended),
                },
                JobStageVisualState(job.stage),
            });
        }
        SyncTable(Item(ID_JOBS), gJobRows, std::move(jobRows));
        FitMainListColumns();
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
        FitMainListColumns();
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

// Imports a project in one background pass: the render adapter opens the
// project once to read its settings, check external files and (only if
// needed) write a packed copy; the job snapshot is then written and hashed in
// a single stream. The work folder is always removed afterwards.
void StartImport(const fs::path& source, bool ignoreAssetWarnings = false) {
    if (gPreflightRunning.exchange(true)) {
        SetStatusText(L"已有工程正在读取，请稍候。");
        return;
    }
    const RenderAdapter* adapter = RenderAdapterForProject(source);
    std::string name = WideToUtf8(Text(Item(ID_JOB_NAME)));
    if (!adapter || !fs::is_regular_file(source) || name.empty()) {
        gPreflightRunning = false;
        gNewJobFlow = false;
        CloseWorkflowProgress();
        ShowInternalNotice(L"无法提交", L"请选择受支持的工程文件，并检查任务名称。");
        return;
    }
    ShowWorkflowProgress(WorkflowProgressStage::Inspecting);
    EnableWindow(Item(ID_SUBMIT), FALSE);
    SetStatusText(ignoreAssetWarnings
        ? L"正在按你的选择跳过全部预检警告并准备工程……"
        : L"正在读取工程设置并检查外部素材……");
    if (gPreflight.joinable()) gPreflight.join();
    gPreflightStop = false;
    gImportPermille = 0;
    const Config config = gApp->GetConfig();
    const fs::path dataDir = gApp->DataDir();
    const fs::path work = gApp->ProjectStorageDir() / L"preflight" /
        Utf8ToWide(NewRandomHex(6));
    gPreflight = std::thread([=, name = std::move(name)] {
        PreflightReport report = adapter->Prepare(
            config, dataDir, source, work, ignoreAssetWarnings, &gPreflightStop);
        auto result = std::make_unique<PreflightUiResult>();
        result->message = report.summary;
        result->canIgnoreAssetWarnings = report.canIgnoreAssetWarnings;
        result->frameStart = report.frameStart;
        result->frameEnd = report.frameEnd;
        result->frameStep = report.frameStep;
        result->cancelled = gPreflightStop.load();
        if (report.passed && !result->cancelled) {
            PostMessageW(gWindow, WM_RM_WORKFLOW_PROGRESS,
                         static_cast<WPARAM>(WorkflowProgressStage::CreatingTask), 0);
            std::wstring error;
            result->jobId = gApp->CreatePreparedDraft(
                name, report, error, &gPreflightStop, [](uint64_t done, uint64_t total) {
                    gImportPermille = total ? static_cast<int>(done * 1000 / total) : 0;
                });
            result->ok = !result->jobId.empty();
            result->cancelled = !result->ok && gPreflightStop.load();
            if (!result->ok)
                result->message = L"工程检查通过，但任务创建失败：" + error;
            else
                result->message += L"\n任务已创建；有可用节点时自动分发，否则由本机渲染。";
            if (result->ok && report.assetWarningsIgnored)
                LogWarning("user ignored all preflight warnings; source=" +
                           WideToUtf8(source.wstring()) + "; missing=" +
                           std::to_string(report.missing.size()) + "; unpacked=" +
                           std::to_string(report.unpacked.size()));
        }
        std::error_code ignored;
        fs::remove_all(work, ignored);
        gPreflightRunning = false;
        if (PostMessageW(gWindow, WM_RM_PREFLIGHT, result->ok ? 1 : 0,
                         reinterpret_cast<LPARAM>(result.get())))
            result.release();
    });
}

void BeginNewJobFlow(const fs::path& source) {
    if (source.empty() || !fs::is_regular_file(source) || !RenderAdapterForProject(source)) {
        ShowInternalNotice(L"无法读取工程", L"请拖入或选择一个受支持的工程文件（.blend）。");
        return;
    }
    if (gPreflightRunning.load()) {
        SetStatusText(L"已有工程正在读取或预检，请稍候。");
        return;
    }
    gNewJobFlow = true;
    SetText(ID_SOURCE_PATH, source.wstring());
    SetText(ID_JOB_NAME, source.stem().wstring());
    StartImport(source);
}

bool IsBlendDropPath(const fs::path& path) {
    return !path.empty() && RenderAdapterForProject(path) != nullptr;
}

// "Blender 工程\0*.blend\0" for every adapter, for the open-file dialog.
std::wstring ProjectFileFilter() {
    std::wstring filter;
    for (const RenderAdapter* adapter : RenderAdapters()) {
        filter += std::wstring(adapter->DisplayName()) + L" 工程";
        filter.push_back(L'\0');
        filter += L"*" + std::wstring(adapter->ProjectExtension());
        filter.push_back(L'\0');
    }
    filter.push_back(L'\0');
    return filter;
}

void SetDropOverlayVisible(bool visible) {
    visible = visible && gCurrentPage == 1;
    if (gDropOverlayVisible == visible) return;
    gDropOverlayVisible = visible;
    if (HWND overlay = Item(ID_DROP_OVERLAY)) {
        ShowWindow(overlay, visible ? SW_SHOW : SW_HIDE);
        if (visible) {
            SetWindowPos(overlay, HWND_TOP, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
            InvalidateRect(overlay, nullptr, TRUE);
        }
    }
}

bool PointIsOverTaskDropArea(POINTL screenPoint) {
    if (gCurrentPage != 1) return false;
    HWND jobs = Item(ID_JOBS);
    if (!jobs || !IsWindowVisible(jobs)) return false;
    RECT bounds{};
    GetWindowRect(jobs, &bounds);
    POINT point{screenPoint.x, screenPoint.y};
    return PtInRect(&bounds, point) != FALSE;
}

bool FirstBlendPath(IDataObject* dataObject, std::wstring& path) {
    if (!dataObject) return false;
    FORMATETC format{CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
    STGMEDIUM medium{};
    if (FAILED(dataObject->GetData(&format, &medium))) return false;
    HDROP drop = reinterpret_cast<HDROP>(medium.hGlobal);
    bool found = false;
    if (drop) {
        const UINT count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
        for (UINT index = 0; index < count && !found; ++index) {
            const UINT length = DragQueryFileW(drop, index, nullptr, 0);
            std::wstring candidate(static_cast<size_t>(length) + 1, L'\0');
            DragQueryFileW(drop, index, candidate.data(), length + 1);
            candidate.resize(length);
            if (IsBlendDropPath(candidate)) {
                path = std::move(candidate);
                found = true;
            }
        }
    }
    ReleaseStgMedium(&medium);
    return found;
}

class MainDropTarget final : public IDropTarget {
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** value) override {
        if (!value) return E_POINTER;
        if (iid == __uuidof(IUnknown) || iid == __uuidof(IDropTarget)) {
            *value = static_cast<IDropTarget*>(this);
            AddRef();
            return S_OK;
        }
        *value = nullptr;
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override {
        return static_cast<ULONG>(InterlockedIncrement(&references_));
    }

    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG remaining = static_cast<ULONG>(InterlockedDecrement(&references_));
        if (!remaining) delete this;
        return remaining;
    }

    HRESULT STDMETHODCALLTYPE DragEnter(IDataObject* dataObject, DWORD, POINTL point,
                                        DWORD* effect) override {
        std::wstring path;
        acceptsBlend_ = FirstBlendPath(dataObject, path);
        return Update(point, effect);
    }

    HRESULT STDMETHODCALLTYPE DragOver(DWORD, POINTL point, DWORD* effect) override {
        return Update(point, effect);
    }

    HRESULT STDMETHODCALLTYPE DragLeave() override {
        acceptsBlend_ = false;
        SetDropOverlayVisible(false);
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE Drop(IDataObject* dataObject, DWORD, POINTL point,
                                   DWORD* effect) override {
        std::wstring path;
        const bool accepted = PointIsOverTaskDropArea(point) &&
            FirstBlendPath(dataObject, path);
        SetDropOverlayVisible(false);
        acceptsBlend_ = false;
        if (effect) *effect = accepted ? DROPEFFECT_COPY : DROPEFFECT_NONE;
        if (accepted) {
            auto* pending = new std::wstring(std::move(path));
            if (!PostMessageW(gWindow, WM_RM_BLEND_DROP, 0,
                              reinterpret_cast<LPARAM>(pending)))
                delete pending;
        }
        return S_OK;
    }

private:
    HRESULT Update(POINTL point, DWORD* effect) {
        const bool accepted = acceptsBlend_ && PointIsOverTaskDropArea(point);
        SetDropOverlayVisible(accepted);
        if (effect) *effect = accepted ? DROPEFFECT_COPY : DROPEFFECT_NONE;
        return S_OK;
    }

    LONG references_ = 1;
    bool acceptsBlend_ = false;
};

std::wstring TransferStateLabel(ProjectTransferState state) {
    switch (state) {
        case ProjectTransferState::Pending: return L"等待上传";
        case ProjectTransferState::Checking: return L"检查缓存";
        case ProjectTransferState::Uploading: return L"上传中";
        case ProjectTransferState::Cached: return L"已存在";
        case ProjectTransferState::Completed: return L"上传完成";
        case ProjectTransferState::Failed: return L"上传失败";
        case ProjectTransferState::Offline: return L"已离线";
        case ProjectTransferState::Excluded: return L"已移出";
    }
    return L"-";
}

std::wstring FormatTransferBytes(uint64_t value) {
    const wchar_t* units[] = {L"B", L"KB", L"MB", L"GB", L"TB"};
    double number = static_cast<double>(value);
    size_t unit = 0;
    while (number >= 1024.0 && unit + 1 < std::size(units)) {
        number /= 1024.0;
        ++unit;
    }
    wchar_t text[64]{};
    swprintf_s(text, unit == 0 ? L"%.0f %s" : L"%.1f %s", number, units[unit]);
    return text;
}

HWND AddDialogControl(HWND parent, const wchar_t* type, const wchar_t* textValue,
                      DWORD style, int x, int y, int width, int height, int id,
                      HFONT font = nullptr) {
    HWND control = CreateWindowExW(
        wcscmp(type, WC_LISTVIEWW) == 0 ? WS_EX_CLIENTEDGE : 0,
        type, textValue, WS_CHILD | WS_VISIBLE | style, x, y, width, height,
        parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
        GetModuleHandleW(nullptr), nullptr);
    SendMessageW(control, WM_SETFONT,
                 reinterpret_cast<WPARAM>(font ? font : gFont), TRUE);
    if (wcscmp(type, L"BUTTON") == 0 && (style & BS_OWNERDRAW) == BS_OWNERDRAW)
        ConfigureOwnerButton(control);
    if (wcscmp(type, L"EDIT") == 0 || wcscmp(type, L"COMBOBOX") == 0) {
        SetPropW(control, kFieldSelfFrameProperty, reinterpret_cast<HANDLE>(1));
        ConfigureModernField(control, type);
    }
    return control;
}

void ApplyWorkflowTheme(HWND window, HWND list) {
    BOOL dark = gDarkTheme;
    if (HMODULE dwm = LoadLibraryW(L"dwmapi.dll")) {
        using SetAttribute = HRESULT(WINAPI*)(HWND, DWORD, LPCVOID, DWORD);
        if (auto setAttribute = reinterpret_cast<SetAttribute>(
                GetProcAddress(dwm, "DwmSetWindowAttribute")))
            setAttribute(window, 20, &dark, sizeof(dark));
        FreeLibrary(dwm);
    }
    if (list) {
        ApplyListTheme(list);
    }
    InvalidateRect(window, nullptr, TRUE);
}

void FitUploadListColumns() {
    if (!gUploadWindow) return;
    FitListColumns(GetDlgItem(gUploadWindow, ID_UPLOAD_LIST),
                   {15, 20, 13, 20, 10, 22},
                   {96, 126, 76, 116, 66, 136});
}

std::set<std::string> SelectedUploadNodeIds() {
    std::set<std::string> selected;
    if (!gUploadWindow) return selected;
    HWND list = GetDlgItem(gUploadWindow, ID_UPLOAD_LIST);
    for (int index = ListView_GetNextItem(list, -1, LVNI_SELECTED);
         index >= 0;
         index = ListView_GetNextItem(list, index, LVNI_SELECTED)) {
        if (index >= static_cast<int>(gUploadRows.Rows().size())) continue;
        const std::string nodeId = WideToUtf8(
            gUploadRows.Rows()[static_cast<size_t>(index)].key);
        if (gUploadSelectableNodeIds.contains(nodeId)) selected.insert(nodeId);
    }
    return selected;
}

void ApplyUploadRowSelection(
    HWND list, const ui::StableTableModel& model,
    const std::set<std::string>& selectableNodeIds, bool invert) {
    if (!list) return;
    for (int index = 0; index < static_cast<int>(model.Rows().size()); ++index) {
        const std::string nodeId = WideToUtf8(
            model.Rows()[static_cast<size_t>(index)].key);
        const bool selectable = selectableNodeIds.contains(nodeId);
        const bool wasSelected =
            (ListView_GetItemState(list, index, LVIS_SELECTED) & LVIS_SELECTED) != 0;
        const bool selected = selectable && (invert ? !wasSelected : true);
        ListView_SetItemState(list, index, selected ? LVIS_SELECTED : 0, LVIS_SELECTED);
    }
}

void SelectUploadRows(bool invert) {
    if (!gUploadWindow) return;
    HWND list = GetDlgItem(gUploadWindow, ID_UPLOAD_LIST);
    gUploadSyncingChecks = true;
    ApplyUploadRowSelection(list, gUploadRows, gUploadSelectableNodeIds, invert);
    gUploadSyncingChecks = false;
}

void UpdateUploadActionButtons() {
    if (!gUploadWindow) return;
    const auto selected = SelectedUploadNodeIds();
    const auto job = FindJob(gUploadJobId);
    bool canAdd = false;
    bool canDisconnect = false;
    bool canRetry = false;
    if (job) {
        for (const auto& nodeId : selected) {
            const auto transfer = std::find_if(
                job->projectTransfers.begin(), job->projectTransfers.end(),
                [&](const ProjectTransfer& value) { return value.nodeId == nodeId; });
            const bool participant = transfer != job->projectTransfers.end() &&
                transfer->state != ProjectTransferState::Excluded;
            canAdd = canAdd || !participant;
            canDisconnect = canDisconnect || participant;
            canRetry = canRetry || (transfer != job->projectTransfers.end() &&
                (transfer->state == ProjectTransferState::Failed ||
                 transfer->state == ProjectTransferState::Offline));
        }
    }
    EnableWindow(GetDlgItem(gUploadWindow, ID_UPLOAD_ADD), canAdd);
    EnableWindow(GetDlgItem(gUploadWindow, ID_UPLOAD_DISCONNECT), canDisconnect);
    EnableWindow(GetDlgItem(gUploadWindow, ID_UPLOAD_RETRY), canRetry);
}

void RefreshUploadWindow() {
    if (!gUploadWindow || gUploadJobId.empty()) return;
    auto job = FindJob(gUploadJobId);
    if (!job) {
        DestroyWindow(gUploadWindow);
        return;
    }
    size_t ready = 0, included = 0;
    std::vector<ui::Row> rows;
    const auto peers = gApp->Peers();
    rows.reserve(job->projectTransfers.size() + peers.size());
    std::set<std::string> knownNodes;
    gUploadSelectableNodeIds.clear();
    for (const auto& transfer : job->projectTransfers) {
        knownNodes.insert(transfer.nodeId);
        gUploadSelectableNodeIds.insert(transfer.nodeId);
        const bool excluded = transfer.state == ProjectTransferState::Excluded;
        if (!excluded) ++included;
        if (!excluded && (transfer.state == ProjectTransferState::Completed ||
                          transfer.state == ProjectTransferState::Cached))
            ++ready;
        const uint64_t total = transfer.totalBytes;
        const int percent = total == 0 ? 0 : static_cast<int>(
            std::min<uint64_t>(100, transfer.transferredBytes * 100 / total));
        std::wstring hardware = Utf8ToWide(transfer.cpuModel);
        if (!transfer.gpuModels.empty()) {
            if (!hardware.empty()) hardware += L" / ";
            hardware += Utf8ToWide(transfer.gpuModels.front());
        }
        std::wstring progress = excluded ? L"-" : std::to_wstring(percent) + L"%  " +
            FormatTransferBytes(transfer.transferredBytes) + L" / " +
            FormatTransferBytes(total);
        rows.push_back({Utf8ToWide(transfer.nodeId), {
            Utf8ToWide(transfer.nodeName), hardware.empty() ? L"未报告" : hardware,
            TransferStateLabel(transfer.state), progress,
            transfer.bytesPerSecond > 0
                ? FormatTransferBytes(static_cast<uint64_t>(transfer.bytesPerSecond)) + L"/s"
                : L"-",
            Utf8ToWide(transfer.error)},
            transfer.state == ProjectTransferState::Failed ||
                    transfer.state == ProjectTransferState::Offline
                ? ui::VisualState::Error
                : transfer.state == ProjectTransferState::Completed ||
                          transfer.state == ProjectTransferState::Cached
                    ? ui::VisualState::Success
                    : transfer.state == ProjectTransferState::Excluded
                        ? ui::VisualState::Muted : ui::VisualState::Active});
    }
    const std::string localNodeId = gApp->GetConfig().nodeId;
    for (const auto& peer : peers) {
        if (peer.nodeId == localNodeId || knownNodes.contains(peer.nodeId) || !peer.trusted)
            continue;
        const bool bundleCapable =
            !job->projectBundle || peer.supportsResumableProjectUpload;
        const bool eligible = peer.shareEnabled && peer.supportsProjectCache &&
            peer.rendererReady && bundleCapable;
        if (eligible) gUploadSelectableNodeIds.insert(peer.nodeId);
        std::wstring hardware = Utf8ToWide(peer.cpuModel);
        if (!peer.gpuModels.empty()) {
            if (!hardware.empty()) hardware += L" / ";
            hardware += Utf8ToWide(peer.gpuModels.front());
        }
        std::wstring detail;
        if (!peer.shareEnabled) detail = L"该电脑未开启算力共享";
        else if (!peer.rendererReady) detail = L"该电脑未检测到可用的 Blender";
        else if (!peer.supportsProjectCache) detail = L"该电脑版本过旧，不支持工程缓存";
        else if (!bundleCapable)
            detail = L"该工程包含 tex 文件夹；请先把该电脑更新到 RenderMesh 0.2.29 或更高版本";
        else detail = L"新加入的在线电脑；勾选并应用后会在后台同步工程";
        rows.push_back({Utf8ToWide(peer.nodeId), {
            Utf8ToWide(peer.name), hardware.empty() ? L"未报告" : hardware,
            eligible ? L"可加入" : L"不可用", L"-", L"-", detail},
            eligible ? ui::VisualState::Success : ui::VisualState::Muted});
    }
    gUploadSyncingChecks = true;
    SyncTable(GetDlgItem(gUploadWindow, ID_UPLOAD_LIST), gUploadRows, std::move(rows));
    gUploadSyncingChecks = false;
    const std::wstring summary = Utf8ToWide(job->name) + L"  ·  " +
        JobStageLabel(job->stage) + L"  ·  参与 " + std::to_wstring(included) +
        L" 台，其中 " + std::to_wstring(ready) + L" 台已同步";
    SetWindowTextIfChanged(GetDlgItem(gUploadWindow, ID_UPLOAD_SUMMARY), summary);
    const bool longWait = !gUploadEditMode && included > ready &&
        job->stage == JobStage::Uploading &&
        job->createdAt.time_since_epoch() !=
            std::chrono::system_clock::duration::zero() &&
        std::chrono::system_clock::now() - job->createdAt >= std::chrono::minutes(1);
    SetWindowTextIfChanged(GetDlgItem(gUploadWindow, ID_UPLOAD_HINT), longWait
        ? L"检测到超过 1 分钟仍未同步完成。可继续等待，或主动取消整个上传任务。"
        : L"选中电脑后使用“加入”或“断开”；支持 Ctrl/Shift 多选、全选、反选和右键操作。断开后不再接收新任务包。");
    HWND cancelUpload = GetDlgItem(gUploadWindow, ID_UPLOAD_CANCEL);
    ShowWindow(cancelUpload, longWait ? SW_SHOW : SW_HIDE);
    if (longWait) {
        SetWindowTextIfChanged(cancelUpload,
            gUploadCancelRunning.load() ? L"正在取消…" : L"取消上传");
        EnableWindow(cancelUpload, !gUploadCancelRunning.load());
    }
    EnableWindow(GetDlgItem(gUploadWindow, ID_UPLOAD_NEXT),
                 !gUploadEditMode && job->stage == JobStage::ReadyToConfigure);
    LayoutUploadWindow(gUploadWindow);
    FitUploadListColumns();
    UpdateUploadActionButtons();
}

void ApplyUploadHostAction(UINT command) {
    if (!gUploadWindow || gUploadJobId.empty()) return;
    const auto selected = SelectedUploadNodeIds();
    if (selected.empty()) {
        MessageBoxW(gUploadWindow, L"请先选中一台或多台电脑。", L"RenderMesh",
                    MB_ICONINFORMATION);
        return;
    }
    auto job = FindJob(gUploadJobId);
    if (!job) return;
    std::wstring error;
    if (command == ID_UPLOAD_RETRY) {
        size_t retried = 0;
        for (const auto& nodeId : selected) {
            const auto transfer = std::find_if(
                job->projectTransfers.begin(), job->projectTransfers.end(),
                [&](const ProjectTransfer& value) { return value.nodeId == nodeId; });
            if (transfer == job->projectTransfers.end() ||
                (transfer->state != ProjectTransferState::Failed &&
                 transfer->state != ProjectTransferState::Offline))
                continue;
            std::wstring retryError;
            if (gApp->RetryProjectTransfer(gUploadJobId, nodeId, retryError)) ++retried;
            else error = retryError;
        }
        if (!retried && !error.empty())
            MessageBoxW(gUploadWindow, error.c_str(), L"RenderMesh", MB_ICONWARNING);
        else if (retried)
            SetStatusText(L"已让 " + std::to_wstring(retried) +
                          L" 台电脑继续同步工程。");
        RefreshUploadWindow();
        return;
    }
    std::set<std::string> participants;
    for (const auto& transfer : job->projectTransfers)
        if (transfer.state != ProjectTransferState::Excluded)
            participants.insert(transfer.nodeId);
    const size_t before = participants.size();
    if (command == ID_UPLOAD_ADD)
        participants.insert(selected.begin(), selected.end());
    else if (command == ID_UPLOAD_DISCONNECT)
        for (const auto& nodeId : selected) participants.erase(nodeId);
    else
        return;
    if (participants.size() == before &&
        ((command == ID_UPLOAD_ADD &&
          std::all_of(selected.begin(), selected.end(), [&](const std::string& value) {
              return participants.contains(value);
          })) ||
         (command == ID_UPLOAD_DISCONNECT &&
          std::none_of(selected.begin(), selected.end(), [&](const std::string& value) {
              return participants.contains(value);
          })))) {
        UpdateUploadActionButtons();
        return;
    }
    const std::vector<std::string> next(participants.begin(), participants.end());
    if (!gApp->SetJobParticipants(gUploadJobId, next, error)) {
        MessageBoxW(gUploadWindow, error.c_str(), L"RenderMesh", MB_ICONWARNING);
    } else if (command == ID_UPLOAD_ADD) {
        SetStatusText(L"已加入所选电脑；工程同步完成后会自动参与渲染。");
    } else {
        SetStatusText(L"已断开所选电脑；不会再向它们分配新的任务包。");
    }
    RefreshUploadWindow();
}

void ShowUploadHostContextMenu(HWND window, POINT screenPoint) {
    const auto selected = SelectedUploadNodeIds();
    if (selected.empty()) return;
    const auto job = FindJob(gUploadJobId);
    if (!job) return;
    bool canAdd = false;
    bool canDisconnect = false;
    bool canRetry = false;
    for (const auto& nodeId : selected) {
        const auto transfer = std::find_if(
            job->projectTransfers.begin(), job->projectTransfers.end(),
            [&](const ProjectTransfer& value) { return value.nodeId == nodeId; });
        const bool participant = transfer != job->projectTransfers.end() &&
            transfer->state != ProjectTransferState::Excluded;
        canAdd = canAdd || !participant;
        canDisconnect = canDisconnect || participant;
        canRetry = canRetry || (transfer != job->projectTransfers.end() &&
            (transfer->state == ProjectTransferState::Failed ||
             transfer->state == ProjectTransferState::Offline));
    }
    static const MenuItemData add{L"加入所选电脑"};
    static const MenuItemData disconnect{L"断开所选电脑", true};
    static const MenuItemData retry{L"重试同步"};
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_OWNERDRAW | (canAdd ? 0 : MF_GRAYED), ID_UPLOAD_ADD,
                reinterpret_cast<LPCWSTR>(&add));
    AppendMenuW(menu, MF_OWNERDRAW | (canDisconnect ? 0 : MF_GRAYED),
                ID_UPLOAD_DISCONNECT, reinterpret_cast<LPCWSTR>(&disconnect));
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_OWNERDRAW | (canRetry ? 0 : MF_GRAYED), ID_UPLOAD_RETRY,
                reinterpret_cast<LPCWSTR>(&retry));
    MENUINFO info{};
    info.cbSize = sizeof(info);
    info.fMask = MIM_BACKGROUND | MIM_STYLE;
    info.hbrBack = gSurfaceBrush;
    info.dwStyle = MNS_CHECKORBMP;
    SetMenuInfo(menu, &info);
    const UINT command = TrackAndCloseContextMenu(
        window, menu, screenPoint.x, screenPoint.y);
    if (command)
        DeferMenuAction([command] { ApplyUploadHostAction(command); });
}

void LayoutUploadWindow(HWND window) {
    RECT client{};
    GetClientRect(window, &client);
    constexpr int margin = 24;
    const int width = std::max(200, static_cast<int>(client.right) - margin * 2);
    MoveWindow(GetDlgItem(window, ID_UPLOAD_TITLE), margin, 18, width, 34, TRUE);
    MoveWindow(GetDlgItem(window, ID_UPLOAD_SUMMARY), margin, 58, width, 26, TRUE);
    MoveWindow(GetDlgItem(window, ID_UPLOAD_HINT), margin, 88, width, 24, TRUE);
    const bool cancelVisible =
        IsWindowVisible(GetDlgItem(window, ID_UPLOAD_CANCEL)) != FALSE;
    if (cancelVisible) {
        MoveWindow(GetDlgItem(window, ID_UPLOAD_HINT), margin, 88,
                   std::max(120, width - 146), 36, TRUE);
        MoveWindow(GetDlgItem(window, ID_UPLOAD_CANCEL),
                   client.right - margin - 132, 84, 132, 36, TRUE);
    }
    const int buttonTop = std::max(400, static_cast<int>(client.bottom) - 56);
    MoveWindow(GetDlgItem(window, ID_UPLOAD_LIST), margin, 126, width,
               std::max(180, buttonTop - 144), TRUE);
    int x = margin;
    auto place = [&](int id, int controlWidth) {
        MoveWindow(GetDlgItem(window, id), x, buttonTop, controlWidth, 36, TRUE);
        x += controlWidth + 10;
    };
    place(ID_UPLOAD_SELECT_ALL, 74);
    place(ID_UPLOAD_INVERT_SELECTION, 74);
    x += 8;
    place(ID_UPLOAD_ADD, 86);
    place(ID_UPLOAD_DISCONNECT, 86);
    place(ID_UPLOAD_RETRY, 112);
    const bool hasNext = IsWindowVisible(GetDlgItem(window, ID_UPLOAD_NEXT)) != FALSE;
    if (hasNext) {
        MoveWindow(GetDlgItem(window, ID_UPLOAD_NEXT), client.right - margin - 126,
                   buttonTop, 126, 36, TRUE);
        MoveWindow(GetDlgItem(window, ID_UPLOAD_CLOSE),
                   client.right - margin - 126 - 10 - 104,
                   buttonTop, 104, 36, TRUE);
    } else {
        MoveWindow(GetDlgItem(window, ID_UPLOAD_CLOSE), client.right - margin - 104,
                   buttonTop, 104, 36, TRUE);
    }
    FitUploadListColumns();
}

LRESULT CALLBACK UploadWindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
        case WM_CREATE: {
            AddDialogControl(window, L"STATIC",
                             gUploadEditMode ? L"编辑渲染主机" : L"工程同步", SS_LEFT,
                             22, 18, 260, 34, ID_UPLOAD_TITLE, gTitleFont);
            AddDialogControl(window, L"STATIC", L"", SS_LEFT,
                             24, 58, 840, 26, ID_UPLOAD_SUMMARY);
            AddDialogControl(window, L"STATIC",
                L"选中电脑后使用“加入”或“断开”；支持 Ctrl/Shift 多选、全选、反选和右键操作。断开后不再接收新任务包。",
                SS_LEFT, 24, 88, 840, 24, ID_UPLOAD_HINT);
            HWND list = AddDialogControl(window, WC_LISTVIEWW, L"",
                LVS_REPORT | LVS_SHOWSELALWAYS | WS_CLIPSIBLINGS,
                24, 126, 850, 330, ID_UPLOAD_LIST);
            ConfigureList(list);
            AddColumn(list, 0, 145, L"电脑"); AddColumn(list, 1, 215, L"硬件");
            AddColumn(list, 2, 105, L"状态"); AddColumn(list, 3, 205, L"进度");
            AddColumn(list, 4, 90, L"速度"); AddColumn(list, 5, 260, L"详情");
            AddDialogControl(window, L"BUTTON", L"全选", BS_OWNERDRAW,
                             24, 478, 86, 36, ID_UPLOAD_SELECT_ALL);
            AddDialogControl(window, L"BUTTON", L"反选", BS_OWNERDRAW,
                             120, 478, 86, 36, ID_UPLOAD_INVERT_SELECTION);
            AddDialogControl(window, L"BUTTON", L"加入", BS_OWNERDRAW,
                             216, 478, 86, 36, ID_UPLOAD_ADD);
            AddDialogControl(window, L"BUTTON", L"断开", BS_OWNERDRAW,
                             312, 478, 86, 36, ID_UPLOAD_DISCONNECT);
            AddDialogControl(window, L"BUTTON", L"重试同步", BS_OWNERDRAW,
                             408, 478, 112, 36, ID_UPLOAD_RETRY);
            HWND cancelUpload = AddDialogControl(
                window, L"BUTTON", L"取消上传", BS_OWNERDRAW,
                742, 84, 132, 36, ID_UPLOAD_CANCEL);
            ShowWindow(cancelUpload, SW_HIDE);
            AddDialogControl(window, L"BUTTON", L"关闭", BS_OWNERDRAW,
                             616, 478, 110, 36, ID_UPLOAD_CLOSE);
            AddDialogControl(window, L"BUTTON", L"下一步", BS_OWNERDRAW,
                             738, 478, 136, 36, ID_UPLOAD_NEXT);
            ShowWindow(GetDlgItem(window, ID_UPLOAD_NEXT),
                       gUploadEditMode ? SW_HIDE : SW_SHOW);
            SetTimer(window, TIMER_UPLOAD_PROGRESS, 500, nullptr);
            ApplyWorkflowTheme(window, list);
            LayoutUploadWindow(window);
            RefreshUploadWindow();
            return 0;
        }
        case WM_SIZE:
            if (wParam != SIZE_MINIMIZED) LayoutUploadWindow(window);
            return 0;
        case WM_GETMINMAXINFO: {
            auto* limits = reinterpret_cast<MINMAXINFO*>(lParam);
            ApplyMonitorWorkArea(window, limits);
            limits->ptMinTrackSize = {DpiValue(window, 860), DpiValue(window, 520)};
            return 0;
        }
        case WM_TIMER:
            if (wParam == TIMER_UPLOAD_PROGRESS) RefreshUploadWindow();
            return 0;
        case WM_COMMAND: {
            const UINT id = LOWORD(wParam);
            if (id == ID_UPLOAD_CLOSE) {
                DestroyWindow(window);
                return 0;
            }
            if (id == ID_UPLOAD_NEXT) {
                const std::string jobId = gUploadJobId;
                DestroyWindow(window);
                ShowConfigWindow(jobId);
                return 0;
            }
            if (id == ID_UPLOAD_CANCEL) {
                StartCancelUploadJob();
                return 0;
            }
            if (id == ID_UPLOAD_SELECT_ALL || id == ID_UPLOAD_INVERT_SELECTION) {
                SelectUploadRows(id == ID_UPLOAD_INVERT_SELECTION);
                UpdateUploadActionButtons();
                return 0;
            }
            if (id == ID_UPLOAD_ADD || id == ID_UPLOAD_DISCONNECT ||
                id == ID_UPLOAD_RETRY) {
                ApplyUploadHostAction(id);
                return 0;
            }
            return 0;
        }
        case WM_NOTIFY: {
            auto* header = reinterpret_cast<NMHDR*>(lParam);
            if (header->code == NM_CUSTOMDRAW)
                return HandleListCustomDraw(reinterpret_cast<NMLVCUSTOMDRAW*>(lParam));
            if (header->idFrom == ID_UPLOAD_LIST &&
                header->code == LVN_ITEMCHANGED && !gUploadSyncingChecks) {
                UpdateUploadActionButtons();
                return 0;
            }
            if (header->idFrom == ID_UPLOAD_LIST && header->code == NM_RCLICK) {
                auto* click = reinterpret_cast<NMITEMACTIVATE*>(lParam);
                HWND list = GetDlgItem(window, ID_UPLOAD_LIST);
                if (click->iItem >= 0 &&
                    !(ListView_GetItemState(list, click->iItem, LVIS_SELECTED) &
                      LVIS_SELECTED)) {
                    ListView_SetItemState(list, -1, 0, LVIS_SELECTED | LVIS_FOCUSED);
                    ListView_SetItemState(list, click->iItem,
                        LVIS_SELECTED | LVIS_FOCUSED,
                        LVIS_SELECTED | LVIS_FOCUSED);
                }
                POINT point{};
                GetCursorPos(&point);
                ShowUploadHostContextMenu(window, point);
                return 0;
            }
            return 0;
        }
        case WM_DRAWITEM: {
            const auto& item = *reinterpret_cast<DRAWITEMSTRUCT*>(lParam);
            if (item.CtlType == ODT_MENU) DrawContextMenuItem(item);
            else DrawOwnerButton(item);
            return TRUE;
        }
        case WM_MEASUREITEM:
            if (reinterpret_cast<MEASUREITEMSTRUCT*>(lParam)->CtlType == ODT_MENU) {
                MeasureContextMenuItem(*reinterpret_cast<MEASUREITEMSTRUCT*>(lParam));
                return TRUE;
            }
            break;
        case WM_CTLCOLORSTATIC: {
            HDC dc = reinterpret_cast<HDC>(wParam);
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, gDarkTheme ? RGB(239, 239, 247) : RGB(31, 31, 38));
            return reinterpret_cast<LRESULT>(gBackgroundBrush);
        }
        case WM_ERASEBKGND: {
            RECT client{}; GetClientRect(window, &client);
            FillRect(reinterpret_cast<HDC>(wParam), &client, gBackgroundBrush);
            return TRUE;
        }
        case WM_DESTROY:
            KillTimer(window, TIMER_UPLOAD_PROGRESS);
            gUploadRows = {};
            gUploadWindow = nullptr;
            gUploadJobId.clear();
            gUploadEditMode = false;
            gUploadSyncingChecks = false;
            gUploadSelectableNodeIds.clear();
            return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

int DialogInt(HWND window, int id, int fallback) {
    try { return std::stoi(Text(GetDlgItem(window, id))); }
    catch (...) { return fallback; }
}

std::wstring DefaultTestFrames(const RenderJob& job) {
    if (job.frames.empty()) return L"";
    std::vector<int> frames{job.frames.front().frame,
                            job.frames[job.frames.size() / 2].frame,
                            job.frames.back().frame};
    std::sort(frames.begin(), frames.end());
    frames.erase(std::unique(frames.begin(), frames.end()), frames.end());
    std::wstring textValue;
    for (int frame : frames) {
        if (!textValue.empty()) textValue += L",";
        textValue += std::to_wstring(frame);
    }
    return textValue;
}

const RenderSoftwareProfile* FindRenderSoftwareProfile(
    const Config& config, const std::string& profileId) {
    auto profile = std::find_if(
        config.renderSoftwareProfiles.begin(), config.renderSoftwareProfiles.end(),
        [&](const RenderSoftwareProfile& value) { return value.id == profileId; });
    return profile == config.renderSoftwareProfiles.end() ? nullptr : &*profile;
}

bool RemoveRenderSoftwareProfile(Config& config, const std::string& profileId,
                                 std::wstring& removedName,
                                 std::wstring& error) {
    auto selected = std::find_if(
        config.renderSoftwareProfiles.begin(), config.renderSoftwareProfiles.end(),
        [&](const RenderSoftwareProfile& profile) {
            return profile.id == profileId;
        });
    if (selected == config.renderSoftwareProfiles.end()) {
        error = L"当前配置尚未保存，无法删除";
        return false;
    }
    if (config.renderSoftwareProfiles.size() <= 1) {
        error = L"至少需要保留一个渲染软件配置";
        return false;
    }
    const size_t removedIndex = static_cast<size_t>(
        std::distance(config.renderSoftwareProfiles.begin(), selected));
    removedName = Utf8ToWide(selected->name);
    config.renderSoftwareProfiles.erase(selected);
    const size_t nextIndex = std::min(
        removedIndex, config.renderSoftwareProfiles.size() - 1);
    const auto& next = config.renderSoftwareProfiles[nextIndex];
    config.lastRenderSoftwareProfileId = next.id;
    config.blenderPath = next.installPath;
    return true;
}

void UpdateRenderSoftwareProfileEditControls(HWND window) {
    HWND edit = GetDlgItem(window, ID_CONFIG_PROFILE_EDIT);
    HWND remove = GetDlgItem(window, ID_CONFIG_PROFILE_DELETE);
    if (!edit || !remove || !gApp) return;
    const Config config = gApp->GetConfig();
    const bool persisted = FindRenderSoftwareProfile(config, gConfigProfileId) != nullptr;
    const bool canDelete = persisted && config.renderSoftwareProfiles.size() > 1;
    SetWindowTextW(edit, gConfigProfileEditing ? L"完成编辑" : L"编辑配置");
    ShowWindow(remove, gConfigProfileEditing ? SW_SHOWNA : SW_HIDE);
    EnableWindow(remove, canDelete ? TRUE : FALSE);
    SetWindowTextW(remove, !canDelete
        ? (persisted ? L"至少保留一个配置" : L"未保存配置")
        : gConfigProfileDeleteArmed ? L"再次点击删除" : L"删除当前配置");
    InvalidateRect(edit, nullptr, FALSE);
    InvalidateRect(remove, nullptr, FALSE);
}

void PopulateRenderSoftwareProfileDrawer(HWND drawer, const Config& config) {
    if (!drawer) return;
    SendMessageW(drawer, WM_SETREDRAW, FALSE, 0);
    SendMessageW(drawer, LB_RESETCONTENT, 0, 0);
    int selected = -1;
    for (size_t index = 0; index < config.renderSoftwareProfiles.size(); ++index) {
        const auto& profile = config.renderSoftwareProfiles[index];
        const std::wstring name = Utf8ToWide(profile.name);
        SendMessageW(drawer, LB_ADDSTRING, 0,
                     reinterpret_cast<LPARAM>(name.c_str()));
        if (profile.id == gConfigProfileId) selected = static_cast<int>(index);
    }
    SendMessageW(drawer, LB_SETITEMHEIGHT, 0, 48);
    SendMessageW(drawer, LB_SETCURSEL, selected, 0);
    SendMessageW(drawer, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(drawer, nullptr, TRUE);
}

void HideRenderSoftwareProfileDrawer(HWND window) {
    if (gConfigProfileDrawer && GetWindow(gConfigProfileDrawer, GW_OWNER) == window)
        ShowWindow(gConfigProfileDrawer, SW_HIDE);
    gConfigProfileDrawerHover = -1;
    if (HWND selector = GetDlgItem(window, ID_CONFIG_PROFILE_SELECT))
        InvalidateRect(selector, nullptr, FALSE);
}

void LoadRenderSoftwareProfileIntoWindow(HWND window) {
    Config config = gApp->GetConfig();
    const RenderSoftwareProfile* profile =
        FindRenderSoftwareProfile(config, gConfigProfileId);
    if (!profile && !config.renderSoftwareProfiles.empty()) {
        profile = &config.renderSoftwareProfiles.front();
        gConfigProfileId = profile->id;
    }
    if (!profile) return;
    SetWindowTextW(GetDlgItem(window, ID_CONFIG_PROFILE_SELECT),
                   (L"当前配置  ·  " + Utf8ToWide(profile->name)).c_str());
    SetWindowTextW(GetDlgItem(window, ID_CONFIG_PROFILE_NAME),
                   Utf8ToWide(profile->name).c_str());
    SetWindowTextW(GetDlgItem(window, ID_CONFIG_PROFILE_SYSTEM),
                   Utf8ToWide(profile->renderSystem).c_str());
    SetWindowTextW(GetDlgItem(window, ID_CONFIG_PROFILE_SOFTWARE),
                   Utf8ToWide(profile->renderSoftware).c_str());
    SetWindowTextW(GetDlgItem(window, ID_CONFIG_PROFILE_PATH),
                   profile->installPath.c_str());
    PopulateRenderSoftwareProfileDrawer(
        gConfigProfileDrawer, config);
    UpdateRenderSoftwareProfileEditControls(window);
}

bool SaveRenderSoftwareProfileFromWindow(HWND window, std::wstring& error) {
    Config config = gApp->GetConfig();
    RenderSoftwareProfile profile;
    profile.id = gConfigProfileId.empty() ? NewRandomHex(8) : gConfigProfileId;
    profile.name = WideToUtf8(Text(GetDlgItem(window, ID_CONFIG_PROFILE_NAME)));
    profile.renderSystem = WideToUtf8(Text(GetDlgItem(window, ID_CONFIG_PROFILE_SYSTEM)));
    profile.renderSoftware = WideToUtf8(Text(GetDlgItem(window, ID_CONFIG_PROFILE_SOFTWARE)));
    profile.installPath = Text(GetDlgItem(window, ID_CONFIG_PROFILE_PATH));
    if (profile.name.empty() || profile.renderSystem.empty() ||
        profile.renderSoftware.empty() || profile.installPath.empty()) {
        error = L"请完整填写配置名称、渲染系统、渲染软件和软件安装路径";
        return false;
    }
    auto existing = std::find_if(
        config.renderSoftwareProfiles.begin(), config.renderSoftwareProfiles.end(),
        [&](const RenderSoftwareProfile& value) { return value.id == profile.id; });
    if (existing == config.renderSoftwareProfiles.end())
        config.renderSoftwareProfiles.push_back(profile);
    else
        *existing = profile;
    config.lastRenderSoftwareProfileId = profile.id;
    if (!BlenderRenderAdapter().ConfigureExecutable(config, profile.installPath, error)) return false;
    if (!gApp->UpdateConfig(std::move(config), error)) return false;
    gConfigProfileId = profile.id;
    LoadRenderSoftwareProfileIntoWindow(window);
    return true;
}

bool SelectRenderSoftwareProfile(HWND window, size_t index) {
    Config config = gApp->GetConfig();
    if (index >= config.renderSoftwareProfiles.size()) return false;
    const auto& profile = config.renderSoftwareProfiles[index];
    const std::wstring profileName = Utf8ToWide(profile.name);
    gConfigProfileId = profile.id;
    config.lastRenderSoftwareProfileId = profile.id;
    config.blenderPath = profile.installPath;
    std::wstring error;
    if (!gApp->UpdateConfig(std::move(config), error)) {
        ShowInternalNotice(L"无法切换软件配置", error);
        return false;
    }
    LoadRenderSoftwareProfileIntoWindow(window);
    SetStatusText(L"已选择渲染软件配置：" + profileName);
    return true;
}

bool DeleteSelectedRenderSoftwareProfile(HWND window, std::wstring& error) {
    Config config = gApp->GetConfig();
    std::wstring removedName;
    if (!RemoveRenderSoftwareProfile(
            config, gConfigProfileId, removedName, error))
        return false;
    const std::string nextProfileId = config.lastRenderSoftwareProfileId;
    if (!gApp->UpdateConfig(std::move(config), error)) return false;
    gConfigProfileId = nextProfileId;
    gConfigProfileDeleteArmed = false;
    LoadRenderSoftwareProfileIntoWindow(window);
    SetStatusText(L"已删除软件配置：" + removedName);
    return true;
}

RECT RenderSoftwareProfileDrawerBounds(const RECT& selectorRect,
                                       size_t profileCount) {
    const int visibleRows = std::clamp(static_cast<int>(profileCount), 1, 4);
    const int height = visibleRows * 48 + 6;
    return {selectorRect.left, selectorRect.bottom + 4, selectorRect.right,
            selectorRect.bottom + 4 + height};
}

void PositionRenderSoftwareProfileDrawer(HWND window, bool show) {
    HWND drawer = gConfigProfileDrawer;
    HWND selector = GetDlgItem(window, ID_CONFIG_PROFILE_SELECT);
    if (!drawer || !selector) return;
    RECT selectorRect{};
    if (!GetWindowRect(selector, &selectorRect)) return;
    const Config config = gApp->GetConfig();
    const RECT bounds = RenderSoftwareProfileDrawerBounds(
        selectorRect, config.renderSoftwareProfiles.size());
    SetWindowPos(drawer, HWND_TOP, bounds.left, bounds.top,
                 bounds.right - bounds.left, bounds.bottom - bounds.top,
                 SWP_NOACTIVATE | (show ? SWP_SHOWWINDOW : 0));
}

void ToggleRenderSoftwareProfileDrawer(HWND window) {
    HWND drawer = gConfigProfileDrawer;
    HWND selector = GetDlgItem(window, ID_CONFIG_PROFILE_SELECT);
    if (!drawer || !selector) return;
    if (IsWindowVisible(drawer)) {
        HideRenderSoftwareProfileDrawer(window);
        return;
    }
    Config config = gApp->GetConfig();
    PopulateRenderSoftwareProfileDrawer(drawer, config);
    PositionRenderSoftwareProfileDrawer(window, true);
    UpdateModernFieldRegion(drawer);
    RedrawWindow(drawer, nullptr, nullptr,
                 RDW_INVALIDATE | RDW_ERASE | RDW_UPDATENOW);
    InvalidateRect(selector, nullptr, FALSE);
}

void DrawRenderSoftwareProfileDrawerItem(const DRAWITEMSTRUCT& item) {
    if (item.itemID == static_cast<UINT>(-1)) return;
    Config config = gApp->GetConfig();
    if (item.itemID >= config.renderSoftwareProfiles.size()) return;
    const auto& profile = config.renderSoftwareProfiles[item.itemID];
    const bool selected = (item.itemState & ODS_SELECTED) != 0 ||
        profile.id == gConfigProfileId;
    const COLORREF background = selected
        ? (gDarkTheme ? RGB(39, 44, 54) : RGB(232, 242, 237))
        : (gDarkTheme ? RGB(31, 32, 42) : RGB(255, 255, 255));
    HBRUSH brush = CreateSolidBrush(background);
    FillRect(item.hDC, &item.rcItem, brush);
    DeleteObject(brush);
    SetBkMode(item.hDC, TRANSPARENT);
    RECT checkRect = item.rcItem;
    checkRect.left += 12;
    checkRect.right = checkRect.left + 24;
    SetTextColor(item.hDC, profile.id == gConfigProfileId
        ? kChromeAccent : (gDarkTheme ? RGB(108, 111, 126) : RGB(147, 151, 163)));
    HGDIOBJ oldFont = SelectObject(item.hDC, gIconFont);
    DrawTextW(item.hDC, profile.id == gConfigProfileId ? L"\xE73E" : L"",
              -1, &checkRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    SelectObject(item.hDC, gSemiboldFont ? gSemiboldFont : gFont);
    RECT nameRect = item.rcItem;
    nameRect.left += 46;
    nameRect.top += 5;
    nameRect.bottom = nameRect.top + 20;
    SetTextColor(item.hDC, gDarkTheme ? RGB(239, 241, 247) : RGB(31, 38, 34));
    const std::wstring name = Utf8ToWide(profile.name);
    DrawTextW(item.hDC, name.c_str(), -1, &nameRect,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    SelectObject(item.hDC, gFont);
    RECT metaRect = item.rcItem;
    metaRect.left += 46;
    metaRect.top += 25;
    metaRect.bottom -= 3;
    SetTextColor(item.hDC, gDarkTheme ? RGB(164, 168, 184) : RGB(101, 107, 117));
    const std::wstring meta = Utf8ToWide(profile.renderSystem) + L"  ·  " +
        Utf8ToWide(profile.renderSoftware);
    DrawTextW(item.hDC, meta.c_str(), -1, &metaRect,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    SelectObject(item.hDC, oldFont);
}

int ConfigContentRight(HWND window, int clientWidth) {
    (void)window;
    constexpr int margin = 24;
    return std::max(margin, clientWidth - margin);
}

void LayoutConfigWindow(HWND window) {
    if (!window) return;
    RECT client{};
    GetClientRect(window, &client);
    constexpr int left = 34;
    const int right = ConfigContentRight(window, client.right);
    constexpr int gap = 12;
    constexpr int labelRight = 136;
    constexpr int newButtonWidth = 138;
    constexpr int editButtonWidth = 116;
    const int newButtonLeft = right - newButtonWidth;
    const int editButtonLeft = newButtonLeft - gap - editButtonWidth;
    const int selectorRight = editButtonLeft - gap;
    MoveWindow(GetDlgItem(window, ID_CONFIG_PROFILE_SELECT), left, 140,
               std::max(120, selectorRight - left), 38, TRUE);
    MoveWindow(GetDlgItem(window, ID_CONFIG_PROFILE_EDIT), editButtonLeft, 140,
               editButtonWidth, 38, TRUE);
    MoveWindow(GetDlgItem(window, ID_CONFIG_PROFILE_NEW), newButtonLeft, 140,
               newButtonWidth, 38, TRUE);
    MoveWindow(GetDlgItem(window, ID_CONFIG_PROFILE_NAME), labelRight, 194,
               std::max(120, right - labelRight), 32, TRUE);
    MoveWindow(GetDlgItem(window, ID_CONFIG_PROFILE_SOFTWARE), 442, 240,
               std::max(120, right - 442), 34, TRUE);
    constexpr int browseWidth = 86;
    const int browseLeft = right - browseWidth;
    MoveWindow(GetDlgItem(window, ID_CONFIG_PROFILE_PATH), labelRight, 288,
               std::max(120, browseLeft - 10 - labelRight), 34, TRUE);
    MoveWindow(GetDlgItem(window, ID_CONFIG_PROFILE_BROWSE), browseLeft, 288,
               browseWidth, 34, TRUE);
    constexpr int profileActionWidth = 150;
    MoveWindow(GetDlgItem(window, ID_CONFIG_PROFILE_SAVE),
               right - profileActionWidth, 334, profileActionWidth, 34, TRUE);
    MoveWindow(GetDlgItem(window, ID_CONFIG_PROFILE_DELETE),
               right - profileActionWidth * 2 - gap, 334,
               profileActionWidth, 34, TRUE);
    MoveWindow(GetDlgItem(window, ID_CONFIG_TEST_FRAMES), left, 596,
               std::max(180, right - left), 34, TRUE);
    constexpr int submitWidth = 148;
    constexpr int cancelWidth = 116;
    MoveWindow(GetDlgItem(window, ID_CONFIG_SUBMIT), right - submitWidth, 686,
               submitWidth, 38, TRUE);
    MoveWindow(GetDlgItem(window, ID_CONFIG_CANCEL),
               right - submitWidth - gap - cancelWidth, 686,
               cancelWidth, 38, TRUE);
}

LRESULT CALLBACK ProfileDrawerSubclassProc(
    HWND window, UINT message, WPARAM wParam, LPARAM lParam,
    UINT_PTR subclassId, DWORD_PTR) {
    switch (message) {
        case WM_PAINT: {
            PAINTSTRUCT paint{};
            HDC dc = BeginPaint(window, &paint);
            RECT client{};
            GetClientRect(window, &client);
            HBRUSH background = CreateSolidBrush(
                gDarkTheme ? RGB(31, 32, 42) : RGB(255, 255, 255));
            FillRect(dc, &client, background);
            DeleteObject(background);
            const int count = static_cast<int>(SendMessageW(window, LB_GETCOUNT, 0, 0));
            const int selected = static_cast<int>(SendMessageW(window, LB_GETCURSEL, 0, 0));
            for (int index = 0; index < count; ++index) {
                DRAWITEMSTRUCT item{};
                item.CtlType = ODT_LISTBOX;
                item.CtlID = ID_CONFIG_PROFILE_DRAWER;
                item.itemID = static_cast<UINT>(index);
                item.itemState = index == selected || index == gConfigProfileDrawerHover
                    ? ODS_SELECTED : 0;
                item.hwndItem = window;
                item.hDC = dc;
                item.rcItem = {0, index * 48, client.right, (index + 1) * 48};
                DrawRenderSoftwareProfileDrawerItem(item);
            }
            EndPaint(window, &paint);
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_MOUSEMOVE: {
            const int index = GET_Y_LPARAM(lParam) / 48;
            const int count = static_cast<int>(SendMessageW(window, LB_GETCOUNT, 0, 0));
            const int next = index >= 0 && index < count ? index : -1;
            if (next != gConfigProfileDrawerHover) {
                gConfigProfileDrawerHover = next;
                InvalidateRect(window, nullptr, FALSE);
            }
            TRACKMOUSEEVENT tracking{sizeof(tracking), TME_LEAVE, window, 0};
            TrackMouseEvent(&tracking);
            return 0;
        }
        case WM_MOUSELEAVE:
            gConfigProfileDrawerHover = -1;
            InvalidateRect(window, nullptr, FALSE);
            return 0;
        case WM_LBUTTONUP: {
            const int index = GET_Y_LPARAM(lParam) / 48;
            const int count = static_cast<int>(SendMessageW(window, LB_GETCOUNT, 0, 0));
            if (index >= 0 && index < count) {
                SendMessageW(window, LB_SETCURSEL, index, 0);
                if (HWND owner = GetWindow(window, GW_OWNER))
                    PostMessageW(owner, WM_COMMAND,
                                 MAKEWPARAM(ID_CONFIG_PROFILE_DRAWER, LBN_SELCHANGE),
                                 reinterpret_cast<LPARAM>(window));
            }
            return 0;
        }
        case WM_NCDESTROY:
            RemoveWindowSubclass(window, ProfileDrawerSubclassProc, subclassId);
            break;
    }
    return DefSubclassProc(window, message, wParam, lParam);
}

LRESULT CALLBACK ConfigWindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
        case WM_CREATE: {
            auto job = FindJob(gConfigJobId);
            if (!job) return -1;
            gConfigProfileEditing = false;
            gConfigProfileDeleteArmed = false;
            const int first = job->frames.empty() ? 1 : job->frames.front().frame;
            const int last = job->frames.empty() ? first : job->frames.back().frame;
            const int step = job->frames.size() > 1
                ? std::max(1, job->frames[1].frame - job->frames[0].frame) : 1;
            AddDialogControl(window, L"STATIC", L"渲染参数设置", SS_LEFT,
                             22, 18, 300, 34, 0, gTitleFont);
            AddDialogControl(window, L"STATIC",
                (L"场景名  " + Utf8ToWide(job->name)).c_str(), SS_LEFT,
                24, 62, 680, 26, 0);
            AddDialogControl(window, L"STATIC", L"渲染软件配置", SS_LEFT,
                             24, 104, 740, 28, 0, gSemiboldFont);
            HWND profileSelector = AddDialogControl(
                window, L"BUTTON", L"当前配置", BS_OWNERDRAW,
                34, 140, 510, 38, ID_CONFIG_PROFILE_SELECT);
            SetPropW(profileSelector, kDropdownAffordanceProperty,
                     reinterpret_cast<HANDLE>(1));
            HWND profileDrawer = CreateWindowExW(
                WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, L"LISTBOX", L"",
                WS_POPUP | WS_VSCROLL | LBS_NOTIFY |
                    LBS_OWNERDRAWFIXED | LBS_HASSTRINGS | LBS_NOINTEGRALHEIGHT,
                0, 0, 510, 102, window, nullptr,
                GetModuleHandleW(nullptr), nullptr);
            SetWindowLongPtrW(profileDrawer, GWLP_ID, ID_CONFIG_PROFILE_DRAWER);
            gConfigProfileDrawer = profileDrawer;
            SetWindowSubclass(profileDrawer, ProfileDrawerSubclassProc, 1, 0);
            SendMessageW(profileDrawer, WM_SETFONT,
                         reinterpret_cast<WPARAM>(gFont), TRUE);
            SetWindowTheme(profileDrawer, L"", nullptr);
            AddDialogControl(window, L"BUTTON", L"编辑配置", BS_OWNERDRAW,
                             418, 140, 116, 38, ID_CONFIG_PROFILE_EDIT);
            AddDialogControl(window, L"BUTTON", L"＋ 新建配置", BS_OWNERDRAW,
                             546, 140, 160, 38, ID_CONFIG_PROFILE_NEW);
            AddDialogControl(window, L"STATIC", L"配置名称", SS_LEFT,
                             34, 198, 96, 24, 0);
            AddDialogControl(window, L"EDIT", L"", ES_AUTOHSCROLL,
                             136, 194, 570, 32, ID_CONFIG_PROFILE_NAME);
            AddDialogControl(window, L"STATIC", L"渲染系统", SS_LEFT,
                             34, 246, 96, 24, 0);
            AddDialogControl(window, L"BUTTON", L"Windows", BS_OWNERDRAW,
                             136, 240, 180, 34, ID_CONFIG_PROFILE_SYSTEM);
            AddDialogControl(window, L"STATIC", L"渲染软件", SS_LEFT,
                             340, 246, 96, 24, 0);
            AddDialogControl(window, L"BUTTON", L"Blender", BS_OWNERDRAW,
                             442, 240, 264, 34, ID_CONFIG_PROFILE_SOFTWARE);
            AddDialogControl(window, L"STATIC", L"安装路径", SS_LEFT,
                             34, 294, 96, 24, 0);
            AddDialogControl(window, L"EDIT", L"", ES_AUTOHSCROLL,
                             136, 288, 474, 34, ID_CONFIG_PROFILE_PATH);
            AddDialogControl(window, L"BUTTON", L"选择…", BS_OWNERDRAW,
                             620, 288, 86, 34, ID_CONFIG_PROFILE_BROWSE);
            AddDialogControl(window, L"BUTTON", L"保存配置", BS_OWNERDRAW,
                             556, 334, 150, 34, ID_CONFIG_PROFILE_SAVE);
            HWND deleteProfile = AddDialogControl(
                window, L"BUTTON", L"删除当前配置", BS_OWNERDRAW,
                394, 334, 150, 34, ID_CONFIG_PROFILE_DELETE);
            ShowWindow(deleteProfile, SW_HIDE);
            AddDialogControl(window, L"STATIC", L"场景参数", SS_LEFT,
                             24, 382, 680, 28, 0, gSemiboldFont);
            AddDialogControl(window, L"STATIC", L"起始帧", SS_LEFT,
                             34, 428, 70, 24, 0);
            AddDialogControl(window, L"EDIT", std::to_wstring(first).c_str(),
                             ES_NUMBER, 108, 424, 100, 30, ID_CONFIG_START);
            AddDialogControl(window, L"STATIC", L"结束帧", SS_LEFT,
                             242, 428, 70, 24, 0);
            AddDialogControl(window, L"EDIT", std::to_wstring(last).c_str(),
                             ES_NUMBER, 316, 424, 100, 30, ID_CONFIG_END);
            AddDialogControl(window, L"STATIC", L"步长", SS_LEFT,
                             450, 428, 54, 24, 0);
            AddDialogControl(window, L"EDIT", std::to_wstring(step).c_str(),
                             ES_NUMBER, 508, 424, 80, 30, ID_CONFIG_STEP);
            AddDialogControl(window, L"STATIC", L"每包帧数", SS_LEFT,
                             34, 476, 86, 24, 0);
            AddDialogControl(window, L"EDIT", std::to_wstring(job->packageSize).c_str(),
                             ES_NUMBER, 124, 472, 84, 30, ID_CONFIG_PACKAGE);
            AddDialogControl(window, L"STATIC", L"1–20 帧，默认 5；同一包只启动一次 Blender。",
                             SS_LEFT, 226, 476, 450, 24, 0);
            AddDialogControl(window, L"STATIC", L"优先测试帧", SS_LEFT,
                             24, 526, 680, 28, 0, gSemiboldFont);
            AddDialogControl(window, L"STATIC",
                L"测试帧优先分配给在线电脑；没有可用节点时由本机渲染。支持半角逗号和范围。",
                SS_LEFT, 34, 562, 680, 24, 0);
            AddDialogControl(window, L"EDIT", DefaultTestFrames(*job).c_str(),
                             ES_AUTOHSCROLL, 34, 596, 672, 34,
                             ID_CONFIG_TEST_FRAMES);
            AddDialogControl(window, L"STATIC",
                L"测试完成后任务会暂停；在任务列表右键选择“全速渲染”才会执行其余帧。",
                SS_LEFT, 34, 642, 672, 28, 0);
            AddDialogControl(window, L"BUTTON", L"取消", BS_OWNERDRAW,
                             430, 686, 116, 38, ID_CONFIG_CANCEL);
            AddDialogControl(window, L"BUTTON", L"开始测试渲染", BS_OWNERDRAW,
                             558, 686, 148, 38, ID_CONFIG_SUBMIT);
            ApplyWorkflowTheme(window, nullptr);
            LoadRenderSoftwareProfileIntoWindow(window);
            LayoutConfigWindow(window);
            return 0;
        }
        case WM_SIZE:
            if (wParam != SIZE_MINIMIZED) {
                LayoutConfigWindow(window);
                if (gConfigProfileDrawer && IsWindowVisible(gConfigProfileDrawer))
                    PositionRenderSoftwareProfileDrawer(window, false);
            }
            return 0;
        case WM_WINDOWPOSCHANGED:
            if (gConfigProfileDrawer && IsWindowVisible(gConfigProfileDrawer))
                PositionRenderSoftwareProfileDrawer(window, false);
            break;
        case WM_COMMAND: {
            const UINT id = LOWORD(wParam);
            if (id == ID_CONFIG_PROFILE_DRAWER &&
                HIWORD(wParam) == LBN_SELCHANGE) {
                HWND drawer = gConfigProfileDrawer;
                const LRESULT selected = SendMessageW(drawer, LB_GETCURSEL, 0, 0);
                HideRenderSoftwareProfileDrawer(window);
                if (selected != LB_ERR) {
                    gConfigProfileDeleteArmed = false;
                    SelectRenderSoftwareProfile(window, static_cast<size_t>(selected));
                }
                return 0;
            }
            if (id == ID_CONFIG_CANCEL) {
                DestroyWindow(window);
                return 0;
            }
            if (id == ID_CONFIG_PROFILE_SELECT) {
                gConfigProfileDeleteArmed = false;
                UpdateRenderSoftwareProfileEditControls(window);
                ToggleRenderSoftwareProfileDrawer(window);
                return 0;
            }
            HideRenderSoftwareProfileDrawer(window);
            if (id != ID_CONFIG_PROFILE_DELETE && gConfigProfileDeleteArmed) {
                gConfigProfileDeleteArmed = false;
                UpdateRenderSoftwareProfileEditControls(window);
            }
            if (id == ID_CONFIG_PROFILE_EDIT) {
                gConfigProfileEditing = !gConfigProfileEditing;
                gConfigProfileDeleteArmed = false;
                UpdateRenderSoftwareProfileEditControls(window);
                return 0;
            }
            if (id == ID_CONFIG_PROFILE_NEW) {
                gConfigProfileId = NewRandomHex(8);
                gConfigProfileDeleteArmed = false;
                SetWindowTextW(GetDlgItem(window, ID_CONFIG_PROFILE_SELECT),
                               L"当前配置  ·  新配置（未保存）");
                SetWindowTextW(GetDlgItem(window, ID_CONFIG_PROFILE_NAME), L"");
                SetWindowTextW(GetDlgItem(window, ID_CONFIG_PROFILE_SYSTEM), L"Windows");
                SetWindowTextW(GetDlgItem(window, ID_CONFIG_PROFILE_SOFTWARE), L"Blender");
                SetWindowTextW(GetDlgItem(window, ID_CONFIG_PROFILE_PATH), L"");
                UpdateRenderSoftwareProfileEditControls(window);
                SetFocus(GetDlgItem(window, ID_CONFIG_PROFILE_NAME));
                return 0;
            }
            if (id == ID_CONFIG_PROFILE_DELETE) {
                if (!gConfigProfileDeleteArmed) {
                    gConfigProfileDeleteArmed = true;
                    UpdateRenderSoftwareProfileEditControls(window);
                    return 0;
                }
                std::wstring error;
                if (!DeleteSelectedRenderSoftwareProfile(window, error))
                    ShowInternalNotice(L"无法删除软件配置", error);
                return 0;
            }
            if (id == ID_CONFIG_PROFILE_BROWSE) {
                auto path = ChooseFile(
                    L"Blender 程序\0blender.exe;blender-launcher.exe\0所有程序\0*.exe\0\0",
                    L"选择 Blender 程序");
                if (!path.empty())
                    SetWindowTextW(GetDlgItem(window, ID_CONFIG_PROFILE_PATH), path.c_str());
                return 0;
            }
            if (id == ID_CONFIG_PROFILE_SAVE) {
                std::wstring error;
                if (!SaveRenderSoftwareProfileFromWindow(window, error))
                    ShowInternalNotice(L"无法保存软件配置", error);
                else
                    SetStatusText(L"渲染软件配置已保存，并设为下次默认选择。");
                return 0;
            }
            if (id == ID_CONFIG_SUBMIT) {
                std::wstring error;
                if (!SaveRenderSoftwareProfileFromWindow(window, error)) {
                    ShowInternalNotice(L"无法使用软件配置", error);
                    return 0;
                }
                const bool ok = gApp->ConfigureAndStartTest(
                    gConfigJobId,
                    DialogInt(window, ID_CONFIG_START, 1),
                    DialogInt(window, ID_CONFIG_END, 1),
                    DialogInt(window, ID_CONFIG_STEP, 1),
                    static_cast<size_t>(std::max(0, DialogInt(window, ID_CONFIG_PACKAGE, 5))),
                    Text(GetDlgItem(window, ID_CONFIG_TEST_FRAMES)), error);
                if (!ok) {
                    ShowInternalNotice(L"无法开始测试", error);
                    return 0;
                }
                SetStatusText(L"测试帧已进入队列；在线节点优先，否则由本机渲染。");
                DestroyWindow(window);
                RefreshLists();
                return 0;
            }
            return 0;
        }
        case WM_DRAWITEM:
            if (reinterpret_cast<DRAWITEMSTRUCT*>(lParam)->CtlType == ODT_LISTBOX &&
                reinterpret_cast<DRAWITEMSTRUCT*>(lParam)->CtlID ==
                    ID_CONFIG_PROFILE_DRAWER)
                DrawRenderSoftwareProfileDrawerItem(
                    *reinterpret_cast<DRAWITEMSTRUCT*>(lParam));
            else
                DrawOwnerButton(*reinterpret_cast<DRAWITEMSTRUCT*>(lParam));
            return TRUE;
        case WM_MEASUREITEM:
            if (reinterpret_cast<MEASUREITEMSTRUCT*>(lParam)->CtlType == ODT_LISTBOX &&
                reinterpret_cast<MEASUREITEMSTRUCT*>(lParam)->CtlID ==
                    ID_CONFIG_PROFILE_DRAWER) {
                reinterpret_cast<MEASUREITEMSTRUCT*>(lParam)->itemHeight = 48;
                return TRUE;
            }
            break;
        case WM_ACTIVATE:
            if (LOWORD(wParam) == WA_INACTIVE && !gPreviewConfigDrawer)
                HideRenderSoftwareProfileDrawer(window);
            break;
        case WM_KEYDOWN:
            if (wParam == VK_ESCAPE &&
                gConfigProfileDrawer && IsWindowVisible(gConfigProfileDrawer)) {
                HideRenderSoftwareProfileDrawer(window);
                SetFocus(GetDlgItem(window, ID_CONFIG_PROFILE_SELECT));
                return 0;
            }
            break;
        case WM_RM_OPEN_CONFIG_DRAWER:
            ToggleRenderSoftwareProfileDrawer(window);
            return 0;
        case WM_TIMER:
            if (wParam == TIMER_CONFIG_DRAWER_PREVIEW) {
                KillTimer(window, TIMER_CONFIG_DRAWER_PREVIEW);
                if (!gConfigProfileDrawer || !IsWindowVisible(gConfigProfileDrawer))
                    ToggleRenderSoftwareProfileDrawer(window);
                return 0;
            }
            break;
        case WM_CTLCOLORSTATIC: {
            HDC dc = reinterpret_cast<HDC>(wParam);
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, gDarkTheme ? RGB(239, 239, 247) : RGB(31, 31, 38));
            return reinterpret_cast<LRESULT>(gBackgroundBrush);
        }
        case WM_CTLCOLOREDIT: {
            HDC dc = reinterpret_cast<HDC>(wParam);
            SetTextColor(dc, gDarkTheme ? RGB(239, 239, 247) : RGB(31, 31, 38));
            SetBkColor(dc, gDarkTheme ? RGB(31, 32, 42) : RGB(255, 255, 255));
            return reinterpret_cast<LRESULT>(gSurfaceBrush);
        }
        case WM_CTLCOLORLISTBOX: {
            HDC dc = reinterpret_cast<HDC>(wParam);
            SetBkColor(dc, gDarkTheme ? RGB(31, 32, 42) : RGB(255, 255, 255));
            SetTextColor(dc, gDarkTheme ? RGB(239, 241, 247) : RGB(31, 38, 34));
            return reinterpret_cast<LRESULT>(gSurfaceBrush);
        }
        case WM_ERASEBKGND: {
            RECT client{}; GetClientRect(window, &client);
            FillRect(reinterpret_cast<HDC>(wParam), &client, gBackgroundBrush);
            return TRUE;
        }
        case WM_DESTROY:
            if (gConfigProfileDrawer) {
                DestroyWindow(gConfigProfileDrawer);
                gConfigProfileDrawer = nullptr;
            }
            gConfigWindow = nullptr;
            gConfigJobId.clear();
            gConfigProfileId.clear();
            gConfigProfileEditing = false;
            gConfigProfileDeleteArmed = false;
            return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

void ShowUploadWindow(const std::string& jobId, bool editMode) {
    if (gUploadWindow && (gUploadJobId != jobId || gUploadEditMode != editMode))
        DestroyWindow(gUploadWindow);
    if (gUploadWindow) {
        RefreshUploadWindow();
        ShowWindow(gUploadWindow, SW_RESTORE);
        SetForegroundWindow(gUploadWindow);
        return;
    }
    gUploadJobId = jobId;
    gUploadEditMode = editMode;
    gUploadSelectableNodeIds.clear();
    gUploadWindow = CreateWindowExW(
        WS_EX_APPWINDOW, L"RenderMeshUploadProgress",
        editMode ? L"RenderMesh · 编辑渲染主机" : L"RenderMesh · 工程同步",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX |
            WS_MAXIMIZEBOX | WS_SIZEBOX | WS_CLIPCHILDREN,
        CW_USEDEFAULT, CW_USEDEFAULT, 960, 620, gWindow, nullptr,
        GetModuleHandleW(nullptr), nullptr);
    CenterOwnedWindow(gUploadWindow, gWindow);
    LayoutUploadWindow(gUploadWindow);
    RefreshUploadWindow();
    ShowWindow(gUploadWindow, SW_SHOW);
    UpdateWindow(gUploadWindow);
}

void ShowConfigWindow(const std::string& jobId) {
    if (gConfigWindow && gConfigJobId != jobId) DestroyWindow(gConfigWindow);
    gConfigJobId = jobId;
    if (gConfigProfileId.empty()) {
        Config config = gApp->GetConfig();
        gConfigProfileId = config.lastRenderSoftwareProfileId;
        if (gConfigProfileId.empty() && !config.renderSoftwareProfiles.empty())
            gConfigProfileId = config.renderSoftwareProfiles.front().id;
    }
    if (gConfigWindow) {
        ShowWindow(gConfigWindow, SW_RESTORE);
        SetForegroundWindow(gConfigWindow);
        return;
    }
    gConfigWindow = CreateWindowExW(
        WS_EX_APPWINDOW, L"RenderMeshSubmitConfig", L"RenderMesh · 渲染参数设置",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        CW_USEDEFAULT, CW_USEDEFAULT, 780, 790, gWindow, nullptr,
        GetModuleHandleW(nullptr), nullptr);
    CenterOwnedWindow(gConfigWindow, gWindow);
    ShowWindow(gConfigWindow, SW_SHOW);
    UpdateWindow(gConfigWindow);
}

void BuildUi() {
    for (auto& page : gPageControls) page.clear();
    AddControl(L"BUTTON", L"接入网络", BS_OWNERDRAW,
               18, 18, 184, 42, ID_SHARE);
    AddControl(L"BUTTON", L"服务器", BS_OWNERDRAW, 18, 96, 184, 42, ID_NAV_DASHBOARD);
    AddControl(L"BUTTON", L"渲染任务", BS_OWNERDRAW, 18, 144, 184, 42, ID_NAV_TASKS);
    AddControl(L"BUTTON", L"本机贡献", BS_OWNERDRAW, 18, 192, 184, 42, ID_NAV_CONTRIBUTIONS);
    AddControl(L"BUTTON", L"设置", BS_OWNERDRAW, 18, 680, 42, 42, ID_NAV_SETTINGS);
    // 服务器页
    PageControl(0, Label(L"服务器", 248, 24, 250, 36, 0, gTitleFont));
    PageControl(0, Label(L"手动连接", 250, 202, 80, 24));
    PageControl(0, AddControl(L"EDIT", L"", ES_AUTOHSCROLL,
                              334, 198, 235, 28, ID_MANUAL_ENDPOINT));
    PageControl(0, AddControl(L"BUTTON", L"连接", BS_OWNERDRAW,
                              579, 197, 82, 30, ID_CONNECT_MANUAL));
    PageControl(0, Label(L"填写另一台电脑的局域网 IP:端口，不要填 127.0.0.1",
                         680, 202, 550, 24));
    HWND matchingId = PageControl(0, AddControl(
        L"EDIT", L"", ES_AUTOHSCROLL | ES_CENTER | ES_NUMBER,
        1030, 126, 150, 38, ID_CLUSTER_TOKEN));
    SendMessageW(matchingId, EM_SETLIMITTEXT, 6, 0);
    SendMessageW(matchingId, EM_SETCUEBANNER, TRUE,
                 reinterpret_cast<LPARAM>(L"输入 6 位匹配 ID"));
    SendMessageW(matchingId, WM_SETFONT,
                 reinterpret_cast<WPARAM>(gMatchIdFont), TRUE);
    PageControl(0, AddControl(L"BUTTON", L"复制", BS_OWNERDRAW,
                              1188, 126, 58, 38, ID_COPY_TOKEN));
    HWND nodes = PageControl(0, AddControl(
        WC_LISTVIEWW, L"", LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS |
            WS_CLIPSIBLINGS,
        250, 238, 990, 454, ID_NODES));
    ConfigureList(nodes);
    AddColumn(nodes, 0, 170, L"电脑"); AddColumn(nodes, 1, 85, L"连接");
    AddColumn(nodes, 2, 150, L"算力状态"); AddColumn(nodes, 3, 280, L"已分配");
    AddColumn(nodes, 4, 220, L"CPU 型号"); AddColumn(nodes, 5, 240, L"GPU 型号");
    AddColumn(nodes, 6, 210, L"地址");

    // 渲染任务页：只保留任务列表，新建通过拖放或右下角按钮进入向导。
    PageControl(1, Label(L"渲染任务", 248, 24, 300, 36, 0, gTitleFont));
    HWND jobs = PageControl(1, AddControl(
        WC_LISTVIEWW, L"", LVS_REPORT | LVS_SHOWSELALWAYS | WS_CLIPSIBLINGS,
        250, 72, 976, 598, ID_JOBS));
    ConfigureList(jobs);
    AddColumn(jobs, 0, 210, L"任务"); AddColumn(jobs, 1, 95, L"状态");
    AddColumn(jobs, 2, 75, L"总帧数"); AddColumn(jobs, 3, 95, L"完成");
    AddColumn(jobs, 4, 100, L"渲染时长"); AddColumn(jobs, 5, 155, L"开始时间");
    AddColumn(jobs, 6, 155, L"结束时间");
    SizeListForVisibleRows(jobs, 10);
    PageControl(1, AddControl(L"BUTTON", L"\xE710", BS_OWNERDRAW,
                              1168, 684, 58, 58, ID_ADD_JOB, gIconFont));
    HWND dropOverlay = AddControl(
        L"BUTTON", L"拖放 .blend 文件到窗口内", BS_OWNERDRAW | WS_CLIPSIBLINGS,
        250, 72, 976, 598, ID_DROP_OVERLAY, gTitleFont);
    ShowWindow(dropOverlay, SW_HIDE);

    // 向导内部复用的隐藏字段，不再展平在任务页。
    for (HWND hidden : {
            AddControl(L"EDIT", L"", ES_AUTOHSCROLL, 0, 0, 1, 1, ID_SOURCE_PATH),
            AddControl(L"EDIT", L"", ES_AUTOHSCROLL, 0, 0, 1, 1, ID_JOB_NAME),
            AddControl(L"EDIT", L"1", ES_NUMBER, 0, 0, 1, 1, ID_FRAME_START),
            AddControl(L"EDIT", L"1", ES_NUMBER, 0, 0, 1, 1, ID_FRAME_END),
            AddControl(L"EDIT", L"1", ES_NUMBER, 0, 0, 1, 1, ID_FRAME_STEP),
            AddControl(L"BUTTON", L"", 0, 0, 0, 1, 1, ID_SUBMIT)})
        ShowWindow(hidden, SW_HIDE);

    // 本机贡献页
    PageControl(2, Label(L"本机贡献", 248, 24, 300, 36, 0, gTitleFont));
    HWND contributionList = PageControl(2, AddControl(
        WC_LISTVIEWW, L"", LVS_REPORT | LVS_SHOWSELALWAYS | WS_CLIPSIBLINGS,
        250, 72, 976, 620, ID_CONTRIBUTION_LIST));
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
    PageControl(3, AddControl(L"EDIT", L"", ES_AUTOHSCROLL,
                              445, 96, 650, 30, ID_NODE_NAME));
    PageControl(3, Label(L"无操作后闲置（秒）", 270, 154, 165, 24));
    PageControl(3, AddControl(L"EDIT", L"180", ES_NUMBER,
                              445, 150, 650, 30, ID_IDLE_SECONDS));
    PageControl(3, Label(L"外观", 270, 208, 155, 24));
    PageControl(3, AddControl(L"BUTTON", L"跟随系统", BS_OWNERDRAW,
                              445, 204, 650, 32, ID_THEME));
    PageControl(3, Label(L"当点击关闭时", 270, 262, 155, 24));
    HWND closeBehavior = PageControl(3, AddControl(
        L"BUTTON", L"每次询问", BS_OWNERDRAW,
        445, 258, 650, 32, ID_CLOSE_BEHAVIOR));
    SetPropW(closeBehavior, kDropdownAffordanceProperty, reinterpret_cast<HANDLE>(1));
    PageControl(3, Label(L"系统启动", 270, 320, 155, 24));
    PageControl(3, AddControl(
        L"BUTTON", L"开机自动启动", BS_OWNERDRAW | WS_CLIPSIBLINGS,
        445, 312, 650, 40, ID_AUTO_START));
    PageControl(3, Label(L"启动方式", 270, 374, 155, 24));
    PageControl(3, AddControl(
        L"BUTTON", L"自启时静默运行", BS_OWNERDRAW | WS_CLIPSIBLINGS,
        445, 366, 650, 40, ID_SILENT_AUTO_START));
    HWND silentInfo = PageControl(3, AddControl(
        L"STATIC", L"\u24D8", SS_CENTER | SS_NOTIFY | WS_CLIPSIBLINGS,
        588, 374, 24, 24, ID_SILENT_AUTO_START_INFO, gFont));
    gSilentAutoStartTooltip = CreateTooltip(
        gWindow, silentInfo,
        L"程序随 Windows 启动后在后台运行，不显示程序面板；"
        L"手动打开 RenderMesh 时仍显示主界面。");
    PageControl(3, Label(L"版本信息", 270, 438, 155, 24));
    const std::wstring versionText =
        L"当前版本  " + Utf8ToWide(kRenderMeshAppVersion);
    PageControl(3, Label(versionText.c_str(), 445, 430, 420, 36,
                         ID_CURRENT_VERSION));
    PageControl(3, AddControl(
        L"BUTTON", L"检查更新", BS_OWNERDRAW,
        920, 428, 175, 36, ID_CHECK_UPDATE));
    PageControl(3, Label(L"Blender 版本", 270, 492, 155, 24));
    PageControl(3, AddControl(
        L"BUTTON", L"安装 / 切换 Blender 版本", BS_OWNERDRAW,
        445, 482, 650, 40, ID_INSTALL_BLENDER));
    PageControl(3, Label(L"Blender 路径", 270, 550, 155, 24));
    PageControl(3, AddControl(
        L"EDIT", L"", ES_AUTOHSCROLL | ES_READONLY,
        445, 542, 500, 36, ID_BLENDER_PATH));
    PageControl(3, AddControl(
        L"BUTTON", L"手动指定…", BS_OWNERDRAW,
        955, 542, 140, 36, ID_BROWSE_BLENDER));
    PageControl(3, Label(L"工程存储", 270, 608, 155, 24));
    PageControl(3, AddControl(
        L"EDIT", L"", ES_AUTOHSCROLL | ES_READONLY,
        445, 600, 500, 36, ID_PROJECT_STORAGE_PATH));
    PageControl(3, AddControl(
        L"BUTTON", L"更改位置…", BS_OWNERDRAW,
        955, 600, 140, 36, ID_CHANGE_PROJECT_STORAGE));

    // 所有页面共享的单一状态栏。任务预检、连接与设置反馈都在这里显示。
    Label(L"就绪", 250, 748, 890, 34, ID_PREFLIGHT, gFont,
          StaticSurfaceRole::Surface);
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
        if (!gUiMetricsReport.empty())
            SetWindowSubclass(control, UiResizeTestSubclassProc,
                              kUiResizeTestSubclassId, 0);
    }
}

void LayoutUi(bool updateColumns) {
    if (gBaseControlRects.empty() || gLayoutBaseClient.cx <= 0) return;
    RECT client{};
    GetClientRect(gWindow, &client);
    const int chromeTop = ChromeHeight(gWindow);
    constexpr int contentAnchor = 232;
    constexpr int rightPadding = 18;
    const int baseContent = std::max(1, static_cast<int>(
        gLayoutBaseClient.cx - contentAnchor - rightPadding));
    const int nextContent = std::max(1, static_cast<int>(
        client.right - contentAnchor - rightPadding));
    const double scale = static_cast<double>(nextContent) / baseContent;
    const int heightDelta = client.bottom - chromeTop - gLayoutBaseClient.cy;
    std::unordered_map<HWND, RECT> targets;
    targets.reserve(gBaseControlRects.size());
    for (const auto& [control, base] : gBaseControlRects) {
        if (!IsWindow(control)) continue;
        RECT next = base;
        next.top += chromeTop;
        next.bottom += chromeTop;
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
        next.right = next.left + std::max(24, static_cast<int>(next.right - next.left));
        next.bottom = next.top + std::max(20, static_cast<int>(next.bottom - next.top));
        targets[control] = next;
    }
    const int left = 250;
    const int right = std::max(left + 620, static_cast<int>(client.right - 24));
    const int width = right - left;
    auto move = [&](int id, int x, int y, int controlWidth, int height) {
        if (HWND control = Item(id))
            targets[control] = RECT{x, y, x + std::max(20, controlWidth), y + height};
    };
    const int taskListTop = chromeTop + 72;
    constexpr int listBottomMargin = 66;
    move(ID_JOBS, left, taskListTop, width,
         std::max(120, static_cast<int>(client.bottom - listBottomMargin - taskListTop)));
    move(ID_DROP_OVERLAY, left, taskListTop, width,
         std::max(120, static_cast<int>(client.bottom - listBottomMargin - taskListTop)));
    const int nodeListTop = chromeTop + 238;
    move(ID_NODES, left, nodeListTop, width,
         std::max(120, static_cast<int>(client.bottom - listBottomMargin - nodeListTop)));
    const int contributionListTop = chromeTop + 72;
    move(ID_CONTRIBUTION_LIST, left, contributionListTop, width,
         std::max(120, static_cast<int>(client.bottom - listBottomMargin -
                                        contributionListTop)));
    constexpr int dashboardCardGap = 12;
    constexpr int dashboardCardsLeft = 236;
    const int dashboardCardsRight = std::max(
        dashboardCardsLeft + 640, static_cast<int>(client.right - 18));
    const int dashboardCardWidth = std::max(
        160, (dashboardCardsRight - dashboardCardsLeft - dashboardCardGap * 3) / 4);
    const int matchingCardLeft =
        dashboardCardsLeft + 3 * (dashboardCardWidth + dashboardCardGap);
    constexpr int matchingCopyWidth = 58;
    move(ID_CLUSTER_TOKEN, matchingCardLeft + 14, chromeTop + 124,
         std::max(72, dashboardCardWidth - matchingCopyWidth - 40), 38);
    move(ID_COPY_TOKEN, matchingCardLeft + dashboardCardWidth - matchingCopyWidth - 12,
         chromeTop + 124, matchingCopyWidth, 38);
    move(ID_PREFLIGHT, left, client.bottom - 54, width, 34);
    move(ID_ADD_JOB, right - 66, client.bottom - 124, 58, 58);
    move(ID_NAV_SETTINGS, 18, client.bottom - 62, 42, 42);
    std::vector<std::pair<HWND, RECT>> orderedTargets;
    orderedTargets.reserve(targets.size());
    for (const auto& target : targets) orderedTargets.push_back(target);
    ApplyChildLayout(gWindow, orderedTargets);
    if (updateColumns) FitMainListColumns();
    if (HWND addJob = Item(ID_ADD_JOB))
        SetWindowPos(addJob, HWND_TOP, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    if (gDropOverlayVisible)
        if (HWND overlay = Item(ID_DROP_OVERLAY))
            SetWindowPos(overlay, HWND_TOP, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    if (gInternalDialog && IsWindow(gInternalDialog)) {
        RECT client{};
        GetClientRect(gWindow, &client);
        SetWindowPos(gInternalDialog, HWND_TOP, 0, 0, client.right, client.bottom,
                     SWP_NOACTIVATE | SWP_SHOWWINDOW);
    }
    if (gWorkflowProgressOverlay && IsWindow(gWorkflowProgressOverlay)) {
        RECT client{};
        GetClientRect(gWindow, &client);
        const int chrome = ChromeHeight(gWindow);
        SetWindowPos(gWorkflowProgressOverlay, HWND_TOP, 0, chrome,
                     client.right, std::max(1L, client.bottom - chrome), SWP_SHOWWINDOW);
    }
}

RECT DetailStopButtonRect(const RECT& client) {
    constexpr int margin = 20;
    constexpr int size = 54;
    return {client.right - margin - size, client.bottom - margin - size,
            client.right - margin, client.bottom - margin};
}

struct DetailWindowLayout {
    RECT summary{};
    RECT frames{};
    RECT preview{};
    RECT previewStatus{};
    RECT openOutput{};
    RECT openSource{};
    RECT retryFailed{};
    RECT stop{};
    RECT fullRender{};
};

DetailWindowLayout CalculateDetailWindowLayout(
    const RECT& client, int chromeTop, bool primaryActionVisible, bool retryVisible) {
    constexpr int gap = 16;
    constexpr int margin = 20;
    constexpr int actionSize = 48;
    constexpr int actionGap = 10;
    const int availableWidth = std::max(960, static_cast<int>(client.right) - margin * 2);
    const int listWidth = std::min(690, std::max(520, availableWidth - gap - 520));
    const int rightX = margin + listWidth + gap;
    const int previewTop = chromeTop + 18;
    const int actionBottom = client.bottom - margin;
    const int actionTop = actionBottom - actionSize;
    const int summaryBottom = actionTop - 10;
    const int summaryTop = summaryBottom - 58;
    const int statusTop = summaryTop - 28;
    const int previewBottom = statusTop - 8;

    DetailWindowLayout layout;
    layout.summary = {rightX, summaryTop, client.right - margin, summaryBottom};
    layout.frames = {margin, chromeTop + 18, margin + listWidth,
                     std::max(chromeTop + 415, static_cast<int>(client.bottom - margin))};
    layout.preview = {rightX, previewTop, client.right - margin,
                      std::max(previewTop + 250, previewBottom)};
    layout.previewStatus = {rightX, statusTop, client.right - margin, statusTop + 24};

    int cursor = client.right - margin;
    if (primaryActionVisible) {
        layout.stop = {cursor - actionSize, actionTop, cursor, actionBottom};
        cursor -= actionSize + actionGap;
    } else {
        layout.stop = DetailStopButtonRect(client);
    }
    layout.fullRender = layout.stop;
    layout.openSource = {cursor - actionSize, actionTop, cursor, actionBottom};
    cursor -= actionSize + actionGap;
    layout.openOutput = {cursor - actionSize, actionTop, cursor, actionBottom};
    cursor -= actionSize + actionGap;
    if (retryVisible)
        layout.retryFailed = {std::max(rightX, cursor - 184), actionTop,
                              cursor, actionBottom};
    else
        layout.retryFailed = {cursor - 184, actionTop, cursor, actionBottom};
    return layout;
}

void LayoutDetailWindow(HWND window) {
    RECT client{};
    GetClientRect(window, &client);
    const int chromeTop = ChromeHeight(window);
    const bool primaryActionVisible =
        IsWindowVisible(GetDlgItem(window, ID_DETAIL_STOP)) != FALSE ||
        IsWindowVisible(GetDlgItem(window, ID_DETAIL_FULL_RENDER)) != FALSE;
    const auto layout = CalculateDetailWindowLayout(
        client, chromeTop, primaryActionVisible,
        IsWindowVisible(GetDlgItem(window, ID_DETAIL_RETRY_FAILED)) != FALSE);
    ApplyChildLayout(window, {
        {GetDlgItem(window, ID_DETAIL_SUMMARY), layout.summary},
        {GetDlgItem(window, ID_DETAIL_FRAMES), layout.frames},
        {GetDlgItem(window, ID_DETAIL_PREVIEW), layout.preview},
        {GetDlgItem(window, ID_DETAIL_PREVIEW_STATUS), layout.previewStatus},
        {GetDlgItem(window, ID_DETAIL_OPEN_OUTPUT), layout.openOutput},
        {GetDlgItem(window, ID_DETAIL_OPEN_SOURCE), layout.openSource},
        {GetDlgItem(window, ID_DETAIL_RETRY_FAILED), layout.retryFailed},
        {GetDlgItem(window, ID_DETAIL_STOP), layout.stop},
        {GetDlgItem(window, ID_DETAIL_FULL_RENDER), layout.fullRender},
    });
    FitListColumns(GetDlgItem(window, ID_DETAIL_FRAMES),
                   {8, 13, 18, 0, 13, 17, 17, 0},
                   {48, 76, 94, 0, 80, 104, 104, 0});
}

struct ShareControlLayout {
    RECT icon{};
    RECT track{};
};

ShareControlLayout CalculateShareControlLayout(HDC dc, const RECT& bounds) {
    (void)dc;
    constexpr int iconSize = 24;
    constexpr int trackWidth = 42;
    constexpr int trackHeight = 22;
    constexpr int gap = 14;
    ShareControlLayout layout;
    const int groupWidth = iconSize + gap + trackWidth;
    const int groupLeft = bounds.left + (bounds.right - bounds.left - groupWidth) / 2;
    const int iconTop = bounds.top + (bounds.bottom - bounds.top - iconSize) / 2;
    layout.icon = {groupLeft, iconTop, groupLeft + iconSize, iconTop + iconSize};
    const int trackTop = bounds.top + (bounds.bottom - bounds.top - trackHeight) / 2;
    layout.track = {layout.icon.right + gap, trackTop,
                    layout.icon.right + gap + trackWidth, trackTop + trackHeight};
    return layout;
}

void DrawOwnerButton(const DRAWITEMSTRUCT& item) {
    RECT rect = item.rcItem;
    int id = static_cast<int>(item.CtlID);
    if (id == ID_DROP_OVERLAY) {
        HBRUSH fill = CreateSolidBrush(gDarkTheme ? RGB(19, 22, 29) : RGB(237, 246, 241));
        FillRect(item.hDC, &rect, fill);
        DeleteObject(fill);
        HPEN border = CreatePen(PS_SOLID, 2, kChromeAccent);
        HGDIOBJ oldPen = SelectObject(item.hDC, border);
        HGDIOBJ oldBrush = SelectObject(item.hDC, GetStockObject(NULL_BRUSH));
        RoundRect(item.hDC, rect.left + 2, rect.top + 2,
                  rect.right - 2, rect.bottom - 2, 18, 18);
        SelectObject(item.hDC, oldBrush);
        SelectObject(item.hDC, oldPen);
        DeleteObject(border);
        SetBkMode(item.hDC, TRANSPARENT);
        SetTextColor(item.hDC, gDarkTheme ? RGB(239, 241, 240) : RGB(28, 42, 34));
        HGDIOBJ oldFont = SelectObject(item.hDC, gTitleFont);
        DrawTextW(item.hDC, L"拖放 .blend 文件到窗口内", -1, &rect,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        SelectObject(item.hDC, oldFont);
        return;
    }
    bool share = id == ID_SHARE;
    const bool settingToggle =
        id == ID_AUTO_START || id == ID_SILENT_AUTO_START;
    bool primary = id == ID_SUBMIT || id == ID_ADD_JOB ||
        id == ID_UPLOAD_NEXT || id == ID_CONFIG_SUBMIT;
    const bool stopAction = id == ID_DETAIL_STOP;
    const bool fullRenderAction = id == ID_DETAIL_FULL_RENDER;
    bool danger = stopAction || id == ID_CONFIG_PROFILE_DELETE ||
        (id == ID_INTERNAL_CANCEL &&
         gInternalDialogMode == InternalDialogMode::CloseBehavior) ||
        (id == ID_INTERNAL_CONFIRM &&
         gInternalDialogMode != InternalDialogMode::Notice &&
         gInternalDialogMode != InternalDialogMode::UpdateAvailable &&
         gInternalDialogMode != InternalDialogMode::CloseBehavior);
    if (id == ID_INTERNAL_CONFIRM &&
        (gInternalDialogMode == InternalDialogMode::UpdateAvailable ||
         gInternalDialogMode == InternalDialogMode::CloseBehavior))
        primary = true;
    int navPage = id == ID_NAV_DASHBOARD ? 0 : id == ID_NAV_TASKS ? 1
        : id == ID_NAV_CONTRIBUTIONS ? 2 : id == ID_NAV_SETTINGS ? 3 : -1;
    bool selected = navPage == gCurrentPage;
    bool pressed = (item.itemState & ODS_SELECTED) != 0;
    bool disabled = (item.itemState & ODS_DISABLED) != 0;
    bool focused = (item.itemState & ODS_FOCUS) != 0;
    bool hovered = GetPropW(item.hwndItem, L"RM_HOVER") != nullptr;
    COLORREF surface = gDarkTheme ? RGB(31, 31, 40) : RGB(255, 255, 255);
    const bool internalDialogButton =
        GetPropW(item.hwndItem, kInternalDialogButtonProperty) != nullptr;
    COLORREF parentSurface = internalDialogButton ? surface : (share || navPage >= 0)
        ? (gDarkTheme ? RGB(23, 24, 32) : RGB(252, 252, 254))
        : (gDarkTheme ? RGB(15, 16, 22) : RGB(244, 245, 249));
    HBRUSH parentBrush = CreateSolidBrush(parentSurface);
    FillRect(item.hDC, &rect, parentBrush);
    DeleteObject(parentBrush);
    if (share) {
        Config config = gApp->GetConfig();
        const bool shareHovered = GetPropW(item.hwndItem, L"RM_HOVER") != nullptr;
        const COLORREF shareFill = shareHovered
            ? (gDarkTheme ? RGB(40, 43, 48) : RGB(237, 246, 241))
            : (gDarkTheme ? RGB(31, 31, 40) : RGB(255, 255, 255));
        HBRUSH shareBrush = CreateSolidBrush(shareFill);
        HPEN sharePen = CreatePen(
            PS_SOLID, 1, gDarkTheme ? RGB(61, 66, 69) : RGB(210, 218, 213));
        HGDIOBJ oldShareBrush = SelectObject(item.hDC, shareBrush);
        HGDIOBJ oldSharePen = SelectObject(item.hDC, sharePen);
        RoundRect(item.hDC, rect.left, rect.top, rect.right, rect.bottom, 12, 12);
        SelectObject(item.hDC, oldShareBrush);
        SelectObject(item.hDC, oldSharePen);
        DeleteObject(shareBrush);
        DeleteObject(sharePen);
        SetBkMode(item.hDC, TRANSPARENT);
        SetTextColor(item.hDC, config.shareEnabled ? kChromeAccent
            : gDarkTheme ? RGB(164, 166, 183) : RGB(103, 105, 119));
        HGDIOBJ oldFont = SelectObject(item.hDC, gIconFont);
        const ShareControlLayout layout = CalculateShareControlLayout(item.hDC, rect);
        RECT iconRect = layout.icon;
        DrawTextW(item.hDC, L"\xE774", -1, &iconRect,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        HBRUSH trackBrush = CreateSolidBrush(config.shareEnabled ? kChromeAccent
            : gDarkTheme ? RGB(91, 91, 103) : RGB(186, 188, 198));
        HGDIOBJ oldBrush = SelectObject(item.hDC, trackBrush);
        HGDIOBJ oldPen = SelectObject(item.hDC, GetStockObject(NULL_PEN));
        RoundRect(item.hDC, layout.track.left, layout.track.top,
                  layout.track.right, layout.track.bottom, 22, 22);
        SelectObject(item.hDC, oldBrush);
        DeleteObject(trackBrush);
        HBRUSH knob = CreateSolidBrush(RGB(255, 255, 255));
        oldBrush = SelectObject(item.hDC, knob);
        const int knobLeft = config.shareEnabled
            ? layout.track.right - 20 : layout.track.left + 2;
        Ellipse(item.hDC, knobLeft, layout.track.top + 2,
                knobLeft + 18, layout.track.top + 20);
        SelectObject(item.hDC, oldBrush);
        SelectObject(item.hDC, oldPen);
        DeleteObject(knob);
        SelectObject(item.hDC, oldFont);
        return;
    }
    if (settingToggle) {
        const bool checked = SendMessageW(
            item.hwndItem, BM_GETCHECK, 0, 0) == BST_CHECKED;
        SetBkMode(item.hDC, TRANSPARENT);
        SetTextColor(item.hDC, disabled
            ? (gDarkTheme ? RGB(108, 109, 122) : RGB(158, 160, 170))
            : hovered ? kChromeAccent
            : (gDarkTheme ? RGB(235, 236, 244) : RGB(42, 44, 52)));
        HGDIOBJ oldFont = SelectObject(item.hDC, gFont);
        RECT label = rect;
        label.left += 2;
        label.right -= 72;
        const std::wstring labelText = Text(item.hwndItem);
        DrawTextW(item.hDC, labelText.c_str(), -1, &label,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

        const int trackWidth = 44;
        const int trackHeight = 24;
        RECT track{
            rect.right - 14 - trackWidth,
            rect.top + (rect.bottom - rect.top - trackHeight) / 2,
            rect.right - 14,
            rect.top + (rect.bottom - rect.top + trackHeight) / 2,
        };
        const COLORREF trackFill = checked && !disabled
            ? kChromeAccent
            : gDarkTheme ? RGB(78, 79, 92) : RGB(187, 189, 199);
        HBRUSH trackBrush = CreateSolidBrush(trackFill);
        HGDIOBJ oldCardBrush = SelectObject(item.hDC, trackBrush);
        HGDIOBJ oldCardPen = SelectObject(item.hDC, GetStockObject(NULL_PEN));
        RoundRect(item.hDC, track.left, track.top,
                  track.right, track.bottom, trackHeight, trackHeight);
        SelectObject(item.hDC, oldCardBrush);
        SelectObject(item.hDC, oldCardPen);
        DeleteObject(trackBrush);
        HBRUSH knobBrush = CreateSolidBrush(RGB(255, 255, 255));
        oldCardBrush = SelectObject(item.hDC, knobBrush);
        const int knobLeft = checked
            ? track.right - trackHeight + 3 : track.left + 3;
        Ellipse(item.hDC, knobLeft, track.top + 3,
                knobLeft + trackHeight - 6, track.bottom - 3);
        SelectObject(item.hDC, oldCardBrush);
        DeleteObject(knobBrush);
        SelectObject(item.hDC, oldFont);
        return;
    }
    COLORREF hover = gDarkTheme ? RGB(45, 51, 49) : RGB(237, 246, 241);
    COLORREF fill = disabled ? (gDarkTheme ? RGB(38, 39, 43) : RGB(238, 239, 241))
        : stopAction ? (pressed ? RGB(166, 29, 45) : hovered ? RGB(235, 63, 78) : RGB(210, 45, 61))
        : fullRenderAction ? (pressed ? RGB(16, 118, 73)
            : hovered ? RGB(31, 181, 112) : RGB(22, 155, 96))
        : danger ? (pressed ? RGB(116, 36, 48) : hovered ? RGB(93, 38, 47) : RGB(69, 34, 41))
        : primary || selected ? (pressed ? RGB(57, 50, 194) : kAccent)
        : pressed || hovered ? hover : surface;
    COLORREF border = disabled ? (gDarkTheme ? RGB(54, 55, 61) : RGB(216, 218, 222))
        : stopAction ? RGB(244, 92, 105)
        : fullRenderAction ? RGB(69, 214, 145)
        : danger ? RGB(196, 72, 88)
        : primary || selected ? kAccent
        : focused ? RGB(126, 118, 255)
        : gDarkTheme ? RGB(61, 66, 69) : RGB(210, 218, 213);
    HBRUSH brush = CreateSolidBrush(fill);
    HPEN pen = CreatePen(PS_SOLID, focused ? 2 : 1, border);
    HGDIOBJ oldBrush = SelectObject(item.hDC, brush);
    HGDIOBJ oldPen = SelectObject(item.hDC, pen);
    RoundRect(item.hDC, rect.left, rect.top, rect.right, rect.bottom, 12, 12);
    SelectObject(item.hDC, oldBrush);
    SelectObject(item.hDC, oldPen);
    DeleteObject(brush);
    DeleteObject(pen);
    if (stopAction) {
        const int glyphSize = 14;
        RECT glyph{
            rect.left + (rect.right - rect.left - glyphSize) / 2,
            rect.top + (rect.bottom - rect.top - glyphSize) / 2,
            rect.left + (rect.right - rect.left + glyphSize) / 2,
            rect.top + (rect.bottom - rect.top + glyphSize) / 2,
        };
        HBRUSH glyphBrush = CreateSolidBrush(disabled
            ? RGB(176, 176, 182) : RGB(255, 255, 255));
        FillRect(item.hDC, &glyph, glyphBrush);
        DeleteObject(glyphBrush);
        return;
    }
    if (fullRenderAction) {
        SetBkMode(item.hDC, TRANSPARENT);
        SetTextColor(item.hDC, disabled ? RGB(176, 176, 182) : RGB(255, 255, 255));
        HGDIOBJ oldFont = SelectObject(item.hDC, gIconFont);
        DrawTextW(item.hDC, L"\xE768", -1, &rect,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        SelectObject(item.hDC, oldFont);
        return;
    }
    if (id == ID_DETAIL_OPEN_OUTPUT || id == ID_DETAIL_OPEN_SOURCE) {
        if (id == ID_DETAIL_OPEN_OUTPUT) {
            SetBkMode(item.hDC, TRANSPARENT);
            SetTextColor(item.hDC, disabled
                ? (gDarkTheme ? RGB(115, 118, 123) : RGB(145, 149, 153))
                : gDarkTheme ? RGB(239, 241, 240) : RGB(31, 38, 34));
            HGDIOBJ oldFont = SelectObject(item.hDC, gIconFont);
            DrawTextW(item.hDC, L"\xE8B9", -1, &rect,
                      DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            SelectObject(item.hDC, oldFont);
        } else {
            HICON icon = LoadIconW(GetModuleHandleW(nullptr),
                                   MAKEINTRESOURCEW(IDI_RENDERMESH));
            const int size = 22;
            DrawIconEx(item.hDC,
                       rect.left + (rect.right - rect.left - size) / 2,
                       rect.top + (rect.bottom - rect.top - size) / 2,
                       icon, size, size, 0, nullptr, DI_NORMAL);
        }
        return;
    }
    SetBkMode(item.hDC, TRANSPARENT);
    const bool iconButton = id == ID_ADD_JOB || id == ID_NAV_SETTINGS;
    HGDIOBJ oldFont = SelectObject(item.hDC, iconButton ? gIconFont : gFont);
    wchar_t text[128]{};
    GetWindowTextW(item.hwndItem, text, static_cast<int>(std::size(text)));
    SetTextColor(item.hDC, disabled ? (gDarkTheme ? RGB(115, 118, 123) : RGB(145, 149, 153))
        : primary || selected ? RGB(255, 255, 255)
        : danger ? RGB(255, 220, 224)
        : gDarkTheme ? RGB(239, 241, 240) : RGB(31, 38, 34));
    const wchar_t* displayText = id == ID_NAV_SETTINGS ? L"\xE713" : text;
    if (id == ID_CONFIG_PROFILE_SELECT &&
        GetPropW(item.hwndItem, kDropdownAffordanceProperty)) {
        RECT labelRect = rect;
        labelRect.left += 14;
        labelRect.right -= 48;
        DrawTextW(item.hDC, displayText, -1, &labelRect,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        RECT chevronRect = rect;
        chevronRect.left = chevronRect.right - 42;
        HGDIOBJ textFont = SelectObject(item.hDC, gIconFont);
        DrawTextW(item.hDC, L"\xE70D", -1, &chevronRect,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        SelectObject(item.hDC, textFont);
    } else {
        DrawTextW(item.hDC, displayText, -1, &rect,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
    SelectObject(item.hDC, oldFont);
}

void DrawPreviewControl(const DRAWITEMSTRUCT& item) {
    RECT rect = item.rcItem;
    const COLORREF surface = gDarkTheme ? RGB(20, 21, 28) : RGB(244, 245, 249);
    HBRUSH background = CreateSolidBrush(surface);
    FillRect(item.hDC, &rect, background);
    DeleteObject(background);
    if (!gPreviewBitmap) return;

    BITMAP bitmap{};
    if (!GetObjectW(gPreviewBitmap, sizeof(bitmap), &bitmap) ||
        bitmap.bmWidth <= 0 || bitmap.bmHeight <= 0)
        return;
    const int availableWidth = std::max(1, static_cast<int>(rect.right - rect.left));
    const int availableHeight = std::max(1, static_cast<int>(rect.bottom - rect.top));
    const double scale = std::min(
        static_cast<double>(availableWidth) / bitmap.bmWidth,
        static_cast<double>(availableHeight) / bitmap.bmHeight);
    const int width = std::max(1, static_cast<int>(bitmap.bmWidth * scale));
    const int height = std::max(1, static_cast<int>(bitmap.bmHeight * scale));
    const int left = rect.left + (availableWidth - width) / 2;
    const int top = rect.top + (availableHeight - height) / 2;
    HDC source = CreateCompatibleDC(item.hDC);
    HGDIOBJ oldBitmap = SelectObject(source, gPreviewBitmap);
    SetStretchBltMode(item.hDC, HALFTONE);
    StretchBlt(item.hDC, left, top, width, height, source, 0, 0,
               bitmap.bmWidth, bitmap.bmHeight, SRCCOPY);
    SelectObject(source, oldBitmap);
    DeleteDC(source);
}

RECT InternalDialogCardRect(HWND window) {
    RECT client{};
    GetClientRect(window, &client);
    const int width = std::min(620, std::max(420, static_cast<int>(client.right) - 64));
    const int height = gInternalDialogMode == InternalDialogMode::PreflightWarningOverride
        ? 390 : gInternalDialogMode == InternalDialogMode::CloseBehavior
            ? 340 : gInternalDialogMode == InternalDialogMode::BlenderMissing
                ? 370 : gInternalDialogMode == InternalDialogMode::BlenderVersionPicker
                    ? 390 : (gInternalDialogMode != InternalDialogMode::Notice ? 310 : 250);
    const int left = (client.right - width) / 2;
    const int top = std::max(24, (static_cast<int>(client.bottom) - height) / 2);
    return {left, top, left + width, top + height};
}

void LayoutInternalDialog(HWND window) {
    const RECT card = InternalDialogCardRect(window);
    HWND cancel = GetDlgItem(window, ID_INTERNAL_CANCEL);
    HWND confirm = GetDlgItem(window, ID_INTERNAL_CONFIRM);
    HWND remember = GetDlgItem(window, ID_INTERNAL_REMEMBER);
    HWND version = GetDlgItem(window, ID_INTERNAL_VERSION);
    const int buttonTop = card.bottom - 62;
    if (gInternalDialogMode == InternalDialogMode::Notice) {
        ShowWindow(cancel, SW_HIDE);
        MoveWindow(confirm, card.right - 154, buttonTop, 124, 40, TRUE);
        if (remember) ShowWindow(remember, SW_HIDE);
    } else if (gInternalDialogMode == InternalDialogMode::CloseBehavior) {
        ShowWindow(cancel, SW_SHOW);
        MoveWindow(cancel, card.right - 430, buttonTop, 190, 40, TRUE);
        MoveWindow(confirm, card.right - 230, buttonTop, 200, 40, TRUE);
        if (remember) {
            ShowWindow(remember, SW_SHOW);
            MoveWindow(remember, card.left + 30, card.bottom - 112, 220, 32, TRUE);
        }
    } else if (gInternalDialogMode == InternalDialogMode::BlenderMissing) {
        ShowWindow(cancel, SW_SHOW);
        MoveWindow(cancel, card.right - 298, buttonTop, 124, 40, TRUE);
        MoveWindow(confirm, card.right - 164, buttonTop, 134, 40, TRUE);
        if (remember) {
            ShowWindow(remember, SW_SHOW);
            MoveWindow(remember, card.left + 30, card.bottom - 112, 360, 32, TRUE);
        }
        if (version) ShowWindow(version, SW_HIDE);
    } else if (gInternalDialogMode == InternalDialogMode::BlenderVersionPicker) {
        ShowWindow(cancel, SW_SHOW);
        MoveWindow(cancel, card.right - 298, buttonTop, 124, 40, TRUE);
        MoveWindow(confirm, card.right - 164, buttonTop, 134, 40, TRUE);
        if (version) {
            ShowWindow(version, SW_SHOW);
            MoveWindow(version, card.left + 30, card.top + 150,
                       card.right - card.left - 60, 260, TRUE);
        }
        if (remember) {
            ShowWindow(remember, SW_SHOW);
            MoveWindow(remember, card.left + 30, card.bottom - 112, 360, 32, TRUE);
        }
    } else {
        ShowWindow(cancel, SW_SHOW);
        MoveWindow(cancel, card.right - 298, buttonTop, 124, 40, TRUE);
        MoveWindow(confirm, card.right - 164, buttonTop, 134, 40, TRUE);
        if (remember) ShowWindow(remember, SW_HIDE);
        if (version) ShowWindow(version, SW_HIDE);
    }
}

void CloseInternalDialog() {
    if (gInternalDialog && IsWindow(gInternalDialog)) DestroyWindow(gInternalDialog);
    gInternalDialog = nullptr;
    if (gInternalDialogBackdrop) {
        DeleteObject(gInternalDialogBackdrop);
        gInternalDialogBackdrop = nullptr;
    }
    gPendingDeleteJobIds.clear();
    gPendingContributionPackageIds.clear();
    if (HWND list = Item(gCurrentPage == 2 ? ID_CONTRIBUTION_LIST : ID_JOBS))
        SetFocus(list);
    if (std::exchange(gMainRefreshDeferredByModal, false) && gWindow)
        PostMessageW(gWindow, WM_RM_REFRESH, 0, 0);
}

HBITMAP CaptureInternalDialogBackdrop() {
    if (!gWindow) return nullptr;
    HideThreadMenuWindows();
    RECT client{};
    GetClientRect(gWindow, &client);
    const int width = std::max(1L, client.right);
    const int height = std::max(1L, client.bottom);
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(info.bmiHeader);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void* rawPixels = nullptr;
    HBITMAP bitmap = CreateDIBSection(
        nullptr, &info, DIB_RGB_COLORS, &rawPixels, nullptr, 0);
    if (!bitmap || !rawPixels) {
        if (bitmap) DeleteObject(bitmap);
        return nullptr;
    }
    HDC windowDc = GetDC(gWindow);
    HDC memoryDc = CreateCompatibleDC(windowDc);
    HGDIOBJ old = SelectObject(memoryDc, bitmap);
    RECT fill{0, 0, width, height};
    FillRect(memoryDc, &fill, gBackgroundBrush);
    constexpr UINT kRenderFullContent = 0x00000002;
    if (!PrintWindow(gWindow, memoryDc, PW_CLIENTONLY | kRenderFullContent))
        SendMessageW(gWindow, WM_PRINT, reinterpret_cast<WPARAM>(memoryDc),
                     PRF_CLIENT | PRF_CHILDREN | PRF_ERASEBKGND);
    SelectObject(memoryDc, old);
    DeleteDC(memoryDc);
    ReleaseDC(gWindow, windowDc);

    const COLORREF veil = gDarkTheme ? RGB(8, 9, 13) : RGB(205, 209, 218);
    constexpr int veilWeight = 58;
    auto* pixels = static_cast<BYTE*>(rawPixels);
    for (int index = 0; index < width * height; ++index) {
        BYTE* pixel = pixels + index * 4;
        pixel[0] = static_cast<BYTE>((pixel[0] * (100 - veilWeight) +
                                     GetBValue(veil) * veilWeight) / 100);
        pixel[1] = static_cast<BYTE>((pixel[1] * (100 - veilWeight) +
                                     GetGValue(veil) * veilWeight) / 100);
        pixel[2] = static_cast<BYTE>((pixel[2] * (100 - veilWeight) +
                                     GetRValue(veil) * veilWeight) / 100);
        pixel[3] = 0xFF;
    }
    return bitmap;
}

RECT WorkflowProgressCardRect(HWND window) {
    RECT client{};
    GetClientRect(window, &client);
    const int width = std::min(640, std::max(420, static_cast<int>(client.right) - 64));
    const int height = gWorkflowProgressCancelVisible ? 304 : 238;
    const int left = (client.right - width) / 2;
    const int top = std::max(24, (static_cast<int>(client.bottom) - height) / 2);
    return {left, top, left + width, top + height};
}

RECT WorkflowProgressDetailRect(HWND window) {
    const RECT card = WorkflowProgressCardRect(window);
    return {card.left + 30, card.top + 86, card.right - 30, card.top + 132};
}

void LayoutWorkflowProgress(HWND window) {
    const RECT card = WorkflowProgressCardRect(window);
    if (HWND progress = GetDlgItem(window, ID_WORKFLOW_PROGRESS))
        MoveWindow(progress, card.left + 34, card.top + 160,
                   card.right - card.left - 68, 10, TRUE);
    if (HWND cancel = GetDlgItem(window, ID_WORKFLOW_CANCEL)) {
        MoveWindow(cancel, card.right - 34 - 132, card.top + 236, 132, 38, TRUE);
        ShowWindow(cancel, gWorkflowProgressCancelVisible ? SW_SHOW : SW_HIDE);
    }
}

bool WorkflowProgressCanBeCancelled() {
    return gWorkflowProgressStage == WorkflowProgressStage::Inspecting ||
        gWorkflowProgressStage == WorkflowProgressStage::CreatingTask ||
        gWorkflowProgressStage == WorkflowProgressStage::MigratingStorage;
}

void RefreshWorkflowCancellation(HWND window) {
    const auto elapsed = std::chrono::steady_clock::now() -
        gWorkflowProgressStartedAt;
    const bool visible = WorkflowProgressCanBeCancelled() &&
        (gWorkflowProgressStage == WorkflowProgressStage::MigratingStorage ||
         gWorkflowProgressCancelRequested || elapsed >= std::chrono::seconds(10));
    if (visible == gWorkflowProgressCancelVisible) return;
    gWorkflowProgressCancelVisible = visible;
    LayoutWorkflowProgress(window);
    InvalidateRect(window, nullptr, FALSE);
}

void SetWorkflowProgressStage(WorkflowProgressStage stage) {
    gWorkflowProgressStage = stage;
    switch (stage) {
        case WorkflowProgressStage::Inspecting:
            gWorkflowProgressTitle = L"正在读取并检查工程";
            gWorkflowProgressDetail =
                L"步骤 1/2 · 读取场景设置并检查外部素材，只打开一次工程";
            if (HWND sourceControl = Item(ID_SOURCE_PATH)) {
                std::error_code sizeError;
                const uint64_t bytes = fs::file_size(fs::path(Text(sourceControl)), sizeError);
                if (!sizeError && bytes >= 1024ULL * 1024ULL * 1024ULL)
                    gWorkflowProgressDetail = L"步骤 1/2 · 正在打开 " +
                        FormatTransferBytes(bytes) + L" 大型工程，读取可能需要数十秒";
            }
            gWorkflowProgressPercent = 20;
            break;
        case WorkflowProgressStage::CreatingTask:
            gWorkflowProgressTitle = L"正在创建渲染任务";
            gWorkflowProgressDetail = L"步骤 2/2 · 写入工程快照并同时计算校验值";
            gWorkflowProgressPercent = 60 + gImportPermille.load() * 38 / 1000;
            break;
        case WorkflowProgressStage::DownloadingUpdate:
            gWorkflowProgressTitle = L"正在下载 RenderMesh 更新";
            gWorkflowProgressDetail = L"从已验证的局域网节点下载并校验新版程序";
            gWorkflowProgressPercent = 58;
            break;
        case WorkflowProgressStage::InstallingBlender:
            gWorkflowProgressTitle = L"正在安装 Blender 5.2 LTS";
            gWorkflowProgressDetail = L"正在准备官方下载，请稍候";
            gWorkflowProgressPercent = 4;
            break;
        case WorkflowProgressStage::FetchingBlenderCatalog:
            gWorkflowProgressTitle = L"正在读取 Blender 官方版本";
            gWorkflowProgressDetail = L"正在检查官方镜像中的 Windows MSI 与 LTS 标记";
            gWorkflowProgressPercent = 22;
            break;
        case WorkflowProgressStage::ApplyingUpdate:
            gWorkflowProgressTitle = L"正在安全安装 RenderMesh 更新";
            gWorkflowProgressDetail = L"新版正在校验并接管；窗口仍可移动，请勿强制结束进程";
            gWorkflowProgressPercent = 86;
            break;
        case WorkflowProgressStage::MigratingStorage:
            gWorkflowProgressTitle = L"正在迁移工程存储";
            gWorkflowProgressDetail = L"复制并校验任务工程、缓存与中间文件";
            gWorkflowProgressPercent = gStorageMigrationPercent.load();
            break;
    }
    if (gWorkflowProgressOverlay && IsWindow(gWorkflowProgressOverlay)) {
        if (HWND progress = GetDlgItem(gWorkflowProgressOverlay, ID_WORKFLOW_PROGRESS))
            SendMessageW(progress, PBM_SETPOS, gWorkflowProgressPercent, 0);
        const RECT card = WorkflowProgressCardRect(gWorkflowProgressOverlay);
        InvalidateRect(gWorkflowProgressOverlay, &card, FALSE);
    }
}

void ShowWorkflowProgress(WorkflowProgressStage stage) {
    if (!gWindow) return;
    const bool creating = !gWorkflowProgressOverlay ||
        !IsWindow(gWorkflowProgressOverlay);
    if (creating) {
        gWorkflowProgressStartedAt = std::chrono::steady_clock::now();
        gWorkflowProgressCancelVisible = false;
        gWorkflowProgressCancelRequested = false;
    }
    SetWorkflowProgressStage(stage);
    if (creating) {
        UpdateWindow(gWindow);
        if (gWorkflowProgressBackdrop) DeleteObject(gWorkflowProgressBackdrop);
        gWorkflowProgressBackdrop = CaptureInternalDialogBackdrop();
        RECT client{};
        GetClientRect(gWindow, &client);
        const int chrome = ChromeHeight(gWindow);
        gWorkflowProgressOverlay = CreateWindowExW(
            0, L"RenderMeshWorkflowProgress", L"",
            WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS,
            0, chrome, client.right, std::max(1L, client.bottom - chrome), gWindow,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_WORKFLOW_OVERLAY)),
            GetModuleHandleW(nullptr), nullptr);
    }
    if (gWorkflowProgressOverlay && IsWindow(gWorkflowProgressOverlay)) {
        RECT client{};
        GetClientRect(gWindow, &client);
        const int chrome = ChromeHeight(gWindow);
        SetWindowPos(gWorkflowProgressOverlay, HWND_TOP, 0, chrome,
                     client.right, std::max(1L, client.bottom - chrome), SWP_SHOWWINDOW);
        if (HWND progress = GetDlgItem(gWorkflowProgressOverlay, ID_WORKFLOW_PROGRESS))
            SendMessageW(progress, PBM_SETPOS, gWorkflowProgressPercent, 0);
        SetFocus(gWorkflowProgressOverlay);
        InvalidateRect(gWorkflowProgressOverlay, nullptr, TRUE);
        UpdateWindow(gWorkflowProgressOverlay);
    }
}

void CloseWorkflowProgress() {
    const bool refreshDeferred =
        std::exchange(gMainRefreshDeferredByModal, false);
    if (gWorkflowProgressOverlay && IsWindow(gWorkflowProgressOverlay))
        DestroyWindow(gWorkflowProgressOverlay);
    gWorkflowProgressOverlay = nullptr;
    gWorkflowProgressStartedAt = {};
    gWorkflowProgressCancelVisible = false;
    gWorkflowProgressCancelRequested = false;
    if (gWorkflowProgressBackdrop) {
        DeleteObject(gWorkflowProgressBackdrop);
        gWorkflowProgressBackdrop = nullptr;
    }
    if (refreshDeferred && gWindow)
        PostMessageW(gWindow, WM_RM_REFRESH, 0, 0);
}

LRESULT CALLBACK WorkflowProgressProc(HWND window, UINT message,
                                      WPARAM wParam, LPARAM lParam) {
    switch (message) {
        case WM_CREATE: {
            HWND progress = CreateWindowExW(
                0, PROGRESS_CLASSW, L"", WS_CHILD | WS_VISIBLE | PBS_SMOOTH,
                0, 0, 100, 10, window,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_WORKFLOW_PROGRESS)),
                GetModuleHandleW(nullptr), nullptr);
            SetWindowTheme(progress, L"", L"");
            SendMessageW(progress, PBM_SETRANGE32, 0, 100);
            SendMessageW(progress, PBM_SETBKCOLOR, 0,
                         gDarkTheme ? RGB(54, 56, 70) : RGB(222, 224, 234));
            SendMessageW(progress, PBM_SETBARCOLOR, 0, kAccent);
            SendMessageW(progress, PBM_SETPOS, gWorkflowProgressPercent, 0);
            HWND cancel = CreateWindowExW(
                0, L"BUTTON",
                gWorkflowProgressStage == WorkflowProgressStage::MigratingStorage
                    ? L"取消迁移" : L"取消上传",
                WS_CHILD | BS_OWNERDRAW,
                0, 0, 132, 38, window,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_WORKFLOW_CANCEL)),
                GetModuleHandleW(nullptr), nullptr);
            SendMessageW(cancel, WM_SETFONT, reinterpret_cast<WPARAM>(gFont), TRUE);
            ShowWindow(cancel, SW_HIDE);
            SetTimer(window, TIMER_WORKFLOW_PROGRESS, 500, nullptr);
            LayoutWorkflowProgress(window);
            return 0;
        }
        case WM_SIZE:
            LayoutWorkflowProgress(window);
            InvalidateRect(window, nullptr, TRUE);
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT paint{};
            HDC dc = BeginPaint(window, &paint);
            RECT client{};
            GetClientRect(window, &client);
            if (gWorkflowProgressBackdrop) {
                BITMAP bitmap{};
                GetObjectW(gWorkflowProgressBackdrop, sizeof(bitmap), &bitmap);
                HDC source = CreateCompatibleDC(dc);
                HGDIOBJ old = SelectObject(source, gWorkflowProgressBackdrop);
                const int sourceTop = gWindow ? ChromeHeight(gWindow) : 0;
                StretchBlt(dc, 0, 0, client.right, client.bottom,
                           source, 0, sourceTop, bitmap.bmWidth,
                           std::max(1L, bitmap.bmHeight - sourceTop), SRCCOPY);
                SelectObject(source, old);
                DeleteDC(source);
            } else {
                FillRect(dc, &client, gBackgroundBrush);
            }
            const RECT card = WorkflowProgressCardRect(window);
            HBRUSH cardBrush = CreateSolidBrush(
                gDarkTheme ? RGB(31, 32, 42) : RGB(255, 255, 255));
            HPEN borderPen = CreatePen(
                PS_SOLID, 1, gDarkTheme ? RGB(70, 72, 88) : RGB(204, 207, 218));
            HGDIOBJ oldBrush = SelectObject(dc, cardBrush);
            HGDIOBJ oldPen = SelectObject(dc, borderPen);
            RoundRect(dc, card.left, card.top, card.right, card.bottom, 18, 18);
            SelectObject(dc, oldPen);
            SelectObject(dc, oldBrush);
            DeleteObject(borderPen);
            DeleteObject(cardBrush);

            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, gDarkTheme ? RGB(245, 245, 251) : RGB(28, 28, 36));
            HGDIOBJ oldFont = SelectObject(dc, gTitleFont);
            RECT title{card.left + 34, card.top + 36, card.right - 34, card.top + 76};
            DrawTextW(dc, gWorkflowProgressTitle.c_str(), -1, &title,
                      DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
            SelectObject(dc, gFont);
            SetTextColor(dc, gDarkTheme ? RGB(188, 190, 204) : RGB(91, 93, 108));
            RECT detail{card.left + 34, card.top + 91, card.right - 34, card.top + 126};
            DrawTextW(dc, gWorkflowProgressDetail.c_str(), -1, &detail,
                      DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
            RECT hint{card.left + 34, card.top + 184, card.right - 34, card.top + 228};
            const wchar_t* hintText = nullptr;
            if (gWorkflowProgressStage == WorkflowProgressStage::MigratingStorage) {
                hintText = gWorkflowProgressCancelRequested
                    ? L"正在安全取消迁移，当前目录保持不变。"
                    : L"校验完成前不会切换目录；可随时取消并继续使用原目录。";
            } else {
                hintText = gWorkflowProgressCancelVisible
                    ? (gWorkflowProgressCancelRequested
                        ? L"正在取消本次上传，请等待当前文件操作安全结束。"
                        : L"可以继续等待，也可以随时取消本次工程导入；取消后不会创建任务。")
                    : L"处理完成后会自动进入下一步，请稍候。";
            }
            DrawTextW(dc, hintText, -1, &hint,
                       DT_LEFT | DT_VCENTER |
                           (gWorkflowProgressCancelVisible ? DT_WORDBREAK : DT_SINGLELINE) |
                           DT_NOPREFIX);
            SelectObject(dc, oldFont);
            EndPaint(window, &paint);
            return 0;
        }
        case WM_TIMER:
            if (wParam == TIMER_WORKFLOW_PROGRESS) {
                RefreshWorkflowCancellation(window);
                if (gWorkflowProgressStage == WorkflowProgressStage::MigratingStorage ||
                    gWorkflowProgressStage == WorkflowProgressStage::CreatingTask) {
                    const int percent =
                        gWorkflowProgressStage == WorkflowProgressStage::CreatingTask
                        ? 60 + gImportPermille.load() * 38 / 1000
                        : gStorageMigrationPercent.load();
                    if (percent != gWorkflowProgressPercent) {
                        gWorkflowProgressPercent = percent;
                        SendMessageW(GetDlgItem(window, ID_WORKFLOW_PROGRESS),
                                     PBM_SETPOS, percent, 0);
                    }
                }
            }
            return 0;
        case WM_COMMAND:
            if (LOWORD(wParam) == ID_WORKFLOW_CANCEL &&
                !gWorkflowProgressCancelRequested && WorkflowProgressCanBeCancelled()) {
                gWorkflowProgressCancelRequested = true;
                if (gWorkflowProgressStage == WorkflowProgressStage::MigratingStorage)
                    gJobOperationCancel = true;
                else
                    gPreflightStop = true;
                EnableWindow(GetDlgItem(window, ID_WORKFLOW_CANCEL), FALSE);
                gWorkflowProgressDetail =
                    gWorkflowProgressStage == WorkflowProgressStage::MigratingStorage
                    ? L"正在安全停止复制与校验……"
                    : L"正在安全停止工程读取、打包或校验……";
                InvalidateRect(window, nullptr, FALSE);
                SetStatusText(
                    gWorkflowProgressStage == WorkflowProgressStage::MigratingStorage
                    ? L"正在取消工程存储迁移……"
                    : L"正在取消本次工程上传……");
            }
            return 0;
        case WM_DRAWITEM:
            DrawOwnerButton(*reinterpret_cast<DRAWITEMSTRUCT*>(lParam));
            return TRUE;
        case WM_GETDLGCODE:
            return DLGC_WANTALLKEYS;
        case WM_KEYDOWN:
        case WM_SYSKEYDOWN:
        case WM_LBUTTONDOWN:
        case WM_LBUTTONUP:
        case WM_RBUTTONDOWN:
        case WM_RBUTTONUP:
        case WM_MOUSEWHEEL:
            return 0;
        case WM_SETCURSOR:
            SetCursor(reinterpret_cast<HWND>(wParam) ==
                    GetDlgItem(window, ID_WORKFLOW_CANCEL)
                ? LoadCursorW(nullptr, IDC_ARROW)
                : LoadCursorW(nullptr, IDC_WAIT));
            return TRUE;
        case WM_DESTROY:
            KillTimer(window, TIMER_WORKFLOW_PROGRESS);
            if (gWorkflowProgressOverlay == window) gWorkflowProgressOverlay = nullptr;
            return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

void ShowInternalDialog(InternalDialogMode mode, std::wstring title,
                        std::wstring subject, std::wstring body) {
    if (!gWindow) return;
    if (gInternalDialog && IsWindow(gInternalDialog)) DestroyWindow(gInternalDialog);
    gInternalDialogMode = mode;
    gInternalDialogTitle = std::move(title);
    gInternalDialogSubject = std::move(subject);
    gInternalDialogBody = std::move(body);
    UpdateWindow(gWindow);
    if (gInternalDialogBackdrop) DeleteObject(gInternalDialogBackdrop);
    gInternalDialogBackdrop = CaptureInternalDialogBackdrop();
    RECT client{};
    GetClientRect(gWindow, &client);
    gInternalDialog = CreateWindowExW(
        0, L"RenderMeshInternalDialog", L"", WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS,
        0, 0, client.right, client.bottom, gWindow,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_INTERNAL_DIALOG)),
        GetModuleHandleW(nullptr), nullptr);
    if (gInternalDialog) {
        SetWindowPos(gInternalDialog, HWND_TOP, 0, 0, client.right, client.bottom,
                     SWP_SHOWWINDOW | SWP_NOACTIVATE);
        InvalidateRect(gInternalDialog, nullptr, TRUE);
    }
}

void ShowDeleteConfirmation(const RenderJob& job) {
    gPendingDeleteJobIds = {job.id};
    ShowInternalDialog(
        InternalDialogMode::DeleteConfirmation, L"删除作业",
        Utf8ToWide(job.name),
        L"将停止所有未完成帧，删除 RenderMesh 托管的工程副本与渲染序列，"
        L"并通知已接收任务的远端节点。\n\n原始 .blend 文件和已另存到用户目录的结果不会被删除。");
}

void ShowInternalNotice(std::wstring title, std::wstring body) {
    ShowInternalDialog(InternalDialogMode::Notice, std::move(title), L"",
                       std::move(body));
}

void ShowPreflightWarningOverride(std::wstring body) {
    // Keep the worker-only test-frame confirmation explicit in this override.
    body += L"\n\n选择“无视警告”后，RenderMesh 会跳过全部预检警告，直接准备工程并进入上传/参数设置。真实出图仍由参与电脑上的测试帧确认；只有源工程无法读取或复制时才不能继续。";
    ShowInternalDialog(InternalDialogMode::PreflightWarningOverride,
                       L"工程预检警告", L"", std::move(body));
}

void SetUpdateCheckButtonBusy(bool busy) {
    HWND button = Item(ID_CHECK_UPDATE);
    if (!button) return;
    SetWindowTextW(button, busy ? L"正在检查…" : L"检查更新");
    EnableWindow(button, busy ? FALSE : TRUE);
    InvalidateRect(button, nullptr, FALSE);
}

void StartLanUpdateCheck(bool manual) {
    if (gDisableUpdateChecks || gUiShuttingDown.load() || !gApp ||
        gUpdateDownloadRunning.load() || gUpdateApplying) {
        if (manual && !gUiShuttingDown.load())
            ShowInternalNotice(
                L"无法检查更新", L"当前运行模式已禁用局域网更新检查。");
        return;
    }
    if (manual) {
        gManualUpdateCheck = true;
        gSkippedUpdateVersion.clear();
        SetUpdateCheckButtonBusy(true);
        SetStatusText(L"正在检查局域网中的新版本…");
    }
    if (gUpdateCheckRunning.load() || gUpdateCheckThread.joinable()) return;
    gUpdateCancel = false;
    gUpdateCheckRunning = true;
    gUpdateCheckThread = std::thread([] {
        auto result = std::make_unique<UpdateCheckResult>();
        try {
            result->offer = gApp->CheckForLanUpdate(result->error, &gUpdateCancel);
        } catch (const std::exception& exception) {
            result->error = L"局域网更新检查异常：" + Utf8ToWide(exception.what());
            LogError("LAN update check exception: " + std::string(exception.what()));
        } catch (...) {
            result->error = L"局域网更新检查发生未知异常";
            LogError("LAN update check unknown exception");
        }
        gUpdateCheckRunning = false;
        if (!gUiShuttingDown.load() && gWindow &&
            PostMessageW(gWindow, WM_RM_UPDATE_CHECKED, 0,
                         reinterpret_cast<LPARAM>(result.get())))
            result.release();
    });
}

void MaybeShowLanUpdateOffer() {
    if (!gPendingUpdateOffer || gUpdateApplying || gUpdateDownloadRunning.load() ||
        gPendingUpdateOffer->version == gSkippedUpdateVersion ||
        gInternalDialog || gWorkflowProgressOverlay || gPreflightRunning.load() ||
        gNewJobFlow)
        return;
    const UpdateOffer& offer = *gPendingUpdateOffer;
    const std::wstring source = offer.nodeName.empty()
        ? Utf8ToWide(offer.address) : Utf8ToWide(offer.nodeName);
    ShowInternalDialog(
        InternalDialogMode::UpdateAvailable,
        L"发现新版 RenderMesh",
        Utf8ToWide(kRenderMeshAppVersion) + L" → " + Utf8ToWide(offer.version),
        L"局域网电脑“" + source + L"”正在运行新版 RenderMesh。\n\n"
        L"更新包会从该已验证节点下载，并校验版本、大小和 SHA-256；安装失败会自动恢复当前版本。建议所有渲染电脑使用最新版本。"
    );
}

void StartLanUpdateDownload(const UpdateOffer& offer) {
    if (gUpdateDownloadRunning.load() || gUpdateDownloadThread.joinable() ||
        gUpdateApplying || !gApp)
        return;
    gPendingUpdateOffer.reset();
    gUpdateCancel = false;
    gUpdateDownloadRunning = true;
    ShowWorkflowProgress(WorkflowProgressStage::DownloadingUpdate);
    gUpdateDownloadThread = std::thread([offer] {
        auto result = std::make_unique<UpdateDownloadResult>();
        result->offer = offer;
        result->ok = gApp->DownloadLanUpdate(
            offer, result->pendingExecutable, result->error, &gUpdateCancel);
        gUpdateDownloadRunning = false;
        if (!gUiShuttingDown.load() && gWindow &&
            PostMessageW(gWindow, WM_RM_UPDATE_DOWNLOADED, 0,
                         reinterpret_cast<LPARAM>(result.get())))
            result.release();
    });
}

void StopLanUpdateThreads() {
    gUpdateCancel = true;
    if (gUpdateCheckThread.joinable()) gUpdateCheckThread.join();
    if (gUpdateDownloadThread.joinable()) gUpdateDownloadThread.join();
    if (gUpdateApplyThread.joinable()) gUpdateApplyThread.join();
    MSG stale{};
    while (gWindow && PeekMessageW(
               &stale, gWindow, WM_RM_UPDATE_CHECKED,
               WM_RM_UPDATE_CHECKED, PM_REMOVE))
        delete reinterpret_cast<UpdateCheckResult*>(stale.lParam);
    while (gWindow && PeekMessageW(
               &stale, gWindow, WM_RM_UPDATE_DOWNLOADED,
               WM_RM_UPDATE_DOWNLOADED, PM_REMOVE))
        delete reinterpret_cast<UpdateDownloadResult*>(stale.lParam);
    while (gWindow && PeekMessageW(
               &stale, gWindow, WM_RM_UPDATE_APPLIED,
               WM_RM_UPDATE_APPLIED, PM_REMOVE))
        delete reinterpret_cast<UpdateApplyResult*>(stale.lParam);
}

bool SaveInstalledBlenderProfile(const BlenderInstallResult& result,
                                 std::wstring& error) {
    if (!gApp || !result.ok) {
        error = L"Blender 安装结果无效";
        return false;
    }
    if (result.executable.empty()) return true;
    Config config = gApp->GetConfig();
    std::string installedProfileId =
        "rendermesh-blender-" + result.release.version;
    std::replace(installedProfileId.begin(), installedProfileId.end(), '.', '-');
    auto profile = std::find_if(
        config.renderSoftwareProfiles.begin(), config.renderSoftwareProfiles.end(),
        [&](const RenderSoftwareProfile& value) {
            return value.id == installedProfileId;
        });
    RenderSoftwareProfile installed{
        installedProfileId,
        "Blender " + result.release.version + (result.release.lts ? " LTS" : ""),
        "Windows", "Blender",
        result.executable};
    if (profile == config.renderSoftwareProfiles.end())
        config.renderSoftwareProfiles.push_back(installed);
    else
        *profile = installed;
    config.lastRenderSoftwareProfileId = installedProfileId;
    config.blenderPath = result.executable;
    return gApp->UpdateConfig(std::move(config), error);
}

void MaybeShowBlenderInstallPrompt() {
    if (gBlenderInstallPromptShown || gBlenderInstallRunning.load() || !gApp ||
        !gUiMetricsReport.empty() || gInternalDialog || gWorkflowProgressOverlay ||
        gPreflightRunning.load() || gNewJobFlow)
        return;
    if (!LocalRendererPath(gApp->GetConfig()).empty()) return;
    gBlenderInstallPromptShown = true;
    gBlenderInstallNvidiaAvailable = HasNvidiaGpu(QueryHardwareInfo());
    std::wstring body =
        L"当前电脑未检测到可用的 Blender，无法接收或提交渲染任务。"
        L"是否要先安装 Blender？\n\n"
        L"RenderMesh 会先读取 Blender 官方镜像中的近期 Windows MSI 版本，"
        L"由你选择版本；下载后校验官方 SHA-256，再启动官方安装程序。";
    if (!gBlenderInstallNvidiaAvailable)
        body += L"\n\n当前没有检测到 NVIDIA 显卡，NVIDIA 默认配置选项不可用。";
    ShowInternalDialog(InternalDialogMode::BlenderMissing,
                       L"检测到没有安装 Blender", L"", std::move(body));
}

void StartBlenderCatalogFetch(bool configureOptix) {
    if (!gApp || gBlenderInstallRunning.load() || gBlenderInstallThread.joinable())
        return;
    gBlenderInstallNvidiaAvailable = HasNvidiaGpu(QueryHardwareInfo());
    gBlenderRequestedOptix = configureOptix && gBlenderInstallNvidiaAvailable;
    gBlenderInstallCancel = false;
    gBlenderInstallRunning = true;
    ShowWorkflowProgress(WorkflowProgressStage::FetchingBlenderCatalog);
    gBlenderInstallThread = std::thread([] {
        auto result = std::make_unique<BlenderCatalogResult>();
        FetchOfficialBlenderCatalog(
            result->releases, gBlenderInstallCancel, result->error);
        gBlenderInstallRunning = false;
        if (!gUiShuttingDown.load() && gWindow &&
            PostMessageW(gWindow, WM_RM_BLENDER_CATALOG, 0,
                         reinterpret_cast<LPARAM>(result.get())))
            result.release();
    });
}

void StartBlenderInstall(const BlenderRelease& release, bool configureOptix) {
    if (!gApp || gBlenderInstallRunning.load() || gBlenderInstallThread.joinable())
        return;
    const std::wstring selected = ChooseFolder(
        L"选择 Blender MSI 下载位置", DefaultBlenderDownloadDirectory());
    if (selected.empty()) return;
    const fs::path downloadDirectory(selected);
    std::wstring validationError;
    if (!ValidateBlenderDownloadDirectory(downloadDirectory, validationError)) {
        ShowInternalNotice(L"无法安装 Blender", validationError);
        return;
    }
    const fs::path dataDir = gApp->DataDir();
    gBlenderInstallCancel = false;
    gBlenderInstallRunning = true;
    ShowWorkflowProgress(WorkflowProgressStage::InstallingBlender);
    gBlenderInstallThread = std::thread(
        [dataDir, downloadDirectory, release, configureOptix] {
            auto result = std::make_unique<BlenderInstallResult>(
                InstallOfficialBlender(
                    release, dataDir, downloadDirectory, configureOptix,
                    gBlenderInstallCancel,
                    [](const BlenderInstallProgress& progress) {
                        auto update = std::make_unique<BlenderInstallProgress>(progress);
                        if (!gUiShuttingDown.load() && gWindow &&
                            PostMessageW(
                                gWindow, WM_RM_BLENDER_INSTALL_PROGRESS, 0,
                                reinterpret_cast<LPARAM>(update.get())))
                            update.release();
                    }));
            gBlenderInstallRunning = false;
            if (!gUiShuttingDown.load() && gWindow &&
                PostMessageW(gWindow, WM_RM_BLENDER_INSTALLED, 0,
                             reinterpret_cast<LPARAM>(result.get())))
                result.release();
        });
}

void StopBlenderInstallThread() {
    gBlenderInstallCancel = true;
    if (gBlenderInstallThread.joinable()) gBlenderInstallThread.join();
    MSG stale{};
    while (gWindow && PeekMessageW(
               &stale, gWindow, WM_RM_BLENDER_INSTALL_PROGRESS,
               WM_RM_BLENDER_CATALOG, PM_REMOVE)) {
        if (stale.message == WM_RM_BLENDER_INSTALL_PROGRESS)
            delete reinterpret_cast<BlenderInstallProgress*>(stale.lParam);
        else if (stale.message == WM_RM_BLENDER_INSTALLED)
            delete reinterpret_cast<BlenderInstallResult*>(stale.lParam);
        else if (stale.message == WM_RM_BLENDER_CATALOG)
            delete reinterpret_cast<BlenderCatalogResult*>(stale.lParam);
    }
}

void DrawInternalBlenderVersionItem(const DRAWITEMSTRUCT& item) {
    if (!item.hDC || !item.hwndItem) return;
    RECT rect = item.rcItem;
    const bool listSelection =
        (item.itemState & ODS_SELECTED) != 0 &&
        (item.itemState & ODS_COMBOBOXEDIT) == 0;
    const bool disabled = (item.itemState & ODS_DISABLED) != 0;
    const COLORREF surface = gDarkTheme ? RGB(31, 32, 42) : RGB(255, 255, 255);
    const COLORREF selected = gDarkTheme ? RGB(68, 62, 145) : RGB(229, 227, 255);
    const COLORREF text = disabled
        ? (gDarkTheme ? RGB(110, 112, 126) : RGB(154, 157, 169))
        : (gDarkTheme ? RGB(244, 244, 249) : RGB(28, 29, 36));
    HBRUSH background = CreateSolidBrush(listSelection ? selected : surface);
    FillRect(item.hDC, &rect, background);
    DeleteObject(background);

    LRESULT selectedIndex = item.itemID == static_cast<UINT>(-1)
        ? SendMessageW(item.hwndItem, CB_GETCURSEL, 0, 0)
        : static_cast<LRESULT>(item.itemID);
    std::wstring label;
    if (selectedIndex != CB_ERR) {
        const LRESULT length = SendMessageW(
            item.hwndItem, CB_GETLBTEXTLEN, selectedIndex, 0);
        if (length >= 0) {
            label.resize(static_cast<size_t>(length) + 1);
            SendMessageW(item.hwndItem, CB_GETLBTEXT, selectedIndex,
                         reinterpret_cast<LPARAM>(label.data()));
            label.resize(static_cast<size_t>(length));
        }
    }
    SetBkMode(item.hDC, TRANSPARENT);
    SetTextColor(item.hDC, text);
    HGDIOBJ oldFont = SelectObject(item.hDC, gFont);
    rect.left += 12;
    rect.right -= 8;
    DrawTextW(item.hDC, label.c_str(), -1, &rect,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS |
                  DT_NOPREFIX);
    if ((item.itemState & ODS_FOCUS) != 0 && listSelection) {
        RECT focus = item.rcItem;
        InflateRect(&focus, -3, -2);
        DrawFocusRect(item.hDC, &focus);
    }
    SelectObject(item.hDC, oldFont);
}

LRESULT CALLBACK InternalDialogProc(HWND window, UINT message, WPARAM wParam,
                                    LPARAM lParam) {
    switch (message) {
        case WM_CREATE: {
            auto addButton = [&](const wchar_t* text, int id) {
                HWND button = CreateWindowExW(
                    0, L"BUTTON", text, WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                    0, 0, 120, 40, window,
                    reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                    GetModuleHandleW(nullptr), nullptr);
                SendMessageW(button, WM_SETFONT, reinterpret_cast<WPARAM>(gFont), TRUE);
                SetPropW(button, kInternalDialogButtonProperty, reinterpret_cast<HANDLE>(1));
                ConfigureOwnerButton(button);
                return button;
            };
            HWND cancel = addButton(
                gInternalDialogMode == InternalDialogMode::UpdateAvailable
                    ? L"跳过本次"
                    : gInternalDialogMode == InternalDialogMode::BlenderMissing
                        ? L"暂不安装"
                    : gInternalDialogMode == InternalDialogMode::CloseBehavior
                        ? L"直接退出" : L"取消",
                ID_INTERNAL_CANCEL);
            HWND confirm = addButton(
                gInternalDialogMode == InternalDialogMode::DeleteConfirmation
                    ? L"删除作业"
                    : gInternalDialogMode ==
                            InternalDialogMode::ContributionDeleteConfirmation
                        ? L"清理记录"
                        : gInternalDialogMode ==
                                InternalDialogMode::PreflightWarningOverride
                            ? L"无视警告"
                            : gInternalDialogMode == InternalDialogMode::UpdateAvailable
                                ? L"立即更新"
                                : gInternalDialogMode == InternalDialogMode::BlenderMissing
                                    ? L"立即安装"
                                : gInternalDialogMode == InternalDialogMode::BlenderVersionPicker
                                    ? L"选择并下载"
                                : gInternalDialogMode == InternalDialogMode::CloseBehavior
                                    ? L"最小化到系统托盘" : L"知道了",
                ID_INTERNAL_CONFIRM);
            if (gInternalDialogMode == InternalDialogMode::CloseBehavior ||
                gInternalDialogMode == InternalDialogMode::BlenderMissing ||
                gInternalDialogMode == InternalDialogMode::BlenderVersionPicker) {
                HWND remember = CreateWindowExW(
                    0, L"BUTTON",
                    gInternalDialogMode == InternalDialogMode::BlenderMissing ||
                            gInternalDialogMode == InternalDialogMode::BlenderVersionPicker
                        ? L"导入默认渲染配置（NVIDIA 专用）"
                        : L"记住本次选择",
                    WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX | WS_TABSTOP,
                    0, 0, 220, 32, window,
                    reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_INTERNAL_REMEMBER)),
                    GetModuleHandleW(nullptr), nullptr);
                SendMessageW(remember, WM_SETFONT, reinterpret_cast<WPARAM>(gFont), TRUE);
                SetPropW(remember, kInternalDialogButtonProperty,
                         reinterpret_cast<HANDLE>(1));
                ConfigureOwnerButton(remember);
                if (gInternalDialogMode == InternalDialogMode::BlenderMissing ||
                    gInternalDialogMode == InternalDialogMode::BlenderVersionPicker) {
                    SendMessageW(remember, BM_SETCHECK,
                                 (gInternalDialogMode == InternalDialogMode::BlenderVersionPicker
                                      ? gBlenderRequestedOptix
                                      : gBlenderInstallNvidiaAvailable)
                                     ? BST_CHECKED : BST_UNCHECKED, 0);
                    EnableWindow(remember,
                                 gBlenderInstallNvidiaAvailable ? TRUE : FALSE);
                }
            }
            if (gInternalDialogMode == InternalDialogMode::BlenderVersionPicker) {
                HWND versions = CreateWindowExW(
                    0, WC_COMBOBOXW, L"",
                    WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_CLIPSIBLINGS |
                        CBS_DROPDOWNLIST | CBS_OWNERDRAWFIXED | CBS_HASSTRINGS,
                    0, 0, 300, 260, window,
                    reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_INTERNAL_VERSION)),
                    GetModuleHandleW(nullptr), nullptr);
                SendMessageW(versions, WM_SETFONT,
                             reinterpret_cast<WPARAM>(gFont), TRUE);
                SetWindowTheme(versions,
                               gDarkTheme ? L"DarkMode_CFD" : L"Explorer",
                               nullptr);
                ConfigureModernField(versions, WC_COMBOBOXW);
                SendMessageW(versions, CB_SETITEMHEIGHT, static_cast<WPARAM>(-1), 34);
                SendMessageW(versions, CB_SETITEMHEIGHT, 0, 34);
                for (const auto& release : gBlenderCatalog)
                    SendMessageW(versions, CB_ADDSTRING, 0,
                                 reinterpret_cast<LPARAM>(release.displayName.c_str()));
                if (!gBlenderCatalog.empty())
                    SendMessageW(versions, CB_SETCURSEL, 0, 0);
            }
            LayoutInternalDialog(window);
            SetFocus(gInternalDialogMode == InternalDialogMode::Notice ||
                     gInternalDialogMode == InternalDialogMode::CloseBehavior ||
                     gInternalDialogMode == InternalDialogMode::BlenderMissing
                ? confirm
                : gInternalDialogMode == InternalDialogMode::BlenderVersionPicker
                    ? GetDlgItem(window, ID_INTERNAL_VERSION) : cancel);
            return 0;
        }
        case WM_SIZE:
            LayoutInternalDialog(window);
            return 0;
        case WM_PAINT: {
            PAINTSTRUCT paint{};
            HDC dc = BeginPaint(window, &paint);
            const COLORREF veil = gDarkTheme ? RGB(8, 9, 13) : RGB(218, 221, 228);
            const COLORREF surface = gDarkTheme ? RGB(31, 32, 42) : RGB(255, 255, 255);
            const COLORREF stroke = gDarkTheme ? RGB(61, 63, 76) : RGB(207, 211, 220);
            const COLORREF text = gDarkTheme ? RGB(244, 244, 249) : RGB(28, 29, 36);
            const COLORREF secondary = gDarkTheme ? RGB(178, 180, 194) : RGB(91, 94, 107);
            RECT client{};
            GetClientRect(window, &client);
            if (gInternalDialogBackdrop) {
                BITMAP bitmap{};
                GetObjectW(gInternalDialogBackdrop, sizeof(bitmap), &bitmap);
                HDC source = CreateCompatibleDC(dc);
                HGDIOBJ oldBitmap = SelectObject(source, gInternalDialogBackdrop);
                SetStretchBltMode(dc, HALFTONE);
                StretchBlt(dc, 0, 0, client.right, client.bottom, source, 0, 0,
                           bitmap.bmWidth, bitmap.bmHeight, SRCCOPY);
                SelectObject(source, oldBitmap);
                DeleteDC(source);
            } else {
                HBRUSH veilBrush = CreateSolidBrush(veil);
                FillRect(dc, &client, veilBrush);
                DeleteObject(veilBrush);
            }
            const RECT card = InternalDialogCardRect(window);
            HBRUSH cardBrush = CreateSolidBrush(surface);
            HPEN cardPen = CreatePen(PS_SOLID, 1, stroke);
            HGDIOBJ oldBrush = SelectObject(dc, cardBrush);
            HGDIOBJ oldPen = SelectObject(dc, cardPen);
            RoundRect(dc, card.left, card.top, card.right, card.bottom, 18, 18);
            SelectObject(dc, oldBrush);
            SelectObject(dc, oldPen);
            DeleteObject(cardBrush);
            DeleteObject(cardPen);

            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, text);
            HGDIOBJ oldFont = SelectObject(dc, gTitleFont);
            RECT title{card.left + 78, card.top + 28, card.right - 30, card.top + 64};
            DrawTextW(dc, gInternalDialogTitle.c_str(), -1, &title,
                      DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

            const int iconX = card.left + 42;
            const int iconY = card.top + 46;
            const bool informational =
                gInternalDialogMode == InternalDialogMode::Notice ||
                gInternalDialogMode == InternalDialogMode::UpdateAvailable ||
                gInternalDialogMode == InternalDialogMode::BlenderMissing ||
                gInternalDialogMode == InternalDialogMode::BlenderVersionPicker ||
                gInternalDialogMode == InternalDialogMode::CloseBehavior;
            HBRUSH iconBrush = CreateSolidBrush(
                informational ? RGB(25, 83, 59) : RGB(111, 37, 48));
            HPEN iconPen = CreatePen(PS_SOLID, 1,
                informational ? kChromeAccent : RGB(220, 88, 103));
            oldBrush = SelectObject(dc, iconBrush);
            oldPen = SelectObject(dc, iconPen);
            Ellipse(dc, iconX - 20, iconY - 20, iconX + 20, iconY + 20);
            SelectObject(dc, oldBrush);
            SelectObject(dc, oldPen);
            DeleteObject(iconBrush);
            DeleteObject(iconPen);
            SelectObject(dc, gFont);
            SetTextColor(dc, RGB(255, 235, 238));
            RECT glyph{iconX - 20, iconY - 20, iconX + 20, iconY + 20};
            DrawTextW(dc,
                informational ? L"i" : L"!",
                1, &glyph, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

            if (!gInternalDialogSubject.empty()) {
                SetTextColor(dc, text);
                RECT subject{card.left + 30, card.top + 88, card.right - 30, card.top + 122};
                const std::wstring line =
                    (gInternalDialogMode ==
                         InternalDialogMode::ContributionDeleteConfirmation
                         ? L"记录："
                         : gInternalDialogMode == InternalDialogMode::UpdateAvailable
                             ? L"版本：" : L"作业：") + gInternalDialogSubject;
                DrawTextW(dc, line.c_str(), -1, &subject,
                          DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            }
            SetTextColor(dc, secondary);
            const int bodyBottom =
                gInternalDialogMode == InternalDialogMode::BlenderVersionPicker
                    ? card.top + 140
                    : card.bottom -
                        (gInternalDialogMode == InternalDialogMode::CloseBehavior ||
                         gInternalDialogMode == InternalDialogMode::BlenderMissing
                             ? 128 : 78);
            RECT body{card.left + 30,
                      card.top + (gInternalDialogSubject.empty() ? 92 : 132),
                      card.right - 30,
                      bodyBottom};
            DrawTextW(dc, gInternalDialogBody.c_str(), -1, &body,
                      DT_LEFT | DT_TOP | DT_WORDBREAK | DT_NOPREFIX);
            SelectObject(dc, oldFont);
            EndPaint(window, &paint);
            return 0;
        }
        case WM_DRAWITEM: {
            const auto& item = *reinterpret_cast<DRAWITEMSTRUCT*>(lParam);
            if (item.CtlID == ID_INTERNAL_VERSION)
                DrawInternalBlenderVersionItem(item);
            else
                DrawOwnerButton(item);
            return TRUE;
        }
        case WM_MEASUREITEM: {
            auto& item = *reinterpret_cast<MEASUREITEMSTRUCT*>(lParam);
            if (item.CtlID == ID_INTERNAL_VERSION) {
                item.itemHeight = 34;
                return TRUE;
            }
            break;
        }
        case WM_CTLCOLORLISTBOX:
            SetBkColor(reinterpret_cast<HDC>(wParam),
                       gDarkTheme ? RGB(31, 32, 42) : RGB(255, 255, 255));
            SetTextColor(reinterpret_cast<HDC>(wParam),
                         gDarkTheme ? RGB(244, 244, 249) : RGB(28, 29, 36));
            return reinterpret_cast<LRESULT>(gSurfaceBrush);
        case WM_COMMAND:
            if (LOWORD(wParam) == ID_INTERNAL_REMEMBER) {
                HWND remember = GetDlgItem(window, ID_INTERNAL_REMEMBER);
                InvalidateRect(remember, nullptr, TRUE);
                return 0;
            }
            if (LOWORD(wParam) == ID_INTERNAL_CANCEL) {
                if (gInternalDialogMode == InternalDialogMode::CloseBehavior) {
                    const bool remember = SendMessageW(
                        GetDlgItem(window, ID_INTERNAL_REMEMBER),
                        BM_GETCHECK, 0, 0) == BST_CHECKED;
                    CloseInternalDialog();
                    if (remember) {
                        Config config = gApp->GetConfig();
                        config.closeBehavior = "exit";
                        std::wstring error;
                        if (gApp->UpdateConfig(config, error)) LoadConfigIntoUi();
                        else LogError("could not remember close behavior: " +
                                      WideToUtf8(error));
                    }
                    gForceExit = true;
                    PostMessageW(gWindow, WM_CLOSE, 0, 0);
                    return 0;
                }
                if (gInternalDialogMode == InternalDialogMode::UpdateAvailable &&
                    gPendingUpdateOffer) {
                    gSkippedUpdateVersion = gPendingUpdateOffer->version;
                    gPendingUpdateOffer.reset();
                }
                CloseInternalDialog();
                return 0;
            }
            if (LOWORD(wParam) == ID_INTERNAL_CONFIRM) {
                if (gInternalDialogMode == InternalDialogMode::Notice) {
                    CloseInternalDialog();
                    return 0;
                }
                const InternalDialogMode mode = gInternalDialogMode;
                if (mode == InternalDialogMode::BlenderMissing) {
                    const bool configureOptix = gBlenderInstallNvidiaAvailable &&
                        SendMessageW(GetDlgItem(window, ID_INTERNAL_REMEMBER),
                                     BM_GETCHECK, 0, 0) == BST_CHECKED;
                    CloseInternalDialog();
                    StartBlenderCatalogFetch(configureOptix);
                    return 0;
                }
                if (mode == InternalDialogMode::BlenderVersionPicker) {
                    const LRESULT selected = SendMessageW(
                        GetDlgItem(window, ID_INTERNAL_VERSION),
                        CB_GETCURSEL, 0, 0);
                    if (selected == CB_ERR ||
                        static_cast<size_t>(selected) >= gBlenderCatalog.size())
                        return 0;
                    const BlenderRelease release =
                        gBlenderCatalog[static_cast<size_t>(selected)];
                    const bool configureOptix = gBlenderInstallNvidiaAvailable &&
                        SendMessageW(GetDlgItem(window, ID_INTERNAL_REMEMBER),
                                     BM_GETCHECK, 0, 0) == BST_CHECKED;
                    CloseInternalDialog();
                    StartBlenderInstall(release, configureOptix);
                    return 0;
                }
                if (mode == InternalDialogMode::CloseBehavior) {
                    const bool remember = SendMessageW(
                        GetDlgItem(window, ID_INTERNAL_REMEMBER),
                        BM_GETCHECK, 0, 0) == BST_CHECKED;
                    CloseInternalDialog();
                    if (remember) {
                        Config config = gApp->GetConfig();
                        config.closeBehavior = "tray";
                        std::wstring error;
                        if (gApp->UpdateConfig(config, error)) LoadConfigIntoUi();
                        else LogError("could not remember close behavior: " +
                                      WideToUtf8(error));
                    }
                    if (!UpdateTrayIcon()) {
                        ShowInternalNotice(
                            L"无法最小化到系统托盘",
                            L"Windows 通知区域暂时不可用，RenderMesh 窗口将保持打开。"
                            L"请稍后重试或选择直接退出。");
                        return 0;
                    }
                    ShowWindow(gWindow, SW_HIDE);
                    return 0;
                }
                const auto jobIds = gPendingDeleteJobIds;
                const auto packageIds = gPendingContributionPackageIds;
                const auto updateOffer = gPendingUpdateOffer;
                CloseInternalDialog();
                if (mode == InternalDialogMode::PreflightWarningOverride) {
                    StartImport(Text(Item(ID_SOURCE_PATH)), true);
                    return 0;
                }
                if (mode == InternalDialogMode::UpdateAvailable) {
                    if (updateOffer) StartLanUpdateDownload(*updateOffer);
                    return 0;
                }
                std::wstring error;
                if (mode == InternalDialogMode::ContributionDeleteConfirmation) {
                    if (!gApp->DeleteContributionHistory(packageIds, error))
                        ShowInternalNotice(L"清理失败", error);
                } else {
                    size_t deleted = 0;
                    for (const auto& jobId : jobIds) {
                        std::wstring jobError;
                        if (gApp->DeleteJob(jobId, jobError)) ++deleted;
                        else error = jobError;
                    }
                    if (deleted != jobIds.size())
                        ShowInternalNotice(
                            L"部分作业删除失败",
                            L"已删除 " + std::to_wstring(deleted) + L" / " +
                                std::to_wstring(jobIds.size()) + L" 个作业。" + error);
                    else
                        SetStatusText(L"已删除 " + std::to_wstring(deleted) + L" 个作业。");
                }
                RefreshLists();
                return 0;
            }
            break;
        case WM_KEYDOWN:
            if (wParam == VK_ESCAPE) {
                CloseInternalDialog();
                return 0;
            }
            if (wParam == VK_RETURN) {
                const int focusedId = GetDlgCtrlID(GetFocus());
                if (focusedId == ID_INTERNAL_REMEMBER) {
                    SendMessageW(GetFocus(), BM_CLICK, 0, 0);
                    return 0;
                }
                const int command = focusedId == ID_INTERNAL_CONFIRM
                    ? ID_INTERNAL_CONFIRM : ID_INTERNAL_CANCEL;
                SendMessageW(window, WM_COMMAND, command, 0);
                return 0;
            }
            if (wParam == VK_TAB &&
                gInternalDialogMode != InternalDialogMode::Notice) {
                HWND cancel = GetDlgItem(window, ID_INTERNAL_CANCEL);
                HWND confirm = GetDlgItem(window, ID_INTERNAL_CONFIRM);
                HWND remember = GetDlgItem(window, ID_INTERNAL_REMEMBER);
                if (gInternalDialogMode == InternalDialogMode::CloseBehavior ||
                    gInternalDialogMode == InternalDialogMode::BlenderMissing ||
                    gInternalDialogMode == InternalDialogMode::BlenderVersionPicker) {
                    HWND focused = GetFocus();
                    HWND version = GetDlgItem(window, ID_INTERNAL_VERSION);
                    if (gInternalDialogMode == InternalDialogMode::BlenderVersionPicker)
                        SetFocus(focused == version ? remember
                            : focused == remember ? cancel
                            : focused == cancel ? confirm : version);
                    else
                        SetFocus(focused == confirm ? remember
                            : focused == remember ? cancel : confirm);
                } else {
                    SetFocus(GetFocus() == cancel ? confirm : cancel);
                }
                return 0;
            }
            break;
        case WM_ERASEBKGND:
            return TRUE;
        case WM_NCDESTROY:
            if (gInternalDialog == window) gInternalDialog = nullptr;
            return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

const ui::StableTableModel* ModelForList(UINT_PTR id) {
    switch (id) {
        case ID_NODES: return &gNodeRows;
        case ID_JOBS: return &gJobRows;
        case ID_CONTRIBUTION_LIST: return &gContributionRows;
        case ID_DETAIL_FRAMES: return &gFrameRows;
        case ID_UPLOAD_LIST: return &gUploadRows;
        default: return nullptr;
    }
}

bool IsKnownListHeader(HWND header) {
    for (int id : {ID_NODES, ID_JOBS, ID_CONTRIBUTION_LIST,
                   ID_DETAIL_FRAMES, ID_UPLOAD_LIST}) {
        HWND list = id == ID_DETAIL_FRAMES && gDetailWindow
            ? GetDlgItem(gDetailWindow, id)
            : id == ID_UPLOAD_LIST && gUploadWindow
                ? GetDlgItem(gUploadWindow, id) : GetDlgItem(gWindow, id);
        if (list && ListView_GetHeader(list) == header) return true;
    }
    return false;
}

LRESULT HandleHeaderCustomDraw(NMCUSTOMDRAW* draw) {
    if (draw->dwDrawStage == CDDS_PREPAINT) {
        RECT bounds{};
        GetClientRect(draw->hdr.hwndFrom, &bounds);
        HBRUSH brush = CreateSolidBrush(
            gDarkTheme ? RGB(38, 39, 50) : RGB(247, 248, 251));
        FillRect(draw->hdc, &bounds, brush);
        DeleteObject(brush);
        return CDRF_NOTIFYITEMDRAW;
    }
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
        case ID_UPLOAD_LIST: return 2;
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
    const int cellHeight = std::max(1L, cell.bottom - cell.top);
    const int pillHeight = std::clamp(cellHeight - 10, 18, 24);
    const int pillTop = cell.top + (cellHeight - pillHeight) / 2;
    RECT pill{cell.left + 8, pillTop, cell.left + 8 + width, pillTop + pillHeight};
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
    const int trackTop = cell.top + std::max(0L, (cell.bottom - cell.top - 6) / 2);
    RECT track{cell.left + 8, trackTop, cell.right - 8, trackTop + 6};
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

void DrawTextCell(const NMLVCUSTOMDRAW& draw, const ui::Row& row, bool selected) {
    RECT cell{};
    ListView_GetSubItemRect(draw.nmcd.hdr.hwndFrom,
                            static_cast<int>(draw.nmcd.dwItemSpec), draw.iSubItem,
                            LVIR_BOUNDS, &cell);
    const COLORREF background = ListRowBackground(
        selected, static_cast<size_t>(draw.nmcd.dwItemSpec));
    HBRUSH brush = CreateSolidBrush(background);
    FillRect(draw.nmcd.hdc, &cell, brush);
    DeleteObject(brush);
    RECT textRect = cell;
    textRect.left += draw.iSubItem == 0 ? 10 : 8;
    textRect.right -= 8;
    SetBkMode(draw.nmcd.hdc, TRANSPARENT);
    SetTextColor(draw.nmcd.hdc, selected
        ? (gDarkTheme ? RGB(255, 255, 255) : RGB(34, 42, 38))
        : (gDarkTheme ? RGB(226, 229, 235) : RGB(42, 46, 44)));
    HGDIOBJ oldFont = SelectObject(draw.nmcd.hdc, gFont);
    const std::wstring value = draw.iSubItem < static_cast<int>(row.cells.size())
        ? row.cells[static_cast<size_t>(draw.iSubItem)] : L"";
    DrawTextW(draw.nmcd.hdc, value.c_str(), -1, &textRect,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
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
        DrawTextCell(*draw, model->Rows()[index], selected);
        return CDRF_SKIPDEFAULT;
    }
    return CDRF_DODEFAULT;
}

#ifdef RM_DEV_TOOLS
void WriteUiMetricsReport() {
    if (gUiMetricsReport.empty()) return;
    HWND jobs = Item(ID_JOBS);
    HWND share = Item(ID_SHARE);
    RECT windowRect{};
    GetWindowRect(gWindow, &windowRect);
    int resizeAffectedChildren = 0;
    int repaintedAfterResize = 0;
    int duplicateLayoutCount = 0;
    gUiResizeTestTracking = true;
    for (int pass = 0; pass < 16; ++pass) {
        std::unordered_map<HWND, RECT> beforeResize;
        for (HWND control = GetWindow(gWindow, GW_CHILD); control;
             control = GetWindow(control, GW_HWNDNEXT)) {
            RECT rect{};
            GetWindowRect(control, &rect);
            MapWindowPoints(HWND_DESKTOP, gWindow, reinterpret_cast<POINT*>(&rect), 2);
            beforeResize.emplace(control, rect);
        }
        gUiResizeLayoutCounts.clear();
        gUiResizePaintCounts.clear();
        const int widthDelta = pass % 2 == 0 ? 160 : 80;
        const int heightDelta = pass % 2 == 0 ? 80 : 40;
        SetWindowPos(gWindow, nullptr, windowRect.left, windowRect.top,
                     windowRect.right - windowRect.left + widthDelta,
                     windowRect.bottom - windowRect.top + heightDelta,
                     SWP_NOACTIVATE | SWP_NOZORDER);
        UpdateWindow(gWindow);
        for (const auto& [control, before] : beforeResize) {
            if (!IsWindow(control) ||
                !(GetWindowLongPtrW(control, GWL_STYLE) & WS_VISIBLE)) continue;
            RECT after{};
            GetWindowRect(control, &after);
            MapWindowPoints(HWND_DESKTOP, gWindow, reinterpret_cast<POINT*>(&after), 2);
            if (!EqualRect(&before, &after)) {
                ++resizeAffectedChildren;
                UpdateWindow(control);
                if (gUiResizePaintCounts[control] > 0) ++repaintedAfterResize;
            }
            if (gUiResizeLayoutCounts[control] > 1) ++duplicateLayoutCount;
        }
    }
    gUiResizeTestTracking = false;
    SetWindowPos(gWindow, nullptr, windowRect.left, windowRect.top,
                 windowRect.right - windowRect.left, windowRect.bottom - windowRect.top,
                 SWP_NOACTIVATE | SWP_NOZORDER);
    const auto chromeButtons = ChromeButtonRects(gWindow);
    auto screenHitTest = [&](POINT clientPoint) {
        ClientToScreen(gWindow, &clientPoint);
        return static_cast<int>(SendMessageW(
            gWindow, WM_NCHITTEST, 0, MAKELPARAM(clientPoint.x, clientPoint.y)));
    };
    POINT titlePoint{DpiValue(gWindow, 180), ChromeHeight(gWindow) / 2};
    POINT maxPoint{(chromeButtons[1].left + chromeButtons[1].right) / 2,
                   ChromeHeight(gWindow) / 2};
    POINT resizePoint{1, ChromeHeight(gWindow) + DpiValue(gWindow, 40)};
    POINT iconPoint{DpiValue(gWindow, 22), ChromeHeight(gWindow) / 2};
    const LONG_PTR chromeStyles = GetWindowLongPtrW(gWindow, GWL_STYLE);
    SendMessageW(gWindow, WM_NCLBUTTONDOWN, HTMAXBUTTON, 0);
    SendMessageW(gWindow, WM_CANCELMODE, 0, 0);
    const bool chromeCancelClearsPressed =
        gChromeStates[gWindow].pressed == HTNOWHERE && GetCapture() != gWindow;
    const int jobsVisibleRows = ListView_GetCountPerPage(jobs);
    RECT shareRect{};
    GetClientRect(share, &shareRect);
    const std::wstring shareText = L"接入网络";
    HDC dc = GetDC(share);
    const ShareControlLayout shareLayout = CalculateShareControlLayout(dc, shareRect);
    ReleaseDC(share, dc);
    const int shareGroupLeft = shareLayout.icon.left;
    const int shareGroupRight = shareLayout.track.right;
    const bool shareControlUsesNetworkIcon =
        Text(share) == L"接入网络" &&
        std::abs((shareGroupLeft + shareGroupRight) -
                 (shareRect.left + shareRect.right)) <= 1;
    RECT dashboardNavRect{};
    GetClientRect(Item(ID_NAV_DASHBOARD), &dashboardNavRect);
    const bool shareControlMatchesNavigationSize =
        shareRect.right == dashboardNavRect.right &&
        shareRect.bottom == dashboardNavRect.bottom;
    const bool sharingBeforeTrayProbe = gApp->GetConfig().shareEnabled;
    ValidateRect(share, nullptr);
    std::wstring trayToggleError;
    const bool trayToggleApplied = ToggleSharingFromTray(trayToggleError);
    RECT trayToggleUpdateRect{};
    const bool trayToggleInvalidated =
        GetUpdateRect(share, &trayToggleUpdateRect, FALSE) != FALSE;
    const bool trayToggleChangedConfig =
        trayToggleApplied &&
        gApp->GetConfig().shareEnabled != sharingBeforeTrayProbe;
    std::wstring trayRestoreError;
    const bool trayToggleRestored = trayToggleApplied &&
        ToggleSharingFromTray(trayRestoreError) &&
        gApp->GetConfig().shareEnabled == sharingBeforeTrayProbe;
    UpdateWindow(share);
    const bool trayShareToggleRepaintsMainSwitch =
        trayToggleChangedConfig && trayToggleInvalidated && trayToggleRestored;
    const Config dashboardConfig = gApp->GetConfig();
    const int pageBeforeDashboardProbe = gCurrentPage;
    ShowPage(0);
    RefreshLists();
    const bool dashboardShowsMatchingId =
        gDashboardView.matchId == Utf8ToWide(dashboardConfig.inviteCode) &&
        !gDashboardView.matchId.empty();
    ShowPage(pageBeforeDashboardProbe);
    auto pageContains = [](size_t page, HWND control) {
        const auto& controls = gPageControls[page];
        return control && std::find(controls.begin(), controls.end(), control) != controls.end();
    };
    const HWND autoStartControl = Item(ID_AUTO_START);
    const HWND silentAutoStartControl = Item(ID_SILENT_AUTO_START);
    const Config startupConfig = gApp->GetConfig();
    const bool autoStartSettingsAvailable =
        pageContains(3, autoStartControl) &&
        pageContains(3, silentAutoStartControl) &&
        (GetWindowLongPtrW(autoStartControl, GWL_STYLE) & BS_OWNERDRAW) == BS_OWNERDRAW &&
        (GetWindowLongPtrW(silentAutoStartControl, GWL_STYLE) & BS_OWNERDRAW) ==
            BS_OWNERDRAW;
    const bool silentAutoStartDependencyVisible =
        (IsWindowEnabled(silentAutoStartControl) != FALSE) == startupConfig.autoStart &&
        (!startupConfig.autoStart ||
         (SendMessageW(silentAutoStartControl, BM_GETCHECK, 0, 0) == BST_CHECKED) ==
             startupConfig.silentAutoStart);
    const LRESULT originalAutoStartState =
        SendMessageW(autoStartControl, BM_GETCHECK, 0, 0);
    const LRESULT toggledAutoStartState = originalAutoStartState == BST_CHECKED
        ? BST_UNCHECKED : BST_CHECKED;
    SendMessageW(autoStartControl, BM_SETCHECK, toggledAutoStartState, 0);
    const bool autoStartToggleStateRoundTrips =
        SendMessageW(autoStartControl, BM_GETCHECK, 0, 0) == toggledAutoStartState;
    SendMessageW(autoStartControl, BM_SETCHECK, originalAutoStartState, 0);
    const bool startupTogglesAreBorderless =
        (GetWindowLongPtrW(autoStartControl, GWL_STYLE) & WS_BORDER) == 0 &&
        (GetWindowLongPtrW(silentAutoStartControl, GWL_STYLE) & WS_BORDER) == 0 &&
        (GetWindowLongPtrW(autoStartControl, GWL_EXSTYLE) & WS_EX_CLIENTEDGE) == 0 &&
        (GetWindowLongPtrW(silentAutoStartControl, GWL_EXSTYLE) & WS_EX_CLIENTEDGE) == 0;
    const bool silentStartTooltipAvailable =
        pageContains(3, Item(ID_SILENT_AUTO_START_INFO)) &&
        gSilentAutoStartTooltip && IsWindow(gSilentAutoStartTooltip) &&
        Text(Item(ID_SILENT_AUTO_START_INFO)) == L"\u24D8";
    const bool versionUpdateControlsAvailable =
        pageContains(3, Item(ID_CURRENT_VERSION)) &&
        pageContains(3, Item(ID_CHECK_UPDATE)) &&
        pageContains(3, Item(ID_INSTALL_BLENDER)) &&
        Text(Item(ID_CURRENT_VERSION)).find(Utf8ToWide(kRenderMeshAppVersion)) !=
            std::wstring::npos &&
        Text(Item(ID_CHECK_UPDATE)) == L"检查更新" &&
        Text(Item(ID_INSTALL_BLENDER)) == L"安装 / 切换 Blender 版本";
    const bool projectStorageSettingsAvailable =
        pageContains(3, Item(ID_PROJECT_STORAGE_PATH)) &&
        pageContains(3, Item(ID_CHANGE_PROJECT_STORAGE)) &&
        (GetWindowLongPtrW(Item(ID_PROJECT_STORAGE_PATH), GWL_STYLE) &
         ES_READONLY) == ES_READONLY &&
        Text(Item(ID_PROJECT_STORAGE_PATH)) ==
            gApp->GetConfig().projectStoragePath.wstring() &&
        Text(Item(ID_CHANGE_PROJECT_STORAGE)) == L"更改位置…";
    const fs::path resolvedBlenderPath = LocalRendererPath(startupConfig);
    const bool manualBlenderPathSettingsAvailable =
        pageContains(3, Item(ID_BLENDER_PATH)) &&
        pageContains(3, Item(ID_BROWSE_BLENDER)) &&
        (GetWindowLongPtrW(Item(ID_BLENDER_PATH), GWL_STYLE) & ES_READONLY) ==
            ES_READONLY &&
        Text(Item(ID_BLENDER_PATH)) == (resolvedBlenderPath.empty()
            ? L"未检测到 Blender" : resolvedBlenderPath.wstring()) &&
        Text(Item(ID_BROWSE_BLENDER)) == L"手动指定…";
    Config silentStartupProbe;
    silentStartupProbe.autoStart = true;
    silentStartupProbe.silentAutoStart = true;
    const bool silentStartupOnlyAppliesToAutoStart =
        ShouldStartSilently(silentStartupProbe, true) &&
        !ShouldStartSilently(silentStartupProbe, false) &&
        !ShouldStartSilently(Config{}, true);
    const bool autoStartCommandIsQuotedAndMarked =
        AutoStartCommand(L"C:\\Render Mesh\\RenderMesh.exe") ==
            L"\"C:\\Render Mesh\\RenderMesh.exe\" --startup";
    const bool dashboardMatchingIdEditorAvailable =
        pageContains(0, Item(ID_CLUSTER_TOKEN)) && pageContains(0, Item(ID_COPY_TOKEN)) &&
        (GetWindowLongPtrW(Item(ID_CLUSTER_TOKEN), GWL_STYLE) & ES_READONLY) == 0;
    const std::wstring matchingIdBeforeReplacement = Text(Item(ID_CLUSTER_TOKEN));
    gLoadingSettings = true;
    SetText(ID_CLUSTER_TOKEN, L"123456");
    SendMessageW(Item(ID_CLUSTER_TOKEN), WM_LBUTTONDOWN, MK_LBUTTON,
                 MAKELPARAM(2, 10));
    SendMessageW(Item(ID_CLUSTER_TOKEN), WM_LBUTTONUP, 0, MAKELPARAM(2, 10));
    for (int index = 0; index < 6; ++index)
        SendMessageW(Item(ID_CLUSTER_TOKEN), WM_CHAR, L'7', 0);
    const bool dashboardMatchingIdClickReplacesWholeValue =
        Text(Item(ID_CLUSTER_TOKEN)) == L"777777";
    SetText(ID_CLUSTER_TOKEN, matchingIdBeforeReplacement);
    gLoadingSettings = false;
    const bool settingsHidesMatchingIdEditor =
        !pageContains(3, Item(ID_CLUSTER_TOKEN)) && !pageContains(3, Item(ID_COPY_TOKEN));
    const uint64_t mutationCountBeforeNoOp = gListMutationCount;
    const bool noOpReportedMutation = SyncTable(Item(ID_NODES), gNodeRows, gNodeRows.Rows());
    const uint64_t stableSnapshotMutationDelta =
        gListMutationCount - mutationCountBeforeNoOp;
    bool scrollAnchorPreserved = true;
    bool selectionPreserved = true;
    bool focusPreserved = true;
    bool horizontalScrollPreserved = true;
    bool horizontalScrollAbsent = true;
    bool nativeOverflowScrollbarHidden = true;
    bool customOverflowScrollbarVisible = false;
    bool customWheelScrollsRows = true;
    bool customScrollbarDragScrollsRows = true;
    bool nativeScrollbarsStayHiddenAfterInput = true;
    bool topPixelOffsetPreserved = true;
    bool controlOrderMatchesModel = true;
    bool structuralUpdateApplied = true;
    bool multipleSelectionPreserved = true;
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
        UpdateWindow(jobs);
        SCROLLBARINFO overflowScrollInfo{};
        overflowScrollInfo.cbSize = sizeof(overflowScrollInfo);
        nativeOverflowScrollbarHidden =
            !GetScrollBarInfo(jobs, OBJID_VSCROLL, &overflowScrollInfo) ||
            (overflowScrollInfo.rgstate[0] & STATE_SYSTEM_INVISIBLE) != 0;
        const auto overflowThumb = CustomListScrollbarThumb(jobs);
        const RECT overflowTrack = CustomListScrollbarTrack(jobs);
        customOverflowScrollbarVisible = overflowThumb &&
            overflowTrack.top > 0 && overflowTrack.bottom > overflowTrack.top &&
            overflowThumb->top >= overflowTrack.top &&
            overflowThumb->bottom <= overflowTrack.bottom &&
            overflowTrack.right - overflowTrack.left <= DpiValue(jobs, 8);
        const int wheelTopBefore = ListView_GetTopIndex(jobs);
        SendMessageW(jobs, WM_MOUSEWHEEL,
                     MAKEWPARAM(0, static_cast<WORD>(
                         static_cast<SHORT>(-WHEEL_DELTA))), 0);
        const int wheelTopAfter = ListView_GetTopIndex(jobs);
        customWheelScrollsRows = wheelTopAfter > wheelTopBefore;
        const auto dragThumb = CustomListScrollbarThumb(jobs);
        if (dragThumb) {
            const int dragTopBefore = ListView_GetTopIndex(jobs);
            const POINT dragStart{
                (dragThumb->left + dragThumb->right) / 2,
                (dragThumb->top + dragThumb->bottom) / 2,
            };
            const int dragDestination = std::max(
                dragStart.y + 1, overflowTrack.bottom - DpiValue(jobs, 2));
            SendMessageW(jobs, WM_LBUTTONDOWN, MK_LBUTTON,
                         MAKELPARAM(dragStart.x, dragStart.y));
            SendMessageW(jobs, WM_MOUSEMOVE, MK_LBUTTON,
                         MAKELPARAM(dragStart.x, dragDestination));
            SendMessageW(jobs, WM_LBUTTONUP, 0,
                         MAKELPARAM(dragStart.x, dragDestination));
            customScrollbarDragScrollsRows =
                ListView_GetTopIndex(jobs) > dragTopBefore;
        } else {
            customScrollbarDragScrollsRows = false;
        }
        SCROLLBARINFO postInputVerticalInfo{};
        postInputVerticalInfo.cbSize = sizeof(postInputVerticalInfo);
        SCROLLBARINFO postInputHorizontalInfo{};
        postInputHorizontalInfo.cbSize = sizeof(postInputHorizontalInfo);
        const LONG_PTR postInputStyle = GetWindowLongPtrW(jobs, GWL_STYLE);
        nativeScrollbarsStayHiddenAfterInput =
            (postInputStyle & (WS_VSCROLL | WS_HSCROLL)) == 0 &&
            (!GetScrollBarInfo(jobs, OBJID_VSCROLL, &postInputVerticalInfo) ||
             (postInputVerticalInfo.rgstate[0] & STATE_SYSTEM_INVISIBLE) != 0) &&
            (!GetScrollBarInfo(jobs, OBJID_HSCROLL, &postInputHorizontalInfo) ||
             (postInputHorizontalInfo.rgstate[0] & STATE_SYSTEM_INVISIBLE) != 0);
        ScrollListToTopIndex(jobs, 0);
        ListView_SetItemState(jobs, -1, 0, LVIS_SELECTED | LVIS_FOCUSED);
        ListView_SetItemState(jobs, 0, LVIS_SELECTED, LVIS_SELECTED);
        ListView_SetItemState(jobs, 1, LVIS_SELECTED, LVIS_SELECTED);
        SyncTable(jobs, gJobRows, syntheticRows);
        int selectedCount = 0;
        for (int selected = ListView_GetNextItem(jobs, -1, LVNI_SELECTED); selected >= 0;
             selected = ListView_GetNextItem(jobs, selected, LVNI_SELECTED))
            ++selectedCount;
        multipleSelectionPreserved = selectedCount == 2;
        ListView_SetItemState(jobs, -1, 0, LVIS_SELECTED | LVIS_FOCUSED);
        const int anchorTarget = std::min(50, gUiTestRows - 1);
        ListView_EnsureVisible(jobs, anchorTarget, FALSE);
        const int topBefore = ListView_GetTopIndex(jobs);
        const int selectedBefore = std::min(gUiTestRows - 1, topBefore + 2);
        ListView_SetItemState(jobs, selectedBefore, LVIS_SELECTED | LVIS_FOCUSED,
                              LVIS_SELECTED | LVIS_FOCUSED);
        RECT topRectBefore{};
        ListView_GetItemRect(jobs, topBefore, &topRectBefore, LVIR_BOUNDS);
        const int horizontalBefore = GetScrollPos(jobs, SB_HORZ);
        horizontalScrollAbsent = horizontalBefore == 0;
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
        horizontalScrollPreserved = GetScrollPos(jobs, SB_HORZ) == 0 &&
            horizontalBefore == 0;
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
        FitMainListColumns();
    }

    auto findStaticByText = [&](const wchar_t* expected) -> HWND {
        for (HWND control = GetWindow(gWindow, GW_CHILD); control;
             control = GetWindow(control, GW_HWNDNEXT)) {
            wchar_t className[32]{};
            wchar_t textValue[128]{};
            GetClassNameW(control, className, static_cast<int>(std::size(className)));
            GetWindowTextW(control, textValue, static_cast<int>(std::size(textValue)));
            if (wcscmp(className, L"Static") == 0 && wcscmp(textValue, expected) == 0)
                return control;
        }
        return nullptr;
    };
    auto staticBrushColor = [&](HWND control) -> COLORREF {
        if (!control) return CLR_INVALID;
        HDC dc = GetDC(control);
        HBRUSH brush = reinterpret_cast<HBRUSH>(
            SendMessageW(gWindow, WM_CTLCOLORSTATIC,
                         reinterpret_cast<WPARAM>(dc), reinterpret_cast<LPARAM>(control)));
        LOGBRUSH description{};
        const bool described = brush && GetObjectW(brush, sizeof(description), &description);
        ReleaseDC(control, dc);
        return described ? description.lbColor : CLR_INVALID;
    };
    auto staticPaintsOpaqueSurface = [&](HWND control) {
        if (!control) return false;
        HDC dc = GetDC(control);
        SendMessageW(gWindow, WM_CTLCOLORSTATIC,
                     reinterpret_cast<WPARAM>(dc), reinterpret_cast<LPARAM>(control));
        const bool opaque = GetBkMode(dc) == OPAQUE;
        ReleaseDC(control, dc);
        return opaque;
    };
    const bool staticLabelBrushesMatchRegions =
        staticBrushColor(findStaticByText(L"渲染任务")) ==
            (gDarkTheme ? RGB(15, 16, 22) : RGB(244, 245, 249));
    const bool staticLabelsPaintOpaqueSurface =
        staticPaintsOpaqueSurface(findStaticByText(L"渲染任务")) &&
        staticPaintsOpaqueSurface(findStaticByText(L"电脑名称"));
    const bool sidebarBrandRemoved =
        !findStaticByText(L"RenderMesh") && !findStaticByText(L"Blender 渲染网格");

    const std::array<int, 3> visibleFieldIds = {
        ID_MANUAL_ENDPOINT, ID_NODE_NAME, ID_IDLE_SECONDS,
    };
    const bool visibleFieldsAvoidLegacyFrames =
        std::all_of(visibleFieldIds.begin(), visibleFieldIds.end(), [](int id) {
            HWND control = Item(id);
            if (!control) return false;
            const LONG_PTR style = GetWindowLongPtrW(control, GWL_STYLE);
            const LONG_PTR exStyle = GetWindowLongPtrW(control, GWL_EXSTYLE);
            return (style & WS_BORDER) == 0 && (exStyle & WS_EX_CLIENTEDGE) == 0;
        });

    auto hasRoundedRegion = [](HWND control) {
        if (!control) return false;
        HRGN region = CreateRectRgn(0, 0, 1, 1);
        const int type = GetWindowRgn(control, region);
        DeleteObject(region);
        return type != ERROR && type != NULLREGION;
    };
    const bool visibleFieldsHaveRoundedRegions =
        std::all_of(visibleFieldIds.begin(), visibleFieldIds.end(), [&](int id) {
            return hasRoundedRegion(Item(id));
        });
    wchar_t themeClass[32]{};
    GetClassNameW(Item(ID_THEME), themeClass,
                  static_cast<int>(std::size(themeClass)));
    const bool themeSelectorUsesOwnerDraw =
        wcscmp(themeClass, L"Button") == 0 &&
        (GetWindowLongPtrW(Item(ID_THEME), GWL_STYLE) & BS_OWNERDRAW) == BS_OWNERDRAW;

    const uint64_t fieldPaintsBeforeProbe = gSelfFramedFieldPaintCount;
    HWND dialogFieldProbe = AddDialogControl(
        gWindow, L"EDIT", L"probe", WS_BORDER | ES_AUTOHSCROLL,
        5, 5, 160, 30, 0x6FFE);
    RedrawWindow(dialogFieldProbe, nullptr, nullptr,
                 RDW_INVALIDATE | RDW_UPDATENOW | RDW_FRAME);
    const LONG_PTR probeStyle = GetWindowLongPtrW(dialogFieldProbe, GWL_STYLE);
    const LONG_PTR probeExStyle = GetWindowLongPtrW(dialogFieldProbe, GWL_EXSTYLE);
    const bool dialogFieldsUseModernFrames =
        (probeStyle & WS_BORDER) == 0 &&
        (probeExStyle & WS_EX_CLIENTEDGE) == 0 &&
        GetPropW(dialogFieldProbe, kFieldSelfFrameProperty) != nullptr &&
        hasRoundedRegion(dialogFieldProbe);
    const bool dialogFieldFrameWasPainted =
        gSelfFramedFieldPaintCount > fieldPaintsBeforeProbe;
    DestroyWindow(dialogFieldProbe);

    auto syntheticRows = [](const wchar_t* prefix, int count, int columns) {
        std::vector<ui::Row> rows;
        rows.reserve(static_cast<size_t>(count));
        for (int index = 0; index < count; ++index) {
            std::vector<std::wstring> cells(static_cast<size_t>(columns), L"-");
            cells[0] = std::wstring(prefix) + L" " + std::to_wstring(index + 1);
            rows.push_back({std::wstring(prefix) + L"-" + std::to_wstring(index),
                            std::move(cells), ui::VisualState::Muted});
        }
        return rows;
    };
    auto listHeight = [](HWND list) {
        RECT rect{};
        GetWindowRect(list, &rect);
        return static_cast<int>(rect.bottom - rect.top);
    };
    auto stableHeightProfile = [&](HWND list, ui::StableTableModel& model,
                                   const wchar_t* prefix, int columns) {
        SyncTable(list, model, syntheticRows(prefix, 0, columns));
        const int emptyHeight = listHeight(list);
        SyncTable(list, model, syntheticRows(prefix, 2, columns));
        const int twoHeight = listHeight(list);
        SyncTable(list, model, syntheticRows(prefix, 12, columns));
        const int twelveHeight = listHeight(list);
        return std::array<int, 3>{emptyHeight, twoHeight, twelveHeight};
    };
    const auto nodeHeightProfile = stableHeightProfile(
        Item(ID_NODES), gNodeRows, L"节点", 7);
    const auto jobHeightProfile = stableHeightProfile(
        jobs, gJobRows, L"任务", 7);
    const auto contributionHeightProfile = stableHeightProfile(
        Item(ID_CONTRIBUTION_LIST), gContributionRows, L"贡献", 10);
    const auto stableProfilePasses = [](const std::array<int, 3>& profile) {
        return profile[0] == profile[1] && profile[1] == profile[2];
    };
    const bool listHeightStableAcrossRowCounts =
        stableProfilePasses(nodeHeightProfile) &&
        stableProfilePasses(jobHeightProfile) &&
        stableProfilePasses(contributionHeightProfile);
    FitMainListColumns();
    const bool mainListsFitWithoutHorizontalScroll =
        ListColumnsFitClient(Item(ID_NODES)) &&
        ListColumnsFitClient(jobs) &&
        ListColumnsFitClient(Item(ID_CONTRIBUTION_LIST)) &&
        GetScrollPos(Item(ID_NODES), SB_HORZ) == 0 &&
        GetScrollPos(jobs, SB_HORZ) == 0 &&
        GetScrollPos(Item(ID_CONTRIBUTION_LIST), SB_HORZ) == 0;
    auto listHeaderFillsClient = [](HWND list) {
        RECT client{};
        GetClientRect(list, &client);
        HWND header = ListView_GetHeader(list);
        int width = 0;
        for (int index = 0; header && index < Header_GetItemCount(header); ++index)
            width += ListView_GetColumnWidth(list, index);
        return header && std::abs(width - (client.right - client.left)) <= 1;
    };
    const bool listHeadersHaveNoScrollCorner =
        listHeaderFillsClient(Item(ID_NODES)) &&
        listHeaderFillsClient(jobs) &&
        listHeaderFillsClient(Item(ID_CONTRIBUTION_LIST));
    auto nativeVerticalScrollbarHidden = [](HWND list) {
        SCROLLBARINFO info{};
        info.cbSize = sizeof(info);
        return !GetScrollBarInfo(list, OBJID_VSCROLL, &info) ||
            (info.rgstate[0] & STATE_SYSTEM_INVISIBLE) != 0;
    };
    const bool nativeListScrollbarsAreHidden =
        nativeVerticalScrollbarHidden(Item(ID_NODES)) &&
        nativeVerticalScrollbarHidden(jobs) &&
        nativeVerticalScrollbarHidden(Item(ID_CONTRIBUTION_LIST));
    const bool schedulingPolicyUiIsRemoved =
        Header_GetItemCount(ListView_GetHeader(Item(ID_NODES))) == 7 &&
        std::all_of(gNodeRows.Rows().begin(), gNodeRows.Rows().end(),
                    [](const ui::Row& row) { return row.cells.size() == 7; });
    const RECT centeringOwner{100, 80, 1100, 780};
    const RECT centeringWork{0, 0, 1920, 1080};
    const RECT centeredProbe = CenteredOwnedWindowRect(
        centeringOwner, centeringWork, 600, 400);
    const bool centeredOwnedWindowGeometry =
        centeredProbe.left == 300 && centeredProbe.top == 230 &&
        centeredProbe.right == 900 && centeredProbe.bottom == 630;

    HWND uploadSelectionProbeParent = CreateWindowExW(
        0, L"STATIC", L"", WS_CHILD, -400, -400, 260, 160, gWindow, nullptr,
        GetModuleHandleW(nullptr), nullptr);
    HWND uploadSelectionProbe = CreateWindowExW(
        0, WC_LISTVIEWW, L"", WS_CHILD | LVS_REPORT | LVS_SHOWSELALWAYS,
        0, 0, 250, 150, uploadSelectionProbeParent, nullptr,
        GetModuleHandleW(nullptr), nullptr);
    ConfigureList(uploadSelectionProbe);
    AddColumn(uploadSelectionProbe, 0, 220, L"电脑");
    ui::StableTableModel uploadSelectionModel;
    SyncTable(uploadSelectionProbe, uploadSelectionModel, {
        {L"probe-a", {L"电脑 A"}, ui::VisualState::Neutral},
        {L"probe-b", {L"电脑 B"}, ui::VisualState::Neutral},
        {L"probe-unavailable", {L"不可加入"}, ui::VisualState::Muted},
    });
    const std::set<std::string> uploadSelectableProbe{"probe-a", "probe-b"};
    auto selectedUploadProbeRows = [&] {
        std::vector<int> selected;
        for (int index = ListView_GetNextItem(uploadSelectionProbe, -1, LVNI_SELECTED);
             index >= 0;
             index = ListView_GetNextItem(uploadSelectionProbe, index, LVNI_SELECTED))
            selected.push_back(index);
        return selected;
    };
    ApplyUploadRowSelection(uploadSelectionProbe, uploadSelectionModel,
                            uploadSelectableProbe, false);
    const bool uploadSelectAllWorks =
        selectedUploadProbeRows() == std::vector<int>({0, 1});
    ListView_SetItemState(uploadSelectionProbe, -1, 0, LVIS_SELECTED);
    ListView_SetItemState(uploadSelectionProbe, 0, LVIS_SELECTED, LVIS_SELECTED);
    ApplyUploadRowSelection(uploadSelectionProbe, uploadSelectionModel,
                            uploadSelectableProbe, true);
    const bool uploadInvertSelectionWorks =
        selectedUploadProbeRows() == std::vector<int>({1});
    const bool uploadSelectAllAndInvertWork =
        uploadSelectAllWorks && uploadInvertSelectionWorks;
    DestroyWindow(uploadSelectionProbeParent);

    HWND configLayoutProbe = CreateWindowExW(
        0, L"STATIC", L"", WS_CHILD, -500, -500, 764, 760, gWindow, nullptr,
        GetModuleHandleW(nullptr), nullptr);
    for (const int id : {
             ID_CONFIG_PROFILE_SELECT, ID_CONFIG_PROFILE_EDIT,
             ID_CONFIG_PROFILE_NEW,
             ID_CONFIG_PROFILE_NAME, ID_CONFIG_PROFILE_SOFTWARE,
             ID_CONFIG_PROFILE_PATH, ID_CONFIG_PROFILE_BROWSE,
             ID_CONFIG_PROFILE_SAVE, ID_CONFIG_PROFILE_DELETE,
             ID_CONFIG_TEST_FRAMES,
             ID_CONFIG_CANCEL, ID_CONFIG_SUBMIT}) {
        CreateWindowExW(
            0, L"STATIC", L"", WS_CHILD, 0, 0, 10, 10, configLayoutProbe,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
            GetModuleHandleW(nullptr), nullptr);
    }
    LayoutConfigWindow(configLayoutProbe);
    RECT configProbeClient{};
    GetClientRect(configLayoutProbe, &configProbeClient);
    const int expectedConfigRight = configProbeClient.right - 24;
    auto controlRightInParent = [&](int id) {
        RECT rect{};
        GetWindowRect(GetDlgItem(configLayoutProbe, id), &rect);
        MapWindowPoints(HWND_DESKTOP, configLayoutProbe,
                        reinterpret_cast<POINT*>(&rect), 2);
        return rect.right;
    };
    const std::array configRightAlignedControls{
        ID_CONFIG_PROFILE_NEW, ID_CONFIG_PROFILE_NAME,
        ID_CONFIG_PROFILE_SOFTWARE, ID_CONFIG_PROFILE_BROWSE,
        ID_CONFIG_PROFILE_SAVE, ID_CONFIG_TEST_FRAMES, ID_CONFIG_SUBMIT,
    };
    const bool configContentUsesFullWidth = std::all_of(
        configRightAlignedControls.begin(), configRightAlignedControls.end(),
        [&](int id) { return controlRightInParent(id) == expectedConfigRight; });
    RECT configEditRect{};
    RECT configNewRect{};
    RECT configDeleteRect{};
    RECT configSaveRect{};
    GetWindowRect(GetDlgItem(configLayoutProbe, ID_CONFIG_PROFILE_EDIT),
                  &configEditRect);
    GetWindowRect(GetDlgItem(configLayoutProbe, ID_CONFIG_PROFILE_NEW),
                  &configNewRect);
    GetWindowRect(GetDlgItem(configLayoutProbe, ID_CONFIG_PROFILE_DELETE),
                  &configDeleteRect);
    GetWindowRect(GetDlgItem(configLayoutProbe, ID_CONFIG_PROFILE_SAVE),
                  &configSaveRect);
    const bool softwareProfileEditorHasDelete =
        configEditRect.right <= configNewRect.left &&
        configDeleteRect.right <= configSaveRect.left;
    DestroyWindow(configLayoutProbe);

    auto sampleTrayIcon = [](HICON icon) {
        struct Sample {
            std::set<DWORD> colors;
            std::array<DWORD, 32 * 32> pixels{};
            int occupied = 0;
        } sample;
        if (!icon) return sample;
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = 32;
        info.bmiHeader.biHeight = -32;
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        void* raw = nullptr;
        HDC screen = GetDC(nullptr);
        HBITMAP bitmap = CreateDIBSection(
            screen, &info, DIB_RGB_COLORS, &raw, nullptr, 0);
        HDC memory = CreateCompatibleDC(screen);
        HGDIOBJ previous = SelectObject(memory, bitmap);
        if (raw) memset(raw, 0, sample.pixels.size() * sizeof(DWORD));
        DrawIconEx(memory, 0, 0, icon, 32, 32, 0, nullptr, DI_NORMAL);
        if (raw) {
            memcpy(sample.pixels.data(), raw,
                   sample.pixels.size() * sizeof(DWORD));
            for (const DWORD pixel : sample.pixels) {
                const DWORD color = pixel & 0x00FFFFFFu;
                if (!color) continue;
                ++sample.occupied;
                sample.colors.insert(color);
            }
        }
        SelectObject(memory, previous);
        DeleteDC(memory);
        DeleteObject(bitmap);
        ReleaseDC(nullptr, screen);
        return sample;
    };
    const auto idleTraySample = sampleTrayIcon(gTrayIcons[0]);
    const auto errorTraySample = sampleTrayIcon(gTrayIcons[1]);
    const auto renderingTraySample = sampleTrayIcon(gTrayIcons[2]);
    int idleErrorDifference = 0;
    int idleRenderingDifference = 0;
    for (size_t index = 0; index < idleTraySample.pixels.size(); ++index) {
        idleErrorDifference +=
            idleTraySample.pixels[index] != errorTraySample.pixels[index];
        idleRenderingDifference +=
            idleTraySample.pixels[index] != renderingTraySample.pixels[index];
    }
    const bool trayIconsUseBrandedStatusBadges =
        idleTraySample.colors.size() >= 4 &&
        errorTraySample.colors.size() >= 4 &&
        renderingTraySample.colors.size() >= 4 &&
        idleTraySample.occupied > 250 && idleTraySample.occupied < 950 &&
        idleErrorDifference > 12 && idleRenderingDifference > 12;

    RECT mainClient{};
    GetClientRect(gWindow, &mainClient);
    auto bottomGap = [&](HWND control) {
        RECT rect{};
        GetWindowRect(control, &rect);
        MapWindowPoints(HWND_DESKTOP, gWindow, reinterpret_cast<POINT*>(&rect), 2);
        return mainClient.bottom - rect.bottom;
    };
    const bool mainListsFillAvailableHeight =
        bottomGap(Item(ID_NODES)) == 66 &&
        bottomGap(jobs) == 66 &&
        bottomGap(Item(ID_CONTRIBUTION_LIST)) == 66;

    std::vector<ui::Row> compactRows = {
        {L"compact-1", {L"紧凑任务 1", L"待提交", L"1", L"0 / 1", L"-", L"-", L"-"},
         ui::VisualState::Warning},
        {L"compact-2", {L"紧凑任务 2", L"待提交", L"1", L"0 / 1", L"-", L"-", L"-"},
         ui::VisualState::Warning},
    };
    SyncTable(jobs, gJobRows, std::move(compactRows));
    LayoutUi();
    RECT settingsNavRect{};
    GetWindowRect(Item(ID_NAV_SETTINGS), &settingsNavRect);
    MapWindowPoints(HWND_DESKTOP, gWindow,
                    reinterpret_cast<POINT*>(&settingsNavRect), 2);
    const bool settingsNavIsBottomLeftIcon =
        settingsNavRect.left == 18 && mainClient.bottom - settingsNavRect.bottom == 20 &&
        settingsNavRect.right - settingsNavRect.left == 42 && Text(Item(ID_NAV_SETTINGS)) == L"设置";
    bool legacySaveButtonFound = false;
    bool settingsFooterCopyFound = false;
    for (HWND control = GetWindow(gWindow, GW_CHILD); control;
         control = GetWindow(control, GW_HWNDNEXT)) {
        wchar_t className[32]{};
        GetClassNameW(control, className, static_cast<int>(std::size(className)));
        if (wcscmp(className, L"Button") == 0 && Text(control) == L"保存设置") {
            legacySaveButtonFound = true;
        }
        if (wcscmp(className, L"Static") == 0 &&
            (Text(control).find(L"端口 8391") != std::wstring::npos ||
             Text(control).find(L"设置会在更改后自动保存") != std::wstring::npos))
            settingsFooterCopyFound = true;
    }
    const bool saveButtonRemoved = !legacySaveButtonFound;
    const Config originalSettings = gApp->GetConfig();
    const std::wstring originalNodeName = Utf8ToWide(originalSettings.nodeName);
    gSettingsDirty = false;
    const std::wstring probeNodeName = originalNodeName + L"-autosave-probe";
    SetWindowTextW(Item(ID_NODE_NAME), probeNodeName.c_str());
    const bool autoSaveWasScheduled = gSettingsDirty;
    SendMessageW(gWindow, WM_TIMER, TIMER_SETTINGS_AUTOSAVE, 0);
    const bool autoSavePersisted =
        Utf8ToWide(gApp->GetConfig().nodeName) == probeNodeName && !gSettingsDirty;
    SetWindowTextW(Item(ID_NODE_NAME), originalNodeName.c_str());
    SendMessageW(gWindow, WM_TIMER, TIMER_SETTINGS_AUTOSAVE, 0);
    const bool autoSaveRestored =
        gApp->GetConfig().nodeName == originalSettings.nodeName && !gSettingsDirty;
    SetWindowTextW(Item(ID_NODE_NAME), L"");
    SendMessageW(gWindow, WM_CLOSE, 0, 0);
    const bool failedAutoSaveBlocksClose = IsWindow(gWindow);
    SetWindowTextW(Item(ID_NODE_NAME), originalNodeName.c_str());
    SendMessageW(gWindow, WM_TIMER, TIMER_SETTINGS_AUTOSAVE, 0);
    const bool settingsAutoSaveEnabled =
        autoSaveWasScheduled && autoSavePersisted && autoSaveRestored &&
        failedAutoSaveBlocksClose;
    auto controlWidth = [](HWND control) {
        RECT rect{};
        GetWindowRect(control, &rect);
        return static_cast<int>(rect.right - rect.left);
    };
    const int settingsWidth = controlWidth(Item(ID_NODE_NAME));
    const bool settingsControlsShareWidth = settingsWidth > 0 &&
        controlWidth(Item(ID_IDLE_SECONDS)) == settingsWidth &&
        controlWidth(Item(ID_THEME)) == settingsWidth &&
        controlWidth(Item(ID_AUTO_START)) == settingsWidth &&
        controlWidth(Item(ID_SILENT_AUTO_START)) == settingsWidth &&
        controlWidth(Item(ID_INSTALL_BLENDER)) == settingsWidth;
    const bool jobsSupportMultiSelect =
        (GetWindowLongPtrW(Item(ID_JOBS), GWL_STYLE) & LVS_SINGLESEL) == 0;
    const bool contributionsSupportMultiSelect =
        (GetWindowLongPtrW(Item(ID_CONTRIBUTION_LIST), GWL_STYLE) & LVS_SINGLESEL) == 0;
    Config profileDrawerProbeConfig;
    profileDrawerProbeConfig.renderSoftwareProfiles = {
        {"ui-profile-a", "Blender 4.3", "Windows", "Blender", L"C:\\Blender43\\blender.exe"},
        {"ui-profile-b", "Blender 5.2", "Windows", "Blender", L"C:\\Blender52\\blender.exe"},
    };
    HWND profileDrawerProbe = CreateWindowExW(
        0, L"LISTBOX", L"", WS_CHILD | LBS_OWNERDRAWFIXED | LBS_HASSTRINGS,
        -400, -400, 320, 120, gWindow, nullptr, GetModuleHandleW(nullptr), nullptr);
    const std::string originalConfigProfileId = gConfigProfileId;
    gConfigProfileId = "ui-profile-b";
    PopulateRenderSoftwareProfileDrawer(profileDrawerProbe, profileDrawerProbeConfig);
    const bool softwareProfileDrawerListsAllProfiles =
        SendMessageW(profileDrawerProbe, LB_GETCOUNT, 0, 0) == 2 &&
        SendMessageW(profileDrawerProbe, LB_GETCURSEL, 0, 0) == 1;
    Config profileDeleteProbe = profileDrawerProbeConfig;
    profileDeleteProbe.lastRenderSoftwareProfileId = "ui-profile-b";
    profileDeleteProbe.blenderPath =
        profileDeleteProbe.renderSoftwareProfiles.back().installPath;
    std::wstring removedProfileName;
    std::wstring profileDeleteError;
    const bool removedSelectedProfile = RemoveRenderSoftwareProfile(
        profileDeleteProbe, "ui-profile-b", removedProfileName,
        profileDeleteError);
    std::wstring protectedProfileName;
    std::wstring protectedProfileError;
    const bool protectedLastProfile = !RemoveRenderSoftwareProfile(
        profileDeleteProbe, "ui-profile-a", protectedProfileName,
        protectedProfileError);
    const bool softwareProfileDeletionIsPersistable =
        removedSelectedProfile && protectedLastProfile &&
        profileDeleteProbe.renderSoftwareProfiles.size() == 1 &&
        profileDeleteProbe.lastRenderSoftwareProfileId == "ui-profile-a" &&
        profileDeleteProbe.blenderPath ==
            profileDeleteProbe.renderSoftwareProfiles.front().installPath;
    DestroyWindow(profileDrawerProbe);
    gConfigProfileId = originalConfigProfileId;
    HWND selectorAffordanceProbe = CreateWindowExW(
        0, L"BUTTON", L"当前配置", WS_CHILD | BS_OWNERDRAW,
        -400, -400, 320, 38, gWindow, nullptr, GetModuleHandleW(nullptr), nullptr);
    SetPropW(selectorAffordanceProbe, kDropdownAffordanceProperty,
             reinterpret_cast<HANDLE>(1));
    const bool softwareProfileSelectorHasChevron =
        GetPropW(selectorAffordanceProbe, kDropdownAffordanceProperty) != nullptr;
    DestroyWindow(selectorAffordanceProbe);
    const RECT drawerSelectorBefore{120, 180, 520, 218};
    const RECT drawerSelectorAfter{310, 265, 710, 303};
    const RECT drawerBefore = RenderSoftwareProfileDrawerBounds(
        drawerSelectorBefore, profileDrawerProbeConfig.renderSoftwareProfiles.size());
    const RECT drawerAfter = RenderSoftwareProfileDrawerBounds(
        drawerSelectorAfter, profileDrawerProbeConfig.renderSoftwareProfiles.size());
    const bool softwareProfileDrawerFollowsOwner =
        drawerAfter.left - drawerBefore.left == 190 &&
        drawerAfter.top - drawerBefore.top == 85 &&
        drawerAfter.right - drawerBefore.right == 190 &&
        drawerAfter.bottom - drawerBefore.bottom == 85;
    HWND detailVisibilityProbe = CreateWindowExW(
        0, L"BUTTON", L"probe", WS_CHILD | WS_VISIBLE,
        -400, -400, 40, 40, gWindow, nullptr, GetModuleHandleW(nullptr), nullptr);
    const bool unchangedDetailRefreshIsVisualNoop =
        !SetControlVisibleIfChanged(detailVisibilityProbe, true) &&
        SetControlVisibleIfChanged(detailVisibilityProbe, false) &&
        !SetControlVisibleIfChanged(detailVisibilityProbe, false);
    DestroyWindow(detailVisibilityProbe);
    const RECT detailLayoutClient{0, 0, 1500, 900};
    const auto detailLayoutProbe = CalculateDetailWindowLayout(
        detailLayoutClient, kChromeHeightDip, false, false);
    const auto detailPrimaryActionLayoutProbe = CalculateDetailWindowLayout(
        detailLayoutClient, kChromeHeightDip, true, false);
    const bool detailPreviewUsesRightColumn =
        detailLayoutProbe.preview.right == 1480 &&
        detailLayoutProbe.preview.right - detailLayoutProbe.preview.left >= 600 &&
        detailLayoutProbe.preview.bottom - detailLayoutProbe.preview.top >= 650;
    constexpr int detailVisibleColumnWidth = 55 + 96 + 130 + 92 + 135 + 135;
    const bool detailListHasNoUnusedBlankColumn =
        detailLayoutProbe.frames.right - detailLayoutProbe.frames.left -
            detailVisibleColumnWidth <= 64 &&
        detailLayoutProbe.preview.left == detailLayoutProbe.frames.right + 16;
    const bool detailSummarySitsBelowPreview =
        detailLayoutProbe.summary.top > detailLayoutProbe.preview.bottom &&
        detailLayoutProbe.summary.left == detailLayoutProbe.preview.left &&
        detailLayoutProbe.summary.right == detailLayoutProbe.preview.right;
    const bool detailExplorerActionsAreIconButtons =
        detailLayoutProbe.openOutput.right - detailLayoutProbe.openOutput.left == 48 &&
        detailLayoutProbe.openOutput.bottom - detailLayoutProbe.openOutput.top == 48 &&
        detailLayoutProbe.openSource.right - detailLayoutProbe.openSource.left == 48 &&
        detailLayoutProbe.openSource.bottom - detailLayoutProbe.openSource.top == 48;
    const bool detailPrimaryActionUsesRightmostSlot =
        EqualRect(&detailPrimaryActionLayoutProbe.stop,
                  &detailPrimaryActionLayoutProbe.fullRender) != FALSE &&
        detailPrimaryActionLayoutProbe.stop.right == detailLayoutClient.right - 20 &&
        detailPrimaryActionLayoutProbe.openSource.right <
            detailPrimaryActionLayoutProbe.stop.left &&
        detailPrimaryActionLayoutProbe.openOutput.right <
            detailPrimaryActionLayoutProbe.openSource.left;
    wchar_t mainTitle[128]{};
    GetWindowTextW(gWindow, mainTitle, static_cast<int>(std::size(mainTitle)));
    const bool mainWindowTitleIsProductOnly = wcscmp(mainTitle, L"RenderMesh") == 0;
    HWND globalStatus = Item(ID_PREFLIGHT);
    RECT globalStatusRect{};
    if (globalStatus) {
        GetWindowRect(globalStatus, &globalStatusRect);
        MapWindowPoints(HWND_DESKTOP, gWindow,
                        reinterpret_cast<POINT*>(&globalStatusRect), 2);
    }
    const bool globalStatusBarIsSingleSurface = globalStatus &&
        GetPropW(globalStatus, kStaticSurfaceRoleProperty) ==
            reinterpret_cast<HANDLE>(static_cast<ULONG_PTR>(StaticSurfaceRole::Surface)) &&
        mainClient.bottom - globalStatusRect.bottom == 20 &&
        !Item(ID_STATUS) && !Item(ID_MANUAL_STATUS) && !Item(ID_SETTINGS_STATUS);
    RECT jobsRect{}, addJobRect{};
    GetWindowRect(jobs, &jobsRect);
    GetWindowRect(Item(ID_ADD_JOB), &addJobRect);
    MapWindowPoints(HWND_DESKTOP, gWindow, reinterpret_cast<POINT*>(&jobsRect), 2);
    MapWindowPoints(HWND_DESKTOP, gWindow, reinterpret_cast<POINT*>(&addJobRect), 2);
    const bool statusBarAlignsWithContent = globalStatus &&
        globalStatusRect.left == jobsRect.left &&
        globalStatusRect.right == jobsRect.right;
    const bool addJobClearsStatusBar =
        addJobRect.bottom <= globalStatusRect.top - 12;
    const int pageBeforeHelpProbe = gCurrentPage;
    ShowPage(1);
    const bool taskHelpUsesStatus =
        Text(Item(ID_PREFLIGHT)).find(L"拖入 .blend 文件") != std::wstring::npos;
    ShowPage(2);
    const bool contributionHelpUsesStatus =
        Text(Item(ID_PREFLIGHT)).find(L"这里只记录本机") != std::wstring::npos;
    ShowPage(pageBeforeHelpProbe);
    const bool pageHelpMovedToStatusBar =
        !Item(ID_TASK_HELP) && !Item(ID_TASK_QUEUE_LABEL) &&
        taskHelpUsesStatus && contributionHelpUsesStatus;
    RECT overlayRect{};
    GetWindowRect(Item(ID_DROP_OVERLAY), &overlayRect);
    MapWindowPoints(HWND_DESKTOP, gWindow,
                    reinterpret_cast<POINT*>(&overlayRect), 2);
    const bool blendDragOverlayCoversTaskList =
        EqualRect(&overlayRect, &jobsRect) != FALSE;
    const bool dragOverlayAcceptsBlendOnly =
        IsBlendDropPath(L"C:\\project\\scene.BLEND") &&
        !IsBlendDropPath(L"C:\\project\\scene.blend1") &&
        !IsBlendDropPath(L"C:\\project\\notes.txt");

    RenderJob deleteProbe;
    deleteProbe.id = "ui-delete-confirmation-probe";
    deleteProbe.name = "不会被删除的测试作业";
    ShowDeleteConfirmation(deleteProbe);
    const bool deleteDialogIsInternal = gInternalDialog &&
        GetParent(gInternalDialog) == gWindow &&
        (GetWindowLongPtrW(gInternalDialog, GWL_STYLE) & WS_CHILD) != 0;
    HWND deleteCancel = gInternalDialog
        ? GetDlgItem(gInternalDialog, ID_INTERNAL_CANCEL) : nullptr;
    HWND deleteConfirm = gInternalDialog
        ? GetDlgItem(gInternalDialog, ID_INTERNAL_CONFIRM) : nullptr;
    const bool deleteDialogDefaultsToCancel = GetFocus() == deleteCancel;
    RECT modalWindowRect{};
    GetWindowRect(gWindow, &modalWindowRect);
    SetWindowPos(gWindow, nullptr, modalWindowRect.left, modalWindowRect.top,
                 modalWindowRect.right - modalWindowRect.left + 37,
                 modalWindowRect.bottom - modalWindowRect.top + 23,
                 SWP_NOACTIVATE | SWP_NOZORDER);
    RECT modalParentClient{}, modalOverlayClient{};
    GetClientRect(gWindow, &modalParentClient);
    if (gInternalDialog) GetClientRect(gInternalDialog, &modalOverlayClient);
    const bool deleteDialogCoversResizedWindow =
        modalOverlayClient.right == modalParentClient.right &&
        modalOverlayClient.bottom == modalParentClient.bottom;
    SetWindowPos(gWindow, nullptr, modalWindowRect.left, modalWindowRect.top,
                 modalWindowRect.right - modalWindowRect.left,
                 modalWindowRect.bottom - modalWindowRect.top,
                 SWP_NOACTIVATE | SWP_NOZORDER);
    if (deleteCancel) SendMessageW(deleteCancel, WM_KEYDOWN, VK_TAB, 0);
    const bool deleteDialogTabMovesFocus = GetFocus() == deleteConfirm;
    if (deleteConfirm) SendMessageW(deleteConfirm, WM_KEYDOWN, VK_ESCAPE, 0);
    const bool deleteDialogEscapeCloses = !gInternalDialog;
    ShowDeleteConfirmation(deleteProbe);
    deleteCancel = gInternalDialog
        ? GetDlgItem(gInternalDialog, ID_INTERNAL_CANCEL) : nullptr;
    if (deleteCancel) SendMessageW(deleteCancel, WM_KEYDOWN, VK_RETURN, 0);
    const bool deleteDialogEnterHonorsCancelFocus = !gInternalDialog;

    ShowPreflightWarningOverride(L"预检未通过：缺失 1 项。\n• C:\\missing.png");
    HWND warningCancel = gInternalDialog
        ? GetDlgItem(gInternalDialog, ID_INTERNAL_CANCEL) : nullptr;
    HWND warningConfirm = gInternalDialog
        ? GetDlgItem(gInternalDialog, ID_INTERNAL_CONFIRM) : nullptr;
    const bool warningOverrideIsInternal =
        gInternalDialog && gInternalDialogMode == InternalDialogMode::PreflightWarningOverride &&
        GetParent(gInternalDialog) == gWindow;
    const bool warningOverrideDefaultsToCancel = GetFocus() == warningCancel;
    const bool warningOverrideIsExplicit =
        warningConfirm && Text(warningConfirm) == L"无视警告" &&
        gInternalDialogBody.find(L"参与电脑上的测试帧确认") != std::wstring::npos;
    if (warningCancel) SendMessageW(warningCancel, WM_KEYDOWN, VK_ESCAPE, 0);
    const bool warningOverrideEscapeCloses = !gInternalDialog;

    ShowInternalDialog(
        InternalDialogMode::CloseBehavior,
        L"关闭 RenderMesh", L"",
        L"直接退出会停止本机发现、同步和渲染服务。\n"
        L"最小化到系统托盘后，RenderMesh 会继续在后台运行。");
    HWND closeExit = gInternalDialog
        ? GetDlgItem(gInternalDialog, ID_INTERNAL_CANCEL) : nullptr;
    HWND closeTray = gInternalDialog
        ? GetDlgItem(gInternalDialog, ID_INTERNAL_CONFIRM) : nullptr;
    HWND closeRemember = gInternalDialog
        ? GetDlgItem(gInternalDialog, ID_INTERNAL_REMEMBER) : nullptr;
    const bool closeDialogIsInternal =
        gInternalDialog && gInternalDialogMode == InternalDialogMode::CloseBehavior &&
        GetParent(gInternalDialog) == gWindow &&
        (GetWindowLongPtrW(gInternalDialog, GWL_STYLE) & WS_CHILD) != 0;
    const bool closeDialogHasExplicitChoices =
        closeExit && Text(closeExit) == L"直接退出" &&
        closeTray && Text(closeTray) == L"最小化到系统托盘" &&
        closeRemember && Text(closeRemember) == L"记住本次选择";
    const bool closeDialogDefaultsToTray = GetFocus() == closeTray;
    if (closeRemember) SendMessageW(closeRemember, BM_CLICK, 0, 0);
    const bool closeDialogRememberIsCheckbox = closeRemember &&
        SendMessageW(closeRemember, BM_GETCHECK, 0, 0) == BST_CHECKED;
    const uint64_t mutationsBeforeModalRefresh = gListMutationCount;
    RefreshLists();
    const std::array<int, 3> backgroundListIds{
        ID_NODES, ID_JOBS, ID_CONTRIBUTION_LIST,
    };
    const bool modalBackgroundListsClipSiblings = std::all_of(
        backgroundListIds.begin(), backgroundListIds.end(), [](int id) {
            const HWND list = Item(id);
            return list &&
                (GetWindowLongPtrW(list, GWL_STYLE) & WS_CLIPSIBLINGS) != 0;
        });
    // Exercise the real regression path: a background list repaints while the
    // modal is open. WS_CLIPSIBLINGS must keep its pixels out of the overlay.
    RedrawWindow(jobs, nullptr, nullptr,
                 RDW_INVALIDATE | RDW_ERASE | RDW_UPDATENOW);
    if (gInternalDialog)
        SetWindowPos(gInternalDialog, HWND_TOP, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    const POINT modalHitPoint{
        (jobsRect.left + jobsRect.right) / 2,
        (jobsRect.top + jobsRect.bottom) / 2,
    };
    const HWND modalHit = ChildWindowFromPointEx(
        gWindow, modalHitPoint, CWP_SKIPINVISIBLE | CWP_SKIPDISABLED);
    const bool modalDialogBlocksListRefresh =
        gInternalDialog && modalHit == gInternalDialog &&
        gMainRefreshDeferredByModal &&
        gListMutationCount == mutationsBeforeModalRefresh;
    CloseInternalDialog();

    gManualUpdateCheck = true;
    SendMessageW(gWindow, WM_RM_UPDATE_CHECKED, 0,
                 reinterpret_cast<LPARAM>(new UpdateCheckResult()));
    HWND latestNoticeConfirm = gInternalDialog
        ? GetDlgItem(gInternalDialog, ID_INTERNAL_CONFIRM) : nullptr;
    const bool latestVersionNoticeUsesInternalDialog =
        gInternalDialog && gInternalDialogMode == InternalDialogMode::Notice &&
        GetParent(gInternalDialog) == gWindow &&
        gInternalDialogBody.find(L"当前已是局域网内可用的最新版本") !=
            std::wstring::npos &&
        latestNoticeConfirm && Text(latestNoticeConfirm) == L"知道了";
    if (latestNoticeConfirm) SendMessageW(latestNoticeConfirm, BM_CLICK, 0, 0);
    const bool latestVersionNoticeClosesFromConfirm = !gInternalDialog;

    const int pageBeforeWorkflowProbe = gCurrentPage;
    ShowPage(3);
    HWND workflowBackgroundAction = Item(ID_INSTALL_BLENDER);
    RECT workflowBackgroundActionRect{};
    GetWindowRect(workflowBackgroundAction, &workflowBackgroundActionRect);
    MapWindowPoints(HWND_DESKTOP, gWindow,
                    reinterpret_cast<POINT*>(&workflowBackgroundActionRect), 2);
    gMainRefreshDeferredByModal = false;
    const uint64_t refreshesBeforeWorkflowProbe = gRefreshCount;
    ShowWorkflowProgress(WorkflowProgressStage::Inspecting);
    RefreshLists();
    RedrawWindow(workflowBackgroundAction, nullptr, nullptr,
                 RDW_INVALIDATE | RDW_ERASE | RDW_UPDATENOW);
    if (gWorkflowProgressOverlay)
        SetWindowPos(gWorkflowProgressOverlay, HWND_TOP, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    RECT workflowOverlayClient{}, workflowParentClient{};
    RECT workflowOverlayRect{};
    GetClientRect(gWindow, &workflowParentClient);
    if (gWorkflowProgressOverlay) {
        GetClientRect(gWorkflowProgressOverlay, &workflowOverlayClient);
        GetWindowRect(gWorkflowProgressOverlay, &workflowOverlayRect);
        MapWindowPoints(HWND_DESKTOP, gWindow,
                        reinterpret_cast<POINT*>(&workflowOverlayRect), 2);
    }
    POINT blockedPoint{
        (addJobRect.left + addJobRect.right) / 2,
        (addJobRect.top + addJobRect.bottom) / 2,
    };
    HWND workflowHit = ChildWindowFromPointEx(
        gWindow, blockedPoint, CWP_SKIPINVISIBLE | CWP_SKIPDISABLED);
    HWND workflowProgress = gWorkflowProgressOverlay
        ? GetDlgItem(gWorkflowProgressOverlay, ID_WORKFLOW_PROGRESS) : nullptr;
    const bool workflowProgressBlocksMainInput =
        gWorkflowProgressOverlay && workflowHit == gWorkflowProgressOverlay &&
        workflowOverlayClient.right == workflowParentClient.right &&
        workflowOverlayRect.top == ChromeHeight(gWindow) &&
        workflowOverlayRect.bottom == workflowParentClient.bottom &&
        workflowProgress && SendMessageW(workflowProgress, PBM_GETPOS, 0, 0) == 20 &&
        GetFocus() == gWorkflowProgressOverlay;
    const POINT workflowCaptionPoint{DpiValue(gWindow, 220),
                                     ChromeHeight(gWindow) / 2};
    const bool workflowProgressAllowsWindowDrag =
        workflowOverlayRect.top == ChromeHeight(gWindow) &&
        ChromeHitTest(gWindow, workflowCaptionPoint) == HTCAPTION;
    HWND workflowCancel = gWorkflowProgressOverlay
        ? GetDlgItem(gWorkflowProgressOverlay, ID_WORKFLOW_CANCEL) : nullptr;
    const bool workflowCancelHiddenBeforeTimeout =
        workflowCancel && !IsWindowVisible(workflowCancel);
    gWorkflowProgressStartedAt =
        std::chrono::steady_clock::now() - std::chrono::seconds(61);
    if (gWorkflowProgressOverlay)
        SendMessageW(gWorkflowProgressOverlay, WM_TIMER,
                     TIMER_WORKFLOW_PROGRESS, 0);
    const bool workflowProgressShowsCancelAfterTimeout =
        workflowCancelHiddenBeforeTimeout && workflowCancel &&
        IsWindowVisible(workflowCancel) && Text(workflowCancel) == L"取消上传";
    gPreflightStop = false;
    if (workflowCancel) SendMessageW(workflowCancel, BM_CLICK, 0, 0);
    const bool workflowProgressCancelSetsStopSignal =
        gPreflightStop.load() && gWorkflowProgressCancelRequested;
    gPreflightStop = false;
    const POINT settingsActionHitPoint{
        (workflowBackgroundActionRect.left + workflowBackgroundActionRect.right) / 2,
        (workflowBackgroundActionRect.top + workflowBackgroundActionRect.bottom) / 2,
    };
    const HWND settingsActionHit = ChildWindowFromPointEx(
        gWindow, settingsActionHitPoint, CWP_SKIPINVISIBLE | CWP_SKIPDISABLED);
    const bool workflowOverlayBlocksSettingsAction =
        gWorkflowProgressOverlay && settingsActionHit == gWorkflowProgressOverlay;
    const bool workflowBackgroundControlsClipSiblings =
        workflowBackgroundAction &&
        (GetWindowLongPtrW(workflowBackgroundAction, GWL_STYLE) &
         WS_CLIPSIBLINGS) != 0;
    const bool workflowRefreshIsDeferred =
        gMainRefreshDeferredByModal &&
        gRefreshCount == refreshesBeforeWorkflowProbe;
    CloseWorkflowProgress();
    gMainRefreshDeferredByModal = false;
    ShowPage(pageBeforeWorkflowProbe);

    const auto catalogBeforeSelectorProbe = gBlenderCatalog;
    gBlenderCatalog = {
        {"5.2", "5.2.0", L"Blender 5.2.0  · LTS", true, L"", L""},
        {"5.1", "5.1.2", L"Blender 5.1.2", false, L"", L""},
    };
    ShowInternalDialog(
        InternalDialogMode::BlenderVersionPicker, L"选择 Blender 版本", L"",
        L"以下版本来自 Blender 官方镜像；LTS 版本已单独标记。");
    HWND blenderVersionSelector = gInternalDialog
        ? GetDlgItem(gInternalDialog, ID_INTERNAL_VERSION) : nullptr;
    const LONG_PTR blenderVersionSelectorStyle = blenderVersionSelector
        ? GetWindowLongPtrW(blenderVersionSelector, GWL_STYLE) : 0;
    const bool blenderVersionSelectorUsesInternalStyle =
        blenderVersionSelector &&
        (blenderVersionSelectorStyle & CBS_OWNERDRAWFIXED) != 0 &&
        SendMessageW(blenderVersionSelector, CB_GETCOUNT, 0, 0) == 2;
    CloseInternalDialog();
    gBlenderCatalog = catalogBeforeSelectorProbe;

    const COLORREF expectedListSurface =
        gDarkTheme ? RGB(31, 32, 42) : RGB(255, 255, 255);
    HWND listThemeProbeParent = CreateWindowExW(
        0, L"STATIC", L"", WS_CHILD, -300, -300, 220, 120, gWindow, nullptr,
        GetModuleHandleW(nullptr), nullptr);
    HWND detailListThemeProbe = CreateWindowExW(
        0, WC_LISTVIEWW, L"", WS_CHILD | LVS_REPORT, 0, 0, 100, 100,
        listThemeProbeParent,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_DETAIL_FRAMES)),
        GetModuleHandleW(nullptr), nullptr);
    HWND uploadListThemeProbe = CreateWindowExW(
        0, WC_LISTVIEWW, L"", WS_CHILD | LVS_REPORT, 110, 0, 100, 100,
        listThemeProbeParent,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_UPLOAD_LIST)),
        GetModuleHandleW(nullptr), nullptr);
    ConfigureList(detailListThemeProbe);
    ConfigureList(uploadListThemeProbe);
    AddColumn(detailListThemeProbe, 0, 100, L"探针");
    AddColumn(uploadListThemeProbe, 0, 100, L"探针");
    auto listClientSampleColor = [](HWND list) {
        RECT client{};
        GetClientRect(list, &client);
        const int width = std::max(1L, client.right - client.left);
        const int height = std::max(1L, client.bottom - client.top);
        BITMAPINFO bitmapInfo{};
        bitmapInfo.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bitmapInfo.bmiHeader.biWidth = width;
        bitmapInfo.bmiHeader.biHeight = -height;
        bitmapInfo.bmiHeader.biPlanes = 1;
        bitmapInfo.bmiHeader.biBitCount = 32;
        bitmapInfo.bmiHeader.biCompression = BI_RGB;
        HDC screen = GetDC(nullptr);
        HDC memory = CreateCompatibleDC(screen);
        void* raw = nullptr;
        HBITMAP bitmap = CreateDIBSection(
            screen, &bitmapInfo, DIB_RGB_COLORS, &raw, nullptr, 0);
        COLORREF sampled = CLR_INVALID;
        if (memory && bitmap && raw) {
            const HGDIOBJ previous = SelectObject(memory, bitmap);
            std::fill_n(static_cast<DWORD*>(raw),
                        static_cast<size_t>(width) * height, 0x00FF00FFu);
            // Exercise the exact message that caused the 0.2.31 transparency
            // regression. The list must fill an arbitrary target DC with its
            // configured surface colour instead of claiming the erase while
            // leaving the previous desktop pixels untouched.
            SendMessageW(list, WM_ERASEBKGND,
                         reinterpret_cast<WPARAM>(memory), 0);
            const int sampleX = std::clamp(width - 8, 0, width - 1);
            const int sampleY = std::clamp(height - 8, 0, height - 1);
            const BYTE* pixel = reinterpret_cast<const BYTE*>(raw) +
                (static_cast<size_t>(sampleY) * width + sampleX) * 4;
            sampled = RGB(pixel[2], pixel[1], pixel[0]);
            SelectObject(memory, previous);
        }
        if (bitmap) DeleteObject(bitmap);
        if (memory) DeleteDC(memory);
        if (screen) ReleaseDC(nullptr, screen);
        return sampled;
    };
    const bool originalDarkTheme = gDarkTheme;
    gDarkTheme = false;
    ApplyListTheme(detailListThemeProbe);
    ApplyListTheme(uploadListThemeProbe);
    const COLORREF lightDetailListPaint =
        listClientSampleColor(detailListThemeProbe);
    const COLORREF lightUploadListPaint =
        listClientSampleColor(uploadListThemeProbe);
    const bool lightListThemeProbePasses =
        ListView_GetBkColor(detailListThemeProbe) == RGB(255, 255, 255) &&
        ListView_GetTextBkColor(detailListThemeProbe) == RGB(255, 255, 255) &&
        ListView_GetBkColor(uploadListThemeProbe) == RGB(255, 255, 255) &&
        ListView_GetTextBkColor(uploadListThemeProbe) == RGB(255, 255, 255) &&
        lightDetailListPaint == RGB(255, 255, 255) &&
        lightUploadListPaint == RGB(255, 255, 255);
    gDarkTheme = true;
    ApplyListTheme(detailListThemeProbe);
    ApplyListTheme(uploadListThemeProbe);
    const COLORREF darkDetailListPaint =
        listClientSampleColor(detailListThemeProbe);
    const COLORREF darkUploadListPaint =
        listClientSampleColor(uploadListThemeProbe);
    const bool darkListThemeProbePasses =
        ListView_GetBkColor(detailListThemeProbe) == RGB(31, 32, 42) &&
        ListView_GetTextBkColor(detailListThemeProbe) == RGB(31, 32, 42) &&
        ListView_GetBkColor(uploadListThemeProbe) == RGB(31, 32, 42) &&
        ListView_GetTextBkColor(uploadListThemeProbe) == RGB(31, 32, 42) &&
        darkDetailListPaint == RGB(31, 32, 42) &&
        darkUploadListPaint == RGB(31, 32, 42);
    gDarkTheme = originalDarkTheme;
    DestroyWindow(listThemeProbeParent);
    const bool themedListBlankAreasMatchSurface =
        ListView_GetBkColor(Item(ID_NODES)) == expectedListSurface &&
        ListView_GetBkColor(Item(ID_JOBS)) == expectedListSurface &&
        ListView_GetBkColor(Item(ID_CONTRIBUTION_LIST)) == expectedListSurface &&
        lightListThemeProbePasses && darkListThemeProbePasses;
    RenderJob completedActionProbe;
    completedActionProbe.stage = JobStage::Completed;
    FrameTask completedFrameProbe;
    completedFrameProbe.taskId = "completed-action-probe";
    completedFrameProbe.state = TaskState::Completed;
    completedActionProbe.frames.push_back(completedFrameProbe);
    RenderJob runningActionProbe;
    runningActionProbe.stage = JobStage::Rendering;
    FrameTask runningFrameProbe;
    runningFrameProbe.taskId = "running-action-probe";
    runningFrameProbe.state = TaskState::Rendering;
    runningActionProbe.frames.push_back(runningFrameProbe);
    RenderJob failedActionProbe;
    failedActionProbe.stage = JobStage::Failed;
    FrameTask failedFrameProbe;
    failedFrameProbe.taskId = "failed-action-probe";
    failedFrameProbe.state = TaskState::Failed;
    failedActionProbe.frames.push_back(failedFrameProbe);
    const bool completedJobHidesStopAction = !IsJobInProgress(completedActionProbe);
    const bool runningJobShowsStopAction = IsJobInProgress(runningActionProbe);
    const bool failedJobShowsRetryAllAction = HasFailedFrames(failedActionProbe);
    const bool failedFrameShowsRetryCurrentAction =
        CanRetryFrame(failedActionProbe.frames.front());
    RenderJob testCompletedActionProbe;
    testCompletedActionProbe.stage = JobStage::TestCompleted;
    const bool testCompletedShowsFullRenderAction =
        DetailPrimaryActionFor(testCompletedActionProbe) ==
            DetailPrimaryAction::FullRender;
    const bool detailPrimaryActionsAreExclusive =
        DetailPrimaryActionFor(testCompletedActionProbe) != DetailPrimaryAction::Stop &&
        DetailPrimaryActionFor(runningActionProbe) == DetailPrimaryAction::Stop;
    const bool assignedLocalShowsRendering =
        LocalComputeStatusText(true, true, true, 1, true) == L"正在渲染";
    const bool assignedLocalShowsQueued =
        LocalComputeStatusText(true, true, true, 1, false) == L"排队中";
    const bool localReadyShowsAvailable =
        LocalComputeStatusText(true, true, true, 0, false) == L"可接单";
    const bool localActiveShowsInUse =
        LocalComputeStatusText(true, true, false, 0, false) == L"使用中";
    const RECT detailClientProbe{0, 0, 1220, 650};
    const RECT detailStopProbe = DetailStopButtonRect(detailClientProbe);
    const bool stopActionIsBottomRightIcon =
        detailStopProbe.right == 1200 && detailStopProbe.bottom == 630 &&
        detailStopProbe.right - detailStopProbe.left == 54 &&
        detailStopProbe.bottom - detailStopProbe.top == 54;
    const bool defaultStatusHidesIdleCounter =
        ReadyStatusText(false, true) == L"已就绪" &&
        ReadyStatusText(false, false) == L"未检测到 Blender" &&
        ReadyStatusText(true, true) == L"正在邀请码配对";
    const bool dashboardStatusIgnoresPeerPollingDetails =
        DashboardStatusText(false, true,
                            L"已连接 testlaptop（192.168.28.28:8391）") == L"已就绪";
    const bool dashboardStatusShowsHandshakeUpgrade =
        DashboardStatusText(false, true,
                            L"连接被拒绝：对方握手协议过旧，请更新到 0.2.28") ==
            L"连接被拒绝：对方握手协议过旧，请更新到 0.2.28";
    Peer activeRemoteProbe;
    activeRemoteProbe.rendererReady = true;
    activeRemoteProbe.shareEnabled = true;
    activeRemoteProbe.idleReadyKnown = true;
    activeRemoteProbe.idleReady = false;
    activeRemoteProbe.idleSeconds = 0;
    const bool remoteActiveShowsInUse =
        RemoteComputeStatusText(activeRemoteProbe) == L"使用中";
    activeRemoteProbe.idleReady = true;
    activeRemoteProbe.idleSeconds = 170592;
    const bool remoteReadyHidesIdleDuration =
        RemoteComputeStatusText(activeRemoteProbe) == L"可接单";
    activeRemoteProbe.idleReadyKnown = false;
    const bool remoteUnknownReadinessShowsInUse =
        RemoteComputeStatusText(activeRemoteProbe) == L"使用中";
    activeRemoteProbe.idleReadyKnown = true;
    activeRemoteProbe.idleReady = false;
    activeRemoteProbe.idleSeconds = 0;
    activeRemoteProbe.assigned = 1;
    const bool assignedRemoteShowsQueued =
        RemoteComputeStatusText(activeRemoteProbe) == L"排队中";
    activeRemoteProbe.activity.rendering = true;
    const bool assignedRemoteShowsRendering =
        RemoteComputeStatusText(activeRemoteProbe) == L"正在渲染";
    const int originalPage = gCurrentPage;
    const int originalPageBeforeSettings = gPageBeforeSettings;
    ShowPage(1);
    NavigateSidebarPage(3);
    const bool enteredSettingsPage = gCurrentPage == 3;
    NavigateSidebarPage(3);
    const bool settingsNavigationTogglesBack =
        enteredSettingsPage && gCurrentPage == 1;
    ShowPage(originalPage);
    gPageBeforeSettings = originalPageBeforeSettings;
    HMENU contextMenuProbe = CreatePopupMenu();
    DestroyMenu(contextMenuProbe);
    const bool contextMenuRepaintedBeforeAction =
        RedrawWindow(gWindow, nullptr, nullptr,
                     RDW_INVALIDATE | RDW_ERASE | RDW_FRAME |
                         RDW_ALLCHILDREN | RDW_UPDATENOW) != FALSE;
    json metrics = {
        {"jobs_visible_rows", jobsVisibleRows},
        {"share_text_width", shareLayout.icon.right - shareLayout.icon.left},
        {"share_text_height", shareLayout.icon.bottom - shareLayout.icon.top},
        {"share_label_width", shareRect.right - shareRect.left},
        {"share_label_height", shareRect.bottom - shareRect.top},
        {"share_text_fits", shareGroupRight - shareGroupLeft <=
                                 shareRect.right - shareRect.left &&
                             shareLayout.icon.bottom - shareLayout.icon.top <=
                                 shareRect.bottom - shareRect.top},
        {"share_control_uses_network_icon", shareControlUsesNetworkIcon},
        {"share_control_matches_navigation_size",
         shareControlMatchesNavigationSize},
        {"tray_share_toggle_repaints_main_switch",
         trayShareToggleRepaintsMainSwitch},
        {"auto_start_settings_available", autoStartSettingsAvailable},
        {"auto_start_toggle_state_round_trips", autoStartToggleStateRoundTrips},
        {"startup_toggles_are_borderless", startupTogglesAreBorderless},
        {"silent_auto_start_dependency_visible", silentAutoStartDependencyVisible},
        {"silent_start_tooltip_available", silentStartTooltipAvailable},
        {"version_update_controls_available", versionUpdateControlsAvailable},
        {"project_storage_settings_available",
         projectStorageSettingsAvailable},
        {"manual_blender_path_settings_available",
         manualBlenderPathSettingsAvailable},
        {"latest_version_notice_uses_internal_dialog",
         latestVersionNoticeUsesInternalDialog},
        {"latest_version_notice_closes_from_confirm",
         latestVersionNoticeClosesFromConfirm},
        {"silent_startup_only_applies_to_auto_start",
         silentStartupOnlyAppliesToAutoStart},
        {"auto_start_command_is_quoted_and_marked",
         autoStartCommandIsQuotedAndMarked},
        {"dashboard_shows_matching_id", dashboardShowsMatchingId},
        {"dashboard_matching_id_editor_available", dashboardMatchingIdEditorAvailable},
        {"dashboard_matching_id_click_replaces_whole_value",
         dashboardMatchingIdClickReplacesWholeValue},
        {"local_ready_shows_available", localReadyShowsAvailable},
        {"local_active_shows_in_use", localActiveShowsInUse},
        {"remote_active_shows_in_use", remoteActiveShowsInUse},
        {"remote_ready_hides_idle_duration", remoteReadyHidesIdleDuration},
        {"remote_unknown_readiness_shows_in_use",
         remoteUnknownReadinessShowsInUse},
        {"test_completed_shows_full_render_action",
         testCompletedShowsFullRenderAction},
        {"detail_primary_actions_are_exclusive", detailPrimaryActionsAreExclusive},
        {"assigned_local_shows_rendering", assignedLocalShowsRendering},
        {"assigned_local_shows_queued", assignedLocalShowsQueued},
        {"assigned_remote_shows_queued", assignedRemoteShowsQueued},
        {"assigned_remote_shows_rendering", assignedRemoteShowsRendering},
        {"detail_primary_action_uses_rightmost_slot",
         detailPrimaryActionUsesRightmostSlot},
        {"settings_hides_matching_id_editor", settingsHidesMatchingIdEditor},
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
        {"multiple_selection_preserved", multipleSelectionPreserved},
        {"focus_preserved", focusPreserved},
        {"horizontal_scroll_preserved", horizontalScrollPreserved},
        {"horizontal_scroll_absent", horizontalScrollAbsent},
        {"top_pixel_offset_preserved", topPixelOffsetPreserved},
        {"control_order_matches_model", controlOrderMatchesModel},
        {"incremental_mutation_delta", incrementalMutationDelta},
        {"structural_update_applied", structuralUpdateApplied},
        {"list_height_stable_across_row_counts", listHeightStableAcrossRowCounts},
        {"main_lists_fit_without_horizontal_scroll",
         mainListsFitWithoutHorizontalScroll},
        {"list_headers_have_no_scroll_corner", listHeadersHaveNoScrollCorner},
        {"native_list_scrollbars_are_hidden", nativeListScrollbarsAreHidden},
        {"native_overflow_scrollbar_hidden", nativeOverflowScrollbarHidden},
        {"custom_overflow_scrollbar_visible", customOverflowScrollbarVisible},
        {"custom_wheel_scrolls_rows", customWheelScrollsRows},
        {"custom_scrollbar_drag_scrolls_rows", customScrollbarDragScrollsRows},
        {"native_scrollbars_stay_hidden_after_input",
         nativeScrollbarsStayHiddenAfterInput},
        {"centered_owned_window_geometry", centeredOwnedWindowGeometry},
        {"scheduling_policy_ui_is_removed", schedulingPolicyUiIsRemoved},
        {"upload_select_all_and_invert_work", uploadSelectAllAndInvertWork},
        {"config_content_uses_full_width", configContentUsesFullWidth},
        {"tray_icons_use_branded_status_badges",
         trayIconsUseBrandedStatusBadges},
        {"static_label_brushes_match_regions", staticLabelBrushesMatchRegions},
        {"static_labels_paint_opaque_surface", staticLabelsPaintOpaqueSurface},
        {"sidebar_brand_removed", sidebarBrandRemoved},
        {"visible_fields_avoid_legacy_frames", visibleFieldsAvoidLegacyFrames},
        {"visible_fields_have_rounded_regions", visibleFieldsHaveRoundedRegions},
        {"theme_selector_uses_owner_draw", themeSelectorUsesOwnerDraw},
        {"dialog_fields_use_modern_frames", dialogFieldsUseModernFrames},
        {"dialog_field_frame_was_painted", dialogFieldFrameWasPainted},
        {"main_lists_fill_available_height", mainListsFillAvailableHeight},
        {"save_button_removed", saveButtonRemoved},
        {"settings_auto_save_enabled", settingsAutoSaveEnabled},
        {"settings_nav_is_bottom_left_icon", settingsNavIsBottomLeftIcon},
        {"settings_navigation_toggles_back", settingsNavigationTogglesBack},
        {"context_menu_repainted_before_action",
         contextMenuRepaintedBeforeAction},
        {"main_window_title_is_product_only", mainWindowTitleIsProductOnly},
        {"settings_controls_share_width", settingsControlsShareWidth},
        {"jobs_support_multi_select", jobsSupportMultiSelect},
        {"contributions_support_multi_select", contributionsSupportMultiSelect},
        {"software_profile_drawer_lists_all_profiles",
         softwareProfileDrawerListsAllProfiles},
        {"software_profile_selector_has_chevron",
         softwareProfileSelectorHasChevron},
        {"software_profile_drawer_follows_owner",
         softwareProfileDrawerFollowsOwner},
        {"software_profile_editor_has_delete",
         softwareProfileEditorHasDelete},
        {"software_profile_deletion_is_persistable",
         softwareProfileDeletionIsPersistable},
        {"unchanged_detail_refresh_is_visual_noop",
         unchangedDetailRefreshIsVisualNoop},
        {"detail_preview_uses_right_column", detailPreviewUsesRightColumn},
        {"detail_list_has_no_unused_blank_column",
         detailListHasNoUnusedBlankColumn},
        {"detail_summary_sits_below_preview", detailSummarySitsBelowPreview},
        {"detail_explorer_actions_are_icon_buttons",
         detailExplorerActionsAreIconButtons},
        {"default_status_hides_idle_counter", defaultStatusHidesIdleCounter},
        {"dashboard_status_ignores_peer_polling_details",
         dashboardStatusIgnoresPeerPollingDetails},
        {"dashboard_status_shows_handshake_upgrade",
         dashboardStatusShowsHandshakeUpgrade},
        {"settings_footer_copy_removed", !settingsFooterCopyFound},
        {"global_status_bar_is_single_surface", globalStatusBarIsSingleSurface},
        {"status_bar_aligns_with_content", statusBarAlignsWithContent},
        {"add_job_clears_status_bar", addJobClearsStatusBar},
        {"page_help_moved_to_status_bar", pageHelpMovedToStatusBar},
        {"blend_drag_overlay_covers_task_list", blendDragOverlayCoversTaskList},
        {"drag_overlay_accepts_blend_only", dragOverlayAcceptsBlendOnly},
        {"completed_job_hides_stop_action", completedJobHidesStopAction},
        {"running_job_shows_stop_action", runningJobShowsStopAction},
        {"failed_job_shows_retry_all_action", failedJobShowsRetryAllAction},
        {"failed_frame_shows_retry_current_action", failedFrameShowsRetryCurrentAction},
        {"stop_action_is_bottom_right_icon", stopActionIsBottomRightIcon},
        {"delete_dialog_is_internal", deleteDialogIsInternal},
        {"delete_dialog_defaults_to_cancel", deleteDialogDefaultsToCancel},
        {"delete_dialog_covers_resized_window", deleteDialogCoversResizedWindow},
        {"delete_dialog_tab_moves_focus", deleteDialogTabMovesFocus},
        {"delete_dialog_escape_closes", deleteDialogEscapeCloses},
        {"delete_dialog_enter_honors_cancel_focus", deleteDialogEnterHonorsCancelFocus},
        {"warning_override_is_internal", warningOverrideIsInternal},
        {"warning_override_defaults_to_cancel", warningOverrideDefaultsToCancel},
        {"warning_override_is_explicit", warningOverrideIsExplicit},
        {"warning_override_escape_closes", warningOverrideEscapeCloses},
        {"close_dialog_is_internal", closeDialogIsInternal},
        {"close_dialog_has_explicit_choices", closeDialogHasExplicitChoices},
        {"close_dialog_defaults_to_tray", closeDialogDefaultsToTray},
        {"close_dialog_remember_is_checkbox", closeDialogRememberIsCheckbox},
        {"modal_dialog_blocks_list_refresh", modalDialogBlocksListRefresh},
        {"modal_background_lists_clip_siblings", modalBackgroundListsClipSiblings},
        {"workflow_progress_blocks_main_input", workflowProgressBlocksMainInput},
        {"workflow_progress_allows_window_drag", workflowProgressAllowsWindowDrag},
        {"workflow_progress_shows_cancel_after_timeout",
         workflowProgressShowsCancelAfterTimeout},
        {"workflow_progress_cancel_sets_stop_signal",
         workflowProgressCancelSetsStopSignal},
        {"workflow_overlay_blocks_settings_action", workflowOverlayBlocksSettingsAction},
        {"workflow_background_controls_clip_siblings",
         workflowBackgroundControlsClipSiblings},
        {"workflow_refresh_is_deferred", workflowRefreshIsDeferred},
        {"blender_version_selector_uses_internal_style",
         blenderVersionSelectorUsesInternalStyle},
        {"themed_list_blank_areas_match_surface", themedListBlankAreasMatchSurface},
        {"light_detail_list_paint", lightDetailListPaint},
        {"light_upload_list_paint", lightUploadListPaint},
        {"dark_detail_list_paint", darkDetailListPaint},
        {"dark_upload_list_paint", darkUploadListPaint},
        {"resize_affected_children", resizeAffectedChildren},
        {"resize_repainted_children", repaintedAfterResize},
        {"resize_duplicate_layout_count", duplicateLayoutCount},
        {"chrome_title_hit", screenHitTest(titlePoint)},
        {"chrome_maximize_hit", screenHitTest(maxPoint)},
        {"chrome_left_resize_hit", screenHitTest(resizePoint)},
        {"chrome_system_menu_hit", screenHitTest(iconPoint)},
        {"chrome_height", ChromeHeight(gWindow)},
        {"compact_chrome_height", kChromeHeightDip == 36},
        {"chrome_title_scales_with_dpi",
         gChromeTitleFormat &&
             std::abs(gChromeTitleFormat->GetFontSize() -
                      static_cast<float>(DpiValue(gWindow, 16))) < 0.1f},
        {"chrome_cancel_clears_pressed", chromeCancelClearsPressed},
        {"frame_log_refresh_interval_ms", IDLE_HEARTBEAT_INTERVAL_MS},
        {"chrome_native_capabilities_preserved",
         (chromeStyles & (WS_THICKFRAME | WS_SYSMENU | WS_MINIMIZEBOX | WS_MAXIMIZEBOX)) ==
             (WS_THICKFRAME | WS_SYSMENU | WS_MINIMIZEBOX | WS_MAXIMIZEBOX)},
    };
    std::wstring ignored;
    WriteFileAtomic(gUiMetricsReport, metrics.dump(2), ignored);
}

#endif  // RM_DEV_TOOLS

std::optional<std::pair<RenderJob, FrameTask>> SelectedDetailFrame() {
    if (!gDetailWindow) return std::nullopt;
    auto job = FindJob(gDetailJobId);
    if (!job) return std::nullopt;
    HWND list = GetDlgItem(gDetailWindow, ID_DETAIL_FRAMES);
    int selected = ListView_GetNextItem(list, -1, LVNI_SELECTED);
    if (selected < 0 || selected >= static_cast<int>(gFrameRows.Rows().size()))
        return std::nullopt;
    const auto& key = gFrameRows.Rows()[static_cast<size_t>(selected)].key;
    auto frame = std::find_if(job->frames.begin(), job->frames.end(),
        [&](const FrameTask& value) {
            return Utf8ToWide(value.taskId.empty()
                ? job->id + ":" + std::to_string(value.frame) : value.taskId) == key;
        });
    if (frame == job->frames.end()) return std::nullopt;
    return std::pair{*job, *frame};
}

bool RetryFrameFromUi(const std::string& jobId, const std::string& taskId, int frameNumber) {
    std::wstring error;
    if (!gApp->RetryFrame(jobId, taskId, error)) {
        ShowInternalNotice(L"无法重新渲染当前帧", error);
        return false;
    }
    SetStatusText(L"帧 " + std::to_wstring(frameNumber) + L" 已重新进入渲染队列。");
    return true;
}

std::wstring LineNumberedLog(const std::string& utf8) {
    std::wstring text = Utf8ToWide(utf8);
    if (text.empty() && !utf8.empty()) return L"日志不是有效的 UTF-8 文本。";
    std::wstring output;
    size_t line = 1;
    size_t start = 0;
    while (start <= text.size()) {
        size_t end = text.find(L'\n', start);
        std::wstring_view value(text.data() + start,
            (end == std::wstring::npos ? text.size() : end) - start);
        if (!value.empty() && value.back() == L'\r') value.remove_suffix(1);
        wchar_t number[16]{};
        swprintf_s(number, L"%5zu  ", line++);
        output += number;
        output += value;
        if (end == std::wstring::npos) break;
        output += L"\r\n";
        start = end + 1;
    }
    return output;
}

void LayoutFrameLogWindow(HWND window) {
    RECT client{};
    GetClientRect(window, &client);
    const int margin = 20;
    const int searchWidth = 230;
    const int buttonWidth = 112;
    const int retryWidth = 162;
    const int chromeTop = ChromeHeight(window);
    int actionsY = chromeTop + 52;
    const int copyX = client.right - margin - buttonWidth * 2 - 8;
    const int openX = client.right - margin - buttonWidth;
    const int logWidth = std::max(100, static_cast<int>(client.right) - margin * 2);
    const int logHeight = std::max(100, static_cast<int>(client.bottom) - chromeTop - 118);
    ApplyChildLayout(window, {
        {GetDlgItem(window, ID_LOG_SUMMARY),
         {margin, chromeTop + 16, client.right - margin, chromeTop + 44}},
        {GetDlgItem(window, ID_LOG_SEARCH),
         {margin, actionsY, margin + searchWidth, actionsY + 34}},
        {GetDlgItem(window, ID_LOG_FIND),
         {margin + searchWidth + 8, actionsY,
          margin + searchWidth + 8 + buttonWidth, actionsY + 34}},
        {GetDlgItem(window, ID_LOG_RETRY),
         {margin + searchWidth + buttonWidth + 16, actionsY,
          margin + searchWidth + buttonWidth + 16 + retryWidth, actionsY + 34}},
        {GetDlgItem(window, ID_LOG_COPY),
         {copyX, actionsY, copyX + buttonWidth, actionsY + 34}},
        {GetDlgItem(window, ID_LOG_OPEN),
         {openX, actionsY, openX + buttonWidth, actionsY + 34}},
        {GetDlgItem(window, ID_LOG_VIEW),
         {margin, chromeTop + 98, margin + logWidth, chromeTop + 98 + logHeight}},
    });
}

void RefreshFrameLogWindow() {
    if (!gFrameLogWindow) return;
    auto job = FindJob(gFrameLogJobId);
    if (!job) {
        SetWindowTextIfChanged(GetDlgItem(gFrameLogWindow, ID_LOG_SUMMARY),
                               L"任务已不存在");
        return;
    }
    auto frame = std::find_if(job->frames.begin(), job->frames.end(),
        [&](const FrameTask& value) { return value.taskId == gFrameLogTaskId; });
    if (frame == job->frames.end()) return;
    ShowWindow(GetDlgItem(gFrameLogWindow, ID_LOG_RETRY),
               CanRetryFrame(*frame) ? SW_SHOW : SW_HIDE);
    std::wstring summary = Utf8ToWide(job->name) + L"    帧 " +
        std::to_wstring(frame->frame) + L"    " + TaskStateLabel(frame->state) +
        L"    渲染电脑 " + (frame->assignedNodeName.empty()
            ? L"-" : Utf8ToWide(frame->assignedNodeName)) +
        L"    渲染时长 " + FormatDuration(frame->startedAt, frame->endedAt) +
        L"    开始 " + FormatTime(frame->startedAt) +
        L"    结束 " + FormatTime(frame->endedAt);
    SetWindowTextIfChanged(GetDlgItem(gFrameLogWindow, ID_LOG_SUMMARY), summary);
    std::wstring title = L"RenderMesh · 帧 " + std::to_wstring(frame->frame) +
        L" · Blender 日志";
    SetWindowTextIfChanged(gFrameLogWindow, title);
    std::wstring next;
    if (!frame->log.empty() && fs::is_regular_file(frame->log)) {
        next = LineNumberedLog(ReadFileText(frame->log));
    } else if (frame->state == TaskState::Pending || frame->state == TaskState::Dispatching) {
        next = L"该帧尚未开始渲染，因此还没有 Blender 日志。";
    } else if (frame->state == TaskState::Assigned || frame->state == TaskState::Rendering) {
        next = L"正在等待渲染电脑回传该帧的 Blender 日志……";
    } else {
        next = L"该帧没有收到 Blender 日志。请确认提交端和渲染端都已更新到最新版本。";
        if (!frame->error.empty()) next += L"\r\n\r\n错误：" + Utf8ToWide(frame->error);
    }
    if (next == gFrameLogDisplayedText) return;
    HWND view = GetDlgItem(gFrameLogWindow, ID_LOG_VIEW);
    int firstVisible = static_cast<int>(SendMessageW(view, EM_GETFIRSTVISIBLELINE, 0, 0));
    int lineCount = static_cast<int>(SendMessageW(view, EM_GETLINECOUNT, 0, 0));
    bool followTail = firstVisible >= std::max(0, lineCount - 20);
    SendMessageW(view, WM_SETREDRAW, FALSE, 0);
    SetWindowTextW(view, next.c_str());
    if (followTail) {
        SendMessageW(view, EM_SETSEL, next.size(), next.size());
        SendMessageW(view, EM_SCROLLCARET, 0, 0);
    } else {
        int current = static_cast<int>(SendMessageW(view, EM_GETFIRSTVISIBLELINE, 0, 0));
        SendMessageW(view, EM_LINESCROLL, 0, firstVisible - current);
        SendMessageW(view, EM_SETSEL, 0, 0);
    }
    SendMessageW(view, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(view, nullptr, TRUE);
    gFrameLogDisplayedText = std::move(next);
}

void FindNextInFrameLog() {
    if (!gFrameLogWindow) return;
    std::wstring needle = Text(GetDlgItem(gFrameLogWindow, ID_LOG_SEARCH));
    if (needle.empty()) return;
    std::wstring haystack = gFrameLogDisplayedText;
    std::transform(needle.begin(), needle.end(), needle.begin(), towlower);
    std::transform(haystack.begin(), haystack.end(), haystack.begin(), towlower);
    HWND view = GetDlgItem(gFrameLogWindow, ID_LOG_VIEW);
    DWORD start = 0, end = 0;
    SendMessageW(view, EM_GETSEL, reinterpret_cast<WPARAM>(&start),
                 reinterpret_cast<LPARAM>(&end));
    size_t found = haystack.find(needle, end);
    if (found == std::wstring::npos) found = haystack.find(needle);
    if (found == std::wstring::npos) {
        MessageBeep(MB_ICONINFORMATION);
        return;
    }
    SendMessageW(view, EM_SETSEL, found, found + needle.size());
    SendMessageW(view, EM_SCROLLCARET, 0, 0);
    SetFocus(view);
}

LRESULT CALLBACK FrameLogWindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    LRESULT chromeResult = 0;
    if (HandleWindowChrome(window, message, wParam, lParam, chromeResult)) return chromeResult;
    switch (message) {
        case WM_CREATE: {
            gFrameLogWindow = window;
            ApplyWindowChromeTheme(window);
            HWND summary = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE,
                20, 16, 1000, 28, window, reinterpret_cast<HMENU>(ID_LOG_SUMMARY),
                GetModuleHandleW(nullptr), nullptr);
            SendMessageW(summary, WM_SETFONT, reinterpret_cast<WPARAM>(gFont), TRUE);
            HWND search = CreateWindowExW(0, L"EDIT", L"",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                20, 52, 230, 34, window, reinterpret_cast<HMENU>(ID_LOG_SEARCH),
                GetModuleHandleW(nullptr), nullptr);
            SendMessageW(search, WM_SETFONT, reinterpret_cast<WPARAM>(gFont), TRUE);
            SetPropW(search, kFieldSelfFrameProperty, reinterpret_cast<HANDLE>(1));
            ConfigureModernField(search, L"EDIT");
            SendMessageW(search, EM_SETCUEBANNER, TRUE,
                         reinterpret_cast<LPARAM>(L"搜索日志"));
            auto button = [&](const wchar_t* label, int id) {
                HWND control = CreateWindowExW(0, L"BUTTON", label,
                    WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                    0, 0, 112, 34, window,
                    reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                    GetModuleHandleW(nullptr), nullptr);
                SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(gFont), TRUE);
                ConfigureOwnerButton(control);
            };
            button(L"查找下一个", ID_LOG_FIND);
            button(L"重新渲染当前帧", ID_LOG_RETRY);
            button(L"复制全部", ID_LOG_COPY);
            button(L"显示日志文件", ID_LOG_OPEN);
            HWND view = CreateWindowExW(0, L"EDIT", L"",
                WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_HSCROLL | ES_MULTILINE |
                    ES_READONLY | ES_AUTOVSCROLL | ES_AUTOHSCROLL | ES_NOHIDESEL,
                20, 98, 1000, 540, window, reinterpret_cast<HMENU>(ID_LOG_VIEW),
                GetModuleHandleW(nullptr), nullptr);
            SendMessageW(view, WM_SETFONT,
                         reinterpret_cast<WPARAM>(gLogFont ? gLogFont : gFont), TRUE);
            SetPropW(view, kFieldSelfFrameProperty, reinterpret_cast<HANDLE>(1));
            ConfigureModernField(view, L"EDIT");
            SendMessageW(view, EM_SETLIMITTEXT, 8 * 1024 * 1024, 0);
            LayoutFrameLogWindow(window);
            SetTimer(window, TIMER_FRAME_LOG, IDLE_HEARTBEAT_INTERVAL_MS, nullptr);
            RefreshFrameLogWindow();
            return 0;
        }
        case WM_SIZE:
            if (wParam != SIZE_MINIMIZED) LayoutFrameLogWindow(window);
            return 0;
        case WM_GETMINMAXINFO: {
            auto* limits = reinterpret_cast<MINMAXINFO*>(lParam);
            ApplyMonitorWorkArea(window, limits);
            limits->ptMinTrackSize = {DpiValue(window, 820), DpiValue(window, 520)};
            return 0;
        }
        case WM_PAINT: {
            PAINTSTRUCT paint{};
            HDC dc = BeginPaint(window, &paint);
            FillRect(dc, &paint.rcPaint, gBackgroundBrush
                ? gBackgroundBrush : reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1));
            DrawChromeGdi(window, dc);
            EndPaint(window, &paint);
            return 0;
        }
        case WM_TIMER:
            if (wParam == TIMER_FRAME_LOG) RefreshFrameLogWindow();
            return 0;
        case WM_COMMAND:
            if (LOWORD(wParam) == ID_LOG_FIND) {
                FindNextInFrameLog();
            } else if (LOWORD(wParam) == ID_LOG_COPY) {
                HWND view = GetDlgItem(window, ID_LOG_VIEW);
                DWORD start = 0, end = 0;
                SendMessageW(view, EM_GETSEL, reinterpret_cast<WPARAM>(&start),
                             reinterpret_cast<LPARAM>(&end));
                SendMessageW(view, EM_SETSEL, 0, -1);
                SendMessageW(view, WM_COPY, 0, 0);
                SendMessageW(view, EM_SETSEL, start, end);
            } else if (LOWORD(wParam) == ID_LOG_OPEN) {
                auto job = FindJob(gFrameLogJobId);
                if (job) {
                    auto frame = std::find_if(job->frames.begin(), job->frames.end(),
                        [&](const FrameTask& value) {
                            return value.taskId == gFrameLogTaskId;
                        });
                    if (frame != job->frames.end() && !frame->log.empty())
                        ShowInExplorer(frame->log);
                }
            } else if (LOWORD(wParam) == ID_LOG_RETRY) {
                auto job = FindJob(gFrameLogJobId);
                if (job) {
                    auto frame = std::find_if(job->frames.begin(), job->frames.end(),
                        [&](const FrameTask& value) {
                            return value.taskId == gFrameLogTaskId;
                        });
                    if (frame != job->frames.end() && RetryFrameFromUi(
                            job->id, frame->taskId, frame->frame)) {
                        RefreshFrameLogWindow();
                        RefreshJobDetail();
                    }
                }
            }
            return 0;
        case WM_DRAWITEM:
            DrawOwnerButton(*reinterpret_cast<DRAWITEMSTRUCT*>(lParam));
            return TRUE;
        case WM_CTLCOLOREDIT: {
            HDC dc = reinterpret_cast<HDC>(wParam);
            SetTextColor(dc, gDarkTheme ? RGB(232, 234, 244) : RGB(31, 31, 38));
            SetBkColor(dc, gDarkTheme ? RGB(19, 20, 27) : RGB(255, 255, 255));
            return reinterpret_cast<LRESULT>(gSurfaceBrush);
        }
        case WM_CTLCOLORSTATIC: {
            HDC dc = reinterpret_cast<HDC>(wParam);
            if (GetDlgCtrlID(reinterpret_cast<HWND>(lParam)) == ID_LOG_VIEW) {
                SetBkMode(dc, OPAQUE);
                SetTextColor(dc, gDarkTheme ? RGB(232, 234, 244) : RGB(31, 31, 38));
                SetBkColor(dc, gDarkTheme ? RGB(19, 20, 27) : RGB(255, 255, 255));
                return reinterpret_cast<LRESULT>(gSurfaceBrush);
            }
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
        case WM_CLOSE:
            DestroyWindow(window);
            return 0;
        case WM_DESTROY:
            KillTimer(window, TIMER_FRAME_LOG);
            gFrameLogWindow = nullptr;
            gFrameLogJobId.clear();
            gFrameLogTaskId.clear();
            gFrameLogDisplayedText.clear();
            return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

void OpenSelectedFrameLog() {
    auto selected = SelectedDetailFrame();
    if (!selected) return;
    gFrameLogJobId = selected->first.id;
    gFrameLogTaskId = selected->second.taskId;
    gFrameLogDisplayedText.clear();
    if (gFrameLogWindow) {
        ShowWindow(gFrameLogWindow, SW_RESTORE);
        SetForegroundWindow(gFrameLogWindow);
        RefreshFrameLogWindow();
        return;
    }
    gFrameLogWindow = CreateWindowExW(
        WS_EX_APPWINDOW, L"RenderMeshFrameLog", L"RenderMesh · Blender 日志",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX |
            WS_MAXIMIZEBOX | WS_SIZEBOX | WS_CLIPCHILDREN,
        CW_USEDEFAULT, CW_USEDEFAULT, 1120, 720, gDetailWindow, nullptr,
        GetModuleHandleW(nullptr), nullptr);
    CenterOwnedWindow(gFrameLogWindow, gDetailWindow ? gDetailWindow : gWindow);
    ShowWindow(gFrameLogWindow, SW_SHOW);
    UpdateWindow(gFrameLogWindow);
    ApplyTheme();
}

void ShowDetailFrameContextMenu(HWND window, POINT screenPoint) {
    HWND list = GetDlgItem(window, ID_DETAIL_FRAMES);
    POINT clientPoint = screenPoint;
    ScreenToClient(list, &clientPoint);
    LVHITTESTINFO hit{};
    hit.pt = clientPoint;
    const int row = ListView_HitTest(list, &hit);
    if (row >= 0) {
        ListView_SetItemState(list, -1, 0, LVIS_SELECTED | LVIS_FOCUSED);
        ListView_SetItemState(list, row, LVIS_SELECTED | LVIS_FOCUSED,
                              LVIS_SELECTED | LVIS_FOCUSED);
    }
    auto selected = SelectedDetailFrame();
    if (!selected || !CanRetryFrame(selected->second)) return;
    static const MenuItemData retryFrame{L"重新渲染当前帧"};
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_OWNERDRAW, ID_DETAIL_RETRY_FRAME,
                reinterpret_cast<LPCWSTR>(&retryFrame));
    MENUINFO info{};
    info.cbSize = sizeof(info);
    info.fMask = MIM_BACKGROUND | MIM_STYLE;
    info.hbrBack = gSurfaceBrush;
    info.dwStyle = MNS_CHECKORBMP;
    SetMenuInfo(menu, &info);
    const UINT command = TrackAndCloseContextMenu(
        window, menu, screenPoint.x, screenPoint.y);
    if (command == ID_DETAIL_RETRY_FRAME) {
        const std::string jobId = selected->first.id;
        const std::string taskId = selected->second.taskId;
        const int frame = selected->second.frame;
        DeferMenuAction([jobId, taskId, frame] {
            if (RetryFrameFromUi(jobId, taskId, frame)) RefreshJobDetail();
        });
    }
}

LRESULT CALLBACK DetailWindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    LRESULT chromeResult = 0;
    if (HandleWindowChrome(window, message, wParam, lParam, chromeResult)) return chromeResult;
    switch (message) {
        case WM_CREATE: {
            gDetailWindow = window;
            ApplyWindowChromeTheme(window);
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
            AddColumn(frames, 1, 96, L"状态");
            AddColumn(frames, 2, 130, L"渲染电脑");
            AddColumn(frames, 3, 0, L"任务包");
            AddColumn(frames, 4, 92, L"渲染时长");
            AddColumn(frames, 5, 135, L"开始时间");
            AddColumn(frames, 6, 135, L"结束时间");
            AddColumn(frames, 7, 0, L"输出 / 错误");
            HWND preview = CreateWindowExW(0, L"STATIC", L"",
                WS_CHILD | WS_VISIBLE | SS_OWNERDRAW,
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
                    WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, 845, y, 330, 36, window,
                    reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                    GetModuleHandleW(nullptr), nullptr);
                SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(gFont), TRUE);
                ConfigureOwnerButton(control);
            };
            auto iconButton = [&](const wchar_t* accessibleText, int id) {
                HWND control = CreateWindowExW(0, L"BUTTON", accessibleText,
                    WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                    0, 0, 48, 48, window,
                    reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                    GetModuleHandleW(nullptr), nullptr);
                SendMessageW(control, WM_SETFONT,
                             reinterpret_cast<WPARAM>(gIconFont), TRUE);
                SetPropW(control, kIconActionProperty, reinterpret_cast<HANDLE>(1));
                ConfigureOwnerButton(control);
                CreateTooltip(window, control, accessibleText);
            };
            iconButton(L"在资源管理器中显示选中帧", ID_DETAIL_OPEN_OUTPUT);
            iconButton(L"在资源管理器中显示源 .blend", ID_DETAIL_OPEN_SOURCE);
            iconButton(L"开始全速渲染", ID_DETAIL_FULL_RENDER);
            button(L"重新渲染失败帧", 411, ID_DETAIL_RETRY_FAILED);
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
            ApplyMonitorWorkArea(window, limits);
            limits->ptMinTrackSize = {DpiValue(window, 1000), DpiValue(window, 600)};
            return 0;
        }
        case WM_PAINT: {
            PAINTSTRUCT paint{};
            HDC dc = BeginPaint(window, &paint);
            FillRect(dc, &paint.rcPaint, gBackgroundBrush
                ? gBackgroundBrush : reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1));
            DrawChromeGdi(window, dc);
            EndPaint(window, &paint);
            return 0;
        }
        case WM_NOTIFY: {
            auto* header = reinterpret_cast<NMHDR*>(lParam);
            if (header->code == NM_CUSTOMDRAW)
                return HandleListCustomDraw(reinterpret_cast<NMLVCUSTOMDRAW*>(lParam));
            if (!gRefreshingJobDetail && header->idFrom == ID_DETAIL_FRAMES &&
                header->code == LVN_ITEMCHANGED)
                UpdateFramePreview();
            if (header->idFrom == ID_DETAIL_FRAMES && header->code == NM_DBLCLK)
                OpenSelectedFrameLog();
            if (header->idFrom == ID_DETAIL_FRAMES && header->code == NM_RCLICK) {
                POINT point{};
                GetCursorPos(&point);
                ShowDetailFrameContextMenu(window, point);
            }
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
                ? LoadPreviewBitmap(result->preview, 1280, 800) : nullptr;
            ReplacePreviewBitmap(bitmap);
            std::wstring status = bitmap ? L"Blender 预览"
                : result->error.empty() ? L"无法生成该图片格式的预览" : result->error;
            SetWindowTextW(GetDlgItem(window, ID_DETAIL_PREVIEW_STATUS), status.c_str());
            return 0;
        }
        case WM_COMMAND:
            if (LOWORD(wParam) == ID_DETAIL_OPEN_SOURCE) {
                if (auto job = FindJob(gDetailJobId))
                    ShowInExplorer(job->sourceProject.empty() ? job->preparedProject : job->sourceProject);
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
                    ShowInternalNotice(L"无法停止任务", error);
                RefreshJobDetail();
            } else if (LOWORD(wParam) == ID_DETAIL_FULL_RENDER) {
                if (auto job = FindJob(gDetailJobId))
                    ExecuteJobContextCommand(ID_JOB_START_FULL, *job);
                RefreshJobDetail();
            } else if (LOWORD(wParam) == ID_DETAIL_RETRY_FAILED) {
                std::wstring error;
                if (!gApp->RetryFailedFrames(gDetailJobId, error))
                    ShowInternalNotice(L"无法重新渲染失败帧", error);
                else
                    SetStatusText(L"失败帧已重新进入渲染队列。");
                RefreshJobDetail();
            }
            return 0;
        case WM_DRAWITEM: {
            const auto& item = *reinterpret_cast<DRAWITEMSTRUCT*>(lParam);
            if (item.CtlType == ODT_MENU) DrawContextMenuItem(item);
            else if (item.CtlID == ID_DETAIL_PREVIEW) DrawPreviewControl(item);
            else DrawOwnerButton(item);
            return TRUE;
        }
        case WM_MEASUREITEM:
            if (reinterpret_cast<MEASUREITEMSTRUCT*>(lParam)->CtlType == ODT_MENU) {
                MeasureContextMenuItem(*reinterpret_cast<MEASUREITEMSTRUCT*>(lParam));
                return TRUE;
            }
            break;
        case WM_CLOSE:
            DestroyWindow(window);
            return 0;
        case WM_DESTROY:
            if (gFrameLogWindow) DestroyWindow(gFrameLogWindow);
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
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX |
            WS_MAXIMIZEBOX | WS_SIZEBOX | WS_CLIPCHILDREN,
        CW_USEDEFAULT, CW_USEDEFAULT, 1220, 650, gWindow, nullptr,
        GetModuleHandleW(nullptr), nullptr);
    CenterOwnedWindow(gDetailWindow, gWindow);
    ApplyTheme();
    ShowWindow(gDetailWindow, SW_SHOW);
    UpdateWindow(gDetailWindow);
}

LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    if (gRestoreInstanceMessage && message == gRestoreInstanceMessage) {
        RestoreMainWindowFromTray();
        return 0;
    }
    if (gTaskbarCreatedMessage && message == gTaskbarCreatedMessage) {
        gTrayIconAdded = false;
        if (!UpdateTrayIcon() && !IsWindowVisible(window)) {
            ShowWindow(window, SW_RESTORE);
            SetForegroundWindow(window);
            ShowInternalNotice(
                L"系统托盘已重新启动",
                L"RenderMesh 无法重新注册通知区域图标，因此已恢复主窗口，避免程序隐藏后无法打开。");
        }
        return 0;
    }
    LRESULT chromeResult = 0;
    if (HandleWindowChrome(window, message, wParam, lParam, chromeResult)) return chromeResult;
    switch (message) {
        case WM_CREATE:
            gWindow = window;
            if (!SetPropW(window, kSingleInstanceKeyProperty,
                          reinterpret_cast<HANDLE>(
                              static_cast<ULONG_PTR>(gSingleInstanceKey)))) {
                gWindow = nullptr;
                return -1;
            }
            ApplyWindowChromeTheme(window);
            BuildUi();
            CaptureBaseLayout();
            LayoutUi();
            gMainDropTarget = new MainDropTarget();
            if (FAILED(RegisterDragDrop(window, gMainDropTarget))) {
                gMainDropTarget->Release();
                gMainDropTarget = nullptr;
            }
            LoadConfigIntoUi();
            ApplyTheme();
            SetTimer(window, TIMER_HEARTBEAT, IDLE_HEARTBEAT_INTERVAL_MS, nullptr);
            if (!gDisableUpdateChecks)
                SetTimer(window, TIMER_UPDATE_CHECK, 4000, nullptr);
            RefreshLists();
            if (gPreviewDropOverlay) SetDropOverlayVisible(true);
            if (gOpenInitialDetail && ListView_GetItemCount(Item(ID_JOBS)) > 0) {
                ListView_SetItemState(Item(ID_JOBS), 0, LVIS_SELECTED | LVIS_FOCUSED,
                                      LVIS_SELECTED | LVIS_FOCUSED);
                OpenSelectedJobDetail();
            }
            return 0;
        case WM_SIZE:
            if (wParam == SIZE_MINIMIZED) return 0;
            LayoutUi();
            return 0;
        case WM_GETMINMAXINFO: {
            auto* limits = reinterpret_cast<MINMAXINFO*>(lParam);
            ApplyMonitorWorkArea(window, limits);
            limits->ptMinTrackSize = {DpiValue(window, 1120), DpiValue(window, 720)};
            return 0;
        }
        case WM_PAINT:
            PaintChrome(window);
            return 0;
        case WM_COMMAND:
            if (HIWORD(wParam) == EN_CHANGE) {
                switch (LOWORD(wParam)) {
                    case ID_NODE_NAME:
                    case ID_IDLE_SECONDS:
                        ScheduleSettingsSave();
                        break;
                    case ID_CLUSTER_TOKEN: {
                        const std::string matchingId = WideToUtf8(
                            Text(Item(ID_CLUSTER_TOKEN)));
                        if (!gLoadingSettings && IsInviteCode(matchingId)) {
                            KillTimer(gWindow, TIMER_SETTINGS_AUTOSAVE);
                            gSettingsDirty = true;
                            SaveSettings();
                        } else {
                            ScheduleSettingsSave();
                        }
                        break;
                    }
                }
            }
            switch (LOWORD(wParam)) {
                case ID_SHARE: {
                    if (gSettingsDirty && !SaveSettings()) break;
                    Config config = gApp->GetConfig();
                    config.shareEnabled = !config.shareEnabled;
                    std::wstring error;
                    if (!gApp->UpdateConfig(config, error)) {
                        ShowInternalNotice(L"无法更改网络接入状态", error);
                        break;
                    }
                    LoadConfigIntoUi();
                    InvalidateRect(Item(ID_SHARE), nullptr, TRUE);
                    RefreshLists();
                    break;
                }
                case ID_AUTO_START: {
                    if (gSettingsDirty && !SaveSettings()) break;
                    const bool enabled = SendMessageW(
                        Item(ID_AUTO_START), BM_GETCHECK, 0, 0) != BST_CHECKED;
                    SendMessageW(Item(ID_AUTO_START), BM_SETCHECK,
                                 enabled ? BST_CHECKED : BST_UNCHECKED, 0);
                    if (!enabled)
                        SendMessageW(Item(ID_SILENT_AUTO_START), BM_SETCHECK,
                                     BST_UNCHECKED, 0);
                    EnableWindow(Item(ID_SILENT_AUTO_START), enabled ? TRUE : FALSE);
                    InvalidateRect(Item(ID_AUTO_START), nullptr, FALSE);
                    InvalidateRect(Item(ID_SILENT_AUTO_START), nullptr, FALSE);
                    if (!SaveSettings()) LoadConfigIntoUi();
                    break;
                }
                case ID_SILENT_AUTO_START: {
                    if (!IsWindowEnabled(Item(ID_SILENT_AUTO_START))) break;
                    if (gSettingsDirty && !SaveSettings()) break;
                    const bool enabled = SendMessageW(
                        Item(ID_SILENT_AUTO_START), BM_GETCHECK, 0, 0) != BST_CHECKED;
                    SendMessageW(Item(ID_SILENT_AUTO_START), BM_SETCHECK,
                                 enabled ? BST_CHECKED : BST_UNCHECKED, 0);
                    InvalidateRect(Item(ID_SILENT_AUTO_START), nullptr, FALSE);
                    if (!SaveSettings()) LoadConfigIntoUi();
                    break;
                }
                case ID_CHECK_UPDATE:
                    StartLanUpdateCheck(true);
                    break;
                case ID_INSTALL_BLENDER:
                    if (gSettingsDirty && !SaveSettings()) break;
                    StartBlenderCatalogFetch(HasNvidiaGpu(QueryHardwareInfo()));
                    break;
                case ID_COPY_TOKEN:
                    CopyText(Text(Item(ID_CLUSTER_TOKEN)));
                    SetStatusText(L"匹配 ID 已复制");
                    break;
                case ID_NAV_DASHBOARD: NavigateSidebarPage(0); RefreshLists(); break;
                case ID_NAV_TASKS: NavigateSidebarPage(1); RefreshLists(); break;
                case ID_NAV_CONTRIBUTIONS: NavigateSidebarPage(2); RefreshLists(); break;
                case ID_NAV_SETTINGS: NavigateSidebarPage(3); RefreshLists(); break;
                case ID_BROWSE_BLENDER: {
                    if (gSettingsDirty && !SaveSettings()) break;
                    auto path = ChooseFile(
                        L"Blender 程序\0blender.exe;blender-launcher.exe\0"
                        L"所有程序\0*.exe\0\0",
                        L"选择 Blender 程序");
                    if (path.empty()) break;
                    Config config = gApp->GetConfig();
                    std::wstring error;
                    if (!BlenderRenderAdapter().ConfigureExecutable(config, path, error) ||
                        !gApp->UpdateConfig(std::move(config), error)) {
                        ShowInternalNotice(L"无法使用该 Blender", error);
                        break;
                    }
                    LoadConfigIntoUi();
                    RefreshLists();
                    SetStatusText(L"Blender 路径已更新");
                    break;
                }
                case ID_BROWSE_SOURCE: {
                    auto path = ChooseFile(ProjectFileFilter().c_str(), L"选择工程文件");
                    if (!path.empty()) BeginNewJobFlow(path);
                    break;
                }
                case ID_ADD_JOB: {
                    auto path = ChooseFile(ProjectFileFilter().c_str(), L"选择工程文件");
                    if (!path.empty()) BeginNewJobFlow(path);
                    break;
                }
                case ID_THEME:
                    ShowThemeSelectionMenu();
                    break;
                case ID_CLOSE_BEHAVIOR:
                    ShowCloseBehaviorSelectionMenu();
                    break;
                case ID_CHANGE_PROJECT_STORAGE:
                    StartProjectStorageMigration();
                    break;
                case ID_CONNECT_MANUAL:
                    ConnectManualPeer();
                    break;
                case ID_SUBMIT:
                    StartImport(Text(Item(ID_SOURCE_PATH)));
                    break;
                case ID_OPEN_DATA:
                    ShellExecuteW(window, L"open", gApp->ProjectStorageDir().c_str(),
                                  nullptr, nullptr, SW_SHOWNORMAL);
                    break;
                case ID_JOB_DETAILS:
                    OpenSelectedJobDetail();
                    break;
            }
            return 0;
        case WM_DRAWITEM:
            if (reinterpret_cast<DRAWITEMSTRUCT*>(lParam)->CtlType == ODT_MENU)
                DrawContextMenuItem(*reinterpret_cast<DRAWITEMSTRUCT*>(lParam));
            else
                DrawOwnerButton(*reinterpret_cast<DRAWITEMSTRUCT*>(lParam));
            return TRUE;
        case WM_MEASUREITEM:
            if (reinterpret_cast<MEASUREITEMSTRUCT*>(lParam)->CtlType == ODT_MENU) {
                MeasureContextMenuItem(*reinterpret_cast<MEASUREITEMSTRUCT*>(lParam));
                return TRUE;
            }
            break;
        case WM_CONTEXTMENU:
            if (reinterpret_cast<HWND>(wParam) == Item(ID_JOBS)) {
                POINT point{static_cast<int>(static_cast<short>(LOWORD(lParam))),
                            static_cast<int>(static_cast<short>(HIWORD(lParam)))};
                if (point.x == -1 && point.y == -1) {
                    const int selected = ListView_GetNextItem(Item(ID_JOBS), -1, LVNI_SELECTED);
                    RECT row{};
                    if (selected >= 0 && ListView_GetItemRect(
                            Item(ID_JOBS), selected, &row, LVIR_BOUNDS)) {
                        point = {row.left + 20, row.bottom};
                        ClientToScreen(Item(ID_JOBS), &point);
                    } else {
                        GetCursorPos(&point);
                    }
                }
                QueueContextMenu(ID_JOBS, point);
                return 0;
            }
            if (reinterpret_cast<HWND>(wParam) == Item(ID_CONTRIBUTION_LIST)) {
                POINT point{static_cast<int>(static_cast<short>(LOWORD(lParam))),
                            static_cast<int>(static_cast<short>(HIWORD(lParam)))};
                if (point.x == -1 && point.y == -1) GetCursorPos(&point);
                QueueContextMenu(ID_CONTRIBUTION_LIST, point);
                return 0;
            }
            break;
        case WM_NOTIFY: {
            auto* header = reinterpret_cast<NMHDR*>(lParam);
            if (header->code == NM_CUSTOMDRAW)
                return HandleListCustomDraw(reinterpret_cast<NMLVCUSTOMDRAW*>(lParam));
            if (header->idFrom == ID_JOBS && header->code == NM_DBLCLK) {
                if (auto job = SelectedJob()) {
                    if (job->stage == JobStage::Uploading ||
                        job->stage == JobStage::UploadFailed)
                        ShowUploadWindow(job->id);
                    else if (job->stage == JobStage::ReadyToConfigure)
                        ShowConfigWindow(job->id);
                    else
                        OpenSelectedJobDetail();
                }
            }
            if (header->idFrom == ID_JOBS && header->code == NM_RCLICK) {
                auto* click = reinterpret_cast<NMITEMACTIVATE*>(lParam);
                if (click->iItem >= 0) {
                    if (!(ListView_GetItemState(Item(ID_JOBS), click->iItem,
                                                LVIS_SELECTED) & LVIS_SELECTED))
                        ListView_SetItemState(Item(ID_JOBS), -1, 0,
                                              LVIS_SELECTED | LVIS_FOCUSED);
                    ListView_SetItemState(Item(ID_JOBS), click->iItem,
                                          LVIS_SELECTED | LVIS_FOCUSED,
                                          LVIS_SELECTED | LVIS_FOCUSED);
                    POINT point = click->ptAction;
                    ClientToScreen(Item(ID_JOBS), &point);
                    QueueContextMenu(ID_JOBS, point);
                }
                return 0;
            }
            if (header->idFrom == ID_CONTRIBUTION_LIST && header->code == NM_RCLICK) {
                auto* click = reinterpret_cast<NMITEMACTIVATE*>(lParam);
                if (click->iItem >= 0) {
                    if (!(ListView_GetItemState(Item(ID_CONTRIBUTION_LIST), click->iItem,
                                                LVIS_SELECTED) & LVIS_SELECTED))
                        ListView_SetItemState(Item(ID_CONTRIBUTION_LIST), -1, 0,
                                              LVIS_SELECTED | LVIS_FOCUSED);
                    ListView_SetItemState(Item(ID_CONTRIBUTION_LIST), click->iItem,
                                          LVIS_SELECTED | LVIS_FOCUSED,
                                          LVIS_SELECTED | LVIS_FOCUSED);
                    POINT point = click->ptAction;
                    ClientToScreen(Item(ID_CONTRIBUTION_LIST), &point);
                    QueueContextMenu(ID_CONTRIBUTION_LIST, point);
                }
                return 0;
            }
            return 0;
        }
        case WM_TIMER:
            if (wParam == TIMER_HEARTBEAT) {
                if (gRefreshQueued.exchange(false)) KillTimer(window, TIMER_BURST_REFRESH);
                ++gHeartbeatTimerRefreshCount;
                RefreshLists();
                MaybeShowLanUpdateOffer();
                MaybeShowBlenderInstallPrompt();
            } else if (wParam == TIMER_BURST_REFRESH) {
                KillTimer(window, TIMER_BURST_REFRESH);
                gRefreshQueued = false;
                ++gBurstTimerRefreshCount;
                RefreshLists();
#ifdef RM_DEV_TOOLS
            } else if (wParam == TIMER_UI_TEST_REPORT) {
                KillTimer(window, TIMER_UI_TEST_REPORT);
                WriteUiMetricsReport();
                PostMessageW(window, WM_CLOSE, 0, 0);
#endif
            } else if (wParam == TIMER_SETTINGS_AUTOSAVE) {
                KillTimer(window, TIMER_SETTINGS_AUTOSAVE);
                if (gSettingsDirty) SaveSettings();
            } else if (wParam == TIMER_MENU_ACTION) {
                KillTimer(window, TIMER_MENU_ACTION);
                auto actions = std::move(gDeferredMenuActions);
                gDeferredMenuActions.clear();
                for (auto& action : actions)
                    if (action) action();
            } else if (wParam == TIMER_CONTEXT_MENU_OPEN) {
                KillTimer(window, TIMER_CONTEXT_MENU_OPEN);
                const UINT control = std::exchange(gPendingContextMenuControl, 0);
                const POINT point = gPendingContextMenuPoint;
                if (control == ID_JOBS) ShowJobContextMenu(point);
                else if (control == ID_CONTRIBUTION_LIST)
                    ShowContributionContextMenu(point);
            } else if (wParam == TIMER_UPDATE_CHECK) {
                KillTimer(window, TIMER_UPDATE_CHECK);
                StartLanUpdateCheck(false);
            }
            return 0;
        case WM_RM_REFRESH:
            HandleQueuedRefresh();
            return 0;
        case WM_RM_BLEND_DROP: {
            std::unique_ptr<std::wstring> path(
                reinterpret_cast<std::wstring*>(lParam));
            if (path && !path->empty()) BeginNewJobFlow(*path);
            return 0;
        }
        case WM_RM_WORKFLOW_PROGRESS:
            ShowWorkflowProgress(static_cast<WorkflowProgressStage>(wParam));
            return 0;
        case WM_DROPFILES: {
            HDROP drop = reinterpret_cast<HDROP>(wParam);
            const UINT count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
            wchar_t path[32768]{};
            if (count > 0) DragQueryFileW(drop, 0, path, static_cast<UINT>(std::size(path)));
            DragFinish(drop);
            if (gCurrentPage != 1) {
                ShowPage(1);
                RefreshLists();
            }
            if (count > 1)
                SetStatusText(L"一次只能新建一个渲染任务，已使用第一个文件。");
            if (count > 0) BeginNewJobFlow(path);
            return 0;
        }
        case WM_RM_PREFLIGHT: {
            std::unique_ptr<PreflightUiResult> result(
                reinterpret_cast<PreflightUiResult*>(lParam));
            CloseWorkflowProgress();
            if (result && result->cancelled) {
                gNewJobFlow = false;
                SetStatusText(L"已取消本次工程上传。未创建渲染任务。");
                EnableWindow(Item(ID_SUBMIT), TRUE);
                RefreshLists();
                return 0;
            }
            SetStatusText(result ? result->message : L"预检未返回结果。");
            EnableWindow(Item(ID_SUBMIT), TRUE);
            if (result && result->frameEnd >= result->frameStart) {
                SetText(ID_FRAME_START, std::to_wstring(result->frameStart));
                SetText(ID_FRAME_END, std::to_wstring(result->frameEnd));
                SetText(ID_FRAME_STEP, std::to_wstring(result->frameStep));
            }
            RefreshLists();
            if (!result || !result->ok) {
                gNewJobFlow = false;
                if (result && result->canIgnoreAssetWarnings)
                    ShowPreflightWarningOverride(result->message);
                else
                    ShowInternalNotice(L"工程预检未通过",
                                       result ? result->message : L"预检失败。");
            } else {
                gNewJobFlow = false;
                auto job = FindJob(result->jobId);
                if (job && job->projectTransfers.empty())
                    ShowConfigWindow(result->jobId);
                else
                    ShowUploadWindow(result->jobId);
            }
            return 0;
        }
        case WM_RM_JOB_OPERATION: {
            std::unique_ptr<JobOperationResult> result;
            {
                std::scoped_lock lock(gJobOperationResultMutex);
                result.reset(gPendingJobOperationResult);
                gPendingJobOperationResult = nullptr;
            }
            if (gJobOperation.joinable()) gJobOperation.join();
            gJobOperationRunning = false;
            if (result) {
                if (result->kind == JobOperationKind::CancelUpload) {
                    gUploadCancelRunning = false;
                    if (result->ok && gUploadWindow &&
                        gUploadJobId == result->jobId) {
                        DestroyWindow(gUploadWindow);
                    } else if (!result->ok && gUploadWindow &&
                               gUploadJobId == result->jobId) {
                        HWND button = GetDlgItem(gUploadWindow, ID_UPLOAD_CANCEL);
                        SetWindowTextW(button, L"取消上传");
                        EnableWindow(button, TRUE);
                    }
                } else if (result->kind == JobOperationKind::StorageMigration) {
                    CloseWorkflowProgress();
                    LoadConfigIntoUi();
                    RefreshLists();
                    SetStatusText(result->message);
                    ShowInternalNotice(
                        result->ok ? L"工程存储迁移完成" : L"工程存储迁移失败",
                        result->message);
                    return 0;
                }
                SetStatusText(result->message);
                if (!result->ok)
                    ShowInternalNotice(
                        result->kind == JobOperationKind::CancelUpload
                            ? L"取消上传失败" : L"作业操作失败",
                        result->message);
            }
            return 0;
        }
        case WM_RM_UPDATE_CHECKED: {
            std::unique_ptr<UpdateCheckResult> result(
                reinterpret_cast<UpdateCheckResult*>(lParam));
            if (gUpdateCheckThread.joinable()) gUpdateCheckThread.join();
            const bool manual = std::exchange(gManualUpdateCheck, false);
            SetUpdateCheckButtonBusy(false);
            if (result && result->offer &&
                result->offer->version != gSkippedUpdateVersion) {
                gPendingUpdateOffer = std::move(result->offer);
                SetStatusText(L"发现可用的 RenderMesh 更新");
                MaybeShowLanUpdateOffer();
            } else if (result && !result->error.empty()) {
                LogError("LAN update check did not complete: " +
                         WideToUtf8(result->error));
                SetStatusText(L"更新检查失败");
                if (manual)
                    ShowInternalNotice(L"检查更新失败", result->error);
            } else if (manual) {
                SetStatusText(L"当前已是最新版本");
                ShowInternalNotice(
                    L"检查更新",
                    L"当前已是局域网内可用的最新版本。\n\n版本：" +
                        Utf8ToWide(kRenderMeshAppVersion));
            }
            return 0;
        }
        case WM_RM_UPDATE_DOWNLOADED: {
            std::unique_ptr<UpdateDownloadResult> result(
                reinterpret_cast<UpdateDownloadResult*>(lParam));
            if (gUpdateDownloadThread.joinable()) gUpdateDownloadThread.join();
            if (!result || !result->ok) {
                CloseWorkflowProgress();
                LogError("LAN update download failed: " + WideToUtf8(
                    result ? result->error : L"missing update download result"));
                if (!gUiShuttingDown.load())
                    ShowInternalNotice(
                        L"更新下载失败",
                        result ? result->error : L"没有收到更新下载结果。当前版本未被修改。");
                return 0;
            }
            if (gUpdateApplyThread.joinable()) gUpdateApplyThread.join();
            SetWorkflowProgressStage(WorkflowProgressStage::ApplyingUpdate);
            gUpdateApplying = true;
            gUpdateApplyRunning = true;
            UpdateDownloadResult downloaded = std::move(*result);
            gUpdateApplyThread = std::thread(
                [downloaded = std::move(downloaded)]() mutable {
                    auto applied = std::make_unique<UpdateApplyResult>();
                    applied->ok = LaunchUpdaterHelper(downloaded, applied->error);
                    gUpdateApplyRunning = false;
                    if (!gUiShuttingDown.load() && gWindow &&
                        PostMessageW(gWindow, WM_RM_UPDATE_APPLIED, 0,
                                     reinterpret_cast<LPARAM>(applied.get())))
                        applied.release();
                });
            return 0;
        }
        case WM_RM_UPDATE_APPLIED: {
            std::unique_ptr<UpdateApplyResult> result(
                reinterpret_cast<UpdateApplyResult*>(lParam));
            if (gUpdateApplyThread.joinable()) gUpdateApplyThread.join();
            CloseWorkflowProgress();
            if (!result || !result->ok) {
                gUpdateApplying = false;
                const std::wstring launchError = result
                    ? result->error : L"没有收到更新接管结果。当前版本未被修改。";
                LogError("LAN update installer could not start: " +
                         WideToUtf8(launchError));
                ShowInternalNotice(L"无法安装更新", launchError);
                return 0;
            }
            gForceExit = true;
            PostMessageW(window, WM_CLOSE, 0, 0);
            return 0;
        }
        case WM_RM_BLENDER_INSTALL_PROGRESS: {
            std::unique_ptr<BlenderInstallProgress> progress(
                reinterpret_cast<BlenderInstallProgress*>(lParam));
            if (progress && gWorkflowProgressOverlay) {
                gWorkflowProgressPercent = progress->percent;
                gWorkflowProgressDetail = progress->detail;
                if (HWND bar = GetDlgItem(
                        gWorkflowProgressOverlay, ID_WORKFLOW_PROGRESS))
                    SendMessageW(bar, PBM_SETPOS, gWorkflowProgressPercent, 0);
                const RECT detail =
                    WorkflowProgressDetailRect(gWorkflowProgressOverlay);
                InvalidateRect(gWorkflowProgressOverlay, &detail, FALSE);
            }
            return 0;
        }
        case WM_RM_BLENDER_CATALOG: {
            std::unique_ptr<BlenderCatalogResult> result(
                reinterpret_cast<BlenderCatalogResult*>(lParam));
            if (gBlenderInstallThread.joinable()) gBlenderInstallThread.join();
            CloseWorkflowProgress();
            if (!result || result->releases.empty()) {
                ShowInternalNotice(
                    L"无法读取 Blender 版本",
                    result ? result->error
                           : L"没有收到 Blender 官方版本目录结果。");
                return 0;
            }
            gBlenderCatalog = std::move(result->releases);
            ShowInternalDialog(
                InternalDialogMode::BlenderVersionPicker,
                L"选择 Blender 版本", L"",
                L"以下版本来自 Blender 官方镜像；LTS 版本已单独标记。"
                L"选择后可指定 MSI 下载目录。安装程序会由 Windows 打开。");
            return 0;
        }
        case WM_RM_BLENDER_INSTALLED: {
            std::unique_ptr<BlenderInstallResult> result(
                reinterpret_cast<BlenderInstallResult*>(lParam));
            if (gBlenderInstallThread.joinable()) gBlenderInstallThread.join();
            CloseWorkflowProgress();
            if (!result || !result->ok) {
                ShowInternalNotice(
                    L"Blender 安装失败",
                    result ? result->error
                           : L"没有收到 Blender 安装结果。已下载的断点会保留。" );
                return 0;
            }
            std::wstring profileError;
            if (!SaveInstalledBlenderProfile(*result, profileError)) {
                ShowInternalNotice(
                    L"Blender 已安装，但配置保存失败",
                    L"官方安装程序已完成，MSI 位于：" +
                        result->installer.wstring() + L"\n\n" + profileError);
                return 0;
            }
            LoadConfigIntoUi();
            RefreshLists();
            std::wstring body = L"Blender " +
                Utf8ToWide(result->release.version) +
                L" 的官方安装程序已完成。\n\nMSI：" +
                result->installer.wstring();
            if (!result->executable.empty())
                body += L"\nBlender：" + result->executable.wstring();
            if (result->optixConfigured)
                body += L"\n\n已导入 NVIDIA OptiX 默认渲染配置。";
            else
                body += L"\n\n请在 Blender 的“编辑 > 偏好设置 > 系统”中，"
                        L"按显卡选择 OptiX、CUDA、HIP 或 oneAPI。";
            if (!result->warning.empty()) body += L"\n\n" + result->warning;
            ShowInternalNotice(L"Blender 安装完成", std::move(body));
            return 0;
        }
        case WM_RM_TRAY: {
            const UINT event = LOWORD(lParam);
            if (event == WM_LBUTTONUP || event == NIN_SELECT || event == NIN_KEYSELECT)
                RestoreMainWindowFromTray();
            else if (event == WM_RBUTTONUP || event == WM_CONTEXTMENU)
                ShowTrayContextMenu();
            return 0;
        }
        case WM_CLOSE:
            switch (ResolveCloseRequest(window)) {
                case CloseRequestResult::Cancel:
                    return 0;
                case CloseRequestResult::Minimize:
                    if (!UpdateTrayIcon()) {
                        ShowInternalNotice(
                            L"无法最小化到系统托盘",
                            L"Windows 通知区域暂时不可用，RenderMesh 窗口将保持打开。"
                            L"请稍后重试或选择直接退出。");
                        return 0;
                    }
                    ShowWindow(window, SW_HIDE);
                    return 0;
                case CloseRequestResult::Exit:
                    break;
            }
            KillTimer(window, TIMER_SETTINGS_AUTOSAVE);
            KillTimer(window, TIMER_MENU_ACTION);
            KillTimer(window, TIMER_CONTEXT_MENU_OPEN);
            KillTimer(window, TIMER_UPDATE_CHECK);
            gDeferredMenuActions.clear();
            if (gSettingsDirty && !SaveSettings()) return 0;
            gJobOperationCancel = true;
            StopLanUpdateThreads();
            StopBlenderInstallThread();
            if (gPreflightRunning.load()) {
                gPreflightStop = true;
                EnableWindow(window, FALSE);
                SetStatusText(L"正在中止 Blender 预检并安全退出……");
                if (gPreflight.joinable()) gPreflight.join();
                EnableWindow(window, TRUE);
            }
            CloseWorkflowProgress();
            if (gDetailWindow) DestroyWindow(gDetailWindow);
            if (gUploadWindow) DestroyWindow(gUploadWindow);
            if (gConfigWindow) DestroyWindow(gConfigWindow);
            DestroyWindow(window);
            return 0;
        case WM_SETTINGCHANGE:
            if (gApp && gApp->GetConfig().theme == "system") ApplyTheme();
            return 0;
        case WM_DESTROY:
            RemoveTrayIcon();
            RemovePropW(window, kSingleInstanceKeyProperty);
            KillTimer(window, TIMER_HEARTBEAT);
            KillTimer(window, TIMER_BURST_REFRESH);
            KillTimer(window, TIMER_UI_TEST_REPORT);
            KillTimer(window, TIMER_SETTINGS_AUTOSAVE);
            KillTimer(window, TIMER_UPDATE_CHECK);
            RevokeDragDrop(window);
            if (gMainDropTarget) {
                gMainDropTarget->Release();
                gMainDropTarget = nullptr;
            }
            gUiShuttingDown = true;
            if (gJobOperation.joinable()) gJobOperation.join();
            {
                std::scoped_lock lock(gJobOperationResultMutex);
                delete gPendingJobOperationResult;
                gPendingJobOperationResult = nullptr;
            }
            gWindow = nullptr;
            PostQuitMessage(0);
            return 0;
        case WM_CTLCOLORSTATIC: {
            HDC dc = reinterpret_cast<HDC>(wParam);
            SetTextColor(dc, gDarkTheme ? RGB(239, 239, 247) : RGB(31, 31, 38));
            HWND control = reinterpret_cast<HWND>(lParam);
            const auto role = static_cast<StaticSurfaceRole>(
                reinterpret_cast<ULONG_PTR>(
                    GetPropW(control, kStaticSurfaceRoleProperty)));
            const bool sidebar = role == StaticSurfaceRole::Sidebar;
            const bool surface = role == StaticSurfaceRole::Surface;
            SetBkMode(dc, OPAQUE);
            SetBkColor(dc, surface
                ? (gDarkTheme ? RGB(31, 31, 40) : RGB(255, 255, 255))
                : sidebar
                ? (gDarkTheme ? RGB(23, 24, 32) : RGB(252, 252, 254))
                : (gDarkTheme ? RGB(15, 16, 22) : RGB(244, 245, 249)));
            return reinterpret_cast<LRESULT>(surface ? gSurfaceBrush
                : sidebar ? gSidebarBrush : gBackgroundBrush);
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

std::atomic<bool> gServeStopRequested = false;
bool gServeConsoleAttached = false;

BOOL WINAPI StopServeOnConsoleSignal(DWORD) {
    gServeStopRequested = true;
    return TRUE;
}

void PrintToParentConsole(const std::wstring& text) {
    // ERROR_ACCESS_DENIED: this process is already attached (serve mode).
    const bool attachedHere = AttachConsole(ATTACH_PARENT_PROCESS) != FALSE;
    if (!attachedHere && GetLastError() != ERROR_ACCESS_DENIED) return;
    HANDLE output = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD written = 0;
    const std::wstring line = text + L"\r\n";
    WriteConsoleW(output, line.c_str(), static_cast<DWORD>(line.size()), &written, nullptr);
    if (attachedHere && !gServeConsoleAttached) FreeConsole();
}


bool StartProcess(const fs::path& executable,
                  const std::vector<std::wstring>& arguments,
                  DWORD creationFlags, PROCESS_INFORMATION& process,
                  std::wstring& error) {
    std::wstring command = QuoteCommandLineArgument(executable.wstring());
    for (const auto& argument : arguments) {
        command.push_back(L' ');
        command += QuoteCommandLineArgument(argument);
    }
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE,
                        creationFlags, nullptr, executable.parent_path().c_str(),
                        &startup, &process)) {
        error = LastErrorMessage();
        return false;
    }
    return true;
}

bool FlushExistingFile(const fs::path& path) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE,
                              FILE_SHARE_READ | FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    const bool ok = FlushFileBuffers(file) != FALSE;
    CloseHandle(file);
    return ok;
}

bool IsTransientUpdateFileError(DWORD error) {
    return error == ERROR_SHARING_VIOLATION || error == ERROR_LOCK_VIOLATION ||
        error == ERROR_ACCESS_DENIED || error == ERROR_BUSY ||
        error == ERROR_USER_MAPPED_FILE;
}

bool DeleteUpdateBackupWithRetry(const fs::path& backup, DWORD& error,
                                 unsigned& retries) {
    const auto deadline = std::chrono::steady_clock::now() +
        std::chrono::seconds(15);
    retries = 0;
    for (;;) {
        if (DeleteFileW(backup.c_str())) return true;
        error = GetLastError();
        if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND)
            return true;
        if (!IsTransientUpdateFileError(error) ||
            std::chrono::steady_clock::now() >= deadline)
            return false;
        ++retries;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

bool ReplaceUpdateFileWithRetry(const fs::path& target, const fs::path& pending,
                                const fs::path& backup, DWORD& error,
                                unsigned& retries) {
    const auto deadline = std::chrono::steady_clock::now() +
        std::chrono::seconds(15);
    retries = 0;
    for (;;) {
        if (ReplaceFileW(target.c_str(), pending.c_str(), backup.c_str(),
                         REPLACEFILE_WRITE_THROUGH, nullptr, nullptr))
            return true;
        error = GetLastError();
        if (!IsTransientUpdateFileError(error) ||
            std::chrono::steady_clock::now() >= deadline)
            return false;
        ++retries;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

bool RestorePreviousUpdateWithRetry(const fs::path& target, const fs::path& backup,
                                    const fs::path& failed, DWORD& error,
                                    unsigned& retries) {
    const auto deadline = std::chrono::steady_clock::now() +
        std::chrono::seconds(15);
    retries = 0;
    for (;;) {
        if (!DeleteFileW(failed.c_str())) {
            error = GetLastError();
            if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND) {
                if (!IsTransientUpdateFileError(error) ||
                    std::chrono::steady_clock::now() >= deadline)
                    return false;
                ++retries;
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                continue;
            }
        }
        if (ReplaceFileW(target.c_str(), backup.c_str(), failed.c_str(),
                         REPLACEFILE_WRITE_THROUGH, nullptr, nullptr)) {
            if (!DeleteFileW(failed.c_str())) {
                const DWORD cleanupError = GetLastError();
                if (cleanupError != ERROR_FILE_NOT_FOUND &&
                    cleanupError != ERROR_PATH_NOT_FOUND)
                    MoveFileExW(failed.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
            }
            error = ERROR_SUCCESS;
            return true;
        }
        error = GetLastError();
        if (!IsTransientUpdateFileError(error) ||
            std::chrono::steady_clock::now() >= deadline)
            return false;
        ++retries;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

fs::path RecoverUpdateExecutableAfterReplaceFailure(
    const fs::path& target, const fs::path& backup) {
    std::error_code ignored;
    if (fs::is_regular_file(target, ignored)) return target;
    ignored.clear();
    if (!fs::is_regular_file(backup, ignored)) return {};
    if (MoveFileExW(backup.c_str(), target.c_str(),
                    MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        return target;
    return backup;
}

void RelaunchAfterUpdateFailure(const fs::path& target, const fs::path& backup,
                                int updateCode, DWORD windowsError) {
    const fs::path executable =
        RecoverUpdateExecutableAfterReplaceFailure(target, backup);
    if (executable.empty()) {
        LogError("LAN updater has no recoverable executable after failure; code=" +
                 std::to_string(updateCode) + "; win32=" +
                 std::to_string(windowsError));
        return;
    }
    DWORD binaryType = 0;
    if (!GetBinaryTypeW(executable.c_str(), &binaryType) ||
        (binaryType != SCS_64BIT_BINARY && binaryType != SCS_32BIT_BINARY))
        return;
    PROCESS_INFORMATION previous{};
    std::wstring launchError;
    if (StartProcess(executable, {}, 0, previous, launchError)) {
        CloseHandle(previous.hThread);
        CloseHandle(previous.hProcess);
        LogWarning("LAN updater relaunched the previous executable after failure; code=" +
                   std::to_string(updateCode) + "; win32=" +
                   std::to_string(windowsError));
    } else {
        LogError("LAN updater could not relaunch the previous executable; code=" +
                 std::to_string(updateCode) + "; win32=" +
                 std::to_string(windowsError) + "; launch=" +
                 WideToUtf8(launchError));
    }
}

bool LaunchUpdaterHelper(const UpdateDownloadResult& result, std::wstring& error) {
    if (!gApp || !result.ok || result.pendingExecutable.empty() ||
        !fs::is_regular_file(result.pendingExecutable)) {
        error = L"已验证的更新文件不存在，当前版本未被修改。";
        return false;
    }
    const fs::path target = ExecutablePath();
    const fs::path staged = target.parent_path() /
        (L"RenderMesh-" + Utf8ToWide(result.offer.version) + L".exe.pending");
    const fs::path backup(target.wstring() + L".previous");
    std::error_code ignored;
    fs::remove(staged, ignored);
    // The download was verified against the signed manifest; the installer
    // helper checks the staged file again before it replaces the executable.
    if (!CopyFileW(result.pendingExecutable.c_str(), staged.c_str(), FALSE) ||
        !FlushExistingFile(staged)) {
        const std::wstring copyError = LastErrorMessage();
        fs::remove(staged, ignored);
        error = L"无法把更新安全地暂存到程序目录：" + copyError;
        return false;
    }
    fs::remove(result.pendingExecutable, ignored);

    const fs::path helper = gApp->DataDir() / L"updates" /
        (L"RenderMesh-Updater-" + std::to_wstring(GetCurrentProcessId()) + L".exe");
    fs::remove(helper, ignored);
    // Run the helper logic from the already verified incoming version. This
    // lets updater hardening take effect on the same release that carries it,
    // instead of copying and re-running the older installed implementation.
    std::wstring helperError;
    if (!CopyFileW(staged.c_str(), helper.c_str(), FALSE))
        helperError = LastErrorMessage();
    else if (!FlushExistingFile(helper))
        helperError = LastErrorMessage();
    if (!helperError.empty()) {
        fs::remove(staged, ignored);
        fs::remove(helper, ignored);
        error = L"无法创建更新助手：" + helperError;
        return false;
    }
    const std::wstring readyEventName = L"Local\\RenderMesh.UpdateReady." +
        std::to_wstring(GetCurrentProcessId()) + L"." +
        std::to_wstring(GetTickCount64());
    HANDLE readyEvent = CreateEventW(
        nullptr, TRUE, FALSE, readyEventName.c_str());
    if (!readyEvent || GetLastError() == ERROR_ALREADY_EXISTS) {
        if (readyEvent) CloseHandle(readyEvent);
        fs::remove(staged, ignored);
        fs::remove(helper, ignored);
        error = L"无法创建更新接管确认信号，当前程序将继续运行。";
        return false;
    }
    PROCESS_INFORMATION process{};
    if (!StartProcess(
            helper,
            {L"--apply-update", std::to_wstring(GetCurrentProcessId()),
             staged.wstring(), target.wstring(), backup.wstring(),
             Utf8ToWide(result.offer.version), Utf8ToWide(result.offer.sha256),
             L"--update-ready-event", readyEventName},
            CREATE_NO_WINDOW, process, error)) {
        CloseHandle(readyEvent);
        fs::remove(staged, ignored);
        fs::remove(helper, ignored);
        error = L"无法启动更新助手：" + error;
        return false;
    }
    CloseHandle(process.hThread);
    HANDLE readyWaits[] = {readyEvent, process.hProcess};
    const DWORD ready = WaitForMultipleObjects(2, readyWaits, FALSE, 10000);
    if (ready != WAIT_OBJECT_0) {
        DWORD helperExit = STILL_ACTIVE;
        GetExitCodeProcess(process.hProcess, &helperExit);
        if (ready == WAIT_TIMEOUT) {
            TerminateProcess(process.hProcess, 29);
            WaitForSingleObject(process.hProcess, 2000);
        }
        CloseHandle(process.hProcess);
        CloseHandle(readyEvent);
        fs::remove(staged, ignored);
        fs::remove(helper, ignored);
        error = ready == WAIT_TIMEOUT
            ? L"更新助手在 10 秒内没有确认接管，当前程序不会退出。"
            : L"更新助手在接管前退出，当前程序不会退出。错误码：" +
                std::to_wstring(helperExit);
        return false;
    }
    CloseHandle(process.hProcess);
    CloseHandle(readyEvent);
    LogInfo("LAN update helper acknowledged readiness; version=" +
            result.offer.version);
    return true;
}

int ApplyDownloadedUpdate(DWORD parentProcessId, const fs::path& pending,
                          const fs::path& target, const fs::path& backup,
                          const std::string& version,
                          const std::string& expectedSha256,
                          const std::wstring& readyEventName) {
    const fs::path expectedBackup(target.wstring() + L".previous");
    const std::wstring pendingName = pending.filename().wstring();
    DWORD binaryType = 0;
    if (parentProcessId == 0 || !pending.is_absolute() || !target.is_absolute() ||
        !backup.is_absolute() ||
        _wcsicmp(target.filename().c_str(), L"RenderMesh.exe") != 0 ||
        pending.parent_path() != target.parent_path() ||
        pendingName.size() < 12 ||
        pendingName.substr(pendingName.size() - 12) != L".exe.pending" ||
        backup != expectedBackup || !IsNewerAppVersion(version, "0.0.0") ||
        expectedSha256.size() != 64 ||
        !std::all_of(expectedSha256.begin(), expectedSha256.end(), [](char ch) {
            return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f');
        }) ||
        !fs::is_regular_file(pending) ||
        Sha256File(pending) != expectedSha256 ||
        !GetBinaryTypeW(pending.c_str(), &binaryType) ||
        (binaryType != SCS_64BIT_BINARY && binaryType != SCS_32BIT_BINARY))
        return 20;

    HANDLE parent = OpenProcess(SYNCHRONIZE, FALSE, parentProcessId);
    if (!parent) {
        const DWORD openError = GetLastError();
        if (!readyEventName.empty() || openError != ERROR_INVALID_PARAMETER)
            return 22;
    }

    if (!readyEventName.empty()) {
        HANDLE readyEvent = OpenEventW(
            EVENT_MODIFY_STATE, FALSE, readyEventName.c_str());
        if (!readyEvent) {
            if (parent) CloseHandle(parent);
            return 28;
        }
        const BOOL signalled = SetEvent(readyEvent);
        CloseHandle(readyEvent);
        if (!signalled) {
            if (parent) CloseHandle(parent);
            return 28;
        }
        LogInfo("LAN updater helper validated payload and acknowledged readiness");
    }

    if (parent) {
        const DWORD wait = WaitForSingleObject(parent, 60000);
        CloseHandle(parent);
        if (wait != WAIT_OBJECT_0) {
            const DWORD waitError = wait == WAIT_FAILED
                ? GetLastError() : WAIT_TIMEOUT;
            if (!readyEventName.empty())
                RelaunchAfterUpdateFailure(target, backup, 21, waitError);
            return 21;
        }
    }

    DWORD removeError = ERROR_SUCCESS;
    unsigned removeRetries = 0;
    if (!DeleteUpdateBackupWithRetry(backup, removeError, removeRetries)) {
        LogError("LAN updater could not remove the previous backup; win32=" +
                 std::to_string(removeError) + "; message=" +
                 WideToUtf8(LastErrorMessage(removeError)));
        RelaunchAfterUpdateFailure(target, backup, 23, removeError);
        return 23;
    }
    if (removeRetries)
        LogInfo("LAN updater removed the previous backup after retries=" +
                std::to_string(removeRetries));

    DWORD replaceError = ERROR_SUCCESS;
    unsigned replaceRetries = 0;
    if (!ReplaceUpdateFileWithRetry(
            target, pending, backup, replaceError, replaceRetries)) {
        LogError("LAN updater could not replace the executable; win32=" +
                 std::to_string(replaceError) + "; message=" +
                 WideToUtf8(LastErrorMessage(replaceError)));
        RelaunchAfterUpdateFailure(target, backup, 24, replaceError);
        return 24;
    }
    if (replaceRetries)
        LogInfo("LAN updater replaced the executable after transient lock retries=" +
                std::to_string(replaceRetries));
    auto restorePrevious = [&] {
        const fs::path failed(target.wstring() + L".failed-update");
        DWORD restoreError = ERROR_SUCCESS;
        unsigned restoreRetries = 0;
        if (!RestorePreviousUpdateWithRetry(
                target, backup, failed, restoreError, restoreRetries)) {
            LogError("LAN updater could not restore the previous executable; win32=" +
                     std::to_string(restoreError) + "; message=" +
                     WideToUtf8(LastErrorMessage(restoreError)));
            return false;
        }
        if (restoreRetries)
            LogInfo("LAN updater restored the previous executable after transient lock retries=" +
                    std::to_string(restoreRetries));
        return true;
    };

    // ReplaceFileW published exactly the file verified above.
    const std::wstring healthEventName = L"Local\\RenderMesh.UpdateHealthy." +
        std::to_wstring(GetCurrentProcessId()) + L"." +
        std::to_wstring(GetTickCount64());
    HANDLE healthEvent = CreateEventW(
        nullptr, TRUE, FALSE, healthEventName.c_str());
    if (!healthEvent || GetLastError() == ERROR_ALREADY_EXISTS) {
        if (healthEvent) CloseHandle(healthEvent);
        const bool restored = restorePrevious();
        PROCESS_INFORMATION previous{};
        std::wstring relaunchError;
        const fs::path relaunch = restored ? target : backup;
        if (StartProcess(relaunch, {}, 0, previous, relaunchError)) {
            CloseHandle(previous.hThread);
            CloseHandle(previous.hProcess);
        }
        return 30;
    }
    PROCESS_INFORMATION replacement{};
    std::wstring launchError;
    if (!StartProcess(target,
                      {L"--updated-from", Utf8ToWide(version),
                       L"--update-health-event", healthEventName}, 0,
                      replacement, launchError)) {
        CloseHandle(healthEvent);
        const bool restored = restorePrevious();
        PROCESS_INFORMATION previous{};
        const fs::path relaunch = restored ? target : backup;
        if (StartProcess(relaunch, {}, 0, previous, launchError)) {
            CloseHandle(previous.hThread);
            CloseHandle(previous.hProcess);
        }
        return 25;
    }
    CloseHandle(replacement.hThread);
    HANDLE startupWaits[] = {healthEvent, replacement.hProcess};
    const DWORD startupResult = WaitForMultipleObjects(
        2, startupWaits, FALSE, 15000);
    if (startupResult == WAIT_TIMEOUT || startupResult == WAIT_FAILED) {
        TerminateProcess(replacement.hProcess, 31);
        WaitForSingleObject(replacement.hProcess, 2000);
    }
    CloseHandle(replacement.hProcess);
    CloseHandle(healthEvent);
    if (startupResult != WAIT_OBJECT_0) {
        const bool restored = restorePrevious();
        PROCESS_INFORMATION previous{};
        const fs::path relaunch = restored ? target : backup;
        if (StartProcess(relaunch, {}, 0, previous, launchError)) {
            CloseHandle(previous.hThread);
            CloseHandle(previous.hProcess);
        }
        return 26;
    }
    MoveFileExW(ExecutablePath().c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
    return 0;
}

}  // namespace

#ifdef RM_DEV_TOOLS
// Verification commands used by the test scripts. They are compiled only into
// RenderMesh-dev.exe; the distributed executable ignores these switches.
struct DeveloperCommand {
    bool selfTest = false;
    bool noDialog = false;
    fs::path report, blenderOverride;
    fs::path renderSource, renderOutputDir, compatibilitySource;
    fs::path previewSource, previewOutput, blenderCatalogReport;
    int renderFrame = 1;
    std::string renderFormat = "PNG";

    bool Requested() const {
        return selfTest || !blenderCatalogReport.empty() || !previewSource.empty() ||
            !renderSource.empty() || !compatibilitySource.empty();
    }
};

bool ParseDeveloperArgument(DeveloperCommand& command, LPWSTR* arguments,
                            int argumentCount, int& index) {
    const std::wstring arg = arguments[index];
    const bool hasValue = index + 1 < argumentCount;
    if (arg == L"--self-test") command.selfTest = true;
    else if (arg == L"--no-dialog") command.noDialog = true;
    else if (arg == L"--preview-delete-confirmation") gPreviewDeleteConfirmation = true;
    else if (arg == L"--preview-config-window") gPreviewConfigWindow = true;
    else if (arg == L"--preview-config-drawer") gPreviewConfigWindow = gPreviewConfigDrawer = true;
    else if (arg == L"--preview-drop-overlay") gPreviewDropOverlay = true;
    else if (arg == L"--preview-blender-missing") gPreviewBlenderMissing = true;
    else if (!hasValue) return false;
    else if (arg == L"--report") command.report = arguments[++index];
    else if (arg == L"--blender") command.blenderOverride = arguments[++index];
    else if (arg == L"--render") command.renderSource = arguments[++index];
    else if (arg == L"--output-dir") command.renderOutputDir = arguments[++index];
    else if (arg == L"--frame") command.renderFrame = _wtoi(arguments[++index]);
    else if (arg == L"--format") command.renderFormat = WideToUtf8(arguments[++index]);
    else if (arg == L"--compatibility") command.compatibilitySource = arguments[++index];
    else if (arg == L"--preview") command.previewSource = arguments[++index];
    else if (arg == L"--preview-output") command.previewOutput = arguments[++index];
    else if (arg == L"--blender-catalog-report") command.blenderCatalogReport = arguments[++index];
    else if (arg == L"--ui-metrics-report") gUiMetricsReport = arguments[++index];
    else if (arg == L"--ui-test-rows")
        gUiTestRows = std::clamp(_wtoi(arguments[++index]), 0, 10000);
    else return false;
    return true;
}

int RunDeveloperCommand(DeveloperCommand command, const fs::path& dataDir) {
    if (!command.blenderCatalogReport.empty()) {
        std::atomic<bool> cancel = false;
        std::vector<BlenderRelease> releases;
        std::wstring catalogError;
        const bool ok = FetchOfficialBlenderCatalog(releases, cancel, catalogError);
        json output = {
            {"ok", ok},
            {"error", WideToUtf8(catalogError)},
            {"latest_probe_ok", false},
            {"releases", json::array()},
        };
        for (const auto& release : releases) {
            output["releases"].push_back({
                {"branch", release.branch}, {"version", release.version},
                {"lts", release.lts}, {"msi_url", WideToUtf8(release.msiUrl)},
                {"sha256_url", WideToUtf8(release.sha256Url)},
            });
        }
        if (ok && !releases.empty()) {
            uint64_t sizeBytes = 0;
            std::string sha256;
            std::wstring probeError;
            const bool probeOk = ProbeOfficialBlenderRelease(
                releases.front(), cancel, sizeBytes, sha256, probeError);
            output["latest_probe_ok"] = probeOk;
            output["latest_size_bytes"] = sizeBytes;
            output["latest_sha256"] = sha256;
            output["latest_probe_error"] = WideToUtf8(probeError);
            if (!probeOk) {
                output["ok"] = false;
                catalogError = probeError;
            }
        }
        std::wstring writeError;
        const bool written = WriteFileAtomic(command.blenderCatalogReport, output.dump(2),
                                             writeError);
        const bool verified = output["ok"].get<bool>();
        PrintToParentConsole(verified ? L"BLENDER_CATALOG_OK" : catalogError);
        return verified && written ? 0 : 13;
    }
    if (command.selfTest) {
        std::wstring result;
        bool ok = RunSelfTest(result);
        if (ok) {
            std::wstring autoStartResult;
            if (!RunAutoStartLogicSelfTest(autoStartResult)) {
                ok = false;
                result = L"Windows 开机自启自检失败：" + autoStartResult;
            }
        }
        if (ok) {
            std::wstring blenderInstallerResult;
            if (!RunBlenderInstallerSelfTest(blenderInstallerResult)) {
                ok = false;
                result = L"Blender 安装器自检失败：" + blenderInstallerResult;
            }
        }
        if (!command.report.empty()) {
            std::wstring ignored;
            WriteFileAtomic(command.report, WideToUtf8(result), ignored);
        }
        PrintToParentConsole(result);
        if (!command.noDialog)
            MessageBoxW(nullptr, result.c_str(),
                        ok ? L"RenderMesh 自检通过" : L"RenderMesh 自检失败",
                        ok ? MB_ICONINFORMATION : MB_ICONERROR);
        return ok ? 0 : 1;
    }

    Config config = ConfigStore(dataDir).Load();
    if (!command.blenderOverride.empty()) config.blenderPath = command.blenderOverride;
    const RenderAdapter& adapter = BlenderRenderAdapter();
    std::atomic<bool> stop = false;
    if (!command.previewSource.empty()) {
        if (command.previewOutput.empty())
            command.previewOutput = dataDir / L"verification" / L"preview.png";
        std::wstring previewError;
        const bool ok = adapter.ConvertPreview(config, dataDir, command.previewSource,
                                               command.previewOutput, stop, previewError);
        PrintToParentConsole(ok ? L"预览生成成功：" + command.previewOutput.wstring()
                                : previewError);
        return ok ? 0 : 6;
    }
    if (!command.renderSource.empty()) {
        // --report may name an `inspect --json` result for the output format
        // and File Output targets of the project.
        RenderPackageRequest request;
        request.config = config;
        request.project = command.renderSource;
        request.outputDir = command.renderOutputDir.empty()
            ? dataDir / L"verification" / L"render" : command.renderOutputDir;
        request.frames = {command.renderFrame};
        request.outputFormat = command.renderFormat;
        request.outputExtension = command.renderFormat == "PNG" ? ".png"
            : command.renderFormat == "JPEG" ? ".jpg" : ".exr";
        const json manifest = command.report.empty()
            ? json(json::value_t::discarded)
            : json::parse(ReadFileText(command.report), nullptr, false);
        if (manifest.is_object()) {
            request.outputFormat = manifest.value("render_format", request.outputFormat);
            request.outputExtension = manifest.value("file_extension", request.outputExtension);
            for (const auto& value : manifest.value("compositor_outputs", json::array())) {
                OutputTarget target;
                if (ParseOutputTargetMetadata(value, target))
                    request.outputs.push_back(std::move(target));
            }
        }
        request.stop = &stop;
        RenderPackageResult result;
        const bool ok = adapter.RenderPackage(request, result);
        fs::path output;
        for (const auto& artifact : result.completed[command.renderFrame])
            if (artifact.logicalPath == "primary") output = artifact.source;
        PrintToParentConsole(ok ? L"渲染成功：" + output.wstring() : result.error);
        return ok ? 0 : 4;
    }
    if (!command.compatibilitySource.empty()) {
        const json manifest = json::parse(ReadFileText(command.report), nullptr, false);
        if (!manifest.is_object()) return 5;
        std::wstring summary;
        const bool ok = adapter.CheckCompatibility(
            config, dataDir, dataDir / L"verification" / L"compatibility",
            manifest.value("version", std::string{}),
            manifest.value("render_engine", std::string{}),
            manifest.value("requires_gpu", false), stop, summary);
        PrintToParentConsole(summary);
        return ok ? 0 : 5;
    }
    return 0;
}
#endif  // RM_DEV_TOOLS

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    OleInitialize(nullptr);
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_LISTVIEW_CLASSES | ICC_BAR_CLASSES};
    InitCommonControlsEx(&controls);
    int argumentCount = 0;
    LPWSTR* arguments = CommandLineToArgvW(GetCommandLineW(), &argumentCount);
    if (argumentCount >= 2 && IsCommandLineCommand(arguments[1])) {
        const int exitCode = RunCommandLine(argumentCount, arguments);
        LocalFree(arguments);
        OleUninitialize();
        return exitCode;
    }
    // `serve` runs the node without a window until Ctrl+C or termination.
    const bool serve = argumentCount >= 2 && std::wstring(arguments[1]) == L"serve";
    fs::path dataDir = DefaultDataDirectory();
    bool headless = serve;
    fs::path stateReport;
    std::string setMatchId;
    bool applyUpdate = false, noUpdateCheck = false, startupInvocation = false;
    bool noAutoStartRegistration = false;
    DWORD updateParentProcessId = 0;
    fs::path updatePending, updateTarget, updateBackup;
    std::string updateVersion, updateSha256, updatedFromVersion;
    std::wstring updateReadyEventName, updateHealthEventName;
    int overridePort = 0, runSeconds = 0, startupWindowDelayMs = 0;
#ifdef RM_DEV_TOOLS
    DeveloperCommand developer;
#endif
    for (int i = serve ? 2 : 1; i < argumentCount; ++i) {
        std::wstring arg = arguments[i];
        if (arg == L"--headless") headless = true;
        else if (arg == L"--startup") startupInvocation = true;
        else if (arg == L"--no-update-check") noUpdateCheck = true;
        else if (arg == L"--no-autostart-registration")
            noAutoStartRegistration = true;
        else if (arg == L"--updated-from" && i + 1 < argumentCount)
            updatedFromVersion = WideToUtf8(arguments[++i]);
        else if (arg == L"--apply-update" && i + 6 < argumentCount) {
            applyUpdate = true;
            updateParentProcessId = static_cast<DWORD>(_wcstoui64(arguments[++i], nullptr, 10));
            updatePending = arguments[++i];
            updateTarget = arguments[++i];
            updateBackup = arguments[++i];
            updateVersion = WideToUtf8(arguments[++i]);
            updateSha256 = WideToUtf8(arguments[++i]);
        }
        else if (arg == L"--update-ready-event" && i + 1 < argumentCount)
            updateReadyEventName = arguments[++i];
        else if (arg == L"--update-health-event" && i + 1 < argumentCount)
            updateHealthEventName = arguments[++i];
        else if (arg == L"--data-dir" && i + 1 < argumentCount) dataDir = arguments[++i];
        else if (arg == L"--port" && i + 1 < argumentCount) overridePort = _wtoi(arguments[++i]);
        else if (arg == L"--run-seconds" && i + 1 < argumentCount) runSeconds = _wtoi(arguments[++i]);
        else if (arg == L"--set-match-id" && i + 1 < argumentCount)
            setMatchId = WideToUtf8(arguments[++i]);
        else if (arg == L"--state-report" && i + 1 < argumentCount)
            stateReport = arguments[++i];
        else if (arg == L"--page" && i + 1 < argumentCount)
            gInitialPage = std::clamp(_wtoi(arguments[++i]), 0, 3);
        else if (arg == L"--detail") gOpenInitialDetail = true;
        else if (arg == L"--ui-test-startup-delay-ms" && i + 1 < argumentCount)
            startupWindowDelayMs = std::clamp(_wtoi(arguments[++i]), 0, 10000);
#ifdef RM_DEV_TOOLS
        else ParseDeveloperArgument(developer, arguments, argumentCount, i);
#endif
    }
    LocalFree(arguments);
    if (applyUpdate) {
        InitializeLogging(dataDir);
        LogInfo("LAN updater helper started; version=" + updateVersion);
        const int result = ApplyDownloadedUpdate(
            updateParentProcessId, updatePending, updateTarget, updateBackup,
            updateVersion, updateSha256, updateReadyEventName);
        if (result == 0)
            LogInfo("LAN updater helper completed; version=" + updateVersion);
        else
            LogError("LAN updater helper failed; version=" + updateVersion +
                     "; code=" + std::to_string(result));
        OleUninitialize();
        return result;
    }
    gDisableUpdateChecks = noUpdateCheck || headless || !gUiMetricsReport.empty();
    gAutoStartRegistrationEnabled = !noAutoStartRegistration;
    InitializeLogging(dataDir);
    if (!updatedFromVersion.empty())
        LogInfo("updated successfully from version " + updatedFromVersion +
                " to " + kRenderMeshAppVersion);
#ifdef RM_DEV_TOOLS
    if (developer.Requested()) {
        const int result = RunDeveloperCommand(std::move(developer), dataDir);
        OleUninitialize();
        return result;
    }
#endif

    if (serve) {
        gServeConsoleAttached = AttachConsole(ATTACH_PARENT_PROCESS) != FALSE;
        SetConsoleCtrlHandler(StopServeOnConsoleSignal, TRUE);
    }
    if (!headless || serve) {
        bool existingInstance = false;
        std::wstring singleInstanceError;
        if (!AcquireSingleInstance(dataDir, !startupInvocation && !serve,
                                   existingInstance, singleInstanceError)) {
            MessageBoxW(nullptr, singleInstanceError.c_str(), L"RenderMesh 启动失败",
                        MB_ICONERROR);
            OleUninitialize();
            return 9;
        }
        if (existingInstance) {
            if (serve)
                PrintToParentConsole(L"RenderMesh already runs with the data folder " +
                                     dataDir.wstring() + L". Stop that process first.");
            OleUninitialize();
            return serve ? 1 : 0;
        }
        if (startupInvocation && noAutoStartRegistration &&
            startupWindowDelayMs > 0)
            Sleep(static_cast<DWORD>(startupWindowDelayMs));
    }

    ConfigStore store(dataDir);
    Config config = store.Load();
    if (!headless && gUiMetricsReport.empty() &&
        gAutoStartRegistrationEnabled &&
        !SyncAutoStartRegistration(config, gAutoStartStartupWarning))
        LogWarning("auto-start registration refresh failed: " +
                   WideToUtf8(gAutoStartStartupWarning));
    if (overridePort >= 1024 && overridePort <= 65535) config.httpPort = overridePort;
    gApp = std::make_unique<NodeApp>(std::move(store), config);
    std::wstring error;
    if (!gApp->Start(error)) {
        MessageBoxW(nullptr, error.c_str(), L"RenderMesh 启动失败", MB_ICONERROR);
        ReleaseSingleInstance();
        return 2;
    }
    if (!setMatchId.empty()) {
        Config updated = gApp->GetConfig();
        updated.inviteCode = setMatchId;
        if (!gApp->UpdateConfig(std::move(updated), error)) {
            PrintToParentConsole(error);
            gApp->Stop();
            ReleaseSingleInstance();
            return 8;
        }
    }
    if (headless) {
        if (serve) {
            if (gSingleInstanceReadyEvent) SetEvent(gSingleInstanceReadyEvent);
            PrintToParentConsole(L"RenderMesh " + Utf8ToWide(kRenderMeshAppVersion) +
                                 L" serves on port " +
                                 std::to_wstring(gApp->GetConfig().httpPort) +
                                 L". Press Ctrl+C to stop.");
        }
        auto deadline = runSeconds > 0
            ? std::chrono::steady_clock::now() + std::chrono::seconds(runSeconds)
            : std::chrono::steady_clock::time_point::max();
        while (std::chrono::steady_clock::now() < deadline && !gServeStopRequested.load())
            Sleep(200);
        if (!stateReport.empty()) {
            const Config current = gApp->GetConfig();
            const auto peers = gApp->Peers();
            json peerStates = json::array();
            for (const auto& peer : peers) {
                peerStates.push_back({
                    {"node_id", peer.nodeId}, {"name", peer.name},
                    {"app_version", peer.appVersion},
                    {"share", peer.shareEnabled}, {"assigned", peer.assigned},
                    {"idle_ready_known", peer.idleReadyKnown},
                    {"idle_ready", peer.idleReady},
                    {"current_task_known", peer.currentTaskKnown},
                    {"current_task_id", peer.currentTaskId},
                    {"idle_seconds", peer.idleSeconds},
                });
            }
            json state = {
                {"invite_code", current.inviteCode},
                {"pairing_pending", current.pairingPending},
                {"manual_peer_endpoint", current.manualPeerEndpoint},
                {"manual_peer_status", WideToUtf8(gApp->ManualPeerStatus())},
                {"cluster_token", current.clusterToken},
                {"peer_count", peers.size()},
                {"peers", peerStates},
            };
            std::wstring ignored;
            WriteFileAtomic(stateReport, state.dump(2), ignored);
        }
        gApp->Stop();
        ReleaseSingleInstance();
        return 0;
    }

    gFont = CreateFontW(-16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                        DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI Variable Text");
    gTitleFont = CreateFontW(-28, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                             OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                             DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI Variable Display");
    gIconFont = CreateFontW(-24, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                            DEFAULT_PITCH | FF_DONTCARE, L"Segoe Fluent Icons");
    gSemiboldFont = CreateFontW(-17, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                                CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                DEFAULT_PITCH | FF_DONTCARE,
                                L"Segoe UI Variable Text");
    gMatchIdFont = CreateFontW(-20, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                               DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                               CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                               DEFAULT_PITCH | FF_DONTCARE,
                               L"Segoe UI Variable Text");
    gLogFont = CreateFontW(-16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                           OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                           FIXED_PITCH | FF_MODERN, L"Cascadia Mono");
    WNDCLASSW windowClass{};
    windowClass.lpfnWndProc = WindowProc;
    windowClass.hInstance = instance;
    windowClass.lpszClassName = L"RenderMeshNativeWindow";
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hIcon = reinterpret_cast<HICON>(LoadImageW(
        instance, MAKEINTRESOURCEW(IDI_RENDERMESH), IMAGE_ICON, 0, 0, LR_DEFAULTSIZE));
    windowClass.hbrBackground = nullptr;
    RegisterClassW(&windowClass);
    WNDCLASSW detailClass = windowClass;
    detailClass.lpfnWndProc = DetailWindowProc;
    detailClass.lpszClassName = L"RenderMeshJobDetail";
    RegisterClassW(&detailClass);
    WNDCLASSW logClass = windowClass;
    logClass.lpfnWndProc = FrameLogWindowProc;
    logClass.lpszClassName = L"RenderMeshFrameLog";
    RegisterClassW(&logClass);
    WNDCLASSW uploadClass = windowClass;
    uploadClass.lpfnWndProc = UploadWindowProc;
    uploadClass.lpszClassName = L"RenderMeshUploadProgress";
    RegisterClassW(&uploadClass);
    WNDCLASSW configClass = windowClass;
    configClass.lpfnWndProc = ConfigWindowProc;
    configClass.lpszClassName = L"RenderMeshSubmitConfig";
    RegisterClassW(&configClass);
    WNDCLASSW internalDialogClass = windowClass;
    internalDialogClass.lpfnWndProc = InternalDialogProc;
    internalDialogClass.lpszClassName = L"RenderMeshInternalDialog";
    internalDialogClass.hIcon = nullptr;
    RegisterClassW(&internalDialogClass);
    WNDCLASSW workflowProgressClass = windowClass;
    workflowProgressClass.lpfnWndProc = WorkflowProgressProc;
    workflowProgressClass.lpszClassName = L"RenderMeshWorkflowProgress";
    workflowProgressClass.hIcon = nullptr;
    RegisterClassW(&workflowProgressClass);
    gWindow = CreateWindowExW(
        0, windowClass.lpszClassName, L"RenderMesh",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX |
            WS_MAXIMIZEBOX | WS_THICKFRAME | WS_CLIPCHILDREN,
        CW_USEDEFAULT, CW_USEDEFAULT, 1280, 820, nullptr, nullptr, instance, nullptr);
    if (!gWindow) {
        error = L"无法创建 RenderMesh 主窗口：" + LastErrorMessage();
        MessageBoxW(nullptr, error.c_str(), L"RenderMesh 启动失败", MB_ICONERROR);
        gApp->Stop();
        ReleaseSingleInstance();
        OleUninitialize();
        return 10;
    }
    if (gSingleInstanceReadyEvent && !SetEvent(gSingleInstanceReadyEvent)) {
        error = L"无法发布 RenderMesh 实例就绪状态：" + LastErrorMessage();
        MessageBoxW(nullptr, error.c_str(), L"RenderMesh 启动失败", MB_ICONERROR);
        DestroyWindow(gWindow);
        gApp->Stop();
        ReleaseSingleInstance();
        OleUninitialize();
        return 11;
    }
    if (!updateHealthEventName.empty()) {
        HANDLE updateHealthEvent = OpenEventW(
            EVENT_MODIFY_STATE, FALSE, updateHealthEventName.c_str());
        const BOOL updateHealthy = updateHealthEvent
            ? SetEvent(updateHealthEvent) : FALSE;
        if (updateHealthEvent) CloseHandle(updateHealthEvent);
        if (!updateHealthy) {
            LogError("updated instance could not acknowledge service readiness");
            DestroyWindow(gWindow);
            gApp->Stop();
            ReleaseSingleInstance();
            OleUninitialize();
            return 12;
        }
        LogInfo("updated instance acknowledged UI and node service readiness");
    }
    gApp->SetChangedCallback([] {
        QueueModelRefresh();
    });
    const bool silentStartup =
        ShouldStartSilently(gApp->GetConfig(), startupInvocation);
    if (!gUiMetricsReport.empty()) {
        SetWindowPos(gWindow, nullptr, -32000, -32000, 1280, 820,
                     SWP_NOACTIVATE | SWP_NOZORDER);
        ShowWindow(gWindow, SW_SHOWNOACTIVATE);
    } else if (silentStartup) {
        if (!UpdateTrayIcon()) {
            LogWarning("silent auto-start could not register the tray icon; showing main window");
            ShowWindow(gWindow, show);
        }
    } else {
        ShowWindow(gWindow, show);
    }
    UpdateWindow(gWindow);
    if (gPreviewBlenderMissing) {
        gBlenderInstallPromptShown = true;
        gBlenderInstallNvidiaAvailable = true;
        ShowInternalDialog(
            InternalDialogMode::BlenderMissing,
            L"检测到没有安装 Blender", L"",
            L"当前电脑未检测到可用的 Blender，无法接收或提交渲染任务。"
            L"是否要先安装 Blender？\n\n"
            L"RenderMesh 会先读取 Blender 官方镜像中的近期 Windows MSI 版本，"
            L"由你选择版本；下载后校验官方 SHA-256，再启动官方安装程序。");
    }
    if (gPreviewDeleteConfirmation) {
        RenderJob previewJob;
        previewJob.id = "ui-preview-job";
        previewJob.name = "双机验收-20260814-1210";
        ShowDeleteConfirmation(previewJob);
    }
    if (gPreviewConfigWindow) {
        for (const auto& job : gApp->Jobs()) {
            if (job.stage == JobStage::ReadyToConfigure) {
                ShowConfigWindow(job.id);
                if (gPreviewConfigDrawer && gConfigWindow)
                    SetTimer(gConfigWindow, TIMER_CONFIG_DRAWER_PREVIEW, 250, nullptr);
                break;
            }
        }
    }
    if (!gUiMetricsReport.empty()) {
        gLastRefresh = std::chrono::steady_clock::now();
        for (int index = 0; index < UI_TEST_BURST_NOTIFICATIONS; ++index)
            QueueModelRefresh();
        SetTimer(gWindow, TIMER_UI_TEST_REPORT, 1200, nullptr);
    }
    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    gApp->SetChangedCallback({});
    StopPreviewGeneration();
    gApp->Stop();
    gPreflightStop = true;
    if (gPreflight.joinable()) gPreflight.join();
    ReleaseGraphics();
    if (gRowHeightImages) ImageList_Destroy(gRowHeightImages);
    DeleteObject(gFont);
    DeleteObject(gTitleFont);
    DeleteObject(gIconFont);
    DeleteObject(gSemiboldFont);
    DeleteObject(gMatchIdFont);
    DeleteObject(gLogFont);
    if (gBackgroundBrush) DeleteObject(gBackgroundBrush);
    if (gSidebarBrush) DeleteObject(gSidebarBrush);
    if (gSurfaceBrush) DeleteObject(gSurfaceBrush);
    if (gInternalDialogBackdrop) DeleteObject(gInternalDialogBackdrop);
    if (gWorkflowProgressBackdrop) DeleteObject(gWorkflowProgressBackdrop);
    ReleaseSingleInstance();
    OleUninitialize();
    return static_cast<int>(message.wParam);
}
