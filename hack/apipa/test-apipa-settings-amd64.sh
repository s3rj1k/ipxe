#!/bin/bash
# Test APIPA settings on QEMU x86_64: claim a link-local address,
# set gateway/dns/ntp/user-class, verify, then ping the host.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
export REPO_ROOT
# shellcheck source=../lib.sh
source "${SCRIPT_DIR}/../lib.sh"

build_ipxe amd64
stage_fat amd64 "${SCRIPT_DIR}/apipa-settings-test.ipxe"
setup_network
trap_cleanup

echo ""
echo "Starting QEMU x86_64 (APIPA settings test)"
echo "  NIC      : TAP ${TAP} via bridge ${BRIDGE}"
echo "  Host IP  : ${HOST_IP} on ${VETH_HOST}"
echo ""

press_any_key
run_qemu_amd64
