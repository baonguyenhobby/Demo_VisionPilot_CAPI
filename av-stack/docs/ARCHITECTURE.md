# VisionPilot on AUTOSAR Adaptive (CAPI) — middleware overlay

Swaps VisionPilot's vehicle I/O from ROS 2 to the AUTOSAR Adaptive Platform,
using AUTOSAR's Common Adaptive Platform Implementation (CAPI).

**Target:** Waveshare UGV Rover with Jetson Orin Nano · JetPack 6.2.1 ·
Ubuntu 22.04 · aarch64.

Nothing in VisionPilot's inference, fusion, planning or visualisation code
changes. The port adds one new backend behind an abstraction that already
exists upstream.

---

## Why this is a small change

VisionPilot's dependency on ROS 2 is confined to two classes behind two pure
abstract interfaces, selected by one preprocessor branch in
`app/vision_pilot.cpp`:

| Abstraction | ROS 2 backend | AP backend (this overlay) |
|---|---|---|
| `CameraInterface` (`get_latest_frame()`) | `CameraRos2Interface` — `sensor_msgs/Image` | **unchanged** — still chosen by `source.mode` (`video` or `v4l2`) |
| `VehicleInterface` (`read()` / `write()`) | `VehicleRos2Interface` — three `Float64` topics | `VehicleApInterface` — `SpeedEvent` in, `ActuationEvent` out |

Only the vehicle side becomes a modelled service. `ENABLE_AP_INTERFACE` does
not touch the camera backend at all, so the same binary runs open-loop against
a recorded clip on a development host (`source.mode = video`) and against the
camera on target (`source.mode = v4l2`). Everything downstream —
`InferencePipeline`, `LongitudinalFusion`, `LateralFusion`, `Planner` — is
reached through plain value types and never learns which middleware is
underneath.

In an AP build, `source.input_vehicle_speed` is no longer required in video
mode: ego speed arrives as `SpeedEvent`, so `frame_speed.txt` becomes the bench
producer's input rather than VisionPilot's.

---

## What is in here

```
model/                          ARXML — validated against CAPI's own AUTOSAR_00049.xsd
  data_types.arxml              VehicleSpeed, ActuationCommand
  service_interfaces.arxml      VehicleStatusService, VehicleControlService
  service_interfaces_someip_deployments.arxml
                                service/event IDs, both events on UDP
  function_groups.arxml         AvPilotFG: Off / Driving / Degraded / Safe

VisionPilot/
  config/vision_pilot_ap.conf   InstanceSpecifiers and the staleness bound
  modules/middleware_interfaces/ap_interface/
    ap_runtime/                 ara::core init, SIGTERM/EM contract, PHM checkpoints
    vehicle_ap_interface/       VehicleInterface backend over ara::com

patches/
  0001-visionpilot-ap-middleware-backend.patch
```

The patch applies cleanly to `autowarefoundation/vision_pilot` `main`
(verified at `f9fb997`) and touches six files:

| File | Change |
|---|---|
| `VisionPilot/CMakeLists.txt` | `option(ENABLE_AP_INTERFACE)`, mutual-exclusion guard with `ENABLE_ROS2_INTERFACE`, `add_subdirectory` |
| `VisionPilot/app/CMakeLists.txt` | link `ap_runtime` + `vehicle_ap_interface`, define `ENABLE_AP_INTERFACE`, rename the binary to `visionpilotd`, install the new `.conf` |
| `VisionPilot/app/vision_pilot.cpp` | backend selection, EM lifecycle, four PHM checkpoints, warning mapping, staleness gate |
| `VisionPilot/modules/config/*` | three InstanceSpecifier keys and `ap.speed_stale_ms` |

It is additive: 212 insertions, 3 deletions. No existing behaviour changes when
`ENABLE_AP_INTERFACE` is off.

---

## Build

**VisionPilot is built separately from CAPI.** It stays an ordinary CMake
project and simply *locates* the SDK, the same way it locates ONNX Runtime —
it is never built through the SDK's own `ab` / `build.sh` wrapper, which knows
nothing about CUDA, TensorRT or OpenCV.

```bash
# 1. Generate the ara::com proxy/skeleton headers
aragen -o /tmp/aragen-out  model/  <capi>/isoft/arxmls/models/

# 2. Configure VisionPilot against them, pointing at the installed SDK
cmake -DONNXRUNTIME_ROOT=/path/to/onnxruntime \
      -DENABLE_AP_INTERFACE=ON \
      -DARA_GEN_OUTPUT=/tmp/aragen-out \
      -DCMAKE_PREFIX_PATH="${SDK_INST_DIR}" \
      ..
make

# 3. Install where CAPI's configurator will find it (the standalone `ab -i`)
install -D -m 0755 visionpilotd "${HOME}/.isoft/tmp/ara_binout/visionpilotd"
```

Step 3 is not optional and is easy to miss: `config.sh` scans
`ARA_GEN_EXECUTABLE_OUTPUT` and matches the model's executable **short-name**
against the filenames there. `BUILDING.md` §5–§6 covers this, plus the RPATH
question that decides whether `visionpilotd` can resolve the `ara::` shared
objects once EM starts it from inside the sysroot.

`ENABLE_AP_INTERFACE` and `ENABLE_ROS2_INTERFACE` are mutually exclusive and
CMake fails loudly if both are set — both provide the `VehicleInterface`
backend.

---

## Integrating with the Waveshare stack

Read from `waveshareteam/ugv_ws`, `ugv_bringup/ugv_bringup/ugv_driver.py`.
The chassis contract is narrow:

| Direction | Topic | Type |
|---|---|---|
| command | `cmd_vel` | `geometry_msgs/Twist` |
| odometry | `/odom` | `nav_msgs/Odometry` |
| IMU | `imu/data_raw`, `imu/mag` | `sensor_msgs/Imu`, `MagneticField` |
| camera | `/image_raw` | `sensor_msgs/Image` |
| LED / battery | `ugv/led_ctrl`, `voltage` | `Float32MultiArray`, `Float32` |

`cmd_vel_callback` reads only `linear.x` and `angular.z`, then writes
`{'T':'13','X':v,'Z':w}` as JSON over serial to the ESP32, which closes the
motor PID loop.

### The unit mismatch — the real integration work

**This is not a middleware problem and the overlay does not solve it.**
VisionPilot's `Planner` emits a bicycle-model pair (tyre angle δ in rad,
acceleration in m/s²). The rover accepts a velocity pair. Someone must:

- integrate acceleration into a target speed — `Twist` has no acceleration
  field, so `v_target = v + a·dt`;
- convert steering to yaw rate — `ω = v·tan(δ)/L`.

Three complications worth knowing before you tune anything:

1. The rover is **skid-steer**, not Ackermann, so `tan(δ)` is an approximation
   whose error grows with steering angle.
2. `ugv_driver.py` forces `|ω| ≥ 0.2` whenever `linear.x == 0`. A lane-keeper
   emitting small corrections at low speed will fight that dead-band.
3. Three config values are **car-sized and wrong on this platform**: `L`
   defaults to 2.67 m (front axle to CoG of a passenger car), `speed_limit` to
   33.3 m/s, and `H.yaml` encodes a car's camera mounting. The homography must
   be regenerated for the rover — otherwise every distance and curvature the
   fusion produces is wrong, and no amount of control tuning will fix it.

### Where the conversion belongs

A **UGV Gateway AA** — the only process on the machine that speaks ROS 2:

```
  /odom  ──►  Gateway  ──►  VehicleStatusService.SpeedEvent   ──►  VisionPilot
  cmd_vel ◄──  Gateway  ◄──  VehicleControlService.ActuationEvent ◄──  VisionPilot
                    (bicycle → Twist conversion lives here)
```

VisionPilot then runs as pure CAPI with no ROS 2 at all, which is the point of
the port. **This gateway is not written yet** — it is the next deliverable.

### Camera device ownership

VisionPilot opens `/dev/video0` directly, so it owns the device exclusively.
The Waveshare vision nodes (`camera.launch.py`, and the AprilTag / gesture /
colour-track pipelines that subscribe `/image_raw`) cannot run against the same
camera while it does. The OAK-D Lite is a separate device and is unaffected.

---

## The design decisions this encodes

**The camera does not cross the service boundary.** On this target it is a
local USB device; modelling it would send each frame out through SOME/IP
loopback and straight back into the same SoC. This preserves AD-2 from the
arc42 deck, and it has a second effect worth noting: with no image on the wire,
*every* type in the model is bounded — no vector, no string — so both events fit
one UDP datagram inside one MTU and the transport question is closed.

**Two interfaces, not one `ObjectPerception`.** VisionPilot does not produce a
generic object list. It produces a steering angle and an acceleration, from a
CIPO/lateral fusion state that stays inside the process. The interfaces model
what actually crosses the boundary.

**Steering and acceleration travel in one sample.** `VehicleRos2Interface`
publishes them on two separate `Float64` topics. They come from one `Plan` and
are only meaningful together; splitting them puts a tearing hazard on the wire
that the supervisor would have to re-detect.

**Two independent staleness detectors.** PHM watches the producer from outside
(alive supervision on `kCycleStart`, deadline supervision across
`kFrameReceived → kPublished`). `VehicleApInterface::speed_age_ms()` watches the
data actually received. Neither depends on the other.

**The degrade is a modelled transition.** `AvPilotFG` lists VisionPilot in
`Driving` only and the Control AA in `Driving` *and* `Degraded`. EM stops one
process and keeps the other because the manifest says so — the one capability
this port buys that ROS 2 cannot express.

---

## Verification status — read before quoting any of this

**Verified.** All ten ARXML files validate against
`isoft/ara-gen/generator/common/schema/AUTOSAR_00049.xsd`, the schema shipped
with CAPI itself. The patch applies cleanly to pristine upstream `main`. Every
`ara::` call was checked against the CAPI headers in this release:
`ara/com/internal/proxy/event.h` (`Subscribe`, `GetNewSamples`,
`SetReceiveHandler`, `GetSubscriptionState`),
`ara/com/internal/skeleton/event.h` (`Allocate`, `Send`),
`ara/com/internal/proxy/proxy.h` (the `InstanceSpecifier` overload of
`StartFindService`), `ara/phm/supervised_entity.h`,
`ara/exec/execution_client.h`, `ara/core/{initialization,future,promise,
instance_specifier}.h`. Generated-symbol shapes come from
`isoft/ara-gen/generator/templates/service/{proxy,skeleton}_service_h.j2`.

**Also verified: `ara-gen` runs over the model and generates clean**, and the
generated symbols match this C++ exactly — `av::vp::cm::proxy::VehicleStatusServiceProxy`
with member `SpeedEvent`, `av::vp::cm::skeleton::VehicleControlServiceSkeleton`
with member `ActuationEvent`, and every struct field name. The
Driving/Degraded asymmetry survives into the generated process manifests. The
`lxml==4.9.1` pin turned out to be a single deprecated alias in `ara-gen`; see
`BUILDING.md`.

**The SDK now exists — this paragraph used to say it did not.** CAPI builds on
Ubuntu 24.04 / GCC 13.3 with the two changes in `BUILDING.md` (the `lxml` alias
and dropping `-Werror` from four modules), installs as a native SDK, and
`config.sh` + `run.sh -R` bring up a machine that reaches `MachineFG:Startup`
with all thirteen platform processes Running. So "CAPI's own C++ on GCC 13" is
no longer the open risk.

**Not verified.** This overlay's own C++ has still not been compiled or linked.
What remains unproven is narrow and testable in an afternoon: whether
`find_package(ara-core / ara-com / ara-log / ara-exec-execution-client /
ara-phm-client / ara-com-nsomeip)` resolves against the installed SDK, and
whether `visionpilotd` finds those shared objects at run time once EM starts it
from inside the sysroot rather than from the build tree.

**The model is complete; one application is not.** `model/integration/` now
carries the deployment half — processes, startup configurations, the
`FunctionGroupStateIRef` mappings that encode the Driving/Degraded asymmetry,
the service instance manifests, the machine extension and the software cluster.
All 112 references resolve.

What is missing is **`vehiclebenchd` itself**: the executable, its ports and its
`Driving`/`Degraded` mapping are modelled, and no C++ implements it. Deploy as
things stand and `visionpilotd` will start, call `StartFindService` on
`VehicleStatusService`, and sit at `bindHandles: {"vector": {0}}` — the exact
failure `samples/helloworld-cm` produces when only its client half is
integrated. On the rover this role is the UGV Gateway AA below; on the bench it
is a small publisher fed from `frame_speed.txt`. Either way it is the long pole,
and it is application work rather than middleware work.

**Two claims in the earlier HANDOFF.md need correcting.**
`cmake/Config.cmake` declares `ARA_ENABLE_COM_FASTDDS` and
`ARA_ENABLE_COM_FSOMEIP`, and `ara-gen` ships a complete DDS generator
(`generator/views/dds/`, `dds_deployment_builder.py`, IDL emission) — but
`com/com/src/fastdds/` and `fsomeip/` are absent from the release. So "no DDS
binding exists" is right about the runtime and wrong about the toolchain.
Separately, `isoft/e2e` ships compiled `.c` for P01, P02, P04, P04m, P05, P06,
P07, P07m, P08, P11, P22 and P44 — E2E profile selection is a choice, not an
availability problem.

**Open.** The UGV gateway is unwritten. E2E profile not selected (a fixed-rate
stream wants counter + timeout, not only a CRC — P04/P07 are the candidates).
`ap.speed_stale_ms` is a placeholder to be tuned on the Orin Nano, never on a
dev host — and the known brownout under combined GPU + sensor load makes late
cycles more likely on this board, not less. CAPI is released for information
only; commercial use needs an AUTOSAR partnership.

**Three runtime questions the machine bring-up surfaced**, none of which the
`helloworld-cm` demo exercises but all of which this application will:

- **Scheduling.** EM logs `Invalid SchedulingPolicy of { RR }, set to default
  value {Other}` and starts the process anyway. A perception loop carrying a
  deadline supervision across `kFrameReceived → kPublished` genuinely wants
  `SCHED_RR`; getting it needs `CAP_SYS_NICE` and a resource group that is not
  `DefaultResourceGroup`. The silent downgrade is the dangerous part.
- **Device ownership.** `/dev/video0` and the GPU are opened by a process EM
  starts, not by your shell. The user, group and OS resource group in
  `deployment.arxml` have to match what V4L2 and CUDA require, or the process
  starts and then fails at first frame.
- **Machine identity.** The unicast address and the `239.0.0.1:30723` SOME/IP
  multicast are baked into the machine configuration at `config.sh` time. Both
  change on the rover, and `nsomeipd` will happily offer on the wrong
  interface.

---

## Note on the other AP implementation

`Autosar_AP_SDC_Carla` links `tatsuyai713/Adaptive-AUTOSAR` — a personal-account,
self-described *educational* implementation that disclaims conformance
certification. It is **not** CAPI, and the `ara::` similarity is a trap:
`ProxyEvent::Subscribe()`, `SetReceiveHandler()` and `SkeletonEvent::Send()`
return `void` there and `ara::core::Result<void>` in CAPI, and PHM is separate
`AliveSupervision` / `LogicalSupervision` classes rather than one
`SupervisedEntity<EnumT>` template. So this overlay compiles against CAPI only.
Retargeting would be ~6 call sites, a different PHM class, and regenerating the
model against schema `autosar_00050.xsd` (CAPI uses `00049`).

Both speak SOME/IP, so two AP machines on different implementations can
interoperate on the wire provided the service IDs, event IDs, event groups and
serialisation layout agree — which is what
`service_interfaces_someip_deployments.arxml` pins down.

---

Sources: `github.com/autowarefoundation/vision_pilot` (main) ·
`github.com/AUTOSAR/capi` (R20-11, contributed by iSOFT) ·
`github.com/waveshareteam/ugv_ws` ·
[UGV Rover Jetson Orin ROS2 — Waveshare Wiki](https://www.waveshare.com/wiki/UGV_Rover_Jetson_Orin_ROS2)
