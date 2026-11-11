#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Host-only design of the half-band of extras/tract (src/halfband.cpp): minimax (equiripple) by linear programming.

A half-band of 4*M - 1 taps: h[c] = 0.5 at the centre c = 2*M - 1, h[c +- (2m+1)] = a[m] for m = 0..M-1, every other tap 0, so that
H(w) + H(pi - w) = 1.  Pass edge 0.2 fs, stop edge 0.3 fs (fs: the rate the filter runs at).  M = 4 (15 taps) reaches only 32.5 dB of
rejection at these edges; the plan's 45 dB needs M = 6 (23 taps), which is what halfband.cpp uses.  The result is frozen as constants
in src/halfband.cpp; this script is not part of any build (numpy and scipy).
usage: python3 design_halfband.py [M]
"""
import sys
import numpy as np
from scipy.optimize import linprog

M = int(sys.argv[1]) if len(sys.argv) > 1 else 6
pe, se = 0.2, 0.3
fp = np.linspace(0, pe, 1500) * 2 * np.pi
fs_ = np.linspace(se, 0.5, 3000) * 2 * np.pi
def row(w):
    return np.array([2 * np.cos((2 * m + 1) * w) for m in range(M)])
A, b = [], []
for w in fp:      # |H - 1| <= d
    r = row(w)
    A.append(list(r) + [-1.0]); b.append(0.5)
    A.append(list(-r) + [-1.0]); b.append(-0.5)
for w in fs_:     # |H| <= d
    r = row(w)
    A.append(list(r) + [-1.0]); b.append(-0.5)
    A.append(list(-r) + [-1.0]); b.append(0.5)
res = linprog([0.0] * M + [1.0], A_ub=A, b_ub=b, bounds=[(None, None)] * M + [(0, None)])
a = res.x[:M]
print(f"M = {M}, {4*M-1} taps; a[m] at offsets 1, 3, 5, ...:")
print("    " + ", ".join(f"{x:.9g}f" for x in a))
# check on a much denser grid
w = np.linspace(0, np.pi, 200001)
H = 0.5 + sum(2 * a[m] * np.cos((2 * m + 1) * w) for m in range(M))
pr = H[w <= pe * 2 * np.pi]
st = H[w >= se * 2 * np.pi]
print(f"ripple below {pe} fs: {20*np.log10(pr.max()):+.4f} / {20*np.log10(pr.min()):+.4f} dB; rejection above {se} fs: "
      f"{-20*np.log10(np.abs(st).max()):.2f} dB")
