#!/usr/bin/env bash
# Offline only; no DVB nodes, USB control requests or network hardware.
set -euo pipefail
cd "$(dirname "$0")/../.."
out=${1:-build/offline}
mkdir -p "$out"
exec > >(tee "$out/offline-tests.log") 2>&1
for test in transport_codec protocol ca_queue program_specific_information; do
    "${HOSTCC:-gcc}" -Wall -Wextra -Werror -fsanitize=address,undefined -g "tests/test_$test.c" -o "$out/test_$test"
    "$out/test_$test"
done
"${HOSTCC:-gcc}" -Wall -Wextra -Werror -fsanitize=address,undefined -g -pthread tests/test_stream_concurrency.c -o "$out/test_stream_concurrency"
"$out/test_stream_concurrency"
"${HOSTCC:-gcc}" -Wall -Wextra -Werror -fsanitize=address,undefined -g -pthread tools/server/*.c -o "$out/smit"
python3 tests/test_http_server.py --binary "$out/smit"
for test in tests/test_*.py; do
    [[ "$test" == tests/test_http_server.py ]] && continue
    PYTHONDONTWRITEBYTECODE=1 python3 "$test" -v
done
