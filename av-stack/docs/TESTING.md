# Testing the four-AA port on WSL2

Four stages, cheapest first. Each one falsifies something specific, and only the
last needs the full application.

**The single most useful thing you can do first: test the architecture with
stub applications.** Four processes that publish synthetic data — no ONNX, no
CUDA, no dataset, no homography — exercise the entire model, EM, the function
groups, SOME/IP discovery, IAM and the `Degraded` transition. That is the part
that is new and unproven. The perception algorithms are not new; they already
work upstream. Testing them at the same time as the middleware means every
failure has two candidate causes.

---

## Stage 0 — the model alone (no SDK, no C++, ~10 minutes)

`ara-gen` is Python-only. It does not need the CAPI runtime, so this runs today.

```bash
cd <overlay>
aragen --list-processes  model/ ${CAPI_SRC_DIR}/isoft/arxmls/models/
aragen -o /tmp/aragen-4aa  model/ ${CAPI_SRC_DIR}/isoft/arxmls/models/
```

What to check, in order of how likely it is to bite:

1. **`SteeringHorizon` generated.** This is the one construct in the model not
   checked against a CAPI example.
   ```bash
   grep -rn "SteeringHorizon\|steeringHorizonRad" /tmp/aragen-4aa/includes/ | head
   ```
   If ara-gen rejects the array type, the fallback is a single `steeringRad`
   field — but that costs Control the ability to keep following a horizon when
   Planning stops, so it is a design downgrade, not a schema edit. Decide
   deliberately.
2. **Four processes.** `--list-processes` should print `/AvDeployment/sensing_process`,
   `perception_process`, `planning_process`, `control_process`.
3. **The Degraded table survived generation.** This is the whole point of the
   port, and it is worth seeing in the generated artefact rather than trusting
   the ARXML:
   ```bash
   grep -A3 '"states"' /tmp/aragen-4aa/processes/*_manifest.json
   ```
   Expect `planning` listed under `Driving` only; the other three under
   `Driving` and `Degraded`; `control` also under `Safe`.
4. **Symbols match what the C++ will be written against** — namespace
   `av::vp::cm`, `proxy::CameraFrameServiceProxy` with member `FrameEvent`,
   `skeleton::DrivingStateServiceSkeleton` with member `DrivingStateEvent`, and
   so on for all four.

If Stage 0 fails, nothing downstream is worth attempting.

---

## Stage 1 — the shared-memory ring, by itself (no CAPI, ~an afternoon)

The ring is the part of the design that ARXML cannot express and therefore the
part no model check will ever catch. Test it as two plain processes before it
goes anywhere near `ara::com`.

Writer: read frames from `input.mp4`, warp and resize each to 1024×512,
write into slot `n % N`, bump `generation` before and after. Reader: map the
segment read-only, read the slot, re-check `generation`, checksum the pixels.

Four things to prove, all of which are real failure modes and none of which
appear under light load:

- **Lap detection.** Slow the reader down deliberately (`usleep`) until the
  writer laps it. The reader must *discard* those frames, not return torn ones.
  A ring that never reports a discard under a deliberately slow reader is not
  detecting the race — it is losing it silently.
- **Restart identity.** Kill and restart the writer. `segmentInstanceId` must
  change and the reader must re-map. If the reader keeps reading the old
  mapping, you have the bug that costs days.
- **N sizing.** `N = 3` is the minimum that keeps the writer off the slot the
  reader holds. Try `N = 2` and watch it fail, so you know the failure's
  signature when you see it later.
- **Backpressure direction.** The writer must never block on the reader. Verify
  writer frame rate is unchanged with the reader stopped entirely.

WSL2 notes for this stage:

```bash
df -h /dev/shm          # need ~12 MB for 3 slots x 2 images x 1.5 MB
ls -l /dev/shm          # the object must be readable by both processes
```
If `/dev/shm` is too small, set `size` under `[automount]`/`/etc/wsl.conf` or
`sudo mount -o remount,size=256M /dev/shm` for a session.

---

## Stage 2 — four stub AAs on CAPI (needs the C++ split, no dataset)

Four small binaries with the real generated interfaces and fake payloads:

| Process | Publishes | Consumes |
|---|---|---|
| `sensingd` | `FrameEvent` with a counter, no real pixels | — |
| `perceptiond` | `DrivingStateEvent` with a sine-wave `cte` | `FrameEvent` |
| `planningd` | `TrajectoryEvent`, horizon filled with a ramp | `DrivingStateEvent`, `SpeedEvent` |
| `controld` | `SpeedEvent`, constant 5 m/s | `TrajectoryEvent` |

Build each standalone against the SDK (see `BUILDING.md` §5), install all four
into `ARA_GEN_EXECUTABLE_OUTPUT` under the names the model declares, then:

```bash
MACHINE_NAME=Machine1 MACHINE_IP=$(hostname -I | awk '{print $1}') \
AA_SRC_FOLDER=<all four app dirs> \
  ${SDK_INST_DIR}/ara-sysroot/config.sh -m ${MACHINE_NAME} -s ${ARA_SYSROOT} \
                                        -a ${AA_SRC_FOLDER} -p ${MACHINE_IP}

sudo -E ${ARA_SYSROOT}/run.sh -R
sudo -E ${ARA_SYSROOT}/run.sh -c AvPilotFG.Driving
```

**The test that matters:**

```bash
sudo -E ${ARA_SYSROOT}/run.sh -c AvPilotFG.Degraded
```

`planningd` should stop. `sensingd`, `perceptiond` and `controld` should keep
running, and `controld` should walk down its stored steering horizon and then
safe-stop on staleness. Watch it in the EM log:

```
EMD #EMD Debug [ ... planning_process ... state change: RunningToIdle ]
```

No C++ implements that transition. If it works with stubs, it will work with
the real algorithms, because the mechanism is entirely in the manifest.

Then `-c AvPilotFG.Safe` — only `controld` survives — and `-c AvPilotFG.Off`.

Swap stubs for real code **one process at a time**, starting with `controld`
(smallest), then `sensingd`, then `planningd`, and `perceptiond` last. At every
step exactly one thing is new.

---

## Stage 3 — the real clip

### What the bench set actually is

Three files, an OpenLane-derived sample already prepared for VisionPilot:

```
input.mp4          the clip
frame_speed.txt    per-frame ego speed
H.yaml             the homography that goes WITH this clip
```

Not the raw OpenLane tree of per-segment jpgs and annotation JSON. That matters
in three ways, all of which make this easier than a research dataset would:

- `sensingd` needs no new source. `camera_interface::FileInterface` already
  reads `input.mp4` under `source.mode = video`; it moves into Sensing
  unchanged, together with `preprocess()`.
- **Do not regenerate `H.yaml`.** It is calibrated for this clip's camera. The
  "regenerate the homography" warning belongs to the *rover*, where the mounting
  is different — not here. Same for `L = 2.67 m` and `speed_limit = 33.3 m/s`:
  wrong on the rover, but they are what produced the baseline below, so leave
  them alone while you are comparing against it.
- `frame_speed.txt` is the ego-speed track, and in the AP build it is
  **Control's input, not VisionPilot's**. `controld` replays it as `SpeedEvent`;
  Planning consumes it as `ego_v`. Nothing else reads the file.

There are no lane annotations here, so ground-truth scoring is not available.
Something better is: an A/B against the upstream build on the same clip.

### The acceptance test: A/B against the monolith

The upstream binary already runs this clip on WSL2 and prints everything needed:

```
plan: tyre=-0.0460 rad  accel=1.030 m/s²  | cte=0.93m(raw=1.19m) |
      cipo=true  dist=43.8 m  vel=-2.86 m/s
Latency pre=21.8  AD=59.9  AS=55.5  ASp=61.6  parallel=61.9  wall=83.7 ms  12 fps
```

So the port is correct **iff the four-AA build produces the same `plan:`
sequence on the same clip**. No ground truth needed, and no judgement call about
whether an overlay "looks right".

1. Capture the baseline from the monolith (`ENABLE_AP_INTERFACE=OFF`,
   `ENABLE_ROS2_INTERFACE=OFF`), `video_loop = false`, `video_realtime = false`.
   `grep '^.*plan:' > baseline.log`.
2. Run the four-AA build on the same clip and config.
3. `diff` the `plan:` lines.

A *structural* difference — missing frames, a shifted sequence — is a port bug.
Small trailing-digit differences are float accumulation order and are fine;
anything visible at the printed precision is not.

**Make it deterministic first.** Perception takes 55–65 ms per frame; if
`sensingd` publishes flat out with `video_realtime = false` it will lap a
3-slot ring continuously and the two runs will not contain the same frames.
For the equivalence run, throttle Sensing to ~10 fps — below the 12–15 fps the
baseline sustains — so nothing is ever dropped. Then run it a second time flat
out, deliberately, to exercise the drop path.

Instrument `seqCounter` continuity at every consumer while you do it: a gap is
a dropped frame and invalidates that run's comparison. That check costs three
lines and will save an afternoon of diffing logs that were never comparable.

### What the split should do to the numbers

The baseline has a clean structure — `wall = pre + parallel`, exactly, on every
frame:

```
21.8 + 61.9 = 83.7   9.9 + 63.8 = 73.7   13.2 + 64.8 = 78.0
12.2 + 56.2 = 68.4  12.5 + 54.8 = 67.3    9.1 + 27.0 = 36.1
```

Preprocessing and inference run **in series** inside one process. Move
`preprocess()` into `sensingd` and they become a two-stage pipeline, so
steady-state throughput should go from `pre + parallel` to `max(pre, parallel)`
— from 67–84 ms to 55–65 ms, i.e. roughly **12–15 fps up to 15–18 fps**. The
per-frame latency is unchanged, because a frame still traverses both stages; it
is throughput that improves.

That is a prediction, not a claim — measure it. Keep the same
`Latency pre= … parallel= … wall= … fps` line in the split build, and add the
age of each frame at each stage (capture `timestampNs` → publish), so you can
see where the time actually goes across four processes.

The IPC cost, for scale: three SOME/IP events per cycle, each a few tens of
bytes over UDP loopback, is on the order of 0.3 ms against an 80 ms frame —
about 0.4%. **The pipeline is inference-bound, not IPC-bound**, which is why
splitting it into four processes is affordable at all. Splitting *Perception*
further would not be: `AD`, `AS` and `ASp` each cost 55–62 ms yet `parallel` is
61.9 ms because they run concurrently on one shared preprocessed frame. One AA
per network would serialise them across process boundaries and turn a maximum
back into a sum.

---

## WSL2 specifics, collected

| Thing | What happens | What to do |
|---|---|---|
| `/dev/video0` | Not present under WSL2 by default | Not needed: the clip is `input.mp4` and `FileInterface` already handles it. `usbipd-win` can forward a real USB camera, but nothing here requires one. |
| `/dev/shm` | Present and works | Check `df -h /dev/shm`; ~12 MB needed. Both processes must be able to open the object — check the user and resource group in `deployment.arxml`, not just your shell. |
| SOME/IP discovery | All four processes on one machine, so SD still goes over a real interface | Set `MACHINE_IP` to the address `nsomeipd` will bind (`hostname -I`). If you use loopback, `sudo ip link set lo multicast on`. Mirrored networking mode makes this behave much more like a normal host than NAT mode does. |
| Clock | WSL2 wall clock can step relative to Windows — you already have `Measure-WslClockOffset.ps1` for exactly this | **Use `CLOCK_MONOTONIC` for every staleness gate, not the wall clock.** `timestampNs` in the events is for correlating and logging; if the staleness logic keys on a clock that can jump, `ap.speed_stale_ms` becomes untunable and the `Degraded` safe-stop fires at random. This is a design point, not just a WSL annoyance. |
| `tsyncd` | Logs `time domain 1 synchronization time out` | Expected — there is no PTP grandmaster on the bench. Ignore it; do not build anything that depends on `tsync` here. |
| `SCHED_RR` | Works as root | Run via `sudo -E`. Check the boot log for `Invalid SchedulingPolicy of { RR }, set to default value {Other}` — EM downgrades silently and starts anyway. |
| GPU | CUDA works in WSL2 with the NVIDIA Windows driver | For architecture testing, CPU ONNX Runtime is fine. Replay is not real-time and does not need to be — decouple the rate in `sensingd` and let the pipeline run as slow as it likes. |
| `sudo` | Drops `ARA_SYSROOT` and `ISOFT_ARA_RUNTIME_DIR` | Always `sudo -E`, or the second terminal talks to a different runtime directory than the machine. |

---

## Order, and what each step buys

1. **Stage 0** — the model is generatable. Minutes. Do it before anything else.
2. **Stage 1** — the ring is correct under lap and restart. An afternoon, and it
   is the only way you will ever find those two bugs deliberately rather than
   at 30 fps.
3. **Stage 2 with stubs** — EM, function groups, discovery, IAM and the
   `Degraded` transition all work. This is the demonstration worth recording.
4. **Stage 2, one real process at a time** — each swap has exactly one new
   variable.
5. **Stage 3** — the real clip, diffed line-for-line against the monolith's
   `plan:` output rather than an overlay that looks about right.

Baseline numbers are from the upstream build run on WSL2 against this clip.
OpenLane background: [OpenDriveLab/OpenLane](https://github.com/OpenDriveLab/OpenLane).
