#include "ra_tests.h"
#include "shaders.h"

static void shader_tests(struct pl_context *ctx, const struct ra *ra)
{
    const char *vert_shader =
        "#version 450                               \n"
        "layout(location=0) in vec2 vertex_pos;     \n"
        "layout(location=1) in vec3 vertex_color;   \n"
        "layout(location=0) out vec3 frag_color;    \n"
        "void main() {                              \n"
        "    gl_Position = vec4(vertex_pos, 0, 1);  \n"
        "    frag_color = vertex_color;             \n"
        "}";

    const char *frag_shader =
        "#version 450                               \n"
        "layout(location=0) in vec3 frag_color;     \n"
        "layout(location=0) out vec4 out_color;     \n"
        "void main() {                              \n"
        "    out_color = vec4(frag_color, 1.0);     \n"
        "}";

    const struct ra_fmt *fbo_fmt;
    enum ra_fmt_caps caps = RA_FMT_CAP_RENDERABLE | RA_FMT_CAP_BLITTABLE;
    fbo_fmt = ra_find_fmt(ra, RA_FMT_FLOAT, 4, 32, true, caps);
    if (!fbo_fmt)
        return;

#define FBO_W 16
#define FBO_H 16

    const struct ra_tex *fbo;
    fbo = ra_tex_create(ra, &(struct ra_tex_params) {
        .format         = fbo_fmt,
        .w              = FBO_W,
        .h              = FBO_H,
        .renderable     = true,
        .storable       = !!(fbo_fmt->caps & RA_FMT_CAP_STORABLE),
        .host_readable  = true,
        .blit_dst       = true,
    });
    REQUIRE(fbo);

    ra_tex_clear(ra, fbo, (float[4]){0});

    const struct ra_fmt *vert_fmt;
    vert_fmt = ra_find_vertex_fmt(ra, RA_FMT_FLOAT, 3);
    REQUIRE(vert_fmt);

    struct vertex { float pos[2]; float color[3]; } vertices[] = {
        {{-1.0, -1.0}, {0, 0, 0}},
        {{ 1.0, -1.0}, {1, 0, 0}},
        {{-1.0,  1.0}, {0, 1, 0}},
        {{ 1.0,  1.0}, {1, 1, 0}},
    };

    const struct ra_pass *pass;
    pass = ra_pass_create(ra, &(struct ra_pass_params) {
        .type           = RA_PASS_RASTER,
        .target_dummy   = *fbo,
        .vertex_shader  = vert_shader,
        .glsl_shader    = frag_shader,

        .vertex_type    = RA_PRIM_TRIANGLE_STRIP,
        .vertex_stride  = sizeof(struct vertex),
        .num_vertex_attribs = 2,
        .vertex_attribs = (struct ra_vertex_attrib[]) {{
            .name     = "vertex_pos",
            .fmt      = ra_find_vertex_fmt(ra, RA_FMT_FLOAT, 2),
            .location = 0,
            .offset   = offsetof(struct vertex, pos),
        }, {
            .name     = "vertex_color",
            .fmt      = ra_find_vertex_fmt(ra, RA_FMT_FLOAT, 3),
            .location = 1,
            .offset   = offsetof(struct vertex, color),
        }},
    });
    REQUIRE(pass);
    REQUIRE(pass->params.cached_program_len);

    ra_pass_run(ra, &(struct ra_pass_run_params) {
        .pass           = pass,
        .target         = fbo,
        .vertex_data    = vertices,
        .vertex_count   = sizeof(vertices) / sizeof(struct vertex),
    });

    static float data[FBO_H * FBO_W * 4] = {0};
    ra_tex_download(ra, &(struct ra_tex_transfer_params) {
        .tex = fbo,
        .ptr = data,
    });

    for (int y = 0; y < FBO_H; y++) {
        for (int x = 0; x < FBO_W; x++) {
            float *color = &data[(y * FBO_W + x) * 4];
            printf("color: %f %f %f %f\n", color[0], color[1], color[2], color[3]);
            REQUIRE(feq(color[0], (x + 0.5) / FBO_W));
            REQUIRE(feq(color[1], (y + 0.5) / FBO_H));
            REQUIRE(feq(color[2], 0.0));
            REQUIRE(feq(color[3], 1.0));
        }
    }

    ra_pass_destroy(ra, &pass);
    ra_tex_clear(ra, fbo, (float[4]){0});

    // Test the use of pl_dispatch
    struct pl_dispatch *dp = pl_dispatch_create(ctx, ra);

    const struct ra_tex *src;
    src = ra_tex_create(ra, &(struct ra_tex_params) {
        .format         = fbo_fmt,
        .w              = FBO_W,
        .h              = FBO_H,
        .sampleable     = true,
        .initial_data   = data,
    });

    // Repeat this a few times to test the caching
    for (int i = 0; i < 10; i++) {
        printf("iteration %d\n", i);
        pl_dispatch_reset_frame(dp);
        struct pl_shader *sh = pl_dispatch_begin(dp);

        // For testing, force the use of CS if possible
        if (ra->caps & RA_CAP_COMPUTE) {
            sh->is_compute = true;
            sh->res.compute_group_size[0] = 8;
            sh->res.compute_group_size[1] = 8;
        }

        pl_shader_deband(sh, src, &(struct pl_deband_params) {
            .iterations     = 0,
            .grain          = 0.0,
        });

        pl_shader_linearize(sh, PL_COLOR_TRC_GAMMA22);
        REQUIRE(pl_dispatch_finish(dp, sh, fbo));
    }

    ra_tex_download(ra, &(struct ra_tex_transfer_params) {
        .tex = fbo,
        .ptr = data,
    });

    for (int y = 0; y < FBO_H; y++) {
        for (int x = 0; x < FBO_W; x++) {
            float *color = &data[(y * FBO_W + x) * 4];
            printf("color: %f %f %f %f\n", color[0], color[1], color[2], color[3]);
            REQUIRE(feq(color[0], pow((x + 0.5) / FBO_W, 2.2)));
            REQUIRE(feq(color[1], pow((y + 0.5) / FBO_H, 2.2)));
            REQUIRE(feq(color[2], 0.0));
            REQUIRE(feq(color[3], 1.0));
        }
    }

    pl_dispatch_destroy(&dp);
    ra_tex_destroy(ra, &src);
    ra_tex_destroy(ra, &fbo);
}

static void scaler_tests(struct pl_context *ctx, const struct ra *ra)
{
    const struct ra_fmt *src_fmt = ra_find_fmt(ra, RA_FMT_FLOAT, 1, 32, true,
                                               RA_FMT_CAP_LINEAR);

    const struct ra_fmt *fbo_fmt = ra_find_fmt(ra, RA_FMT_FLOAT, 1, 32, true,
                                               RA_FMT_CAP_RENDERABLE);
    if (!src_fmt || !fbo_fmt)
        return;

    float *fbo_data = NULL;
    struct pl_shader_obj *lut = NULL;

    static float data_5x5[5][5] = {
        { 0, 0, 0, 0, 0 },
        { 0, 0, 0, 0, 0 },
        { 0, 0, 1, 0, 0 },
        { 0, 0, 0, 0, 0 },
        { 0, 0, 0, 0, 0 },
    };

    const struct ra_tex *dot5x5 = ra_tex_create(ra, &(struct ra_tex_params) {
        .w              = 5,
        .h              = 5,
        .format         = src_fmt,
        .sampleable     = true,
        .sample_mode    = RA_TEX_SAMPLE_LINEAR,
        .address_mode   = RA_TEX_ADDRESS_CLAMP,
        .initial_data   = &data_5x5[0][0],
    });

    const struct ra_tex *fbo = ra_tex_create(ra, &(struct ra_tex_params) {
        .w              = 100,
        .h              = 100,
        .format         = fbo_fmt,
        .renderable     = true,
        .storable       = !!(fbo_fmt->caps & RA_FMT_CAP_STORABLE),
        .host_readable  = true,
    });

    struct pl_dispatch *dp = pl_dispatch_create(ctx, ra);
    if (!dot5x5 || !fbo || !dp)
        goto error;

    struct pl_shader *sh = pl_dispatch_begin(dp);
    REQUIRE(pl_shader_sample_polar(sh,
        &(struct pl_sample_src) {
            .tex        = dot5x5,
            .new_w      = fbo->params.w,
            .new_h      = fbo->params.h,
        },
        &(struct pl_sample_polar_params) {
            .filter     = pl_filter_ewa_lanczos,
            .lut        = &lut,
        }
    ));
    REQUIRE(pl_dispatch_finish(dp, sh, fbo));

    fbo_data = malloc(fbo->params.w * fbo->params.h * sizeof(float));
    REQUIRE(ra_tex_download(ra, &(struct ra_tex_transfer_params) {
        .tex            = fbo,
        .ptr            = fbo_data,
    }));

    int max = 255;
    printf("P2\n%d %d\n%d\n", fbo->params.w, fbo->params.h, max);
    for (int y = 0; y < fbo->params.h; y++) {
        for (int x = 0; x < fbo->params.w; x++) {
            float v = fbo_data[y * fbo->params.h + x];
            printf("%d ", (int) round(fmin(fmax(v, 0.0), 1.0) * max));
        }
        printf("\n");
    }

error:
    free(fbo_data);
    pl_shader_obj_destroy(&lut);
    pl_dispatch_destroy(&dp);
    ra_tex_destroy(ra, &dot5x5);
    ra_tex_destroy(ra, &fbo);
}

int main()
{
    struct pl_context *ctx = pl_test_context();
    struct pl_vulkan_params params = pl_vulkan_default_params;
    params.debug = true;

    const struct pl_vulkan *vk = pl_vulkan_create(ctx, &params);
    if (!vk)
        return SKIP;

    const struct ra *ra = vk->ra;
    ra_texture_tests(ra);
    shader_tests(ctx, ra);
    scaler_tests(ctx, ra);

    pl_vulkan_destroy(&vk);
    pl_context_destroy(&ctx);
}
