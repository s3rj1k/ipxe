#!/bin/bash
set -e

cd "$(dirname "$0")/src"

GIT_VERSION=$(git describe --tags --always --long --abbrev=1 --match "v*" 2> /dev/null || echo "")

if [[ $GIT_VERSION =~ ^v([0-9]+)\.([0-9]+)\.([0-9]+)-([0-9]+)-g(.+)$ ]]; then
	VERSION_MAJOR="${BASH_REMATCH[1]}"
	VERSION_MINOR="${BASH_REMATCH[2]}"
	VERSION_PATCH="${BASH_REMATCH[3]}"
	GITVERSION="${BASH_REMATCH[5]}"
else
	VERSION_MAJOR=1
	VERSION_MINOR=0
	VERSION_PATCH=0
	GITVERSION="${GIT_VERSION:-unknown}"
fi

echo "=== Cleaning build artifacts ==="
make clean

echo "=== Formatting APIPA files with clang-format ==="
clang-format -i \
	usr/apipa.c \
	hci/commands/apipa_cmd.c \
	include/usr/apipa.h \
	include/ipxe/apipa.h

echo "=== Building ARM64 tests ==="
make bin-arm64-linux/tests.linux \
	VERSION_MAJOR="$VERSION_MAJOR" \
	VERSION_MINOR="$VERSION_MINOR" \
	VERSION_PATCH="$VERSION_PATCH" \
	GITVERSION="$GITVERSION"

echo "=== Running ARM64 tests with valgrind ==="
valgrind --leak-check=full --error-exitcode=1 --errors-for-leak-kinds=all \
	bin-arm64-linux/tests.linux

echo "=== Tests passed, building ARM64 EFI ISO ==="
make bin-arm64-efi/ipxe.iso \
	VERSION_MAJOR="$VERSION_MAJOR" \
	VERSION_MINOR="$VERSION_MINOR" \
	VERSION_PATCH="$VERSION_PATCH" \
	GITVERSION="$GITVERSION" \
	EMBED=embedded.ipxe

echo "Built: bin-arm64-efi/ipxe.iso"
