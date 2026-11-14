#!/usr/bin/env python3
"""干净子集 + 稳健指标（中位误差）上的模型对比。

前几轮的教训：
- 用 CSV 前向差分 dq 当特征 = 泄漏，单步误差假到 0.04°；
- 少数片段有单帧 50~350° 的跳（帧率不一致 / 捕捉抖动），均值误差全被它们带歪，
  所以先按"单帧中位位移"筛片段，再看中位误差。
"""
import numpy as np, glob, os, json, sys
TAU = 2 * np.pi
CLIP = '/root/work/soma-v3/mocap/clip'
KEEP = ('walk', 'run', 'jog', 'turn', 'stair', 'step', 'stride', 'slope', 'sidestep', 'march', 'navigate')


def unwrap(q):
    q = q.copy()
    for k in range(q.shape[1]):
        d = np.diff(q[:, k])
        d -= 2 * np.pi * np.round(d / (2 * np.pi))
        q[1:, k] = q[0, k] + np.cumsum(d)
    return q


def load_clips():
    man = {x['id']: x for x in json.load(open('/root/work/soma-v3/mocap/manifest.json'))}
    out = []
    for f in sorted(glob.glob(os.path.join(CLIP, '*.csv'))):
        cid = os.path.basename(f)[:-4]
        a = np.loadtxt(f, delimiter=',', dtype=np.float64)
        if a.ndim != 2 or len(a) < 90:
            continue
        desc = man.get(cid, {}).get('desc', '?')
        if not any(k in desc.lower() for k in KEEP):
            continue
        q = unwrap(a[:, 2:12])
        stepmed = np.median(np.degrees(np.abs(np.diff(q, axis=0))))
        if stepmed > 3.0:            # 抖到不像走路了
            continue
        dq_ = a[:, 22:32] - a[:, 2:12]
        a[:, 22:32] = q + (dq_ - 2 * np.pi * np.round(dq_ / (2 * np.pi)))
        a[:, 2:12] = q
        back = np.zeros_like(q)
        back[1:] = (q[1:] - q[:-1]) * 60.0
        a[:, 12:22] = back
        out.append((cid, man.get(cid, {}).get('subject', cid[:2]), a, desc, stepmed))
    return out


def fourier(ph, u, K=4):
    cols = [np.ones_like(ph)]
    for k in range(1, K + 1):
        cols += [np.sin(TAU * k * ph), np.cos(TAU * k * ph)]
    cols += [u]
    for k in range(1, 3):
        cols += [u * np.sin(TAU * k * ph), u * np.cos(TAU * k * ph)]
    return np.stack(cols, 1)


class CPG:
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
        cols = [q, dq, s1, c1, s2, c2, s3, c3, q * s1, q * c1]
        if self.rich:
            cols += [q * s2, q * c2]
        return np.concatenate(cols, 1)

    def fit(self, d):
        X = self.mk(d, d[:, 2:12], d[:, 12:22])
        Y = d[:, 22:32] - d[:, 2:12]
        XtX = X.T @ X
        lam = self.lam * np.trace(XtX) / XtX.shape[0]
        self.W = np.linalg.solve(XtX + lam * np.eye(XtX.shape[0]), X.T @ Y)
        self.mu = X.mean(0)
        return self

    def predict_delta(self, row, nxt, q, dq):
        return (self.mk(row[None], q[None], dq[None]) @ self.W)[0]


class Shipped:
    """现役 assets/spinal_learned.json（按 spinal.h 的 feats 顺序直接算）。"""

    def __init__(self, path='/root/work/soma-v3/assets/spinal_learned.json'):
        j = json.load(open(path))
        self.A = np.array(j['A'], float)
        self.b = np.array(j['b'], float)

    def mk(self, row, q, dq):
        ph, u = row[1], row[0]
        s1, c1 = np.sin(TAU * ph), np.cos(TAU * ph)
        s2, c2 = np.sin(2 * TAU * ph), np.cos(2 * TAU * ph)
        s3, c3 = np.sin(3 * TAU * ph), np.cos(3 * TAU * ph)
        uu = np.array([u, 0, 0, 0])
        return np.concatenate([q, dq, [s1, c1, s2, c2, s3, c3], q * s1, q * c1,
                               uu, uu * s1, uu * c1])

    def predict_delta(self, row, nxt, q, dq):
        return self.mk(row, q, dq) @ self.A + self.b


class Freeze:
    def predict_delta(self, row, nxt, q, dq): return np.zeros(10)


class Vel:
    def predict_delta(self, row, nxt, q, dq): return dq / 60.0


def eval_all(model, clips, H, step=4):
    errs = []
    for cid, subj, d, desc, _ in clips:
        starts = np.arange(0, len(d) - H - 1, step)
        if not len(starts):
            continue
        q = d[starts, 2:12].copy(); dq = d[starts, 12:22].copy()
        for h in range(H):
            rows, nxts = d[starts + h], d[starts + h + 1]
            dl = np.stack([model.predict_delta(rows[i], nxts[i], q[i], dq[i]) for i in range(len(starts))])
            dq = dl * 60.0; q = q + dl
        errs.append(np.degrees(np.abs(q - d[starts + H, 22:32])).mean(1))
    e = np.concatenate(errs)
    return float(np.mean(e)), float(np.median(e))


def single(model, clips, step=7):
    errs, base = [], []
    for cid, subj, d, desc, _ in clips:
        idx = np.arange(0, len(d) - 1, step)
        pred = np.stack([model.predict_delta(d[i], d[i + 1], d[i, 2:12], d[i, 12:22]) for i in idx])
        true = d[idx, 22:32] - d[idx, 2:12]
        errs.append(np.degrees(np.abs(pred - true)).mean(1))
        base.append(np.degrees(np.abs(true)).mean(1))
    return float(np.median(np.concatenate(errs))), float(np.median(np.concatenate(base)))


def main():
    clips = load_clips()
    subs = sorted(set(c[1] for c in clips))
    print('干净片段 %d 个，受试者 %d 人：%s' % (len(clips), len(subs), subs), flush=True)
    va_ids = set(s for i, s in enumerate(subs) if i % 5 == 4)
    va = [c for c in clips if c[1] in va_ids]
    tr = [c for c in clips if c[1] not in va_ids]
    dtr = np.concatenate([c[2] for c in tr])
    print('训练 %d 帧 / 留出 %d 帧（%s）' % (len(dtr), sum(len(c[2]) for c in va), sorted(va_ids)), flush=True)
    print('%-16s %s' % ('模型', '单步中位 | 1步(中位) 6步 24步'), flush=True)
    cands = [('冻结', Freeze()), ('匀速', Vel()), ('现役 spinal_learned', Shipped()),
             ('线性58 重训', Ridge(1e-3)), ('线性78 重训', Ridge(1e-3, True)),
             ('CPG k4', CPG(4, False)), ('CPG k4 +dq', CPG(4, True)),
             ('CPG k4 +dq ×0.9', CPG(4, True, 0.9))]
    for tag, m in cands:
        if hasattr(m, 'fit'):
            m.fit(dtr)
        s, sb = single(m, va)
        r1 = eval_all(m, va, 1)
        r6 = eval_all(m, va, 6)
        r24 = eval_all(m, va, 24)
        print('%-16s %5.2f°(不动%5.2f) | %6.2f°/%5.2f°  %6.2f°/%5.2f°  %6.2f°/%5.2f°  ρ=%.2f'
              % (tag, s, sb, r1[0], r1[1], r6[0], r6[1], r24[0], r24[1], getattr(m, 'rho', float('nan'))), flush=True)


if __name__ == '__main__':
    main()
