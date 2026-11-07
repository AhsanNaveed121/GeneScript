#!/bin/sh
# LLVM backend tests (Phase 2).  For every tests/llvm/*.gene:
#   1. generate IR with the LLVM backend               -> bin/llvm-tests/<name>.ll
#   2. verify the IR (llvm-as if installed, then clang compiling it with -Werror)
#   3. link with runtime/gs_runtime.c and run the native executable
#   4. compare its output with tests/llvm/expected/<name>.out
#   5. compare its output with the Phase 1 interpreter's output
# Usage: tests/llvm/run_llvm_tests.sh [interpreter] [llvm-backend]
# Env:   CLANG (default: clang)

INTERP=${1:-bin/genescript}
BACKEND=${2:-bin/genescript-llvm}
CLANG=${CLANG:-clang}
OUT=bin/llvm-tests

if ! command -v "$CLANG" >/dev/null 2>&1; then
    echo "ERROR: '$CLANG' not found; install clang (e.g. sudo apt install clang llvm) or set CLANG=..."
    exit 1
fi
LLVM_AS=""
for c in llvm-as $(ls /usr/bin/llvm-as-* 2>/dev/null | sort -V -r); do
    if command -v "$c" >/dev/null 2>&1; then LLVM_AS=$c; break; fi
done

mkdir -p "$OUT"
echo "clang: $("$CLANG" --version | head -n 1)"
[ -n "$LLVM_AS" ] && echo "llvm-as: $LLVM_AS" || echo "llvm-as: not found (clang verification only)"

if ! "$CLANG" -std=c11 -O2 -Wall -Wextra -Werror -c runtime/gs_runtime.c -o "$OUT/gs_runtime.o"; then
    echo "FAIL  runtime/gs_runtime.c does not compile"
    exit 1
fi

pass=0
fail=0
for t in tests/llvm/*.gene; do
    name=$(basename "$t" .gene)
    ll="$OUT/$name.ll"
    exe="$OUT/$name"
    expected="tests/llvm/expected/$name.out"
    why=""

    if ! "$BACKEND" "$t" -o "$ll" 2>"$OUT/$name.codegen.err"; then
        why="IR generation failed: $(cat "$OUT/$name.codegen.err")"
    elif [ -n "$LLVM_AS" ] && ! "$LLVM_AS" "$ll" -o /dev/null 2>"$OUT/$name.verify.err"; then
        why="llvm-as rejected IR: $(cat "$OUT/$name.verify.err")"
    elif ! "$CLANG" -Werror -c -x ir "$ll" -o "$OUT/$name.o" 2>"$OUT/$name.clang.err"; then
        why="clang rejected IR: $(cat "$OUT/$name.clang.err")"
    elif ! "$CLANG" "$OUT/$name.o" "$OUT/gs_runtime.o" -o "$exe" 2>"$OUT/$name.link.err"; then
        why="link failed: $(cat "$OUT/$name.link.err")"
    else
        native=$("$exe" 2>&1); native_rc=$?
        interp=$("$INTERP" "$t" 2>&1); interp_rc=$?
        want=$(tr -d '\r' < "$expected" 2>/dev/null)
        if [ ! -f "$expected" ]; then
            why="missing $expected"
        elif [ "$native" != "$want" ]; then
            why="native output differs from $expected:
$(printf '%s\n' "$native" | diff "$expected" - )"
        elif [ "$native" != "$interp" ] || [ "$native_rc" -ne "$interp_rc" ]; then
            why="native (rc=$native_rc) and interpreter (rc=$interp_rc) disagree:
$(printf '%s\n' "$interp" > "$OUT/$name.interp.out"; printf '%s\n' "$native" | diff "$OUT/$name.interp.out" -)"
        fi
    fi

    if [ -z "$why" ]; then
        echo "PASS  $name   (native == expected == interpreter, exit $native_rc)"
        printf '%s\n' "$native" | sed 's/^/      | /'
        pass=$((pass + 1))
    else
        echo "FAIL  $name"
        printf '%s\n' "$why" | sed 's/^/      /'
        fail=$((fail + 1))
    fi
done

echo "------------------------------------------"
echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ]
