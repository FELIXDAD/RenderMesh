#include "Core.hpp"

#include <bcrypt.h>
#include <dxgi1_2.h>
#include <shlobj.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <cwctype>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <set>
#include <sstream>

namespace rm {

namespace {

std::mutex gLogMutex;
fs::path gLogDirectory;
fs::path gLogPath;

std::string TrimAscii(std::string value) {
    auto whitespace = [](unsigned char ch) { return std::isspace(ch) != 0; };
    value.erase(value.begin(), std::find_if_not(value.begin(), value.end(), whitespace));
    value.erase(std::find_if_not(value.rbegin(), value.rend(), whitespace).base(), value.end());
    return value;
}

std::string LogDayStamp() {
    SYSTEMTIME time{};
    GetLocalTime(&time);
    char value[16]{};
    std::snprintf(value, sizeof(value), "%04u-%02u-%02u",
                  time.wYear, time.wMonth, time.wDay);
    return value;
}

std::string LogTimestamp() {
    SYSTEMTIME time{};
    GetLocalTime(&time);
    char value[32]{};
    std::snprintf(value, sizeof(value), "%04u-%02u-%02u %02u:%02u:%02u.%03u",
                  time.wYear, time.wMonth, time.wDay,
                  time.wHour, time.wMinute, time.wSecond, time.wMilliseconds);
    return value;
}

void WriteLogLine(const char* level, const std::string& message) {
    std::scoped_lock lock(gLogMutex);
    if (gLogDirectory.empty()) return;
    fs::path expected = gLogDirectory / Utf8ToWide("RenderMesh-" + LogDayStamp() + ".log");
    if (expected != gLogPath) gLogPath = std::move(expected);
    std::string clean = message;
    std::replace(clean.begin(), clean.end(), '\r', ' ');
    std::replace(clean.begin(), clean.end(), '\n', ' ');
    std::ofstream output(gLogPath, std::ios::binary | std::ios::app);
    if (output)
        output << LogTimestamp() << " [" << level << "] " << clean << "\r\n";
}

std::string InviteCodeFromToken(const std::string& token) {
    std::string prefix = Sha256Prefix(token, 8);
    unsigned long value = 0;
    try {
        value = std::stoul(prefix, nullptr, 16);
    } catch (...) {
        return NewInviteCode();
    }
    char code[7]{};
    std::snprintf(code, sizeof(code), "%06lu", value % 1000000ul);
    return code;
}

std::wstring QuoteArg(const std::wstring& value) {
    if (value.find_first_of(L" \t\"") == std::wstring::npos) return value;
    std::wstring out = L"\"";
    size_t slashes = 0;
    for (wchar_t ch : value) {
        if (ch == L'\\') {
            ++slashes;
        } else if (ch == L'\"') {
            out.append(slashes * 2 + 1, L'\\');
            out.push_back(ch);
            slashes = 0;
        } else {
            out.append(slashes, L'\\');
            slashes = 0;
            out.push_back(ch);
        }
    }
    out.append(slashes * 2, L'\\');
    out.push_back(L'\"');
    return out;
}

struct ProcessResult {
    bool started = false;
    DWORD exitCode = static_cast<DWORD>(-1);
    std::wstring error;
};

ProcessResult RunProcess(
    const fs::path& executable,
    const std::vector<std::wstring>& arguments,
    const fs::path& logPath,
    const Config* renderPolicy = nullptr,
    std::atomic<bool>* stopRequested = nullptr) {
    ProcessResult result;
    fs::create_directories(logPath.parent_path());
    HANDLE log = CreateFileW(
        logPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (log == INVALID_HANDLE_VALUE) {
        result.error = L"无法创建日志：" + LastErrorMessage();
        return result;
    }

    std::wstring command = QuoteArg(executable.wstring());
    for (const auto& arg : arguments) {
        command.push_back(L' ');
        command += QuoteArg(arg);
    }
    std::vector<wchar_t> buffer(command.begin(), command.end());
    buffer.push_back(L'\0');

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_HIDE;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    startup.hStdOutput = log;
    startup.hStdError = log;
    PROCESS_INFORMATION process{};
    DWORD flags = CREATE_NO_WINDOW;
    // Windows 10/11 support nested jobs. Keeping the renderer in the parent's
    // job avoids ERROR_ACCESS_DENIED under managed desktop/agent environments,
    // while the child job below still enforces the Blender process-tree cap.
    if (renderPolicy) flags |= CREATE_SUSPENDED;
    BOOL created = CreateProcessW(
        executable.c_str(), buffer.data(), nullptr, nullptr, TRUE, flags, nullptr,
        logPath.parent_path().c_str(), &startup, &process);
    CloseHandle(log);
    if (!created) {
        result.error = L"无法启动进程：" + LastErrorMessage();
        return result;
    }
    result.started = true;

    WindowsJobLimiter limiter;
    int lastPercent = -1;
    if (renderPolicy) {
        std::wstring policyError;
        if (!limiter.Assign(process.hProcess, policyError)) {
            TerminateProcess(process.hProcess, 90);
            result.error = L"无法限制 Blender 进程：" + policyError;
        } else {
            ResumeThread(process.hThread);
        }
    }

    if (result.error.empty()) {
        for (;;) {
            if (renderPolicy) {
                int wanted = IdleSeconds() >= renderPolicy->idleAfterSeconds
                    ? 100
                    : renderPolicy->activeCpuPercent;
                if (wanted != lastPercent) {
                    std::wstring policyError;
                    if (!limiter.SetCpuPercent(wanted, policyError)) {
                        result.error = policyError;
                        TerminateProcess(process.hProcess, 91);
                        break;
                    }
                    lastPercent = wanted;
                }
            }
            if (stopRequested && stopRequested->load()) {
                TerminateProcess(process.hProcess, 92);
                result.error = L"任务因应用退出而中止";
                break;
            }
            DWORD wait = WaitForSingleObject(
                process.hProcess, (renderPolicy || stopRequested) ? 500 : INFINITE);
            if (wait == WAIT_OBJECT_0) break;
            if (wait == WAIT_FAILED) {
                result.error = LastErrorMessage();
                break;
            }
        }
    }
    GetExitCodeProcess(process.hProcess, &result.exitCode);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return result;
}

std::string ReadText(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(input), {});
}

bool WriteTextAtomic(const fs::path& path, const std::string& text, std::wstring& error) {
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    fs::path temporary = path;
    temporary += L".tmp";
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output) {
            error = L"无法写入配置临时文件";
            return false;
        }
        output.write(text.data(), static_cast<std::streamsize>(text.size()));
        output.flush();
        if (!output) {
            error = L"配置写入不完整";
            return false;
        }
    }
    if (!MoveFileExW(
            temporary.c_str(), path.c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        error = L"无法原子替换配置：" + LastErrorMessage();
        DeleteFileW(temporary.c_str());
        return false;
    }
    return true;
}

const char* kPreflightScript = R"PY(import bpy, addon_utils, json, os, sys, glob, re, traceback

args = sys.argv[sys.argv.index('--') + 1:]
prepared, report_path = args[0], args[1]
validation_start, validation_end, validation_step = map(int, args[2:5])
warnings = []

def path_key(path):
    return os.path.normcase(os.path.normpath(bpy.path.abspath(path)))

runtime_roots = []
for resource_kind in ('SYSTEM', 'LOCAL'):
    try:
        value = bpy.utils.resource_path(resource_kind)
        if value:
            runtime_roots.append(path_key(value))
    except Exception:
        pass

def is_runtime_resource(path):
    key = path_key(path)
    for root in runtime_roots:
        try:
            if os.path.commonpath([key, root]) == root:
                return True
        except ValueError:
            pass
    # A .blend can remember the absolute install path of Blender's Essentials
    # asset library. Another worker with the same major.minor version may have
    # Blender installed elsewhere, so accept it only when the same path suffix
    # exists in this runtime's own datafiles/assets tree.
    marker = os.sep + 'datafiles' + os.sep + 'assets' + os.sep
    marker_index = key.lower().find(marker.lower())
    if marker_index >= 0:
        suffix = key[marker_index + len(marker):]
        for root in runtime_roots:
            equivalent = os.path.join(root, 'datafiles', 'assets', suffix)
            if os.path.isfile(equivalent):
                return True
    return False

def expected_paths(path, item=None):
    if not path:
        return []
    path = bpy.path.abspath(path)
    if '<UDIM>' in path:
        if item is not None:
            expected = sorted({getattr(tile, 'number', None)
                               for tile in getattr(item, 'tiles', [])})
            expected = [tile for tile in expected if tile]
            if expected:
                return [path.replace('<UDIM>', str(tile)) for tile in expected]
        tiles = sorted(glob.glob(path.replace('<UDIM>', '*')))
        return tiles or [path]
    if '####' in path:
        duration = max(1, int(getattr(item, 'frame_duration', 1)))
        return [path.replace('####', f'{frame:04d}') for frame in range(1, duration + 1)]
    if item is not None and getattr(item, 'source', '') == 'SEQUENCE':
        match = re.search(r'(\d+)(?=\.[^.]+$)', path)
        if match:
            width = len(match.group(1))
            start = int(match.group(1))
            expression = re.compile(
                '^' + re.escape(path[:match.start(1)]) + r'(\d{' + str(width) + '})' +
                re.escape(path[match.end(1):]) + '$', re.IGNORECASE)
            discovered = []
            for candidate in glob.glob(path[:match.start(1)] + '*' + path[match.end(1):]):
                found = expression.match(candidate)
                if found:
                    discovered.append(int(found.group(1)))
            for packed in list(getattr(item, 'packed_files', []) or []):
                packed_path = bpy.path.abspath(getattr(packed, 'filepath', '') or '')
                found = expression.match(packed_path)
                if found:
                    discovered.append(int(found.group(1)))
            duration = max(1, int(getattr(item, 'frame_duration', 1)))
            if discovered:
                duration = max(duration, max(discovered) - start + 1)
            return [path[:match.start(1)] + f'{start + index:0{width}d}' + path[match.end(1):]
                    for index in range(duration)]
    return [path]

def packed_path_keys(item, candidates):
    keys = set()
    packed_files = list(getattr(item, 'packed_files', []) or [])
    for packed in packed_files:
        filepath = getattr(packed, 'filepath', '') or ''
        if filepath:
            keys.add(path_key(filepath))
        tile_number = int(getattr(packed, 'tile_number', 0) or 0)
        if tile_number:
            for candidate in candidates:
                if str(tile_number) in os.path.basename(candidate):
                    keys.add(path_key(candidate))
    # packed_file represents the sole source for ordinary, non-sequence images.
    if getattr(item, 'packed_file', None) and len(candidates) == 1:
        keys.add(path_key(candidates[0]))
    return keys

def dependency_state(path, item=None):
    candidates = expected_paths(path, item)
    packed = packed_path_keys(item, candidates) if item is not None else set()
    missing = [candidate for candidate in candidates
               if path_key(candidate) not in packed and not os.path.exists(candidate)]
    unpacked = [candidate for candidate in candidates
                if path_key(candidate) not in packed and os.path.exists(candidate)]
    return candidates, missing, unpacked

def gather():
    rows = []
    groups = [
        ('image', bpy.data.images), ('font', bpy.data.fonts),
        ('sound', bpy.data.sounds), ('movie', bpy.data.movieclips),
        ('library', bpy.data.libraries), ('cache', bpy.data.cache_files),
        ('volume', getattr(bpy.data, 'volumes', [])),
    ]
    for kind, values in groups:
        for item in values:
            path = getattr(item, 'filepath', '') or ''
            if not path or path in {'<builtin>', '<generated>'}:
                continue
            candidates, missing, unpacked = dependency_state(path, item)
            packed = bool(candidates) and not missing and not unpacked
            rows.append({
                'kind': kind, 'owner': getattr(item, 'name', ''),
                'path': bpy.path.abspath(path), 'packed': packed,
                'exists': not missing, 'missing_paths': missing,
                'unpacked_paths': unpacked,
            })
    known = {os.path.normcase(os.path.normpath(x['path'])) for x in rows}
    try:
        # Blender's own path inventory catches external references that do not
        # live in one of the common datablock collections above. packed=True
        # means already-packed paths are skipped.
        for path in bpy.utils.blend_paths(absolute=True, packed=True, local=False):
            # Files shipped inside this exact Blender runtime are guaranteed by
            # the already-enforced major.minor version match and are not project
            # assets (for example Essentials brush libraries).
            if is_runtime_resource(path):
                continue
            key = os.path.normcase(os.path.normpath(path))
            if key not in known:
                _, missing, unpacked = dependency_state(path)
                rows.append({'kind': 'external', 'owner': '', 'path': path,
                             'packed': False, 'exists': not missing,
                             'missing_paths': missing,
                             'unpacked_paths': unpacked})
                known.add(key)
    except Exception as exc:
        warnings.append(f'blend_paths: {exc}')
    return rows

engine = bpy.context.scene.render.engine
cycles_device = getattr(bpy.context.scene.cycles, 'device', 'CPU') if engine == 'CYCLES' else ''
addon_versions = {}
for module in addon_utils.modules(refresh=True):
    version = getattr(module, 'bl_info', {}).get('version', ())
    addon_versions[module.__name__] = '.'.join(str(part) for part in version) or 'unknown'
enabled_addons = []
for addon in bpy.context.preferences.addons:
    name = addon.module
    enabled_addons.append(name + '@' + addon_versions.get(name, 'unknown'))

uses_optix_denoiser = (engine == 'CYCLES' and
                       bool(getattr(bpy.context.scene.cycles, 'use_denoising', False)) and
                       getattr(bpy.context.scene.cycles, 'denoiser', '') == 'OPTIX')
uses_compositor = getattr(bpy.context.scene, 'compositing_node_group', None) is not None
original_scene = bpy.context.scene
primary_outputs = {str(frame): bpy.path.abspath(original_scene.render.frame_path(frame=frame))
                   for frame in range(validation_start, validation_end + 1, validation_step)}
compositor_outputs = []
compositor_tree = getattr(original_scene, 'compositing_node_group', None)
if compositor_tree:
    output_nodes = [node for node in compositor_tree.nodes if node.type == 'OUTPUT_FILE']
    for index, node in enumerate(output_nodes):
        base_directory = (getattr(node, 'base_path', None) or
                          getattr(node, 'directory', None) or '//')
        compositor_outputs.append({
            'id': f'output-{index}', 'name': node.name,
            'directory': bpy.path.abspath(base_directory),
        })
report = {'pass': False, 'version': f'{bpy.app.version[0]}.{bpy.app.version[1]}',
          'render_engine': engine,
          'render_format': bpy.context.scene.render.image_settings.file_format,
          'file_extension': bpy.context.scene.render.file_extension,
          'use_file_extension': bpy.context.scene.render.use_file_extension,
          'cpu_safe_while_active': (engine == 'CYCLES' and cycles_device == 'CPU' and
                                    not uses_optix_denoiser and not uses_compositor),
          'requires_gpu': engine != 'CYCLES' or cycles_device == 'GPU',
          'addons': sorted(enabled_addons),
          'primary_outputs': primary_outputs,
          'compositor_outputs': compositor_outputs,
          'missing': [], 'unpacked': [], 'warnings': warnings,
          'validation_render': False, 'validation_frames': 0, 'validation_error': ''}
try:
    before = gather()
    report['missing'] = [dict(x, path=missing) for x in before
                         for missing in x.get('missing_paths', [])]
    if not report['missing']:
        try:
            bpy.ops.file.pack_libraries()
        except Exception as exc:
            warnings.append(f'pack_libraries: {exc}')
        try:
            bpy.ops.file.pack_all()
        except Exception as exc:
            warnings.append(f'pack_all: {exc}')
        bpy.ops.wm.save_as_mainfile(filepath=prepared, copy=True)
        after = gather()
        report['missing'] = [dict(x, path=missing) for x in after
                             for missing in x.get('missing_paths', [])]
        report['unpacked'] = [dict(x, path=unpacked) for x in after
                              for unpacked in x.get('unpacked_paths', [])]
        assets_ok = not report['missing'] and not report['unpacked'] and os.path.isfile(prepared)
        if assets_ok:
            undefined_nodes = []
            node_trees = list(bpy.data.node_groups)
            node_trees.extend(material.node_tree for material in bpy.data.materials
                              if getattr(material, 'node_tree', None))
            node_trees.extend(world.node_tree for world in bpy.data.worlds
                              if getattr(world, 'node_tree', None))
            node_trees.extend(light.node_tree for light in bpy.data.lights
                              if getattr(light, 'node_tree', None))
            node_trees.extend(scene.compositing_node_group for scene in bpy.data.scenes
                              if getattr(scene, 'compositing_node_group', None))
            seen_trees = set()
            for tree in node_trees:
                if tree.as_pointer() in seen_trees:
                    continue
                seen_trees.add(tree.as_pointer())
                for node in tree.nodes:
                    if getattr(node, 'bl_idname', '') == 'NodeUndefined' or node.type == 'UNDEFINED':
                        undefined_nodes.append(f'{tree.name}/{node.name}')
            if undefined_nodes:
                raise RuntimeError('Undefined nodes, likely missing add-on: ' +
                                   ', '.join(undefined_nodes[:12]))
            scene = bpy.context.scene
            scene.render.resolution_x = min(max(1, scene.render.resolution_x), 64)
            scene.render.resolution_y = min(max(1, scene.render.resolution_y), 64)
            scene.render.resolution_percentage = 100
            if scene.render.engine == 'CYCLES':
                scene.cycles.samples = 1
                scene.cycles.use_denoising = False
            for validation_frame in range(validation_start, validation_end + 1, validation_step):
                scene.frame_set(validation_frame)
                render_result = bpy.ops.render.render(write_still=False)
                if 'FINISHED' not in render_result:
                    raise RuntimeError(
                        f'Validation render for frame {validation_frame} returned {render_result}')
                report['validation_frames'] += 1
            report['validation_render'] = True
            report['pass'] = True
except Exception:
    report['validation_error'] = traceback.format_exc()
    warnings.append(report['validation_error'])
with open(report_path, 'w', encoding='utf-8') as f:
    json.dump(report, f, ensure_ascii=False, indent=2)
)PY";

const char* kCompatibilityScript = R"PY(import bpy, addon_utils, json, sys
args = sys.argv[sys.argv.index('--') + 1:]
manifest_path, report_path = args[0], args[1]
with open(manifest_path, 'r', encoding='utf-8') as f:
    manifest = json.load(f)
available = set()
available_versions = {}
for module in addon_utils.modules(refresh=True):
    available.add(module.__name__)
    available.add(module.__name__.split('.')[0])
    version = getattr(module, 'bl_info', {}).get('version', ())
    available_versions[module.__name__] = '.'.join(str(part) for part in version) or 'unknown'
enabled = set(addon.module for addon in bpy.context.preferences.addons)
requirements = []
for value in manifest.get('addons', []):
    name, separator, version = value.rpartition('@')
    requirements.append((name if separator else value, version if separator else ''))
missing_installed = [name for name, version in requirements
                     if name not in available and name.split('.')[0] not in available]
missing_enabled = [name for name, version in requirements
                   if name not in enabled]
version_mismatch = [f'{name} (need {version}, have {available_versions.get(name, "unknown")})'
                    for name, version in requirements
                    if version and version != 'unknown' and name in available_versions and
                    available_versions[name] != version]
missing = sorted(set(missing_installed + missing_enabled + version_mismatch))
version = f'{bpy.app.version[0]}.{bpy.app.version[1]}'
result = {'pass': version == manifest.get('version') and not missing,
          'version': version, 'missing_addons': missing,
          'missing_installed': missing_installed, 'missing_enabled': missing_enabled,
          'version_mismatch': version_mismatch}
engine = manifest.get('render_engine', '')
try:
    if engine:
        bpy.context.scene.render.engine = engine
    result['engine_available'] = True
except Exception:
    result['engine_available'] = False
result['render_engine'] = engine
result['pass'] = result['pass'] and result['engine_available']
result['gpu_devices'] = []
if engine == 'CYCLES' and manifest.get('requires_gpu', False):
    try:
        cycles_preferences = bpy.context.preferences.addons['cycles'].preferences
        cycles_preferences.get_devices()
        result['gpu_devices'] = sorted({f'{device.type}:{device.name}'
                                        for device in cycles_preferences.devices
                                        if device.type != 'CPU' and device.use})
    except Exception:
        result['gpu_devices'] = []
    result['pass'] = result['pass'] and bool(result['gpu_devices'])
with open(report_path, 'w', encoding='utf-8') as f:
    json.dump(result, f, ensure_ascii=False, indent=2)
)PY";

const char* kPreviewScript = R"PY(import bpy, os, sys

args = sys.argv[sys.argv.index('--') + 1:]
source, destination = args
image = bpy.data.images.load(source, check_existing=False)
scene = bpy.context.scene
scene.render.image_settings.file_format = 'PNG'
scene.render.image_settings.color_mode = 'RGBA'
os.makedirs(os.path.dirname(destination), exist_ok=True)
image.save_render(destination, scene=scene)
)PY";

const char* kBatchRenderScript = R"PY(import bpy, json, os, shutil, sys, traceback
args = sys.argv[sys.argv.index('--') + 1:]
output_dir, manifest_path, stop_path, frames_json, targets_json = args[:5]
frames = json.loads(frames_json)
targets = json.loads(targets_json)
scene = bpy.context.scene
completed = []
artifacts = {}
error = ''
primary_dir = os.path.join(output_dir, 'primary')
compositor_root = os.path.join(output_dir, 'compositor')
os.makedirs(primary_dir, exist_ok=True)
tree = getattr(scene, 'compositing_node_group', None)
nodes = [node for node in tree.nodes if node.type == 'OUTPUT_FILE'] if tree else []
if len(nodes) != len(targets):
    raise RuntimeError(f'Compositor output count changed: expected {len(targets)}, have {len(nodes)}')
for node, target in zip(nodes, targets):
    identifier = target.get('id', '')
    if not identifier or '/' in identifier or '\\' in identifier or identifier in {'.', '..'}:
        raise RuntimeError('Invalid compositor output identifier')
    sandbox_directory = os.path.join(compositor_root, identifier)
    if hasattr(node, 'base_path'):
        node.base_path = sandbox_directory
    elif hasattr(node, 'directory'):
        node.directory = sandbox_directory
    else:
        raise RuntimeError(f'File Output node {node.name} has no writable directory')

def file_state(root):
    result = {}
    if not os.path.isdir(root):
        return result
    for current, _, names in os.walk(root):
        for name in names:
            path = os.path.join(current, name)
            try:
                stat = os.stat(path)
                result[path] = (stat.st_mtime_ns, stat.st_size)
            except OSError:
                pass
    return result

def persist():
    temporary = manifest_path + '.tmp'
    with open(temporary, 'w', encoding='utf-8') as handle:
        json.dump({'completed': completed, 'artifacts': artifacts, 'error': error}, handle,
                  ensure_ascii=False, indent=2)
    os.replace(temporary, manifest_path)
try:
    for index, frame in enumerate(frames):
        if index and os.path.exists(stop_path):
            break
        scene.frame_set(int(frame))
        before = file_state(primary_dir)
        before.update(file_state(compositor_root))
        scene.render.filepath = os.path.join(primary_dir, f'frame_{int(frame):06d}')
        result = bpy.ops.render.render(write_still=True)
        if 'FINISHED' not in result:
            raise RuntimeError(f'Render frame {frame} returned {result}')
        after = file_state(primary_dir)
        after.update(file_state(compositor_root))
        changed = sorted(path for path, state in after.items() if before.get(path) != state)
        frame_artifacts = []
        capture_dir = os.path.join(output_dir, 'captured', str(int(frame)))
        os.makedirs(capture_dir, exist_ok=True)
        for artifact_index, path in enumerate(changed):
            if os.path.commonpath([os.path.abspath(path), os.path.abspath(primary_dir)]) == \
                    os.path.abspath(primary_dir):
                logical = 'primary'
            else:
                relative = os.path.relpath(path, compositor_root).replace(os.sep, '/')
                logical = 'compositor/' + relative
            extension = os.path.splitext(path)[1]
            captured = os.path.join(capture_dir, f'{artifact_index:04d}{extension}')
            shutil.copy2(path, captured)
            frame_artifacts.append({
                'logical': logical,
                'source': os.path.relpath(captured, output_dir).replace(os.sep, '/'),
            })
        if not any(value['logical'] == 'primary' for value in frame_artifacts):
            raise RuntimeError(f'Primary render output missing for frame {frame}')
        artifacts[str(int(frame))] = frame_artifacts
        completed.append(int(frame))
        persist()
except Exception:
    error = traceback.format_exc()
    persist()
    raise
persist()
)PY";

}  // namespace

std::string WideToUtf8(const std::wstring& value) {
    if (value.empty()) return {};
    int size = WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
                                   nullptr, 0, nullptr, nullptr);
    std::string output(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
                        output.data(), size, nullptr, nullptr);
    return output;
}

std::wstring Utf8ToWide(const std::string& value) {
    if (value.empty()) return {};
    int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                                   static_cast<int>(value.size()), nullptr, 0);
    if (size <= 0) return L"";
    std::wstring output(static_cast<size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                        static_cast<int>(value.size()), output.data(), size);
    return output;
}

std::wstring LastErrorMessage(DWORD code) {
    wchar_t* message = nullptr;
    DWORD size = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
            FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, code, 0, reinterpret_cast<wchar_t*>(&message), 0, nullptr);
    std::wstring output = size ? std::wstring(message, size) : L"Windows 错误 " + std::to_wstring(code);
    if (message) LocalFree(message);
    while (!output.empty() && (output.back() == L'\r' || output.back() == L'\n')) output.pop_back();
    return output;
}

std::string NewRandomHex(size_t bytes) {
    std::vector<unsigned char> data(bytes);
    if (BCryptGenRandom(nullptr, data.data(), static_cast<ULONG>(data.size()),
                        BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) {
        throw std::runtime_error("BCryptGenRandom failed");
    }
    std::ostringstream output;
    output << std::hex << std::setfill('0');
    for (unsigned char value : data) output << std::setw(2) << static_cast<int>(value);
    return output.str();
}

std::string NewInviteCode() {
    uint32_t value = 0;
    constexpr uint32_t range = 1000000;
    constexpr uint32_t limit = UINT32_MAX - (UINT32_MAX % range);
    do {
        if (BCryptGenRandom(nullptr, reinterpret_cast<PUCHAR>(&value), sizeof(value),
                            BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) {
            throw std::runtime_error("BCryptGenRandom failed");
        }
    } while (value >= limit);
    char code[7]{};
    std::snprintf(code, sizeof(code), "%06u", value % range);
    return code;
}

bool IsInviteCode(const std::string& value) {
    return value.size() == 6 &&
        std::all_of(value.begin(), value.end(), [](unsigned char ch) {
            return ch >= '0' && ch <= '9';
        });
}

bool IsSafeOutputExtension(const std::string& value) {
    return value.empty() || (value.size() >= 2 && value.size() <= 16 && value.front() == '.' &&
        std::all_of(value.begin() + 1, value.end(), [](unsigned char ch) {
            return std::isalnum(ch) != 0 || ch == '_';
        }));
}

std::vector<std::vector<int>> BuildFramePackages(
    const std::vector<int>& frames, size_t packageSize) {
    std::vector<std::vector<int>> result;
    if (packageSize == 0) return result;
    for (size_t offset = 0; offset < frames.size(); offset += packageSize) {
        size_t end = std::min(frames.size(), offset + packageSize);
        result.emplace_back(frames.begin() + static_cast<std::ptrdiff_t>(offset),
                            frames.begin() + static_cast<std::ptrdiff_t>(end));
    }
    return result;
}

std::string Sha256Prefix(const std::string& value, size_t chars) {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    DWORD objectSize = 0, resultSize = 0;
    std::array<unsigned char, 32> digest{};
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0)
        return {};
    BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH,
                      reinterpret_cast<PUCHAR>(&objectSize), sizeof(objectSize), &resultSize, 0);
    std::vector<unsigned char> object(objectSize);
    if (BCryptCreateHash(algorithm, &hash, object.data(), objectSize, nullptr, 0, 0) == 0) {
        BCryptHashData(hash, reinterpret_cast<PUCHAR>(const_cast<char*>(value.data())),
                       static_cast<ULONG>(value.size()), 0);
        BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0);
    }
    if (hash) BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(algorithm, 0);
    std::ostringstream output;
    output << std::hex << std::setfill('0');
    for (unsigned char byte : digest) output << std::setw(2) << static_cast<int>(byte);
    return output.str().substr(0, chars);
}

std::string Sha256File(const fs::path& path, std::atomic<bool>* stopRequested) {
    std::ifstream input(path, std::ios::binary);
    if (!input) return {};
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    DWORD objectSize = 0, resultSize = 0;
    std::array<unsigned char, 32> digest{};
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0)
        return {};
    BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH,
                      reinterpret_cast<PUCHAR>(&objectSize), sizeof(objectSize), &resultSize, 0);
    std::vector<unsigned char> object(objectSize);
    bool ok = BCryptCreateHash(algorithm, &hash, object.data(), objectSize, nullptr, 0, 0) == 0;
    // Windows GUI threads commonly have a 1 MiB stack. Keep the streaming
    // buffer on the heap so hashing a project cannot exhaust that stack.
    std::vector<char> buffer(256 * 1024);
    while (ok && input) {
        if (stopRequested && stopRequested->load()) {
            ok = false;
            break;
        }
        input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        std::streamsize count = input.gcount();
        if (count > 0)
            ok = BCryptHashData(hash, reinterpret_cast<PUCHAR>(buffer.data()),
                                static_cast<ULONG>(count), 0) == 0;
    }
    ok = ok && !input.bad() &&
        BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0) == 0;
    if (hash) BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(algorithm, 0);
    if (!ok) return {};
    std::ostringstream output;
    output << std::hex << std::setfill('0');
    for (unsigned char byte : digest) output << std::setw(2) << static_cast<int>(byte);
    return output.str();
}

std::string HmacSha256(const std::string& key, const std::string& value) {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    DWORD objectSize = 0, resultSize = 0;
    std::array<unsigned char, 32> digest{};
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr,
                                    BCRYPT_ALG_HANDLE_HMAC_FLAG) != 0)
        return {};
    BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH,
                      reinterpret_cast<PUCHAR>(&objectSize), sizeof(objectSize), &resultSize, 0);
    std::vector<unsigned char> object(objectSize);
    if (BCryptCreateHash(algorithm, &hash, object.data(), objectSize,
                         reinterpret_cast<PUCHAR>(const_cast<char*>(key.data())),
                         static_cast<ULONG>(key.size()), 0) == 0) {
        BCryptHashData(hash, reinterpret_cast<PUCHAR>(const_cast<char*>(value.data())),
                       static_cast<ULONG>(value.size()), 0);
        BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0);
    }
    if (hash) BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(algorithm, 0);
    std::ostringstream output;
    output << std::hex << std::setfill('0');
    for (unsigned char byte : digest) output << std::setw(2) << static_cast<int>(byte);
    return output.str();
}

fs::path ExecutableDirectory() {
    std::vector<wchar_t> path(32768);
    DWORD size = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    return fs::path(std::wstring(path.data(), size)).parent_path();
}

fs::path DefaultDataDirectory() {
    PWSTR value = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &value))) {
        fs::path result = fs::path(value) / L"RenderMesh";
        CoTaskMemFree(value);
        return result;
    }
    return ExecutableDirectory() / L"data";
}

double IdleSeconds() {
    LASTINPUTINFO info{};
    info.cbSize = sizeof(info);
    if (!GetLastInputInfo(&info)) return 0;
    // Both values deliberately use the 32-bit tick domain. Unsigned subtraction
    // remains correct across GetTickCount's 49.7-day rollover.
    DWORD elapsed = GetTickCount() - info.dwTime;
    return static_cast<double>(elapsed) / 1000.0;
}

void InitializeLogging(const fs::path& dataDir) {
    std::scoped_lock lock(gLogMutex);
    gLogDirectory = dataDir / L"logs";
    std::error_code ignored;
    fs::create_directories(gLogDirectory, ignored);
    gLogPath = gLogDirectory / Utf8ToWide("RenderMesh-" + LogDayStamp() + ".log");
}

void LogInfo(const std::string& message) { WriteLogLine("INFO", message); }
void LogWarning(const std::string& message) { WriteLogLine("WARN", message); }
void LogError(const std::string& message) { WriteLogLine("ERROR", message); }

fs::path CurrentLogPath() {
    std::scoped_lock lock(gLogMutex);
    return gLogPath;
}

HardwareInfo QueryHardwareInfo() {
    HardwareInfo result;
    wchar_t cpu[512]{};
    DWORD cpuBytes = sizeof(cpu);
    if (RegGetValueW(HKEY_LOCAL_MACHINE,
                     L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0",
                     L"ProcessorNameString", RRF_RT_REG_SZ, nullptr, cpu, &cpuBytes) ==
        ERROR_SUCCESS) {
        result.cpuModel = TrimAscii(WideToUtf8(cpu));
    }
    if (result.cpuModel.empty()) result.cpuModel = "Unknown CPU";

    IDXGIFactory1* factory = nullptr;
    if (SUCCEEDED(CreateDXGIFactory1(IID_IDXGIFactory1,
                                     reinterpret_cast<void**>(&factory)))) {
        for (UINT index = 0;; ++index) {
            IDXGIAdapter1* adapter = nullptr;
            HRESULT status = factory->EnumAdapters1(index, &adapter);
            if (status == DXGI_ERROR_NOT_FOUND) break;
            if (FAILED(status)) break;
            DXGI_ADAPTER_DESC1 description{};
            if (SUCCEEDED(adapter->GetDesc1(&description)) &&
                !(description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) {
                std::string name = TrimAscii(WideToUtf8(description.Description));
                if (!name.empty() &&
                    std::find(result.gpuModels.begin(), result.gpuModels.end(), name) ==
                        result.gpuModels.end())
                    result.gpuModels.push_back(std::move(name));
            }
            adapter->Release();
        }
        factory->Release();
    }
    return result;
}

ConfigStore::ConfigStore(fs::path dataDir)
    : dataDir_(std::move(dataDir)), configPath_(dataDir_ / L"config.json") {}

Config ConfigStore::Load() {
    Config config;
    wchar_t hostname[256]{};
    DWORD hostnameSize = 256;
    GetComputerNameW(hostname, &hostnameSize);
    config.nodeName = WideToUtf8(std::wstring(hostname, hostnameSize));
    config.nodeId = NewRandomHex(16);
    config.inviteCode = NewInviteCode();
    config.clusterToken = NewRandomHex(16);
    if (!fs::exists(configPath_)) {
        std::wstring ignored;
        Save(config, ignored);
        return config;
    }
    try {
        json data = json::parse(ReadText(configPath_));
        config.nodeId = data.value("node_id", config.nodeId);
        config.nodeName = data.value("node_name", config.nodeName);
        config.clusterToken = data.value("cluster_token", config.clusterToken);
        config.inviteCode = data.value("invite_code", std::string{});
        if (!IsInviteCode(config.inviteCode))
            config.inviteCode = InviteCodeFromToken(config.clusterToken);
        config.pairingPending = data.value("pairing_pending", false);
        if (!config.pairingPending && config.clusterToken.size() < 16)
            config.clusterToken = NewRandomHex(16);
        config.shareEnabled = data.value("share_enabled", false);
        config.activeCpuPercent = std::clamp(data.value("active_cpu_percent", 15), 1, 100);
        config.idleAfterSeconds = std::clamp(data.value("idle_after_seconds", 180), 1, 86400);
        config.httpPort = std::clamp(data.value("http_port", 8391), 1024, 65535);
        config.discoveryPort = std::clamp(data.value("discovery_port", 8390), 1024, 65535);
        config.blenderPath = Utf8ToWide(data.value("blender_path", std::string{}));
        config.manualPeerEndpoint = data.value("manual_peer_endpoint", std::string{});
        config.theme = data.value("theme", "system");
        if (config.theme != "system" && config.theme != "light" && config.theme != "dark")
            config.theme = "system";
    } catch (...) {
        fs::path backup = configPath_;
        backup += L".broken";
        std::error_code ec;
        fs::rename(configPath_, backup, ec);
    }
    return config;
}

bool ConfigStore::Save(const Config& config, std::wstring& error) {
    json data = {
        {"node_id", config.nodeId},
        {"node_name", config.nodeName},
        {"invite_code", config.inviteCode},
        {"cluster_token", config.clusterToken},
        {"pairing_pending", config.pairingPending},
        {"share_enabled", config.shareEnabled},
        {"active_cpu_percent", config.activeCpuPercent},
        {"idle_after_seconds", config.idleAfterSeconds},
        {"http_port", config.httpPort},
        {"discovery_port", config.discoveryPort},
        {"blender_path", WideToUtf8(config.blenderPath.wstring())},
        {"manual_peer_endpoint", config.manualPeerEndpoint},
        {"theme", config.theme},
    };
    return WriteTextAtomic(configPath_, data.dump(2), error);
}

fs::path FindBlender(const Config& config) {
    std::vector<fs::path> candidates;
    if (!config.blenderPath.empty()) {
        std::wstring filename = config.blenderPath.filename().wstring();
        std::transform(filename.begin(), filename.end(), filename.begin(),
                       [](wchar_t ch) { return static_cast<wchar_t>(std::towlower(ch)); });
        if (filename == L"blender-launcher.exe")
            candidates.push_back(config.blenderPath.parent_path() / L"blender.exe");
        candidates.push_back(config.blenderPath);
    }
    candidates.push_back(ExecutableDirectory() / L"Blender" / L"blender.exe");
    wchar_t found[32768]{};
    if (SearchPathW(nullptr, L"blender.exe", nullptr, 32768, found, nullptr) > 0)
        candidates.emplace_back(found);
    for (const wchar_t* variable : {L"ProgramFiles", L"ProgramFiles(x86)"}) {
        wchar_t root[32768]{};
        DWORD length = GetEnvironmentVariableW(variable, root, 32768);
        if (!length) continue;
        fs::path foundation = fs::path(root) / L"Blender Foundation";
        std::error_code ec;
        if (!fs::is_directory(foundation, ec)) continue;
        std::vector<fs::path> installs;
        for (const auto& entry : fs::directory_iterator(foundation, ec)) {
            fs::path executable = entry.path() / L"blender.exe";
            if (fs::is_regular_file(executable, ec)) installs.push_back(executable);
        }
        std::sort(installs.rbegin(), installs.rend());
        candidates.insert(candidates.end(), installs.begin(), installs.end());
    }
    std::error_code ec;
    for (const auto& candidate : candidates)
        if (fs::is_regular_file(candidate, ec)) return fs::absolute(candidate, ec);
    return {};
}

bool GeneratePreviewImage(
    const Config& config,
    const fs::path& dataDir,
    const fs::path& source,
    const fs::path& destination,
    std::atomic<bool>& stopRequested,
    std::wstring& error) {
    if (!fs::is_regular_file(source)) {
        error = L"预览源文件不存在";
        return false;
    }
    fs::path blender = FindBlender(config);
    if (blender.empty()) {
        error = L"未找到 Blender，无法生成该格式的预览";
        return false;
    }
    std::error_code ignored;
    fs::create_directories(destination.parent_path(), ignored);
    fs::remove(destination, ignored);
    fs::path work = dataDir / L"previews";
    fs::create_directories(work, ignored);
    fs::path script = work / L"generate-preview.py";
    {
        std::ofstream output(script, std::ios::binary | std::ios::trunc);
        output << kPreviewScript;
        if (!output) {
            error = L"无法写入预览转换脚本";
            return false;
        }
    }
    ProcessResult process = RunProcess(
        blender,
        {L"--background", L"--factory-startup", L"--disable-autoexec", L"--python",
         script.wstring(), L"--", source.wstring(), destination.wstring()},
        work / L"generate-preview.log", nullptr, &stopRequested);
    if (stopRequested.load()) {
        error = L"预览生成已取消";
        return false;
    }
    if (!process.started || process.exitCode != 0 || !fs::is_regular_file(destination)) {
        error = process.error.empty() ? L"Blender 无法生成该图片格式的预览" : process.error;
        return false;
    }
    return true;
}

PreflightReport PreflightBlend(
    const Config& config,
    const fs::path& dataDir,
    const fs::path& source,
    const fs::path& prepared,
    int validationFrameStart,
    int validationFrameEnd,
    int validationFrameStep,
    std::atomic<bool>* stopRequested) {
    PreflightReport report;
    report.sourceBlend = source;
    fs::path blender = FindBlender(config);
    if (blender.empty()) {
        report.summary = L"未找到 Blender。请先安装 Blender，或在设置中选择 blender.exe。";
        return report;
    }
    if (source.extension() != L".blend") {
        report.summary = L"只能提交 .blend 工程文件。";
        return report;
    }
    fs::path tools = dataDir / L"tools";
    fs::create_directories(tools);
    fs::path script = tools / L"preflight.py";
    fs::path reportPath = prepared.parent_path() / L"preflight.json";
    fs::path log = prepared.parent_path() / L"preflight.log";
    {
        std::ofstream output(script, std::ios::binary | std::ios::trunc);
        output << kPreflightScript;
    }
    ProcessResult process = RunProcess(
        blender,
         {L"--background", L"--disable-autoexec", source.wstring(), L"--python", script.wstring(), L"--",
         prepared.wstring(), reportPath.wstring(), std::to_wstring(validationFrameStart),
         std::to_wstring(validationFrameEnd), std::to_wstring(validationFrameStep)},
        log, nullptr, stopRequested);
    if (!process.started || process.exitCode != 0 || !fs::exists(reportPath)) {
        report.summary = L"Blender 预检失败。请查看：" + log.wstring();
        if (!process.error.empty()) report.summary += L"\n" + process.error;
        return report;
    }
    try {
        json data = json::parse(ReadText(reportPath));
        report.passed = data.value("pass", false);
        report.blenderVersion = data.value("version", std::string{});
        report.renderEngine = data.value("render_engine", std::string{});
        report.outputFormat = data.value("render_format", std::string{});
        report.outputExtension = data.value("use_file_extension", true)
            ? data.value("file_extension", std::string{}) : std::string{};
        if (report.outputFormat.empty() || !IsSafeOutputExtension(report.outputExtension)) {
            report.passed = false;
            report.warnings.push_back("The project output format has no safe file extension");
        }
        report.cpuSafeWhileActive = data.value("cpu_safe_while_active", false);
        report.requiresGpu = data.value("requires_gpu", false);
        report.addons = data.value("addons", std::vector<std::string>{});
        json primaryOutputRows = data.value("primary_outputs", json::object());
        for (const auto& [frameText, pathValue] : primaryOutputRows.items()) {
            try {
                int frame = std::stoi(frameText);
                fs::path path = Utf8ToWide(pathValue.get<std::string>());
                if (frame >= 1 && frame <= 1000000 && path.is_absolute())
                    report.primaryOutputs[frame] = std::move(path);
            } catch (...) {
            }
        }
        for (const auto& target : data.value("compositor_outputs", json::array())) {
            OutputTarget outputTarget;
            outputTarget.id = target.value("id", "");
            outputTarget.name = target.value("name", "");
            outputTarget.directory = Utf8ToWide(target.value("directory", ""));
            if (!outputTarget.id.empty() && outputTarget.directory.is_absolute())
                report.compositorOutputs.push_back(std::move(outputTarget));
        }
        report.warnings = data.value("warnings", std::vector<std::string>{});
        std::string validationError = data.value("validation_error", std::string{});
        auto parseDependencies = [](const json& values) {
            std::vector<DependencyItem> result;
            for (const auto& item : values) {
                result.push_back({item.value("kind", "unknown"), item.value("owner", ""),
                                  item.value("path", "")});
            }
            return result;
        };
        report.missing = parseDependencies(data.value("missing", json::array()));
        report.unpacked = parseDependencies(data.value("unpacked", json::array()));
        report.preparedBlend = prepared;
        if (report.passed) {
            report.summary = L"预检通过：素材已打包，Blender " +
                Utf8ToWide(report.blenderVersion) + L"，插件环境 " +
                std::to_wstring(report.addons.size()) + L" 项，输出格式 " +
                Utf8ToWide(report.outputFormat) + L"。";
        } else {
            report.summary = L"预检未通过：缺失 " + std::to_wstring(report.missing.size()) +
                L" 项，无法打包 " + std::to_wstring(report.unpacked.size()) + L" 项。";
            const auto& rows = report.missing.empty() ? report.unpacked : report.missing;
            for (size_t i = 0; i < std::min<size_t>(rows.size(), 5); ++i)
                report.summary += L"\n• " + Utf8ToWide(rows[i].path);
            if (rows.empty() && !validationError.empty())
                report.summary += L"\n验证渲染失败；可能是相机、插件、节点或渲染引擎错误。详见：" +
                    log.wstring();
        }
    } catch (const std::exception& exception) {
        report.summary = L"无法读取预检报告：" + Utf8ToWide(exception.what());
    }
    return report;
}

bool CheckWorkerCompatibility(
    const Config& config,
    const fs::path& dataDir,
    const fs::path& blend,
    const std::string& expectedVersion,
    const std::string& expectedEngine,
    bool requiresGpu,
    const std::vector<std::string>& requiredAddons,
    std::atomic<bool>& stopRequested,
    std::wstring& summary) {
    fs::path blender = FindBlender(config);
    if (blender.empty()) {
        summary = L"Worker 未安装 Blender";
        return false;
    }
    fs::path tools = dataDir / L"tools";
    fs::create_directories(tools);
    fs::path script = tools / L"compatibility.py";
    fs::path manifest = blend.parent_path() / L"required-environment.json";
    fs::path report = blend.parent_path() / L"worker-environment.json";
    fs::path log = blend.parent_path() / L"compatibility.log";
    {
        std::ofstream output(script, std::ios::binary | std::ios::trunc);
        output << kCompatibilityScript;
    }
    {
        std::ofstream output(manifest, std::ios::binary | std::ios::trunc);
        output << json{{"version", expectedVersion}, {"render_engine", expectedEngine},
                       {"requires_gpu", requiresGpu},
                       {"addons", requiredAddons}}.dump(2);
    }
    ProcessResult process = RunProcess(
        blender,
        {L"--background", L"--disable-autoexec", blend.wstring(), L"--python", script.wstring(),
         L"--", manifest.wstring(), report.wstring()},
        log, &config, &stopRequested);
    if (!process.started || process.exitCode != 0 || !fs::exists(report)) {
        summary = L"Worker 环境检查失败：" + log.wstring();
        return false;
    }
    try {
        json data = json::parse(ReadText(report));
        std::string version = data.value("version", std::string{});
        auto missing = data.value("missing_addons", std::vector<std::string>{});
        bool engineAvailable = data.value("engine_available", expectedEngine.empty());
        auto gpuDevices = data.value("gpu_devices", std::vector<std::string>{});
        if (version != expectedVersion) {
            summary = L"Blender 版本不一致：需要 " + Utf8ToWide(expectedVersion) + L"，本机 " +
                Utf8ToWide(version);
            return false;
        }
        if (!missing.empty()) {
            summary = L"缺少插件：";
            for (size_t i = 0; i < std::min<size_t>(missing.size(), 8); ++i)
                summary += (i ? L"、" : L"") + Utf8ToWide(missing[i]);
            return false;
        }
        if (!engineAvailable) {
            summary = L"缺少渲染引擎：" + Utf8ToWide(expectedEngine);
            return false;
        }
        if (requiresGpu && expectedEngine == "CYCLES" && gpuDevices.empty()) {
            summary = L"Worker 没有已启用的 Cycles GPU 渲染设备";
            return false;
        }
        summary = L"Worker 环境兼容";
        return data.value("pass", false);
    } catch (const std::exception& exception) {
        summary = L"Worker 环境报告无效：" + Utf8ToWide(exception.what());
        return false;
    }
}

bool RenderBlenderFrame(
    const Config& config,
    const fs::path& blend,
    const fs::path& outputDir,
    int frame,
    const std::string& outputFormat,
    const std::string& outputExtension,
    bool applySharingLimit,
    std::atomic<bool>& stopRequested,
    fs::path& output,
    std::wstring& error) {
    fs::path blender = FindBlender(config);
    if (blender.empty()) {
        error = L"未找到 Blender";
        return false;
    }
    fs::create_directories(outputDir);
    fs::path prefix = outputDir / L"frame_######";
    fs::path log = outputDir / (L"frame_" + std::to_wstring(frame) + L".log");
    if (outputFormat.empty() || !IsSafeOutputExtension(outputExtension)) {
        error = L"工程输出格式信息无效";
        return false;
    }
    std::wstring extension = Utf8ToWide(outputExtension);
    std::wstring frameText = std::to_wstring(frame);
    output = outputDir / (L"frame_" +
        std::wstring(6 - std::min<size_t>(6, frameText.size()), L'0') + frameText + extension);
    std::error_code removeError;
    fs::remove(output, removeError);
    ProcessResult process = RunProcess(
        blender,
        {L"--background", L"--disable-autoexec", blend.wstring(), L"--render-output",
         prefix.wstring(), L"--render-frame", std::to_wstring(frame)},
        log, applySharingLimit ? &config : nullptr, &stopRequested);
    if (!process.started || process.exitCode != 0) {
        error = process.error.empty()
            ? L"Blender 渲染失败，退出码 " + std::to_wstring(process.exitCode) + L"。日志：" +
                  log.wstring()
            : process.error;
        return false;
    }
    if (!fs::exists(output)) {
        error = L"Blender 已退出，但没有生成帧文件。日志：" + log.wstring();
        return false;
    }
    return true;
}

bool RenderBlenderPackage(
    const Config& config,
    const fs::path& blend,
    const fs::path& outputDir,
    const std::vector<int>& frames,
    const std::string& outputFormat,
    const std::string& outputExtension,
    bool applySharingLimit,
    bool stopAfterCurrentOnActivity,
    std::atomic<bool>& stopRequested,
    std::atomic<bool>& finishCurrentThenStop,
    const std::vector<OutputTarget>& compositorOutputs,
    std::map<int, std::vector<RenderArtifact>>& completed,
    std::wstring& error) {
    completed.clear();
    if (frames.empty() || frames.size() > 5 || outputFormat.empty() ||
        !IsSafeOutputExtension(outputExtension)) {
        error = L"渲染任务包参数无效";
        return false;
    }
    fs::path blender = FindBlender(config);
    if (blender.empty()) {
        error = L"未找到 Blender";
        return false;
    }
    fs::create_directories(outputDir);
    fs::path script = outputDir / L"render-package.py";
    fs::path manifest = outputDir / L"render-package.json";
    fs::path stopMarker = outputDir / L"stop-after-current";
    fs::path log = outputDir / L"render-package.log";
    {
        std::ofstream output(script, std::ios::binary | std::ios::trunc);
        output << kBatchRenderScript;
        if (!output) {
            error = L"无法写入批量渲染脚本";
            return false;
        }
    }
    std::error_code ignored;
    fs::remove(manifest, ignored);
    for (int frame : frames) {
        std::wstring frameText = std::to_wstring(frame);
        fs::path expected = outputDir / (L"frame_" +
            std::wstring(6 - std::min<size_t>(6, frameText.size()), L'0') + frameText +
            Utf8ToWide(outputExtension));
        fs::remove(expected, ignored);
    }

    std::atomic<bool> processFinished = false;
    std::thread stopMarkerThread([&] {
        while (!processFinished.load() && !stopRequested.load()) {
            if (fs::is_regular_file(stopMarker)) {
                finishCurrentThenStop = true;
                return;
            }
            if (stopAfterCurrentOnActivity &&
                IdleSeconds() < config.idleAfterSeconds) {
                finishCurrentThenStop = true;
                // Detection and marker publication deliberately happen in the
                // same thread, so no second polling interval can start another
                // Blender frame after activity has already been observed.
                std::ofstream(stopMarker, std::ios::binary | std::ios::trunc) << "stop";
                return;
            }
            Sleep(50);
        }
    });
    json outputTargets = json::array();
    for (const auto& target : compositorOutputs)
        outputTargets.push_back({{"id", target.id}, {"name", target.name}});
    ProcessResult process = RunProcess(
        blender,
        {L"--background", L"--disable-autoexec", blend.wstring(), L"--python",
         script.wstring(), L"--", outputDir.wstring(), manifest.wstring(),
         stopMarker.wstring(), Utf8ToWide(json(frames).dump()),
         Utf8ToWide(outputTargets.dump())},
        log, applySharingLimit ? &config : nullptr, &stopRequested);
    processFinished = true;
    if (stopMarkerThread.joinable()) stopMarkerThread.join();

    try {
        if (fs::is_regular_file(manifest)) {
            json progress = json::parse(ReadText(manifest));
            json artifactRows = progress.value("artifacts", json::object());
            for (int frame : progress.value("completed", std::vector<int>{})) {
                if (std::find(frames.begin(), frames.end(), frame) == frames.end()) continue;
                std::vector<RenderArtifact> values;
                for (const auto& artifact : artifactRows.value(
                         std::to_string(frame), json::array())) {
                    std::string logical = artifact.value("logical", "");
                    fs::path relative = Utf8ToWide(artifact.value("source", ""));
                    fs::path source = outputDir / relative;
                    bool safeRelative = relative.is_relative() && !relative.has_root_name() &&
                        !relative.has_root_directory();
                    for (const auto& component : relative)
                        safeRelative = safeRelative && component != L".." && component != L".";
                    if (!logical.empty() && safeRelative &&
                        fs::is_regular_file(source))
                        values.push_back({logical, source});
                }
                if (!values.empty()) completed[frame] = std::move(values);
            }
            std::string scriptError = progress.value("error", "");
            if (!scriptError.empty()) error = Utf8ToWide(scriptError);
        }
    } catch (const std::exception& exception) {
        error = L"批量渲染进度文件无效：" + Utf8ToWide(exception.what());
    }
    if (!process.started || process.exitCode != 0) {
        if (error.empty()) {
            error = process.error.empty()
                ? L"Blender 批量渲染失败，退出码 " + std::to_wstring(process.exitCode) +
                      L"。日志：" + log.wstring()
                : process.error;
        }
        return false;
    }
    return true;
}

bool WriteResultBundle(
    const fs::path& destination,
    const std::vector<RenderArtifact>& artifacts,
    std::wstring& error,
    std::atomic<bool>* stopRequested) {
    if (artifacts.empty() || artifacts.size() > 4096) {
        error = L"结果文件包条目数量无效";
        return false;
    }
    fs::create_directories(destination.parent_path());
    uint64_t requiredBytes = 512ull * 1024 * 1024;
    for (const auto& artifact : artifacts) {
        std::error_code ec;
        uint64_t size = fs::file_size(artifact.source, ec);
        if (ec || size > UINT64_MAX - requiredBytes) {
            error = L"无法计算结果文件包所需空间";
            return false;
        }
        requiredBytes += size;
    }
    ULARGE_INTEGER freeBytes{};
    if (!GetDiskFreeSpaceExW(destination.parent_path().c_str(), &freeBytes, nullptr, nullptr) ||
        freeBytes.QuadPart < requiredBytes) {
        error = L"结果文件包所在磁盘空间不足";
        return false;
    }
    fs::path temporary = destination;
    temporary += L".tmp";
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    if (!output) {
        error = L"无法创建结果文件包";
        return false;
    }
    const char magic[8] = {'R', 'M', 'B', 'N', 'D', 'L', '1', '\0'};
    output.write(magic, sizeof(magic));
    uint32_t count = static_cast<uint32_t>(artifacts.size());
    output.write(reinterpret_cast<const char*>(&count), sizeof(count));
    std::set<std::string> logicalPaths;
    std::vector<char> buffer(1024 * 1024);
    for (const auto& artifact : artifacts) {
        if (stopRequested && stopRequested->load()) {
            error = L"结果文件包已取消";
            output.close();
            std::error_code ignored;
            fs::remove(temporary, ignored);
            return false;
        }
        std::error_code ec;
        uint64_t size = fs::file_size(artifact.source, ec);
        if (ec || artifact.logicalPath.empty() || artifact.logicalPath.size() > 4096 ||
            artifact.logicalPath.find('\0') != std::string::npos ||
            !logicalPaths.insert(artifact.logicalPath).second) {
            error = L"结果文件包包含无效或重复条目";
            output.close();
            fs::remove(temporary, ec);
            return false;
        }
        uint32_t pathSize = static_cast<uint32_t>(artifact.logicalPath.size());
        output.write(reinterpret_cast<const char*>(&pathSize), sizeof(pathSize));
        output.write(reinterpret_cast<const char*>(&size), sizeof(size));
        output.write(artifact.logicalPath.data(), pathSize);
        std::ifstream input(artifact.source, std::ios::binary);
        uint64_t remaining = size;
        while (input && remaining) {
            if (stopRequested && stopRequested->load()) break;
            size_t wanted = static_cast<size_t>(std::min<uint64_t>(buffer.size(), remaining));
            input.read(buffer.data(), static_cast<std::streamsize>(wanted));
            size_t read = static_cast<size_t>(input.gcount());
            if (!read) break;
            output.write(buffer.data(), static_cast<std::streamsize>(read));
            remaining -= read;
        }
        if (remaining || !output) {
            error = L"结果文件包写入不完整";
            output.close();
            fs::remove(temporary, ec);
            return false;
        }
    }
    output.flush();
    output.close();
    if (!MoveFileExW(temporary.c_str(), destination.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        error = LastErrorMessage();
        DeleteFileW(temporary.c_str());
        return false;
    }
    return true;
}

bool ExtractResultBundle(
    const fs::path& bundle,
    const fs::path& stagingDirectory,
    std::map<std::string, fs::path>& artifacts,
    std::wstring& error,
    std::atomic<bool>* stopRequested) {
    artifacts.clear();
    std::ifstream input(bundle, std::ios::binary);
    char magic[8]{};
    uint32_t count = 0;
    input.read(magic, sizeof(magic));
    input.read(reinterpret_cast<char*>(&count), sizeof(count));
    if (!input || std::memcmp(magic, "RMBNDL1\0", 8) != 0 || !count || count > 4096) {
        error = L"结果文件包头无效";
        return false;
    }
    std::error_code ec;
    fs::remove_all(stagingDirectory, ec);
    fs::create_directories(stagingDirectory, ec);
    if (ec) {
        error = L"无法创建结果暂存目录";
        return false;
    }
    uint64_t total = 0;
    std::vector<char> buffer(1024 * 1024);
    for (uint32_t index = 0; index < count; ++index) {
        if (stopRequested && stopRequested->load()) {
            error = L"结果文件包解包已取消";
            fs::remove_all(stagingDirectory, ec);
            return false;
        }
        uint32_t pathSize = 0;
        uint64_t fileSize = 0;
        input.read(reinterpret_cast<char*>(&pathSize), sizeof(pathSize));
        input.read(reinterpret_cast<char*>(&fileSize), sizeof(fileSize));
        if (!input || !pathSize || pathSize > 4096 ||
            fileSize > 100ull * 1024 * 1024 * 1024 ||
            total > 100ull * 1024 * 1024 * 1024 - fileSize) {
            error = L"结果文件包条目大小无效";
            fs::remove_all(stagingDirectory, ec);
            return false;
        }
        std::string logical(pathSize, '\0');
        input.read(logical.data(), static_cast<std::streamsize>(pathSize));
        fs::path relative = Utf8ToWide(logical);
        bool safe = !logical.empty() && logical.find('\0') == std::string::npos &&
            logical.find('\\') == std::string::npos && relative.is_relative() &&
            !relative.has_root_name() && !relative.has_root_directory();
        for (const auto& component : relative)
            safe = safe && component != L".." && component != L".";
        if (!input || !safe || artifacts.contains(logical)) {
            error = L"结果文件包包含不安全路径";
            fs::remove_all(stagingDirectory, ec);
            return false;
        }
        fs::path destination = stagingDirectory / relative;
        fs::create_directories(destination.parent_path(), ec);
        std::ofstream output(destination, std::ios::binary | std::ios::trunc);
        uint64_t remaining = fileSize;
        while (input && output && remaining) {
            if (stopRequested && stopRequested->load()) break;
            size_t wanted = static_cast<size_t>(std::min<uint64_t>(buffer.size(), remaining));
            input.read(buffer.data(), static_cast<std::streamsize>(wanted));
            size_t read = static_cast<size_t>(input.gcount());
            if (!read) break;
            output.write(buffer.data(), static_cast<std::streamsize>(read));
            remaining -= read;
        }
        if (remaining || !output) {
            error = L"结果文件包内容不完整";
            output.close();
            fs::remove_all(stagingDirectory, ec);
            return false;
        }
        total += fileSize;
        artifacts.emplace(std::move(logical), std::move(destination));
    }
    char trailing = 0;
    if (input.read(&trailing, 1)) {
        error = L"结果文件包含有多余数据";
        fs::remove_all(stagingDirectory, ec);
        artifacts.clear();
        return false;
    }
    return true;
}

std::wstring TaskStateLabel(TaskState state) {
    switch (state) {
        case TaskState::Pending: return L"待提交";
        case TaskState::Dispatching: return L"发送中";
        case TaskState::Assigned: return L"已分配";
        case TaskState::Rendering: return L"渲染中";
        case TaskState::Completed: return L"已完成";
        case TaskState::Failed: return L"失败";
        case TaskState::Stopped: return L"已停止";
    }
    return L"未知";
}

WindowsJobLimiter::WindowsJobLimiter() {
    handle_ = CreateJobObjectW(nullptr, nullptr);
    if (!handle_) return;
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!SetInformationJobObject(handle_, JobObjectExtendedLimitInformation, &limits, sizeof(limits))) {
        CloseHandle(handle_);
        handle_ = nullptr;
    }
}

WindowsJobLimiter::~WindowsJobLimiter() {
    if (handle_) CloseHandle(handle_);
}

bool WindowsJobLimiter::Assign(HANDLE process, std::wstring& error) {
    if (!handle_) {
        error = L"无法创建 Windows Job Object";
        return false;
    }
    if (!AssignProcessToJobObject(handle_, process)) {
        error = LastErrorMessage();
        return false;
    }
    return true;
}

bool WindowsJobLimiter::SetCpuPercent(int percent, std::wstring& error) {
    if (!handle_ || percent < 1 || percent > 100) {
        error = L"CPU 百分比无效";
        return false;
    }
    JOBOBJECT_CPU_RATE_CONTROL_INFORMATION info{};
    if (percent < 100) {
        info.ControlFlags = JOB_OBJECT_CPU_RATE_CONTROL_ENABLE |
                            JOB_OBJECT_CPU_RATE_CONTROL_HARD_CAP;
        info.CpuRate = static_cast<DWORD>(percent * 100);
    }
    if (!SetInformationJobObject(handle_, JobObjectCpuRateControlInformation, &info, sizeof(info))) {
        error = LastErrorMessage();
        return false;
    }
    return true;
}

}  // namespace rm
