#!/bin/sh
# End-to-end tests for the genec command-line compiler (Phase 2, Part 4).
#
# [features]   tests/compiler/*.gene: `genec f.gene` (interpreter) and the
#              executable from `genec -c f.gene -o prog` must BOTH exit 0 and
#              BOTH print exactly tests/compiler/expected/<name>.out.  The
#              program is also built with AddressSanitizer and must behave the
#              same with no memory error or leak report.
# [runtime]    tests/compiler/runtime_errors/*.gene: both must exit 1 and print
#              exactly expected/<name>.out.
# [errors]     tests/compiler/errors/*.gene: `genec -c` and `genec -S` must exit
#              1, leave no executable and no .ll, and print exactly
#              errors/<name>.err, which must also be the interpreter's output.
# [regression] every tests/*.gene, tests/llvm/*.gene and tests/llvm/errors/*.gene
#              program: `genec` must match the Phase 1 interpreter exactly, and
#              the native executable must match both in output and exit status.
#              Programs with compile errors must produce no executable.
# [cli]        genec's options and failure handling.
#
# Usage: tests/compiler/run_compiler_tests.sh [genec] [phase1-interpreter]
# Env:   CLANG (default clang), GS_NO_ASAN=1 to skip sanitizer runs.

GENEC=${1:-./genec}
PHASE1=${2:-bin/genescript}
CLANG=${CLANG:-clang}
ROOT=$(pwd)
OUT="$ROOT/bin/compiler-tests"
case "$GENEC" in /*) ;; *) GENEC="$ROOT/$GENEC" ;; esac

if ! command -v "$CLANG" >/dev/null 2>&1; then
    echo "ERROR: '$CLANG' not found; install clang (e.g. sudo apt install clang) or set CLANG=..."
    exit 1
fi
[ -x "$GENEC" ] || { echo "ERROR: $GENEC not built (run make)"; exit 1; }

rm -rf "$OUT"
mkdir -p "$OUT/features" "$OUT/runtime" "$OUT/errors" "$OUT/regression" "$OUT/cli"

pass=0
fail=0
ok()  { echo "PASS  $1"; pass=$((pass + 1)); }
bad() { echo "FAIL  $1"; printf '%s\n' "$2" | sed 's/^/      /'; fail=$((fail + 1)); }
expected_text() { tr -d '\r' < "$1"; }

ASAN=1
[ -n "$GS_NO_ASAN" ] && ASAN=""
if [ -n "$ASAN" ] && ! "$CLANG" -std=c11 -g -O1 -fsanitize=address -fno-omit-frame-pointer \
        -c runtime/gs_runtime.c -o "$OUT/gs_runtime_asan.o" 2>/dev/null; then
    echo "note: AddressSanitizer unavailable; sanitizer runs skipped"; ASAN=""
fi
export ASAN_OPTIONS="exitcode=99:detect_leaks=1:abort_on_error=0"

# build_and_run <gene> <dir> <name>
# Sets: interp/interp_rc (genec interpreter), native/native_rc, why (non-empty on failure).
build_and_run() {
    why=""
    interp=$("$GENEC" "$1" 2>&1); interp_rc=$?
    exe="$2/$3"
    cdiag=$("$GENEC" -c "$1" -o "$exe" 2>&1); crc=$?
    if [ "$crc" -ne 0 ]; then
        why="genec -c failed (exit $crc):
$cdiag"; return
    fi
    if [ ! -x "$exe" ]; then
        why="genec -c exited 0 but wrote no executable"; return
    fi
    native=$("$exe" 2>&1); native_rc=$?
    if [ -n "$ASAN" ]; then
        if ! "$GENEC" -S "$1" -o "$exe.ll" 2>"$exe.err" ||
           ! "$CLANG" -g -fsanitize=address -Wno-override-module "$exe.ll" "$OUT/gs_runtime_asan.o" \
                -o "$exe.asan" 2>>"$exe.err"; then
            why="ASan build failed: $(cat "$exe.err")"; return
        fi
        asan=$("$exe.asan" 2>&1); asan_rc=$?
        if [ "$asan_rc" -eq 99 ] || printf '%s' "$asan" | grep -q 'Sanitizer'; then
            why="AddressSanitizer report (exit $asan_rc):
$asan"; return
        fi
        if [ "$asan" != "$native" ] || [ "$asan_rc" -ne "$native_rc" ]; then
            why="ASan build behaves differently (exit $asan_rc):
$asan"; return
        fi
    fi
}

# check_expected <label> <text> <rc> <expected-file> <expected-rc>
check_expected() {
    if [ "$3" -ne "$5" ]; then
        why="$1 exited $3, expected $5:
$2"
    elif [ "$2" != "$(expected_text "$4")" ]; then
        why="$1 output differs from $4:
$(printf '%s\n' "$2" | diff "$4" -)"
    fi
}

# ---- [features] ----------------------------------------------------------------
echo "== [features] tests/compiler/*.gene  (interpreter == native == expected, both exit 0${ASAN:+, ASan clean})"
for t in tests/compiler/*.gene; do
    name=$(basename "$t" .gene)
    expected="tests/compiler/expected/$name.out"
    build_and_run "$t" "$OUT/features" "$name"
    if [ -z "$why" ] && [ ! -f "$expected" ]; then why="missing $expected"; fi
    [ -z "$why" ] && check_expected interpreter "$interp" "$interp_rc" "$expected" 0
    [ -z "$why" ] && check_expected native "$native" "$native_rc" "$expected" 0
    [ -z "$why" ] && ok "$name" || bad "$name" "$why"
done

# ---- [runtime] -----------------------------------------------------------------
echo "== [runtime] tests/compiler/runtime_errors/*.gene  (both exit 1, same output)"
for t in tests/compiler/runtime_errors/*.gene; do
    name=$(basename "$t" .gene)
    expected="tests/compiler/runtime_errors/expected/$name.out"
    build_and_run "$t" "$OUT/runtime" "$name"
    if [ -z "$why" ] && [ ! -f "$expected" ]; then why="missing $expected"; fi
    [ -z "$why" ] && check_expected interpreter "$interp" "$interp_rc" "$expected" 1
    [ -z "$why" ] && check_expected native "$native" "$native_rc" "$expected" 1
    [ -z "$why" ] && ok "$name   (exit 1)" || bad "$name" "$why"
done

# ---- [errors] ------------------------------------------------------------------
echo "== [errors] tests/compiler/errors/*.gene  (rejected: exit 1, no executable, no .ll)"
for t in tests/compiler/errors/*.gene; do
    name=$(basename "$t" .gene)
    expected="tests/compiler/errors/$name.err"
    exe="$OUT/errors/$name"
    why=""
    cdiag=$("$GENEC" -c "$t" -o "$exe" 2>&1); crc=$?
    sdiag=$("$GENEC" -S "$t" -o "$exe.ll" 2>&1); src=$?
    idiag=$("$GENEC" "$t" 2>&1); irc=$?
    if [ ! -f "$expected" ]; then
        why="missing $expected"
    elif [ -e "$exe" ]; then
        why="genec -c produced an executable for an invalid program"
    elif [ -e "$exe.ll" ]; then
        why="genec -S wrote a .ll file for an invalid program"
    else
        check_expected "genec -c" "$cdiag" "$crc" "$expected" 1
        [ -z "$why" ] && check_expected "genec -S" "$sdiag" "$src" "$expected" 1
        [ -z "$why" ] && check_expected "genec (interpreter)" "$idiag" "$irc" "$expected" 1
    fi
    [ -z "$why" ] && ok "$name" || bad "$name" "$why"
done

# ---- [regression] --------------------------------------------------------------
echo "== [regression] Phase 1 and LLVM test programs through genec  (native == interpreter)"
for t in tests/*.gene tests/llvm/*.gene tests/llvm/errors/*.gene; do
    name=$(echo "$t" | sed 's|^tests/||; s|\.gene$||; s|/|_|g')
    why=""
    phase1=$("$PHASE1" "$t" 2>&1); phase1_rc=$?
    interp=$("$GENEC" "$t" 2>&1); interp_rc=$?
    if [ "$interp" != "$phase1" ] || [ "$interp_rc" -ne "$phase1_rc" ]; then
        bad "$name" "genec (exit $interp_rc) differs from the Phase 1 interpreter (exit $phase1_rc)"
        continue
    fi
    if printf '%s' "$interp" | grep -q '^Compilation stopped:'; then
        exe="$OUT/regression/$name"
        cdiag=$("$GENEC" -c "$t" -o "$exe" 2>&1); crc=$?
        if [ "$crc" -ne 1 ] || [ -e "$exe" ]; then
            why="invalid program: genec -c exited $crc$([ -e "$exe" ] && echo ' and wrote an executable')"
        elif [ "$cdiag" != "$interp" ]; then
            why="compile diagnostics differ from the interpreter's:
$(printf '%s\n' "$interp" > "$OUT/cmp"; printf '%s\n' "$cdiag" | diff "$OUT/cmp" -)"
        fi
        [ -z "$why" ] && ok "$name   (rejected, same diagnostics)" || bad "$name" "$why"
        continue
    fi
    build_and_run "$t" "$OUT/regression" "$name"
    if [ -z "$why" ] && { [ "$native" != "$interp" ] || [ "$native_rc" -ne "$interp_rc" ]; }; then
        why="native (exit $native_rc) differs from the interpreter (exit $interp_rc):
$(printf '%s\n' "$interp" > "$OUT/cmp"; printf '%s\n' "$native" | diff "$OUT/cmp" -)"
    fi
    [ -z "$why" ] && ok "$name   (exit $native_rc)" || bad "$name" "$why"
done

# ---- [cli] ---------------------------------------------------------------------
echo "== [cli] genec options and failure handling"
SAMPLE="$ROOT/tests/sample1_filter.gene"
SAMPLE_OUT="GGCCG
TACAATGGCCAT
AUGGCCAUUGUA"
cd "$OUT/cli" || exit 1

# check_cli <name> <expected rc> <text that must appear in output (or "")> <command...>
check_cli() {
    cname=$1; want_rc=$2; want_text=$3; shift 3
    cout=$("$@" 2>&1); crc=$?
    if [ "$crc" -ne "$want_rc" ]; then
        bad "$cname" "exit $crc, expected $want_rc:
$cout"
    elif [ -n "$want_text" ] && ! printf '%s' "$cout" | grep -qF -e "$want_text"; then
        bad "$cname" "output does not mention '$want_text':
$cout"
    else
        ok "$cname"
    fi
}

check_cli "interpret: genec file.gene" 0 "TACAATGGCCAT" "$GENEC" "$SAMPLE"

rm -f sample1_filter.ll
check_cli "-S writes <name>.ll by default" 0 "" "$GENEC" -S "$SAMPLE"
[ -s sample1_filter.ll ] && grep -q 'define ptr @gs_gene_nucleus' sample1_filter.ll &&
    ok "-S output is LLVM IR (sample1_filter.ll)" || bad "-S output is LLVM IR" "sample1_filter.ll missing or not IR"

check_cli "-S -o custom.ll" 0 "" "$GENEC" -S "$SAMPLE" -o custom.ll
[ -s custom.ll ] && ok "-S -o wrote custom.ll" || bad "-S -o wrote custom.ll" "custom.ll missing"

rm -f sample1_filter
check_cli "-c writes <name> by default" 0 "" "$GENEC" -c "$SAMPLE"
if [ -x sample1_filter ] && [ "$(./sample1_filter)" = "$SAMPLE_OUT" ]; then
    ok "-c default executable runs (./sample1_filter)"
else
    bad "-c default executable runs" "missing or wrong output"
fi
check_cli "-c -o prog" 0 "" "$GENEC" -c "$SAMPLE" -o prog
[ -x prog ] && [ "$(./prog)" = "$SAMPLE_OUT" ] && ok "-c -o executable runs (./prog)" ||
    bad "-c -o executable runs" "prog missing or wrong output"
ls ./*.ll 2>/dev/null | grep -qv -e custom.ll -e sample1_filter.ll &&
    bad "-c leaves no .ll behind" "$(ls ./*.ll)" || ok "-c leaves no .ll behind"

check_cli "missing source file" 1 "Cannot open file" "$GENEC" -c nosuchfile.gene -o never
[ -e never ] && bad "missing source: no executable" "never was created" || ok "missing source: no executable"
check_cli "no arguments" 1 "Usage" "$GENEC"
check_cli "unknown option" 1 "unknown option" "$GENEC" -x "$SAMPLE"
check_cli "-S and -c together" 1 "cannot be used together" "$GENEC" -S -c "$SAMPLE"
check_cli "-o without -S/-c" 1 "-o needs -S or -c" "$GENEC" "$SAMPLE" -o prog2
check_cli "-o without a name" 1 "-o needs a file name" "$GENEC" -c "$SAMPLE" -o
check_cli "two source files" 1 "only one source file" "$GENEC" -c "$SAMPLE" "$SAMPLE"

check_cli "clang not installed" 1 "could not run" env CLANG=/nonexistent/clang "$GENEC" -c "$SAMPLE" -o noclang
[ -e noclang ] && bad "clang not installed: no executable" "noclang was created" ||
    ok "clang not installed: no executable"
check_cli "clang fails" 1 "failed (exit 1)" env CLANG=false "$GENEC" -c "$SAMPLE" -o clangfail
[ -e clangfail ] && bad "clang fails: no executable" "clangfail was created" ||
    ok "clang fails: no executable"
check_cli "runtime not found" 1 "GS_RUNTIME" env GS_RUNTIME=/nonexistent/gs_runtime.c "$GENEC" -c "$SAMPLE" -o nort
cd "$ROOT" || exit 1

echo "------------------------------------------"
echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ]
