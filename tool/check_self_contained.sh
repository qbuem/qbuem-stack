#!/usr/bin/env bash
# ─────────────────────────────────────────────────────────────────────────────
# Guard: every public header compiles when included on its OWN, so any module can
# be composed independently without relying on a particular include order.
#
# For each include/qbuem/**/*.hpp it compiles a TU that includes only that header
# (-fsyntax-only). Catches headers that silently rely on a transitive include.
#
# Run on macOS (Apple clang) where the full set is platform-applicable; on Linux
# a few macOS-only headers (e.g. core/kqueue_reactor.hpp pulls <sys/event.h>) are
# skipped via the DENYLIST below. The 4-platform build+ctest covers the rest.
# ─────────────────────────────────────────────────────────────────────────────
set -uo pipefail
cd "$(dirname "$0")/.."

CXX="${CXX:-c++}"

# Headers that only build on a specific OS (pull OS-only system headers directly).
DENY_LINUX="core/kqueue_reactor.hpp"     # <sys/event.h>
DENY_MAC=""                              # (none currently — all guarded)

uname_s="$(uname -s)"
deny=""
[ "$uname_s" = "Linux" ]  && deny="$DENY_LINUX"
[ "$uname_s" = "Darwin" ] && deny="$DENY_MAC"

tmp="$(mktemp -d)"; trap 'rm -rf "$tmp"' EXIT
fail=0; n=0; skipped=0
for h in $(find include/qbuem -name '*.hpp' | sort); do
  rel="${h#include/}"               # qbuem/....hpp
  case " $deny " in *" ${rel#qbuem/} "*) skipped=$((skipped+1)); continue;; esac
  n=$((n+1))
  printf '#include <%s>\nint main(){return 0;}\n' "$rel" > "$tmp/probe.cpp"
  if ! "$CXX" -std=c++23 -Iinclude -fsyntax-only "$tmp/probe.cpp" 2>"$tmp/err.txt"; then
    echo "NOT self-contained: $rel"
    grep -m1 'error:' "$tmp/err.txt" | sed 's/^/    /' || true
    fail=$((fail+1))
  fi
done

echo "Checked $n headers ($skipped skipped for this OS); $fail not self-contained."
[ "$fail" -eq 0 ]
