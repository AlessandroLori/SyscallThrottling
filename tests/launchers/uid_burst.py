import os
import time
import subprocess
import sys

# Prima aggiungere al meccanismo ciò che si vuole throttlare
'''
sudo ../../user/scthctl off
sudo ../../user/scthctl resetstats
sudo ../../user/scthctl addsys 63
sudo ../../user/scthctl adduid 1000
sudo ../../user/scthctl setmax 3
sudo ../../user/scthctl setpolicy 0
sudo ../../user/scthctl on
sudo python3 uid_burst.py
sudo ../../user/scthctl stats
'''

# =========================
# VALORI EDITABILI
# =========================
TARGET_UID = 1000   # UID deve matchare adduid
CMD = ["uname"]     # 
N = 20              # quanti processi concorrenti per ondata
DUR = 10            # durata del while, se ancora nel ciclo viene lanciata un'altra ondata di thread
STDOUT_NULL = True
STDERR_NULL = True
# =========================

if os.geteuid() != 0:
    print("Errore: questo script va lanciato con sudo/root, perché usa setuid/setgid sui figli.")
    sys.exit(1)

out = subprocess.DEVNULL if STDOUT_NULL else None
err = subprocess.DEVNULL if STDERR_NULL else None

def demote():
    # imposta gid/uid del figlio prima della exec
    os.setgid(TARGET_UID)
    os.setuid(TARGET_UID)

print(f"[UID TEST] TARGET_UID={TARGET_UID}, CMD={CMD}, N={N}, DUR={DUR}s")
print()

t_end = time.time() + DUR
rounds = 0
spawned = 0

while time.time() < t_end:
    ps = [
        subprocess.Popen(
            CMD,
            stdout=out,
            stderr=err,
            preexec_fn=demote
        )
        for _ in range(N)
    ]
    spawned += len(ps)
    rounds += 1

    for p in ps:
        p.wait()

print(f"done: rounds={rounds}, total_processes={spawned}")