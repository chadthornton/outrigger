#!/usr/bin/env python3
"""compare.py A.f32 B.f32: two renders (interleaved stereo float32) -> max abs difference, difference RMS in dB below
A's RMS, and whether they're bit-identical."""
import sys, array, math
def load(p):
    a = array.array('f'); a.frombytes(open(p, 'rb').read()); return a
a, b = load(sys.argv[1]), load(sys.argv[2])
n = min(len(a), len(b))
same = a[:n] == b[:n] and len(a) == len(b)
mx = 0.0; sd = 0.0; sa = 0.0
for i in range(n):
    d = abs(a[i] - b[i]); mx = max(mx, d); sd += d * d; sa += a[i] * a[i]
db = 10 * math.log10(sd / sa) if sd > 0 and sa > 0 else float('-inf')
print(f'{{"frames":{n//2},"identical":{str(same).lower()},"max_abs_diff":{mx:.3g},"diff_db_below_signal":{db:.1f}}}')
