#pragma once
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include "spinal.h"

/* ================= 骨髓中枢 =================
   60 Hz，每帧干四件事：

   1) 把身体的十个关节角量出来（q，弧度）。
   2) 用一张矩阵预测"下一帧这十个角会变成什么样"：
          q' = q + A·[q, dq, sin φ, cos φ, sin 2φ, cos 2φ] + b
      比上一版多了**相位特征**：步态是周期运动，纯线性动力学预测不了周期，
      给它"这是循环里的第几帧"，它才分得清左脚刚落地和右脚在摆中段。
   3) 一边预测一边学（归一化 LMS）。
   4) 每 0.8 秒做一次**多步 rollout 对账**：拿 24 步前的状态往前推 24 步，
      跟真实轨迹比。单步误差小不代表会预测，会走 0.4 秒才叫会预测。

   十个角（顺序固定，改了就要重新学）：
     0 髋L  1 膝L  2 踝L   3 髋R  4 膝R  5 踝R
     6 躯干俯仰   7 躯干侧倾   8 颈俯仰   9 左肩摆
*/
struct SpinalCore {
  static const int D = MatrixSpinal::D;   /* 10 */
  MatrixSpinal sp;

  float qPrev[D]  = {0};
  float dqPrev[D] = {0};
  float yPred[D]  = {0};
  float qNow[D]   = {0};
  double phPrev = 0;        /* 上一帧的步态相位（0..1） */

  double err = 0;           /* 单步预测误差（弧度，十根均值） */
  double errAvg = 0;
  double errPeak = 0;
  double roll = -1;         /* 多步 rollout 误差（弧度），-1 = 还没攒够历史 */
  double rollAvg = -1;
  long long ticks = 0;
  bool warm = false;
  double lr = 0.09;        /* 有相位特征之后可以学快一点 */
  long long savedAt = 0;

  /* ---- 历史环：rollout 对账要回头看 0.4 秒 ---- */
  static const int RH = 160, RH_STEPS = 24;
  float histQ[RH][D];
  float histPh[RH];
  int histN = 0;

  void pushHist(const float q[D], double ph) {
    int i = histN % RH;
    for (int k = 0; k < D; ++k) histQ[i][k] = q[k];
    histPh[i] = (float)ph;
    ++histN;
  }
  const float* histAt(int back) const {           /* back=0 最新 */
    int i = ((histN - 1 - back) % RH + RH) % RH;
    return histQ[i];
  }
  float histPhaseAt(int back) const {
    int i = ((histN - 1 - back) % RH + RH) % RH;
    return histPh[i];
  }

  static float ang(float dx, float dy) { return std::atan2(dx, -dy + 1e-6f); }
  static float angUp(float dx, float dy) { return std::atan2(dx, dy + 1e-6f); }

  static void measure(const Frame& f, float out[D]) {
    for (int side = 0; side < 2; ++side) {
      const int h = 10 + side * 4, k = 11 + side * 4, a = 12 + side * 4, t = 13 + side * 4;
      const V3& hip = f.nodes[h];
      const V3& knee = f.nodes[k];
      const V3& ankle = f.nodes[a];
      const V3& toe = f.nodes[t];
      const float t1 = ang(knee.x - hip.x, knee.y - hip.y);
      const float t2 = ang(ankle.x - knee.x, ankle.y - knee.y);
      out[side * 3 + 0] = t1;
      out[side * 3 + 1] = t1 - t2;
      out[side * 3 + 2] = std::atan2(toe.y - ankle.y, std::fabs(toe.x - ankle.x) + 1e-6f);
    }
    const V3& hips = f.nodes[0];
    const V3& chest = f.nodes[1];
    const V3& neck = f.nodes[2];
    const V3& head = f.nodes[3];
    out[6] = angUp(chest.x - hips.x, chest.y - hips.y);
    out[7] = std::atan2(chest.z - hips.z, chest.y - hips.y + 1e-6f);
    out[8] = angUp(head.x - neck.x, head.y - neck.y);
    out[9] = ang(f.nodes[5].x - f.nodes[4].x, f.nodes[5].y - f.nodes[4].y);
  }

  /* 相位 → 四个周期特征。φ 是 0..1 的"循环帧号"，2φ 那一对给的是摆动/支撑的二次谐波。 */
  static void phaseFeat(double ph, double* out) {
    const double TAU = 6.283185307179586;
    out[0] = std::sin(TAU * ph);
    out[1] = std::cos(TAU * ph);
    out[2] = std::sin(2.0 * TAU * ph);
    out[3] = std::cos(2.0 * TAU * ph);
  }

  /* 意图：4 维，全部来自"外面想要它怎样"，不是关节本身。
     u0 速度(m/s)  u1 抬手(0..1)  u2 前倾(0..1)  u3 转头(-1..1) */
  float u[MatrixSpinal::U] = {0, 0, 0, 0};
  float uPrev[MatrixSpinal::U] = {0, 0, 0, 0};

  void setIntent(const float in[MatrixSpinal::U]) {
    for (int k = 0; k < MatrixSpinal::U; ++k) { uPrev[k] = u[k]; u[k] = in[k]; }
  }
  void buildX(const float q[D], const float dq[D], double ph, double x[MatrixSpinal::DX]) const {
    MatrixSpinal::feats(q, dq, ph, uPrev, x);
  }

  /* ---- 多步 rollout：拿 H 步前的状态，按模型往前推 H 步，跟真实比 ---- */
  void rolloutCheck() {
    const int H = RH_STEPS;
    if (histN < H + 2) return;
    float q[D], dq[D];
    for (int k = 0; k < D; ++k) q[k] = histAt(H)[k];
    for (int k = 0; k < D; ++k) dq[k] = (histAt(H)[k] - histAt(H + 1)[k]) * 60.0f;
    for (int s = 0; s < H; ++s) {
      const double ph = histPhaseAt(H - s);        /* 用真实相位当"时钟" */
      double x[MatrixSpinal::DX];
      MatrixSpinal::feats(q, dq, ph, u, x);
      for (int k = 0; k < D; ++k) {
        double acc = sp.b[k];
        for (int i = 0; i < MatrixSpinal::DX; ++i) acc += sp.A[i][k] * x[i];
        const float qOld = q[k];
        q[k] = clampf(qOld + (float)acc, -3.0f, 3.0f);
        dq[k] = (q[k] - qOld) * 60.0f;
      }
    }
    double s2 = 0;
    for (int k = 0; k < D; ++k) { const double d = q[k] - histAt(0)[k]; s2 += d * d; }
    roll = std::sqrt(s2 / (double)D);
    rollAvg = (rollAvg < 0) ? roll : rollAvg + (roll - rollAvg) * 0.25;
  }

  /* 本体感觉不是干净的：0.6° 的高斯噪声 + 很慢的漂移。
     真脊髓也是在噪声里做预测的，这里照做 —— 学习会在噪声上平均掉。 */
  float noiseDeg = 0.0f;
  float driftDegPerMin = 0.0f;
  unsigned rngState = 12345u;
  float gauss() {
    rngState = rngState * 1664525u + 1013904223u;
    const float u1 = ((rngState >> 8) & 0xFFFF) / 65535.0f + 1e-6f;
    rngState = rngState * 1664525u + 1013904223u;
    const float u2 = ((rngState >> 8) & 0xFFFF) / 65535.0f;
    return std::sqrt(-2.0f * std::log(u1)) * std::cos(6.2831853f * u2);
  }

  void step(const Frame& f) {
    measure(f, qNow);
    if (noiseDeg > 0.0f) {
      const float sg = noiseDeg * 0.0174533f;
      for (int i = 0; i < D; ++i) qNow[i] += sg * gauss();
    }
    if (driftDegPerMin != 0.0f) {
      const float dv = driftDegPerMin * 0.0174533f / 3600.0f;
      for (int i = 0; i < D; ++i) qNow[i] += dv * gauss();
    }
    const double ph = (double)f.phase;

    /* 骨头里只要有一个非有限数，这一帧就别学了 —— 一次 NaN 会顺着
       LMS 更新（g = lr*(ytrue-pred)/xx）把整个 A/b 染成 NaN，
       然后就是每秒一次的"已清零重学"，学了个寂寞。 */
    {
      bool bad = !std::isfinite(ph);
      for (int i = 0; i < D && !bad; ++i) if (!std::isfinite(qNow[i])) bad = true;
      if (bad) { warm = false; return; }
    }

    if (warm) {
      /* --- 单步对账 --- */
      double s = 0;
      for (int i = 0; i < D; ++i) {
        const double pred = (double)qPrev[i] + (double)yPred[i];
        const double d = pred - (double)qNow[i];
        s += d * d;
      }
      err = std::sqrt(s / (double)D);
      if (!std::isfinite(err)) { err = 0; }
      errAvg += (err - errAvg) * 0.02;
      if (!std::isfinite(errAvg)) errAvg = 0;
      if (err > errPeak) errPeak = err;

      /* --- 学 --- */
      double x[MatrixSpinal::DX];
      buildX(qPrev, dqPrev, phPrev, x);
      double xx = 1e-3;
      for (int i = 0; i < MatrixSpinal::DX; ++i) xx += x[i] * x[i];
      for (int k = 0; k < D; ++k) {
        double pred = sp.b[k];
        for (int i = 0; i < MatrixSpinal::DX; ++i) pred += sp.A[i][k] * x[i];
        const double ytrue = (double)qNow[k] - (double)qPrev[k];
        double g = lr * (ytrue - pred) / xx;
        if (!std::isfinite(g)) continue;
        if (g > 0.5) g = 0.5; else if (g < -0.5) g = -0.5;
        for (int i = 0; i < MatrixSpinal::DX; ++i) sp.A[i][k] += g * x[i];
        sp.b[k] += g * 0.25;
      }
    }

    for (int i = 0; i < D; ++i) dqPrev[i] = warm ? (qNow[i] - qPrev[i]) * 60.0f : 0.0f;
    sp.predict(qNow, dqPrev, ph, u, yPred);
    for (int i = 0; i < D; ++i) qPrev[i] = qNow[i];
    phPrev = ph;
    warm = true;
    ++ticks;

    pushHist(qNow, ph);
    if (histN > RH_STEPS + 2 && (ticks % 48) == 0) rolloutCheck();
    if ((ticks % 60) == 0) resetIfInsane();   /* 每秒体检一次，坏了就清零重学 */
  }

  /* ---- 用模型往前推 n 步（不碰真值），给渲染层当"接下来会是什么姿态" ----
     这就是"循环帧 + 预测"：动作不是查表查出来的，是模型按相位一帧帧推出来的。 */
  void predictHorizon(int n, float out[D]) const {
    float q[D], dq[D];
    for (int k = 0; k < D; ++k) { q[k] = qNow[k]; dq[k] = dqPrev[k]; }
    double ph = phPrev;
    const double stepPh = (histN > 1) ? (double)(histPhaseAt(0) - histPhaseAt(1)) : 0.0;
    double dph = stepPh;
    if (dph < 0) dph += 1.0;
    for (int s = 0; s < n; ++s) {
      double x[MatrixSpinal::DX];
      MatrixSpinal::feats(q, dq, ph, u, x);
      for (int k = 0; k < D; ++k) {
        double acc = sp.b[k];
        for (int i = 0; i < MatrixSpinal::DX; ++i) acc += sp.A[i][k] * x[i];
        const float qo = q[k];
        q[k] = clampf(qo + (float)acc, -3.0f, 3.0f);
        dq[k] = (q[k] - qo) * 60.0f;
      }
      ph += dph; if (ph >= 1.0) ph -= 1.0;
    }
    for (int k = 0; k < D; ++k) out[k] = q[k];
  }

  /* 脊髓自己也在抖：预测越离谱，它越不自信（0=有把握，1=全靠猜） */
  float doubt() const {
    const double e = errAvg / 0.16;
    return (float)(e > 1.0 ? 1.0 : (e < 0.0 ? 0.0 : e));
  }

  /* 反射：不自信的时候先把自己走慢一点 */
  float reflexScale() const {
    const float d = doubt();
    if (d < 0.30f) return 1.0f;
    return 1.0f - 0.55f * std::min(1.0f, (d - 0.30f) / 0.55f);
  }

  void reset() { *this = SpinalCore(); }

  /* 体检：矩阵里只要有一个 NaN/Inf 就不许写盘。
     之前就是这么废掉的 —— 一次 NaN 被写进 spinal_learned.json，
     下次启动读进来，所有帧的统计全变 nan，页面 JSON.parse 直接抛异常，
     表现就是"她又不动了"。 */
  void resetIfInsane() {
    if (sane()) return;
    sp = MatrixSpinal();
    errAvg = 0; errPeak = 0; rollAvg = -1; warm = false;
    static int quiet = 0;
    if (++quiet % 60 == 1)   /* 真坏起来是每秒一次，别刷满整个日志 */
      std::fprintf(stderr, "spinal: 预测矩阵出现非有限数，已清零重学（第 %d 次）\n", quiet);
  }

  bool sane() const {
    /* A 是 [DX][D]，必须用 A[i][k]；写成 A[k][i] 是越界读（i 会到 DX），
       -O2 下编译器会拿这个 UB 做边界假设，sane() 就变成一个随机判 false 的东西，
       表现是每秒一次"非有限数"重置、errAvg 永远 0。 */
    for (int i = 0; i < MatrixSpinal::DX; ++i)
      for (int k = 0; k < D; ++k) if (!std::isfinite(sp.A[i][k])) {
        return false; }
    for (int k = 0; k < D; ++k) if (!std::isfinite(sp.b[k])) return false;
    return true;
  }

  bool save(const char* path) const {
    if (!sane()) { std::fprintf(stderr, "spinal: 矩阵里有 NaN，拒绝写盘\n"); return false; }
    FILE* f = std::fopen(path, "w");
    if (!f) return false;
    std::fprintf(f, "{\"D\":%d,\"DX\":%d,\"rmseDeg\":%.4f,\"rollDeg\":%.4f,\"ticks\":%lld,\"A\":[",
                 D, MatrixSpinal::DX, errAvg * 57.29578, rollAvg * 57.29578, (long long)ticks);
    for (int i = 0; i < MatrixSpinal::DX; ++i) {
      std::fprintf(f, "%s[", i ? "," : "");
      for (int k = 0; k < D; ++k) std::fprintf(f, "%s%.9g", k ? "," : "", sp.A[i][k]);
      std::fprintf(f, "]");
    }
    std::fprintf(f, "],\"b\":[");
    for (int k = 0; k < D; ++k) std::fprintf(f, "%s%.9g", k ? "," : "", sp.b[k]);
    std::fprintf(f, "]}\n");
    std::fclose(f);
    return true;
  }

  bool load(const char* path) {
    FILE* f = std::fopen(path, "rb");
    if (!f) return false;
    std::string s;
    char buf[8192];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) s.append(buf, n);
    std::fclose(f);
    /* 维度变了就重学：老文件是 20 维输入的，硬套会把十个角全搞乱 */
    if (s.find("\"DX\":58") == std::string::npos) return false;
    if (!sp.load(path)) return false;
    if (const char* t = std::strstr(s.c_str(), "\"ticks\":")) ticks = std::strtoll(t + 8, nullptr, 10);
    if (const char* t = std::strstr(s.c_str(), "\"rollDeg\":")) {
      const char* q = std::strchr(t, ':');
      rollAvg = q ? std::strtod(q + 1, nullptr) / 57.29578 : -1;
    }
    savedAt = ticks;
    return true;
  }
};
