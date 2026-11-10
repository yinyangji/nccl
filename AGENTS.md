:# NCCL Agent 开发指南

> 本文件面向 AI Agent 与自动化辅助开发工具。人类开发者也可参考，但主要目标是让 Agent 在修改本仓库时保持一致性与正确性。

## 项目概述

- **项目**：NVIDIA NCCL（NVIDIA Collective Communications Library）
- **语言**：C++ / CUDA / Makefile / CMake
- **主要平台**：Linux（x86_64 / ARM64），Windows 为有限支持
- **当前分支示例**：`feature/nccl-ratls`（基于 NCCL v2.32.6-1）

## 通用原则

1. **先读后改**：修改任何子系统前，先通读相关代码与现有注释。
2. **匹配风格**：遵循现有命名、缩进、错误处理模式。
3. **最小变更**：只改必要文件，避免顺带重构无关代码。
4. **动态加载优先**：NCCL 对 CUDA driver/runtime、OpenSSL、IB verbs 等均使用运行时 `dlopen`，新增外部依赖时**不要**直接静态链接。
5. **头文件谨慎**：公共头仅 `nccl.h` 与 `nccl_device.h` 系列；内部头在 `src/include/`。

## 构建与验证

### 标准构建

```bash
# 默认构建（需 CUDA 环境）
make -j $(nproc)

# 启用 TLS（OpenSSL 3）
make -j $(nproc) TLS_BACKEND=OPENSSL3

# CMake 路径
mkdir build && cd build
cmake .. -DTLS_BACKEND=OPENSSL3
make -j $(nproc)
```

### 验证清单

修改后必须执行：

- [ ] `make -j $(nproc) TLS_BACKEND=OPENSSL3` 通过（如改动涉及 TLS/Socket/加密）
- [ ] 静态分析：`make format`（如果修改了 C++ 源文件）
- [ ] 运行 nccl-tests 中相关用例（如 all_reduce、all_gather），至少 `-b 8 -e 128M`
- [ ] 如涉及新 API，更新 `src/nccl.h.in` 与 public header 生成规则

## 代码风格

- 缩进：2 个空格（由 `.clang-format` 控制）
- 命名：
  - 函数：`ncclFooBar`，内部静态函数可加模块前缀如 `cryptFooBar`
  - 宏：`NCCL_FOO_BAR`
  - 类型：`ncclFoo_t`
  - 结构体：`ncclFoo` 或 `ncclFoo_v23200`（版本化配置结构）
- 错误处理：统一使用 `NCCLCHECK()` / `NCCLCHECKGOTO()` / `WARN()` / `INFO()`
- 内存分配：优先使用 `ncclCalloc()` / `ncclCudaMalloc()` 等封装，释放对应使用 `free()` / `ncclCudaFree()`

## 子系统指引

| 子系统 | 关键目录 | 修改注意 |
|---|---|---|
| Socket transport | `src/transport/net_socket/` | 注意与 `src/misc/crypt.cc` 的 TLS 集成 |
| IB/RDMA transport | `src/transport/net_ib/` | 涉及 MR 注册、GDR、DMA-BUF；改前读 `src/transport/net_ib/reg.cc` |
| TLS/加密 | `src/misc/crypt.cc`, `src/misc/ratls_*.cc` | 使用 `ratls_openssl.h` 动态符号表，不直接链接 OpenSSL |
| 公共 API | `src/nccl.h.in` | 版本化结构体末尾追加字段，更新 `NCCL_*_INITIALIZER` |
| 构建 | `src/Makefile`, `src/misc/CMakeLists.txt` | Makefile 用 wildcard，CMake 需显式列出源文件 |
| 设备代码 | `src/device/`, `src/nccl_device/` | CUDA 代码，编译规则特殊，勿随意改 Makefile 中 device 相关目标 |

## 远程证明 / TEE 代码特殊规则

- TDX quote 获取统一走 `src/misc/ratls_tdx.cc`，不直接在内联代码里 `open("/dev/tdx_guest")`。
- 证书中的自定义 OID 目前使用占位符 `1.3.6.1.4.1.8072.666.1`；生产化前需替换为正式 OID。
- MRTD allowlist 长度固定为 48 字节；新增 allowlist 类型时同步校验 `ratlsMrtdAllowlistLen`。
- 不要假设所有节点都能访问 `/dev/tdx_guest`；代码应优雅降级并返回 `ncclSystemError` + 清晰日志。

## 提交规范

```
<subsystem>: <imperative short summary>

<body: what and why, not how>
```

示例：

```
misc: add RA-TLS verify callback for TDX quotes

Implement ratlsVerifyCallback() to extract the embedded TDREPORT
from a peer's self-signed certificate and compare its MRTD against
the configured allowlist.
```

## 与人类协作者沟通

- 长任务使用 `TodoList` 跟踪。
- 编译失败时先汇报具体错误文本再提修复方案。
- 不要臆测 CUDA/IB 驱动版本；必要时检查 `nvcc --version`、`ibv_devinfo`、`openssl version`。
