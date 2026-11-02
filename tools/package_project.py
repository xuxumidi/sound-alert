"""Package the deliverable without credentials, machine-specific builds or test mail."""
from pathlib import Path
import hashlib
import zipfile

root=Path(__file__).resolve().parents[1]
skip_names={'secrets.h','compile_commands.json','manifest.sha256'}
skip_dirs={'.git','.pio','__pycache__'}
files=[p for p in root.rglob('*') if p.is_file() and p.name not in skip_names
       and not (set(p.relative_to(root).parts)&skip_dirs) and p.suffix not in {'.eml','.pem','.pyc'}]
files.sort(key=lambda p:p.relative_to(root).as_posix())
manifest=root/'manifest.sha256'
manifest.write_text(''.join(hashlib.sha256(p.read_bytes()).hexdigest()+'  '+p.relative_to(root).as_posix()+'\n' for p in files),encoding='utf-8')
archive=root.parent/'声音邮件提醒-第一版.zip'
with zipfile.ZipFile(archive,'w',zipfile.ZIP_DEFLATED) as z:
    for p in files+[manifest]: z.write(p,'sound-alert/'+p.relative_to(root).as_posix())
with zipfile.ZipFile(archive) as z:
    assert z.testzip() is None
    assert not any(Path(n).name=='secrets.h' for n in z.namelist())
    assert 'sound-alert/src/main.cpp' in z.namelist()
print(archive,archive.stat().st_size,'bytes;',len(files)+1,'files; no credentials')
