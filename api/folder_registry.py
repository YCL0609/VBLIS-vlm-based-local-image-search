import os
import json
import pathlib
import threading
import path_keys
from security import is_within
from media_image import IMAGE_EXTENSIONS, is_macos_junk_dir, is_macos_junk_file
from media_video import VIDEO_EXTENSIONS


class FolderRegistry:
    """Per-collection selected-folder state (persisted in indexed_folders.json) and the folder views
    built on it. Every method takes the collection (or its name) explicitly."""

    def __init__(self, data_dir, cache_dir):
        self.data_dir = pathlib.Path(data_dir)
        self.cache_dir = pathlib.Path(cache_dir)
        self.selected_folders_by_collection = {}
        self._roots_synced_collections = set()
        self._roots_sync_lock = threading.Lock()
        self._load()

    def _load(self):
        config_path = self.data_dir / "indexed_folders.json"
        legacy_path = self.cache_dir / "indexed_folders.json"
        target_path = config_path if config_path.exists() else (legacy_path if legacy_path.exists() else None)
        
        if target_path:
            try:
                with open(target_path, "r", encoding="utf-8") as f:
                    data = json.load(f)
                    if isinstance(data, dict):
                        self.selected_folders_by_collection = {
                            k: set(v) for k, v in data.items() if isinstance(v, list)
                        }
                if target_path == legacy_path:
                    self.save()
                    try:
                        legacy_path.unlink()
                    except Exception:
                        pass
            except Exception as e:
                print(f"[Warning] Failed to load indexed_folders.json: {e}")

    def save(self):
        config_path = self.data_dir / "indexed_folders.json"
        try:
            data = {
                k: sorted(list(v)) for k, v in self.selected_folders_by_collection.items()
            }
            with open(config_path, "w", encoding="utf-8") as f:
                json.dump(data, f, ensure_ascii=False, indent=2)
        except Exception as e:
            print(f"[Warning] Failed to save indexed_folders.json: {e}")

    def selected(self, collection_name):
        return self.selected_folders_by_collection.setdefault(collection_name, set())

    def add(self, collection_name, folder_path: str):
        norm_new = path_keys.normalize(folder_path)
        new_key = path_keys.key(norm_new)
        existing = self.selected(collection_name)

        for ex in list(existing):
            ex_key = path_keys.key(ex)
            if path_keys.is_under(new_key, ex_key):
                return
            if path_keys.is_under(ex_key, new_key):
                existing.discard(ex)

        existing.add(norm_new)
        self.save()

    def remove(self, collection_name, folder_path: str):
        target_key = path_keys.key(folder_path)
        existing = self.selected(collection_name)
        to_remove = {
            ex for ex in existing
            if path_keys.is_under(path_keys.key(ex), target_key)
        }
        for r in to_remove:
            existing.discard(r)
        self.save()

    def forget(self, collection_name):
        """Drop the selected folders of a cleared or deleted collection."""
        self.selected_folders_by_collection.pop(collection_name, None)
        self.save()

    def is_path_allowed(self, path: str, active_collection) -> bool:
        """True when path lies in a folder indexed under any collection (used to restrict file-serving endpoints)."""
        def check():
            roots = [r for folders in list(self.selected_folders_by_collection.values()) for r in list(folders)]
            return is_within(path, roots)

        if check():
            return True
        # Older indexes can hold records whose folders were never written to indexed_folders.json
        # (only get_indexed_folders back-fills them), so do that once per collection on a miss.
        # Concurrent misses (a grid of thumbnails) wait for that back-fill instead of failing early.
        collection = active_collection
        if collection is None:
            return False
        with self._roots_sync_lock:
            if collection.name in self._roots_synced_collections:
                return check()
            # A failed back-fill is not remembered, so the next miss retries it
            if self._record_unlisted_record_folders(collection):
                self._roots_synced_collections.add(collection.name)
        return check()

    def _record_unlisted_record_folders(self, collection) -> bool:
        """Add the topmost folders of records outside every selected folder to the selected folders.
        Works on the given collection (which a model switch may no longer make the active one). Returns
        False if it failed."""
        try:
            results = collection.get(include=["metadatas"])
            selected_folders = self.selected_folders_by_collection.setdefault(collection.name, set())
            roots = list(selected_folders)
            unlisted_parents = set()
            for meta in (results or {}).get("metadatas") or []:
                if meta and "path" in meta:
                    p = path_keys.normalize(meta["path"])
                    if not is_within(p, roots):
                        unlisted_parents.add(os.path.dirname(p))
            added = False
            kept = []
            for parent in sorted(unlisted_parents, key=lambda x: len(x)):
                parent_key = path_keys.key(parent)
                if not any(path_keys.is_under(parent_key, path_keys.key(top)) for top in kept):
                    kept.append(parent)
                    selected_folders.add(parent)
                    added = True
            if added:
                self.save()
            return True
        except Exception as e:
            print(f"[Warning] Failed to record folders of unlisted index records: {e}")
            return False

    def build_tree(self, collection):
        """Get a list of unique parent directories and their subfolders currently indexed with existence status and item counts."""
        try:
            results = collection.get(include=["metadatas"])
            if not results or not results.get("metadatas"):
                return []

            seen_db_keys = set()
            db_items = []
            for meta in results["metadatas"]:
                if meta and "path" in meta:
                    p = path_keys.normalize(meta["path"])
                    m_type = meta.get("media_type", "image")
                    item_key = path_keys.key(p)
                    composite = (item_key, m_type)
                    if composite not in seen_db_keys:
                        seen_db_keys.add(composite)
                        db_items.append({"path": p, "key": item_key, "media_type": m_type})

            if not db_items:
                return []

            db_keys = {item["key"] for item in db_items}

            # 1. Start with persisted selected folders for this collection
            selected_folders = self.selected(collection.name)
            selected_before = set(selected_folders)
            active_folders = set()

            for sf in list(selected_folders):
                sf_norm = path_keys.normalize(sf)
                sf_key = path_keys.key(sf_norm)
                has_items = any(path_keys.is_under(item_key, sf_key) for item_key in db_keys)
                if has_items:
                    active_folders.add(sf_norm)
                elif not os.path.isdir(sf_norm):
                    # Keep a selected folder that exists but has no records yet (its index task just
                    # started), or /image would 403 its files until the first batch lands
                    selected_folders.discard(sf)

            # 2. For any db paths not covered by active selected folders, find their topmost common roots
            covered_keys = set()
            for sf in active_folders:
                sf_key = path_keys.key(sf)
                for item_key in db_keys:
                    if path_keys.is_under(item_key, sf_key):
                        covered_keys.add(item_key)

            uncovered_parents = set()
            for item in db_items:
                if item["key"] not in covered_keys:
                    uncovered_parents.add(os.path.dirname(item["path"]))
            if uncovered_parents:
                topmost_parents = {}
                for parent in sorted(uncovered_parents, key=lambda x: len(x)):
                    parent_key = path_keys.key(parent)
                    if not any(path_keys.is_under(parent_key, top_key) for top_key in topmost_parents):
                        topmost_parents[parent_key] = parent

                for parent in topmost_parents.values():
                    active_folders.add(parent)
                    selected_folders.add(parent)

            # 3. Collapse any nested subfolders (parent folder takes precedence as root)
            final_root_folders = set()
            for f in sorted(active_folders, key=lambda x: len(x)):
                f_key = path_keys.key(f)
                if not any(path_keys.is_under(f_key, path_keys.key(top)) for top in final_root_folders):
                    final_root_folders.add(f)

            # This runs on every poll, so only rewrite indexed_folders.json when the selection changed
            if selected_folders != selected_before:
                self.save()

            # 4. Build hierarchical tree with subfolders and file counts for each root folder
            output = []
            for root_path in sorted(final_root_folders, key=lambda x: x.lower()):
                root_key = path_keys.key(root_path)
                root_items = [it for it in db_items if path_keys.is_under(it["key"], root_key)]

                # Scan disk file counts for this root folder tree
                disk_counts_by_dir = {}
                if os.path.isdir(root_path):
                    try:
                        for r, dir_names, f_list in os.walk(root_path):
                            dir_names[:] = [d for d in dir_names if not is_macos_junk_dir(d)]
                            r_norm = path_keys.normalize(r)
                            r_key = path_keys.key(r_norm)
                            imgs = 0
                            vids = 0
                            for fname in f_list:
                                if is_macos_junk_file(fname):
                                    continue
                                ext = os.path.splitext(fname)[1].lower()
                                if ext in IMAGE_EXTENSIONS:
                                    imgs += 1
                                elif ext in VIDEO_EXTENSIONS:
                                    vids += 1
                            if imgs > 0 or vids > 0:
                                disk_counts_by_dir[r_key] = {"images": imgs, "videos": vids}
                    except Exception:
                        pass

                subfolder_data = {}  # directory key -> {"path": path as recorded, "images": n, "videos": n}
                direct_images = 0
                direct_videos = 0

                for it in root_items:
                    file_dir = os.path.dirname(it["path"])
                    file_dir_key = path_keys.key(file_dir)
                    is_img = it["media_type"] != "video"

                    if file_dir_key == root_key:
                        if is_img:
                            direct_images += 1
                        else:
                            direct_videos += 1
                    else:
                        counts = subfolder_data.get(file_dir_key)
                        if counts is None:
                            counts = subfolder_data[file_dir_key] = {"path": file_dir, "images": 0, "videos": 0}
                        if is_img:
                            counts["images"] += 1
                        else:
                            counts["videos"] += 1

                subfolders_list = []
                for sub_key, counts in subfolder_data.items():
                    sub_dir = counts["path"]
                    rel = os.path.relpath(sub_dir, root_path)
                    depth = len(rel.split(os.sep))

                    direct_disk_imgs = disk_counts_by_dir.get(sub_key, {}).get("images", counts["images"])
                    direct_disk_vids = disk_counts_by_dir.get(sub_key, {}).get("videos", counts["videos"])
                    sub_disk_imgs = sum(
                        c["images"] for d, c in disk_counts_by_dir.items() if path_keys.is_under(d, sub_key)
                    )
                    sub_disk_vids = sum(
                        c["videos"] for d, c in disk_counts_by_dir.items() if path_keys.is_under(d, sub_key)
                    )

                    subfolders_list.append({
                        "path": sub_dir,
                        "name": os.path.basename(sub_dir) or sub_dir,
                        "relative_path": rel,
                        "depth": depth,
                        "exists": os.path.isdir(sub_dir),
                        "image_count": counts["images"],
                        "video_count": counts["videos"],
                        "item_count": counts["images"] + counts["videos"],
                        "disk_image_count": max(counts["images"], sub_disk_imgs),
                        "disk_video_count": max(counts["videos"], sub_disk_vids),
                        "direct_disk_image_count": max(counts["images"], direct_disk_imgs),
                        "direct_disk_video_count": max(counts["videos"], direct_disk_vids)
                    })

                subfolders_list.sort(key=lambda s: s["relative_path"].lower())

                total_images = direct_images + sum(s["image_count"] for s in subfolders_list)
                total_videos = direct_videos + sum(s["video_count"] for s in subfolders_list)
                root_disk_imgs = sum(c["images"] for c in disk_counts_by_dir.values())
                root_disk_vids = sum(c["videos"] for c in disk_counts_by_dir.values())

                output.append({
                    "path": root_path,
                    "name": os.path.basename(root_path) or root_path,
                    "exists": os.path.isdir(root_path),
                    "direct_image_count": direct_images,
                    "direct_video_count": direct_videos,
                    "image_count": total_images,
                    "video_count": total_videos,
                    "item_count": total_images + total_videos,
                    "total_disk_image_count": max(total_images, root_disk_imgs),
                    "total_disk_video_count": max(total_videos, root_disk_vids),
                    "subfolder_count": len(subfolders_list),
                    "subfolders": subfolders_list
                })

            return output
        except Exception as e:
            print(f"Error getting indexed folders: {e}")
            return []

    def folder_contents(self, collection, folder_path: str, limit: int = 50000):
        """List supported media in a folder with indexing status and ghost record detection."""
        if not os.path.isdir(folder_path):
            raise ValueError("Folder does not exist or is not accessible.")

        normalized_folder = path_keys.normalize(folder_path)
        folder_key = path_keys.key(normalized_folder)

        # 1. Fetch all indexed paths for this folder from ChromaDB
        indexed_map = {} # path key -> {"path": meta_path, "media_type": meta_type}
        if collection is not None:
            try:
                res = collection.get(include=["metadatas"])
                if res and res.get("metadatas"):
                    for meta in res["metadatas"]:
                        if meta and "path" in meta:
                            raw_p = meta["path"]
                            p_norm = path_keys.normalize(raw_p)
                            p_key = path_keys.key(p_norm)
                            if path_keys.is_under(p_key, folder_key):
                                if p_key not in indexed_map:
                                    indexed_map[p_key] = {
                                        "path": p_norm,
                                        "media_type": meta.get("media_type", "image")
                                    }
            except Exception as e:
                print(f"[Warning] Error querying ChromaDB metadata in get_folder_contents: {e}")

        items = []
        image_count = 0
        video_count = 0
        seen_disk_paths = set()

        # 2. Scan physical files on disk
        for root, dir_names, files in os.walk(normalized_folder):
            dir_names[:] = [d for d in dir_names if not is_macos_junk_dir(d)]
            for file in files:
                if is_macos_junk_file(file) or not path_keys.is_utf8_safe(file):
                    continue
                extension = os.path.splitext(file)[1].lower()
                if extension not in IMAGE_EXTENSIONS and extension not in VIDEO_EXTENSIONS:
                    continue

                file_path = os.path.join(root, file)
                p_norm = path_keys.normalize(file_path)
                p_key = path_keys.key(p_norm)
                seen_disk_paths.add(p_key)

                media_type = "image" if extension in IMAGE_EXTENSIONS else "video"

                try:
                    stat = os.stat(file_path)
                except OSError:
                    continue

                if media_type == "image":
                    image_count += 1
                else:
                    video_count += 1

                is_indexed = p_key in indexed_map

                items.append({
                    "name": file,
                    "path": file_path,
                    "relative_path": os.path.relpath(file_path, normalized_folder),
                    "media_type": media_type,
                    "extension": extension,
                    "size": stat.st_size,
                    "modified_at": stat.st_mtime,
                    "is_indexed": is_indexed,
                    "exists_on_disk": True
                })

        # 3. Detect ghost records (in vector DB, but no longer on disk)
        missing_on_disk_items = []
        for ghost_key, info in indexed_map.items():
            if ghost_key not in seen_disk_paths:
                ghost_path = info["path"]
                ghost_name = os.path.basename(ghost_path) or ghost_path
                ghost_ext = os.path.splitext(ghost_path)[1].lower()
                ghost_media = info.get("media_type", "image")
                missing_on_disk_items.append({
                    "name": ghost_name,
                    "path": ghost_path,
                    "relative_path": os.path.relpath(ghost_path, normalized_folder),
                    "media_type": ghost_media,
                    "extension": ghost_ext,
                    "size": 0,
                    "modified_at": 0,
                    "is_indexed": True,
                    "exists_on_disk": False
                })

        all_items = items + missing_on_disk_items
        all_items.sort(key=lambda item: item["relative_path"].lower())

        indexed_count = sum(1 for it in all_items if it.get("is_indexed"))
        unindexed_count = sum(1 for it in all_items if not it.get("is_indexed") and it.get("exists_on_disk"))
        missing_on_disk_count = len(missing_on_disk_items)
        truncated = len(all_items) > limit

        return {
            "folder_path": normalized_folder,
            "items": all_items[:limit],
            "image_count": image_count,
            "video_count": video_count,
            "total_count": len(all_items),
            "indexed_count": indexed_count,
            "unindexed_count": unindexed_count,
            "missing_on_disk_count": missing_on_disk_count,
            "truncated": truncated,
            "limit": limit
        }
