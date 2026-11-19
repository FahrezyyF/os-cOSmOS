#include "user/ulib.h"

// ifconfig - show the network interface (RTL8139 driver, static QEMU user-network address)
int main(void) {
    struct SyscallNetInfo n;
    net_info(&n);
    if (!n.present) {
        printf("ifconfig: no network card (start QEMU with -device rtl8139, `make run` does)\n");
        return 1;
    }
    printf_color(C_WHITE, "eth0: %s, link up\n", n.driver);
    printf("  ether   %02x:%02x:%02x:%02x:%02x:%02x\n", n.mac[0], n.mac[1], n.mac[2], n.mac[3], n.mac[4], n.mac[5]);
    printf("  inet    %u.%u.%u.%u  netmask %u.%u.%u.%u\n", n.ip[0], n.ip[1], n.ip[2], n.ip[3], n.netmask[0],
           n.netmask[1], n.netmask[2], n.netmask[3]);
    printf("  gateway %u.%u.%u.%u  dns %u.%u.%u.%u\n", n.gateway[0], n.gateway[1], n.gateway[2], n.gateway[3],
           n.dns[0], n.dns[1], n.dns[2], n.dns[3]);
    printf("  RX %u packets (%u dropped), TX %u packets; UDP %u in / %u out\n", n.rx_packets, n.rx_dropped,
           n.tx_packets, n.udp_rx, n.udp_tx);
    return 0;
}
