#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include "reference_template.h"

namespace soundalert {
constexpr size_t kRate = 16000, kBlock = 1024, kFft = 512, kBands = 24;
struct Detection { bool fired; float score; uint64_t endSample; };

// No networking, files, allocations or wall-clock dependence in the audio path.
class Detector {
 public:
  explicit Detector(float threshold = 0.91f) : threshold_(threshold) {}
  Detection push(const int16_t* pcm) {
    float feature[kBands];
    extract(pcm, feature);
    std::memcpy(history_[head_], feature, sizeof(feature));
    head_ = (head_ + 1) % kTemplateFrames;
    ++frames_;
    float score = 0;
    if (frames_ >= kTemplateFrames) {
      for (size_t t=0; t<kTemplateFrames; ++t)
        for (size_t b=0; b<kBands; ++b)
          score += history_[(head_+t)%kTemplateFrames][b] * kReference[t][b];
      score /= kReferenceWeight;
    }
    if (score < threshold_ - 0.10f) {
      if (++lowFrames_ >= 3) latched_ = false;
    } else lowFrames_ = 0;
    const uint64_t end = frames_ * kBlock;
    const bool separated = !everFired_ || end-lastEnd_ >= kTemplateFrames*kBlock*4/5;
    const bool fire = frames_ >= kTemplateFrames && score >= threshold_ && !latched_ && separated;
    if (fire) { latched_=true; everFired_=true; lastEnd_=end; }
    return {fire, score, end};
  }
  void extract(const int16_t* pcm, float* feature) {
    float power[kFft/2+1] = {};
    double rms = 0;
    for (size_t i=0;i<kBlock;++i) rms += double(pcm[i])*pcm[i];
    if (rms/kBlock < 0.11) { std::memset(feature,0,kBands*sizeof(float)); return; }
    constexpr float pi=3.14159265358979323846f;
    for (size_t offset=0; offset<=512; offset+=256) {
      for (size_t i=0;i<kFft;++i) {
        real_[i]=pcm[offset+i]/32768.0f * (0.5f-0.5f*std::cos(2*pi*i/(kFft-1)));
        imag_[i]=0;
      }
      for (size_t i=1,j=0;i<kFft;++i) {
        size_t bit=kFft>>1;
        for (;j&bit;bit>>=1) j^=bit;
        j^=bit;
        if (i<j) { std::swap(real_[i],real_[j]); std::swap(imag_[i],imag_[j]); }
      }
      for (size_t len=2;len<=kFft;len*=2) {
        float wrStep=std::cos(-2*pi/len), wiStep=std::sin(-2*pi/len);
        for (size_t i=0;i<kFft;i+=len) {
          float wr=1,wi=0;
          for(size_t j=0;j<len/2;++j) {
            size_t a=i+j, b=a+len/2;
            float vr=real_[b]*wr-imag_[b]*wi, vi=real_[b]*wi+imag_[b]*wr;
            real_[b]=real_[a]-vr; imag_[b]=imag_[a]-vi;
            real_[a]+=vr; imag_[a]+=vi;
            float next=wr*wrStep-wi*wiStep; wi=wr*wiStep+wi*wrStep; wr=next;
          }
        }
      }
      for(size_t i=0;i<=kFft/2;++i) power[i]+=real_[i]*real_[i]+imag_[i]*imag_[i];
    }
    float maxEnergy=0;
    for(size_t b=0;b<kBands;++b) {
      float energy=0;
      for(unsigned i=kBandEdges[b];i<kBandEdges[b+1];++i) energy+=power[i];
      feature[b]=energy/(kBandEdges[b+1]-kBandEdges[b]);
      maxEnergy=std::max(maxEnergy,feature[b]);
    }
    float mean=0, norm=0, floor=std::max(1e-12f,maxEnergy*1e-5f);
    for(size_t b=0;b<kBands;++b) { feature[b]=std::log(feature[b]+floor); mean+=feature[b]; }
    mean/=kBands;
    for(size_t b=0;b<kBands;++b) { feature[b]-=mean; norm+=feature[b]*feature[b]; }
    norm=std::sqrt(norm);
    for(size_t b=0;b<kBands;++b) feature[b]=norm>1e-8f ? feature[b]/norm : 0;
  }
 private:
  float threshold_;
  float history_[kTemplateFrames][kBands] = {};
  float real_[kFft] = {}, imag_[kFft] = {};
  size_t head_=0, lowFrames_=0;
  uint64_t frames_=0,lastEnd_=0;
  bool latched_=false,everFired_=false;
};
}
