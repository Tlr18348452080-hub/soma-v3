#!/usr/bin/env python3
"""SOMA 脊髓矩阵层：受试者留出评估 + 多模型对比。

数据 = mocap/seq/<受试者>.csv（bvh2seq 产出，60Hz）
列 = u0(=speed/2), ph, q[10], dq[10], q_next[10]
训练目标恒为 delta = q_next - q（运行时 q += A·x + b）

留出按**受试者**切，不按块切 —— 按块切会把同一个人的同一种走法
放进训练集，报出来的泛化误差是虚的。
"""
import numpy as np, glob, os, sys, json, time

SEQ = '/root/work/soma-v3/mocap/seq'
TAU = 2 * np.pi
RICH = False


def load():
    subs = {}
    for f in sorted(glob.glob(os.path.join(SEQ, '*.csv'))):
        sid = os.path.basename(f)[:-4]
        d = np.loadtxt(f, delimiter=',', dtype=np.float32)
        if d.ndim != 2 or len(d) < 40:
            continue
        subs[sid] = d
    return subs


def split_subjects(subs, fold=5, which=4):
    ids = sorted(subs)
    va = [s for i, s in enumerate(ids) if i % fold == which]
    tr = [s for i, s in enumerate(ids) if i % fold != which]
    return tr, va


def cat(subs, ids):
    return np.concatenate([subs[s] for s in ids], axis=0)


def feats_base(d):
    """与 cpp/spinal.h 的 feats() 完全一致：58 维。"""
    q, dq = d[:, 2:12], d[:, 12:22]
    u, ph = d[:, 0:1], d[:, 1]
    s1, c1 = np.sin(TAU * ph)[:, None], np.cos(TAU * ph)[:, None]
    s2, c2 = np.sin(2 * TAU * ph)[:, None], np.cos(2 * TAU * ph)[:, None]
    s3, c3 = np.sin(3 * TAU * ph)[:, None], np.cos(3 * TAU * ph)[:, None]
    uu = np.concatenate([u, np.zeros((len(d), 3), np.float32)], 1)
    return np.concatenate([q, dq, s1, c1, s2, c2, s3, c3,
                           q * s1, q * c1, uu, uu * s1, uu * c1], 1)


def feats_rich(d):
    """加 2φ/3φ 的关节交互 —— 相位锁定的"换牙"更细。"""
    q, dq = d[:, 2:12], d[:, 12:22]
    u, ph = d[:, 0:1], d[:, 1]
    s1, c1 = np.sin(TAU * ph)[:, None], np.cos(TAU * ph)[:, None]
    s2, c2 = np.sin(2 * TAU * ph)[:, None], np.cos(2 * TAU * ph)[:, None]
    s3, c3 = np.sin(3 * TAU * ph)[:, None], np.cos(3 * TAU * ph)[:, None]
    uu = np.concatenate([u, np.zeros((len(d), 3), np.float32)], 1)
    return np.concatenate([q, dq, s1, c1, s2, c2, s3, c3,
                           q * s1, q * c1, q * s2, q * c2, q * s3, q * c3,
                           uu, uu * s1, uu * c1], 1)


class Ridge:
    def __init__(self, lam=1e-4):
        self.lam = lam

    def fit(self, X, Y):
        XtX = X.T @ X
        lam = self.lam * np.trace(XtX) / XtX.shape[0]
        self.W = np.linalg.solve(XtX + lam * np.eye(XtX.shape[0], dtype=X.dtype), X.T @ Y)
        return self

    def predict(self, X):
        return X @ self.W


class MLP:
    def __init__(self, hin, hid=48, hout=10, epochs=60, lr=3e-3, seed=0):
        r = np.random.default_rng(seed)
        self.W1 = (r.standard_normal((hin, hid)) * np.sqrt(2.0 / hin)).astype(np.float32)
        self.b1 = np.zeros(hid, np.float32)
        self.W2 = (r.standard_normal((hid, hout)) * np.sqrt(1.0 / hid)).astype(np.float32)
        self.b2 = np.zeros(hout, np.float32)
        self.epochs, self.lr = epochs, lr
        self.mu = np.zeros(hin, np.float32)
        self.sd = np.ones(hin, np.float32)

    def fit(self, X, Y):
        self.mu, self.sd = X.mean(0), X.std(0) + 1e-6
        Xn = (X - self.mu) / self.sd
        p = [self.W1, self.b1, self.W2, self.b2]
        m = [np.zeros_like(a) for a in p]
        v = [np.zeros_like(a) for a in p]
        b1_, b2_, eps = 0.9, 0.999, 1e-8
        n = len(Xn)
        for ep in range(self.epochs):
            h = np.tanh(Xn @ self.W1 + self.b1)
            pred = h @ self.W2 + self.b2
            g = (pred - Y) * (2.0 / (n * Y.shape[1]))
            gW2 = h.T @ g
            gb2 = g.sum(0)
            gh = (g @ self.W2.T) * (1 - h * h)
            gW1 = Xn.T @ gh
            gb1 = gh.sum(0)
            for i, gg in enumerate([gW1, gb1, gW2, gb2]):
                m[i] = b1_ * m[i] + (1 - b1_) * gg
                v[i] = b2_ * v[i] + (1 - b2_) * gg * gg
                mh = m[i] / (1 - b1_ ** (ep + 1))
                vh = v[i] / (1 - b2_ ** (ep + 1))
                p[i] -= self.lr * mh / (np.sqrt(vh) + eps)
        self.W1, self.b1, self.W2, self.b2 = p
        return self

    def predict(self, X):
        h = np.tanh(((X - self.mu) / self.sd) @ self.W1 + self.b1)
        return h @ self.W2 + self.b2


def feats_fn(d, q, dq):
    u, ph = d[:, 0:1], d[:, 1]
    s1, c1 = np.sin(TAU * ph)[:, None], np.cos(TAU * ph)[:, None]
    s2, c2 = np.sin(2 * TAU * ph)[:, None], np.cos(2 * TAU * ph)[:, None]
    s3, c3 = np.sin(3 * TAU * ph)[:, None], np.cos(3 * TAU * ph)[:, None]
    uu = np.concatenate([u, np.zeros((len(d), 3), np.float32)], 1)
    if RICH:
        return np.concatenate([q, dq, s1, c1, s2, c2, s3, c3,
                               q * s1, q * c1, q * s2, q * c2, q * s3, q * c3,
                               uu, uu * s1, uu * c1], 1)
    return np.concatenate([q, dq, s1, c1, s2, c2, s3, c3,
                           q * s1, q * c1, uu, uu * s1, uu * c1], 1)


def single_step_deg(model, X, Y):
    return np.degrees(np.abs(model.predict(X) - Y)).mean()


def rollout_deg(pred_fn, d, H, damp=1.0, stride=3):
    """闭环：只喂模型自己的预测，相位/速度用真值当"时钟"。"""
    idx = np.arange(0, len(d), stride)
    if len(idx) < H + 2:
        return float('nan')
    frames = d[idx]
    q = frames[:, 2:12].copy()
    dq = frames[:, 12:22].copy()
    for h in range(H):
        nxt = np.minimum(idx + h, len(d) - 1)
        x = feats_fn(d[nxt], q, dq)
        delta = pred_fn(x) * damp
        dq = delta * 60.0
        q = q + delta
    truth = d[np.minimum(idx + H * stride, len(d) - 1), 22:32]
    return np.degrees(np.abs(q - truth)).mean()


def main():
    global RICH
    subs = load()
    tr_ids, va_ids = split_subjects(subs)
    dtr, dva = cat(subs, tr_ids), cat(subs, va_ids)
    Ytr = dtr[:, 22:32] - dtr[:, 2:12]
    Yva = dva[:, 22:32] - dva[:, 2:12]
    print('受试者 %d | 训练 %d 人 %d 帧 | 留出 %d 人 %d 帧 %s'
          % (len(subs), len(tr_ids), len(dtr), len(va_ids), len(dva), va_ids), flush=True)

    for tag, ffn, rich, maker in [
        ('线性58（现役）', feats_base, False, lambda: Ridge(1e-4)),
        ('线性98 +2φ3φ交互', feats_rich, True, lambda: Ridge(1e-4)),
        ('MLP58-48-10', feats_base, False, lambda: MLP(58, 48, 10, epochs=80)),
        ('MLP98-64-10', feats_rich, True, lambda: MLP(98, 64, 10, epochs=80)),
    ]:
        RICH = rich
        t0 = time.time()
        model = maker().fit(ffn(dtr), Ytr)
        s_tr = single_step_deg(model, ffn(dtr), Ytr)
        s_va = single_step_deg(model, ffn(dva), Yva)
        r6 = rollout_deg(model.predict, dva, 6)
        r24 = rollout_deg(model.predict, dva, 24, 0.9)
        print('%-20s 单步 训练%.2f 留出%.2f | 6步 %.2f | 24步(.9) %.2f | %.0fs'
              % (tag, s_tr, s_va, r6, r24, time.time() - t0), flush=True)
        np.save('/tmp/model_%d.npy' % len(tag), np.zeros(1))


if __name__ == '__main__':
    main()
