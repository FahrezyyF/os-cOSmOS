#include "user/ulib.h"

// ping <ip> [count] - ICMP echo (the gateway 10.0.2.2 answers in QEMU's user network)
int main(int argc, char *argv[]) {
    uint8_t ip[4];
    uint32_t count = 4;
    if (argc < 2 || !parse_ip(argv[1], ip) || (argc > 2 && !parse_uint(argv[2], &count))) {
        printf("usage: ping <a.b.c.d> [count]   e.g. ping 10.0.2.2\n");
        return 2;
    }
    printf("PING %s: 32 data bytes\n", argv[1]);
    uint32_t ok = 0;
    for (uint32_t i = 1; i <= count; i++) {
        int32_t rtt = net_ping(ip, (uint16_t) i, 2000);
        if (rtt >= 0) {
            printf("reply from %s: icmp_seq=%u time=%d ms\n", argv[1], i, rtt);
            ok++;
        } else {
            printf_color(C_LIGHT_RED, "icmp_seq=%u: %s\n", i, rtt == -2 ? "timeout" : "cannot send (no route / ARP)");
        }
        if (i < count)
            sleep_ms(1000);
    }
    printf("%u packets sent, %u received\n", count, ok);
    return ok > 0 ? 0 : 1;
}
