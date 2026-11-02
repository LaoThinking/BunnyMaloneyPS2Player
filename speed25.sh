#!/bin/bash

python3 <<'PY'
from pathlib import Path

p = Path("main.c")
s = p.read_text()

old = '''(int)((elapsed * 24ULL) / kBUSCLK) + 1;'''
new = '''(int)((elapsed * 25ULL) / kBUSCLK) + 1;'''

if old not in s:
    raise SystemExit("Could not find the 24 FPS timer calculation.")

s = s.replace(old, new, 1)

s = s.replace(
    'printf("[VIDEO] Playback started. Timer-based 24 FPS sync.\\n");',
    'printf("[VIDEO] Playback started. Timer-based 25 FPS sync.\\n");'
)

p.write_text(s)

print("Playback changed from 24 FPS to 25 FPS.")
PY

make clean
make
