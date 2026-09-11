# Building CAPI, and generating this model

git clone --recursive https://github.com/baonguyenhobby/Demo_VisionPilot_CAPI.git
git submodule update --init --recursive

Two separate toolchains, often confused:

| | What it is | Language | Needed for |
|---|---|---|---|
| **`sdk-utils`** | builds CAPI itself into an SDK | Python + C++ | the `ara::` libraries you link against |
| **`ara-gen`** | turns ARXML into C++ and manifests | Python only | the proxy/skeleton headers your code includes |

`ara-gen` does **not** need the SDK. You can generate and inspect the model
before CAPI has ever been compiled — which is the cheapest way to find modelling
mistakes.

---

## 0. The Ubuntu 24.04 patch: GCC 13 shall be used instead of GCC 9
```bash
cd ~/Demo_VisionPilot_CAPI
git -C capi apply ../patches/0001-capi-build-on-ubuntu-24.04-gcc-13.patch
```
  #sed -i 's/xml_parser\.setElementClassLookup(/xml_parser.set_element_class_lookup(/' \
  #    isoft/ara-gen/generator/common/lxml_preparser.py
  
  #Note: comment out add_compile_options(-Werror) to relaxed GCC 13 warning classes
  #for m in log per ucm idsm; do
  #  sed -i 's/^\(\s*\)add_compile_options(-Werror[^)]*)/\1# add_compile_options(-Werror)  # relaxed: GCC 13 warning classes/' \
  #    $m/CMakeLists.txt
  #done
---

## 1. Dependencies

```bash
sudo apt install -y build-essential pkg-config cmake make patch tar sed git \
    gperf autoconf libtool pigz python3-pip git-lfs
git lfs install

pip install gitpython json5 jsonpath pyelftools Jinja2 lxml xmltodict pyinstaller psutil
```

Versions CAPI tested with: gitpython 3.1.50, json5 0.14.0, jsonpath 0.82.2,
pyelftools 0.32, Jinja2 3.1.6, lxml 4.9.1, xmltodict 0.14.2, pyinstaller 6.21.0,
psutil 7.2.2. On 24.04 take current versions and apply the `lxml` fix above.
`jsonpath 0.82.2` is only needed by `ara-gen`.

`sdk-utils build` **downloads and compiles** these from source, so the machine
needs network access:

| | | |
|---|---|---|
| openssl 3.5.1 | rapidjson 1.1.0 | Fast-DDS 3.4.0 |
| Fast-CDR 2.3.0 | foonathan-memory 0.7 | tinyxml2 9.0.0 |
| asio 1.36.0 | libseccomp 2.5.1 | concurrentqueue 1.0.4 |
| Eigen 5.0.1 | zlib 1.2.11 | googletest 1.10 |

---

## 2. Environment

```bash
cat >> ~/.bashrc <<'EOF'

# --- CAPI / VisionPilot AP overlay ---
export CAPI_SRC_DIR="$HOME/Demo_VisionPilot_CAPI/capi"
export SDK_UTILS="$CAPI_SRC_DIR/isoft/sdk-utils/bin/sdk-utils.py"
export CAPI_BUILD_DIR="$HOME/capi-build"
export SDK_OUTPUT_DIR="$HOME/capi-build/sdk"
export SDK_INST_DIR="$HOME/capi-sdk"
export ARA_SYSROOT="$SDK_INST_DIR/ara-sysroot"
export ARA_GEN_OUT="$HOME/aragen-4aa"
export ARA_GEN_EXECUTABLE_OUTPUT="$HOME/.isoft/tmp/ara_binout"
export OVERLAY="$HOME/Demo_VisionPilot_CAPI/av-stack/overlay"
export PATH="$PATH:$CAPI_SRC_DIR/isoft/ara-gen"
EOF

source ~/.bashrc
chmod +x $SDK_UTILS
```

## 3. Build the SDK

```bash
cd ~/Demo_VisionPilot_CAPI
echo "gitdir: $HOME/Demo_VisionPilot_CAPI/.git/modules/capi" > capi/.git
git -C capi rev-parse --short HEAD      # 112916a — proves the fix is sound

${SDK_UTILS} -d build -o "${CAPI_BUILD_DIR}" -v 2608 "apall#${CAPI_SRC_DIR}"
```

```bash
${SDK_UTILS} -d pack -o "${SDK_OUTPUT_DIR}" "${CAPI_BUILD_DIR}"
ls -l "${SDK_OUTPUT_DIR}"/SDK-*.run

"${SDK_OUTPUT_DIR}"/SDK-*.run "${SDK_INST_DIR}"
```

```bash
grep -rIl "${SDK_INST_DIR}" ${SDK_INST_DIR} | head
find ${SDK_INST_DIR} -name "*onfig.cmake" | sed "s|${SDK_INST_DIR}||" | sort
ls $ARA_SYSROOT/usr/lib | head -40
```

```bash
FW="$ARA_SYSROOT/ara/framework/1.0.0"
grep -rh "add_library(" $FW/lib/cmake/{ara-core,ara-com,ara-log,ara-exec-execution-client,ara-phm-client,ara-com-nsomeip}/ | sort -u
```


`apall#<path>` is `NAME#URL` — package `apall` taken from a local path rather
than downloaded. `-v 2608` is just the build version string (defaults to today's
date). `-b` selects the build type, default `Debug`. Add `-s` if disk is tight.

## 4. Generate from the model

```bash
cd <this overlay>
aragen -o ~/aragen-4aa  av-stack/model/  capi/isoft/arxmls/models/
```

CAPI's own `isoft/arxmls/models/` must be on the input list — the model
references `Machine1`, its connector, its resource group, `ProcessStateMachine`
and `/AUTOSAR/StdTypes/*` from there.

Useful without generating anything:

```bash
aragen --list-processes  model/ ${CAPI_SRC_DIR}/isoft/arxmls/models/   # expect /AvDeployment/*
aragen --list-machines   model/ ${CAPI_SRC_DIR}/isoft/arxmls/models/
aragen -e /VisionPilotApp/exe/visionpilotd -o /tmp/out  model/ ...     # one executable only
```

### The output layout matters

```
/tmp/aragen-out/
  includes/       av/vp/cm/*_proxy.h, *_skeleton.h, impl_type_*.h   <- you #include these
  net-bindings/   av/vp/cm/nsomeip/*.h  AND  **/*.cpp               <- these must be COMPILED
  processes/      <process>_manifest.json, _nsomeip.json, ...       <- EM/com read these
  machines/       machine configuration
```

Both `includes/` and `net-bindings/` are include roots. The `.cpp` files under
`net-bindings/` define `Initialize<Service>()` / `Deinitialize<Service>()`, which
`ara::core::Initialize()` calls to register each service with its binding. **Omit
them and the application links, starts, and silently never offers or finds
anything.** The overlay's `ap_interface/CMakeLists.txt` globs and compiles them,
and defines `HAS_NSOMEIP_BINDING` to select the matching branch in the generated
`runtime.cpp`.

## 5. Build VisionPilot — standalone, against the installed SDK

**VisionPilot is built separately from CAPI and is not a CAPI package.** It is
an ordinary CMake project that happens to `find_package` a handful of `ara::`
targets. Two reasons this split is deliberate rather than expedient:

- VisionPilot needs ONNX Runtime, CUDA/TensorRT and OpenCV. The SDK's project
  wrapper models none of them, and teaching it to would drag the whole
  perception toolchain inside the platform build for nothing.
- The coupling is genuinely narrow: six `find_package` calls in
  `ap_interface/CMakeLists.txt`, plus the generated sources under
  `net-bindings/`. No other module in the tree knows CAPI exists.

So the SDK is a **dependency to be located**, exactly like ONNX Runtime. Find
where it put its CMake package configs:

```bash
find ${SDK_INST_DIR} -name 'ara-core*onfig.cmake' -printf '%h\n' | sort -u
```

then point the build at it:

```bash
cmake -DONNXRUNTIME_ROOT=/path/to/onnxruntime \
      -DENABLE_AP_INTERFACE=ON \
      -DARA_GEN_OUTPUT=/tmp/aragen-out \
      -DCMAKE_PREFIX_PATH="${SDK_INST_DIR}" \
      ..
make
```

Pass the aragen output **root**, not `includes/`. (The CMake accepts either, but
the root is the documented form.)

If `find_package(ara-core REQUIRED)` still fails, the configs sit deeper than
`CMAKE_PREFIX_PATH`'s search rules reach — pass the exact directory as
`-Dara-core_DIR=<dir>` (and likewise for `ara-com`, `ara-log`,
`ara-exec-execution-client`, `ara-phm-client`, `ara-com-nsomeip`) rather than
moving files around inside the SDK.

### Wiring the four AAs into the tree

They are four executables in VisionPilot's own build tree, not four projects.
They share `config` / `logging` / `common`, each links a different subset of the
existing module libraries, and this tree already knows how to find ONNX Runtime,
OpenCV and CUDA. What is *standalone* is the relationship to CAPI, not the
applications' relationship to each other.

Top-level `CMakeLists.txt`:

```cmake
if(ENABLE_AP_INTERFACE)
    list(APPEND CMAKE_MODULE_PATH ${CMAKE_SOURCE_DIR}/cmake)
    include(AraApplication)
    ara_find_capi()          # find_package the six ara:: packages -> ap::capi
    ara_generated_common()   # net-bindings/av/vp/cm/**.cpp -> ap_generated_common
    add_subdirectory(modules/middleware_interfaces/ap_interface)   # ap_runtime
    add_subdirectory(apps)   # sensingd, perceptiond, planningd, controld
endif()
```

Then:

```bash
make -j$(nproc)
make ara-install        # copies all four into ARA_GEN_EXECUTABLE_OUTPUT
```

`ara-install` is the standalone equivalent of `ab -i`, and §6 explains why it is
not optional.

Two things `cmake/AraApplication.cmake` gets right that a naive setup does not:

- **`OUTPUT_NAME` per executable.** `add_ara_executable(... ARA_NAME sensingd)`
  makes the binary's filename match the model's `EXECUTABLE` short-name. The
  configurator resolves `/SensingApp/exe/sensingd` by matching that name against
  filenames it finds; a target called `vp_sensing` is never matched.
- **Per-executable binding sources.** ara-gen emits two kinds of `.cpp` under
  `net-bindings/`: shared serialisation glue under `av/vp/cm/`, and
  `Initialize<Service>()` / `Deinitialize<Service>()` under a directory per
  executable. Only the first belongs in a shared library. A
  `GLOB_RECURSE net-bindings/*.cpp` — which is what the single-binary version of
  this overlay did — links all four executables' `Initialize()` definitions into
  every process and registers services a process has no ports for. Each
  executable compiles only its own.

The per-executable directory name is chosen by ara-gen and is not guaranteed to
equal the executable name, so check once and fix the four `BINDING_DIR` lines in
`apps/CMakeLists.txt` if they differ:

```bash
ls ${ARA_GEN_OUTPUT}/net-bindings/
```

### Runtime library resolution — the part that bites

Linking at build time is not enough. EM starts `visionpilotd` from inside
`${ARA_SYSROOT}`, not from the SDK tree, and it does not inherit your shell's
`LD_LIBRARY_PATH`. Two workable answers:

- **Dev host** — the SDK sits at a stable absolute path, so a baked RPATH is
  fine:
  `-DCMAKE_BUILD_WITH_INSTALL_RPATH=ON -DCMAKE_INSTALL_RPATH=<sdk lib dir>`.
- **Target** — ship the `ara::` shared objects into the SWCL's own `lib/`
  beside `bin/visionpilotd`, and build with
  `-DCMAKE_INSTALL_RPATH='$ORIGIN/../lib'`. This is what the
  `swcls/<name>/<version>/{bin,lib}` layout exists for, and it makes the SWCL
  self-contained — which is what UCM needs anyway to update it as a unit.

A missing `.so` here shows up as EM reporting the process failed to start with
**no application log at all**, because it never reached `main()`. Check with
`ldd` against the sysroot copy, not against your build tree.

### aarch64

The SDK built here is `...-x86_64-ubuntu24.04-native-...`. The Orin Nano needs
an aarch64 SDK — built natively on the board (slow but straightforward) or
cross-built. Nothing in this overlay is architecture-specific; the model and
the generated code are identical either way.

## 6. Put the executable where the configurator looks

`config.sh` does not read your build directory. `buildExecutableFQN2PathDict`
scans `ARA_GEN_EXECUTABLE_OUTPUT` — `~/.isoft/tmp/ara_binout` by default — and
matches each executable's **short-name from the model** against the filenames
it finds there. CAPI's own samples get this from `ab -i`; a standalone build
does it explicitly:

```bash
install -D -m 0755 visionpilotd \
    "${ARA_GEN_EXECUTABLE_OUTPUT:-$HOME/.isoft/tmp/ara_binout}/visionpilotd"
```

Two failure modes, both of which cost real time if you don't know their shape:

- **Nothing installed.** `config.sh` runs for a minute, then fails with
  `IntegrateSWCL, can't find executablePath for
  executableFQN:/VisionPilotApp/exe/visionpilotd ... exists:False`. The real
  cause is `self._executableFQN2PathDict:{}` further up the log — an empty
  scan, not a broken model.
- **Installed under the wrong name.** Identical error. The model says
  `visionpilotd`; upstream CMake produces `VisionPilot`. The patch sets
  `OUTPUT_NAME visionpilotd` for exactly this reason.

## 7. Configure and run the machine

```bash
MACHINE_NAME=Machine1 MACHINE_IP=<this host's IP> AA_SRC_FOLDER=<app dir> \
  ${SDK_INST_DIR}/ara-sysroot/config.sh -m ${MACHINE_NAME} -s ${ARA_SYSROOT} \
                                        -a ${AA_SRC_FOLDER} -p ${MACHINE_IP}

sudo -E ${ARA_SYSROOT}/run.sh -R                       # start the machine
sudo -E ${ARA_SYSROOT}/run.sh -c AvPilotFG.Driving     # start both applications
sudo -E ${ARA_SYSROOT}/run.sh -c AvPilotFG.Degraded    # <- stops VisionPilot, keeps the other
sudo -E ${ARA_SYSROOT}/run.sh -s                       # stop the machine
```

`AA_SRC_FOLDER` must cover **both** applications. A config run that integrates
only one of them still reports `ret_code:0`, and produces a machine where the
surviving process calls `StartFindService` and sits at
`bindHandles: {"vector": {0}}` forever — a silent half-deployment that reads
like a discovery bug.

`sudo -E` matters: plain `sudo` drops `ARA_SYSROOT` and `ISOFT_ARA_RUNTIME_DIR`,
and the `-c` command then talks to a different runtime directory than the
machine is using.

Application function groups do **not** start themselves. EM auto-starts
`MachineFG` only; `AvPilotFG` stays `Off` until something requests a
transition, which is what the `-c` line does. There is no error if you forget —
the processes simply never start.

Logs go to DLT; use [dlt-viewer 2.30.0](https://github.com/COVESA/dlt-viewer/releases/tag/2.30.0)
and filter on context `#VPA` for this application's own messages.

That `Degraded` line is the demonstration worth recording: EM stops one process
and keeps the other purely because the execution manifest says so.

---

## Modelling rules the schema will not catch

Found by running `ara-gen`, not by XSD validation:

- **`EVENT-ID` must be < 0x8000.** SOME/IP splits the 16-bit message-ID field by
  its top bit (0 methods, 1 events) and the binding sets that bit itself, so the
  model carries only the lower 15 bits. `0x8001` — which the earlier arc42 deck
  specified — is rejected with
  `[CM ] CODE-001: Events in some/ip must have event id < 0x8000`.
  On the wire the event is still message ID `0x8002`; that is the binding's doing.
- **No `--` inside XML comments.** Illegal in XML, and `ara-gen` fails the parse
  before it reaches any AUTOSAR rule.
- Provided and required instances of the same interface must carry the **same**
  instance ID or discovery silently never matches.
