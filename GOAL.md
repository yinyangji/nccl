# NCCL RA-TLS 目标文档

## 目标

为 NCCL 的 **Socket transport**（TCP/IPoIB）增加 **RA-TLS（Remote Attestation TLS）** 能力，使 TDX CVM 之间的 NCCL 控制/数据面连接可以通过 **TDX 远程证明** 进行双向身份认证，替代当前基于 PSK 的 TLS 模式。

## 范围

- 仅针对 **Socket transport**；原生 IB/RDMA verbs 路径不在本次范围内。
- 第一 TEE 目标为 **Intel TDX**；SEV-SNP 等后续可扩展。
- 最小可用版本（MVP）：
  - 基于 X.509 自签证书。
  - 证书扩展中嵌入 TDX TDREPORT。
  - OpenSSL verify callback 提取并比对 **MRTD measurement**。
  - 通过 `ncclSetEncryption()` 配置 allowlist。

## 成功标准

1. `feature/nccl-ratls` 分支可以完整编译通过：`make -j TLS_BACKEND=OPENSSL3`。
2. 在两台 TDX CVM 上运行 NCCL 程序，配置 `NCCL_ENCRYPTION_MODE_RATLS` 后，Socket 连接成功建立。
3. MRTD 不匹配时，连接被明确拒绝并打印告警。
4. MRTD 匹配或 allowlist 为 nullptr 时，通信正常。
5. 性能相对 `NCCL_ENCRYPTION_MODE_PSK` 损失在可接受范围（< 10%）。

## 分支

- 分支名：`feature/nccl-ratls`
- 基线：`master` (v2.32.6-1)
- 当前最新 commit：`a436f8bd`

## 关键文件

| 文件 | 说明 |
|---|---|
| `src/misc/ratls_tdx.cc` / `src/include/ratls_tdx.h` | TDX TDREPORT 获取与 MRTD 提取 |
| `src/misc/ratls_cert.cc` / `src/include/ratls_cert.h` | 自签证书生成与 quote 校验 |
| `src/include/ratls_openssl.h` | 共享动态加载的 OpenSSL 符号表 |
| `src/misc/crypt.cc` | RA-TLS 模式集成、verify callback |
| `src/nccl.h.in` | 公共 API 扩展 |
| `src/misc/CMakeLists.txt` | 构建系统 |
| `docs/CVM-RDMA-分布式推理-技术分析.md` | 技术分析文档 |

## 验证环境要求

- Linux x86_64
- CUDA toolkit（与 NCCL 版本匹配）
- OpenSSL 3 开发库（`libssl-dev` / `openssl-devel`）
- TDX CVM 环境（用于运行时测试）
