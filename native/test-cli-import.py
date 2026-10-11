"""End-to-end test of the command line and the single-pass project import.

Usage: python test-cli-import.py <RenderMesh.exe> <blender.exe>

Builds Blender fixtures, then drives RenderMesh only through its command line:
inspect, serve, status, submit, jobs, job, test, start, stop, resume, retry,
delete and an unknown command. It checks
that projects are packed only when needed, that tex-folder projects keep
relative paths in the bundle, that the preflight work folders are removed and
that a job renders with the real Blender.
"""
import json
import os
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))


def free_port():
    with socket.socket() as probe:
        probe.bind(('127.0.0.1', 0))
        return probe.getsockname()[1]


class Cli:
    def __init__(self, exe, data_dir):
        self.exe = exe
        self.data_dir = data_dir

    def run(self, *args, expect=0, timeout=900):
        command = [self.exe, *args, '--json', '--data-dir', self.data_dir]
        started = time.time()
        result = subprocess.run(command, capture_output=True, timeout=timeout)
        elapsed = time.time() - started
        text = result.stdout.decode('utf-8', 'replace').strip()
        try:
            value = json.loads(text.splitlines()[-1]) if text else {}
        except json.JSONDecodeError as error:
            raise SystemExit(f'{args}: output is not JSON: {text!r}') from error
        if result.returncode != expect:
            raise SystemExit(f'{args}: exit {result.returncode}, expected {expect}: {value}')
        value['_seconds'] = elapsed
        return value


def read_bundle(path):
    entries = {}
    with open(path, 'rb') as handle:
        if handle.read(8) != b'RMPKG1\0\0':
            raise SystemExit('bundle magic is wrong')
        (count,) = struct.unpack('<I', handle.read(4))
        for _ in range(count):
            path_size, size = struct.unpack('<IQ', handle.read(12))
            name = handle.read(path_size).decode('utf-8')
            entries[name] = handle.read(size)
    return entries


def blender_paths(blender, blend):
    script = ("import bpy, json\n"
              "print('PATHS=' + json.dumps({i.name: [i.filepath, bool(i.packed_file)] "
              "for i in bpy.data.images}))\n")
    result = subprocess.run([blender, '--background', '--factory-startup', blend,
                             '--python-expr', script], capture_output=True, timeout=300)
    for line in result.stdout.decode('utf-8', 'replace').splitlines():
        if line.startswith('PATHS='):
            return json.loads(line[6:])
    raise SystemExit(f'Blender could not read {blend}')


def wait_stage(cli, job_id, stages, timeout=600):
    deadline = time.time() + timeout
    while time.time() < deadline:
        job = cli.run('job', job_id)['job']
        if job['stage'] in stages:
            return job
        time.sleep(1)
    raise SystemExit(f'job {job_id} did not reach {stages}: {job}')


def main():
    exe, blender = os.path.abspath(sys.argv[1]), os.path.abspath(sys.argv[2])
    root = tempfile.mkdtemp(prefix='RenderMesh-cli-')
    serve = None
    try:
        fixtures = os.path.join(root, 'fixtures')
        subprocess.run([blender, '--background', '--factory-startup', '--python',
                        os.path.join(HERE, 'tests', 'make_import_fixtures.py'), '--',
                        fixtures, '40'], check=True, capture_output=True, timeout=600)
        data = os.path.join(root, 'data')
        os.makedirs(data)
        with open(os.path.join(data, 'config.json'), 'w', encoding='utf-8') as handle:
            json.dump({'blender_path': blender, 'http_port': free_port(),
                       'discovery_port': free_port(), 'share_enabled': False}, handle)
        cli = Cli(exe, data)

        assert cli.run('version')['version']
        assert 'COMMANDS' in cli.run('help')['help']
        cli.run('frobnicate', expect=2)

        plain = cli.run('inspect', os.path.join(fixtures, 'plain', 'plain.blend'))
        assert plain['pass'] and plain['pending_changes'] == 0, plain
        assert plain['frame_start'] == 1 and plain['frame_end'] == 3, plain
        external = cli.run('inspect', os.path.join(fixtures, 'external', 'external.blend'))
        assert external['pass'] and external['pending_changes'] == 1, external
        tex = cli.run('inspect', os.path.join(fixtures, 'texproject', 'shot.blend'))
        assert tex['pass'] and tex['bundled'] == 2 and tex['pending_changes'] == 2, tex
        missing_path = os.path.join(fixtures, 'missing', 'missing.blend')
        missing = cli.run('inspect', missing_path, expect=1)
        assert not missing['pass'] and missing['can_ignore_asset_warnings'], missing
        assert missing['missing'], missing
        forced = cli.run('inspect', missing_path, '--ignore-warnings')
        assert forced['pass'] and forced['validation_skipped_by_user'], forced

        cli.run('status', expect=3)
        serve = subprocess.Popen([exe, 'serve', '--data-dir', data],
                                 stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        deadline = time.time() + 30
        while True:
            result = subprocess.run([exe, 'status', '--json', '--data-dir', data],
                                    capture_output=True, timeout=30)
            if result.returncode == 0:
                break
            if time.time() > deadline:
                raise SystemExit('serve did not start')
            time.sleep(0.5)
        status = json.loads(result.stdout.decode('utf-8'))
        assert status['node']['renderer_ready'], status
        storage = status['node']['project_storage']

        jobs = {}
        for name, path, extra in [
                ('plain', os.path.join(fixtures, 'plain', 'plain.blend'), []),
                ('external', os.path.join(fixtures, 'external', 'external.blend'), []),
                ('tex', os.path.join(fixtures, 'texproject', 'shot.blend'), []),
                ('missing', missing_path, ['--ignore-warnings'])]:
            submitted = cli.run('submit', path, '--name', name, '--wait', *extra)
            job = submitted['job']
            assert job['stage'] == 'ready', submitted
            jobs[name] = job['id']
            print(f'submit {name}: {submitted["_seconds"]:.1f}s')
        cli.run('submit', missing_path, expect=1)

        preflight = os.path.join(storage, 'preflight')
        assert not os.path.isdir(preflight) or not os.listdir(preflight), os.listdir(preflight)
        job_dir = lambda key: os.path.join(storage, 'jobs', jobs[key])
        plain_copy = os.path.join(job_dir('plain'), 'source.blend')
        assert os.path.getsize(plain_copy) == os.path.getsize(
            os.path.join(fixtures, 'plain', 'plain.blend')), 'plain copy differs'
        packed = blender_paths(blender, os.path.join(job_dir('external'), 'source.blend'))
        assert all(state[1] for state in packed.values()), packed

        bundle = read_bundle(os.path.join(job_dir('tex'), 'project.rmp'))
        assert set(bundle) == {'source.blend', 'tex/albedo.png', 'tex/maps/rough.png'}, bundle
        extracted = os.path.join(root, 'tex-extracted')
        for name, content in bundle.items():
            target = os.path.join(extracted, *name.split('/'))
            os.makedirs(os.path.dirname(target), exist_ok=True)
            with open(target, 'wb') as handle:
                handle.write(content)
        images = blender_paths(blender, os.path.join(extracted, 'source.blend'))
        images = {name: [path.replace(chr(92), '/'), packed]
                  for name, (path, packed) in images.items()}
        assert images['albedo.png'] == ['//tex/albedo.png', False], images
        assert images['rough.png'] == ['//tex/maps/rough.png', False], images
        assert images['decal.png'][1], images

        assert len(cli.run('jobs')['jobs']) == 4
        for key in ('plain', 'tex'):
            cli.run('test', jobs[key], '--frames', '1-2', '--test-frames', '1')
            wait_stage(cli, jobs[key], {'test_completed', 'test_failed'})
            job = cli.run('job', jobs[key])['job']
            assert job['stage'] == 'test_completed', job
            cli.run('start', jobs[key])
            job = wait_stage(cli, jobs[key], {'completed', 'failed'})
            assert job['stage'] == 'completed', job
            outputs = [frame['output'] for frame in job['frames']]
            assert all(os.path.isfile(path) for path in outputs), outputs
        cli.run('start', jobs['plain'], expect=1)
        # The "external" job is still idle in stage "ready".
        stopped = cli.run('stop', jobs['external'])['job']
        assert stopped['stage'] == 'stopped', stopped
        assert stopped['frames_stopped'] == stopped['frames_total'] > 0, stopped
        cli.run('retry', jobs['external'], expect=1)
        resumed = cli.run('resume', jobs['external'])['job']
        assert resumed['stage'] in {'testing', 'rendering'}, resumed
        cli.run('resume', jobs['external'], expect=1)
        assert cli.run('stop', jobs['external'])['job']['stage'] == 'stopped'
        cli.run('delete', jobs['missing'])
        assert not os.path.exists(job_dir('missing'))
        cli.run('job', jobs['missing'], expect=1)
        print('CLI_IMPORT_OK')
    finally:
        if serve:
            serve.terminate()
            serve.wait(timeout=30)
        shutil.rmtree(root, ignore_errors=True)


if __name__ == '__main__':
    main()
