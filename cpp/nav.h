/* ================= 屋里怎么走：格子 + A* =================
   3 室 1 厅之后，"朝目标走直线"不够用了：客厅正中间站着沙发和茶几，
   从东边去门口必须往南绕。靠"撞上就侧一下"只能贴着家具磨。

   这里是最小可用的一套：
   - 0.20 m 一格，格子中心落在任何家具/墙（h > 0.18）里就算挡；
   - 障碍按她的身体半径 0.30 m 外扩，走出来的路她真的过得去；
   - A* 四邻 + 一点转向代价，避免锯齿；
   - 取路径上离她最远的"还能看见"的点（视线用格子采样判），拐角就不会一格一拐。

   60 Hz 里不每帧重算：目标变了或者过了 0.6 s 才重算一次。
*/
#pragma once
#include <vector>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include "vec.h"
#include "room.h"

struct NavGrid {
  float x0 = -4.0f, z0 = -3.30f, cell = 0.20f;
  int nx = 0, nz = 0;
  std::vector<uint8_t> blk;
  bool ready = false;

  void build(const RoomColl& rc, float r) {
    x0 = -4.05f; z0 = -3.30f; cell = 0.20f;
    nx = (int)std::lround((6.40f - x0) / cell);   /* +x 撑到门外走廊尽头 */
    nz = (int)std::lround((9.70f - z0) / cell);
    blk.assign((size_t)nx * nz, 0);
    for (int iz = 0; iz < nz; ++iz) {
      for (int ix = 0; ix < nx; ++ix) {
        const float x = x0 + (ix + 0.5f) * cell;
        const float z = z0 + (iz + 0.5f) * cell;
        uint8_t b = 0;
        for (const auto& bx : rc.b) {
          if (bx.h <= 0.18f) continue;                 /* 矮东西（地毯、淋浴地盘）不算挡路 */
          if (x > bx.x0 - r && x < bx.x1 + r && z > bx.z0 - r && z < bx.z1 + r) { b = 1; break; }
        }
        blk[(size_t)iz * nx + ix] = b;
      }
    }
    ready = true;
  }
  int ix(float x) const { int i = (int)((x - x0) / cell); return i < 0 ? 0 : (i >= nx ? nx - 1 : i); }
  int iz(float z) const { int i = (int)((z - z0) / cell); return i < 0 ? 0 : (i >= nz ? nz - 1 : i); }
  bool freeCell(int ix, int iz) const {
    if (ix < 0 || iz < 0 || ix >= nx || iz >= nz) return false;
    return blk[(size_t)iz * nx + ix] == 0;
  }
  /* 世界坐标这一点她能不能站（格子被挡就算不能） */
  bool freeAt(float x, float z) const { return ready && freeCell(ix(x), iz(z)); }

  /* 两点之间直线能不能走：沿线段每半格采样 */
  bool los(float ax, float az, float bx, float bz) const {
    if (!ready) return false;
    const float dx = bx - ax, dz = bz - az;
    const float d = std::sqrt(dx * dx + dz * dz);
    const int n = (int)(d / (cell * 0.5f)) + 1;
    for (int i = 0; i <= n; ++i) {
      const float u = (float)i / (float)n;
      if (!freeAt(ax + dx * u, az + dz * u)) return false;
    }
    return true;
  }

  /* A*：返回路径上"现在就该去的那个点"（最远的可见点）。无路可走返回 false。 */
  bool next(float ax, float az, float bx, float bz, float& ox, float& oz) {
    if (!ready) return false;
    int sx = ix(ax), sz = iz(az), tx = ix(bx), tz = iz(bz);
    if (!freeCell(tx, tz)) {
      /* 目标格子被挡：就近找一个能站的格子，别整条路都作废 */
      int best = -1; float bd = 1e9f;
      for (int dz2 = -5; dz2 <= 5; ++dz2) for (int dx2 = -5; dx2 <= 5; ++dx2) {
        const int cxx = tx + dx2, czz = tz + dz2;
        if (!freeCell(cxx, czz)) continue;
        const float dd = (float)(dx2 * dx2 + dz2 * dz2);
        if (dd < bd) { bd = dd; best = czz * nx + cxx; }
      }
      if (best < 0) {
#ifdef NAV_DEBUG
        fprintf(stderr, "[nav] 目标周围都是障碍 t=(%d,%d)\n", tx, tz);
#endif
        return false;
      }
      tx = best % nx; tz = best / nx;
    }
    if (!freeCell(sx, sz)) {
      /* 她这会儿正贴着家具（身体半径压进外扩区）：先给一个最近的空格让她挪出去，
         而不是直接放弃 —— 放弃就等于她站在角落里再也收不到路。 */
      int bestc = -1; float bd = 1e9f;
      for (int dz2 = -4; dz2 <= 4; ++dz2) for (int dx2 = -4; dx2 <= 4; ++dx2) {
        const int cxx = sx + dx2, czz = sz + dz2;
        if (!freeCell(cxx, czz)) continue;
        const float dd = (float)(dx2 * dx2 + dz2 * dz2);
        if (dd < bd) { bd = dd; bestc = czz * nx + cxx; }
      }
      if (bestc < 0) return false;
      ox = x0 + ((bestc % nx) + 0.5f) * cell;
      oz = z0 + ((bestc / nx) + 0.5f) * cell;
#ifdef NAV_DEBUG
      fprintf(stderr, "[nav] 起点在障碍里 (%d,%d)，先去 (%.2f,%.2f)\n", sx, sz, ox, oz);
#endif
      return true;
    }
    if (sx == tx && sz == tz) {
      /* 起点和目标落在同一格：回格子中心，别回原目标（原目标可能压在家具里） */
      ox = x0 + (tx + 0.5f) * cell; oz = z0 + (tz + 0.5f) * cell;
      return true;
    }

    const int N = nx * nz;
    std::vector<float> g(N, 1e9f);
    std::vector<int> from(N, -1);
    std::vector<uint8_t> closed(N, 0);
    std::vector<int> heap; heap.reserve(512);
    auto push = [&](int id, float f) {
      heap.push_back(id);
      int i = (int)heap.size() - 1;
      while (i > 0) {
        const int p = (i - 1) / 2;
        if (g[heap[p]] + 0.0f <= g[heap[i]] + 0.0f) break;
        std::swap(heap[p], heap[i]); i = p;
      }
      (void)f;
    };
    (void)push;
    /* 格子少（约 2300 个），直接用"每轮扫一遍最小值"的朴素 Dijkstra，够快也够短 */
    const int s0 = sz * nx + sx, t0 = tz * nx + tx;
    g[s0] = 0.0f;
    for (int iter = 0; iter < N; ++iter) {
      int cur = -1; float bd = 1e9f;
      for (int i = 0; i < N; ++i) if (!closed[i] && g[i] < bd) { bd = g[i]; cur = i; }
      if (cur < 0) break;
      if (cur == t0) break;
      closed[cur] = 1;
      const int cx = cur % nx, cz = cur / nx;
      const int ox2[4] = {1, -1, 0, 0}, oz2[4] = {0, 0, 1, -1};
      for (int k = 0; k < 4; ++k) {
        const int nxx = cx + ox2[k], nzz = cz + oz2[k];
        if (!freeCell(nxx, nzz)) continue;
        const int id = nzz * nx + nxx;
        /* 转弯加一点代价，路径不至于一格一折 */
        float turn = 0.0f;
        if (from[cur] >= 0) {
          const int px = from[cur] % nx, pz = from[cur] / nx;
          if ((nxx - cx) != (cx - px) || (nzz - cz) != (cz - pz)) turn = 0.12f;
        }
        const float ng = g[cur] + 1.0f + turn;
        if (ng < g[id]) { g[id] = ng; from[id] = cur; }
      }
    }
    if (g[t0] > 1e8f) {
#ifdef NAV_DEBUG
      int reach = 0;
      for (int i = 0; i < N; ++i) if (g[i] < 1e8f) ++reach;
      fprintf(stderr, "[nav] 无路 s=(%d,%d) t=(%d,%d) 到达格 %d\n", sx, sz, tx, tz, reach);
#endif
      return false;
    }
    /* 回溯成路点（世界坐标） */
    std::vector<int> path;
    for (int c = t0; c >= 0; c = from[c]) { path.push_back(c); if (c == s0) break; }
    if (path.empty() || path.back() != s0) return false;
    std::vector<V3> pts;
    for (int i = (int)path.size() - 1; i >= 0; --i) {
      const int c = path[i];
      pts.push_back(V3{x0 + ((c % nx) + 0.5f) * cell, 0.0f, z0 + ((c / nx) + 0.5f) * cell});
    }
    /* 取最远的可见点：视线穿过格子判，天然避开拐角擦墙 */
    int best = 0;
    for (int i = (int)pts.size() - 1; i >= 1; --i) {
      if (los(ax, az, pts[i].x, pts[i].z)) { best = i; break; }
    }
    if (best == 0) best = (pts.size() > 1) ? 1 : 0;
    ox = pts[best].x; oz = pts[best].z;
    return true;
  }
};
