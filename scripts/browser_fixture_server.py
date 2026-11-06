"""Loopback-only deterministic browser fixture; not part of the ADE runtime."""
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json

PAGE = b'''<!doctype html><html><head><meta charset="utf-8"><title>ADE Browser Fixture</title></head>
<body style="font-family:sans-serif"><h1 id="probe">ADE_BROWSER_FIXTURE_4862</h1>
<button id="increment" onclick="document.getElementById('counter').textContent=String(Number(document.getElementById('counter').textContent)+1);console.log('counter incremented')">Increment</button>
<output id="counter">0</output><a href="/page2">Next page</a>
<script>console.log('ADE fixture ready');</script></body></html>'''

class Handler(BaseHTTPRequestHandler):
    def do_GET(self):
        body = PAGE if self.path != '/page2' else b'<!doctype html><title>ADE Second Page</title><h1>SECOND_PAGE</h1>'
        self.send_response(200)
        self.send_header('Content-Type', 'text/html; charset=utf-8')
        self.send_header('Content-Length', str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, fmt, *args):
        pass

if __name__ == '__main__':
    with ThreadingHTTPServer(('127.0.0.1', 0), Handler) as server:
        print(json.dumps({'url': f'http://127.0.0.1:{server.server_port}', 'purpose': 'ADE browser regression fixture'}), flush=True)
        server.serve_forever()
