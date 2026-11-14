#include "walker.h"
#include <cstdio>
int main(int argc,char**argv){
  bool dopose = argc>1 && argv[1][0]=='1';
  Walker W; W.setMode(1); Frame f;
  for(int i=0;i<120*30;i++){
    W.step(1.0f/120.0f);
    if(dopose && i%12==0) W.pose(f);
    if(i%600==0) printf("pose=%d t=%5.1f x=%7.2f vx=%.2f falls=%d\n",dopose,W.t,W.p.x,W.v.x,W.falls);
  }
  printf("FINAL pose=%d x=%.2f falls=%d\n",dopose,W.p.x,W.falls);
}
