#undef NDEBUG
#include "../src/video-roundtrip.h"
#include "../src/mpeg4-edit.h"
#include <libavutil/log.h>
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define W 192
#define H 144
static unsigned errors;
static void log_cb(void *ptr,int level,const char *fmt,va_list ap) {
    (void)ptr;
    if(level<=AV_LOG_ERROR){errors++;vfprintf(stderr,fmt,ap);}
}
static void scene(uint8_t *p,int t) {
    for(int y=0;y<H;y++)for(int x=0;x<W;x++){
        int xx=(x+t*3)%W, yy=(y+t*2)%H;
        int i=(y*W+x)*4;
        p[i]=((xx/12+yy/13)%2)*180+30;
        p[i+1]=(xx*2+yy+t*3)&255;
        p[i+2]=((yy/7+xx/9)%2)*150+40;p[i+3]=255;
    }
}
static void exercise(int fps,int x,int y,int residual,int loss) {
    struct retro_video_options o=retro_video_default_options(RETRO_VIDEO_MPEG4_PART2);
    o.width=W;o.height=H;o.fps=fps;o.bitrate=260000;o.gop_size=24;
    o.mosh_mode=RETRO_MOSH_INSTRUMENT;o.mosh_layers=RETRO_LAYER_COMPRESSED;
    o.mosh_mv_x=x;o.mosh_mv_y=y;o.mosh_residual=residual;o.mosh_residual_loss=loss;
    struct retro_video *v=retro_video_create(&o);assert(v);
    o.mosh_layers=0;struct retro_video *clean=retro_video_create(&o);assert(clean);
    uint8_t source[W*H*4],a[sizeof source],b[sizeof source];int decoded=0,changed=0;
    unsigned before=errors;
    for(int t=0;t<110;t++){
        scene(source,t);
        int n=retro_video_process(v,source,W*4,a,W*4), c=retro_video_process(clean,source,W*4,b,W*4);
        assert(n>=0&&n==c);
        if(n==1){decoded++;changed+=memcmp(a,b,sizeof a)!=0;}
    }
    uint64_t rewritten=retro_video_compressed_packets(v),vectors=retro_video_compressed_vectors(v),coeff=retro_video_compressed_coefficients(v),blocks=retro_video_compressed_blocks(v);
    printf("COMPRESSED fps=%d x=%d y=%d residual=%d loss=%d decoded=%d changed=%d packets=%llu vectors=%llu coefficients=%llu blocks=%llu skipped=%llu errors=%u\n",
        fps,x,y,residual,loss,decoded,changed,(unsigned long long)rewritten,(unsigned long long)vectors,(unsigned long long)coeff,(unsigned long long)blocks,(unsigned long long)retro_video_compressed_unsupported(v),errors-before);
    assert(decoded==110&&errors==before);
    if(x==100&&y==100&&residual==100&&!loss)assert(!rewritten&&!changed);
    else {
        assert(rewritten>=70&&changed>=60);
        if(x!=100||y!=100)assert(vectors>100);
        if(residual!=100)assert(coeff>100);
        if(loss)assert(blocks>100);
    }
    retro_video_destroy(clean);retro_video_destroy(v);
}
int main(void) {
    av_log_set_callback(log_cb);
    exercise(12,100,100,100,0);
    exercise(15,-200,300,100,0);
    exercise(30,100,100,0,0);
    exercise(24,250,-100,50,35);
    uint8_t bad[]={0,0,1,0xb6,0xff,0xff,0xff};
    uint8_t *out=(uint8_t*)1;size_t bytes=99;struct mpeg4_edit_stats s={0};
    struct mpeg4_edit_options o={W,H,15,250,100,100,0,0};
    assert(mpeg4_edit_p_vop(bad,sizeof bad,&o,&out,&bytes,&s)==-1&&!out&&!bytes);
    assert(s.vectors==0&&s.coefficients==0&&s.blocks==0);
    av_log_set_callback(av_log_default_callback);
    return 0;
}
