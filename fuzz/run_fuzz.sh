#!/usr/bin/env bash
# ─────────────────────────────────────────────────────────────────────────────
# Build + run every libFuzzer harness in fuzz/ under ASan + UBSan.
#
# Usage:
#   CXX=clang++ TIME=20 fuzz/run_fuzz.sh          # 20s per target (default 20)
#   CXX=/opt/homebrew/opt/llvm/bin/clang++ fuzz/run_fuzz.sh   # macOS (brew LLVM)
#
# Requires a clang with libFuzzer (Apple clang does NOT ship it — use Homebrew
# LLVM on macOS; Linux clang has it). Exits non-zero if any target crashes,
# times out (per-input >5s = likely a parser hang), or trips a sanitizer.
# ─────────────────────────────────────────────────────────────────────────────
set -uo pipefail
cd "$(dirname "$0")/.."

CXX="${CXX:-clang++}"
TIME="${TIME:-20}"
SAN="-fsanitize=fuzzer,address,undefined -fno-sanitize-recover=undefined"
# -D__cpp_concepts=202002L: clang + libstdc++ reports 201907L, but libstdc++'s
# <expected> requires >= 202002L (the C++20 final value). Harmless on libc++.
COMMON="-std=c++23 -g -O1 -Iinclude -D__cpp_concepts=202002L -Wno-builtin-macro-redefined"
OUT="$(mktemp -d)"; trap 'rm -rf "$OUT"' EXIT

# Verify libFuzzer is available before doing anything.
printf 'extern "C" int LLVMFuzzerTestOneInput(const unsigned char*d,unsigned long n){(void)d;(void)n;return 0;}\n' > "$OUT/probe.cpp"
if ! $CXX $COMMON $SAN "$OUT/probe.cpp" -o "$OUT/probe" 2>"$OUT/probe.err"; then
  echo "ERROR: '$CXX' lacks libFuzzer (-fsanitize=fuzzer). On macOS use Homebrew LLVM:"
  echo "  CXX=/opt/homebrew/opt/llvm/bin/clang++ fuzz/run_fuzz.sh"
  exit 2
fi

# target name -> extra source files (http parser needs its .cpp implementation).
extra_src() {
  case "$1" in
    http_parser) echo "src/http/parser.cpp src/http/request.cpp src/http/response.cpp" ;;
    *) echo "" ;;
  esac
}

fail=0
for h in fuzz/fuzz_*.cpp; do
  name="$(basename "$h" .cpp | sed 's/^fuzz_//')"
  bin="$OUT/fz_$name"
  if ! $CXX $COMMON $SAN "$h" $(extra_src "$name") -o "$bin" 2>"$OUT/${name}.cc"; then
    echo "  ✗ $name: build failed"; sed 's/^/      /' "$OUT/${name}.cc" | grep -m2 error: || true
    fail=$((fail+1)); continue
  fi
  ASAN_OPTIONS=detect_leaks=0 "$bin" -max_total_time="$TIME" -timeout=5 -rss_limit_mb=2048 \
      -print_final_stats=1 > "$OUT/${name}.run" 2>&1
  rc=$?
  bug="$(grep -oE 'ERROR: AddressSanitizer: [a-z-]+|ERROR: libFuzzer: (timeout|deadly signal|out-of-memory)|runtime error:' "$OUT/${name}.run" | head -1)"
  cov="$(grep -oE 'cov: [0-9]+' "$OUT/${name}.run" | tail -1)"
  if [ "$rc" = 0 ] && [ -z "$bug" ]; then
    echo "  ✓ $name  ($cov)"
  else
    echo "  ✗ $name  rc=$rc  ${bug:-unknown}"
    # surface any crash/timeout reproducer libFuzzer wrote
    for art in crash-* timeout-* oom-*; do [ -f "$art" ] && echo "      reproducer: $art"; done
    fail=$((fail+1))
  fi
done

echo "fuzz: $fail target(s) failed."
[ "$fail" -eq 0 ]
