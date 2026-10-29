Welcome to cOSmOS!

This file lives on the root file system chosen in the GRUB menu (EXT2 or CosmoFS)
and was put here from the host by `make insert-shell` (rootfs/ in the repository).

Things to try in the shell:
  ls -l                      list this folder with sizes and dates
  tree                       the whole tree
  cat docs/planets.txt | grep -n "gas|ice"
  find planets.txt           indexed lookup by name
  find / "*.sh"              wildcard search over the tree
  sh scripts/demo.sh         run a shell script (or just: scripts/demo.sh)
  free                       paging and swap statistics
  memtest 8                  allocate 8 MiB and verify it (forces swapping on small RAM)
  heap                       malloc / kmalloc statistics
