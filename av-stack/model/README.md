# ARXML model — VisionPilot on AUTOSAR Adaptive (CAPI)

Complete model for a single machine running two Adaptive Applications.
Vehicle I/O only: there is no camera interface, because on both the bench and
the rover the camera is a local device, not a service.

```
data_types.arxml                            VehicleSpeed, ActuationCommand
service_interfaces.arxml                    VehicleStatusService, VehicleControlService
service_interfaces_someip_deployments.arxml service/event IDs, both events on UDP
function_groups.arxml                       AvPilotFG: Off / Driving / Degraded / Safe
app/
  visionpilot_design.arxml                  executable visionpilotd, ports on vp_root
  vehiclebench_design.arxml                 executable vehiclebenchd, the mirror ports
integration/
  service_instances_someip_deployments.arxml  provided/required instances (IDs 1 and 2)
  deployment.arxml                            processes, startup configs, all mappings, IAM grants
  machine_extension.arxml                     DLT log channels added to Machine1
  distribution.arxml                          software cluster claiming AvPilotFG
```

## Verified

**`ara-gen` runs over this model and generates clean.** That is the real check;
everything below it is corroboration.

- `--list-processes` reports `/AvDeployment/visionpilot_process` and
  `/AvDeployment/vehiclebench_process`.
- Per-executable generation produces the proxy, skeleton, type headers, SOME/IP
  binding and runtime sources for both applications.
- Full-model generation emits 356 files with no errors or warnings.
- **The generated symbols match the C++ exactly**: namespace `av::vp::cm`,
  `proxy::VehicleStatusServiceProxy` with member `SpeedEvent`,
  `skeleton::VehicleControlServiceSkeleton` with member `ActuationEvent`, and
  struct fields `timestampNs`, `seqCounter`, `speedMps`, `valid`,
  `steeringRad`, `accelerationMps2`, `warning`.
- **The state asymmetry survives generation.** In the process manifests EM
  actually reads:
  `visionpilot_process` → `states: ["Driving"]`,
  `vehiclebench_process` → `states: ["Driving", "Degraded"]`.

Also:

- All ten files validate against `AUTOSAR_00049.xsd`, the schema shipped inside
  CAPI (`isoft/ara-gen/generator/common/schema/`).
- All **112** references the model makes resolve, checked against the 4,688
  fully-qualified names defined by these files plus CAPI's own 353 model files.
  That covers the four reference classes the arc42 deck's §8.1 table calls out —
  `FUNCTION-GROUP-STATE-IREF`, `COMMUNICATION-CONNECTOR-REF`,
  `ServiceInstanceToPortPrototypeMapping`, `EXECUTABLE-REF`.
- The InstanceSpecifiers in `vision_pilot_ap.conf` match the port paths this
  model declares.

Still not verified: the generated code has not been **compiled**. The SDK now
exists and a machine boots from it (see `../BUILDING.md`), so what is untested
is this overlay's own link against it, not CAPI itself.

One gap the generator cannot flag: `vehiclebenchd` is fully modelled here —
executable, ports, `Driving`/`Degraded` mapping — and **no C++ implements it**.
The model is consistent; the deployment is half a system. `visionpilotd` alone
will start, find nothing, and wait.

## Two rules the schema does not enforce

Both found by running the generator, after XSD validation had already passed:

- **`EVENT-ID` must be < 0x8000.** SOME/IP splits the 16-bit message-ID field by
  its top bit (0 methods, 1 events); the binding sets that bit, so the model
  carries only the lower 15 bits. The earlier arc42 deck specified `0x8001`,
  which `ara-gen` rejects outright. Corrected here to `0x0002` / `0x0003`; on
  the wire the events are still `0x8002` / `0x8003`.
- **No `--` inside an XML comment.** Illegal XML; the parse fails before any
  AUTOSAR rule is reached.

## Generate

    aragen -o /tmp/aragen-out  ./model  <capi>/isoft/arxmls/models/

`isoft/arxmls/models/` **must** be on the input list. The model references
things defined there and nothing here redeclares them:

| Referenced | Defined in |
|---|---|
| `/AUTOSAR/StdTypes/*` | `common/base_type.arxml` |
| `/ISOFT/Development/Machine1` | `development_machines/machine1/common/machine.arxml` |
| `/ISOFT/Development/MachineDesign1/CommunicationConnector1` | `development_machines/system/machine1_design.arxml` |
| `/ISOFT/Development/Machine1/OS/DefaultResourceGroup` | `.../simple_module_instantiations/os.arxml` |
| `/ISOFT/ProcessModes/ProcessStateMachine` | `development_machines/system/mode_groups.arxml` |

Observed output (generating for `-e /VisionPilotApp/exe/visionpilotd`):

    includes/av/vp/cm/vehiclestatusservice_proxy.h
    includes/av/vp/cm/vehiclecontrolservice_skeleton.h
    includes/av/vp/cm/impl_type_vehiclespeed.h
    includes/av/vp/cm/impl_type_actuationcommand.h
    net-bindings/av/vp/cm/nsomeip/*.h
    net-bindings/av/vp/cm/*_common.cpp
    net-bindings/visionpilot/runtime.cpp
    net-bindings/visionpilot/{VehicleStatusService,VehicleControlService}_runtime.cpp

Two things this layout implies, both easy to get wrong:

1. `includes/` and `net-bindings/` are **both** include roots.
2. The `.cpp` files under `net-bindings/` must be **compiled into the
   application**. They define `Initialize<Service>()` / `Deinitialize<Service>()`,
   which `ara::core::Initialize()` calls to register each service with its
   binding. Leave them out and the application links, starts, and then silently
   never offers or finds anything — it looks like a discovery fault, not a build
   fault. `ap_interface/CMakeLists.txt` globs and compiles them, and defines
   `HAS_NSOMEIP_BINDING` to select the matching branch of `runtime.cpp`.

Then: `cmake -DENABLE_AP_INTERFACE=ON -DARA_GEN_OUTPUT=/tmp/aragen-out ..`
(the output **root**, not `includes/`).

## Naming contract

The InstanceSpecifier a process passes to `ara::com` is
`<executable>/<root SWC prototype>/<port>`:

| Process | InstanceSpecifier | Interface |
|---|---|---|
| VisionPilot | `visionpilotd/vp_root/RPort_VehicleStatus` | requires `VehicleStatusService` |
| VisionPilot | `visionpilotd/vp_root/PPort_VehicleControl` | provides `VehicleControlService` |
| VehicleBench | `vehiclebenchd/vb_root/PPort_VehicleStatus` | provides `VehicleStatusService` |
| VehicleBench | `vehiclebenchd/vb_root/RPort_VehicleControl` | requires `VehicleControlService` |

Renaming an executable, root prototype or port silently breaks the binding at
run time. There is no build-time check — the symptom is discovery that never
completes.

## The state asymmetry

In `integration/deployment.arxml`:

| Process | Driving | Degraded |
|---|---|---|
| `visionpilot_process` | listed | **not listed** |
| `vehiclebench_process` | listed | listed |

On `SetState(AvPilotFG.Degraded)`, EM stops VisionPilot and keeps the other
process, because the manifest says so. No C++ implements the degrade — it is
the one behaviour this port buys that ROS 2 cannot express, and it lives
entirely in this file.

## Deliberately not modelled

**PHM supervision.** `ara-gen` supports `PHM-SUPERVISED-ENTITY-INTERFACE`
(`generator/parser/exe_builder.py`), but CAPI ships **no example of it** — not
in `isoft/arxmls`, not in `samples`. Authoring it from parser source is a
larger and riskier job than the communication model and is not needed to verify
the data path, so `ap.supervised_entity_instance` is empty and checkpoints are
dropped. The `ReportCheckpoint` calls stay in `ap_runtime` and cost nothing
while unconfigured.

When the supervision model is authored, the checkpoint IDs must equal
`visionpilot::ap::Checkpoint` (0..3). `ara::phm` transports the enum as a plain
`uint32_t`, so a mismatch is silent — no compile error, no run-time error, just
supervision watching the wrong transition.

**E2E protection.** `isoft/e2e` ships twelve compiled profiles; none is bound to
these events yet. A fixed-rate stream wants a counter and a timeout rather than
only a CRC, so P04 or P07 are the candidates.
