#include "walker.h"
#include <cstdio>
int main(){
  for(float sagK: {0.5f,0.8f})
   for(float T: {0.50f,0.56f,0.62f})
    for(float toe: {0.0f,0.03f,0.06f,0.09f})
     for(float heel: {0.0f,0.03f,0.05f}) {
      Walker W; W.P.sagK=sagK; W.P.Tstep=T; W.P.toePush=toe; W.P.heelBrake=heel;
      W.setMode(1);
      float mz=0; for(int i=0;i<240*14;i++){W.step(1.0f/240.0f); mz=std::max(mz,std::fabs(W.p.z));}
      printf("sagK=%.1f T=%.2f toe=%.2f heel=%.2f | v=%.2f sway=%.3f falls=%d\n",sagK,T,toe,heel,W.p.x/14.0f,mz,W.falls);
     }
}
