# CI / CD 工作流说明

[English](ci.md) · [简体中文](ci.zh-CN.md)

本目录包含 GitHub Actions 自动化工作流配置，负责持续集成、多架构编译验证以及制品打包发布。

## 工作流清单

| 工作流 | 配置文件 | 说明 |
| --- | --- | --- |
| **Smoke Test** | [`smoke-test.yml`](smoke-test.yml) | 快速冒烟测试：在每次 PR 与 Push 时自动验证编译与离屏渲染。 |
| **Artifact & Release** | [`artifact-release.yml`](artifact-release.yml) | 制品打包与发布：多架构矩阵（`x86_64`、`aarch64`）、打包（`.tar.gz`、`.deb`）与 Release 创建。 |

## 触发规则

| 事件 / 触发标识 | Smoke Test | Artifact & Release | 执行动作 |
| --- | :---: | :---: | --- |
| 常规 Push 到 `main` / `master` | 触发执行 | 自动跳过 | 极速质量校验（~30-40秒），无任何打包开销。 |
| 针对 `main` / `master` 的 Pull Request | 触发执行 | 自动跳过 | 自动化门禁：校验子模块拉取、代码编译与冒烟测试退出码。 |
| 提交信息包含 `[build-artifact]` | 触发执行 | 构建制品包 | 在 `x86_64` 与 `aarch64` 上编译并上传 Actions 产物供下载。 |
| 提交信息包含 `[build-release]` 或 `v*` Tag | 触发执行 | 发布正式版 | 全架构打包、计算 `SHA256SUMS.txt` 校验和并自动创建 GitHub Release。 |
| 手动调度（`workflow_dispatch`） | 可选执行 | 按参数执行 | 支持网页一键手动触发，通过 `publish` 开关决定是否发版。 |

## 构建矩阵与架构支持

| 架构 | Runner 镜像 | 输出目标 | 适配说明 |
| --- | --- | --- | --- |
| `x86_64` | `ubuntu-24.04` | `.tar.gz`、`.deb` (amd64) | 主流 64 位 x86 PC 与直播推流主机基线。 |
| `aarch64` | `ubuntu-24.04-arm` | `.tar.gz`、`.deb` (arm64) | 运行于 GitHub 官方原生 ARM64 机器，免 QEMU 模拟极速完成。 |

## 发布产物形态

| 产物格式 | 命名规则 | 包含内容 |
| --- | --- | --- |
| **通用便携包** | `pw-mpris-visualcard-<ver>-linux-<arch>.tar.gz` | 便携二进制、systemd 服务模板、`install.sh` 与 `uninstall.sh`。 |
| **Debian / Ubuntu 安装包** | `pw-mpris-visualcard_<ver>_<deb-arch>.deb` | 二进制落入 `/usr/bin`，systemd 服务落入 `/usr/lib/systemd/user`。 |
| **校验和列表** | `SHA256SUMS.txt` | 包含当前批次所有发布制品的 SHA256 签名。 |

## 便携编译开关 (PORTABLE=1)

上游 `Makefile` 默认启用 `-march=native`，以在本机 CPU 上最大化唱片自转性能。但在 CI 云端构建通用二进制时，必须兼容各型号 CPU（避免因缺失目标机器指令集而抛出 `Illegal instruction` 崩溃）；因此所有 CI 发布构建统一显式传入 `PORTABLE=1`。

## 本地模拟验证

在提交前可通过以下命令在本地模拟 CI 的冒烟测试验证流程：

```bash
make clean && make PORTABLE=1 -j$(nproc)
./pw-mpris-visualcard-native --demo --lyrics 4 --time 1 --album 1 --dump /tmp/smoke.png
```
