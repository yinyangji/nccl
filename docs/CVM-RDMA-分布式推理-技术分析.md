# NCCL RDMA 中 TLS 的作用

> 在 NCCL 的 RDMA/IB 路径上，`tls` 指的是 **Thread Local Storage（线程局部存储）**，不是 Transport Layer Security 加密，也不是 UCX 的 `UCX_TLS` 环境变量。

---

## 代码事实

开源 NCCL 的 RDMA/IB 代码中，真正用到 `thread_local` 的仅有 **两处**：

| 文件 | 变量 | 作用 |
|---|---|---|
| `src/transport/net_ib/reg.cc:12` | `static thread_local uintptr_t pageSize = 0;` | 缓存 `sysconf(_SC_PAGESIZE)`，避免 MR 注册热路径重复获取页大小 |
| `src/transport/net_ib/gdr.cc:45` | `static thread_local int ibDmaSupportInitDev;` | 在 `std::call_once` 探测 DMA-BUF/GDR 支持时，传递“当前要初始化哪个设备” |

---

## 作用一：`pageSize` 缓存

```cpp
static thread_local uintptr_t pageSize = 0;
if (pageSize == 0) pageSize = sysconf(_SC_PAGESIZE);
```

- MR 注册需要把地址按页对齐。
- `sysconf` 虽可走 vdso，但 NCCL 热路径上连这点开销也要省掉。
- `thread_local` 避免多线程首次初始化竞争，同时让热路径无额外全局锁。

---

## 作用二：`ibDmaSupportInitDev` 参数通道

```cpp
static thread_local int ibDmaSupportInitDev;
ncclResult_t ncclIbDmaBufSupport(int dev) {
  ibDmaSupportInitDev = dev;
  std::call_once(onces[dev], ibDmaBufSupportInitOnce);
}
```

- `std::call_once` 的回调不接受参数。
- 若用普通全局变量，多线程并发探测不同 `dev` 会互相覆盖目标设备。
- `thread_local` 让每个线程写自己的副本，回调只读本线程值，天然线程安全。

---

## 多视角总结

| 视角 | 核心观点 |
|---|---|
| **NCCL** | TLS 用于线程级上下文隔离与轻量缓存，避免锁和竞争 |
| **RDMA/IB** | 优化 MR 注册热路径，安全并发探测 GDR/DMA-BUF 能力 |
| **Linux 内核** | 与 `errno` 的线程隔离思想一致，减少系统调用和原子操作 |
| **虚拟化/容器** | 降低多线程/多租户场景下设备状态串扰导致的误判风险 |
| **大模型架构** | 支持多线程 proxy 模型，保持高扩展性；同时避免把核心资源放 TLS 带来的迁移问题 |

---

## 结论

NCCL RDMA 中 TLS 的设计哲学是 **“最小化、线程私有、避免锁”**：

1. `pageSize` 缓存页大小，消除 MR 注册热路径的重复系统调用。
2. `ibDmaSupportInitDev` 作为 `std::call_once` 的隐式参数，支持多线程安全并发探测不同 IB 设备的 DMA-BUF 支持。

它只承载“线程级缓存/参数传递”这类轻量状态，核心资源（QP、CQ、MR）仍通过显式结构传递，兼顾性能、正确性与可扩展性。


---

## 八、T1/T2 深度分析：TCP 明文 vs TCP+TLS

### 8.1 实验定义

| 组别 | 传输方式 | 加密 | 适用场景 |
|---|---|---|---|
| **T1** | TCP/IPoIB | 明文 | 内部信任网络 |
| **T2** | TCP/IPoIB + TLS | AES-128-GCM | 跨 CVM/不可信网络 |

两者都基于 NCCL 的 `net_socket` transport，差别仅在 TLS 记录层加密。

### 8.2 核心性能数据

`1024 / 512 / 并发 4`：

| 指标 | T1 明文 TCP | T2 TCP+TLS | 变化 |
|---|---:|---:|---|
| 吞吐 tokens/s | 80.68 | 76.25 | -5.5% |
| TTFT ms | 508.0 | 485.5 | -4.4% |
| TPOT ms | 48.63 | 51.56 | +6.0% |
| E2E s | 25.36 | 26.83 | +5.8% |

### 8.3 为什么 T2 比 T1 略差

NCCL 的 TLS 实现（`src/misc/crypt.cc`）在 Socket transport 上工作：

```
应用数据
  ↓ SSL_write_ex
TLS 记录层加密（AES-128-GCM）
  ↓ 写入 socket
TCP/IPoIB
  ↓ 对端 socket
TLS 记录层解密
  ↓ SSL_read_ex
应用数据
```

额外开销：

1. **加解密计算**：每个 send/recv 都要做 AES-GCM。
2. **TLS 记录头**：每包增加少量元数据。
3. **内存拷贝**：TLS 库内部需要 in-place 或临时 buffer。
4. **握手延迟**：首次连接有 TLS 1.3 握手，但 NCCL bootstrap 连接建立后复用。

但总体影响很小（~5%），因为：

- 推理 workload 的跨机消息小（~64 KB），TLS 计算量不大。
- 主要瓶颈在 GPU→Host 拷贝和 decoding 阶段的同步，不在网络。

### 8.4 T1/T2 比 T3 多了几次内存拷贝

从网络传输角度：

| 路径 | 额外拷贝 |
|---|---|
| T1/T2 TCP/IPoIB | GPU→Host + Host→Kernel Socket Buffer（≈2 次显式/隐式拷贝） |
| T3 RDMA + 共享 DMA-BUF | GPU→Host + Host→共享页（≈2 次拷贝） |

**核心差异不在拷贝次数，而在拷贝位置和开销：**

- T1/T2 的第二次拷贝发生在 **内核 socket buffer**，要经过 TCP/IP 协议栈。
- T3 的第二次拷贝发生在 **TDX 共享页**，绕过 TCP 协议栈，直接 RDMA。

如果 T3 走 SWIOTLB 路径，则：

```
GPU → Host 内存 → SWIOTLB bounce buffer → IB NIC
```

这也是 2 次拷贝，但 SWIOTLB bounce 在内核中隐式完成，且同步开销更大。

### 8.5 结论

- T2（TCP+TLS）相对 T1 损失约 5%，加密开销可接受。
- T1/T2 和 T3 的拷贝次数相近，但 RDMA 在小消息/no-overlap/no-GDR 场景下没有体现出优势。
- 当需要跨 CVM 加密时，**TCP+TLS 是一个简单有效的方案**。

---

## 九、Socket+TLS 跨 CVM 加密分布式推理

### 9.1 能不能用

**能。** NCCL 的 Socket transport 本质上就是 TCP over IPoIB（或普通以太网）。只要 CVM 之间 IP 可达，Socket+TLS 就能工作。

启用方式（当前已支持的 PSK 模式）：

```c
ncclEncryptionConfig_t cfg = NCCL_ENCRYPTION_CONFIG_INITIALIZER;
cfg.mode = NCCL_ENCRYPTION_MODE_PSK;
cfg.psk = "至少 32 字节的预共享密钥";
ncclSetEncryption(&cfg);
```

### 9.2 TLS 在哪里加密

NCCL 的 TLS 在 **应用层完成加密**：

```
NCCL 用户态 buffer
  ↓
SSL_write_ex() / SSL_read_ex()  // OpenSSL 3
  ↓
TLS 1.3 记录层（AES-128-GCM）
  ↓
Socket send/recv
  ↓
TCP/IPoIB 或 TCP/以太网
```

数据在离开 NCCL 进程之前就已经是密文，经过的所有中间网络设备（交换机、IPoIB 适配器）都只看到密文。

### 9.3 跨 CVM 走的是什么网络

如果 IB 网卡直通到 CVM，NCCL Socket transport 默认会尝试使用 IB 设备的 IPoIB 接口（`NCCL_SOCKET_IFNAME` 可指定）。此时：

```
CVM A 的 NCCL 进程
  ↓
Socket (TCP)
  ↓
IPoIB 接口
  ↓
IB 网卡（直通）
  ↓
物理 IB 网络
  ↓
IB 网卡（直通）
  ↓
IPoIB 接口
  ↓
CVM B 的 NCCL 进程
```

IB 网卡在硬件层是直通的，但 TCP/IPoIB 仍然是 TCP 协议栈，不是 RDMA verbs。

### 9.4 超过 2 台 CVM 是否有效

**有效。** Socket+TLS 是标准的 TCP 连接，天然支持任意多机的星型/全连接拓扑。

NCCL bootstrap 会为每一对需要通信的 rank 建立一条 TLS 连接：

```
Node 0 ↔ Node 1 (TLS)
Node 0 ↔ Node 2 (TLS)
Node 1 ↔ Node 2 (TLS)
...
```

每条连接独立协商 TLS 1.3 会话，使用同一 PSK。

### 9.5 局限

| 局限 | 说明 |
|---|---|
| 性能 | TCP/IPoIB 带宽低于原生 RDMA，延迟更高 |
| PSK 管理 | 所有节点需要相同预共享密钥，分发和轮换是运维负担 |
| 无身份认证 | PSK 只证明"持有相同密钥"，不证明对方是合法 CVM |
| 不支持 RDMA verbs | TLS 不能直接在 IB RDMA 上运行 |

---

## 十、PSK 预共享密钥 vs RA-TLS

### 10.1 PSK 的问题

NCCL 当前 TLS 使用 PSK（Pre-Shared Key）：

```c
// OpenSSL 3 PSK callback
SSL_CTX_set_psk_use_session_callback(client_ctx, cryptPskUseSession);
SSL_CTX_set_psk_find_session_callback(server_ctx, cryptPskFindSession);
```

问题：

1. **密钥分发风险**：PSK 需要提前部署到所有节点，泄露后无补救手段。
2. **无远程证明**：PSK 只能证明对方知道密钥，不能证明对方运行在期望的 TDX 实例里。
3. **不能区分实例**：一旦 PSK 泄露，攻击者可以用任意实例加入集群。

### 10.2 RA-TLS 是什么

**RA-TLS（Remote Attestation TLS）** 把 TDX 远程证明和 TLS 结合起来：

1. 每个节点生成临时 RSA 密钥对。
2. 节点用自己的私钥签发一张自签 X.509 证书，证书扩展里嵌入 **TDREPORT（TDX 证明报告）**。
3. TLS 握手时双方交换证书。
4. 验证对端证书时，提取 TDREPORT，检查 MRTD 等测量值是否在 allowlist 中。

这样：

- **无需预共享密钥**：身份由硬件证明保证。
- **可验证运行环境**：确保对端是符合预期的 TDX 镜像。
- **临时证书**：每次进程启动生成新密钥，前向安全。

### 10.3 能不能在 CVM 里用 RA-TLS

**能，而且已经实现。** 在 `feature/nccl-ratls` 分支上，我们为 NCCL Socket transport 增加了基于 TDX quote 的 RA-TLS：

```c
ncclEncryptionConfig_t cfg = NCCL_ENCRYPTION_CONFIG_INITIALIZER;
cfg.mode = NCCL_ENCRYPTION_MODE_RATLS;
cfg.ratlsMrtdAllowlist = expected_mrtd_48_bytes;
cfg.ratlsMrtdAllowlistLen = 48;
ncclSetEncryption(&cfg);
```

### 10.4 RA-TLS 实现要点

| 模块 | 文件 | 作用 |
|---|---|---|
| TDX 证明 | `src/misc/ratls_tdx.cc` | 从 `/dev/tdx_guest` 读取 TDREPORT |
| 证书生成/校验 | `src/misc/ratls_cert.cc` | 生成带 quote 扩展的自签证书，校验对端 quote |
| OpenSSL 符号 | `src/include/ratls_openssl.h` | 复用 NCCL 动态加载的 OpenSSL 3 符号 |
| TLS 集成 | `src/misc/crypt.cc` | RA-TLS 模式初始化、verify callback、模式切换 |
| 公共 API | `src/nccl.h.in` | 新增 `NCCL_ENCRYPTION_MODE_RATLS` 和 allowlist 字段 |
| 构建 | `src/misc/CMakeLists.txt` | 编译新源文件 |

### 10.5 RA-TLS 数据流

#### Mermaid 版

```mermaid
sequenceDiagram
    participant A as CVM A NCCL
    participant TA as /dev/tdx_guest
    participant B as CVM B NCCL
    participant TB as /dev/tdx_guest

    A->>TA: ioctl GET_REPORT0
    TA-->>A: TDREPORT + MRTD
    A->>A: 生成 RSA 自签证书，嵌入 TDREPORT

    B->>TB: ioctl GET_REPORT0
    TB-->>B: TDREPORT + MRTD
    B->>B: 生成 RSA 自签证书，嵌入 TDREPORT

    A->>B: TLS ClientHello + 证书
    B->>B: 提取证书中的 quote，比对 MRTD allowlist
    B->>A: TLS ServerHello + 证书
    A->>A: 提取证书中的 quote，比对 MRTD allowlist
    A<->B: 加密 NCCL Socket 通信
```

#### HTML5 嵌入版

<div style="background:#f8fafc;border:1px solid #e2e8f0;border-radius:12px;padding:24px;font-family:system-ui,sans-serif;">
  <div style="display:flex;flex-direction:column;gap:16px;">
    <div style="display:flex;align-items:center;gap:12px;">
      <div style="background:#dbeafe;border:2px solid #3b82f6;border-radius:8px;padding:10px 16px;font-weight:700;color:#1e40af;">CVM A NCCL</div>
      <span style="color:#64748b;">→</span>
      <div style="background:#f1f5f9;border:1px solid #cbd5e1;border-radius:6px;padding:8px 12px;font-size:13px;">/dev/tdx_guest</div>
      <span style="color:#64748b;">→</span>
      <div style="background:#fef3c7;border:1px solid #fbbf24;border-radius:6px;padding:8px 12px;font-size:13px;">TDREPORT + MRTD</div>
      <span style="color:#64748b;">→</span>
      <div style="background:#dcfce7;border:1px solid #4ade80;border-radius:6px;padding:8px 12px;font-size:13px;">RSA 自签证书（含 quote）</div>
    </div>
    <div style="display:flex;align-items:center;gap:12px;">
      <div style="background:#fce7f3;border:2px solid #db2777;border-radius:8px;padding:10px 16px;font-weight:700;color:#831843;">CVM B NCCL</div>
      <span style="color:#64748b;">→</span>
      <div style="background:#f1f5f9;border:1px solid #cbd5e1;border-radius:6px;padding:8px 12px;font-size:13px;">/dev/tdx_guest</div>
      <span style="color:#64748b;">→</span>
      <div style="background:#fef3c7;border:1px solid #fbbf24;border-radius:6px;padding:8px 12px;font-size:13px;">TDREPORT + MRTD</div>
      <span style="color:#64748b;">→</span>
      <div style="background:#dcfce7;border:1px solid #4ade80;border-radius:6px;padding:8px 12px;font-size:13px;">RSA 自签证书（含 quote）</div>
    </div>
    <div style="text-align:center;color:#0f766e;font-weight:700;padding:12px;background:#e0f2fe;border-radius:8px;">
      双向 TLS 握手 + 证书中 quote 的 MRTD 校验
    </div>
    <div style="text-align:center;color:#64748b;font-size:13px;">
      校验通过后，NCCL Socket 通信由 TLS 1.3（AES-128-GCM）加密
    </div>
  </div>
</div>

### 10.6 RA-TLS 与 IRMA/AEGIS 的关系

| 方案 | 加密位置 | 适用 transport | 特点 |
|---|---|---|---|
| **T2 TCP+TLS** | 应用层 Socket | TCP/IPoIB | 简单，有 PSK 管理问题 |
| **RA-TLS** | 应用层 Socket | TCP/IPoIB | 用 TDX quote 替代 PSK，可远程证明 |
| **IRMA + AEGIS** | CVM host memory | RDMA verbs | 高性能，需改 NCCL/内核共享页方案 |

RA-TLS 是 **Socket transport 上的身份认证增强**，不直接解决 RDMA 加密问题；IRMA/AEGIS 是 **RDMA transport 上的高性能加密**。两者可以共存：

- 控制面/小流量用 RA-TLS（Socket）。
- 数据面大流量用 IRMA/AEGIS（RDMA）。

---

## 十一、原生 NCCL RDMA 在 CVM 中能不能走通

### 11.1 问题重述

> 不做任何修改，原生 NCCL 的 RDMA（`net_ib` transport）在 TDX CVM 里能不能跑起来？

**能跑，但走的是 SWIOTLB bounce buffer 路径，性能受限。**

### 11.2 为什么能跑

NCCL 调用 `ibv_reg_mr()` 注册内存时，并不关心底层是不是 CVM。内核会：

1. `pin_user_pages_fast()` 把用户页 pin 住。
2. `dma_map_sgtable()` 做 DMA 映射。
3. 在 TDX CVM 中，`is_swiotlb_force_bounce(dev)` 为 true，强制分配 bounce buffer。
4. 数据实际通过 SWIOTLB buffer 完成 DMA。

所以 NCCL 不需要改代码就能在 CVM 里把 RDMA 跑通。

### 11.3 为什么不能跑出高性能

因为 CVM 的内存默认是加密的（private），IB 网卡 DMA 不能直接读。内核只能：

```
private page（密文/加密元数据）
  ↓ CPU 拷贝 + 解密/转换
SWIOTLB bounce buffer（明文/可被网卡访问）
  ↓ DMA
IB NIC
```

这个额外的 CPU 拷贝和同步限制了带宽和延迟。

### 11.4 高性能需要什么

需要 NCCL + 内核配合：

1. NCCL 分配 **共享 DMA-BUF 页**（`ncclTdxAlloc()`）。
2. 把 GPU 数据拷贝到共享页。
3. 注册共享页时带上 `DMA_ATTR_CC_SHARED`。
4. 内核绕过 SWIOTLB，直接返回未加密物理地址给网卡。

这正是 T3 修改方案所做的：

```
ncclTdxAlloc() → 共享 DMA-BUF
  ↓
NCCL 把数据写到这里
  ↓
ibv_reg_mr() with DMA_ATTR_CC_SHARED
  ↓
dma_direct_map_phys(): CC_SHARED 分支
  ↓
IB 网卡直接 DMA
```

### 11.5 谁决定能不能支持

- **NCCL**：决定调用哪个 transport、分配什么内存、传什么 dma_attrs。
- **Linux 内核**：决定是否支持 `DMA_ATTR_CC_SHARED`、共享页机制、IB 驱动的 dma_map 实现。
- **TDX 模块/ hypervisor**：决定是否允许共享页、quote 生成等。

所以：

> **原生 NCCL RDMA 能在 CVM 里跑通，但跑出高性能需要 NCCL 改代码 + 内核支持共享 DMA-BUF。**

---

## 十二、总结与建议

### 12.1 各方案定位

| 方案 | 安全等级 | 性能 | 复杂度 | 推荐场景 |
|---|---|---|---|---|
| T1 TCP 明文 | 低 | 中 | 低 | 内部可信网络 |
| T2 TCP+TLS (PSK) | 中 | 中- | 低 | 跨 CVM，简单部署 |
| **RA-TLS (Socket)** | **高** | 中- | 中 | 跨 CVM，需要远程证明 |
| T3 RDMA + 共享 DMA-BUF | 中（明文） | 高 | 高 | CVM 内高性能 RDMA |
| IRMA + AEGIS (RDMA) | 高 | 高 | 高 | CVM 内加密高性能 RDMA |

### 12.2 最终推荐

1. **需要跨 CVM 加密，但 workload 以 decoding 小消息为主**：
   - 先用 **T2 TCP+TLS** 验证可行性。
   - 再用 **RA-TLS** 替代 PSK，解决密钥分发和远程证明问题。

2. **需要高性能加密 RDMA**：
   - 走 **IRMA + AEGIS-128L** 方案（共享页内加密 + RDMA）。
   - 或基于 T3 的 `ncclTdxAlloc` 共享 DMA-BUF，自行在 net plugin 中加解密。

3. **模型大到必须多机 RDMA**：
   - 优先解决 **bypass SWIOTLB** 的共享内存页方案。
   - 在共享页内完成加密，再交给 RDMA。

4. **当前 Qwen72B-B200 7 链路测试结论**：
   - 2 机 PP2、batch=4 的 decoding 场景下，**TCP/TLS 已经足够好**。
   - RDMA 优势在长 prompt / 大 batch / 多机场景才会体现。
   - TDX CVM 的 SWIOTLB/共享页机制是 RDMA 性能的关键瓶颈。
