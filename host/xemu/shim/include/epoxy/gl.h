#ifndef XBRT_SHIM_EPOXY_GL_H
#define XBRT_SHIM_EPOXY_GL_H

typedef unsigned int GLuint, GLenum;
typedef int GLint, GLsizei;
typedef void GLvoid;

#define GL_TEXTURE_2D 0x0DE1
#define GL_RGBA 0x1908
#define GL_UNSIGNED_BYTE 0x1401
#define GL_TEXTURE_MIN_FILTER 0x2801
#define GL_TEXTURE_MAG_FILTER 0x2800
#define GL_NEAREST 0x2600

void glGenTextures(GLsizei n, GLuint *textures);
void glBindTexture(GLenum target, GLuint texture);
void glTexImage2D(GLenum target, GLint level, GLint internalformat, GLsizei width, GLsizei height, GLint border,
                  GLenum format, GLenum type, const GLvoid *pixels);
void glTexParameteri(GLenum target, GLenum pname, GLint param);
void glDeleteTextures(GLsizei n, const GLuint *textures);

#endif
