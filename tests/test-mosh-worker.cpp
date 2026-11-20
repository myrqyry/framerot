#undef NDEBUG
#include "../src/codec-worker.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <chrono>
#include <thread>
static void pause_ms(void) { std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
int main(void) {
    struct retro_video_options o=retro_video_default_options(RETRO_VIDEO_MPEG4_PART2);
    o.width=64; o.height=64; o.fps=30; o.gop_size=8; o.bitrate=120000;
    struct codec_worker *w=codec_worker_create(&o);
    assert(w);
    unsigned char input[64*64*4], output[sizeof input];
    for (size_t i=0;i<sizeof input;i+=4) {input[i]=180;input[i+1]=40;input[i+2]=90;input[i+3]=255;}
    for (int i=0;i<1000;i++) codec_worker_submit(w,input,64*4);
    int got=0;
    for (int i=0;i<3000&&!got;i++) { got=codec_worker_take(w,output); if(!got) pause_ms(); }
    assert(got==1 && output[0]>120 && output[1]<100 && output[3]==255);
    struct codec_worker_stats s=codec_worker_stats(w);
    assert(s.submitted>0 && s.completed>0 && s.dropped>0 && s.max_pending<=1);
    printf("WORKER submitted=%llu completed=%llu dropped=%llu max_pending=%u\n",(unsigned long long)s.submitted,(unsigned long long)s.completed,(unsigned long long)s.dropped,s.max_pending);
    codec_worker_destroy(w);
    o.bitrate=0;
    w=codec_worker_create(&o); assert(w);
    for(int i=0;i<600;i++) { codec_worker_submit(w,input,64*4); pause_ms(); }
    s=codec_worker_stats(w);
    assert(s.errors>0 && s.errors<=4 && !codec_worker_take(w,output));
    printf("RECOVERY errors=%llu\n",(unsigned long long)s.errors);
    codec_worker_destroy(w);
    return 0;
}
