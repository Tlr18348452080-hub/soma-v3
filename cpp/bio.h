/* 生物层（bio.h）：肌肉、能量代谢、疲劳、体温
   ============================================================
   这一层不驱动关节（那要全身 QP / 力矩控制），它做的是"事后解释"：
     运动学给的姿态 + 地面反作用力 → 准静态逆动力学 → 关节力矩
     → 需要的肌肉激活度 → 耗能 / 产热 / 疲劳 / 体温

   肌肉：Hill 型，力-长度 f_l、力-速度 f_v、激活动力学（tau_act/tau_deact）。
   能量：基础代谢 + 激活维持代价 + 做正功的额外代价（效率 0.25）。
   疲劳：按激活度累积、按时间恢复，直接压低可用肌力（1 - 0.6·fat）。
   体温：热容 + 环境散热 + 蒸发（超过 37°C 才开始出汗）。 */

#pragma once
#include "vec.h"
#include <cmath>
#include <algorithm>
#include <cstdio>

struct MuscleGroup {
  const char* name = "";
  float Fmax = 1000;   // 最大等长力 N
  float arm = 0.04f;   // 力臂 m
  float mass = 3.0f;   // 肌肉质量 kg
  float act = 0;       // 激活 0..1
  float fat = 0;       // 疲劳 0..1
  float len = 1.0f;    // 归一化长度（1 = 最适长度）
  float vel = 0;       // 归一化速度（正 = 缩短）
  float force = 0;     // 当前输出力 N
  float heat = 0;      // 产热 W
};

struct Bio {
  /* ---- 参数（都是可调的经验值，注释里给的是现实量级） ---- */
  float bodyMass = 70.0f;
  float bmrPerKg = 1.2f;        // W/kg，静息
  float actCost = 12.0f;        // W/kg，满激活时的维持代价
  /* 标定说明：这条走道机器人的站姿是半蹲（髋高 0.895 / 腿长 0.90），
     静态膝力矩比真人直立高好几倍，所以满激活代价取 12 而不是生理上的 ~18，
     才能让走路 0.85 m/s 的代谢落回 3~5 W/kg 的现实区间。 */
  float eff = 0.25f;            // 肌肉机械效率
  float tauAct = 0.015f;        // 激活时间常数 s
  float tauDeact = 0.055f;      // 松弛时间常数 s
  float fatMax = 0.70f;         // 疲劳上限（再累也不会完全没力）
  /* 情绪/疼：由 main.cpp 的 Endocrine 每帧写进来，只被 walker.h 的步态增益读 */
  float dopa = 0.35f, cort = 0.20f, pain = 0.0f;
  float tauFat = 62.0f;         // 疲劳趋向目标的时间常数 s
  float actThresh = 0.35f;      // 低于这个激活度不算费劲（站着半蹲不算累）
  float tauRec = 45.0f;         // 恢复时间常数 s
  float lastDry = 0, lastSweat = 0;   /* 上一帧的干散热/出汗功率（调试用） */
  int nTemp = 0; float sumDt = 0;
  float dbgCap = 0, dbgT = 0, dbgHeat = 0;
  float heatCap = 3.0f * 70.0f * 1000.0f;   // J/K（人体比热≈3.5 kJ/(kg·K)）
  float tempEnv = 25.0f, coolK = 6.5f;      // W/K 干散热
  float evapMax = 520.0f;       // W，最大出汗散热
  /* 体温必须用 double：37 °C 处 float 的一个 ulp 是 3.8e-6 K，
     而单步温升只有 2e-6 K —— 每步都被舍入成一整个 ulp，冷却项直接被吃掉。 */
  double temp = 37.0;

  /* ---- 慢变量（小时级的账本）----
     血糖、水、睡眠不是每帧的力学，是"这一天过得怎么样"。
     它们不驱动关节，只从两条路慢慢影响身体：
       · 糖原低 / 睡不够 → 可用力下降（aMax）
       · 水不够 → 出汗散热打折 → 体温更容易爬上去
     数值都以"1.0 = 满"表达，写盘和 UI 都用这个。 */
  float kcal = 1.0f;        // 糖原 1.0 ≈ 1600 kcal
  float kcalMax = 1600.0f;
  float water = 1.0f;       // 水合 1.0 ≈ 2.5 L 可蒸发量
  float waterMax = 2.5f;
  float sleep = 1.0f;       // 清醒度 1.0 = 刚睡醒，0 = 熬穿
  float hunger = 0.0f, thirst = 0.0f, sleepDebt = 0.0f;
  /* 膀胱：喝水是进项，时间是消耗项。0 = 空的，1 = 憋不住。
     它不解便秘，只让"她该去卫生间了"这件事有出处 —— 憋着会疼、会烦、走不快。 */
  float bladder = 0.0f;
  float lastPeeL = 0;
  float lastEatKcal = 0, lastDrinkL = 0;

  MuscleGroup m[6];

  /* ---- 输出 ---- */
  float torque[6] = {0, 0, 0, 0, 0, 0};   // 髋/膝/踝 × 两条腿（N·m）
  float metabRest = 0, metabMuscle = 0, metabTotal = 0;  // W
  float oxygen = 1.0f;   // 0..1：心肺给的氧债状态（由 organs 层写回）
  float mechPower = 0;                    // W 机械功率（正功）
  float heatProd = 0;                     // W
  float fatAvg = 0;

  void init() {
    /* 每腿三组：髋伸肌、膝伸肌、踝跖屈肌。
       Fmax 参考：股四头肌 ~5000 N，臀大肌 ~3000 N，小腿三头肌 ~4000 N（成年人量级）。 */
    struct D { const char* n; float f, a, ms; } d[6] = {
        {"髋伸", 3000.f, 0.055f, 7.0f}, {"膝伸", 5000.f, 0.045f, 7.5f}, {"踝跖屈", 4000.f, 0.050f, 4.0f},
        {"髋伸", 3000.f, 0.055f, 7.0f}, {"膝伸", 5000.f, 0.045f, 7.5f}, {"踝跖屈", 4000.f, 0.050f, 4.0f}};
    for (int i = 0; i < 6; ++i) {
      m[i] = MuscleGroup{};
      m[i].name = d[i].n; m[i].Fmax = d[i].f; m[i].arm = d[i].a; m[i].mass = d[i].ms;
    }
  }

  void reset() {
    for (int i = 0; i < 6; ++i) { m[i].act = 0; m[i].fat = 0; m[i].force = 0; m[i].heat = 0; }
    temp = 37.0f; metabTotal = metabRest = metabMuscle = 0; mechPower = heatProd = 0; fatAvg = 0;
    kcal = water = sleep = 1.0f; hunger = thirst = sleepDebt = 0.0f; bladder = 0.0f;
  }

  /* 可出力系数：满血 = 1.0。饿/渴/困各扣一点，不上纲上线。 */
  float conditionK() const {
    return (0.78f + 0.22f * kcal) * (0.86f + 0.14f * sleep) * (0.92f + 0.08f * water);
  }

  /* ---- 吃 / 喝 / 睡：慢变量的入口 ---- */
  void eat(float kcalIn) {
    lastEatKcal = kcalIn > 0 ? kcalIn : 0;
    kcal = std::min(1.0f, kcal + lastEatKcal / kcalMax);
  }
  void drink(float L) {
    lastDrinkL = L > 0 ? L : 0;
    water = std::min(1.0f, water + lastDrinkL / waterMax);
    bladder = std::min(1.0f, bladder + lastDrinkL / 0.55f);   /* 0.55 L 就快满了 */
  }
  /* 解手：排掉 0.35 L 左右，不排干净也舒服多了 */
  void pee() {
    const float L = std::min(0.35f, bladder * 0.55f);
    lastPeeL = L;
    bladder = std::max(0.0f, bladder - L / 0.55f);
  }
  void sleepFor(float sec) { sleep = std::min(1.0f, sleep + sec / (8.0f * 3600.0f)); }

  /* 每小时级的账本：代谢烧糖、出汗失水、清醒度自己掉。 */
  void slowStep(float dt) {
    kcal = std::max(0.0f, kcal - (metabTotal * dt / 4184.0f) / kcalMax);
    /* 不显性失水：呼吸/皮肤/尿，约 0.10 L/h。不出汗也一样在掉。 */
    const float baseLps = 0.104f / 3600.0f;
    water = std::max(0.0f, water - ((lastSweat * dt / 2.4e6f) + baseLps * dt) / waterMax);
    const float wear = 1.0f + 1.5f * fatAvg;
    sleep = std::max(0.0f, sleep - dt / (16.0f * 3600.0f) * wear);
    hunger = 1.0f - kcal;
    thirst = 1.0f - water;
    sleepDebt = 1.0f - sleep;
    /* 膀胱只进不出：0.55 L 容量，按 0.06 L/h 的生成速度走（喝了水另算） */
    bladder = std::min(1.0f, bladder + (0.060f / 3600.0f * dt) / 0.55f);
  }

  /* Hill 力-长度（高斯型）与力-速度（短缩段双曲、拉长段封顶 1.4） */
  static float fl(float L) { float e = (L - 1.0f) / 0.45f; return std::exp(-e * e); }
  static float fv(float v) {
    if (v <= 0) return std::min(1.4f, 1.0f + v * 0.4f);          // 离心（被拉长）
    return std::max(0.0f, (1.0f - v) / (1.0f + v * 3.0f));       // 向心（缩短）
  }

  /* 一个子步：给两条腿的关节力矩（N·m）和关节角速度（rad/s），
     正力矩=伸展方向，正角速度=伸展。算出激活、力、耗能、疲劳。 */
  void step(float dt, const float tq[6], const float w[6]) {
    mechPower = 0;
    metabMuscle = 0;
    for (int i = 0; i < 6; ++i) {
      float aMax = (1.0f - 0.6f * m[i].fat) * conditionK();  // 疲劳 + 饿/渴/困压低可用力
      float need = std::fabs(tq[i]) / std::max(1e-3f, m[i].Fmax * m[i].arm * aMax);
      float u = std::min(1.0f, std::max(0.0f, need));     // 需要的激活
      float tau = (u > m[i].act) ? tauAct : tauDeact;
      m[i].act += (u - m[i].act) * std::min(1.0f, dt / tau);
      m[i].vel = std::fabs(w[i]) * 0.12f;                 // 归一化纤维速度（≈ 角速度 × 力臂/最优长度）
      float f = m[i].Fmax * m[i].act * fl(m[i].len) * fv(m[i].vel) * aMax;
      m[i].force = f;
      float power = std::fabs(tq[i] * w[i]);              // W
      if (tq[i] * w[i] > 0) mechPower += power;           // 正功（向心）
      /* 代谢：维持 + 激活代价 + 做正功的额外代价 */
      float p = m[i].mass * (1.0f + actCost * m[i].act);
      if (tq[i] * w[i] > 0) p += power / eff;
      m[i].heat = p;
      metabMuscle += p;
      /* 单块肌肉的疲劳在循环外统一按整体负荷更新（见下面），这里先跳过 */
    }
    metabRest = bmrPerKg * bodyMass;
    metabTotal = metabRest + metabMuscle;
    /* 疲劳按整体代谢负荷算：站着的半蹲(负荷≈2倍静息)不算累，跑步(≈6倍)才算。
       用「目标 + 时间常数」而不是累积速率 —— 会自己饱和，也能自己恢复。 */
    float loadR = metabTotal / std::max(1.0f, metabRest);
    float target = fatMax * std::min(1.0f, std::max(0.0f, (loadR - 2.6f) / 4.0f));
    /* 空着肚子 / 欠着觉的时候，同样的负荷更累 */
    target = std::min(fatMax, target + 0.10f * hunger + 0.12f * sleepDebt);
    float tauF = (target > fatAvg) ? tauFat : tauRec;
    fatAvg += (target - fatAvg) * std::min(1.0f, dt / tauF);
    for (int i = 0; i < 6; ++i) m[i].fat = fatAvg;
  }

  /* 体温：产热 - 干散热 - 出汗蒸发 */
  void tempStep(float dt) {
    heatProd = std::max(0.0f, metabTotal - mechPower);
    float dry = coolK * (float)(temp - tempEnv);
    float sweat = evapMax * (0.45f + 0.55f * water)
                * std::min(1.0f, std::max(0.0f, (float)(temp - 37.0) / 1.5f));
    lastDry = dry; lastSweat = sweat; nTemp++; sumDt += dt;
    dbgCap = heatCap; dbgT = temp; dbgHeat = heatProd;
    temp += (double)(heatProd - dry - sweat) * dt / (double)heatCap;
    temp = std::min(42.0, std::max(20.0, temp));
  }

  float metabPerKg() const { return metabTotal / bodyMass; }
};
