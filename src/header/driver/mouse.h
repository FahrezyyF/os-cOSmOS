#ifndef _MOUSE_H
#define _MOUSE_H

#include <stdint.h>
#include <stdbool.h>

// 8042 PS/2 controller ports & commands
#define PS2_DATA_PORT          0x60
#define PS2_STATUS_PORT        0x64
#define PS2_COMMAND_PORT       0x64
#define PS2_STATUS_OUTPUT_FULL 0x01
#define PS2_STATUS_INPUT_FULL  0x02
#define PS2_STATUS_AUX_DATA    0x20
#define PS2_CMD_ENABLE_AUX     0xA8
#define PS2_CMD_READ_CONFIG    0x20
#define PS2_CMD_WRITE_CONFIG   0x60
#define PS2_CMD_WRITE_AUX      0xD4

// PS/2 mouse commands
#define MOUSE_CMD_SET_DEFAULTS 0xF6
#define MOUSE_CMD_ENABLE       0xF4
#define MOUSE_CMD_SAMPLE_RATE  0xF3
#define MOUSE_CMD_GET_ID       0xF2
#define MOUSE_ACK              0xFA

#define MOUSE_BUTTON_LEFT      0x1
#define MOUSE_BUTTON_RIGHT     0x2
#define MOUSE_BUTTON_MIDDLE    0x4

/**
 * MouseState, snapshot of the pointer
 *
 * @param x, y     Absolute position in pointer space (pixels), clamped to the screen
 * @param buttons  MOUSE_BUTTON_* bitmask
 * @param wheel    Accumulated wheel movement since the last mouse_get_state() (negative = up)
 * @param packets  Number of packets decoded since boot
 * @param changed  Whether anything changed since the last mouse_get_state()
 */
struct MouseState {
    int32_t  x;
    int32_t  y;
    uint8_t  buttons;
    int32_t  wheel;
    uint32_t packets;
    bool     changed;
};

/**
 * Enable the auxiliary PS/2 port, configure the mouse (with IntelliMouse wheel
 * detection), and unmask IRQ12.
 *
 * @param width  Pointer space width (x is clamped to [0, width))
 * @param height Pointer space height (y is clamped to [0, height))
 * @return true if a mouse acknowledged the configuration
 */
bool mouse_init(uint16_t width, uint16_t height);

// IRQ12 handler, called by main_interrupt_handler()
void mouse_isr(void);

bool mouse_is_present(void);
bool mouse_has_wheel(void);

// Copy the current state and reset `changed` / `wheel`
void mouse_get_state(struct MouseState *out);

#endif
