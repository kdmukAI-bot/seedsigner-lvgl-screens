#!/usr/bin/env python3
"""Drop-in `python3 -m http.server` replacement that disables client caching.

The web playground and screenshot gallery fetch their assets (locales.json,
scenario catalogs, font packs, the WASM bundle) at runtime. Served with plain
http.server those responses carry only Last-Modified and no Cache-Control, so
browsers apply heuristic caching and keep serving a stale locales.json / .wasm
after a rebuild — i.e. a freshly added locale or font pack doesn't appear until a
manual hard-refresh. For a live LAN dev server that's a footgun, so every
response here is marked no-store.

Same CLI shape as http.server: `nocache_server.py [PORT] [--bind ADDR] [--directory DIR]`.
"""

import argparse
import functools
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer


class NoCacheHandler(SimpleHTTPRequestHandler):
    def end_headers(self):
        self.send_header("Cache-Control", "no-store, no-cache, must-revalidate, max-age=0")
        self.send_header("Pragma", "no-cache")
        self.send_header("Expires", "0")
        super().end_headers()


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("port", nargs="?", type=int, default=8000)
    ap.add_argument("--bind", default="0.0.0.0")
    ap.add_argument("--directory", default=".")
    args = ap.parse_args()

    handler = functools.partial(NoCacheHandler, directory=args.directory)
    with ThreadingHTTPServer((args.bind, args.port), handler) as httpd:
        print(f"no-cache server on {args.bind}:{args.port} serving {args.directory}", flush=True)
        httpd.serve_forever()


if __name__ == "__main__":
    main()
