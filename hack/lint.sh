#!/bin/bash
# Run clang-format, scan-build and gcc -fanalyzer against APIPA sources
# inside a Docker container, for both x86_64 and arm64 targets.
#
# Usage:
#   hack/lint.sh               — run all analyzers (both arches)
#   hack/lint.sh format        — apply clang-format in place to APIPA sources
#   hack/lint.sh format-check  — clang-format dry-run check
#   hack/lint.sh shellcheck    — shellcheck on hack/ scripts
#   hack/lint.sh scan-build    — Clang static analyzer (both arches)
#   hack/lint.sh gcc-analyzer  — GCC -fanalyzer (both arches)
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
IMAGE_NAME="ipxe-apipa-lint"

APIPA_OBJS_X64=(
	bin-x86_64-efi/apipa.o
)

APIPA_OBJS_ARM64=(
	bin-arm64-efi/apipa.o
)

APIPA_SOURCES=(
	src/net/apipa.c
	src/include/ipxe/apipa.h
)

SHELL_SOURCES=(
	hack/lint.sh
	hack/lib.sh
	hack/apipa/test-apipa-conflict-amd64.sh
	hack/apipa/test-apipa-conflict-arm64.sh
	hack/apipa/test-apipa-settings-amd64.sh
	hack/apipa/test-apipa-settings-arm64.sh
)

build_docker()
{
	echo "Building Docker image..."
	docker build -q -t "${IMAGE_NAME}" -f "${REPO_ROOT}/hack/Dockerfile.lint" "${REPO_ROOT}"
}

docker_run()
{
	docker run --rm -v "${REPO_ROOT}:/src:ro" -w /src/src "${IMAGE_NAME}" "$@"
}

run_format_check()
{
	local rc=0
	echo ""
	echo "=== clang-format check ==="
	for src in "${APIPA_SOURCES[@]}"; do
		echo "  ${src}"
		if ! docker_run clang-format --dry-run --Werror \
				--style=file:/src/hack/.clang-format \
				"/src/${src}" 2>&1; then
			rc=1
		fi
	done
	return "${rc}"
}

run_format()
{
	# Use SUDO_UID/GID when invoked under sudo so output files
	# stay owned by the original user.
	local uid="${SUDO_UID:-$(id -u)}"
	local gid="${SUDO_GID:-$(id -g)}"
	echo ""
	echo "=== clang-format apply ==="
	for src in "${APIPA_SOURCES[@]}"; do
		echo "  ${src}"
		docker run --rm \
			-v "${REPO_ROOT}:/src" -w /src/src \
			--user "${uid}:${gid}" \
			"${IMAGE_NAME}" \
			clang-format -i \
			--style=file:/src/hack/.clang-format \
			"/src/${src}"
	done
}

run_shellcheck()
{
	local rc=0
	echo ""
	echo "=== shellcheck ==="
	for src in "${SHELL_SOURCES[@]}"; do
		echo "  ${src}"
		if ! docker_run shellcheck -x --source-path=SCRIPTDIR \
				"/src/${src}" 2>&1; then
			rc=1
		fi
	done
	return "${rc}"
}

run_scan_build_arch()
{
	local arch="$1"
	shift
	local objs=("$@")
	local rc=0

	local cross cc
	case "${arch}" in
		x86_64)
			cross="CROSS=x86_64-linux-gnu-"
			cc="x86_64-linux-gnu-gcc"
			;;
		arm64)
			cross="CROSS=aarch64-linux-gnu-"
			cc="aarch64-linux-gnu-gcc"
			;;
	esac

	echo ""
	echo "--- scan-build [${arch}] ---"
	for obj in "${objs[@]}"; do
		echo "  ${obj}"
		if ! docker run --rm \
			-v "${REPO_ROOT}:/src" -w /src/src \
			"${IMAGE_NAME}" \
			bash -c "rm -f '${obj}' && scan-build \
                --use-cc=${cc} \
                -o /tmp/scan-results \
                --status-bugs \
                -enable-checker alpha.security.ArrayBoundV2 \
                -enable-checker alpha.security.MallocOverflow \
                -enable-checker alpha.security.ReturnPtrRange \
                -enable-checker alpha.security.taint.TaintPropagation \
                -enable-checker alpha.core.BoolAssignment \
                -enable-checker alpha.core.CastSize \
                -enable-checker alpha.core.IdenticalExpr \
                -enable-checker alpha.core.SizeofPtr \
                make '${obj}' ${cross}" 2>&1; then
			rc=1
		fi
	done
	return "${rc}"
}

run_scan_build()
{
	local rc=0
	echo ""
	echo "=== scan-build ==="
	run_scan_build_arch x86_64 "${APIPA_OBJS_X64[@]}" || rc=1
	run_scan_build_arch arm64 "${APIPA_OBJS_ARM64[@]}" || rc=1
	return "${rc}"
}

run_gcc_analyzer_arch()
{
	local arch="$1"
	shift
	local objs=("$@")
	local rc=0

	local cross
	case "${arch}" in
		x86_64) cross="CROSS=x86_64-linux-gnu-" ;;
		arm64) cross="CROSS=aarch64-linux-gnu-" ;;
	esac

	local extra="-fanalyzer -Wuninitialized -Wnull-dereference -Wformat-security"

	echo ""
	echo "--- gcc -fanalyzer [${arch}] ---"
	for obj in "${objs[@]}"; do
		echo "  ${obj}"
		if ! docker run --rm \
			-v "${REPO_ROOT}:/src" -w /src/src \
			"${IMAGE_NAME}" \
			bash -c "rm -f '${obj}' && make '${obj}' ${cross} EXTRA_CFLAGS='${extra}' && rm -f '${obj}'" 2>&1; then
			rc=1
		fi
	done
	return "${rc}"
}

run_gcc_analyzer()
{
	local rc=0
	echo ""
	echo "=== gcc -fanalyzer ==="
	run_gcc_analyzer_arch x86_64 "${APIPA_OBJS_X64[@]}" || rc=1
	run_gcc_analyzer_arch arm64 "${APIPA_OBJS_ARM64[@]}" || rc=1
	return "${rc}"
}

MODE="${1:-all}"
EXIT_CODE=0

build_docker

case "${MODE}" in
	format) run_format ;;
	format-check) run_format_check || EXIT_CODE=1 ;;
	shellcheck) run_shellcheck || EXIT_CODE=1 ;;
	scan-build) run_scan_build || EXIT_CODE=1 ;;
	gcc-analyzer) run_gcc_analyzer || EXIT_CODE=1 ;;
	all)
		run_format_check || EXIT_CODE=1
		run_shellcheck || EXIT_CODE=1
		run_scan_build || EXIT_CODE=1
		run_gcc_analyzer || EXIT_CODE=1
		;;
	*)
		echo "Usage: $0 [format|format-check|shellcheck|scan-build|gcc-analyzer|all]"
		exit 1
		;;
esac

if [[ ${EXIT_CODE} -eq 0 ]]; then
	echo ""
	echo "All checks passed."
fi

exit "${EXIT_CODE}"
