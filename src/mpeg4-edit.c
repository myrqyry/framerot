/* Standalone bounded MPEG-4 Simple Profile P-VOP transcriber.
 * GPL-2.0; see COPYING and mpeg4-vlc-tables.h for table attribution.
 * No private FFmpeg APIs, no arbitrary byte corruption. */
#include "mpeg4-edit.h"
#include "mpeg4-vlc-tables.h"
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include "portable-sync.h"

struct bits {const uint8_t *data;size_t size,pos;};
struct output {uint8_t *data;size_t cap,pos;bool failed;};
struct vlc {int16_t index;uint8_t len;};
enum {LOOK_BITS=13, LOOK_SIZE=1<<LOOK_BITS};
static struct vlc mbtab[LOOK_SIZE], pytab[LOOK_SIZE], mvtab[LOOK_SIZE],
    intertab[LOOK_SIZE], intratab[LOOK_SIZE], lumtab[LOOK_SIZE], chromtab[LOOK_SIZE];

static void add_vlc(struct vlc *tab,unsigned i,unsigned code,unsigned len)
{
    if(!len || len>LOOK_BITS) return;
    unsigned base=code<<(LOOK_BITS-len),count=1u<<(LOOK_BITS-len);
    for(unsigned j=0;j<count;j++) tab[base+j]=(struct vlc){(int16_t)i,(uint8_t)len};
}
static void init_tables(void)
{
    for(unsigned i=0;i<28;i++)add_vlc(mbtab,i,ff_h263_inter_MCBPC_code[i],ff_h263_inter_MCBPC_bits[i]);
    for(unsigned i=0;i<16;i++)add_vlc(pytab,i,ff_h263_cbpy_tab[i][0],ff_h263_cbpy_tab[i][1]);
    for(unsigned i=0;i<33;i++)add_vlc(mvtab,i,ff_mvtab[i][0],ff_mvtab[i][1]);
    for(unsigned i=0;i<103;i++){
        add_vlc(intertab,i,ff_inter_vlc[i][0],ff_inter_vlc[i][1]);
        add_vlc(intratab,i,ff_mpeg4_intra_vlc[i][0],ff_mpeg4_intra_vlc[i][1]);
    }
    for(unsigned i=0;i<13;i++){
        add_vlc(lumtab,i,ff_mpeg4_DCtab_lum[i][0],ff_mpeg4_DCtab_lum[i][1]);
        add_vlc(chromtab,i,ff_mpeg4_DCtab_chrom[i][0],ff_mpeg4_DCtab_chrom[i][1]);
    }
}
static unsigned peek(const struct bits *b,unsigned n)
{
    size_t byte=b->pos/8;
    unsigned value=0;
    for(size_t i=0;i<3;i++){value<<=8;if(byte+i<b->size)value|=b->data[byte+i];}
    return (value>>(24-(b->pos%8)-n))&((1u<<n)-1);
}
static int get(struct bits *b,unsigned n)
{
    if(n>16 || b->pos+n>b->size*8)return -1;
    unsigned v=peek(b,n);b->pos+=n;return (int)v;
}
static int vlc_get(struct bits *b,const struct vlc *table)
{
    struct vlc v=table[peek(b,LOOK_BITS)];
    if(!v.len || b->pos+v.len>b->size*8)return -1;
    b->pos+=v.len;return v.index;
}
static void put(struct output *o,unsigned len,unsigned value)
{
    if(o->failed || len>24 || o->pos+len>o->cap*8){o->failed=true;return;}
    for(unsigned i=len;i>0;i--){
        /* Zero each output byte on first write, including reused storage.
         * Avoid clearing the entire worst-case 16 MiB capacity each frame. */
        if(o->pos%8==0)o->data[o->pos/8]=0;
        if((value>>(i-1))&1u)o->data[o->pos/8]|=(uint8_t)(1u<<(7-o->pos%8));
        o->pos++;
    }
}
static void copy(struct output *o,const struct bits *b,size_t start,size_t end)
{
    struct bits src=*b;src.pos=start;
    while(start<end&&!o->failed){
        unsigned n=(unsigned)(end-start>16?16:end-start);
        int v=get(&src,n);if(v<0){o->failed=true;return;}
        put(o,n,(unsigned)v);start+=n;
    }
}
static bool read_header(struct bits *b,const struct mpeg4_edit_options *o,int *fcode)
{
    size_t base=SIZE_MAX;
    for(size_t i=0;i+4<b->size;i++)
        if(!b->data[i]&&!b->data[i+1]&&b->data[i+2]==1&&b->data[i+3]==0xb6){
            if(base!=SIZE_MAX)return false;
            base=i+4;
        }
    if(base==SIZE_MAX)return false;
    b->pos=base*8;
    if(get(b,2)!=1)return false;
    unsigned count=0;int v;
    while((v=get(b,1))==1)if(++count>16)return false;
    if(v!=0||get(b,1)!=1)return false;
    unsigned n=1;while((1u<<n)<(unsigned)o->fps)n++;
    v=get(b,n);
    if(v<0||v>=o->fps||get(b,1)!=1||get(b,1)!=1||get(b,1)<0)return false;
    /* Our encoder always sets intra DC threshold code zero (DC VLC
     * for all QP) and never emits field/interlaced syntax. */
    if(get(b,3)!=0 || get(b,5)<=0)return false;
    *fcode=get(b,3);return *fcode>0;
}
static bool motion_get(struct bits *b,int fcode,int *value)
{
    int c=vlc_get(b,mvtab);if(c<0)return false;
    if(!c){*value=0;return true;}
    int sign=get(b,1),shift=fcode-1,r=shift?get(b,(unsigned)shift):0;
    if(sign<0||r<0)return false;
    int v=((c-1)<<shift)+r+1;
    *value=sign?-v:v;return true;
}
static void motion_put(struct output *o,int fcode,int value)
{
    int shift=fcode-1,mag=value<0?-value:value;
    if(!mag){put(o,1,1);return;}
    unsigned code=(unsigned)((mag-1)>>shift)+1;
    put(o,ff_mvtab[code][1],ff_mvtab[code][0]);put(o,1,value<0);
    if(shift)put(o,(unsigned)shift,(unsigned)(mag-1)&((1u<<shift)-1));
}
static int scale_motion(int value,int scale,int fcode)
{
    int bound=32<<(fcode-1);
    int v=value*scale/100;
    if(v < -bound)v=-bound;
    if(v > bound-1)v=bound-1;
    return v;
}

struct coeff {int pos,level;};
struct block {struct coeff c[64];int count;size_t start,end;};
static int max_level(bool intra,bool last,int run)
{
    const int8_t *runs=intra?ff_mpeg4_intra_run:ff_inter_run;
    const int8_t *levels=intra?ff_mpeg4_intra_level:ff_inter_level;
    int split=intra?67:58,max=0;
    for(int i=last?split:0;i<(last?102:split);i++)if(runs[i]==run&&levels[i]>max)max=levels[i];
    return max;
}
static int max_run(bool intra,bool last,int level)
{
    const int8_t *runs=intra?ff_mpeg4_intra_run:ff_inter_run;
    const int8_t *levels=intra?ff_mpeg4_intra_level:ff_inter_level;
    int split=intra?67:58,max=-1;
    for(int i=last?split:0;i<(last?102:split);i++)if(levels[i]==level&&runs[i]>max)max=runs[i];
    return max;
}
static bool read_coeff(struct bits *b,bool intra,int *run,int *level,bool *last)
{
    const struct vlc *table=intra?intratab:intertab;
    const int8_t *runs=intra?ff_mpeg4_intra_run:ff_inter_run;
    const int8_t *levels=intra?ff_mpeg4_intra_level:ff_inter_level;
    int split=intra?67:58;
    int idx=vlc_get(b,table),escape=0;
    if(idx<0)return false;
    if(idx==102){
        int first=get(b,1);if(first<0)return false;
        if(!first)escape=1;
        else {int second=get(b,1);if(second<0)return false;escape=second?3:2;}
        if(escape==3){
            int l=get(b,1),r=get(b,6);
            if(l<0||r<0||get(b,1)!=1)return false;
            int v=get(b,12);if(v<0||get(b,1)!=1)return false;
            if(v&2048)v-=4096;
            if(!v||v==-2048)return false;
            *last=l!=0;*run=r;*level=v;return true;
        }
        idx=vlc_get(b,table);if(idx<0||idx>=102)return false;
    }
    *last=idx>=split;*run=runs[idx];*level=levels[idx];
    if(escape==1)*level+=max_level(intra,*last,*run);
    if(escape==2)*run+=max_run(intra,*last,*level)+1;
    int sign=get(b,1);if(sign<0)return false;
    if(sign)*level=-*level;
    return true;
}
static bool read_block(struct bits *b,bool intra,bool coded,struct block *blk,int n)
{
    blk->start=b->pos;blk->count=0;
    if(intra){
        int size=vlc_get(b,n<4?lumtab:chromtab);
        if(size<0||size>12||get(b,(unsigned)size)<0||(size>8&&get(b,1)!=1))return false;
    }
    if(coded){
        int pos=intra?0:-1;
        for(;;){
            int run,level;bool last;
            if(!read_coeff(b,intra,&run,&level,&last))return false;
            pos+=run+1;
            if(pos>63||blk->count>=64)return false;
            blk->c[blk->count++]=(struct coeff){pos,level};
            if(last)break;
        }
    }
    blk->end=b->pos;return true;
}
static void write_block(struct output *o,const struct block *b)
{
    int prev=-1;
    for(int i=0;i<b->count;i++){
        int run=b->c[i].pos-prev-1,level=b->c[i].level,mag=level<0?-level:level;
        bool last=i==b->count-1;
        int index=-1;
        for(int j=last?58:0;j<(last?102:58);j++)
            if(ff_inter_run[j]==run&&ff_inter_level[j]==mag){index=j;break;}
        if(index>=0){
            put(o,ff_inter_vlc[index][1],ff_inter_vlc[index][0]);put(o,1,level<0);
        }else{
            put(o,7,3);put(o,2,3);put(o,1,last);put(o,6,(unsigned)run);put(o,1,1);
            put(o,12,(unsigned)level&4095u);put(o,1,1);
        }
        prev=b->c[i].pos;
    }
}
static uint32_t hash(uint32_t v)
{
    v^=v>>16;v*=0x7feb352du;v^=v>>15;v*=0x846ca68bu;return v^(v>>16);
}
static bool transcribe_mb(struct bits *b,struct output *o,const struct mpeg4_edit_options *opt,
                          int fcode,unsigned mb,struct mpeg4_edit_stats *stats)
{
    size_t start=b->pos;
    int skip=get(b,1),cbpc;
    if(skip<0)return false;
    if(skip){
        /* A skipped macroblock carries no residuals or explicit motion.
         * Turn selected ones into Inter/CBP=0 and write bounded *deltas*.
         * Subsequent MB predictors see this newly coded vector: this is
         * deliberately codec-native corruption, not absolute vector control. */
        uint32_t h=hash(0xa513c917u ^ (uint32_t)mb*0x9e3779b9u);
        if(!opt->phantom_percent || h%100u >= (unsigned)opt->phantom_percent){
            put(o,1,1);
            return true;
        }
        int step=1+opt->phantom_percent/20; /* 1..6 half-pel units */
        int dx=(h&0x100u)?step:-step;
        int dy=(h&0x200u)?step:-step;
        put(o,1,0); /* no longer skipped */
        put(o,ff_h263_inter_MCBPC_bits[0],ff_h263_inter_MCBPC_code[0]);
        put(o,ff_h263_cbpy_tab[15][1],ff_h263_cbpy_tab[15][0]);
        motion_put(o,fcode,dx);
        motion_put(o,fcode,dy);
        stats->phantom_blocks++;
        stats->vectors+=2;
        return !o->failed;
    }
    cbpc=vlc_get(b,mbtab);
    /* Our own encoder doesn't require macroblock stuffing. Fail closed. */
    if(cbpc<0||cbpc==20)return false;
    bool intra=(cbpc&4)!=0,q=(cbpc&8)!=0;
    if(intra&&get(b,1)<0)return false;
    int py=vlc_get(b,pytab);if(py<0)return false;
    if(!intra)py^=15;
    int cbp=(py<<2)|(cbpc&3),quant=q?get(b,2):0;
    if(quant<0)return false;
    int count=(!intra)?((cbpc&16)?8:2):0;
    int mv[8]={0};
    for(int i=0;i<count;i++)if(!motion_get(b,fcode,&mv[i]))return false;
    struct block blocks[6];
    for(int n=0;n<6;n++)if(!read_block(b,intra,(cbp&(32>>n))!=0,&blocks[n],n))return false;
    if(intra){copy(o,b,start,b->pos);return !o->failed;}
    int new_cbp=cbp;
    bool changed=false,changed_block[6]={false};
    for(int i=0;i<count;i++){
        int scale=(i&1)?opt->y_scale:opt->x_scale;
        int next=scale==100?mv[i]:scale_motion(mv[i],scale,fcode);
        if(next!=mv[i]){stats->vectors++;changed=true;mv[i]=next;}
    }
    for(int n=0;n<6;n++){
        struct block *blk=&blocks[n];int c=0;
        bool drop=blk->count&&opt->residual_loss>0&&
            hash(opt->seed^(uint32_t)(mb*6+n))%100u<(unsigned)opt->residual_loss;
        for(int i=0;i<blk->count;i++){
            int v=blk->c[i].level, pos=blk->c[i].pos;
            /* Positions are progressive zigzag scan indices, NOT absolute
             * spatial frequency or independent horizontal/vertical axes. */
            int band=pos<10?0:pos<32?1:2;
            int base=v*opt->residual_scale/100;
            int scaled=drop?0:opt->frequency[band]==0?base:
                v*opt->residual_scale*(100+opt->frequency[band])/10000;
            if(scaled>2047)scaled=2047;
            if(scaled< -2047)scaled=-2047;
            if(base>2047)base=2047;
            if(base< -2047)base=-2047;
            if(!drop && opt->frequency[band] && base!=scaled)
                stats->frequency_coefficients[band]++;
            if(v!=scaled){stats->coefficients++;changed=true;changed_block[n]=true;}
            if(scaled)blk->c[c++]=(struct coeff){pos,scaled};
        }
        if(blk->count&&!c){new_cbp&=~(32>>n);stats->blocks++;}
        blk->count=c;
    }
    if(!changed){copy(o,b,start,b->pos);return !o->failed;}
    int new_cbpc=(cbpc&~3)|(new_cbp&3),new_py=(new_cbp>>2)^15;
    put(o,1,0);put(o,ff_h263_inter_MCBPC_bits[new_cbpc],ff_h263_inter_MCBPC_code[new_cbpc]);
    put(o,ff_h263_cbpy_tab[new_py][1],ff_h263_cbpy_tab[new_py][0]);if(q)put(o,2,(unsigned)quant);
    for(int i=0;i<count;i++)motion_put(o,fcode,mv[i]);
    for(int n=0;n<6;n++)if(new_cbp&(32>>n)){
        if(changed_block[n])write_block(o,&blocks[n]);else copy(o,b,blocks[n].start,blocks[n].end);
    }
    return !o->failed;
}

void mpeg4_edit_buffer_destroy(struct mpeg4_edit_buffer *scratch)
{
    if(!scratch)return;
    free(scratch->data);
    *scratch=(struct mpeg4_edit_buffer){0};
}

static int edit_p_vop(const uint8_t *data,size_t len,const struct mpeg4_edit_options *opt,
                      struct mpeg4_edit_buffer *scratch,
                      uint8_t **out,size_t *out_size,struct mpeg4_edit_stats *stats)
{
    if(out)*out=NULL;
    if(out_size)*out_size=0;
    if(stats)*stats=(struct mpeg4_edit_stats){0};
    if(!data||!out||!out_size||!stats||!opt||len<8||len>4u*1024u*1024u||
       opt->width<16||opt->width>2048||opt->height<16||opt->height>2048||
       opt->width%16||opt->height%16||opt->fps<1||opt->fps>60||
       opt->x_scale< -500||opt->x_scale>500||opt->y_scale< -500||opt->y_scale>500||
       opt->residual_scale< -200||opt->residual_scale>200||opt->residual_loss<0||opt->residual_loss>100||
       opt->phantom_percent<0||opt->phantom_percent>100||
       opt->frequency[0]< -100||opt->frequency[0]>200||
       opt->frequency[1]< -100||opt->frequency[1]>200||
       opt->frequency[2]< -100||opt->frequency[2]>200)return -1;
    struct bits b={data,len,0};int fcode;
    if(!read_header(&b,opt,&fcode))return -1;
    if(opt->x_scale==100&&opt->y_scale==100&&opt->residual_scale==100&&
       !opt->residual_loss&&!opt->phantom_percent&&
       !opt->frequency[0]&&!opt->frequency[1]&&!opt->frequency[2])return 0;
    framerot_init_vlc_once(init_tables);
    size_t mb_count=(size_t)(opt->width/16)*(size_t)(opt->height/16);
    /* Bound by max 6x64 coefficients, each escape <=30 bits, per MB.
     * Allocate only as much as could be needed, plus the small header. */
    size_t cap=len+mb_count*1500+64;
    if(cap>16u*1024u*1024u)cap=16u*1024u*1024u;
    uint8_t *bytes=NULL;
    if(scratch){
        if(scratch->capacity<cap){
            uint8_t *grown=realloc(scratch->data,cap);
            if(!grown)return -1;
            scratch->data=grown;
            scratch->capacity=cap;
        }
        bytes=scratch->data;
    }else{
        bytes=malloc(cap);
        if(!bytes)return -1;
    }
    struct output o={bytes,cap,0,false};
    struct mpeg4_edit_stats local={0};
    copy(&o,&b,0,b.pos);
    for(size_t mb=0;mb<mb_count;mb++)
        if(!transcribe_mb(&b,&o,opt,fcode,(unsigned)mb,&local))goto unsupported;
    /* MPEG-4 stuffing at frame end: 0 followed by one bits to byte
     * alignment. Reject unknown tails, slices, or resync syntax. */
    size_t remaining=b.size*8-b.pos;
    if(remaining==0 || remaining>8 || get(&b,1)!=0)goto unsupported;
    while(b.pos<b.size*8)if(get(&b,1)!=1)goto unsupported;
    if(!local.vectors&&!local.coefficients&&!local.blocks&&!local.phantom_blocks){
        if(!scratch)free(o.data);
        return 0;
    }
    put(&o,1,0);while(!o.failed&&o.pos%8)put(&o,1,1);
    if(o.failed)goto unsupported;
    *out=o.data;*out_size=o.pos/8;*stats=local;return 1;
unsupported:
    if(!scratch)free(o.data);
    return -1;
}

int mpeg4_edit_p_vop(const uint8_t *data,size_t len,const struct mpeg4_edit_options *opt,
                     uint8_t **out,size_t *out_size,struct mpeg4_edit_stats *stats)
{
    return edit_p_vop(data,len,opt,NULL,out,out_size,stats);
}

int mpeg4_edit_p_vop_reuse(const uint8_t *data,size_t len,const struct mpeg4_edit_options *opt,
                           struct mpeg4_edit_buffer *scratch,
                           uint8_t **out,size_t *out_size,struct mpeg4_edit_stats *stats)
{
    if(!scratch){
        if(out)*out=NULL;
        if(out_size)*out_size=0;
        if(stats)*stats=(struct mpeg4_edit_stats){0};
        return -1;
    }
    return edit_p_vop(data,len,opt,scratch,out,out_size,stats);
}
