#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <arm_neon.h>
__attribute__((target("+bf16"))) static long go(uint16_t rounded[3]){
  const float af[8]={1,2,3,4,5,6,7,8}, bf[8]={2,-1,.5f,2,-2,3,4,.25f};
  bfloat16_t ah[8], bh[8];
  for(int i=0;i<8;i++){ ah[i]=vcvth_bf16_f32(af[i]); bh[i]=vcvth_bf16_f32(bf[i]); }
  bfloat16x8_t a=vld1q_bf16(ah), b=vld1q_bf16(bh);
  float32x4_t acc=vbfdotq_f32(vdupq_n_f32(0.0f),a,b);
  long r=0; float32_t o[4]; vst1q_f32(o,acc); for(int i=0;i<4;i++) r+=(long)(o[i]*4);
  uint32_t bits[3]={0x3f808000u,0x3f818000u,0x7f800001u};
  for(int i=0;i<3;i++){ float f; memcpy(&f,&bits[i],4); bfloat16_t h=vcvth_bf16_f32(f); memcpy(&rounded[i],&h,2); }
  return r;
}
int main(void){ uint16_t rounded[3]; long r=go(rounded); printf("bf16 r=%ld round=%04x,%04x,%04x\n",r,rounded[0],rounded[1],rounded[2]); return 0; }
