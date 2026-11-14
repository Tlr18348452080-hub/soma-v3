#include "walker.h"
#include <cstdio>
int main(){
  printf("%6s %6s %8s %8s %8s %6s\n","vDes","mode","实际速度","侧移峰","骨长err","falls");
  for (float v : {0.4f,0.6f,0.85f,1.1f,1.3f,1.6f,1.9f,2.2f}) {
    Walker W; W.setMode(v>1.2f?2:1); W.P.vWalk=v; W.P.vRun=v; W.P.vDes=v;
    Frame f; double me=0, mz=0;
    for(int i=0;i<60*20;i++){ W.step(1.0f/60.0f); if(i%6==0){W.pose(f); me=std::max(me,(double)f.err); mz=std::max(mz,(double)std::fabs(f.comZ));} }
    printf("%6.2f %6s %8.2f %8.3f %8.4f %6d\n", v, v>1.2f?"run":"walk", W.p.x/20.0f, mz, me, W.falls);
  }
}
