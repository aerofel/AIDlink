#include <stdio.h>
#include "derive.h"
#include "fixgate.h"
int main(int c,char**v){ FILE*f=fopen(v[1],"r"); derive_state_t d={0}; fixgate_t g={0};
 double gs=0,trk=0; bool ht=false, valid=false; unsigned ms; double la,lo; int holds=0,n=0; double sum=0; int k=0;
 while(fscanf(f,"%u %lf %lf",&ms,&la,&lo)==3){ n++;
  fg_verdict_t fv=fixgate_feed(&g,la,lo,ms,gs,trk,valid&&ht);
  if(fv==FG_HOLD){holds++; fprintf(stderr,"HOLD t=%u (%.4f,%.4f) last (%.4f,%.4f) dt=%ums gs=%.0f trk=%.1f\n",ms,la,lo,g.lat,g.lon,ms-g.changed_ms,gs,trk); continue;}
  derive_update(&d,la,lo,ms,-1,0,false,&gs,&trk,&ht); valid=true;
  if(n>40){sum+=gs;k++;}
 } printf("polls %d holds %d mean gs after warmup %.0f\n",n,holds,sum/k); }
