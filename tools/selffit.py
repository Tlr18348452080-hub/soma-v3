#!/usr/bin/env python3
"""在自身数据上拟合 58 维脊髓矩阵，导出 assets 用的 JSON（带截距 b）。"""
import numpy as np, sys, json
from self_train import load, prep, feats

SRC = '/tmp/selfseq_big.txt'
eps = load(SRC); chunks = prep(eps)
n = len(chunks); cut = int(n * 0.8); tr, va = chunks[:cut], chunks[cut:]


def build(ch, with_b=True):
    X = np.concatenate([feats(q, dq, ph, u)[:-1] for q, dq, ph, u in ch])
    if with_b:
        X = np.concatenate([X, np.ones((len(X), 1))], 1)
    Y = np.concatenate([(q[1:] - q[:-1]) for q, dq, ph, u in ch])
    return X, Y


X, Y = build(tr)
XtX = X.T @ X
lam = 1e-4 * np.trace(XtX) / X.shape[1]
W = np.linalg.solve(XtX + lam * np.eye(X.shape[1]), X.T @ Y)
Xv, Yv = build(va)
ss = np.degrees(np.abs(Xv @ W - Yv))


def roll(ch, H):
    errs = []
    for q, dq, ph, u in ch:
        st = np.arange(0, len(q) - H - 1, 3); qq = q[st].copy(); dd = dq[st].copy()
        for h in range(H):
            x = np.concatenate([feats(qq, dd, ph[st + h], u[st + h]), np.ones((len(qq), 1))], 1)
            dl = x @ W; dd = dl * 60.0; qq = qq + dl
        errs.append(np.degrees(np.abs(qq - q[st + H])).mean(1))
    e = np.concatenate(errs); return float(e.mean()), float(np.median(e))


r6 = roll(va, 6); r24 = roll(va, 24)
print('带截距: 单步 均值%.2f 中位%.2f | 6步 %.2f/%.2f | 24步 %.2f/%.2f' % (ss.mean(), np.median(ss), r6[0], r6[1], r24[0], r24[1]))
out = {"D": 10, "DX": 58, "rmseDeg": round(r6[0], 2), "rollDeg": round(r24[0], 2),
       "ticks": int(sum(len(c[0]) for c in tr)), "src": "self-seq soma3d",
       "A": W[:-1].T.tolist(), "b": W[-1].tolist()}
json.dump(out, open('/tmp/spinal_new.json', 'w'))
print('→ /tmp/spinal_new.json')
