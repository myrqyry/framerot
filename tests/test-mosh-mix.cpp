#include <obs.h>
#include <obs-module.h>
#include <graphics/graphics.h>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <thread>
#include <vector>
#include <array>

static constexpr int W=128,H=96;
static const char *name(void *) {return "Framerot alpha pattern";}
static uint32_t width(void *) {return W;}
static uint32_t height(void *) {return H;}
static void *create(obs_data_t *,obs_source_t *)
{
    std::vector<uint8_t> bytes(W*H*4);
    for(int y=0;y<H;y++) for(int x=0;x<W;x++) {
        uint8_t *p=&bytes[(y*W+x)*4];
        int a=x<W/3?0:(x<2*W/3?128:255);
        /* Input source outputs premultiplied RGBA. High frequency content
         * is deliberately impossible to retain through a 64px codec. */
        p[0]=((x+y)%2?240:30)*a/255;
        p[1]=80*a/255;p[2]=200*a/255;p[3]=a;
    }
    const uint8_t *ptr[]={bytes.data()};
    obs_enter_graphics();auto *tex=gs_texture_create(W,H,GS_RGBA,1,ptr,0);obs_leave_graphics();
    return tex;
}
static void destroy(void *t){obs_enter_graphics();gs_texture_destroy((gs_texture_t*)t);obs_leave_graphics();}
static void render(void *t,gs_effect_t *){obs_source_draw((gs_texture_t*)t,0,0,W,H,false);}
static obs_source_info info={.id="framerot_mix_input",.type=OBS_SOURCE_TYPE_INPUT,
    .output_flags=OBS_SOURCE_VIDEO|OBS_SOURCE_SRGB,.get_name=name,.create=create,.destroy=destroy,
    .get_width=width,.get_height=height,.video_render=render};

static std::vector<uint8_t> sample(obs_source_t *s)
{
    std::vector<uint8_t> pixels(W*H*4);
    obs_enter_graphics();
    auto *tr=gs_texrender_create(GS_RGBA,GS_ZS_NONE);
    if(gs_texrender_begin_with_color_space(tr,W,H,GS_CS_SRGB)){
        vec4 bg{0,0,0,0}; gs_clear(GS_CLEAR_COLOR,&bg,1,0);
        gs_projection_push();gs_ortho(0,W,0,H,-100,100);
        gs_blend_state_push();gs_reset_blend_state();gs_enable_blending(true);
        gs_blend_function(GS_BLEND_ONE,GS_BLEND_INVSRCALPHA);
        obs_source_video_render(s);
        gs_blend_state_pop();gs_projection_pop();gs_texrender_end(tr);
        auto *stage=gs_stagesurface_create(W,H,GS_RGBA);
        gs_stage_texture(stage,gs_texrender_get_texture(tr));
        uint8_t *p=nullptr;uint32_t stride=0;
        if(gs_stagesurface_map(stage,&p,&stride)){
            for(int y=0;y<H;y++)std::copy_n(p+y*stride,W*4,&pixels[y*W*4]);
            gs_stagesurface_unmap(stage);
        }
        gs_stagesurface_destroy(stage);
    }
    gs_texrender_destroy(tr);obs_leave_graphics();return pixels;
}
int main(int argc,char **argv){
    if(argc!=3||!obs_startup("en-US",nullptr,nullptr))return 2;
    obs_video_info vi{};vi.graphics_module="libobs-opengl";vi.fps_num=30;vi.fps_den=1;
    vi.base_width=vi.output_width=W;vi.base_height=vi.output_height=H;vi.output_format=VIDEO_FORMAT_RGBA;vi.scale_type=OBS_SCALE_DISABLE;
    if(obs_reset_video(&vi)!=OBS_VIDEO_SUCCESS)return 3;
    obs_register_source(&info);obs_module_t *m=nullptr;
    if(obs_open_module(&m,argv[1],argv[2])!=MODULE_SUCCESS||!obs_init_module(m))return 4;
    auto *s=obs_source_create(info.id,"alpha-input",nullptr,nullptr);auto dry=sample(s);
    auto *settings=obs_data_create();obs_data_set_int(settings,"mode",0);obs_data_set_int(settings,"codec_height",64);obs_data_set_int(settings,"fps",30);
    obs_data_set_int(settings,"wet_mix",100);obs_data_set_bool(settings,"preserve_alpha",true);
    auto *f=obs_source_create("framerot_filter","alpha-filter",settings,nullptr);
    obs_source_filter_add(s,f);obs_set_output_source(0,s);
    auto get=[&](int mix,bool alpha){obs_data_set_int(settings,"wet_mix",mix);obs_data_set_bool(settings,"preserve_alpha",alpha);obs_source_update(f,settings);
        std::vector<uint8_t> result;
        for(int i=0;i<12;i++){std::this_thread::sleep_for(std::chrono::milliseconds(50));result=sample(s);}return result;};
    auto wet=get(100,true),half=get(50,true),zero=get(0,true),opaque=get(100,false);
    size_t transparent=(H/2*W+10)*4, partial=(H/2*W+W/2)*4;
    int diff=0,blend=0,zero_error=0;
    for(int y=10;y<H-10;y++)for(int x=2*W/3+5;x<W-5;x++){
        size_t idx=(y*W+x)*4;
        diff+=std::abs(int(wet[idx])-int(dry[idx]));
        blend+=std::abs(int(half[idx])-(int(wet[idx])+int(dry[idx]))/2);
        zero_error+=std::abs(int(zero[idx])-int(dry[idx]));
    }
    std::printf("MIX dry_alpha=%d wet_alpha=%d half_alpha=%d transparent=%d opaque=%d diff=%d blend_error=%d zero_error=%d\n",dry[partial+3],wet[partial+3],half[partial+3],wet[transparent+3],opaque[transparent+3],diff,blend,zero_error);
    bool ok=dry[partial+3]>=120&&dry[partial+3]<=136&&std::abs(int(wet[partial+3])-int(dry[partial+3]))<5&&
        std::abs(int(half[partial+3])-int(dry[partial+3]))<5&&wet[transparent+3]<5&&opaque[transparent+3]>245&&
        diff>10000&&blend<diff/5&&zero_error<1000;
    // A second filter in the stack must have its own codec and preserve the first's output.
    auto *other=obs_source_create("framerot_filter","texture-filter",nullptr,nullptr);
    obs_source_filter_add(s,other);
    auto *profile=obs_data_create();
    obs_data_set_int(profile,"mode",9);
    obs_data_set_int(profile,"codec_height",64);
    obs_data_set_int(profile,"kbps",48);
    obs_data_set_int(profile,"fps",30);
    obs_data_set_int(profile,"wet_mix",100);
    obs_data_set_bool(profile,"preserve_alpha",true);
    auto *properties=obs_source_properties(other);
    auto *mode=obs_properties_get(properties,"mode");
    if(obs_property_list_item_count(mode)!=10 || obs_properties_get(properties,"crush_codec"))return 6;
    obs_properties_destroy(properties);
    for(int c:{0,4,9}){
        obs_data_set_int(profile,"mode",c);
        obs_source_update(other,profile);
        std::vector<uint8_t> result;
        for(int i=0;i<12;i++){std::this_thread::sleep_for(std::chrono::milliseconds(60));result=sample(s);}
        int changed=0;
        for(int y=10;y<H-10;y++)for(int x=2*W/3+5;x<W-5;x++){
            size_t idx=(y*W+x)*4;
            changed+=std::abs(int(result[idx])-int(opaque[idx]));
        }
        std::printf("MOSH_STACK mode=%d delta=%d alpha=%d\n",c,changed,result[partial+3]);
        ok=ok&&changed>1000&&result[partial+3]>245;
    }
    obs_data_release(profile);obs_source_filter_remove(s,other);obs_source_release(other);
    obs_set_output_source(0,nullptr);obs_source_filter_remove(s,f);obs_source_release(f);obs_source_release(s);obs_data_release(settings);obs_shutdown();
    return ok?0:5;
}
