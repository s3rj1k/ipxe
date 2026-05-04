#!/bin/bash
# Test APIPA ARP-conflict retry on QEMU x86_64: most candidate
# addresses are pre-claimed, iPXE must retry to find a free one.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
export REPO_ROOT
# shellcheck source=../lib.sh
source "${SCRIPT_DIR}/../lib.sh"

build_ipxe amd64
stage_fat amd64 "${SCRIPT_DIR}/apipa-settings-test.ipxe"
setup_network
setup_arp_conflicts
trap_cleanup

echo ""
echo "Starting QEMU x86_64 (APIPA conflict resolution)"
echo "  NIC       : TAP ${TAP} via bridge ${BRIDGE}"
echo "  Host IP   : ${HOST_IP} on ${VETH_HOST}"
echo "  Conflicts : 169.254.1-190.x claimed, 191-254.x free"
echo ""

press_any_key
run_qemu_amd64
