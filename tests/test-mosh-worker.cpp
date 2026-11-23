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
    /* A single live slider update is a guaranteed mailbox acceptance,
     * not a best-effort try_lock that may silently lose the new setting.
     * Subsequent real MPEG-4 P-frames must consume that setting. */
    struct retro_video_options live=retro_video_default_options(RETRO_VIDEO_MPEG4_PART2);
    live.width=96;live.height=96;live.fps=30;live.gop_size=30;live.bitrate=900000;
    live.mosh_mode=RETRO_MOSH_INSTRUMENT;
    live.mosh_layers=RETRO_LAYER_COMPRESSED;
    struct codec_worker *active=codec_worker_create(&live);assert(active);
    live.mosh_frequency[2]=-100;
    assert(codec_worker_set_controls(active,&live));
    assert(!codec_worker_set_controls(nullptr,&live));
    assert(!codec_worker_set_controls(active,nullptr));
    unsigned char pattern[96*96*4],decoded[sizeof pattern];
    bool applied=false;
    for(int t=0;t<400 && !applied;t++){
        for(int y=0;y<96;y++)for(int x=0;x<96;x++){
            size_t idx=(size_t)(y*96+x)*4;
            pattern[idx]=(unsigned char)((x*17+y*23+t*31)&255);
            pattern[idx+1]=(unsigned char)((x*7^y*19^t*13)&255);
            pattern[idx+2]=(unsigned char)((x*y+t*17)&255);
            pattern[idx+3]=255;
        }
        codec_worker_submit(active,pattern,96*4);
        codec_worker_take(active,decoded);
        if(codec_worker_stats(active).frequency_coefficients[2]>0)applied=true;
        else pause_ms();
    }
    assert(applied);
    assert(codec_worker_stats(active).errors==0);
    uint64_t edited=codec_worker_stats(active).frequency_coefficients[2];
    /* A one-shot switch back to neutral must stop generating new edits.
     * A single old in-flight frame is allowed; after a drain window,
     * the subsequent window must remain neutral. */
    live.mosh_frequency[2]=0;
    assert(codec_worker_set_controls(active,&live));
    auto send_pattern=[&](int t){
        for(int y=0;y<96;y++)for(int x=0;x<96;x++){
            size_t idx=(size_t)(y*96+x)*4;
            pattern[idx]=(unsigned char)((x*17+y*23+t*31)&255);
            pattern[idx+1]=(unsigned char)((x*7^y*19^t*13)&255);
            pattern[idx+2]=(unsigned char)((x*y+t*17)&255);
            pattern[idx+3]=255;
        }
        codec_worker_submit(active,pattern,96*4);
        codec_worker_take(active,decoded);
        pause_ms();
    };
    for(int t=400;t<470;t++)send_pattern(t);
    uint64_t after_drain=codec_worker_stats(active).frequency_coefficients[2];
    for(int t=470;t<540;t++)send_pattern(t);
    assert(codec_worker_stats(active).frequency_coefficients[2]==after_drain);
    assert(after_drain>=edited);
    assert(codec_worker_stats(active).errors==0);
    printf("LIVE_CONTROL guaranteed=true high_band_edits=%llu neutral_stable=true\n",
        (unsigned long long)edited);
    codec_worker_destroy(active);
    o.bitrate=0;
    w=codec_worker_create(&o); assert(w);
    for(int i=0;i<600;i++) { codec_worker_submit(w,input,64*4); pause_ms(); }
    s=codec_worker_stats(w);
    assert(s.errors>0 && s.errors<=4 && !codec_worker_take(w,output));
    printf("RECOVERY errors=%llu\n",(unsigned long long)s.errors);
    codec_worker_destroy(w);
    return 0;
}
