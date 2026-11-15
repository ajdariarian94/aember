import logging
import webbrowser
from functools import partial
from http.server import HTTPServer, SimpleHTTPRequestHandler
from pathlib import Path
from threading import Thread

from aember.core.context import Context
from aember.core.step import Step

log = logging.getLogger("aember.docs.serve")


class SilentHTTPRequestHandler(SimpleHTTPRequestHandler):
    def log_message(self, format: str, *args) -> None:
        pass  # suppress request logs


class DocsServeStep(Step):

    def __init__(self, doc_path: Path | str = "doc/generated/html", port: int = 8000):
        self.doc_path = Path(doc_path)
        self.port = port

    def run(self, ctx: Context) -> None:
        doc_path = self.doc_path.resolve()

        if not doc_path.exists():
            raise FileNotFoundError(f"Documentation path does not exist: {doc_path}")

        log.info("Serving documentation at http://localhost:%d/", self.port)
        log.debug("Documentation directory: %s", doc_path)

        handler = partial(SilentHTTPRequestHandler, directory=str(doc_path))
        server = HTTPServer(("0.0.0.0", self.port), handler)

        Thread(
            target=lambda: webbrowser.open(f"http://localhost:{self.port}/"),
            daemon=True,
        ).start()

        try:
            log.info("Documentation server started (Ctrl+C to stop)")
            server.serve_forever()
        except KeyboardInterrupt:
            log.info("Stopping documentation server")
        finally:
            server.server_close()
