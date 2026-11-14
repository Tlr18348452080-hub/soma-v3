#pragma once
/* 身体 ⇄ 那张 JSON 表 的搬运工。
   脊髓只认识 BodyState，不认识 Walker；Walker 只吐 Frame。
   这里把两边接起来 —— 也是"任意一帧都能存下、重放"的落点。 */
#include "state.h"
#include "walker.h"

inline void capture(Walker& W, BodyState& st, Frame& f) {
  W.pose(f);
  st.t = W.t;
  for (int i = 0; i < 18; ++i) {
    st.node[i] = f.nodes[i];
    st.nodeR[i] = f.radii[i];
    st.nodeV[i] = {0, 0, 0};
  }
  for (int k = 0; k < 2; ++k) {
    st.foot[k] = W.feet[k].ank;
    st.footContact[k] = W.feet[k].contact;
    st.footForce[k] = k == 0 ? f.grfL[1] : f.grfR[1];
    st.cpgPh[k] = W.cpg.ph[k];
  }
  st.vDes = W.P.vDes;
  st.metab = f.metab; st.fat = f.fat; st.temp = f.temp;
  st.mu = W.floorMu;
  st.fallen = f.fallen != 0;
}

/* 反过来：把表里大脑能改的东西写回身体（现在只有意图） */
inline void apply(const BodyState& st, Walker& W) {
  W.P.vDes = st.vDes;
}
