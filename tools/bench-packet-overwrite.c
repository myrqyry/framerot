/* GPL-2.0. Opt-in microbenchmark for the real framerot packet rewrite.
 * The former path below is identical to the removed production code.
 * This measures only replacement overhead, NOT the encode/decode/filter FPS.
 */
#define _POSIX_C_SOURCE 200809L
#include "../src/packet-overwrite.h"
#include <libavcodec/avcodec.h>
#undef NDEBUG
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

enum { BATCH=512, ROUNDS=23, WARMUP=3, MAX_BYTES=8192 };
struct scenario {const char *name;int input,output;bool shared;};
static const struct scenario cases[]={
    {"equal-4K",4336,4336,false},
    {"shrink",4638,350,false},
    {"grow",350,4638,false},
    {"shared-equal",4336,4336,true},
};

static double ns(void)
{
    struct timespec t;
    assert(clock_gettime(CLOCK_MONOTONIC,&t)==0);
    return t.tv_sec*1e9 + t.tv_nsec;
}

/* Historical implementation, kept only in this benchmark for comparison. */
static int old_replacement(AVPacket *packet,const uint8_t *bytes,int size)
{
    AVPacket *replacement=av_packet_alloc();
    if(!replacement||av_new_packet(replacement,size)<0||
       av_packet_copy_props(replacement,packet)<0){
        av_packet_free(&replacement);
        return -1;
    }
    memcpy(replacement->data,bytes,(size_t)size);
    av_packet_unref(packet);
    av_packet_move_ref(packet,replacement);
    av_packet_free(&replacement);
    return 0;
}

/* Pre-allocates unique encoder-like packets OUTSIDE the timed replacement.
 * For shared runs, the reference retains its original bytes for COW checks. */
static double run(const struct scenario *c,bool old)
{
    AVPacket *packets[BATCH],*refs[BATCH]={0};
    static uint8_t out[MAX_BYTES];
    for(int i=0;i<c->output;i++)out[i]=(uint8_t)(i*23+77);
    for(int i=0;i<BATCH;i++){
        packets[i]=av_packet_alloc();
        assert(packets[i] && av_new_packet(packets[i],c->input)==0);
        memset(packets[i]->data,0x1d,(size_t)c->input);
        packets[i]->pts=i+19;packets[i]->dts=i+17;
        packets[i]->duration=3;packets[i]->pos=i+5;
        packets[i]->time_base=(AVRational){1,18};
        if(i%8==0) {
            uint8_t *meta=av_packet_new_side_data(packets[i],AV_PKT_DATA_STRINGS_METADATA,5);
            assert(meta);memcpy(meta,"meta",5);
        }
        if(c->shared){
            refs[i]=av_packet_alloc();assert(refs[i]);
            assert(av_packet_ref(refs[i],packets[i])==0);
        }
    }
    double start=ns();
    for(int i=0;i<BATCH;i++){
        int err=old ? old_replacement(packets[i],out,c->output):
            framerot_packet_overwrite(packets[i],out,(size_t)c->output);
        assert(err==0);
    }
    double elapsed=ns()-start;
    for(int i=0;i<BATCH;i++){
        AVPacket *pkt=packets[i];
        assert(pkt->size==c->output && memcmp(pkt->data,out,(size_t)c->output)==0);
        assert(pkt->pts==i+19 && pkt->dts==i+17 && pkt->duration==3);
        assert(pkt->pos==i+5 && pkt->time_base.num==1 && pkt->time_base.den==18);
        assert(pkt->side_data_elems==(i%8==0 ? 1 : 0));
        if(refs[i]){
            assert(refs[i]->size==c->input && refs[i]->data[0]==0x1d);
            av_packet_free(&refs[i]);
        }
        av_packet_free(&packets[i]);
    }
    return elapsed/BATCH;
}

static int compare_double(const void *a,const void *b)
{
    double x=*(const double*)a,y=*(const double*)b;
    return (x>y)-(x<y);
}

int main(void)
{
    printf("packet-overwrite benchmark: FFmpeg libavcodec=%u batch=%d samples=%d,"
           " clock=CLOCK_MONOTONIC, unit=ns/edit\n",
           avcodec_version(),BATCH,ROUNDS);
    for(size_t i=0;i<sizeof cases/sizeof *cases;i++){
        const struct scenario *c=&cases[i];
        double old[ROUNDS],modern[ROUNDS];
        for(int r=-WARMUP;r<ROUNDS;r++){
            bool old_first=(r%2)==0;
            double a=run(c,old_first);
            double b=run(c,!old_first);
            if(r>=0){
                if(old_first){old[r]=a;modern[r]=b;}
                else {modern[r]=a;old[r]=b;}
            }
        }
        qsort(old,ROUNDS,sizeof *old,compare_double);
        qsort(modern,ROUNDS,sizeof *modern,compare_double);
        double before=old[ROUNDS/2],after=modern[ROUNDS/2];
        printf("%-14s input=%d output=%d shared=%d old=%.0f new=%.0f ratio=%.2fx\n",
               c->name,c->input,c->output,(int)c->shared,
               before,after,before/after);
    }
    return 0;
}
