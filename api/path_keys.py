"""One definition of path identity for the whole backend.

Every place that stores, compares or looks up a file path goes through here, so the meaning of
"the same file" is defined once per platform:

* Linux filesystems (ext4, btrfs, xfs, zfs) are case-sensitive: ``/Media/Cat.jpg`` and
  ``/Media/cat.jpg`` are two different files, so paths are compared exactly.
* Windows NTFS and the default macOS APFS are case-insensitive: both spellings name one file, so
  the comparison key is case-folded.
* macOS also returns decomposed (NFD) file names while the strings a user types or pastes are
  usually composed (NFC). Both spellings name one file there, so macOS paths are reduced to NFC
  before they are stored or compared (HFS+ normalizes to NFD, APFS keeps whatever was written,
  so normalizing is the only way to make the two agree).

``normalize()`` is the spelling to store and show; ``key()`` is the spelling to compare. Keys
always keep the platform separator of the running system (``os.sep``), so a key is still a real
path on this machine.
"""
import os
import sys
import unicodedata

#: Platforms whose default filesystem compares file names case-insensitively.
CASE_INSENSITIVE = sys.platform == "win32" or sys.platform == "darwin"
_IS_WINDOWS = sys.platform == "win32"
_IS_MACOS = sys.platform == "darwin"


def normalize(path) -> str:
    """Absolute, ``..``-free path, NFC on macOS. This is the spelling the backend stores and returns."""
    normalized = os.path.abspath(os.path.normpath(os.fspath(path)))
    if _IS_MACOS:
        normalized = unicodedata.normalize("NFC", normalized)
    return normalized


def key(path) -> str:
    """The comparison key of a path: filesystem case rules applied, still a usable path on this host."""
    normalized = normalize(path)
    if _IS_WINDOWS:
        # normcase is the exact rule NTFS implements (case folding + backslashes), and keeps keys
        # comparable with paths/keys written by older builds of the app.
        return os.path.normcase(normalized)
    if _IS_MACOS:
        return normalized.casefold()
    return normalized


def name_key(name: str) -> str:
    """Comparison key of a file *name* (one path component): NFC on macOS, case-folded where the
    filesystem is case-insensitive. Unlike key(), it never resolves the name against the cwd."""
    if _IS_MACOS:
        name = unicodedata.normalize("NFC", name)
    if _IS_WINDOWS:
        return name.lower()
    if _IS_MACOS:
        return name.casefold()
    return name


def is_utf8_safe(path: str) -> bool:
    """Whether a path read from the filesystem can be stored and returned as UTF-8. Linux file names are byte
    strings: Python decodes the byte that does not form UTF-8 as a lone surrogate, and any later UTF-8 encode
    of that string (Chroma metadata, a JSON response) fails for the whole batch. Such files are better skipped
    than allowed to break the indexing of their folder."""
    try:
        path.encode("utf-8")
    except UnicodeEncodeError:
        return False
    return True


def same(left, right) -> bool:
    """True when the two paths name the same file on this platform."""
    try:
        return key(left) == key(right)
    except (TypeError, ValueError, OSError):
        return False


def is_within(path, roots) -> bool:
    """True when path is one of roots or lies below one.

    Purely lexical (no realpath), so ``..`` is resolved and ``/Photos2`` is not inside ``/Photos``.
    Symlinks are deliberately not resolved: creating one already needs local write access, which
    implies the ability to read through it.
    """
    if not path or "\0" in str(path):
        return False
    try:
        path_key = key(path)
        for root in roots:
            if not root:
                continue
            root_key = key(root)
            if path_key == root_key or path_key.startswith(root_key.rstrip(os.sep) + os.sep):
                return True
    except (TypeError, ValueError, OSError):
        return False
    return False


def is_under(path_key: str, root_key: str) -> bool:
    """True when an already-computed key lies under a root key (or equals it).

    Useful inside loops that compare many paths against the same roots without re-normalizing.
    """
    if not path_key or not root_key:
        return False
    root_key = root_key.rstrip(os.sep)
    return path_key == root_key or path_key.startswith(root_key + os.sep)
