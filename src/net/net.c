#include "header/net/net.h"
#include "header/kernel/syscall.h"
#include "header/kernel/user.h"
#include "header/process/process.h"
#include "header/process/scheduler.h"
#include "header/driver/timer.h"
#include "header/driver/serial.h"
#include "header/stdlib/string.h"

/* ================================ CONFIGURATION ==================================== */

static const uint8_t my_ip[4]   = {10, 0, 2, 15};
static const uint8_t gateway[4] = {10, 0, 2, 2};
static const uint8_t netmask[4] = {255, 255, 255, 0};
static const uint8_t dns[4]     = {10, 0, 2, 3};
static const uint8_t broadcast_mac[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

#define ETH_TYPE_IPV4 0x0800
#define ETH_TYPE_ARP  0x0806
#define IP_PROTO_ICMP 1
#define IP_PROTO_UDP  17
#define ARP_CACHE     8
#define PING_SOCKET   0xFFFFu

static bool    up = false;
static uint8_t my_mac[6];
static struct {
    uint32_t rx_packets, tx_packets, rx_dropped, udp_rx, udp_tx;
} stats;

static struct {
    bool    used;
    uint8_t ip[4];
    uint8_t mac[6];
} arp_cache[ARP_CACHE];
static uint32_t arp_next = 0;

struct Datagram {
    uint8_t  ip[4];
    uint16_t port;
    uint16_t length;
    uint8_t  data[NET_UDP_PAYLOAD_MAX];
};

// Receive queues are static: datagrams arrive in interrupt context, where kmalloc is off limits
static struct {
    bool            used;
    uint32_t        owner;
    uint16_t        port;
    struct Datagram queue[NET_SOCKET_QUEUE];
    uint32_t        head, count;
} sockets[NET_SOCKET_MAX];

static struct {
    bool     waiting;
    bool     answered;
    uint16_t id, sequence;
    uint32_t sent_ms, rtt_ms;
} ping;

static uint16_t ip_id = 1;

/* ================================ HELPERS ========================================== */

static uint16_t be16(const uint8_t *p) {
    return (uint16_t) (p[0] << 8 | p[1]);
}

static void put16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t) (v >> 8);
    p[1] = (uint8_t) v;
}

static uint16_t checksum(const uint8_t *data, uint32_t len) {
    uint32_t sum = 0;
    for (uint32_t i = 0; i + 1 < len; i += 2)
        sum += (uint32_t) (data[i] << 8 | data[i + 1]);
    if (len & 1)
        sum += (uint32_t) data[len - 1] << 8;
    while (sum >> 16)
        sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t) ~sum;
}

static bool same_ip(const uint8_t *a, const uint8_t *b) {
    return memcmp(a, b, 4) == 0;
}

static uint32_t irq_save(void) {
    uint32_t flags;
    __asm__ volatile("pushf; pop %0; cli" : "=r"(flags) : : "memory");
    return flags;
}

static void irq_restore(uint32_t flags) {
    if (flags & 0x200)
        __asm__ volatile("sti");
}

/* ================================ ETHERNET / ARP =================================== */

static bool send_ethernet(const uint8_t *dst_mac, uint16_t type, const uint8_t *payload, uint32_t len) {
    uint8_t frame[14 + NET_MTU];
    if (len > NET_MTU)
        return false;
    memcpy(frame, dst_mac, 6);
    memcpy(frame + 6, my_mac, 6);
    put16(frame + 12, type);
    memcpy(frame + 14, payload, len);
    uint32_t flags = irq_save(); // the IRQ handler also transmits (ARP / ping replies)
    bool ok = rtl8139_send(frame, 14 + len);
    irq_restore(flags);
    if (ok)
        stats.tx_packets++;
    return ok;
}

static void arp_store(const uint8_t *ip, const uint8_t *mac) {
    for (uint32_t i = 0; i < ARP_CACHE; i++) {
        if (arp_cache[i].used && same_ip(arp_cache[i].ip, ip)) {
            memcpy(arp_cache[i].mac, mac, 6);
            return;
        }
    }
    arp_cache[arp_next].used = true;
    memcpy(arp_cache[arp_next].ip, ip, 4);
    memcpy(arp_cache[arp_next].mac, mac, 6);
    arp_next = (arp_next + 1) % ARP_CACHE;
}

static bool arp_lookup(const uint8_t *ip, uint8_t *mac) {
    for (uint32_t i = 0; i < ARP_CACHE; i++) {
        if (arp_cache[i].used && same_ip(arp_cache[i].ip, ip)) {
            memcpy(mac, arp_cache[i].mac, 6);
            return true;
        }
    }
    return false;
}

static void arp_send(uint16_t op, const uint8_t *target_mac, const uint8_t *target_ip, const uint8_t *eth_dst) {
    uint8_t a[28];
    put16(a, 1);             // Ethernet
    put16(a + 2, ETH_TYPE_IPV4);
    a[4] = 6;
    a[5] = 4;
    put16(a + 6, op);        // 1 request, 2 reply
    memcpy(a + 8, my_mac, 6);
    memcpy(a + 14, my_ip, 4);
    memcpy(a + 18, target_mac, 6);
    memcpy(a + 24, target_ip, 4);
    send_ethernet(eth_dst, ETH_TYPE_ARP, a, sizeof(a));
}

static void arp_receive(const uint8_t *a, uint32_t len) {
    if (len < 28 || be16(a) != 1 || be16(a + 2) != ETH_TYPE_IPV4)
        return;
    arp_store(a + 14, a + 8); // learn the sender either way
    if (be16(a + 6) == 1 && same_ip(a + 24, my_ip))
        arp_send(2, a + 8, a + 14, a + 8);
}

// MAC of the next hop; asks with ARP and waits (interrupts on) for up to 1 s
static bool resolve(const uint8_t *ip, uint8_t *mac) {
    if (ip[0] == 255 && ip[1] == 255 && ip[2] == 255 && ip[3] == 255) {
        memcpy(mac, broadcast_mac, 6);
        return true;
    }
    uint8_t hop[4];
    bool local = true;
    for (int i = 0; i < 4; i++)
        local = local && (ip[i] & netmask[i]) == (my_ip[i] & netmask[i]);
    memcpy(hop, local ? ip : gateway, 4);
    if (arp_lookup(hop, mac))
        return true;
    uint8_t zero[6] = {0};
    for (int attempt = 0; attempt < 3; attempt++) {
        arp_send(1, zero, hop, broadcast_mac);
        uint32_t start = timer_get_ms();
        __asm__ volatile("sti");
        while (timer_get_ms() - start < 330) {
            if (arp_lookup(hop, mac))
                return true;
            __asm__ volatile("hlt");
        }
    }
    return false;
}

/* ================================ IPv4 / ICMP / UDP ================================ */

static bool send_ipv4(const uint8_t *dst, uint8_t protocol, const uint8_t *payload, uint32_t len) {
    uint8_t mac[6];
    uint8_t packet[NET_MTU];
    if (len + 20 > NET_MTU || !resolve(dst, mac))
        return false;
    memset(packet, 0, 20);
    packet[0] = 0x45; // IPv4, 20-byte header
    put16(packet + 2, (uint16_t) (20 + len));
    put16(packet + 4, ip_id++);
    packet[8] = 64;   // TTL
    packet[9] = protocol;
    memcpy(packet + 12, my_ip, 4);
    memcpy(packet + 16, dst, 4);
    put16(packet + 10, checksum(packet, 20));
    memcpy(packet + 20, payload, len);
    return send_ethernet(mac, ETH_TYPE_IPV4, packet, 20 + len);
}

static void icmp_receive(const uint8_t *src, const uint8_t *icmp, uint32_t len) {
    if (len < 8)
        return;
    if (icmp[0] == 8) { // echo request: answer it (pinging cOSmOS from the host works)
        uint8_t reply[NET_MTU];
        if (len > sizeof(reply))
            return;
        memcpy(reply, icmp, len);
        reply[0] = 0;
        put16(reply + 2, 0);
        put16(reply + 2, checksum(reply, len));
        send_ipv4(src, IP_PROTO_ICMP, reply, len);
    } else if (icmp[0] == 0 && ping.waiting && be16(icmp + 4) == ping.id && be16(icmp + 6) == ping.sequence) {
        ping.answered = true;
        ping.rtt_ms   = timer_get_ms() - ping.sent_ms;
        scheduler_request_resched();
    }
}

static void udp_receive(const uint8_t *src, const uint8_t *udp, uint32_t len) {
    if (len < 8)
        return;
    uint16_t src_port = be16(udp), dst_port = be16(udp + 2), ulen = be16(udp + 4);
    if (ulen < 8 || ulen > len)
        return;
    for (uint32_t i = 0; i < NET_SOCKET_MAX; i++) {
        if (!sockets[i].used || sockets[i].port != dst_port)
            continue;
        if (sockets[i].count == NET_SOCKET_QUEUE) {
            stats.rx_dropped++;
            return;
        }
        struct Datagram *d = &sockets[i].queue[(sockets[i].head + sockets[i].count) % NET_SOCKET_QUEUE];
        memcpy(d->ip, src, 4);
        d->port   = src_port;
        d->length = (uint16_t) (ulen - 8 > NET_UDP_PAYLOAD_MAX ? NET_UDP_PAYLOAD_MAX : ulen - 8);
        memcpy(d->data, udp + 8, d->length);
        sockets[i].count++;
        stats.udp_rx++;
        scheduler_request_resched();
        return;
    }
    stats.rx_dropped++; // nobody listens on that port
}

void net_receive_frame(const uint8_t *frame, uint32_t length) {
    stats.rx_packets++;
    if (length < 14)
        return;
    uint16_t type = be16(frame + 12);
    const uint8_t *p = frame + 14;
    uint32_t len = length - 14;
    if (type == ETH_TYPE_ARP) {
        arp_receive(p, len);
        return;
    }
    if (type != ETH_TYPE_IPV4 || len < 20 || (p[0] >> 4) != 4)
        return;
    uint32_t header = (uint32_t) (p[0] & 0xF) * 4, total = be16(p + 2);
    if (header < 20 || total > len || total < header || (!same_ip(p + 16, my_ip) && p[16] != 255))
        return;
    arp_store(p + 12, frame + 6); // remember who talks to us
    if (p[9] == IP_PROTO_ICMP)
        icmp_receive(p + 12, p + header, total - header);
    else if (p[9] == IP_PROTO_UDP)
        udp_receive(p + 12, p + header, total - header);
}

/* ================================ INTERFACE ======================================== */

bool net_init(void) {
    if (!rtl8139_init())
        return false;
    rtl8139_get_mac(my_mac);
    pic_unmask_irq(rtl8139_irq());
    up = true;
    serial_printf("[net] up: 10.0.2.15/24, gateway 10.0.2.2, DNS 10.0.2.3 (QEMU user network)\n");
    return true;
}

bool net_handle_irq(uint8_t irq) {
    if (!up || irq != rtl8139_irq())
        return false;
    rtl8139_handle_interrupt();
    pic_ack(irq);
    return true;
}

bool net_has_datagram(uint32_t pid, uint32_t socket) {
    if (socket == PING_SOCKET)
        return ping.answered && ping.id == (uint16_t) pid;
    return socket < NET_SOCKET_MAX && sockets[socket].used && sockets[socket].owner == pid && sockets[socket].count > 0;
}

void net_on_process_exit(uint32_t pid) {
    for (uint32_t i = 0; i < NET_SOCKET_MAX; i++)
        if (sockets[i].used && sockets[i].owner == pid)
            sockets[i].used = false;
    if (ping.waiting && ping.id == (uint16_t) pid)
        ping.waiting = false;
}

/* ================================ SYSTEM CALLS ===================================== */

static bool copy_from_user(void *dst, uint32_t src, uint32_t size) {
    if (!user_range_is_valid((void*) src, size))
        return false;
    memcpy(dst, (void*) src, size);
    return true;
}

static void copy_to_user(uint32_t dst, const void *src, uint32_t size) {
    if (user_range_is_valid((void*) dst, size))
        memcpy((void*) dst, src, size);
}

static int32_t socket_of(struct ProcessControlBlock *me, int32_t s) {
    if (me == NULL || s < 0 || s >= NET_SOCKET_MAX || !sockets[s].used || sockets[s].owner != me->metadata.pid)
        return -1;
    return s;
}

// Timed waits: the deadline survives the restart of the system call
static bool deadline_passed(struct ProcessControlBlock *me) {
    return (int32_t) (timer_get_ms() - me->metadata.wake_time_ms) >= 0;
}

void net_syscall(const struct InterruptFrame *frame, uint32_t eax, uint32_t ebx, uint32_t ecx, uint32_t edx) {
    (void) edx;
    struct ProcessControlBlock *me = process_get_current_running_pcb_pointer();
    switch (eax) {
        case SYS_NET_INFO: {
            struct SyscallNetInfo info;
            memset(&info, 0, sizeof(info));
            info.present = up;
            strncpy(info.driver, up ? "rtl8139" : "none", sizeof(info.driver) - 1);
            memcpy(info.mac, my_mac, 6);
            memcpy(info.ip, my_ip, 4);
            memcpy(info.gateway, gateway, 4);
            memcpy(info.netmask, netmask, 4);
            memcpy(info.dns, dns, 4);
            info.configured = up;
            info.rx_packets = stats.rx_packets;
            info.tx_packets = stats.tx_packets;
            info.rx_dropped = stats.rx_dropped;
            info.udp_rx     = stats.udp_rx;
            info.udp_tx     = stats.udp_tx;
            copy_to_user(ebx, &info, sizeof(info));
            break;
        }
        case SYS_NET_UDP_BIND: {
            int32_t result = -1;
            uint16_t port = (uint16_t) ebx;
            if (up && me != NULL) {
                if (port == 0) { // ephemeral port
                    static uint16_t next_port = 49152;
                    port = next_port++;
                    if (next_port == 0)
                        next_port = 49152;
                }
                bool taken = false;
                for (uint32_t i = 0; i < NET_SOCKET_MAX; i++)
                    taken = taken || (sockets[i].used && sockets[i].port == port);
                for (uint32_t i = 0; i < NET_SOCKET_MAX && !taken && result < 0; i++) {
                    if (!sockets[i].used) {
                        uint32_t flags = irq_save();
                        sockets[i].used  = true;
                        sockets[i].owner = me->metadata.pid;
                        sockets[i].port  = port;
                        sockets[i].head  = sockets[i].count = 0;
                        irq_restore(flags);
                        result = (int32_t) i;
                    }
                }
            }
            copy_to_user(ecx, &result, sizeof(result));
            break;
        }
        case SYS_NET_UDP_CLOSE:
            if (socket_of(me, (int32_t) ebx) >= 0)
                sockets[ebx].used = false;
            break;
        case SYS_NET_UDP_SEND: {
            struct SyscallUdp u;
            if (!copy_from_user(&u, ebx, sizeof(u)))
                break;
            u.result = -1;
            int32_t s = socket_of(me, u.socket);
            uint8_t packet[8 + NET_UDP_PAYLOAD_MAX];
            if (s >= 0 && u.len <= NET_UDP_PAYLOAD_MAX && copy_from_user(packet + 8, (uint32_t) u.buf, u.len)) {
                put16(packet, sockets[s].port);
                put16(packet + 2, u.port);
                put16(packet + 4, (uint16_t) (8 + u.len));
                put16(packet + 6, 0); // UDP checksum is optional over IPv4
                if (send_ipv4(u.ip, IP_PROTO_UDP, packet, 8 + u.len)) {
                    u.result = (int32_t) u.len;
                    stats.udp_tx++;
                }
            }
            copy_to_user(ebx + offsetof(struct SyscallUdp, result), &u.result, sizeof(u.result));
            break;
        }
        case SYS_NET_UDP_RECV: {
            struct SyscallUdp u;
            if (!copy_from_user(&u, ebx, sizeof(u)))
                break;
            int32_t s = socket_of(me, u.socket);
            u.result = -1;
            if (s >= 0) {
                uint32_t flags = irq_save();
                if (sockets[s].count > 0) {
                    struct Datagram *d = &sockets[s].queue[sockets[s].head];
                    uint32_t n = d->length < u.len ? d->length : u.len;
                    irq_restore(flags);
                    if (user_range_is_valid(u.buf, n)) {
                        memcpy(u.buf, d->data, n);
                        memcpy(u.ip, d->ip, 4);
                        u.port   = d->port;
                        u.result = (int32_t) n;
                    }
                    flags = irq_save();
                    sockets[s].head = (sockets[s].head + 1) % NET_SOCKET_QUEUE;
                    sockets[s].count--;
                    irq_restore(flags);
                    me->metadata.timed_wait = false;
                } else {
                    irq_restore(flags);
                    // first call: start the timeout; after a restart: keep the deadline
                    if (!me->metadata.timed_wait) {
                        me->metadata.timed_wait    = true; // "timed wait in progress"
                        me->metadata.wake_time_ms = timer_get_ms() + u.timeout_ms;
                    }
                    if (u.timeout_ms == 0 || deadline_passed(me)) {
                        me->metadata.timed_wait = false;
                        u.result = -2;
                    } else {
                        scheduler_block_current(frame, PROCESS_WAIT_NET, (uint32_t) s, true);
                    }
                }
            }
            copy_to_user(ebx, &u, sizeof(u));
            break;
        }
        case SYS_NET_PING: {
            struct SyscallPing p;
            if (!copy_from_user(&p, ebx, sizeof(p)) || me == NULL)
                break;
            p.result = -1;
            if (up) {
                if (!me->metadata.timed_wait) { // first call: send the echo request
                    uint8_t icmp[8 + 32];
                    memset(icmp, 0, sizeof(icmp));
                    icmp[0] = 8;
                    put16(icmp + 4, (uint16_t) me->metadata.pid);
                    put16(icmp + 6, p.sequence);
                    for (int i = 8; i < (int) sizeof(icmp); i++)
                        icmp[i] = (uint8_t) ('a' + (i - 8) % 26);
                    put16(icmp + 2, checksum(icmp, sizeof(icmp)));
                    ping.waiting  = true;
                    ping.answered = false;
                    ping.id       = (uint16_t) me->metadata.pid;
                    ping.sequence = p.sequence;
                    ping.sent_ms  = timer_get_ms();
                    if (!send_ipv4(p.ip, IP_PROTO_ICMP, icmp, sizeof(icmp))) {
                        ping.waiting = false;
                        copy_to_user(ebx, &p, sizeof(p));
                        break;
                    }
                    me->metadata.timed_wait    = true;
                    me->metadata.wake_time_ms = timer_get_ms() + p.timeout_ms;
                }
                if (ping.answered && ping.id == (uint16_t) me->metadata.pid) {
                    p.result = (int32_t) ping.rtt_ms;
                } else if (deadline_passed(me)) {
                    p.result = -2;
                } else {
                    scheduler_block_current(frame, PROCESS_WAIT_NET, PING_SOCKET, true);
                }
                ping.waiting = false;
                me->metadata.timed_wait = false;
            }
            copy_to_user(ebx, &p, sizeof(p));
            break;
        }
        default:
            break;
    }
}
