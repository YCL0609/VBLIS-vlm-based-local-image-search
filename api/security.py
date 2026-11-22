"""Local API hardening: Host/CORS restrictions, upload cap, log redaction, and the desktop owner.

The API listens on localhost only. TrustedHost rejects requests whose Host header is not a local
name, CORS names the Vite dev server and the QuickJS shell as the only origins that may read
responses, and the upload check turns away oversized image uploads before they are read.

There is no shared-secret token: the QuickJS shell serves the page and starts the backend as the
same user, so every request on this loopback interface belongs to the local owner. The network
listener has its own account and session layer (see network_gate.py); this module only marks
requests on the desktop app.
"""
import logging
import os
import re

from starlette.datastructures import Headers
from starlette.middleware.cors import CORSMiddleware
from starlette.middleware.trustedhost import TrustedHostMiddleware
from starlette.responses import JSONResponse

from auth_context import LOCAL_OWNER, SCOPE_KEY

ALLOWED_HOSTS = ["127.0.0.1", "localhost"]
# The QuickJS shell serves the UI at 127.0.0.1:11120 and the API at 127.0.0.1:8000; both are the
# same loopback origin as far as the browser is concerned, but any localhost port may read.
CORS_ORIGIN_REGEX = r"^http://(localhost|127\.0\.0\.1)(:\d+)?$"
MAX_UPLOAD_BYTES = 50 * 1024 * 1024
# Multipart framing added on top of the file itself
UPLOAD_OVERHEAD_BYTES = 1024 * 1024
UPLOAD_PATH = "/search/image"

# Once the network listener runs, the access log also stops showing which files were asked for
_PATH_QUERY_RE = re.compile(r"([?&](?:path|folder_path)=)[^&\s\"]*")
_redact_paths = False


def redact_paths_in_access_log(enabled: bool = True) -> None:
    global _redact_paths
    _redact_paths = enabled


def is_within(path: str, roots) -> bool:
    """True when path is one of roots or lies below one. Purely lexical (abspath + normcase), so
    '..' is resolved and 'C:\\Photos2' is not inside 'C:\\Photos'. Symlinks/junctions are
    deliberately not resolved: creating one needs local write access, which already implies file access."""
    if not path or "\0" in path:
        return False
    try:
        key = os.path.normcase(os.path.abspath(path))
        for root in roots:
            if not root:
                continue
            root_key = os.path.normcase(os.path.abspath(root))
            if key == root_key or key.startswith(root_key.rstrip(os.sep) + os.sep):
                return True
    except (ValueError, OSError):
        return False
    return False


class LocalOwnerMiddleware:
    """Marks every desktop request as the local owner, and rejects an oversized image upload
    before its body is read.

    Pure ASGI middleware (not BaseHTTPMiddleware) so streamed FileResponses and the many
    thumbnail requests aren't slowed down. The network listener does not use this module: its
    gate sets the scope from the signed-in account instead."""

    def __init__(self, app):
        self.app = app

    async def __call__(self, scope, receive, send):
        scope_type = scope["type"]
        if scope_type == "lifespan":
            await self.app(scope, receive, send)
            return

        scope[SCOPE_KEY] = LOCAL_OWNER

        if scope_type == "http" and scope["method"] == "POST" and scope["path"] == UPLOAD_PATH:
            length = Headers(scope=scope).get("content-length")
            if length is not None and length.isdigit() and int(length) > MAX_UPLOAD_BYTES + UPLOAD_OVERHEAD_BYTES:
                await JSONResponse({"detail": "Upload too large (max 50 MB)"}, status_code=413)(scope, receive, send)
                return

        await self.app(scope, receive, send)


def install(app) -> None:
    """Order, outermost to innermost: TrustedHost -> CORS -> LocalOwner -> app. add_middleware
    makes the last one added the outermost, so a 413 response carries the CORS headers too."""
    app.add_middleware(LocalOwnerMiddleware)
    app.add_middleware(
        CORSMiddleware,
        allow_origin_regex=CORS_ORIGIN_REGEX,
        allow_methods=["GET", "POST", "DELETE"],
        allow_headers=["Content-Type"],
        allow_credentials=False,
    )
    app.add_middleware(TrustedHostMiddleware, allowed_hosts=ALLOWED_HOSTS)


class RedactPathsFilter(logging.Filter):
    """uvicorn's access log prints the request path with its query string; hide local paths."""

    def filter(self, record: logging.LogRecord) -> bool:
        if isinstance(record.args, tuple):
            record.args = tuple(
                _PATH_QUERY_RE.sub(r"\1***", a) if isinstance(a, str) else a for a in record.args
            )
        return True


def install_access_log_redaction() -> None:
    access_logger = logging.getLogger("uvicorn.access")
    if not any(isinstance(f, RedactPathsFilter) for f in access_logger.filters):
        access_logger.addFilter(RedactPathsFilter())
