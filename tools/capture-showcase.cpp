// GPL-2.0. Capture the installed OBS filter on a supplied raw RGBA clip.
// This process owns a separate libobs context; never opens the user's scenes.
#include <obs.h>
#include <graphics/graphics.h>
#include <atomic>
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <mutex>
#include <string>
#include <vector>

static uint32_t W,H;
static std::vector<uint8_t> clip;
static size_t frames;
static std::atomic<bool> playing{false};
static std::atomic<size_t> position{0};
static const char *name(void*) {return "Showcase footage";}
static uint32_t width(void*) {return W;}
static uint32_t height(void*) {return H;}
static void *create(obs_data_t*,obs_source_t*) {
    obs_enter_graphics();
    auto *t=gs_texture_create(W,H,GS_RGBA,1,nullptr,GS_DYNAMIC);
    obs_leave_graphics();return t;
}
static void destroy(void *t) {
    obs_enter_graphics();gs_texture_destroy(static_cast<gs_texture_t*>(t));obs_leave_graphics();
}
static void tick(void*,float) {if(playing.load())position.fetch_add(1);}
static void render(void *t,gs_effect_t*) {
    size_t i=position.load()%frames;
    gs_texture_set_image(static_cast<gs_texture_t*>(t),clip.data()+i*size_t(W)*H*4,W*4,false);
    obs_source_draw(static_cast<gs_texture_t*>(t),0,0,W,H,false);
}
struct Capture {
    std::mutex lock;
    std::condition_variable done;
    std::vector<uint8_t> bytes;
    size_t count=0, warmup=0, target=0, received=0;
};
static void receive(void *opaque,video_data *v) {
    auto &s=*static_cast<Capture*>(opaque);
    std::lock_guard<std::mutex> guard(s.lock);
    if(s.count==s.target)return;
    if(s.received++<s.warmup){
        if(s.received==s.warmup)playing.store(true);
        return;
    }
    if(v->linesize[0]<W*4 || !v->data[0])return;
    for(uint32_t y=0;y<H;y++)
        std::copy_n(v->data[0]+size_t(y)*v->linesize[0],W*4,
                    s.bytes.data()+(s.count*H+y)*W*4);
    if(++s.count==s.target)s.done.notify_one();
}
int main(int argc,char **argv) {
    if(argc!=10){
        std::cerr<<"usage: capture-showcase MODULE DATA INPUT.rgba W H FPS LOOPS PRESET OUTPUT.bgra\n";
        return 2;
    }
    W=std::stoul(argv[4]);H=std::stoul(argv[5]);
    unsigned fps=std::stoul(argv[6]),loops=std::stoul(argv[7]);
    std::string preset=argv[8];
    if(W<16||H<16||W>1920||H>1080||!fps||fps>60||!loops||loops>4)return 2;
    std::ifstream input(argv[3],std::ios::binary|std::ios::ate);
    if(!input)return 2;
    auto length=input.tellg();
    size_t frame_size=size_t(W)*H*4;
    if(length<=0||size_t(length)%frame_size||size_t(length)>256*1024*1024)return 2;
    clip.resize(size_t(length));input.seekg(0);input.read(reinterpret_cast<char*>(clip.data()),length);
    if(!input)return 2;
    frames=clip.size()/frame_size;
    if(clip.size()*loops>512*1024*1024)return 2;
    if(!obs_startup("en-US",nullptr,nullptr))return 3;
    obs_video_info vi{};
    vi.graphics_module="libobs-opengl";
    vi.fps_num=fps;vi.fps_den=1;vi.base_width=vi.output_width=W;vi.base_height=vi.output_height=H;
    vi.output_format=VIDEO_FORMAT_BGRA;vi.scale_type=OBS_SCALE_DISABLE;
    vi.range=VIDEO_RANGE_FULL;vi.colorspace=VIDEO_CS_SRGB;
    if(obs_reset_video(&vi)!=OBS_VIDEO_SUCCESS)return 4;
    obs_source_info info{};
    info.id="framerot_showcase_input";info.type=OBS_SOURCE_TYPE_INPUT;
    info.output_flags=OBS_SOURCE_VIDEO|OBS_SOURCE_SRGB;
    info.get_name=name;info.create=create;info.destroy=destroy;info.get_width=width;info.get_height=height;
    info.video_tick=tick;info.video_render=render;
    obs_register_source(&info);
    obs_module_t *module=nullptr;
    if(obs_open_module(&module,argv[1],argv[2])!=MODULE_SUCCESS||!obs_init_module(module))return 5;
    auto *source=obs_source_create(info.id,"Showcase",nullptr,nullptr);
    if(!source)return 6;
    obs_source_t *filter=nullptr;
    if(preset!="clean"){
        filter=obs_source_create("framerot_filter","framerot showcase",nullptr,nullptr);
        if(!filter)return 6;
        auto *settings=obs_source_get_settings(filter);
        auto *props=obs_source_properties(filter);
        auto *recipe=obs_properties_get(props,"instrument_recipe");
        if(preset!="instrument"){
            int r=preset=="memory-melt"?1:preset=="motion-smear"?2:preset=="time-ribbons"?3:0;
            if(r){
                if(!recipe)return 7;
                obs_data_set_int(settings,"instrument_recipe",r);
                if(!obs_property_modified(recipe,settings))return 7;
            }else if(preset.rfind("mode-",0)==0){
                int mode=std::stoi(preset.substr(5));
                if(mode<0||mode>9)return 7;
                obs_data_set_int(settings,"mode",mode);
                obs_property_modified(obs_properties_get(props,"mode"),settings);
            }else return 7;
        }
        obs_source_update(filter,settings);
        std::cout<<"SETTINGS "<<obs_data_get_json(settings)<<"\n";
        obs_properties_destroy(props);obs_data_release(settings);
        obs_source_filter_add(source,filter);
    }
    obs_set_output_source(0,source);
    Capture cap;cap.target=frames*loops;cap.warmup=fps;cap.bytes.resize(cap.target*frame_size);
    video_scale_info conversion{};
    conversion.format=VIDEO_FORMAT_BGRA;conversion.width=W;conversion.height=H;
    conversion.range=VIDEO_RANGE_FULL;conversion.colorspace=VIDEO_CS_SRGB;
    obs_add_raw_video_callback(&conversion,receive,&cap);
    {
        std::unique_lock<std::mutex> lock(cap.lock);
        if(!cap.done.wait_for(lock,std::chrono::seconds(10+cap.target/fps),[&]{return cap.count==cap.target;})){
            std::cerr<<"capture timed out "<<cap.count<<" of "<<cap.target<<"\n";
        }
    }
    obs_remove_raw_video_callback(receive,&cap);
    obs_set_output_source(0,nullptr);
    if(filter){obs_source_filter_remove(source,filter);obs_source_release(filter);}
    obs_source_release(source);obs_shutdown();
    if(cap.count!=cap.target)return 8;
    std::ofstream out(argv[9],std::ios::binary);
    out.write(reinterpret_cast<const char*>(cap.bytes.data()),cap.bytes.size());
    if(!out)return 9;
    std::cout<<"CAPTURED "<<preset<<" frames="<<cap.count<<" size="<<W<<"x"<<H<<" fps="<<fps<<"\n";
}
