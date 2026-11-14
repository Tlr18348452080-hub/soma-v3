/* 用真实 mocap（CMU BVH → 我们的 10 个关节角）离线训练脊髓矩阵。
   用法: trainspine <data.csv> <out.json> [epochs] */
#include "spinal.h"
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <vector>
#include <string>
#include <algorithm>
#include <random>

struct Sample { float u[4]; double ph; float q[10], dq[10], qn[10]; };

int main(int argc, char** argv) {
  const char* csv = argc > 1 ? argv[1] : "/tmp/pose_seq.csv";
  const char* out = argc > 2 ? argv[2] : "assets/spinal_learned.json";
  const int epochs = argc > 3 ? atoi(argv[3]) : 24;

  FILE* f = fopen(csv, "r");
  if (!f) { printf("no csv\n"); return 1; }
  std::vector<Sample> S;
  char line[4096];
  while (fgets(line, sizeof line, f)) {
    Sample s{};
    const char* p = line;
    char* end = nullptr;
    s.u[0] = strtof(p, &end); if (end == p) continue; p = end + 1;
    s.ph = strtod(p, &end); p = end + 1;
    for (int k = 0; k < 10; ++k) { s.q[k] = strtof(p, &end); p = end + 1; }
    for (int k = 0; k < 10; ++k) { s.dq[k] = strtof(p, &end); p = end + 1; }
    for (int k = 0; k < 10; ++k) { s.qn[k] = strtof(p, &end); p = end + 1; }
    s.u[1] = s.u[2] = s.u[3] = 0;
    S.push_back(s);
  }
  fclose(f);
  printf("samples %zu\n", S.size());
  if (S.size() < 100) return 1;

  /* 按块留出 20% 做验证：每 5 段取 1 段 */
  std::vector<int> tr, va;
  const int BLK = 240;
  for (size_t i = 0; i < S.size(); ++i) (((i / BLK) % 5) == 4 ? va : tr).push_back((int)i);

  MatrixSpinal A;
  {
    /* 岭回归：正规方程 (XᵀX + λI) a = Xᵀy。DX=58，58×58 直接用 Cholesky 解，
       比在线 LMS 稳得多 —— 而且不会出现"训练集拟合很好、一滚就炸"。 */
    const int n = MatrixSpinal::DX;
    std::vector<double> XtX((size_t)n * n, 0.0), XtY((size_t)n * 10, 0.0);
    std::vector<double> x(n);
    for (int idx : tr) {
      MatrixSpinal::feats(S[idx].q, S[idx].dq, S[idx].ph, S[idx].u, x.data());
      for (int i = 0; i < n; ++i) {
        for (int j = 0; j < n; ++j) XtX[(size_t)i * n + j] += x[i] * x[j];
        for (int k = 0; k < 10; ++k) XtY[(size_t)i * 10 + k] += x[i] * ((double)S[idx].qn[k] - S[idx].q[k]);
      }
    }
    double tr = 0; for (int i = 0; i < n; ++i) tr += XtX[(size_t)i * n + i];
    const double lam = 1e-4 * tr / n;
    for (int i = 0; i < n; ++i) XtX[(size_t)i * n + i] += lam;
    /* Cholesky 分解 XtX = L Lᵀ */
    std::vector<double> L((size_t)n * n, 0.0);
    for (int i = 0; i < n; ++i) {
      for (int j = 0; j <= i; ++j) {
        double sum = XtX[(size_t)i * n + j];
        for (int k2 = 0; k2 < j; ++k2) sum -= L[(size_t)i * n + k2] * L[(size_t)j * n + k2];
        if (i == j) L[(size_t)i * n + j] = std::sqrt(sum > 1e-12 ? sum : 1e-12);
        else L[(size_t)i * n + j] = sum / L[(size_t)j * n + j];
      }
    }
    for (int k = 0; k < 10; ++k) {
      std::vector<double> y(n), z(n), w(n);
      for (int i = 0; i < n; ++i) y[i] = XtY[(size_t)i * 10 + k];
      for (int i = 0; i < n; ++i) { double sum = y[i]; for (int j = 0; j < i; ++j) sum -= L[(size_t)i * n + j] * z[j]; z[i] = sum / L[(size_t)i * n + i]; }
      for (int i = n - 1; i >= 0; --i) { double sum = z[i]; for (int j = i + 1; j < n; ++j) sum -= L[(size_t)j * n + i] * w[j]; w[i] = sum / L[(size_t)i * n + i]; }
      for (int i = 0; i < n; ++i) A.A[i][k] = w[i];
      A.b[k] = 0.0;
    }
  }
  /* 线性模型的闭环增益：一步预测准、滚起来发散，是因为没有回复力。
     把 A 整体缩一点，等于给"预测"加阻尼：单步误差几乎不变，长视野不发散。 */
  const double kScale = argc > 3 ? atof(argv[3]) : 1.0;
  if (kScale > 0 && kScale < 1.0) for (int i = 0; i < MatrixSpinal::DX; ++i) for (int k = 0; k < 10; ++k) A.A[i][k] *= kScale;
  printf("A scale %.2f\n", kScale);
  {
    double err = 0; int cnt2 = 0;
    std::vector<double> x(MatrixSpinal::DX);
    for (int idx : va) {
      MatrixSpinal::feats(S[idx].q, S[idx].dq, S[idx].ph, S[idx].u, x.data());
      for (int k = 0; k < 10; ++k) {
        double pred = A.b[k];
        for (int i = 0; i < MatrixSpinal::DX; ++i) pred += A.A[i][k] * x[i];
        err += std::fabs(((double)S[idx].qn[k] - S[idx].q[k]) - pred); ++cnt2;
      }
    }
    printf("holdout 单步 %.4f rad (%.2f°)\n", err / cnt2, err / cnt2 * 57.2958);
  }

  /* 闭环 rollout：运行时只用 6 步前瞻（predictHorizon(6)），所以这里也量 6 步；
     再给一个带阻尼的 24 步，用来看长视野会不会发散。 */
  auto rollout = [&](int H, float damp, const char* tag) {
    double sum = 0; int cnt = 0, worst = 0;
    for (size_t s0 = 0; s0 + H + 2 < va.size(); s0 += 1) {
      const int start = va[s0];
      float q[10], dq[10];
      for (int k = 0; k < 10; ++k) { q[k] = S[start].q[k]; dq[k] = S[start].dq[k]; }
      for (int h = 0; h < H; ++h) {
        const Sample& s2 = S[start + h];
        double x[MatrixSpinal::DX];
        MatrixSpinal::feats(q, dq, s2.ph, s2.u, x);
        for (int k = 0; k < 10; ++k) {
          double pred = A.b[k];
          for (int i = 0; i < MatrixSpinal::DX; ++i) pred += A.A[i][k] * x[i];
          const float qNext = q[k] + damp * (float)pred;
          dq[k] = (qNext - q[k]) * 60.0f;
          q[k] = qNext;
        }
      }
      for (int k = 0; k < 10; ++k) {
        const double e = std::fabs(q[k] - S[start + H].q[k]);
        sum += e; ++cnt; if (e > worst) worst = (int)e;
      }
    }
    printf("%s %d 步 rollout: 平均 %.2f°\n", tag, H, sum / cnt * 57.2958);
    return sum / cnt * 57.2958;
  };
  const double r6 = rollout(6, 1.0f, "闭环");
  rollout(24, 0.9f, "阻尼 0.9");

  {
    FILE* o = fopen(out, "w");
    if (!o) { printf("cannot write %s\n", out); return 1; }
    fprintf(o, "{\"D\":%d,\"DX\":%d,\"rmseDeg\":%.2f,\"ticks\":%d,\"src\":\"cmu-mocap\",\"A\":[", MatrixSpinal::D, MatrixSpinal::DX, r6, (int)S.size());
    for (int i = 0; i < MatrixSpinal::DX; ++i) {
      fprintf(o, "%s[", i ? "," : "");
      for (int k = 0; k < MatrixSpinal::D; ++k) fprintf(o, "%s%.6f", k ? "," : "", A.A[i][k]);
      fprintf(o, "]");
    }
    fprintf(o, "],\"b\":[");
    for (int k = 0; k < MatrixSpinal::D; ++k) fprintf(o, "%s%.6f", k ? "," : "", A.b[k]);
    fprintf(o, "]}\n");
    fclose(o);
  }
  printf("saved %s\n", out);
  return 0;
}
