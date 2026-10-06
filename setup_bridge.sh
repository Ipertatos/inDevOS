#!/bin/bash
#
# Bridge setup for QEMU networking (tap0 <-> Linux bridge <-> uplink NIC)
#
# This makes the VM appear directly on your LAN (DHCP comes from your real network).
# WARNING: Bridging can disrupt host connectivity if you move the host IP to the bridge.
#
# Usage:
#   sudo -v
#   UPLINK_IF=eth0 BR_NAME=br0 ./setup_bridge.sh
#
# Optional:
#   MOVE_UPLINK_IP=1   # move host IPv4 addresses + default route from uplink to bridge (recommended for proper bridging)
#
set -euo pipefail

echo "Setting up bridged networking for QEMU (tap0)..."

# Print a helpful message on errors (works with `set -e`)
trap 'echo "❌ setup_bridge.sh failed at line ${LINENO}: ${BASH_COMMAND}" >&2' ERR

# WSL2 note:
# WSL2 networking is NAT'd behind a virtual switch; true L2 bridging to the physical LAN
# generally isn't supported the same way as on a normal Linux host. In that environment,
# you'll typically never see your LAN's DHCP broadcasts/offers through a "bridge".
if grep -qi microsoft /proc/sys/kernel/osrelease 2>/dev/null || grep -qi microsoft /proc/version 2>/dev/null; then
  echo "❌ Detected WSL. Linux bridge mode is not reliably supported here."
  echo "   Use one of these instead:"
  echo "   - make run        (QEMU user networking; built-in DHCP)"
  echo "   - make run-tap    (tap0 + dnsmasq DHCP + NAT inside WSL)"
  exit 1
fi

# Sudo handling:
# - If we have a TTY, allow sudo to prompt interactively.
# - If we don't have a TTY (CI/automation), require passwordless sudo.
SUDO="sudo"
if [ "$(id -u)" -eq 0 ]; then
  SUDO=""
elif [ -t 0 ]; then
  SUDO="sudo"
else
  if ! sudo -n true 2>/dev/null; then
    echo "❌ sudo needs a password and no TTY is available for prompting."
    echo "   Run from a real terminal, or authenticate first (then re-run):"
    echo "     sudo -v"
    exit 1
  fi
  SUDO="sudo -n"
fi

BR_NAME="${BR_NAME:-br0}"

# Try to auto-detect uplink from default route if not provided.
if [ "${UPLINK_IF:-}" = "" ]; then
  # `ip route get` can fail on some hosts (no default route / restricted env).
  # With `set -euo pipefail`, a failing pipeline would abort the script, so we guard it.
  UPLINK_IF="$((ip route get 1.1.1.1 2>/dev/null || true) | awk '{for(i=1;i<=NF;i++) if ($i=="dev"){print $(i+1); exit}}')"
fi
UPLINK_IF="${UPLINK_IF:-}"

if [ -z "$UPLINK_IF" ]; then
  echo "❌ Could not auto-detect uplink interface. Set UPLINK_IF (e.g. eth0, enp3s0)."
  exit 1
fi

echo "Bridge:   $BR_NAME"
echo "Uplink:   $UPLINK_IF"
echo "Tap dev:  tap0"

# Create bridge if missing
if ! ip link show "$BR_NAME" &>/dev/null; then
  echo "Creating bridge $BR_NAME..."
  $SUDO ip link add name "$BR_NAME" type bridge
fi

# Create tap0 if missing
if ! ip link show tap0 &>/dev/null; then
  echo "Creating tap0..."
  $SUDO ip tuntap add dev tap0 mode tap
fi

# Bring devices up
$SUDO ip link set "$BR_NAME" up
$SUDO ip link set tap0 up

# Attach tap0 to bridge
$SUDO ip link set tap0 master "$BR_NAME"

# Attach uplink to bridge (this is what makes it a real LAN bridge)
echo "Enslaving $UPLINK_IF into $BR_NAME..."
$SUDO ip link set "$UPLINK_IF" down || true
$SUDO ip link set "$UPLINK_IF" master "$BR_NAME"
$SUDO ip link set "$UPLINK_IF" up || true

# Proper bridging expects the host IP to live on the bridge, not the enslaved uplink.
if [ "${MOVE_UPLINK_IP:-0}" = "1" ]; then
  echo "Moving host IPv4 addresses from $UPLINK_IF to $BR_NAME..."
  # Capture existing IPv4 CIDRs on uplink
  IPV4_CIDRS="$($SUDO ip -4 -o addr show dev "$UPLINK_IF" scope global | awk '{print $4}')"
  if [ -n "$IPV4_CIDRS" ]; then
    $SUDO ip -4 addr flush dev "$UPLINK_IF"
    for cidr in $IPV4_CIDRS; do
      $SUDO ip -4 addr add "$cidr" dev "$BR_NAME"
    done
  fi

  # Ensure default route goes via bridge (best effort)
  GW="$($SUDO ip route show default 2>/dev/null | awk '/default/ {print $3; exit}')"
  if [ -n "$GW" ]; then
    $SUDO ip route replace default via "$GW" dev "$BR_NAME" || true
  else
    $SUDO ip route replace default dev "$BR_NAME" || true
  fi
else
  echo "NOTE: MOVE_UPLINK_IP is not set. Many setups require moving the host IP to the bridge."
  echo "      If your host loses connectivity or the bridge doesn't pass traffic, rerun with:"
  echo "        MOVE_UPLINK_IP=1 UPLINK_IF=$UPLINK_IF BR_NAME=$BR_NAME ./setup_bridge.sh"
fi

echo "✅ Bridge setup complete!"
echo "  Bridge: $BR_NAME"
echo "  Uplink enslaved: $UPLINK_IF"
echo "  Tap: tap0"
echo ""
echo "Now run: make run-bridge"


