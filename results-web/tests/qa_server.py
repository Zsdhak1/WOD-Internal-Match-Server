"""Isolated real HTTP fixture for the headless CDP test; never opens production data."""
import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from server import Store, AppServer

folder = Path(sys.argv[1]).resolve()
store = Store(folder / 'fixture.sqlite3')
store.init_password('Headless-only-password-7')
server = AppServer(('127.0.0.1', 0), store)
(folder / 'server-port.txt').write_text(str(server.server_address[1]), encoding='utf-8')
try:
    server.serve_forever()
finally:
    server.server_close()
