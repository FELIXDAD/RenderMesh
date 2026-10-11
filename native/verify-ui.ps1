[CmdletBinding()]
param(
    [ValidateSet('Release','Debug')]
    [string]$Configuration = 'Release',
    [string]$Executable = '',
    [switch]$SkipBuild
)

$ErrorActionPreference = 'Stop'
$nativeRoot = $PSScriptRoot
$repoRoot = Split-Path -Parent $nativeRoot

& (Join-Path $nativeRoot 'test-ui.ps1')
if (-not $SkipBuild) {
    & (Join-Path $nativeRoot 'build.ps1') -Configuration $Configuration
}

$exe = if ($Executable) { [IO.Path]::GetFullPath($Executable) } else {
    Join-Path $nativeRoot "build\$Configuration\RenderMesh.exe"
}
if (-not (Test-Path -LiteralPath $exe -PathType Leaf)) {
    throw 'UI verification executable was not found.'
}
& (Join-Path $nativeRoot 'test-add-button.ps1') -Executable $exe
& (Join-Path $nativeRoot 'test-context-menu-dismissal.ps1') -Executable $exe
& (Join-Path $nativeRoot 'test-autostart-settings-ui.ps1') -ExePath $exe
& (Join-Path $nativeRoot 'test-blender-installer-ui.ps1') -ExePath $exe
$verificationRoot = Join-Path $nativeRoot 'build\ui-regression'
$dataDir = Join-Path $verificationRoot 'data'
$reportPath = Join-Path $verificationRoot 'metrics.json'
New-Item -ItemType Directory -Force -Path $verificationRoot | Out-Null

$process = Start-Process -FilePath $exe -ArgumentList @(
    '--data-dir', $dataDir,
    '--port', '49391',
    '--page', '1',
    '--ui-test-rows', '100',
    '--ui-metrics-report', $reportPath,
    '--no-update-check'
) -WindowStyle Hidden -PassThru -Wait
if ($process.ExitCode -ne 0) {
    throw "RenderMesh UI regression process failed with exit code $($process.ExitCode)."
}
if (-not (Test-Path -LiteralPath $reportPath -PathType Leaf)) {
    throw 'RenderMesh did not produce the UI regression report.'
}

$metrics = Get-Content -LiteralPath $reportPath -Raw | ConvertFrom-Json
$checks = [ordered]@{
    jobs_visible_rows = $metrics.jobs_visible_rows -ge 12
    local_ready_shows_available = [bool]$metrics.local_ready_shows_available
    local_active_shows_in_use = [bool]$metrics.local_active_shows_in_use
    remote_active_shows_in_use = [bool]$metrics.remote_active_shows_in_use
    remote_ready_hides_idle_duration =
        [bool]$metrics.remote_ready_hides_idle_duration
    remote_unknown_readiness_does_not_guess_from_idle_time =
        [bool]$metrics.remote_unknown_readiness_shows_in_use
    test_completed_shows_full_render_action =
        [bool]$metrics.test_completed_shows_full_render_action
    detail_primary_actions_are_exclusive =
        [bool]$metrics.detail_primary_actions_are_exclusive
    assigned_local_shows_rendering = [bool]$metrics.assigned_local_shows_rendering
    assigned_local_shows_queued = [bool]$metrics.assigned_local_shows_queued
    assigned_remote_shows_queued = [bool]$metrics.assigned_remote_shows_queued
    assigned_remote_shows_rendering = [bool]$metrics.assigned_remote_shows_rendering
    detail_primary_action_uses_rightmost_slot =
        [bool]$metrics.detail_primary_action_uses_rightmost_slot
    stable_snapshot_is_noop = -not $metrics.stable_snapshot_mutated -and
        $metrics.stable_snapshot_mutation_delta -eq 0
    one_row_change_is_incremental = $metrics.incremental_mutation_delta -eq 1
    structural_update_is_exercised = [bool]$metrics.structural_update_applied
    list_height_is_stable_across_row_counts =
        [bool]$metrics.list_height_stable_across_row_counts
    static_text_background_matches_its_region =
        [bool]$metrics.static_label_brushes_match_regions
    visible_fields_do_not_use_legacy_frames =
        [bool]$metrics.visible_fields_avoid_legacy_frames
    visible_fields_have_rounded_regions =
        [bool]$metrics.visible_fields_have_rounded_regions
    theme_selector_uses_owner_draw =
        [bool]$metrics.theme_selector_uses_owner_draw
    dialog_fields_use_the_same_modern_frame =
        [bool]$metrics.dialog_fields_use_modern_frames
    dialog_field_frame_is_actually_painted =
        [bool]$metrics.dialog_field_frame_was_painted
    server_tasks_and_contributions_fill_available_height =
        [bool]$metrics.main_lists_fill_available_height
    settings_save_button_is_removed = [bool]$metrics.save_button_removed
    settings_are_saved_automatically = [bool]$metrics.settings_auto_save_enabled
    settings_navigation_is_bottom_left_gear =
        [bool]$metrics.settings_nav_is_bottom_left_icon
    settings_navigation_toggles_back_to_previous_page =
        [bool]$metrics.settings_navigation_toggles_back
    context_menu_surface_is_repainted_before_action =
        [bool]$metrics.context_menu_repainted_before_action
    duplicate_sidebar_brand_is_removed = [bool]$metrics.sidebar_brand_removed
    main_window_title_is_product_only =
        [bool]$metrics.main_window_title_is_product_only
    share_control_uses_centered_network_icon =
        [bool]$metrics.share_control_uses_network_icon
    share_control_matches_navigation_size =
        [bool]$metrics.share_control_matches_navigation_size
    tray_share_toggle_repaints_main_switch =
        [bool]$metrics.tray_share_toggle_repaints_main_switch
    auto_start_settings_available = [bool]$metrics.auto_start_settings_available
    auto_start_toggle_keeps_owner_draw_state =
        [bool]$metrics.auto_start_toggle_state_round_trips
    startup_toggle_rows_have_no_frame =
        [bool]$metrics.startup_toggles_are_borderless
    silent_auto_start_dependency_visible =
        [bool]$metrics.silent_auto_start_dependency_visible
    silent_start_explanation_uses_hover_tooltip =
        [bool]$metrics.silent_start_tooltip_available
    settings_show_current_version_and_update_button =
        [bool]$metrics.version_update_controls_available
    settings_show_project_storage_directory =
        [bool]$metrics.project_storage_settings_available
    settings_show_manual_blender_path =
        [bool]$metrics.manual_blender_path_settings_available
    latest_version_notice_uses_render_mesh_style =
        [bool]$metrics.latest_version_notice_uses_internal_dialog -and
        [bool]$metrics.latest_version_notice_closes_from_confirm
    silent_startup_only_applies_to_auto_start =
        [bool]$metrics.silent_startup_only_applies_to_auto_start
    auto_start_command_is_quoted_and_marked =
        [bool]$metrics.auto_start_command_is_quoted_and_marked
    dashboard_shows_matching_id =
        [bool]$metrics.dashboard_shows_matching_id
    dashboard_matching_id_is_editable_and_copyable =
        [bool]$metrics.dashboard_matching_id_editor_available
    dashboard_matching_id_replaces_atomically =
        [bool]$metrics.dashboard_matching_id_click_replaces_whole_value
    settings_no_longer_duplicates_matching_id =
        [bool]$metrics.settings_hides_matching_id_editor
    settings_controls_share_one_width =
        [bool]$metrics.settings_controls_share_width
    settings_footer_copy_is_removed =
        [bool]$metrics.settings_footer_copy_removed
    status_messages_use_one_bottom_surface =
        [bool]$metrics.global_status_bar_is_single_surface
    status_bar_is_full_width_and_aligned =
        [bool]$metrics.status_bar_aligns_with_content
    add_job_button_sits_above_status_bar =
        [bool]$metrics.add_job_clears_status_bar
    task_and_contribution_help_moved_to_status_bar =
        [bool]$metrics.page_help_moved_to_status_bar
    blend_drag_overlay_covers_task_list =
        [bool]$metrics.blend_drag_overlay_covers_task_list
    drag_overlay_only_accepts_blend_files =
        [bool]$metrics.drag_overlay_accepts_blend_only
    blend_loading_progress_blocks_other_operations =
        [bool]$metrics.workflow_progress_blocks_main_input
    blend_loading_progress_keeps_window_movable =
        [bool]$metrics.workflow_progress_allows_window_drag
    blend_loading_progress_offers_delayed_cancel =
        [bool]$metrics.workflow_progress_shows_cancel_after_timeout -and
        [bool]$metrics.workflow_progress_cancel_sets_stop_signal
    blender_catalog_overlay_stays_above_settings_action =
        [bool]$metrics.workflow_overlay_blocks_settings_action -and
        [bool]$metrics.workflow_background_controls_clip_siblings
    blender_catalog_overlay_defers_background_refresh =
        [bool]$metrics.workflow_refresh_is_deferred
    blender_version_selector_uses_internal_style =
        [bool]$metrics.blender_version_selector_uses_internal_style
    completed_jobs_hide_stop_action =
        [bool]$metrics.completed_job_hides_stop_action
    running_jobs_show_stop_action =
        [bool]$metrics.running_job_shows_stop_action
    failed_jobs_show_retry_all_action =
        [bool]$metrics.failed_job_shows_retry_all_action
    failed_frames_show_retry_current_action =
        [bool]$metrics.failed_frame_shows_retry_current_action
    stop_action_is_bottom_right_icon =
        [bool]$metrics.stop_action_is_bottom_right_icon
    static_labels_paint_their_theme_surface =
        [bool]$metrics.static_labels_paint_opaque_surface
    delete_confirmation_is_an_internal_child =
        [bool]$metrics.delete_dialog_is_internal
    delete_confirmation_defaults_to_cancel =
        [bool]$metrics.delete_dialog_defaults_to_cancel
    delete_confirmation_covers_window_after_resize =
        [bool]$metrics.delete_dialog_covers_resized_window
    delete_confirmation_supports_tab_and_escape =
        [bool]$metrics.delete_dialog_tab_moves_focus -and
        [bool]$metrics.delete_dialog_escape_closes
    enter_on_cancel_never_deletes =
        [bool]$metrics.delete_dialog_enter_honors_cancel_focus
    missing_asset_override_is_internal_and_explicit =
        [bool]$metrics.warning_override_is_internal -and
        [bool]$metrics.warning_override_is_explicit
    missing_asset_override_defaults_to_cancel =
        [bool]$metrics.warning_override_defaults_to_cancel
    missing_asset_override_supports_escape =
        [bool]$metrics.warning_override_escape_closes
    close_confirmation_uses_render_mesh_style =
        [bool]$metrics.close_dialog_is_internal
    close_confirmation_has_only_explicit_choices =
        [bool]$metrics.close_dialog_has_explicit_choices
    close_confirmation_defaults_to_tray =
        [bool]$metrics.close_dialog_defaults_to_tray
    close_confirmation_has_remember_checkbox =
        [bool]$metrics.close_dialog_remember_is_checkbox
    internal_dialog_blocks_background_list_refresh =
        [bool]$metrics.modal_dialog_blocks_list_refresh -and
        [bool]$metrics.modal_background_lists_clip_siblings
    list_blank_areas_follow_the_active_theme =
        [bool]$metrics.themed_list_blank_areas_match_surface
    scroll_anchor_preserved = [bool]$metrics.scroll_anchor_preserved
    selection_preserved = [bool]$metrics.selection_preserved
    multiple_selection_is_preserved = [bool]$metrics.multiple_selection_preserved
    jobs_support_multi_select = [bool]$metrics.jobs_support_multi_select
    contributions_support_multi_select = [bool]$metrics.contributions_support_multi_select
    main_lists_fit_without_horizontal_scroll =
        [bool]$metrics.main_lists_fit_without_horizontal_scroll
    list_headers_have_no_scroll_corner =
        [bool]$metrics.list_headers_have_no_scroll_corner
    native_list_scrollbars_are_hidden =
        [bool]$metrics.native_list_scrollbars_are_hidden
    native_overflow_scrollbar_is_hidden =
        [bool]$metrics.native_overflow_scrollbar_hidden
    custom_overflow_scrollbar_is_visible =
        [bool]$metrics.custom_overflow_scrollbar_visible
    mouse_wheel_scrolls_custom_list =
        [bool]$metrics.custom_wheel_scrolls_rows
    custom_scrollbar_drag_moves_rows =
        [bool]$metrics.custom_scrollbar_drag_scrolls_rows
    native_scrollbars_stay_hidden_after_input =
        [bool]$metrics.native_scrollbars_stay_hidden_after_input
    centered_owned_window_geometry =
        [bool]$metrics.centered_owned_window_geometry
    scheduling_policy_ui_is_removed =
        [bool]$metrics.scheduling_policy_ui_is_removed
    upload_select_all_and_invert_work =
        [bool]$metrics.upload_select_all_and_invert_work
    render_config_uses_the_full_content_width =
        [bool]$metrics.config_content_uses_full_width
    tray_icons_use_branded_status_badges =
        [bool]$metrics.tray_icons_use_branded_status_badges
    software_profile_drawer_lists_all_profiles =
        [bool]$metrics.software_profile_drawer_lists_all_profiles
    software_profile_selector_has_chevron =
        [bool]$metrics.software_profile_selector_has_chevron
    software_profile_drawer_follows_owner_window =
        [bool]$metrics.software_profile_drawer_follows_owner
    software_profile_editor_can_delete_profiles =
        [bool]$metrics.software_profile_editor_has_delete
    software_profile_deletion_preserves_a_valid_selection =
        [bool]$metrics.software_profile_deletion_is_persistable
    unchanged_detail_refresh_is_a_visual_noop =
        [bool]$metrics.unchanged_detail_refresh_is_visual_noop
    detail_preview_uses_right_column =
        [bool]$metrics.detail_preview_uses_right_column
    detail_list_has_no_unused_blank_column =
        [bool]$metrics.detail_list_has_no_unused_blank_column
    detail_summary_sits_below_preview =
        [bool]$metrics.detail_summary_sits_below_preview
    detail_explorer_actions_are_icon_buttons =
        [bool]$metrics.detail_explorer_actions_are_icon_buttons
    default_status_hides_idle_counter =
        [bool]$metrics.default_status_hides_idle_counter
    dashboard_status_does_not_flash_peer_ip =
        [bool]$metrics.dashboard_status_ignores_peer_polling_details
    dashboard_status_shows_handshake_upgrade =
        [bool]$metrics.dashboard_status_shows_handshake_upgrade
    focus_preserved = [bool]$metrics.focus_preserved
    horizontal_scroll_preserved = [bool]$metrics.horizontal_scroll_preserved
    horizontal_scroll_is_absent = [bool]$metrics.horizontal_scroll_absent
    top_pixel_offset_preserved = [bool]$metrics.top_pixel_offset_preserved
    control_order_matches_model = [bool]$metrics.control_order_matches_model
    idle_heartbeat_is_1hz = $metrics.idle_heartbeat_interval_ms -eq 1000
    burst_refresh_is_coalesced = $metrics.burst_refresh_min_interval_ms -ge 16
    burst_notifications_coalesce_to_one_refresh =
        $metrics.ui_test_burst_notifications -eq 100 -and
        $metrics.observed_burst_refreshes -eq 1
    idle_heartbeat_was_observed = $metrics.observed_heartbeat_refreshes -ge 1 -and
        $metrics.observed_heartbeat_refreshes -le 2
    share_control_fits = [bool]$metrics.share_text_fits
    resized_children_were_repainted = $metrics.resize_affected_children -gt 0 -and
        $metrics.resize_repainted_children -eq $metrics.resize_affected_children
    resize_layout_is_single_pass = $metrics.resize_duplicate_layout_count -eq 0
    custom_titlebar_is_draggable = $metrics.chrome_title_hit -eq 2
    maximize_keeps_snap_layout_hit_target = $metrics.chrome_maximize_hit -eq 9
    frameless_window_keeps_native_resize = $metrics.chrome_left_resize_hit -eq 10
    titlebar_icon_keeps_system_menu = $metrics.chrome_system_menu_hit -eq 3
    cancelled_caption_click_clears_pressed_state =
        [bool]$metrics.chrome_cancel_clears_pressed
    titlebar_is_compact = [bool]$metrics.compact_chrome_height
    titlebar_product_name_scales_with_dpi =
        [bool]$metrics.chrome_title_scales_with_dpi
    frame_log_idle_refresh_is_1hz = $metrics.frame_log_refresh_interval_ms -eq 1000
    native_window_capabilities_are_preserved =
        [bool]$metrics.chrome_native_capabilities_preserved
}
$failed = @($checks.GetEnumerator() | Where-Object { -not $_.Value })
if ($failed.Count -gt 0) {
    throw ('UI regression checks failed: ' + (($failed | ForEach-Object Key) -join ', '))
}

$fullResetCalls = Get-ChildItem -LiteralPath (Join-Path $nativeRoot 'src') -File |
    Select-String -SimpleMatch 'ListView_DeleteAllItems'
if ($fullResetCalls) {
    throw 'A full ListView reset call was reintroduced into the native UI.'
}
$legacyControls = Get-ChildItem -LiteralPath (Join-Path $nativeRoot 'src') -File |
    Select-String -Pattern 'BS_PUSHBUTTON|DarkMode_ItemsView'
if ($legacyControls) {
    throw 'A legacy default button or header theme was reintroduced into the native UI.'
}

$winMainSource = Get-Content -LiteralPath (Join-Path $nativeRoot 'src\WinMain.cpp') -Raw
$deleteCommand = [regex]::Match(
    $winMainSource,
    'case ID_JOB_DELETE:\s*\{(?<body>[\s\S]*?)\n\s*break;',
    [Text.RegularExpressions.RegexOptions]::CultureInvariant)
if (-not $deleteCommand.Success -or
    $deleteCommand.Groups['body'].Value -match 'MessageBoxW' -or
    $deleteCommand.Groups['body'].Value -notmatch 'ShowDeleteConfirmation') {
    throw 'Job deletion must use the in-app RenderMesh confirmation surface.'
}
$contextMenu = [regex]::Match(
    $winMainSource,
    'void ShowJobContextMenu\(POINT screenPoint\)\s*\{(?<body>[\s\S]*?)\n\}',
    [Text.RegularExpressions.RegexOptions]::CultureInvariant)
if (-not $contextMenu.Success -or
    $contextMenu.Groups['body'].Value -notmatch
        'TrackAndCloseContextMenu\(\s*gWindow, menu[\s\S]*DeferMenuAction' -or
    $contextMenu.Groups['body'].Value -match
        'DestroyMenu\(menu\);\s*if \(command\) ExecuteJobContextCommand') {
    throw 'Context-menu commands must run only after the popup menu has closed.'
}
$rawTrackedMenus = [regex]::Matches(
    $winMainSource,
    'TrackPopupMenuEx\s*\(',
    [Text.RegularExpressions.RegexOptions]::CultureInvariant)
if ($rawTrackedMenus.Count -ne 1 -or
    $winMainSource -notmatch 'UINT TrackAndCloseContextMenu\(') {
    throw 'Every popup menu must use the single close-and-compose barrier.'
}
$menuBarrier = [regex]::Match(
    $winMainSource,
    'UINT TrackAndCloseContextMenu\([\s\S]*?\n\}',
    [Text.RegularExpressions.RegexOptions]::CultureInvariant)
if (-not $menuBarrier.Success -or
    $menuBarrier.Value -notmatch 'TPM_NOANIMATION' -or
    $menuBarrier.Value -notmatch 'TPM_NONOTIFY' -or
    $menuBarrier.Value -notmatch 'HideThreadMenuWindows') {
    throw 'Tracked menus must disable DWM dismissal animation and hide residual menu windows.'
}
if ($winMainSource -notmatch 'constexpr UINT_PTR TIMER_MENU_ACTION' -or
    $winMainSource -notmatch 'void DeferMenuAction\(' -or
    ([regex]::Matches($winMainSource, 'DeferMenuAction\(').Count -lt 5)) {
    throw 'Every menu command must run after the native menu message cycle has drained.'
}
if ($winMainSource -notmatch 'constexpr UINT_PTR TIMER_CONTEXT_MENU_OPEN' -or
    $winMainSource -notmatch 'void QueueContextMenu\(' -or
    $winMainSource -notmatch
        'header->idFrom == ID_JOBS && header->code == NM_RCLICK[\s\S]*?QueueContextMenu\(ID_JOBS' -or
    $winMainSource -notmatch
        'wParam == TIMER_CONTEXT_MENU_OPEN[\s\S]*?ShowJobContextMenu') {
    throw 'List context menus must open only after the right-click notification cycle drains.'
}
if ($contextMenu.Groups['body'].Value -notmatch
        'job\.stage == JobStage::Stopped[\s\S]*?ID_JOB_RESUME' -or
    $winMainSource -notmatch
        'case ID_JOB_RESUME:[\s\S]*?ResumeStoppedJob') {
    throw 'Stopped render jobs must expose the resume-render command in their context menu.'
}
$backdropCapture = [regex]::Match(
    $winMainSource,
    'HBITMAP CaptureInternalDialogBackdrop\(\)\s*\{(?<body>[\s\S]*?)\n\}',
    [Text.RegularExpressions.RegexOptions]::CultureInvariant)
if (-not $backdropCapture.Success -or
    $backdropCapture.Groups['body'].Value -match 'GetDC\(nullptr\)|ClientToScreen|SRCCOPY' -or
    $backdropCapture.Groups['body'].Value -notmatch 'PrintWindow|WM_PRINT') {
    throw 'Internal dialogs must capture only their owner, never popup windows from the desktop.'
}
$previewUpdate = [regex]::Match(
    $winMainSource,
    'void UpdateFramePreview\(\)\s*\{(?<body>[\s\S]*?)\n\}',
    [Text.RegularExpressions.RegexOptions]::CultureInvariant)
$previewBody = if ($previewUpdate.Success) { $previewUpdate.Groups['body'].Value } else { '' }
if (-not $previewUpdate.Success -or
    ($previewBody -match 'selected < 0[\s\S]{0,400}?SetWindowTextW') -or
    ($previewBody -match 'gPreviewTaskId = frame\.taskId;\s*ReplacePreviewBitmap\(nullptr\)')) {
    throw 'Frame changes must retain the previous preview until the next bitmap is ready.'
}
if (($winMainSource -notmatch 'PageControl\(0, AddControl\(\s*L"EDIT"[\s\S]{0,180}?ID_CLUSTER_TOKEN') -or
    $winMainSource -notmatch 'config\.inviteCode = WideToUtf8\(Text\(Item\(ID_CLUSTER_TOKEN\)\)\)' -or
    $winMainSource -notmatch 'case ID_COPY_TOKEN:[\s\S]{0,160}?CopyText') {
    throw 'The dashboard matching ID must be editable, copyable, and persisted.'
}
if ($winMainSource -notmatch 'EM_SETCUEBANNER' -or
    $winMainSource -notmatch 'WM_SETFONT[\s\S]{0,100}?gMatchIdFont' -or
    $winMainSource -notmatch 'fieldIds\s*=\s*\{[\s\S]{0,160}?ID_CLUSTER_TOKEN') {
    throw 'The dashboard matching ID must look like an editable, large, focused text field.'
}
if ($winMainSource -notmatch 'RegisterDragDrop\(window, gMainDropTarget\)' -or
    $winMainSource -notmatch 'case WM_RM_BLEND_DROP:' -or
    $winMainSource -notmatch '拖放 \.blend 文件到窗口内') {
    throw 'The render-task page is missing its live .blend drag overlay.'
}
if ($winMainSource -notmatch 'ID_JOB_BULK_START_FULL' -or
    $winMainSource -notmatch 'ID_JOB_BULK_DELETE' -or
    $winMainSource -notmatch 'ID_CONTRIBUTION_CLEAR_SELECTED' -or
    $winMainSource -notmatch 'DeleteContributionHistory') {
    throw 'Bulk task and contribution-history commands are missing.'
}
if ($winMainSource -notmatch 'ID_CONFIG_PROFILE_SELECT' -or
    $winMainSource -notmatch 'ID_CONFIG_PROFILE_DRAWER' -or
    $winMainSource -notmatch 'ID_CONFIG_PROFILE_NAME' -or
    $winMainSource -notmatch 'ID_CONFIG_PROFILE_SYSTEM' -or
    $winMainSource -notmatch 'ID_CONFIG_PROFILE_SOFTWARE' -or
    $winMainSource -notmatch 'ID_CONFIG_PROFILE_PATH') {
    throw 'The render-parameter window is missing software configuration fields.'
}
if ($winMainSource -notmatch 'ID_AUTO_START' -or
    $winMainSource -notmatch 'ID_SILENT_AUTO_START' -or
    $winMainSource -notmatch 'kSettingToggleCheckedProperty' -or
    $winMainSource -match 'const COLORREF cardFill' -or
    $winMainSource -notmatch 'RegSetValueExW' -or
    $winMainSource -notmatch 'RegDeleteValueW' -or
    $winMainSource -notmatch 'L"--startup"' -or
    $winMainSource -notmatch
        '!headless\s*&&\s*gUiMetricsReport\.empty\(\)[\s\S]{0,180}?SyncAutoStartRegistration') {
    throw 'Settings must expose per-user auto-start and startup-only silent mode.'
}
$hasStartupUpdateTimer = $winMainSource.Contains("SetTimer(window, TIMER_UPDATE_CHECK, 4000")
$hasRepeatingUpdateTimer = $winMainSource.Contains("SetTimer(window, TIMER_UPDATE_CHECK, 30000")
if ($winMainSource -notmatch 'ID_SILENT_AUTO_START_INFO' -or
    $winMainSource -notmatch 'ID_CURRENT_VERSION' -or
    $winMainSource -notmatch 'ID_CHECK_UPDATE' -or
    -not $hasStartupUpdateTimer -or $hasRepeatingUpdateTimer) {
    throw 'Settings need a tooltip, current-version update action, and one startup-only update check.'
}
if ($winMainSource -notmatch 'AcquireSingleInstance\(dataDir' -or
    $winMainSource -notmatch 'kSingleInstanceKeyProperty' -or
    $winMainSource -notmatch 'message\s*==\s*gRestoreInstanceMessage' -or
    $winMainSource -match 'RenderMesh-SelfTest-[\s\S]{0,400}?RegSetValueExW') {
    throw 'Silent auto-start must be single-instance and self-tests must not modify the real Run key.'
}
$listSubclass = [regex]::Match(
    $winMainSource,
    'LRESULT CALLBACK ListViewSubclassProc\([\s\S]*?\n\}',
    [Text.RegularExpressions.RegexOptions]::CultureInvariant)
if (-not $listSubclass.Success -or
    $listSubclass.Value -match
        'case WM_PAINT:[\s\S]{0,240}?HideNativeListScrollbars' -or
    $listSubclass.Value -match
        'case WM_ERASEBKGND:[\s\S]{0,180}?return TRUE' -or
    $listSubclass.Value -notmatch
        'case WM_MOUSEWHEEL:[\s\S]{0,800}?GET_WHEEL_DELTA_WPARAM[\s\S]{0,800}?ScrollListToTopIndex') {
    throw 'List surfaces must erase opaquely, and mouse-wheel input must drive the custom row scroller.'
}
$configureList = [regex]::Match(
    $winMainSource,
    'void ConfigureList\([\s\S]*?\n\}',
    [Text.RegularExpressions.RegexOptions]::CultureInvariant)
if (-not $configureList.Success -or
    $configureList.Value -notmatch 'WS_CLIPSIBLINGS') {
    throw 'List views must clip sibling windows so a repaint cannot cover the internal modal layer.'
}
$hideNativeScrollbars = [regex]::Match(
    $winMainSource,
    'void HideNativeListScrollbars\([^;]*?\)\s*\{[\s\S]*?\n\}',
    [Text.RegularExpressions.RegexOptions]::CultureInvariant)
$fitListColumns = [regex]::Match(
    $winMainSource,
    'void FitListColumns\([\s\S]*?\n\}',
    [Text.RegularExpressions.RegexOptions]::CultureInvariant)
if (-not $hideNativeScrollbars.Success -or
    $hideNativeScrollbars.Value -notmatch 'WS_VSCROLL' -or
    $hideNativeScrollbars.Value -notmatch 'WS_HSCROLL' -or
    -not $fitListColumns.Success -or
    $fitListColumns.Value -notmatch 'HideNativeListScrollbars') {
    throw 'Native list scrollbars must be removed after list layout so they cannot replace the custom scrollbar.'
}
$syncTable = [regex]::Match(
    $winMainSource,
    'bool SyncTable\([\s\S]*?\n\}',
    [Text.RegularExpressions.RegexOptions]::CultureInvariant)
if (-not $syncTable.Success -or
    $syncTable.Value -match 'RDW_UPDATENOW' -or
    $syncTable.Value -match
        'operations\.empty\(\)[\s\S]{0,180}?InvalidateCustomListScrollbar') {
    throw 'Incremental table synchronization must not force background erasure or synchronous repaint.'
}
$applyTheme = [regex]::Match(
    $winMainSource,
    'void ApplyTheme\(\)[\s\S]*?\n\}',
    [Text.RegularExpressions.RegexOptions]::CultureInvariant)
if (-not $applyTheme.Success -or $applyTheme.Value -match 'RDW_ERASE') {
    throw 'Theme/page repaint must retain the existing composed frame instead of exposing a white erase pass.'
}
$configLayout = [regex]::Match(
    $winMainSource,
    'void LayoutConfigWindow\([\s\S]*?\n\}',
    [Text.RegularExpressions.RegexOptions]::CultureInvariant)
if (-not $configLayout.Success -or
    $configLayout.Value -match 'DpiValue\(') {
    throw 'Render-parameter controls already use physical client coordinates; DPI scaling them again leaves an oversized right gutter.'
}
if ($winMainSource -notmatch 'CreateTooltip' -or
    $winMainSource -notmatch 'ID_DETAIL_OPEN_OUTPUT' -or
    $winMainSource -notmatch 'ID_DETAIL_OPEN_SOURCE' -or
    $winMainSource -notmatch 'ID_DETAIL_FULL_RENDER' -or
    $winMainSource -notmatch 'ExecuteJobContextCommand\(ID_JOB_START_FULL') {
    throw 'Detail explorer actions must be compact icon buttons with tooltips.'
}
$preflightHandler = [regex]::Match(
    $winMainSource,
    'case WM_RM_PREFLIGHT:(?<body>[\s\S]*?)case WM_RM_JOB_OPERATION:',
    [Text.RegularExpressions.RegexOptions]::CultureInvariant)
if (-not $preflightHandler.Success -or
    $preflightHandler.Groups['body'].Value -match 'MessageBoxW' -or
    $preflightHandler.Groups['body'].Value -notmatch 'ShowInternalNotice') {
    throw 'Preflight failures must use the in-app RenderMesh notice surface.'
}
if ($winMainSource -notmatch 'PreflightWarningOverride' -or
    $winMainSource -notmatch 'L"无视警告"' -or
    $winMainSource -notmatch '跳过全部预检警告' -or
    $winMainSource -notmatch 'worker-only test-frame confirmation' -or
    $winMainSource -notmatch 'canIgnoreAssetWarnings' -or
    $winMainSource -notmatch 'StartImport\(Text\(Item\(ID_SOURCE_PATH\)\), true\)') {
    throw 'Ignorable missing-asset warnings need an explicit safe override flow.'
}
if ($winMainSource -match 'L"RenderMesh · Blender 分布式渲染农场"') {
    throw 'The main window title must only display RenderMesh.'
}
if ($winMainSource -match 'TIMER_PREVIEW_RESIZE' -or
    $winMainSource -notmatch 'ID_DETAIL_PREVIEW\) DrawPreviewControl') {
    throw 'Detail preview resizing must reuse the decoded bitmap without blocking regeneration.'
}
foreach ($requiredRetrySurface in @(
        'ID_JOB_RETRY_FAILED',
        'ID_DETAIL_RETRY_FAILED',
        'ID_DETAIL_RETRY_FRAME',
        'ID_LOG_RETRY')) {
    if ($winMainSource -notmatch [regex]::Escape($requiredRetrySurface)) {
        throw "Missing retry surface: $requiredRetrySurface"
    }
}
if ($winMainSource -notmatch
        'DetailPrimaryActionFor\(job\)' -or
    $winMainSource -notmatch
        'SetControlVisibleIfChanged\(GetDlgItem\(gDetailWindow, ID_DETAIL_STOP\),[\s\S]*?DetailPrimaryAction::Stop' -or
    $winMainSource -notmatch
        'SetControlVisibleIfChanged\(GetDlgItem\(gDetailWindow, ID_DETAIL_FULL_RENDER\),[\s\S]*?DetailPrimaryAction::FullRender' -or
    $winMainSource -notmatch
        'SetControlVisibleIfChanged\(GetDlgItem\(gDetailWindow, ID_DETAIL_RETRY_FAILED\),[\s\S]*?HasFailedFrames') {
    throw 'Detail full-render, stop, and retry actions must follow live job state.'
}
if ($winMainSource -notmatch
        'if \(visibilityChanged\) LayoutDetailWindow\(gDetailWindow\);') {
    throw 'Unchanged detail refreshes must not relayout and repaint the whole window.'
}
[pscustomobject]@{
    Result = 'UI_REGRESSION_OK'
    JobsVisibleRows = $metrics.jobs_visible_rows
    StableSnapshotMutations = $metrics.stable_snapshot_mutation_delta
    IncrementalMutationDelta = $metrics.incremental_mutation_delta
    ScrollAnchorPreserved = $metrics.scroll_anchor_preserved
    SelectionPreserved = $metrics.selection_preserved
    FullResetCalls = 0
    LegacyControlStyles = 0
}
