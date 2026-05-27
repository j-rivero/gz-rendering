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
#ifndef GZ_RENDERING_O3DE_O3DEGLINTEROP_HH_
#define GZ_RENDERING_O3DE_O3DEGLINTEROP_HH_

// M4 step 2b helpers. Compiled as a normal gz-rendering TU (NOT the Atom TU):
// it includes EGL/GL headers and must never see Atom/AzCore. It talks to the
// backend only through the plain-C++ O3deBackend interface. Kept behind
// GZ_O3DE_INTEROP so non-interop builds neither compile nor link GL/EGL.
namespace gz
{
  namespace rendering
  {
    /// \brief Headless self-test of the Vulkan->GL zero-copy import path.
    ///
    /// Fetches the exportable interop image (O3deBackend::GetInteropImport),
    /// creates a private EGL/desktop-GL context, imports the FD as a GL texture
    /// (GL_EXT_memory_object_fd), reads the texels back, and compares them to the
    /// deterministic gradient the backend uploaded. Logs a PASS/FAIL summary.
    /// Self-contained: it creates and tears down its own GL context, so it may be
    /// run on any thread (the backend's render thread at bootstrap is fine).
    /// \return True if the GL-imported texels match the uploaded gradient.
    bool RunO3deInteropGlSelfTest();
  }
}
#endif
