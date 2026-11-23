#undef NDEBUG
#include "../src/video-roundtrip.h"
#include "../src/mpeg4-edit.h"
#include <libavutil/log.h>
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum { W=192, H=144, FRAMES=75 };
static void still(uint8_t *pixels)
{
    for(int y=0;y<H;y++)for(int x=0;x<W;x++){
        size_t p=((size_t)y*W+x)*4;
        /* Nonuniform but strictly unchanging still: it cannot generate
         * meaningful decoder motion without packet surgery. */
        pixels[p]=(uint8_t)(((x/8+y/9)%2)*160+45);
        pixels[p+1]=(uint8_t)((x*7+y*3)&255);
        pixels[p+2]=(uint8_t)(((x/11+y/5)%2)*130+35);
        pixels[p+3]=255;
    }
}
int main(void)
{
    struct retro_video_options opt=retro_video_default_options(RETRO_VIDEO_MPEG4_PART2);
    opt.width=W;opt.height=H;opt.fps=15;opt.bitrate=280000;opt.gop_size=45;
    opt.mosh_mode=RETRO_MOSH_INSTRUMENT;
    opt.mosh_layers=RETRO_LAYER_COMPRESSED;
    opt.mosh_mv_x=opt.mosh_mv_y=100;
    opt.mosh_residual=100;opt.mosh_residual_loss=0;
    opt.mosh_phantom=45;
    struct retro_video *phantom=retro_video_create(&opt);
    assert(phantom);
    opt.mosh_phantom=0;
    struct retro_video *clean=retro_video_create(&opt);
    assert(clean);
    uint8_t src[W*H*4],a[W*H*4],b[W*H*4];
    still(src);
    int changed=0,frames=0;
    uint64_t converted_when_disabled=0;
    for(int t=0;t<FRAMES;t++){
        /* Turn off a live effect without reconstructing the codec. */
        if(t==55){
            converted_when_disabled=retro_video_compressed_phantom_blocks(phantom);
            opt.mosh_phantom=0;
            assert(retro_video_set_live_options(phantom,&opt));
        }
        int n=retro_video_process(phantom,src,W*4,a,W*4);
        int c=retro_video_process(clean,src,W*4,b,W*4);
        assert(n>=0 && n==c);
        if(n==1){
            frames++;
            if(t<55)changed+=memcmp(a,b,sizeof a)!=0;
            if(t==0)assert(memcmp(a,b,sizeof a)==0);
        }
    }
    uint64_t converted=retro_video_compressed_phantom_blocks(phantom);
    printf("PHANTOM still frames=%d changed=%d converted=%llu packets=%llu unsupported=%llu\n",
        frames,changed,(unsigned long long)converted,
        (unsigned long long)retro_video_compressed_packets(phantom),
        (unsigned long long)retro_video_compressed_unsupported(phantom));
    assert(frames==FRAMES);
    assert(converted>100);
    assert(changed>=25);
    assert(retro_video_compressed_unsupported(phantom)==0);
    /* Turning the setting off must stop injections immediately; decoded
     * reference history may still contain artifacts until a fresh I. */
    assert(converted==converted_when_disabled);
    retro_video_destroy(phantom);retro_video_destroy(clean);
    /* Full intensity should remain syntactically valid across consecutive
     * edited P-VOPs, not just with sparse isolated macroblocks. */
    opt.mosh_phantom=100;
    struct retro_video *full=retro_video_create(&opt);
    assert(full);
    for(int t=0;t<45;t++)
        assert(retro_video_process(full,src,W*4,a,W*4)==1);
    assert(retro_video_compressed_phantom_blocks(full)>1500);
    assert(retro_video_compressed_unsupported(full)==0);
    retro_video_destroy(full);
    opt.mosh_phantom=-1;assert(!retro_video_create(&opt));
    opt.mosh_phantom=101;assert(!retro_video_create(&opt));
    return 0;
}
