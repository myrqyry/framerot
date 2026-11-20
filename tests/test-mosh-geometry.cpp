#include <obs.h>
#include <obs-module.h>
#include <graphics/graphics.h>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <thread>
#include <vector>

#ifdef MOSH_LIVE_GEOMETRY
/* The real reported setup: a 640x480 camera inside a 1920x1080 scene. */
static constexpr uint32_t SOURCE_W = 640, SOURCE_H = 480;
static constexpr uint32_t CANVAS_W = 1920, CANVAS_H = 1080;
static constexpr float POS_X = 0, POS_Y = 0, SOURCE_SCALE = 1.0f;
static constexpr int SOURCE_SAMPLE_X = 320, SOURCE_SAMPLE_Y = 240;
static constexpr int OUTSIDE_X = 850, OUTSIDE_Y = 510;
static constexpr int MARKER_X = 1460, MARKER_Y = 780;
static constexpr int CORNER_X = 1840, CORNER_Y = 1000;
#else
static constexpr uint32_t SOURCE_W = 256, SOURCE_H = 144;
static constexpr uint32_t CANVAS_W = 640, CANVAS_H = 360;
static constexpr float POS_X = 40, POS_Y = 30, SOURCE_SCALE = 1.5f;
static constexpr int SOURCE_SAMPLE_X = 160, SOURCE_SAMPLE_Y = 100;
static constexpr int OUTSIDE_X = 450, OUTSIDE_Y = 100;
static constexpr int MARKER_X = 500, MARKER_Y = 260;
static constexpr int CORNER_X = 610, CORNER_Y = 330;
#endif

struct SourceData { gs_texture_t *texture = nullptr; };
static const char *name(void *) { return "Geometry test"; }
static uint32_t width(void *) { return SOURCE_W; }
static uint32_t height(void *) { return SOURCE_H; }

static void *create(obs_data_t *, obs_source_t *)
{
    auto *s = new SourceData;
    std::vector<uint8_t> rgba(SOURCE_W * SOURCE_H * 4);
    for (size_t i = 0; i < rgba.size(); i += 4) {
        rgba[i] = 30; rgba[i+1] = 95; rgba[i+2] = 225; rgba[i+3] = 255;
    }
    const uint8_t *bytes[] = {rgba.data()};
    obs_enter_graphics();
    s->texture = gs_texture_create(SOURCE_W, SOURCE_H, GS_RGBA, 1, bytes, 0);
    obs_leave_graphics();
    return s;
}
static void destroy(void *data)
{
    auto *s = (SourceData *)data;
    obs_enter_graphics();
    if (s->texture) gs_texture_destroy(s->texture);
    obs_leave_graphics();
    delete s;
}
static void render(void *data, gs_effect_t *)
{
    auto *s = (SourceData *)data;
    if (s->texture) obs_source_draw(s->texture, 0, 0, SOURCE_W, SOURCE_H, false);
}
static obs_source_info source_info = {
    .id = "datamosh_geometry_input",
    .type = OBS_SOURCE_TYPE_INPUT,
    .output_flags = OBS_SOURCE_VIDEO,
    .get_name = name,
    .create = create,
    .destroy = destroy,
    .get_width = width,
    .get_height = height,
    .video_render = render,
};

static bool pixel_color(const uint8_t *data, uint32_t stride, int x, int y, char wanted)
{
    const uint8_t *p = data + (size_t)y * stride + (size_t)x * 4;
    if (wanted == 'g')
        return p[1] > 165 && p[1] > p[0] * 2 && p[1] > p[2] * 2;
    if (wanted == 'b')
        return p[2] > 135 && p[2] > p[0] * 2;
    return p[0] < 15 && p[1] < 15 && p[2] < 15;
}

int main(int argc, char **argv)
{
    if (argc != 3) return 2;
    setbuf(stdout, nullptr);
    if (!obs_startup("en-US", nullptr, nullptr)) return 3;
    obs_video_info cfg{};
    cfg.graphics_module = "libobs-opengl";
    cfg.fps_num = 30;
    cfg.fps_den = 1;
    cfg.base_width = CANVAS_W;
    cfg.base_height = CANVAS_H;
    cfg.output_width = CANVAS_W;
    cfg.output_height = CANVAS_H;
    cfg.output_format = VIDEO_FORMAT_RGBA;
    cfg.scale_type = OBS_SCALE_DISABLE;
    if (obs_reset_video(&cfg) != OBS_VIDEO_SUCCESS) return 4;
    obs_register_source(&source_info);

    obs_module_t *module = nullptr;
    if (obs_open_module(&module, argv[1], argv[2]) != MODULE_SUCCESS ||
        !obs_init_module(module)) return 5;

    obs_source_t *source = obs_source_create("datamosh_geometry_input", "geometry-source", nullptr, nullptr);
    obs_data_t *settings = obs_data_create();
    obs_data_set_int(settings, "mode", 0);
    obs_data_set_int(settings, "fps", 12);
    obs_data_set_int(settings, "kbps", 96);
    obs_data_set_int(settings, "gop", 8);
    obs_source_t *filter = obs_source_create("framerot_filter", "geometry-filter", settings, nullptr);
    obs_data_release(settings);
    if (!source || !filter) return 6;
    obs_source_filter_add(source, filter);
    obs_set_output_source(0, source);

    std::vector<uint8_t> green(32 * 32 * 4);
    for (size_t i = 0; i < green.size(); i += 4) {
        green[i] = 10; green[i+1] = 240; green[i+2] = 5; green[i+3] = 255;
    }
    obs_enter_graphics();
    const uint8_t *green_bytes[] = {green.data()};
    gs_texture_t *marker = gs_texture_create(32, 32, GS_RGBA, 1, green_bytes, 0);
    obs_leave_graphics();
    if (!marker) return 7;

    int inside = 0, outside = 0, marker_correct = 0, corner_empty = 0;
    for (int frame = 0; frame < 8; ++frame) {
        std::this_thread::sleep_for(std::chrono::milliseconds(110));
        obs_enter_graphics();
        gs_texrender_t *canvas = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
        if (canvas && gs_texrender_begin_with_color_space(canvas, CANVAS_W, CANVAS_H, GS_CS_SRGB)) {
            struct vec4 clear;
            vec4_set(&clear, 0, 0, 0, 1);
            gs_clear(GS_CLEAR_COLOR, &clear, 1, 0);
            gs_projection_push();
            gs_ortho(0, CANVAS_W, 0, CANVAS_H, -100, 100);

            /* Source occupies its configured scene-item transform, not the whole canvas. */
            gs_matrix_push();
            gs_matrix_translate3f(POS_X, POS_Y, 0);
            gs_matrix_scale3f(SOURCE_SCALE, SOURCE_SCALE, 1);
            obs_source_video_render(source);
            gs_matrix_pop();

            /* A completely independent scene element *after* the filter.
             * If filter code overwrites the scene projection, this marker
             * moves or disappears outside its independently specified coordinates. */
            gs_matrix_push();
            gs_matrix_translate3f(MARKER_X, MARKER_Y, 0);
            gs_effect_t *effect = obs_get_base_effect(OBS_EFFECT_DEFAULT);
            gs_eparam_t *image = gs_effect_get_param_by_name(effect, "image");
            gs_effect_set_texture(image, marker);
            while (gs_effect_loop(effect, "Draw"))
                gs_draw_sprite(marker, 0, 32, 32);
            gs_matrix_pop();

            gs_projection_pop();
            gs_texrender_end(canvas);

            gs_stagesurf_t *stage = gs_stagesurface_create(CANVAS_W, CANVAS_H, GS_RGBA);
            if (stage) {
                gs_stage_texture(stage, gs_texrender_get_texture(canvas));
                uint8_t *pixels = nullptr;
                uint32_t stride = 0;
                if (gs_stagesurface_map(stage, &pixels, &stride)) {
                    inside += pixel_color(pixels, stride, SOURCE_SAMPLE_X, SOURCE_SAMPLE_Y, 'b');
                    outside += pixel_color(pixels, stride, OUTSIDE_X, OUTSIDE_Y, 'k');
                    marker_correct += pixel_color(pixels, stride, MARKER_X + 10, MARKER_Y + 10, 'g');
                    corner_empty += pixel_color(pixels, stride, CORNER_X, CORNER_Y, 'k');
                    gs_stagesurface_unmap(stage);
                }
                gs_stagesurface_destroy(stage);
            }
        }
        if (canvas) gs_texrender_destroy(canvas);
        obs_leave_graphics();
    }
    std::printf("GEOMETRY_CANVAS source=%ux%u canvas=%ux%u inside=%d outside=%d marker=%d corner=%d\n",
                SOURCE_W, SOURCE_H, CANVAS_W, CANVAS_H, inside, outside,
                marker_correct, corner_empty);

    obs_enter_graphics();
    gs_texture_destroy(marker);
    obs_leave_graphics();
    obs_set_output_source(0, nullptr);
    obs_source_filter_remove(source, filter);
    obs_source_release(filter);
    obs_source_release(source);
    obs_shutdown();

    /* This checks transform behavior, not merely whether edges are filled. */
    return inside >= 6 && outside >= 6 && marker_correct >= 6 && corner_empty >= 6 ? 0 : 8;
}
