#!/usr/bin/env python3
"""在她自己的数据上试特征集：哪些真的能让 6/24 步更准。"""
import numpy as np, sys
sys.path.insert(0, '.')
from self_train import load, prep, Ridge
TAU = 2 * np.pi


def mk(kind):
    def f(q, dq, ph, u):
        s = {k: np.sin(k * TAU * ph)[:, None] for k in (1, 2, 3, 4)}
        c = {k: np.cos(k * TAU * ph)[:, None] for k in (1, 2, 3, 4)}
        base = [q, dq, s[1], c[1], s[2], c[2], s[3], c[3], q * s[1], q * c[1], u, u * s[1], u * c[1]]
        if kind == 'F2':      # 双频刚度
            base += [q * s[2], q * c[2]]
        elif kind == 'F3':    # 双频 + 速度×相位
            base += [q * s[2], q * c[2], dq * s[1], dq * c[1]]
        elif kind == 'F4':    # 再加 4 次谐波
            base += [q * s[2], q * c[2], dq * s[1], dq * c[1], q * s[3], q * c[3], s[4], c[4]]
        return np.concatenate(base, 1)
    return f


def fit(chunks, f, lam=1e-4):
    X = np.concatenate([f(q, dq, ph, u)[:-1] for q, dq, ph, u in chunks])
    Y = np.concatenate([(q[1:] - q[:-1]) for q, dq, ph, u in chunks])
    XtX = X.T @ X
    l = lam * np.trace(XtX) / XtX.shape[0]
    return np.linalg.solve(XtX + l * np.eye(XtX.shape[0]), X.T @ Y), X.shape[1]


def roll(W, f, chunks, H):
    errs = []
    for q, dq, ph, u in chunks:
        if len(q) < H + 2: continue
        st = np.arange(0, len(q) - H - 1, 3)
        qq = q[st].copy(); dd = dq[st].copy()
        for h in range(H):
            dl = f(qq, dd, ph[st + h], u[st + h]) @ W
            dd = dl * 60.0; qq = qq + dl
        errs.append(np.degrees(np.abs(qq - q[st + H])).mean(1))
    e = np.concatenate(errs)
    return e.mean(), np.median(e)


eps = load('/tmp/selfseq_big.txt'); chunks = prep(eps)
n = len(chunks); cut = int(n * 0.8); tr, va = chunks[:cut], chunks[cut:]
print('%-4s %4s %10s %16s %16s' % ('set', 'dim', '单步中位', '6步 均值/中位', '24步 均值/中位'))
for k in ('F1', 'F2', 'F3', 'F4'):
    f = mk(k)
    W, dim = fit(tr, f)
    X = np.concatenate([f(q, dq, ph, u)[:-1] for q, dq, ph, u in va])
    Y = np.concatenate([(q[1:] - q[:-1]) for q, dq, ph, u in va])
    ss = np.degrees(np.abs(X @ W - Y)); r6 = roll(W, f, va, 6); r24 = roll(W, f, va, 24)
    print('%-4s %4d %10.2f %16s %16s' % (k, dim, np.median(ss),
          '%.2f/%.2f' % r6, '%.2f/%.2f' % r24), flush=True)
