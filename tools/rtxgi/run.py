"""Lance le path tracer d'exemple du SDK RTXGI de NVIDIA (branche krc) sur un scénario, sans
fenêtre visible ni prise de focus, et attend la fin.

  python -I tools/rtxgi/run.py --out F:/rtxgi/out/essai --script "10:tech=ref;10:den=acc;300:capture=ref;301:exit"
"""

import argparse
import ctypes
import os
import subprocess
import time
from ctypes import wintypes

EXE = 'F:/rtxgi/sdk/Bin/x64/Pathtracer.exe'


def run(out, script, extra_args=(), timeout=1800, width=1280, height=720, visible=False):
    os.makedirs(out, exist_ok=True)
    env = dict(os.environ, KRC_OUT=out, KRC_SCRIPT=script, KRC_HIDDEN='0' if visible else '1')
    args = [EXE, '-width', str(width), '-height', str(height)] + list(extra_args)
    p = subprocess.Popen(args, cwd=os.path.dirname(EXE), env=env)
    user32 = ctypes.windll.user32
    took_focus = False
    t0 = time.time()
    while p.poll() is None:
        if time.time() - t0 > timeout:
            p.kill()
            print('délai dépassé')
            break
        pid = wintypes.DWORD()
        user32.GetWindowThreadProcessId(user32.GetForegroundWindow(), ctypes.byref(pid))
        took_focus |= pid.value == p.pid
        time.sleep(0.2)
    return {'code': p.returncode, 'secondes': round(time.time() - t0, 1), 'focus_pris': took_focus}


if __name__ == '__main__':
    ap = argparse.ArgumentParser()
    ap.add_argument('--out', required=True)
    ap.add_argument('--script', required=True)
    ap.add_argument('--visible', action='store_true')
    a, rest = ap.parse_known_args()
    print(run(a.out, a.script, rest, visible=a.visible))
