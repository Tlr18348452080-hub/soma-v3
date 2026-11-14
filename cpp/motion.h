#pragma once
/* ================= 动作通道（姿态叠加，不是播放动画）=================
   关键帧只写"相对物理结果偏移多少"，不接管关节。
   所以被推的时候手仍然会甩、脚下仍然在找平衡 —— 动作在上面盖一层，不是把它顶掉。
   weight 由调度器给：刚开始 0→1 渐入，结束时 1→0 渐出，中途被打断也能平滑收掉。
*/
#include "motions_def.h"

struct MotionPlayer {
  const Motion* cur = nullptr;
  std::string name;
  float t = 0, weight = 1.0f;
  bool done = true;
  bool fading = false;
  float fadeT = 0, fadeDur = 0.15f;

  void play(const Motion& m) {
    if (cur && cur->name == m.name) return;      /* 同一个不重播，免得鬼畜 */
    cur = &m; name = m.name; t = 0; done = false; fading = false; weight = 1.0f;
  }
  void stop() { if (cur) { fading = true; fadeT = 0; } }
  void hardStop() { cur = nullptr; name.clear(); done = true; weight = 0; fading = false; }

  /* 能不能被新动作打断：播完自己的 interrupt_at 之后就可以 */
  bool interruptible() const { return !cur || t >= cur->interruptAt; }
  float progress() const { return cur ? clampf(t / (cur->duration > 1e-3f ? cur->duration : 1.0f), 0, 1) : 0; }

  void step(float dt) {
    if (!cur) return;
    t += dt;
    if (fading) {
      fadeT += dt;
      weight = 1.0f - clampf(fadeT / fadeDur, 0, 1);
      if (weight <= 0) { hardStop(); return; }
    }
    if (t >= cur->duration && !fading) { t = cur->duration; done = true; stop(); }
    if (done && !fading) hardStop();
  }

  void applyTo(V3* nodes) const {
    if (!cur || weight <= 0.001f) return;
    const auto& F = cur->frames;
    if (F.empty()) return;
    size_t i = 0;
    while (i + 1 < F.size() && F[i + 1].t <= t) ++i;
    const MotionFrame& a = F[i];
    const MotionFrame& b = F[i + 1 < F.size() ? i + 1 : i];
    float span = b.t - a.t;
    float u = span > 1e-6f ? clampf((t - a.t) / span, 0, 1) : 1.0f;
    u = u * u * (3.0f - 2.0f * u);        /* smoothstep：起止都不突兀 */
    for (int n = 0; n < 18; ++n) {
      V3 oa{0, 0, 0}, ob{0, 0, 0};
      for (auto& o : a.offs) if (o.first == n) { oa = o.second; break; }
      for (auto& o : b.offs) if (o.first == n) { ob = o.second; break; }
      V3 o = lerp(oa, ob, u);
      if (len(o) < 1e-6f) continue;
      nodes[n] += o * weight;
    }
  }
};
