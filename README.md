# Demo_VisionPilot_CAPI

Porting [VisionPilot](https://github.com/autowarefoundation/vision_pilot) — an
Autoware camera-only L2 pilot — onto the
[AUTOSAR Adaptive Platform](https://github.com/AUTOSAR/capi), decomposed into
four Adaptive Applications along the reference AD boundaries.

Both upstreams are submodules, pinned. Everything of ours lives in `av-stack/`.

```
Demo_VisionPilot_CAPI/
├── README.md          this file
├── BUILD.md           CAPI SDK on Ubuntu 24.04, then building the four AAs
├── av-stack/          the port
│   ├── model/           the AUTOSAR model — 12 ARXML files
│   ├── overlay/         what gets copied into the vision_pilot submodule
│   │   └── VisionPilot/
│   │       ├── app/            the four AAs + the shared-memory ring,
│   │       │                 dropped straight into upstream's app/
│   │       ├── cmake/           AraApplication.cmake — locating an INSTALLED SDK
│   │       ├── modules/.../ap_interface/   ap_runtime: ara::core, EM, PHM
│   │       └── config/          vision_pilot_ap.conf
│   ├── docs/
│   │   ├── ARCHITECTURE.md      design decisions, and what is deliberately absent
│   │   └── TESTING.md           four stages, cheapest falsification first
│   └── scripts/
│       └── apply-overlay.sh     copies overlay/ in, prints the CMake edits
├── patches/           additive patch against upstream (superseded — see status)
├── vision_pilot/      submodule @ f9fb997
└── capi/              submodule @ 112916a (v1.0.0)
```

| submodule | pinned at | why |
|---|---|---|
| `vision_pilot` | `f9fb997` | the commit the overlay and its patch were verified against |
| `capi` | `112916a` (`v1.0.0`) | the first AUTOSAR CAPI release, contributed by iSOFT |

Upstream `vision_pilot` `main` has since moved to `79905b9`; the pin is
deliberately behind so the port reproduces.

---

## What the port actually is

Upstream VisionPilot is one process: capture → preprocess → inference → fusion →
planning → actuation. This splits it into four, one per block of the standard
decomposition, and puts the boundaries in an AUTOSAR model:

```
sensingd  ──FrameEvent──▶  perceptiond ──DrivingStateEvent──▶ planningd
   ▲                            │                                 │
   │                            │  (debug overlay only)      TrajectoryEvent
   │                            ◀────────────────────────────     │
   │                                                              ▼
   └──────────────── SpeedEvent ◀───────────────────────────── controld ──▶ chassis
```

Four event-only service interfaces, all SOME/IP, all bounded, all one datagram.
Camera frames are the exception: 3 MB per capture never enters `ara::com`.
Sensing writes them into a shared-memory ring and publishes a 34-byte
`FrameDescriptor`; Perception maps the same segment read-only. The interface is
fully modelled, only the buffer transport is not — `av-stack/docs/ARCHITECTURE.md`
says what that costs.

**The one capability this buys that ROS 2 cannot express** is the function-group
table in `av-stack/model/integration/deployment.arxml`:

| | Off | Driving | Degraded | Safe |
|---|---|---|---|---|
| `sensingd` | | ✓ | ✓ | |
| `perceptiond` | | ✓ | ✓ | |
| `planningd` | | ✓ | | |
| `controld` | | ✓ | ✓ | ✓ |

`SetState(AvPilotFG.Degraded)` stops Planning and leaves the other three
running. No C++ implements that. Control keeps following the remainder of the
20-step steering horizon it already holds, decaying, then safe-stops on
staleness — which is why `Trajectory` carries the whole horizon rather than the
single value upstream applied.

VisionPilot is built **separately** from CAPI and simply *locates* the SDK, the
same way it locates ONNX Runtime. Nothing goes through CAPI's `ab` / `build.sh`.

---

## Quick start

```bash
git submodule update --init --depth 1

# 1. generate from the model (ara-gen is Python-only; no SDK needed)
aragen -o ~/aragen-4aa  av-stack/model/  capi/isoft/arxmls/models/

aragen -e /SensingApp/exe/sensingd,/PerceptionApp/exe/perceptiond,\
/PlanningApp/exe/planningd,/ControlApp/exe/controld \
       -o ~/aragen-4aa  av-stack/model/  capi/isoft/arxmls/models/

# 2. lay the overlay into the VisionPilot submodule
./av-stack/scripts/apply-overlay.sh

# 3. build — see BUILD.md
```

Two `aragen` runs, not one: without `-e` you get the process manifests, with
`-e` you get the proxy/skeleton headers and bindings. Both land in the same
output directory. `capi/isoft/arxmls/models/` must be on the input list — the
model references `Machine1`, its connector, `DefaultResourceGroup` and
`ProcessStateMachine` from there.

---

## Status — read before quoting any of this

**Verified.** All twelve ARXML files validate against `AUTOSAR_00049.xsd` as
shipped in CAPI. `ara-gen` generates clean: four processes, the correct port
topology per executable, `SteeringHorizon` emitted as
`ara::core::Array<float, 20>`, and the Driving/Degraded/Safe table intact in the
process manifests EM actually reads. The CAPI SDK builds on Ubuntu 24.04 /
GCC 13.3 and boots a machine with thirteen platform processes.

**Not verified.** None of the C++ under `av-stack/overlay/` has been compiled.
It is written against the real `ara::com` API — `Subscribe`, `GetNewSamples`,
`Send`, the skeleton's `InstanceSpecifier` constructor, and the
`Promise`/`StartFindService` idiom taken from CAPI's own `samples/helloworld-cm`
— but reading headers is not the same as compiling against them.

**Known gaps.**

- `patches/` still describes the superseded two-AA layout.
- `av-stack/docs/ARCHITECTURE.md` still describes VisionPilot + VehicleBench.
- InstanceSpecifiers are constants in the four `main.cpp` files rather than
  config keys, because `modules/config` has no `ap_*` fields yet.
- PHM supervision is unmodelled, so `ApRuntime` is constructed with an empty
  supervised-entity specifier and checkpoints are dropped.
- E2E profile not selected. `isoft/e2e` ships twelve; a fixed-rate stream wants
  a counter and a timeout, so P04 or P07.
- On the rover, `L = 2.67 m`, `speed_limit = 33.3 m/s` and `H.yaml` are all
  car-sized and wrong. No middleware work compensates for that.

CAPI is released for information only; commercial exploitation requires an
AUTOSAR partnership.
