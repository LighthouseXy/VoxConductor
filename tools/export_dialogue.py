"""Read a copied SD journal without changing it. Never contacts a network service."""
import argparse
import json
from pathlib import Path


def records(path):
    with path.open(encoding="utf-8", errors="replace") as stream:
        for line in stream:
            if not line.strip():
                continue
            try:
                row = json.loads(line)
            except (ValueError, TypeError):
                continue
            if isinstance(row, dict) and isinstance(row.get("turn_id"), str):
                yield row


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("journal", type=Path, help="Copied journal file or copied SD directory (all managed volumes)")
    parser.add_argument("--turn", help="Only show this turn_id")
    parser.add_argument("--json", action="store_true", help="Output valid JSONL")
    args = parser.parse_args()
    paths = ([p for p in sorted(args.journal.glob("dialogue.[1-4].jsonl"))] + [args.journal / "dialogue.jsonl"]
             if args.journal.is_dir() else [args.journal])
    for row in (row for path in paths if path.exists() for row in records(path)):
        if args.turn and row["turn_id"] != args.turn:
            continue
        if args.json:
            print(json.dumps(row, ensure_ascii=False))
        else:
            # JSON quoting keeps control characters from becoming terminal commands.
            print(json.dumps({key: row[key] for key in
                  ("turn_id", "event", "text", "text_offset", "text_total_bytes", "error", "generation_status", "playback_status")
                  if key in row}, ensure_ascii=False))


if __name__ == "__main__":
    main()
