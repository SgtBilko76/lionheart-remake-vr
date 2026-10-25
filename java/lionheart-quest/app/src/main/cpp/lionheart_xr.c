/*
 * Lionheart Remake - Meta Quest OpenXR host.
 *
 * The Java game (LionEngine Android backend) draws every frame with a hardware Canvas into a Surface backed
 * by a SurfaceTexture. Each frame is an atlas of 640x480 cells: the engine frame buffer (playfield, or the
 * whole picture in menus) and, in levels, one cell per stereo layer (background components, front
 * foreground, HUD), see StereoRouter.java. This thread owns the GL context and the OpenXR session: it latches
 * the newest atlas, renders every layer per eye as a plane at its own depth (each plane subtends the same
 * angle as the original screen, so the centre view matches the 2D game), and polls the Touch controllers,
 * which are handed back to Java as a gamepad.
 *
 * OpenXR lifecycle follows QuakeQuest's TBXR_Common.c (loader init, EGL context, session, swapchains).
 *
 * Copyright (C) 2026 - GPL-3.0-or-later (same licence as Lionheart Remake / LionEngine).
 */
#include <jni.h>
#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <unistd.h>

#include <android/log.h>
#include <android/surface_texture.h>
#include <android/surface_texture_jni.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>

#define XR_USE_PLATFORM_ANDROID
#define XR_USE_GRAPHICS_API_OPENGL_ES
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#define TAG "LionheartXR"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

#define XR_CHECK(call)                                                                   \
    do {                                                                                 \
        XrResult r_ = (call);                                                            \
        if (XR_FAILED(r_)) {                                                             \
            char s_[XR_MAX_RESULT_STRING_SIZE] = {0};                                    \
            if (g.instance) xrResultToString(g.instance, r_, s_);                        \
            LOGE("%s failed: %d %s (line %d)", #call, (int) r_, s_, __LINE__);           \
            return false;                                                                \
        }                                                                                \
    } while (0)

/* Button bits sent to Java (XrBridge.onInput). Keep in sync with XrBridge.java. */
enum {
    BTN_A = 1 << 0, BTN_B = 1 << 1, BTN_X = 1 << 2, BTN_Y = 1 << 3,
    BTN_LTRIG = 1 << 4, BTN_RTRIG = 1 << 5, BTN_LGRIP = 1 << 6, BTN_RGRIP = 1 << 7,
    BTN_MENU = 1 << 8, BTN_LSTICK = 1 << 9, BTN_RSTICK = 1 << 10
};

/* Atlas layout, keep in sync with StereoRouter.java / StereoLayers.java. */
#define CELL_W 640
#define CELL_H 480
#define ATLAS_COLS 2
#define LAYER_BG_COUNT 3
#define LAYER_PLAYFIELD 4
#define LAYER_FOREGROUND 5
#define LAYER_HUD 6
#define MESH_ROWS 32

typedef struct {
    XrSwapchain handle;
    int32_t width, height;
    uint32_t count;
    GLuint *fbos;
    XrSwapchainImageOpenGLESKHR *images;
} Swapchain;

typedef struct {
    int64_t time;
    int mask, kinds, w, h;
} FrameInfo;

#define INFO_RING 32

typedef struct {
    /* config (vr.cfg) */
    float screen_distance, screen_width, screen_y;
    float depth_sky, depth_bg_near, floor_top, floor_bottom, depth_fg, depth_hud;
    float stereo;
    float bg_r, bg_g, bg_b;

    /* java */
    JavaVM *vm;
    JNIEnv *env;
    jobject activity;
    jclass bridge;
    jmethodID on_input;
    ASurfaceTexture *surface_texture;
    int atlas_w, atlas_h;

    /* frame infos posted by Java */
    pthread_mutex_t info_lock;
    FrameInfo ring[INFO_RING];
    int ring_next;
    FrameInfo current;
    bool have_frame;

    /* thread */
    pthread_t thread;
    volatile bool running;

    /* egl/gl */
    EGLDisplay display;
    EGLConfig config;
    EGLContext context;
    EGLSurface pbuffer;
    GLuint game_tex;
    GLuint prog;
    GLint u_mvp, u_screen, u_disp, u_tex, u_st, u_cell, u_atlas, u_linearize;
    GLuint vao, vbo;
    float st_matrix[16];
    bool srgb;

    /* openxr */
    XrInstance instance;
    XrSystemId system;
    XrSession session;
    XrSpace local_space;
    XrSessionState state;
    bool session_running;
    XrViewConfigurationView view_cfg[2];
    XrView views[2];
    Swapchain eye[2];

    /* input */
    XrActionSet action_set;
    XrAction a_a, a_b, a_x, a_y, a_menu, a_lclick, a_rclick;
    XrAction a_ltrig, a_rtrig, a_lgrip, a_rgrip;
    XrAction a_lx, a_ly, a_rx, a_ry, a_lstick, a_rstick;
    uint32_t buttons;
    int frame_count;

    /* passthrough (XR_FB_passthrough), toggled with a left stick click */
    bool pt_supported;
    bool pt_on;
    XrPassthroughFB pt;
    XrPassthroughLayerFB pt_layer;
    PFN_xrCreatePassthroughFB pfnCreatePassthrough;
    PFN_xrPassthroughStartFB pfnPassthroughStart;
    PFN_xrPassthroughPauseFB pfnPassthroughPause;
    PFN_xrCreatePassthroughLayerFB pfnCreatePassthroughLayer;
    PFN_xrPassthroughLayerResumeFB pfnPassthroughLayerResume;
    PFN_xrPassthroughLayerPauseFB pfnPassthroughLayerPause;
    char state_path[512];
    int last_logged_mask;
} App;

static App g;

static void passthrough_init(void);
static void passthrough_set(bool on);

/* ---------------------------------------------------------------- config */

static void load_config(const char *dir)
{
    g.screen_distance = 2.2f;
    g.screen_width = 2.6f;
    g.screen_y = 0.0f;
    g.depth_sky = 40.0f;
    g.depth_bg_near = 7.0f;
    g.floor_top = 14.0f;
    g.floor_bottom = 2.5f;
    g.depth_fg = 1.95f;
    g.depth_hud = 1.8f;
    g.stereo = 1.0f;
    /* Pure black around the game (the swapchain is sRGB: 0.02 linear already shows as dark grey). */
    g.bg_r = 0.0f;
    g.bg_g = 0.0f;
    g.bg_b = 0.0f;
    if (!dir) return;
    snprintf(g.state_path, sizeof(g.state_path), "%s/passthrough.on", dir);
    if (access(g.state_path, F_OK) == 0) g.pt_on = true;
    char path[512];
    snprintf(path, sizeof(path), "%s/vr.cfg", dir);
    FILE *f = fopen(path, "r");
    if (!f) {
        LOGI("no %s, using defaults", path);
        return;
    }
    static const struct {
        const char *key;
        size_t offset;
    } keys[] = {
        {"screen_distance", offsetof(App, screen_distance)}, {"screen_width", offsetof(App, screen_width)},
        {"screen_y", offsetof(App, screen_y)},               {"depth_sky", offsetof(App, depth_sky)},
        {"depth_bg_near", offsetof(App, depth_bg_near)},     {"floor_top", offsetof(App, floor_top)},
        {"floor_bottom", offsetof(App, floor_bottom)},       {"depth_fg", offsetof(App, depth_fg)},
        {"depth_hud", offsetof(App, depth_hud)},             {"stereo", offsetof(App, stereo)},
    };
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        char key[64];
        float v;
        if (line[0] == '#' || sscanf(line, " %63[^= ] = %f", key, &v) != 2) continue;
        for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
            if (!strcmp(key, keys[i].key)) {
                *(float *) ((char *) &g + keys[i].offset) = v;
                LOGI("cfg %s = %f", key, v);
            }
        }
    }
    fclose(f);
}

/* ---------------------------------------------------------------- math */

typedef struct {
    float m[16]; /* column major */
} Mat4;

static Mat4 mat_mul(const Mat4 *a, const Mat4 *b)
{
    Mat4 r;
    for (int c = 0; c < 4; c++)
        for (int rr = 0; rr < 4; rr++) {
            float s = 0;
            for (int k = 0; k < 4; k++) s += a->m[k * 4 + rr] * b->m[c * 4 + k];
            r.m[c * 4 + rr] = s;
        }
    return r;
}

static Mat4 mat_projection(XrFovf fov, float nearz, float farz)
{
    const float l = tanf(fov.angleLeft), r = tanf(fov.angleRight);
    const float d = tanf(fov.angleDown), u = tanf(fov.angleUp);
    Mat4 m;
    memset(&m, 0, sizeof(m));
    m.m[0] = 2.0f / (r - l);
    m.m[5] = 2.0f / (u - d);
    m.m[8] = (r + l) / (r - l);
    m.m[9] = (u + d) / (u - d);
    m.m[10] = -(farz + nearz) / (farz - nearz);
    m.m[11] = -1.0f;
    m.m[14] = -2.0f * farz * nearz / (farz - nearz);
    return m;
}

/* Inverse of the rigid pose transform (world -> eye). */
static Mat4 mat_view(XrPosef p)
{
    const float x = p.orientation.x, y = p.orientation.y, z = p.orientation.z, w = p.orientation.w;
    /* Rotation matrix, element (row, col) stored at r[row * 3 + col]. */
    const float r[9] = {1 - 2 * (y * y + z * z), 2 * (x * y - z * w),     2 * (x * z + y * w),
                        2 * (x * y + z * w),     1 - 2 * (x * x + z * z), 2 * (y * z - x * w),
                        2 * (x * z - y * w),     2 * (y * z + x * w),     1 - 2 * (x * x + y * y)};
    Mat4 m;
    memset(&m, 0, sizeof(m));
    /* Upper 3x3 = R^T: element (row, col) = R(col, row); column-major index col * 4 + row. */
    for (int row = 0; row < 3; row++)
        for (int col = 0; col < 3; col++) m.m[col * 4 + row] = r[col * 3 + row];
    const float tx = p.position.x, ty = p.position.y, tz = p.position.z;
    for (int row = 0; row < 3; row++)
        m.m[12 + row] = -(m.m[0 + row] * tx + m.m[4 + row] * ty + m.m[8 + row] * tz);
    m.m[15] = 1.0f;
    return m;
}

/* ---------------------------------------------------------------- GL */

static GLuint compile(GLenum type, const char *src)
{
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024];
        glGetShaderInfoLog(s, sizeof(log), NULL, log);
        LOGE("shader compile: %s", log);
    }
    return s;
}

/* A layer plane: grid in (u, v) with v = 0 at the top. Its position keeps the angle of the original screen
 * point seen from the origin, pushed out to the layer depth (1/depth interpolated from top to bottom). */
static const char *VS =
    "#version 300 es\n"
    "layout(location = 0) in vec2 aUv;\n"
    "uniform mat4 uMvp;\n"
    "uniform vec4 uScreen;\n" /* width, height, distance, centre y */
    "uniform vec2 uDisp;\n"   /* 1/depth at top, 1/depth at bottom */
    "out vec2 vUv;\n"
    "void main() {\n"
    "    float depth = 1.0 / mix(uDisp.x, uDisp.y, aUv.y);\n"
    "    vec3 p = vec3((aUv.x - 0.5) * uScreen.x, uScreen.w + (0.5 - aUv.y) * uScreen.y, -uScreen.z);\n"
    "    vUv = aUv;\n"
    "    gl_Position = uMvp * vec4(p * (depth / uScreen.z), 1.0);\n"
    "}\n";

/* Sharp bilinear sampling of the pixel art inside the cell, premultiplied alpha output. */
static const char *FS =
    "#version 300 es\n"
    "#extension GL_OES_EGL_image_external_essl3 : require\n"
    "precision highp float;\n"
    "uniform samplerExternalOES uTex;\n"
    "uniform mat4 uSt;\n"
    "uniform vec4 uCell;\n" /* cell x, y (atlas px, top-left origin), content width, height */
    "uniform vec2 uAtlas;\n"
    "uniform float uLinearize;\n"
    "in vec2 vUv;\n"
    "out vec4 oColor;\n"
    "void main() {\n"
    "    vec2 px = vUv * uCell.zw;\n"
    "    vec2 fw = max(fwidth(px), vec2(1e-4));\n"
    "    vec2 p = floor(px) + 0.5 + clamp((fract(px) - 0.5) / fw, -0.5, 0.5);\n"
    "    p = clamp(p, vec2(0.5), uCell.zw - 0.5);\n"
    "    vec2 a = (uCell.xy + p) / uAtlas;\n"
    "    a.y = 1.0 - a.y;\n"
    "    vec4 c = texture(uTex, (uSt * vec4(a, 0.0, 1.0)).xy);\n"
    "    vec3 rgb = c.a > 0.0 ? c.rgb / c.a : vec3(0.0);\n"
    "    rgb = mix(rgb, pow(rgb, vec3(2.2)), uLinearize);\n"
    "    oColor = vec4(rgb * c.a, c.a);\n"
    "}\n";

static bool gl_init(void)
{
    g.display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    eglInitialize(g.display, NULL, NULL);
    const EGLint attribs[] = {EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
                              EGL_DEPTH_SIZE, 0, EGL_STENCIL_SIZE, 0, EGL_SAMPLES, 0,
                              EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT_KHR, EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
                              EGL_NONE};
    EGLint n = 0;
    if (!eglChooseConfig(g.display, attribs, &g.config, 1, &n) || n < 1) {
        LOGE("eglChooseConfig failed");
        return false;
    }
    const EGLint ctx_attribs[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
    g.context = eglCreateContext(g.display, g.config, EGL_NO_CONTEXT, ctx_attribs);
    const EGLint pb_attribs[] = {EGL_WIDTH, 16, EGL_HEIGHT, 16, EGL_NONE};
    g.pbuffer = eglCreatePbufferSurface(g.display, g.config, pb_attribs);
    if (g.context == EGL_NO_CONTEXT || !eglMakeCurrent(g.display, g.pbuffer, g.pbuffer, g.context)) {
        LOGE("EGL context failed 0x%x", eglGetError());
        return false;
    }
    LOGI("GL: %s / %s", glGetString(GL_RENDERER), glGetString(GL_VERSION));

    glGenTextures(1, &g.game_tex);
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, g.game_tex);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    int r = ASurfaceTexture_attachToGLContext(g.surface_texture, g.game_tex);
    if (r != 0) {
        LOGE("ASurfaceTexture_attachToGLContext failed %d", r);
        return false;
    }

    g.prog = glCreateProgram();
    glAttachShader(g.prog, compile(GL_VERTEX_SHADER, VS));
    glAttachShader(g.prog, compile(GL_FRAGMENT_SHADER, FS));
    glLinkProgram(g.prog);
    GLint ok = 0;
    glGetProgramiv(g.prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[1024];
        glGetProgramInfoLog(g.prog, sizeof(log), NULL, log);
        LOGE("program link failed: %s", log);
        return false;
    }
    g.u_mvp = glGetUniformLocation(g.prog, "uMvp");
    g.u_screen = glGetUniformLocation(g.prog, "uScreen");
    g.u_disp = glGetUniformLocation(g.prog, "uDisp");
    g.u_tex = glGetUniformLocation(g.prog, "uTex");
    g.u_st = glGetUniformLocation(g.prog, "uSt");
    g.u_cell = glGetUniformLocation(g.prog, "uCell");
    g.u_atlas = glGetUniformLocation(g.prog, "uAtlas");
    g.u_linearize = glGetUniformLocation(g.prog, "uLinearize");

    /* Triangle strip, two columns, MESH_ROWS rows. */
    float mesh[(MESH_ROWS + 1) * 4];
    for (int i = 0; i <= MESH_ROWS; i++) {
        const float v = (float) i / MESH_ROWS;
        mesh[i * 4 + 0] = 0.0f;
        mesh[i * 4 + 1] = v;
        mesh[i * 4 + 2] = 1.0f;
        mesh[i * 4 + 3] = v;
    }
    glGenVertexArrays(1, &g.vao);
    glBindVertexArray(g.vao);
    glGenBuffers(1, &g.vbo);
    glBindBuffer(GL_ARRAY_BUFFER, g.vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(mesh), mesh, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, 0);
    glBindVertexArray(0);
    return true;
}

/* ---------------------------------------------------------------- OpenXR setup */

static bool create_swapchain(Swapchain *sc, int w, int h, int64_t format)
{
    XrSwapchainCreateInfo ci = {XR_TYPE_SWAPCHAIN_CREATE_INFO};
    ci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
    ci.format = format;
    ci.sampleCount = 1;
    ci.width = w;
    ci.height = h;
    ci.faceCount = 1;
    ci.arraySize = 1;
    ci.mipCount = 1;
    XR_CHECK(xrCreateSwapchain(g.session, &ci, &sc->handle));
    sc->width = w;
    sc->height = h;
    XR_CHECK(xrEnumerateSwapchainImages(sc->handle, 0, &sc->count, NULL));
    sc->images = calloc(sc->count, sizeof(XrSwapchainImageOpenGLESKHR));
    for (uint32_t i = 0; i < sc->count; i++) sc->images[i].type = XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR;
    XR_CHECK(xrEnumerateSwapchainImages(sc->handle, sc->count, &sc->count,
                                        (XrSwapchainImageBaseHeader *) sc->images));
    sc->fbos = calloc(sc->count, sizeof(GLuint));
    glGenFramebuffers((GLsizei) sc->count, sc->fbos);
    for (uint32_t i = 0; i < sc->count; i++) {
        glBindFramebuffer(GL_FRAMEBUFFER, sc->fbos[i]);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, sc->images[i].image, 0);
        GLenum st = glCheckFramebufferStatus(GL_FRAMEBUFFER);
        if (st != GL_FRAMEBUFFER_COMPLETE) LOGE("swapchain fbo incomplete 0x%x", st);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    return true;
}

static XrPath path(const char *s)
{
    XrPath p = XR_NULL_PATH;
    xrStringToPath(g.instance, s, &p);
    return p;
}

static XrAction make_action(XrActionType type, const char *name)
{
    XrActionCreateInfo ci = {XR_TYPE_ACTION_CREATE_INFO};
    ci.actionType = type;
    strncpy(ci.actionName, name, sizeof(ci.actionName) - 1);
    strncpy(ci.localizedActionName, name, sizeof(ci.localizedActionName) - 1);
    XrAction a = XR_NULL_HANDLE;
    if (XR_FAILED(xrCreateAction(g.action_set, &ci, &a))) LOGE("xrCreateAction %s failed", name);
    return a;
}

static bool input_init(void)
{
    XrActionSetCreateInfo asi = {XR_TYPE_ACTION_SET_CREATE_INFO};
    strcpy(asi.actionSetName, "gameplay");
    strcpy(asi.localizedActionSetName, "Gameplay");
    XR_CHECK(xrCreateActionSet(g.instance, &asi, &g.action_set));

    g.a_a = make_action(XR_ACTION_TYPE_BOOLEAN_INPUT, "button_a");
    g.a_b = make_action(XR_ACTION_TYPE_BOOLEAN_INPUT, "button_b");
    g.a_x = make_action(XR_ACTION_TYPE_BOOLEAN_INPUT, "button_x");
    g.a_y = make_action(XR_ACTION_TYPE_BOOLEAN_INPUT, "button_y");
    g.a_menu = make_action(XR_ACTION_TYPE_BOOLEAN_INPUT, "button_menu");
    g.a_lclick = make_action(XR_ACTION_TYPE_BOOLEAN_INPUT, "left_stick_click");
    g.a_rclick = make_action(XR_ACTION_TYPE_BOOLEAN_INPUT, "right_stick_click");
    g.a_ltrig = make_action(XR_ACTION_TYPE_FLOAT_INPUT, "left_trigger");
    g.a_rtrig = make_action(XR_ACTION_TYPE_FLOAT_INPUT, "right_trigger");
    g.a_lgrip = make_action(XR_ACTION_TYPE_FLOAT_INPUT, "left_grip");
    g.a_rgrip = make_action(XR_ACTION_TYPE_FLOAT_INPUT, "right_grip");
    g.a_lx = make_action(XR_ACTION_TYPE_FLOAT_INPUT, "left_stick_x");
    g.a_ly = make_action(XR_ACTION_TYPE_FLOAT_INPUT, "left_stick_y");
    g.a_rx = make_action(XR_ACTION_TYPE_FLOAT_INPUT, "right_stick_x");
    g.a_ry = make_action(XR_ACTION_TYPE_FLOAT_INPUT, "right_stick_y");
    g.a_lstick = make_action(XR_ACTION_TYPE_VECTOR2F_INPUT, "left_stick");
    g.a_rstick = make_action(XR_ACTION_TYPE_VECTOR2F_INPUT, "right_stick");

    const XrActionSuggestedBinding b[] = {
        {g.a_a, path("/user/hand/right/input/a/click")},
        {g.a_b, path("/user/hand/right/input/b/click")},
        {g.a_x, path("/user/hand/left/input/x/click")},
        {g.a_y, path("/user/hand/left/input/y/click")},
        {g.a_menu, path("/user/hand/left/input/menu/click")},
        {g.a_lclick, path("/user/hand/left/input/thumbstick/click")},
        {g.a_rclick, path("/user/hand/right/input/thumbstick/click")},
        {g.a_ltrig, path("/user/hand/left/input/trigger/value")},
        {g.a_rtrig, path("/user/hand/right/input/trigger/value")},
        {g.a_lgrip, path("/user/hand/left/input/squeeze/value")},
        {g.a_rgrip, path("/user/hand/right/input/squeeze/value")},
        {g.a_lx, path("/user/hand/left/input/thumbstick/x")},
        {g.a_ly, path("/user/hand/left/input/thumbstick/y")},
        {g.a_rx, path("/user/hand/right/input/thumbstick/x")},
        {g.a_ry, path("/user/hand/right/input/thumbstick/y")},
        {g.a_lstick, path("/user/hand/left/input/thumbstick")},
        {g.a_rstick, path("/user/hand/right/input/thumbstick")},
    };
    XrInteractionProfileSuggestedBinding sb = {XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
    sb.interactionProfile = path("/interaction_profiles/oculus/touch_controller");
    sb.suggestedBindings = b;
    sb.countSuggestedBindings = sizeof(b) / sizeof(b[0]);
    XR_CHECK(xrSuggestInteractionProfileBindings(g.instance, &sb));

    XrSessionActionSetsAttachInfo ai = {XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
    ai.countActionSets = 1;
    ai.actionSets = &g.action_set;
    XR_CHECK(xrAttachSessionActionSets(g.session, &ai));
    return true;
}

static bool xr_init(void)
{
    PFN_xrInitializeLoaderKHR init_loader = NULL;
    if (XR_SUCCEEDED(xrGetInstanceProcAddr(XR_NULL_HANDLE, "xrInitializeLoaderKHR",
                                           (PFN_xrVoidFunction *) &init_loader)) && init_loader) {
        XrLoaderInitInfoAndroidKHR li = {XR_TYPE_LOADER_INIT_INFO_ANDROID_KHR};
        li.applicationVM = g.vm;
        li.applicationContext = g.activity;
        init_loader((XrLoaderInitInfoBaseHeaderKHR *) &li);
    }

    const char *exts[3] = {XR_KHR_OPENGL_ES_ENABLE_EXTENSION_NAME, XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME,
                           XR_FB_PASSTHROUGH_EXTENSION_NAME};
    uint32_t ext_count = 2;
    {
        uint32_t n_ext = 0;
        xrEnumerateInstanceExtensionProperties(NULL, 0, &n_ext, NULL);
        XrExtensionProperties *props = calloc(n_ext, sizeof(XrExtensionProperties));
        for (uint32_t i = 0; i < n_ext; i++) props[i].type = XR_TYPE_EXTENSION_PROPERTIES;
        xrEnumerateInstanceExtensionProperties(NULL, n_ext, &n_ext, props);
        for (uint32_t i = 0; i < n_ext; i++)
            if (!strcmp(props[i].extensionName, XR_FB_PASSTHROUGH_EXTENSION_NAME)) g.pt_supported = true;
        free(props);
        if (g.pt_supported) ext_count = 3;
        LOGI("XR_FB_passthrough %s", g.pt_supported ? "available" : "not available");
    }
    XrInstanceCreateInfoAndroidKHR ia = {XR_TYPE_INSTANCE_CREATE_INFO_ANDROID_KHR};
    ia.applicationVM = g.vm;
    ia.applicationActivity = g.activity;
    XrInstanceCreateInfo ci = {XR_TYPE_INSTANCE_CREATE_INFO};
    ci.next = &ia;
    strcpy(ci.applicationInfo.applicationName, "Lionheart VR");
    ci.applicationInfo.applicationVersion = 2;
    strcpy(ci.applicationInfo.engineName, "LionEngine");
    ci.applicationInfo.engineVersion = 11;
    ci.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 34);
    ci.enabledExtensionCount = ext_count;
    ci.enabledExtensionNames = exts;
    XR_CHECK(xrCreateInstance(&ci, &g.instance));

    XrSystemGetInfo si = {XR_TYPE_SYSTEM_GET_INFO};
    si.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    XR_CHECK(xrGetSystem(g.instance, &si, &g.system));

    PFN_xrGetOpenGLESGraphicsRequirementsKHR get_req = NULL;
    XR_CHECK(xrGetInstanceProcAddr(g.instance, "xrGetOpenGLESGraphicsRequirementsKHR",
                                   (PFN_xrVoidFunction *) &get_req));
    XrGraphicsRequirementsOpenGLESKHR req = {XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_ES_KHR};
    XR_CHECK(get_req(g.instance, g.system, &req));

    XrGraphicsBindingOpenGLESAndroidKHR gb = {XR_TYPE_GRAPHICS_BINDING_OPENGL_ES_ANDROID_KHR};
    gb.display = g.display;
    gb.config = g.config;
    gb.context = g.context;
    XrSessionCreateInfo sci = {XR_TYPE_SESSION_CREATE_INFO};
    sci.next = &gb;
    sci.systemId = g.system;
    XR_CHECK(xrCreateSession(g.instance, &sci, &g.session));

    XrReferenceSpaceCreateInfo rs = {XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    rs.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    rs.poseInReferenceSpace.orientation.w = 1.0f;
    XR_CHECK(xrCreateReferenceSpace(g.session, &rs, &g.local_space));

    uint32_t n = 0;
    XR_CHECK(xrEnumerateViewConfigurationViews(g.instance, g.system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0,
                                               &n, NULL));
    for (int i = 0; i < 2; i++) g.view_cfg[i].type = XR_TYPE_VIEW_CONFIGURATION_VIEW;
    XR_CHECK(xrEnumerateViewConfigurationViews(g.instance, g.system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 2,
                                               &n, g.view_cfg));

    uint32_t nf = 0;
    XR_CHECK(xrEnumerateSwapchainFormats(g.session, 0, &nf, NULL));
    int64_t *formats = calloc(nf, sizeof(int64_t));
    XR_CHECK(xrEnumerateSwapchainFormats(g.session, nf, &nf, formats));
    int64_t format = GL_RGBA8;
    for (uint32_t i = 0; i < nf; i++) {
        if (formats[i] == GL_SRGB8_ALPHA8) {
            format = GL_SRGB8_ALPHA8;
            break;
        }
    }
    free(formats);
    g.srgb = format == GL_SRGB8_ALPHA8;
    const int ew = (int) g.view_cfg[0].recommendedImageRectWidth;
    const int eh = (int) g.view_cfg[0].recommendedImageRectHeight;
    LOGI("swapchain format 0x%llx (srgb=%d), eye %dx%d", (long long) format, g.srgb, ew, eh);

    for (int i = 0; i < 2; i++) {
        if (!create_swapchain(&g.eye[i], ew, eh, format)) return false;
        g.views[i].type = XR_TYPE_VIEW;
    }
    passthrough_init();
    return input_init();
}

/* ---------------------------------------------------------------- passthrough */

static void passthrough_init(void)
{
    if (!g.pt_supported) return;
#define PT_FN(name, var) xrGetInstanceProcAddr(g.instance, name, (PFN_xrVoidFunction *) &g.var)
    PT_FN("xrCreatePassthroughFB", pfnCreatePassthrough);
    PT_FN("xrPassthroughStartFB", pfnPassthroughStart);
    PT_FN("xrPassthroughPauseFB", pfnPassthroughPause);
    PT_FN("xrCreatePassthroughLayerFB", pfnCreatePassthroughLayer);
    PT_FN("xrPassthroughLayerResumeFB", pfnPassthroughLayerResume);
    PT_FN("xrPassthroughLayerPauseFB", pfnPassthroughLayerPause);
#undef PT_FN
    if (!g.pfnCreatePassthrough || !g.pfnCreatePassthroughLayer) {
        g.pt_supported = false;
        return;
    }
    XrPassthroughCreateInfoFB pci = {XR_TYPE_PASSTHROUGH_CREATE_INFO_FB};
    XrResult r = g.pfnCreatePassthrough(g.session, &pci, &g.pt);
    if (XR_FAILED(r)) {
        LOGE("xrCreatePassthroughFB failed %d", (int) r);
        g.pt_supported = false;
        return;
    }
    XrPassthroughLayerCreateInfoFB lci = {XR_TYPE_PASSTHROUGH_LAYER_CREATE_INFO_FB};
    lci.passthrough = g.pt;
    lci.purpose = XR_PASSTHROUGH_LAYER_PURPOSE_RECONSTRUCTION_FB;
    r = g.pfnCreatePassthroughLayer(g.session, &lci, &g.pt_layer);
    if (XR_FAILED(r)) {
        LOGE("xrCreatePassthroughLayerFB failed %d", (int) r);
        g.pt_supported = false;
        return;
    }
    LOGI("passthrough ready");
}

static void passthrough_set(bool on)
{
    if (!g.pt_supported) {
        g.pt_on = false;
        return;
    }
    if (on) {
        g.pfnPassthroughStart(g.pt);
        g.pfnPassthroughLayerResume(g.pt_layer);
    } else {
        g.pfnPassthroughLayerPause(g.pt_layer);
        g.pfnPassthroughPause(g.pt);
    }
    g.pt_on = on;
    if (g.state_path[0]) {
        if (on) {
            FILE *f = fopen(g.state_path, "w");
            if (f) fclose(f);
        } else {
            unlink(g.state_path);
        }
    }
    LOGI("passthrough %s", on ? "on" : "off");
}

/* ---------------------------------------------------------------- Java callbacks */

static void java_finish(void)
{
    if (!g.env || !g.activity) return;
    jclass cls = (*g.env)->GetObjectClass(g.env, g.activity);
    jmethodID m = (*g.env)->GetMethodID(g.env, cls, "finish", "()V");
    (*g.env)->CallVoidMethod(g.env, g.activity, m);
    (*g.env)->DeleteLocalRef(g.env, cls);
}

/* ---------------------------------------------------------------- input */

static float get_float(XrAction a)
{
    XrActionStateGetInfo gi = {XR_TYPE_ACTION_STATE_GET_INFO};
    gi.action = a;
    XrActionStateFloat s = {XR_TYPE_ACTION_STATE_FLOAT};
    if (XR_FAILED(xrGetActionStateFloat(g.session, &gi, &s)) || !s.isActive) return 0.0f;
    return s.currentState;
}

static bool get_bool(XrAction a)
{
    XrActionStateGetInfo gi = {XR_TYPE_ACTION_STATE_GET_INFO};
    gi.action = a;
    XrActionStateBoolean s = {XR_TYPE_ACTION_STATE_BOOLEAN};
    if (XR_FAILED(xrGetActionStateBoolean(g.session, &gi, &s)) || !s.isActive) return false;
    return s.currentState;
}

static XrVector2f get_vec2(XrAction a)
{
    XrActionStateGetInfo gi = {XR_TYPE_ACTION_STATE_GET_INFO};
    gi.action = a;
    XrActionStateVector2f s = {XR_TYPE_ACTION_STATE_VECTOR2F};
    XrVector2f z = {0, 0};
    if (XR_FAILED(xrGetActionStateVector2f(g.session, &gi, &s)) || !s.isActive) return z;
    return s.currentState;
}

/* Analog trigger/grip to digital with hysteresis. */
static void analog_bit(uint32_t *bits, uint32_t bit, float v)
{
    if (*bits & bit) {
        if (v < 0.25f) *bits &= ~bit;
    } else if (v > 0.45f) {
        *bits |= bit;
    }
}

static void poll_input(void)
{
    if (g.state != XR_SESSION_STATE_FOCUSED) {
        if (g.buttons) {
            g.buttons = 0;
            (*g.env)->CallStaticVoidMethod(g.env, g.bridge, g.on_input, 0, 0.0f, 0.0f, 0.0f, 0.0f);
        }
        return;
    }
    XrActiveActionSet aas = {g.action_set, XR_NULL_PATH};
    XrActionsSyncInfo sync = {XR_TYPE_ACTIONS_SYNC_INFO};
    sync.countActiveActionSets = 1;
    sync.activeActionSets = &aas;
    if (XR_FAILED(xrSyncActions(g.session, &sync))) return;

    uint32_t b = g.buttons & (BTN_LTRIG | BTN_RTRIG | BTN_LGRIP | BTN_RGRIP);
    if (get_bool(g.a_a)) b |= BTN_A;
    if (get_bool(g.a_b)) b |= BTN_B;
    if (get_bool(g.a_x)) b |= BTN_X;
    if (get_bool(g.a_y)) b |= BTN_Y;
    if (get_bool(g.a_menu)) b |= BTN_MENU;
    if (get_bool(g.a_lclick)) b |= BTN_LSTICK;
    if (get_bool(g.a_rclick)) b |= BTN_RSTICK;
    analog_bit(&b, BTN_LTRIG, get_float(g.a_ltrig));
    analog_bit(&b, BTN_RTRIG, get_float(g.a_rtrig));
    analog_bit(&b, BTN_LGRIP, get_float(g.a_lgrip));
    analog_bit(&b, BTN_RGRIP, get_float(g.a_rgrip));

    /* Read the x/y components too: on Quest the vector2f binding alone can stay at 0,0. */
    XrVector2f l = get_vec2(g.a_lstick), r = get_vec2(g.a_rstick);
    float lx = get_float(g.a_lx), ly = get_float(g.a_ly);
    float rx = get_float(g.a_rx), ry = get_float(g.a_ry);
    if (fabsf(lx) < fabsf(l.x)) lx = l.x;
    if (fabsf(ly) < fabsf(l.y)) ly = l.y;
    if (fabsf(rx) < fabsf(r.x)) rx = r.x;
    if (fabsf(ry) < fabsf(r.y)) ry = r.y;

    if (b != g.buttons) LOGI("buttons 0x%x", b);
    if ((b & BTN_LSTICK) && !(g.buttons & BTN_LSTICK)) passthrough_set(!g.pt_on);
    g.buttons = b;
    (*g.env)->CallStaticVoidMethod(g.env, g.bridge, g.on_input, (jint) b, lx, ly, rx, ry);
    if ((*g.env)->ExceptionCheck(g.env)) {
        (*g.env)->ExceptionDescribe(g.env);
        (*g.env)->ExceptionClear(g.env);
    }
}

static void handle_events(void)
{
    XrEventDataBuffer ev = {XR_TYPE_EVENT_DATA_BUFFER};
    while (xrPollEvent(g.instance, &ev) == XR_SUCCESS) {
        switch (ev.type) {
        case XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED: {
            const XrEventDataSessionStateChanged *e = (const XrEventDataSessionStateChanged *) &ev;
            g.state = e->state;
            LOGI("session state %d", (int) g.state);
            if (g.state == XR_SESSION_STATE_READY) {
                XrSessionBeginInfo bi = {XR_TYPE_SESSION_BEGIN_INFO};
                bi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                if (XR_SUCCEEDED(xrBeginSession(g.session, &bi))) {
                    g.session_running = true;
                    if (g.pt_on) passthrough_set(true);
                }
            } else if (g.state == XR_SESSION_STATE_STOPPING) {
                xrEndSession(g.session);
                g.session_running = false;
            } else if (g.state == XR_SESSION_STATE_EXITING || g.state == XR_SESSION_STATE_LOSS_PENDING) {
                g.running = false;
            }
            break;
        }
        case XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING:
            g.running = false;
            break;
        case XR_TYPE_EVENT_DATA_INTERACTION_PROFILE_CHANGED:
            LOGI("xrPollEvent: received XR_TYPE_EVENT_DATA_INTERACTION_PROFILE_CHANGED");
            break;
        default:
            break;
        }
        ev.type = XR_TYPE_EVENT_DATA_BUFFER;
        ev.next = NULL;
    }
}

/* ---------------------------------------------------------------- frame */

static void latch_game_frame(void)
{
    if (ASurfaceTexture_updateTexImage(g.surface_texture) != 0) return;
    ASurfaceTexture_getTransformMatrix(g.surface_texture, g.st_matrix);
    const int64_t ts = ASurfaceTexture_getTimestamp(g.surface_texture);

    /* Newest info posted before the buffer was queued. */
    pthread_mutex_lock(&g.info_lock);
    const FrameInfo *best = NULL;
    for (int i = 0; i < INFO_RING; i++) {
        const FrameInfo *f = &g.ring[i];
        if (f->time == 0 || f->time > ts) continue;
        if (!best || f->time > best->time) best = f;
    }
    if (best && best->mask && best->w > 0 && best->h > 0) {
        g.current = *best;
        g.have_frame = true;
    }
    pthread_mutex_unlock(&g.info_lock);

    if (g.have_frame && g.current.mask != g.last_logged_mask) {
        g.last_logged_mask = g.current.mask;
        LOGI("layers mask 0x%x kinds 0x%x content %dx%d", g.current.mask, g.current.kinds, g.current.w,
             g.current.h);
    }
}

static void layer_disparity(int id, float *top, float *bottom)
{
    const float screen = 1.0f / g.screen_distance;
    *top = *bottom = screen;
    if (g.stereo < 0.5f) return;
    if (id < LAYER_BG_COUNT) {
        int count = 0;
        for (int i = 0; i < LAYER_BG_COUNT; i++)
            if (g.current.mask & (1 << i)) count = i + 1;
        const float t = count > 1 ? (float) id / (float) (count - 1) : 0.0f;
        const float d = (1.0f - t) / g.depth_sky + t / g.depth_bg_near;
        *top = *bottom = d;
        if (g.current.kinds & (1 << id)) {
            *top = 1.0f / g.floor_top;
            *bottom = 1.0f / g.floor_bottom;
        }
    } else if (id == LAYER_FOREGROUND) {
        *top = *bottom = 1.0f / g.depth_fg;
    } else if (id == LAYER_HUD) {
        *top = *bottom = 1.0f / g.depth_hud;
    }
}

static void draw_layers(const Mat4 *mvp)
{
    const float sw = g.screen_width;
    const float sh = sw * (float) g.current.h / (float) g.current.w;

    glUseProgram(g.prog);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, g.game_tex);
    glUniform1i(g.u_tex, 0);
    glUniformMatrix4fv(g.u_st, 1, GL_FALSE, g.st_matrix);
    glUniformMatrix4fv(g.u_mvp, 1, GL_FALSE, mvp->m);
    glUniform4f(g.u_screen, sw, sh, g.screen_distance, g.screen_y);
    glUniform2f(g.u_atlas, (float) g.atlas_w, (float) g.atlas_h);
    glUniform1f(g.u_linearize, g.srgb ? 1.0f : 0.0f);
    glBindVertexArray(g.vao);

    /* Far to near. */
    static const int order[] = {0, 1, 2, LAYER_PLAYFIELD, LAYER_FOREGROUND, LAYER_HUD};
    for (size_t i = 0; i < sizeof(order) / sizeof(order[0]); i++) {
        const int id = order[i];
        if (!(g.current.mask & (1 << id))) continue;
        if (g.pt_on && id < LAYER_BG_COUNT) continue; /* room instead of the game backgrounds */
        float top, bottom;
        layer_disparity(id, &top, &bottom);
        glUniform2f(g.u_disp, top, bottom);
        glUniform4f(g.u_cell, (float) (id % ATLAS_COLS * CELL_W), (float) (id / ATLAS_COLS * CELL_H),
                    (float) g.current.w, (float) g.current.h);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, (MESH_ROWS + 1) * 2);
    }
    glBindVertexArray(0);
}

static uint32_t acquire(Swapchain *sc)
{
    uint32_t idx = 0;
    XrSwapchainImageAcquireInfo ai = {XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    xrAcquireSwapchainImage(sc->handle, &ai, &idx);
    XrSwapchainImageWaitInfo wi = {XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    wi.timeout = XR_INFINITE_DURATION;
    xrWaitSwapchainImage(sc->handle, &wi);
    glBindFramebuffer(GL_FRAMEBUFFER, sc->fbos[idx]);
    glViewport(0, 0, sc->width, sc->height);
    return idx;
}

static void release(Swapchain *sc)
{
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    XrSwapchainImageReleaseInfo ri = {XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    xrReleaseSwapchainImage(sc->handle, &ri);
}

static void render_frame(void)
{
    XrFrameWaitInfo wi = {XR_TYPE_FRAME_WAIT_INFO};
    XrFrameState fs = {XR_TYPE_FRAME_STATE};
    if (XR_FAILED(xrWaitFrame(g.session, &wi, &fs))) return;
    XrFrameBeginInfo bi = {XR_TYPE_FRAME_BEGIN_INFO};
    if (XR_FAILED(xrBeginFrame(g.session, &bi))) return;

    const XrCompositionLayerBaseHeader *layers[2];
    uint32_t layer_count = 0;
    XrCompositionLayerPassthroughFB ptl = {XR_TYPE_COMPOSITION_LAYER_PASSTHROUGH_FB};
    XrCompositionLayerProjection proj = {XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    XrCompositionLayerProjectionView pv[2];

    if (fs.shouldRender) {
        latch_game_frame();

        XrViewLocateInfo li = {XR_TYPE_VIEW_LOCATE_INFO};
        li.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
        li.displayTime = fs.predictedDisplayTime;
        li.space = g.local_space;
        XrViewState vs = {XR_TYPE_VIEW_STATE};
        uint32_t nv = 0;
        xrLocateViews(g.session, &li, &vs, 2, &nv, g.views);

        glDisable(GL_DEPTH_TEST);
        glDisable(GL_CULL_FACE);
        for (int i = 0; i < 2; i++) {
            acquire(&g.eye[i]);
            if (g.pt_on) glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
            else glClearColor(g.bg_r, g.bg_g, g.bg_b, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT);
            if (g.have_frame) {
                const Mat4 p = mat_projection(g.views[i].fov, 0.05f, 200.0f);
                const Mat4 v = mat_view(g.views[i].pose);
                const Mat4 mvp = mat_mul(&p, &v);
                glEnable(GL_BLEND);
                glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
                draw_layers(&mvp);
                glDisable(GL_BLEND);
            }
            release(&g.eye[i]);
            memset(&pv[i], 0, sizeof(pv[i]));
            pv[i].type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;
            pv[i].pose = g.views[i].pose;
            pv[i].fov = g.views[i].fov;
            pv[i].subImage.swapchain = g.eye[i].handle;
            pv[i].subImage.imageRect.extent.width = g.eye[i].width;
            pv[i].subImage.imageRect.extent.height = g.eye[i].height;
        }
        proj.space = g.local_space;
        proj.viewCount = 2;
        proj.views = pv;
        if (g.pt_on) {
            ptl.layerHandle = g.pt_layer;
            layers[layer_count++] = (const XrCompositionLayerBaseHeader *) &ptl;
            /* Eye images are premultiplied: transparent pixels show the room. */
            proj.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
        }
        layers[layer_count++] = (const XrCompositionLayerBaseHeader *) &proj;
    }

    XrFrameEndInfo ei = {XR_TYPE_FRAME_END_INFO};
    ei.displayTime = fs.predictedDisplayTime;
    ei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    ei.layerCount = layer_count;
    ei.layers = layers;
    xrEndFrame(g.session, &ei);

    if ((++g.frame_count % 720) == 0) LOGI("frames %d state %d", g.frame_count, (int) g.state);
}

static void *xr_thread(void *arg)
{
    (void) arg;
    (*g.vm)->AttachCurrentThread(g.vm, &g.env, NULL);
    if (!gl_init() || !xr_init()) {
        LOGE("VR init failed, closing");
        java_finish();
        (*g.vm)->DetachCurrentThread(g.vm);
        return NULL;
    }
    LOGI("OpenXR ready, atlas %dx%d, stereo %.0f", g.atlas_w, g.atlas_h, g.stereo);
    while (g.running) {
        handle_events();
        if (!g.session_running) {
            usleep(10000);
            continue;
        }
        poll_input();
        render_frame();
    }
    LOGI("XR thread exit");
    if (g.session) xrDestroySession(g.session);
    if (g.instance) xrDestroyInstance(g.instance);
    g.session = XR_NULL_HANDLE;
    g.instance = XR_NULL_HANDLE;
    java_finish();
    (*g.vm)->DetachCurrentThread(g.vm);
    return NULL;
}

/* ---------------------------------------------------------------- JNI */

JNIEXPORT jint JNI_OnLoad(JavaVM *vm, void *reserved)
{
    (void) reserved;
    g.vm = vm;
    pthread_mutex_init(&g.info_lock, NULL);
    return JNI_VERSION_1_6;
}

JNIEXPORT void JNICALL Java_com_b3dgs_lionheart_quest_XrBridge_nativeStart(JNIEnv *env, jclass cls, jobject activity,
                                                                          jobject surface_texture, jint w, jint h,
                                                                          jstring files_dir)
{
    if (g.running) return;
    g.activity = (*env)->NewGlobalRef(env, activity);
    g.bridge = (jclass) (*env)->NewGlobalRef(env, cls);
    g.on_input = (*env)->GetStaticMethodID(env, cls, "onInput", "(IFFFF)V");
    g.surface_texture = ASurfaceTexture_fromSurfaceTexture(env, surface_texture);
    g.atlas_w = w;
    g.atlas_h = h;
    const char *dir = files_dir ? (*env)->GetStringUTFChars(env, files_dir, NULL) : NULL;
    load_config(dir);
    if (dir) (*env)->ReleaseStringUTFChars(env, files_dir, dir);
    g.running = true;
    pthread_create(&g.thread, NULL, xr_thread, NULL);
}

JNIEXPORT void JNICALL Java_com_b3dgs_lionheart_quest_XrBridge_nativeFrameInfo(JNIEnv *env, jclass cls, jlong time,
                                                                              jint mask, jint kinds, jint w, jint h)
{
    (void) env;
    (void) cls;
    pthread_mutex_lock(&g.info_lock);
    FrameInfo *f = &g.ring[g.ring_next];
    f->time = time;
    f->mask = mask;
    f->kinds = kinds;
    f->w = w;
    f->h = h;
    g.ring_next = (g.ring_next + 1) % INFO_RING;
    pthread_mutex_unlock(&g.info_lock);
}

JNIEXPORT void JNICALL Java_com_b3dgs_lionheart_quest_XrBridge_nativeStop(JNIEnv *env, jclass cls)
{
    (void) env;
    (void) cls;
    if (!g.running) return;
    if (g.session) xrRequestExitSession(g.session);
    g.running = false;
}
