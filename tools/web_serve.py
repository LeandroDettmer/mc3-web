#!/usr/bin/env python3
"""Development server for the browser build (make web-serve).

    python3 tools/web_serve.py --root work-remix/build-web --disc game-remix-usa

Serves the build at / and the extracted disc at /disc/, with what the page
needs that a plain static server lacks. Both options repeat: a file missing
from the first --root is looked up in the next (the start page from one,
mcdub3.js / mcdub3remix.js from both games' builds); every --disc is also at
/discs/<folder name>/, and /discs.json lists them for the start page's game
picker (the first is /disc/).
  - Cross-Origin-Opener-Policy / -Embedder-Policy: threads (SharedArrayBuffer)
    only exist in a cross-origin isolated page;
  - HTTP Range requests: the game reads the disc in 1 MB ranges (src/web_fs.c);
  - /disc.list: the disc's files, one relative path per line (WASMFS has no
    directory listing over HTTP);
  - /disc.manifest: the same files with size and mtime, JSON (web/disc_cache.js:
    a file changed here gets a new key in the browser's disc cache).
Standard library only. Binds to localhost unless --bind says otherwise: the
disc is yours, not the network's.
"""
import argparse
import io
import json
import os
import re
import sys
import urllib.parse
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer

DISC_PREFIX = "/disc"
LIST_PATH = DISC_PREFIX + ".list"
DISCS_PREFIX = "/discs/"
DISCS_JSON = "/discs.json"


class _Slice:
    """The first `n` bytes from a file's current position (a Range body)."""

    def __init__(self, f, n):
        self.f, self.n = f, n

    def read(self, k=-1):
        k = self.n if k is None or k < 0 else min(k, self.n)
        self.n -= k
        return self.f.read(k) if k else b""

    def close(self):
        self.f.close()


def disc_list(disc):
    paths = []
    for top, dirs, files in os.walk(disc):
        dirs.sort()
        for name in sorted(files):
            paths.append(os.path.relpath(os.path.join(top, name), disc).replace(os.sep, "/"))
    return ("\n".join(paths) + "\n").encode("utf-8")


def disc_manifest(disc):
    files = []
    for rel in disc_list(disc).decode("utf-8").split("\n"):
        if rel:
            st = os.stat(os.path.join(disc, *rel.split("/")))
            files.append({"p": rel, "s": st.st_size, "m": int(st.st_mtime)})
    return json.dumps({"files": files}).encode("utf-8")


def disc_info(path):
    """The start page's view of a disc folder: game-remix* is the Remix, the
    rest the DUB Edition; the edition is what follows the game's prefix."""
    name = os.path.basename(path)
    game, prefix = ("remix", "game-remix") if name.startswith("game-remix") else ("dub", "game")
    return {"name": name, "game": game, "edition": name[len(prefix):].lstrip("-")}


class Handler(SimpleHTTPRequestHandler):
    root = "."
    roots = []
    disc = None
    discs = {}
    extensions_map = {**SimpleHTTPRequestHandler.extensions_map,
                      ".wasm": "application/wasm", ".js": "text/javascript"}

    def end_headers(self):
        self.send_header("Cross-Origin-Opener-Policy", "same-origin")
        self.send_header("Cross-Origin-Embedder-Policy", "require-corp")
        self.send_header("Cross-Origin-Resource-Policy", "same-origin")
        self.send_header("Accept-Ranges", "bytes")
        self.send_header("Cache-Control", "no-cache")     # always the newest build
        super().end_headers()

    def _url_path(self):
        return urllib.parse.unquote(urllib.parse.urlsplit(self.path).path)

    def translate_path(self, path):
        p = urllib.parse.unquote(urllib.parse.urlsplit(path).path)
        bases = self.roots or [self.root]
        if self.disc and (p == DISC_PREFIX or p.startswith(DISC_PREFIX + "/")):
            bases, p = [self.disc], p[len(DISC_PREFIX):]
        elif p.startswith(DISCS_PREFIX):
            name, _, rest = p[len(DISCS_PREFIX):].partition("/")
            if name in self.discs:
                bases, p = [self.discs[name]], rest
        # Never above the base: '..', '.' and empty parts (WASMFS asks for
        # /disc//file) are dropped.
        parts = [x for x in p.split("/") if x not in ("", ".", "..")]
        for base in bases:
            full = os.path.join(base, *parts)
            if os.path.exists(full):
                return full
        return os.path.join(bases[0], *parts)

    def _send_bytes(self, body, ctype):
        self.send_response(200)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        return io.BytesIO(body)

    def send_head(self):
        up = self._url_path()
        if self.disc and up == LIST_PATH:
            return self._send_bytes(disc_list(self.disc), "text/plain; charset=utf-8")
        if up == DISCS_JSON:
            body = json.dumps([disc_info(d) for d in self.discs.values()]).encode("utf-8")
            return self._send_bytes(body, "application/json")
        if up.startswith(DISCS_PREFIX) and up.endswith(".list") and up[len(DISCS_PREFIX):-5] in self.discs:
            return self._send_bytes(disc_list(self.discs[up[len(DISCS_PREFIX):-5]]), "text/plain; charset=utf-8")
        if self.disc and up == DISC_PREFIX + ".manifest":
            return self._send_bytes(disc_manifest(self.disc), "application/json")
        if up.startswith(DISCS_PREFIX) and up.endswith(".manifest") and up[len(DISCS_PREFIX):-9] in self.discs:
            return self._send_bytes(disc_manifest(self.discs[up[len(DISCS_PREFIX):-9]]), "application/json")

        rng = self.headers.get("Range")
        path = self.translate_path(self.path)
        m = re.fullmatch(r"\s*bytes=(\d*)-(\d*)\s*", rng or "")
        if not m or not os.path.isfile(path) or not (m.group(1) or m.group(2)):
            return super().send_head()          # whole file, listing, 404; multi-range too

        size = os.path.getsize(path)
        if m.group(1):
            start = int(m.group(1))
            end = min(int(m.group(2)), size - 1) if m.group(2) else size - 1
        else:                                   # bytes=-n: the last n bytes
            start, end = max(size - int(m.group(2)), 0), size - 1
        if start >= size or start > end:
            self.send_response(416)
            self.send_header("Content-Range", f"bytes */{size}")
            self.send_header("Content-Length", "0")
            self.end_headers()
            return None
        f = open(path, "rb")
        f.seek(start)
        self.send_response(206)
        self.send_header("Content-Type", self.guess_type(path))
        self.send_header("Content-Range", f"bytes {start}-{end}/{size}")
        self.send_header("Content-Length", str(end - start + 1))
        self.send_header("Last-Modified", self.date_time_string(int(os.path.getmtime(path))))
        self.end_headers()
        return _Slice(f, end - start + 1)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--root", required=True, action="append",
                    help="the build: work-<game>/build-web (repeat: files missing in one come from the next)")
    ap.add_argument("--disc", action="append",
                    help="an extracted disc (folder with default.xbe); the first at /disc/, each at /discs/<name>/")
    ap.add_argument("--port", type=int, default=3000)
    ap.add_argument("--bind", default="127.0.0.1", help="address to listen on (default: localhost only)")
    a = ap.parse_args()

    Handler.roots = [os.path.abspath(r) for r in a.root]
    Handler.root = Handler.roots[0]
    if not os.path.isfile(os.path.join(Handler.root, "index.html")):
        sys.exit(f"error: no index.html in {Handler.root}: build first (make web)")
    for d in a.disc or []:
        d = os.path.abspath(d)
        if not os.path.isfile(os.path.join(d, "default.xbe")):
            sys.exit(f"error: no default.xbe in {d}")
        Handler.discs[os.path.basename(d)] = d
    if a.disc:
        Handler.disc = next(iter(Handler.discs.values()))
    else:
        print("note: no --disc: the page will load, the game will not find its disc", file=sys.stderr)

    httpd = ThreadingHTTPServer((a.bind, a.port), Handler)
    print(f"serving {Handler.root} at http://{'localhost' if a.bind == '127.0.0.1' else a.bind}:{a.port}/"
          + (f", disc {Handler.disc} at /disc/" if Handler.disc else "")
          + (f" ({len(Handler.discs)} discs at /discs/)" if len(Handler.discs) > 1 else ""))
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
