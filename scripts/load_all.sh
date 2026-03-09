#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
USCTM_DIR="$ROOT/external/Linux-sys_call_table-discoverer-master"
KMOD_DIR="$ROOT/kernel"
KBUILD="/lib/modules/$(uname -r)/build"

echo "[*] Build USCTM (direct Kbuild)..."
make -C "$KBUILD" M="$USCTM_DIR" clean
make -C "$KBUILD" M="$USCTM_DIR" modules

echo "[*] Build scthrottle..."
make -C "$KBUILD" M="$KMOD_DIR" modules

echo "[*] Load USCTM..."
if lsmod | grep -q '^the_usctm\b'; then
  echo "    - already loaded"
else
  sudo insmod "$USCTM_DIR/the_usctm.ko"
fi

echo "[*] Read sys_call_table_address..."
SYS_ADDR="$(sudo cat /sys/module/the_usctm/parameters/sys_call_table_address)"
if [[ -z "$SYS_ADDR" || "$SYS_ADDR" == "0" || "$SYS_ADDR" == "0x0" ]]; then
  echo "[-] sys_call_table_address is empty/0. Check: sudo dmesg | grep USCTM"
  exit 1
fi

HEX_ADDR="$(python3 - <<PY
v=int("$SYS_ADDR")
print(hex(v))
PY
)"
echo "[+] sys_call_table_address(dec)=$SYS_ADDR hex=$HEX_ADDR"

echo "[*] Reload scthrottle with sys_call_table_addr..."
if lsmod | grep -q '^scthrottle\b'; then
  sudo pkill -9 scthctl 2>/dev/null || true
  sudo fuser -k /dev/scthrottle 2>/dev/null || true

  if ! sudo rmmod scthrottle; then
    echo "[-] Cannot rmmod scthrottle (still in use). Aborting."
    sudo fuser -v /dev/scthrottle || true
    exit 1
  fi
fi

sudo insmod "$KMOD_DIR/scthrottle.ko" sys_call_table_addr="$SYS_ADDR"

echo "[*] Done. Loaded modules:"
lsmod | egrep '^(the_usctm|scthrottle)\b' || true

echo "[*] scthrottle log tail:"
sudo dmesg | grep scthrottle | tail -n 10 || true