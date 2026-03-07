#!/usr/bin/env bash
set -euo pipefail

sudo rmmod scthrottle 2>/dev/null || true
sudo rmmod the_usctm 2>/dev/null || true

echo "[+] Unloaded (if possible)."
lsmod | egrep '^(the_usctm|scthrottle)\b' || true