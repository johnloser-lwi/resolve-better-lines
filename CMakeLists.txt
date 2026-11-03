cmake_minimum_required(VERSION 3.24)

# macOS: universal arm64 + x86_64, set BEFORE project() because that is when
# CMake reads them. Resolve runs native arm64 on Apple Silicon and x86_64 on
# Intel, and a single-slice bundle is another way to be silently not listed --
# the host loads the slice matching its own process and says nothing when none
# does. The deployment target is set low so it is never what rejects a machine.
if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Darwin")
    option(BETTERLINE_UNIVERSAL "Build a universal arm64 + x86_64 binary" ON)
    if(BETTERLINE_UNIVERSAL AND NOT CMAKE_OSX_ARCHITECTURES)
        set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "" FORCE)
    endif()
    if(NOT CMAKE_OSX_DEPLOYMENT_TARGET)
        set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "" FORCE)
    endif()
endif()

# CUDA is deliberately NOT listed here. Naming it in project(LANGUAGES ...)
# enables the language immediately, and enabling CUDA installs a default
# CMAKE_CUDA_ARCHITECTURES -- after which any `if(NOT DEFINED ...)` guard
# silently never fires and the plugin ships PTX-only code the driver must JIT at
# load. The architectures must be chosen BEFORE enable_language(CUDA) below.
project(BetterLine VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT WIN32 AND NOT APPLE)
    message(FATAL_ERROR "This project targets Windows and macOS.")
endif()

# The Cocoa side of the platform layer is Objective-C++.
if(APPLE)
    enable_language(OBJCXX)
endif()

# Single-config generators (Makefiles, Ninja -- the usual macOS command line)
# leave the build type empty, and the bundle path below is keyed on $<CONFIG>:
# an empty one would assemble into build//BetterLine.ofx.bundle and a
# debug-flagged, unoptimised build would be what got installed.
get_property(_multi_config GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
if(NOT _multi_config AND NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# An OFX bundle names its payload directory after the platform, and the host
# looks in exactly one place. Get this wrong and the plugin is simply never
# found, with no error from anywhere. macOS uses a single MacOS directory for
# every slice, universal or not -- NOT MacOS-x86-64, which Resolve ignores.
if(WIN32)
    set(OFX_ARCH_DIR "Win64")
else()
    set(OFX_ARCH_DIR "MacOS")
endif()

# Static CRT. An OFX bundle is loaded by a host that will not have our
# redistributables installed, and a missing VCRUNTIME dll fails as a *silent
# no-load*: Resolve simply never lists the plugin, with no error anywhere.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# --- CUDA --------------------------------------------------------------------
# Set architectures first, then enable. Real SASS for Turing/Ampere/Ada/Blackwell
# plus virtual PTX so architectures newer than this toolkit still run.
# CUDA 13 dropped everything below Turing, so there is no point listing 60/70.
# No CUDA on Apple, at any version -- the toolkit was dropped after macOS 10.13
# and Apple Silicon never had it. macOS therefore runs the CPU path: the plugin
# declares no GPU support there, so Resolve hands it CPU buffers. A Metal path
# is the macOS counterpart and is NOT written yet (see README).
if(APPLE)
    set(BETTERLINE_CUDA OFF CACHE BOOL "Build the CUDA render path" FORCE)
endif()
option(BETTERLINE_CUDA "Build the CUDA render path" ON)
set(BETTERLINE_HAVE_CUDA OFF)
if(BETTERLINE_CUDA)
    set(CMAKE_CUDA_ARCHITECTURES 75 86 89 120 120-virtual)
    include(CheckLanguage)
    check_language(CUDA)
    if(CMAKE_CUDA_COMPILER)
        enable_language(CUDA)
        set(CMAKE_CUDA_STANDARD 17)
        set(CMAKE_CUDA_STANDARD_REQUIRED ON)
        set(BETTERLINE_HAVE_CUDA ON)
        message(STATUS "CUDA enabled, architectures: ${CMAKE_CUDA_ARCHITECTURES}")
    else()
        message(WARNING "No CUDA compiler found - building the CPU path only. "
                        "Resolve's pipeline is GPU-resident, so this build will be slow.")
    endif()
endif()

set(OFX_ROOT     "${CMAKE_SOURCE_DIR}/third_party/openfx")
set(OFX_INCLUDE  "${OFX_ROOT}/OpenFX-1.4/include")
set(OFX_SUPPORT  "${OFX_ROOT}/Support")

# --- OFX C++ support library -------------------------------------------------
# Vendored from C:\ProgramData\Blackmagic Design\DaVinci Resolve\Support\Developer
# (ProgramData, not Program Files). Blackmagic's copy is used rather than upstream
# OpenFX specifically because it BINDS OFX::Private::gDrawSuite -- upstream ships
# ofxDrawSuite.h but never fetches it, which forces a plugin to wrap the interact
# main entry by hand just to steal the draw context out of the draw action's
# inArgs. Vendoring this copy deletes that whole shim.
#
# ofxsHWNDInteract.cpp is deliberately excluded: it references symbols
# (gHWNDInteractSuite, kOfxHWndInteract*) that are declared nowhere in the SDK, so
# it does not compile. Blackmagic's own Makefile omits it too. It is a vestigial
# Foundry extension, not a Resolve feature -- do not try to revive it.
add_library(ofxsupport STATIC
    "${OFX_SUPPORT}/Library/ofxsCore.cpp"
    "${OFX_SUPPORT}/Library/ofxsImageEffect.cpp"
    "${OFX_SUPPORT}/Library/ofxsInteract.cpp"
    "${OFX_SUPPORT}/Library/ofxsLog.cpp"
    "${OFX_SUPPORT}/Library/ofxsMultiThread.cpp"
    "${OFX_SUPPORT}/Library/ofxsParams.cpp"
    "${OFX_SUPPORT}/Library/ofxsProperty.cpp"
    "${OFX_SUPPORT}/Library/ofxsPropertyValidation.cpp"
)
target_include_directories(ofxsupport PUBLIC "${OFX_INCLUDE}" "${OFX_SUPPORT}/include")
# Linked into a loadable module, so it must be position-independent everywhere.
set_target_properties(ofxsupport PROPERTIES POSITION_INDEPENDENT_CODE ON)
if(WIN32)
    target_compile_definitions(ofxsupport PUBLIC NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
endif()

# --- CUDA render path --------------------------------------------------------
# Its own library so the plugin and the parity test link the exact same compiled
# kernel rather than two separate builds of it. Two builds of "the same" kernel
# is precisely the situation the parity test exists to rule out.
if(BETTERLINE_HAVE_CUDA)
    add_library(blcuda STATIC src/render/CudaKernel.cu)
    target_include_directories(blcuda PUBLIC src)
    set_target_properties(blcuda PROPERTIES
        CUDA_SEPARABLE_COMPILATION OFF
        CUDA_RUNTIME_LIBRARY Static
        POSITION_INDEPENDENT_CODE ON
    )
    # Deliberately NOT --use_fast_math. The whole point of the shared header is
    # that the CPU and GPU agree; fast-math lets nvcc substitute lower-precision
    # intrinsics on one side of that pair only, and the parity test would start
    # failing for a reason that has nothing to do with the code.
endif()

# --- The plugin --------------------------------------------------------------
# src/core is header-only and includes no OFX header, so the offline harness
# links the identical code Resolve runs.
add_library(BetterLine MODULE
    src/BetterLinePlugin.cpp
    src/HostProbe.cpp
    src/Preset.cpp
    src/PresetIO.cpp
    src/core/SvgPath.cpp
    src/interact/Overlay.cpp
)
target_include_directories(BetterLine PRIVATE src)
target_link_libraries(BetterLine PRIVATE ofxsupport)

# The OS layer: one implementation per platform behind src/platform/Platform.h.
if(WIN32)
    target_sources(BetterLine PRIVATE src/platform/PlatformWin.cpp)
    # shell32/ole32 for IFileDialog and the known folders.
    target_link_libraries(BetterLine PRIVATE shell32 ole32)
else()
    target_sources(BetterLine PRIVATE src/platform/PlatformMac.mm)
    set_source_files_properties(src/platform/PlatformMac.mm PROPERTIES
        COMPILE_OPTIONS "-fobjc-arc")
    target_link_libraries(BetterLine PRIVATE "-framework AppKit" "-framework Foundation")
endif()

if(BETTERLINE_HAVE_CUDA)
    target_link_libraries(BetterLine PRIVATE blcuda)
    target_compile_definitions(BetterLine PRIVATE BETTERLINE_HAVE_CUDA)
endif()

# OFX plugins are DLLs with a .ofx extension and a single exported entry point.
set_target_properties(BetterLine PROPERTIES
    PREFIX ""
    SUFFIX ".ofx"
    OUTPUT_NAME "BetterLine"
)

# --- Offline harness ---------------------------------------------------------
# Renders a path spec straight to PNG with no host in the loop. This is where
# antialiasing quality, caps, dash boundaries and taper continuity get judged.
# Preset.cpp is deliberately free of OFX and Win32, so the harness links it and
# the JSON schema is exercised with no host in the loop.
add_executable(linetest tools/linetest.cpp src/Preset.cpp src/core/SvgPath.cpp)
target_include_directories(linetest PRIVATE src third_party/stb)

# Not everything the harness knows is a picture. The stagger ordering has to be
# a permutation -- two strokes assigned the same turn would animate together,
# which no still image would reveal.
enable_testing()
add_test(NAME stagger_order COMMAND linetest x --sheet check)

# --- CPU/GPU parity ----------------------------------------------------------
# A linetest render is NOT proof the GPU path agrees: it uses the CPU shader.
# In Resolve every frame goes through the kernel, so this is the only thing
# standing between a reassociated float and a bug nobody sees until it ships.
if(BETTERLINE_HAVE_CUDA)
    enable_testing()
    add_executable(cudatest tools/cudatest.cpp)
    target_include_directories(cudatest PRIVATE src)
    target_link_libraries(cudatest PRIVATE blcuda)
    add_test(NAME cuda_parity COMMAND cudatest)
endif()

# --- Assemble the .ofx.bundle ------------------------------------------------
# Per-config path: with a multi-config generator a single shared bundle dir means
# a Debug build silently overwrites the Release one, and the install scripts
# cannot tell which config they are actually shipping.
set(BUNDLE_DIR "${CMAKE_BINARY_DIR}/$<CONFIG>/BetterLine.ofx.bundle")
add_custom_command(TARGET BetterLine POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${BUNDLE_DIR}/Contents/${OFX_ARCH_DIR}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:BetterLine>" "${BUNDLE_DIR}/Contents/${OFX_ARCH_DIR}/"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_SOURCE_DIR}/packaging/Info.plist" "${BUNDLE_DIR}/Contents/"
    COMMENT "Assembling BetterLine.ofx.bundle"
)

# Apple Silicon refuses to load arm64 code with no signature at all, and the
# failure is -- again -- the plugin silently not appearing. The linker normally
# ad-hoc signs arm64 output, but the copy above is what ships, so it is signed
# explicitly. Ad-hoc ("-") is enough for a locally built plugin; distributing
# to other machines needs a Developer ID signature and notarisation instead.
if(APPLE)
    add_custom_command(TARGET BetterLine POST_BUILD
        COMMAND codesign --force --sign - --timestamp=none
                "${BUNDLE_DIR}/Contents/${OFX_ARCH_DIR}/$<TARGET_FILE_NAME:BetterLine>"
        COMMENT "Ad-hoc signing the bundle binary"
    )
endif()
