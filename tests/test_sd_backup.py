"""Exercise backup/export on synthetic temporary files only; no card access."""
import importlib.util
import json
import subprocess
import sys
import tempfile
from pathlib import Path
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("backup_sd", ROOT / "tools/backup_sd.py")
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)
with tempfile.TemporaryDirectory(prefix="vox-sd-backup-") as tmp:
    root = Path(tmp).resolve()
    card = root / "card"
    card.mkdir()
    active = card / "dialogue.jsonl"
    archive = card / "dialogue.1.jsonl"
    active.write_text(json.dumps({"turn_id": "new", "text": "测试"}) + "\n")
    archive.write_text(json.dumps({"turn_id": "old", "text": "旧测试"}) + "\n")
    (card / "unrelated.txt").write_text("must survive")
    manifest = module.backup(card, root / "copy")
    assert active.exists() and archive.exists() and "unrelated.txt" not in manifest
    assert manifest[archive.name]["sha256"] == module.digest(archive)
    output = subprocess.check_output([sys.executable, str(ROOT / "tools/export_dialogue.py"), str(root / "copy"), "--json"], text=True)
    assert {json.loads(line)["turn_id"] for line in output.splitlines()} == {"new", "old"}
    events = []
    original_sync, original_unlink = module.sync_directory, Path.unlink
    def synced(path):
        events.append(("sync", path))
        original_sync(path)
    def removed(path, *args, **kwargs):
        events.append(("remove", path))
        return original_unlink(path, *args, **kwargs)
    destination = root / "nested" / "retire"
    with patch.object(module, "sync_directory", synced), patch.object(Path, "unlink", removed):
        module.backup(card, destination, True)
    removed_at = next(i for i, event in enumerate(events) if event[0] == "remove")
    assert all(("sync", path) in events[:removed_at] for path in (destination, destination.parent, root))
    assert events[-1] == ("sync", card)
    assert active.exists() and not archive.exists() and (card / "unrelated.txt").exists()
    archive.write_text("kept\n")
    with patch.object(module, "sync_directory", side_effect=OSError("flush failed")):
        try:
            module.backup(card, root / "unflushed", True)
        except OSError:
            pass
        else:
            raise AssertionError("Directory flush failure must prevent source deletion")
    assert archive.exists()
    (card / "erase.pending").write_text("pending")
    try:
        module.backup(card, root / "fenced", True)
    except ValueError:
        pass
    else:
        raise AssertionError("Deletion fence must prevent archive retirement")
    assert archive.exists()
    (card / "erase.pending").unlink()
    (card / "context0.json").symlink_to(active)
    try:
        module.backup(card, root / "symlink", True)
    except ValueError:
        pass
    else:
        raise AssertionError("Symlinks must be refused")
    assert archive.exists()
print("PASS: synthetic verified backup, explicit archive retirement, deletion fence, symlink and multi-volume export")
