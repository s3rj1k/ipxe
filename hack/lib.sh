#!/bin/bash
# Shared helpers for iPXE APIPA QEMU test scripts.
# Source this file -- do not execute directly.

set -euo pipefail

BRIDGE="apipa_br"
TAP="apipa_tap"
VETH_HOST="apipa_host"
VETH_PEER="apipa_peer"
HOST_IP="169.254.1.42"
NIC_MAC="52:54:00:12:34:57"

# QEMU <-> TAP <-> bridge <-> veth_peer --- veth_host (169.254.1.42/16)
setup_network()
{
	echo "Setting up network: bridge=${BRIDGE} tap=${TAP} veth=${VETH_HOST}/${VETH_PEER}"

	teardown_network 2> /dev/null || true

	sudo ip link add "${BRIDGE}" type bridge
	sudo ip link set "${BRIDGE}" up

	sudo ip tuntap add dev "${TAP}" mode tap user "$(id -un)"
	sudo ip link set "${TAP}" master "${BRIDGE}"
	sudo ip link set "${TAP}" up

	sudo ip link add "${VETH_HOST}" type veth peer name "${VETH_PEER}"
	sudo ip link set "${VETH_PEER}" master "${BRIDGE}"
	sudo ip link set "${VETH_PEER}" up
	sudo ip link set "${VETH_HOST}" up

	sudo ip addr add "${HOST_IP}/16" dev "${VETH_HOST}"

	echo "Network ready: host ${HOST_IP} on ${VETH_HOST}"
}

# Claim 169.254.1.0/24 -- 169.254.190.0/24 (~75% of candidates) so
# retries are reliably triggered without exceeding RFC 3927
# MAX_CONFLICTS = 10 (which forces a 60s rate-limit delay).
setup_arp_conflicts()
{
	echo "Seeding ARP conflicts: 169.254.1.0/24 -- 169.254.190.0/24 on ${VETH_HOST}"
	local i
	for i in $(seq 1 190); do
		sudo ip route add local "169.254.${i}.0/24" dev "${VETH_HOST}" 2> /dev/null || true
	done
}

teardown_network()
{
	sudo ip link del "${TAP}" 2> /dev/null || true
	sudo ip link del "${VETH_HOST}" 2> /dev/null || true
	sudo ip link del "${BRIDGE}" 2> /dev/null || true
}

trap_cleanup()
{
	trap 'echo "Cleaning up..."; teardown_network; rm -rf "${FAT_DIR:-}"' \
		EXIT SIGINT SIGTERM
}

press_any_key()
{
	read -rp "Press any key to continue..." -n1
	echo ""
}

find_qemu()
{
	local arch="$1"
	local q
	for q in "qemu-system-${arch}" "/usr/libexec/qemu-kvm" "/usr/bin/qemu-system-${arch}"; do
		if command -v "$q" &> /dev/null; then
			echo "$q"
			return
		fi
	done
	echo "ERROR: qemu-system-${arch} not found" >&2
	exit 1
}

find_ovmf_code()
{
	local f
	for f in \
		/usr/share/OVMF/OVMF_CODE_4M.fd \
		/usr/share/edk2/ovmf/OVMF_CODE.fd \
		/usr/share/OVMF/OVMF_CODE.fd \
		/usr/share/qemu/OVMF.fd \
		/usr/share/ovmf/OVMF.fd; do
		[[ -f $f ]] && {
			echo "$f"
			return
		}
	done
	echo "ERROR: OVMF firmware not found. Install the ovmf package." >&2
	exit 1
}

find_ovmf_vars()
{
	local f
	for f in \
		/usr/share/OVMF/OVMF_VARS_4M.fd \
		/usr/share/edk2/ovmf/OVMF_VARS.fd \
		/usr/share/OVMF/OVMF_VARS.fd; do
		[[ -f $f ]] && {
			echo "$f"
			return
		}
	done
	echo "ERROR: OVMF VARS not found. Install the ovmf package." >&2
	exit 1
}

find_aavmf()
{
	local f
	for f in \
		/usr/share/AAVMF/AAVMF_CODE.fd \
		/usr/share/edk2/aarch64/QEMU_EFI.silent.fd \
		/usr/share/edk2/aarch64/QEMU_EFI.fd \
		/usr/share/qemu-efi-aarch64/QEMU_EFI.fd; do
		[[ -f $f ]] && {
			echo "$f"
			return
		}
	done
	echo "ERROR: AAVMF firmware not found. Install the qemu-efi-aarch64 package." >&2
	exit 1
}

build_ipxe()
{
	local arch="$1"
	local target cross

	case "${arch}" in
		amd64)
			target="x86_64"
			cross="CROSS=x86_64-linux-gnu-"
			;;
		arm64)
			target="arm64"
			cross="CROSS=aarch64-linux-gnu-"
			;;
		*)
			echo "ERROR: unknown arch ${arch}" >&2
			exit 1
			;;
	esac

	echo "Building iPXE (${target}-efi)..."
	make -C "${REPO_ROOT}/src" "bin-${target}-efi/ipxe.efi" \
		${cross} -j"$(nproc)" 2>&1 | tail -5
	echo "Built: src/bin-${target}-efi/ipxe.efi"
}

stage_fat()
{
	local arch="$1"
	local script="$2"
	local boot_name target

	case "${arch}" in
		amd64)
			boot_name="BOOTX64.EFI"
			target="x86_64"
			;;
		arm64)
			boot_name="BOOTAA64.EFI"
			target="arm64"
			;;
		*)
			echo "ERROR: unknown arch ${arch}" >&2
			exit 1
			;;
	esac

	local efi_src="${REPO_ROOT}/src/bin-${target}-efi/ipxe.efi"
	if [[ ! -f ${efi_src} ]]; then
		echo "ERROR: ${efi_src} not found. Build first." >&2
		exit 1
	fi

	FAT_DIR="$(mktemp -d /tmp/ipxe-apipa-fat.XXXXXX)"
	mkdir -p "${FAT_DIR}/EFI/BOOT"
	cp "${efi_src}" "${FAT_DIR}/EFI/BOOT/${boot_name}"
	cp "${script}" "${FAT_DIR}/EFI/BOOT/autoexec.ipxe"
	echo "Staged FAT: ${FAT_DIR}/EFI/BOOT/${boot_name} + autoexec.ipxe"
}

run_qemu_amd64()
{
	local qemu ovmf_code ovmf_vars ovmf_vars_tmp
	qemu="$(find_qemu x86_64)"
	ovmf_code="$(find_ovmf_code)"
	ovmf_vars="$(find_ovmf_vars)"
	ovmf_vars_tmp="/tmp/ipxe-apipa-ovmf-vars.fd"

	cp "${ovmf_vars}" "${ovmf_vars_tmp}"

	echo "  QEMU     : ${qemu}"
	echo "  Firmware : ${ovmf_code}"

	"${qemu}" \
		-machine q35 \
		-cpu max \
		-m 512M \
		-drive if=pflash,format=raw,readonly=on,file="${ovmf_code}" \
		-drive if=pflash,format=raw,file="${ovmf_vars_tmp}" \
		-drive file="fat:rw:${FAT_DIR}",format=raw,media=disk \
		-device virtio-net-pci,netdev=net0,mac="${NIC_MAC}",romfile= \
		-netdev tap,id=net0,ifname="${TAP}",script=no,downscript=no \
		-no-reboot \
		-nographic
}

run_qemu_arm64()
{
	local qemu aavmf aavmf_tmp flash_size
	qemu="$(find_qemu aarch64)"
	aavmf="$(find_aavmf)"
	aavmf_tmp="/tmp/ipxe-apipa-aavmf.fd"
	flash_size=$((64 * 1024 * 1024))

	cp "${aavmf}" "${aavmf_tmp}"
	truncate -s "${flash_size}" "${aavmf_tmp}"

	echo "  QEMU     : ${qemu}"
	echo "  Firmware : ${aavmf}"

	"${qemu}" \
		-machine virt \
		-cpu max \
		-m 512M \
		-bios "${aavmf_tmp}" \
		-drive file="fat:rw:${FAT_DIR}",format=raw,media=disk \
		-device virtio-rng-pci \
		-device virtio-net-pci,netdev=net0,mac="${NIC_MAC}",romfile= \
		-netdev tap,id=net0,ifname="${TAP}",script=no,downscript=no \
		-no-reboot \
		-nographic
}
