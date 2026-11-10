:# NCCL 开发规则（Cursor / Agent 通用）

## 1. 项目约束

- **公共 API 变更需要广泛讨论**：`src/nccl.h.in` 和 `src/nccl_device.h` 是 NCCL 对外唯一头文件。新增 API 前必须确认 ABI 兼容性。
- **不要直接链接外部库**：CUDA、OpenSSL、IB verbs 均通过运行时 `dlopen`/`dlsym` 加载。新增符号必须在内部符号表中声明。
- **不要破坏 Windows 构建**：Linux-only 代码用 `#if defined(NCCL_OS_LINUX)` 包裹，并在 CMake/Makefile 中条件编译。

## 2. 目录与文件约定

```
src/
  misc/           # 通用工具：crypt, socket, alloc, os, ratls_*
  transport/      # 网络 transport：net_socket, net_ib
  include/        # 内部头文件
  device/         # CUDA kernel / device 代码
  enqueue/        # 执行引擎
  graph/          # 拓扑图算法
  init.cc         # 初始化入口
  bootstrap.cc    # bootstrap 网络
  proxy.cc        # proxy 线程
```

- 新增 transport 功能：参考 `src/transport/net_socket/` 与 `src/transport/net_ib/` 目录结构。
- 新增 misc 模块：在 `src/misc/` 放 `.cc`，在 `src/include/` 放同名 `.h`。
- Makefile 自动包含 `misc/*.cc`，但 CMake 需在 `src/misc/CMakeLists.txt` 显式添加。

## 3. 错误处理模式

所有函数返回 `ncclResult_t`，标准模式：

```cpp
ncclResult_t ncclFoo(...) {
  ncclResult_t ret = ncclSuccess;
  void* ptr = nullptr;
  NCCLCHECKGOTO(ncclCalloc(&ptr, size), ret, fail);
  // ... use ptr ...
fail:
  free(ptr);
  return ret;
}
```

禁止：
- 直接 `return -1;`
- 忽略 `NCCLCHECK` 返回值
- 使用裸 `malloc`/`new` 而不通过 NCCL 封装（除非与外部 C 库交互）

## 4. 动态符号加载

参考 `src/include/ratls_openssl.h`：

```cpp
#define NCCL_FOO_SYMBOLS(SYMBOL) \
  SYMBOL(some_function) \
  SYMBOL(another_function)

struct ncclFooSymbols {
#define NCCL_FOO_DECLARE_SYMBOL(name) decltype(&::name) pfn_##name;
  NCCL_FOO_SYMBOLS(NCCL_FOO_DECLARE_SYMBOL)
};
```

加载代码在 `cryptLoadOpenSslSymbols()` 等位置。

## 5. 加密与 TEE

- TLS 代码集中在 `src/misc/crypt.cc`。
- RA-TLS 代码：
  - TDX 证明：`src/misc/ratls_tdx.cc`
  - 证书：`src/misc/ratls_cert.cc`
  - 符号表：`src/include/ratls_openssl.h`
- 新增 TEE 类型时：新增 `ratls_<tee>.cc/h`，保持接口与 `ratls_tdx.h` 一致。
- 证书 OID、MRTD 长度、quote 结构变更需同步更新 `ratls_cert.h` 与 verify callback。

## 6. 测试要求

- 单元测试：NCCL 本身无内置单元测试框架，复杂逻辑应提供可独立编译的小测试或外部 nccl-tests 验证。
- 性能回归：改动 transport/加密路径后，用 nccl-tests 对比基线（至少 `all_reduce`, `all_gather`, `sendrecv`）。
- TEE 相关：在 TDX CVM 上验证通过后再合并；无 TDX 环境时必须保证非 TDX 路径不崩溃。

## 7. 文档同步

- 新增公共 API：同步更新 `docs/` 中相关用户指南。
- 新增 TEE/加密方案：在 `docs/CVM-RDMA-分布式推理-技术分析.md` 或新建技术文档中补充设计说明。
- 大改动：同步更新 `GOAL.md` / `UNFINISHED.md`（如果存在）。
