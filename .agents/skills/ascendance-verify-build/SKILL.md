---
name: ascendance-verify-build
description: >-
  Use this skill to run local and CI build/test verification across ESP32-S3
  firmware, the React WebHID single-file bundle, and the Android Companion app,
  or to monitor GitHub Actions and create release tags cleanly.
---

# ASCENDANCE Build & Verification Workflow

Execute only the verification steps matching the subsystems touched. Ensure Android environment rules (`PATH`, `HOME`, `GIT_CONFIG_NOSYSTEM=1`) and `noexec` storage constraints are respected when running locally.

## 1. WebHID React App (`tools/webhid_tuner/`)

- **On desktop Linux/macOS/CI:**
  ```bash
  cd tools/webhid_tuner
  npm ci
  npm test
  npm run build
  ```
- **On Android host where `/storage/emulated/0` is mounted `noexec` (`EACCES` / `ERR_DLOPEN_FAILED` on native Rollup/esbuild binaries):**
  Copy `index.html`, `package.json`, `package-lock.json`, `vite.config.js`, `public/`, `scripts/`, `src/`, `tests/` to an executable directory under `$HOME/.cache/webhid_verify` and run `npm ci && npm test && npm run build` there, then clean up the cache directory afterward.
- **Validate Built Output:**
  Confirm `dist/index.html` contains one valid `<script>` block using `node --check`, and verify `tools/webhid_tuner/dist/` and `node_modules/` remain untracked via `git status -s`.

## 2. Android Companion (`tools/android_companion/`)

- **On CI / systems with JDK 17 + Gradle 8.11.1:**
  ```bash
  cd tools/android_companion
  gradle testDebugUnitTest lintDebug assembleDebug
  ```
- **Without local JDK/Gradle:** Verify balanced Kotlin/XML delimiters locally and rely on GitHub Actions workflow `.github/workflows/android.yml` after pushing.

## 3. ESP32-S3 Firmware (`main/`)

- **With ESP-IDF v6.1 installed:**
  ```bash
  idf.py set-target esp32s3
  idf.py build
  ```
- **Audit C Changes Locally First:** Check no packed-struct member address-taking (`-Waddress-of-packed-member`), no writes to forbidden GPIOs `33..37`, and OLED UI lines remain `<= 20` chars. Trigger `.github/workflows/build.yml` on push to compile under `espressif/idf:v6.1`.

## 4. Monitoring CI & Tagging Releases

- Check CI runs via GitHub REST API if `gh` is unavailable:
  ```bash
  curl -s "https://api.github.com/repos/ASCENDINGHeat/ascendance-esp32-dac/actions/runs?per_page=5"
  ```
- **Only** create/push a `vX.Y.Z` annotated release tag when the user explicitly requests a release tag (triggering `.github/workflows/release.yml` to compile firmware, signed APK, and `dist/index.html`).
