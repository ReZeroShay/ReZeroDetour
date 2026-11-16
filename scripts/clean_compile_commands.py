import json
import shutil
from pathlib import Path

build_dir = Path("build").resolve()
compile_db = build_dir / "compile_commands.json"

entries = json.loads(compile_db.read_text(encoding="utf-8"))

kept = []
removed = []

deps_dir = (build_dir / "_deps").resolve()

for entry in entries:
    file_path = Path(entry["file"])

    if not file_path.is_absolute():
        file_path = Path(entry["directory"]) / file_path

    try:
        file_path.resolve().relative_to(deps_dir)
        removed.append(entry)
    except ValueError:
        kept.append(entry)

print(f"Original: {len(entries)}")
print(f"Kept:     {len(kept)}")
print(f"Removed:  {len(removed)}")

# 备份
shutil.copy2(compile_db, compile_db.with_suffix(".json.bak"))

compile_db.write_text(
    json.dumps(kept, indent=2, ensure_ascii=False) + "\n", encoding="utf-8"
)
