#!/usr/bin/env python3
"""Independent, dependency-free local competition results service (Python 3.10+)."""
from __future__ import annotations

import argparse
import csv
import datetime as dt
import hashlib
import hmac
import io
import json
import os
from pathlib import Path
from contextlib import contextmanager
import secrets
import sqlite3
import sys
import threading
import time
from http import HTTPStatus
from http.cookies import SimpleCookie
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlsplit

# Frozen onedir assets live under PyInstaller's bundle; writable data belongs
# beside the executable, not in _internal (or a transient onefile directory).
ROOT = Path(__file__).resolve().parent
RUNTIME_ROOT = Path(sys.executable).resolve().parent if getattr(sys, 'frozen', False) else ROOT
MAX_BODY = 2 * 1024 * 1024
MAX_TEAMS = 5000
SESSION_SECONDS = 8 * 60 * 60
STATUSES = {'scheduled', 'live', 'overtime', 'finished', 'cancelled'}


class APIError(Exception):
    def __init__(self, status, message):
        self.status, self.message = status, message


def unique_object(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise APIError(400, f'JSON含重复属性：{key}，不允许静默覆盖')
        result[key] = value
    return result


def text(value, label, maximum=120, required=True):
    if not isinstance(value, str):
        raise APIError(400, f'{label}必须是文本')
    value = value.strip()
    if (required and not value) or len(value) > maximum or any(ord(c) < 32 or ord(c) == 127 or 0xD800 <= ord(c) <= 0xDFFF for c in value):
        raise APIError(400, f'{label}不能为空、含控制字符或超过{maximum}字')
    return value


def integer(value, label, low=0, high=100000):
    if type(value) is not int or not low <= value <= high:
        raise APIError(400, f'{label}必须是{low}至{high}的整数')
    return value


def password_hash(password, salt):
    return hashlib.pbkdf2_hmac('sha256', password.encode('utf-8'), bytes.fromhex(salt), 310000).hex()


def validate_password(password):
    if not isinstance(password, str) or not 12 <= len(password) <= 128 or any(0xD800 <= ord(c) <= 0xDFFF for c in password):
        raise APIError(400, '密码长度必须为12至128个字符')


class Store:
    def __init__(self, path):
        self.path = Path(path).resolve()
        self.path.parent.mkdir(parents=True, exist_ok=True)
        with self.connect() as db:
            db.executescript('''
                PRAGMA journal_mode=WAL;
                CREATE TABLE IF NOT EXISTS settings (key TEXT PRIMARY KEY, value TEXT NOT NULL);
                CREATE TABLE IF NOT EXISTS teams (
                    id INTEGER PRIMARY KEY AUTOINCREMENT,
                    name TEXT NOT NULL, name_key TEXT NOT NULL UNIQUE,
                    short_name TEXT NOT NULL DEFAULT '', organization TEXT NOT NULL DEFAULT '',
                    version INTEGER NOT NULL DEFAULT 1);
                CREATE TABLE IF NOT EXISTS matches (
                    id INTEGER PRIMARY KEY AUTOINCREMENT,
                    team_a INTEGER NOT NULL REFERENCES teams(id),
                    team_b INTEGER NOT NULL REFERENCES teams(id),
                    date TEXT NOT NULL, round TEXT NOT NULL,
                    score_a INTEGER NOT NULL DEFAULT 0, score_b INTEGER NOT NULL DEFAULT 0,
                    overtime INTEGER NOT NULL DEFAULT 0,
                    overtime_score_a INTEGER NOT NULL DEFAULT 0,
                    overtime_score_b INTEGER NOT NULL DEFAULT 0,
                    winner_id INTEGER REFERENCES teams(id), status TEXT NOT NULL,
                    version INTEGER NOT NULL DEFAULT 1, CHECK(team_a != team_b));
            ''')

    @contextmanager
    def connect(self):
        db = sqlite3.connect(self.path, timeout=10)
        db.row_factory = sqlite3.Row
        db.execute('PRAGMA foreign_keys=ON')
        try:
            with db:
                yield db
        finally:
            db.close()

    def init_password(self, password=None, reset=False):
        with self.connect() as db:
            exists = db.execute("SELECT 1 FROM settings WHERE key='password_hash'").fetchone()
            if exists and not reset:
                return None
            generated = password is None
            if generated:
                password = secrets.token_urlsafe(18)
            validate_password(password)
            salt = secrets.token_hex(16)
            for key, value in [('password_salt', salt), ('password_hash', password_hash(password, salt))]:
                db.execute('INSERT OR REPLACE INTO settings VALUES (?,?)', (key, value))
            return password if generated else None

    def check_password(self, password):
        if not isinstance(password, str) or len(password) > 128 or any(0xD800 <= ord(c) <= 0xDFFF for c in password):
            return False
        with self.connect() as db:
            values = dict(db.execute('SELECT key,value FROM settings'))
        return hmac.compare_digest(password_hash(password, values['password_salt']), values['password_hash'])

    def teams(self, db=None):
        if db is None:
            with self.connect() as connection:
                return self.teams(connection)
        return [dict(r) for r in db.execute('SELECT id,name,short_name,organization,version FROM teams ORDER BY id')]

    def matches(self, db=None):
        if db is None:
            with self.connect() as connection:
                return self.matches(connection)
        rows = db.execute('''SELECT m.*, a.name AS name_a, b.name AS name_b,
                          w.name AS winner_name FROM matches m
                          JOIN teams a ON m.team_a=a.id JOIN teams b ON m.team_b=b.id
                          LEFT JOIN teams w ON m.winner_id=w.id ORDER BY m.date DESC,m.id DESC''')
        return [dict(r, overtime=bool(r['overtime'])) for r in rows]

    def snapshot(self):
        with self.connect() as db:
            # Same SQLite snapshot for teams/matches, even during a concurrent write.
            db.execute('BEGIN')
            teams, matches = self.teams(db), self.matches(db)
        ranked = {t['id']: dict(t, played=0, wins=0, losses=0, kills=0, against=0, points=0) for t in teams}
        for m in matches:
            if m['status'] != 'finished':
                continue
            a, b = ranked[m['team_a']], ranked[m['team_b']]
            sa = m['score_a'] + m['overtime_score_a']
            sb = m['score_b'] + m['overtime_score_b']
            for team, other, score, opponent in [(a, b, sa, sb), (b, a, sb, sa)]:
                team['played'] += 1
                team['kills'] += score
                team['points'] += score  # Every recorded kill = one point; no invented win bonus.
                team['against'] += opponent
                team['wins'] += int(m['winner_id'] == team['id'])
                team['losses'] += int(m['winner_id'] == other['id'])
        rows = sorted(ranked.values(), key=lambda t: (-t['points'], -t['wins'], -(t['kills']-t['against']), t['name'].casefold(), t['id']))
        previous, rank = None, 0
        for index, row in enumerate(rows):
            key = (row['points'], row['wins'], row['kills'] - row['against'])
            if key != previous:
                rank = index + 1
            row['rank'] = rank
            previous = key
        return {'teams': teams, 'matches': matches, 'standings': rows,
                'rules': '击杀 +1；已完成比赛的常规及加时击杀计入积分。按积分、胜场、净击杀排序；同项并列。常规同分必须进入SD加时，互杀同分继续；完成时加时分差恰好为1，不自动判胜。',
                'generated_at': dt.datetime.now(dt.timezone.utc).isoformat()}

    def save_team(self, data, item_id=None):
        name = text(data.get('name'), '队名')
        short = text(data.get('short_name', ''), '简称', 32, False)
        org = text(data.get('organization', ''), '单位', 120, False)
        with self.connect() as db:
            db.execute('BEGIN IMMEDIATE')
            try:
                if item_id is None:
                    if db.execute('SELECT COUNT(*) FROM teams').fetchone()[0] >= MAX_TEAMS:
                        raise APIError(400, f'最多支持{MAX_TEAMS}支队伍')
                    item_id = db.execute('INSERT INTO teams(name,name_key,short_name,organization) VALUES (?,?,?,?)',
                                         (name, name.casefold(), short, org)).lastrowid
                else:
                    self.require_version(db, 'teams', item_id, data)
                    db.execute('UPDATE teams SET name=?,name_key=?,short_name=?,organization=?,version=version+1 WHERE id=?',
                               (name, name.casefold(), short, org, item_id))
            except sqlite3.IntegrityError:
                raise APIError(409, '队名已存在（忽略首尾空格及英文大小写），未覆盖')
        return {'id': item_id}

    @staticmethod
    def require_version(db, table, item_id, data):
        row = db.execute(f'SELECT version FROM {table} WHERE id=?', (item_id,)).fetchone()
        if not row:
            raise APIError(404, '记录不存在')
        if type(data.get('version')) is not int or data['version'] != row['version']:
            raise APIError(409, '记录已被其他操作更新，请刷新后重新编辑')

    def save_match(self, data, item_id=None):
        a = integer(data.get('team_a'), '队伍A', 1, 2**31-1)
        b = integer(data.get('team_b'), '队伍B', 1, 2**31-1)
        if a == b:
            raise APIError(400, '比赛双方不能是同一队伍')
        date = text(data.get('date'), '日期', 10)
        try:
            if dt.date.fromisoformat(date).isoformat() != date:
                raise ValueError()
        except ValueError:
            raise APIError(400, '日期必须为有效的 YYYY-MM-DD')
        round_name = text(data.get('round'), '轮次', 80)
        status = data.get('status')
        if not isinstance(status, str) or status not in STATUSES:
            raise APIError(400, '无效比赛状态')
        sa, sb = integer(data.get('score_a', 0), 'A常规击杀'), integer(data.get('score_b', 0), 'B常规击杀')
        overtime = data.get('overtime', False)
        if type(overtime) is not bool:
            raise APIError(400, '加时必须是布尔值')
        oa = integer(data.get('overtime_score_a', 0), 'A加时击杀')
        ob = integer(data.get('overtime_score_b', 0), 'B加时击杀')
        winner = data.get('winner_id')
        if winner is not None and (type(winner) is not int or winner not in (a, b)):
            raise APIError(400, '胜方必须为比赛双方之一或空值')
        if overtime and sa != sb:
            raise APIError(400, '只有常规比分相同才能记录加时')
        if not overtime and (oa or ob):
            raise APIError(400, '未启用加时不能录入加时击杀')
        if status == 'overtime' and (not overtime or sa != sb):
            raise APIError(400, '加时中需要勾选加时且常规同分')
        if status == 'finished':
            if sa == sb and (not overtime or oa == ob):
                raise APIError(400, '常规同分必须加时；加时仍同分不能完成或自动判胜')
            if overtime and abs(oa - ob) != 1:
                raise APIError(400, 'SD突然死亡加时完成时分差必须恰好为1；双方互杀同分继续')
            expected = a if sa + oa > sb + ob else b
            if winner is not None and winner != expected:
                raise APIError(400, '所选胜方与击杀比分冲突')
            winner = expected
        elif winner is not None:
            raise APIError(400, '未完成比赛不能指定胜方')
        with self.connect() as db:
            db.execute('BEGIN IMMEDIATE')
            if db.execute('SELECT COUNT(*) FROM teams WHERE id IN (?,?)', (a, b)).fetchone()[0] != 2:
                raise APIError(400, '比赛双方队伍不存在')
            values = (a, b, date, round_name, sa, sb, int(overtime), oa, ob, winner, status)
            columns = 'team_a,team_b,date,round,score_a,score_b,overtime,overtime_score_a,overtime_score_b,winner_id,status'
            if item_id is None:
                item_id = db.execute(f'INSERT INTO matches({columns}) VALUES ({",".join("?" for _ in values)})', values).lastrowid
            else:
                self.require_version(db, 'matches', item_id, data)
                assignments = ','.join(c+'=?' for c in columns.split(','))
                db.execute(f'UPDATE matches SET {assignments},version=version+1 WHERE id=?', values + (item_id,))
        return {'id': item_id, 'winner_id': winner}

    def delete(self, table, item_id, data):
        with self.connect() as db:
            db.execute('BEGIN IMMEDIATE')
            self.require_version(db, table, item_id, data)
            if table == 'teams' and db.execute('SELECT 1 FROM matches WHERE team_a=? OR team_b=? LIMIT 1', (item_id, item_id)).fetchone():
                raise APIError(409, '队伍已有比赛；请先删除相关比赛，不会级联丢失结果')
            db.execute(f'DELETE FROM {table} WHERE id=?', (item_id,))
        return {'deleted': item_id}

    def preview_import(self, data):
        format_name, content = data.get('format'), data.get('content')
        if not isinstance(content, str) or any(0xD800 <= ord(c) <= 0xDFFF for c in content) or len(content.encode('utf-8')) > MAX_BODY:
            raise APIError(400, '名单必须是最多2MiB的有效Unicode文本')
        mode = data.get('duplicate_mode', 'reject')
        if mode not in ('reject', 'skip'):
            raise APIError(400, '重复策略只能是 reject 或 skip；不支持覆盖')
        aliases = {'队名': 'name', '队伍': 'name', '队伍名称': 'name', '名称': 'name',
                   '简称': 'short_name', '单位': 'organization', '学校': 'organization'}
        try:
            if format_name == 'csv':
                reader = csv.DictReader(io.StringIO(content.lstrip('\ufeff')), strict=True)
                if not reader.fieldnames:
                    raise APIError(400, 'CSV需要表头 name 或 队名')
                headers = [aliases.get(key.strip(), key.strip()) for key in reader.fieldnames]
                if len(set(headers)) != len(headers):
                    raise APIError(400, 'CSV表头含重复列（含中英别名重复），未导入')
                if 'name' not in headers:
                    raise APIError(400, 'CSV需要表头 name 或 队名')
                rows = list(reader)
            elif format_name == 'json':
                rows = json.loads(content.lstrip('\ufeff'), object_pairs_hook=unique_object)
                if isinstance(rows, dict):
                    rows = rows.get('teams')
                if not isinstance(rows, list):
                    raise APIError(400, 'JSON应为队伍对象数组或 {"teams": [...]}')
            else:
                raise APIError(400, '仅支持CSV与JSON；请将XLSX另存为UTF-8 CSV')
        except (csv.Error, json.JSONDecodeError) as exc:
            raise APIError(400, f'名单解析失败：{exc}')
        if not rows or len(rows) > MAX_TEAMS:
            raise APIError(400, f'名单应含1至{MAX_TEAMS}行')
        existing = self.teams()
        seen = {t['name'].casefold() for t in existing}
        preview, accepted, errors, skipped = [], [], 0, 0
        for index, raw in enumerate(rows, 1):
            result = {'row': index, 'name': '', 'action': 'add', 'message': ''}
            try:
                if not isinstance(raw, dict):
                    raise APIError(400, '每行必须是队伍对象')
                normalized = {}
                for key, value in raw.items():
                    if key is None:
                        raise APIError(400, 'CSV列数超过表头')
                    key = aliases.get(str(key).strip(), str(key).strip())
                    if key not in ('name', 'short_name', 'organization'):
                        raise APIError(400, f'未知列：{key}')
                    if key in normalized:
                        raise APIError(400, f'重复列：{key}')
                    normalized[key] = value
                name = text(normalized.get('name'), '队名')
                result['name'] = name
                row = {'name': name, 'short_name': text(normalized.get('short_name', ''), '简称', 32, False),
                       'organization': text(normalized.get('organization', ''), '单位', 120, False)}
                if name.casefold() in seen:
                    if mode == 'skip':
                        result.update(action='skip', message='重复队名，按所选策略跳过（不覆盖）')
                        skipped += 1
                    else:
                        raise APIError(409, '与已有队伍或名单内队伍重复；可明确选择跳过，不会覆盖')
                else:
                    seen.add(name.casefold())
                    accepted.append(row)
            except APIError as exc:
                errors += 1
                result.update(action='error', message=exc.message)
            preview.append(result)
        if len(existing) + len(accepted) > MAX_TEAMS:
            errors += 1
            preview.append({'row': 0, 'name': '', 'action': 'error', 'message': f'导入后超过{MAX_TEAMS}队上限'})
        fingerprint = hashlib.sha256(json.dumps(existing, sort_keys=True, ensure_ascii=False).encode()).hexdigest()
        return {'rows': preview, 'add_count': len(accepted), 'skip_count': skipped, 'error_count': errors,
                'can_commit': errors == 0 and bool(accepted), '_accepted': accepted, '_fingerprint': fingerprint}

    def commit_import(self, preview):
        if not preview['can_commit']:
            raise APIError(400, '预览含错误或没有新增队伍，不能提交')
        with self.connect() as db:
            db.execute('BEGIN IMMEDIATE')
            fingerprint = hashlib.sha256(json.dumps(self.teams(db), sort_keys=True, ensure_ascii=False).encode()).hexdigest()
            if fingerprint != preview['_fingerprint']:
                raise APIError(409, '队伍名单在预览后有变化，请重新预览，不会覆盖')
            for row in preview['_accepted']:
                db.execute('INSERT INTO teams(name,name_key,short_name,organization) VALUES (?,?,?,?)',
                           (row['name'], row['name'].casefold(), row['short_name'], row['organization']))
        return {'imported': len(preview['_accepted']), 'skipped': preview['skip_count']}


class AppServer(ThreadingHTTPServer):
    daemon_threads = True
    def __init__(self, address, store, allowed_hosts=None, secure_cookie=False):
        super().__init__(address, Handler)
        self.store, self.secure_cookie = store, secure_cookie
        self.allowed_hosts = set(allowed_hosts or ['127.0.0.1', 'localhost', '[::1]'])
        self.sessions, self.failures = {}, {}
        self.lock = threading.RLock()


class Handler(BaseHTTPRequestHandler):
    server_version = 'ResultsWeb/1.0'
    sys_version = ''
    def log_message(self, format, *args):
        # No request bodies/cookies/passwords in logs.
        print(f'[{self.log_date_time_string()}] {self.client_address[0]} {format % args}', flush=True)

    def send(self, status, payload, content_type='application/json; charset=utf-8', headers=None):
        if isinstance(payload, (dict, list)):
            payload = json.dumps(payload, ensure_ascii=False).encode('utf-8')
        elif isinstance(payload, str):
            payload = payload.encode('utf-8')
        self.send_response(status)
        self.send_header('Content-Type', content_type)
        self.send_header('Content-Length', str(len(payload)))
        self.send_header('Cache-Control', 'no-store')
        self.send_header('X-Content-Type-Options', 'nosniff')
        self.send_header('X-Frame-Options', 'DENY')
        self.send_header('Referrer-Policy', 'same-origin')
        self.send_header('Content-Security-Policy', "default-src 'self'; script-src 'self'; style-src 'self'; img-src 'self'; connect-src 'self'; frame-ancestors 'none'; base-uri 'none'; form-action 'self'")
        for name, value in (headers or {}).items():
            self.send_header(name, value)
        self.end_headers()
        self.wfile.write(payload)

    def parse_body(self):
        if self.headers.get('Transfer-Encoding'):
            raise APIError(400, '不支持分块请求')
        try:
            size = int(self.headers.get('Content-Length', '0'))
        except ValueError:
            raise APIError(400, '无效内容长度')
        if not 0 < size <= MAX_BODY:
            raise APIError(413, '请求不能为空或超过2MiB')
        if self.headers.get('Content-Type', '').split(';')[0].strip().lower() != 'application/json':
            raise APIError(415, '请求必须是application/json')
        try:
            data = json.loads(self.rfile.read(size).decode('utf-8-sig'), object_pairs_hook=unique_object)
        except (ValueError, UnicodeError):
            raise APIError(400, '无效UTF-8 JSON')
        if not isinstance(data, dict):
            raise APIError(400, '请求JSON必须为对象')
        return data

    def check_host_origin(self):
        host = self.headers.get('Host', '')
        hostname = host.rsplit(':', 1)[0] if ':' in host and not host.endswith(']') else host
        if hostname.lower() not in self.server.allowed_hosts:
            raise APIError(403, 'Host不在允许列表；请使用配置的展示地址')
        origin = self.headers.get('Origin')
        if origin and origin not in ('http://' + host, 'https://' + host):
            raise APIError(403, '拒绝跨站请求')
        if self.headers.get('Sec-Fetch-Site') == 'cross-site':
            raise APIError(403, '拒绝跨站请求')

    def session(self, write=False):
        cookie = SimpleCookie()
        try:
            cookie.load(self.headers.get('Cookie', ''))
        except Exception:
            raise APIError(401, '请登录管理员')
        sid = cookie.get('results_session')
        with self.server.lock:
            session = self.server.sessions.get(sid.value if sid else '')
            if not session or session['expires'] < time.time():
                if sid:
                    self.server.sessions.pop(sid.value, None)
                raise APIError(401, '登录已失效，请重新登录')
            if write and not hmac.compare_digest(self.headers.get('X-CSRF-Token', '').encode('utf-8'), session['csrf'].encode('utf-8')):
                raise APIError(403, 'CSRF校验失败，请刷新后台')
            return session

    def do_GET(self):
        self.handle_request('GET')
    def do_POST(self):
        self.handle_request('POST')
    def do_PUT(self):
        self.handle_request('PUT')
    def do_DELETE(self):
        self.handle_request('DELETE')

    def handle_request(self, method):
        try:
            self.connection.settimeout(15)
            self.check_host_origin()
            path = urlsplit(self.path).path
            if method == 'GET':
                self.get_route(path)
            else:
                self.write_route(method, path)
        except APIError as exc:
            self.send(exc.status, {'error': exc.message})
        except (BrokenPipeError, ConnectionResetError, TimeoutError):
            pass
        except Exception as exc:
            print(f'Internal error: {type(exc).__name__}: {exc}', flush=True)
            self.send(500, {'error': '服务器内部错误，请查看本地日志'})

    def get_route(self, path):
        if path == '/api/public':
            self.send(200, self.server.store.snapshot())
        elif path == '/api/session':
            session = self.session()
            self.send(200, {'username': 'admin', 'csrf': session['csrf'], 'expires_at': session['expires']})
        elif path == '/api/admin':
            self.session()
            self.send(200, self.server.store.snapshot())
        elif path in ('/api/export/standings.csv', '/api/export/matches.csv'):
            self.session()
            snapshot = self.server.store.snapshot()
            output = io.StringIO(newline='')
            writer = csv.writer(output)
            if path.endswith('standings.csv'):
                writer.writerow(['排名', '队名', '单位', '场次', '胜', '负', '击杀积分', '被击杀', '净击杀'])
                rows = [[t['rank'], t['name'], t['organization'], t['played'], t['wins'], t['losses'], t['points'], t['against'], t['kills']-t['against']] for t in snapshot['standings']]
            else:
                writer.writerow(['ID', '日期', '轮次', '队伍A', '队伍B', 'A常规击杀', 'B常规击杀', '加时', 'A加时击杀', 'B加时击杀', '胜方', '状态'])
                rows = [[m['id'], m['date'], m['round'], m['name_a'], m['name_b'], m['score_a'], m['score_b'], '是' if m['overtime'] else '否', m['overtime_score_a'], m['overtime_score_b'], m['winner_name'] or '', m['status']] for m in snapshot['matches']]
            for row in rows:
                # Avoid spreadsheet formula execution, even for legitimate team names.
                writer.writerow(["'" + v if isinstance(v, str) and v.lstrip().startswith(('=', '+', '-', '@')) else v for v in row])
            self.send(200, '\ufeff' + output.getvalue(), 'text/csv; charset=utf-8', {'Content-Disposition': f'attachment; filename="{path.rsplit("/", 1)[1]}"'})
        else:
            static = {'/': ('index.html', 'text/html'), '/admin': ('admin.html', 'text/html'), '/admin/': ('admin.html', 'text/html'),
                      '/assets/arena-background.jpg': ('assets/arena-background.jpg', 'image/jpeg'), '/assets/arena-corners.jpg': ('assets/arena-corners.jpg', 'image/jpeg'),
                      '/styles.css': ('styles.css', 'text/css'), '/display-clean.css': ('display-clean.css', 'text/css'), '/display.js': ('display.js', 'text/javascript'), '/admin.js': ('admin.js', 'text/javascript')}
            if path not in static:
                raise APIError(404, '页面不存在')
            filename, mime = static[path]
            self.send(200, (ROOT / 'static' / filename).read_bytes(), mime + '; charset=utf-8')

    def write_route(self, method, path):
        data = self.parse_body()
        if path == '/api/login' and method == 'POST':
            ip, now = self.client_address[0], time.time()
            with self.server.lock:
                self.server.failures = {key: [t for t in values if now - t < 300] for key, values in self.server.failures.items() if any(now-t < 300 for t in values)}
                attempts = self.server.failures.setdefault(ip, [])
                if len(attempts) >= 10:
                    raise APIError(429, '登录失败过多，请5分钟后重试')
                if data.get('username') != 'admin' or not self.server.store.check_password(data.get('password')):
                    attempts.append(now)
                    raise APIError(401, '管理员名或密码错误')
                self.server.failures.pop(ip, None)
                self.server.sessions = {k: v for k, v in self.server.sessions.items() if v['expires'] > now}
                if len(self.server.sessions) >= 100:
                    raise APIError(429, '会话过多，请稍后再试')
                sid = secrets.token_urlsafe(32)
                csrf = secrets.token_urlsafe(32)
                self.server.sessions[sid] = {'csrf': csrf, 'expires': now + SESSION_SECONDS, 'previews': {}}
            flags = '; Secure' if self.server.secure_cookie else ''
            self.send(200, {'username': 'admin', 'csrf': csrf}, 'application/json; charset=utf-8',
                      {'Set-Cookie': f'results_session={sid}; Path=/; HttpOnly; SameSite=Strict; Max-Age={SESSION_SECONDS}{flags}'})
            return
        session = self.session(write=True)
        if path == '/api/logout' and method == 'POST':
            cookie = SimpleCookie(self.headers.get('Cookie', ''))
            with self.server.lock:
                self.server.sessions.pop(cookie['results_session'].value, None)
            self.send(200, {'ok': True}, headers={'Set-Cookie': 'results_session=; Path=/; HttpOnly; SameSite=Strict; Max-Age=0'})
        elif path == '/api/password' and method == 'POST':
            if not self.server.store.check_password(data.get('current_password')):
                raise APIError(401, '当前密码错误')
            validate_password(data.get('new_password'))
            self.server.store.init_password(data['new_password'], reset=True)
            with self.server.lock:
                self.server.sessions.clear()
            self.send(200, {'ok': True}, headers={'Set-Cookie': 'results_session=; Path=/; HttpOnly; SameSite=Strict; Max-Age=0'})
        elif path == '/api/import/preview' and method == 'POST':
            preview = self.server.store.preview_import(data)
            token = secrets.token_urlsafe(24)
            with self.server.lock:
                session['previews'] = {token: {'preview': preview, 'expires': time.time() + 600}}
            self.send(200, dict({k: v for k, v in preview.items() if not k.startswith('_')}, token=token))
        elif path == '/api/import/commit' and method == 'POST':
            token = data.get('token')
            if not isinstance(token, str):
                raise APIError(400, '缺少预览令牌')
            with self.server.lock:
                entry = session['previews'].pop(token, None)
                if not entry or entry['expires'] < time.time():
                    raise APIError(400, '预览已过期或已使用，请重新预览')
                result = self.server.store.commit_import(entry['preview'])
            self.send(200, result)
        else:
            parts = path.strip('/').split('/')
            if len(parts) not in (2, 3) or parts[0] != 'api' or parts[1] not in ('teams', 'matches'):
                raise APIError(404, '接口不存在')
            table = parts[1]
            item_id = None
            if len(parts) == 3:
                try:
                    item_id = int(parts[2])
                    if not 1 <= item_id <= 2**31 - 1:
                        raise ValueError()
                except ValueError:
                    raise APIError(400, '无效记录ID')
            if (method == 'POST' and item_id is not None) or (method in ('PUT', 'DELETE') and item_id is None):
                raise APIError(405, '请求方法与接口不匹配')
            if method == 'DELETE':
                result = self.server.store.delete(table, item_id, data)
            elif method in ('POST', 'PUT'):
                save = self.server.store.save_team if table == 'teams' else self.server.store.save_match
                result = save(data, item_id)
            else:
                raise APIError(405, '不支持的操作')
            self.send(201 if method == 'POST' else 200, result)


def main():
    # Windows redirected consoles may default to cp1252. Chinese status/help
    # must not abort startup (including direct frozen EXE launches).
    for stream in (sys.stdout, sys.stderr):
        if stream is not None and hasattr(stream, 'reconfigure'):
            stream.reconfigure(encoding='utf-8', errors='backslashreplace')
    parser = argparse.ArgumentParser(description='独立比赛结果网页（无需第三方依赖）')
    parser.add_argument('--host', default='127.0.0.1', help='默认仅本机；局域网可指定本机LAN IP或0.0.0.0')
    parser.add_argument('--port', type=int, default=8765)
    parser.add_argument('--db', default=str(RUNTIME_ROOT / 'data' / 'results.sqlite3'))
    parser.add_argument('--allowed-host', action='append', default=[], help='允许的URL主机名/IP，不含端口，可重复')
    parser.add_argument('--reset-password', action='store_true', help='用环境变量或新随机密码重置管理员；会影响现有登录')
    parser.add_argument('--secure-cookie', action='store_true', help='仅HTTPS反向代理场景启用')
    args = parser.parse_args()
    if not 1 <= args.port <= 65535 or args.port == 19387:
        parser.error('端口须为1至65535且不能为DSH GUI的19387')
    store = Store(args.db)
    password = store.init_password(os.environ.get('RESULTS_ADMIN_PASSWORD'), args.reset_password)
    if password:
        print(f'首次设置/重置管理员：admin\n一次性生成密码：{password}\n请立即保存，此密码不会再次显示。', flush=True)
    allowed = {'127.0.0.1', 'localhost', '[::1]'} | {h.lower() for h in args.allowed_host}
    if args.host not in ('0.0.0.0', '::'):
        allowed.add(args.host.lower())
    server = AppServer((args.host, args.port), store, allowed, args.secure_cookie)
    print(f'只读展示：http://{args.host}:{args.port}/\n管理后台：http://{args.host}:{args.port}/admin\n数据库：{store.path}\n允许Host：{", ".join(sorted(allowed))}', flush=True)
    if args.host != '127.0.0.1':
        print('警告：HTTP密码不加密；仅用于可信局域网。公网必须使用HTTPS与访问限制。0.0.0.0需用--allowed-host配置实际LAN IP。', flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print('正在关闭服务。', flush=True)
    finally:
        server.server_close()


if __name__ == '__main__':
    main()
