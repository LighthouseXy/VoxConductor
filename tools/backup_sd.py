"""Back up only VoxConductor-managed files from a mounted, offline SD card.

Never use while ESP32 is writing the same filesystem. Archives are retired only
with an explicit flag, after a byte-for-byte verified copy and manifest exist.
"""
import argparse
import hashlib
import json
import os
import shutil
from pathlib import Path

FILES = ("context0.json", "context1.json", "dialogue.jsonl", "history.idx",
         "erase.pending", "dialogue.rewrite", "conversations.txt",
         *(f"dialogue.{i}.jsonl" for i in range(1, 5)))


def digest(path):
    h = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(65536), b""):
            h.update(block)
    return h.hexdigest()


def sync_directory(path):
    descriptor = os.open(path, os.O_RDONLY)
    try:
        os.fsync(descriptor)
    finally:
        os.close(descriptor)


def backup(source, destination, retire=False):
    source, destination = source.resolve(), destination.resolve()
    if not source.is_dir() or destination == source or source in destination.parents:
        raise ValueError("Backup destination must be outside the SD source directory")
    new_directories = []
    parent = destination
    while not parent.exists():
        new_directories.append(parent)
        parent = parent.parent
    destination.mkdir(parents=True, exist_ok=False)
    manifest = {}
    for name in FILES:
        original = source / name
        if original.is_symlink():
            raise ValueError(f"Refusing symlink: {name}")
        if not original.exists():
            continue
        before = digest(original)
        target = destination / name
        shutil.copyfile(original, target)
        with target.open("rb") as stream:
            os.fsync(stream.fileno())
        if before != digest(target) or before != digest(original):
            raise ValueError(f"File changed while copying: {name}; nothing retired")
        manifest[name] = {"sha256": before, "bytes": target.stat().st_size}
    if not manifest:
        raise ValueError("No managed files found; nothing retired")
    with (destination / "manifest.json").open("w", encoding="utf-8") as stream:
        stream.write(json.dumps(manifest, indent=2) + "\n")
        stream.flush()
        os.fsync(stream.fileno())
    # 文件内容、文件名和新目录链都落盘后，才允许删除原封存卷。
    for directory in [*new_directories, parent]:
        sync_directory(directory)
    if retire:
        if (source / "erase.pending").exists():
            raise ValueError("Deletion is incomplete; resolve it before retiring archives")
        # Verify all archives again before removing any of them.
        archives = [name for name in manifest if name in FILES[-4:]]
        for name in archives:
            if digest(source / name) != manifest[name]["sha256"] or digest(destination / name) != manifest[name]["sha256"]:
                raise ValueError("Archive changed; nothing retired")
        try:
            for name in archives:
                (source / name).unlink()
        finally:
            if archives:
                sync_directory(source)
    return manifest


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("destination", type=Path, help="New directory outside the SD card")
    parser.add_argument("--retire-archives", action="store_true", help="Remove ONLY the four archived journals after verified backup; keep active journal and snapshots")
    args = parser.parse_args()
    result = backup(args.source, args.destination, args.retire_archives)
    print(f"Verified {len(result)} managed files. Backup contains private plaintext dialogue; keep it private.")


if __name__ == "__main__":
    main()
