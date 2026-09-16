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
```

#C++ Algorithmic Differentiation (CppAD) includes `<coin-or/IpIpoptApplication.hpp>`; Ubuntu 24.04 ships the headers
#under a different directory name. Resolve it rather than hard-coding:
```bash
hdr_dir="$(dirname "$(find /usr/include -name IpIpoptApplication.hpp | head -1)")"
echo "$hdr_dir"
[ -n "$hdr_dir" ] && sudo ln -sfn "$hdr_dir" /usr/include/coin-or
ls -ld /usr/include/coin-or	
```

### ONNX Runtime
```bash	
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
export SDK_SYSROOT="$SDK_INST_DIR/ara-sysroot"   # headers, libs, config.sh, build.sh
export ARA_SYSROOT="$HOME/capi-machine"          # the machine root config.sh builds, run.sh boots
export ARA_GEN_OUT="$HOME/aragen-4aa"
export ARA_GEN_EXECUTABLE_OUTPUT="$HOME/.isoft/tmp/ara_binout"
export OVERLAY="$HOME/Demo_VisionPilot_CAPI/av-stack/overlay"
export PATH="$PATH:$CAPI_SRC_DIR/isoft/ara-gen"
export ONNXRUNTIME_ROOT="$HOME/onnxruntime"
export ISOFT_ARA_FSH_SYSROOT="$ARA_SYSROOT"
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

### Copy weights of AutoDrive, AutoSteer, AutoSpeed from VisionPilot into /usr/share/visionpilot

```bash
# configuration
sudo mkdir -p /usr/share/visionpilot/config
sudo cp ~/Demo_VisionPilot_CAPI/vision_pilot/VisionPilot/config/* /usr/share/visionpilot/config/

# model weights — NOT in config/, see the table below
sudo mkdir -p /usr/share/visionpilot/modules/models/weights
sudo cp ~/Demo_VisionPilot_CAPI/vision_pilot/VisionPilot/modules/models/weights/*.onnx \
        /usr/share/visionpilot/modules/models/weights/
```

### The OpenLane dataset
```bash
sudo apt install -y pipx && pipx install gdown && pipx ensurepath && exec $SHELL -l

mkdir -p ~/data && cd ~/data
gdown --folder "https://drive.google.com/drive/folders/1-Sxgz3XHzFD6XtETz1sVFRtDKY3W57QB"
ls ~/data/test_data/
```

```bash
D="$HOME/dataset/test_data/test_open_lane_2"      # the clip you want

sudo sed -i "s|^source.input_video .*|source.input_video         = $D/input.mp4|"        /usr/share/visionpilot/config/vision_pilot_test.conf
sudo sed -i "s|^source.input_vehicle_speed .*|source.input_vehicle_speed = $D/frame_speed.txt|" /usr/share/visionpilot/config/vision_pilot_test.conf
sudo sed -i 's|^source.video_loop .*|source.video_loop = true|'                          /usr/share/visionpilot/config/vision_pilot_test.conf
sudo sed -i 's|^engine.provider .*|engine.provider     = cpu|'                           /usr/share/visionpilot/config/vision_pilot.conf
sudo cp "$D/H.yaml" /usr/share/visionpilot/config/H.yaml

# homography_C_matrix.yaml is DERIVED from the clip's H.yaml and ships nowhere
cd ~/Demo_VisionPilot_CAPI/vision_pilot/VisionPilot/scripts
python3 - <<EOF
import sys; sys.path.insert(0, '.')
from pathlib import Path
import cv2, numpy as np
from find_homography_C_matrix import load_homography_H_matrix, find_homography_C_matrix
C = find_homography_C_matrix(load_homography_H_matrix(Path("$D/H.yaml")))
fs = cv2.FileStorage("/tmp/homography_C_matrix.yaml", cv2.FILE_STORAGE_WRITE)
fs.write("C", C.astype(np.float32)); fs.release()
EOF
sudo cp /tmp/homography_C_matrix.yaml /usr/share/visionpilot/config/
```

### Visualisation

`show_window` is hard-coded `false` in `perception/main.cpp` — an EM-started
process inherits no `DISPLAY`. The overlay is served over WebRTC instead:

```bash
sudo sed -i 's|^visualization_on .*|visualization_on = true|; s|^webrtc_on .*|webrtc_on = true|' \
    /usr/share/visionpilot/config/vision_pilot.conf
```

### 6. Configure the machine

# Note: Configure the machine — after every `make ara-install`
```bash
cd ~/Demo_VisionPilot_CAPI
"$SDK_SYSROOT/config.sh" -m Machine1 -a "$PWD/av-stack" -s "$ARA_SYSROOT" -p 192.168.14.98
```

`-a` and `-s` are undocumented in `--help`. `-s` must differ from the SDK sysroot
or configMachine tries to copy the SDK onto itself. `config.sh` copies the
binaries into the SWCL, so run it after `ara-install`, never before.

#After a failed config.sh. A failed run deletes ara/ara_ver1.json on the way out, so what's left is a half-written machine that the next run will happily build on top of. 
#Wipe both the SDK install and the machine, reinstall, re-run
```bash
sudo -E "$ARA_SYSROOT/run.sh" -s #Stop the machine before you delete
rm -rf "$SDK_INST_DIR" "$ARA_SYSROOT"
"$SDK_OUTPUT_DIR"/SDK-*.run "$SDK_INST_DIR"
mkdir -p "$SDK_SYSROOT/ara/framework/1.0.0/share/samples"   # stale precondition in config.sh
```

### 7. Testing AvPilotFG.Driving
```bash
# 0. confirm what the log already says
pgrep -x emd || echo "emd not running"
pgrep -x nsomeipd || echo "nsomeipd not running"

sudo -v                                        # else backgrounded run.sh exits 255 with an EMPTY log

sudo ip link add ara0 type dummy                         # else every daemon: "Cannot assign requested address"
sudo ip addr add 192.168.14.98/24 dev ara0
sudo ip link set ara0 up
sudo ip route add 224.0.0.0/4 dev ara0

cd ~/Demo_VisionPilot_CAPI/vision_pilot/VisionPilot/build
sudo mkdir -p /run/ara && sudo chown "$USER" /run/ara # else "EMD exited with 255"
```

`ara0` carries the SDK's own default machine address, so the machine survives a
DHCP change. A real host interface won't do: raw sockets fail on WSL2's mirrored
adapters, which kills `tsyncd` and takes `MachineFG` down with it.

```bash
# 1. arm the capture BEFORE perceptiond starts — the check is a one-shot static
mkdir -p /tmp/vp_frames && chmod 777 /tmp/vp_frames

# 2. boot. run.sh -R execs ara_loader in the FOREGROUND, so background it.
sudo -E "$ARA_SYSROOT/run.sh" -R > /tmp/machine.log 2>&1 &
sleep 15
pgrep -x emd && echo "machine up"

# 3. now the state changes have something to talk to
rm -f /tmp/vp_frames/*.jpg
sudo -E "$ARA_SYSROOT/run.sh" -C AvPilotFG
sudo -E "$ARA_SYSROOT/run.sh" -c AvPilotFG.Init
sleep 10
grep -a -E "init (ok|FAILED)" /tmp/machine.log | tail -4

sudo -E "$ARA_SYSROOT/run.sh" -c AvPilotFG.Driving
sleep 19
for p in sensingd perceptiond planningd controld; do printf "%-12s %s\n" "$p" "$(pgrep -x $p || echo -)"; done
ls /tmp/vp_frames/*.jpg | wc -l
```

Expected — all four with PIDs, and in the log:
```
frame ring: created /visionpilot_frames  1024x512  3 slots  9.0 MB  instance=<N>
perceptiond: found CameraFrameService
frame ring: mapped  /visionpilot_frames  1024x512  instance=<N>      <- same N
perceptiond: running
planningd: running
ctrl: tyre=0.0027 rad  accel=-3.566 m/s2  ->  v=19.87 m/s  omega=+0.019 rad/s  |  ego=20.05 m/s  age=0 ms
```

### 8. Testing AvPilotFG.Degraded
```bash
sudo -E "$ARA_SYSROOT/run.sh" -c AvPilotFG.Degraded
sleep 10
for p in sensingd perceptiond planningd controld; do printf "%-12s %s\n" "$p" "$(pgrep -x $p || echo -)"; done
grep -a -E "\[INFO\]  ctrl:" /tmp/machine.log | tail -4
```

`planningd` gone, the other three running, `age` climbing past 200 ms, and
Control at `tyre=0.0000 accel=-4.000` once the 20-step horizon is exhausted.

### 9. Stores driving video as MP4 16:9 
# Get the frame rate stored during Driving
```bash
cd /tmp/vp_frames
python3 - <<'EOF'
import glob, os, datetime
f = sorted(glob.glob("f*.jpg")); t = [os.path.getmtime(x) for x in f]
bad = [i for i in range(1, len(f)) if t[i] < t[i-1]]
print(f"MIXED - clean run is f000000..{f[bad[0]-1]} ({bad[0]} frames)" if bad
      else f"{len(f)} frames, {t[-1]-t[0]:.1f}s, {(len(f)-1)/(t[-1]-t[0]):.2f} fps")
EOF
```

```bash
cd /tmp/vp_frames
FPS=5.6 # !!!It's returned above!!!
START=0                 # first frame of your segment
N=353                   # 62.9 s at 5.6 fps
DUR=$(awk -v n=$N -v f=$FPS 'BEGIN{printf "%.3f", n/f}')

ffmpeg -y \
  -framerate $FPS -start_number $START -t $DUR -i f%06d.jpg \
  -f lavfi -t $DUR -i anullsrc=channel_layout=stereo:sample_rate=48000 \
  -vf "scale=1920:-2:flags=lanczos,pad=1920:1080:0:(1080-ih)/2:color=black,setsar=1,format=yuv420p" \
  -c:v libx264 -preset slow -crf 18 -profile:v high -level 4.0 \
  -r 30 -g 60 \
  -c:a aac -b:a 128k -shortest -movflags +faststart \
  driving_1080p.mp4

ls -lh driving_1080p.mp4
cp driving_1080p.mp4 /mnt/c/Users/$USER/Desktop/
```

### 10. Stop the running machine
sudo -E "$ARA_SYSROOT/run.sh" -c AvPilotFG.Off    # stop the four AAs (lowercase c)
sudo -E "$ARA_SYSROOT/run.sh" -s                  # stop the machine

#Verify
pgrep -x emd || echo "machine stopped"
jobs                                              # the backgrounded -R should show Done


#In case errors, look for details:
```bash
pgrep -x sensingd; pgrep -x perceptiond; pgrep -x planningd; pgrep -x controld
grep -a -nE "phase=run|\[ERROR\]|Terminated with exit code|OfferService|find" /tmp/machine.log | tail -30
grep -a -nE "terminate called|what\(\):|Terminated with exit code|Enter Timeout|terminated Unexpected" /tmp/machine.log | tail -15
```

```bash
grep -a -nE "Enter Timeout|Terminated with exit code|terminated Unexpected|\[ERROR\]|kRunning|ReportExecutionState" /tmp/machine.log | tail -25
sed -n '5020,5090p' /tmp/machine.log
```



### Function group states

```bash
sudo -E "$ARA_SYSROOT/run.sh" -c AvPilotFG.Init      # 4 init processes run and exit
sudo -E "$ARA_SYSROOT/run.sh" -c AvPilotFG.Driving   # the 4 AAs
sudo -E "$ARA_SYSROOT/run.sh" -c AvPilotFG.Degraded  # stops Planning only
sudo -E "$ARA_SYSROOT/run.sh" -c AvPilotFG.Safe      # Control only
sudo -E "$ARA_SYSROOT/run.sh" -c AvPilotFG.Off

sudo -E "$ARA_SYSROOT/run.sh" -C AvPilotFG
```

If a process exits non-zero, the group falls into Undefined and `-C` returns
empty. `-c AvPilotFG.Off` recovers it; if not, stop and re-boot the machine.



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
