# Collect source files into the "sources" variable and unit test files into the
# "gtest_sources" variable.
gz_get_libsources_and_unittests(sources gtest_sources)

set(engine_name "o3de")

gz_add_component(${engine_name} SOURCES ${sources} GET_TARGET_NAME o3de_target)

target_link_libraries(${o3de_target}
  PUBLIC
    ${gz-common_LIBRARIES}
    gz-math::eigen3
  PRIVATE
    gz-plugin::register)

#============================================================================
# Embedded O3DE/Atom runtime
#
# All Atom/AzCore code lives in the single TU O3deBackend.cc, which must be
# compiled with O3DE's exact compile model (exceptions off, hidden visibility,
# C++20, O3DE defines) and linked against the prebuilt O3DE libraries. The rest
# of the component keeps the normal gz-rendering flags and only sees the plain
# O3deBackend interface.
#
# NOTE (PoC): O3DE is consumed as a *prebuilt* tree under vendor/o3de (built
# out-of-band via Ninja Multi-Config). We do not add_subdirectory() it: O3DE's
# engine-registration + gem build system is incompatible with this single-config
# build. A later milestone will formalise discovery/versioning of these paths.
#============================================================================
set(O3DE_ROOT  "${CMAKE_SOURCE_DIR}/vendor/o3de")
set(O3DE_BUILD "${O3DE_ROOT}/build/linux")
set(O3DE_BIN   "${O3DE_BUILD}/bin/profile")
set(O3DE_LIB   "${O3DE_BUILD}/lib/profile")
set(O3DE_3P    "$ENV{HOME}/o3de-packages/packages")

# Include surface needed by O3deBackend.cc (Atom RPI/RHI/Feature + AzCore/
# Framework/GameFramework + generated + 3rdParty headers reached transitively).
set(O3DE_BACKEND_INCLUDES
  ${O3DE_ROOT}/Code/Framework/AtomCore
  ${O3DE_ROOT}/Code/Framework/AzCore
  ${O3DE_ROOT}/Code/Framework/AzCore/Platform/Linux
  ${O3DE_ROOT}/Code/Framework/AzCore/Platform/Common
  ${O3DE_ROOT}/Code/Framework/AzFramework
  ${O3DE_ROOT}/Code/Framework/AzFramework/Platform/Common/Xcb
  ${O3DE_ROOT}/Code/Framework/AzFramework/Platform/Linux
  ${O3DE_ROOT}/Code/Framework/AzGameFramework/AzGameFramework/..
  ${O3DE_ROOT}/Code/Framework/AzNetworking
  ${O3DE_ROOT}/Code/Framework/AzNetworking/Platform/Common
  ${O3DE_ROOT}/Code/Framework/AzNetworking/Platform/Linux
  ${O3DE_BUILD}/Code/Framework/AzNetworking/Azcg/Generated/AzNetworking
  ${O3DE_ROOT}/Code/Legacy/CryCommon
  ${O3DE_ROOT}/Gems/Atom/RPI/Code/Include
  ${O3DE_ROOT}/Gems/Atom/RPI/Code/External
  ${O3DE_ROOT}/Gems/Atom/RHI/Code/Include
  ${O3DE_ROOT}/Gems/Atom/Feature/Common/Code/Include
  ${O3DE_ROOT}/Gems/Atom/Feature/Common/Code/3rdParty/ACES
  ${O3DE_ROOT}/Gems/Atom/Utils/Code/Include
  ${O3DE_ROOT}/Gems/Atom/Bootstrap/Code/Include
  ${O3DE_ROOT}/Gems/AtomLyIntegration/AtomBridge/Code/Include)

set(O3DE_BACKEND_SYSTEM_INCLUDES
  ${O3DE_3P}/RapidJSON-1.1.0-rev1-multiplatform/RapidJSON/include
  ${O3DE_3P}/RapidXML-1.13-rev1-multiplatform/RapidXML/include
  ${O3DE_3P}/cityhash-1.1-multiplatform/cityhash/src
  ${O3DE_3P}/zlib-1.2.11-rev5-linux/zlib/include
  ${O3DE_3P}/lz4-1.9.4-rev2-linux/lz4/include
  ${O3DE_3P}/Lua-5.4.4-rev1-linux/Lua/include)

# ABI-critical: these must match how the prebuilt O3DE libs were compiled, or
# the AZ allocator/reflection machinery misbehaves at runtime.
set(O3DE_BACKEND_DEFINES
  AZ_BUILD_CONFIGURATION_TYPE="profile"
  AZ_ENABLE_DEBUG_TOOLS
  AZ_ENABLE_TRACING
  AZ_PROFILE_BUILD
  CMAKE_INTDIR="profile"
  _HAS_EXCEPTIONS=0
  LINUX
  LINUX64
  NDEBUG
  O3DE_DISABLE_CONDITIONAL_EXPLICIT=1
  PAL_TRAIT_LINUX_WINDOW_MANAGER_XCB
  _PROFILE)

set_source_files_properties(${CMAKE_CURRENT_SOURCE_DIR}/O3deBackend.cc
  PROPERTIES
    INCLUDE_DIRECTORIES   "${O3DE_BACKEND_INCLUDES};${O3DE_BACKEND_SYSTEM_INCLUDES}"
    COMPILE_DEFINITIONS   "${O3DE_BACKEND_DEFINES}"
    # -std=c++20 last on the command line wins over the project-wide C++17.
    COMPILE_OPTIONS       "-std=c++20;-fno-exceptions;-fvisibility=hidden")

# Link the prebuilt O3DE libraries. AzGameFramework is whole-archived so its
# GameApplication static-module self-registration survives; the rest are wrapped
# in a group to resolve the static/shared cross-references regardless of order.
target_link_libraries(${o3de_target}
  PRIVATE
    -Wl,--whole-archive ${O3DE_LIB}/libAzGameFramework.a -Wl,--no-whole-archive
    -Wl,--start-group
      ${O3DE_LIB}/libAtomCore.a
      ${O3DE_LIB}/libAtom_Utils.Static.a
      ${O3DE_LIB}/libAtom_AtomBridge.Static.a
      ${O3DE_LIB}/libAzNetworking.a
      ${O3DE_BIN}/libAtom_RPI.Public.so
      ${O3DE_BIN}/libAtom_Feature_Common.Public.so
      ${O3DE_BIN}/libAtom_RHI.Public.so
      ${O3DE_BIN}/libAtom_RHI.Reflect.so
      ${O3DE_BIN}/libAzFramework.so
      ${O3DE_BIN}/libAzFramework.NativeUI.so
      ${O3DE_BIN}/libAzCore.so
    -Wl,--end-group)

# Find the O3DE shared libs at runtime (PoC: point straight at the build tree).
set_target_properties(${o3de_target} PROPERTIES
  BUILD_RPATH   "${O3DE_BIN}"
  INSTALL_RPATH "${O3DE_BIN}")

# Note that plugins are currently being installed in 2 places:
# /lib and the engine-plugins dir
install(TARGETS ${o3de_target}
  DESTINATION ${GZ_RENDERING_ENGINE_RELATIVE_INSTALL_DIR})

# Build the unit tests
gz_build_tests(TYPE UNIT
               SOURCES ${gtest_sources}
               LIB_DEPS ${o3de_target}
               ENVIRONMENT GZ_RENDERING_INSTALL_PREFIX=${CMAKE_INSTALL_PREFIX})
