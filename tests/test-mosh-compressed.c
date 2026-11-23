#undef NDEBUG
#include "../src/video-roundtrip.h"
#include "../src/mpeg4-edit.h"
#include "../src/packet-overwrite.h"
#include <libavutil/log.h>
#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <limits.h>
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
/* Real MPEG-4 P packets must be identical through owned and reusable
 * transcribers, including after the scratch bytes are deliberately dirtied. */
static void exercise_scratch(void) {
    const AVCodec *codec=avcodec_find_encoder(AV_CODEC_ID_MPEG4);
    assert(codec);
    AVCodecContext *enc=avcodec_alloc_context3(codec);
    AVFrame *frame=av_frame_alloc();
    AVPacket *packet=av_packet_alloc();
    assert(enc&&frame&&packet);
    enc->width=W;enc->height=H;enc->pix_fmt=AV_PIX_FMT_YUV420P;
    enc->time_base=(AVRational){1,15};enc->framerate=(AVRational){15,1};
    enc->bit_rate=260000;enc->gop_size=24;enc->max_b_frames=0;
    assert(avcodec_open2(enc,codec,NULL)>=0);
    frame->width=W;frame->height=H;frame->format=AV_PIX_FMT_YUV420P;
    assert(av_frame_get_buffer(frame,32)>=0);
    struct mpeg4_edit_options opt={W,H,15,-200,300,50,25,7};
    struct mpeg4_edit_buffer scratch={0};
    int checked=0;
    for(int t=0;t<20;t++){
        assert(av_frame_make_writable(frame)>=0);
        for(int y=0;y<H;y++)for(int x=0;x<W;x++)
            frame->data[0][y*frame->linesize[0]+x]=(uint8_t)(((x+t*3)/8+(y+t)/9)%2*210+24);
        for(int y=0;y<H/2;y++)for(int x=0;x<W/2;x++){
            frame->data[1][y*frame->linesize[1]+x]=(uint8_t)((x+t*5)&255);
            frame->data[2][y*frame->linesize[2]+x]=(uint8_t)((y+t*7)&255);
        }
        frame->pts=t;
        assert(avcodec_send_frame(enc,frame)>=0);
        assert(avcodec_receive_packet(enc,packet)>=0);
        if(!(packet->flags&AV_PKT_FLAG_KEY)){
            uint8_t *owned=NULL,*borrowed=NULL;size_t n_owned=0,n_borrowed=0;
            struct mpeg4_edit_stats a={0},b={0};
            int r=mpeg4_edit_p_vop(packet->data,(size_t)packet->size,&opt,&owned,&n_owned,&a);
            assert(r==1);
            int q=mpeg4_edit_p_vop_reuse(packet->data,(size_t)packet->size,&opt,
                &scratch,&borrowed,&n_borrowed,&b);
            assert(q==r && borrowed==scratch.data && n_owned==n_borrowed);
            assert(memcmp(owned,borrowed,n_owned)==0);
            assert(memcmp(&a,&b,sizeof a)==0);
            assert(scratch.capacity>=n_borrowed);
            /* Writing must clear previously-set bits before ORing new bits. */
            memset(scratch.data,0xff,scratch.capacity);
            borrowed=NULL;n_borrowed=0;
            assert(mpeg4_edit_p_vop_reuse(packet->data,(size_t)packet->size,&opt,
                &scratch,&borrowed,&n_borrowed,&b)==1);
            assert(n_owned==n_borrowed && memcmp(owned,borrowed,n_owned)==0);
            free(owned);checked++;
        }
        av_packet_unref(packet);
    }
    assert(checked>=12);
    uint8_t *unused=(uint8_t*)1;size_t unused_size=77;
    struct mpeg4_edit_options neutral={W,H,15,100,100,100,0,7};
    uint8_t unsupported[]={0,0,1,0xb6,0xff,0xff,0xff};
    struct mpeg4_edit_stats unchanged={3,4,5};
    assert(mpeg4_edit_p_vop_reuse(unsupported,sizeof unsupported,&neutral,
        &scratch,&unused,&unused_size,&unchanged)==-1);
    assert(!unused&&!unused_size&&!unchanged.vectors&&!unchanged.coefficients&&!unchanged.blocks);
    assert(scratch.data && scratch.capacity);
    mpeg4_edit_buffer_destroy(&scratch);
    assert(!scratch.data&&!scratch.capacity);
    av_packet_free(&packet);av_frame_free(&frame);avcodec_free_context(&enc);
}
/* Packet overwrite must preserve FFmpeg-owned metadata and detach only
 * when shared. The editor scratch is independent of the encoder packet. */
static void exercise_packet_overwrite(void) {
    AVPacket *p=av_packet_alloc(),*kept=av_packet_alloc();
    assert(p&&kept&&av_new_packet(p,160)==0);
    memset(p->data,0x17,160);
    uint8_t *side=av_packet_new_side_data(p,AV_PKT_DATA_STRINGS_METADATA,5);
    assert(side);memcpy(side,"meta",5);
    p->pts=101;p->dts=97;p->duration=4;p->pos=321;
    p->flags=AV_PKT_FLAG_KEY;p->time_base=(AVRational){1,15};
    uint8_t replacement[160],shorter[40],grown[8192];
    memset(replacement,0x4a,sizeof replacement);
    memset(shorter,0xab,sizeof shorter);
    memset(grown,0xe3,sizeof grown);
    /* A uniquely owned, size-equal payload should be edited in-place. */
    uint8_t *first=p->data;
    assert(framerot_packet_overwrite(p,replacement,sizeof replacement)==0);
    assert(p->data==first && p->size==(int)sizeof replacement);
    assert(memcmp(p->data,replacement,sizeof replacement)==0);
    assert(p->pts==101&&p->dts==97&&p->duration==4&&p->pos==321);
    assert(p->flags==AV_PKT_FLAG_KEY&&p->time_base.num==1&&p->time_base.den==15);
    assert(p->side_data_elems==1 && memcmp(p->side_data[0].data,"meta",5)==0);
    assert(av_packet_ref(kept,p)==0);
    /* Make writable must detach before changing a referenced payload. */
    assert(framerot_packet_overwrite(p,shorter,sizeof shorter)==0);
    assert(p->size==(int)sizeof shorter);
    assert(memcmp(p->data,shorter,sizeof shorter)==0);
    assert(kept->size==160 && memcmp(kept->data,replacement,160)==0);
    assert(p->data!=kept->data);
    assert(p->pts==101&&p->dts==97&&p->duration==4&&p->pos==321);
    assert(p->side_data_elems==1&&memcmp(p->side_data[0].data,"meta",5)==0);
    /* Growth reallocates if necessary, without discarding packet props. */
    assert(framerot_packet_overwrite(p,grown,sizeof grown)==0);
    assert(p->size==(int)sizeof grown && memcmp(p->data,grown,sizeof grown)==0);
    assert(p->pts==101&&p->dts==97&&p->duration==4&&p->pos==321);
    assert(p->flags==AV_PKT_FLAG_KEY && p->side_data_elems==1);
    assert(memcmp(p->side_data[0].data,"meta",5)==0);
    assert(kept->size==160&&memcmp(kept->data,replacement,160)==0);
    assert(framerot_packet_overwrite(p,NULL,1)<0);
    assert(framerot_packet_overwrite(p,grown,0)<0);
    assert(framerot_packet_overwrite(NULL,grown,sizeof grown)<0);
    assert(framerot_packet_overwrite(p,grown,(size_t)INT_MAX+1u)<0);
    assert(p->size==(int)sizeof grown && p->pts==101);
    av_packet_free(&kept);av_packet_free(&p);
}
int main(void) {
    av_log_set_callback(log_cb);
    exercise_packet_overwrite();
    exercise_scratch();
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
