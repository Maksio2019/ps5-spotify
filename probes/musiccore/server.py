# Serves page.html to the PS5 music core and prints what the page reports.
# Run: python3 -I probes/musiccore/server.py  (port 8765, all interfaces)
import http.server, os, sys, time

PAGE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "page.html")

class Handler(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        body = open(PAGE, "rb").read()
        self.send_response(200)
        self.send_header("Content-Type", "text/html; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)
        print(time.strftime("%H:%M:%S"), "GET", self.path, "from", self.client_address[0],
              "UA:", self.headers.get("User-Agent"), flush=True)

    def do_POST(self):
        length = int(self.headers.get("Content-Length", 0))
        data = self.rfile.read(length).decode("utf-8", "replace")
        self.send_response(204)
        self.end_headers()
        print(time.strftime("%H:%M:%S"), data, flush=True)

    def log_message(self, *args):
        pass

http.server.ThreadingHTTPServer(("0.0.0.0", 8765), Handler).serve_forever()
