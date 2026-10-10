"""Credential-free source archive. --output-dir supports the project's build/packages."""
import argparse
import datetime
import hashlib
import json
from pathlib import Path
import re
import subprocess
import tempfile
import zipfile

ROOT = Path(__file__).resolve().parents[1]
PROJECT = ROOT.parent
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--output-dir', type=Path, default=ROOT / 'packages')
parser.add_argument('--include-verification', action='store_true',
                    help='Attach local generated QA reports/screenshots; source-only by default')
args = parser.parse_args()
match = re.search(r'project\(Scompetition\s+VERSION\s+([\d.]+)', (PROJECT / 'CMakeLists.txt').read_text(encoding='utf-8'))
if not match:
    raise SystemExit('Cannot prove project version from CMakeLists.txt')
version = match.group(1)
stamp = datetime.datetime.now(datetime.timezone.utc).strftime('%Y%m%d-%H%M%S')
name = f'results-web_v{version}_{stamp}_cross-platform_source'
output = args.output_dir.resolve() / (name + '.zip')
output.parent.mkdir(parents=True, exist_ok=True)
# No captured child-process pipes: outputs go to an owned temporary file.
def git_result(arguments):
    with tempfile.TemporaryFile() as log:
        result = subprocess.run(['git', '-C', str(PROJECT), *arguments], stdout=log, stderr=subprocess.DEVNULL)
        if result.returncode:
            return None
        log.seek(0)
        return log.read().decode('utf-8', errors='replace').strip()

revision = git_result(['rev-parse', 'HEAD'])
status = git_result(['status', '--porcelain'])
def evidence(filename):
    source = ROOT / 'verification' / filename
    if not source.is_file():
        return {'passed': False, 'note': 'Verification report unavailable'}
    try:
        return json.loads(source.read_text(encoding='utf-8'))
    except (ValueError, OSError):
        return {'passed': False, 'note': 'Verification report unreadable'}
http = evidence('http-report.json') if args.include_verification else {'passed': False, 'note': 'Source-only package; local QA evidence not attached'}
browser = evidence('browser-report.json') if args.include_verification else {'passed': False, 'note': 'Source-only package; local QA evidence not attached'}
metadata = {'component': 'results-web', 'project_version': version, 'utc_timestamp': stamp,
            'platform': 'cross-platform', 'configuration': 'source', 'git_revision': revision or 'unknown',
            'git_dirty': bool(status) if status is not None else None,
            'git_dirty_note': 'Measured via git status; null means Git status unavailable',
            'runtime': 'Python >=3.10 standard library; native HTML/CSS/JS',
            'http_test_report': http, 'browser_test_report': browser,
            'browser_visual_tested': bool(browser.get('passed') and browser.get('browser_visual_tested')),
            'browser_note': 'Isolated Chromium headless QA only; user live browser/F11/physical monitor not tested',
            'excluded': ['data', 'packages', '__pycache__', '*.pyc', '*.log', '.learnings', 'temporary test databases', 'browser profiles'], 'files': []}
selected = [ROOT/'server.py', ROOT/'README.md', ROOT/'.gitignore', ROOT/'verification'/'README.md']
directories = ['static', 'templates', 'tests', 'tools']
if args.include_verification:
    directories.append('verification')
for directory in directories:
    selected.extend(p for p in (ROOT/directory).rglob('*') if p.is_file()
                    and '__pycache__' not in p.parts and p.suffix != '.pyc'
                    and not any(part.startswith('.qa-runtime-') for part in p.parts)
                    and '.sqlite3' not in p.name and p.suffix != '.log')
with zipfile.ZipFile(output, 'x', zipfile.ZIP_DEFLATED) as archive:
    for file in sorted(set(selected)):
        relative = file.relative_to(ROOT).as_posix()
        data = file.read_bytes()
        if file == ROOT/'README.md':
            data = data.replace('[规则符合性审查](<../docs/COMPETITION_RULE_COMPLIANCE.md>)'.encode(),
                                '主项目 docs/COMPETITION_RULE_COMPLIANCE.md（不在独立网页包内）'.encode())
        metadata['files'].append({'path': relative, 'bytes': len(data), 'sha256': hashlib.sha256(data).hexdigest()})
        archive.writestr(f'{name}/{relative}', data)
    archive.writestr(f'{name}/BUILD_METADATA.json', json.dumps(metadata, ensure_ascii=False, indent=2))
with zipfile.ZipFile(output) as archive:
    if archive.testzip() is not None:
        raise SystemExit('ZIP CRC verification failed')
    if any('/data/' in entry or '.sqlite3' in entry or '__pycache__' in entry or '.qa-runtime-' in entry for entry in archive.namelist()):
        raise SystemExit('Archive contains runtime/private data')
print(output)
print(f'Packaged {len(set(selected))} files; ZIP CRC verified; no runtime credentials/database.')
