#!/bin/bash
set -e

# WSL/Host Setup Script for Host-Only Networking with NAT
# matches the 'tapconfig' defaults in inDevOS (192.168.56.x)

TAP_DEV="tap0"
TAP_IP="192.168.56.1"
TAP_NET="192.168.56.0/24"
UPLINK_DEV=$(ip route | grep default | awk '{print $5}' | head -n1)

if [ -z "$UPLINK_DEV" ]; then
    echo "Warning: Could not detect default uplink interface. Internet access might not work."
    UPLINK_DEV="eth0"
fi

echo "Setting up $TAP_DEV for WSL/Host-Only networking..."
echo "  Host IP:   $TAP_IP"
echo "  Guest IP:  192.168.56.2 (configured in OS via 'tapconfig')"
echo "  Uplink:    $UPLINK_DEV (NAT)"

# Create tap interface
if ! ip link show $TAP_DEV >/dev/null 2>&1; then
    sudo ip tuntap add dev $TAP_DEV mode tap
    echo "  Created $TAP_DEV"
fi

# Set IP address on host side
sudo ip addr flush dev $TAP_DEV
sudo ip addr add $TAP_IP/24 dev $TAP_DEV
# Set static MAC address for consistency
sudo ip link set dev $TAP_DEV address fe:00:00:00:00:01
sudo ip link set $TAP_DEV up
sudo sysctl -w net.ipv4.conf.$TAP_DEV.rp_filter=0
sudo sysctl -w net.ipv4.conf.all.rp_filter=0
sudo ip neigh flush all dev $TAP_DEV
echo "  Configured $TAP_DEV with $TAP_IP/24 and MAC fe:00:00:00:00:01"

# Enable IP forwarding (for internet access)
echo 1 | sudo tee /proc/sys/net/ipv4/ip_forward >/dev/null

# Set up NAT/Masquerading (so guest can reach internet)
echo "  Setting up NAT (iptables)..."
# Clean up old rules to avoid duplicates (simple approach)
sudo iptables -t nat -D POSTROUTING -s $TAP_NET -o $UPLINK_DEV -j MASQUERADE 2>/dev/null || true
sudo iptables -t nat -A POSTROUTING -s $TAP_NET -o $UPLINK_DEV -j MASQUERADE

# Accept traffic from tap0
sudo iptables -D INPUT -i $TAP_DEV -j ACCEPT 2>/dev/null || true
sudo iptables -A INPUT -i $TAP_DEV -j ACCEPT
# Accept traffic TO tap0
sudo iptables -D OUTPUT -o $TAP_DEV -j ACCEPT 2>/dev/null || true
sudo iptables -A OUTPUT -o $TAP_DEV -j ACCEPT
sudo iptables -D FORWARD -i $TAP_DEV -o $UPLINK_DEV -j ACCEPT 2>/dev/null || true
sudo iptables -A FORWARD -i $TAP_DEV -o $UPLINK_DEV -j ACCEPT
sudo iptables -D FORWARD -i $UPLINK_DEV -o $TAP_DEV -m state --state RELATED,ESTABLISHED -j ACCEPT 2>/dev/null || true
sudo iptables -A FORWARD -i $UPLINK_DEV -o $TAP_DEV -m state --state RELATED,ESTABLISHED -j ACCEPT

echo "✅ Setup complete."
echo "   Run 'make run-tap' and then use 'tapconfig' inside inDevOS."
