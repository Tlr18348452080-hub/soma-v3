#pragma once
/* ================= 感知 / 内脏 / 碰撞 =================
   这一层只做三件事，不做判断（判断是大脑的）：
     1) 把身体外面的东西变成数字：看没看见、听没听见、碰到没有、碰得多重；
     2) 内脏跟着代谢走：心率、呼吸率（1 Hz 级慢变量，喂回生物层）；
     3) 碰撞：她的胶囊 vs 主角的胶囊，算出"重叠多少、往哪边推"。
   参数全部来自 assets/soma.json 的 senses / colliders / body.organs。 */
#include <cmath>
#include <algorithm>
#include "walker.h"   /* V3 / Frame */

struct SenseCfg {
  float fovDeg = 210.0f, visRange = 25.0f;
  float hearRange = 18.0f;
  float painN = 180.0f, withdrawN = 90.0f, tickleN = 8.0f;
  float gainNeck = 1.8f, gainPalm = 1.4f, gainWaist = 1.6f, gainFoot = 1.2f, gainFace = 1.5f;
  /* 本体感觉：脊髓量关节角时的那点噪声（真值来自 soma.json 的 proprioception 段） */
  float proprioNoiseDeg = 0.6f, proprioDriftDeg = 1.2f;
};

struct OrganCfg {
  float restBpm = 58, maxBpm = 186, tauHeartUp = 3.5f, tauHeartDown = 12.0f, reserve = 3.2f;
  float restRpm = 12, maxRpm = 42, tauLungUp = 4.0f, tauLungDown = 10.0f;
};

struct PlayerState {
  float x = 0, z = 0, yaw = 0, speed = 0;
  bool valid = false;
};

struct OrganState {
  float hr = 58, br = 12;      /* 心率 bpm、呼吸 次/分 */
  float oxygen = 1.0f;         /* 0..1，长时间高心率会掉 */
  void update(float metabW, float bmrW, float dt, const OrganCfg& C) {
    const float ex = std::max(0.0f, std::min(1.0f, (metabW - bmrW) / (C.reserve * bmrW + 1e-3f)));
    const float hrT = C.restBpm + (C.maxBpm - C.restBpm) * ex;
    const float brT = C.restRpm + (C.maxRpm - C.restRpm) * ex;
    const float kh = 1.0f - std::exp(-dt / (hrT > hr ? C.tauHeartUp : C.tauHeartDown));
    const float kb = 1.0f - std::exp(-dt / (brT > br ? C.tauLungUp : C.tauLungDown));
    hr += (hrT - hr) * kh;
    br += (brT - br) * kb;
    /* 心率长期压在 85% 以上，氧债开始显形；休息时会慢慢还回来 */
    const float load = std::max(0.0f, (hr - 0.85f * C.maxBpm) / (0.15f * C.maxBpm));
    oxygen += ((1.0f - 0.35f * load) - oxygen) * (1.0f - std::exp(-dt / 6.0f));
    oxygen = std::max(0.55f, std::min(1.0f, oxygen));
  }
};

struct Senses {
  float dist = 99, bearing = 0;
  bool sees = false, heard = false;
  float touchN = 0, touchGain = 0;
  float headYaw = 0, headPitch = 0;
  float pushX = 0, pushZ = 0;   /* 碰撞推开的位移，主循环去应用 */

  void reset() { touchN = 0; touchGain = 0; pushX = pushZ = 0; }

  static float segSegDist(const V3& p1, const V3& q1, const V3& p2, const V3& q2) {
    const V3 d1{q1.x - p1.x, q1.y - p1.y, q1.z - p1.z};
    const V3 d2{q2.x - p2.x, q2.y - p2.y, q2.z - p2.z};
    const V3 r{p1.x - p2.x, p1.y - p2.y, p1.z - p2.z};
    const float a = d1.x*d1.x + d1.y*d1.y + d1.z*d1.z;
    const float e = d2.x*d2.x + d2.y*d2.y + d2.z*d2.z;
    const float f = d2.x*r.x + d2.y*r.y + d2.z*r.z;
    float s = 0, t = 0;
    if (a <= 1e-9f && e <= 1e-9f) return std::sqrt(r.x*r.x + r.y*r.y + r.z*r.z);
    if (a <= 1e-9f) { t = std::max(0.0f, std::min(1.0f, f / e)); }
    else {
      const float c = d1.x*r.x + d1.y*r.y + d1.z*r.z;
      if (e <= 1e-9f) { s = std::max(0.0f, std::min(1.0f, -c / a)); }
      else {
        const float b = d1.x*d2.x + d1.y*d2.y + d1.z*d2.z;
        const float den = a * e - b * b;
        if (den > 1e-9f) s = std::max(0.0f, std::min(1.0f, (b * f - c * e) / den));
        t = (b * s + f) / e;
        if (t < 0) { t = 0; s = std::max(0.0f, std::min(1.0f, -c / a)); }
        else if (t > 1) { t = 1; s = std::max(0.0f, std::min(1.0f, (b - c) / a)); }
      }
    }
    const float px = p1.x + d1.x * s, py = p1.y + d1.y * s, pz = p1.z + d1.z * s;
    const float qx = p2.x + d2.x * t, qy = p2.y + d2.y * t, qz = p2.z + d2.z * t;
    const float dx = px - qx, dy = py - qy, dz = pz - qz;
    return std::sqrt(dx*dx + dy*dy + dz*dz);
  }

  void update(const Frame& f, const PlayerState& P, const SenseCfg& C, float dt) {
    reset();
    const V3 com{f.comX, f.comY, f.comZ};
    const float dx = P.x - com.x, dz = P.z - com.z;
    dist = std::sqrt(dx*dx + dz*dz);
    if (dist > 1e-3f) bearing = std::atan2(dz, dx); else bearing = 0;
    while (bearing > 3.14159265f) bearing -= 6.2831853f;
    while (bearing < -3.14159265f) bearing += 6.2831853f;

    const bool inFov = std::fabs(bearing) < 0.5f * C.fovDeg * 0.0174533f;
    sees = P.valid && dist < C.visRange && inFov;
    /* 听觉按"脚步声"算：跑起来听得远，站着不动几乎听不到 */
    const float hearR = C.hearRange * (0.25f + 0.75f * std::min(1.0f, P.speed / 1.6f));
    heard = P.valid && dist < hearR;

    /* 头：看见了 / 听见了就转过去，转过去的过程有惯性（tau ~0.22 s） */
    float want = 0;
    if (sees || heard) want = std::max(-1.15f, std::min(1.15f, bearing));
    headYaw += (want - headYaw) * (1.0f - std::exp(-dt / 0.22f));
    const float pitchWant = sees ? std::max(-0.45f, std::min(0.35f, (1.56f - f.nodes[3].y) / std::max(0.6f, dist))) : 0.0f;
    headPitch += (pitchWant - headPitch) * (1.0f - std::exp(-dt / 0.30f));

    /* 碰撞：主角胶囊（r 0.26，高 1.24）对她的每根骨头胶囊 */
    if (P.valid && dist < 2.0f) {
      const float pTop = 1.24f;
      float worst = 0, worstGain = 0;
      /* 17 根骨头全查：a/b 是骨架的边，r 是那根骨头的半径（来自 colliders.self） */
      const int NB = 17;
      for (int a = 0; a < NB; ++a) {
        if (!f.bones[a][0] && !f.bones[a][1] && a != 0) continue;
        const int ia = (a == 0 ? 0 : f.bones[a][0]);
        const int ib = (a == 0 ? 1 : f.bones[a][1]);
        const V3& A = f.nodes[ia];
        const V3& B = f.nodes[ib];
        const float rSelf = (a < 2 ? 0.092f : (a < 4 ? 0.077f : 0.05f));
        const V3 pa{P.x, 0.02f, P.z};
        const V3 pb{P.x, pTop, P.z};
        const float d = segSegDist(A, B, pa, pb);
        const float pen = (rSelf + 0.26f) - d;
        if (pen > worst) {
          worst = pen;
          const float y = 0.5f * (A.y + B.y);
          worstGain = (y > 1.35f ? C.gainNeck : (y > 1.05f ? C.gainWaist : C.gainFoot));
        }
      }
      if (worst > 0) {
        touchN = worst * 900.0f * worstGain;    /* 重叠 1 cm ≈ 9 N·gain */
        touchGain = worstGain;
        const float nx = (com.x - P.x), nz = (com.z - P.z);
        const float L = std::sqrt(nx*nx + nz*nz) + 1e-4f;
        pushX = nx / L * std::min(worst, 0.15f);
        pushZ = nz / L * std::min(worst, 0.15f);
      }
    }
  }
};
