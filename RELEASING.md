# Releasing Quartz RTOS

This document describes how to cut a new release.  Releases are driven by a
git tag; the CI/CD pipeline handles everything else automatically.

---

## Versioning policy (SemVer)

| Kind of change | Version bump | Example |
|---|---|---|
| Bug fix, refactor, new port, doc update | **Patch** | `1.0.0 → 1.0.1` |
| New API or feature (backward-compatible) | **Minor** | `1.0.0 → 1.1.0` |
| Removed or renamed API, ABI break | **Major** | `1.0.0 → 2.0.0` |

---

## Pre-release checklist

```sh
# 1. Make sure the host tests pass
cmake -B build && cmake --build build --parallel
ctest --test-dir build --output-on-failure

# 2. Confirm there are no uncommitted changes
git status
```

Everything must be committed and pushed to `main` before you create the tag.
The CI workflow does a fresh checkout of the repo at the tagged commit, so any
local-only changes will not be included in the release.

---

## Step-by-step release

There are two independent release types, each triggered by a different tag
prefix.

### Full project release — tag prefix `v`

Creates a GitHub Release titled **"Quartz RTOS vX.Y.Z"**.

```sh
# 1. Commit and push all changes
git add .
git commit -m "Prepare release v1.2.3"
git push origin main

# 2. Create an annotated tag
git tag -a v1.2.3 -m "Release 1.2.3"

# 3. Push the tag — triggers release.yml
git push origin v1.2.3
```

### Arduino library release — tag prefix `arduino-v`

Creates a GitHub Release titled **"QuartzRTOS Arduino Library vX.Y.Z"** with
`QuartzRTOS.zip` attached.

```sh
# 1. Commit and push all changes
git add .
git commit -m "Prepare Arduino library release v1.2.3"
git push origin main

# 2. Create an annotated tag with the arduino- prefix
git tag -a arduino-v1.2.3 -m "Arduino Library Release 1.2.3"

# 3. Push the tag — triggers release-arduino.yml
git push origin arduino-v1.2.3
```

The versions can track together or diverge — e.g. you can ship an Arduino
library bug fix as `arduino-v1.0.1` without bumping the full project version.

That's it.  You do not need to edit `library.properties`, `rtos_version.h`, or
any other file — the packaging script reads the tag and stamps the version
automatically.

---

## What happens automatically

After the tag push the `.github/workflows/release.yml` workflow:

1. Checks out the repo at the tagged commit (full history for `git describe`).
2. Configures and builds the host port (`cmake -B build && cmake --build build`).
3. Runs all unit tests (`ctest --output-on-failure`).
4. Runs `python3 tools/package_arduino.py --zip QuartzRTOS.zip`, which:
   - Reads the version from the tag via `git describe`.
   - Stamps `include/rtos_version.h` and `extras/arduino/library.properties`.
   - Assembles `extras/arduino/src/` from the main source tree.
   - Packages everything into `QuartzRTOS.zip`.
5. Creates a GitHub Release tagged `v<version>` and attaches `QuartzRTOS.zip`
   as a downloadable asset.

---

## Installing the released library

Users can install `QuartzRTOS.zip` in the Arduino IDE via:

> **Sketch → Include Library → Add .ZIP Library…**

Or they can clone/download the `extras/arduino/` directory directly.

---

## If a release needs to be re-done

Delete the tag locally and remotely, then re-push:

```sh
# Full project release
git tag -d v1.2.3
git push origin --delete v1.2.3
git tag -a v1.2.3 -m "Release 1.2.3"
git push origin v1.2.3

# Arduino library release
git tag -d arduino-v1.2.3
git push origin --delete arduino-v1.2.3
git tag -a arduino-v1.2.3 -m "Arduino Library Release 1.2.3"
git push origin arduino-v1.2.3
```
