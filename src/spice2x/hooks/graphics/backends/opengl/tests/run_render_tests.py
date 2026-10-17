#!/usr/bin/env python3
"""Pixel-test the production OpenGL 1.1 transform on a software EGL drawable.

No Windows machine or game data is used. The separate WGL context/swap hooks
still require Windows hardware validation; EGL exercises the real GL renderer.
"""
import ctypes as C
from pathlib import Path
import subprocess
import tempfile
import numpy as np

ROOT = Path(__file__).resolve().parents[5]
BACKEND = ROOT / 'hooks/graphics/backends/opengl'
PROTOTYPES = '''
const GLubyte *glGetString(GLenum); void glGetIntegerv(GLenum, GLint*); GLenum glGetError();
void glGenTextures(GLsizei, GLuint*); void glBindTexture(GLenum, GLuint);
void glTexImage2D(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void*);
void glTexParameteri(GLenum, GLenum, GLint); void glReadPixels(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void*);
void glPixelStorei(GLenum, GLint);
void glCopyTexSubImage2D(GLenum, GLint, GLint, GLint, GLint, GLint, GLsizei, GLsizei);
void glReadBuffer(GLenum); void glDrawBuffer(GLenum); void glViewport(GLint, GLint, GLsizei, GLsizei);
void glClearColor(GLfloat, GLfloat, GLfloat, GLfloat); void glClear(GLbitfield);
void glMatrixMode(GLenum); void glLoadIdentity();
void glOrtho(GLdouble, GLdouble, GLdouble, GLdouble, GLdouble, GLdouble);
void glEnable(GLenum); void glDisable(GLenum); void glTexEnvi(GLenum, GLenum, GLint);
void glBegin(GLenum); void glEnd(); void glTexCoord2f(GLfloat, GLfloat); void glVertex2f(GLfloat, GLfloat);
void glFinish();
'''
CONSTANTS = dict(GL_NO_ERROR=0, GL_MAX_TEXTURE_SIZE=0x0d33, GL_MAX_VIEWPORT_DIMS=0x0d3a, GL_TEXTURE_2D=0x0de1,
    GL_RGB=0x1907, GL_UNSIGNED_BYTE=0x1401, GL_LINEAR=0x2601, GL_NEAREST=0x2600,
    GL_TEXTURE_MIN_FILTER=0x2801, GL_TEXTURE_MAG_FILTER=0x2800, GL_TEXTURE_WRAP_S=0x2802,
    GL_TEXTURE_WRAP_T=0x2803, GL_CLAMP=0x2900, GL_SCISSOR_TEST=0x0c11, GL_DEPTH_TEST=0x0b71,
    GL_BLEND=0x0be2, GL_CULL_FACE=0x0b44, GL_COLOR_BUFFER_BIT=0x4000, GL_PROJECTION=0x1701,
    GL_MODELVIEW=0x1700, GL_TEXTURE_ENV=0x2300, GL_TEXTURE_ENV_MODE=0x2200, GL_REPLACE=0x1e01,
    GL_PACK_ALIGNMENT=0x0d05,
    GL_QUADS=7)

with tempfile.TemporaryDirectory(prefix='spice-opengl-test-') as tmp:
    temp = Path(tmp)
    (temp / 'GL').mkdir()
    (temp / 'windows.h').write_text('// Linux test: renderer itself has no Win32 calls.\n')
    (temp / 'GL/gl.h').write_text('#pragma once\nusing GLenum=unsigned; using GLbitfield=unsigned; '
        'using GLuint=unsigned; using GLint=int; using GLsizei=int; using GLfloat=float; '
        'using GLdouble=double; using GLubyte=unsigned char;\n' +
        '\n'.join(f'#define {k} {v}' for k,v in CONSTANTS.items()) + '\nextern "C" {\n' + PROTOTYPES + '\n}\n')
    bridge = temp / 'bridge.cpp'
    bridge.write_text('''
#include "hooks/graphics/backends/opengl/opengl_resize.h"
#include <dlfcn.h>
#include <cstring>
using namespace graphics::opengl;
extern "C" {
bool needs_resize(float x, float y, int ox, int oy, int duplicate) {
    cfg::fullscreen_setting scene; scene.scale_x=x; scene.scale_y=y;
    scene.offset_x=ox; scene.offset_y=oy; scene.duplicate=static_cast<cfg::ScreenDuplicateMode>(duplicate);
    return requires_resize(scene);
}
void *create() { return new ImageResize; }
void destroy(void *state) { delete static_cast<ImageResize*>(state); }
static decltype(glCopyTexSubImage2D) *real_copy = nullptr;
static int copy_count = 0;
static void counting_copy(GLenum target, GLint level, GLint xoffset, GLint yoffset,
        GLint x, GLint y, GLsizei width, GLsizei height) {
    ++copy_count;
    real_copy(target, level, xoffset, yoffset, x, y, width, height);
}
int copies() { return copy_count; }
bool draw(void *state, int sx, int sy, int sw, int sh, int ow, int oh,
        float x, float y, int ox, int oy, int duplicate, bool linear) {
    GlApi gl; void *lib = dlopen("libGL.so.1", RTLD_NOW | RTLD_LOCAL);
    if (!gl.load([&](const char *name) { return dlsym(lib, name); })) return false;
    real_copy = gl.glCopyTexSubImage2D;
    copy_count = 0;
    gl.glCopyTexSubImage2D = counting_copy;
    cfg::fullscreen_setting scene; scene.scale_x=x; scene.scale_y=y;
    scene.offset_x=ox; scene.offset_y=oy; scene.duplicate=static_cast<cfg::ScreenDuplicateMode>(duplicate);
    return static_cast<ImageResize*>(state)->draw(gl, {sx, sy, sw, sh}, ow, oh, scene, linear, 0x0404);
}
bool readback(int width, int height, unsigned char *out) {
    if (!out) return false;
    GlApi gl; void *lib = dlopen("libGL.so.1", RTLD_NOW | RTLD_LOCAL);
    if (!gl.load([&](const char *name) { return dlsym(lib, name); })) return false;
    auto pixels = graphics::opengl::read_pixels(gl, width, height, 0x0404);
    if (!pixels) return false;
    std::memcpy(out, pixels.get(), static_cast<size_t>(width) * height * 3);
    return true;
}
}
''')
    output = temp / 'render.so'
    subprocess.run(['g++', '-std=c++20', '-Wall', '-Wextra', '-Werror', '-Wno-class-memaccess',
        '-Wno-deprecated-declarations', '-shared', '-fPIC', '-I'+str(temp), '-I'+str(ROOT),
        str(BACKEND / 'opengl_resize.cpp'), str(bridge), '-ldl', '-o', str(output)], check=True)
    render = C.CDLL(str(output))
    render.needs_resize.argtypes = [C.c_float, C.c_float, C.c_int, C.c_int, C.c_int]
    render.needs_resize.restype = C.c_bool
    assert not render.needs_resize(1, 1, 0, 0, 0)
    for scene in [(1.001,1,0,0,0),(1,.999,0,0,0),(1,1,1,0,0),(1,1,0,-1,0),(1,1,0,0,1),(1,1,0,0,2)]:
        assert render.needs_resize(*scene)
    render.create.restype = C.c_void_p
    render.draw.argtypes = [C.c_void_p] + [C.c_int] * 6 + [C.c_float] * 2 + [C.c_int] * 3 + [C.c_bool]
    render.draw.restype = C.c_bool
    render.copies.restype = C.c_int
    render.readback.argtypes = [C.c_int, C.c_int, C.POINTER(C.c_ubyte)]
    render.readback.restype = C.c_bool
    render.destroy.argtypes = [C.c_void_p]
    egl = C.CDLL('libEGL.so.1'); gl = C.CDLL('libGL.so.1')
    egl.eglGetProcAddress.argtypes=[C.c_char_p]; egl.eglGetProcAddress.restype=C.c_void_p
    get_display = C.CFUNCTYPE(C.c_void_p,C.c_uint,C.c_void_p,C.c_void_p)(egl.eglGetProcAddress(b'eglGetPlatformDisplayEXT'))
    display = get_display(0x31dd, None, None)
    def bind(lib, name, result, args):
        f=getattr(lib,name);f.restype=result;f.argtypes=args;return f
    initialize=bind(egl,'eglInitialize',C.c_uint,[C.c_void_p,C.c_void_p,C.c_void_p])
    assert initialize(display,None,None)
    assert bind(egl,'eglBindAPI',C.c_uint,[C.c_uint])(0x30a2)
    config=C.c_void_p(); count=C.c_int()
    attrs=(C.c_int*13)(0x3033,1,0x3040,8,0x3024,8,0x3023,8,0x3022,8,0x3021,8,0x3038)
    assert bind(egl,'eglChooseConfig',C.c_uint,[C.c_void_p,C.c_void_p,C.c_void_p,C.c_int,C.c_void_p])(display,attrs,C.byref(config),1,C.byref(count)) and count.value
    attrs=(C.c_int*5)(0x3057,64,0x3056,64,0x3038)
    surface=bind(egl,'eglCreatePbufferSurface',C.c_void_p,[C.c_void_p,C.c_void_p,C.c_void_p])(display,config,attrs)
    create_context=bind(egl,'eglCreateContext',C.c_void_p,[C.c_void_p,C.c_void_p,C.c_void_p,C.c_void_p])
    end=(C.c_int*1)(0x3038)
    game=create_context(display,config,None,end); own=create_context(display,config,None,end)
    assert surface and game and own
    make_current=bind(egl,'eglMakeCurrent',C.c_uint,[C.c_void_p,C.c_void_p,C.c_void_p,C.c_void_p])
    clear_color=bind(gl,'glClearColor',None,[C.c_float]*4)
    clear=bind(gl,'glClear',None,[C.c_uint]); enable=bind(gl,'glEnable',None,[C.c_uint]); disable=bind(gl,'glDisable',None,[C.c_uint])
    scissor=bind(gl,'glScissor',None,[C.c_int]*4)
    finish=bind(gl,'glFinish',None,[])
    error=bind(gl,'glGetError',C.c_uint,[])
    get_int=bind(gl,'glGetIntegerv',None,[C.c_uint,C.c_void_p])
    viewport=bind(gl,'glViewport',None,[C.c_int]*4)
    state=render.create()
    def paint():
        assert make_current(display,surface,surface,game)
        disable(0x0c11); clear_color(0,0,0,1); clear(0x4000)
        enable(0x0c11); scissor(16,16,32,32); clear_color(1,1,1,1); clear(0x4000)
        viewport(3,4,51,49); finish()
    def rgb_pixels(width=64,height=64):
        assert make_current(display,surface,surface,own)
        image=np.zeros((height * width * 3,),dtype=np.uint8)
        assert render.readback(width,height,image.ctypes.data_as(C.POINTER(C.c_ubyte)))
        return image.reshape((height,width,3))
    def pixels(width=64,height=64):
        return rgb_pixels(width,height)[:,:,0]
    def draw(source, output, scale_x=1, scale_y=1, offset_x=0, offset_y=0, duplicate=0, linear=False):
        return render.draw(state, *source, *output, scale_x, scale_y,
                           offset_x, offset_y, duplicate, linear)
    cases=[(1,1,0,0,0),(.75,.75,0,0,0),(1,1,5,7,0),(.75,1.25,-4,-3,0),(2,2,0,0,0),(.6,.6,0,0,1),(.6,.6,0,0,2)]
    checks=0
    for sx,sy,ox,oy,duplicate in cases:
        paint()
        enable(0xffff) # Game error state must survive work in the separate context.
        assert make_current(display,surface,surface,own)
        assert draw((0,0,64,64),(64,64),sx,sy,ox,oy,duplicate)
        assert render.copies()==1
        actual=pixels()
        x,y_top=np.meshgrid(np.arange(64)+.5,np.arange(64)+.5)
        y=64-y_top
        w,h=64*sx,64*sy; left=(64-w)/2+ox*sx; bottom=(64-h)/2-oy*sy
        def white(offset):
            tx=(x-left-offset)/sx;ty=(y-bottom)/sy
            return (tx>=16)&(tx<48)&(ty>=16)&(ty<48)
        expected=white(0)
        if duplicate: expected |= white(-w if duplicate==1 else w)
        assert np.array_equal(actual,expected.astype(np.uint8)*255), (sx,sy,ox,oy,duplicate,np.sum(actual!=expected*255))
        assert make_current(display,surface,surface,game)
        view=(C.c_int*4)();get_int(0x0ba2,view)
        assert list(view)==[3,4,51,49]
        assert error()==0x0500 and error()==0
        checks+=1

    # A non-square source at an offset must be copied once, scaled into the output,
    # and retain its top-down color orientation without exposing the surrounding black.
    source=(5,7,15,9); output=(37,29)
    assert make_current(display,surface,surface,game)
    disable(0x0c11); clear_color(0,0,0,1); clear(0x4000)
    enable(0x0c11)
    scissor(source[0],source[1],source[2],source[3]//2); clear_color(0,0,1,1); clear(0x4000)
    scissor(source[0],source[1]+source[3]//2,source[2],source[3]-source[3]//2)
    clear_color(1,0,0,1); clear(0x4000); finish()
    assert make_current(display,surface,surface,own)
    assert draw(source,output)
    assert render.copies()==1
    actual=rgb_pixels(*output)
    assert np.all(actual[:,:,1] == 0)
    assert np.all(actual[:12,:,0] == 255) and np.all(actual[:12,:,2] == 0)
    assert np.all(actual[-12:,:,0] == 0) and np.all(actual[-12:,:,2] == 255)

    # Viewport limits are cached per private context. Normal FHD is valid, while
    # Mesa's 16384 limit must reject both over-wide and over-tall output untouched.
    limits=(C.c_int*2)(); get_int(0x0d3a,limits)
    max_width,max_height=limits[0],limits[1]
    assert max_width >= 1920 and max_height >= 1080, (max_width,max_height)
    paint(); assert make_current(display,surface,surface,own)
    assert draw((0,0,64,64),(1920,1080))
    assert render.copies()==1
    _=rgb_pixels()
    for oversized in ((max_width+1,64),(64,max_height+1)):
        paint(); assert make_current(display,surface,surface,own)
        before=rgb_pixels()
        assert not draw((0,0,64,64),oversized)
        assert render.copies()==0
        assert np.array_equal(before,rgb_pixels()), oversized

    paint();assert make_current(display,surface,surface,own)
    before=pixels()
    assert not draw((0,0,65536,64),(65536,64))
    assert render.copies()==0
    assert np.array_equal(before,pixels())
    assert not draw((60,60,5,5),(64,64))
    assert render.copies()==0
    assert np.array_equal(before,pixels())
    assert draw((0,0,64,64),(64,64),linear=True)
    assert error()==0
    # Reuse the 64x64 texture for a smaller image. Linear sampling must not
    # blend the new white image with the old magenta padding.
    assert make_current(display,surface,surface,game)
    disable(0x0c11); clear_color(1,0,1,1); clear(0x4000); finish()
    assert make_current(display,surface,surface,own)
    assert draw((0,0,64,64),(64,64),linear=True)
    _=pixels()
    assert make_current(display,surface,surface,game)
    disable(0x0c11); clear_color(1,1,1,1); clear(0x4000); finish()
    assert make_current(display,surface,surface,own)
    assert draw((0,0,60,60),(60,60),1.2,1,-10,0,linear=True)
    image=rgb_pixels(60,60)
    assert np.all(image[30,53] == 255), image[30,53].tolist()
    assert make_current(display,surface,surface,game)
    disable(0x0c11); clear_color(1,1,1,1); clear(0x4000); finish()
    assert make_current(display,surface,surface,own)
    assert draw((0,0,60,60),(60,60),.6,.6,duplicate=2,linear=True)
    image=rgb_pixels(60,60)
    assert np.all(image[30,55] == 255), image[30,55].tolist()

    # RGB readback is packed, top-down, and preserves channel order for odd row widths.
    assert make_current(display,surface,surface,game)
    disable(0x0c11); clear_color(0,0,0,1); clear(0x4000)
    enable(0x0c11); scissor(0,0,3,1); clear_color(0,0,1,1); clear(0x4000)
    scissor(0,1,3,1); clear_color(0,1,0,1); clear(0x4000); finish()
    rgb=rgb_pixels(3,2)
    assert np.all(rgb[0] == (0,255,0)) and np.all(rgb[1] == (0,0,255)), rgb.tolist()
    dummy=(C.c_ubyte*1)()
    assert not render.readback(65536,1,dummy)
    assert not render.readback(1,65536,dummy)
    # Integer translations must preserve every source pixel with either filter.
    for width,height in ((64,64),(60,60)):
        for axis in (0,1):
            for ox,oy in ((1,0),(-1,0),(0,1),(0,-1)):
                for linear in (False,True):
                    assert make_current(display,surface,surface,game)
                    disable(0x0c11); clear_color(0,0,0,1); clear(0x4000)
                    enable(0x0c11); clear_color(1,1,1,1)
                    for line in range(0,64,2):
                        scissor(line,0,1,64) if axis==0 else scissor(0,line,64,1)
                        clear(0x4000)
                    finish()
                    assert make_current(display,surface,surface,own)
                    assert draw((0,0,width,height),(width,height),1,1,ox,oy,linear=linear)
                    x,y_top=np.meshgrid(np.arange(width),np.arange(height))
                    y=height-1-y_top
                    tx,ty=x-ox,y+oy
                    visible=(x<width)&(y<height)&(tx>=0)&(tx<width)&(ty>=0)&(ty<height)
                    expected=visible&((tx if axis==0 else ty)%2==0)
                    assert np.array_equal(pixels(width,height),expected.astype(np.uint8)*255), (width,height,axis,ox,oy,linear)
    # Magnified edges must repeat the current edge pixel, including at POT limits.
    for width,height in ((64,64),(60,60)):
        for sign in (-1,1):
            assert make_current(display,surface,surface,game)
            disable(0x0c11); clear_color(1,1,1,1); clear(0x4000); finish()
            assert make_current(display,surface,surface,own)
            assert draw((0,0,width,height),(width,height),2,2,sign*width//4,-sign*height//4,linear=True)
            assert np.all(pixels(width,height)[:height,:width]==255)
    for width,height in ((1,1),(1,3),(3,1)):
        assert make_current(display,surface,surface,game)
        disable(0x0c11); clear_color(1,1,1,1); clear(0x4000); finish()
        assert make_current(display,surface,surface,own)
        assert draw((0,0,width,height),(width,height),2.5,2.5,linear=True)
        assert np.all(pixels(width,height)[:height,:width]==255)
    render.destroy(state)
    assert make_current(display,None,None,None)
    destroy_context=bind(egl,'eglDestroyContext',C.c_uint,[C.c_void_p,C.c_void_p])
    assert destroy_context(display,own) and destroy_context(display,game)
    assert bind(egl,'eglDestroySurface',C.c_uint,[C.c_void_p,C.c_void_p])(display,surface)
    assert bind(egl,'eglTerminate',C.c_uint,[C.c_void_p])(display)
    print(f'OpenGL pixel tests passed: {checks} transform cases, integer pixel alignment, clamped edges, texture reuse, size limits and preserved game state.')
