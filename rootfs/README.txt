Welcome to cOSmOS!

This file lives on the root file system chosen in the GRUB menu (EXT2 or CosmoFS)
and was put here from the host by `make insert-shell` (rootfs/ in the repository).

Programs (every one is its own process):
  /bin    separate shell commands (ELF):  ls cat grep ps kill pmap sched ...
  /apps   applications (CEX):  hello clock top counter paint bounce gclock
          ifconfig ping nslookup udpsend udpecho
  /games  snake  (not in PATH at first: `export PATH=$PATH:/games`)

Things to try in the shell:
  hello a b c                one program, its argc / argv
  clock                      background clock in the bottom-right corner
  ps -l ; top                processes, CPU, memory (q quits top)
  counter A 300 5 & counter B 500 5 &    two programs at once
  sh scripts/m4demo.sh       processes, pipes, PATH and the scheduler in one script
  gclock ; bounce ; paint    windows (graphics mode), drag them, click to focus
  ls /bin | grep -c .        a pipeline of two separate programs
  path                       PATH as a dynamic array (saved in /etc/environment)
  pmap 1                     memory regions and page list of the shell
  execinfo hello             the CEX executable header of /apps/hello
  ping 10.0.2.2 ; nslookup example.com     the network card (UDP / ICMP)
  cat docs/planets.txt | grep -n "gas|ice"
  find / "*.sh"              wildcard search over the tree
  memtest 8                  allocate 8 MiB and verify it (forces swapping on small RAM)
Ctrl+C stops the program in the foreground; `kill <pid>` any other one.
