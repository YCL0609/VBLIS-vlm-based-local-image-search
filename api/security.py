"""Local API hardening: Host and CORS restrictions plus an upload size cap.

The API listens on localhost only. TrustedHost rejects requests whose Host header is not a local
name, CORS names the packaged renderer (app://vblis) and the Vite dev server as the only origins
that may read responses, and UploadLimit turns away oversized image uploads before they are read.
File-system access is guarded per router (see path_keys and is_within below)."""
import path_keys
from starlette.datastructures import Headers
from starlette.middleware.cors import CORSMiddleware
from starlette.middleware.trustedhost import TrustedHostMiddleware
from starlette.responses import JSONResponse

ALLOWED_HOSTS = ["127.0.0.1", "localhost"]
# app://vblis is the packaged renderer (electron/main.js); any localhost port is the Vite dev server.
CORS_ORIGIN_REGEX = r"^(app://vblis|http://(localhost|127\.0\.0\.1)(:\d+)?)$"
MAX_UPLOAD_BYTES = 50 * 1024 * 1024
# Multipart framing added on top of the file itself
UPLOAD_OVERHEAD_BYTES = 1024 * 1024
UPLOAD_PATH = "/search/image"


def is_within(path: str, roots) -> bool:
    """True when path is one of roots or lies below one (see path_keys.is_within).

    Kept here because every guard and router already imports it from security; the platform rules
    (case sensitivity, macOS Unicode normalization) live in path_keys."""
    return path_keys.is_within(path, roots)


class UploadLimitMiddleware:
    """Rejects an oversized image upload from its Content-Length before the body is read.

    Pure ASGI middleware (not BaseHTTPMiddleware) so streamed FileResponses and the many
    thumbnail requests aren't slowed down."""

    def __init__(self, app):
        self.app = app

    async def __call__(self, scope, receive, send):
        if scope["type"] != "http" or scope["method"] != "POST" or scope["path"] != UPLOAD_PATH:
            await self.app(scope, receive, send)
            return

        length = Headers(scope=scope).get("content-length")
        if length is not None and length.isdigit() and int(length) > MAX_UPLOAD_BYTES + UPLOAD_OVERHEAD_BYTES:
            await JSONResponse({"detail": "Upload too large (max 50 MB)"}, status_code=413)(scope, receive, send)
            return

        await self.app(scope, receive, send)


def install(app) -> None:
    """Order, outermost to innermost: TrustedHost -> CORS -> UploadLimit -> app. add_middleware makes
    the last one added the outermost, so the 413 response carries the CORS headers too."""
    app.add_middleware(UploadLimitMiddleware)
    app.add_middleware(
        CORSMiddleware,
        allow_origin_regex=CORS_ORIGIN_REGEX,
        allow_methods=["GET", "POST", "DELETE"],
        allow_headers=["Content-Type"],
        allow_credentials=False,
    )
    app.add_middleware(TrustedHostMiddleware, allowed_hosts=ALLOWED_HOSTS)
