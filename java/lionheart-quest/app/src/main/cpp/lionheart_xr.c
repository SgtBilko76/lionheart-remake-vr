/*
 * Lionheart Remake - Meta Quest OpenXR host.
 *
 * Stage 1: the Java game (LionEngine Android backend) draws every frame with a hardware Canvas into a
 * Surface backed by a SurfaceTexture. This thread owns the GL context and the OpenXR session: it latches
 * the newest game frame (ASurfaceTexture_updateTexImage), copies it into a quad-layer swapchain (the
 * virtual screen) and polls the Touch controllers, which are handed back to Java as a gamepad.
 *
 * OpenXR lifecycle follows QuakeQuest's TBXR_Common.c (loader init, EGL context, session, swapchains).
 *
 * Copyright (C) 2026 - GPL-3.0-or-later (same licence as Lionheart Remake / LionEngine).
 */
#include <jni.h>
#include <pthread.h>
#include <stdbool.h>
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

typedef struct {
    XrSwapchain handle;
    int32_t width, height;
    uint32_t count;
    GLuint *fbos;
    XrSwapchainImageOpenGLESKHR *images;
} Swapchain;

typedef struct {
    /* config (vr.cfg) */
    float screen_distance, screen_width, screen_y;
    float bg_r, bg_g, bg_b;

    /* java */
    JavaVM *vm;
    JNIEnv *env;
    jobject activity;
    jclass bridge;
    jmethodID on_input;
    ASurfaceTexture *surface_texture;
    int game_w, game_h;

    /* thread */
    pthread_t thread;
    volatile bool running;

    /* egl/gl */
    EGLDisplay display;
    EGLConfig config;
    EGLContext context;
    EGLSurface pbuffer;
    GLuint game_tex;
    GLuint prog, prog_u_tex, prog_u_mat;
    GLuint vao, vbo;
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
    Swapchain screen;
    bool screen_has_image;

    /* input */
    XrActionSet action_set;
    XrAction a_a, a_b, a_x, a_y, a_menu, a_lclick, a_rclick;
    XrAction a_ltrig, a_rtrig, a_lgrip, a_rgrip;
    XrAction a_lx, a_ly, a_rx, a_ry, a_lstick, a_rstick;
    XrPath left_hand, right_hand;
    uint32_t buttons;
    int frame_count;
} App;

static App g;

/* ---------------------------------------------------------------- config */

static void load_config(const char *dir)
{
    g.screen_distance = 2.2f;
    g.screen_width = 2.6f;
    g.screen_y = 0.0f;
    g.bg_r = 0.02f;
    g.bg_g = 0.02f;
    g.bg_b = 0.03f;
    if (!dir) return;
    char path[512];
    snprintf(path, sizeof(path), "%s/vr.cfg", dir);
    FILE *f = fopen(path, "r");
    if (!f) {
        LOGI("no %s, using defaults", path);
        return;
    }
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        char key[64];
        float v;
        if (line[0] == '#' || sscanf(line, " %63[^= ] = %f", key, &v) != 2) continue;
        if (!strcmp(key, "screen_distance")) g.screen_distance = v;
        else if (!strcmp(key, "screen_width")) g.screen_width = v;
        else if (!strcmp(key, "screen_y")) g.screen_y = v;
        LOGI("cfg %s = %f", key, v);
    }
    fclose(f);
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

static const char *VS =
    "#version 300 es\n"
    "layout(location = 0) in vec2 aPos;\n"
    "uniform mat4 uMat;\n"
    "out vec2 vUv;\n"
    "void main() {\n"
    "    vec2 uv = aPos * 0.5 + 0.5;\n"
    "    vUv = (uMat * vec4(uv, 0.0, 1.0)).xy;\n"
    "    gl_Position = vec4(aPos, 0.0, 1.0);\n"
    "}\n";

/* The game draws sRGB values; the swapchain is sRGB, so decode here and let the hardware re-encode. */
static const char *FS =
    "#version 300 es\n"
    "#extension GL_OES_EGL_image_external_essl3 : require\n"
    "precision mediump float;\n"
    "uniform samplerExternalOES uTex;\n"
    "uniform float uLinearize;\n"
    "in vec2 vUv;\n"
    "out vec4 oColor;\n"
    "void main() {\n"
    "    vec4 c = texture(uTex, vUv);\n"
    "    vec3 lin = pow(c.rgb, vec3(2.2));\n"
    "    oColor = vec4(mix(c.rgb, lin, uLinearize), 1.0);\n"
    "}\n";

static GLint g_u_linearize;

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
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
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
        LOGE("program link failed");
        return false;
    }
    g.prog_u_tex = glGetUniformLocation(g.prog, "uTex");
    g.prog_u_mat = glGetUniformLocation(g.prog, "uMat");
    g_u_linearize = glGetUniformLocation(g.prog, "uLinearize");

    static const float quad[] = {-1, -1, 1, -1, -1, 1, 1, 1};
    glGenVertexArrays(1, &g.vao);
    glBindVertexArray(g.vao);
    glGenBuffers(1, &g.vbo);
    glBindBuffer(GL_ARRAY_BUFFER, g.vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);
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

    const char *exts[] = {XR_KHR_OPENGL_ES_ENABLE_EXTENSION_NAME, XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME};
    XrInstanceCreateInfoAndroidKHR ia = {XR_TYPE_INSTANCE_CREATE_INFO_ANDROID_KHR};
    ia.applicationVM = g.vm;
    ia.applicationActivity = g.activity;
    XrInstanceCreateInfo ci = {XR_TYPE_INSTANCE_CREATE_INFO};
    ci.next = &ia;
    strcpy(ci.applicationInfo.applicationName, "Lionheart VR");
    ci.applicationInfo.applicationVersion = 1;
    strcpy(ci.applicationInfo.engineName, "LionEngine");
    ci.applicationInfo.engineVersion = 11;
    ci.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 34);
    ci.enabledExtensionCount = 2;
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
    LOGI("swapchain format 0x%llx (srgb=%d), eye %ux%u", (long long) format, g.srgb,
         g.view_cfg[0].recommendedImageRectWidth, g.view_cfg[0].recommendedImageRectHeight);

    for (int i = 0; i < 2; i++) {
        /* The projection layer only carries the dark backdrop in stage 1, so it can stay small. */
        if (!create_swapchain(&g.eye[i], 512, 512, format)) return false;
        g.views[i].type = XR_TYPE_VIEW;
    }
    if (!create_swapchain(&g.screen, g.game_w, g.game_h, format)) return false;
    return input_init();
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

/* ---------------------------------------------------------------- frame loop */

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
                if (XR_SUCCEEDED(xrBeginSession(g.session, &bi))) g.session_running = true;
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

static void draw_screen(void)
{
    ASurfaceTexture_updateTexImage(g.surface_texture);
    float m[16];
    ASurfaceTexture_getTransformMatrix(g.surface_texture, m);

    acquire(&g.screen);
    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glUseProgram(g.prog);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, g.game_tex);
    glUniform1i(g.prog_u_tex, 0);
    glUniformMatrix4fv(g.prog_u_mat, 1, GL_FALSE, m);
    glUniform1f(g_u_linearize, g.srgb ? 1.0f : 0.0f);
    glBindVertexArray(g.vao);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glBindVertexArray(0);
    release(&g.screen);
    g.screen_has_image = true;
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
    XrCompositionLayerProjection proj = {XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    XrCompositionLayerProjectionView pv[2];
    XrCompositionLayerQuad quad = {XR_TYPE_COMPOSITION_LAYER_QUAD};

    if (fs.shouldRender) {
        XrViewLocateInfo li = {XR_TYPE_VIEW_LOCATE_INFO};
        li.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
        li.displayTime = fs.predictedDisplayTime;
        li.space = g.local_space;
        XrViewState vs = {XR_TYPE_VIEW_STATE};
        uint32_t nv = 0;
        xrLocateViews(g.session, &li, &vs, 2, &nv, g.views);

        for (int i = 0; i < 2; i++) {
            acquire(&g.eye[i]);
            glClearColor(g.bg_r, g.bg_g, g.bg_b, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT);
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
        layers[layer_count++] = (const XrCompositionLayerBaseHeader *) &proj;

        draw_screen();
        quad.space = g.local_space;
        quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
        quad.subImage.swapchain = g.screen.handle;
        quad.subImage.imageRect.extent.width = g.screen.width;
        quad.subImage.imageRect.extent.height = g.screen.height;
        quad.pose.orientation.w = 1.0f;
        quad.pose.position.x = 0.0f;
        quad.pose.position.y = g.screen_y;
        quad.pose.position.z = -g.screen_distance;
        quad.size.width = g.screen_width;
        quad.size.height = g.screen_width * (float) g.game_h / (float) g.game_w;
        if (g.screen_has_image) layers[layer_count++] = (const XrCompositionLayerBaseHeader *) &quad;
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
    LOGI("OpenXR ready, game surface %dx%d", g.game_w, g.game_h);
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
    g.game_w = w;
    g.game_h = h;
    const char *dir = files_dir ? (*env)->GetStringUTFChars(env, files_dir, NULL) : NULL;
    load_config(dir);
    if (dir) (*env)->ReleaseStringUTFChars(env, files_dir, dir);
    g.running = true;
    pthread_create(&g.thread, NULL, xr_thread, NULL);
}

JNIEXPORT void JNICALL Java_com_b3dgs_lionheart_quest_XrBridge_nativeStop(JNIEnv *env, jclass cls)
{
    (void) env;
    (void) cls;
    if (!g.running) return;
    if (g.session) xrRequestExitSession(g.session);
    g.running = false;
}
