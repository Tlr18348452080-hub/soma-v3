#pragma once
#include <cstdio>
#include <cstring>
#include <string>
#include <cmath>

/* ================= 脊髓矩阵层（"仿 AI"）=================
   它不是神经网络，是一张从人体动作数据里拟合出来的线性映射：
        x_{k+1} = A·x_k + b        x = [十个关节角, 十个角速度]
   输入是身体现在什么样，输出是"下一秒最可能的样子"。
   它只做人会做的事 —— 走路、跑步、转身、收腿；不会做情理之外的事，
   那些留给大脑（主 AI 线程），而大脑只在被问的时候回答 false 或一句 JSON。
*/
struct MatrixSpinal {
  /* 10 个自由度 = 6 个下肢关节 + 躯干俯仰/侧倾 + 颈 + 摆臂。
     从 6 涨到 10 之后，脊髓第一次"知道"上半身在干嘛：
     走路时手臂在摆、躯干在跟着重心侧倾，这些是它以前预测不到的部分。 */
  /* DX = 20 个"现在什么样" + 4 个"循环走到第几帧"：
     sin/cos(φ) 和 sin/cos(2φ)。步态是周期运动，纯线性动力学预测不了周期，
     有了这两对特征，模型才知道"现在是左脚刚落地"还是"右脚在摆动中段"。 */
  /* 特征 = 现在什么样 + 循环走到第几帧 + 两者的乘积。
     最后一组（q·sinφ、q·cosφ）是关键：同一条腿在支撑相和摆动相的刚度完全不同，
     线性模型表达不了，乘上相位就等于让 A 在循环里"换一副牙"。 */
  static const int D = 10, DX = 4 * 10 + 6 + 4 + 8, PH = 6;   /* + u(4) + u*sin/cos(8) */
  static const int U = 4;                                     /* 意图维度：速度/抬手/前倾/转头 */
  double A[DX][D] = {};
  double b[D] = {};
  int fps = 20;
  double rmseDeg = -1;
  bool loaded = false;

  bool load(const char* path) {
    FILE* f = std::fopen(path, "rb");
    if (!f) return false;
    std::string s;
    char buf[4096];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) s.append(buf, n);
    std::fclose(f);
    const char* p = s.c_str();
    /* A：DX 行 × D 列 */
    const char* a = std::strstr(p, "\"A\"");
    if (!a) return false;
    const char* q = std::strchr(a, '[');
    if (!q) return false;
    ++q;
    for (int i = 0; i < DX; ++i) {
      while (*q && *q != '[') ++q;
      if (!*q) return false;
      ++q;
      for (int k = 0; k < D; ++k) {
        char* end = nullptr;
        A[i][k] = std::strtod(q, &end);
        q = end;
        while (*q == ',' || *q == ' ') ++q;
      }
      while (*q && *q != ']') ++q;
      ++q;
    }
    const char* bb = std::strstr(p, "\"b\"");
    if (bb) {
      const char* r = std::strchr(bb, '[');
      if (r) {
        ++r;
        for (int k = 0; k < D; ++k) {
          char* end = nullptr;
          b[k] = std::strtod(r, &end);
          r = end;
          while (*r == ',' || *r == ' ') ++r;
        }
      }
    }
    if ((bb = std::strstr(p, "\"rmseDeg\""))) {
      const char* c = std::strchr(bb, ':');
      if (c) rmseDeg = std::strtod(c + 1, nullptr);
    }
    loaded = true;
    return true;
  }

  /* q: 当前六个关节角   dq: 当前角速度   out: 预测的下一帧角度 */
  static void feats(const float q[D], const float dq[D], double ph, const float u[U], double x[DX]) {
    const double TAU = 6.283185307179586;
    for (int i = 0; i < D; ++i) { x[i] = q[i]; x[D + i] = dq[i]; }
    const double s1 = std::sin(TAU * ph), c1 = std::cos(TAU * ph);
    x[2 * D + 0] = s1; x[2 * D + 1] = c1;
    x[2 * D + 2] = std::sin(2.0 * TAU * ph); x[2 * D + 3] = std::cos(2.0 * TAU * ph);
    x[2 * D + 4] = std::sin(3.0 * TAU * ph); x[2 * D + 5] = std::cos(3.0 * TAU * ph);
    for (int i = 0; i < D; ++i) {
      x[2 * D + 6 + i] = q[i] * s1;
      x[3 * D + 6 + i] = q[i] * c1;
    }
    /* 意图维度：让模型知道"现在想快一点 / 想抬手"，而不是只会预测一种走法 */
    for (int k = 0; k < U; ++k) x[4 * D + 6 + k] = u[k];
    for (int k = 0; k < U; ++k) {
      x[4 * D + 6 + U + k] = u[k] * s1;
      x[4 * D + 6 + U + U + k] = u[k] * c1;
    }
  }

  void predict(const float q[D], const float dq[D], double ph, const float u[U], float out[D]) const {
    double x[DX];
    feats(q, dq, ph, u, x);
    for (int k = 0; k < D; ++k) {
      double acc = b[k];
      for (int i = 0; i < DX; ++i) acc += A[i][k] * x[i];
      out[k] = (float)acc;
    }
  }
};

/* ================= 节律输出层（从人数据学来的"走路长什么样"）=================
   q(φ) = Σ_{k=0..K} [ a_k cos(kφ) + b_k sin(kφ) ]
   相位 φ 由 CPG 给；系数从 motion-data.js 拟合（walk RMSE 4.41°，0.95 Hz）。
   六个数：髋L 膝L 踝L 髋R 膝R 踝R（弧度）。
*/
struct GaitLayer {
  static const int K = 3, NB = 2 * (K + 1);
  double coef[6][NB] = {};
  bool loaded = false;
  double T = 21;       // 帧（数据里一个周期多少帧）
  double fps = 20;

  bool load(const char* path, const char* gait = "walk") {
    FILE* f = std::fopen(path, "rb");
    if (!f) return false;
    std::string s; char buf[4096]; size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) s.append(buf, n);
    std::fclose(f);
    std::string key = std::string("\"") + gait + "\":{";
    const char* p = std::strstr(s.c_str(), key.c_str());
    if (!p) return false;
    const char* c = std::strstr(p, "\"coef\"");
    if (!c) return false;
    c = std::strchr(c, '[');
    if (!c) return false;
    ++c;
    for (int j = 0; j < 6; ++j) {
      while (*c && *c != '[') ++c;
      if (!*c) return false;
      ++c;
      for (int k = 0; k < NB; ++k) {
        char* end = nullptr;
        coef[j][k] = std::strtod(c, &end);
        c = end;
        while (*c == ',' || *c == ' ') ++c;
      }
      while (*c && *c != ']') ++c;
      ++c;
    }
    if (const char* t = std::strstr(p, "\"T\"")) { const char* q = std::strchr(t, ':'); if (q) T = std::strtod(q + 1, nullptr); }
    if (const char* t = std::strstr(p, "\"fps\"")) { const char* q = std::strchr(t, ':'); if (q) fps = std::strtod(q + 1, nullptr); }
    loaded = true;
    return true;
  }

  /* phi 是弧度；out 六个关节角（弧度） */
  void pose(double phi, float out[6]) const {
    double b[NB];
    for (int k = 0; k <= K; ++k) { b[2*k] = std::cos(k*phi); b[2*k+1] = std::sin(k*phi); }
    for (int j = 0; j < 6; ++j) {
      double acc = 0;
      for (int i = 0; i < NB; ++i) acc += coef[j][i] * b[i];
      out[j] = (float)acc;
    }
  }
};
