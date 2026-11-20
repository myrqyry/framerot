#undef NDEBUG
#include "../src/video-roundtrip.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define W 128
#define H 96
static void scene(uint8_t *p,int t) {
    for (int y=0;y<H;y++) for(int x=0;x<W;x++) {
        int i=(y*W+x)*4;
        p[i]=((x+t*3)/8%2)*180+30;p[i+1]=(x+y+t*2)&255;
        p[i+2]=((y+t)/9%2)*190+25;p[i+3]=255;
    }
}
static void test_random_instrument(void)
{
    unsigned refs=0,preds=0,layers=0;
    for(uint64_t seed=0;seed<1024;seed++) {
        struct retro_video_options o=retro_video_random_instrument(seed);
        struct retro_video_options repeat=retro_video_random_instrument(seed);
        assert(o.codec==RETRO_VIDEO_MPEG4_PART2 && o.mosh_mode==RETRO_MOSH_INSTRUMENT);
        assert(o.height>=144 && o.height<=288 && retro_video_supported_size(o.codec,o.width,o.height));
        assert(o.fps>=12 && o.fps<=24 && o.bitrate>=160000 && o.bitrate<=650000);
        assert(o.gop_size>=12 && o.gop_size<=24);
        assert(o.mosh_reference>=0 && o.mosh_reference<=RETRO_REFERENCE_ANCHOR);
        assert(o.mosh_predictive>=0 && o.mosh_predictive<=RETRO_PREDICTIVE_REWIND);
        assert(!(o.mosh_layers&~15u));
        assert(o.mosh_reference==repeat.mosh_reference && o.mosh_predictive==repeat.mosh_predictive &&
               o.mosh_layers==repeat.mosh_layers && o.mosh_mv_x==repeat.mosh_mv_x && o.height==repeat.height);
        unsigned count=(o.mosh_reference!=0)+(o.mosh_predictive!=0);
        for (unsigned bit=1;bit<=8;bit<<=1) count+=(o.mosh_layers&bit)!=0;
        assert(count>=1 && count<=2);
        assert(o.mosh_p_burst>=1 && o.mosh_p_burst<=o.mosh_p_interval/2);
        assert(o.mosh_rewind_depth>=2 && o.mosh_rewind_depth<o.gop_size-1);
        assert(o.mosh_anchor_span>=2 && o.mosh_anchor_span<=4);
        assert(o.mosh_mv_x>=-150 && o.mosh_mv_x<=250 && o.mosh_mv_y>=-150 && o.mosh_mv_y<=250);
        assert(o.mosh_residual>=30 && o.mosh_residual<=110 && o.mosh_residual_loss>=0 && o.mosh_residual_loss<=30);
        refs|=1u<<o.mosh_reference;preds|=1u<<o.mosh_predictive;layers|=o.mosh_layers;
    }
    assert(refs==7 && preds==31 && layers==15);
    /* Exercise the real FFmpeg pipeline across deterministic rolls at their
     * actual reference resolutions, not merely a parameter validator. */
    unsigned rendered=0;
    for(uint64_t seed=0;seed<32;seed++) {
        struct retro_video_options o=retro_video_random_instrument(seed);
        struct retro_video *v=retro_video_create(&o);assert(v);
        size_t bytes=(size_t)o.width*o.height*4;
        uint8_t *src=malloc(bytes),*out=malloc(bytes);assert(src&&out);
        unsigned decoded=0;
        for(int t=0;t<72;t++) {
            for(int y=0;y<o.height;y++) for(int x=0;x<o.width;x++) {
                size_t i=((size_t)y*o.width+x)*4;
                src[i]=((x+t*3)/12%2)*180+30;
                src[i+1]=(x+y+t*2)&255;
                src[i+2]=((y+t)/9%2)*190+25;src[i+3]=255;
            }
            int n=retro_video_process(v,src,o.width*4,out,o.width*4);
            if(n<0) fprintf(stderr,"RANDOM_DECODE_ERROR seed=%llu frame=%d\n",(unsigned long long)seed,t);
            assert(n>=0);decoded+=n;
        }
        assert(decoded>=36);rendered+=decoded;
        free(src);free(out);retro_video_destroy(v);
    }
    printf("RANDOM_INSTRUMENT 1024 seeds, 32 real codec runs, %u decoded frames\n",rendered);
}
int main(void) {
    struct retro_video_options o=retro_video_default_options(RETRO_VIDEO_MPEG4_PART2);
    o.width=W;o.height=H;o.fps=15;o.gop_size=12;o.bitrate=180000;
    o.mosh_mode=RETRO_MOSH_INSTRUMENT;
    o.mosh_reference=RETRO_REFERENCE_ANCHOR;o.mosh_anchor_span=4;
    o.mosh_predictive=RETRO_PREDICTIVE_DROP;o.mosh_p_interval=4;
    o.mosh_layers=RETRO_LAYER_FLOW|RETRO_LAYER_PEEL|RETRO_LAYER_SHIMMER;
    o.mosh_vector_strength=250;o.mosh_vector_persistence=65;
    o.mosh_slice_history=4;o.mosh_slice_height=8;o.mosh_rounding_interval=1;
    struct retro_video *v=retro_video_create(&o); assert(v);
    uint8_t src[W*H*4], out[sizeof src];
    int decoded=0;
    for (int t=0;t<150;t++) {scene(src,t);int n=retro_video_process(v,src,W*4,out,W*4);assert(n>=0);decoded+=n;}
    assert(decoded>75 && retro_video_anchored_keyframes(v)>0 && retro_video_dropped_pframes(v)>0 &&
        retro_video_motion_vectors_seen(v)>0 && retro_video_motion_rounding_flips(v)>0 &&
        retro_video_temporal_rows_reused(v)>0);
    printf("INSTRUMENT decoded=%d anchor=%llu drop=%llu vectors=%llu shimmer=%llu peel=%llu\n",decoded,
      (unsigned long long)retro_video_anchored_keyframes(v),(unsigned long long)retro_video_dropped_pframes(v),
      (unsigned long long)retro_video_motion_vectors_seen(v),(unsigned long long)retro_video_motion_rounding_flips(v),
      (unsigned long long)retro_video_temporal_rows_reused(v));
    retro_video_destroy(v);
    o.mosh_reference=RETRO_REFERENCE_LIVE;o.mosh_predictive=RETRO_PREDICTIVE_LIVE;o.mosh_layers=0;
    v=retro_video_create(&o);assert(v);
    for(int t=0;t<60;t++){
        if(t==5) retro_video_set_anchor_hold(v,true);
        if(t==20) {
            o.mosh_mv_x=200;
            o.mosh_residual=50;
            assert(retro_video_set_live_options(v,&o));
            uint64_t held=retro_video_anchored_keyframes(v);
            assert(held>0);
        }
        if(t==40) retro_video_set_anchor_hold(v,false);
        scene(src,t);assert(retro_video_process(v,src,W*4,out,W*4)>=0);
        if(t==39) assert(retro_video_anchored_keyframes(v)>0);
    }
    assert(retro_video_anchor_refreshes(v)>1);
    uint64_t replay=retro_video_anchored_keyframes(v);
    for(int t=60;t<80;t++){scene(src,t);assert(retro_video_process(v,src,W*4,out,W*4)>=0);}
    assert(retro_video_anchored_keyframes(v)==replay);
    retro_video_destroy(v);
    o.mosh_layers=128;assert(!retro_video_create(&o));
    test_random_instrument();
    return 0;
}
