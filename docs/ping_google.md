# How to Ping Google.com in inDevOS

Your inDevOS kernel already has a complete networking stack implemented! Here's how to get DHCP working and ping google.com:

## Prerequisites
- Boot your inDevOS kernel
- Ensure you're running in a VM with proper network configuration (NAT or bridged)

## Step-by-Step Guide

### 1. Check Initial Network Status
```
net
```
This shows your current network configuration, MAC address, and driver status.

### 2. Start DHCP Client
```
dhcp
```
This command will:
- Send DHCP DISCOVER packet
- Wait for DHCP OFFER
- Send DHCP REQUEST
- Configure IP, subnet mask, gateway, and DNS server automatically

### 3. Verify DHCP Configuration
```
net
```
You should now see:
- A valid IP address (like 192.168.x.x or 10.x.x.x)
- Subnet mask
- Gateway IP
- DNS server IP

### 4. Test DNS Resolution
```
dns google.com
```
This will resolve google.com to its IP address (something like 172.217.14.142).

### 5. Ping Google!
```
ping <google-ip-from-step-4>
```
For example:
```
ping 172.217.14.142
```

## Alternative Testing

### Test Local Connectivity First
If you want to test step by step:

1. Ping your gateway:
   ```
   ping <gateway-ip>
   ```

2. Ping a known external IP:
   ```
   ping 8.8.8.8
   ```

3. Then try DNS and ping google.com

## Available Network Commands

- `net` - Show network status
- `dhcp` - Start DHCP client
- `dns <hostname>` - Resolve hostname to IP
- `ping <ip>` - Send ICMP ping to IP address
- `mac` - Show MAC address
- `sendtest` - Send test ARP packet
- `netset` - Manually set network configuration

## Implemented Features

Your OS includes:
- ✅ DHCP client (DISCOVER/OFFER/REQUEST/ACK)
- ✅ DNS resolver (A record queries)
- ✅ ICMP ping (echo request/reply)
- ✅ ARP protocol
- ✅ IP routing
- ✅ UDP/IP stack
- ✅ Ethernet frame handling
- ✅ Network device abstraction

## Troubleshooting

1. **DHCP fails**: Check VM network settings (NAT/bridge mode)
2. **No network driver**: Ensure RTL8139 or E1000 is available in VM
3. **Ping timeouts**: Try gateway first, then external IPs
4. **DNS fails**: Verify DHCP configured DNS server correctly

The networking stack is fully functional - just run the commands above!








