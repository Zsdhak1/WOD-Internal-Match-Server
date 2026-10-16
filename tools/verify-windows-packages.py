"""Verify the standalone Windows x64 Server and Client packages with no developer PATH.
Runs dependency-only GUI probes; never starts a real match. results-web is
deprecated and no longer packaged.
"""
import argparse
import os
from pathlib import Path
import struct
import subprocess
import tempfile
import zipfile

COMPONENTS = ('Server', 'Client')


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
    folders = {name: next(root.glob(f'Scompetition-{name}_*')) for name in COMPONENTS}
    assert len(list(root.iterdir())) == len(COMPONENTS), 'Unexpected content in split release staging'
    for name, exe in [('Server', 'Scompetition.exe'), ('Client', 'ScompetitionClient.exe')]:
        folder = folders[name]
        for required in (exe, 'Qt6Core.dll', 'Qt6Gui.dll', 'Qt6Widgets.dll', 'Qt6Network.dll',
                         'Qt6Multimedia.dll', 'platforms/qwindows.dll', 'msvcp140.dll',
                         'vcruntime140.dll', 'tools/ffmpeg/ffmpeg.exe', 'tools/ffmpeg/ffprobe.exe',
                         'broadcast_layout.ini', 'build-info.json', 'qt.conf'):
            assert (folder/required).is_file(), f'{name}: missing {required}'
        assert list((folder/'Assets/gamefinishvideo').glob('*.mp4')), 'Missing settlement video assets'
        if name == 'Server':
            for bgm in ('bgm_prepare.m4a', 'bgm_match.m4a', 'bgm_interlude.m4a', 'countdown_beep.wav'):
                assert (folder/'Assets/bgm'/bgm).is_file(), f'Server: missing Assets/bgm/{bgm}'
        assert len(list(folder.glob('Scompetition*.exe'))) == 1, 'Mixed desktop components'
        x64(folder/exe)
        x64(folder/'tools/ffmpeg/ffmpeg.exe')
        # qwindows (not offscreen) is tested; flag prevents ports/windows/settings.
        subprocess.run([str(folder/exe), '--package-smoke'], cwd=folder,
                       env=environment, check=True, timeout=30)
        subprocess.run([str(folder/'tools/ffmpeg/ffmpeg.exe'), '-version'], cwd=folder,
                       env=environment, check=True, timeout=30,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    for folder in folders.values():
        for file in folder.rglob('*'):
            if not file.is_file():
                continue
            assert not any(part in ('data', '__pycache__', '.learnings', 'logs') for part in file.parts), file
            assert not any(s in file.name for s in ('.sqlite', '.pyc', '.dmp')), file
    print('PASS: Server/Client x64 packages; Qt/FFmpeg start with sanitized PATH; no runtime data.')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--directory', required=True, type=Path)
    parser.add_argument('--archives', nargs='*', type=Path)
    args = parser.parse_args()
    verify(args.directory.resolve())
    if args.archives:
        assert len(args.archives) == len(COMPONENTS)
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
        print('PASS: Server/Client ZIP archives, manifest presence and extracted startup.')


if __name__ == '__main__':
    main()
