#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "../../src/net_tcp.h"
#include "../../src/net_device.h"

// Simple TCP test application
// This demonstrates basic TCP connectivity

void connect_callback(tcp_connection_t *conn) {
    printf("TCP Test: Connected to server!\n");

    // Send a test message
    const char *message = "Hello from inDevOS TCP client!";
    tcp_send(conn, (const uint8_t*)message, strlen(message));

    printf("TCP Test: Sent message to server\n");
}

void data_callback(tcp_connection_t *conn, const uint8_t *data, uint32_t len) {
    printf("TCP Test: Received %u bytes: ", len);
    for (uint32_t i = 0; i < len && i < 100; i++) {
        printf("%c", data[i]);
    }
    if (len > 100) printf("...");
    printf("\n");

    // Echo the data back
    tcp_send(conn, data, len);
}

void close_callback(tcp_connection_t *conn) {
    printf("TCP Test: Connection closed\n");
}

int main(int argc, char *argv[]) {
    printf("=== inDevOS TCP Test Client ===\n");

    // Check if we have network connectivity
    uint8_t ip[4];
    net_get_ip_address(ip);

    if (ip[0] == 0 && ip[1] == 0 && ip[2] == 0 && ip[3] == 0) {
        printf("TCP Test: No IP address configured. Run 'dhcp' first.\n");
        return 1;
    }

    printf("TCP Test: Local IP: %d.%d.%d.%d\n", ip[0], ip[1], ip[2], ip[3]);

    // For demonstration, try to connect to a local server
    // In a real scenario, you'd specify the target IP and port
    uint8_t target_ip[4] = {127, 0, 0, 1}; // localhost
    uint16_t target_port = 8080;

    if (argc >= 2) {
        // Parse target IP from command line
        if (sscanf(argv[1], "%hhu.%hhu.%hhu.%hhu", &target_ip[0], &target_ip[1], &target_ip[2], &target_ip[3]) != 4) {
            printf("TCP Test: Invalid IP address format. Use: tcp_test <ip> [port]\n");
            return 1;
        }
    }

    if (argc >= 3) {
        target_port = atoi(argv[2]);
    }

    printf("TCP Test: Connecting to %d.%d.%d.%d:%u\n",
           target_ip[0], target_ip[1], target_ip[2], target_ip[3], target_port);

    // Create TCP connection
    tcp_connection_t *conn = tcp_connect(target_ip, target_port, connect_callback);

    if (!conn) {
        printf("TCP Test: Failed to create connection\n");
        return 1;
    }

    // Set callbacks
    conn->data_callback = data_callback;
    conn->close_callback = close_callback;

    printf("TCP Test: Connection initiated. Waiting for response...\n");
    printf("TCP Test: Connection state: %d\n", conn->state);

    // In a real application, we'd have an event loop here
    // For this test, we'll just wait a bit and then close
    for (volatile int i = 0; i < 10000000; i++) {
        // Busy wait - in a real app, this would be handled by the network thread
    }

    printf("TCP Test: Closing connection\n");
    tcp_close(conn);

    printf("TCP Test: Test completed\n");
    return 0;
}


















