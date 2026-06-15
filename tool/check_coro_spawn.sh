#!/usr/bin/env bash
# ─────────────────────────────────────────────────────────────────────────────
# Guard: no CAPTURING immediately-invoked coroutine-lambda passed to spawn().
#
# The pattern  dispatcher.spawn([&]() -> Task<...> { ...co_await...; }())  is
# undefined behaviour: the trailing () invokes the lambda immediately, producing a
# coroutine whose frame references the temporary closure object — which is
# destroyed at the end of the spawn() statement while a worker thread is still
# running the coroutine (stack/heap use-after-scope). A CAPTURELESS  spawn([]...)
# is fine (no state to dangle); the fix for a capturing one is a free-function
# coroutine that owns its state by value.
#
# This locks in the 2026-06-15 fix of ~13 examples that crashed on this exact bug.
# ─────────────────────────────────────────────────────────────────────────────
set -uo pipefail
cd "$(dirname "$0")/.."

# Match .spawn( followed by '[' and a non-']' char == a non-empty capture list.
hits="$(grep -rEn '\.spawn\(\[[^]]' include/ src/ examples/ tests/ 2>/dev/null || true)"

if [ -n "$hits" ]; then
  echo "ERROR: capturing coroutine-lambda passed to spawn() (use-after-scope risk):"
  echo "$hits"
  echo
  echo "Fix: use a free-function coroutine that takes its state BY VALUE, or a"
  echo "captureless []() lambda. See docs/audit/2026-06-15_io-pipeline-connectivity.md."
  exit 1
fi
echo "OK: no capturing immediately-invoked coroutine-lambda spawns."
