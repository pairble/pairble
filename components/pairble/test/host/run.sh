#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
#
# Compiles the component's pure input-handling code for this computer, with
# the address and undefined-behaviour sanitizers, and runs the tests.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT
${CC:-cc} -std=c11 -Wall -Wextra -Werror -g -fsanitize=address,undefined -fno-sanitize-recover=all \
    -I "$here/../../src" "$here/test_text.c" "$here/../../src/pairble_text.c" -o "$out/test_text"
"$out/test_text"
