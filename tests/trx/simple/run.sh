#!/bin/sh
set -eu
cd "$(dirname "$0")/../../.."
mkdir -p out/trx_simple
cc -std=c99 -O2 -Wall -Wextra -Werror -Iinclude -Isrc/trx/xt -Isrc/trx/xc -Isrc/trx/b2 tests/trx/simple/test_simple.c src/trx/xt/mdkit_trx_xt.c src/trx/xc/mdkit_trx_xc.c src/trx/b2/mdkit_trx_b2.c -lm -o out/trx_simple/test_simple
out/trx_simple/test_simple
