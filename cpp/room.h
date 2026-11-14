/* ---------- 屋子：碰撞盒 + 触觉神经末梢 ----------
   为什么要有这一层：
   1) 物理只知道自己站在地上、边界是个矩形，家具对她来说不存在 —— 所以她直接穿过床和桌子。
      用户看到的"穿模"最严重的一类就是这个（不是 VRM 蒙皮穿，是她整个身体插进家具）。
   2) 触觉不能凭空造：得先有"她在哪、碰到什么、碰得多重"，才谈得上"被摸到有什么感觉"。
      所以这里做两件事：AABB 推挤（不穿进去）+ 接触点转成"神经末梢被压了多少"。

   碰撞盒来自 web/room.js（`assets/room.json`），家具坐标和画面上摆的是同一份，
   不是我在 C++ 里重新编一遍 —— 画面和物理必须是同一个屋子。 */
#pragma once
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include "vec.h"

struct RoomColl {
  struct Box { float x0, x1, z0, z1, h; };
  std::vector<Box> b;

  bool load(const char* path) {
    FILE* f = std::fopen(path, "rb");
    if (!f) return false;
    std::string s;
    char buf[4096];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) s.append(buf, n);
    std::fclose(f);
    b.clear();
    /* 只需要把 {"x0":..,"x1":..,"z0":..,"z1":..,"h":..} 里的数抠出来 ——
       这份 json 是我们自己导出的，字段顺序固定，不做通用解析。 */
    const char* p = s.c_str();
    while ((p = std::strstr(p, "\"x0\"")) != nullptr) {
      Box bx{0, 0, 0, 0, 0};
      const char* q = p;
      auto numAfter = [&](const char* key) -> float {
        const char* k = std::strstr(q, key);
        if (!k) return 0.0f;
        k = std::strchr(k, ':');
        if (!k) return 0.0f;
        return (float)std::atof(k + 1);
      };
      bx.x0 = numAfter("\"x0\""); bx.x1 = numAfter("\"x1\"");
      bx.z0 = numAfter("\"z0\""); bx.z1 = numAfter("\"z1\"");
      bx.h  = numAfter("\"h\"");
      if (bx.x1 > bx.x0 && bx.z1 > bx.z0) b.push_back(bx);
      p += 4;
    }
    return !b.empty();
  }

  /* 圆（半径 r）和盒子在水平面的穿透：返回穿透深度，法线写进 nx/nz。
     y 高于盒子高度就不算碰（能从桌面上面过去）。 */
  float contact(float x, float z, float y, float r, float& nx, float& nz, int* which = nullptr) const {
    float best = 0.0f;
    nx = nz = 0.0f;
    for (size_t i = 0; i < b.size(); ++i) {
      const Box& bx = b[i];
      if (y > bx.h + 0.02f) continue;                 /* 在上方：跨过去了 */
      const float cx = clampf(x, bx.x0, bx.x1);
      const float cz = clampf(z, bx.z0, bx.z1);
      const float dx = x - cx, dz = z - cz;
      const float d2 = dx * dx + dz * dz;
      if (d2 > r * r) continue;
      float d = std::sqrt(d2);
      float depth = r - d;
      /* 圆心已经在盒子里面：沿最近的边推出去，比"从中心推"稳 */
      if (d < 1e-5f) {
        const float dl = x - bx.x0, dr = bx.x1 - x, db = z - bx.z0, df = bx.z1 - z;
        const float m = std::min(std::min(dl, dr), std::min(db, df));
        if (m == dl)      { nx = -1; nz = 0; depth = dl + r; }
        else if (m == dr) { nx =  1; nz = 0; depth = dr + r; }
        else if (m == db) { nx = 0; nz = -1; depth = db + r; }
        else              { nx = 0; nz =  1; depth = df + r; }
      } else {
        nx = dx / d; nz = dz / d;
      }
      if (depth > best) { best = depth; if (which) *which = (int)i; }
    }
    return best;
  }
};

/* ---------- 触觉：固定的几处神经末梢 ----------
   用户的要求是：不必每个部位都写，写"固定方位 + 力度 + 感知程度"，有多个就多行。
   所以每一条就是：贴着哪根骨、往哪个方向偏多少、碰到多大面积算"压到了"、
   那里有多少末梢、她有多敏感。力=穿透深度×硬度（再乘上撞上去的速度）。
   数字不是瞎写的：手掌末梢最密、脚底次之、后背最钝，这些比例来自皮肤的慢/快适应感受器密度
   （指尖 ~140/cm²、手掌 ~60、前臂 ~20、背 ~10），这里按"相对密度"落到 0..1。 */
struct TouchSite {
  const char* name;
  int node;        /* 贴哪根骨（节点号） */
  float ox, oy, oz;/* 相对该节点的固定方位（米） */
  float r;         /* 接触半径：多大的东西碰到才算 */
  float nerves;    /* 末梢数（相对量，用来算"同时压到多少"） */
  float sens;      /* 敏感度 0..1 */
};

static const TouchSite TOUCH_SITES[] = {
  /* 手掌：最灵。前后各一个点，够描述"被握住"和"手背被碰" */
  {"palm.L",  6, 0.00f,  0.00f,  0.02f, 0.055f, 1.00f, 0.95f},
  {"palm.R",  9, 0.00f,  0.00f,  0.02f, 0.055f, 1.00f, 0.95f},
  {"knuckle.L", 6, 0.00f, -0.02f, -0.03f, 0.045f, 0.55f, 0.70f},
  {"knuckle.R", 9, 0.00f, -0.02f, -0.03f, 0.045f, 0.55f, 0.70f},
  /* 前臂 / 肘：中等 */
  {"forearm.L", 5, 0.00f, 0.00f, 0.00f, 0.050f, 0.45f, 0.55f},
  {"forearm.R", 8, 0.00f, 0.00f, 0.00f, 0.050f, 0.45f, 0.55f},
  /* 肩、上臂：日常会蹭到门框、柜子 */
  {"shoulder.L", 4, 0.02f, 0.02f, 0.00f, 0.070f, 0.55f, 0.45f},
  {"shoulder.R", 7, 0.02f, 0.02f, 0.00f, 0.070f, 0.55f, 0.45f},
  /* 躯干：胸（贴着人时最要紧）、后腰（靠墙） */
  {"chest",   1, 0.00f,  0.05f, 0.09f, 0.110f, 1.40f, 0.75f},
  {"waist",   0, 0.00f,  0.02f, -0.10f, 0.110f, 0.90f, 0.50f},
  /* 膝、脚：跪/蹲/踢到东西 */
  {"knee.L", 11, 0.00f, -0.02f, 0.05f, 0.060f, 0.60f, 0.60f},
  {"knee.R", 15, 0.00f, -0.02f, 0.05f, 0.060f, 0.60f, 0.60f},
  /* 小腿：最容易被矮家具（茶几 0.42、床沿 0.60）穿过去的一段，
     没有这一条就查不到"腿插进桌子里" */
  {"shin.L", 11, 0.00f, -0.26f, 0.00f, 0.055f, 0.50f, 0.50f},
  {"shin.R", 15, 0.00f, -0.26f, 0.00f, 0.055f, 0.50f, 0.50f},
  {"sole.L", 12, 0.00f, -0.04f, 0.00f, 0.070f, 0.85f, 0.65f},
  {"sole.R", 16, 0.00f, -0.04f, 0.00f, 0.070f, 0.85f, 0.65f},
  /* 头、颈：被摸头 / 撞到头 */
  {"head",    3, 0.00f,  0.08f, 0.00f, 0.090f, 0.70f, 0.80f},
  {"neck",    2, 0.00f,  0.02f, 0.00f, 0.060f, 0.40f, 0.75f},
};
static const int N_TOUCH = (int)(sizeof(TOUCH_SITES) / sizeof(TOUCH_SITES[0]));
