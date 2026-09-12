# ---------------------------------------------------------------------------
# AraApplication.cmake — building Adaptive Applications against an INSTALLED
# CAPI SDK.
#
# VisionPilot is built separately from CAPI and is not a CAPI package. It is an
# ordinary CMake project that LOCATES the SDK, exactly the way it locates ONNX
# Runtime. Nothing here goes through `ab` or `build.sh`: those know nothing
# about CUDA, TensorRT or OpenCV, and teaching them would drag the perception
# toolchain into the platform build for no gain.
#
# Usage from the top-level CMakeLists.txt:
#
#   list(APPEND CMAKE_MODULE_PATH ${CMAKE_SOURCE_DIR}/cmake)
#   include(AraApplication)
#   ara_find_capi()                     # once
#   ara_generated_common()              # once — the shared binding sources
#   add_subdirectory(apps)              # the four executables
#
# Configure with:
#
#   cmake -DENABLE_AP_INTERFACE=ON \
#         -DARA_GEN_OUTPUT=/tmp/aragen-4aa \
#         -DCMAKE_PREFIX_PATH="${ARA_SYSROOT}/ara/framework/1.0.0;${ARA_SYSROOT}/usr" ..
# ---------------------------------------------------------------------------

# Where config.sh will look for built executables. This is the standalone
# equivalent of `ab -i`, and it is not optional: buildExecutableFQN2PathDict
# scans this directory and matches each executable's SHORT-NAME from the model
# against the filenames it finds. Nothing installed here means config.sh fails
# with "IntegrateSWCL, can't find executablePath".
set(ARA_GEN_EXECUTABLE_OUTPUT "$ENV{ARA_GEN_EXECUTABLE_OUTPUT}" CACHE PATH
    "Directory config.sh scans for built executables")
if(NOT ARA_GEN_EXECUTABLE_OUTPUT)
    set(ARA_GEN_EXECUTABLE_OUTPUT "$ENV{HOME}/.isoft/tmp/ara_binout" CACHE PATH
        "Directory config.sh scans for built executables" FORCE)
endif()

# Run-time library resolution. EM starts these binaries from inside
# ${ARA_SYSROOT}, not from the SDK tree, and does not inherit your shell's
# LD_LIBRARY_PATH. A missing .so surfaces as EM reporting the process failed to
# start with NO application log at all, because it never reached main().
#
#   dev host : point this at the SDK's lib dir (stable absolute path)
#   target   : leave the default and ship the ara:: .so files into the SWCL's
#              own lib/ beside bin/ — that is what swcls/<name>/<ver>/{bin,lib}
#              is for, and it makes the SWCL self-contained, which UCM needs
#              anyway to update it as a unit.
set(ARA_SWCL_RPATH "$ORIGIN/../lib" CACHE STRING
    "RPATH baked into each Adaptive Application")

# ---------------------------------------------------------------------------
# ara_find_capi() — locate the SDK and expose it as the ap::capi target.
# ---------------------------------------------------------------------------
function(ara_find_capi)
    if(TARGET ap::capi)
        return()
    endif()

    # CAPI installs standard CMake package configs (isoft/apcommon-cmake-modules).
    # If any of these fail, the configs sit deeper than CMAKE_PREFIX_PATH's
    # search rules reach — pass the exact directory as -D<pkg>_DIR=<dir> rather
    # than moving files around inside the SDK. Find them with:
    #   find ${SDK_INST_DIR} -name 'ara-core*onfig.cmake' -printf '%h\n' | sort -u
    find_package(ara-core REQUIRED)
    find_package(ara-com REQUIRED)
    find_package(ara-log REQUIRED)
    find_package(ara-exec-execution-client REQUIRED)
    find_package(ara-phm-client REQUIRED)

    # The binding is a LINK-TIME choice, not a code-time one. Swapping it
    # changes this line, HAS_NSOMEIP_BINDING below, and the service instance
    # manifest — no application source moves.
    find_package(ara-com-nsomeip REQUIRED)

    add_library(ap_capi INTERFACE)
    target_link_libraries(ap_capi INTERFACE
            ara::core ara_com ara::log
            ara::exec::execution_client ara::phm::phm_client
            ara_com_nsomeip)

    # Library search path for the SDK's own third-party libs (OpenSSL, fastdds,
    # tinyxml2, ...), which live in <sysroot>/usr/lib. Some SDK package configs
    # name them plainly -- `crypto` rather than an absolute path -- so without
    # this the link fails with "cannot find -lcrypto" while the file sits right
    # there. Derived from ara-core_DIR so it needs no environment variable.
    # Derived from where find_package actually found ara-core, NOT from
    # ARA_SYSROOT: that variable names the MACHINE root (what config.sh builds
    # and run.sh boots), which is a different directory from the SDK sysroot.
    # Conflating the two makes configMachine try to copy the SDK onto itself.
    get_filename_component(_sysroot "${ara-core_DIR}/../../../../../.." ABSOLUTE)
    if(NOT EXISTS "${_sysroot}/usr/lib")
        message(FATAL_ERROR
            "Could not locate the CAPI sysroot (looked at '${_sysroot}'). "
            "Set ARA_SYSROOT to <SDK_INST_DIR>/ara-sysroot.")
    endif()
    target_link_directories(ap_capi INTERFACE "${_sysroot}/usr/lib")

    # The SDK's own name, e.g. SDK-1.0.0.2608-x86_64-ubuntu24.04-native-Debug.
    # configMachine resolves an executable FQN to
    #   <binout>/<AppPackage>/exe/<exeName>/<SDK_NAME>/<exeName>
    # so ara-install has to reproduce that path exactly. Read rather than
    # hard-coded: the string changes with every -v you pass to sdk-utils.
    file(READ "${_sysroot}/.release.json" _release_json)
    string(REGEX MATCH "\"SDK_NAME\"[ \t]*:[ \t]*\"([^\"]+)\"" _m "${_release_json}")
    if(NOT CMAKE_MATCH_1)
        message(FATAL_ERROR "Could not read SDK_NAME from ${_sysroot}/.release.json")
    endif()
    set(ARA_SDK_NAME "${CMAKE_MATCH_1}" CACHE STRING "Installed CAPI SDK name" FORCE)
    message(STATUS "CAPI SDK: ${ARA_SDK_NAME}")

    # RPATH for the same two directories. EM starts these binaries without your
    # shell's LD_LIBRARY_PATH; a missing .so then shows up as EM reporting the
    # process failed to start with no application log at all, because it never
    # reached main().
    # --disable-new-dtags emits DT_RPATH rather than DT_RUNPATH. The difference
    # is decisive here: DT_RUNPATH applies ONLY to the binary's own direct
    # dependencies, while DT_RPATH is inherited down the whole chain. The SDK's
    # libraries carry no runpath of their own, so libara-com.so's own
    # dependencies (libisoft_manifestreader.so.1 and friends) are unresolvable
    # under DT_RUNPATH even though the path is right there in the binary.
    # ONNX Runtime, for perceptiond. EM starts processes with LD_LIBRARY_PATH
    # pointing at the SDK and nothing else, so a library found at build time via
    # -DONNXRUNTIME_ROOT is NOT found at run time unless its directory is in the
    # RPATH. The failure is exit code 127 with no application log at all.
    if(ONNXRUNTIME_ROOT AND EXISTS "${ONNXRUNTIME_ROOT}/lib")
        target_link_options(ap_capi INTERFACE "-Wl,-rpath,${ONNXRUNTIME_ROOT}/lib")
    endif()

    target_link_options(ap_capi INTERFACE
            "-Wl,--disable-new-dtags"
            "-Wl,-rpath,${_sysroot}/usr/lib"
            "-Wl,-rpath,${_sysroot}/ara/framework/1.0.0/lib")

    target_compile_features(ap_capi INTERFACE cxx_std_17)
    add_library(ap::capi ALIAS ap_capi)
endfunction()

# ---------------------------------------------------------------------------
# _ara_gen_root(<out>) — accept either the ara-gen output root or its includes/
# subdirectory, since passing the latter is an easy mistake to make.
# ---------------------------------------------------------------------------
function(_ara_gen_root out)
    if(NOT DEFINED ARA_GEN_OUTPUT)
        message(FATAL_ERROR
            "ENABLE_AP_INTERFACE=ON requires -DARA_GEN_OUTPUT=<ara-gen output root>. "
            "See model/README.md.")
    endif()
    if(EXISTS "${ARA_GEN_OUTPUT}/includes/av/vp/cm")
        set(${out} "${ARA_GEN_OUTPUT}" PARENT_SCOPE)
    elseif(EXISTS "${ARA_GEN_OUTPUT}/../includes/av/vp/cm")
        get_filename_component(_r "${ARA_GEN_OUTPUT}/.." ABSOLUTE)
        set(${out} "${_r}" PARENT_SCOPE)
    else()
        message(FATAL_ERROR
            "ARA_GEN_OUTPUT='${ARA_GEN_OUTPUT}' does not look like an ara-gen output "
            "root (expected includes/av/vp/cm underneath it). Regenerate with:\n"
            "  aragen -o <out> model/ <capi>/isoft/arxmls/models/")
    endif()
endfunction()

# ---------------------------------------------------------------------------
# ara_generated_common() — the binding sources SHARED by every executable.
#
# ara-gen emits two kinds of .cpp under net-bindings/:
#
#   net-bindings/av/vp/cm/**.cpp     serialisation and binding glue for the
#                                    service interfaces — same for everyone
#   net-bindings/<exe>/**.cpp        Initialize<Service>() / Deinitialize<Service>()
#                                    for the services THAT executable declares
#
# Only the first kind belongs in a shared library. Compiling every executable's
# runtime.cpp into one library — which is what a naive
# `GLOB_RECURSE net-bindings/*.cpp` does — links four sets of Initialize()
# definitions into every process and registers services a process has no ports
# for. That was correct when there was one executable and is wrong now.
# ---------------------------------------------------------------------------
function(ara_generated_common)
    if(TARGET ap_generated_common)
        return()
    endif()
    _ara_gen_root(_root)

    file(GLOB_RECURSE _shared "${_root}/net-bindings/av/vp/cm/*.cpp")
    if(NOT _shared)
        message(FATAL_ERROR
            "No shared binding sources under ${_root}/net-bindings/av/vp/cm. "
            "Did ara-gen run over the whole model?")
    endif()

    add_library(ap_generated_common STATIC ${_shared})
    target_include_directories(ap_generated_common PUBLIC
            "${_root}/includes"
            "${_root}/net-bindings")
    # Selects the #ifdef branches in the generated runtime.cpp. Must agree with
    # the binding actually linked in ara_find_capi().
    target_compile_definitions(ap_generated_common PUBLIC HAS_NSOMEIP_BINDING)
    target_link_libraries(ap_generated_common PUBLIC ap::capi)
    set_target_properties(ap_generated_common PROPERTIES POSITION_INDEPENDENT_CODE ON)
endfunction()

# ---------------------------------------------------------------------------
# add_ara_executable(
#     TARGET      <cmake target name>
#     ARA_NAME    <EXECUTABLE SHORT-NAME from the model>
#     BINDING_DIR <subdirectory of net-bindings/ holding this exe's runtime>
#     SOURCES     <...>
#     LIBS        <...>)
#
# ARA_NAME is not cosmetic. It becomes the binary's filename, and CAPI's
# configurator resolves /XxxApp/exe/<ARA_NAME> by matching that short-name
# against filenames under ARA_GEN_EXECUTABLE_OUTPUT. A binary called
# "VisionPilot" is never matched.
#
# BINDING_DIR is whatever ara-gen actually named the per-executable directory —
# it is NOT guaranteed to equal ARA_NAME. Check once with:
#     ls ${ARA_GEN_OUTPUT}/net-bindings/
# ---------------------------------------------------------------------------
function(add_ara_executable)
    cmake_parse_arguments(A "" "TARGET;ARA_NAME;ARA_FQN;BINDING_DIR" "SOURCES;LIBS" ${ARGN})
    if(NOT A_TARGET OR NOT A_ARA_NAME OR NOT A_ARA_FQN OR NOT A_BINDING_DIR)
        message(FATAL_ERROR "add_ara_executable: TARGET, ARA_NAME and BINDING_DIR are required")
    endif()
    _ara_gen_root(_root)

    file(GLOB_RECURSE _runtime "${_root}/net-bindings/${A_BINDING_DIR}/*.cpp")
    if(NOT _runtime)
        message(FATAL_ERROR
            "No generated runtime sources under ${_root}/net-bindings/${A_BINDING_DIR}.\n"
            "Without them ara::core::Initialize() registers nothing, and the process "
            "links, starts, and silently never offers or finds anything — it looks "
            "like a discovery fault, not a build fault.\n"
            "Available directories: run  ls ${_root}/net-bindings/")
    endif()

    add_executable(${A_TARGET} ${A_SOURCES} ${_runtime})
    target_link_libraries(${A_TARGET} PRIVATE ap_generated_common ${A_LIBS})

    set_target_properties(${A_TARGET} PROPERTIES
            OUTPUT_NAME "${A_ARA_NAME}"
            BUILD_WITH_INSTALL_RPATH ON
            INSTALL_RPATH "${ARA_SWCL_RPATH}")

    # The standalone `ab -i`. Depend on the aggregate target `ara-install` to
    # place every executable where config.sh will find it.
    # <binout>/<AppPackage>/exe/<exeName>/<SDK_NAME>/<exeName> -- the layout
    # `ab -i` produces and configMachine searches. A flat copy is NOT found, and
    # the failure reads "can't find executablePath", which points at the model
    # rather than at the install.
    string(REGEX REPLACE "^/([^/]+)/.*$" "\\1" _pkg "${A_ARA_FQN}")
    if(_pkg STREQUAL A_ARA_FQN)
        message(FATAL_ERROR "ARA_FQN must look like /SensingApp/exe/sensingd, got '${A_ARA_FQN}'")
    endif()
    set(_dest "${ARA_GEN_EXECUTABLE_OUTPUT}/${_pkg}/exe/${A_ARA_NAME}/${ARA_SDK_NAME}")

    add_custom_target(ara-install-${A_TARGET}
            COMMAND ${CMAKE_COMMAND} -E make_directory "${_dest}"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "$<TARGET_FILE:${A_TARGET}>"
                    "${_dest}/${A_ARA_NAME}"
            DEPENDS ${A_TARGET}
            COMMENT "ara-install: ${A_ARA_NAME} -> ${_dest}")

    if(NOT TARGET ara-install)
        add_custom_target(ara-install)
    endif()
    add_dependencies(ara-install ara-install-${A_TARGET})

    install(TARGETS ${A_TARGET} RUNTIME DESTINATION bin)
endfunction()
