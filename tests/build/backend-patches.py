from pathlib import Path
import hashlib
import json
import os
import subprocess
import sys
import tempfile

root = Path(__file__).resolve().parents[2]
work = Path(sys.argv[1]).resolve()
source = Path(sys.argv[2]).resolve()
assert work.is_relative_to(root / '.build')
work.mkdir(parents=True, exist_ok=True)
lock = json.loads((root / 'build/dependencies.lock.json').read_text())
index = source / '.git/index'
index_before = index.read_bytes()
for patch in lock['libdatachannel']['patches']:
    assert hashlib.sha256((root / patch['path']).read_bytes()).hexdigest() == patch['sha256']

for mode, expected in [('hash', 'backend patch hash mismatch'),
                       ('base', 'backend base revision mismatch'),
                       ('dirty', 'backend base source is dirty')]:
    with tempfile.TemporaryDirectory(prefix=mode + '-', dir=work) as temporary:
        fixture = Path(temporary)
        fixture_source = fixture / '.build/source'
        fixture_source.mkdir(parents=True)
        # source is read-only; only this disposable worktree is intentionally incomplete
        (fixture_source / '.git').write_text('gitdir: ' + str(source / '.git') + '\n')
        fixture_lock = json.loads(json.dumps(lock))
        if mode == 'base':
            fixture_lock['libdatachannel']['commit'] = '0' * 40
        for patch in fixture_lock['libdatachannel']['patches']:
            target = fixture / patch['path']
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes((root / patch['path']).read_bytes())
        if mode == 'hash':
            (fixture / fixture_lock['libdatachannel']['patches'][0]['path']).write_bytes(b'invalid patch\n')
        (fixture / 'build/dependencies.lock.json').write_text(json.dumps(fixture_lock))
        result = subprocess.run(['cmake', '-DROOT=' + str(fixture),
                                 '-DSOURCE=' + str(fixture_source),
                                 '-DDESTINATION=' + str(fixture / '.build/patched'),
                                 '-P', str(root / 'build/patches/prepare-backend.cmake')],
                                text=True, capture_output=True)
        assert result.returncode != 0 and expected in result.stderr, result.stdout + result.stderr
        assert not (fixture / '.build/patched').exists()
        print('PASS rejection before copying:', mode)
print('PASS locked patch hashes and source validation')
assert index.read_bytes() == index_before, 'source index changed during read-only validation'
print('PASS source index preserved byte-for-byte')
