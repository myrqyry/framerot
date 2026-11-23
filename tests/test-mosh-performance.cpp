#include <obs.h>
#include <obs-module.h>
#include <graphics/graphics.h>
#include <atomic>
#include <chrono>
#include <charconv>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <thread>
#include <vector>

static constexpr uint32_t W=1920,H=1080;
struct Input {gs_texture_t *tex;std::atomic<unsigned> tick{0};};
static const char *name(void *){return "Framerot performance pattern";}
static uint32_t width(void *){return W;}
static uint32_t height(void *){return H;}
static void *create(obs_data_t *,obs_source_t *)
{
    auto *p=new Input{};
    std::vector<uint8_t> pixels(W*H*4);
    for(unsigned y=0;y<H;y++) for(unsigned x=0;x<W;x++){
        size_t i=(size_t)(y*W+x)*4;
        pixels[i]=(uint8_t)(x/3+y);pixels[i+1]=(uint8_t)((x/32^y/32)*43);
        pixels[i+2]=(uint8_t)(x+y*3);pixels[i+3]=255;
    }
    const uint8_t *data[]={pixels.data()};
    obs_enter_graphics();p->tex=gs_texture_create(W,H,GS_RGBA,1,data,0);obs_leave_graphics();
    return p;
}
static void destroy(void *data){auto *p=(Input*)data;obs_enter_graphics();gs_texture_destroy(p->tex);obs_leave_graphics();delete p;}
static void tick(void *data,float){((Input*)data)->tick.fetch_add(1);}
static void render(void *data,gs_effect_t *){
    auto *p=(Input*)data;int phase=(int)(p->tick.load()%64);
    obs_source_draw(p->tex,-phase,-phase,W+64,H+64,false);
}
static obs_source_info info={.id="framerot_perf_input",.type=OBS_SOURCE_TYPE_INPUT,
    .output_flags=OBS_SOURCE_VIDEO,.get_name=name,.create=create,.destroy=destroy,
    .get_width=width,.get_height=height,.video_tick=tick,.video_render=render};

static bool decimal(const char *s,int min,int max,int *out)
{
    int v=0;
    auto r=std::from_chars(s,s+std::strlen(s),v);
    if(r.ec!=std::errc{}||*r.ptr||r.ptr==s||v<min||v>max)return false;
    *out=v;
    return true;
}
int main(int argc,char **argv)
{
    int seconds=8,codec_height=288;
    if(argc!=3 && argc!=5)return 2;
    if(argc==5 && (!decimal(argv[3],2,60,&seconds) ||
                   !decimal(argv[4],64,576,&codec_height) || codec_height%16))return 2;
    if(!obs_startup("en-US",nullptr,nullptr))return 2;
    obs_video_info v{};v.graphics_module="libobs-opengl";v.fps_num=60;v.fps_den=1;
    v.base_width=v.output_width=W;v.base_height=v.output_height=H;
    v.output_format=VIDEO_FORMAT_NV12;v.colorspace=VIDEO_CS_709;v.range=VIDEO_RANGE_PARTIAL;v.scale_type=OBS_SCALE_DISABLE;
    if(obs_reset_video(&v)!=OBS_VIDEO_SUCCESS)return 3;
    obs_register_source(&info);obs_module_t *module=nullptr;
    if(obs_open_module(&module,argv[1],argv[2])!=MODULE_SUCCESS||!obs_init_module(module))return 4;
    auto *s=obs_source_create(info.id,"perf-input",nullptr,nullptr);
    auto *settings=obs_data_create();
    obs_data_set_int(settings,"mode",9);obs_data_set_int(settings,"fps",30);obs_data_set_int(settings,"codec_height",codec_height);
    obs_data_set_int(settings,"kbps",650);obs_data_set_int(settings,"gop",24);obs_data_set_int(settings,"reference",2);
    obs_data_set_bool(settings,"layer_flow",false);obs_data_set_bool(settings,"layer_compressed",true);obs_data_set_int(settings,"mv_x",180);
    obs_data_set_int(settings,"residual_scale",65);obs_data_set_int(settings,"residual_loss",15);
    obs_data_set_bool(settings,"preserve_alpha",false);
    auto *f=obs_source_create("framerot_filter","perf-filter",settings,nullptr);obs_data_release(settings);
    if(!s||!f)return 5;
    obs_set_output_source(0,s);
    auto measure=[seconds](const char *label){
        std::this_thread::sleep_for(std::chrono::seconds(1));
        uint32_t a=obs_get_total_frames(),b=obs_get_lagged_frames();
        std::this_thread::sleep_for(std::chrono::seconds(seconds));
        uint32_t frames=obs_get_total_frames()-a,lag=obs_get_lagged_frames()-b;
        std::printf("PERF %s frames=%u lagged=%u average_ms=%.3f\n",label,frames,lag,obs_get_average_frame_time_ns()/1e6);
        return frames>0;
    };
    bool ok=measure("baseline");obs_source_filter_add(s,f);
    ok=measure("instrument")&&ok;
    obs_set_output_source(0,nullptr);obs_source_filter_remove(s,f);obs_source_release(f);obs_source_release(s);obs_shutdown();
    return ok?0:6;
}
