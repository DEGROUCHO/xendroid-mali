import hashlib, json, os, shutil, subprocess, sys, zipfile
from pathlib import Path
from datetime import datetime

work = Path(__file__).resolve().parent
inventory = json.loads((work / 'disk-inventory.json').read_text())
lab = Path('/path/to/xendroid-ultimate-mali/lab')
lock = lab / 'build/BUILD.lock'
owner = 'verified generated-cache cleanup'
apply = '--apply' in sys.argv

def digest(p):
    h = hashlib.sha256()
    with p.open('rb') as f:
        for b in iter(lambda: f.read(1024*1024), b''): h.update(b)
    return h.hexdigest()

def free(): return shutil.disk_usage('/System/Volumes/Data').free

with lock.open('x') as f: f.write(owner)
try:
    processes = subprocess.check_output(['ps', '-axo', 'command'], text=True)
    assert not any(x in processes for x in ['org.gradle.launcher.daemon.bootstrap.GradleDaemon', 'GradleWrapperMain', 'ninja -C']), 'build active'
    manifest = {'observed_at': datetime.now().isoformat(), 'free_before': free(), 'entries': []}
    for candidate in inventory['candidates']:
        for e in candidate['entries']:
            p = Path(e['path'])
            if not p.exists(): continue
            assert not p.is_symlink()
            if p.is_dir():
                assert p.parent == Path('~/.gradle/caches/8.11.1/transforms')
                files = [f for f in p.rglob('*') if f.is_file()]
                assert all(not f.is_symlink() for f in p.rglob('*'))
                assert sum(f.stat().st_size for f in files) == e['logical_bytes'], 'cache changed: '+str(p)
                jars = [f for f in files if f.name == 'classes.jar']
                assert jars and any(f.name == 'libe.so' for f in files)
                with zipfile.ZipFile(jars[0]) as z:
                    assert any(n.startswith('xendroid/emulator/') for n in z.namelist())
                manifest['entries'].append({'path': str(p), 'kind': 'expanded XenDroid AAR cache', 'bytes': e['logical_bytes'], 'files': [{'path': str(f.relative_to(p)), 'bytes': f.stat().st_size, 'sha256': digest(f)} for f in files]})
            else:
                assert (p.name == 'libe-before.so' and str(p.parent).startswith('/tmp/xd-safe-build.')) or (p.name == 'libe.so' and str(p.parent).startswith('/tmp/xd-ui-build.'))
                assert str(p.parent) not in candidate['preserve_latest']
                assert p.stat().st_size == e['logical_bytes'] and digest(p) == e['sha256']
                # Two retained byte-identical native backup copies must still exist.
                for keep in candidate['preserve_latest']:
                    assert digest(Path(keep)/p.name) == e['sha256']
                manifest['entries'].append({'path': str(p), 'kind': 'byte-identical temporary native backup', 'bytes': e['logical_bytes'], 'sha256': e['sha256']})
    manifest['logical_bytes'] = sum(e['bytes'] for e in manifest['entries'])
    (work/'cleanup-preview.json').write_text(json.dumps(manifest, indent=2))
    print('Verified preview:', len(manifest['entries']), 'entries;', manifest['logical_bytes'], 'logical bytes', flush=True)
    if apply:
        for e in manifest['entries']:
            p = Path(e['path'])
            if p.is_dir(): shutil.rmtree(p)
            else: p.unlink()
        manifest['free_after'] = free()
        manifest['actual_free_delta'] = manifest['free_after']-manifest['free_before']
        manifest['all_selected_removed'] = all(not Path(e['path']).exists() for e in manifest['entries'])
        (work/'cleanup-result.json').write_text(json.dumps(manifest, indent=2))
        print('Removed verified generated entries; actual free delta:', manifest['actual_free_delta'], flush=True)
finally:
    if lock.exists() and lock.read_text() == owner: lock.unlink()
