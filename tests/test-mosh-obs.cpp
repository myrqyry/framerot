#include <obs.h>
#include <obs-module.h>
#include <graphics/graphics.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

#ifdef MOSH_PORTRAIT
static constexpr uint32_t W = 144, H = 256;
#else
static constexpr uint32_t W = 256, H = 144;
#endif
struct Input {
    gs_texture_t *texture = nullptr;
    std::atomic<int> tick{0};
};

static const char *input_name(void *) { return "Datamosh moving test source"; }
static uint32_t input_width(void *) { return W; }
static uint32_t input_height(void *) { return H; }

static void *input_create(obs_data_t *, obs_source_t *)
{
    auto *s = new Input{};
    obs_enter_graphics();
    s->texture = gs_texture_create(W, H, GS_RGBA, 1, nullptr, GS_DYNAMIC);
    obs_leave_graphics();
    return s;
}

static void input_destroy(void *data)
{
    auto *s = (Input *)data;
    obs_enter_graphics();
    if (s->texture) gs_texture_destroy(s->texture);
    obs_leave_graphics();
    delete s;
}

static void input_tick(void *data, float)
{
    ((Input *)data)->tick.fetch_add(1);
}

static void input_render(void *data, gs_effect_t *)
{
    auto *s = (Input *)data;
    if (!s->texture) return;
    const int t = s->tick.load();
    std::vector<uint8_t> rgba(W * H * 4);
    for (uint32_t y = 0; y < H; ++y)
        for (uint32_t x = 0; x < W; ++x) {
            size_t i = (y * W + x) * 4;
            rgba[i] = (uint8_t)((x * 3 + t * 7) & 255);
            rgba[i + 1] = (uint8_t)((y * 2 + (x / 12 + t / 4) % 2 * 180) & 255);
            rgba[i + 2] = (uint8_t)(((x + t * 5) / 10 + y / 12) % 2 * 180 + 25);
            rgba[i + 3] = 255;
        }
    gs_texture_set_image(s->texture, rgba.data(), W * 4, false);
    obs_source_draw(s->texture, 0, 0, W, H, false);
}

static obs_source_info input_info = {
    .id = "mosh_test_source",
    .type = OBS_SOURCE_TYPE_INPUT,
    .output_flags = OBS_SOURCE_VIDEO,
    .get_name = input_name,
    .create = input_create,
    .destroy = input_destroy,
    .get_width = input_width,
    .get_height = input_height,
    .video_render = input_render,
};

int main(int argc, char **argv)
{
    if (argc != 3) return 2;
    setbuf(stdout, nullptr);
    if (!obs_startup("en-US", nullptr, nullptr)) return 3;
    obs_video_info video{};
    video.graphics_module = "libobs-opengl";
    video.fps_num = 30;
    video.fps_den = 1;
    video.base_width = W;
    video.base_height = H;
    video.output_width = W;
    video.output_height = H;
    video.output_format = VIDEO_FORMAT_RGBA;
    video.scale_type = OBS_SCALE_DISABLE;
    if (obs_reset_video(&video) != OBS_VIDEO_SUCCESS) return 4;

    input_info.video_tick = input_tick;
    obs_register_source(&input_info);
    obs_module_t *module = nullptr;
    auto status = obs_open_module(&module, argv[1], argv[2]);
    if (status != MODULE_SUCCESS || !obs_init_module(module)) {
        std::fprintf(stderr, "MODULE_LOAD_FAILED %d\n", (int)status);
        return 5;
    }

    /* UI defaults must follow the selected effect without writing user
     * overrides. The current fixed defaults (96kbps/12fps/6 GOP) must fail
     * this regression when switching to Keyframe Anchor or Flow Drift. */
    obs_source_t *preset_source = obs_source_create(
        "framerot_filter", "mosh-preset-test", nullptr, nullptr);
    if (!preset_source) return 15;
    obs_data_t *preset_settings = obs_source_get_settings(preset_source);
    obs_properties_t *preset_props = obs_source_properties(preset_source);
    obs_property_t *preset_mode = obs_properties_get(preset_props, "mode");
    if (!preset_settings || !preset_props || !preset_mode) return 16;
    struct Showcase {
        int mode, fps, kbps, gop, p_interval, p_burst;
    };
    const Showcase cases[] = {
        {0, 12, 96, 6, 5, 3},
        {1, 15, 160, 12, 5, 3},
        {2, 15, 160, 18, 12, 5},
        {3, 15, 160, 18, 3, 3},
        {4, 15, 230, 24, 5, 3},
        {5, 15, 200, 24, 5, 3},
        {6, 15, 160, 12, 5, 3},
        {7, 15, 220, 24, 5, 3},
        {8, 15, 200, 18, 5, 3}
    };
    for (const auto &entry : cases) {
        obs_data_set_int(preset_settings, "mode", entry.mode);
        obs_property_modified(preset_mode, preset_settings);
        if (obs_data_get_int(preset_settings, "fps") != entry.fps ||
            obs_data_get_int(preset_settings, "kbps") != entry.kbps ||
            obs_data_get_int(preset_settings, "gop") != entry.gop) {
            std::fprintf(stderr, "SHOWCASE_DEFAULTS_FAILED mode=%d fps=%lld kbps=%lld gop=%lld\n",
                entry.mode, (long long)obs_data_get_int(preset_settings, "fps"),
                (long long)obs_data_get_int(preset_settings, "kbps"),
                (long long)obs_data_get_int(preset_settings, "gop"));
            return 17;
        }
        if (obs_data_get_int(preset_settings, "p_interval") != entry.p_interval ||
            obs_data_get_int(preset_settings, "p_burst") != entry.p_burst) {
            std::fprintf(stderr, "SHOWCASE_PACKET_INTERVAL_FAILED mode=%d\n", entry.mode);
            return 18;
        }
        if (entry.mode == 4 &&
            (obs_data_get_int(preset_settings, "vector_strength") != 250 ||
             obs_data_get_int(preset_settings, "vector_persistence") != 65))
            return 20;
        if (entry.mode == 5 && obs_data_get_int(preset_settings, "rounding_interval") != 1)
            return 21;
        if (entry.mode == 6 && obs_data_get_int(preset_settings, "anchor_span") != 4)
            return 22;
        if (entry.mode == 7 && obs_data_get_int(preset_settings, "rewind_depth") != 4)
            return 26;
        if (entry.mode == 8 &&
            (obs_data_get_int(preset_settings, "slice_history") != 4 ||
             obs_data_get_int(preset_settings, "slice_height") != 12))
            return 27;
        /* Returning to each mode should not leave the last mode's defaults. */
        obs_source_update(preset_source, preset_settings);
    }
    obs_data_set_int(preset_settings, "fps", 23);
    obs_data_set_int(preset_settings, "gop", 42);
    obs_data_set_int(preset_settings, "mode", 4);
    obs_property_modified(preset_mode, preset_settings);
    if (obs_data_get_int(preset_settings, "fps") != 23 ||
        obs_data_get_int(preset_settings, "gop") != 42 ||
        obs_data_get_int(preset_settings, "kbps") != 230) {
        std::fprintf(stderr, "SHOWCASE_OVERRIDES_CLOBBERED\n");
        return 23;
    }
    obs_properties_destroy(preset_props);
    obs_data_release(preset_settings);
    obs_source_release(preset_source);

    /* Loading a saved mode should get its corresponding defaults, while
     * explicitly saved settings are never silently overwritten. */
    obs_data_t *saved = obs_data_create();
    obs_data_set_int(saved, "mode", 6);
    obs_data_set_int(saved, "fps", 19);
    obs_source_t *reload = obs_source_create("framerot_filter", "mosh-reload-test", saved, nullptr);
    if (!reload) return 24;
    obs_data_t *reloaded = obs_source_get_settings(reload);
    if (obs_data_get_int(reloaded, "fps") != 19 ||
        obs_data_get_int(reloaded, "kbps") != 160 ||
        obs_data_get_int(reloaded, "gop") != 12) {
        std::fprintf(stderr, "SHOWCASE_RELOAD_FAILED fps=%lld kbps=%lld gop=%lld\n",
            (long long)obs_data_get_int(reloaded, "fps"),
            (long long)obs_data_get_int(reloaded, "kbps"),
            (long long)obs_data_get_int(reloaded, "gop"));
        return 25;
    }
    obs_data_release(reloaded);
    obs_source_release(reload);
    obs_data_release(saved);
    std::printf("SHOWCASE_DEFAULTS checked=9 explicit_overrides=preserved saved_mode=reloaded\n");

    /* Verify the clean-break OBS filter identity. */
    const char *display_name = obs_source_get_display_name("framerot_filter");
    if (!display_name || std::strcmp(display_name, "framerot") != 0) {
        std::fprintf(stderr, "RENAME_DISPLAY_FAILED: %s\n",
                     display_name ? display_name : "(missing)");
        return 30;
    }
    std::printf("RENAME_OBS_DISPLAY name=%s source_id=framerot_filter\n", display_name);

    auto *source = obs_source_create("mosh_test_source", "mosh-test-input", nullptr, nullptr);
    auto *settings = obs_data_create();
    obs_data_set_int(settings, "skip_keyframes", 1);
    obs_data_set_int(settings, "gop", 4);
    obs_data_set_int(settings, "fps", 12);
    obs_data_set_int(settings, "kbps", 64);
    auto *filter = obs_source_create("framerot_filter", "mosh-test-filter", settings, nullptr);
    obs_data_release(settings);
    if (!source || !filter) {
        std::fprintf(stderr, "FILTER_CREATE_FAILED\n");
        return 6;
    }

    obs_properties_t *props = obs_source_properties(filter);
    if (!props || !obs_properties_get(props, "mode") ||
        !obs_properties_get(props, "p_interval") ||
        !obs_properties_get(props, "p_burst") ||
        !obs_properties_get(props, "vector_strength") ||
        !obs_properties_get(props, "vector_persistence") ||
        !obs_properties_get(props, "rounding_interval") ||
        !obs_properties_get(props, "anchor_span") ||
        !obs_properties_get(props, "rewind_depth") ||
        !obs_properties_get(props, "slice_history") ||
        !obs_properties_get(props, "slice_height") ||
        !obs_properties_get(props, "skip_keyframes") ||
        !obs_properties_get(props, "gop") || !obs_properties_get(props, "fps") ||
        !obs_properties_get(props, "kbps")) {
        std::fprintf(stderr, "FILTER_PROPERTIES_FAILED\n");
        return 7;
    }
    auto *classic = obs_properties_get(props, "skip_keyframes");
    auto *interval = obs_properties_get(props, "p_interval");
    auto *burst = obs_properties_get(props, "p_burst");
    if (!obs_property_visible(classic) || obs_property_visible(interval) ||
        obs_property_visible(burst)) {
        std::fprintf(stderr, "MODE_PROPERTY_INITIAL_VISIBILITY_FAILED\n");
        return 9;
    }
    obs_data_t *mode_preview = obs_source_get_settings(filter);
    obs_data_set_int(mode_preview, "mode", 2);
    obs_property_modified(obs_properties_get(props, "mode"), mode_preview);
    if (obs_property_visible(classic) || !obs_property_visible(interval) ||
        !obs_property_visible(burst)) {
        std::fprintf(stderr, "MODE_PROPERTY_BURST_VISIBILITY_FAILED\n");
        return 10;
    }
    obs_data_set_int(mode_preview, "mode", 3);
    obs_property_modified(obs_properties_get(props, "mode"), mode_preview);
    if (obs_property_visible(classic) || !obs_property_visible(interval) ||
        obs_property_visible(burst)) {
        std::fprintf(stderr, "MODE_PROPERTY_ECHO_VISIBILITY_FAILED\n");
        return 11;
    }
    obs_data_set_int(mode_preview, "mode", 4);
    obs_property_modified(obs_properties_get(props, "mode"), mode_preview);
    if (obs_property_visible(classic) || obs_property_visible(interval) ||
        obs_property_visible(burst) ||
        !obs_property_visible(obs_properties_get(props, "vector_strength")) ||
        !obs_property_visible(obs_properties_get(props, "vector_persistence"))) {
        std::fprintf(stderr, "MODE_PROPERTY_VECTOR_VISIBILITY_FAILED\n");
        return 12;
    }
    obs_data_set_int(mode_preview, "mode", 5);
    obs_property_modified(obs_properties_get(props, "mode"), mode_preview);
    if (obs_property_visible(classic) || obs_property_visible(interval) ||
        obs_property_visible(burst) ||
        obs_property_visible(obs_properties_get(props, "vector_strength")) ||
        obs_property_visible(obs_properties_get(props, "vector_persistence")) ||
        !obs_property_visible(obs_properties_get(props, "rounding_interval"))) {
        std::fprintf(stderr, "MODE_PROPERTY_SHIMMER_VISIBILITY_FAILED\n");
        return 13;
    }
    obs_data_set_int(mode_preview, "mode", 6);
    obs_property_modified(obs_properties_get(props, "mode"), mode_preview);
    if (obs_property_visible(classic) || obs_property_visible(interval) ||
        obs_property_visible(burst) ||
        obs_property_visible(obs_properties_get(props, "vector_strength")) ||
        obs_property_visible(obs_properties_get(props, "vector_persistence")) ||
        obs_property_visible(obs_properties_get(props, "rounding_interval")) ||
        !obs_property_visible(obs_properties_get(props, "anchor_span"))) {
        std::fprintf(stderr, "MODE_PROPERTY_ANCHOR_VISIBILITY_FAILED\n");
        return 14;
    }
    obs_data_set_int(mode_preview, "mode", 7);
    obs_property_modified(obs_properties_get(props, "mode"), mode_preview);
    if (obs_property_visible(classic) || obs_property_visible(interval) ||
        obs_property_visible(burst) ||
        !obs_property_visible(obs_properties_get(props, "rewind_depth")) ||
        obs_property_visible(obs_properties_get(props, "slice_history")) ||
        obs_property_visible(obs_properties_get(props, "slice_height"))) {
        std::fprintf(stderr, "MODE_PROPERTY_REWIND_VISIBILITY_FAILED\n");
        return 28;
    }
    obs_data_set_int(mode_preview, "mode", 8);
    obs_property_modified(obs_properties_get(props, "mode"), mode_preview);
    if (obs_property_visible(classic) || obs_property_visible(interval) ||
        obs_property_visible(burst) ||
        obs_property_visible(obs_properties_get(props, "rewind_depth")) ||
        !obs_property_visible(obs_properties_get(props, "slice_history")) ||
        !obs_property_visible(obs_properties_get(props, "slice_height"))) {
        std::fprintf(stderr, "MODE_PROPERTY_PEEL_VISIBILITY_FAILED\n");
        return 29;
    }
    obs_data_release(mode_preview);
    obs_properties_destroy(props);
    obs_source_filter_add(source, filter);
    obs_set_output_source(0, source);

    int decoded_nonblack = 0, filled_left_edge = 0, filled_right_edge = 0;
    int filled_top_edge = 0, filled_bottom_edge = 0, sample_changes = 0;
    int last_red = -1, last_green = -1;
    for (int frame = 0; frame < 108; ++frame) {
        /* Switch among compressed-packet modes during live render. */
        if (frame == 12 || frame == 24 || frame == 36 || frame == 48 ||
            frame == 60 || frame == 72 || frame == 84 || frame == 96) {
            obs_data_t *updated = obs_source_get_settings(filter);
            obs_data_set_int(updated, "mode",
                frame == 12 ? 1 : frame == 24 ? 2 : frame == 36 ? 3 :
                frame == 48 ? 4 : frame == 60 ? 5 : frame == 72 ? 6 :
                frame == 84 ? 7 : 8);
            obs_data_set_int(updated, "p_interval", frame == 36 ? 3 : 8);
            obs_data_set_int(updated, "p_burst", 3);
            obs_data_set_int(updated, "vector_strength", 250);
            obs_data_set_int(updated, "vector_persistence", 65);
            obs_data_set_int(updated, "rounding_interval", 1);
            obs_data_set_int(updated, "anchor_span", 2);
            obs_data_set_int(updated, "rewind_depth", 4);
            obs_data_set_int(updated, "slice_history", 4);
            obs_data_set_int(updated, "slice_height", 12);
            obs_data_set_int(updated, "skip_keyframes", frame == 48 ? 2 : 0);
            obs_source_update(filter, updated);
            obs_data_release(updated);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(110));
        obs_enter_graphics();
        gs_texrender_t *target = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
        if (target && gs_texrender_begin_with_color_space(target, W, H, GS_CS_SRGB)) {
            vec4 clear{0, 0, 0, 1};
            gs_clear(GS_CLEAR_COLOR, &clear, 1, 0);
            gs_projection_push();
            gs_ortho(0, W, 0, H, -100, 100);
            obs_source_video_render(source);
            gs_projection_pop();
            gs_texrender_end(target);
            gs_stagesurf_t *stage = gs_stagesurface_create(W, H, GS_RGBA);
            if (stage) {
                gs_stage_texture(stage, gs_texrender_get_texture(target));
                uint8_t *pixels = nullptr;
                uint32_t stride = 0;
                if (gs_stagesurface_map(stage, &pixels, &stride)) {
                    const uint8_t *middle = pixels + (H / 2) * stride + (W / 2) * 4;
                    const uint8_t *left = pixels + (H / 2) * stride + 3 * 4;
                    const uint8_t *right = pixels + (H / 2) * stride + (W - 4) * 4;
                    const uint8_t *top = pixels + 3 * stride + (W / 2) * 4;
                    const uint8_t *bottom = pixels + (H - 4) * stride + (W / 2) * 4;
                    decoded_nonblack += (middle[0] + middle[1] + middle[2] > 45);
                    filled_left_edge += (left[0] + left[1] + left[2] > 24);
                    filled_right_edge += (right[0] + right[1] + right[2] > 24);
                    filled_top_edge += (top[0] + top[1] + top[2] > 24);
                    filled_bottom_edge += (bottom[0] + bottom[1] + bottom[2] > 24);
                    if (last_red >= 0 && (middle[0] != last_red || middle[1] != last_green))
                        ++sample_changes;
                    last_red = middle[0];
                    last_green = middle[1];
                    gs_stagesurface_unmap(stage);
                }
                gs_stagesurface_destroy(stage);
            }
        }
        if (target) gs_texrender_destroy(target);
        obs_leave_graphics();
    }
    std::printf("MOSH_OBS %ux%u rendered=%d left=%d right=%d top=%d bottom=%d changed=%d\n",
                W, H, decoded_nonblack, filled_left_edge, filled_right_edge,
                filled_top_edge, filled_bottom_edge, sample_changes);
    obs_set_output_source(0, nullptr);
    obs_source_filter_remove(source, filter);
    obs_source_release(filter);
    obs_source_release(source);
    obs_shutdown();
    if (decoded_nonblack < 40 || filled_left_edge < 40 || filled_right_edge < 40 ||
        filled_top_edge < 40 || filled_bottom_edge < 40 || sample_changes < 12)
        return 8;
    return 0;
}
