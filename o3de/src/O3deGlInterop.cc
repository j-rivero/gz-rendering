/*
 * Copyright (C) 2024 Open Source Robotics Foundation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 */

// M4 step 2b: prove the Vulkan->GL zero-copy import. This is an ordinary
// gz-rendering TU (normal flags); it must NOT include any Atom/AzCore header.
// It reaches the backend only through the plain-C++ O3deBackend interface.

#include "O3deGlInterop.hh"

#if !defined(GZ_O3DE_INTEROP_BUILD)
// Patch-free / non-interop build: no GL/EGL dependency, self-test is a no-op.
namespace gz { namespace rendering {
bool RunO3deInteropGlSelfTest() { return false; }
}}  // namespace gz::rendering
#else

#include "O3deBackend.hh"

#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <vector>

#include <unistd.h>

#define GL_GLEXT_PROTOTYPES 1
#include <EGL/egl.h>
#include <GL/gl.h>
#include <GL/glext.h>

// GL_EXT_memory_object{,_fd} enums (define defensively in case glext.h is older).
#ifndef GL_HANDLE_TYPE_OPAQUE_FD_EXT
#define GL_HANDLE_TYPE_OPAQUE_FD_EXT 0x9586
#endif
#ifndef GL_TEXTURE_TILING_EXT
#define GL_TEXTURE_TILING_EXT 0x9580
#endif
#ifndef GL_OPTIMAL_TILING_EXT
#define GL_OPTIMAL_TILING_EXT 0x9584
#endif
#ifndef GL_LINEAR_TILING_EXT
#define GL_LINEAR_TILING_EXT 0x9585
#endif

namespace gz
{
namespace rendering
{

namespace
{
  // Resolve a GL entry point through EGL; logs and returns null on failure.
  template <typename Fn>
  Fn GlProc(const char *_name)
  {
    auto p = reinterpret_cast<Fn>(eglGetProcAddress(_name));
    if (!p)
      std::fprintf(stderr, "[gz-o3de] gltest: missing GL entry point %s\n", _name);
    return p;
  }

  bool GlError(const char *_where)
  {
    bool any = false;
    for (GLenum e = glGetError(); e != GL_NO_ERROR; e = glGetError())
    {
      std::fprintf(stderr, "[gz-o3de] gltest: GL error 0x%04x at %s\n", e, _where);
      any = true;
    }
    return any;
  }
}  // namespace

bool RunO3deInteropGlSelfTest()
{
  O3deInteropImport import;
  if (!O3deBackend::Instance().GetInteropImport(import))
  {
    std::fprintf(stderr,
        "[gz-o3de] gltest: GetInteropImport() returned false (interop not "
        "built/enabled, or image not ready) -- skipping GL self-test\n");
    return false;
  }
  std::fprintf(stderr,
      "[gz-o3de] gltest: importing fd=%d %ux%u allocSize=%llu offset=%llu\n",
      import.fd, import.width, import.height,
      static_cast<unsigned long long>(import.allocationSize),
      static_cast<unsigned long long>(import.allocationOffset));

  // ---- Private EGL desktop-GL context (surfaceless, pbuffer fallback) ----
  EGLDisplay dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
  if (dpy == EGL_NO_DISPLAY || !eglInitialize(dpy, nullptr, nullptr))
  {
    std::fprintf(stderr, "[gz-o3de] gltest: eglInitialize failed\n");
    ::close(import.fd);
    return false;
  }
  eglBindAPI(EGL_OPENGL_API);
  const EGLint cfgAttr[] = {
      EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
      EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
      EGL_NONE};
  EGLConfig cfg = nullptr;
  EGLint numCfg = 0;
  eglChooseConfig(dpy, cfgAttr, &cfg, 1, &numCfg);
  const EGLint ctxAttr[] = {
      EGL_CONTEXT_MAJOR_VERSION, 4,
      EGL_CONTEXT_MINOR_VERSION, 5,
      EGL_NONE};
  EGLContext ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ctxAttr);
  if (ctx == EGL_NO_CONTEXT)
  {
    std::fprintf(stderr, "[gz-o3de] gltest: eglCreateContext failed\n");
    eglTerminate(dpy);
    ::close(import.fd);
    return false;
  }
  EGLSurface surf = EGL_NO_SURFACE;
  if (!eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx))
  {
    // No EGL_KHR_surfaceless_context: fall back to a tiny pbuffer.
    const EGLint pbAttr[] = {EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE};
    surf = eglCreatePbufferSurface(dpy, cfg, pbAttr);
    if (surf == EGL_NO_SURFACE || !eglMakeCurrent(dpy, surf, surf, ctx))
    {
      std::fprintf(stderr, "[gz-o3de] gltest: eglMakeCurrent failed\n");
      eglDestroyContext(dpy, ctx);
      eglTerminate(dpy);
      ::close(import.fd);
      return false;
    }
  }
  std::fprintf(stderr, "[gz-o3de] gltest: GL %s / %s\n",
      reinterpret_cast<const char *>(glGetString(GL_VERSION)),
      reinterpret_cast<const char *>(glGetString(GL_RENDERER)));

  // ---- Resolve the import entry points ----
  auto glCreateMemoryObjectsEXT_ =
      GlProc<PFNGLCREATEMEMORYOBJECTSEXTPROC>("glCreateMemoryObjectsEXT");
  auto glImportMemoryFdEXT_ =
      GlProc<PFNGLIMPORTMEMORYFDEXTPROC>("glImportMemoryFdEXT");
  auto glCreateTextures_ =
      GlProc<PFNGLCREATETEXTURESPROC>("glCreateTextures");
  auto glTextureParameteri_ =
      GlProc<PFNGLTEXTUREPARAMETERIPROC>("glTextureParameteri");
  auto glTextureStorageMem2DEXT_ =
      GlProc<PFNGLTEXTURESTORAGEMEM2DEXTPROC>("glTextureStorageMem2DEXT");
  auto glGetTextureImage_ =
      GlProc<PFNGLGETTEXTUREIMAGEPROC>("glGetTextureImage");

  bool ok = glCreateMemoryObjectsEXT_ && glImportMemoryFdEXT_ &&
      glCreateTextures_ && glTextureParameteri_ && glTextureStorageMem2DEXT_ &&
      glGetTextureImage_;

  std::vector<uint8_t> pixels;
  if (ok)
  {
    // ---- Import the FD as GL memory + create the aliasing texture ----
    // glImportMemoryFdEXT takes ownership of the fd (GL closes it), so we must
    // not close import.fd afterwards on the success path.
    GLuint mem = 0;
    glCreateMemoryObjectsEXT_(1, &mem);
    glImportMemoryFdEXT_(mem, import.allocationSize,
        GL_HANDLE_TYPE_OPAQUE_FD_EXT, import.fd);
    ok = !GlError("glImportMemoryFdEXT");

    GLuint tex = 0;
    glCreateTextures_(GL_TEXTURE_2D, 1, &tex);
    // The Atom image is optimal-tiled; GL must import with the matching tiling.
    glTextureParameteri_(tex, GL_TEXTURE_TILING_EXT, GL_OPTIMAL_TILING_EXT);
    glTextureStorageMem2DEXT_(tex, 1, GL_RGBA8,
        static_cast<GLsizei>(import.width), static_cast<GLsizei>(import.height),
        mem, import.allocationOffset);
    ok = ok && !GlError("glTextureStorageMem2DEXT");

    // ---- Read the texels back and compare to the uploaded gradient ----
    pixels.assign(static_cast<size_t>(import.width) * import.height * 4u, 0u);
    glGetTextureImage_(tex, 0, GL_RGBA, GL_UNSIGNED_BYTE,
        static_cast<GLsizei>(pixels.size()), pixels.data());
    ok = ok && !GlError("glGetTextureImage");
  }

  bool match = false;
  if (ok)
  {
    // Expected: R=x, G=y, B=128, A=255 (see ProveFdExportOnce upload).
    size_t good = 0;
    size_t total = 0;
    int firstBadX = -1, firstBadY = -1;
    for (uint32_t y = 0u; y < import.height; ++y)
    {
      for (uint32_t x = 0u; x < import.width; ++x)
      {
        const uint8_t *px =
            &pixels[(static_cast<size_t>(y) * import.width + x) * 4u];
        const bool pixelOk = px[0] == static_cast<uint8_t>(x) &&
            px[1] == static_cast<uint8_t>(y) && px[2] == 128u && px[3] == 255u;
        if (pixelOk)
          ++good;
        else if (firstBadX < 0)
        {
          firstBadX = static_cast<int>(x);
          firstBadY = static_cast<int>(y);
        }
        ++total;
      }
    }
    match = (good == total);
    const uint8_t *s = &pixels[0];
    const uint8_t *m = &pixels[(static_cast<size_t>(import.height / 2u) *
        import.width + import.width / 2u) * 4u];
    std::fprintf(stderr,
        "[gz-o3de] gltest: %zu/%zu texels matched the gradient. "
        "sample(0,0)=[%u %u %u %u] expect[0 0 128 255]; "
        "center=[%u %u %u %u]\n",
        good, total, s[0], s[1], s[2], s[3], m[0], m[1], m[2], m[3]);
    if (!match)
      std::fprintf(stderr,
          "[gz-o3de] gltest: first mismatch at (%d,%d)\n", firstBadX, firstBadY);
  }

  std::fprintf(stderr, "[gz-o3de] gltest: %s -- Vulkan->GL zero-copy import %s\n",
      match ? "PASS" : "FAIL", match ? "verified" : "did NOT verify");

  // Optional: dump the texels we read *through the GL import* to a PPM, so the
  // result of the zero-copy path is a viewable artifact (no screenshot needed).
  if (const char *dumpPath = std::getenv("GZ_O3DE_INTEROP_GLDUMP"))
  {
    if (!pixels.empty())
    {
      if (FILE *fp = std::fopen(dumpPath, "wb"))
      {
        std::fprintf(fp, "P6\n%u %u\n255\n", import.width, import.height);
        std::vector<uint8_t> rgb(static_cast<size_t>(import.width) *
            import.height * 3u);
        for (size_t i = 0, n = static_cast<size_t>(import.width) * import.height;
            i < n; ++i)
        {
          rgb[i * 3u + 0u] = pixels[i * 4u + 0u];
          rgb[i * 3u + 1u] = pixels[i * 4u + 1u];
          rgb[i * 3u + 2u] = pixels[i * 4u + 2u];
        }
        std::fwrite(rgb.data(), 1u, rgb.size(), fp);
        std::fclose(fp);
        std::fprintf(stderr,
            "[gz-o3de] gltest: wrote GL-imported image to %s\n", dumpPath);
      }
    }
  }

  // ---- Teardown ----
  eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
  if (surf != EGL_NO_SURFACE)
    eglDestroySurface(dpy, surf);
  eglDestroyContext(dpy, ctx);
  eglTerminate(dpy);
  // On failure paths before glImportMemoryFdEXT consumed it, the fd may leak; we
  // accept that for a one-shot self-test. (Success: GL owns it.)
  return match;
}

}  // namespace rendering
}  // namespace gz

#endif  // GZ_O3DE_INTEROP_BUILD
