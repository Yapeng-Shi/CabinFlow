# 旧依赖取证与活动构建对照

记录日期：2026-09-30；环境：WSL2 Ubuntu 24.04 / x86_64。

`runtime/build.sh` 是历史安装脚本，不是当前构建入口。本次只读取源码与现有
包版本，未执行 apt、历史安装脚本或安装新依赖。迁移状态和删除许可仍以
[能力账本](capability-migration.md)为准。

## 旧脚本的可确认内容

- `set -ex`：失败即退出；设置 `TZ=Asia/Shanghai`。
- 无条件运行 `apt-get clean/autoclean/update`，随后 `apt-get install -y`。
- 包清单：`libssl-dev gcc g++ make curl git gdb openssh-server build-essential
  libboost-all-dev net-tools vim libzmq3-dev libgoogle-glog-dev cmake libbsd-dev`。
- 分别检查 `docker/build/install/eventpp` 与 `docker/build/install/simdjson`，
  调用 `install_eventpp.sh` / `install_simdjson.sh`；目录不存在或子脚本失败则退出。
- SHA-256：`d362096acac660c0b5fe6cd0c42a55b17bc6b7c8005fe7cbba3179d1bd8d2dd0`。

当前取证输入中没有 `docker/build/install/` 和两个安装子脚本。不能推断 eventpp、
simdjson 的版本、下载来源、安装参数或副作用，更不能把脚本末尾的完成提示当作
实际安装成功。旧脚本会修改系统包及缓存，不为了取证直接执行它。

## 消费者与迁移决策

| 历史依赖 | 旧消费者证据 | 当前活动构建 | 当前结论 |
|---|---|---|---|
| eventpp | `infra-controller/CMakeLists.txt` 的 find/link；`channel.h`、`StackFlow.h` | Runtime 的节点、队列和控制面不链接 eventpp | 保留旧依赖记录；不恢复旧模块构建 |
| simdjson | `unit-manager/CMakeLists.txt` 的 `/usr/local/lib/libsimdjson.a`；`remote_action.cpp`、`remote_server.cpp` | Gateway 使用唯一 Protobuf 协议 | 不增加 JSON 解析或运行时回退；旧版本未知 |
| ZeroMQ | `hybrid-comm/` 的 libzmq 头、pzmq 实现和旧样例 | `transport/zmq/` 通过 pkg-config 的 libzmq target 私有链接 | 依赖归 Transport 实现，Runtime Core 不暴露 socket |
| glog | 旧 network/sample 的日志依赖；旧 apt 清单含 `libgoogle-glog-dev` | `observability/` 采用项目 Logger | 不把第三方旧日志作为新公开 API |
| Boost、OpenSSL、libbsd | 旧 apt 清单 | 活动 Runtime CMake 没有声明这些依赖 | 安装清单不证明必需；未逐项证明旧消费者，保留未知边界 |
| Protobuf | 非旧脚本安装项 | `protocol/` 使用 `protobuf::libprotobuf` | 新协议明确依赖，不猜测 JSON 格式 |
| 编译/调试/运维工具 | gcc/g++/make/cmake/git/curl/gdb 等旧 apt 清单 | 标准入口 `scripts/test.sh` 和 CMake presets | 与库/API 依赖分开记录，不把 ssh/vim 作为 Runtime 必需依赖 |

活动 CMake 不装配 `infra-controller/`、`unit-manager/`、`network/`、`hybrid-comm/`、
`node/test/`、`sample/` 或旧 `utils/`；这些仍是取证材料，不是兼容实现。

## 当前环境版本与证据边界

已读取的包版本：CMake `3.28.3-1build7`；`libprotobuf-dev` / `protobuf-compiler`
`3.21.12-8.2ubuntu0.3`；`libzmq3-dev` `4.3.5-1build2`；pkg-config `1.8.1-2build1`。
实际查询的 protoc 为 `3.21.12`，libzmq pkg-config 为 `4.3.5`。
这些是本机快照，不是 eventpp/simdjson 的版本，也不是目标板验证。

`legacy_dependency_inventory_test` 固定旧脚本指纹、保留路径以及根 CMake 不接入
旧模块的边界。它是静态取证检查，不运行安装、不证明旧安装流程可复现，
不代替旧 TCP/控制/ZMQ 行为测试，也不构成删除许可。
根构建检查仅匹配直接、单行的 `add_subdirectory` 声明；不是任意多行或间接
构建接入的完整证明。活动依赖方向仍需结合实际 CMake 与构建结果审查。
