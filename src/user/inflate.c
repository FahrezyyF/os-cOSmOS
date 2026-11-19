#include <stddef.h>
#include "user/inflate.h"

// Only malloc / free are needed: the same prototypes in cOSmOS (ulib) and on the host (tests)
void *malloc(size_t size);
void  free(void *ptr);

/* Huffman decoding tables, canonical codes (as in zlib's puff.c) */
struct Huffman {
    uint16_t count[16];   // codes per length
    uint16_t symbol[320]; // symbols ordered by code
};

struct State {
    const uint8_t *in;
    uint32_t in_len, in_pos;
    uint32_t bitbuf, bitcnt;
    uint8_t *out;
    uint32_t out_cap, out_pos;
    bool error;
};

static uint32_t bits(struct State *s, uint32_t need) {
    uint32_t val = s->bitbuf;
    while (s->bitcnt < need) {
        if (s->in_pos >= s->in_len) {
            s->error = true;
            return 0;
        }
        val |= (uint32_t) s->in[s->in_pos++] << s->bitcnt;
        s->bitcnt += 8;
    }
    s->bitbuf = val >> need;
    s->bitcnt -= need;
    return val & ((1u << need) - 1);
}

static int decode(struct State *s, const struct Huffman *h) {
    int code = 0, first = 0, index = 0;
    for (int len = 1; len < 16; len++) {
        code |= (int) bits(s, 1);
        if (s->error)
            return -1;
        int count = h->count[len];
        if (code - count < first)
            return h->symbol[index + (code - first)];
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
    }
    return -1;
}

static int build(struct Huffman *h, const uint16_t *length, int n) {
    uint16_t offs[16];
    for (int len = 0; len < 16; len++)
        h->count[len] = 0;
    for (int sym = 0; sym < n; sym++)
        h->count[length[sym]]++;
    if (h->count[0] == n)
        return 0;
    int left = 1;
    for (int len = 1; len < 16; len++) {
        left <<= 1;
        left -= h->count[len];
        if (left < 0)
            return left; // over-subscribed
    }
    offs[1] = 0;
    for (int len = 1; len < 15; len++)
        offs[len + 1] = (uint16_t) (offs[len] + h->count[len]);
    for (int sym = 0; sym < n; sym++)
        if (length[sym] != 0)
            h->symbol[offs[length[sym]]++] = (uint16_t) sym;
    return left;
}

static const uint16_t len_base[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
                                      35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
static const uint16_t len_extra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
static const uint16_t dist_base[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769,
                                       1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
static const uint16_t dist_extra[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

static int codes(struct State *s, const struct Huffman *lencode, const struct Huffman *distcode) {
    for (;;) {
        int symbol = decode(s, lencode);
        if (symbol < 0)
            return -10;
        if (symbol < 256) {
            if (s->out_pos >= s->out_cap)
                return -2;
            s->out[s->out_pos++] = (uint8_t) symbol;
        } else if (symbol == 256) {
            return 0;
        } else {
            symbol -= 257;
            if (symbol >= 29)
                return -10;
            uint32_t len = len_base[symbol] + bits(s, len_extra[symbol]);
            int dsym = decode(s, distcode);
            if (dsym < 0 || dsym >= 30)
                return -11;
            uint32_t dist = dist_base[dsym] + bits(s, dist_extra[dsym]);
            if (s->error)
                return -1;
            if (dist > s->out_pos)
                return -11;
            if (s->out_pos + len > s->out_cap)
                return -2;
            for (uint32_t i = 0; i < len; i++, s->out_pos++)
                s->out[s->out_pos] = s->out[s->out_pos - dist];
        }
    }
}

static int stored(struct State *s) {
    s->bitbuf = 0;
    s->bitcnt = 0;
    if (s->in_pos + 4 > s->in_len)
        return -1;
    uint32_t len = s->in[s->in_pos] | (uint32_t) s->in[s->in_pos + 1] << 8;
    uint32_t nlen = s->in[s->in_pos + 2] | (uint32_t) s->in[s->in_pos + 3] << 8;
    s->in_pos += 4;
    if (len != (~nlen & 0xFFFF))
        return -2;
    if (s->in_pos + len > s->in_len || s->out_pos + len > s->out_cap)
        return -1;
    for (uint32_t i = 0; i < len; i++)
        s->out[s->out_pos + i] = s->in[s->in_pos + i];
    s->in_pos += len;
    s->out_pos += len;
    return 0;
}

static int fixed(struct State *s) {
    static struct Huffman lencode, distcode;
    static bool built = false;
    if (!built) {
        uint16_t lengths[288];
        int sym = 0;
        for (; sym < 144; sym++) lengths[sym] = 8;
        for (; sym < 256; sym++) lengths[sym] = 9;
        for (; sym < 280; sym++) lengths[sym] = 7;
        for (; sym < 288; sym++) lengths[sym] = 8;
        build(&lencode, lengths, 288);
        for (sym = 0; sym < 30; sym++) lengths[sym] = 5;
        build(&distcode, lengths, 30);
        built = true;
    }
    return codes(s, &lencode, &distcode);
}

static int dynamic(struct State *s) {
    static const uint8_t order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
    uint16_t lengths[320];
    struct Huffman lencode, distcode;
    int nlen = (int) bits(s, 5) + 257, ndist = (int) bits(s, 5) + 1, ncode = (int) bits(s, 4) + 4;
    if (s->error || nlen > 286 || ndist > 30)
        return -3;
    int index = 0;
    for (; index < ncode; index++)
        lengths[order[index]] = (uint16_t) bits(s, 3);
    for (; index < 19; index++)
        lengths[order[index]] = 0;
    if (build(&lencode, lengths, 19) != 0)
        return -4;
    index = 0;
    while (index < nlen + ndist) {
        int symbol = decode(s, &lencode);
        if (symbol < 0)
            return symbol;
        if (symbol < 16) {
            lengths[index++] = (uint16_t) symbol;
        } else {
            uint16_t len = 0;
            int repeat;
            if (symbol == 16) {
                if (index == 0)
                    return -5;
                len = lengths[index - 1];
                repeat = 3 + (int) bits(s, 2);
            } else if (symbol == 17) {
                repeat = 3 + (int) bits(s, 3);
            } else {
                repeat = 11 + (int) bits(s, 7);
            }
            if (index + repeat > nlen + ndist)
                return -6;
            while (repeat--)
                lengths[index++] = len;
        }
    }
    if (lengths[256] == 0)
        return -9;
    int err = build(&lencode, lengths, nlen);
    if (err < 0 || (err > 0 && nlen - lencode.count[0] != 1))
        return -7;
    err = build(&distcode, lengths + nlen, ndist);
    if (err < 0 || (err > 0 && ndist - distcode.count[0] != 1))
        return -8;
    return codes(s, &lencode, &distcode);
}

int inflate_zlib(const uint8_t *src, uint32_t src_len, uint8_t *dst, uint32_t dst_cap, uint32_t *out_len) {
    *out_len = 0;
    if (src_len < 2 || (src[0] & 0x0F) != 8 || ((uint32_t) src[0] << 8 | src[1]) % 31 != 0 || (src[1] & 0x20))
        return -20; // not a zlib stream (or uses a preset dictionary)
    struct State s = {.in = src, .in_len = src_len, .in_pos = 2, .out = dst, .out_cap = dst_cap};
    int last, err;
    do {
        last = (int) bits(&s, 1);
        int type = (int) bits(&s, 2);
        if (s.error)
            return -1;
        err = type == 0 ? stored(&s) : type == 1 ? fixed(&s) : type == 2 ? dynamic(&s) : -12;
        if (err != 0)
            return err;
    } while (!last);
    // Adler-32 of the output (after the stream, byte aligned)
    s.bitbuf = s.bitcnt = 0;
    if (s.in_pos + 4 <= s.in_len) {
        uint32_t a = 1, b = 0;
        for (uint32_t i = 0; i < s.out_pos; i++) {
            a = (a + dst[i]) % 65521;
            b = (b + a) % 65521;
        }
        uint32_t want = (uint32_t) s.in[s.in_pos] << 24 | (uint32_t) s.in[s.in_pos + 1] << 16 |
                        (uint32_t) s.in[s.in_pos + 2] << 8 | s.in[s.in_pos + 3];
        if (want != (b << 16 | a))
            return -21;
    }
    *out_len = s.out_pos;
    return 0;
}

bool czip_is(const uint8_t *data, uint32_t len) {
    return len >= CZIP_HEADER_SIZE && data[0] == 'C' && data[1] == 'Z' && data[2] == 'I' && data[3] == 'P';
}

bool czip_unpack(const uint8_t *data, uint32_t len, uint8_t **out, uint32_t *out_len) {
    if (!czip_is(data, len))
        return false;
    uint32_t size = data[4] | (uint32_t) data[5] << 8 | (uint32_t) data[6] << 16 | (uint32_t) data[7] << 24;
    uint8_t *buf = malloc(size + 1);
    if (buf == NULL)
        return false;
    uint32_t got;
    if (inflate_zlib(data + CZIP_HEADER_SIZE, len - CZIP_HEADER_SIZE, buf, size, &got) != 0 || got != size) {
        free(buf);
        return false;
    }
    buf[size] = 0;
    *out = buf;
    *out_len = size;
    return true;
}
