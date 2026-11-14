#pragma once
/* 参数表里的"动作/道具"结构。只依赖 vec.h，不依赖 Walker，
   这样 motion.h（播放器）和 config.h（加载器）都能用，不会绕成环。*/
#include "vec.h"
#include <string>
#include <vector>
#include <utility>

struct MotionFrame {
  float t = 0;
  std::vector<std::pair<int, V3>> offs;   /* 节点号 → 相对偏移（米） */
};

struct Motion {
  std::string name;
  float duration = 1.0f, interruptAt = 1e9f, cost = 0;
  std::vector<MotionFrame> frames;
  bool loaded = false;
};

struct Prop {
  std::string id, kind = "box";
  float x0 = 0, x1 = 0, z0 = 0, z1 = 0, h = 0;   /* 方块型 */
  float x = 0, z = 0, w = 0.2f, d = 0.2f;        /* 点状道具（食物/水/座位） */
  float kcal = 0, bite = 0, ml = 0, facing = 0;
  bool wall() const { return h > 0.24f; }
  bool solid() const { return kind == "platform" || kind == "wall" || kind == "seat"; }
};

struct SomaCfg {
  bool ok = false;
  std::vector<Motion> motions;                 /* 按名字线性找，数量少，够用 */
  std::vector<Prop> props;
  float muFoot = 0.8f, muBody = 0.9f;
  float brainWait = 3.0f;
  float bx0 = -20, bx1 = 20, bz0 = -6, bz1 = 6;

  const Motion* find(const char* n) const {
    for (auto& m : motions) if (m.name == n) return &m;
    return nullptr;
  }
};
