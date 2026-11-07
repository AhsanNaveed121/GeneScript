#!/bin/sh
# Runs every tests/fix*.gene and compares its output (stdout + stderr)
# with tests/expected/<name>.out. Usage: tests/run_tests.sh [path/to/genescript]

BIN=${1:-bin/genescript}
pass=0
fail=0

for t in tests/fix*.gene; do
    name=$(basename "$t" .gene)
    expected="tests/expected/$name.out"
    actual=$("$BIN" "$t" 2>&1)

    if [ -f "$expected" ] && [ "$actual" = "$(cat "$expected")" ]; then
        echo "PASS  $name"
        pass=$((pass + 1))
    else
        echo "FAIL  $name"
        if [ -f "$expected" ]; then
            printf '%s\n' "$actual" | diff "$expected" - | sed 's/^/      /'
        else
            echo "      missing $expected"
        fi
        fail=$((fail + 1))
    fi
    printf '%s\n' "$actual" | sed 's/^/      | /'
done

echo "------------------------------------------"
echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ]
