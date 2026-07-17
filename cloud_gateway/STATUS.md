# cloud_gateway 当前进度与待解决问题

> 更新时间：2026-07-17
> 工程位置：`D:\ai-hardware-agent-examples\cloud_gateway`

---

## 1. 当前状态概览

| 项目 | 状态 |
|---|---|
| 目录结构 | ✅ 完成 |
| CMakeLists.txt (跨平台) | ✅ 完成（Linux/Windows 均可编译） |
| 公共头 (`include/cloud_gateway/`) | ✅ 4 个文件 |
| WS 层（handshake/frame/server） | ✅ 含 epoll/select 双实现 |
| Codec (envelope/audio frame) | ✅ |
| Session + Manager | ✅ |
| ASR/LLM/TTS Stub 后端 | ✅ |
| main.cpp + CLI 参数 | ✅ |
| 测试客户端 (C++ WS client) | ✅ |
| **单元测试** | ✅ 34/36 通过，2 个失败 |
| 集成测试 (gateway + test_client) | ✅ 跑通完整 hello → audio → LLM → TTS → emotion function call |
| 文档 (`docs/cloud_gateway/`) | ✅ 5 份 |

可执行文件 (`build/`)：
- `cloud_gateway.exe` — 服务端
- `gateway_test_client.exe` — 集成测试客户端
- `gateway_unit_tests.exe` — 单元测试运行器

---

## 2. 已修复的关键 Bug

| Bug | 现象 | 修复 |
|---|---|---|
| `htons` 与 `WsConnection::close()` 同名 | Windows 编译失败，宏替换把方法名改成 `closesocket` | 改用 `cg_close` 作为 POSIX close 的别名 |
| `WSAStartup` 未初始化 | 测试客户端第一帧 send 即失败 | `main()` 开头加 `WSAStartup(MAKEWORD(2,2),&wsad)` |
| 路径引号 `../cloud_gateway/log.hpp` | include 路径错 | 改为 `cloud_gateway/log.hpp` |
| `enum LogLevel::ERROR` 与 Win32 `ERROR` 宏冲突 | Linux OK，Windows 编译错 | 改名为 `LOG_ERROR` |
| `ParseResult` 在 `frame.hpp` 和 `handshake.hpp` 双定义 | 编译错 | 重命名为 `HandshakeParseResult` |
| `errno`/`EAGAIN` 等未定义 | Windows 编译错 | `#ifdef _WIN32` 加宏映射 |
| `vector::pop_front` 不存在 | 编译错 | 改用 `deque` |
| `fcntl`/`arpa/inet.h` 在 Windows 缺失 | Windows 编译错 | 抽出 `EventLoop` 抽象层，Windows 走 select + Winsock |
| `setenv` 在 Windows 不存在 | 配置解析编译错 | Windows 用 `_putenv_s` |
| `SIGPIPE` 在 Windows 不存在 | main.cpp 编译错 | `#ifndef _WIN32` 包起来 |
| **写队列 race** | TTS 触发 WSAEFAULT 10014，第二个 TTS batch 直接断流 | `write_queue_` 加 `std::mutex write_mu_`，send/recv 两端都上锁 |
| StubTts 多次调用 cb | 同一回合出现 N 对 Start/End+ status 抖动 | 合并为单次 `cb(std::move(batch))` |
| `on_audio_binary` 每个 Frame 都触发 ASR | 同一段音频被识别 N 次 | 改为只在 `AudioOp::End` 触发，Frame 只入 `audio_buf_` |

---

## 3. 仍然失败的 2 个单元测试

### 3.1 `sha1.known_vectors` — "abc" 哈希值错误

**期望**：`a9993e364706816aba3e25717850c26c9cd0d89f`
**当前**：未知的错误值（空字符串测试通过：`da39a3ee5e6b4b0d3255bfef95601890afd80709`）

**排查思路**：
1. 空字符串通过说明初始化、`update`、`finalize` 主流程没问题
2. "abc" 是单 block、单次 transform 场景
3. 可能位置：
   - `w[]` 大端字节读取错误（取 4 字节拼成 u32）
   - `transform` 中 `f`/`k` 公式错了
   - `rotl` 实现错了
   - `bit_count_` 累加/长度编码错了
4. 最快定位方式：写一个小程序，对标准库 SHA1 比对；或者对照 Python `hashlib.sha1(b"abc").hexdigest()` 走一遍

**已实现修复思路**：把 `sha1.cpp` 替换为已知正确的实现版本（如 boost 的 `sha1.hpp` 移植，或网上公认的 100 行实现）。

### 3.2 `wsframe.masked_client_frame_unmask` — `has_frame()` 返回 false

**测试代码**：
```cpp
std::vector<std::uint8_t> raw = {0x81, 0x86, 0x01, 0x02, 0x03, 0x04};
std::string payload = "hello!";  // 6 bytes
std::uint8_t mask[4] = {0x01, 0x02, 0x03, 0x04};
for (std::size_t i = 0; i < payload.size(); ++i) {
    raw.push_back(static_cast<std::uint8_t>(payload[i] ^ mask[i & 3]));
}
cg::WsFrameParser p;
p.push(raw.data(), raw.size());
EXPECT_TRUE(p.has_frame());   // ← 失败
```

**问题**：12 字节输入，FIN=1, opcode=text, MASK=1, length=6, 4字节 mask + 6字节 masked payload。

**检查 `WsFrameParser::try_parse_one`**：
```cpp
const std::uint8_t* p = buf_.data();
bool fin     = (p[0] & 0x80) != 0;     // 0x81 & 0x80 = 0x80 → true ✓
bool masked  = (p[1] & 0x80) != 0;     // 0x86 & 0x80 = 0x80 → true ✓
std::uint8_t op_v = p[0] & 0x0F;     // 0x81 & 0x0F = 0x01 (text) ✓
std::uint64_t plen = p[1] & 0x7F;     // 0x86 & 0x7F = 6 ✓
std::size_t header_len = 2;
if (plen == 126) ... else if (plen == 127) ...  // skip
if (masked) header_len += 4;          // header_len = 6
if (buf_.size() < header_len + plen) return false;  // 12 < 6+6 = 12 ✓
```

后续应该能成功解析。失败原因可能是：
- `apply_mask` 实现错了
- 或者 `has_frame()` 在 `try_parse_one` 返回 false 后被错误地调用

**已实现修复思路**：在 `apply_mask` 处加 printf，确认解掩码后 payload 是否真的等于 "hello!"。

---

## 4. 进一步待办（按优先级）

### 4.1 高优
1. 修上面 2 个失败的单元测试
2. 写 **CI script**（`run_tests.bat` / `run_tests.sh`）一键跑：build → unit → integration
3. `cmake --build` 加 `ctest` 集成，CMake 里 `add_test()` 三个 target

### 4.2 中优
4. 把 `run_tests.sh` 写完（Linux/POSIX 版本）
5. 在 `deployment.md` 加 **CI/CD** 段落，描述 GitHub Actions 怎么跑
6. 在 `api_reference.md` 加 **TestCase/Register 宏** 文档
7. 单元测试输出加 `--verbose` 选项（看到每个 EXPECT 通过/失败的具体值）

### 4.3 低优
8. 把 StubLlm 的情绪切换做成可配置（启动参数 `--llm-emotion joy/sad/...`）
9. `add_perf_test`：500 并发 idle session，事件循环耗时
10. `tests/integration/` 目录下加 Python 客户端，跨进程验证

---

## 5. 关键文件索引

| 路径 | 行数 | 作用 |
|---|---|---|
| `src/ws/server.cpp` | ~430 | epoll/select 抽象、WsConnection 状态机、并发安全的 write_queue |
| `src/session/session.cpp` | ~310 | 每连接的会话生命周期，串接 ASR→LLM→TTS |
| `src/upstream/llm_backend.cpp` | ~30 | Stub：固定对话脚本 + 一次 emotion function_call |
| `src/upstream/tts_backend.cpp` | ~80 | Stub：440Hz 正弦 + 振幅包络，G.711A 编码 |
| `src/codec/envelope.cpp` | ~120 | 文本/二进制双帧编解码 |
| `test/test_main.cpp` | ~615 | 自带测试 harness，36 个测试 |

---

## 6. 复现命令

```powershell
# 一次性环境（PowerShell）
$env:PATH = "D:\ai-hardware-agent-examples\cloud_gateway\examples\goldieos\tools\build\tools\compiler\riscv\cc_riscv32_musl_105\cc_riscv32_musl_fp_win\bin;D:\ai-hardware-agent-examples\cloud_gateway\examples\goldieos\tools\build\tools;" + $env:PATH

# 编译
cd D:\ai-hardware-agent-examples\cloud_gateway\build
cmake .. -G "MinGW Makefiles"
cmake --build .

# 跑单元测试
.\gateway_unit_tests.exe

# 集成测试
.\cloud_gateway.exe --port 9099 &
.\gateway_test_client.exe --host 127.0.0.1 --port 9099
```