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

```sh
# 1. Commit and push all changes
git add .
git commit -m "Prepare release v1.2.3"
git push origin main

# 2. Create an annotated tag (replace 1.2.3 with the actual version)
git tag -a v1.2.3 -m "Release 1.2.3"

# 3. Push the tag — this triggers the release workflow
git push origin v1.2.3
```

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
git tag -d v1.2.3
git push origin --delete v1.2.3

# Fix whatever needs fixing, commit, push, then re-tag
git tag -a v1.2.3 -m "Release 1.2.3"
git push origin v1.2.3
```
