#include "header/driver/mouse.h"
#include "header/cpu/interrupt.h"
#include "header/cpu/portio.h"

#define PS2_TIMEOUT 100000

static volatile struct MouseState state;
static volatile uint8_t packet[4];
static volatile uint8_t packet_index = 0;
static uint8_t  packet_size = 3;
static uint16_t bound_w     = 1;
static uint16_t bound_h     = 1;
static bool     present     = false;

static bool ps2_wait_write(void) {
    for (uint32_t i = 0; i < PS2_TIMEOUT; i++)
        if (!(in(PS2_STATUS_PORT) & PS2_STATUS_INPUT_FULL))
            return true;
    return false;
}

static bool ps2_wait_read(void) {
    for (uint32_t i = 0; i < PS2_TIMEOUT; i++)
        if (in(PS2_STATUS_PORT) & PS2_STATUS_OUTPUT_FULL)
            return true;
    return false;
}

static bool mouse_send(uint8_t value) {
    if (!ps2_wait_write()) return false;
    out(PS2_COMMAND_PORT, PS2_CMD_WRITE_AUX);
    if (!ps2_wait_write()) return false;
    out(PS2_DATA_PORT, value);
    return ps2_wait_read() && in(PS2_DATA_PORT) == MOUSE_ACK;
}

static bool mouse_set_sample_rate(uint8_t rate) {
    return mouse_send(MOUSE_CMD_SAMPLE_RATE) && mouse_send(rate);
}

bool mouse_init(uint16_t width, uint16_t height) {
    bound_w = width ? width : 1;
    bound_h = height ? height : 1;
    state.x = bound_w / 2;
    state.y = bound_h / 2;

    __asm__ volatile("cli"); // poll the controller without the keyboard ISR stealing bytes

    // Drain stale bytes
    for (int i = 0; i < 16 && (in(PS2_STATUS_PORT) & PS2_STATUS_OUTPUT_FULL); i++)
        in(PS2_DATA_PORT);

    bool ok = ps2_wait_write();
    if (ok) {
        out(PS2_COMMAND_PORT, PS2_CMD_ENABLE_AUX);
        ok = ps2_wait_write();
    }
    if (ok) {
        out(PS2_COMMAND_PORT, PS2_CMD_READ_CONFIG);
        ok = ps2_wait_read();
    }
    if (ok) {
        uint8_t config = in(PS2_DATA_PORT);
        config |= 0x02;              // enable IRQ12
        config &= (uint8_t) ~0x20;   // enable the mouse clock
        ok = ps2_wait_write();
        if (ok) {
            out(PS2_COMMAND_PORT, PS2_CMD_WRITE_CONFIG);
            ok = ps2_wait_write();
            if (ok)
                out(PS2_DATA_PORT, config);
        }
    }
    ok = ok && mouse_send(MOUSE_CMD_SET_DEFAULTS);

    // IntelliMouse handshake: sample rates 200, 100, 80 then ID 3 = scroll wheel
    if (ok && mouse_set_sample_rate(200) && mouse_set_sample_rate(100) && mouse_set_sample_rate(80) &&
        mouse_send(MOUSE_CMD_GET_ID) && ps2_wait_read()) {
        packet_size = (in(PS2_DATA_PORT) == 3) ? 4 : 3;
    }
    ok = ok && mouse_send(MOUSE_CMD_ENABLE);

    present      = ok;
    packet_index = 0;
    __asm__ volatile("sti");
    if (present)
        pic_unmask_irq(IRQ_MOUSE);
    return present;
}

static void mouse_decode_packet(void) {
    uint8_t flags = packet[0];
    if (flags & 0xC0) // X/Y overflow, the movement value is garbage
        return;
    int32_t dx = (int32_t) packet[1] - ((flags & 0x10) ? 256 : 0);
    int32_t dy = (int32_t) packet[2] - ((flags & 0x20) ? 256 : 0);

    int32_t x = state.x + dx;
    int32_t y = state.y - dy; // PS/2 reports +y as "up"
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x >= bound_w) x = bound_w - 1;
    if (y >= bound_h) y = bound_h - 1;
    state.x       = x;
    state.y       = y;
    state.buttons = flags & 0x07;
    if (packet_size == 4) {
        int8_t z = (int8_t) (packet[3] << 4) >> 4; // 4-bit two's complement wheel delta
        state.wheel += z;
    }
    state.packets++;
    state.changed = true;
}

void mouse_isr(void) {
    uint8_t status = in(PS2_STATUS_PORT);
    if ((status & PS2_STATUS_OUTPUT_FULL) && (status & PS2_STATUS_AUX_DATA)) {
        uint8_t data = in(PS2_DATA_PORT);
        // Byte 0 always has bit 3 set; use it to resynchronise after a lost byte
        if (packet_index == 0 && !(data & 0x08)) {
            pic_ack(IRQ_MOUSE);
            return;
        }
        packet[packet_index++] = data;
        if (packet_index >= packet_size) {
            packet_index = 0;
            mouse_decode_packet();
        }
    }
    pic_ack(IRQ_MOUSE);
}

bool mouse_is_present(void) {
    return present;
}

bool mouse_has_wheel(void) {
    return packet_size == 4;
}

void mouse_get_state(struct MouseState *out) {
    uint32_t eflags;
    __asm__ volatile("pushf; pop %0; cli" : "=r"(eflags));
    out->x        = state.x;
    out->y        = state.y;
    out->buttons  = state.buttons;
    out->wheel    = state.wheel;
    out->packets  = state.packets;
    out->changed  = state.changed;
    state.changed = false;
    state.wheel   = 0;
    if (eflags & 0x200)
        __asm__ volatile("sti");
}
