"""Real HTTP requests against a live local server, using only Python stdlib."""
import http.client
import importlib.util
import json
from pathlib import Path
import tempfile
import threading
import os
import socket
import subprocess
import sys
import time
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('results_server', ROOT / 'server.py')
server_module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(server_module)


class HTTPTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(dir=ROOT)
        self.db_path = Path(self.temp.name) / 'test.sqlite3'
        self.store = server_module.Store(self.db_path)
        self.password = 'Test-only-password-9!'
        self.store.init_password(self.password)
        self.server = server_module.AppServer(('127.0.0.1', 0), self.store)
        self.port = self.server.server_address[1]
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()
        self.cookie = ''
        self.csrf = ''

    def tearDown(self):
        self.server.shutdown()
        self.server.server_close()
        self.thread.join(timeout=3)
        self.temp.cleanup()

    def request(self, method, path, body=None, auth=True, headers=None, raw=None):
        conn = http.client.HTTPConnection('127.0.0.1', self.port, timeout=8)
        merged = {}
        if auth and self.cookie:
            merged['Cookie'] = self.cookie
        if auth and self.csrf:
            merged['X-CSRF-Token'] = self.csrf
        payload = None
        if body is not None:
            payload = json.dumps(body, ensure_ascii=False).encode('utf-8')
            merged['Content-Type'] = 'application/json'
        if raw is not None:
            payload = raw
        merged.update(headers or {})
        conn.request(method, path, payload, merged)
        response = conn.getresponse()
        content = response.read()
        result = json.loads(content) if 'application/json' in response.getheader('Content-Type', '') else content
        result_headers = dict(response.getheaders())
        status = response.status
        conn.close()
        return status, result, result_headers

    def login(self):
        status, result, headers = self.request('POST', '/api/login', {'username': 'admin', 'password': self.password}, auth=False)
        self.assertEqual(status, 200)
        self.cookie = headers['Set-Cookie'].split(';')[0]
        self.csrf = result['csrf']
        self.assertIn('HttpOnly', headers['Set-Cookie'])
        self.assertIn('SameSite=Strict', headers['Set-Cookie'])

    def team(self, name):
        status, result, _ = self.request('POST', '/api/teams', {'name': name})
        self.assertEqual(status, 201, result)
        return result['id']

    def snapshot(self):
        status, result, _ = self.request('GET', '/api/public', auth=False)
        self.assertEqual(status, 200)
        return result

    def match_body(self, a, b, **values):
        return dict({'team_a': a, 'team_b': b, 'date': '2026-10-10', 'round': '小组赛 R1', 'score_a': 3,
                     'score_b': 1, 'status': 'finished', 'overtime': False, 'overtime_score_a': 0,
                     'overtime_score_b': 0, 'winner_id': None}, **values)

    def test_static_public_and_auth_security(self):
        for path, mime in [('/', 'text/html'), ('/admin', 'text/html'), ('/styles.css', 'text/css'),
                           ('/display.js', 'text/javascript'), ('/admin.js', 'text/javascript'),
                           ('/display-clean.css', 'text/css'), ('/assets/arena-background.jpg', 'image/jpeg'),
                           ('/assets/arena-corners.jpg', 'image/jpeg')]:
            status, result, headers = self.request('GET', path, auth=False)
            self.assertEqual(status, 200)
            self.assertIn(mime, headers['Content-Type'])
            self.assertIn("script-src 'self'", headers['Content-Security-Policy'])
            self.assertEqual(headers['X-Content-Type-Options'], 'nosniff')
        public = self.snapshot()
        self.assertNotIn('csrf', public)
        self.assertNotIn('password', json.dumps(public))
        for method, path, body in [('GET','/api/admin',None),('GET','/api/export/standings.csv',None),('POST','/api/teams',{'name':'X'})]:
            self.assertEqual(self.request(method,path,body,auth=False)[0],401)
        self.assertEqual(self.request('POST','/api/login',{'username':'admin','password':'wrong'},auth=False)[0],401)
        self.login()
        self.assertEqual(self.request('POST','/api/teams',{'name':'X'},headers={'X-CSRF-Token':'wrong'})[0],403)
        self.assertEqual(self.request('POST','/api/teams',{'name':'X'},headers={'Origin':'http://evil.test'})[0],403)
        self.assertEqual(self.request('GET','/api/public',headers={'Host':'evil.test'})[0],403)
        self.assertEqual(self.request('GET','/server.py')[0],404)
        self.assertEqual(self.request('GET','/../data/results.sqlite3')[0],404)
        self.assertEqual(self.request('POST','/api/teams',raw=b'{oops',headers={'Content-Type':'application/json'})[0],400)
        self.assertEqual(self.request('POST','/api/teams',raw=b'{}',headers={'Content-Type':'text/plain'})[0],415)
        self.assertEqual(self.request('POST','/api/logout',{})[0],200)
        self.assertEqual(self.request('GET','/api/admin')[0],401)

    def test_team_crud_versions_and_persistence(self):
        self.login()
        tid = self.team(' 星火 Team ')
        self.assertEqual(self.request('POST','/api/teams',{'name':'星火 team'})[0],409)
        self.assertEqual(self.request('POST','/api/teams',{'name':'x'*121})[0],400)
        self.assertEqual(self.request('POST','/api/teams',{'name':'bad\nname'})[0],400)
        self.assertEqual(self.request('POST','/api/teams',{'name':5})[0],400)
        status, _, _ = self.request('PUT',f'/api/teams/{tid}',{'name':'新名称','short_name':'NEW','organization':'大学','version':1})
        self.assertEqual(status,200)
        self.assertEqual(self.request('PUT',f'/api/teams/{tid}',{'name':'旧编辑','version':1})[0],409)
        self.assertEqual(server_module.Store(self.db_path).teams()[0]['name'],'新名称')
        self.assertEqual(self.request('DELETE',f'/api/teams/{tid}',{'version':1})[0],409)
        self.assertEqual(self.request('DELETE',f'/api/teams/{tid}',{'version':2})[0],200)
        self.assertEqual(self.request('DELETE',f'/api/teams/{tid}',{'version':2})[0],404)
        self.assertEqual(self.snapshot()['teams'],[])

    def test_import_bom_chinese_preview_duplicates_atomic_and_stale(self):
        self.login()
        self.team('Existing')
        body = {'format':'csv','content':'\ufeff队名,简称,单位\r\n星火队,星火,中文大学\r\nExisting,E,Example\r\n星火队,X,Duplicate\r\n','duplicate_mode':'reject'}
        status, preview, _ = self.request('POST','/api/import/preview',body)
        self.assertEqual(status,200)
        self.assertEqual(preview['error_count'],2)
        self.assertEqual(len(self.snapshot()['teams']),1)
        self.assertEqual(self.request('POST','/api/import/commit',{'token':preview['token']})[0],400)
        body['duplicate_mode']='skip'
        _, preview, _ = self.request('POST','/api/import/preview',body)
        self.assertEqual(preview['skip_count'],2)
        self.assertTrue(preview['can_commit'])
        status, result, _ = self.request('POST','/api/import/commit',{'token':preview['token']})
        self.assertEqual(status,200)
        self.assertEqual(result['imported'],1)
        self.assertEqual(self.request('POST','/api/import/commit',{'token':preview['token']})[0],400)
        teams = self.snapshot()['teams']
        self.assertEqual(teams[0]['organization'],'')  # Existing never overwritten.
        self.assertEqual(teams[1]['organization'],'中文大学')
        _, preview, _ = self.request('POST','/api/import/preview',{'format':'json','content':json.dumps({'teams':[{'name':'Blue Robotics','short_name':'BR'}]})})
        self.team('Concurrent edit')
        self.assertEqual(self.request('POST','/api/import/commit',{'token':preview['token']})[0],409)
        _, preview, _ = self.request('POST','/api/import/preview',{'format':'json','content':'[{"name":"Blue Robotics"}]'})
        self.assertEqual(self.request('POST','/api/import/commit',{'token':preview['token']})[0],200)
        for content in ['name,unknown\nX,Y', 'name\nX,Extra', 'name,name\nX,Y', 'name\n"unterminated']:
            status, result, _ = self.request('POST','/api/import/preview',{'format':'csv','content':content})
            self.assertTrue(status == 400 or result['error_count'] > 0)
        self.assertEqual(self.request('POST','/api/import/preview',{'format':'json','content':'not-json'})[0],400)
        self.assertEqual(self.request('POST','/api/import/preview',{'format':'json','content':'[{"name":"Original","name":"Overwritten"}]'})[0],400)
        self.assertEqual(self.request('POST','/api/import/preview',{'format':'json','content':'[]'})[0],400)
        self.assertEqual(self.request('POST','/api/import/preview',{'format':'xlsx','content':'x'})[0],400)

    def test_match_rules_crud_and_rankings(self):
        self.login()
        a,b,c = self.team('Alpha'),self.team('Beta'),self.team('未参赛队')
        base = self.match_body(a,b)
        for change in [dict(team_b=a),dict(team_b=99999),dict(date='2026-02-30'),dict(date='20261010'),
                       dict(score_a=-1),dict(score_a=True),dict(score_a=1.5),dict(score_a=100001),dict(status=[]),
                       dict(winner_id=c),dict(winner_id=b),dict(overtime=True),dict(overtime_score_a=1),
                       dict(score_a=2,score_b=2),dict(score_a=2,score_b=2,overtime=True),
                       dict(status='live',winner_id=a),dict(status='overtime')]:
            self.assertEqual(self.request('POST','/api/matches',dict(base,**change))[0],400,change)
        status, result, _ = self.request('POST','/api/matches',base)
        self.assertEqual(status,201)
        self.assertEqual(result['winner_id'],a)
        mid=result['id']
        tied=self.match_body(a,b,score_a=2,score_b=2,overtime=True,overtime_score_a=0,overtime_score_b=1)
        status,result,_=self.request('POST','/api/matches',tied)
        self.assertEqual(status,201)
        self.assertEqual(result['winner_id'],b)
        self.assertEqual(self.request('POST','/api/matches',self.match_body(a,c,score_a=100,status='live'))[0],201)
        ranked = {t['id']:t for t in self.snapshot()['standings']}
        self.assertEqual((ranked[a]['points'],ranked[a]['wins'],ranked[a]['losses'],ranked[a]['played']), (5,1,1,2))
        self.assertEqual((ranked[b]['points'],ranked[b]['wins'],ranked[b]['losses']), (4,1,1))
        self.assertEqual(ranked[c]['played'],0)
        self.assertEqual(self.request('DELETE',f'/api/teams/{a}',{'version':1})[0],409)
        updated=dict(base,score_a=4,version=1)
        self.assertEqual(self.request('PUT',f'/api/matches/{mid}',updated)[0],200)
        self.assertEqual(self.request('PUT',f'/api/matches/{mid}',updated)[0],409)
        self.assertEqual(self.request('DELETE',f'/api/matches/{mid}',{'version':2})[0],200)
        ranked = {t['id']:t for t in self.snapshot()['standings']}
        self.assertEqual(ranked[b]['points'],3)
        self.assertEqual(ranked[a]['points'],2)
        self.assertEqual(ranked[a]['losses'],1)

    def test_rank_ties_and_export_formula_safety(self):
        self.login()
        a,b,c,d = [self.team(n) for n in ['=HYPERLINK("evil")','Beta','Gamma','Delta']]
        for x,y in [(a,b),(c,d)]:
            self.assertEqual(self.request('POST','/api/matches',self.match_body(x,y,score_a=2,score_b=1))[0],201)
        ranked = {t['id']:t for t in self.snapshot()['standings']}
        self.assertEqual(ranked[a]['rank'],ranked[c]['rank'])
        self.assertEqual(ranked[b]['rank'],ranked[d]['rank'])
        for export in ['standings','matches']:
            status, content, headers = self.request('GET',f'/api/export/{export}.csv')
            self.assertEqual(status,200)
            self.assertTrue(content.startswith(b'\xef\xbb\xbf'))
            self.assertIn("'=HYPERLINK",content.decode('utf-8-sig'))
            self.assertIn('attachment',headers['Content-Disposition'])

    def test_limits_long_names_expired_preview_and_bad_types(self):
        self.login()
        tid=self.team('很长的中文队伍' * 10)
        self.assertEqual(self.request('PUT',f'/api/teams/{tid}',{'name':'Bad version','version':True})[0],409)
        self.assertEqual(self.request('POST','/api/teams',raw=b'{"name":"\\ud800"}',headers={'Content-Type':'application/json'})[0],400)
        _,preview,_=self.request('POST','/api/import/preview',{'format':'json','content':'[{"name":"Expiring"}]'})
        sid=self.cookie.split('=',1)[1]
        self.server.sessions[sid]['previews'][preview['token']]['expires']=0
        self.assertEqual(self.request('POST','/api/import/commit',{'token':preview['token']})[0],400)
        self.assertEqual(self.request('POST','/api/teams',raw=b'{"name":"X"}',headers={'Content-Type':'application/json','Content-Length':str(server_module.MAX_BODY+1)})[0],413)
        large=json.dumps([{'name':f'Team {i}'} for i in range(5001)])
        self.assertEqual(self.request('POST','/api/import/preview',{'format':'json','content':large})[0],400)
        # Exercise data cap without sending 5000 sequential network writes.
        with self.store.connect() as db:
            db.executemany('INSERT INTO teams(name,name_key) VALUES (?,?)', [(f'Team {i}',f'team {i}') for i in range(4999)])
        self.assertEqual(self.request('POST','/api/teams',{'name':'Over cap'})[0],400)
        _,preview,_=self.request('POST','/api/import/preview',{'format':'csv','content':'name\nOver cap'})
        self.assertGreater(preview['error_count'],0)
        self.assertEqual(len(self.snapshot()['standings']),5000)

    def test_sd_mutual_kills_id_range_and_empty_password(self):
        self.login()
        a, b = self.team('SD A'), self.team('SD B')
        for oa, ob in [(0, 2), (3, 1), (4, 4)]:
            body = self.match_body(a, b, score_a=2, score_b=2, overtime=True,
                                   overtime_score_a=oa, overtime_score_b=ob)
            self.assertEqual(self.request('POST', '/api/matches', body)[0], 400)
        body = self.match_body(a, b, score_a=2, score_b=2, overtime=True,
                               overtime_score_a=3, overtime_score_b=4)
        status, result, _ = self.request('POST', '/api/matches', body)
        self.assertEqual(status, 201)
        self.assertEqual(result['winner_id'], b)
        ranked = {t['id']: t for t in self.snapshot()['standings']}
        self.assertEqual((ranked[a]['points'], ranked[b]['points']), (5, 6))
        self.assertEqual(self.request('DELETE', '/api/teams/999999999999999999999999999', {'version': 1})[0], 400)
        with self.assertRaises(server_module.APIError):
            self.store.init_password('', reset=True)
        self.assertTrue(self.store.check_password(self.password))
        generated = self.store.init_password(None, reset=True)
        self.assertGreaterEqual(len(generated), 12)
        self.assertTrue(self.store.check_password(generated))

    def test_password_change_expiry_and_rate_limit(self):
        self.login()
        self.assertEqual(self.request('POST','/api/password',{'current_password':self.password,'new_password':'short'})[0],400)
        self.assertEqual(self.request('POST','/api/password',{'current_password':'bad','new_password':'new-password-long'})[0],401)
        self.assertEqual(self.request('POST','/api/password',{'current_password':self.password,'new_password':'new-password-long'})[0],200)
        self.assertEqual(self.request('GET','/api/session')[0],401)
        self.password='new-password-long'
        self.login()
        sid=self.cookie.split('=',1)[1]
        self.server.sessions[sid]['expires']=0
        self.assertEqual(self.request('GET','/api/session')[0],401)
        for _ in range(10):
            self.assertEqual(self.request('POST','/api/login',{'username':'admin','password':'bad'},auth=False)[0],401)
        self.assertEqual(self.request('POST','/api/login',{'username':'admin','password':self.password},auth=False)[0],429)


class CLISmokeTest(unittest.TestCase):
    def test_real_cli_startup_and_http(self):
        # Inherit stdio rather than capture subprocess pipes (Windows harness boundary).
        with tempfile.TemporaryDirectory(dir=ROOT) as temp:
            with socket.socket() as probe:
                probe.bind(('127.0.0.1',0))
                port=probe.getsockname()[1]
            env=os.environ.copy()
            env['RESULTS_ADMIN_PASSWORD']='Temporary-smoke-password-7'
            process=subprocess.Popen([sys.executable,str(ROOT/'server.py'),'--host','127.0.0.1','--port',str(port),
                                      '--db',str(Path(temp)/'cli.sqlite3')],env=env,stdin=subprocess.DEVNULL)
            try:
                deadline=time.monotonic()+8
                last_error=None
                while time.monotonic()<deadline:
                    if process.poll() is not None:
                        self.fail(f'CLI exited early: {process.returncode}')
                    try:
                        connection=http.client.HTTPConnection('127.0.0.1',port,timeout=1)
                        connection.request('GET','/api/public')
                        response=connection.getresponse()
                        body=json.loads(response.read())
                        connection.close()
                        self.assertEqual(response.status,200)
                        self.assertEqual(body['teams'],[])
                        self.assertTrue((Path(temp)/'cli.sqlite3').exists())
                        break
                    except OSError as exc:
                        last_error=exc
                        time.sleep(.05)  # Bounded startup retry inside the test, not a managed-job poll.
                else:
                    self.fail(f'CLI HTTP startup timed out: {last_error}')
            finally:
                process.terminate()
                try:
                    process.wait(timeout=3)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait(timeout=3)


if __name__ == '__main__':
    unittest.main(verbosity=2, testRunner=unittest.TextTestRunner(stream=sys.stdout, verbosity=2))
