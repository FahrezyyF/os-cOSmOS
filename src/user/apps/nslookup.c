#include "user/ulib.h"

/**
 * nslookup <name> [server] - DNS A-record lookup over UDP port 53 (RFC 1035), default
 * server 10.0.2.3 (QEMU's DNS forwarder): a complete UDP request / response exchange.
 */

static uint32_t build_query(uint8_t *q, const char *name, uint16_t id) {
    memset(q, 0, 12);
    q[0] = (uint8_t) (id >> 8);
    q[1] = (uint8_t) id;
    q[2] = 0x01; // recursion desired
    q[5] = 1;    // one question
    uint32_t pos = 12;
    const char *label = name;
    while (*label) {
        const char *dot = strchr(label, '.');
        uint32_t len = dot ? (uint32_t) (dot - label) : (uint32_t) strlen(label);
        if (len == 0 || len > 63 || pos + len + 6 > 300)
            return 0;
        q[pos++] = (uint8_t) len;
        memcpy(q + pos, label, len);
        pos += len;
        label += len;
        if (*label == '.')
            label++;
    }
    q[pos++] = 0;
    q[pos++] = 0; q[pos++] = 1; // type A
    q[pos++] = 0; q[pos++] = 1; // class IN
    return pos;
}

// Skip a (possibly compressed) name, return the new offset
static uint32_t skip_name(const uint8_t *m, uint32_t len, uint32_t pos) {
    while (pos < len) {
        if (m[pos] == 0)
            return pos + 1;
        if ((m[pos] & 0xC0) == 0xC0)
            return pos + 2;
        pos += (uint32_t) m[pos] + 1;
    }
    return len;
}

int main(int argc, char *argv[]) {
    uint8_t server[4] = {10, 0, 2, 3};
    if (argc < 2 || (argc > 2 && !parse_ip(argv[2], server))) {
        printf("usage: nslookup <name> [dns server ip]   e.g. nslookup example.com\n");
        return 2;
    }
    int32_t s = udp_bind(0);
    if (s < 0) {
        printf("nslookup: no network / no free socket\n");
        return 1;
    }
    uint8_t query[320], reply[1472];
    uint16_t id = (uint16_t) (uptime_ms() ^ getpid());
    uint32_t qlen = build_query(query, argv[1], id);
    if (qlen == 0) {
        printf("nslookup: bad name\n");
        return 2;
    }
    printf("Server: %u.%u.%u.%u#53\n", server[0], server[1], server[2], server[3]);
    for (int attempt = 0; attempt < 3; attempt++) {
        if (udp_send(s, server, 53, query, qlen) < 0) {
            printf("nslookup: send failed\n");
            break;
        }
        uint8_t from[4];
        uint16_t port;
        int32_t n = udp_recv(s, from, &port, reply, sizeof(reply), 2000);
        if (n == -2)
            continue;
        if (n < 12 || reply[0] != query[0] || reply[1] != query[1])
            continue;
        uint32_t rcode = reply[3] & 0xF, answers = (uint32_t) (reply[6] << 8 | reply[7]);
        if (rcode != 0) {
            printf("%s: %s\n", argv[1], rcode == 3 ? "no such domain (NXDOMAIN)" : "server error");
            udp_close(s);
            return 1;
        }
        uint32_t pos = skip_name(reply, (uint32_t) n, 12) + 4, found = 0;
        for (uint32_t a = 0; a < answers && pos + 10 <= (uint32_t) n; a++) {
            pos = skip_name(reply, (uint32_t) n, pos);
            uint16_t type = (uint16_t) (reply[pos] << 8 | reply[pos + 1]);
            uint16_t rdlen = (uint16_t) (reply[pos + 8] << 8 | reply[pos + 9]);
            pos += 10;
            if (type == 1 && rdlen == 4 && pos + 4 <= (uint32_t) n) {
                printf_color(C_LIGHT_GREEN, "Name: %s  Address: %u.%u.%u.%u\n", argv[1], reply[pos], reply[pos + 1],
                             reply[pos + 2], reply[pos + 3]);
                found++;
            }
            pos += rdlen;
        }
        if (found == 0)
            printf("%s: no A record\n", argv[1]);
        udp_close(s);
        return found ? 0 : 1;
    }
    printf("nslookup: no answer from the server (timeout)\n");
    udp_close(s);
    return 1;
}
