"""Read-only directory listing for the folder picker.

A page served by the desktop shell cannot learn an absolute path from a drag-and-drop or from a
native folder dialog, so the web UI asks the API which folders exist instead. The listing is
directory-only: file names are never returned, and nothing here reads file contents.
"""
import os

from fastapi import APIRouter, HTTPException, Query

router = APIRouter(tags=["fs"])

# A folder with more subfolders than this (a network share, /usr/bin) is cut off rather than
# serialized whole; the UI shows the first page and the user can pick a child to drill down.
MAX_ENTRIES = 5000


def _home_directory() -> str:
    home = os.path.expanduser("~")
    return home if os.path.isabs(home) else os.sep


@router.get("/fs/list")
def list_directory(path: str | None = Query(None, description="Absolute folder to list; the home folder when omitted.")):
    requested = (path or "").strip()
    target = os.path.abspath(os.path.expanduser(requested)) if requested else _home_directory()

    if not os.path.exists(target):
        raise HTTPException(status_code=404, detail=f"Folder not found: {target}")
    if not os.path.isdir(target):
        raise HTTPException(status_code=400, detail=f"Not a folder: {target}")

    entries = []
    truncated = False
    try:
        with os.scandir(target) as scanner:
            for entry in scanner:
                try:
                    if not entry.is_dir():
                        continue
                except OSError:
                    # A dangling link or an entry we may not stat is simply not offered
                    continue
                entries.append({"name": entry.name, "path": entry.path, "hidden": entry.name.startswith(".")})
                if len(entries) >= MAX_ENTRIES:
                    truncated = True
                    break
    except PermissionError:
        raise HTTPException(status_code=403, detail=f"Permission denied: {target}")
    except OSError as exc:
        raise HTTPException(status_code=400, detail=f"Cannot read folder: {target} ({exc.strerror or exc})")

    entries.sort(key=lambda item: item["name"].casefold())
    parent = os.path.dirname(target)
    return {
        "path": target,
        "parent": parent if parent and parent != target else None,
        "home": _home_directory(),
        "separator": os.sep,
        "truncated": truncated,
        "entries": entries,
    }
