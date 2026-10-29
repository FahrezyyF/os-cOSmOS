#include "header/cpu/gdt.h"
#include "header/cpu/interrupt.h"

/**
 * global_descriptor_table, predefined GDT.
 * Initial SegmentDescriptor already set properly according to Intel Manual & OSDev.
 * Table entry : [{Null Descriptor}, {Kernel Code}, {Kernel Data (variable, etc)},
 * ...].
 */
struct GlobalDescriptorTable global_descriptor_table = {
    .table = {
        {
            0
        },
        {
            .segment_low = 0xFFFF,
            .base_low = 0,
            .base_mid = 0,
            .type_bit = 0xA,
            .non_system = 1,
            .privilege = 0,
            .present = 1,
            .limit_high = 0xF,
            .long_mode = 0,
            .operation_size = 1,
            .granularity = 1,
            .base_high = 0
        },
        {
            .segment_low = 0xFFFF,
            .base_low = 0,
            .base_mid = 0,
            .type_bit = 0x2,
            .non_system = 1,
            .privilege = 0,
            .present = 1,
            .limit_high = 0xF,
            .long_mode = 0,
            .operation_size = 1,
            .granularity = 1,
            .base_high = 0
        },
        {
            // User code segment: same as kernel code, DPL 3
            .segment_low = 0xFFFF,
            .base_low = 0,
            .base_mid = 0,
            .type_bit = 0xA,
            .non_system = 1,
            .privilege = 0x3,
            .present = 1,
            .limit_high = 0xF,
            .long_mode = 0,
            .operation_size = 1,
            .granularity = 1,
            .base_high = 0
        },
        {
            // User data segment: same as kernel data, DPL 3
            .segment_low = 0xFFFF,
            .base_low = 0,
            .base_mid = 0,
            .type_bit = 0x2,
            .non_system = 1,
            .privilege = 0x3,
            .present = 1,
            .limit_high = 0xF,
            .long_mode = 0,
            .operation_size = 1,
            .granularity = 1,
            .base_high = 0
        },
        {
            // Task State Segment: base is filled by gdt_install_tss()
            .limit_high     = (sizeof(struct TSSEntry) & (0xF << 16)) >> 16,
            .segment_low    = sizeof(struct TSSEntry),
            .base_high      = 0,
            .base_mid       = 0,
            .base_low       = 0,
            .non_system     = 0,    // S bit
            .type_bit       = 0x9,  // 32-bit available TSS
            .privilege      = 0,    // DPL
            .present        = 1,    // P bit
            .operation_size = 1,    // D/B bit
            .long_mode      = 0,    // L bit
            .granularity    = 0,    // G bit
        },
        {0}
    }
};

void gdt_install_tss(void) {
    uint32_t base = (uint32_t) &_interrupt_tss_entry;
    global_descriptor_table.table[5].base_high = (base & (0xFF << 24)) >> 24;
    global_descriptor_table.table[5].base_mid  = (base & (0xFF << 16)) >> 16;
    global_descriptor_table.table[5].base_low  = base & 0xFFFF;
}

/**
 * _gdt_gdtr, predefined system GDTR.
 * GDT pointed by this variable is already set to point global_descriptor_table above.
 * From: https://wiki.osdev.org/Global_Descriptor_Table, GDTR.size is GDT size minus
 * 1.
 */
struct GDTR _gdt_gdtr = {
    .size = sizeof(global_descriptor_table) - 1,
    .address = &global_descriptor_table
};