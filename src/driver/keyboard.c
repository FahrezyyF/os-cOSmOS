#include "header/driver/keyboard.h"
#include "header/cpu/portio.h"
#include "header/stdlib/string.h"
#include "header/cpu/interrupt.h"
#include "stdbool.h"

const char keyboard_scancode_1_to_ascii_map[256] = {
      0, 0x1B, '1', '2', '3', '4', '5', '6',  '7', '8', '9',  '0',  '-', '=', '\b', '\t',
    'q',  'w', 'e', 'r', 't', 'y', 'u', 'i',  'o', 'p', '[',  ']', '\n',   0,  'a',  's',
    'd',  'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`',   0, '\\',  'z', 'x',  'c',  'v',
    'b',  'n', 'm', ',', '.', '/',   0, '*',    0, ' ',   0,    0,    0,   0,    0,    0,
      0,    0,   0,   0,   0,   0,   0,   0,    0,   0, '-',    0,    0,   0,  '+',    0,
      0,    0,   0,   0,   0,   0,   0,   0,    0,   0,   0,    0,    0,   0,    0,    0,
      0,    0,   0,   0,   0,   0,   0,   0,    0,   0,   0,    0,    0,   0,    0,    0,
      0,    0,   0,   0,   0,   0,   0,   0,    0,   0,   0,    0,    0,   0,    0,    0,

      0,    0,   0,   0,   0,   0,   0,   0,    0,   0,   0,    0,    0,   0,    0,    0,
      0,    0,   0,   0,   0,   0,   0,   0,    0,   0,   0,    0,    0,   0,    0,    0,
      0,    0,   0,   0,   0,   0,   0,   0,    0,   0,   0,    0,    0,   0,    0,    0,
      0,    0,   0,   0,   0,   0,   0,   0,    0,   0,   0,    0,    0,   0,    0,    0,
      0,    0,   0,   0,   0,   0,   0,   0,    0,   0,   0,    0,    0,   0,    0,    0,
      0,    0,   0,   0,   0,   0,   0,   0,    0,   0,   0,    0,    0,   0,    0,    0,
      0,    0,   0,   0,   0,   0,   0,   0,    0,   0,   0,    0,    0,   0,    0,    0,
      0,    0,   0,   0,   0,   0,   0,   0,    0,   0,   0,    0,    0,   0,    0,    0,
};

const char keyboard_scancode_1_to_ascii_shift_map[256] = {
      0, 0x1B, '!', '@', '#', '$', '%', '^',  '&', '*', '(',  ')',  '_', '+', '\b', '\t',
    'Q',  'W', 'E', 'R', 'T', 'Y', 'U', 'I',  'O', 'P', '{',  '}', '\n',   0,  'A',  'S',
    'D',  'F', 'G', 'H', 'J', 'K', 'L', ':',  '"', '~',   0,  '|',  'Z', 'X',  'C',  'V',
    'B',  'N', 'M', '<', '>', '?',   0, '*',    0, ' ',   0,    0,    0,   0,    0,    0,
      0,    0,   0,   0,   0,   0,   0,   0,    0,   0, '-',    0,    0,   0,  '+',    0,
};

static volatile struct KeyboardDriverState keyboard_state = {
    .read_extended_mode = false,
    .keyboard_input_on  = false,
    .buffer_head        = 0,
    .buffer_tail        = 0,
    .shift_pressed      = false,
    .ctrl_pressed       = false,
    .alt_pressed        = false,
    .caps_lock          = false,
};

static void keyboard_buffer_push(char c) {
    uint8_t next = (keyboard_state.buffer_tail + 1) % KEYBOARD_BUFFER_SIZE;
    if (next == keyboard_state.buffer_head)
        return; // Buffer full, drop the newest key
    keyboard_state.keyboard_buffer[keyboard_state.buffer_tail] = c;
    keyboard_state.buffer_tail = next;
}

static char translate_extended(uint8_t code) {
    switch (code) {
        case EXT_SCANCODE_UP:        return KEY_ARROW_UP;
        case EXT_SCANCODE_DOWN:      return KEY_ARROW_DOWN;
        case EXT_SCANCODE_LEFT:      return KEY_ARROW_LEFT;
        case EXT_SCANCODE_RIGHT:     return KEY_ARROW_RIGHT;
        case EXT_SCANCODE_HOME:      return KEY_HOME;
        case EXT_SCANCODE_END:       return KEY_END;
        case EXT_SCANCODE_PAGE_UP:   return KEY_PAGE_UP;
        case EXT_SCANCODE_PAGE_DOWN: return KEY_PAGE_DOWN;
        case EXT_SCANCODE_INSERT:    return KEY_INSERT;
        case EXT_SCANCODE_DELETE:    return KEY_DELETE;
        case 0x1C:                   return '\n'; // Keypad Enter
        case 0x35:                   return '/';  // Keypad '/'
        default:                     return 0;
    }
}

static char translate_regular(uint8_t code) {
    bool shift = keyboard_state.shift_pressed;
    char c = shift ? keyboard_scancode_1_to_ascii_shift_map[code] : keyboard_scancode_1_to_ascii_map[code];
    bool is_letter = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
    if (is_letter && keyboard_state.caps_lock)
        c ^= 0x20; // Caps Lock inverts the case chosen by Shift
    if (keyboard_state.ctrl_pressed && is_letter)
        c &= 0x1F; // Ctrl+A = 0x01 ... Ctrl+Z = 0x1A
    return c;
}

void keyboard_isr(void) {
    // Always read exactly one scancode per IRQ1 so the controller buffer never fills up
    uint8_t scancode = in(KEYBOARD_DATA_PORT);

    if (scancode == EXTENDED_SCANCODE_BYTE) {
        keyboard_state.read_extended_mode = true;
        pic_ack(IRQ_KEYBOARD);
        return;
    }

    bool    extended = keyboard_state.read_extended_mode;
    bool    released = (scancode & SCANCODE_BREAK_BIT) != 0;
    uint8_t code     = scancode & (uint8_t) ~SCANCODE_BREAK_BIT;
    keyboard_state.read_extended_mode = false;

    // Modifier state is tracked even when input is off, so it never gets out of sync
    switch (code) {
        case SCANCODE_LEFT_SHIFT:
        case SCANCODE_RIGHT_SHIFT:
            if (!extended) // E0 2A / E0 36 are "fake shifts" sent around PrintScreen etc.
                keyboard_state.shift_pressed = !released;
            break;
        case SCANCODE_CTRL:
            keyboard_state.ctrl_pressed = !released;
            break;
        case SCANCODE_ALT:
            keyboard_state.alt_pressed = !released;
            break;
        case SCANCODE_CAPS_LOCK:
            if (!released && !extended)
                keyboard_state.caps_lock = !keyboard_state.caps_lock;
            break;
        default:
            if (!released && keyboard_state.keyboard_input_on) {
                char c = extended ? translate_extended(code) : translate_regular(code);
                if (c != 0)
                    keyboard_buffer_push(c);
            }
            break;
    }

    pic_ack(IRQ_KEYBOARD);
}

void keyboard_state_activate(void) {
    keyboard_state.keyboard_input_on = true;
}

void keyboard_state_deactivate(void) {
    keyboard_state.keyboard_input_on = false;
}

void get_keyboard_buffer(char *buf) {
    if (keyboard_state.buffer_head == keyboard_state.buffer_tail) {
        *buf = 0;
        return;
    }
    *buf = keyboard_state.keyboard_buffer[keyboard_state.buffer_head];
    keyboard_state.buffer_head = (keyboard_state.buffer_head + 1) % KEYBOARD_BUFFER_SIZE;
}

char keyboard_getchar(void) {
    char c = 0;
    for (;;) {
        get_keyboard_buffer(&c);
        if (c != 0)
            return c;
        __asm__ volatile("hlt");
    }
}

bool keyboard_is_ctrl_pressed(void) {
    return keyboard_state.ctrl_pressed;
}

bool keyboard_is_shift_pressed(void) {
    return keyboard_state.shift_pressed;
}

bool keyboard_is_alt_pressed(void) {
    return keyboard_state.alt_pressed;
}
