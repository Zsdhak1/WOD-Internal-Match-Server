"""Verify all three standalone Windows x64 packages with no developer PATH.
Runs dependency-only GUI probes and an isolated HTTP fixture; never starts a
real match or uses a real results DB. stdout of web service is deliberately
not captured because it prints a temporary generated admin password.
"""
import argparse
import json
import os
from pathlib import Path
import socket
import struct
import subprocess
import tempfile
import time
from urllib.request import urlopen
import zipfile


def x64(path):
    with path.open('rb') as f:
        assert f.read(2) == b'MZ', f'Not a PE file: {path}'
        f.seek(0x3c)
        offset = struct.unpack('<I', f.read(4))[0]
        f.seek(offset)
        assert f.read(4) == b'PE\0\0'
        assert struct.unpack('<H', f.read(2))[0] == 0x8664, f'Not x64: {path}'


def verify(root):
    environment = os.environ.copy()
    environment['PATH'] = str(Path(os.environ['SystemRoot'])/'System32')
    for key in list(environment):
        if key.startswith(('QT_', 'QML', 'PYTHON', 'RESULTS_')):
            environment.pop(key)
    environment['PYTHONUTF8'] = '1'
    assert os.name == 'nt' and struct.calcsize('P') == 8, '64-bit Windows Python required'
    folders = {name: next(root.glob(f'Scompetition-{name}_*')) for name in ('Server', 'Client', 'ResultsWeb')}
    assert len(list(root.iterdir())) == 3, 'Unexpected content in split release staging'
    for name, exe in [('Server', 'Scompetition.exe'), ('Client', 'ScompetitionClient.exe')]:
        folder = folders[name]
        for required in (exe, 'Qt6Core.dll', 'Qt6Gui.dll', 'Qt6Widgets.dll', 'Qt6Network.dll',
                         'Qt6Multimedia.dll', 'platforms/qwindows.dll', 'msvcp140.dll',
                         'vcruntime140.dll', 'tools/ffmpeg/ffmpeg.exe', 'tools/ffmpeg/ffprobe.exe',
                         'broadcast_layout.ini', 'build-info.json', 'qt.conf'):
            assert (folder/required).is_file(), f'{name}: missing {required}'
        assert list((folder/'Assets/gamefinishvideo').glob('*.mp4')), 'Missing settlement video assets'
        assert len(list(folder.glob('Scompetition*.exe'))) == 1, 'Mixed desktop components'
        x64(folder/exe)
        x64(folder/'tools/ffmpeg/ffmpeg.exe')
        # qwindows (not offscreen) is tested; flag prevents ports/windows/settings.
        subprocess.run([str(folder/exe), '--package-smoke'], cwd=folder,
                       env=environment, check=True, timeout=30)
        subprocess.run([str(folder/'tools/ffmpeg/ffmpeg.exe'), '-version'], cwd=folder,
                       env=environment, check=True, timeout=30,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    folder = folders['ResultsWeb']
    x64(folder/'ResultsWeb.exe')
    assert (folder/'_internal/static/assets/arena-background.jpg').is_file()
    assert (folder/'_internal/static/assets/arena-corners.jpg').is_file()
    subprocess.run([str(folder/'ResultsWeb.exe'), '--help'], cwd=folder, env=environment,
                   check=True, timeout=30, stdout=subprocess.DEVNULL)
    with tempfile.TemporaryDirectory(prefix='scomp-package-qa-') as temp:
        with socket.socket() as sock:
            sock.bind(('127.0.0.1', 0))
            port = sock.getsockname()[1]
        process = subprocess.Popen([str(folder/'ResultsWeb.exe'), '--port', str(port),
                                    '--db', str(Path(temp)/'test.sqlite3')],
                                   cwd=folder, env=environment,
                                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        try:
            deadline = time.monotonic()+30
            while True:
                if process.poll() is not None:
                    raise AssertionError('Frozen HTTP service exited before startup')
                try:
                    with urlopen(f'http://127.0.0.1:{port}/api/public', timeout=1) as response:
                        payload = json.load(response)
                    assert 'standings' in payload
                    break
                except (OSError, ValueError):
                    if time.monotonic() >= deadline:
                        raise AssertionError('Frozen HTTP service failed startup')
                    time.sleep(.1)
            for endpoint in ('/', '/admin', '/display-clean.css', '/display.js', '/admin.js',
                             '/assets/arena-background.jpg', '/assets/arena-corners.jpg'):
                with urlopen(f'http://127.0.0.1:{port}{endpoint}', timeout=5) as response:
                    assert response.status == 200 and response.read(), endpoint
        finally:
            if process.poll() is None:
                process.terminate()
            try:
                process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait(timeout=10)
    for folder in folders.values():
        for file in folder.rglob('*'):
            if not file.is_file():
                continue
            assert not any(part in ('data', '__pycache__', '.learnings', 'logs') for part in file.parts), file
            assert not any(s in file.name for s in ('.sqlite', '.pyc', '.dmp')), file
    print('PASS: three x64 packages; Qt/FFmpeg start with sanitized PATH; frozen HTTP/assets serve; no runtime data.')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--directory', required=True, type=Path)
    parser.add_argument('--archives', nargs='*', type=Path)
    args = parser.parse_args()
    verify(args.directory.resolve())
    if args.archives:
        assert len(args.archives) == 3
        with tempfile.TemporaryDirectory(prefix='scomp-extracted-qa-') as temp:
            extracted = Path(temp)
            for archive in args.archives:
                with zipfile.ZipFile(archive) as z:
                    assert z.testzip() is None, archive
                    assert any(n.endswith('/PACKAGE_MANIFEST.json') for n in z.namelist())
                    for member in z.namelist():
                        destination = (extracted/member).resolve()
                        assert destination.is_relative_to(extracted.resolve()), member
                    z.extractall(extracted)
            # Re-run from extracted ZIPs, not just the original staging tree.
            verify(extracted)
        print('PASS: all three ZIP archives, manifest presence and extracted startup.')


if __name__ == '__main__':
    main()
