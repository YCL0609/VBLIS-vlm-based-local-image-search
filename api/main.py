import os

import uvicorn
from fastapi import FastAPI

# Importing app_state runs bootstrap (env, HF_HOME, data migration) before anything loads transformers
import app_state
import security
from bootstrap import API_TOKEN
from routers import indexing, media, models, queue, search, settings

app = FastAPI(title="Local Image Search API", lifespan=app_state.lifespan)

# Only clients holding the token (the Electron app passes it via env) may call the API
security.install(app, API_TOKEN)

for _module in (settings, search, indexing, queue, models, media):
    app.include_router(_module.router)

# Where the API listens. The Electron main process passes these; the defaults keep a plain
# "python main.py" usable, and a second instance can run beside the first (VBLIS_API_PORT=8001).
API_HOST = os.getenv("VBLIS_API_HOST", "127.0.0.1")
API_PORT = int(os.getenv("VBLIS_API_PORT", "8000"))

def _announce(host: str, port: int) -> None:
    """Tell a standalone user where the API is and where the client token lives."""
    import paths

    print(f"[VBLIS] API listening on http://{host}:{port}", flush=True)
    print(f"[VBLIS] Data folder: {paths.DATA_ROOT}", flush=True)
    token_path = security.token_file_path()
    if not os.getenv(security.API_TOKEN_ENV) and token_path is not None:
        print(f"[VBLIS] Client token: {token_path} (send as {security.API_TOKEN_HEADER})", flush=True)


if __name__ == "__main__":
    import multiprocessing
    multiprocessing.freeze_support()
    import parent_watch
    parent_watch.start()
    _announce(API_HOST, API_PORT)
    uvicorn.run(app, host=API_HOST, port=API_PORT)
