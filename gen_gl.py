"""Generate opengl32.def + signature-correct stubs from mingw GL headers.

Hand-written functions live in gl_sw.c; everything else logs once and no-ops
so GLEW/wglGetProcAddress never sees a NULL, and stdcall stacks stay honest.
"""
from __future__ import annotations

import os
import re
import sys

ROOT = os.path.dirname(os.path.abspath(__file__))
INC = r"C:\zig\lib\libc\include\any-windows-any\GL"

# Implemented in gl_sw.c. Names must match exported symbols.
HAND = {
    # WGL
    "wglChoosePixelFormat",
    "wglSetPixelFormat",
    "wglGetPixelFormat",
    "wglDescribePixelFormat",
    "wglSwapBuffers",
    "wglCreateContext",
    "wglDeleteContext",
    "wglMakeCurrent",
    "wglShareLists",
    "wglGetCurrentDC",
    "wglGetCurrentContext",
    "wglGetProcAddress",
    "wglCopyContext",
    "wglCreateLayerContext",
    "wglDescribeLayerPlane",
    "wglGetDefaultProcAddress",
    "wglGetLayerPaletteEntries",
    "wglSetLayerPaletteEntries",
    "wglRealizeLayerPalette",
    "wglSwapLayerBuffers",
    "wglSwapMultipleBuffers",
    "wglUseFontBitmapsA",
    "wglUseFontBitmapsW",
    "wglUseFontOutlinesA",
    "wglUseFontOutlinesW",
    "wglCreateContextAttribsARB",
    "wglChoosePixelFormatARB",
    "wglGetPixelFormatAttribivARB",
    "wglGetPixelFormatAttribfvARB",
    "wglGetExtensionsStringARB",
    "wglGetExtensionsStringEXT",
    "wglSwapIntervalEXT",
    "wglGetSwapIntervalEXT",
    # GL state / query
    "glGetString",
    "glGetStringi",
    "glGetError",
    "glGetIntegerv",
    "glGetFloatv",
    "glGetBooleanv",
    "glGetDoublev",
    "glGetIntegeri_v",
    "glEnable",
    "glDisable",
    "glIsEnabled",
    "glEnablei",
    "glDisablei",
    "glViewport",
    "glScissor",
    "glClear",
    "glClearColor",
    "glClearDepth",
    "glClearDepthf",
    "glClearStencil",
    "glFinish",
    "glFlush",
    "glHint",
    "glPixelStorei",
    "glPixelStoref",
    "glBlendFunc",
    "glBlendFuncSeparate",
    "glBlendEquation",
    "glBlendEquationSeparate",
    "glColorMask",
    "glColorMaski",
    "glDepthFunc",
    "glDepthMask",
    "glDepthRange",
    "glCullFace",
    "glFrontFace",
    "glPolygonMode",
    "glPolygonOffset",
    "glLineWidth",
    "glPointSize",
    "glStencilFunc",
    "glStencilOp",
    "glStencilFuncSeparate",
    "glStencilOpSeparate",
    "glStencilMask",
    "glStencilMaskSeparate",
    "glPolygonStipple",
    "glReadPixels",
    "glReadBuffer",
    "glDrawBuffer",
    "glDrawBuffers",
    "glActiveTexture",
    # textures
    "glGenTextures",
    "glDeleteTextures",
    "glBindTexture",
    "glIsTexture",
    "glTexImage2D",
    "glTexSubImage2D",
    "glTexImage3D",
    "glTexStorage2D",
    "glTexParameteri",
    "glTexParameterf",
    "glTexParameteriv",
    "glTexParameterfv",
    "glGetTexImage",
    "glGetTexLevelParameteriv",
    "glGetTexParameteriv",
    "glCompressedTexImage2D",
    "glGenerateMipmap",
    "glCopyTexSubImage2D",
    "glPixelStorei",
    # buffers / VAO
    "glGenBuffers",
    "glDeleteBuffers",
    "glBindBuffer",
    "glBufferData",
    "glBufferSubData",
    "glMapBuffer",
    "glMapBufferRange",
    "glUnmapBuffer",
    "glBindBufferBase",
    "glBindBufferRange",
    "glCopyBufferSubData",
    "glGenVertexArrays",
    "glDeleteVertexArrays",
    "glBindVertexArray",
    "glVertexAttribPointer",
    "glVertexAttribIPointer",
    "glEnableVertexAttribArray",
    "glDisableVertexAttribArray",
    "glVertexAttribDivisor",
    "glVertexAttrib1f",
    "glVertexAttrib2f",
    "glVertexAttrib3f",
    "glVertexAttrib4f",
    "glVertexAttrib1fv",
    "glVertexAttrib2fv",
    "glVertexAttrib3fv",
    "glVertexAttrib4fv",
    "glVertexAttrib4Nub",
    "glVertexAttrib4Nubv",
    "glBindAttribLocation",
    "glGetAttribLocation",
    # shaders
    "glCreateShader",
    "glShaderSource",
    "glCompileShader",
    "glDeleteShader",
    "glCreateProgram",
    "glAttachShader",
    "glDetachShader",
    "glLinkProgram",
    "glUseProgram",
    "glDeleteProgram",
    "glGetShaderiv",
    "glGetProgramiv",
    "glGetShaderInfoLog",
    "glGetProgramInfoLog",
    "glGetUniformLocation",
    "glGetUniformBlockIndex",
    "glUniformBlockBinding",
    "glBindFragDataLocation",
    "glUniform1i",
    "glUniform2i",
    "glUniform3i",
    "glUniform4i",
    "glUniform1ui",
    "glUniform1f",
    "glUniform2f",
    "glUniform3f",
    "glUniform4f",
    "glUniform1iv",
    "glUniform2iv",
    "glUniform3iv",
    "glUniform4iv",
    "glUniform1uiv",
    "glUniform1fv",
    "glUniform2fv",
    "glUniform3fv",
    "glUniform4fv",
    "glUniformMatrix2fv",
    "glUniformMatrix3fv",
    "glUniformMatrix4fv",
    "glUniformMatrix2x3fv",
    "glUniformMatrix3x2fv",
    "glUniformMatrix2x4fv",
    "glUniformMatrix4x2fv",
    "glUniformMatrix3x4fv",
    "glUniformMatrix4x3fv",
    "glTransformFeedbackVaryings",
    "glBeginTransformFeedback",
    "glEndTransformFeedback",
    # FBO
    "glGenFramebuffers",
    "glDeleteFramebuffers",
    "glBindFramebuffer",
    "glFramebufferTexture2D",
    "glFramebufferTexture",
    "glFramebufferRenderbuffer",
    "glCheckFramebufferStatus",
    "glGenRenderbuffers",
    "glDeleteRenderbuffers",
    "glBindRenderbuffer",
    "glRenderbufferStorage",
    "glBlitFramebuffer",
    "glDrawBuffers",
    # draw
    "glDrawArrays",
    "glDrawElements",
    "glDrawRangeElements",
    "glDrawArraysInstanced",
    "glDrawElementsInstanced",
    "glDrawElementsBaseVertex",
    "glDrawElementsInstancedBaseVertex",
    "glMultiDrawElements",
    "glMultiDrawElementsBaseVertex",
    "glBegin",
    "glEnd",
    "glVertex2f",
    "glVertex2fv",
    "glVertex3f",
    "glTexCoord2f",
    "glColor4ub",
    "glColor4f",
    # compute / sync / sampler (honest no-ops with logs)
    "glDispatchCompute",
    "glBindImageTexture",
    "glMemoryBarrier",
    "glFenceSync",
    "glClientWaitSync",
    "glDeleteSync",
    "glWaitSync",
    "glGenSamplers",
    "glDeleteSamplers",
    "glBindSampler",
    "glSamplerParameteri",
    "glSamplerParameterf",
}


# Forwarding to glhost64.exe (see glhost.h). Every hand-written gl* function is
# one of:
#   L  answered here only (queries, sync objects)
#   H  forwarded by hand-written fw_<name> in gl_fwd.c (pointer arguments)
#   F  forwarded instead of run here while the host is up (drawing)
#   B  (default) run here, then forwarded - generated
FWD_LOCAL = {
    "glGetString", "glGetStringi", "glGetError", "glGetIntegerv", "glGetFloatv",
    "glGetBooleanv", "glGetDoublev", "glGetIntegeri_v", "glIsEnabled", "glIsTexture",
    "glGetTexLevelParameteriv", "glGetTexParameteriv", "glCheckFramebufferStatus",
    "glGetShaderiv", "glGetProgramiv", "glGetShaderInfoLog", "glGetProgramInfoLog",
    "glGetAttribLocation", "glGetUniformBlockIndex", "glFenceSync", "glClientWaitSync",
    "glDeleteSync", "glWaitSync", "glFinish", "glFlush",
}
FWD_HAND = {
    "glDrawBuffers", "glTexImage2D", "glTexSubImage2D", "glTexImage3D",
    "glCompressedTexImage2D", "glTexParameteriv", "glTexParameterfv", "glReadPixels",
    "glGetTexImage", "glGenTextures", "glDeleteTextures", "glGenBuffers", "glDeleteBuffers",
    "glBufferData", "glBufferSubData", "glMapBuffer", "glMapBufferRange", "glUnmapBuffer",
    "glGenVertexArrays", "glDeleteVertexArrays", "glVertexAttribPointer",
    "glVertexAttribIPointer", "glVertexAttrib1fv", "glVertexAttrib2fv", "glVertexAttrib3fv",
    "glVertexAttrib4fv", "glVertexAttrib4Nubv", "glBindAttribLocation", "glCreateShader",
    "glShaderSource", "glCreateProgram", "glGetUniformLocation", "glBindFragDataLocation",
    "glUniform1iv", "glUniform2iv", "glUniform3iv", "glUniform4iv", "glUniform1uiv",
    "glUniform1fv", "glUniform2fv", "glUniform3fv", "glUniform4fv", "glUniformMatrix2fv",
    "glUniformMatrix3fv", "glUniformMatrix4fv", "glUniformMatrix2x3fv", "glUniformMatrix3x2fv",
    "glUniformMatrix2x4fv", "glUniformMatrix4x2fv", "glUniformMatrix3x4fv",
    "glUniformMatrix4x3fv", "glTransformFeedbackVaryings", "glGenFramebuffers",
    "glDeleteFramebuffers", "glGenRenderbuffers", "glDeleteRenderbuffers", "glDrawArrays",
    "glDrawElements", "glDrawRangeElements", "glDrawArraysInstanced", "glDrawElementsInstanced",
    "glDrawElementsBaseVertex", "glDrawElementsInstancedBaseVertex", "glMultiDrawElements",
    "glMultiDrawElementsBaseVertex", "glPolygonStipple", "glVertex2fv", "glGenSamplers",
    "glDeleteSamplers", "glPixelStorei", "glPixelStoref",
}
FWD_ONLY = {
    "glClear", "glBlitFramebuffer", "glCopyTexSubImage2D", "glBegin", "glEnd", "glVertex2f",
    "glVertex3f", "glTexCoord2f", "glColor4ub", "glColor4f", "glDispatchCompute",
}
# Replayed through hk_<name>(const uint64_t *a) in glhost.c, which keeps
# per-context state or maps the default framebuffer.
FB_ATTACH = re.compile(r"^glFramebuffer(Texture\w*|Renderbuffer)$")
HOST_HOOK = {
    "glBindFramebuffer", "glDrawBuffer", "glReadBuffer", "glUseProgram", "glLinkProgram",
    "glCompileShader", "glDeleteShader", "glDeleteProgram", "glBindBuffer",
}
XL_KIND = {
    "texture": "GLH_K_TEX", "buffer": "GLH_K_BUF", "framebuffer": "GLH_K_FBO",
    "renderbuffer": "GLH_K_RBO", "array": "GLH_K_VAO", "program": "GLH_K_OBJ",
    "shader": "GLH_K_OBJ", "sampler": "GLH_K_SAMP",
}
FLOAT_T = {"GLfloat", "GLclampf"}
DOUBLE_T = {"GLdouble", "GLclampd"}
SIGNED_T = {"GLint", "GLsizei", "GLshort", "GLbyte", "GLintptr", "GLsizeiptr", "GLfixed"}


def fwd_mode(name: str) -> str:
    if not name.startswith("gl"):
        return ""
    if name in FWD_LOCAL:
        return "L"
    if name in FWD_HAND:
        return "H"
    if name in FWD_ONLY:
        return "F"
    return "B"


def split_args(args: str) -> list[tuple[str, str]]:
    """(type, name) per parameter."""
    if void_args(args):
        return []
    out = []
    for p in args.split(","):
        p = p.strip()
        m = re.match(r"(.*?)([A-Za-z_]\w*)$", p)
        out.append((m.group(1).strip(), m.group(2)))
    return out


def gen_forwarding(protos: dict) -> None:
    gen = sorted(n for n in HAND if fwd_mode(n) in ("B", "F") and n in protos)
    ops = ["/* Generated by gen_gl.py - do not edit. */",
           "#ifndef GLHOST_OPS_H", "#define GLHOST_OPS_H", '#include "glhost.h"', "enum {"]
    for i, n in enumerate(gen):
        ops.append("\tGLH_OP_%s = GLH_OP_GEN + %d," % (n, i))
    ops.append("\tGLH_OP_GEN_END = GLH_OP_GEN + %d" % len(gen))
    ops.append("};")
    ops.append("static const char *const glh_gen_names[] = {")
    ops.extend('\t"%s",' % n for n in gen)
    ops.append("};")
    ops.append("#endif")

    dll = ["/* Generated by gen_gl.py - do not edit. */", '#include "gl_fwd.h"',
           '#include "glhost_ops.h"', ""]
    host = ["/* Generated by gen_gl.py - do not edit. Included by glhost.c. */", ""]
    # Every hand-written GL entry point gets a real function pointer on the host.
    for n in sorted(x for x in HAND if x.startswith("gl") and x in protos):
        ret, args = protos[n]
        host.append("typedef %s(APIENTRY *T_%s)(%s);" % (ret, n, args))
        host.append("static T_%s P_%s;" % (n, n))
    host.append("")
    host.append("static int gen_load(void)")
    host.append("{")
    host.append("\tint miss = 0;")
    for n in sorted(x for x in HAND if x.startswith("gl") and x in protos):
        host.append('\tP_%s = (T_%s)glh_proc("%s");' % (n, n, n))
        if fwd_mode(n) in ("B", "F"):
            host.append("\tmiss += !P_%s;" % n)
    host.append("\treturn miss;")
    host.append("}")
    host.append("")
    for n in gen:
        if n in HOST_HOOK:
            host.append("static void hk_%s(const uint64_t *a);" % n)
    host.append("static int fb_is_dflt(GLenum target);")
    host.append("")
    host.append("static int gen_dispatch(uint32_t op, const uint64_t *a, uint32_t na)")
    host.append("{")
    host.append("\tswitch (op) {")

    for n in gen:
        ret, args = protos[n]
        if ret.strip() != "void":
            raise SystemExit("forwarded %s returns %s; make it H or L" % (n, ret))
        params = split_args(args)
        enc, dec = [], []
        for i, (t, pn) in enumerate(params):
            if "*" in t or t == "GLsync":
                raise SystemExit("forwarded %s has pointer arg %s %s; make it H" % (n, t, pn))
            if t in FLOAT_T:
                enc.append("\tfa_[%d] = fw_f(%s);" % (i, pn))
                dec.append("glh_f(a[%d])" % i)
            elif t in DOUBLE_T:
                enc.append("\tfa_[%d] = fw_d(%s);" % (i, pn))
                dec.append("glh_d(a[%d])" % i)
            elif t in SIGNED_T:
                enc.append("\tfa_[%d] = (uint64_t)(int64_t)(%s);" % (i, pn))
                if t == "GLint" and pn == "location":
                    dec.append("xl_loc((GLint)(int64_t)a[%d])" % i)
                else:
                    dec.append("(%s)(int64_t)a[%d]" % (t, i))
            else:
                enc.append("\tfa_[%d] = (uint64_t)(%s);" % (i, pn))
                if t == "GLuint" and pn in XL_KIND:
                    dec.append("xl(%s, (GLuint)a[%d])" % (XL_KIND[pn], i))
                else:
                    dec.append("(%s)a[%d]" % (t, i))
        names = ", ".join(pn for _, pn in params)
        na = len(params)
        dll.append("void APIENTRY %s(%s);" % (n, args))
        dll.append("void APIENTRY fw_%s(%s)" % (n, args))
        dll.append("{")
        if na:
            dll.append("\tuint64_t fa_[%d];" % na)
        if fwd_mode(n) == "B":
            dll.append("\t%s(%s);" % (n, names))
            dll.append("\tif (!glfwd_on())")
            dll.append("\t\treturn;")
        else:
            dll.append("\tif (!glfwd_on()) {")
            dll.append("\t\t%s(%s);" % (n, names))
            dll.append("\t\treturn;")
            dll.append("\t}")
        dll.extend(enc)
        dll.append("\tfw_call(GLH_OP_%s, %s, %d, NULL, 0);" % (n, "fa_" if na else "NULL", na))
        dll.append("}")
        dll.append("")
        host.append("\tcase GLH_OP_%s:" % n)
        host.append("\t\tif (na < %d)" % na)
        host.append("\t\t\treturn -1;")
        if FB_ATTACH.match(n):
            # The host's stand-in for framebuffer 0 must keep its attachments.
            host.append("\t\tif (fb_is_dflt((GLenum)a[0]))")
            host.append("\t\t\treturn 1;")
        if n in HOST_HOOK:
            host.append("\t\thk_%s(a);" % n)
        else:
            host.append("\t\tP_%s(%s);" % (n, ", ".join(dec)))
        host.append("\t\treturn 1;")
    host.append("\t}")
    host.append("\treturn 0;")
    host.append("}")

    for path, lines in (("glhost_ops.h", ops), ("gl_fwd_gen.c", dll), ("glhost_gen.c", host)):
        with open(os.path.join(ROOT, path), "w", encoding="utf-8", newline="\n") as f:
            f.write("\n".join(lines))
            f.write("\n")
    print("forwarding: %d generated, %d hand, %d local" % (
        len(gen), len([n for n in HAND if fwd_mode(n) == "H"]),
        len([n for n in HAND if fwd_mode(n) == "L"])))


def parse_glapi(text: str, kind: str) -> list[tuple[str, str, str, str]]:
    """Return (name, ret, args, raw) tuples."""
    out = []
    if kind == "glapi":
        rx = re.compile(
            r"GLAPI\s+(.+?)\s*APIENTRY\s+(gl\w+)\s*\((.*?)\)\s*;"
        )
    elif kind == "wingdi":
        rx = re.compile(
            r"WINGDIAPI\s+(.+?)\s*APIENTRY\s+(gl\w+)\s*\((.*?)\)\s*;"
        )
    else:
        rx = re.compile(
            r"^([A-Za-z0-9_ \*]+?)\s+WINAPI\s+(wgl\w+)\s*\((.*?)\)\s*;", re.M
        )
    for m in rx.finditer(text):
        ret, name, args = m.group(1).strip(), m.group(2), m.group(3).strip()
        ret = re.sub(r"\s+", " ", ret).strip()
        args = re.sub(r"\s+", " ", args).strip()
        if "#" in ret or "#" in args or "typedef" in ret:
            continue
        out.append((name, ret, args, m.group(0)))
    return out


def void_args(args: str) -> bool:
    a = args.strip()
    return a == "" or a == "void"


def unused_args(args: str) -> str:
    if void_args(args):
        return ""
    parts = []
    for i, p in enumerate(args.split(",")):
        p = p.strip()
        if p == "void":
            continue
        # unnamed parameter
        if re.search(r"\*|\[", p) and not re.search(r"\w+$", p.replace("*", " ")):
            parts.append("/* arg */")
        ident = re.findall(r"[A-Za-z_]\w*$", p.replace("[]", ""))
        if ident:
            parts.append("(void)%s;" % ident[-1])
        else:
            parts.append("(void)0;")
    return " ".join(parts)


def stub_body(name: str, ret: str, args: str) -> str:
    unused = unused_args(args)
    ni = 'gl_ni("%s");' % name
    r = ret.strip()
    if r == "void":
        return "%s %s" % (unused, ni)
    if "*" in r or r in ("GLsync",):
        return "%s %s return 0;" % (unused, ni)
    if r in ("GLboolean", "BOOL"):
        return "%s %s return 0;" % (unused, ni)
    return "%s %s return 0;" % (unused, ni)


def main() -> int:
    gl_h = open(os.path.join(INC, "gl.h"), encoding="latin1").read()
    glext = open(os.path.join(INC, "glext.h"), encoding="latin1").read()
    wglext = open(os.path.join(INC, "wglext.h"), encoding="latin1").read()

    protos = {}
    for name, ret, args, raw in parse_glapi(gl_h, "wingdi"):
        protos[name] = (ret, args)
    for name, ret, args, raw in parse_glapi(glext, "glapi"):
        protos.setdefault(name, (ret, args))
    WGL_CORE = {
        "wglChoosePixelFormat": ("int", "HDC hdc, CONST PIXELFORMATDESCRIPTOR *ppfd"),
        "wglSetPixelFormat": ("BOOL", "HDC hdc, int fmt, CONST PIXELFORMATDESCRIPTOR *ppfd"),
        "wglGetPixelFormat": ("int", "HDC hdc"),
        "wglDescribePixelFormat": ("int", "HDC hdc, int fmt, UINT size, LPPIXELFORMATDESCRIPTOR ppfd"),
        "wglSwapBuffers": ("BOOL", "HDC hdc"),
        "wglCreateContext": ("HGLRC", "HDC hdc"),
        "wglDeleteContext": ("BOOL", "HGLRC rc"),
        "wglMakeCurrent": ("BOOL", "HDC hdc, HGLRC rc"),
        "wglShareLists": ("BOOL", "HGLRC a, HGLRC b"),
        "wglGetCurrentDC": ("HDC", "void"),
        "wglGetCurrentContext": ("HGLRC", "void"),
        "wglGetProcAddress": ("PROC", "LPCSTR name"),
        "wglCopyContext": ("BOOL", "HGLRC a, HGLRC b, UINT mask"),
        "wglCreateLayerContext": ("HGLRC", "HDC hdc, int layer"),
        "wglDescribeLayerPlane": ("BOOL", "HDC hdc, int fmt, int layer, UINT size, LPLAYERPLANEDESCRIPTOR pd"),
        "wglGetDefaultProcAddress": ("PROC", "LPCSTR name"),
        "wglGetLayerPaletteEntries": ("int", "HDC hdc, int layer, int start, int n, COLORREF *c"),
        "wglSetLayerPaletteEntries": ("int", "HDC hdc, int layer, int start, int n, CONST COLORREF *c"),
        "wglRealizeLayerPalette": ("BOOL", "HDC hdc, int layer, BOOL realize"),
        "wglSwapLayerBuffers": ("BOOL", "HDC hdc, UINT planes"),
        "wglSwapMultipleBuffers": ("DWORD", "UINT n, CONST WGLSWAP *bufs"),
        "wglUseFontBitmapsA": ("BOOL", "HDC hdc, DWORD first, DWORD count, DWORD list"),
        "wglUseFontBitmapsW": ("BOOL", "HDC hdc, DWORD first, DWORD count, DWORD list"),
        "wglUseFontOutlinesA": ("BOOL", "HDC hdc, DWORD first, DWORD count, DWORD list, FLOAT d, FLOAT e, int fmt, LPGLYPHMETRICSFLOAT gmf"),
        "wglUseFontOutlinesW": ("BOOL", "HDC hdc, DWORD first, DWORD count, DWORD list, FLOAT d, FLOAT e, int fmt, LPGLYPHMETRICSFLOAT gmf"),
        "wglGetExtensionsStringARB": ("const char *", "HDC hdc"),
        "wglGetExtensionsStringEXT": ("const char *", "void"),
        "wglCreateContextAttribsARB": ("HGLRC", "HDC hdc, HGLRC share, const int *attr"),
        "wglChoosePixelFormatARB": ("BOOL", "HDC hdc, const int *ia, const FLOAT *fa, UINT nmax, int *fmts, UINT *nout"),
        "wglGetPixelFormatAttribivARB": ("BOOL", "HDC hdc, int fmt, int layer, UINT n, const int *attr, int *vals"),
        "wglGetPixelFormatAttribfvARB": ("BOOL", "HDC hdc, int fmt, int layer, UINT n, const int *attr, FLOAT *vals"),
        "wglSwapIntervalEXT": ("BOOL", "int interval"),
        "wglGetSwapIntervalEXT": ("int", "void"),
    }
    for name, (ret, args) in WGL_CORE.items():
        protos.setdefault(name, (ret, args))

    # System opengl32 export list — keep the drop-in a complete replacement.
    sys_exports = [
        "GlmfBeginGlsBlock",
        "GlmfCloseMetaFile",
        "GlmfEndGlsBlock",
        "GlmfEndPlayback",
        "GlmfInitPlayback",
        "GlmfPlayGlsRecord",
        "glDebugEntry",
    ]
    # Will fill from protos that start with gl or wgl plus the Glmf ones.

    stub_names = sorted(n for n in protos if n not in HAND)
    hand_in_headers = sorted(n for n in protos if n in HAND)

    lines = []
    lines.append("/* Generated by gen_gl.py — do not edit. */")
    lines.append("#include <string.h>")
    lines.append("#include <windows.h>")
    lines.append("#include <GL/gl.h>")
    lines.append("#include <GL/glext.h>")
    lines.append("#include <GL/wglext.h>")
    lines.append("void gl_ni(const char *name);")
    lines.append("")
    for name in sorted(HAND):
        if name not in protos:
            continue
        ret, args = protos[name]
        conv = "APIENTRY" if name.startswith("gl") else "WINAPI"
        lines.append("extern %s %s %s(%s);" % (ret, conv, name, args))
        if fwd_mode(name) in ("B", "F", "H"):
            lines.append("extern %s %s fw_%s(%s);" % (ret, conv, name, args))
    lines.append("")
    for name in stub_names:
        ret, args = protos[name]
        conv = "APIENTRY" if name.startswith("gl") else "WINAPI"
        lines.append("%s %s %s(%s)" % (ret, conv, name, args))
        lines.append("{ %s }" % stub_body(name, ret, args))
        lines.append("")

    # Metafile / debug leftovers from the system DLL export table.
    extra = """
int WINAPI GlmfBeginGlsBlock(int a) { (void)a; gl_ni("GlmfBeginGlsBlock"); return 0; }
int WINAPI GlmfCloseMetaFile(int a) { (void)a; gl_ni("GlmfCloseMetaFile"); return 0; }
int WINAPI GlmfEndGlsBlock(int a) { (void)a; gl_ni("GlmfEndGlsBlock"); return 0; }
int WINAPI GlmfEndPlayback(int a) { (void)a; gl_ni("GlmfEndPlayback"); return 0; }
int WINAPI GlmfInitPlayback(int a) { (void)a; gl_ni("GlmfInitPlayback"); return 0; }
int WINAPI GlmfPlayGlsRecord(int a) { (void)a; gl_ni("GlmfPlayGlsRecord"); return 0; }
int WINAPI glDebugEntry(int a, int b) { (void)a; (void)b; return 0; }
"""
    lines.append(extra)

    lines.append("typedef struct GlProc { const char *n; PROC p; } GlProc;")
    lines.append("static const GlProc g_procs[] = {")
    all_names = sorted(set(list(protos.keys()) + list(HAND)))
    for name in all_names:
        if name.startswith("Glmf") or name == "glDebugEntry":
            continue
        target = "fw_" + name if name in HAND and fwd_mode(name) in ("B", "F", "H") else name
        lines.append('    { "%s", (PROC)%s },' % (name, target))
    lines.append("};")
    lines.append(
        "PROC gl_lookup_proc(const char *name)\n"
        "{\n"
        "    int i, n = (int)(sizeof(g_procs) / sizeof(g_procs[0]));\n"
        "    char base[128];\n"
        "    size_t len;\n"
        "    if (!name) return NULL;\n"
        "    for (i = 0; i < n; i++)\n"
        "        if (strcmp(g_procs[i].n, name) == 0) return g_procs[i].p;\n"
        "    len = strlen(name);\n"
        "    if (len > 3 && len < sizeof(base)) {\n"
        "        const char *suf = name + len - 3;\n"
        "        if (strcmp(suf, \"ARB\") == 0 || strcmp(suf, \"EXT\") == 0 ||\n"
        "            strcmp(suf, \"NV\") == 0) {\n"
        "            memcpy(base, name, len - 3);\n"
        "            base[len - 3] = 0;\n"
        "            for (i = 0; i < n; i++)\n"
        "                if (strcmp(g_procs[i].n, base) == 0) return g_procs[i].p;\n"
        "        }\n"
        "    }\n"
        "    return NULL;\n"
        "}\n"
    )

    stub_path = os.path.join(ROOT, "gl_stubs.c")
    with open(stub_path, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(lines))
        f.write("\n")

    def_names = []
    def_names.extend(sys_exports)
    def_names.extend(sorted(n for n in protos if n.startswith("gl") or n.startswith("wgl")))
    # unique preserve order
    seen = set()
    ordered = []
    for n in def_names:
        if n not in seen:
            seen.add(n)
            ordered.append(n)

    def_path = os.path.join(ROOT, "opengl32.def")
    with open(def_path, "w", encoding="utf-8", newline="\n") as f:
        f.write("LIBRARY opengl32\nEXPORTS\n")
        for n in ordered:
            if n in HAND and fwd_mode(n) in ("B", "F", "H"):
                f.write("%s=fw_%s\n" % (n, n))
            else:
                f.write("%s\n" % n)
    gen_forwarding(protos)

    print(
        "wrote %s (%d stubs, %d hand) and %s (%d exports)"
        % (stub_path, len(stub_names), len(hand_in_headers), def_path, len(ordered))
    )
    missing_hand = sorted(HAND - set(protos) - set(sys_exports))
    if missing_hand:
        print("hand names not in headers (ok if we declare them):", ", ".join(missing_hand))
    return 0


if __name__ == "__main__":
    sys.exit(main())
