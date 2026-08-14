# RenderMesh v0.1 product specification

Source: user requirements in the originating Codex task.

1. Build an internal distributed render farm for many Windows office computers.
2. Every computer running the app can submit render work and use other opted-in computers' compute.
3. Compute sharing has an explicit on/off setting and defaults off.
4. Shared rendering is accepted only after a configurable no-input interval, default 3 minutes; active computers reject all remote rendering work.
5. If keyboard/mouse activity resumes during a shared package, finish the current frame, then stop and requeue the remaining frames. Do not apply CPU percentage limits.
6. Provide a server list showing online computers and assigned render counts.
7. Initially support Blender cluster rendering.
8. Deliver a lightweight C++ or C# Windows desktop EXE; recipients should only need to unzip and run it, with no runtime/dependency installation.
9. Check whether Blender is installed before use.
10. Before distributing a project, inspect required plugins and linked/relative-path assets so a missing plugin or asset cannot be treated as a render-compatible worker/project.
