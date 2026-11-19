#include "user/ulib.h"

// udpsend <ip> <port> <text...> - send one UDP datagram (to the host: 10.0.2.2, e.g. `nc -ul 6000`)
int main(int argc, char *argv[]) {
    uint8_t ip[4];
    uint32_t port;
    if (argc < 4 || !parse_ip(argv[1], ip) || !parse_uint(argv[2], &port) || port == 0 || port > 65535) {
        printf("usage: udpsend <a.b.c.d> <port> <text>...\n");
        return 2;
    }
    char msg[512] = "";
    for (int i = 3; i < argc; i++) {
        if (i > 3)
            strcat_s(msg, sizeof(msg), " ");
        strcat_s(msg, sizeof(msg), argv[i]);
    }
    strcat_s(msg, sizeof(msg), "\n");
    int32_t s = udp_bind(0);
    int32_t n = s >= 0 ? udp_send(s, ip, (uint16_t) port, msg, (uint32_t) strlen(msg)) : -1;
    if (n < 0)
        printf("udpsend: failed (no network or no route)\n");
    else
        printf("udpsend: %d bytes to %s:%u\n", n, argv[1], port);
    if (s >= 0)
        udp_close(s);
    return n < 0;
}
