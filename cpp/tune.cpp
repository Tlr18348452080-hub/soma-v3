#include "walker.h"
#include <cstdio>
#include <vector>
struct R{float speed,sway,falls,err;};
static R ev(float sagK,float lead,float T,float kp,float kd,float latK,float vDes=0.85f){
  Walker W; W.P.sagK=sagK; W.P.leadBias=lead; W.P.Tstep=T; W.P.ankleKp=kp; W.P.ankleKd=kd; W.P.latK=latK;
  W.setMode(1); W.P.vDes=vDes;
  float mz=0; for(int i=0;i<240*12;i++){W.step(1.0f/240.0f); mz=std::max(mz,std::fabs(W.p.z));}
  R r; r.speed=W.p.x/12.0f; r.sway=mz; r.falls=(float)W.falls; W.pose(*(new Frame)); r.err=0; return r;
}
int main(){
  for(float sagK: {0.3f,0.55f,0.9f,1.3f})
   for(float lead: {0.02f,0.06f,0.10f,0.14f})
    for(float T: {0.50f,0.56f,0.62f})
     for(float kp: {4.0f,7.0f,12.0f})
     {
       R r=ev(sagK,lead,T,kp,0.55f,0.85f);
       printf("sagK=%.2f lead=%.2f T=%.2f kp=%.0f | v=%.2f sway=%.3f falls=%.0f\n",sagK,lead,T,kp,r.speed,r.sway,r.falls);
     }
}
