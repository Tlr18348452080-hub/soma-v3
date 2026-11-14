#!/usr/bin/env python3
"""用她自己的身体（soma3d --seqdump）训练脊髓矩阵层。

为什么不用 mocap 直接当先验：CMU 的骨架拓扑和 walker 不一样，同一个
SpinalCore::measure() 量出来的角分布完全不同（髋 28.7° vs -0.6°），
拿它当先验等于给模型喂错标尺，运行时只能靠在线 LMS 慢慢抹掉。
mocap 的价值在姿态先验（体坐标手臂偏移，与骨架无关），不在这 10 个角。

列：ep ph u0 u1 u2 u3 q0..q9
目标：delta = q_{t+1} - q_t（运行时 q += A·x + b）
特征：与 cpp/spinal.h 的 feats() 逐位一致，58 维。
"""
import numpy as np, sys

TAU = 2 * np.pi
SRC = sys.argv[1] if len(sys.argv) > 1 else '/tmp/selfseq_big.txt'


def load(path):
    a = np.loadtxt(path)
    eps = []
    for ep in np.unique(a[:, 0]).astype(int):
        d = a[a[:, 0] == ep]
        if len(d) < 40:
            continue
        eps.append(d)
    return eps


def feats(q, dq, ph, u):
    """u: (N,4)，顺序 = vDes/2, armRaise, lean, waveHz/3.2（同 main.cpp:659）"""
    n = len(q)
    s1, c1 = np.sin(TAU * ph)[:, None], np.cos(TAU * ph)[:, None]
    s2, c2 = np.sin(2 * TAU * ph)[:, None], np.cos(2 * TAU * ph)[:, None]
    s3, c3 = np.sin(3 * TAU * ph)[:, None], np.cos(3 * TAU * ph)[:, None]
    return np.concatenate([q, dq, s1, c1, s2, c2, s3, c3,
                           q * s1, q * c1, u, u * s1, u * c1], 1)


def prep(eps):
    out = []
    for d in eps:
        q = d[:, 6:16]
        ph, u = d[:, 1], d[:, 2:6]
        dq = np.zeros_like(q)
        dq[1:] = (q[1:] - q[:-1]) * 60.0
        out.append((q, dq, ph, u))
    return out


class Ridge:
    def __init__(self, lam=1e-4):
        self.lam = lam

    def fit(self, chunks):
        X = np.concatenate([feats(q, dq, ph, u)[:-1] for q, dq, ph, u in chunks])
        Y = np.concatenate([(q[1:] - q[:-1]) for q, dq, ph, u in chunks])
        XtX = X.T @ X
        lam = self.lam * np.trace(XtX) / XtX.shape[0]
        self.W = np.linalg.solve(XtX + lam * np.eye(XtX.shape[0]), X.T @ Y)
        self.b = np.zeros(10)
        return self

    def delta(self, q, dq, ph, u):
        return feats(q, dq, ph, u) @ self.W


def single(m, chunks):
    X = np.concatenate([feats(q, dq, ph, u)[:-1] for q, dq, ph, u in chunks])
    Y = np.concatenate([(q[1:] - q[:-1]) for q, dq, ph, u in chunks])
    e = np.degrees(np.abs(X @ m.W - Y))
    return float(e.mean()), float(np.median(e)), float(np.degrees(np.abs(Y)).mean())


def rollout(m, chunks, H):
    errs = []
    for q, dq, ph, u in chunks:
        if len(q) < H + 2:
            continue
        starts = np.arange(0, len(q) - H - 1, 3)
        qq = q[starts].copy(); dd = dq[starts].copy()
        for h in range(H):
            dl = m.delta(qq, dd, ph[starts + h], u[starts + h])
            dd = dl * 60.0
            qq = qq + dl
        errs.append(np.degrees(np.abs(qq - q[starts + H])).mean(1))
    e = np.concatenate(errs)
    return float(e.mean()), float(np.median(e))


class Neg:   # 冻结 / 匀速 基线
    def __init__(self, kind): self.kind = kind
    def delta(self, q, dq, ph, u): return np.zeros_like(q) if self.kind == 'freeze' else dq / 60.0


def main():
    eps = load(SRC)
    chunks = prep(eps)
    print('episodes %d | 帧 %d' % (len(chunks), sum(len(c[0]) for c in chunks)), flush=True)
    n = len(chunks); cut = int(n * 0.8)
    tr, va = chunks[:cut], chunks[cut:]
    m = Ridge(1e-4).fit(tr)
    print('训练帧 %d / 留出帧 %d' % (sum(len(c[0]) for c in tr), sum(len(c[0]) for c in va)), flush=True)
    for tag, mm in [('冻结', Neg('freeze')), ('匀速', Neg('vel')), ('线性58', m)]:
        s = single(mm, va) if tag == '线性58' else None
        r = {H: rollout(mm, va, H) for H in (1, 6, 24)}
        extra = ('单步 %.2f°/中位%.2f°' % (s[0], s[1])) if s else ''
        print('%-6s %s | 1步 %.2f/%.2f | 6步 %.2f/%.2f | 24步 %.2f/%.2f'
              % (tag, extra, r[1][0], r[1][1], r[6][0], r[6][1], r[24][0], r[24][1]), flush=True)
    np.save('/tmp/self_W.npy', m.W)


if __name__ == '__main__':
    main()
