#include "walker.h"
#include <cstdio>
int main(){
  Walker W; W.setMode(1);
  for(int i=0;i<120*20;i++){ W.step(1.0f/120.0f);
    if(i%120==0) printf("t=%5.1f x=%7.2f vx=%.2f vRef=%.2f xRef=%.2f z=%+.3f falls=%d ph=%.2f c=%d%d\n",
       W.t,W.p.x,W.v.x,W.vRef,W.xRef,W.p.z,W.falls,W.phase,W.feet[0].contact,W.feet[1].contact); }
}
