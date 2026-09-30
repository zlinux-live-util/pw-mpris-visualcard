# CI / CD Workflows

[English](ci.md) · [简体中文](ci.zh-CN.md)

This directory defines the automated GitHub Actions workflows for continuous integration, multi-architecture verification, and release packaging.

## Workflow Catalog

| Workflow | File | Purpose |
| --- | --- | --- |
| **Smoke Test** | [`smoke-test.yml`](smoke-test.yml) | Fast compile verification and headless PNG rendering test on every PR and push. |
| **Artifact & Release** | [`artifact-release.yml`](artifact-release.yml) | Multi-architecture build matrix (`x86_64`, `aarch64`), packaging (`.tar.gz`, `.deb`), and GitHub Release creation. |

## Trigger Rules

| Event / Keyword | Smoke Test | Artifact & Release | Action |
| --- | :---: | :---: | --- |
| Push to `main` / `master` (routine) | Active | Skipped | Fast validation without packaging overhead (~30-40s). |
| Pull Request to `main` / `master` | Active | Skipped | Quality gate verifying build integrity and smoke test exit code. |
| Commit with `[build-artifact]` | Active | Builds Artifacts | Compiles on `x86_64` and `aarch64`, uploads packages to GitHub Actions. |
| Commit with `[build-release]` or tag `v*` | Active | Publishes Release | Packages binaries, generates `SHA256SUMS.txt`, and publishes GitHub Release. |
| `workflow_dispatch` (Manual) | Optional | Configurable | Runs on-demand; optional `publish` boolean controls release creation. |

## Build Matrix & Architectures

| Architecture | Runner Image | Packaging Target | Notes |
| --- | --- | --- | --- |
| `x86_64` | `ubuntu-24.04` | `.tar.gz`, `.deb` (amd64) | Standard 64-bit PC / streaming rig baseline. |
| `aarch64` | `ubuntu-24.04-arm` | `.tar.gz`, `.deb` (arm64) | Native 64-bit ARM runner without QEMU emulation. |

## Release Artifacts

| Format | File Pattern | Included Contents |
| --- | --- | --- |
| **Portable Tarball** | `pw-mpris-visualcard-<ver>-linux-<arch>.tar.gz` | Portable binary, systemd service template, `install.sh`, `uninstall.sh`. |
| **Debian / Ubuntu Package** | `pw-mpris-visualcard_<ver>_<deb-arch>.deb` | Binary installed to `/usr/bin`, systemd unit installed to `/usr/lib/systemd/user`. |
| **Checksums** | `SHA256SUMS.txt` | SHA256 checksums for all release assets. |

## Portable Flag (PORTABLE=1)

By default, the upstream `Makefile` compiles with `-march=native` to maximize album rotation performance on the host CPU. Release binaries built in CI must be portable across diverse client CPUs (avoiding `Illegal instruction` crashes due to missing AVX/AVX2 extensions); therefore, all CI jobs pass `PORTABLE=1` during compilation.

## Local Verification

Run the exact commands used by CI to verify changes before pushing:

```bash
make clean && make PORTABLE=1 -j$(nproc)
./pw-mpris-visualcard-native --demo --lyrics 4 --time 1 --album 1 --dump /tmp/smoke.png
```
