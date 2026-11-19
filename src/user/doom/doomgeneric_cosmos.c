// doomgeneric platform layer for cOSmOS (bonus: DOOM).
//
// DOOM runs as a normal user process: it draws its 320x200 frame scaled 2x into a window
// of the cOSmOS window compositor, reads raw key presses / releases from that window
// (SYSCALL_WEVENT_KEY_DOWN / KEY_UP, scancode set 1), times itself with SYS_UPTIME /
// SYS_SLEEP and loads the WAD from the root file system through the small C library in
// src/user/libc (the shareware WAD is stored CZIP-compressed at /games/doom1.wad).

#include "doomkeys.h"
#include "m_argv.h"
#include "doomgeneric.h"
#include "i_system.h"

#include <stdio.h>
#include <string.h>
#include "user/gfx.h"

#define SCALE 2

static struct Canvas canvas;

#define KEYQUEUE_SIZE 64
static unsigned short key_queue[KEYQUEUE_SIZE];
static unsigned int key_write = 0, key_read = 0;

// Scancode set 1 -> DOOM key (0 = ignore)
static unsigned char doom_key(unsigned char sc, bool extended) {
    if (extended) {
        switch (sc) {
            case 0x48: return KEY_UPARROW;
            case 0x50: return KEY_DOWNARROW;
            case 0x4B: return KEY_LEFTARROW;
            case 0x4D: return KEY_RIGHTARROW;
            case 0x1C: return KEY_ENTER;
            case 0x1D: return KEY_FIRE;      // right Ctrl
            case 0x38: return KEY_LALT;      // right Alt: strafe
            case 0x47: return KEY_HOME;
            case 0x4F: return KEY_END;
            case 0x49: return KEY_PGUP;
            case 0x51: return KEY_PGDN;
            case 0x52: return KEY_INS;
            case 0x53: return KEY_DEL;
            default:   return 0;
        }
    }
    static const char row1[] = "1234567890-=";   // 0x02 - 0x0D
    static const char row2[] = "qwertyuiop[]";   // 0x10 - 0x1B
    static const char row3[] = "asdfghjkl;'`";   // 0x1E - 0x29
    static const char row4[] = "\\zxcvbnm,./";   // 0x2B - 0x35
    if (sc >= 0x02 && sc <= 0x0D) return (unsigned char) row1[sc - 0x02];
    if (sc >= 0x10 && sc <= 0x1B) return (unsigned char) row2[sc - 0x10];
    if (sc >= 0x1E && sc <= 0x29) return (unsigned char) row3[sc - 0x1E];
    if (sc >= 0x2B && sc <= 0x35) return (unsigned char) row4[sc - 0x2B];
    if (sc >= 0x3B && sc <= 0x44) return (unsigned char) (KEY_F1 + (sc - 0x3B));
    switch (sc) {
        case 0x01: return KEY_ESCAPE;
        case 0x0E: return KEY_BACKSPACE;
        case 0x0F: return KEY_TAB;
        case 0x1C: return KEY_ENTER;
        case 0x1D: return KEY_FIRE;     // left Ctrl
        case 0x2A:
        case 0x36: return KEY_RSHIFT;   // run
        case 0x38: return KEY_LALT;     // strafe
        case 0x39: return KEY_USE;      // space
        case 0x57: return KEY_F11;
        case 0x58: return KEY_F12;
        default:   return 0;
    }
}

static void queue_key(int pressed, unsigned char key) {
    key_queue[key_write] = (unsigned short) ((pressed << 8) | key);
    key_write = (key_write + 1) % KEYQUEUE_SIZE;
}

static void pump_events(void) {
    struct SyscallWindowEvent e;
    for (canvas_event(&canvas, &e, false); e.type != SYSCALL_WEVENT_NONE; canvas_event(&canvas, &e, false)) {
        if (e.type == SYSCALL_WEVENT_CLOSE) {
            I_Quit(); // saves the configuration, then exits
        } else if (e.type == SYSCALL_WEVENT_KEY_DOWN || e.type == SYSCALL_WEVENT_KEY_UP) {
            unsigned char key = doom_key(e.scancode, e.extended);
            if (key != 0)
                queue_key(e.type == SYSCALL_WEVENT_KEY_DOWN, key);
        }
    }
}

void DG_Init(void) {
    if (!gfx_available()) {
        printf("doom: needs the graphics mode (the window compositor is off in VGA text mode)\n");
        exit(1);
    }
    if (!canvas_open_flags(&canvas, DOOMGENERIC_RESX * SCALE, DOOMGENERIC_RESY * SCALE, "DOOM", -1, -1,
                           SYSCALL_WINDOW_FOCUS)) {
        printf("doom: cannot open a window\n");
        exit(1);
    }
}

void DG_DrawFrame(void) {
    const uint32_t *src = DG_ScreenBuffer;
    for (int y = 0; y < DOOMGENERIC_RESY; y++) {
        uint32_t *row0 = &canvas.px[(y * SCALE) * canvas.w];
        for (int x = 0; x < DOOMGENERIC_RESX; x++) {
            uint32_t p = src[y * DOOMGENERIC_RESX + x] & 0x00FFFFFF;
            row0[x * 2] = p;
            row0[x * 2 + 1] = p;
        }
        memcpy(row0 + canvas.w, row0, (size_t) canvas.w * 4); // second line of the 2x scale
    }
    canvas_present(&canvas);
    pump_events();

    // Frame rate in the title bar (the bonus asks for > 15 fps)
    static uint32_t frames = 0, second = 0;
    frames++;
    uint32_t now = uptime_ms();
    if (now - second >= 1000) {
        char title[SYSCALL_WINDOW_TITLE_MAX];
        snprintf(title, sizeof(title), "DOOM  %u fps", frames * 1000 / (now - second));
        canvas_title(&canvas, title);
        frames = 0;
        second = now;
    }
}

void DG_SleepMs(uint32_t ms) {
    sleep_ms(ms);
}

uint32_t DG_GetTicksMs(void) {
    return uptime_ms();
}

int DG_GetKey(int *pressed, unsigned char *key) {
    if (key_read == key_write) {
        pump_events();
        if (key_read == key_write)
            return 0;
    }
    unsigned short k = key_queue[key_read];
    key_read = (key_read + 1) % KEYQUEUE_SIZE;
    *pressed = k >> 8;
    *key = (unsigned char) (k & 0xFF);
    return 1;
}

void DG_SetWindowTitle(const char *title) {
    (void) title; // the title bar shows the frame rate instead
}

int main(int argc, char **argv) {
    // Default to the shareware WAD on the root file system
    char *args[16];
    int n = 0;
    bool has_iwad = false;
    for (int i = 0; i < argc && n < 13; i++) {
        args[n++] = argv[i];
        if (strcmp(argv[i], "-iwad") == 0)
            has_iwad = true;
    }
    if (!has_iwad) {
        args[n++] = "-iwad";
        args[n++] = "/games/doom1.wad";
    }
    args[n] = NULL;
    printf("doom: starting (pid %u). Click the window if it lost the keyboard; Esc opens the menu.\n", getpid());
    doomgeneric_Create(n, args);
    for (;;)
        doomgeneric_Tick();
}
