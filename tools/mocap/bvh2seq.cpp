#include "walker.h"
/* BVH(CMU) → 我们的 10 个关角度 → CSV。走的是同一套 measure()，所以和运行时的约定一致。 */
#include "spinal_core.h"
#include <cstdio>
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <string>
#include <vector>
#include <map>
#include <array>

struct Joint { std::string name; int parent = -1; int chan[6] = {0,0,0,0,0,0}; int nch = 0;
               double off[3] = {0,0,0}; int cbase = 0; };

static void rotMat(const double r[3], const int order[3], int n, double M[3][3]) {
  double m[3][3] = {{1,0,0},{0,1,0},{0,0,1}};
  for (int k = 0; k < n; ++k) {
    const int ax = order[k];
    const double a = r[ax] * 3.14159265358979 / 180.0, c = std::cos(a), s = std::sin(a);
    double R[3][3] = {{1,0,0},{0,1,0},{0,0,1}};
    if (ax == 0) { R[1][1] = c; R[1][2] = -s; R[2][1] = s; R[2][2] = c; }
    if (ax == 1) { R[0][0] = c; R[0][2] = s; R[2][0] = -s; R[2][2] = c; }
    if (ax == 2) { R[0][0] = c; R[0][1] = -s; R[1][0] = s; R[1][1] = c; }
    double T[3][3];
    for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) { T[i][j] = 0;
      for (int l = 0; l < 3; ++l) T[i][j] += m[i][l] * R[l][j]; }
    memcpy(m, T, sizeof m);
  }
  memcpy(M, m, sizeof m);
}

int main(int argc, char** argv) {
  FILE* out = fopen(argc > 2 ? argv[1] : "/tmp/pose_seq.csv", "w");
  char posePath[1024];
  snprintf(posePath, sizeof posePath, "%s.pose", argc > 2 ? argv[1] : "/tmp/pose_seq.csv");
  FILE* pout = fopen(posePath, "w");   /* 用法: bvh2seq <out.csv> <a.bvh> ... */
  for (int ai = 2; ai < argc; ++ai) {
    FILE* f = fopen(argv[ai], "r");
    if (!f) { printf("skip %s\n", argv[ai]); continue; }
    std::vector<Joint> js;
    char line[4096];
    int mode = 0; int nframes = 0; double ftime = 1.0 / 120.0;
    std::vector<int> stack;
    while (fgets(line, sizeof line, f)) {
      if (strstr(line, "MOTION")) { mode = 1; continue; }
      if (mode == 0) {
        char name[128];
        if (sscanf(line, " ROOT %127s", name) == 1 || sscanf(line, " JOINT %127s", name) == 1 ||
            sscanf(line, "ROOT %127s", name) == 1 || sscanf(line, "JOINT %127s", name) == 1) {
          Joint j; j.name = name;
          j.parent = stack.empty() ? -1 : stack.back();
          js.push_back(j); stack.push_back((int)js.size() - 1);
          continue;
        }
        if (strstr(line, "End Site")) { stack.push_back(stack.empty() ? -1 : stack.back()); continue; }
        if (strchr(line, '}')) { if (!stack.empty()) stack.pop_back(); continue; }
        double a, b, c;
        if (sscanf(line, " OFFSET %lf %lf %lf", &a, &b, &c) == 3 || sscanf(line, "OFFSET %lf %lf %lf", &a, &b, &c) == 3) {
          if (!stack.empty()) { js[stack.back()].off[0] = a; js[stack.back()].off[1] = b; js[stack.back()].off[2] = c; }
          continue;
        }
        char ch[512];
        if (strstr(line, "CHANNELS")) {
          const char* p = strstr(line, "CHANNELS");
          int n = atoi(p + 9);
          p = strchr(p, ' ');
          std::vector<int> order;
          for (int k = 0; k < n; ++k) {
            p = strchr(p + 1, ' ');
            if (!p) break;
            if (p[1] == 'X') order.push_back(0);
            else if (p[1] == 'Y') order.push_back(1);
            else order.push_back(2);
          }
          if (!stack.empty()) {
            Joint& j = js[stack.back()];
            j.nch = n;
            for (int k = 0; k < 3; ++k) j.chan[k] = order[k];
            j.chan[3] = (int)order.size() > 3 ? order[3] : 0;
          }
          continue;
        }
      } else {
        if (strstr(line, "Frames:")) { nframes = atoi(strchr(line, ':') + 1); continue; }
        if (strstr(line, "Frame Time:")) { ftime = atof(strchr(line, ':') + 1); continue; }
        break;
      }
    }
    /* 通道基址 */
    int cb = 0;
    for (auto& j : js) { j.cbase = cb; cb += j.nch; }
    /* 名字 → 节点 */
    std::map<std::string, int> idx;
    for (size_t i = 0; i < js.size(); ++i) idx[js[i].name] = (int)i;
    const char* need[] = {"Hips", "Spine1", "Neck", "Head", "LeftArm", "LeftForeArm", "LeftHand",
                          "RightArm", "RightForeArm", "RightHand", "LeftUpLeg", "LeftLeg", "LeftFoot", "LeftToeBase",
                          "RightUpLeg", "RightLeg", "RightFoot", "RightToeBase"};
    bool ok = true;
    for (auto n : need) if (!idx.count(n)) { ok = false; printf("%s missing %s\n", argv[ai], n); break; }
    if (!ok) { fclose(f); continue; }

    std::vector<std::vector<double>> rows;
    std::vector<double> vals(cb);
    int got = 0;
    while (fgets(line, sizeof line, f) && got < nframes) {
      char* p = line;
      for (int k = 0; k < cb; ++k) vals[k] = strtod(p, &p);
      rows.push_back(vals);
      ++got;
    }
    fclose(f);
    if (rows.size() < 60) { printf("short %s\n", argv[ai]); continue; }

    /* FK */
    std::vector<double*> pos(js.size(), nullptr);
    std::vector<double> buf(js.size() * 3);
    auto fk = [&](const std::vector<double>& r) {
      std::vector<std::array<double,9>> R(js.size());   /* 3x3 行优先，别写成 3 个元素的数组 */
      for (size_t i = 0; i < js.size(); ++i) {
        double rr[3] = {0,0,0};
        int order[3] = {js[i].chan[0], js[i].chan[1], js[i].chan[2]};
        int base = js[i].cbase;
        int nrot = js[i].nch > 3 ? 3 : js[i].nch;
        int off = js[i].nch > 3 ? 3 : 0;      /* root 前 3 个是位移 */
        for (int k = 0; k < nrot; ++k) rr[order[k]] = r[base + off + k];
        double M[3][3];
        rotMat(rr, order, nrot, M);
        double parentR[3][3] = {{1,0,0},{0,1,0},{0,0,1}};
        double px = 0, py = 0, pz = 0;
        if (js[i].parent >= 0) {
          const int pi = js[i].parent;
          parentR[0][0] = R[pi][0]; parentR[0][1] = R[pi][1]; parentR[0][2] = R[pi][2];
          parentR[1][0] = R[pi][3]; parentR[1][1] = R[pi][4]; parentR[1][2] = R[pi][5];
          parentR[2][0] = R[pi][6]; parentR[2][1] = R[pi][7]; parentR[2][2] = R[pi][8];
          px = buf[pi * 3]; py = buf[pi * 3 + 1]; pz = buf[pi * 3 + 2];
        }
        double ox = js[i].off[0], oy = js[i].off[1], oz = js[i].off[2];
        if (js[i].parent < 0) { ox += r[0]; oy += r[1]; oz += r[2]; }
        double wx = px + parentR[0][0] * ox + parentR[0][1] * oy + parentR[0][2] * oz;
        double wy = py + parentR[1][0] * ox + parentR[1][1] * oy + parentR[1][2] * oz;
        double wz = pz + parentR[2][0] * ox + parentR[2][1] * oy + parentR[2][2] * oz;
        buf[i * 3] = wx; buf[i * 3 + 1] = wy; buf[i * 3 + 2] = wz;
        for (int a = 0; a < 3; ++a) for (int b2 = 0; b2 < 3; ++b2) {
          double acc = 0;
          for (int l = 0; l < 3; ++l) acc += parentR[a][l] * M[l][b2];
          R[i][a * 3 + b2] = acc;
        }
      }
    };
    std::vector<float> qs(rows.size() * 10), speeds(rows.size());
    std::vector<double> lY(rows.size(), 0.0), rY(rows.size(), 0.0);
    double metersPerUnit = 0.01;   /* 由 t=0 的腿长定出来；CMU 的单位不是 cm，不能硬除 100 */
    std::vector<float> poses(rows.size() * 12, 0.0f);   /* 手L 肘L 手R 肘R，各自 (fwd,up,left) 米 */
    for (size_t t = 0; t < rows.size(); ++t) {
      fk(rows[t]);
      Frame fr;
      const char* order18[] = {"Hips","Spine1","Neck","Head","LeftArm","LeftForeArm","LeftHand",
                               "RightArm","RightForeArm","RightHand","LeftUpLeg","LeftLeg","LeftFoot","LeftToeBase",
                               "RightUpLeg","RightLeg","RightFoot","RightToeBase"};
      for (int i = 0; i < 18; ++i) {
        const int j = idx[order18[i]];
        fr.nodes[i] = V3{(float)(buf[j*3]/100.0), (float)(buf[j*3+1]/100.0), (float)(buf[j*3+2]/100.0)};
      }
      float q[SpinalCore::D];
      SpinalCore::measure(fr, q);
      {   /* 手臂偏移写进 hips 坐标系：这样和世界朝向无关。
             CMU 的骨架比例不是米（髋高约 0.9 单位），先按跑步机身高归一化到 1.0 m。 */
        static double bodyScale = 1.0;
        /* 归一化必须用**同一条运动链**里的长度：CMU 的 OFFSET（骨长）和通道值不是同一套单位，
           拿根节点的 y 当身高会得到 15 倍偏小的肢体。用髋→踝的垂直距离（全是 OFFSET 累出来的）。 */
        if (t == 0) {
          const double hipY = fr.nodes[0].y;
          const double ankY = 0.5 * (fr.nodes[12].y + fr.nodes[16].y);
          const double legLen = hipY - ankY;
          bodyScale = (legLen > 0.05) ? 0.92 / legLen : 1.0;
          metersPerUnit = bodyScale / 100.0;   /* fr 单位 = buf/100，所以每 raw 单位这么多米 */
        }
        auto sub = [&](int a, int b) { return V3{fr.nodes[a].x - fr.nodes[b].x, fr.nodes[a].y - fr.nodes[b].y, fr.nodes[a].z - fr.nodes[b].z}; };
        auto cross = [](V3 a, V3 b) { return V3{a.y*b.z - a.z*b.y, a.z*b.x - a.x*b.z, a.x*b.y - a.y*b.x}; };
        auto norm = [](V3 a) { float l = std::sqrt(a.x*a.x + a.y*a.y + a.z*a.z) + 1e-6f; return V3{a.x/l, a.y/l, a.z/l}; };
        auto dot = [](V3 a, V3 b) { return a.x*b.x + a.y*b.y + a.z*b.z; };
        const V3 up = norm(sub(1, 0));
        const V3 left = norm(sub(10, 14));          /* 左髋 − 右髋 = 身体左侧 */
        const V3 fwd = norm(cross(up, left));
        auto put = [&](int slot, int node, int sh) {
          const V3 d = sub(node, sh);
          poses[t*12 + slot*3 + 0] = (float)(dot(d, fwd) * bodyScale);
          poses[t*12 + slot*3 + 1] = (float)(dot(d, up) * bodyScale);
          poses[t*12 + slot*3 + 2] = (float)(dot(d, left) * bodyScale);
        };
        put(0, 6, 4); put(1, 5, 4);   /* 左手/左肘 相对左肩 */
        put(2, 9, 7); put(3, 8, 7);   /* 右手/右肘 相对右肩 */
        lY[t] = (double)fr.nodes[12].y;   /* 左脚踝高度 */
        rY[t] = (double)fr.nodes[16].y;   /* 右脚踝高度 */
      }
      for (int k = 0; k < 10; ++k) qs[t * 10 + k] = q[k];
      if (t > 0) {
        const double dx = (buf[0] - buf[0]);      /* 根位移在 r[0..2]，下面单独取 */
        (void)dx;
      }
      speeds[t] = 0;
    }
    /* 速度：根位移差分 */
    const int stride = (int)std::lround((1.0 / 60.0) / ftime) < 1 ? 1 : (int)std::lround((1.0 / 60.0) / ftime);
    for (size_t t = 1; t < rows.size(); ++t) {
      const double dx = rows[t][0] - rows[t-1][0], dz = rows[t][2] - rows[t-1][2];
      speeds[t] = (float)(std::sqrt(dx*dx + dz*dz) * metersPerUnit / ftime);
    }
    /* 相位：用左髋角（q0）的均值上穿 */
    double mean = 0; for (size_t t = 0; t < rows.size(); ++t) mean += qs[t*10];
    mean /= rows.size();
    std::vector<size_t> cross;
    for (size_t t = 1; t < rows.size(); ++t)
      if (qs[(t-1)*10] <= mean && qs[t*10] > mean) cross.push_back(t);
    /* 没有检测到步态周期的段也别丢：给一个匀速相位兜底 */
    if (cross.size() < 2) {
      const double T = 1.10;
      for (size_t t = 0; t + stride < rows.size(); t += stride) {
        const double ph = std::fmod((double)t * ftime / T, 1.0);
        double q[10], dq[10];
        for (int k = 0; k < 10; ++k) q[k] = qs[t*10+k];
        for (int k = 0; k < 10; ++k) dq[k] = (qs[(t+stride)*10+k] - qs[t*10+k]) * 60.0;
        for (int k = 0; k < 10; ++k) dq[k] = std::fmax(-6.0, std::fmin(6.0, dq[k]));
        fprintf(out, "%f,%f", (double)speeds[t] / 2.0, ph);
        for (int k = 0; k < 10; ++k) fprintf(out, ",%f", q[k]);
        for (int k = 0; k < 10; ++k) fprintf(out, ",%f", dq[k]);
        for (int k = 0; k < 10; ++k) fprintf(out, ",%f", qs[(t+stride)*10+k]);
        fprintf(out, "\n");
        if (pout) { fprintf(pout, "%f,%f,%f", (double)speeds[t] / 2.0, std::sin(6.283185307179586 * ph), std::cos(6.283185307179586 * ph));
          for (int k = 0; k < 12; ++k) fprintf(pout, ",%f", poses[t*12+k]); fprintf(pout, "\n"); }
      }
      printf("%s: %zu frames, 无周期（兜底相位）\n", argv[ai], rows.size());
      continue;
    }
    for (size_t i = 1; i < cross.size(); ++i) {
      const size_t a = cross[i-1], b2 = cross[i];
      for (size_t t = a; t + (size_t)stride < b2 && t + stride < rows.size(); t += stride) {
        const double ph = (double)(t - a) / (double)(b2 - a);
        double q[10], dq[10];
        for (int k = 0; k < 10; ++k) q[k] = qs[t*10+k];
        for (int k = 0; k < 10; ++k) dq[k] = (qs[(t+stride)*10+k] - qs[t*10+k]) * 60.0;
        fprintf(out, "%f,%f", (double)speeds[t] / 2.0, ph);
        for (int k = 0; k < 10; ++k) fprintf(out, ",%f", q[k]);
        for (int k = 0; k < 10; ++k) fprintf(out, ",%f", dq[k]);
        for (int k = 0; k < 10; ++k) fprintf(out, ",%f", qs[(t+stride)*10+k]);
        fprintf(out, "\n");
        if (pout) { fprintf(pout, "%f,%f,%f", (double)speeds[t] / 2.0, std::sin(6.283185307179586 * ph), std::cos(6.283185307179586 * ph));
          for (int k = 0; k < 12; ++k) fprintf(pout, ",%f", poses[t*12+k]); fprintf(pout, "\n"); }
      }
    }
    /* 真实步态的四个数：周期、步幅、抬脚高度、速度。
       拿它跟 walker 的 cycleT / stepHeight 公式对，才能知道她的步态像不像人。 */
    {
      /* 每只脚各自的最低点＝它自己的"地面"，抬脚高度按它自己的摆幅算 */
      const double lmin = *std::min_element(lY.begin(), lY.end());
      const double rmin = *std::min_element(rY.begin(), rY.end());
      std::vector<double> Ts, Ls, Hs, Vs;
      for (size_t i = 1; i < cross.size(); ++i) {
        const size_t a = cross[i-1], b2 = cross[i];
        if (b2 <= a + stride * 4) continue;
        Ts.push_back((double)(b2 - a) * ftime);
        const double dx = rows[b2][0] - rows[a][0], dz = rows[b2][2] - rows[a][2];
        Ls.push_back(std::sqrt(dx*dx + dz*dz) * metersPerUnit);
        double h = 0, vsum = 0;
        for (size_t t = a; t < b2; ++t) {
          h = std::fmax(h, std::fmax(lY[t] - lmin, rY[t] - rmin) * metersPerUnit);
          vsum += speeds[t];
        }
        Hs.push_back(h);
        Vs.push_back(vsum / (double)(b2 - a));
      }
      if (!Ts.empty()) {
        auto med = [](std::vector<double> v) { std::sort(v.begin(), v.end()); return v[v.size()/2]; };
        printf("STAT %s T=%.3f stride=%.3f stepH=%.3f v=%.3f n=%zu\n", argv[ai], med(Ts), med(Ls), med(Hs), med(Vs), Ts.size());
      }
    }
    printf("%s: %zu frames, %zu cycles\n", argv[ai], rows.size(), cross.size());
  }
  fclose(out);
  if (pout) fclose(pout);
  return 0;
}
