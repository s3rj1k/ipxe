#!/bin/bash
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
IPXE_IMAGE="${SCRIPT_DIR}/src/bin-arm64-efi/ipxe.iso"
BRIDGE_IFACE="br0"
TAP_IFACE="tap-ipxe"
RANDOM_MAC=$(printf "52:54:00:%02x:%02x:%02x" $((RANDOM % 256)) $((RANDOM % 256)) $((RANDOM % 256)))

if [ ! -f "$IPXE_IMAGE" ]; then
	echo "Building iPXE ARM64 EFI image..."
	"${SCRIPT_DIR}/build-arm64-efi.sh"
fi

if ! ip link show "$BRIDGE_IFACE" &> /dev/null; then
	echo "Error: Bridge interface $BRIDGE_IFACE does not exist"
	exit 1
fi

if ! ip link show "$TAP_IFACE" &> /dev/null; then
	ip tuntap add dev "$TAP_IFACE" mode tap
fi

ip link set "$TAP_IFACE" up
ip link set "$TAP_IFACE" master "$BRIDGE_IFACE"

/usr/libexec/qemu-kvm \
	-machine virt \
	-cpu host \
	-m 1024M \
	-bios /usr/share/edk2/aarch64/QEMU_EFI.silent.fd \
	-drive file="${IPXE_IMAGE}",format=raw \
	-netdev tap,id=net0,ifname="$TAP_IFACE",script=no,downscript=no \
	-device virtio-net-pci,netdev=net0,mac="$RANDOM_MAC",romfile= \
	-nographic

ip link set "$TAP_IFACE" nomaster
ip link set "$TAP_IFACE" down
ip link delete "$TAP_IFACE"
