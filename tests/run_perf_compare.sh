#!/usr/bin/env bash
set -euo pipefail
export LC_ALL=C

THREADS="${THREADS:-300}"
CALLS="${CALLS:-10}"
MAXTOK="${MAXTOK:-200}"
RUNS="${RUNS:-1}"

OUTCSV="${OUTCSV:-perf_compare.csv}"
BENCH="./bench_uname"

if [[ ! -x "$BENCH" ]]; then
  echo "bench_uname non trovato. Esegui: make bench_uname"
  exit 1
fi

cleanup_config() {
  sudo ./../user/scthctl off || true
  sudo ./../user/scthctl resetstats || true

  sudo ./../user/scthctl delprog bench_uname 2>/dev/null || true
  sudo ./../user/scthctl delprog uname 2>/dev/null || true
  sudo ./../user/scthctl delprog python3 2>/dev/null || true

  sudo ./../user/scthctl delsys 63 2>/dev/null || true
  sudo ./../user/scthctl delsys 39 2>/dev/null || true

  # svuota tutti gli uid configurati
  local uid_list
  uid_list="$(sudo ./../user/scthctl listuid || true)"

  while read -r line; do
    if [[ "$line" =~ ^[[:space:]]*([0-9]+)$ ]]; then
      sudo ./../user/scthctl deluid "${BASH_REMATCH[1]}" 2>/dev/null || true
    fi
  done <<< "$uid_list"
}

setup_policy() {
  local policy="$1"

  cleanup_config

  sudo ./../user/scthctl setmax "$MAXTOK"
  sudo ./../user/scthctl setpolicy "$policy"
  sudo ./../user/scthctl on

  sudo ./../user/scthctl addsys 63
  sudo ./../user/scthctl addprog bench_uname
}

perf_extract() {
  local file="$1"
  local event="$2"

  sudo awk -F, -v ev="$event" '
    $3 == ev {
      gsub(/[[:space:]]/, "", $1);
      print $1;
    }
  ' "$file" | tail -n1
}

normalize_metric() {
  local v="${1:-}"
  if [[ -z "$v" ]]; then
    echo ""
    return
  fi

  case "$v" in
    "<notsupported>"|"<notcounted>"|"<not supported>"|"<not counted>")
      echo ""
      ;;
    *)
      echo "$v"
      ;;
  esac
}

run_one() {
  local policy_name="$1"
  local policy_num="$2"
  local run_id="$3"

  local perf_out="/tmp/perf_${policy_name}_${run_id}_$$.csv"
  sudo rm -f "$perf_out"

  echo
  echo "[RUN] policy=$policy_name run=$run_id threads=$THREADS calls=$CALLS max=$MAXTOK"

  setup_policy "$policy_num"

  sudo perf stat \
    -x, \
    -e cycles,task-clock,context-switches \
    -o "$perf_out" \
    -- "$BENCH" "$THREADS" "$CALLS"

  local cycles_raw
  local task_clock_raw
  local csw_raw

  cycles_raw="$(perf_extract "$perf_out" "cycles")"
  task_clock_raw="$(perf_extract "$perf_out" "task-clock")"
  csw_raw="$(perf_extract "$perf_out" "context-switches")"

  local cycles
  local task_clock
  local csw

  cycles="$(normalize_metric "$cycles_raw")"
  task_clock="$(normalize_metric "$task_clock_raw")"
  csw="$(normalize_metric "$csw_raw")"

  if [[ -z "$task_clock" || -z "$csw" ]]; then
    echo "Errore nel parsing delle metriche perf:"
    sudo cat "$perf_out"
    sudo rm -f "$perf_out"
    exit 1
  fi

  echo "$policy_name,$run_id,$THREADS,$CALLS,$MAXTOK,$cycles,$task_clock,$csw" >> "$OUTCSV"

  sudo rm -f "$perf_out"
}

echo "policy,run,threads,calls,max_per_epoch,cycles,task_clock_ms,context_switches" > "$OUTCSV"

for r in $(seq 1 "$RUNS"); do
  run_one FIFO 0 "$r"
done

for r in $(seq 1 "$RUNS"); do
  run_one WAKE_RACE 1 "$r"
done

echo
echo "[DONE] risultati salvati in $OUTCSV"