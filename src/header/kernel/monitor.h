#ifndef _MONITOR_H
#define _MONITOR_H

/**
 * cOSmOS kernel monitor: an interactive kernel-mode console used to demonstrate
 * Milestone 2 (file system CRUD on EXT2 and CosmoFS) before the user-mode shell of
 * Chapter 3 exists. Commands: type `help`.
 */
void monitor_run(void) __attribute__((noreturn));

#endif
