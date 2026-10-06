/*
 * udp_test - userspace SOCK_DGRAM smoke test
 *
 *   udp_test send <ip> <port> <message>
 *   udp_test recv <port>
 */
#include "../include/syscalls.h"

static void print_str(const char *s) {
    size_t n = 0;
    while (s[n]) n++;
    write(STDOUT_FILENO, s, n);
}

static int parse_u16(const char *s, uint16_t *out) {
    uint32_t n = 0;
    if (!s || !*s) return -1;
    while (*s) {
        if (*s < '0' || *s > '9') return -1;
        n = n * 10 + (uint32_t)(*s - '0');
        if (n > 65535) return -1;
        s++;
    }
    *out = (uint16_t)n;
    return 0;
}

static int parse_ip(const char *s, uint8_t ip[4]) {
    int part = 0;
    uint32_t n = 0;
    int digits = 0;
    for (;; s++) {
        if (*s >= '0' && *s <= '9') {
            n = n * 10 + (uint32_t)(*s - '0');
            if (n > 255) return -1;
            digits++;
            continue;
        }
        if (digits == 0) return -1;
        ip[part++] = (uint8_t)n;
        n = 0;
        digits = 0;
        if (*s == '.') {
            if (part >= 4) return -1;
            continue;
        }
        if (*s == '\0')
            return part == 4 ? 0 : -1;
        return -1;
    }
}

static int str_eq(const char *a, const char *b) {
    while (*a && *b && *a == *b) { a++; b++; }
    return *a == *b;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        print_str("Usage: udp_test send <ip> <port> <msg>\n");
        print_str("       udp_test recv <port>\n");
        return 1;
    }

    int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd < 0) {
        print_str("udp_test: socket() failed\n");
        return 1;
    }

    if (str_eq(argv[1], "send")) {
        if (argc < 5) {
            print_str("Usage: udp_test send <ip> <port> <msg>\n");
            return 1;
        }
        uint8_t ip[4];
        uint16_t port;
        if (parse_ip(argv[2], ip) != 0 || parse_u16(argv[3], &port) != 0) {
            print_str("udp_test: bad ip/port\n");
            return 1;
        }
        sockaddr_in_t dest;
        dest.sin_family = AF_INET;
        dest.sin_port = htons(port);
        dest.sin_addr = (uint32_t)ip[0] | ((uint32_t)ip[1] << 8) |
                        ((uint32_t)ip[2] << 16) | ((uint32_t)ip[3] << 24);
        for (int i = 0; i < 8; i++) dest.sin_zero[i] = 0;

        size_t len = 0;
        while (argv[4][len]) len++;
        int n = sendto(fd, argv[4], (uint32_t)len, 0, &dest, sizeof(dest));
        if (n < 0) {
            print_str("udp_test: sendto failed\n");
            return 1;
        }
        print_str("udp_test: sent ok\n");
        return 0;
    }

    if (str_eq(argv[1], "recv")) {
        if (argc < 3) {
            print_str("Usage: udp_test recv <port>\n");
            return 1;
        }
        uint16_t port;
        if (parse_u16(argv[2], &port) != 0) {
            print_str("udp_test: bad port\n");
            return 1;
        }
        sockaddr_in_t bind_addr;
        bind_addr.sin_family = AF_INET;
        bind_addr.sin_port = htons(port);
        bind_addr.sin_addr = 0;
        for (int i = 0; i < 8; i++) bind_addr.sin_zero[i] = 0;
        if (bind(fd, &bind_addr, sizeof(bind_addr)) != 0) {
            print_str("udp_test: bind failed\n");
            return 1;
        }
        print_str("udp_test: waiting...\n");
        char buf[256];
        sockaddr_in_t src;
        int n = recvfrom(fd, buf, sizeof(buf) - 1, 0, &src, 0);
        if (n < 0) {
            print_str("udp_test: recvfrom failed\n");
            return 1;
        }
        buf[n] = '\0';
        print_str("udp_test: got: ");
        print_str(buf);
        print_str("\n");
        return 0;
    }

    print_str("udp_test: unknown command\n");
    return 1;
}
