// Blender implementation of RenderAdapter: executable discovery, first-run
// initialisation, single-pass project preparation, worker compatibility
// probes, packaged frame rendering and preview conversion.

#include "Render.hpp"

#include <cwctype>
#include <set>

namespace rm {
namespace {

const char* kBlenderBootstrapScript = R"PY(import bpy, os, sys

args = sys.argv[sys.argv.index('--') + 1:]
ready_path = args[0]
config_dir = bpy.utils.user_resource('CONFIG')
if not config_dir:
    raise RuntimeError('Blender did not provide a user configuration directory')
os.makedirs(config_dir, exist_ok=True)
preferences = os.path.join(config_dir, 'userpref.blend')
if not os.path.isfile(preferences):
    result = bpy.ops.wm.save_userpref()
    if 'FINISHED' not in result:
        raise RuntimeError(f'Unable to save Blender user preferences: {result}')
temporary = ready_path + '.tmp'
with open(temporary, 'w', encoding='utf-8') as handle:
    handle.write('RENDERMESH_BLENDER_READY\n')
os.replace(temporary, ready_path)
print('RENDERMESH_BLENDER_READY')
)PY";

std::mutex gBlenderReadinessMutex;
std::set<std::wstring> gReadyBlenders;

std::vector<std::wstring> BlenderBootstrapArguments(
    const fs::path& script,
    const fs::path& readyMarker) {
    return {L"--background", L"--factory-startup", L"--disable-autoexec", L"-noaudio",
            L"--python-exit-code", L"95", L"--python", script.wstring(), L"--",
            readyMarker.wstring()};
}

std::wstring BlenderReadinessKey(const fs::path& blender) {
    std::error_code error;
    fs::path resolved = fs::weakly_canonical(blender, error);
    if (error) {
        error.clear();
        resolved = fs::absolute(blender, error);
    }
    if (error) resolved = blender;
    std::wstring key = resolved.wstring();
    std::transform(key.begin(), key.end(), key.begin(), [](wchar_t value) {
        return static_cast<wchar_t>(std::towlower(value));
    });
    error.clear();
    auto modified = fs::last_write_time(blender, error);
    if (!error) key += L"|" + std::to_wstring(
        static_cast<long long>(modified.time_since_epoch().count()));
    return key;
}

bool EnsureBlenderReady(
    const fs::path& blender,
    const fs::path& workDirectory,
    std::atomic<bool>* stopRequested,
    std::wstring& error) {
    if (stopRequested && stopRequested->load()) {
        error = L"Blender 初始化已取消";
        return false;
    }
    const std::wstring key = BlenderReadinessKey(blender);
    std::scoped_lock lock(gBlenderReadinessMutex);
    if (gReadyBlenders.contains(key)) return true;
    if (stopRequested && stopRequested->load()) {
        error = L"Blender 初始化已取消";
        return false;
    }

    fs::path tools = workDirectory / L"tools";
    std::error_code directoryError;
    fs::create_directories(tools, directoryError);
    if (directoryError) {
        error = L"无法创建 Blender 初始化目录：" + Utf8ToWide(directoryError.message());
        return false;
    }
    fs::path script = tools / L"initialize-blender.py";
    fs::path log = tools / L"initialize-blender.log";
    fs::path readyMarker = tools / L"initialize-blender.ready";
    if (!WriteFileText(script, kBlenderBootstrapScript)) {
        error = L"无法写入 Blender 初始化脚本";
        return false;
    }
    std::error_code removeError;
    fs::remove(readyMarker, removeError);
    fs::remove(readyMarker.wstring() + L".tmp", removeError);
    ProcessResult process = RunProcess(
        blender, BlenderBootstrapArguments(script, readyMarker), log, stopRequested,
        std::chrono::seconds(15));
    if (!process.started || process.exitCode != 0) {
        if (process.timedOut) {
            error = L"Blender 首次无界面初始化超过 15 秒，进程已终止。日志：" + log.wstring();
        } else if (!process.error.empty()) {
            error = L"Blender 首次无界面初始化失败：" + process.error + L"。日志：" +
                log.wstring();
        } else {
            error = L"Blender 首次无界面初始化失败，退出码 " +
                std::to_wstring(process.exitCode) + L"。日志：" + log.wstring();
        }
        return false;
    }
    if (!fs::is_regular_file(readyMarker) ||
        ReadFileText(readyMarker).find("RENDERMESH_BLENDER_READY") == std::string::npos) {
        error = L"Blender 初始化未返回就绪标记。日志：" + log.wstring();
        return false;
    }
    gReadyBlenders.insert(key);
    LogInfo("Blender first-run readiness completed; executable=" + WideToUtf8(blender.wstring()));
    return true;
}

const char* kCompositorMetadataPython = R"PY(def compositor_tree(scene):
    return (getattr(scene, 'compositing_node_group', None) or
            getattr(scene, 'node_tree', None))

def output_item_labels(node):
    labels = []
    modern_items = getattr(node, 'file_output_items', None)
    if modern_items is not None:
        labels.extend(str(item.name) for item in modern_items if getattr(item, 'name', ''))
    for collection_name in ('file_slots', 'layer_slots'):
        collection = getattr(node, collection_name, None)
        if collection is not None:
            for item in collection:
                name = str(getattr(item, 'name', '') or '')
                path = str(getattr(item, 'path', '') or '')
                label = name if not path or path == name else f'{name}: {path}'
                if label:
                    labels.append(label)
    if not labels:
        labels.extend(str(socket.name) for socket in node.inputs
                      if socket.identifier != '__extend__' and socket.name)
    return list(dict.fromkeys(labels))

def discover_compositor_outputs(scene):
    rows = []
    tree = compositor_tree(scene)
    if tree is None:
        return rows
    visited = set()
    def visit(current_tree, parent_path, parent_active):
        pointer = current_tree.as_pointer()
        if pointer in visited:
            return
        visited.add(pointer)
        for index, node in enumerate(current_tree.nodes):
            node_path = parent_path + [index]
            if node.type == 'OUTPUT_FILE':
                base_directory = getattr(node, 'base_path', None)
                if base_directory is None:
                    base_directory = getattr(node, 'directory', None)
                if not base_directory:
                    base_directory = '//'
                rows.append({
                    'id': f'output-{len(rows)}',
                    'name': node.name,
                    'directory': bpy.path.abspath(base_directory),
                    'outputs': output_item_labels(node),
                    'file_pattern': str(getattr(node, 'file_name', '') or ''),
                    'scene': scene.name,
                    'node_path': node_path,
                    'required': bool(parent_active and
                                     getattr(scene.render, 'use_compositing', True) and
                                     not getattr(node, 'mute', False)),
                })
            child_tree = getattr(node, 'node_tree', None)
            if child_tree is not None:
                visit(child_tree, node_path,
                      parent_active and not getattr(node, 'mute', False))
        visited.remove(pointer)
    visit(tree, [], True)
    return rows
)PY";

// Opens the project once and does everything the submitter needs: scene
// settings, primary and File Output targets, the external-file inventory and,
// only when something must change, a self-contained copy. Files below a
// sibling `tex` folder travel in the project bundle and keep `//tex/...`
// relative paths; everything else that Blender can pack is packed. A project
// that needs no change is reported as-is so the caller copies it unmodified.
const std::string kPrepareScript = std::string(R"PY(import bpy, glob, json, os, re, sys, traceback

args = sys.argv[sys.argv.index('--') + 1:]
report_path, prepared_path = args[0], args[1]
ignore_warnings = len(args) > 2 and args[2] == '1'
inspect_only = len(args) > 3 and args[3] == '1'
warnings = []
)PY") + kCompositorMetadataPython + R"PY(

source_dir = os.path.dirname(bpy.data.filepath)
tex_root = os.path.normcase(os.path.normpath(os.path.join(source_dir, 'tex')))
has_tex = os.path.isdir(tex_root)

def resolve(path, item=None):
    library = getattr(item, 'library', None) if item is not None else None
    return bpy.path.abspath(path, library=library) if library else bpy.path.abspath(path)

def path_key(path, item=None):
    return os.path.normcase(os.path.normpath(resolve(path, item)))

def inside(key, root):
    try:
        return os.path.commonpath([key, root]) == root
    except ValueError:
        return False

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
    if any(inside(key, root) for root in runtime_roots):
        return True
    # A .blend can remember the absolute install path of Blender's Essentials
    # asset library. Another worker with the same major.minor version may have
    # Blender installed elsewhere, so accept it only when the same path suffix
    # exists in this runtime's own datafiles/assets tree.
    marker = os.sep + 'datafiles' + os.sep + 'assets' + os.sep
    index = key.lower().find(marker.lower())
    return index >= 0 and any(
        os.path.isfile(os.path.join(root, 'datafiles', 'assets', key[index + len(marker):]))
        for root in runtime_roots)

def expected_paths(path, item=None):
    path = resolve(path, item)
    if '<UDIM>' in path:
        if item is not None:
            tiles = sorted({getattr(tile, 'number', None) for tile in getattr(item, 'tiles', [])})
            tiles = [tile for tile in tiles if tile]
            if tiles:
                return [path.replace('<UDIM>', str(tile)) for tile in tiles]
        return sorted(glob.glob(path.replace('<UDIM>', '*'))) or [path]
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
            found = []
            for candidate in glob.glob(path[:match.start(1)] + '*' + path[match.end(1):]):
                hit = expression.match(candidate)
                if hit:
                    found.append(int(hit.group(1)))
            for packed in list(getattr(item, 'packed_files', []) or []):
                hit = expression.match(resolve(getattr(packed, 'filepath', '') or '', item))
                if hit:
                    found.append(int(hit.group(1)))
            duration = max(1, int(getattr(item, 'frame_duration', 1)))
            if found:
                duration = max(duration, max(found) - start + 1)
            return [path[:match.start(1)] + f'{start + index:0{width}d}' + path[match.end(1):]
                    for index in range(duration)]
    return [path]

def packed_keys(item, candidates):
    keys = set()
    for packed in list(getattr(item, 'packed_files', []) or []):
        filepath = getattr(packed, 'filepath', '') or ''
        if filepath:
            keys.add(path_key(filepath, item))
        tile = int(getattr(packed, 'tile_number', 0) or 0)
        if tile:
            keys.update(path_key(candidate) for candidate in candidates
                        if str(tile) in os.path.basename(candidate))
    # packed_file is the sole source of an ordinary, non-sequence image.
    if getattr(item, 'packed_file', None) and len(candidates) == 1:
        keys.add(path_key(candidates[0]))
    return keys

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
            stored = getattr(item, 'filepath', '') or ''
            if not stored or stored in {'<builtin>', '<generated>'}:
                continue
            rows.append({'kind': kind, 'item': item, 'owner': getattr(item, 'name', ''),
                         'stored': stored, 'path': resolve(stored, item)})
    known = {path_key(row['path']) for row in rows}
    try:
        # Blender's own path inventory also covers references that are not
        # datablocks (for example modifier caches). Runtime files of this
        # Blender version (Essentials assets) are not project files.
        for stored in bpy.utils.blend_paths(absolute=False, packed=True, local=False):
            path = resolve(stored)
            key = path_key(path)
            if key in known or is_runtime_resource(path):
                continue
            rows.append({'kind': 'external', 'item': None, 'owner': '',
                         'stored': stored, 'path': path})
            known.add(key)
    except Exception as exc:
        warnings.append(f'blend_paths: {exc}')
    for row in rows:
        item = row['item']
        candidates = expected_paths(row['stored'], item) if item is not None \
            else expected_paths(row['path'])
        packed = packed_keys(item, candidates) if item is not None else set()
        row['missing'] = [path for path in candidates
                          if path_key(path) not in packed and not os.path.exists(path)]
        row['unpacked'] = [path for path in candidates
                           if path_key(path) not in packed and os.path.exists(path)]
        directory = os.path.normcase(os.path.normpath(os.path.dirname(row['path'])))
        row['in_tex'] = has_tex and inside(directory, tex_root)
    return rows

def packable(row):
    item = row['item']
    if item is None or getattr(item, 'library', None) is not None:
        return False
    if row['kind'] == 'image':
        return getattr(item, 'source', '') in {'FILE', 'TILED'}
    return row['kind'] in {'font', 'sound', 'library', 'volume'}

def entry(row, path):
    return {'kind': row['kind'], 'owner': row['owner'], 'path': path}

def editable(row):
    item = row['item']
    return item is not None and getattr(item, 'library', None) is None

def classify(rows):
    missing, unpacked, to_pack, rewrite = [], [], [], []
    for row in rows:
        missing.extend(entry(row, path) for path in row['missing'])
        if not row['unpacked']:
            continue
        relative = row['stored'].startswith('//')
        if row['in_tex']:
            # Shipped in the project bundle: the stored path must stay
            # relative to the .blend so it resolves next to the copy.
            if not relative:
                if editable(row):
                    rewrite.append(row)
                else:
                    warnings.append('Absolute path into tex cannot be made relative: ' +
                                    row['path'])
        elif packable(row):
            to_pack.append(row)
        else:
            unpacked.extend(entry(row, path) for path in row['unpacked'])
            # A relative path would not resolve from the copy in another
            # folder; an absolute one still works locally and on shared drives.
            if relative and editable(row):
                rewrite.append(row)
            elif relative:
                warnings.append('Relative path cannot be rewritten: ' + row['path'])
    return missing, unpacked, to_pack, rewrite

def pack(rows):
    kinds = {row['kind'] for row in rows}
    try:
        if 'library' in kinds:
            bpy.ops.file.pack_libraries()
        if not has_tex or 'volume' in kinds:
            # Volumes can only be packed through pack_all.
            bpy.ops.file.pack_all()
        else:
            for row in rows:
                if row['kind'] in {'image', 'font', 'sound'}:
                    row['item'].pack()
    except Exception as exc:
        warnings.append(f'pack: {exc}')

def undefined_nodes():
    trees = list(bpy.data.node_groups)
    for collection in (bpy.data.materials, bpy.data.worlds, bpy.data.lights):
        trees.extend(value.node_tree for value in collection
                     if getattr(value, 'node_tree', None))
    trees.extend(scene.compositing_node_group for scene in bpy.data.scenes
                 if getattr(scene, 'compositing_node_group', None))
    found, seen = [], set()
    for tree in trees:
        if tree.as_pointer() in seen:
            continue
        seen.add(tree.as_pointer())
        found.extend(f'{tree.name}/{node.name}' for node in tree.nodes
                     if getattr(node, 'bl_idname', '') == 'NodeUndefined' or
                     node.type == 'UNDEFINED')
    return found

scene = bpy.context.scene
engine = scene.render.engine
cycles_device = getattr(scene.cycles, 'device', 'CPU') if engine == 'CYCLES' else ''
frame_start, frame_end = int(scene.frame_start), int(scene.frame_end)
frame_step = max(1, int(scene.frame_step))
report = {
    'pass': False, 'prepared': False, 'tex_bundle': has_tex,
    'version': f'{bpy.app.version[0]}.{bpy.app.version[1]}',
    'scene_name': scene.name, 'render_engine': engine,
    'render_format': scene.render.image_settings.file_format,
    'file_extension': scene.render.file_extension,
    'use_file_extension': scene.render.use_file_extension,
    'requires_gpu': engine != 'CYCLES' or cycles_device == 'GPU',
    'frame_start': frame_start, 'frame_end': frame_end, 'frame_step': frame_step,
    'primary_outputs': {str(frame): bpy.path.abspath(scene.render.frame_path(frame=frame))
                        for frame in range(frame_start, frame_end + 1, frame_step)},
    'compositor_outputs': discover_compositor_outputs(scene),
    'missing': [], 'unpacked': [], 'packed': 0, 'bundled': 0, 'warnings': warnings,
    'asset_warnings_ignored': ignore_warnings, 'validation_skipped_by_user': False,
    'validation_error': '',
}
try:
    rows = gather()
    missing, unpacked, to_pack, rewrite = classify(rows)
    report['bundled'] = sum(1 for row in rows if row['in_tex'] and row['unpacked'])
    blocked = bool(missing or unpacked) and not ignore_warnings
    report['pack_required'], report['rewrite_required'] = len(to_pack), len(rewrite)
    if inspect_only:
        report['missing'], report['unpacked'] = missing, unpacked
        if not blocked and not ignore_warnings:
            nodes = undefined_nodes()
            if nodes:
                raise RuntimeError('Undefined nodes, likely missing add-on: ' +
                                   ', '.join(nodes[:12]))
        report['validation_skipped_by_user'] = ignore_warnings and bool(missing or unpacked)
        report['pass'] = not blocked
        blocked = True
    elif not blocked and to_pack:
        pack(to_pack)
        report['packed'] = len(to_pack)
        missing, unpacked, still, rewrite = classify(gather())
        # Anything Blender could not pack is reported, never silently dropped.
        unpacked += [entry(row, path) for row in still for path in row['unpacked']]
        blocked = bool(missing or unpacked) and not ignore_warnings
    if not inspect_only:
        report['missing'], report['unpacked'] = missing, unpacked
    if not blocked:
        if not ignore_warnings:
            nodes = undefined_nodes()
            if nodes:
                raise RuntimeError('Undefined nodes, likely missing add-on: ' +
                                   ', '.join(nodes[:12]))
        if to_pack or rewrite:
            # relative_remap=False keeps //tex/... paths valid next to the copy.
            for row in rewrite:
                row['item'].filepath = (
                    '//' + os.path.relpath(row['path'], source_dir).replace(os.sep, '/')
                    if row['in_tex'] else row['path'])
            bpy.ops.wm.save_as_mainfile(filepath=prepared_path, copy=True,
                                        relative_remap=False)
            report['prepared'] = os.path.isfile(prepared_path)
        report['validation_skipped_by_user'] = ignore_warnings and bool(missing or unpacked)
        report['pass'] = report['prepared'] or not (to_pack or rewrite)
except Exception:
    report['validation_error'] = traceback.format_exc()
    warnings.append(report['validation_error'])
report['warnings'] = list(dict.fromkeys(warnings))
with open(report_path, 'w', encoding='utf-8') as handle:
    json.dump(report, handle, ensure_ascii=False)
)PY";

const char* kCompatibilityScript = R"PY(import bpy, json, sys
args = sys.argv[sys.argv.index('--') + 1:]
manifest_path, report_path = args[0], args[1]
with open(manifest_path, 'r', encoding='utf-8') as f:
    manifest = json.load(f)
version = f'{bpy.app.version[0]}.{bpy.app.version[1]}'
result = {'pass': version == manifest.get('version'), 'version': version}

def enable_cycles_gpu():
    preferences = bpy.context.preferences.addons['cycles'].preferences
    preferred = getattr(preferences, 'compute_device_type', 'NONE')
    backends = []
    for backend in (preferred, 'OPTIX', 'CUDA', 'HIP', 'ONEAPI'):
        if backend and backend != 'NONE' and backend not in backends:
            backends.append(backend)
    failures = []
    for backend in backends:
        try:
            preferences.compute_device_type = backend
            preferences.get_devices()
            devices = [device for device in preferences.devices if device.type == backend]
            if devices:
                for device in preferences.devices:
                    device.use = device.type == backend
                return sorted({f'{device.type}:{device.name}' for device in devices})
        except Exception as exc:
            failures.append(f'{backend}: {exc!r}')
    raise RuntimeError('No usable Cycles GPU device: ' + '; '.join(failures))

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
if manifest.get('requires_gpu', False):
    if engine == 'CYCLES':
        try:
            result['gpu_devices'] = enable_cycles_gpu()
        except Exception as exc:
            result['gpu_error'] = repr(exc)
    elif engine in {'BLENDER_EEVEE', 'BLENDER_EEVEE_NEXT'}:
        try:
            scene = bpy.context.scene
            scene.use_nodes = False
            scene.world = None
            scene.render.use_sequencer = False
            scene.render.resolution_x = 1
            scene.render.resolution_y = 1
            scene.render.resolution_percentage = 100
            for obj in list(scene.objects):
                bpy.data.objects.remove(obj, do_unlink=True)
            camera_data = bpy.data.cameras.new('__RenderMesh_Eevee_Probe_Camera')
            camera = bpy.data.objects.new('__RenderMesh_Eevee_Probe_Camera', camera_data)
            scene.collection.objects.link(camera)
            scene.camera = camera
            bpy.ops.render.render(write_still=False)
            result['gpu_devices'] = ['EEVEE_RENDER_CONTEXT']
            try:
                import gpu
                renderer = gpu.platform.renderer_get()
                vendor = gpu.platform.vendor_get()
                if renderer:
                    result['gpu_devices'] = [f'{vendor}:{renderer}']
            except Exception:
                pass
        except Exception as exc:
            result['gpu_error'] = repr(exc)
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

const char* kBatchRenderScript = R"PY(import bpy, ctypes, json, os, re, shutil, sys, time, traceback
args = sys.argv[sys.argv.index('--') + 1:]
output_dir, manifest_path, stop_path, frames_json, targets_json, phase_mutex_name = args[:6]
frames = json.loads(frames_json)
targets = json.loads(targets_json)
scene = bpy.context.scene
completed = []
artifacts = {}
started_at = {}
ended_at = {}
error = ''
primary_dir = os.path.join(output_dir, 'primary')
compositor_root = os.path.join(output_dir, 'compositor')
os.makedirs(primary_dir, exist_ok=True)
kernel32 = ctypes.WinDLL('kernel32', use_last_error=True)
kernel32.CreateMutexW.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_wchar_p]
kernel32.CreateMutexW.restype = ctypes.c_void_p
kernel32.WaitForSingleObject.argtypes = [ctypes.c_void_p, ctypes.c_uint32]
kernel32.WaitForSingleObject.restype = ctypes.c_uint32
kernel32.ReleaseMutex.argtypes = [ctypes.c_void_p]
kernel32.ReleaseMutex.restype = ctypes.c_int
kernel32.CloseHandle.argtypes = [ctypes.c_void_p]
kernel32.CloseHandle.restype = ctypes.c_int
phase_mutex = kernel32.CreateMutexW(None, False, phase_mutex_name)
if not phase_mutex:
    raise OSError(ctypes.get_last_error(), 'Unable to open RenderMesh frame phase mutex')

def enable_cycles_gpu():
    preferences = bpy.context.preferences.addons['cycles'].preferences
    preferred = getattr(preferences, 'compute_device_type', 'NONE')
    backends = []
    for backend in (preferred, 'OPTIX', 'CUDA', 'HIP', 'ONEAPI'):
        if backend and backend != 'NONE' and backend not in backends:
            backends.append(backend)
    failures = []
    for backend in backends:
        try:
            preferences.compute_device_type = backend
            preferences.get_devices()
            devices = [device for device in preferences.devices if device.type == backend]
            if devices:
                for device in preferences.devices:
                    device.use = device.type == backend
                return sorted({f'{device.type}:{device.name}' for device in devices})
        except Exception as exc:
            failures.append(f'{backend}: {exc!r}')
    raise RuntimeError('No usable Cycles GPU device: ' + '; '.join(failures))

def compositor_tree(value):
    return (getattr(value, 'compositing_node_group', None) or
            getattr(value, 'node_tree', None))

def node_at_path(value, path):
    tree = compositor_tree(value)
    node = None
    for index in path:
        if tree is None or not isinstance(index, int) or index < 0 or index >= len(tree.nodes):
            return None
        node = tree.nodes[index]
        tree = getattr(node, 'node_tree', None)
    return node

legacy_tree = compositor_tree(scene)
legacy_nodes = ([node for node in legacy_tree.nodes if node.type == 'OUTPUT_FILE']
                if legacy_tree else [])
if any('node_path' not in target for target in targets) and len(legacy_nodes) != len(targets):
    raise RuntimeError(f'Compositor output count changed: expected {len(targets)}, '
                       f'have {len(legacy_nodes)}')
resolved_nodes = []
for index, target in enumerate(targets):
    exact_locator = 'node_path' in target
    node = (node_at_path(scene, target.get('node_path', []))
            if exact_locator else legacy_nodes[index])
    if (node is None or node.type != 'OUTPUT_FILE' or
            (exact_locator and target.get('required', False) and
             getattr(node, 'mute', False))):
        raise RuntimeError(f'Compositor File Output node changed: {target.get("name", index)}')
    expected_scene = target.get('scene', '')
    if expected_scene and expected_scene != scene.name:
        raise RuntimeError(f'Compositor scene changed: expected {expected_scene}, have {scene.name}')
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
    configured_name = target.get('file_pattern', '')
    if configured_name and hasattr(node, 'file_name'):
        node.file_name = configured_name
    resolved_nodes.append((node, target, exact_locator))

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
        json.dump({'completed': completed, 'artifacts': artifacts,
                   'started_at': started_at, 'ended_at': ended_at, 'error': error}, handle,
                   ensure_ascii=False, indent=2)
    os.replace(temporary, manifest_path)
try:
    if scene.render.engine == 'CYCLES' and getattr(scene.cycles, 'device', 'CPU') == 'GPU':
        print('RenderMesh Cycles GPU: ' + ', '.join(enable_cycles_gpu()))
    for index, frame in enumerate(frames):
        if kernel32.WaitForSingleObject(phase_mutex, 0xffffffff) not in (0, 0x80):
            raise OSError(ctypes.get_last_error(), 'Unable to lock RenderMesh frame phase mutex')
        if os.path.exists(stop_path):
            kernel32.ReleaseMutex(phase_mutex)
            break
        active_frame = int(frame)
        started_at[str(active_frame)] = time.time_ns() // 1000000
        persist()
        print(f'RENDERMESH_FRAME_BEGIN {active_frame}', flush=True)
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
                stem, logical_extension = os.path.splitext(relative)
                frame_pattern = rf'(?<!\d)0*{int(frame)}(?!\d)'
                if re.search(frame_pattern, os.path.basename(stem)) is None:
                    relative = f'{stem}_{int(frame):06d}{logical_extension}'
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
        for _, target, exact_locator in resolved_nodes:
            if not exact_locator or not target.get('required', False):
                continue
            prefix = 'compositor/' + target['id'] + '/'
            if not any(value['logical'].startswith(prefix) for value in frame_artifacts):
                raise RuntimeError(
                    f'File Output node {target.get("name", target["id"])} produced no file '
                    f'for frame {frame}')
        artifacts[str(int(frame))] = frame_artifacts
        completed.append(int(frame))
        ended_at[str(active_frame)] = time.time_ns() // 1000000
        print(f'RENDERMESH_FRAME_END {active_frame} completed', flush=True)
        persist()
        active_frame = None
        kernel32.ReleaseMutex(phase_mutex)
except Exception:
    error = traceback.format_exc()
    if active_frame is not None:
        print(f'RENDERMESH_FRAME_END {active_frame} failed', flush=True)
    persist()
    raise
persist()
kernel32.CloseHandle(phase_mutex)
)PY";

constexpr size_t kMaxFrameLogBytes = 4 * 1024 * 1024;

std::string SanitizeBlenderLog(std::string_view value) {
    std::string clean;
    clean.reserve(std::min(value.size(), kMaxFrameLogBytes));
    for (size_t index = 0; index < value.size(); ++index) {
        unsigned char ch = static_cast<unsigned char>(value[index]);
        if (ch == 0x1b) {
            if (index + 1 < value.size() && value[index + 1] == '[') {
                index += 2;
                while (index < value.size()) {
                    unsigned char code = static_cast<unsigned char>(value[index]);
                    if (code >= 0x40 && code <= 0x7e) break;
                    ++index;
                }
            }
            continue;
        }
        if (ch == '\r' || ch == '\n' || ch == '\t' || ch >= 0x20)
            clean.push_back(static_cast<char>(ch));
    }
    if (clean.size() <= kMaxFrameLogBytes) return clean;
    constexpr size_t headBytes = 256 * 1024;
    constexpr std::string_view notice =
        "\n\n--- RenderMesh: log exceeded 4 MiB; middle section omitted ---\n\n";
    const size_t tailBytes = kMaxFrameLogBytes - headBytes - notice.size();
    return clean.substr(0, headBytes) + std::string(notice) +
        clean.substr(clean.size() - tailBytes);
}

// Cuts the section of one frame out of the package log (the batch script
// prints RENDERMESH_FRAME_BEGIN/END markers). The first frame also keeps the
// Blender start-up output. Writes `destination` only when the text changed
// since `previous`; returns true when it wrote a new snapshot.
bool UpdateFrameLogSnapshot(const std::string& contents, const fs::path& destination,
                            int frame, bool& complete, std::string& previous) {
    complete = false;
    const std::string beginMarker = "RENDERMESH_FRAME_BEGIN " + std::to_string(frame);
    const std::string endMarker = "RENDERMESH_FRAME_END " + std::to_string(frame) + " ";
    const size_t begin = contents.find(beginMarker);
    if (begin == std::string::npos) return false;
    size_t contentStart = contents.find('\n', begin);
    contentStart = contentStart == std::string::npos ? contents.size() : contentStart + 1;
    const size_t end = contents.find(endMarker, contentStart);
    complete = end != std::string::npos;
    const size_t segmentEnd = complete ? end : contents.size();
    std::string segment;
    if (begin == contents.find("RENDERMESH_FRAME_BEGIN ")) segment.append(contents, 0, begin);
    if (segmentEnd > contentStart)
        segment.append(contents, contentStart, segmentEnd - contentStart);
    segment = "RenderMesh Blender log - frame " + std::to_string(frame) +
        (complete ? " - complete\n\n" : " - rendering\n\n") + SanitizeBlenderLog(segment);
    if (segment == previous) return false;
    std::wstring error;
    if (!WriteFileAtomic(destination, segment, error)) return false;
    previous = std::move(segment);
    return true;
}

// Reads the frames listed as completed in the batch manifest.
void CollectCompletedFrames(const json& progress, const fs::path& outputDir,
                            const std::vector<int>& frames,
                            std::map<int, std::vector<RenderArtifact>>& completed,
                            const std::function<void(int, std::vector<RenderArtifact>&)>& added) {
    const json artifactRows = progress.value("artifacts", json::object());
    for (int frame : progress.value("completed", std::vector<int>{})) {
        if (std::find(frames.begin(), frames.end(), frame) == frames.end() ||
            completed.contains(frame))
            continue;
        std::vector<RenderArtifact> values;
        for (const auto& artifact : artifactRows.value(std::to_string(frame), json::array())) {
            const std::string logical = artifact.value("logical", "");
            const fs::path relative = Utf8ToWide(artifact.value("source", ""));
            bool safe = !logical.empty() && relative.is_relative() &&
                !relative.has_root_name() && !relative.has_root_directory();
            for (const auto& component : relative)
                safe = safe && component != L".." && component != L".";
            if (safe && fs::is_regular_file(outputDir / relative))
                values.push_back({logical, outputDir / relative});
        }
        if (values.empty()) continue;
        auto& stored = completed[frame] = std::move(values);
        if (added) added(frame, stored);
    }
}

std::wstring Lowercase(std::wstring value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](wchar_t ch) { return static_cast<wchar_t>(std::towlower(ch)); });
    return value;
}

fs::path ParsePreparedPath(const json& data, const fs::path& prepared, const fs::path& source) {
    return data.value("prepared", false) && fs::is_regular_file(prepared) ? prepared : source;
}

bool CanForceSubmitAfterPreflight(
    bool passed, bool warningsAlreadyIgnored, bool submissionMetadataValid,
    bool sourceAvailable) {
    return !warningsAlreadyIgnored && !passed && submissionMetadataValid && sourceAvailable;
}

class BlenderAdapter final : public RenderAdapter {
public:
    std::string_view Id() const override { return "blender"; }
    std::wstring_view DisplayName() const override { return L"Blender"; }
    std::wstring_view ProjectExtension() const override { return L".blend"; }

    fs::path LocateExecutable(const Config& config) const override {
        std::vector<fs::path> candidates;
        if (!config.blenderPath.empty()) {
            if (Lowercase(config.blenderPath.filename().wstring()) == L"blender-launcher.exe")
                candidates.push_back(config.blenderPath.parent_path() / L"blender.exe");
            candidates.push_back(config.blenderPath);
        }
        candidates.push_back(ExecutableDirectory() / L"Blender" / L"blender.exe");
        wchar_t found[MAX_PATH]{};
        if (SearchPathW(nullptr, L"blender.exe", nullptr, MAX_PATH, found, nullptr) > 0)
            candidates.emplace_back(found);
        for (const wchar_t* variable : {L"ProgramFiles", L"ProgramFiles(x86)"}) {
            wchar_t root[MAX_PATH]{};
            if (!GetEnvironmentVariableW(variable, root, MAX_PATH)) continue;
            std::error_code ec;
            std::vector<fs::path> installs;
            for (const auto& entry :
                 fs::directory_iterator(fs::path(root) / L"Blender Foundation", ec)) {
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

    bool ConfigureExecutable(Config& config, const fs::path& selectedPath,
                             std::wstring& error) const override {
        if (selectedPath.empty()) {
            error = L"请选择 Blender 的 blender.exe";
            return false;
        }
        std::error_code pathError;
        fs::path executable = selectedPath;
        if (fs::is_directory(executable, pathError)) executable /= L"blender.exe";
        executable = fs::absolute(executable, pathError).lexically_normal();
        if (Lowercase(executable.filename().wstring()) == L"blender-launcher.exe")
            executable = executable.parent_path() / L"blender.exe";
        if (pathError || Lowercase(executable.filename().wstring()) != L"blender.exe" ||
            !fs::is_regular_file(executable, pathError)) {
            error = L"所选位置中没有找到 blender.exe";
            return false;
        }
        return SetActiveProfileExecutable(config, executable, error);
    }

    PreflightReport Prepare(const Config& config, const fs::path& dataDir,
                            const fs::path& source, const fs::path& workDir,
                            bool ignoreAssetWarnings, std::atomic<bool>* cancel,
                            bool inspectOnly) const override {
        PreflightReport report;
        report.sourceProject = source;
        report.software = std::string(Id());
        if (_wcsicmp(source.extension().c_str(), L".blend") != 0 ||
            !fs::is_regular_file(source)) {
            report.summary = L"只能提交存在的 .blend 工程文件。";
            return report;
        }
        const fs::path blender = LocateExecutable(config);
        if (blender.empty()) {
            report.summary = L"未找到 Blender。请先安装 Blender，或在设置中选择 blender.exe。";
            return report;
        }
        if (!EnsureBlenderReady(blender, dataDir, cancel, report.summary)) return report;
        std::error_code ignored;
        fs::create_directories(workDir, ignored);
        const fs::path script = workDir / L"prepare.py";
        const fs::path reportPath = workDir / L"prepare.json";
        const fs::path prepared = workDir / L"prepared.blend";
        const fs::path log = workDir / L"prepare.log";
        if (!WriteFileText(script, kPrepareScript)) {
            report.summary = L"无法写入工程检查脚本：" + script.wstring();
            return report;
        }
        // One Blender process reads the settings, checks the files and writes a
        // copy only when one is needed. -noaudio skips audio device start-up.
        const ProcessResult process = RunProcess(
            blender,
            {L"--background", L"--disable-autoexec", L"-noaudio", source.wstring(),
             L"--python", script.wstring(), L"--", reportPath.wstring(), prepared.wstring(),
             ignoreAssetWarnings ? L"1" : L"0", inspectOnly ? L"1" : L"0"},
            log, cancel);
        if (cancel && cancel->load()) {
            report.summary = L"工程读取已取消。";
            return report;
        }
        std::string text;
        if (!process.started || !ReadFileText(reportPath, text)) {
            report.summary = L"Blender 无法读取工程。请查看：" + log.wstring();
            if (!process.error.empty()) report.summary += L"\n" + process.error;
            return report;
        }
        const json data = json::parse(text, nullptr, false);
        if (!data.is_object()) {
            report.summary = L"工程检查报告无效。请查看：" + log.wstring();
            return report;
        }
        ParseReport(data, process, source, prepared, ignoreAssetWarnings, log, report);
        return report;
    }

    bool CheckCompatibility(const Config& config, const fs::path& dataDir,
                            const fs::path& workDir, const std::string& expectedVersion,
                            const std::string& expectedEngine, bool requiresGpu,
                            std::atomic<bool>& stop, std::wstring& summary) const override {
        const fs::path blender = LocateExecutable(config);
        if (blender.empty()) {
            summary = L"Worker 未安装 Blender";
            return false;
        }
        if (!EnsureBlenderReady(blender, dataDir, &stop, summary)) return false;
        std::error_code ignored;
        fs::create_directories(workDir, ignored);
        const fs::path script = workDir / L"compatibility.py";
        const fs::path manifest = workDir / L"required-environment.json";
        const fs::path reportPath = workDir / L"worker-environment.json";
        const fs::path log = workDir / L"compatibility.log";
        WriteFileText(script, kCompatibilityScript);
        WriteFileText(manifest, json{{"version", expectedVersion},
                                     {"render_engine", expectedEngine},
                                     {"requires_gpu", requiresGpu}}.dump());
        // Compatibility depends only on this computer's Blender installation,
        // render engine and GPU; the project itself is not opened. An older
        // Blender could abort while loading a newer file before it reports
        // the actionable version mismatch.
        const ProcessResult process = RunProcess(
            blender,
            {L"--background", L"--disable-autoexec", L"-noaudio", L"--python",
             script.wstring(), L"--", manifest.wstring(), reportPath.wstring()},
            log, &stop);
        std::string text;
        if (!process.started || process.exitCode != 0 || !ReadFileText(reportPath, text)) {
            summary = !process.started
                ? L"Worker 无法启动 Blender 兼容性检查"
                : L"Worker 的 Blender 兼容性检查异常退出（代码 " +
                    std::to_wstring(process.exitCode) + L"）";
            const std::string tail = ReadFileTail(log, 4096);
            summary += tail.empty() ? L"；日志：" + log.wstring() : L"\n" + Utf8ToWide(tail);
            return false;
        }
        const json data = json::parse(text, nullptr, false);
        const std::string version = data.is_object() ? data.value("version", "") : "";
        const auto gpuDevices = data.is_object()
            ? data.value("gpu_devices", std::vector<std::string>{})
            : std::vector<std::string>{};
        if (!data.is_object()) {
            summary = L"Worker 环境报告无效";
            return false;
        }
        if (version != expectedVersion) {
            summary = L"Blender 版本不一致：需要 " + Utf8ToWide(expectedVersion) + L"，本机 " +
                Utf8ToWide(version);
            return false;
        }
        if (!data.value("engine_available", expectedEngine.empty())) {
            summary = L"缺少渲染引擎：" + Utf8ToWide(expectedEngine);
            return false;
        }
        if (requiresGpu && gpuDevices.empty() &&
            (expectedEngine == "CYCLES" || expectedEngine.starts_with("BLENDER_EEVEE"))) {
            summary = expectedEngine == "CYCLES"
                ? L"Worker 没有已启用的 Cycles GPU 渲染设备"
                : L"Worker 没有可供 Eevee 使用的 GPU 图形设备";
            const std::string gpuError = data.value("gpu_error", "");
            if (!gpuError.empty()) summary += L"：" + Utf8ToWide(gpuError);
            return false;
        }
        summary = L"Worker 环境兼容";
        return data.value("pass", false);
    }

    bool RenderPackage(const RenderPackageRequest& request,
                       RenderPackageResult& result) const override;

    bool ConvertPreview(const Config& config, const fs::path& dataDir,
                        const fs::path& image, const fs::path& png,
                        std::atomic<bool>& stop, std::wstring& error) const override {
        if (!fs::is_regular_file(image)) {
            error = L"预览源文件不存在";
            return false;
        }
        const fs::path blender = LocateExecutable(config);
        if (blender.empty()) {
            error = L"未找到 Blender，无法生成该格式的预览";
            return false;
        }
        if (!EnsureBlenderReady(blender, dataDir, &stop, error)) return false;
        std::error_code ignored;
        fs::create_directories(png.parent_path(), ignored);
        fs::remove(png, ignored);
        const fs::path work = dataDir / L"previews";
        fs::create_directories(work, ignored);
        const fs::path script = work / L"generate-preview.py";
        if (!WriteFileText(script, kPreviewScript)) {
            error = L"无法写入预览转换脚本";
            return false;
        }
        const ProcessResult process = RunProcess(
            blender,
            {L"--background", L"--factory-startup", L"--disable-autoexec", L"-noaudio",
             L"--python", script.wstring(), L"--", image.wstring(), png.wstring()},
            work / L"generate-preview.log", &stop);
        if (stop.load()) {
            error = L"预览生成已取消";
            return false;
        }
        if (!process.started || process.exitCode != 0 || !fs::is_regular_file(png)) {
            error = process.error.empty() ? L"Blender 无法生成该图片格式的预览" : process.error;
            return false;
        }
        return true;
    }

private:
    static void ParseReport(const json& data, const ProcessResult& process,
                            const fs::path& source, const fs::path& prepared,
                            bool ignoreAssetWarnings, const fs::path& log,
                            PreflightReport& report) {
        report.passed = data.value("pass", false);
        report.assetWarningsIgnored = data.value("asset_warnings_ignored", false);
        report.validationSkippedByUser = data.value("validation_skipped_by_user", false);
        report.sceneName = data.value("scene_name", "");
        report.softwareVersion = data.value("version", "");
        report.renderEngine = data.value("render_engine", "");
        report.outputFormat = data.value("render_format", "");
        report.outputExtension = data.value("use_file_extension", true)
            ? data.value("file_extension", "") : std::string{};
        report.requiresGpu = data.value("requires_gpu", false);
        report.warnings = data.value("warnings", std::vector<std::string>{});
        report.packedCount = data.value("packed", 0);
        report.bundledCount = data.value("bundled", 0);
        report.pendingChanges =
            data.value("pack_required", 0) + data.value("rewrite_required", 0);
        report.frameStart = data.value("frame_start", 0);
        report.frameEnd = data.value("frame_end", 0);
        report.frameStep = data.value("frame_step", 0);
        if (process.exitCode != 0) {
            report.passed = false;
            report.warnings.push_back(
                "Blender exited after writing the report; exit code " +
                std::to_string(process.exitCode));
        }
        bool metadataValid = true;
        if (report.frameStart < kBlenderFrameMin || report.frameEnd < report.frameStart ||
            report.frameEnd > kBlenderFrameMax || report.frameStep < 1 ||
            report.frameStep > 1000000) {
            metadataValid = false;
            report.warnings.push_back("The project frame range is invalid");
        }
        if (report.outputFormat.empty() || !IsSafeOutputExtension(report.outputExtension)) {
            metadataValid = false;
            report.warnings.push_back("The project output format has no safe file extension");
        }
        if (!metadataValid) report.passed = false;
        const json primaryOutputs = data.value("primary_outputs", json::object());
        for (const auto& [frameText, pathValue] : primaryOutputs.items()) {
            const int frame = std::atoi(frameText.c_str());
            if (!pathValue.is_string()) continue;
            fs::path path = Utf8ToWide(pathValue.get<std::string>());
            if (frame >= kBlenderFrameMin && frame <= kBlenderFrameMax && path.is_absolute())
                report.primaryOutputs[frame] = std::move(path);
        }
        for (const auto& row : data.value("compositor_outputs", json::array())) {
            OutputTarget target;
            if (ParseOutputTargetMetadata(row, target)) {
                target.directory = Utf8ToWide(row.value("directory", ""));
                if (target.directory.is_absolute()) {
                    report.compositorOutputs.push_back(std::move(target));
                    continue;
                }
            }
            if (!ignoreAssetWarnings) report.passed = false;
            report.warnings.push_back("Invalid compositor output metadata or destination");
        }
        auto dependencies = [](const json& values) {
            std::vector<DependencyItem> rows;
            for (const auto& item : values)
                rows.push_back({item.value("kind", "unknown"), item.value("owner", ""),
                                item.value("path", "")});
            return rows;
        };
        report.missing = dependencies(data.value("missing", json::array()));
        report.unpacked = dependencies(data.value("unpacked", json::array()));
        const std::string validationError = data.value("validation_error", "");
        const bool sourceAvailable = fs::is_regular_file(source);
        report.preparedProject = ParsePreparedPath(data, prepared, source);
        report.preparedCopy = report.preparedProject == prepared;
        if (ignoreAssetWarnings && metadataValid && sourceAvailable) {
            report.passed = true;
            report.assetWarningsIgnored = true;
            report.validationSkippedByUser = true;
        }
        report.canIgnoreAssetWarnings = CanForceSubmitAfterPreflight(
            report.passed, report.assetWarningsIgnored, metadataValid, sourceAvailable);
        const auto& rows = report.missing.empty() ? report.unpacked : report.missing;
        if (report.passed) {
            size_t outputItems = 0;
            for (const auto& target : report.compositorOutputs)
                outputItems += target.outputs.size();
            report.summary = report.assetWarningsIgnored
                ? L"已按用户选择跳过全部预检警告，Blender "
                : L"工程检查通过：Blender ";
            report.summary += Utf8ToWide(report.softwareVersion) + L"，输出格式 " +
                Utf8ToWide(report.outputFormat) + L"。\n工程帧范围 " +
                std::to_wstring(report.frameStart) + L"–" + std::to_wstring(report.frameEnd) +
                L"，File Output 节点 " + std::to_wstring(report.compositorOutputs.size()) +
                L" 个，输出项 " + std::to_wstring(outputItems) + L" 个。";
            const int pending = report.pendingChanges;
            if (report.preparedCopy)
                report.summary += L"\n已打包 " + std::to_wstring(report.packedCount) +
                    L" 项外部素材。";
            else if (pending > 0 && report.packedCount == 0)
                report.summary += L"\n提交时会打包或修正 " + std::to_wstring(pending) +
                    L" 项外部素材。";
            else
                report.summary += L"\n工程无需重新打包，将直接使用原文件快照。";
            if (report.bundledCount > 0)
                report.summary += L"\n同级 tex 文件夹中的 " +
                    std::to_wstring(report.bundledCount) + L" 项素材会随工程包传输。";
            if (report.assetWarningsIgnored && !rows.empty()) {
                report.summary += L"\n已保留 " + std::to_wstring(rows.size()) +
                    L" 项素材警告；任务会按你的选择直接进入下一步。";
                for (size_t i = 0; i < std::min<size_t>(rows.size(), 5); ++i)
                    report.summary += L"\n• " + Utf8ToWide(rows[i].path);
            }
            return;
        }
        report.summary = L"预检未通过：缺失 " + std::to_wstring(report.missing.size()) +
            L" 项，无法打包 " + std::to_wstring(report.unpacked.size()) + L" 项。";
        for (size_t i = 0; i < std::min<size_t>(rows.size(), 5); ++i)
            report.summary += L"\n• " + Utf8ToWide(rows[i].path);
        if (!validationError.empty()) {
            if (rows.empty())
                report.summary += L"\n工程准备失败；可能是插件、节点或工程数据错误。";
            report.summary += L"\n详见：" + log.wstring();
        }
    }
};

bool BlenderAdapter::RenderPackage(const RenderPackageRequest& request,
                                   RenderPackageResult& result) const {
    auto& completed = result.completed;
    auto& frameLogs = result.frameLogs;
    auto& error = result.error;
    completed.clear();
    frameLogs.clear();
    error.clear();
    const auto& frames = request.frames;
    const fs::path& outputDir = request.outputDir;
    std::atomic<bool> neverStop = false;
    std::atomic<bool> localFinish = false;
    std::atomic<bool>& stopRequested = request.stop ? *request.stop : neverStop;
    std::atomic<bool>& finishCurrentThenStop =
        request.finishCurrentThenStop ? *request.finishCurrentThenStop : localFinish;
    std::atomic<bool>* abortBeforeFirstFrame = request.abortBeforeFirstFrame;
    if (frames.empty() || frames.size() > 20 ||
        !std::all_of(frames.begin(), frames.end(), [](int frame) {
            return frame >= kBlenderFrameMin && frame <= kBlenderFrameMax;
        }) || request.outputFormat.empty() || !IsSafeOutputExtension(request.outputExtension)) {
        error = L"渲染任务包参数无效";
        return false;
    }
    const fs::path blender = LocateExecutable(request.config);
    if (blender.empty()) {
        error = L"未找到 Blender";
        return false;
    }
    std::error_code ignored;
    fs::create_directories(outputDir, ignored);
    const fs::path script = outputDir / L"render-package.py";
    const fs::path manifest = outputDir / L"render-package.json";
    const fs::path stopMarker = outputDir / L"stop-after-current";
    const fs::path log = outputDir / L"render-package.log";

    // First-run initialisation can take seconds; an origin that stops the
    // package meanwhile must be able to abort it.
    std::atomic<bool> readinessFinished = false;
    std::atomic<bool> readinessStop = false;
    std::thread readinessCancellation([&] {
        while (!readinessFinished.load()) {
            if (stopRequested.load() ||
                (abortBeforeFirstFrame && abortBeforeFirstFrame->load())) {
                readinessStop = true;
                return;
            }
            Sleep(25);
        }
    });
    const bool blenderReady = EnsureBlenderReady(blender, outputDir, &readinessStop, error);
    readinessFinished = true;
    readinessCancellation.join();
    if (!blenderReady || stopRequested.load() ||
        (abortBeforeFirstFrame && abortBeforeFirstFrame->load())) {
        if (abortBeforeFirstFrame && abortBeforeFirstFrame->load() && !stopRequested.load()) {
            finishCurrentThenStop = true;
            error = L"任务包在首帧开始前已停止";
        }
        return false;
    }
    if (!WriteFileText(script, kBatchRenderScript)) {
        error = L"无法写入批量渲染脚本";
        return false;
    }
    fs::remove(manifest, ignored);

    // The batch script holds this mutex from a frame's start until its end.
    // A hard abort is only allowed while the host owns it (no frame started).
    const std::wstring phaseMutexName =
        L"Local\\RenderMesh-frame-phase-" + Utf8ToWide(NewRandomHex(12));
    HANDLE phaseMutex = CreateMutexW(nullptr, FALSE, phaseMutexName.c_str());
    if (!phaseMutex) {
        error = L"无法创建帧阶段同步对象：" + LastErrorMessage();
        return false;
    }

    std::atomic<bool> processFinished = false;
    std::atomic<bool> processStopRequested = false;
    std::thread stopMarkerThread([&] {
        while (!processFinished.load()) {
            if (stopRequested.load()) {
                processStopRequested = true;
                return;
            }
            if (fs::is_regular_file(stopMarker)) {
                finishCurrentThenStop = true;
                if (abortBeforeFirstFrame && abortBeforeFirstFrame->load()) {
                    const DWORD phase = WaitForSingleObject(phaseMutex, 0);
                    if (phase == WAIT_OBJECT_0 || phase == WAIT_ABANDONED) {
                        processStopRequested = true;
                        while (!processFinished.load()) Sleep(5);
                        ReleaseMutex(phaseMutex);
                    }
                }
                return;
            }
            if (request.stopAfterCurrentOnActivity &&
                !IsComputerIdleForSharedWork(IdleSeconds(), request.config.idleAfterSeconds)) {
                finishCurrentThenStop = true;
                // Detection and marker publication happen in the same thread, so
                // no second polling interval can start another frame after
                // activity was observed.
                WriteFileText(stopMarker, "stop");
                return;
            }
            Sleep(50);
        }
    });

    json outputTargets = json::array();
    for (const auto& target : request.outputs)
        outputTargets.push_back(OutputTargetLocatorMetadata(target));
    std::set<int> startedCallbacks;
    std::mutex startedCallbacksMutex;
    auto reportFrameStarts = [&](const json& progress) {
        const json startedRows = progress.value("started_at", json::object());
        for (int frame : frames) {
            const int64_t started = startedRows.value(std::to_string(frame), int64_t{0});
            if (started <= 0 || !request.frameStarted) continue;
            std::scoped_lock lock(startedCallbacksMutex);
            if (startedCallbacks.insert(frame).second)
                request.frameStarted(frame, std::chrono::system_clock::time_point(
                    std::chrono::milliseconds(started)));
        }
    };
    auto readManifest = [&] {
        std::string text;
        return ReadFileText(manifest, text) ? json::parse(text, nullptr, false)
                                            : json(json::value_t::discarded);
    };
    std::map<int, std::string> lastFrameLogs;
    auto lastLogScan = std::chrono::steady_clock::time_point{};
    auto collectProgress = [&](bool final) {
        const auto now = std::chrono::steady_clock::now();
        if (final || now - lastLogScan >= std::chrono::seconds(1)) {
            lastLogScan = now;
            const std::string contents = ReadFileText(log);
            for (int frame : frames) {
                const fs::path frameLog = outputDir / (L"frame-" + std::to_wstring(frame) + L".log");
                bool logComplete = false;
                if (UpdateFrameLogSnapshot(contents, frameLog, frame, logComplete,
                                           lastFrameLogs[frame])) {
                    frameLogs[frame] = frameLog;
                    if (request.frameLogUpdated) request.frameLogUpdated(frame, frameLog, logComplete);
                }
            }
        }
        const json progress = readManifest();
        if (!progress.is_object()) return;
        reportFrameStarts(progress);
        const json startedRows = progress.value("started_at", json::object());
        const json endedRows = progress.value("ended_at", json::object());
        CollectCompletedFrames(progress, outputDir, frames, completed,
            [&](int frame, std::vector<RenderArtifact>& artifacts) {
                FrameRenderTiming timing;
                const int64_t started = startedRows.value(std::to_string(frame), int64_t{0});
                const int64_t ended = endedRows.value(std::to_string(frame), int64_t{0});
                if (started > 0 && ended >= started) {
                    timing.startedAt = std::chrono::system_clock::time_point(
                        std::chrono::milliseconds(started));
                    timing.endedAt = std::chrono::system_clock::time_point(
                        std::chrono::milliseconds(ended));
                }
                if (request.frameCompleted) request.frameCompleted(frame, artifacts, timing);
            });
        const std::string scriptError = progress.value("error", "");
        if (!scriptError.empty()) error = Utf8ToWide(scriptError);
    };
    // Frame starts are polled faster than results so even a very short Eevee
    // frame reports its start before its result.
    std::thread frameStartThread([&] {
        while (!processFinished.load()) {
            if (const json progress = readManifest(); progress.is_object())
                reportFrameStarts(progress);
            Sleep(20);
        }
    });
    const ProcessResult process = RunProcess(
        blender,
        {L"--background", L"--disable-autoexec", request.project.wstring(), L"--python",
         script.wstring(), L"--", outputDir.wstring(), manifest.wstring(),
         stopMarker.wstring(), Utf8ToWide(json(frames).dump()),
         Utf8ToWide(outputTargets.dump()), phaseMutexName},
        log, &processStopRequested, std::chrono::milliseconds::zero(),
        [&] { collectProgress(false); });
    processFinished = true;
    frameStartThread.join();
    stopMarkerThread.join();
    CloseHandle(phaseMutex);
    collectProgress(true);
    if (frameLogs.empty()) {
        // Blender failed before the first frame marker: keep its whole output.
        std::string fallback = "RenderMesh Blender log - frame " + std::to_string(frames.front()) +
            " - process output\n\n" + SanitizeBlenderLog(ReadFileText(log));
        const fs::path path = outputDir / (L"frame-" + std::to_wstring(frames.front()) + L".log");
        std::wstring writeError;
        if (WriteFileAtomic(path, fallback, writeError)) {
            frameLogs[frames.front()] = path;
            if (request.frameLogUpdated) request.frameLogUpdated(frames.front(), path, true);
        }
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
    if (!ValidateFramePackageCompletion(
            frames.size(), completed.size(), finishCurrentThenStop.load(), error)) {
        const std::string tail = ReadFileTail(log, 4096);
        error += tail.empty() ? L"；日志：" + log.wstring() : L"\n" + Utf8ToWide(tail);
        return false;
    }
    return true;
}

}  // namespace

const RenderAdapter& BlenderRenderAdapter() {
    static const BlenderAdapter adapter;
    return adapter;
}

std::vector<const RenderAdapter*> RenderAdapters() {
    return {&BlenderRenderAdapter()};
}

const RenderAdapter* FindRenderAdapter(std::string_view id) {
    if (id.empty()) return &BlenderRenderAdapter();
    for (const RenderAdapter* adapter : RenderAdapters())
        if (adapter->Id() == id) return adapter;
    return nullptr;
}

const RenderAdapter* RenderAdapterForProject(const fs::path& project) {
    const std::wstring extension = Lowercase(project.extension().wstring());
    for (const RenderAdapter* adapter : RenderAdapters())
        if (adapter->ProjectExtension() == extension) return adapter;
    return nullptr;
}

#ifdef RM_DEV_TOOLS
bool RunBlenderReadinessSelfTest(std::wstring& report) {
    const fs::path script = L"C:\\RenderMesh Test\\initialize-blender.py";
    const fs::path marker = L"C:\\RenderMesh Test\\initialize-blender.ready";
    const std::vector<std::wstring> expected = {
        L"--background", L"--factory-startup", L"--disable-autoexec", L"-noaudio",
        L"--python-exit-code", L"95", L"--python", script.wstring(), L"--",
        marker.wstring()};
    const std::string bootstrap = kBlenderBootstrapScript;
    const std::string batch = kBatchRenderScript;
    auto contains = [](const std::string& text, const char* needle) {
        return text.find(needle) != std::string::npos;
    };
    auto strictGpuSelection = [&](const std::string& value) {
        return contains(value, "device.type == backend") &&
            contains(value, "device.use = device.type == backend");
    };
    if (contains(kPrepareScript, "bpy.ops.render.render") ||
        !contains(kPrepareScript, "'frame_start': frame_start") ||
        !contains(kPrepareScript, "'scene_name': scene.name") ||
        !contains(kPrepareScript, "relative_remap=False") ||
        !contains(kPrepareScript, "'validation_skipped_by_user'") ||
        !contains(kPrepareScript, "discover_compositor_outputs(scene)")) {
        report = L"Blender prepare script contract self-test failed";
        return false;
    }
    if (!CanForceSubmitAfterPreflight(false, false, true, true) ||
        CanForceSubmitAfterPreflight(true, false, true, true) ||
        CanForceSubmitAfterPreflight(false, true, true, true) ||
        CanForceSubmitAfterPreflight(false, false, false, true) ||
        CanForceSubmitAfterPreflight(false, false, true, false)) {
        report = L"Missing-asset warning override boundary self-test failed";
        return false;
    }
    if (TaskStateLabel(TaskState::Assigned) != L"排队中" ||
        TaskStateLabel(TaskState::Rendering) != L"渲染中" ||
        BlenderBootstrapArguments(script, marker) != expected ||
        !contains(bootstrap, "userpref.blend") ||
        !contains(bootstrap, "os.path.isfile(preferences)") ||
        !contains(bootstrap, "bpy.ops.wm.save_userpref()") ||
        !contains(bootstrap, "os.replace(temporary, ready_path)") ||
        !contains(bootstrap, "RENDERMESH_BLENDER_READY") ||
        !contains(batch, "node_at_path") || !contains(batch, "produced no file") ||
        !contains(batch, "RENDERMESH_FRAME_BEGIN") || !contains(batch, "RENDERMESH_FRAME_END") ||
        !contains(batch, "started_at[str(active_frame)]") ||
        !contains(batch, "ended_at[str(active_frame)]") ||
        !contains(batch, "WaitForSingleObject(phase_mutex") ||
        !contains(batch, "ReleaseMutex(phase_mutex)") ||
        !contains(batch, "if os.path.exists(stop_path):") ||
        contains(batch, "if index and os.path.exists(stop_path):") ||
        !strictGpuSelection(kCompatibilityScript) || !strictGpuSelection(batch)) {
        report = L"Blender first-run readiness launch contract self-test failed";
        return false;
    }
    const fs::path root = fs::temp_directory_path() /
        Utf8ToWide("RenderMesh-frame-log-self-test-" + NewRandomHex(8));
    const std::string packageLog =
        "Blender 5.0 startup\nRENDERMESH_FRAME_BEGIN 1\n"
        "Fra:1 | Rendering 1 / 64\n\x1b[31mwarning\x1b[0m\n"
        "RENDERMESH_FRAME_END 1 completed\n"
        "RENDERMESH_FRAME_BEGIN 2\nFra:2 | Rendering 64 / 64\n"
        "RENDERMESH_FRAME_END 2 completed\n";
    bool firstComplete = false, secondComplete = false;
    std::string firstPrevious, secondPrevious;
    const bool firstChanged = UpdateFrameLogSnapshot(
        packageLog, root / L"frame-1.log", 1, firstComplete, firstPrevious);
    const bool secondChanged = UpdateFrameLogSnapshot(
        packageLog, root / L"frame-2.log", 2, secondComplete, secondPrevious);
    const std::string firstText = ReadFileText(root / L"frame-1.log");
    const std::string secondText = ReadFileText(root / L"frame-2.log");
    const bool unchanged = !UpdateFrameLogSnapshot(
        packageLog, root / L"frame-1.log", 1, firstComplete, firstPrevious);
    std::error_code ignored;
    fs::remove_all(root, ignored);
    if (!firstChanged || !secondChanged || !firstComplete || !secondComplete || !unchanged ||
        !contains(firstText, "Blender 5.0 startup") || !contains(firstText, "Fra:1") ||
        contains(firstText, "\x1b") || !contains(secondText, "Fra:2") ||
        contains(secondText, "Blender 5.0 startup")) {
        report = L"Per-frame Blender log extraction self-test failed";
        return false;
    }
    return true;
}
#endif  // RM_DEV_TOOLS

}  // namespace rm
