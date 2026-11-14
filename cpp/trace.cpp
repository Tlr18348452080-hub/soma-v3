#include "walker.h"
#include <cstdio>
int main(int argc,char**argv){
  Walker W; W.setMode(argc>1?atoi(argv[1]):1);
  for(int i=0;i<240*4;i++){
    W.step(1.0f/240.0f);
    if(i%12==0){
      printf("t=%.2f ph=%.2f vRef=%.2f xRef=%+.2f p=(%+.3f,%+.3f) v=(%+.2f,%+.2f) zmp=(%+.3f,%+.3f) ank=(%+.3f,%+.3f)(%+.3f,%+.3f) sw%d%d c%d%d f%d\n",
        W.t,W.phase,W.vRef,W.xRef,W.p.x,W.p.z,W.v.x,W.v.z,W.lastZmpX,W.lastZmpZ,
        W.feet[0].ank.x,W.feet[0].ank.z,W.feet[1].ank.x,W.feet[1].ank.z,
        W.feet[0].swing,W.feet[1].swing,W.feet[0].contact,W.feet[1].contact,W.falls);
    }
  }
}
