#pragma once
/* ================= 内脏层（1 Hz）=================
   从内到外：心 → 肺 → 胃 → 肝 → 肾 → 皮。
   全都吃 Bio 的慢变量，输出一组能给 HUD 看、能被上层读的数。 */
#include <cmath>

struct Heart {
  float bpm = 58, rest = 58, maxBpm = 186, strokeMl = 70;
  float tauUp = 3.5f, tauDown = 12.0f, reserve = 3.2f;
  float outputLpm() const { return bpm * strokeMl / 1000.0f; }
  void step(float dt, float metabW, float bmrW) {
    const float drive = (metabW - bmrW) / (bmrW * reserve + 1e-3f);
    const float target = rest + (maxBpm - rest) * std::fmin(1.0f, std::fmax(0.0f, drive));
    const float tau = target > bpm ? tauUp : tauDown;
    bpm += (target - bpm) * (1.0f - std::exp(-dt / std::fmax(0.05f, tau)));
  }
};

struct Lungs {
  float rpm = 12, rest = 12, maxRpm = 42, tidalMl = 500, deadMl = 150;
  float tauUp = 4.0f, tauDown = 10.0f;
  float minuteVentL() const { return rpm * tidalMl / 1000.0f; }
  void step(float dt, float metabW, float bmrW) {
    const float drive = std::fmin(1.0f, std::fmax(0.0f, (metabW - bmrW) / (bmrW * 2.4f + 1e-3f)));
    const float target = rest + (maxRpm - rest) * drive;
    const float tau = target > rpm ? tauUp : tauDown;
    rpm += (target - rpm) * (1.0f - std::exp(-dt / std::fmax(0.05f, tau)));
  }
};

struct Gut {
  float stomachKcal = 240, capacity = 1200, emptyTauH = 3.5f;
  void step(float dt, float burnKcalPerSec) {
    const float rate = stomachKcal / (emptyTauH * 3600.0f);
    stomachKcal = std::fmax(0.0f, stomachKcal - rate * dt - burnKcalPerSec * dt * 0.25f);
  }
  void eat(float kcal) { stomachKcal = std::fmin(capacity, stomachKcal + kcal); }
  float hunger() const { return 1.0f - std::fmin(1.0f, stomachKcal / 240.0f); }
};

struct Organs {
  Heart heart; Lungs lungs; Gut gut;
  float liverGlycogenKcal = 400, liverMax = 400, releaseMaxH = 260;
  float bmrW = 82.0f;
  void step(float dt, float metabW) {
    heart.step(dt, metabW, bmrW);
    lungs.step(dt, metabW, bmrW);
    gut.step(dt, (metabW / 4.184e6f) * 239.0f);
    /* 肝：血糖低就放糖，累了先掉糖原 */
    const float need = std::fmax(0.0f, (metabW - bmrW)) / 4.184e6f * 239.0f;
    liverGlycogenKcal = std::fmax(0.0f, liverGlycogenKcal - (need + releaseMaxH / 3600.0f * 0.3f) * dt);
  }
  void load(const JValRef& j);
};
