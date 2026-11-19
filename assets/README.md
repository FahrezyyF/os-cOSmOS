# assets

Large data files for the cOSmOS root file system, stored as **CZIP** (`"CZIP"`, original
size, zlib stream; see `tools/czip.py` and `src/user/inflate.c`) so that everything fits
in the 4 MiB EXT2 / CosmoFS region. `make insert-shell` (and `make run`) copies them to the
disk images; programs inflate them in memory.

| File | Installed as | Content | Source |
| --- | --- | --- | --- |
| `doom1.wad` | `/games/doom1.wad` | DOOM shareware IWAD v1.9 (4,196,020 bytes, MD5 `f0cefca49926d00903cf57551d901abe`), CZIP 1.75 MB | id Software shareware episode (freely redistributable unmodified shareware) |
| `badapple.cvid` | `/media/badapple.cvid` | "Bad Apple!!" shadow-art PV as a CVID video, 128x96, 24 fps, 5,258 frames (3:39), CZIP 0.96 MB | converted with `make video VIDEO=bad_apple.mp4` from the PV (Touhou Project fan work by Alstroemeria Records / Anira) |

Regenerate:

```bash
python3 tools/czip.py doom1.wad assets/doom1.wad
make video VIDEO=bad_apple.mp4            # needs ffmpeg (or FFMPEG=/path/to/ffmpeg)
```

Both files are third-party works included only for the IF2130 course bonuses (DOOM, Bad Apple!!).
