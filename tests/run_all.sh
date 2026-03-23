#!/usr/bin/env bash
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"

run() {
    if [[ ${EUID:-$(id -u)} -eq 0 ]]; then
        "$@"
    else
        sudo "$@"
    fi
}

echo "[*] reload modules"
run "$ROOT/scripts/unload_all.sh"
run "$ROOT/scripts/load_all.sh"

echo "[*] build tests"
make -C "$HERE" clean all

tests=(
  t01_no_match
  t02_immediate_only
  t03_fifo_burst
  t04_wakerace_burst
  t05_abort_off
  t06_dynamic_setmax
  t07_multisyscall
  t08_permissions
  t09_dup_config
  t10_policy_transition
  t11_nonroot_uid_match
  t12_root_uid_nomatch
  t13_setmax_invalid
  t15_wakerace_to_fifo
  t14_unload_during_wait
)

fails=0
for t in "${tests[@]}"; do
    echo
    echo "==================== $t ===================="
    if run "$HERE/$t"; then
        echo "[OK] $t"
    else
        echo "[ERR] $t"
        fails=$((fails+1))
    fi
done

echo
if [[ $fails -eq 0 ]]; then
    echo "[SUMMARY] all tests passed"
else
    echo "[SUMMARY] failures: $fails"
fi

exit "$fails"
