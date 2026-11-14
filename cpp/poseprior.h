#pragma once
/* ================= 姿态先验：u → 手/肘的偏移 =================
   这里没有"挥手动作"这种条目。模型只学一件事：
       给定意图 u=(速度, 抬手, 前倾, 摆手频率) 和当前自己的摆动相位，
       两只手相对身体的偏移应该是多少（12 个数：手 L/R、肘 L/R 各 3 轴）。
   数据来自"探索"：采集时把手臂按随机幅度/频率/平均抬高乱推一阵，
   记录"推成什么样"和"当时的 u 是多少"，反过来学 u → 偏移。
   于是运行时给任意 u（包括没采过的组合）都能得到连续的手位，
   走路 + 抬手 + 摆手可以同时成立，不需要列举。 */
#include <cmath>
#include <cstdio>
#include <string>

struct PosePrior {
  static const int F = 10;   /* [1, u0, sin a, cos a, u2, s, c, sin a*s, sin a*c, u2*s]，a = u1*1.15 */
  static const int O = 12;   /* 手L(3) 手R(3) 肘L(3) 肘R(3) */
  float W[O][F];
  float lr = 0.02f;
  long long n = 0;
  float rmse = 0;

  PosePrior() { for (int o = 0; o < O; ++o) for (int f = 0; f < F; ++f) W[o][f] = 0; }
  static void feats(const float u[4], const float wave[2], float x[F]) {
    /* 抬手本质是绕肩转一个角，用 sin/cos 编码才线性可分（直接拿 u1 学不出来） */
    const float a = u[1] * 1.15f;
    const float sa = std::sin(a), ca = std::cos(a);
    x[0] = 1.0f; x[1] = u[0];
    x[2] = sa;   x[3] = ca;
    x[4] = u[2];
    x[5] = wave[0]; x[6] = wave[1];
    x[7] = sa * wave[0]; x[8] = sa * wave[1];
    x[9] = u[2] * wave[0];
  }
  void train(const float x[F], const float y[O]) {
    float xx = 1e-3f;
    for (int f = 0; f < F; ++f) xx += x[f] * x[f];
    double s2 = 0;
    for (int o = 0; o < O; ++o) {
      float p = 0;
      for (int f = 0; f < F; ++f) p += W[o][f] * x[f];
      const float e = y[o] - p;
      s2 += e * e;
      const float g = lr * e / xx;
      for (int f = 0; f < F; ++f) W[o][f] += g * x[f];
    }
    rmse += (std::sqrt(s2 / O) - rmse) * 0.01f;
    ++n;
  }
  void eval(const float x[F], float y[O]) const {
    for (int o = 0; o < O; ++o) {
      float p = 0;
      for (int f = 0; f < F; ++f) p += W[o][f] * x[f];
      y[o] = p;
    }
  }
  bool save(const char* path) const {
    FILE* f = fopen(path, "w");
    if (!f) return false;
    fprintf(f, "{\"F\":%d,\"O\":%d,\"n\":%lld,\"rmse\":%.5f,\"W\":[", F, O, n, rmse);
    for (int o = 0; o < O; ++o) {
      fprintf(f, "%s[", o ? "," : "");
      for (int i = 0; i < F; ++i) fprintf(f, "%s%.6f", i ? "," : "", W[o][i]);
      fprintf(f, "]");
    }
    fprintf(f, "]}\n");
    fclose(f);
    return true;
  }
  bool load(const char* path) {
    FILE* f = fopen(path, "r");
    if (!f) return false;
    std::string s;
    char buf[4096];
    size_t k;
    while ((k = fread(buf, 1, sizeof(buf), f)) > 0) s.append(buf, k);
    fclose(f);
    char fkey[24]; std::snprintf(fkey, sizeof fkey, "\"F\":%d", F);
    if (s.find(fkey) == std::string::npos) return false;
    size_t p = s.find("\"W\":[");
    if (p == std::string::npos) return false;
    p += 5;
    for (int o = 0; o < O; ++o) {
      p = s.find('[', p);
      if (p == std::string::npos) return false;
      ++p;
      for (int i = 0; i < F; ++i) {
        W[o][i] = (float)strtod(s.c_str() + p, nullptr);
        p = s.find_first_of(",]", p);
        if (p == std::string::npos) return false;
        if (s[p] == ',') ++p;
      }
      p = s.find(']', p);
    }
    return true;
  }
};
