/**
 * FFmpeg filter for applying GLSL transitions between video streams.
 *
 * @see https://gl-transitions.com/
 */
#include <stdio.h>
#include "string.h"
#include "libavutil/opt.h"
#include "internal.h"
#include "framesync.h"
#include <emscripten/threading.h>

#ifndef __APPLE__
// # define GL_TRANSITION_USING_EGL //remove this line if you don't want to use EGL
#endif

#ifdef __APPLE__
# define __gl_h_
# define GL_DO_NOT_WARN_IF_MULTI_GL_VERSION_HEADERS_INCLUDED
# include <OpenGL/gl3.h>
#else
# include <GL/glew.h>
#endif

#ifdef GL_TRANSITION_USING_EGL
# include <EGL/egl.h>
# include <EGL/eglext.h>
#else
# include <GLFW/glfw3.h>
#include <GLES3/gl3.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <float.h>

#define FROM (0)
#define TO   (1)

#define PIXEL_FORMAT (GL_RGB)

#ifdef GL_TRANSITION_USING_EGL
static const EGLint configAttribs[] = {
        EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
        EGL_BLUE_SIZE, 8,
        EGL_GREEN_SIZE, 8,
        EGL_RED_SIZE, 8,
        EGL_DEPTH_SIZE, 8,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
        EGL_NONE};
#endif
static const float position[12] = {
        -1.0f, -1.0f, 1.0f, -1.0f, -1.0f, 1.0f, -1.0f, 1.0f, 1.0f, -1.0f, 1.0f, 1.0f
};


static const GLchar *v_shader_source =
        "attribute vec2 position;\n"
        "varying vec2 _uv;\n"
        "void main(void) {\n"
        "  gl_Position = vec4(position, 0, 1);\n"
        "  vec2 uv = position * 0.5 + 0.5;\n"
        "  _uv = vec2(uv.x, 1.0 - uv.y);\n"
        "}\n";

static const GLchar *f_shader_template =
        "precision highp float;\n"
        "varying vec2 _uv;\n"
        "uniform sampler2D from;\n"
        "uniform sampler2D to;\n"
        "uniform float progress;\n"
        "uniform float ratio;\n"
        "uniform float _fromR;\n"
        "uniform float _toR;\n"
        "\n"
        "vec4 getFromColor(vec2 uv) {\n"
        "  return texture2D(from, vec2(uv.x, 1.0 - uv.y));\n"
        "}\n"
        "\n"
        "vec4 getToColor(vec2 uv) {\n"
        "  return texture2D(to, vec2(uv.x, 1.0 - uv.y));\n"
        "}\n"
        "\n"
        "\n%s\n"
        "void main() {\n"
        "  gl_FragColor = transition(_uv);\n"
        "}\n";

// default to a basic fade effect
static const GLchar *f_default_transition_source =
        "vec4 transition (vec2 uv) {\n"
        "  return mix(\n"
        "    getFromColor(uv),\n"
        "    getToColor(uv),\n"
        "    progress\n"
        "  );\n"
        "}\n";

typedef struct {
    const AVClass *class;
    FFFrameSync fs;

    // input options
    double duration;
    double offset;
    char *source;

    // timestamp of the first frame in the output, in the timebase units
    int64_t first_pts;

    //fbo
    GLuint  _fbo;
    GLuint  _color;

    // uniforms
    GLuint        from;
    GLuint        to;
    GLint         progress;
    GLint         ratio;
    GLint         _fromR;
    GLint         _toR;

    // internal state
    GLuint        posBuf;
    GLuint        program;
#ifdef GL_TRANSITION_USING_EGL
    EGLDisplay eglDpy;
    EGLConfig eglCfg;
    EGLSurface eglSurf;
    EGLContext eglCtx;
#else
    GLFWwindow    *window;
#endif

    GLchar *f_shader_source;
} GLTransitionContext;
int frameNum = 0;
#define OFFSET(x) offsetof(GLTransitionContext, x)
#define FLAGS AV_OPT_FLAG_FILTERING_PARAM|AV_OPT_FLAG_VIDEO_PARAM

static const AVOption gltransition_options[] = {
        { "duration", "transition duration in seconds", OFFSET(duration), AV_OPT_TYPE_DOUBLE, {.dbl=1.0}, 0, DBL_MAX, FLAGS },
        { "offset", "delay before startingtransition in seconds", OFFSET(offset), AV_OPT_TYPE_DOUBLE, {.dbl=0.0}, 0, DBL_MAX, FLAGS },
        { "source", "path to the gl-transition source file (defaults to basic fade)", OFFSET(source), AV_OPT_TYPE_STRING, {.str = NULL}, CHAR_MIN, CHAR_MAX, FLAGS },
        { "window", "path to the gl-transition window pointer (defaults to basic fade)", OFFSET(window), AV_OPT_TYPE_INT64, {.i64 = NULL}, LLONG_MIN, LLONG_MAX, FLAGS },
        {NULL}
};

FRAMESYNC_DEFINE_CLASS(gltransition, GLTransitionContext, fs);

static GLuint build_shader(AVFilterContext *ctx, const GLchar *shader_source, GLenum type)
{
    GLuint shader = glCreateShader(type);
    if (!shader || !glIsShader(shader)) {
        return 0;
    }

    glShaderSource(shader, 1, &shader_source, 0);
    glCompileShader(shader);

    GLint status;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &status);
    int infoLength, charsWritten;
    char *infoLog;
    glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &infoLength);
    if(infoLength > 0)
    {
        // infoLog = malloc(infoLength);
        // glGetShaderInfoLog(shader, infoLength, &charsWritten, infoLog);
        // if (charsWritten > 0)
        //     printf("Shader info : %s\n" ,infoLog);
        // free(infoLog);
    }
    return (status == GL_TRUE ? shader : 0);
}

static int build_program(AVFilterContext *ctx)
{
    GLuint v_shader, f_shader;
    GLTransitionContext *c = ctx->priv;

    if (!(v_shader = build_shader(ctx, v_shader_source, GL_VERTEX_SHADER))) {
        av_log(ctx, AV_LOG_ERROR, "invalid vertex shader\n");
        return -1;
    }

    char *source = NULL;

    if (c->source) {
        FILE *f = fopen(c->source, "rb");
        av_log(ctx, AV_LOG_INFO, "gl_transition %s", c->source);
        if (!f) {
            av_log(ctx, AV_LOG_ERROR, "invalid transition source file \"%s\"\n", c->source);
            return -1;
        }

        fseek(f, 0, SEEK_END);
        unsigned long fsize = ftell(f);
        fseek(f, 0, SEEK_SET);

        source = malloc(fsize + 1);
        fread(source, fsize, 1, f);
        fclose(f);

        source[fsize] = 0;
    }

    const char *transition_source = source ? source : f_default_transition_source;

    int len = strlen(f_shader_template) + strlen(transition_source);
    c->f_shader_source = av_calloc(len, sizeof(*c->f_shader_source));
    if (!c->f_shader_source) {
        return AVERROR(ENOMEM);
    }

    snprintf(c->f_shader_source, len * sizeof(*c->f_shader_source), f_shader_template, transition_source);
    av_log(ctx, AV_LOG_DEBUG, "\n%s\n", c->f_shader_source);

    if (source) {
        free(source);
        source = NULL;
    }
    // printf("c->f_shader_source = %s", c->f_shader_source);
    if (!(f_shader = build_shader(ctx, c->f_shader_source, GL_FRAGMENT_SHADER))) {
        av_log(ctx, AV_LOG_ERROR, "invalid fragment shader\n");
        return -1;
    }


    c->program = glCreateProgram();
    glAttachShader(c->program, v_shader);
    glAttachShader(c->program, f_shader);
    glLinkProgram(c->program);

    GLint status;
    glGetProgramiv(c->program, GL_LINK_STATUS, &status);

    return status == GL_TRUE ? 0 : -1;
}

static void setup_fbo(AVFilterLink *fromLink)
{
    AVFilterContext     *ctx = fromLink->dst;
    av_log(ctx, AV_LOG_ERROR, "setup_fbo enter 256 fromLink w:%d, h:%d",fromLink->w, fromLink->h);
    GLTransitionContext *c = ctx->priv;
    glGenTextures(1, &c->_color);
    glBindTexture(GL_TEXTURE_2D, c->_color);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);

    glTexImage2D(GL_TEXTURE_2D, 0, PIXEL_FORMAT, fromLink->w , fromLink->h, 0, PIXEL_FORMAT, GL_UNSIGNED_BYTE, 0);

    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);


    glGenFramebuffers(1, &c->_fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, c->_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, c->_color, 0);
    GLenum buffers[] = { GL_COLOR_ATTACHMENT0 };
    //    gl_utils::checkGLError("FBO::FBO 4");

    glDrawBuffers(1, buffers);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    int error = glGetError();
    av_log(ctx, AV_LOG_ERROR,"setup_fbo exit error:%d, fbo:%d, color:%d", error, c->_fbo, c->_color);
}

static void setup_vbo(GLTransitionContext *c)
{
    glGenBuffers(1, &c->posBuf);
    glBindBuffer(GL_ARRAY_BUFFER, c->posBuf);
    glBufferData(GL_ARRAY_BUFFER, sizeof(position), position, GL_STATIC_DRAW);

    GLint loc = glGetAttribLocation(c->program, "position");
    glEnableVertexAttribArray(loc);
    glVertexAttribPointer(loc, 2, GL_FLOAT, GL_FALSE, 0, 0);
}

static void setup_tex(AVFilterLink *fromLink)
{
    AVFilterContext     *ctx = fromLink->dst;
    GLTransitionContext *c = ctx->priv;

    { // from
        glGenTextures(1, &c->from);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, c->from);

        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);

        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, fromLink->w, fromLink->h, 0, PIXEL_FORMAT, GL_UNSIGNED_BYTE, NULL);

        glUniform1i(glGetUniformLocation(c->program, "from"), 0);
    }

    { // to
        glGenTextures(1, &c->to);
        glActiveTexture(GL_TEXTURE0 + 1);
        glBindTexture(GL_TEXTURE_2D, c->to);

        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);

        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, fromLink->w, fromLink->h, 0, PIXEL_FORMAT, GL_UNSIGNED_BYTE, NULL);

        glUniform1i(glGetUniformLocation(c->program, "to"), 1);
    }
}

static void setup_uniforms(AVFilterLink *fromLink)
{
    AVFilterContext     *ctx = fromLink->dst;
    GLTransitionContext *c = ctx->priv;

    c->progress = glGetUniformLocation(c->program, "progress");
    glUniform1f(c->progress, 0.0f);

    // TODO: this should be output ratio
    c->ratio = glGetUniformLocation(c->program, "ratio");
    glUniform1f(c->ratio, fromLink->w / (float)fromLink->h);

    c->_fromR = glGetUniformLocation(c->program, "_fromR");
    glUniform1f(c->_fromR, fromLink->w / (float)fromLink->h);

    // TODO: initialize this in config_props for "to" input
    c->_toR = glGetUniformLocation(c->program, "_toR");
    glUniform1f(c->_toR, fromLink->w / (float)fromLink->h);
}

static int setup_gl(AVFilterLink *inLink)
{
    AVFilterContext *ctx = inLink->dst;
    GLTransitionContext *c = ctx->priv;


#ifdef GL_TRANSITION_USING_EGL
    //init EGL
    // 1. Initialize EGL
    // c->eglDpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);

  #define MAX_DEVICES 4
  EGLDeviceEXT eglDevs[MAX_DEVICES];
  EGLint numDevices;

  PFNEGLQUERYDEVICESEXTPROC eglQueryDevicesEXT =(PFNEGLQUERYDEVICESEXTPROC)
          eglGetProcAddress("eglQueryDevicesEXT");

  eglQueryDevicesEXT(MAX_DEVICES, eglDevs, &numDevices);

  PFNEGLGETPLATFORMDISPLAYEXTPROC eglGetPlatformDisplayEXT =  (PFNEGLGETPLATFORMDISPLAYEXTPROC)
          eglGetProcAddress("eglGetPlatformDisplayEXT");

  c->eglDpy = eglGetPlatformDisplayEXT(EGL_PLATFORM_DEVICE_EXT, eglDevs[0], 0);

  EGLint major, minor;
  eglInitialize(c->eglDpy, &major, &minor);
  av_log(ctx, AV_LOG_DEBUG, "%d%d", major, minor);
  // 2. Select an appropriate configuration
  EGLint numConfigs;
  EGLint pbufferAttribs[] = {
          EGL_WIDTH,
          inLink->w,
          EGL_HEIGHT,
          inLink->h,
          EGL_NONE,
          };
  eglChooseConfig(c->eglDpy, configAttribs, &c->eglCfg, 1, &numConfigs);
  // 3. Create a surface
  c->eglSurf = eglCreatePbufferSurface(c->eglDpy, c->eglCfg,
                                       pbufferAttribs);
  // 4. Bind the API
  eglBindAPI(EGL_OPENGL_API);
  // 5. Create a context and make it current
  c->eglCtx = eglCreateContext(c->eglDpy, c->eglCfg, EGL_NO_CONTEXT, NULL);
  eglMakeCurrent(c->eglDpy, c->eglSurf, c->eglSurf, c->eglCtx);
#else
  if (c->window == NULL) {
      c->window = glfwCreateWindow(inLink->w, inLink->h, "", NULL, NULL);
      av_log(ctx, AV_LOG_ERROR, "glfwCreateWindow called");
  } else {
      av_log(ctx, AV_LOG_ERROR, "glfwCreateWindow not call c->window:%p \n", c->window);
  }
  if (!c->window) {
      av_log(ctx, AV_LOG_ERROR, "setup_gl ERROR");
      return -1;
  }
//   glfwMakeContextCurrent(c->window);

emscripten_webgl_make_context_current(c->window);
#endif

//#ifndef __APPLE__
//  glewExperimental = GL_TRUE;
//  glewInit();
//  av_log(ctx, AV_LOG_INFO, "glewInit called");
//#endif
  glViewport(0, 0, inLink->w, inLink->h);

  int ret;
  if((ret = build_program(ctx)) < 0) {
      return ret;
  }


  glUseProgram(c->program);
  setup_vbo(c);
  setup_uniforms(inLink);

  setup_tex(inLink);
  setup_fbo(inLink);

  emscripten_webgl_make_context_current(0);
  return 0;
}

static AVFrame *apply_transition(FFFrameSync *fs,
                                 AVFilterContext *ctx,
                                 AVFrame *fromFrame,
                                 const AVFrame *toFrame)
                                 {

    GLTransitionContext *c = ctx->priv;
    AVFilterLink *fromLink = ctx->inputs[FROM];
    AVFilterLink *toLink = ctx->inputs[TO];
    AVFilterLink *outLink = ctx->outputs[0];
    AVFrame *outFrame;

    outFrame = ff_get_video_buffer(outLink, outLink->w, outLink->h);
    if (!outFrame) {
        return NULL;
    }

    av_frame_copy_props(outFrame, fromFrame);

#ifdef GL_TRANSITION_USING_EGL
    eglMakeCurrent(c->eglDpy, c->eglSurf, c->eglSurf, c->eglCtx);
#else
//    glfwMakeContextCurrent(c->window);
emscripten_webgl_make_context_current(c->window);
#endif

    glBindFramebuffer(GL_FRAMEBUFFER, c->_fbo);
    glUseProgram(c->program);

    const float ts = ((fs->pts - c->first_pts) / (float)fs->time_base.den) - c->offset;
    const float progress = FFMAX(0.0f, FFMIN(1.0f, ts / c->duration));
    frameNum++;
    av_log(ctx, AV_LOG_ERROR, "frameNum:%d, from pts:%llu, to pts:%llu, transition '%s' %llu %f %f\n",frameNum,fromFrame->pts, toFrame->pts, c->source, fs->pts - c->first_pts, ts, progress);
    glUniform1f(c->progress, progress);
    int unpackBits = 0;
    glGetIntegerv(GL_UNPACK_ALIGNMENT, &unpackBits);
    av_log(ctx, AV_LOG_ERROR, "GL_UNPACK_ALIGNMENT 111 packBits :%d \n", unpackBits);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, c->from);
//    glPixelStorei(GL_UNPACK_ROW_LENGTH, 480);

#ifdef defined(GL_UNPACK_ROW_LENGTH) && !defined(__EMSCRIPTEN__)
    av_log(ctx, AV_LOG_ERROR, "GL_UNPACK_ALIGNMENT 666 \n");
    glPixelStorei(GL_UNPACK_ROW_LENGTH, fromFrame->linesize[0] / 3);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, fromLink->w, fromLink->h, 0, PIXEL_FORMAT, GL_UNSIGNED_BYTE, fromFrame->data[0]);
#else
    if(fromFrame->linesize[0] / 3 == fromLink->w){
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, fromLink->w, fromLink->h, 0, PIXEL_FORMAT, GL_UNSIGNED_BYTE, fromFrame->data[0]);
    }else{
//        int linesize = fromFrame->linesize[0] / 3;
//        av_log(ctx, AV_LOG_ERROR, "from glTexImage2D  480===%d,%d\n",linesize,fromLink->w);
//        unsigned  char* ptrdst = malloc(fromLink->w*fromLink->h*3);
//        for(int i =0 ; i <fromLink->h ; i++){
//            memcpy(ptrdst+fromLink->w*3*i,(fromFrame->data[0])+linesize*i*3,fromLink->w*3);
//        }
//        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, fromLink->w, fromLink->h, 0, PIXEL_FORMAT, GL_UNSIGNED_BYTE, ptrdst);
//        free(ptrdst);

            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, fromLink->w, fromLink->h, 0, PIXEL_FORMAT, GL_UNSIGNED_BYTE, NULL);
            av_log(ctx, AV_LOG_ERROR, "glTexImage2D  480 fromLink-w:%d, h:%d, linesize[0]:%d \n",  fromLink->w, fromLink->h,fromFrame->linesize[0]);
            for( int y = 0; y < fromLink->h; y++)
            {
                char *row = fromFrame->data[0] + ((y + 0)*fromFrame->linesize[0]  + 0);
                glTexSubImage2D(GL_TEXTURE_2D, 0, 0, y, fromLink->w, 1, GL_RGB, GL_UNSIGNED_BYTE, row);
            }
    }


#endif

   // glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, (fromFrame->linesize[0]) / 3, fromLink->h, 0, PIXEL_FORMAT, GL_UNSIGNED_BYTE, fromFrame->data[0]);

    av_log(ctx, AV_LOG_ERROR, "glTexImage2D  480 fromLink-w:%d, h:%d, linesize[0]:%d \n",  fromLink->w, fromLink->h,fromFrame->linesize[0]);

    glActiveTexture(GL_TEXTURE0 + 1);
    glBindTexture(GL_TEXTURE_2D, c->to);

#ifdef defined(GL_UNPACK_ROW_LENGTH) && !defined(__EMSCRIPTEN__)
    glPixelStorei(GL_UNPACK_ROW_LENGTH, toFrame->linesize[0] / 3);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, toLink->w, toLink->h, 0, PIXEL_FORMAT, GL_UNSIGNED_BYTE, toFrame->data[0]);
#else
    if(toFrame->linesize[0] / 3 == toLink->w){
        av_log(ctx, AV_LOG_ERROR, "glTexImage2D  480===\n");
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, toLink->w, toLink->h, 0, PIXEL_FORMAT, GL_UNSIGNED_BYTE, toFrame->data[0]);
    }else{
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, toLink->w, toLink->h, 0, PIXEL_FORMAT, GL_UNSIGNED_BYTE, NULL);
        av_log(ctx, AV_LOG_ERROR, "glTexImage2D 222  toLink-w:%d, h:%d, linesize[0]:%d \n",  toLink->w, toLink->h,toFrame->linesize[0]);
        for( int y = 0; y < toLink->h; y++)
        {
            char *row = toFrame->data[0] + ((y + 0)*toFrame->linesize[0]  + 0);
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, y, toLink->w, 1, GL_RGB, GL_UNSIGNED_BYTE, row);
        }
//        av_log(ctx, AV_LOG_ERROR, "unglTexImage2D  480===\n");
//        int linesize =  toFrame->linesize[0]/ 3;
//        int toLinkw = toLink->w;
//        unsigned char* ptrdst = malloc(toLinkw*toLink->h*3);
//
//        av_log(ctx, AV_LOG_ERROR, "to glTexImage2D  480===%d,%d\n",linesize,toLink->w);
//
//        for(int i =0 ; i <toLink->h ; i++){
//            memcpy(ptrdst+toLinkw*3*i,(toFrame->data[0])+linesize*i*3,toLinkw*3);
//        }
//        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, toLinkw, toLink->h, 0, PIXEL_FORMAT, GL_UNSIGNED_BYTE, ptrdst);
//        free(ptrdst);
    }

#endif
 //   glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, (toFrame->linesize[0]) / 3, toLink->h, 0, PIXEL_FORMAT, GL_UNSIGNED_BYTE, toFrame->data[0]);

    av_log(ctx, AV_LOG_ERROR, "glTexImage2D 222  toLink-w:%d, h:%d, linesize[0]:%d \n",  toLink->w, toLink->h,toFrame->linesize[0]);

    glDrawArrays(GL_TRIANGLES, 0, 6);
    int errorRet = glGetError();
    av_log(ctx, AV_LOG_ERROR, "glReadPixel error: %d, outLink-w:%d, h:%d, toLink w:%d, h:%d,linesize[0]:%d \n", errorRet, outLink->w, outLink->h, toLink->w, toLink->h,outFrame->linesize[0]);
    glGetIntegerv(GL_UNPACK_ALIGNMENT, &unpackBits);
    av_log(ctx, AV_LOG_ERROR, "GL_UNPACK_ALIGNMENT 222 packBits :%d \n", unpackBits);

    int packBits = 0;
    glGetIntegerv(GL_PACK_ALIGNMENT, &packBits);
    av_log(ctx, AV_LOG_ERROR, "GL_PACK_ALIGNMENT packBits11111 :%d \n", packBits);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);

#ifdef  defined(GL_PACK_ROW_LENGTH) && !defined(__EMSCRIPTEN__)
    av_log(ctx, AV_LOG_ERROR, "GL_UNPACK_ALIGNMENT 666888 \n");
    glPixelStorei(GL_PACK_ROW_LENGTH, outFrame->linesize[0] / 3);
#else
    av_log(ctx, AV_LOG_ERROR, "GL_UNPACK_ALIGNMENT 666877 \n");
#endif

    glReadPixels(0, 0, (outFrame->linesize[0] / 3), outLink->h, PIXEL_FORMAT, GL_UNSIGNED_BYTE, (GLvoid *)outFrame->data[0]);

#ifdef defined(GL_PACK_ROW_LENGTH) && !defined(__EMSCRIPTEN__)
    av_log(ctx, AV_LOG_ERROR, "GL_UNPACK_ALIGNMENT 666888 \n");
    glPixelStorei(GL_PACK_ROW_LENGTH, 0);
#else
    av_log(ctx, AV_LOG_ERROR, "GL_UNPACK_ALIGNMENT 666877 \n");
#endif

    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    av_log(ctx, AV_LOG_ERROR, "GL_UNPACK_ALIGNMENT 222 glGet error:%d", glGetError());
    glGetIntegerv(GL_UNPACK_ALIGNMENT, &unpackBits);

    av_log(ctx, AV_LOG_ERROR, "GL_UNPACK_ALIGNMENT 111 packBits :%d", packBits);

#ifdef defined(GL_UNPACK_ROW_LENGTH) && !defined(__EMSCRIPTEN__)
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
#endif

    av_log(ctx, AV_LOG_ERROR, "GL_UNPACK_ALIGNMENT 444 glGet error:%d", glGetError());

    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    av_frame_free(&fromFrame);
    emscripten_webgl_make_context_current(0);
    return outFrame;
}

 static int blend_frame(FFFrameSync *fs)
 {
    AVFilterContext *ctx = fs->parent;
    GLTransitionContext *c = ctx->priv;

    AVFrame *fromFrame, *toFrame, *outFrame;
    int ret;

    ret = ff_framesync_dualinput_get(fs, &fromFrame, &toFrame);
    if (ret < 0) {
        return ret;
    }

    if (c->first_pts == AV_NOPTS_VALUE && fromFrame && fromFrame->pts != AV_NOPTS_VALUE) {
        c->first_pts = fromFrame->pts;
    }

    if (!toFrame) {
        return ff_filter_frame(ctx->outputs[0], fromFrame);
    }

    outFrame = apply_transition(fs, ctx, fromFrame, toFrame);
    if (!outFrame) {
        return AVERROR(ENOMEM);
    }

    return ff_filter_frame(ctx->outputs[0], outFrame);
                                 }

 static av_cold int init(AVFilterContext *ctx)
 {
    GLTransitionContext *c = ctx->priv;
    c->fs.on_event = blend_frame;
    c->first_pts = AV_NOPTS_VALUE;


    // av_log_set_level(AV_LOG_ERROR);
#ifndef GL_TRANSITION_USING_EGL
// if (!glfwInit())
// {
//   return -1;
// }
#endif

return 0;
}

static av_cold void uninit(AVFilterContext *ctx) {
    GLTransitionContext *c = ctx->priv;
    ff_framesync_uninit(&c->fs);

#ifdef GL_TRANSITION_USING_EGL
    if (c->eglDpy) {
        glDeleteTextures(1, &c->from);
        glDeleteTextures(1, &c->to);
        glDeleteBuffers(1, &c->posBuf);
        glDeleteProgram(c->program);
        eglTerminate(c->eglDpy);
    }
#else
if (c->window) {
        emscripten_webgl_make_context_current(c->window);
        glDeleteTextures(1, &c->_color);
        glDeleteFramebuffers(1, &c->_fbo);
        glDeleteTextures(1, &c->from);
        glDeleteTextures(1, &c->to);
        glDeleteBuffers(1, &c->posBuf);
        glDeleteProgram(c->program);
        //  glfwDestroyWindow(c->window);
        emscripten_webgl_make_context_current(0);

    }

#endif

if (c->f_shader_source) {
        av_freep(&c->f_shader_source);
    }
}

static int query_formats(AVFilterContext *ctx)
{
    static const enum AVPixelFormat formats[] = {
            AV_PIX_FMT_RGB24,
            AV_PIX_FMT_NONE
    };

    return ff_set_common_formats(ctx, ff_make_format_list(formats));
}

static int activate(AVFilterContext *ctx)
{
    GLTransitionContext *c = ctx->priv;
    return ff_framesync_activate(&c->fs);
}

static int config_output(AVFilterLink *outLink)
{
    AVFilterContext *ctx = outLink->src;
    GLTransitionContext *c = ctx->priv;
    AVFilterLink *fromLink = ctx->inputs[FROM];
    AVFilterLink *toLink = ctx->inputs[TO];
    int ret;

    if (fromLink->format != toLink->format) {
        av_log(ctx, AV_LOG_ERROR, "inputs must be of same pixel format\n");
        return AVERROR(EINVAL);
    }

    if (fromLink->w != toLink->w || fromLink->h != toLink->h) {
        av_log(ctx, AV_LOG_ERROR, "First input link %s parameters "
                                  "(size %dx%d) do not match the corresponding "
                                  "second input link %s parameters (size %dx%d)\n",
                                  ctx->input_pads[FROM].name, fromLink->w, fromLink->h,
                                  ctx->input_pads[TO].name, toLink->w, toLink->h);
        return AVERROR(EINVAL);
    }

    outLink->w = fromLink->w;
    outLink->h = fromLink->h;
    // outLink->time_base = fromLink->time_base;
    outLink->frame_rate = fromLink->frame_rate;

    if ((ret = ff_framesync_init_dualinput(&c->fs, ctx)) < 0) {
        return ret;
    }

    return ff_framesync_configure(&c->fs);
}

static const AVFilterPad avfilter_gltransition_inputs[] = {
        {
            .name = "from",
            .type = AVMEDIA_TYPE_VIDEO,
            .config_props = setup_gl,
            },
            {
            .name = "to",
            .type = AVMEDIA_TYPE_VIDEO,
            },
            };

static const AVFilterPad avfilter_gltransition_outputs[] = {
        {
            .name = "default",
            .type = AVMEDIA_TYPE_VIDEO,
            .config_props = config_output,
            },
            };

AVFilter ff_vf_gltransition = {
        .name          = "gltransition",
        .description   = NULL_IF_CONFIG_SMALL("OpenGL blend transitions"),
        .priv_size     = sizeof(GLTransitionContext),
        .preinit       = gltransition_framesync_preinit,
        .init          = init,
        .uninit        = uninit,
        FILTER_INPUTS(avfilter_gltransition_inputs),
        FILTER_OUTPUTS(avfilter_gltransition_outputs),
        // .inputs        = gltransition_inputs,
        // .outputs       = gltransition_outputs,
        FILTER_QUERY_FUNC(query_formats),
        .activate      = activate,
        .priv_class    = &gltransition_class,
        .flags         = AVFILTER_FLAG_SUPPORT_TIMELINE_GENERIC
};
