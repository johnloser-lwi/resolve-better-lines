cmake_minimum_required(VERSION 3.24)

# CUDA is deliberately NOT listed here. Naming it in project(LANGUAGES ...)
# enables the language immediately, and enabling CUDA installs a default
# CMAKE_CUDA_ARCHITECTURES -- after which any `if(NOT DEFINED ...)` guard
# silently never fires and the plugin ships PTX-only code the driver must JIT at
# load. The architectures must be chosen BEFORE enable_language(CUDA) below.
project(BetterLine VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT WIN32)
    message(FATAL_ERROR "This project currently targets Windows only.")
endif()

# Static CRT. An OFX bundle is loaded by a host that will not have our
# redistributables installed, and a missing VCRUNTIME dll fails as a *silent
# no-load*: Resolve simply never lists the plugin, with no error anywhere.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# --- CUDA --------------------------------------------------------------------
# Set architectures first, then enable. Real SASS for Turing/Ampere/Ada/Blackwell
# plus virtual PTX so architectures newer than this toolkit still run.
# CUDA 13 dropped everything below Turing, so there is no point listing 60/70.
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
target_compile_definitions(ofxsupport PUBLIC NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)

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
# comdlg32 for the file dialogs, shell32/ole32 for the folder picker.
target_link_libraries(BetterLine PRIVATE ofxsupport comdlg32 shell32 ole32)

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
    COMMAND ${CMAKE_COMMAND} -E make_directory "${BUNDLE_DIR}/Contents/Win64"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:BetterLine>" "${BUNDLE_DIR}/Contents/Win64/"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_SOURCE_DIR}/packaging/Info.plist" "${BUNDLE_DIR}/Contents/"
    COMMENT "Assembling BetterLine.ofx.bundle"
)
