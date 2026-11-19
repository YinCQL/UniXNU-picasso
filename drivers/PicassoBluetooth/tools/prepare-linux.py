from pathlib import Path
import hashlib,json
root=Path(__file__).resolve().parents[1]
lock=json.loads((root/'tools/source-lock.json').read_text())
for name,digest in lock['files'].items():
    if hashlib.sha256((root/'linux'/Path(name).name).read_bytes()).hexdigest()!=digest:
        raise SystemExit('Locked Linux source differs: '+name)
print('Verified locked Linux btqca sources')
