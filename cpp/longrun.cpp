#include "walker.h"
#include <cstdio>
int main(int argc,char**argv){
  int mode = argc>1? atoi(argv[1]) : 1;
  Walker W; W.setMode(mode); Frame f;
  double maxErr=0; int prevFalls=0;
  for(int i=0;i<120*120;i++){ W.step(1.0f/120.0f);
    if(i%12==0){ W.pose(f); maxErr=std::max(maxErr,(double)f.err);
      if(f.falls!=prevFalls){ printf("FALL t=%.1f x=%.1f z=%.3f v=(%.2f,%.2f)\n",f.t,f.comX,f.comZ,W.v.x,W.v.z); prevFalls=f.falls; } } }
  printf("mode=%d 120s: x=%.1f speed=%.2f falls=%d maxErr=%.4f z=%+.3f\n",mode,W.p.x,W.p.x/120.0,W.falls,maxErr,W.p.z);
}
