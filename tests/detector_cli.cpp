#include <cstdio>
#include <cstdlib>
#include <memory>
#include "detector.h"
#include "delivery.h"

int main(int argc,char** argv) {
  if(argc<2) { std::fprintf(stderr,"usage: detector_cli raw16le.pcm [threshold]\n"); return 2; }
  if(soundalert::afterData(250)!=soundalert::Delivery::Accepted ||
     soundalert::afterData(451)!=soundalert::Delivery::Retry ||
     soundalert::afterData(550)!=soundalert::Delivery::PermanentFailure ||
     soundalert::afterData(-1)!=soundalert::Delivery::Uncertain) return 3;
  FILE* in=nullptr;
#ifdef _MSC_VER
  if(fopen_s(&in,argv[1],"rb") || !in) return 2;
#else
  in=std::fopen(argv[1],"rb"); if(!in)return 2;
#endif
  auto detector=std::make_unique<soundalert::Detector>(argc>2?float(std::atof(argv[2])):0.91f);
  int16_t pcm[soundalert::kBlock];
  float maxScore=0;
  unsigned count=0;
  std::printf("{\"events\":[");
  while(std::fread(pcm,sizeof(int16_t),soundalert::kBlock,in)==soundalert::kBlock) {
    auto d=detector->push(pcm);
    if(d.score>maxScore) maxScore=d.score;
    if(d.fired) {
      std::printf("%s{\"end_sample\":%llu,\"seconds\":%.6f,\"score\":%.6f}",
        count++?",":"",(unsigned long long)d.endSample,double(d.endSample)/soundalert::kRate,d.score);
    }
  }
  std::fclose(in);
  std::printf("],\"count\":%u,\"max_score\":%.6f}\n",count,maxScore);
  return 0;
}
