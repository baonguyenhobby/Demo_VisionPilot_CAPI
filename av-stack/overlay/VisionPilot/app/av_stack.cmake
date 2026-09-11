# ---------------------------------------------------------------------------
# The four Adaptive Applications.
#
# The AV stack, decomposed into four Adaptive Applications: Sensing,
# Perception, Planning, Control. They live under app/ beside upstream's
# VisionPilot binary, and are NOT four separate projects.
#
# This file is include()d from app/CMakeLists.txt rather than being a
# subdirectory, so that the component directories sit directly under app/ and
# upstream's own CMakeLists.txt gains exactly one guarded line. include() does
# not change CMAKE_CURRENT_SOURCE_DIR, so the relative SOURCES paths below
# resolve against app/. That is deliberate: they share config / logging / common, they each
# need a different subset of the existing module libraries, and this tree
# already knows how to find ONNX Runtime, OpenCV and CUDA. Four standalone
# projects would mean four copies of that knowledge and four chances for them to
# drift.
#
# What is standalone is the relationship to CAPI: the SDK is located with
# find_package, never built through. See cmake/AraApplication.cmake.
#
# Each executable's ARA_NAME must equal the EXECUTABLE SHORT-NAME in the model,
# because config.sh resolves the FQN by matching that name against filenames it
# finds under ARA_GEN_EXECUTABLE_OUTPUT:
#
#   /SensingApp/exe/sensingd        <-  sensingd
#   /PerceptionApp/exe/perceptiond  <-  perceptiond
#   /PlanningApp/exe/planningd      <-  planningd
#   /ControlApp/exe/controld        <-  controld
#
# BINDING_DIR is whatever ara-gen named the per-executable directory under
# net-bindings/. It is not guaranteed to match ARA_NAME — check once with
#   ls ${ARA_GEN_OUTPUT}/net-bindings/
# and correct these four lines if they differ.
# ---------------------------------------------------------------------------

# --- Sensing ---------------------------------------------------------------
# Sensor abstraction: capture plus mounting-specific preprocessing (the
# homography warp and the resize to 1024x512). Owns /dev/video0 or the clip,
# and owns the shared-memory frame ring. No inference, no vehicle model.
#
# Moving preprocess() here is what turns `wall = pre + parallel` into a
# two-stage pipeline: steady-state throughput becomes max(pre, parallel)
# instead of their sum.
add_ara_executable(
        TARGET      vp_sensing
        ARA_NAME    sensingd
        BINDING_DIR sensing
        SOURCES     sensing/main.cpp
                    sensing/frame_ring_writer.cpp
        LIBS        ap_runtime config logging common
                    camera_interface       # FileInterface + V4L2CameraInterface
                    image_preprocessing    # ImagePreprocessor, the C matrix
)

# --- Perception ------------------------------------------------------------
# Inference and fusion. Consumes frames by reference, publishes the fused
# lane-relative state and CIPO estimate. Produces no command and owns no device.
#
# AD, AS and ASp each cost 55-62 ms but run CONCURRENTLY on one shared
# preprocessed frame, which is why the measured `parallel` is a maximum and not
# a sum. Do not split this executable further: one AA per network would
# serialise them across process boundaries and turn that maximum back into a sum.
#
# visualization and vp_debug are linked because the overlay lives here — it
# needs the frame, and the frame must not go on the wire. It draws the plan from
# the debug-only required port on TrajectoryService.
add_ara_executable(
        TARGET      vp_perception
        ARA_NAME    perceptiond
        BINDING_DIR perception
        SOURCES     perception/main.cpp
                    perception/frame_ring_reader.cpp
        LIBS        ap_runtime config logging common
                    engine                 # OnnxEngine
                    models                 # AutoDrive, AutoSteer, AutoSpeed
                    fusion                 # LongitudinalFusion, LateralFusion
                    visualization
                    vp_debug
)

# --- Planning --------------------------------------------------------------
# Planner::compute_plan and nothing else. Seven inputs, all of them arriving on
# the two required ports. No device, no chassis, no inference.
add_ara_executable(
        TARGET      vp_planning
        ARA_NAME    planningd
        BINDING_DIR planning
        SOURCES     planning/main.cpp
        LIBS        ap_runtime config logging common
                    planning               # Planner
)

# --- Control ---------------------------------------------------------------
# The last thing before the actuator, and the only process that touches the
# chassis. Applies steeringHorizonRad[1], converts the bicycle pair into the
# chassis's velocity pair, rate-limits, and safe-stops on staleness. Also senses
# ego speed and publishes it.
#
# Deliberately does NOT link engine, models or fusion. If it ever needs them,
# the decomposition has gone wrong.
#
# On the bench it replays frame_speed.txt as SpeedEvent and logs what it would
# have written; on the rover it is the only process that links ROS 2.
add_ara_executable(
        TARGET      vp_control
        ARA_NAME    controld
        BINDING_DIR control
        SOURCES     control/main.cpp
        LIBS        ap_runtime config logging common
                    vehicle_interface
)

# Suppress unresolved-symbol errors from system shared libs (gdal->curl, ICU
# from miniforge3) pulled in transitively by opencv_viz, which we never use.
# Same workaround the upstream app target carries.
foreach(_t vp_sensing vp_perception vp_planning vp_control)
    target_link_options(${_t} PRIVATE -Wl,--allow-shlib-undefined)
endforeach()
