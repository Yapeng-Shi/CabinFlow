# gRPC 生命周期补证（2026-10-07）

## 本轮结论

补齐服务端退出、Qt 旧连接通知及连接中关闭的回归，没有修改生产协议、Runtime、模型或依赖。
Debug 与限定 gRPC ASan/LSan 通过；gRPC TSan 和 Qt 音频 LSan **仍未通过**。
这不是全项目并发安全、真实语音体验或硬件验收完成。

## 环境与范围

- x86_64 / Ubuntu 24.04 / WSL2，内核 `6.6.87.2-microsoft-standard-WSL2-cabinflow-vcan`。
- GCC 13.3.0，系统 gRPC 1.51.1、Protobuf 3.21.12、Qt 6.4.2。
- 项目 TSan 编译参数为 `-fsanitize=thread -fno-omit-frame-pointer`，本轮补设 Debug 获得源码行号。
  系统 gRPC/Abseil/Protobuf 动态库没有随项目重新插桩。
- 主代理实现测试并执行验证；显式 GPT-6.1 Sol / `max` 对相关所有权、关闭路径和测试做只读前审及复审。
  没有调用 Astra，没有修改此前 gRPC/CAN 的其他未提交内容。

## 新增回归

| 用例 | 直接验证的边界 |
|---|---|
| `concurrent_worker_send_and_server_shutdown` | 每轮先提交 3 MiB 在途写，客户端不读且禁预读/BDP扩窗；两 worker 与服务器 Shutdown 竞争；join 后核对一次 OnDone、迟到 send 拒绝、连接 weak_ptr 最终失效。每次测试 5 轮。 |
| `grpc_old_generation_notification_contract` | 第一代 stop 后不处理 GUI 队列，等 worker settled，再显式建立第二代；旧断连通知不影响新连接，第二代正常断连通知仍能交付，没有自动重连。 |
| `close_during_grpc_connection_contract` | 测试 TCP peer 保活但不提供 HTTP/2 服务；在 gRPC 流尚未建立时关闭。requestClose 不同步等待连接，随后 QTimer 仍运行，含关闭调用的总等待预算为 6 秒。音乐保持 disabled，无回答音频或车辆已知状态。 |

Qt 用例验证的是测试后端下的生命周期，不是本轮重跑真实模型或真实音乐。
连接等待沿用现有 worker 内的 5 秒期限，测试不声称 TryCancel 总能立即中断连接握手。

## 实际执行结果

日志均保存在 `/home/projects/CabinFlow/runtime/build/`，为本地证据，不进入 Git。

| 检查 | 结果 | 日志（相对上述目录） |
|---|---|---|
| 最终 Debug 全套 | 37/37，包含最终关闭计时断言 | `voice-demo/safety-budget-full-20261007.log` |
| 完整 stream 合同重复 | 5/5，每次含新增的 5 轮服务端退出竞争 | `voice-demo/safety-stream-repeat5-20261007.log` |
| 最终 Qt 合同重复 | 3/3，含 6 秒总预算与调用不阻塞断言 | `voice-demo/safety-qt-budget-repeat3-20261007.log` |
| gRPC ASan + LSan | 4/4：voice、stream、control、data | `grpc-can-asan/safety-ctest-20261007.log` |
| 四项 gRPC TSan | CTest exit 8，0/4；641 / 281 / 277 / 921 条报告，共 2120 条 | `grpc-can-tsan/safety-ctest-20261007.log` |
| 加入 Shutdown 后的完整 stream TSan | 功能断言通过，exit 66、302 条报告；计数属于整个测试，不是单一 Shutdown 用例 | `grpc-can-tsan/safety-shutdown-20261007.log` |
| 最小 gRPC API 对照 | echo 断言通过，exit 66、143 条报告 | `grpc-can-tsan/safety-probe-20261007.log` |
| Qt ASan + LSan 首次 | CTest exit 8，1144 bytes / 13 allocations | `grpc-can-asan/safety-qt-20261007.log` |
| Qt 最终版 ASan + LSan | PTY 中确认业务断言通过，但 exit 1、880 bytes / 10 allocations | `grpc-can-asan/safety-qt-budget-pty-20261007.log` |

Qt 泄漏栈涉及 PulseAudio / GStreamer / Qt Multimedia，包含 QAudioOutput 初始化及异步音频操作。
不同运行的分配数量变化，不能据此宣布根因属于第三方、已修复，或一定是新增测试引起的回归。
此前 264 bytes / 3 allocations 等历史失败仍保留，不被本轮数值覆盖。

## TSan 调查：证据与推断分开

带行号报告中，Gateway 相关访问包含：

- `OnReadDone` 读取 reactor 的 `connection_` / handler，与 `GrpcService::Connect` 中分配、构造的写入冲突。
- `GrpcConnection::send` 读取 `reactor_`，与 reactor 构造时的赋值冲突。
- gRPC/Abseil 内部 eventfd、调度、callback、分配释放等访问也有报告。

针对当前调用链的代码审查未找到可证明的 P1/P2 项目竞态：

- 队列、写在途标记、close、Finish 和 OnDone detach 共用 connection mutex；在途 protobuf 保留到写完成。
- 当前生产路径只在读回调准入后向 worker 发布输出去向，没有找到 attach 前 send。
- Qt CQ 状态在同一 mutex 下变更，Finish 后关闭并排空 CQ，析构 join；GUI generation 过滤旧通知。

gRPC 1.51.1 的 handler 在取得完整 reactor 后才 SetupReactor；构造中的 StartRead 先写 backlog，绑定后才发起实际读。
OnDone 必须等待其他 reactions 结束。依据为本机对应头文件及
[固定版本 callback handler 源码](https://github.com/grpc/grpc/blob/v1.51.1/include/grpcpp/impl/codegen/server_callback_handlers.h)、
[固定版本 reactor 源码](https://github.com/grpc/grpc/blob/v1.51.1/include/grpcpp/support/server_callback.h)与
[官方 callback 生命周期说明](https://grpc.io/docs/languages/cpp/best_practices/)。

为对照系统库边界，本轮写了最小 echo：只使用 gRPC API 和生成的 Protobuf，不链接 CabinFlow Gateway/Runtime。
它保留读写 buffer，写完成后 Finish，OnDone 删除 reactor；同样产生 143 条报告。
诊断源码保留在 `runtime/build/grpc-can-tsan/safety_probe_20261007.cpp`，SHA-256：
`865b545334ec96ee56d1cee3ff8897e379aeab11eb81b6f6bfdbf5b6299ee367`。
该源码只是本地产物，不是生产回退路径或正式 CTest。

**推断**：未插桩依赖中的同步可能使 TSan 看不到 happens-before，当前报告与这一解释相符。
最小对照只证明不含 Gateway/Runtime 也会报告，不能排除项目其他真实竞态；因此仍保持 TSan failed。
没有加 suppression、手工 happens-before 注解，没有关闭 sanitizer 或把功能断言通过改称 sanitizer 通过。

## 复现命令

以下命令本轮已执行；重复测试会重新运行，但应为新日志使用新文件名，保留历史失败。

```bash
cd /home/projects/CabinFlow
cmake --build runtime/build/voice-demo --target grpc_stream_contract_test voice_client_test -j 4
ctest --test-dir runtime/build/voice-demo --output-on-failure
ctest --test-dir runtime/build/voice-demo -R '^grpc_stream_contract_test$' --repeat until-fail:5 --output-on-failure
ctest --test-dir runtime/build/voice-demo -R '^voice_client_test$' --repeat until-fail:3 --output-on-failure
ASAN_OPTIONS=detect_leaks=1 ctest --test-dir runtime/build/grpc-can-asan -R '^(grpc_stream_contract_test|control_gateway_grpc_test|data_plane_gateway_grpc_test|voice_gateway_grpc_test)$' --output-on-failure
setarch x86_64 -R env TSAN_OPTIONS=halt_on_error=0:exitcode=66 ctest --test-dir runtime/build/grpc-can-tsan -R '^(grpc_stream_contract_test|control_gateway_grpc_test|data_plane_gateway_grpc_test|voice_gateway_grpc_test)$' --output-on-failure
```

`setarch -R` 只限定当前进程的地址布局，不修改全局 ASLR，也不关闭 TSan。
首次冲突定位另用了 `halt_on_error=1`；完整运行没有遇到第一条报告就停止。

## 未完成与后续边界

- gRPC TSan：同版本依赖完整插桩后的对照尚未做，不能凭当前栈给所有报告定为误报。
- Qt 音频 LSan：最小音频初始化/播放/退出对照及根因修复仍待进行。
- 固定语音用例质量、人工听验、演示视频和网易云官方授权不属于本轮验收；既有未完成状态不变。
- 本轮未安装新依赖、升级生产库、切换模型、启用真实音乐，未提交或推送 Git。
