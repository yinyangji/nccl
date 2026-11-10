:# NCCL RA-TLS 未完成事项与续工指南

> 本文件记录当前工作进度和剩余任务，换环境后可直接按此继续。

## 当前状态

- 分支：`feature/nccl-ratls`
- 最新 commit：`a436f8bd`
- 已完成：TDX 证明、证书生成/校验、公共 API、TLS 集成、构建配置、技术分析文档
- **未完成：完整编译验证和运行时测试**

## 已提交变更

```
a436f8bd docs: add CVM RDMA distributed inference technical analysis
0491afb2 misc: add RA-TLS support for TDX CVM Socket transport
```

## 剩余工作清单

### 1. 编译验证（高优先级）

在 b200-chenpengyu 或任何有 CUDA + OpenSSL 3 的环境执行：

```bash
cd /path/to/nccl
make -j $(nproc) TLS_BACKEND=OPENSSL3
```

预期问题：
- OpenSSL 符号表中的某些函数可能在不同 OpenSSL 3 子版本间签名不同。
- `MBSTRING_ASC`、`NID_*` 等宏的可用性。
- `EVP_sha256` 动态加载是否成功。

修复思路：
- 若编译报错某 OpenSSL 符号未找到，在 `src/include/ratls_openssl.h` 的对应 SYMBOL 列表中增删，并在 `src/misc/crypt.cc` 的 `cryptLoadOpenSslSymbols()` 中同步。
- 若 `X509_*`/`ASN1_*` 宏缺失，确认 `<openssl/asn1.h>` / `<openssl/x509.h>` 已包含在 `ratls_openssl.h` 中。

### 2. 运行时功能测试（高优先级）

#### 2.1 非 TDX 环境降级测试

在普通 Linux 机器（非 TDX）上：

```bash
export NCCL_DEBUG=INFO
# 运行任意 nccl-tests，例如 all_reduce
./build/all_reduce -b 8 -e 128M -f 2 -g 2
```

配置 RA-TLS 时，预期连接建立失败（无法打开 `/dev/tdx_guest`），应看到清晰错误日志。

#### 2.2 TDX CVM 双向测试

在两台 TDX CVM 上：

```c
// 程序中设置
ncclEncryptionConfig_t cfg = NCCL_ENCRYPTION_CONFIG_INITIALIZER;
cfg.mode = NCCL_ENCRYPTION_MODE_RATLS;
// cfg.ratlsMrtdAllowlist = nullptr;      // 接受任意 TDX 实例
cfg.ratlsMrtdAllowlist = expected_48byte_mrtd;
cfg.ratlsMrtdAllowlistLen = 48;
ncclSetEncryption(&cfg);
```

测试场景：
- allowlist 为空：连接应成功。
- allowlist 设置正确 MRTD：连接应成功。
- allowlist 设置错误 MRTD：连接应在 TLS handshake 阶段失败。

### 3. 代码审查点（中优先级）

- [ ] `src/misc/ratls_cert.cc` 中 `OBJ_txt2obj` 返回的 `ASN1_OBJECT*` 在所有路径下正确释放。
- [ ] `ncclRAtlsGenerateCert()` 失败时，`cryptSetRatls()` 调用 `cryptClearRatls()` 不会 double-free。
- [ ] `cryptSetMasterKey()` 与 `cryptSetRatls()` 切换时，SSL_CTX 的 verify callback 和 PSK callback 状态正确重置。
- [ ] `ratlsVerifyCallback()` 对 `X509_V_ERR_DEPTH_ZERO_SELF_SIGNED_CERT` 等自签错误的处理是否足够严格。
- [ ] 证书有效期目前为 1 天，考虑是否应更短或更长。
- [ ] 自定义 OID `1.3.6.1.4.1.8072.666.1` 是否需替换为 NVIDIA 正式 OID。

### 4. 扩展方向（低优先级）

- [ ] SEV-SNP 支持：增加 `ratls_snp.cc` / `ratls_snp.h`。
- [ ] 完整 quote 验签：集成 Intel QVL，而非仅比对 MRTD。
- [ ] 允许配置多个 MRTD（allowlist 数组）。
- [ ] 证书中嵌入 TCB 版本、策略等更多 TD 属性。
- [ ] 性能基准测试：对比 PSK TLS 与 RA-TLS。

## 换环境后快速上手

```bash
# 1. 拉取分支
git clone https://github.com/yinyangji/nccl.git
cd nccl
git checkout feature/nccl-ratls

# 2. 阅读当前文档
# GOAL.md      — 项目目标
# UNFINISHED.md — 本文件，剩余任务
# docs/CVM-RDMA-分布式推理-技术分析.md — 技术分析

# 3. 编译
make -j $(nproc) TLS_BACKEND=OPENSSL3

# 4. 测试
# 见上方 2.1 / 2.2
```

## 已知风险

- 当前 MRTD 比对是最小可用版本，未验证 quote 的密码学签名，生产环境需接入 Intel QVL。
- 自签证书无 CA 链，依赖 verify callback 中的 quote 校验；如果 quote 校验被绕过，证书本身不可信。
- RA-TLS 只保护 Socket transport；如果 NCCL 回退到 IB/RDMA，该路径仍为明文（除非同时启用 IRMA/AEGIS）。
