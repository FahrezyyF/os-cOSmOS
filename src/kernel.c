void kernel_setup(void) {
    volatile char *vga_buffer = (volatile char*) 0xB8000;
    vga_buffer[0] = 'O';
    vga_buffer[1] = 0x07;
    vga_buffer[2] = 'S';
    vga_buffer[3] = 0x07;

    while (1) {
        asm volatile("hlt");
    }
}