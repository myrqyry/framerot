/* GPL-2.0; see COPYING. */
#include "codec-worker.h"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

struct codec_worker {
    std::thread thread;
    std::mutex lock;
    std::mutex controls_lock; /* independent of frame queue/result ownership */
    std::condition_variable wake;
    retro_video_options options,controls;
    std::vector<uint8_t> pending,active,result,scratch;
    size_t row;
    bool stop=false,queued=false,ready=false;
    std::atomic<uint64_t> dropped{0};
    std::atomic<bool> fault{false},hold{false};
    struct codec_worker_stats stats{};
    explicit codec_worker(const retro_video_options &o):
        options(o),controls(o),
        pending(size_t(o.width)*o.height*4),active(pending.size()),
        result(pending.size()),scratch(pending.size()),row(size_t(o.width)*4){}
};

static uint64_t now_ns()
{
    return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

static void worker_main(codec_worker *w)
{
    struct retro_video *codec = NULL;
    uint64_t retry_after = 0;
    bool recovering = false;
    std::unique_lock<std::mutex> guard(w->lock);
    while (!w->stop) {
        w->wake.wait(guard,[&]{return w->queued||w->stop;});
        if (w->stop) break;
        w->active.swap(w->pending);
        w->queued=false;
        guard.unlock();
        struct retro_video_options controls;
        {
            std::lock_guard<std::mutex> controls_guard(w->controls_lock);
            controls=w->controls;
        }
        uint64_t start=now_ns();
        int rc=0;
        bool attempted=false, restarted=false;
        uint64_t ep=0,ev=0,ec=0,eb=0,eph=0,eu=0,frequency[3]={0};
        if (codec || start >= retry_after) {
            attempted=true;
            if (!codec) {
                codec=retro_video_create(&w->options);
                restarted=codec && recovering;
            }
            if (codec) {
                retro_video_set_anchor_hold(codec,w->hold.load());
                retro_video_set_live_options(codec,&controls);
                ep=retro_video_compressed_packets(codec);ev=retro_video_compressed_vectors(codec);
                ec=retro_video_compressed_coefficients(codec);eb=retro_video_compressed_blocks(codec);
                eph=retro_video_compressed_phantom_blocks(codec);
                for(int band=0;band<3;band++)
                    frequency[band]=retro_video_compressed_frequency_coefficients(codec,band);
                eu=retro_video_compressed_unsupported(codec);
            }
            rc=codec ? retro_video_process(codec,w->active.data(),(int)w->row,w->scratch.data(),(int)w->row) : -1;
            if(codec) {
                ep=retro_video_compressed_packets(codec)-ep;ev=retro_video_compressed_vectors(codec)-ev;
                ec=retro_video_compressed_coefficients(codec)-ec;eb=retro_video_compressed_blocks(codec)-eb;
                eph=retro_video_compressed_phantom_blocks(codec)-eph;
                for(int band=0;band<3;band++)
                    frequency[band]=retro_video_compressed_frequency_coefficients(codec,band)-frequency[band];
                eu=retro_video_compressed_unsupported(codec)-eu;
            }
            if (rc<0) {
                retro_video_destroy(codec); codec=NULL;
                retry_after=now_ns()+250000000ull;
                recovering=true;
            }
        }
        uint64_t duration=now_ns()-start;
        guard.lock();
        if (!attempted) {
            ++w->dropped;
            continue;
        }
        w->stats.edited_packets+=ep;w->stats.edited_vectors+=ev;
        w->stats.edited_coefficients+=ec;w->stats.edited_blocks+=eb;
        w->stats.phantom_blocks+=eph;w->stats.edit_unsupported+=eu;
        for(int band=0;band<3;band++)
            w->stats.frequency_coefficients[band]+=frequency[band];
        w->stats.total_codec_ns+=duration;
        if (duration>w->stats.max_codec_ns) w->stats.max_codec_ns=duration;
        if (restarted && rc>=0) {w->stats.recovered++; recovering=false;}
        if (rc<0) {w->stats.errors++; w->ready=false; w->fault.store(true);}
        else {
            w->stats.completed++;
            if (rc==1) w->fault.store(false);
            if (rc==1) {
                w->result.swap(w->scratch);
                w->ready=true;
            }
        }
    }
    guard.unlock();
    retro_video_destroy(codec);
}

struct codec_worker *codec_worker_create(const retro_video_options *o)
{
    if(!o||!retro_video_supported_size(o->codec,o->width,o->height))return nullptr;
    try{
        auto w=std::make_unique<codec_worker>(*o);
        w->thread=std::thread(worker_main,w.get());
        return w.release();
    }catch(...){return nullptr;}
}
void codec_worker_destroy(codec_worker *w)
{
    if(!w)return;
    {std::lock_guard<std::mutex> guard(w->lock);w->stop=true;}
    w->wake.notify_one();w->thread.join();delete w;
}
bool codec_worker_submit(codec_worker *w,const uint8_t *pixels,int stride)
{
    if(!w||!pixels||stride<int(w->row))return false;
    std::unique_lock<std::mutex> guard(w->lock,std::try_to_lock);
    if(!guard.owns_lock()){++w->dropped;return false;}
    if(w->queued)++w->dropped;
    for(int y=0;y<w->options.height;y++)
        std::memcpy(w->pending.data()+size_t(y)*w->row,pixels+size_t(y)*stride,w->row);
    w->queued=true;++w->stats.submitted;w->stats.max_pending=1;
    guard.unlock();w->wake.notify_one();return true;
}
bool codec_worker_set_controls(codec_worker *w,const retro_video_options *o)
{
    if(!w||!o)return false;
    std::lock_guard<std::mutex> guard(w->controls_lock);
    w->controls=*o;
    return true;
}
void codec_worker_set_anchor_hold(codec_worker *w,bool hold){if(w)w->hold.store(hold);}
bool codec_worker_faulted(codec_worker *w){return !w||w->fault.load();}
bool codec_worker_take(codec_worker *w,uint8_t *pixels)
{
    if(!w||!pixels)return false;
    std::unique_lock<std::mutex> guard(w->lock,std::try_to_lock);
    if(!guard.owns_lock()||!w->ready)return false;
    std::memcpy(pixels,w->result.data(),w->result.size());w->ready=false;return true;
}
struct codec_worker_stats codec_worker_stats(codec_worker *w)
{
    if(!w)return {};
    std::lock_guard<std::mutex> guard(w->lock);
    auto s=w->stats;s.dropped=w->dropped.load();return s;
}
