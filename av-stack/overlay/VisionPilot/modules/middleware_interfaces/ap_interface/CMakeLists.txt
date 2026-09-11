cmake_minimum_required(VERSION 3.22.1)

# ---------------------------------------------------------------------------
# AUTOSAR Adaptive (CAPI) middleware interface — shared application-side code.
#
# What used to live here has been split in two, because there are now four
# Adaptive Applications rather than one:
#
#   cmake/AraApplication.cmake   locating the SDK (ara_find_capi), the shared
#                                generated binding sources (ara_generated_common),
#                                and add_ara_executable()
#   this directory               the application-side code every AA shares
#
# ap_runtime is that shared code: ara::core::Initialize / Deinitialize in the
# right order, the SIGTERM and Execution Management contract
# (ReportExecutionState(kRunning), clean shutdown on kTerminating), and the PHM
# checkpoint wrapper. All four processes need it and none should reimplement it.
#
# The old ap_platform target is gone. It globbed EVERY .cpp under net-bindings/
# into one library, which was right when there was one executable and is wrong
# now: it would link all four executables' Initialize<Service>() definitions
# into every process and register services a process has no ports for. The split
# is in AraApplication.cmake — shared serialisation glue in
# ap_generated_common, per-executable runtime.cpp compiled only into that
# executable.
#
# vehicle_ap_interface is also gone, and that one is a consequence of the
# decomposition rather than a cleanup. It implemented VisionPilot's
# VehicleInterface back when one process both read ego speed and wrote
# actuation. Those are now two processes on opposite sides of the model:
# Control senses speed and PROVIDES VehicleStatusService, Planning REQUIRES it,
# and VehicleControlService is retired entirely because actuation no longer
# crosses a boundary.
#
# Do not simply discard that file when you port it — SPLIT it. Its proxy half
# (StartFindService on VehicleStatusService, Subscribe, GetNewSamples,
# SetReceiveHandler, the speed_age_ms staleness accessor) is exactly what
# planningd needs and is already written against the CAPI headers. Its skeleton
# half was VehicleControlService and does not survive; controld needs a
# VehicleStatusService skeleton instead, which is new code but the same shape.
# ---------------------------------------------------------------------------

if(NOT TARGET ap::capi)
    message(FATAL_ERROR
        "ap_interface requires ara_find_capi() to have run. Include "
        "cmake/AraApplication.cmake from the top-level CMakeLists.txt first.")
endif()

add_subdirectory(ap_runtime)
