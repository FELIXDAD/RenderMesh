# RenderMesh UI design QA

## Comparison target

- Latest user references:
  - `C:\Users\king\AppData\Local\Temp\codex-clipboard-eecd6ee3-3341-4a9c-8c11-803a8c90ab09.png`
  - `C:\Users\king\AppData\Local\Temp\codex-clipboard-4ef62b3b-2732-4ff8-8fec-c35c2a9b6b6d.png`
  - `C:\Users\king\AppData\Local\Temp\codex-clipboard-a6f3ce25-1630-4dc6-bb4e-acc6b7b3c777.png`
  - `C:\Users\king\AppData\Local\Temp\codex-clipboard-c36285e1-06dd-4751-b042-6baa4c83fdb7.png`
  - `C:\Users\king\AppData\Local\Temp\codex-clipboard-edbc5d81-58b3-47d2-b404-b18c9a6ce4a9.png`
  - `C:\Users\king\AppData\Local\Temp\codex-clipboard-2a3e3e53-1952-401e-a687-e4db6463ea4f.png`
  - `C:\Users\king\AppData\Local\Temp\codex-clipboard-7002e448-b2a5-4761-9293-2b222b81fbcc.png`
  - `C:\Users\king\AppData\Local\Temp\codex-clipboard-59a110e5-5ef5-46b1-a276-84b1c0219518.png`
  - `C:\Users\king\AppData\Local\Temp\codex-clipboard-f51dce7d-d870-423a-b788-f8874f917e1f.png`
  - `C:\Users\king\AppData\Local\Temp\codex-clipboard-8e4709ff-1682-42a8-9cf2-dd67b3c8dee9.png`
- Viewport checked through Windows Graphics Capture: 1120 × 720 logical pixels.
- States checked: task queue, settings, dark theme, light theme, theme transition, network entry, and shared bottom status bar.

## Full-view comparison evidence

- The main title now reads only `RenderMesh`; the former descriptive suffix is removed without changing the native drag, resize, minimize, maximize, or close behavior.
- The top-left network control uses the Segoe Fluent Icons globe glyph plus the existing toggle. Its accessible name remains `接入网络`, while the visible text-only treatment is gone.
- Task, server, and contribution lists reserve one consistent 66 px footer area for the shared status bar and otherwise fill the available content height.
- Settings fields now share the same left edge and width. The port/log explanation and auto-save explanation are removed from the settings canvas.
- Dark and light captures both keep a coherent background, input, button, list, and status-surface palette; no stale black backing rectangles or white ListView blocks were observed.

## Interaction and workflow evidence

- Job context-menu commands are posted only after `DestroyMenu`, so opening details or deletion cannot leave the popup menu painted over the next surface.
- Preflight and inspection failures use the in-app RenderMesh notice surface instead of a native MessageBox.
- Preflight, connection, render, and auto-save feedback all route through one bottom status bar.
- A prepared job with no eligible remote participants enters `ReadyToConfigure`. Test and full rendering can fall back to the local worker, while available remote participants remain preferred.
- The settings controls continue to auto-save with a 300 ms debounce; the result appears in the bottom status bar.
- The task-page status bar now spans the exact full width of the task list instead of reserving a cut-out on the right.
- The add-job button is positioned above the status bar with a fixed 12 px clearance, so it no longer collides with or interrupts the footer surface.
- The render-parameter window now puts the reusable software profile above scene parameters. The selected configuration, name, system, software, installation path, browse action and save action share one compact hierarchy; the last selection persists.
- Task and contribution lists use native extended multi-selection. Right-clicking within an existing selection keeps the whole selection; right-clicking an unselected row starts a new selection before showing batch actions.
- The software-profile selector now presents a persistent downward chevron and opens a dark, two-line drawer. Every saved profile remains visible, the active row has a green check, and clicking a row switches then closes the drawer.
- The job-detail preview consumes the full right column. The two Explorer actions are 48 px icon buttons at the lower-right with their full accessible names retained for hover tooltips.
- A completed job-detail window captured before and after an idle 1.6-second heartbeat produced byte-identical screenshots, confirming that unchanged data does not trigger a visible repaint.
- The bottom status bar no longer exposes the idle-seconds counter. Its normal state is simply `已就绪`; pairing and missing-Blender states still replace it with actionable status text.

## Automated evidence

- Release self-test, invoked headlessly with `--no-dialog` twice consecutively: both exit code 0 and `SELF_TEST_OK`.
- The self-test includes a local-only prepared-job flow and verifies all three test frames complete on the submitting node.
- Release UI regression: `UI_REGRESSION_OK`; 19 visible task rows, zero full resets, stable scroll/selection/focus, single-pass resize layout, and 1 Hz idle refresh.
- Release build: `native/build/Release-final/RenderMesh.exe`, 2,931,712 bytes.
- Final layout build: `native/build/Release-v2/RenderMesh.exe`, 2,932,736 bytes; `UI_REGRESSION_OK` with explicit full-width status-bar and add-button clearance checks.
- Reference and implementation settings screenshots were emitted together for direct visual comparison.
- Software-profile persistence and selected contribution-history deletion are covered by headless self-tests; multi-selection preservation is covered by the UI metrics report.
- Final software-profile and batch-operation build: `native/build/Release-v4/RenderMesh.exe`, 2,994,688 bytes; self-test, UI model tests, add-button dialog test, and UI regression all passed.
- Render-parameter visual verification: `native/build/profile-window-qa.png`; the software-profile section and scene parameters are aligned, unclipped, and use the established dark control styling.
- Final dropdown/detail build: `native/build/Release-v12/RenderMesh.exe`, 3,001,344 bytes; UI regression passed including profile enumeration, chevron affordance, detail no-op refresh, expanded preview, icon actions, and idle-counter removal.
- Final visual evidence: `native/build/config-drawer-v12-0.jpg`, `native/build/detail-window-qa-v12.jpg`, and the combined reference/implementation comparison `native/build/ui-qa-config-detail-v12.jpg`.

## Resolution checklist

- [x] Close context menus before executing details or deletion.
- [x] Use only `RenderMesh` in the main title.
- [x] Replace preflight failure MessageBox with an internal notice.
- [x] Permit local-only submission and local test rendering.
- [x] Consolidate status feedback into one bottom status bar.
- [x] Align settings controls to one width and remove footer copy.
- [x] Replace the text-only network entry with a Fluent icon and toggle.
- [x] Verify dark and light states visually.
- [x] Align the bottom status bar edge-to-edge with the task list and move the add-job button above it.
- [x] Pass Release self-test and UI regression.
- [x] Add reusable Blender software profiles to the render-parameter window and remember the last selection.
- [x] Support task multi-select with batch full-speed render and batch delete.
- [x] Support contribution multi-select with selected-history cleanup only.
- [x] Show every software profile in an obvious dark dropdown drawer with selected-state guidance.
- [x] Expand the detail preview and replace verbose Explorer buttons with tooltip-backed icon actions.
- [x] Keep the unchanged detail window visually stable across idle heartbeats.
- [x] Replace the idle-seconds status copy with a stable `已就绪` default.

final result: passed
