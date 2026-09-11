#!/usr/bin/env bash
# ---------------------------------------------------------------------------
# Copy av-stack/overlay/VisionPilot/ into the vision_pilot submodule.
#
# The overlay mirrors the submodule's own directory layout, so this is a plain
# copy — nothing is generated or rewritten. It is ADDITIVE: no upstream file is
# overwritten. The two upstream files that DO need changing are CMakeLists.txt
# edits this script prints rather than applies, because silently patching a
# submodule's build files is how you lose track of what you changed.
#
# Re-runnable. Run it again after editing anything under overlay/.
# ---------------------------------------------------------------------------
set -euo pipefail

AVSTACK="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ROOT="$(cd "${AVSTACK}/.." && pwd)"
VP="${ROOT}/vision_pilot/VisionPilot"

if [[ ! -d "${VP}" ]]; then
    echo "error: ${VP} not found." >&2
    echo "       Run: git submodule update --init --depth 1" >&2
    exit 1
fi

echo "av-stack/overlay -> vision_pilot/VisionPilot"
for d in common control perception planning sensing; do
    cp -a "${AVSTACK}/overlay/VisionPilot/app/${d}" "${VP}/app/"
done
cp -a "${AVSTACK}/overlay/VisionPilot/app/av_stack.cmake"          "${VP}/app/"
cp -a "${AVSTACK}/overlay/VisionPilot/cmake/."                     "${VP}/cmake/"
cp -a "${AVSTACK}/overlay/VisionPilot/modules/middleware_interfaces/ap_interface" \
                                                                   "${VP}/modules/middleware_interfaces/"
cp -a "${AVSTACK}/overlay/VisionPilot/config/vision_pilot_ap.conf" "${VP}/config/"

echo
echo "copied:"
find "${VP}/app/av_stack.cmake" "${VP}/app/sensing" "${VP}/app/perception" \
     "${VP}/app/planning" "${VP}/app/control" "${VP}/app/common" \
     "${VP}/cmake/AraApplication.cmake" \
     "${VP}/modules/middleware_interfaces/ap_interface" -type f | sed "s|${VP}/|  |"

cat <<'EDITS'

------------------------------------------------------------------------------
Two edits remain, both additive, both in upstream files. Apply by hand so the
submodule diff stays legible.

1. VisionPilot/CMakeLists.txt — beside the ROS 2 option (~line 20):

   option(ENABLE_AP_INTERFACE "Enable AUTOSAR Adaptive (CAPI) support" OFF)

   if(ENABLE_ROS2_INTERFACE AND ENABLE_AP_INTERFACE)
       message(FATAL_ERROR
           "ENABLE_ROS2_INTERFACE and ENABLE_AP_INTERFACE are mutually exclusive.")
   endif()

   and, AFTER add_subdirectory(modules/debug) but BEFORE add_subdirectory(app):

   if(ENABLE_AP_INTERFACE)
       list(APPEND CMAKE_MODULE_PATH ${CMAKE_SOURCE_DIR}/cmake)
       include(AraApplication)
       ara_find_capi()          # creates the ap::capi target
       ara_generated_common()   # shared binding sources
       add_subdirectory(modules/middleware_interfaces/ap_interface)
   endif()

   Order matters: ap_interface checks for ap::capi, and both must exist before
   app/ is descended into.

2. VisionPilot/app/CMakeLists.txt — at the end, one guarded line:

   if(ENABLE_AP_INTERFACE)
       include(${CMAKE_CURRENT_LIST_DIR}/av_stack.cmake)
   endif()

   include(), not add_subdirectory(): the components sit directly under app/,
   so there is no subdirectory to descend into.

Then, from the repository root:

   cmake -DENABLE_AP_INTERFACE=ON -DENABLE_ROS2_INTERFACE=OFF -DGPU=OFF \
         -DARA_GEN_OUTPUT="$ARA_GEN_OUT" \
         -DCMAKE_PREFIX_PATH="$ARA_SYSROOT/ara/framework/1.0.0;$ARA_SYSROOT/usr" \
         -DONNXRUNTIME_ROOT=<path> ..
   make vp_control -j1        # smallest first; its errors are the shared ones
   make ara-install           # the standalone `ab -i`

See BUILD.md for the SDK prerequisites and av-stack/docs/TESTING.md for what to
run once it links.
------------------------------------------------------------------------------
EDITS
