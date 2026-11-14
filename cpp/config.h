#pragma once
/* ================= 参数表加载（assets/soma.json → 运行期结构）=================
   原则：**表里有什么就覆盖什么，表里没有的保留内置默认值**。
   所以删掉某个字段不会炸，只会退回代码里的数。
*/
#include "json.h"
#include "walker.h"
#include "motions_def.h"
#include "senses.h"
#include <map>
#include <string>
#include <vector>

inline bool applyBody(const JVal& body, Params& P) {
  const JVal* sk = body.get("skeleton");
  if (sk) {
    #define SETP(k) if (sk->has(#k)) P.k = (float)sk->numOr(#k, P.k)
    SETP(g); SETP(hipH); SETP(thigh); SETP(shank); SETP(ankleH); SETP(hipHalfW);
    SETP(footLen); SETP(footW); SETP(torso1); SETP(torso2); SETP(headR); SETP(neckLen);
    SETP(upperArm); SETP(foreArm); SETP(shoulderHalfW);
    #undef SETP
  }
  const JVal* ga = body.get("gait");
  if (ga) {
    #define SETP(k) if (ga->has(#k)) P.k = (float)ga->numOr(#k, P.k)
    SETP(vDes); SETP(vWalk); SETP(vRun); SETP(Tstep); SETP(duty); SETP(runDuty);
    SETP(liftWalk); SETP(liftRun); SETP(sagK); SETP(hipKp); SETP(hipKd); SETP(latK);
    SETP(ankleKp); SETP(ankleKd); SETP(latStiff); SETP(leadBias); SETP(alphaCap);
    SETP(betaStep); SETP(toePush); SETP(heelBrake); SETP(zmpLimX); SETP(zmpLimZ);
    SETP(bounce); SETP(armAmp); SETP(armLift);
    #undef SETP
  }
  return sk != nullptr || ga != nullptr;
}

inline bool applyBio(const JVal& body, Bio& B) {
  const JVal* me = body.get("metabolism");
  const JVal* th = body.get("thermo");
  if (body.has("mass_kg")) B.bodyMass = (float)body.numOr("mass_kg", B.bodyMass);
  if (me) {
    B.bmrPerKg   = (float)me->numOr("bmrPerKg", B.bmrPerKg);
    B.actCost    = (float)me->numOr("actCost", B.actCost);
    B.eff        = (float)me->numOr("eff", B.eff);
    B.tauAct     = (float)me->numOr("tauAct", B.tauAct);
    B.tauDeact   = (float)me->numOr("tauDeact", B.tauDeact);
    B.fatMax     = (float)me->numOr("fatMax", B.fatMax);
    B.tauFat     = (float)me->numOr("tauFat", B.tauFat);
    B.actThresh  = (float)me->numOr("actThresh", B.actThresh);
    B.tauRec     = (float)me->numOr("tauRec", B.tauRec);
  }
  if (th) {
    B.heatCap = (float)th->numOr("heatCap", B.heatCap);
    B.tempEnv = (float)th->numOr("tempEnv", B.tempEnv);
    B.coolK   = (float)th->numOr("coolK", B.coolK);
    B.evapMax = (float)th->numOr("evapMax", B.evapMax);
    B.temp    = th->numOr("temp0", B.temp);
  }
  const JVal* mu = body.get("muscles");
  if (mu && mu->t == JVal::ARR) {
    for (int i = 0; i < 6 && i < mu->size(); ++i) {
      const JVal& m = mu->arr[i];
      B.m[i].Fmax = (float)m.numOr("fmax", B.m[i].Fmax);
      B.m[i].arm  = (float)m.numOr("arm", B.m[i].arm);
      B.m[i].mass = (float)m.numOr("mass", B.m[i].mass);
    }
  }
  return me != nullptr || th != nullptr;
}

inline void applySenses(const JVal& root, SenseCfg& SC, OrganCfg& OC) {
  if (const JVal* se = root.get("senses")) {
    if (const JVal* v = se->get("vision")) {
      SC.fovDeg   = (float)v->numOr("fov_deg", SC.fovDeg);
      SC.visRange = (float)v->numOr("range_m", SC.visRange);
    }
    if (const JVal* h = se->get("hearing")) SC.hearRange = (float)h->numOr("range_m", SC.hearRange);
    if (const JVal* pr = se->get("proprioception")) {
      SC.proprioNoiseDeg = (float)pr->numOr("noise_deg", SC.proprioNoiseDeg);
      SC.proprioDriftDeg = (float)pr->numOr("drift_deg_per_min", SC.proprioDriftDeg);
    }
    if (const JVal* t = se->get("touch")) {
      SC.painN     = (float)t->numOr("pain_thresh_n", SC.painN);
      SC.withdrawN = (float)t->numOr("withdraw_n", SC.withdrawN);
      SC.tickleN   = (float)t->numOr("tickle_n", SC.tickleN);
      if (const JVal* zs = t->get("zones")) {
        for (int i = 0; i < zs->size(); ++i) {
          const JVal& z = zs->arr[i];
          const std::string id = z.strOr("id", "");
          const float g = (float)z.numOr("gain", 1.0);
          if (id == "neck") SC.gainNeck = g;
          else if (id == "palm") SC.gainPalm = g;
          else if (id == "waist") SC.gainWaist = g;
          else if (id == "foot") SC.gainFoot = g;
          else if (id == "face") SC.gainFace = g;
        }
      }
    }
  }
  if (const JVal* body = root.get("body")) {
    if (const JVal* og = body->get("organs")) {
      if (const JVal* ht = og->get("heart")) {
        OC.restBpm      = (float)ht->numOr("rest_bpm", OC.restBpm);
        OC.maxBpm       = (float)ht->numOr("max_bpm", OC.maxBpm);
        OC.tauHeartUp   = (float)ht->numOr("tau_up_s", OC.tauHeartUp);
        OC.tauHeartDown = (float)ht->numOr("tau_down_s", OC.tauHeartDown);
        OC.reserve      = (float)ht->numOr("reserve", OC.reserve);
      }
      if (const JVal* lu = og->get("lungs")) {
        OC.restRpm     = (float)lu->numOr("rest_rpm", OC.restRpm);
        OC.maxRpm      = (float)lu->numOr("max_rpm", OC.maxRpm);
        OC.tauLungUp   = (float)lu->numOr("tau_up_s", OC.tauLungUp);
        OC.tauLungDown = (float)lu->numOr("tau_down_s", OC.tauLungDown);
      }
    }
  }
}

inline void loadConfig(const char* path, Params& P, Bio& B, SomaCfg& C, SenseCfg* SC = nullptr, OrganCfg* OC = nullptr) {
  JVal root;
  if (!jsonLoad(path, root)) return;
  C.ok = true;
  if (const JVal* body = root.get("body")) { applyBody(*body, P); applyBio(*body, B); }
  if (SC && OC) applySenses(root, *SC, *OC);

  if (const JVal* w = root.get("world")) {
    if (const JVal* g = w->get("ground")) {
      C.muFoot = (float)g->numOr("mu_foot", C.muFoot);
      C.muBody = (float)g->numOr("mu_body", C.muBody);
    }
    if (const JVal* b = w->get("bounds")) {
      if (const JVal* x = b->get("x")) if (x->size() == 2) { C.bx0 = (float)x->arr[0].num; C.bx1 = (float)x->arr[1].num; }
      if (const JVal* z = b->get("z")) if (z->size() == 2) { C.bz0 = (float)z->arr[0].num; C.bz1 = (float)z->arr[1].num; }
    }
    if (const JVal* ps = w->get("props")) {
      for (int i = 0; i < ps->size(); ++i) {
        const JVal& p = ps->arr[i];
        Prop q;
        q.id = p.strOr("id", "");
        q.kind = p.strOr("kind", "box");
        q.x0 = (float)p.numOr("x0", 0); q.x1 = (float)p.numOr("x1", 0);
        q.z0 = (float)p.numOr("z0", 0); q.z1 = (float)p.numOr("z1", 0);
        q.x  = (float)p.numOr("x", 0);  q.z  = (float)p.numOr("z", 0);
        q.h  = (float)p.numOr("h", 0);
        q.w  = (float)p.numOr("w", 0.2f); q.d = (float)p.numOr("d", 0.2f);
        q.kcal = (float)p.numOr("kcal", 0); q.bite = (float)p.numOr("bite", 0);
        q.ml = (float)p.numOr("ml", 0); q.facing = (float)p.numOr("facing_deg", 0);
        C.props.push_back(q);
      }
    }
  }

  if (const JVal* ms = root.get("motions")) {
    for (auto& kv : ms->obj) {
      if (kv.first == "note") continue;
      const JVal& m = kv.second;
      Motion mo;
      mo.name = kv.first;
      mo.duration = (float)m.numOr("duration", 1.0);
      mo.interruptAt = (float)m.numOr("interrupt_at", 1e9);
      mo.cost = (float)m.numOr("cost", 0);
      if (const JVal* ks = m.get("keys")) {
        for (int i = 0; i < ks->size(); ++i) {
          const JVal& k = ks->arr[i];
          MotionFrame fr;
          fr.t = (float)k.numOr("t", 0);
          if (const JVal* off = k.get("off")) {
            for (auto& nv : off->obj) {
              int node = std::atoi(nv.first.c_str());
              const JVal& a = nv.second;
              if (a.t == JVal::ARR && a.size() == 3)
                fr.offs.emplace_back(node, V3{(float)a.arr[0].num, (float)a.arr[1].num, (float)a.arr[2].num});
            }
          }
          mo.frames.push_back(std::move(fr));
        }
      }
      if (!mo.frames.empty()) mo.loaded = true;
      C.motions.push_back(std::move(mo));
    }
  }

  if (const JVal* br = root.get("brain")) C.brainWait = (float)br->numOr("wait_max_s", C.brainWait);
}
