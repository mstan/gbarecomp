#!/usr/bin/env python3
"""Serve a web build locally with the headers a PROXY_TO_PTHREAD build needs.

SharedArrayBuffer (pthreads) is only available to cross-origin-isolated pages,
so every response carries COOP/COEP. Caching is disabled so a rebuild is picked
up on reload.

Usage: python3 packaging/web/serve.py <web dir> [port]
"""
import functools
import http.server
import sys


class Handler(http.server.SimpleHTTPRequestHandler):
    extensions_map = {
        **http.server.SimpleHTTPRequestHandler.extensions_map,
        ".wasm": "application/wasm",
        ".js": "text/javascript",
    }

    def end_headers(self):
        self.send_header("Cross-Origin-Opener-Policy", "same-origin")
        self.send_header("Cross-Origin-Embedder-Policy", "require-corp")
        self.send_header("Cache-Control", "no-store")
        super().end_headers()


def main() -> int:
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    port = int(sys.argv[2]) if len(sys.argv) > 2 else 8080
    handler = functools.partial(Handler, directory=sys.argv[1])
    with http.server.ThreadingHTTPServer(("127.0.0.1", port), handler) as server:
        print(f"serving {sys.argv[1]} on http://127.0.0.1:{port}/", flush=True)
        server.serve_forever()
    return 0


if __name__ == "__main__":
    sys.exit(main())
