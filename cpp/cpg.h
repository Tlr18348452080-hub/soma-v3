#pragma once
#include <cmath>
#include <algorithm>

/* ================= 脊髓层：中枢模式发生器（CPG） =================

   为什么要有它：
   以前"走路"是 phase += dt/cycleT 这个外部时钟推进的 —— 用户点 walk，
   动画表按相位把关节摆出来，这叫播放，不叫运动。这次换掉：

   运动是**自己长出来的**：一小组耦合振荡器互相推相位，
   脊髓里没有"第 27 帧该摆哪"，只有节律和互相拉扯的耦合。
   上层（大脑）只给"意图"—— 走快点/慢点/停，改的是频率和幅度，
   不碰任何一个关节角。

   每个振荡器 = 一侧腿的屈伸节律；左右腿的目标相位差 π（反相），
   靠耦合项把相位差拉回 π —— 所以一条腿在支撑时，另一条自动在摆动，
   不需要任何人告诉它"现在该谁支撑"。
*/
struct CPG {
  /* 左右腿相位。初值就反相，之后靠耦合维持（不是靠代码写死） */
  float ph[2] = {0.0f, 3.14159265f};
  float w = 2.0f * 3.14159265f * 0.95f;   // 角频率（rad/s），0.95 Hz 步行节律
  float wTgt = 2.0f * 3.14159265f * 0.95f;
  float amp = 1.0f;                        // 幅度（0 = 停，1 = 正常）
  float ampTgt = 0.0f;
  float drive = 1.0f;                      // 下行驱动（网状脊髓兴奋度 0~1）
  float couple = 3.0f;                     // 耦合强度
  float t = 0.0f;

  /* 意图：只有频率、幅度、开关这三样能从上面下来 */
  void intent(float freqHz, float ampv) { wTgt = 2.0f * 3.14159265f * freqHz; ampTgt = ampv; }

  /* 突然要动（借步、被推）：幅度立刻拉满，不等平滑爬坡 —— 这是惊吓反应 */
  void kick() { amp = std::max(amp, 1.0f); }

  /* 借步重定时：把"要迈的那条腿"直接推到摆动相起点，另一条跟着反相。
     这一步不是作弊 —— 真脊髓的借步反射也是立刻改发到这条腿的运动神经元，
     不会等着一个和身体无关的节拍器转到下一个摆动窗口。 */
  void catchPhase(int leg, float duty) {
    float sw = duty * 6.28318531f + 0.02f;
    ph[leg] = sw;
    float other = sw + 3.14159265f;
    if (other > 6.28318531f) other -= 6.28318531f;
    ph[1 - leg] = other;
    amp = std::max(amp, 1.0f);
  }

  void step(float dt) {
    /* 频率和幅度是"平滑靠近"，不是瞬间跳 —— 真实神经驱动有惯性 */
    w += (wTgt - w) * std::min(1.0f, dt * 6.0f);
    amp += (ampTgt - amp) * std::min(1.0f, dt * 4.0f);
    if (amp < 1e-3f) amp = 0.0f;

    /* 相位推进 + 反相耦合：
       err = ph[1]-ph[0] 应该等于 π；偏差转成对两条腿相反的推拉 */
    for (int i = 0; i < 2; ++i) ph[i] += w * dt;
    float err = wrapPi(ph[1] - ph[0] - 3.14159265f);
    ph[0] += couple * err * dt;
    ph[1] -= couple * err * dt;
    for (int i = 0; i < 2; ++i) ph[i] = wrapPi(ph[i]);
    t += dt;
  }

  static float wrapPi(float a) {
    while (a > 3.14159265f) a -= 2.0f * 3.14159265f;
    while (a < -3.14159265f) a += 2.0f * 3.14159265f;
    return a;
  }

  /* ---- 输出：到运动神经元的"节律命令"（-1~1），不是关节角。
     具体摆哪个角度，由下面 reflex() 和身体的几何共同决定。 ---- */
  float swing(int leg) const { return std::sin(ph[leg]) * amp; }          // 屈/伸
  float stance(int leg) const { return 0.5f * (1.0f - swing(leg)); }      // 支撑相程度 0~1
  bool inSwing(int leg) const { return swing(leg) > 0.0f; }
  /* 左右腿相位差（诊断用，理论值 π） */
  float phaseGap() const { return std::fabs(wrapPi(ph[1] - ph[0])); }
};

/* ================= 反射弧 =================
   脊髓不只会发节律，还会被身体的感觉反过来调：
   - 牵张反射：关节被拉得越偏，回到目标角的力越大（stretch）
   - 负载反应（伸肌推力）：脚底压得越实，支撑腿越要蹬（load）
   - 屈肌反射：摆动腿撞到东西，立刻加大抬腿（stumble）
   这些都是局部回路，不需要任何全局规划 —— 这就是"脊髓"的味道。 */
struct Reflex {
  float stretchGain = 1.8f;
  float loadGain = 0.9f;
  float stumbleGain = 1.2f;

  float stretch(float q, float qRef) const { return stretchGain * (qRef - q); }
  /* 蹬地：groundForce 归一化到体重（0~2），只加在支撑腿的伸肌上 */
  float load(float groundForceNorm) const {
    return loadGain * std::max(0.0f, groundForceNorm - 0.2f);
  }
  /* 绊倒：摆动腿碰到比预期高的东西时抬腿 */
  float stumble(float clearance, float want) const {
    return stumbleGain * std::max(0.0f, want - clearance);
  }
};
