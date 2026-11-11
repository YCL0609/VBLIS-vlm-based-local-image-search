"""Start the backend on its own, without Electron.

This runs the API by hand so it can be developed, scripted or run as a service:

    python run.py                                   # http://127.0.0.1:8000, data next to this file
    python run.py --port 8011                       # a second instance beside the first
    python run.py --data-dir ~/.local/share/vblis   # keep models/index outside the repo
    python run.py --reload                          # restart on code changes (development)
"""
import argparse
import os
import socket
import sys
from pathlib import Path

API_DIR = Path(__file__).parent.resolve()


def _parse_args(argv):
    parser = argparse.ArgumentParser(
        prog="run.py",
        description="Run the local image search API without the Electron shell.",
    )
    parser.add_argument("--host", help="bind address (default 127.0.0.1, or VBLIS_API_HOST)")
    parser.add_argument("--port", type=int, help="listen port (default 8000, or VBLIS_API_PORT)")
    parser.add_argument("--data-dir", help="models/index/cache folder (default: this api folder)")
    parser.add_argument("--reload", action="store_true", help="restart the server when Python files change")
    parser.add_argument("--log-level", default="info", choices=["critical", "error", "warning", "info", "debug", "trace"])
    return parser.parse_args(argv)


def _apply_env(args) -> None:
    """Put CLI flags into the environment before anything imports paths/bootstrap."""
    if args.data_dir:
        os.environ["VBLIS_DATA_DIR"] = str(Path(args.data_dir).expanduser().resolve())
    if args.host:
        os.environ["VBLIS_API_HOST"] = args.host
    if args.port:
        os.environ["VBLIS_API_PORT"] = str(args.port)


def _port_is_free(host: str, port: int) -> bool:
    family = socket.AF_INET6 if ":" in host else socket.AF_INET
    probe_host = "::" if ":" in host else "0.0.0.0" if host in ("0.0.0.0", "") else host
    try:
        with socket.socket(family, socket.SOCK_STREAM) as sock:
            sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            sock.bind((probe_host, port))
        return True
    except OSError:
        return False


def main(argv=None) -> int:
    args = _parse_args(argv)
    _apply_env(args)
    # Allow `python api/run.py` (and scripts outside the folder) to import the modules here
    if str(API_DIR) not in sys.path:
        sys.path.insert(0, str(API_DIR))

    # Check the port before importing the app: loading torch and the model takes seconds, and a
    # second instance should fail fast with a usable hint instead of after that wait.
    host = os.getenv("VBLIS_API_HOST", "127.0.0.1")
    port = int(os.getenv("VBLIS_API_PORT", "8000"))
    if not _port_is_free(host, port):
        print(
            f"[VBLIS] {host}:{port} is already in use; pick another with --port "
            f"(e.g. python run.py --port {port + 1}).",
            file=sys.stderr,
            flush=True,
        )
        return 1

    import uvicorn

    import main as api_main
    import paths

    host, port = api_main.API_HOST, api_main.API_PORT

    device = "unknown"
    try:
        import devices

        wants_gpu = os.getenv("USE_GPU", "True").lower() == "true"
        device = devices.label(devices.preferred() if wants_gpu else devices.CPU)
    except Exception as exc:  # the torch/model layer is optional for the banner
        device = f"unknown ({type(exc).__name__})"

    def say(text: str) -> None:
        print(text, flush=True)

    say(f"[VBLIS] API on http://{host}:{port}  (device: {device})")
    say(f"[VBLIS] Data folder: {paths.DATA_ROOT}")
    say("[VBLIS] Ctrl+C to stop")

    if args.reload:
        os.chdir(API_DIR)  # uvicorn's reloader watches the current directory
        uvicorn.run(
            "main:app",
            host=host,
            port=port,
            log_level=args.log_level,
            reload=True,
            reload_dirs=[str(API_DIR)],
        )
    else:
        uvicorn.run(api_main.app, host=host, port=port, log_level=args.log_level)
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        sys.exit(130)
