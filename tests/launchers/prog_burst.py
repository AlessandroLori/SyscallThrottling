import time
import subprocess

# Prima aggiungere al meccanismo ciò che si vuole throttlare
'''
sudo ../../user/scthctl resetstats
sudo ../../user/scthctl addsys 63
sudo ../../user/scthctl addprog uname
sudo ../../user/scthctl setmax 3
sudo ../../user/scthctl setpolicy 0
sudo ../../user/scthctl on
python3 prog_burst.py
'''

# =========================
# VALORI EDITABILI
# =========================
CMD = ["uname"]   # esempio: ["uname"], ["id"], ["date"], ["whoami"], ["python3", "-c", "import os; os.getpid()"]
N = 20            # processi per ondata
DUR = 10          # durata del while, se ancora nel ciclo viene lanciata un'altra ondata di thread
STDOUT_NULL = True
STDERR_NULL = True
# =========================

out = subprocess.DEVNULL if STDOUT_NULL else None
err = subprocess.DEVNULL if STDERR_NULL else None

print(f"[PROGRAM-NAME TEST] CMD={CMD}, N={N}, DUR={DUR}s")
print()

t_end = time.time() + DUR
rounds = 0
spawned = 0

while time.time() < t_end:
    ps = [subprocess.Popen(CMD, stdout=out, stderr=err) for _ in range(N)]
    spawned += len(ps)
    rounds += 1

    for p in ps:
        p.wait()

print(f"done: rounds={rounds}, total_processes={spawned}")