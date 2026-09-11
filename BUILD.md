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
#git -C capi         diff > patches/0001-capi-build-on-ubuntu-24.04-gcc-13.patch
#git -C vision_pilot diff > patches/0002-vision_pilot-enable-ap-interface.patch
cd ~/Demo_VisionPilot_CAPI
git -C capi apply ../patches/0001-capi-build-on-ubuntu-24.04-gcc-13.patch
git -C vision_pilot diff > patches/0002-vision_pilot-enable-ap-interface.patch
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

sudo apt update && sudo apt install -y --no-install-recommends \
    build-essential cmake git wget ca-certificates \
    libopencv-dev \
    libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev libgstreamer-plugins-bad1.0-dev \
    gstreamer1.0-plugins-base gstreamer1.0-plugins-good \
    gstreamer1.0-plugins-bad gstreamer1.0-plugins-ugly gstreamer1.0-nice \
    libnice-dev libsrtp2-dev libboost-system-dev nlohmann-json3-dev \
    coinor-libipopt-dev libcppad-dev liblapack-dev libblas-dev

hdr_dir="$(dirname "$(find /usr/include -name IpIpoptApplication.hpp | head -1)")"
echo "$hdr_dir"
[ -n "$hdr_dir" ] && sudo ln -sfn "$hdr_dir" /usr/include/coin-or
ls -ld /usr/include/coin-or	
	
mkdir -p ~/ort_extract && cd ~/ort_extract
wget -O ort.tgz "https://github.com/microsoft/onnxruntime/releases/download/v1.26.0/onnxruntime-linux-x64-1.26.0.tgz"
tar -xzf ort.tgz
mv */ ~/onnxruntime          # glob, because the inner dir name isn't guaranteed to match
cd ~ && rm -rf ~/ort_extract
ls ~/onnxruntime             # expect include/ and lib/	
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
export ONNXRUNTIME_ROOT="$HOME/onnxruntime"
EOF

source ~/.bashrc
chmod +x $SDK_UTILS
```

## 3. Build the SDK

```bash
cd ~/Demo_VisionPilot_CAPI
rm -rf "$CAPI_BUILD_DIR"
echo "gitdir: $HOME/Demo_VisionPilot_CAPI/.git/modules/capi" > capi/.git
git -C capi rev-parse --short HEAD      # 112916a — proves the fix is sound

${SDK_UTILS} -d build -o "${CAPI_BUILD_DIR}" -v 2608 "apall#${CAPI_SRC_DIR}"
```

```bash
rm -rf "$CAPI_BUILD_DIR/tmp-sdk-packager" "$SDK_OUTPUT_DIR"
$SDK_UTILS -d pack -o "$SDK_OUTPUT_DIR" "$CAPI_BUILD_DIR"
ls -l "$SDK_OUTPUT_DIR"/SDK-*.run          # timestamp must be NOW

rm -rf "$SDK_INST_DIR"
"$SDK_OUTPUT_DIR"/SDK-*.run "$SDK_INST_DIR"
grep -n "uint64" "$ARA_SYSROOT"/ara/framework/1.0.0/include/isoft/e2e/Platform_Types.h
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

## 4. Generate from the model

```bash
cd ~/Demo_VisionPilot_CAPI

# 4a. manifests
aragen -o "$ARA_GEN_OUT"  av-stack/model/  capi/isoft/arxmls/models/

# 4b. headers + net-bindings
aragen -e /SensingApp/exe/sensingd,/PerceptionApp/exe/perceptiond,/PlanningApp/exe/planningd,/ControlApp/exe/controld \
       -o "$ARA_GEN_OUT"  av-stack/model/  capi/isoft/arxmls/models/
```

#Note: if it throws an error about Element '{http://autosar.org/schema/r4.0}ARRAY-SIZE' at /av-stack/model/data_types.arxml
```bash
cd ~/Demo_VisionPilot_CAPI/av-stack/model
python3 - <<'EOF'
p = 'data_types.arxml'
s = open(p).read()
line = '              <ARRAY-SIZE>20</ARRAY-SIZE>\n'
cat  = '              <CATEGORY>ARRAY</CATEGORY>\n'
assert s.count(line) == 1, "ARRAY-SIZE not found exactly once"
assert s.count(cat)  == 1, "CATEGORY>ARRAY not found exactly once"
s = s.replace(line, '').replace(cat, cat + line, 1)
open(p, 'w').write(s)
EOF
sed -n '282,292p' data_types.arxml
``` 

#Check it produced what the build needs:
```bash
ls "$ARA_GEN_OUT"/processes/           # 4 dirs
ls "$ARA_GEN_OUT"/includes/av/vp/cm/   # the service headers
ls "$ARA_GEN_OUT"/net-bindings/        # av/ + one dir per executable
```

#Inspection only, generates nothing:
```bash
aragen --list-processes  av-stack/model/  capi/isoft/arxmls/models/   # expect /AvDeployment/*
aragen --list-machines   av-stack/model/  capi/isoft/arxmls/models/
```

## 5. Build VisionPilot — standalone, against the installed SDK
```bash
./av-stack/scripts/apply-overlay.sh
```

```bash
cd ~/Demo_VisionPilot_CAPI/vision_pilot/VisionPilot
rm -rf build && mkdir build && cd build
cmake -DENABLE_AP_INTERFACE=ON -DENABLE_ROS2_INTERFACE=OFF -DGPU=OFF \
      -DARA_GEN_OUTPUT="$ARA_GEN_OUT" \
      -DCMAKE_PREFIX_PATH="$ARA_SYSROOT/ara/framework/1.0.0;$ARA_SYSROOT/usr" \
      -DONNXRUNTIME_ROOT="$HOME/onnxruntime" \
      .. >/dev/null && make vp_control -j1 2>&1 | grep -E "error|Error" | head -30
```

# In case it throws compilation errors, use this command to see details: 
  ```bash
  cd ~/Demo_VisionPilot_CAPI/vision_pilot/VisionPilot/build
  make vp_planning vp_sensing vp_perception vp_control -j$(nproc) 2>&1 | grep -E "error:" | head -20
  ```
# In case it throws linking errors e.g. collect2: error: ld returned 1 exit status, use this command to see details:   
  ```bash
  cd ~/Demo_VisionPilot_CAPI/vision_pilot/VisionPilot/build
  make vp_planning vp_sensing vp_perception vp_control -j$(nproc) 2>&1 | grep -E "undefined reference|cannot find -l|DSO missing" | sed 's/.*undefined reference to //' | sort -u | head -40
  ```
Phase	    Signature
compile	    path/file.cpp:120:15: error: ...
link	    undefined reference to, cannot find -lfoo, DSO missing from command line
link        (always last)	collect2: error: ld returned 1 exit status
make        itself	make[2]: *** [...] Error 1

#Note: if we changed av-stack to solve compilation/linking errors, trigger:
```bash
cd ~/Demo_VisionPilot_CAPI && ./av-stack/scripts/apply-overlay.sh >/dev/null
cd vision_pilot/VisionPilot/build
make vp_sensing vp_perception vp_planning vp_control -j$(nproc) 2>&1 | grep -E "undefined reference|cannot find -l|error:|Error [0-9]" | sort -u | head -30
ls -l sensingd perceptiond planningd controld
```

```bash
cd vision_pilot/VisionPilot/build
make ara-install
ls -l "$ARA_GEN_EXECUTABLE_OUTPUT"
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
