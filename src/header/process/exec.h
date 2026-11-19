#ifndef _EXEC_H
#define _EXEC_H

#include <stdint.h>
#include "header/process/process.h"

/**
 * Executable loader (book 4.1.3.2 "Load Executable"). The file has already been read into
 * a kernel buffer; the NEW process' page directory is active (CR3), so copying to user
 * addresses faults pages in on demand (and records them in the PCB page list).
 *
 * Formats, chosen by the first bytes of the file:
 *   "CEX\x7f"  cOSmOS Executable (header/process/cex.h): sections with their own protection
 *   "\x7fELF"  ELF32 i386 executable: PT_LOAD segments with their own protection
 *   anything   flat binary (book chapter 3), copied to `flat_load_address` (0x0)
 *
 * Fills pcb->memory.regions / entry / image_size and pcb->metadata.format.
 * @return PROCESS_CREATE_SUCCESS or PROCESS_CREATE_FAIL_*
 */
int32_t exec_load_image(struct ProcessControlBlock *pcb, const uint8_t *file, uint32_t size,
                        uint32_t flat_load_address);

/**
 * Validate a CEX image without loading it (also used by the host tool's self-check).
 * @return NULL when valid, otherwise a short reason
 */
const char *cex_validate(const uint8_t *file, uint32_t size);

#endif
