# License

```
MIT License

Copyright (C) 2026 Nguyen Nguyen (baonguyenpro@gmail.com)

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

The MIT license above covers **only** the original work authored in this
repository:

```
av-stack/model/          the AUTOSAR model (ARXML)
av-stack/overlay/        the four Adaptive Applications, the shared-memory
                         frame ring, and the CMake that builds them
av-stack/scripts/        helper scripts
av-stack/docs/           documentation
patches/                 the patch files themselves, as authored diffs
README.md, BUILD.md, LICENSE.md
```

Everything reached through `vision_pilot/` and `capi/` is third-party. Neither
upstream's source is copied into this repository; both are git submodules
pinned to an upstream commit, and each remains governed by its own license.

This repository is a reference and teaching scaffold demonstrating an AUTOSAR
Adaptive decomposition. It is **not intended for deployment on a real
vehicle**.

---

## ⚠ AUTOSAR CAPI — released for information only, not open source

`capi/` is the AUTOSAR Common Adaptive Platform Implementation. **It is not
released under an open-source license.** Read this before using anything in
this repository for any purpose beyond study.

`capi/LICENSE` states, verbatim:

> Disclaimer
>
> This work (specification and/or software implementation) and the material
> contained in it, as released by AUTOSAR, is for the purpose of information
> only. AUTOSAR and the companies that have contributed to it shall not be
> liable for any use of the work.
>
> The material contained in this work is protected by copyright and other
> types of intellectual property rights. The commercial exploitation of the
> material contained in this work requires a license to such intellectual
> property rights.
>
> This work may be utilized or reproduced without any modification, in any
> form or by any means, for informational purposes only. For any other
> purpose, no part of the work may be utilized or reproduced, in any form
> or by any means, without permission in writing from the publisher.
>
> The work has been developed for automotive applications only. It has
> neither been developed, nor tested for non-automotive applications.
>
> The word AUTOSAR and the AUTOSAR logo are registered trademarks.

Source: https://github.com/AUTOSAR/capi — pinned here at `112916a` (v1.0.0).

The CAPI baseline was contributed by **iSOFT Infrastructure Software Co., Ltd.**,
Copyright (c) 2024-2026 (see `capi/NOTICE`). `capi/LICENSES/` and `capi/NOTICE`
additionally document the third-party components CAPI itself includes or
depends on; those are unaffected by this repository and remain governed by
their own terms.

**AUTOSAR and the AUTOSAR logo are registered trademarks.** Nothing in this
repository is endorsed by, affiliated with, or approved by AUTOSAR or iSOFT.

### Two things this repository does that a reader should weigh

**1. It patches CAPI source.** `patches/0001-capi-build-on-ubuntu-24.04-gcc-13.patch`
modifies six files in `capi/`, including `isoft/e2e/src/isoft/e2e/Platform_Types.h`,
which implements AUTOSAR-standard platform types. The disclaimer above permits
utilization or reproduction *without any modification*, for informational
purposes; any other purpose requires written permission from the publisher.
This repository does not redistribute modified CAPI source — it distributes a
diff, applied locally by the user — but applying it does modify the work on
your machine.

**2. The patch files contain small excerpts of CAPI source.** A unified diff
carries a few lines of surrounding context from the original files. Those
context lines are CAPI's material, reproduced here only to the extent needed to
describe the change.

Whether your intended use falls within "informational purposes" is a question
for you and, if the use is commercial, for your legal advisor. This file is
documentation, not legal advice, and its author is not a lawyer. If you intend
to exploit any of this commercially, obtain the AUTOSAR license the disclaimer
refers to — an AUTOSAR partnership — before doing so.

---

## Third-party components

### 1. VisionPilot (git submodule — `vision_pilot/`)

```
Source:   https://github.com/autowarefoundation/vision_pilot
Pinned:   f9fb997
License:  Apache License, Version 2.0  (vision_pilot/LICENSE)
Copyright: the Autoware Foundation and contributors
```

Its code is not copied into this repository; the submodule references the
upstream commit. See that repository's `LICENSE`.

As of the pinned commit `f9fb997`, VisionPilot ships **no** `NOTICE` file, so
the attribution-notice obligation of Apache 2.0 §4(d) adds nothing beyond
retaining the license itself. If a later upstream commit introduces one, that
file must accompany any redistribution and this paragraph needs revisiting.

This repository modifies two of its files via
`patches/0002-vision_pilot-enable-ap-interface.patch`:

```
VisionPilot/CMakeLists.txt       adds the ENABLE_AP_INTERFACE option and the
                                 CAPI locate/generate hooks
VisionPilot/app/CMakeLists.txt   includes av_stack.cmake under that option,
                                 and links cppad_lib into the upstream target
```

Apache 2.0 §4(b) requires modified files to carry prominent notices stating that
they were changed. The patch file is that record: it states what changed, where,
and why. Everything else this project adds to the VisionPilot tree is **new
files** under `av-stack/overlay/`, copied in by `av-stack/scripts/apply-overlay.sh`
and covered by the MIT license above — no upstream file is overwritten.

VisionPilot also ships `DISCLAIMER.md` and `Functional_Safety/`. Read them. They
apply to the upstream project and are not superseded by anything here.

### 2. Build and runtime dependencies (not redistributed)

None of the following is included in this repository. `BUILD.md` instructs the
user to obtain each from its own upstream, and each is governed solely by its
own license, which you should consult directly:

| component | obtained from | used for |
|---|---|---|
| ONNX Runtime | github.com/microsoft/onnxruntime (release tarball) | inference |
| OpenCV | distribution package `libopencv-dev` | image handling |
| Ipopt | distribution package `coinor-libipopt-dev` | MPC solver |
| CppAD | distribution package `libcppad-dev` | automatic differentiation |
| GStreamer, libnice, libsrtp | distribution packages | WebRTC visualisation |
| Boost, Eigen, nlohmann/json | distribution packages | various |

CAPI's own bundled and vendored dependencies — Fast DDS, OpenSSL, Eigen,
TinyXML2, libseccomp and others — are documented in `capi/NOTICE` and
`capi/LICENSES/` and are not enumerated again here.

### 3. Datasets and model weights (not redistributed, not owned)

The OpenLane test clips referenced by `BUILD.md` are downloaded by the user from
their original source and are **not** part of this repository. They remain the
property of their publishers under their own terms; consult those before any use
beyond local evaluation.

The ONNX model weights under `vision_pilot/VisionPilot/modules/models/weights/`
belong to the VisionPilot project and are governed by its license, not by the
MIT license above.

### 4. Reference material

The algorithmic approaches in VisionPilot's planning and control stages follow
well-known formulations also taught in Udacity's open self-driving-car course
repositories (https://github.com/udacity). Those materials remain the property
of Udacity and its contributors under their own licenses. No substantial
portion of their code is reproduced here; they are cited as educational
reference only.

---

## Summary

| you want to | you may |
|---|---|
| read, study, reproduce this decomposition | yes, freely |
| reuse our model, applications, CMake, scripts | yes, under MIT |
| build and run the whole stack for evaluation | yes, subject to CAPI's "informational purposes" limit |
| exploit any of it commercially | not without an AUTOSAR license for the CAPI material |

When in doubt about the last row, the question is about AUTOSAR's intellectual
property, not about this repository's MIT license — and it is one to put to a
lawyer rather than to this file.
