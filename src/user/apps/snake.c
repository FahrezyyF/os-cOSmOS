#include "user/ulib.h"

/**
 * snake - a game in the text console (book 4.3.4 example of an external application,
 * bonus "aplikasi lain"): a separate process that blocks the shell while it runs, reads
 * the keyboard through system calls and draws with the console syscalls.
 *
 *   arrows / WASD steer, P pauses, Q (or Ctrl+C) quits. Eat '@' to grow; speed rises.
 */

#define MAX_LEN 1024

static uint8_t rows, cols;      // console size
static uint8_t top = 2;         // first row of the field (row 0: title, row 1: border)
static int     fw, fh;          // field size inside the border
static int     sx[MAX_LEN], sy[MAX_LEN];
static int     len, dir_x = 1, dir_y = 0;
static int     food_x, food_y, score = 0;
static uint32_t seed;

static uint32_t rnd(void) {
    seed = seed * 1103515245u + 12345u;
    return seed >> 8;
}

static void put(int x, int y, const char *s, uint8_t color) {
    console_set_cursor((uint8_t) (top + y), (uint8_t) (1 + x));
    console_raw_write(s, (uint32_t) strlen(s), color);
}

static void status_line(const char *extra) {
    char line[160];
    snprintf(line, sizeof(line), " SNAKE  score %-4d length %-4d  arrows/WASD move, P pause, Q quit  %s", score, len,
             extra);
    console_set_cursor(0, 0);
    uint32_t n = (uint32_t) strlen(line);
    console_raw_write(line, n < cols ? n : cols, C_BLACK | C_LIGHT_GREEN << 4);
    for (uint32_t i = n; i < cols; i++)
        console_raw_write(" ", 1, C_BLACK | C_LIGHT_GREEN << 4);
}

static bool on_snake(int x, int y, int from) {
    for (int i = from; i < len; i++)
        if (sx[i] == x && sy[i] == y)
            return true;
    return false;
}

static void place_food(void) {
    do {
        food_x = (int) (rnd() % (uint32_t) fw);
        food_y = (int) (rnd() % (uint32_t) fh);
    } while (on_snake(food_x, food_y, 0));
    put(food_x, food_y, "@", C_LIGHT_RED);
}

static void draw_border(void) {
    char line[256];
    for (int x = 0; x < fw + 2 && x < (int) sizeof(line) - 1; x++)
        line[x] = '#';
    line[fw + 2] = '\0';
    console_set_cursor(1, 0);
    console_raw_write(line, (uint32_t) (fw + 2), C_DARK_GRAY);
    console_set_cursor((uint8_t) (top + fh), 0);
    console_raw_write(line, (uint32_t) (fw + 2), C_DARK_GRAY);
    for (int y = 0; y < fh; y++) {
        console_set_cursor((uint8_t) (top + y), 0);
        console_raw_write("#", 1, C_DARK_GRAY);
        console_set_cursor((uint8_t) (top + y), (uint8_t) (fw + 1));
        console_raw_write("#", 1, C_DARK_GRAY);
    }
}

int main(int argc, char *argv[]) {
    (void) argc;
    (void) argv;
    struct SyscallScreenInfo screen;
    console_screen_info(&screen);
    rows = screen.rows;
    cols = screen.cols;
    fw = cols - 2 > 76 ? 76 : cols - 2;
    fh = rows - 4 > 30 ? 30 : rows - 4;
    seed = uptime_ms() ^ (getpid() << 16);
    console_clear_screen();
    draw_border();

    len = 4;
    for (int i = 0; i < len; i++) {
        sx[i] = fw / 2 - i;
        sy[i] = fh / 2;
        put(sx[i], sy[i], i == 0 ? "O" : "o", C_LIGHT_GREEN);
    }
    place_food();
    status_line("");

    bool paused = false, over = false;
    uint32_t delay = 120;
    while (!over) {
        // Input: every pending key (the snake only turns, never reverses)
        char c;
        while ((c = getchar_nowait()) != 0) {
            if (c == 'q' || c == 'Q') {
                over = true;
            } else if (c == 'p' || c == 'P') {
                paused = !paused;
                status_line(paused ? "[PAUSED]" : "");
            } else if ((c == KEY_UP || c == 'w') && dir_y == 0) {
                dir_x = 0; dir_y = -1;
            } else if ((c == KEY_DOWN || c == 's') && dir_y == 0) {
                dir_x = 0; dir_y = 1;
            } else if ((c == KEY_LEFT || c == 'a') && dir_x == 0) {
                dir_x = -1; dir_y = 0;
            } else if ((c == KEY_RIGHT || c == 'd') && dir_x == 0) {
                dir_x = 1; dir_y = 0;
            }
        }
        if (over)
            break;
        sleep_ms(paused ? 100 : delay); // the process blocks between frames: others keep running
        if (paused)
            continue;

        int nx = sx[0] + dir_x, ny = sy[0] + dir_y;
        if (nx < 0 || ny < 0 || nx >= fw || ny >= fh || on_snake(nx, ny, 0)) {
            over = true;
            break;
        }
        bool grow = nx == food_x && ny == food_y;
        if (!grow) // erase the tail
            put(sx[len - 1], sy[len - 1], " ", C_LIGHT_GRAY);
        else if (len < MAX_LEN)
            len++;
        for (int i = len - 1; i > 0; i--) {
            sx[i] = sx[i - 1];
            sy[i] = sy[i - 1];
        }
        sx[0] = nx;
        sy[0] = ny;
        put(sx[1], sy[1], "o", C_LIGHT_GREEN);
        put(nx, ny, "O", C_LIGHT_GREEN);
        if (grow) {
            score += 10;
            if (delay > 50)
                delay -= 5;
            beep(880, 30);
            place_food();
            status_line("");
        }
    }
    status_line("[GAME OVER]");
    console_set_cursor((uint8_t) (top + fh + 1), 0);
    printf_color(C_YELLOW, "Game over! Score %d, length %d.\n", score, len);
    return 0;
}
