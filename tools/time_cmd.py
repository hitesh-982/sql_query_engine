import sys
import subprocess
import time
import resource

if len(sys.argv) < 2:
    sys.exit(1)

t0 = time.time()
p = subprocess.run(sys.argv[1:])
t1 = time.time()

usage = resource.getrusage(resource.RUSAGE_CHILDREN)
rss_kb = usage.ru_maxrss
if sys.platform == "darwin":
    rss_kb //= 1024

sys.stderr.write(f"Elapsed: {t1 - t0:.3f} s | Peak RSS: {rss_kb / 1024:.2f} MB\n")
sys.exit(p.returncode)

