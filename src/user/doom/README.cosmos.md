# DOOM for cOSmOS

Source: [doomgeneric](https://github.com/ozkl/doomgeneric) (commit e257a09, GPL-2.0, see
`LICENSE`), the portable files only. cOSmOS adds `doomgeneric_cosmos.c` (window, keys,
timing) and builds it against the small C library in `src/user/libc`. The original files are
unmodified. Build: `make user-programs` -> `/games/doom` (CEX). Run: `/games/doom` (or
`export PATH=$PATH:/games`, then `doom`), WAD: `/games/doom1.wad`.
