#include "walker.h"
#include <cstdio>
int main(){
  Walker W; W.setMode(1);
  int prev=0;
  for(int i=0;i<240*12;i++){ W.step(1.0f/240.0f);
    if(W.falls!=prev){ printf("FALL at t=%.3f  p=(%.2f,%.2f) v=(%.2f,%.2f) phase=%.2f\n",W.t,W.p.x,W.p.z,W.v.x,W.v.z,W.phase); prev=W.falls; } }
  printf("end t=%.2f x=%.2f falls=%d\n",W.t,W.p.x,W.falls);
}
