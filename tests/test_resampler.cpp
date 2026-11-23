#include "resampler.h"
#include <cstdio>
#include <cmath>
#include <vector>
int main(){
  const int inRate=44100, seconds=2, n=inRate*seconds;
  std::vector<float> in((size_t)n*2);
  for(int i=0;i<n;i++){ float v=0.5f*sinf(2*3.14159265f*440*i/inRate); in[i*2]=v; in[i*2+1]=v; }
  audio::Resampler r; r.reset(inRate);
  std::vector<int16_t> out((size_t)48000*3*2), collected;
  int pos=0; int got=0;
  while(pos<n){
    int chunk=std::min(1024,n-pos);
    r.append(in.data()+(size_t)pos*2, chunk); pos+=chunk;
    int frames=r.process(out.data(), 4096, pos>=n);
    collected.insert(collected.end(), out.begin(), out.begin()+frames*2);
    got+=frames;
  }
  int guard=0;
  while(!r.drained() && guard++<100){
    int frames=r.process(out.data(),4096,true);
    if(frames<=0) break;
    collected.insert(collected.end(), out.begin(), out.begin()+frames*2);
    got+=frames;
  }
  // zero crossings on left channel
  int zc=0; for(size_t i=1;i<collected.size()/2;i++){ if((collected[(i-1)*2]<0)!=(collected[i*2]<0)) zc++; }
  double freq = zc/2.0/((double)got/48000.0);
  double peak=0; for(int16_t v:collected) peak=std::max(peak,(double)abs(v));
  printf("frames=%d expected~%d freq=%.1fHz peak=%.0f\n", got, 48000*seconds, freq, peak);
  return 0;
}
