/* Real codec regression: independently cut three MPEG-4 inter-residual scan
 * bands, verify actual decoded pixels and safe in-flight control updates. */
#undef NDEBUG
#include "../src/video-roundtrip.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum { W=192,H=144,COUNT=72 };
static void texture(uint8_t *p,int t)
{
    for(int y=0;y<H;y++)for(int x=0;x<W;x++){
        const size_t i=((size_t)y*W+x)*4;
        int xx=x+3*t,yy=y+2*t;
        p[i]=(uint8_t)((xx*17 ^ yy*29 ^ t*13)&255);
        p[i+1]=(uint8_t)((((xx/3)+(yy/5)+t*7)&1)?240:15);
        p[i+2]=(uint8_t)((xx*yy/3+t*31)&255);
        p[i+3]=255;
    }
}
static struct retro_video_options settings(void)
{
    struct retro_video_options o=retro_video_default_options(RETRO_VIDEO_MPEG4_PART2);
    o.width=W;o.height=H;o.fps=15;o.bitrate=1600000;o.gop_size=35;
    o.mosh_mode=RETRO_MOSH_INSTRUMENT;o.mosh_layers=RETRO_LAYER_COMPRESSED;
    o.mosh_mv_x=o.mosh_mv_y=100;o.mosh_residual=100;
    return o;
}
static void preset_bitrate(void)
{
    /* Exercise the shipped Spectral Melt values at the actual recipe bitrate,
     * not only high-bitrate texture footage. */
    struct retro_video_options o=settings();
    o.fps=18;o.bitrate=360000;o.gop_size=30;
    o.mosh_frequency[1]=-50;o.mosh_frequency[2]=-100;
    struct retro_video *altered=retro_video_create(&o);assert(altered);
    o.mosh_layers=0;
    struct retro_video *clean=retro_video_create(&o);assert(clean);
    uint8_t src[W*H*4],a[W*H*4],b[W*H*4];
    int different=0;
    for(int t=0;t<72;t++){
        texture(src,t);
        assert(retro_video_process(clean,src,W*4,a,W*4)==1);
        assert(retro_video_process(altered,src,W*4,b,W*4)==1);
        different+=memcmp(a,b,sizeof a)!=0;
    }
    printf("SPECTRAL_MELT different=%d mid=%llu high=%llu unsupported=%llu\n",
        different,
        (unsigned long long)retro_video_compressed_frequency_coefficients(altered,1),
        (unsigned long long)retro_video_compressed_frequency_coefficients(altered,2),
        (unsigned long long)retro_video_compressed_unsupported(altered));
    assert(different>=40);
    assert(retro_video_compressed_frequency_coefficients(altered,1)>100);
    assert(retro_video_compressed_unsupported(altered)==0);
    retro_video_destroy(clean);retro_video_destroy(altered);
}
static void fracture_preset_bitrate(void)
{
    /* The Spectral Fracture recipe should actually alter decoded output at
     * its shipped bitrate and look different from conservative Spectral Melt.
     * Each stream gets the same input texture and separate decoder state. */
    struct retro_video_options o=settings();
    o.fps=18;o.bitrate=340000;o.gop_size=42;
    o.mosh_residual=135;
    o.mosh_frequency[0]=-45;o.mosh_frequency[1]=120;o.mosh_frequency[2]=200;
    struct retro_video *fracture=retro_video_create(&o);assert(fracture);
    struct retro_video_options melt=o;
    melt.mosh_residual=100;melt.mosh_frequency[0]=0;
    melt.mosh_frequency[1]=-50;melt.mosh_frequency[2]=-100;
    struct retro_video *soft=retro_video_create(&melt);assert(soft);
    o.mosh_layers=0;
    struct retro_video *clean=retro_video_create(&o);assert(clean);
    uint8_t src[W*H*4],a[W*H*4],b[W*H*4],c[W*H*4];
    int changed=0,distinct=0;
    for(int t=0;t<90;t++){
        texture(src,t);
        assert(retro_video_process(clean,src,W*4,a,W*4)==1);
        assert(retro_video_process(fracture,src,W*4,b,W*4)==1);
        assert(retro_video_process(soft,src,W*4,c,W*4)==1);
        changed+=memcmp(a,b,sizeof a)!=0;
        distinct+=memcmp(b,c,sizeof b)!=0;
    }
    assert(changed>60&&distinct>60);
    for(int band=0;band<3;band++)
        assert(retro_video_compressed_frequency_coefficients(fracture,band)>100);
    assert(retro_video_compressed_unsupported(fracture)==0);
    assert(retro_video_compressed_unsupported(soft)==0);
    printf("SPECTRAL_FRACTURE changed=%d distinct_from_melt=%d low=%llu mid=%llu high=%llu unsupported=%llu\n",
        changed,distinct,
        (unsigned long long)retro_video_compressed_frequency_coefficients(fracture,0),
        (unsigned long long)retro_video_compressed_frequency_coefficients(fracture,1),
        (unsigned long long)retro_video_compressed_frequency_coefficients(fracture,2),
        (unsigned long long)retro_video_compressed_unsupported(fracture));
    retro_video_destroy(clean);retro_video_destroy(soft);retro_video_destroy(fracture);
}
int main(void)
{
    preset_bitrate();
    fracture_preset_bitrate();
    struct retro_video_options o=settings();
    struct retro_video *clean=retro_video_create(&o);assert(clean);
    struct retro_video *v[3];
    for(int j=0;j<3;j++){
        struct retro_video_options b=o;
        b.mosh_frequency[j]=-100;
        v[j]=retro_video_create(&b);assert(v[j]);
    }
    uint8_t src[W*H*4], reference[W*H*4], altered[3][W*H*4];
    int changed[3]={0},distinct=0,frames=0;
    for(int t=0;t<COUNT;t++){
        texture(src,t);
        int c=retro_video_process(clean,src,W*4,reference,W*4);
        assert(c==1);frames++;
        for(int j=0;j<3;j++){
            int r=retro_video_process(v[j],src,W*4,altered[j],W*4);
            assert(r==c);
            changed[j]+=(memcmp(reference,altered[j],sizeof reference)!=0);
        }
        if(memcmp(altered[0],altered[1],sizeof reference)!=0 &&
           memcmp(altered[1],altered[2],sizeof reference)!=0)distinct++;
    }
    for(int j=0;j<3;j++){
        uint64_t edits=retro_video_compressed_frequency_coefficients(v[j],j);
        printf("FREQUENCY band=%d frames=%d changed=%d edited=%llu unsupported=%llu\n",
           j,frames,changed[j],(unsigned long long)edits,
           (unsigned long long)retro_video_compressed_unsupported(v[j]));
        assert(changed[j]>25);
        assert(edits>30);
        assert(retro_video_compressed_unsupported(v[j])==0);
        for(int k=0;k<3;k++)if(k!=j)
            assert(retro_video_compressed_frequency_coefficients(v[j],k)==0);
    }
    assert(distinct>25);
    /* Live change is scalar-only; it must immediately move edits to the
     * selected band without resetting the codec or kept reference. */
    struct retro_video_options b=o;b.mosh_frequency[0]=-100;
    struct retro_video *live=retro_video_create(&b);assert(live);
    uint64_t low_before_switch=0;
    for(int t=0;t<45;t++){
        if(t==20){
            low_before_switch=retro_video_compressed_frequency_coefficients(live,0);
            assert(low_before_switch>0);
            b.mosh_frequency[0]=0;b.mosh_frequency[2]=-100;
            assert(retro_video_set_live_options(live,&b));
        }
        texture(src,t);
        assert(retro_video_process(live,src,W*4,altered[0],W*4)==1);
    }
    assert(retro_video_compressed_frequency_coefficients(live,0)==low_before_switch);
    assert(retro_video_compressed_frequency_coefficients(live,2)>0);
    assert(retro_video_compressed_unsupported(live)==0);
    /* Out of range live settings must be rejected, not partially applied. */
    b.mosh_frequency[1]=201;assert(!retro_video_set_live_options(live,&b));
    retro_video_destroy(live);
    /* Verify signed gain, clipping and Phantom Drift can compose in one
     * legitimate decoder stream with normal clean I-frame recovery. */
    b=o;b.mosh_frequency[0]=200;b.mosh_frequency[1]=-75;
    b.mosh_frequency[2]=200;b.mosh_phantom=55;
    struct retro_video *composed=retro_video_create(&b);assert(composed);
    int active=0;
    for(int t=0;t<90;t++){
        texture(src,t<65?t:65); /* force skipped MBs on the static tail */
        assert(retro_video_process(composed,src,W*4,altered[0],W*4)==1);
        active++;
    }
    assert(active==90);
    assert(retro_video_compressed_frequency_coefficients(composed,0)>100);
    assert(retro_video_compressed_frequency_coefficients(composed,1)>100);
    assert(retro_video_compressed_frequency_coefficients(composed,2)>100);
    assert(retro_video_compressed_phantom_blocks(composed)>100);
    assert(retro_video_compressed_unsupported(composed)==0);
    printf("FREQUENCY_COMPOSED low=%llu mid=%llu high=%llu phantom=%llu\n",
        (unsigned long long)retro_video_compressed_frequency_coefficients(composed,0),
        (unsigned long long)retro_video_compressed_frequency_coefficients(composed,1),
        (unsigned long long)retro_video_compressed_frequency_coefficients(composed,2),
        (unsigned long long)retro_video_compressed_phantom_blocks(composed));
    retro_video_destroy(composed);
    for(int j=0;j<3;j++)retro_video_destroy(v[j]);
    retro_video_destroy(clean);
    o.mosh_frequency[0]=-101;assert(!retro_video_create(&o));
    o.mosh_frequency[0]=0;o.mosh_frequency[2]=201;assert(!retro_video_create(&o));
    return 0;
}
