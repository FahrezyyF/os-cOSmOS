#include "user/ulib.h"

/**
 * udpecho [port] - UDP echo server (default port 5555, which `make run` forwards from the
 * host: `nc -u localhost 5555`). Prints every datagram and sends it back in upper case.
 * Runs until Ctrl+C (or `udpecho 5555 &` in the background).
 */
int main(int argc, char *argv[]) {
    uint32_t port = 5555;
    if (argc > 1 && (!parse_uint(argv[1], &port) || port == 0 || port > 65535)) {
        printf("usage: udpecho [port]\n");
        return 2;
    }
    int32_t s = udp_bind((uint16_t) port);
    if (s < 0) {
        printf("udpecho: cannot bind port %u (no network or port in use)\n", port);
        return 1;
    }
    printf("udpecho: listening on UDP port %u (pid %u), Ctrl+C stops\n", port, getpid());
    char buf[1500];
    for (;;) {
        uint8_t ip[4];
        uint16_t from_port;
        int32_t n = udp_recv(s, ip, &from_port, buf, sizeof(buf) - 1, 60000); // the process sleeps meanwhile
        if (n < 0)
            continue;
        buf[n] = '\0';
        printf_color(C_LIGHT_CYAN, "[%u.%u.%u.%u:%u] %s%s", ip[0], ip[1], ip[2], ip[3], from_port, buf,
                     n > 0 && buf[n - 1] == '\n' ? "" : "\n");
        for (int32_t i = 0; i < n; i++)
            if (buf[i] >= 'a' && buf[i] <= 'z')
                buf[i] = (char) (buf[i] - 32);
        udp_send(s, ip, from_port, buf, (uint32_t) n);
    }
}
