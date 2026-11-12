import os

import uvicorn
from fastapi import FastAPI

# Importing app_state runs bootstrap (env, HF_HOME, data migration) before anything loads transformers
import app_state
import security
from routers import fs, indexing, media, models, queue, search, settings

app = FastAPI(title="Local Image Search API", lifespan=app_state.lifespan)

# Local-only hardening: Host/CORS checks and the image upload cap (see security.py)
security.install(app)

for _module in (settings, search, indexing, queue, models, media, fs):
    app.include_router(_module.router)


@app.get("/health")
def health():
    """Readiness probe for the desktop shell (quickjs), which waits for this before loading the UI.

    Answers as soon as the process serves requests, so a model still loading does not block the shell."""
    return {"status": "ok", "service": "vblis-api"}

# Where the API listens. The Electron main process passes these; the defaults keep a plain
# "python main.py" usable, and a second instance can run beside the first (VBLIS_API_PORT=8001).
API_HOST = os.getenv("VBLIS_API_HOST", "127.0.0.1")
API_PORT = int(os.getenv("VBLIS_API_PORT", "8000"))

def _announce(host: str, port: int) -> None:
    """Tell a standalone user where the API is and where its data lives."""
    import paths

    print(f"[VBLIS] API listening on http://{host}:{port}", flush=True)
    print(f"[VBLIS] Data folder: {paths.DATA_ROOT}", flush=True)


if __name__ == "__main__":
    import multiprocessing
    multiprocessing.freeze_support()
    import parent_watch
    parent_watch.start()
    _announce(API_HOST, API_PORT)
    uvicorn.run(app, host=API_HOST, port=API_PORT)
