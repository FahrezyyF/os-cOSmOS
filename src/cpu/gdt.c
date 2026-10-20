#include "header/cpu/gdt.h"

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
        }
    }
};

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