#!/usr/bin/env python3
"""脊髓层模型对比（诚实版）。

两条纪律：
1. 禁止用前向差分当特征 —— CSV 里的 dq=(q_next-q)*60 就是训练目标本身，
   拿它当特征单步误差会"降到 0.04°"，那是背答案。运行时喂的是后向差分。
2. 相位当"给定的钟" —— 运行时相位来自步态振荡器，所以评估时也用真值相位，
   只是不给关节角。这样量的是"关节动力学学得像不像"。
"""
import numpy as np, glob, os, time
TAU = 2 * np.pi
SEQ = '/root/work/soma-v3/mocap/seq'


def load():
    out = {}
    for f in sorted(glob.glob(os.path.join(SEQ, '*.csv'))):
        a = np.loadtxt(f, delimiter=',', dtype=np.float64)
        if a.ndim != 2 or len(a) < 60:
            continue
        q = a[:, 2:12]
        bad = np.zeros(len(a), bool)
        bad[1:] = np.abs(a[:-1, 22:32] - a[1:, 2:12]).max(1) > 0.087
        seg = np.cumsum(bad)
        back = np.zeros_like(q)
        back[1:] = (q[1:] - q[:-1]) * 60.0
        back[bad] = 0.0
        a[:, 12:22] = back
        out[os.path.basename(f)[:-4]] = (a, seg)
    return out


def build(ids, subs):
    d = np.concatenate([subs[s][0] for s in ids])
    seg = np.concatenate([subs[s][1] + i * 10**6 for i, s in enumerate(ids)])
    return d, seg


def fourier(ph, u, K=4):
    cols = [np.ones_like(ph)]
    for k in range(1, K + 1):
        cols += [np.sin(TAU * k * ph), np.cos(TAU * k * ph)]
    cols += [u]
    for k in range(1, 3):
        cols += [u * np.sin(TAU * k * ph), u * np.cos(TAU * k * ph)]
    return np.stack(cols, 1)


class CPG:
    """参考轨迹 R(φ,u) + 误差回拉：q' = R(φ') + K(q - R(φ)) + M·dq
    闭环误差 e' = K e，K 的谱半径 < 1 就保证不发散 —— 这是"会走路"的结构先验，
    不是拟合出来的巧合。"""

    def __init__(self, K=4, extra_dq=True, shrink=1.0):
        self.K, self.extra_dq, self.shrink = K, extra_dq, shrink

    def fit(self, d):
        F = fourier(d[:, 1], d[:, 0], self.K)
        self.R = np.linalg.lstsq(F, d[:, 22:32], rcond=None)[0]
        ref = F @ self.R
        refn = fourier(d[1:, 1], d[1:, 0], self.K) @ self.R
        E = d[:-1, 2:12] - ref[:-1]
        Y = d[1:, 22:32] - refn
        X = np.concatenate([E, d[:-1, 12:22]], 1) if self.extra_dq else E
        M = np.linalg.lstsq(X, Y, rcond=None)[0]
        M[:10] *= self.shrink
        self.M = M
        self.rho = float(np.abs(np.linalg.eigvals(M[:10])).max())
        self.ref_mae = np.degrees(np.abs(ref - d[:, 2:12])).mean()
        return self

    def predict_delta(self, row, nxt, q, dq):
        r0 = fourier(row[1:2], row[0:1], self.K)[0] @ self.R
        r1 = fourier(nxt[1:2], nxt[0:1], self.K)[0] @ self.R
        e = q - r0
        if self.extra_dq:
            e = np.concatenate([e, dq])
        return r1 - q + self.M.T @ e


class Ridge:
    def __init__(self, lam=1e-3, rich=False):
        self.lam, self.rich = lam, rich

    def mk(self, d, q, dq):
        ph, u = d[:, 1], d[:, 0]
        s1, c1 = np.sin(TAU * ph)[:, None], np.cos(TAU * ph)[:, None]
        s2, c2 = np.sin(2 * TAU * ph)[:, None], np.cos(2 * TAU * ph)[:, None]
        s3, c3 = np.sin(3 * TAU * ph)[:, None], np.cos(3 * TAU * ph)[:, None]
        uu = np.stack([u, np.zeros_like(u), np.zeros_like(u), np.zeros_like(u)], 1)
        cols = [q, dq, s1, c1, s2, c2, s3, c3, q * s1, q * c1]
        if self.rich:
            cols += [q * s2, q * c2]
        return np.concatenate(cols + [uu, uu * s1, uu * c1], 1)

    def fit(self, d):
        X = self.mk(d, d[:, 2:12], d[:, 12:22])
        Y = d[:, 22:32] - d[:, 2:12]
        XtX = X.T @ X
        lam = self.lam * np.trace(XtX) / XtX.shape[0]
        self.W = np.linalg.solve(XtX + lam * np.eye(XtX.shape[0]), X.T @ Y)
        return self

    def predict_delta(self, row, nxt, q, dq):
        return (self.mk(row[None], q[None], dq[None]) @ self.W)[0]


class Freeze:
    def predict_delta(self, row, nxt, q, dq): return np.zeros(10)


class Vel:
    def predict_delta(self, row, nxt, q, dq): return dq / 60.0


class PhaseOnly:
    """纯相位回归：q = R(φ,u)，delta = R(φ')-q。没有任何反馈项。"""

    def __init__(self, K=4):
        self.c = CPG(K, extra_dq=False)

    def fit(self, d):
        self.c.fit(d)
        return self

    def predict_delta(self, row, nxt, q, dq):
        r1 = fourier(nxt[1:2], nxt[0:1], self.c.K)[0] @ self.c.R
        return r1 - q


def rollout(model, d, seg, H, step=9):
    starts = np.arange(0, len(d) - H - 2, step)
    starts = starts[seg[starts] == seg[starts + H]]
    q = d[starts, 2:12].copy(); dq = d[starts, 12:22].copy()
    for h in range(H):
        rows, nxts = d[starts + h], d[starts + h + 1]
        dl = np.stack([model.predict_delta(rows[i], nxts[i], q[i], dq[i]) for i in range(len(starts))])
        dq = dl * 60.0
        q = q + dl
    return np.degrees(np.abs(q - d[starts + H, 22:32])).mean()


def single_step(model, d):
    idx = np.arange(0, len(d) - 1, 11)
    pred = np.stack([model.predict_delta(d[i], d[i + 1], d[i, 2:12], d[i, 12:22]) for i in idx])
    true = d[idx, 22:32] - d[idx, 2:12]
    return np.degrees(np.abs(pred - true)).mean(), np.degrees(np.abs(true)).mean()


def main():
    subs = load()
    ids = sorted(subs)
    va_ids = [s for i, s in enumerate(ids) if i % 5 == 4]
    tr_ids = [s for s in ids if s not in va_ids]
    dtr, _ = build(tr_ids, subs)
    dva, segva = build(va_ids, subs)
    print('训练 %d 人 %d 帧 | 留出 %d 人 %d 帧 %s'
          % (len(tr_ids), len(dtr), len(va_ids), len(dva), va_ids), flush=True)

    cands = [('冻结(不动)', Freeze()), ('匀速外推', Vel()),
             ('线性58 诚实', Ridge(1e-3)), ('线性78 诚实', Ridge(1e-3, True)),
             ('纯相位 k4', PhaseOnly(4)),
             ('CPG k4 无dq', CPG(4, False)), ('CPG k4 +dq', CPG(4, True)),
             ('CPG k4 +dq K.85', CPG(4, True, 0.85)), ('CPG k6 +dq', CPG(6, True))]
    for tag, m in cands:
        t0 = time.time()
        if hasattr(m, 'fit'):
            m.fit(dtr)
        ss, base = single_step(m, dva)
        r = {H: rollout(m, dva, segva, H) for H in (1, 6, 24)}
        print('%-18s 单步%.2f°(不动%.2f°) | 1步%.2f 6步%.2f 24步%.2f | ρ=%.2f | %.0fs'
              % (tag, ss, base, r[1], r[6], r[24], getattr(m, 'rho', float('nan')), time.time() - t0), flush=True)


if __name__ == '__main__':
    main()
