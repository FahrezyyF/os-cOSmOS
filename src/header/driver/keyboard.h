#ifndef _KEYBOARD_H
#define _KEYBOARD_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "header/cpu/interrupt.h"

#define EXT_SCANCODE_UP        0x48
#define EXT_SCANCODE_DOWN      0x50
#define EXT_SCANCODE_LEFT      0x4B
#define EXT_SCANCODE_RIGHT     0x4D
#define EXT_SCANCODE_HOME      0x47
#define EXT_SCANCODE_END       0x4F
#define EXT_SCANCODE_PAGE_UP   0x49
#define EXT_SCANCODE_PAGE_DOWN 0x51
#define EXT_SCANCODE_INSERT    0x52
#define EXT_SCANCODE_DELETE    0x53

#define KEYBOARD_DATA_PORT     0x60
#define EXTENDED_SCANCODE_BYTE 0xE0

// Make scancodes of modifier keys (scancode set 1)
#define SCANCODE_LEFT_SHIFT    0x2A
#define SCANCODE_RIGHT_SHIFT   0x36
#define SCANCODE_CTRL          0x1D
#define SCANCODE_ALT           0x38
#define SCANCODE_CAPS_LOCK     0x3A
#define SCANCODE_BREAK_BIT     0x80

/**
 * Special (non-ASCII) keys are delivered through get_keyboard_buffer() as
 * single chars in the 0x80-0x8F range so ASCII consumers can ignore them.
 * Ctrl+<letter> is delivered as the ASCII control code (Ctrl+C = 0x03).
 */
#define KEY_ARROW_UP           ((char) 0x80)
#define KEY_ARROW_DOWN         ((char) 0x81)
#define KEY_ARROW_LEFT         ((char) 0x82)
#define KEY_ARROW_RIGHT        ((char) 0x83)
#define KEY_HOME               ((char) 0x84)
#define KEY_END                ((char) 0x85)
#define KEY_PAGE_UP            ((char) 0x86)
#define KEY_PAGE_DOWN          ((char) 0x87)
#define KEY_INSERT             ((char) 0x88)
#define KEY_DELETE             ((char) 0x89)

// Size of the key ring buffer (power of two)
#define KEYBOARD_BUFFER_SIZE   64

/**
 * keyboard_scancode_1_to_ascii_map[256], Convert scancode values that correspond to ASCII printables
 * How to use this array: ascii_char = k[scancode]
 *
 * By default, QEMU using scancode set 1 (from empirical testing)
 */
extern const char keyboard_scancode_1_to_ascii_map[256];

// Same as keyboard_scancode_1_to_ascii_map, but with Shift held (US layout)
extern const char keyboard_scancode_1_to_ascii_shift_map[256];

/**
 * KeyboardDriverState - Contain all driver states
 *
 * @param read_extended_mode Optional, can be used for signaling next read is extended scancode (ex. arrow keys)
 * @param keyboard_input_on  Indicate whether keyboard ISR is activated or not
 * @param keyboard_buffer    Storing keyboard input values in ASCII (ring buffer, oldest key first)
 * @param buffer_head        Index of the next key to be read
 * @param buffer_tail        Index where the ISR stores the next key
 * @param shift_pressed      Left or right Shift is held
 * @param ctrl_pressed       Left or right Ctrl is held
 * @param alt_pressed        Left or right Alt is held
 * @param caps_lock          Caps Lock toggle state
 */
struct KeyboardDriverState {
    bool    read_extended_mode;
    bool    keyboard_input_on;
    char    keyboard_buffer[KEYBOARD_BUFFER_SIZE];
    uint8_t buffer_head;
    uint8_t buffer_tail;
    bool    shift_pressed;
    bool    ctrl_pressed;
    bool    alt_pressed;
    bool    caps_lock;
} __attribute((packed));





/* -- Driver Interfaces -- */

// Activate keyboard ISR / start listen keyboard & save to buffer
void keyboard_state_activate(void);

// Deactivate keyboard ISR / stop listening keyboard interrupt
void keyboard_state_deactivate(void);

// Get keyboard buffer value and flush the buffer - @param buf Pointer to char buffer
// Pops the oldest pending key, *buf = 0 when no key is pending (non-blocking)
void get_keyboard_buffer(char *buf);

// Blocking read: halt the CPU until a key is available and return it
char keyboard_getchar(void);

// Whether Ctrl / Shift / Alt are currently held (for key chords such as Ctrl+C)
bool keyboard_is_ctrl_pressed(void);
bool keyboard_is_shift_pressed(void);
bool keyboard_is_alt_pressed(void);

/* -- Keyboard Interrupt Service Routine -- */

/**
 * Handling keyboard interrupt & process scancodes into ASCII character.
 * Will start listen and process keyboard scancode if keyboard_input_on.
 */
void keyboard_isr(void);

#endif
