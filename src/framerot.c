/*
 * framerot: experimental OBS MPEG-4 datamosh filter, developed in 2026.
 * GNU GPL version 2. See COPYING and README.md for upstream attribution.
 */
#include <obs-module.h>
#include <graphics/graphics.h>
#include <stdint.h>
#include <stdbool.h>
#include "portable-sync.h"
#include <util/platform.h>
#include "video-obs.h"
#include "video-roundtrip.h"

OBS_DECLARE_MODULE()

struct mosh_filter {
    obs_source_t *source; /* OBS owns this borrowed handle */
    gs_texrender_t *capture;
    struct retro_video_obs *codec;
    struct retro_video_options options;
    struct framerot_mutex *config_lock;
    int wet_mix;
    bool preserve_alpha;
    gs_effect_t *mix_effect;
    float elapsed;
    bool due, hold, reset;
    obs_hotkey_id hold_key, reset_key;
    uint64_t retry_at, render_count, render_ns, render_max_ns;
};

static const char *mosh_name(void *unused)
{
    UNUSED_PARAMETER(unused);
    return "framerot";
}

/* Defaults are per effect, not an arbitrary compromise across modes.
 * Set DEFAULT values only: OBS explicitly saved/user-entered values retain
 * priority even when a mode is changed or a scene is reloaded. */
struct mosh_showcase {
    int fps, kbps, gop, skip_keyframes, p_interval, p_burst;
    int vector_strength, vector_persistence, rounding_interval, anchor_span;
    int rewind_depth, slice_history, slice_height;
};

static const struct mosh_showcase showcase[] = {
    /* Classic: original crunchy, rapid I-frame recovery. */
    {12, 96, 6, 1, 5, 3, 250, 65, 1, 4, 4, 4, 12},
    /* Tear: regular P packet loss. */
    {15, 160, 12, 1, 5, 3, 250, 65, 1, 4, 4, 4, 12},
    /* Burst: longer predictive-frame interruptions. */
    {15, 160, 18, 1, 12, 5, 250, 65, 1, 4, 4, 4, 12},
    /* Echo: earlier compressed motion repeats frequently. */
    {15, 160, 18, 1, 3, 3, 250, 65, 1, 4, 4, 4, 12},
    /* Flow Drift: exported motion vectors and feedback. */
    {15, 230, 24, 1, 5, 3, 250, 65, 1, 4, 4, 4, 12},
    /* Motion Shimmer: modify every P-VOP interpolation rounding bit. */
    {15, 200, 24, 1, 5, 3, 250, 65, 1, 4, 4, 4, 12},
    /* Keyframe Anchor: held I-frame plus live P-frame continuation. */
    {15, 160, 12, 1, 5, 3, 250, 65, 1, 4, 4, 4, 12},
    /* Rewind: real P packets from four frames earlier. */
    {15, 220, 24, 1, 5, 3, 250, 65, 1, 4, 4, 4, 12},
    /* Peel: bands from four different decoded moments. */
    {15, 200, 18, 1, 5, 3, 250, 65, 1, 4, 4, 4, 12},
    /* Instrument: expressive motion, bounded reference and feedback. */
    {18, 320, 18, 1, 5, 3, 150, 40, 1, 3, 4, 4, 12},
};

static void mosh_showcase_defaults(obs_data_t *settings, int mode)
{
    if (mode < RETRO_MOSH_KEYFRAME_SKIP || mode > RETRO_MOSH_INSTRUMENT)
        mode = RETRO_MOSH_INSTRUMENT;
    const struct mosh_showcase *p = &showcase[mode];
    obs_data_set_default_int(settings, "fps", p->fps);
    obs_data_set_default_int(settings, "kbps", p->kbps);
    obs_data_set_default_int(settings, "gop", p->gop);
    obs_data_set_default_int(settings, "skip_keyframes", p->skip_keyframes);
    obs_data_set_default_int(settings, "p_interval", p->p_interval);
    obs_data_set_default_int(settings, "p_burst", p->p_burst);
    obs_data_set_default_int(settings, "vector_strength", p->vector_strength);
    obs_data_set_default_int(settings, "vector_persistence", p->vector_persistence);
    obs_data_set_default_int(settings, "rounding_interval", p->rounding_interval);
    obs_data_set_default_int(settings, "anchor_span", p->anchor_span);
    obs_data_set_default_int(settings, "rewind_depth", p->rewind_depth);
    obs_data_set_default_int(settings, "slice_history", p->slice_history);
    obs_data_set_default_int(settings, "slice_height", p->slice_height);
    const bool instrument=mode==RETRO_MOSH_INSTRUMENT;
    obs_data_set_default_int(settings,"codec_height",instrument?192:144);
    obs_data_set_default_int(settings,"reference",instrument?RETRO_REFERENCE_ANCHOR:RETRO_REFERENCE_LIVE);
    obs_data_set_default_int(settings,"predictive",RETRO_PREDICTIVE_LIVE);
    obs_data_set_default_bool(settings,"layer_compressed",instrument);
    obs_data_set_default_bool(settings,"layer_flow",instrument);
    obs_data_set_default_bool(settings,"layer_shimmer",false);
    obs_data_set_default_bool(settings,"layer_peel",false);
    obs_data_set_default_int(settings,"mv_x",instrument?160:180);
    obs_data_set_default_int(settings,"mv_y",100);
    obs_data_set_default_int(settings,"residual_scale",instrument?70:65);
    obs_data_set_default_int(settings,"residual_loss",instrument?10:0);
    obs_data_set_default_int(settings,"phantom_drift",0);
    obs_data_set_default_int(settings,"frequency_low",0);
    obs_data_set_default_int(settings,"frequency_mid",0);
    obs_data_set_default_int(settings,"frequency_high",0);

}

static void mosh_defaults(obs_data_t *settings)
{
    obs_data_set_default_int(settings, "mode", RETRO_MOSH_INSTRUMENT);
    obs_data_set_default_int(settings,"wet_mix",100);
    obs_data_set_default_bool(settings,"preserve_alpha",true);
    obs_data_set_default_int(settings,"instrument_recipe",0);
    mosh_showcase_defaults(settings, (int)obs_data_get_int(settings,"mode"));
}

static void mosh_update(void *data, obs_data_t *settings)
{
    struct mosh_filter *s = data;
    /* Handles saved modes when OBS restores an existing scene. */
    int64_t selected=obs_data_get_int(settings,"mode");
    if(selected<RETRO_MOSH_KEYFRAME_SKIP||selected>RETRO_MOSH_INSTRUMENT)
        obs_data_set_int(settings,"mode",RETRO_MOSH_INSTRUMENT);
    mosh_showcase_defaults(settings, (int)obs_data_get_int(settings, "mode"));
    struct retro_video_options next = retro_video_default_options(RETRO_VIDEO_MPEG4_PART2);
    next.width = 176;
    int64_t height = obs_data_get_int(settings,"codec_height");
    if(height<64) height=64;
    if(height>576) height=576;
    next.height = (int)((height+8)/16)*16;
    int mix = (int)obs_data_get_int(settings,"wet_mix");
    if(mix<0) mix=0;
    if(mix>100) mix=100;
    next.mosh_reference=(enum retro_reference)obs_data_get_int(settings,"reference");
    if(next.mosh_reference<RETRO_REFERENCE_LIVE || next.mosh_reference>RETRO_REFERENCE_ANCHOR)
        next.mosh_reference=RETRO_REFERENCE_LIVE;
    next.mosh_predictive=(enum retro_predictive)obs_data_get_int(settings,"predictive");
    if(next.mosh_predictive<RETRO_PREDICTIVE_LIVE || next.mosh_predictive>RETRO_PREDICTIVE_REWIND)
        next.mosh_predictive=RETRO_PREDICTIVE_LIVE;
    next.mosh_layers=(obs_data_get_bool(settings,"layer_flow")?RETRO_LAYER_FLOW:0) |
        (obs_data_get_bool(settings,"layer_peel")?RETRO_LAYER_PEEL:0) |
        (obs_data_get_bool(settings,"layer_shimmer")?RETRO_LAYER_SHIMMER:0) |
        (obs_data_get_bool(settings,"layer_compressed")?RETRO_LAYER_COMPRESSED:0);
    next.mosh_mv_x=(int)obs_data_get_int(settings,"mv_x");
    next.mosh_mv_y=(int)obs_data_get_int(settings,"mv_y");
    next.mosh_residual=(int)obs_data_get_int(settings,"residual_scale");
    next.mosh_residual_loss=(int)obs_data_get_int(settings,"residual_loss");
    next.mosh_phantom=(int)obs_data_get_int(settings,"phantom_drift");
    const char *freq_keys[]={"frequency_low","frequency_mid","frequency_high"};
    for(int band=0;band<3;band++){
        int v=(int)obs_data_get_int(settings,freq_keys[band]);
        next.mosh_frequency[band]=v< -100?-100:v>200?200:v;
    }
    if(next.mosh_mv_x< -500)next.mosh_mv_x=-500;
    if(next.mosh_mv_x>500)next.mosh_mv_x=500;
    if(next.mosh_mv_y< -500)next.mosh_mv_y=-500;
    if(next.mosh_mv_y>500)next.mosh_mv_y=500;
    if(next.mosh_residual< -200)next.mosh_residual=-200;
    if(next.mosh_residual>200)next.mosh_residual=200;
    if(next.mosh_residual_loss<0)next.mosh_residual_loss=0;
    if(next.mosh_residual_loss>100)next.mosh_residual_loss=100;
    if(next.mosh_phantom<0)next.mosh_phantom=0;
    if(next.mosh_phantom>100)next.mosh_phantom=100;
    next.fps = (int)obs_data_get_int(settings, "fps");
    next.bitrate = (int)obs_data_get_int(settings, "kbps") * 1000;
    next.gop_size = (int)obs_data_get_int(settings, "gop");
    next.mosh_skip_keyframes = (int)obs_data_get_int(settings, "skip_keyframes");
    next.mosh_mode = (enum retro_mosh_mode)obs_data_get_int(settings, "mode");
    next.mosh_p_interval = (int)obs_data_get_int(settings, "p_interval");
    next.mosh_p_burst = (int)obs_data_get_int(settings, "p_burst");
    next.mosh_vector_strength = (int)obs_data_get_int(settings, "vector_strength");
    next.mosh_vector_persistence = (int)obs_data_get_int(settings, "vector_persistence");
    next.mosh_rounding_interval = (int)obs_data_get_int(settings, "rounding_interval");
    next.mosh_anchor_span = (int)obs_data_get_int(settings, "anchor_span");
    next.mosh_rewind_depth = (int)obs_data_get_int(settings, "rewind_depth");
    next.mosh_slice_history = (int)obs_data_get_int(settings, "slice_history");
    next.mosh_slice_height = (int)obs_data_get_int(settings, "slice_height");
    if (next.fps < 1) next.fps = 1;
    if (next.fps > 30) next.fps = 30;
    if (next.bitrate < 8000) next.bitrate = 8000;
    if (next.bitrate > 2000000) next.bitrate = 2000000;
    if (next.gop_size < 2) next.gop_size = 2;
    if (next.gop_size > 120) next.gop_size = 120;
    if (next.mosh_skip_keyframes < 0) next.mosh_skip_keyframes = 0;
    if (next.mosh_skip_keyframes > 8) next.mosh_skip_keyframes = 8;
    if (next.mosh_mode < RETRO_MOSH_KEYFRAME_SKIP || next.mosh_mode > RETRO_MOSH_INSTRUMENT)
        next.mosh_mode = RETRO_MOSH_KEYFRAME_SKIP;
    if (next.mosh_p_interval < 2) next.mosh_p_interval = 2;
    if (next.mosh_p_interval > 60) next.mosh_p_interval = 60;
    if (next.mosh_p_burst < 1) next.mosh_p_burst = 1;
    if (next.mosh_p_burst >= next.mosh_p_interval)
        next.mosh_p_burst = next.mosh_p_interval - 1;
    if (next.mosh_vector_strength < 0) next.mosh_vector_strength = 0;
    if (next.mosh_vector_strength > 500) next.mosh_vector_strength = 500;
    if (next.mosh_vector_persistence < 0) next.mosh_vector_persistence = 0;
    if (next.mosh_vector_persistence > 100) next.mosh_vector_persistence = 100;
    if (next.mosh_rounding_interval < 1) next.mosh_rounding_interval = 1;
    if (next.mosh_rounding_interval > 60) next.mosh_rounding_interval = 60;
    if (next.mosh_anchor_span < 2) next.mosh_anchor_span = 2;
    if (next.mosh_anchor_span > 12) next.mosh_anchor_span = 12;
    if (next.mosh_rewind_depth < 2) next.mosh_rewind_depth = 2;
    if (next.mosh_rewind_depth > 12) next.mosh_rewind_depth = 12;
    if (next.mosh_slice_history < 2) next.mosh_slice_history = 2;
    if (next.mosh_slice_history > 8) next.mosh_slice_history = 8;
    if (next.mosh_slice_height < 1) next.mosh_slice_height = 1;
    if (next.mosh_slice_height > 64) next.mosh_slice_height = 64;
    framerot_mutex_lock(s->config_lock);
    s->options = next;
    s->wet_mix=mix;
    s->preserve_alpha=obs_data_get_bool(settings,"preserve_alpha");
    s->due=true;
    framerot_mutex_unlock(s->config_lock);
}

static void mosh_property_visibility(obs_properties_t *props, obs_data_t *settings)
{
    const int mode=(int)obs_data_get_int(settings,"mode");
    const bool instrument=mode==RETRO_MOSH_INSTRUMENT;
    const int reference=(int)obs_data_get_int(settings,"reference");
    const int predictive=(int)obs_data_get_int(settings,"predictive");
    const bool compressed=instrument && obs_data_get_bool(settings,"layer_compressed");
    const bool flow=mode==RETRO_MOSH_VECTOR_DRIFT || (instrument && obs_data_get_bool(settings,"layer_flow"));
    const bool peel=mode==RETRO_MOSH_FRAME_PEEL || (instrument && obs_data_get_bool(settings,"layer_peel"));
    const bool shimmer=mode==RETRO_MOSH_MOTION_SHIMMER || (instrument && obs_data_get_bool(settings,"layer_shimmer"));
    const char *keys[]={"instrument_recipe","reference","predictive","layer_flow","layer_peel","layer_shimmer","layer_compressed"};
    for(size_t i=0;i<sizeof keys/sizeof *keys;i++) obs_property_set_visible(obs_properties_get(props,keys[i]),instrument);
    const char *compressed_keys[]={"mv_x","mv_y","residual_scale","residual_loss","phantom_drift",
                                   "frequency_low","frequency_mid","frequency_high"};
    for(size_t i=0;i<sizeof compressed_keys/sizeof *compressed_keys;i++) obs_property_set_visible(obs_properties_get(props,compressed_keys[i]),compressed);
    obs_property_set_visible(obs_properties_get(props,"skip_keyframes"),mode==RETRO_MOSH_KEYFRAME_SKIP || (instrument && reference==RETRO_REFERENCE_SKIP));
    obs_property_set_visible(obs_properties_get(props,"p_interval"),(mode>=RETRO_MOSH_PFRAME_DROP && mode<=RETRO_MOSH_PFRAME_ECHO) ||
        (instrument && predictive>=RETRO_PREDICTIVE_DROP && predictive<=RETRO_PREDICTIVE_ECHO));
    obs_property_set_visible(obs_properties_get(props,"p_burst"),mode==RETRO_MOSH_PFRAME_BURST || (instrument && predictive==RETRO_PREDICTIVE_BURST));
    obs_property_set_visible(obs_properties_get(props,"vector_strength"),flow);
    obs_property_set_visible(obs_properties_get(props,"vector_persistence"),flow);
    obs_property_set_visible(obs_properties_get(props,"rounding_interval"),shimmer);
    obs_property_set_visible(obs_properties_get(props,"anchor_span"),mode==RETRO_MOSH_KEYFRAME_ANCHOR || (instrument && reference==RETRO_REFERENCE_ANCHOR));
    obs_property_set_visible(obs_properties_get(props,"rewind_depth"),mode==RETRO_MOSH_PFRAME_REWIND || (instrument && predictive==RETRO_PREDICTIVE_REWIND));
    obs_property_set_visible(obs_properties_get(props,"slice_history"),peel);
    obs_property_set_visible(obs_properties_get(props,"slice_height"),peel);
}

static bool mosh_controls_changed(obs_properties_t *props, obs_property_t *property, obs_data_t *settings)
{
    UNUSED_PARAMETER(property);
    mosh_property_visibility(props,settings);
    return true;
}

static bool mosh_mode_changed(obs_properties_t *props, obs_property_t *property, obs_data_t *settings)
{
    mosh_showcase_defaults(settings,(int)obs_data_get_int(settings,"mode"));
    return mosh_controls_changed(props,property,settings);
}

/* Explicit, one-shot starting points. Do not store a selected preset that
 * could overwrite custom edits on the next properties refresh. */
static bool mosh_recipe_changed(obs_properties_t *props,obs_property_t *property,obs_data_t *settings)
{
    const int recipe=(int)obs_data_get_int(settings,"instrument_recipe");
    if(recipe==0) return false;
    obs_data_set_int(settings,"instrument_recipe",0);
    if(obs_data_get_int(settings,"mode")!=RETRO_MOSH_INSTRUMENT || recipe<1 || recipe>7) return true;
    if(recipe==7) {
        /* Counterpoint to Spectral Melt: intentionally exaggerated inter
         * residuals create sharp, unstable ringing as reference frames age.
         * This is a recipe of existing codec controls, not a new decoder. */
        obs_data_set_int(settings,"reference",RETRO_REFERENCE_LIVE);
        obs_data_set_int(settings,"predictive",RETRO_PREDICTIVE_LIVE);
        obs_data_set_bool(settings,"layer_compressed",true);
        obs_data_set_bool(settings,"layer_flow",false);
        obs_data_set_bool(settings,"layer_shimmer",false);
        obs_data_set_bool(settings,"layer_peel",false);
        obs_data_set_int(settings,"codec_height",192);
        obs_data_set_int(settings,"fps",18);
        obs_data_set_int(settings,"kbps",340);
        obs_data_set_int(settings,"gop",42);
        obs_data_set_int(settings,"mv_x",100);
        obs_data_set_int(settings,"mv_y",100);
        obs_data_set_int(settings,"residual_scale",135);
        obs_data_set_int(settings,"residual_loss",0);
        obs_data_set_int(settings,"phantom_drift",0);
        obs_data_set_int(settings,"frequency_low",-45);
        obs_data_set_int(settings,"frequency_mid",120);
        obs_data_set_int(settings,"frequency_high",200);
        return mosh_controls_changed(props,property,settings);
    }
    if(recipe==6) {
        /* Remove temporal detail selectively, leaving low scan positions
         * untouched; user can raise bands again in Packet editing. */
        obs_data_set_int(settings,"reference",RETRO_REFERENCE_LIVE);
        obs_data_set_int(settings,"predictive",RETRO_PREDICTIVE_LIVE);
        obs_data_set_bool(settings,"layer_compressed",true);
        obs_data_set_bool(settings,"layer_flow",false);
        obs_data_set_bool(settings,"layer_shimmer",false);
        obs_data_set_bool(settings,"layer_peel",false);
        obs_data_set_int(settings,"codec_height",192);
        obs_data_set_int(settings,"fps",18);
        obs_data_set_int(settings,"kbps",360);
        obs_data_set_int(settings,"gop",30);
        obs_data_set_int(settings,"mv_x",100);
        obs_data_set_int(settings,"mv_y",100);
        obs_data_set_int(settings,"residual_scale",100);
        obs_data_set_int(settings,"residual_loss",0);
        obs_data_set_int(settings,"phantom_drift",0);
        obs_data_set_int(settings,"frequency_low",0);
        obs_data_set_int(settings,"frequency_mid",-50);
        obs_data_set_int(settings,"frequency_high",-100);
        return mosh_controls_changed(props,property,settings);
    }
    if(recipe==5) {
        /* A real codec effect for stills and nearly static video. */
        obs_data_set_int(settings,"reference",RETRO_REFERENCE_LIVE);
        obs_data_set_int(settings,"predictive",RETRO_PREDICTIVE_LIVE);
        obs_data_set_bool(settings,"layer_compressed",true);
        obs_data_set_bool(settings,"layer_flow",false);
        obs_data_set_bool(settings,"layer_shimmer",false);
        obs_data_set_bool(settings,"layer_peel",false);
        obs_data_set_int(settings,"codec_height",192);
        obs_data_set_int(settings,"fps",18);
        obs_data_set_int(settings,"kbps",300);
        obs_data_set_int(settings,"gop",60);
        obs_data_set_int(settings,"mv_x",100);
        obs_data_set_int(settings,"mv_y",100);
        obs_data_set_int(settings,"residual_scale",100);
        obs_data_set_int(settings,"residual_loss",0);
        obs_data_set_int(settings,"phantom_drift",45);
        obs_data_set_int(settings,"frequency_low",0);
        obs_data_set_int(settings,"frequency_mid",0);
        obs_data_set_int(settings,"frequency_high",0);
        return mosh_controls_changed(props,property,settings);
    }
    if(recipe==4) {
        /* Monotonic time seeds a fresh editable roll only on explicit choice. */
        struct retro_video_options o=retro_video_random_instrument(os_gettime_ns());
        obs_data_set_int(settings,"reference",o.mosh_reference);
        obs_data_set_int(settings,"predictive",o.mosh_predictive);
        obs_data_set_bool(settings,"layer_compressed",o.mosh_layers&RETRO_LAYER_COMPRESSED);
        obs_data_set_bool(settings,"layer_flow",o.mosh_layers&RETRO_LAYER_FLOW);
        obs_data_set_bool(settings,"layer_shimmer",o.mosh_layers&RETRO_LAYER_SHIMMER);
        obs_data_set_bool(settings,"layer_peel",o.mosh_layers&RETRO_LAYER_PEEL);
        obs_data_set_int(settings,"codec_height",o.height);
        obs_data_set_int(settings,"fps",o.fps);
        obs_data_set_int(settings,"kbps",o.bitrate/1000);
        obs_data_set_int(settings,"gop",o.gop_size);
        obs_data_set_int(settings,"skip_keyframes",o.mosh_skip_keyframes);
        obs_data_set_int(settings,"anchor_span",o.mosh_anchor_span);
        obs_data_set_int(settings,"p_interval",o.mosh_p_interval);
        obs_data_set_int(settings,"p_burst",o.mosh_p_burst);
        obs_data_set_int(settings,"rewind_depth",o.mosh_rewind_depth);
        obs_data_set_int(settings,"rounding_interval",o.mosh_rounding_interval);
        obs_data_set_int(settings,"mv_x",o.mosh_mv_x);
        obs_data_set_int(settings,"mv_y",o.mosh_mv_y);
        obs_data_set_int(settings,"residual_scale",o.mosh_residual);
        obs_data_set_int(settings,"residual_loss",o.mosh_residual_loss);
        obs_data_set_int(settings,"phantom_drift",o.mosh_phantom);
        obs_data_set_int(settings,"frequency_low",o.mosh_frequency[0]);
        obs_data_set_int(settings,"frequency_mid",o.mosh_frequency[1]);
        obs_data_set_int(settings,"frequency_high",o.mosh_frequency[2]);
        obs_data_set_int(settings,"vector_strength",o.mosh_vector_strength);
        obs_data_set_int(settings,"vector_persistence",o.mosh_vector_persistence);
        obs_data_set_int(settings,"slice_history",o.mosh_slice_history);
        obs_data_set_int(settings,"slice_height",o.mosh_slice_height);
        return mosh_controls_changed(props,property,settings);
    }
    const bool melt=recipe==1, smear=recipe==2, peel=recipe==3;
    obs_data_set_int(settings,"reference",melt?RETRO_REFERENCE_ANCHOR:RETRO_REFERENCE_LIVE);
    obs_data_set_int(settings,"predictive",peel?RETRO_PREDICTIVE_ECHO:RETRO_PREDICTIVE_LIVE);
    obs_data_set_bool(settings,"layer_compressed",melt||smear);
    obs_data_set_bool(settings,"layer_flow",smear);
    obs_data_set_bool(settings,"layer_shimmer",false);
    obs_data_set_bool(settings,"layer_peel",peel);
    obs_data_set_int(settings,"codec_height",melt?288:peel?144:192);
    obs_data_set_int(settings,"fps",melt?30:15);
    obs_data_set_int(settings,"kbps",melt?650:peel?180:250);
    obs_data_set_int(settings,"gop",melt?18:24);
    obs_data_set_int(settings,"skip_keyframes",1);
    obs_data_set_int(settings,"anchor_span",4);
    obs_data_set_int(settings,"p_interval",3);
    obs_data_set_int(settings,"p_burst",3);
    obs_data_set_int(settings,"rewind_depth",4);
    obs_data_set_int(settings,"rounding_interval",1);
    obs_data_set_int(settings,"mv_x",melt?180:smear?250:100);
    obs_data_set_int(settings,"mv_y",100);
    obs_data_set_int(settings,"residual_scale",melt?65:smear?40:100);
    obs_data_set_int(settings,"residual_loss",melt?15:smear?25:0);
    obs_data_set_int(settings,"phantom_drift",0);
    obs_data_set_int(settings,"frequency_low",0);
    obs_data_set_int(settings,"frequency_mid",0);
    obs_data_set_int(settings,"frequency_high",0);
    obs_data_set_int(settings,"vector_strength",250);
    obs_data_set_int(settings,"vector_persistence",65);
    obs_data_set_int(settings,"slice_history",6);
    obs_data_set_int(settings,"slice_height",12);
    return mosh_controls_changed(props,property,settings);
}

static bool hold_clicked(obs_properties_t *p,obs_property_t *b,void *data)
{
    UNUSED_PARAMETER(p); UNUSED_PARAMETER(b);
    struct mosh_filter *s=data;
    if(s) {framerot_mutex_lock(s->config_lock);s->hold=true;framerot_mutex_unlock(s->config_lock);}
    return false;
}
static bool release_clicked(obs_properties_t *p,obs_property_t *b,void *data)
{
    UNUSED_PARAMETER(p); UNUSED_PARAMETER(b);
    struct mosh_filter *s=data;
    if(s) {framerot_mutex_lock(s->config_lock);s->hold=false;framerot_mutex_unlock(s->config_lock);}
    return false;
}
static bool reset_clicked(obs_properties_t *p,obs_property_t *b,void *data)
{
    UNUSED_PARAMETER(p); UNUSED_PARAMETER(b);
    struct mosh_filter *s=data;
    if(s) {framerot_mutex_lock(s->config_lock);s->hold=false;s->reset=true;framerot_mutex_unlock(s->config_lock);}
    return false;
}
static void hold_hotkey(void *data,obs_hotkey_id id,obs_hotkey_t *key,bool pressed)
{
    UNUSED_PARAMETER(id); UNUSED_PARAMETER(key);
    struct mosh_filter *s=data;
    if(s) {framerot_mutex_lock(s->config_lock);s->hold=pressed;framerot_mutex_unlock(s->config_lock);}
}
static void reset_hotkey(void *data,obs_hotkey_id id,obs_hotkey_t *key,bool pressed)
{
    UNUSED_PARAMETER(id); UNUSED_PARAMETER(key);
    if(pressed) reset_clicked(NULL,NULL,data);
}

static obs_properties_t *mosh_properties(void *data)
{
    struct mosh_filter *s = data;
    obs_properties_t *props = obs_properties_create();
    obs_property_t *mode = obs_properties_add_list(props, "mode", "Effect",
        OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
    obs_property_list_add_int(mode,"Instrument — compose operations",RETRO_MOSH_INSTRUMENT);
    obs_property_list_add_int(mode, "Classic — keyframe loss", RETRO_MOSH_KEYFRAME_SKIP);
    obs_property_list_add_int(mode, "Tear — periodic P-frame loss", RETRO_MOSH_PFRAME_DROP);
    obs_property_list_add_int(mode, "Burst — packets lost in runs", RETRO_MOSH_PFRAME_BURST);
    obs_property_list_add_int(mode, "Echo — replay compressed motion", RETRO_MOSH_PFRAME_ECHO);
    obs_property_list_add_int(mode, "Flow Drift — MPEG-4 motion vectors", RETRO_MOSH_VECTOR_DRIFT);
    obs_property_list_add_int(mode, "Motion Shimmer — P-frame rounding bit", RETRO_MOSH_MOTION_SHIMMER);
    obs_property_list_add_int(mode, "Keyframe Anchor — reuse earlier I-frame", RETRO_MOSH_KEYFRAME_ANCHOR);
    obs_property_list_add_int(mode, "P-Frame Rewind — older compressed motion", RETRO_MOSH_PFRAME_REWIND);
    obs_property_list_add_int(mode, "Frame Peel — temporal bands", RETRO_MOSH_FRAME_PEEL);
    obs_property_set_modified_callback(mode, mosh_mode_changed);
    obs_property_t *recipe=obs_properties_add_list(props,"instrument_recipe","Load starting point",OBS_COMBO_TYPE_LIST,OBS_COMBO_FORMAT_INT);
    obs_property_list_add_int(recipe,"Custom / choose a starting point",0);
    obs_property_list_add_int(recipe,"Memory Melt — lingering reference",1);
    obs_property_list_add_int(recipe,"Motion Smear — exaggerated movement",2);
    obs_property_list_add_int(recipe,"Time Ribbons — temporal strips",3);
    obs_property_list_add_int(recipe,"Phantom Drift — animate stills",5);
    obs_property_list_add_int(recipe,"Spectral Melt — selective residual loss",6);
    obs_property_list_add_int(recipe,"Spectral Fracture — boosted detail distortion",7);
    obs_property_list_add_int(recipe,"Randomize (experimental)",4);
    obs_property_set_long_description(recipe,"Selecting a starting point replaces effect and codec settings, but preserves wet mix and live alpha. Edit any setting afterward.");
    obs_property_set_modified_callback(recipe,mosh_recipe_changed);

    obs_property_t *ref=obs_properties_add_list(props,"reference","Reference frames",OBS_COMBO_TYPE_LIST,OBS_COMBO_FORMAT_INT);
    obs_property_list_add_int(ref,"Live",RETRO_REFERENCE_LIVE);
    obs_property_list_add_int(ref,"Skip keyframes",RETRO_REFERENCE_SKIP);
    obs_property_list_add_int(ref,"Cycle anchor",RETRO_REFERENCE_ANCHOR);
    obs_property_t *pred=obs_properties_add_list(props,"predictive","Predictive frames",OBS_COMBO_TYPE_LIST,OBS_COMBO_FORMAT_INT);
    obs_property_list_add_int(pred,"Live",RETRO_PREDICTIVE_LIVE);
    obs_property_list_add_int(pred,"Tear",RETRO_PREDICTIVE_DROP);
    obs_property_list_add_int(pred,"Burst",RETRO_PREDICTIVE_BURST);
    obs_property_list_add_int(pred,"Echo",RETRO_PREDICTIVE_ECHO);
    obs_property_list_add_int(pred,"Rewind",RETRO_PREDICTIVE_REWIND);
    obs_properties_add_bool(props,"layer_shimmer","Layer: Motion Shimmer");
    obs_properties_add_bool(props,"layer_flow","Layer: Flow Drift");
    obs_properties_add_bool(props,"layer_peel","Layer: Frame Peel");
    obs_property_t *compressed=obs_properties_add_bool(props,"layer_compressed","Layer: Packet editing");
    obs_property_set_long_description(compressed,"Rewrite actual MPEG-4 motion deltas and inter residual coefficients before decoding.");
    obs_properties_add_int_slider(props,"mv_x","Motion delta X (%)",-500,500,5);
    obs_properties_add_int_slider(props,"mv_y","Motion delta Y (%)",-500,500,5);
    obs_properties_add_int_slider(props,"residual_scale","Residual strength (%)",-200,200,5);
    obs_properties_add_int_slider(props,"residual_loss","Residual block loss (%)",0,100,1);
    const char *frequency_keys[]={"frequency_low","frequency_mid","frequency_high"};
    const char *frequency_labels[]={"Spectral low scan change (%)",
                                   "Spectral mid scan change (%)",
                                   "Spectral high scan change (%)"};
    for(int band=0;band<3;band++){
        obs_property_t *p=obs_properties_add_int_slider(props,frequency_keys[band],
            frequency_labels[band],-100,200,5);
        obs_property_set_long_description(p,
            "Reweight quantized MPEG-4 predictive-frame residual coefficients by zigzag scan position. "
            "Low: 0–9, mid: 10–31, high: 32–63. 0 leaves unchanged, -100 removes, "
            "+100 doubles. This is an approximate frequency order, not true spectral filtering; "
            "I-frames are not edited.");
    }
    obs_property_t *phantom=obs_properties_add_int_slider(props,"phantom_drift",
        "Phantom Drift (skipped blocks %)",0,100,1);
    obs_property_set_long_description(phantom,
        "Turns a proportion of skipped MPEG-4 macroblocks into residual-free moving blocks. "
        "Works on textured stills and static footage; 0 disables the effect. "
        "Motion deltas influence predictive neighbors, so output is intentionally unstable.");
    obs_properties_add_int_slider(props, "skip_keyframes", "Classic: skipped keyframes", 0, 8, 1);
    obs_properties_add_int_slider(props, "p_interval", "P-frame interval (Tear/Burst/Echo)", 2, 60, 1);
    obs_properties_add_int_slider(props, "p_burst", "Burst: frames lost per interval", 1, 30, 1);
    obs_properties_add_int_slider(props, "vector_strength", "Flow: motion exaggeration (%)", 0, 500, 10);
    obs_properties_add_int_slider(props, "vector_persistence", "Flow: previous-frame persistence (%)", 0, 100, 5);
    obs_properties_add_int_slider(props, "rounding_interval", "Shimmer: edit every N P-frames", 1, 60, 1);
    obs_properties_add_int_slider(props, "anchor_span", "Anchor: keyframes per held image", 2, 12, 1);
    obs_properties_add_int_slider(props, "rewind_depth", "Rewind: P-frames back", 2, 12, 1);
    obs_properties_add_int_slider(props, "slice_history", "Peel: frames in history", 2, 8, 1);
    obs_properties_add_int_slider(props, "slice_height", "Peel: band height (codec pixels)", 1, 64, 1);
    obs_properties_add_int_slider(props, "gop", "Keyframe interval (frames)", 2, 120, 1);
    obs_properties_add_int_slider(props, "kbps", "Bitrate (kb/s)", 8, 2000, 1);
    obs_properties_add_int_slider(props, "fps", "Codec frame rate", 1, 30, 1);
    obs_properties_add_int_slider(props,"codec_height","Codec height (pixels)",64,576,16);
    obs_properties_add_int_slider(props,"wet_mix","Wet mix (%)",0,100,1);
    obs_properties_add_bool(props,"preserve_alpha","Preserve live source alpha");
    obs_properties_add_button2(props,"hold_anchor","Hold reference",hold_clicked,s);
    obs_properties_add_button2(props,"release_anchor","Release reference",release_clicked,s);
    obs_properties_add_button2(props,"reset_codec","Reset codec and history",reset_clicked,s);
    const char *dynamic[]={"reference","predictive","layer_shimmer","layer_flow","layer_peel","layer_compressed"};
    for(size_t i=0;i<sizeof dynamic/sizeof *dynamic;i++)
        obs_property_set_modified_callback(obs_properties_get(props,dynamic[i]),mosh_controls_changed);
    obs_data_t *settings=s?obs_source_get_settings(s->source):obs_data_create();
    if(!s) mosh_defaults(settings);
    mosh_property_visibility(props,settings);
    obs_data_release(settings);
    return props;
}

static void *mosh_create(obs_data_t *settings, obs_source_t *source)
{
    struct mosh_filter *s = bzalloc(sizeof(*s));
    if (!s) return NULL;
    s->source = source;
    s->config_lock=framerot_mutex_create();
    if(!s->config_lock) {bfree(s);return NULL;}
    s->due=true;
    s->hold_key=obs_hotkey_register_source(source,"framerot.hold","framerot: hold reference (while pressed)",hold_hotkey,s);
    s->reset_key=obs_hotkey_register_source(source,"framerot.reset","framerot: reset codec and history",reset_hotkey,s);
    mosh_update(s, settings);
    return s;
}

static void mosh_destroy(void *data)
{
    struct mosh_filter *s = data;
    if (!s) return;
    if(s->render_count) blog(LOG_INFO,"[framerot] render callbacks=%llu avg_ms=%.3f max_ms=%.3f",
        (unsigned long long)s->render_count,(double)s->render_ns/s->render_count/1e6,(double)s->render_max_ns/1e6);
    if(s->hold_key!=OBS_INVALID_HOTKEY_ID) obs_hotkey_unregister(s->hold_key);
    if(s->reset_key!=OBS_INVALID_HOTKEY_ID) obs_hotkey_unregister(s->reset_key);
    retro_video_obs_destroy(s->codec);
    obs_enter_graphics();
    if (s->capture) gs_texrender_destroy(s->capture);
    if(s->mix_effect) gs_effect_destroy(s->mix_effect);
    obs_leave_graphics();
    framerot_mutex_destroy(s->config_lock);
    bfree(s);
}

static void mosh_tick(void *data, float seconds)
{
    struct mosh_filter *s = data;
    framerot_mutex_lock(s->config_lock);int fps=s->options.fps;framerot_mutex_unlock(s->config_lock);
    if (fps <= 0) return;
    s->elapsed += seconds;
    const float interval = 1.0f / (float)fps;
    if (s->elapsed >= interval) {
        framerot_mutex_lock(s->config_lock);s->due=true;framerot_mutex_unlock(s->config_lock);
        s->elapsed -= interval * (int)(s->elapsed / interval);
    }
}

static void draw_texture(gs_texture_t *texture, uint32_t width, uint32_t height)
{
    if (!texture) return;
    gs_effect_t *effect = obs_get_base_effect(OBS_EFFECT_DEFAULT);
    gs_eparam_t *image = gs_effect_get_param_by_name(effect, "image");
    if (!image) return;
    gs_effect_set_texture(image, texture);
    gs_blend_state_push();
    gs_reset_blend_state();
    gs_enable_blending(false);
    /* Draw in OBS's existing source projection. Overwriting it with a
     * source-sized ortho corrupts scene item placement on larger canvases. */
    while (gs_effect_loop(effect, "Draw"))
        gs_draw_sprite(texture, 0, width, height);
    gs_blend_state_pop();
}

/* Input capture is straight alpha (DrawAlphaDivide). Return premultiplied
 * source pixels; RGB comes from the codec, alpha from the live source. */
static const char *mix_shader =
    "uniform float4x4 ViewProj; uniform texture2d image; uniform texture2d dry; "
    "uniform float amount; uniform float preserve; "
    "sampler_state samp {Filter=Linear; AddressU=Clamp; AddressV=Clamp;}; "
    "struct Vertex {float4 pos:POSITION; float2 uv:TEXCOORD0;}; "
    "Vertex vs(Vertex v) { v.pos=mul(float4(v.pos.xyz,1.0),ViewProj); return v; } "
    "float4 ps(Vertex v):TARGET {float4 d=dry.Sample(samp,v.uv); float4 w=image.Sample(samp,v.uv); "
    "float a=lerp(lerp(d.a,1.0,amount),d.a,preserve); return float4(lerp(d.rgb,w.rgb,amount)*a,a); } "
    "technique Draw {pass {vertex_shader=vs(v); pixel_shader=ps(v);}}";

static void draw_mix(struct mosh_filter *s,gs_texture_t *dry,gs_texture_t *wet,uint32_t width,uint32_t height,int mix,bool alpha)
{
    if(!dry) {draw_texture(wet,width,height);return;}
    if(!s->mix_effect) {
        char *error=NULL;
        s->mix_effect=gs_effect_create(mix_shader,"framerot-mix",&error);
        if(error) {blog(LOG_WARNING,"[framerot] mix shader: %s",error);bfree(error);}
    }
    if(!s->mix_effect) {draw_texture(dry,width,height);return;}
    gs_effect_t *effect=s->mix_effect;
    gs_effect_set_texture(gs_effect_get_param_by_name(effect,"image"),wet?wet:dry);
    gs_effect_set_texture(gs_effect_get_param_by_name(effect,"dry"),dry);
    gs_effect_set_float(gs_effect_get_param_by_name(effect,"amount"),wet?(float)mix/100.0f:0);
    gs_effect_set_float(gs_effect_get_param_by_name(effect,"preserve"),alpha?1.0f:0.0f);
    gs_blend_state_push();
    gs_reset_blend_state();
    gs_enable_blending(true);
    gs_blend_function(GS_BLEND_ONE,GS_BLEND_INVSRCALPHA);
    while(gs_effect_loop(effect,"Draw")) gs_draw_sprite(dry,0,width,height);
    gs_blend_state_pop();
}

static void mosh_render_impl(void *data, gs_effect_t *unused)
{
    UNUSED_PARAMETER(unused);
    struct mosh_filter *s=data;
    obs_source_t *target=obs_filter_get_target(s->source);
    const uint32_t width=target?obs_source_get_base_width(target):0;
    const uint32_t height=target?obs_source_get_base_height(target):0;
    if(!width || !height) {obs_source_skip_video_filter(s->source);return;}
    /* The MPEG-4 path is SDR. Do not feed an HDR framebuffer into an
     * unvalidated 8-bit color transform. */
    const enum gs_color_space space=gs_get_color_space();
    if(space==GS_CS_709_EXTENDED || space==GS_CS_709_SCRGB) {obs_source_skip_video_filter(s->source);return;}
    framerot_mutex_lock(s->config_lock);
    struct retro_video_options sized=s->options;
    int mix=s->wet_mix;bool alpha=s->preserve_alpha;
    bool reset=s->reset,due=s->due,hold=s->hold;s->reset=false;s->due=false;
    framerot_mutex_unlock(s->config_lock);
    uint64_t macroblocks=((uint64_t)width*sized.height+(uint64_t)height*8)/((uint64_t)height*16);
    if(macroblocks<1) macroblocks=1;
    if(macroblocks>64) macroblocks=64;
    sized.width=(int)(macroblocks*16);
    if(reset || !retro_video_obs_matches(s->codec,&sized)) {
        if(s->codec || reset || os_gettime_ns()>=s->retry_at) {
            retro_video_obs_destroy(s->codec);
            s->codec=retro_video_obs_create(&sized);
            s->retry_at=os_gettime_ns()+250000000ull;
            due=true;
        }
    }
    retro_video_obs_controls(s->codec,&sized);
    retro_video_obs_hold(s->codec,hold);
    gs_texture_t *processed=retro_video_obs_render(s->codec,NULL,false);
    if(!due && processed && mix==100 && !alpha) {
        draw_texture(processed,width,height);
        return;
    }
    if(mix==0 && !due) {obs_source_skip_video_filter(s->source);return;}
    if(!s->capture) s->capture=gs_texrender_create(GS_RGBA,GS_ZS_NONE);
    if(!s->capture) {obs_source_skip_video_filter(s->source);return;}
    gs_texrender_reset(s->capture);
    if(!obs_source_process_filter_begin_with_color_space(s->source,GS_RGBA,GS_CS_SRGB,OBS_NO_DIRECT_RENDERING)) return;
    if(!gs_texrender_begin(s->capture,width,height)) {
        /* Balance the begun filter rather than claiming a stale target. */
        obs_source_process_filter_end(s->source,obs_get_base_effect(OBS_EFFECT_DEFAULT),width,height);
        return;
    }
    gs_blend_state_push();gs_reset_blend_state();gs_enable_blending(false);
    gs_projection_push();gs_ortho(0,(float)width,0,(float)height,-100,100);
    obs_source_process_filter_tech_end(s->source,obs_get_base_effect(OBS_EFFECT_DEFAULT),width,height,"DrawAlphaDivide");
    gs_projection_pop();gs_texrender_end(s->capture);gs_blend_state_pop();
    gs_texture_t *dry=gs_texrender_get_texture(s->capture);
    if(due) processed=retro_video_obs_render(s->codec,s->capture,true);
    if(processed && mix==100 && !alpha) draw_texture(processed,width,height);
    else draw_mix(s,dry,processed,width,height,mix,alpha);
}

static void mosh_render(void *data,gs_effect_t *effect)
{
    struct mosh_filter *s=data;uint64_t start=os_gettime_ns();
    mosh_render_impl(data,effect);
    uint64_t ns=os_gettime_ns()-start;
    s->render_count++;s->render_ns+=ns;if(ns>s->render_max_ns)s->render_max_ns=ns;
}

static struct obs_source_info mosh_info = {
    .id = "framerot_filter",
    .type = OBS_SOURCE_TYPE_FILTER,
    .output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_SRGB,
    .get_name = mosh_name,
    .create = mosh_create,
    .destroy = mosh_destroy,
    .update = mosh_update,
    .get_defaults = mosh_defaults,
    .get_properties = mosh_properties,
    .video_tick = mosh_tick,
    .video_render = mosh_render,
};

bool obs_module_load(void)
{
    obs_register_source(&mosh_info);
    return true;
}
