#include "walker.h"
#include <cstdio>
#include <cmath>
static float wof(const Walker&W){return std::sqrt(9.81f/W.P.hipH);}
int main(){
  Walker W; W.setMode(1);
  for(int i=0;i<240*3;i++) W.step(1.0f/240.0f);
  W.push(0,0,0.9f);
  for(int i=0;i<240*2;i++){
    W.step(1.0f/240.0f);
    if(i%10==0) printf("t=%.2f ph=%.2f pz=%+.3f vz=%+.2f capz=%+.3f zmpz=%+.3f ank z=(%+.3f,%+.3f) c=(%d%d) falls=%d\n",
      W.t,W.phase,W.p.z,W.v.z,W.p.z+W.v.z/wof(W),W.lastZmpZ,W.feet[0].ank.z,W.feet[1].ank.z,W.feet[0].contact,W.feet[1].contact,W.falls);
  }
}
