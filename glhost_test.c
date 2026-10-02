/* Drives x86\opengl32.dll the way Haydee does - shaders, client-memory
 * arrays, a VBO with client indices, an FBO sampled afterwards, readback -
 * so glhost64.exe can be checked without the game.
 *
 *   glhost_test.exe [frames]    prints the centre and corner pixels */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <GL/gl.h>
#include <GL/glext.h>
#include <stdio.h>
#include <stdlib.h>

#define F(ret, name, args) typedef ret(APIENTRY *T_##name) args; static T_##name name##_;
F(void, glClearColor, (GLfloat, GLfloat, GLfloat, GLfloat))
F(void, glClear, (GLbitfield))
F(void, glViewport, (GLint, GLint, GLsizei, GLsizei))
F(GLuint, glCreateShader, (GLenum))
F(void, glShaderSource, (GLuint, GLsizei, const GLchar *const *, const GLint *))
F(void, glCompileShader, (GLuint))
F(GLuint, glCreateProgram, (void))
F(void, glAttachShader, (GLuint, GLuint))
F(void, glLinkProgram, (GLuint))
F(void, glUseProgram, (GLuint))
F(GLint, glGetUniformLocation, (GLuint, const GLchar *))
F(void, glUniform4f, (GLint, GLfloat, GLfloat, GLfloat, GLfloat))
F(void, glUniform1i, (GLint, GLint))
F(void, glEnableVertexAttribArray, (GLuint))
F(void, glDisableVertexAttribArray, (GLuint))
F(void, glVertexAttribPointer, (GLuint, GLint, GLenum, GLboolean, GLsizei, const void *))
F(void, glDrawArrays, (GLenum, GLint, GLsizei))
F(void, glDrawElements, (GLenum, GLsizei, GLenum, const void *))
F(void, glGenBuffers, (GLsizei, GLuint *))
F(void, glBindBuffer, (GLenum, GLuint))
F(void, glBufferData, (GLenum, GLsizeiptr, const void *, GLenum))
F(void, glGenTextures, (GLsizei, GLuint *))
F(void, glBindTexture, (GLenum, GLuint))
F(void, glTexImage2D, (GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void *))
F(void, glTexParameteri, (GLenum, GLenum, GLint))
F(void, glGenFramebuffers, (GLsizei, GLuint *))
F(void, glBindFramebuffer, (GLenum, GLuint))
F(void, glFramebufferTexture2D, (GLenum, GLenum, GLenum, GLuint, GLint))
F(void, glReadPixels, (GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void *))
F(void, glActiveTexture, (GLenum))
F(GLenum, glGetError, (void))

typedef int(WINAPI *T_cpf)(HDC, const PIXELFORMATDESCRIPTOR *);
typedef BOOL(WINAPI *T_spf)(HDC, int, const PIXELFORMATDESCRIPTOR *);
typedef HGLRC(WINAPI *T_cc)(HDC);
typedef BOOL(WINAPI *T_mc)(HDC, HGLRC);
typedef BOOL(WINAPI *T_sb)(HDC);
typedef PROC(WINAPI *T_gpa)(LPCSTR);

static const char *vs_src =
	"#version 330 core\n"
	"layout(location = 0) in vec2 v_pos;\n"
	"layout(location = 1) in vec2 v_uv;\n"
	"out vec2 f_uv;\n"
	"void main() { f_uv = v_uv; gl_Position = vec4(v_pos, 0.0, 1.0); }\n";
static const char *fs_col =
	"#version 330 core\n"
	"uniform vec4 tint;\n"
	"in vec2 f_uv;\n"
	"out vec4 o_Color;\n"
	"void main() { o_Color = tint * vec4(f_uv, 1.0 - f_uv.x, 1.0); }\n";
static const char *fs_tex =
	"#version 330 core\n"
	"uniform sampler2D img;\n"
	"in vec2 f_uv;\n"
	"out vec4 o_Color;\n"
	"void main() { o_Color = texture(img, f_uv); }\n";

static LRESULT CALLBACK wp(HWND h, UINT m, WPARAM w, LPARAM l)
{
	return DefWindowProcA(h, m, w, l);
}

static GLuint prog(const char *vs, const char *fs)
{
	GLuint v = glCreateShader_(GL_VERTEX_SHADER), f = glCreateShader_(GL_FRAGMENT_SHADER);
	GLuint p = glCreateProgram_();
	glShaderSource_(v, 1, &vs, NULL);
	glCompileShader_(v);
	glShaderSource_(f, 1, &fs, NULL);
	glCompileShader_(f);
	glAttachShader_(p, v);
	glAttachShader_(p, f);
	glLinkProgram_(p);
	return p;
}

int main(int argc, char **argv)
{
	HMODULE gl = LoadLibraryA("x86\\opengl32.dll");
	T_gpa gpa;
	WNDCLASSA wc = { 0 };
	HWND hwnd;
	HDC dc;
	PIXELFORMATDESCRIPTOR pfd = { sizeof(pfd), 1 };
	HGLRC rc;
	GLuint p_col, p_tex, vbo, tex, fbo;
	int frames = argc > 1 ? atoi(argv[1]) : 120, i;
	static const float tri[] = { -0.9f, -0.9f, 0, 0, 0.9f, -0.9f, 1, 0, 0, 0.9f, 0.5f, 1 };
	static const float quad[] = { -1, -1, 0, 0, 1, -1, 1, 0, 1, 1, 1, 1, -1, 1, 0, 1 };
	static const unsigned short qidx[] = { 0, 1, 2, 0, 2, 3 };
	unsigned char px[4], corner[4];

	if (!gl) {
		printf("no x86\\opengl32.dll\n");
		return 1;
	}
	gpa = (T_gpa)(void *)GetProcAddress(gl, "wglGetProcAddress");
#define L(name) name##_ = (T_##name)(void *)gpa(#name)
	L(glClearColor); L(glClear); L(glViewport); L(glCreateShader); L(glShaderSource);
	L(glCompileShader); L(glCreateProgram); L(glAttachShader); L(glLinkProgram);
	L(glUseProgram); L(glGetUniformLocation); L(glUniform4f); L(glUniform1i);
	L(glEnableVertexAttribArray); L(glDisableVertexAttribArray); L(glVertexAttribPointer);
	L(glDrawArrays); L(glDrawElements); L(glGenBuffers); L(glBindBuffer); L(glBufferData);
	L(glGenTextures); L(glBindTexture); L(glTexImage2D); L(glTexParameteri);
	L(glGenFramebuffers); L(glBindFramebuffer); L(glFramebufferTexture2D); L(glReadPixels);
	L(glActiveTexture); L(glGetError);

	wc.lpfnWndProc = wp;
	wc.hInstance = GetModuleHandleA(NULL);
	wc.lpszClassName = "glhost_test";
	RegisterClassA(&wc);
	hwnd = CreateWindowA("glhost_test", "glhost test", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 100, 100,
			     656, 519, NULL, NULL, wc.hInstance, NULL);
	if (argc > 2)
		SetWindowPos(hwnd, HWND_TOPMOST, 100, 100, 656, 519, SWP_SHOWWINDOW);
	dc = GetDC(hwnd);
	pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
	pfd.cColorBits = 32;
	((T_spf)(void *)GetProcAddress(gl, "wglSetPixelFormat"))(
		dc, ((T_cpf)(void *)GetProcAddress(gl, "wglChoosePixelFormat"))(dc, &pfd), &pfd);
	rc = ((T_cc)(void *)GetProcAddress(gl, "wglCreateContext"))(dc);
	((T_mc)(void *)GetProcAddress(gl, "wglMakeCurrent"))(dc, rc);

	p_col = prog(vs_src, fs_col);
	p_tex = prog(vs_src, fs_tex);
	glGenBuffers_(1, &vbo);
	glBindBuffer_(GL_ARRAY_BUFFER, vbo);
	glBufferData_(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);
	glBindBuffer_(GL_ARRAY_BUFFER, 0);
	glGenTextures_(1, &tex);
	glBindTexture_(GL_TEXTURE_2D, tex);
	glTexImage2D_(GL_TEXTURE_2D, 0, GL_RGBA8, 256, 256, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
	glTexParameteri_(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri_(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glGenFramebuffers_(1, &fbo);
	glBindFramebuffer_(GL_FRAMEBUFFER, fbo);
	glFramebufferTexture2D_(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);

	for (i = 0; i < frames; i++) {
		MSG m;
		float t = (float)i / (float)(frames > 1 ? frames - 1 : 1);
		while (PeekMessageA(&m, NULL, 0, 0, PM_REMOVE))
			DispatchMessageA(&m);
		/* 1: a triangle from client memory into the FBO. */
		glBindFramebuffer_(GL_FRAMEBUFFER, fbo);
		glViewport_(0, 0, 256, 256);
		glClearColor_(0.1f, 0.1f, 0.1f, 1);
		glClear_(GL_COLOR_BUFFER_BIT);
		glUseProgram_(p_col);
		glUniform4f_(glGetUniformLocation_(p_col, "tint"), 1, 1, t, 1);
		glEnableVertexAttribArray_(0);
		glEnableVertexAttribArray_(1);
		glVertexAttribPointer_(0, 2, GL_FLOAT, GL_FALSE, 16, tri);
		glVertexAttribPointer_(1, 2, GL_FLOAT, GL_FALSE, 16, tri + 2);
		glDrawArrays_(GL_TRIANGLES, 0, 3);
		/* 2: the FBO texture on a quad from a VBO, client indices. */
		glBindFramebuffer_(GL_FRAMEBUFFER, 0);
		glViewport_(0, 0, 640, 480);
		glClearColor_(0.2f, 0.3f, 0.8f, 1);
		glClear_(GL_COLOR_BUFFER_BIT);
		glUseProgram_(p_tex);
		glActiveTexture_(GL_TEXTURE0);
		glBindTexture_(GL_TEXTURE_2D, tex);
		glUniform1i_(glGetUniformLocation_(p_tex, "img"), 0);
		glBindBuffer_(GL_ARRAY_BUFFER, vbo);
		glVertexAttribPointer_(0, 2, GL_FLOAT, GL_FALSE, 16, (const void *)0);
		glVertexAttribPointer_(1, 2, GL_FLOAT, GL_FALSE, 16, (const void *)8);
		glBindBuffer_(GL_ARRAY_BUFFER, 0);
		glViewport_(80, 60, 480, 360);
		glDrawElements_(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, qidx);
		glViewport_(0, 0, 640, 480);
		if (i == frames - 1) {
			glReadPixels_(320, 200, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
			glReadPixels_(5, 5, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, corner);
			printf("centre %u %u %u %u, corner %u %u %u %u, GL error 0x%X\n", px[0], px[1],
			       px[2], px[3], corner[0], corner[1], corner[2], corner[3],
			       glGetError_());
		}
		((T_sb)(void *)GetProcAddress(gl, "wglSwapBuffers"))(dc);
		if (argc > 2)
			Sleep(10);
	}
	return 0;
}
