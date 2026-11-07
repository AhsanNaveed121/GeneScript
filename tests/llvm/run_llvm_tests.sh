#!/bin/sh
# LLVM backend tests (Phase 2).
#
# [runtime]  tests/llvm/runtime_unit.c: the C runtime alone, under ASan.
# [valid]    tests/llvm/*.gene: generate IR -> llvm-as -> clang -Werror -> link
#            -> run; output (stdout+stderr) and exit code must equal
#            tests/llvm/expected/<name>.out AND the Phase 1 interpreter.  The
#            program is also linked against an AddressSanitizer runtime and
#            must behave identically with no use-after-free / double-free /
#            leak reports.
# [errors]   tests/llvm/errors/*.gene: the backend must reject the program
#            before any IR is written (exit 1, no .ll), with diagnostics equal
#            to errors/<name>.err AND to the interpreter's.
# [phase1]   tests/*.gene: every Phase 1 program is pushed through the backend.
#            Programs using constructs not lowered yet are SKIPped (listed);
#            all others must match the interpreter exactly.
#
# Usage: tests/llvm/run_llvm_tests.sh [interpreter] [llvm-backend]
# Env:   CLANG (default clang), GS_NO_ASAN=1 to skip sanitizer runs.

INTERP=${1:-bin/genescript}
BACKEND=${2:-bin/genescript-llvm}
CLANG=${CLANG:-clang}
OUT=bin/llvm-tests
UNSUPPORTED="is not supported by the LLVM backend yet"

if ! command -v "$CLANG" >/dev/null 2>&1; then
    echo "ERROR: '$CLANG' not found; install clang (e.g. sudo apt install clang llvm) or set CLANG=..."
    exit 1
fi
LLVM_AS=""
for c in llvm-as $(ls /usr/bin/llvm-as-* 2>/dev/null | sort -V -r); do
    if command -v "$c" >/dev/null 2>&1; then LLVM_AS=$c; break; fi
done

rm -rf "$OUT"
mkdir -p "$OUT/errors" "$OUT/phase1"
echo "clang:   $("$CLANG" --version | head -n 1)"
[ -n "$LLVM_AS" ] && echo "llvm-as: $LLVM_AS" || echo "llvm-as: not found (clang verification only)"

pass=0
fail=0
skip=0
skipped=""
ok()   { echo "PASS  $1"; pass=$((pass + 1)); }
bad()  { echo "FAIL  $1"; printf '%s\n' "$2" | sed 's/^/      /'; fail=$((fail + 1)); }

if ! "$CLANG" -std=c11 -O2 -Wall -Wextra -Werror -c runtime/gs_runtime.c -o "$OUT/gs_runtime.o"; then
    echo "FAIL  runtime/gs_runtime.c does not compile"; exit 1
fi
ASAN=1
[ -n "$GS_NO_ASAN" ] && ASAN=""
if [ -n "$ASAN" ] && ! "$CLANG" -std=c11 -g -O1 -fsanitize=address -fno-omit-frame-pointer \
        -c runtime/gs_runtime.c -o "$OUT/gs_runtime_asan.o" 2>/dev/null; then
    echo "note: AddressSanitizer unavailable; sanitizer runs skipped"; ASAN=""
fi
export ASAN_OPTIONS="exitcode=99:detect_leaks=1:abort_on_error=0"

# compile_and_run <gene> <name> <dir>: sets $why (empty on success), $native, $native_rc
compile_and_run() {
    ll="$3/$2.ll"
    why=""
    if ! "$BACKEND" "$1" -o "$ll" 2>"$3/$2.codegen.err"; then
        why="IR generation failed: $(cat "$3/$2.codegen.err")"; return
    fi
    if [ -n "$LLVM_AS" ] && ! "$LLVM_AS" "$ll" -o /dev/null 2>"$3/$2.verify.err"; then
        why="llvm-as rejected IR: $(cat "$3/$2.verify.err")"; return
    fi
    if ! "$CLANG" -Werror -c -x ir "$ll" -o "$3/$2.o" 2>"$3/$2.clang.err"; then
        why="clang rejected IR: $(cat "$3/$2.clang.err")"; return
    fi
    if ! "$CLANG" "$3/$2.o" "$OUT/gs_runtime.o" -o "$3/$2" 2>"$3/$2.link.err"; then
        why="link failed: $(cat "$3/$2.link.err")"; return
    fi
    native=$("$3/$2" 2>&1); native_rc=$?
    if [ -n "$ASAN" ]; then
        if ! "$CLANG" -fsanitize=address "$3/$2.o" "$OUT/gs_runtime_asan.o" -o "$3/$2.asan" 2>"$3/$2.link.err"; then
            why="ASan link failed: $(cat "$3/$2.link.err")"; return
        fi
        asan=$("$3/$2.asan" 2>&1); asan_rc=$?
        if [ "$asan_rc" -eq 99 ] || printf '%s' "$asan" | grep -q 'Sanitizer'; then
            why="AddressSanitizer report (rc=$asan_rc):
$asan"; return
        fi
        if [ "$asan" != "$native" ] || [ "$asan_rc" -ne "$native_rc" ]; then
            why="ASan build behaves differently (rc=$asan_rc):
$asan"; return
        fi
    fi
}

# same_as_interpreter <gene> <actual> <rc>: appends to $why on mismatch
same_as_interpreter() {
    interp=$("$INTERP" "$1" 2>&1); interp_rc=$?
    if [ "$2" != "$interp" ] || [ "$3" -ne "$interp_rc" ]; then
        why="differs from the interpreter (backend/native rc=$3, interpreter rc=$interp_rc):
$(printf '%s\n' "$interp" > "$OUT/cmp.interp"; printf '%s\n' "$2" | diff "$OUT/cmp.interp" -)"
    fi
}

# ---- [runtime] ---------------------------------------------------------------
echo "== [runtime] C runtime unit test"
if [ -f tests/llvm/runtime_unit.c ]; then
    flags=""; [ -n "$ASAN" ] && flags="-fsanitize=address -fno-omit-frame-pointer -g"
    if ! "$CLANG" -std=c11 -Wall -Wextra -Werror $flags -Iruntime tests/llvm/runtime_unit.c \
            runtime/gs_runtime.c -o "$OUT/runtime_unit" 2>"$OUT/runtime_unit.err"; then
        bad runtime_unit "$(cat "$OUT/runtime_unit.err")"
    elif r=$("$OUT/runtime_unit" 2>&1); then
        ok "runtime_unit   ($r)"
    else
        bad runtime_unit "$r"
    fi
fi

# ---- [valid] -----------------------------------------------------------------
echo "== [valid] tests/llvm/*.gene  (native == expected == interpreter${ASAN:+, ASan clean})"
for t in tests/llvm/*.gene; do
    name=$(basename "$t" .gene)
    expected="tests/llvm/expected/$name.out"
    compile_and_run "$t" "$name" "$OUT"
    if [ -z "$why" ]; then
        if [ ! -f "$expected" ]; then
            why="missing $expected"
        elif [ "$native" != "$(tr -d '\r' < "$expected")" ]; then
            why="native output differs from $expected:
$(printf '%s\n' "$native" | diff "$expected" -)"
        else
            same_as_interpreter "$t" "$native" "$native_rc"
        fi
    fi
    [ -z "$why" ] && ok "$name   (exit $native_rc)" || bad "$name" "$why"
done

# ---- [errors] ----------------------------------------------------------------
echo "== [errors] tests/llvm/errors/*.gene  (rejected before IR, diagnostics == expected == interpreter)"
for t in tests/llvm/errors/*.gene; do
    name=$(basename "$t" .gene)
    ll="$OUT/errors/$name.ll"
    diag=$("$BACKEND" "$t" -o "$ll" 2>&1); rc=$?
    why=""
    if [ "$rc" -eq 0 ]; then
        why="backend accepted an invalid program"
    elif [ -e "$ll" ]; then
        why="backend failed but still wrote $ll"
    elif printf '%s' "$diag" | grep -q "$UNSUPPORTED"; then
        why="rejected for an unsupported construct, not the semantic error:
$diag"
    elif [ "$diag" != "$(tr -d '\r' < "tests/llvm/errors/$name.err")" ]; then
        why="diagnostics differ from errors/$name.err:
$(printf '%s\n' "$diag" | diff "tests/llvm/errors/$name.err" -)"
    else
        same_as_interpreter "$t" "$diag" "$rc"
    fi
    [ -z "$why" ] && ok "$name" || bad "$name" "$why"
done

# ---- [phase1] ----------------------------------------------------------------
echo "== [phase1] tests/*.gene through the backend  (must match the interpreter)"
for t in tests/*.gene; do
    name=$(basename "$t" .gene)
    diag=$("$BACKEND" "$t" -o "$OUT/phase1/$name.ll" 2>&1); rc=$?
    if [ "$rc" -ne 0 ] && printf '%s' "$diag" | grep -q "$UNSUPPORTED"; then
        skip=$((skip + 1))
        first=$(printf '%s\n' "$diag" | grep -m1 "$UNSUPPORTED" | sed "s/ $UNSUPPORTED\.//; s/^[^:]*:[0-9]*: //")
        skipped="$skipped
      $name: needs $first"
        continue
    fi
    if [ "$rc" -ne 0 ]; then
        why=""; same_as_interpreter "$t" "$diag" "$rc"
        [ -z "$why" ] && ok "$name   (compile-time error, same diagnostics)" || bad "$name" "$why"
        continue
    fi
    compile_and_run "$t" "$name" "$OUT/phase1"
    [ -z "$why" ] && same_as_interpreter "$t" "$native" "$native_rc"
    [ -z "$why" ] && ok "$name   (native, exit $native_rc)" || bad "$name" "$why"
done
if [ "$skip" -gt 0 ]; then
    echo "SKIP  $skip Phase 1 programs use constructs not lowered yet:$skipped"
fi

echo "------------------------------------------"
echo "$pass passed, $fail failed, $skip skipped"
[ "$fail" -eq 0 ]
