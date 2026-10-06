#!/bin/bash
# Network testing script for inDevOS
# This script provides a step-by-step guide to test DHCP and ping functionality

echo "=== inDevOS Network Test Guide ==="
echo ""
echo "Your OS already has all the networking components implemented!"
echo "Here's the step-by-step procedure to test DHCP and ping google.com:"
echo ""
echo "1. Boot your OS and wait for the kernel to load"
echo ""
echo "2. Check current network status:"
echo "   Command: net"
echo "   This will show your current IP, MAC address, and driver status"
echo ""
echo "3. Start DHCP client to get network configuration:"
echo "   Command: dhcp"
echo "   This will automatically request IP, gateway, DNS from DHCP server"
echo ""
echo "4. Check network status again to verify DHCP worked:"
echo "   Command: net"
echo "   You should now see a valid IP address, gateway, and DNS server"
echo ""
echo "5. Test DNS resolution:"
echo "   Command: dns google.com"
echo "   This will resolve google.com to its IP address"
echo ""
echo "6. Ping google.com:"
echo "   Command: ping <google-ip-from-step-5>"
echo "   Example: ping 172.217.14.142"
echo ""
echo "Alternative: You can also ping your gateway first to test local connectivity:"
echo "   Command: ping <gateway-ip-from-step-4>"
echo ""
echo "Available commands:"
echo "   net       - Show network status"
echo "   dhcp      - Start DHCP client"
echo "   dns <host> - Resolve hostname to IP"
echo "   ping <ip> - Send ICMP ping to IP address"
echo "   mac       - Show MAC address"
echo "   sendtest  - Send test ARP packet"
echo ""
echo "Troubleshooting:"
echo "   - If DHCP fails, check if you're running in a VM with NAT/bridge networking"
echo "   - If ping fails, try pinging your gateway first"
echo "   - Use 'net' command between each step to verify changes"
echo ""








