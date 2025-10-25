# fsevent

面向 macOS 的 FSEvents 监听与增量同步组件。该仓库现在拆分为三部分，以配合 App Store 审核要求并支持开机自启：

1. **FSEventCore (C 库)** — 位于 `fsevent/`，暴露 `fsevent_core_run` 等接口，封装 LevelDB、FSEvents 与 HTTP 处理逻辑，供命令行工具与后台 Helper 共享。
2. **FSEventHelper (Login Item)** — 位于 `Apps/FSEventHelper/`，一个纯后台的 Login Item 进程，禁用 HTTP 服务，仅负责启动核心监控。主应用通过 `SMAppService` 管理其开机启动权限。
3. **FSEventApp (GUI 主程序)** — 位于 `Apps/FSEventApp/`，用于展示 UI、引导用户授权、控制是否在登录时启动 Helper，并可通过 App Group/XPC 与 Helper 交换数据。

## 目录结构

```
Apps/
  FSEventApp/        # GUI 入口 (Swift)
  FSEventHelper/     # Login Item (Objective-C)
fsevent/             # 核心 C 模块 (FSEventCore)
fsevent.xcodeproj/   # Xcode 工程配置
```

## 核心库 API

`fsevent/FSEventCore.h` 暴露以下接口，供 Helper 与其他宿主调用：

```c
int fsevent_core_run(const fsevent_core_configuration *configuration);
int fsevent_core_run_default(void);
int fsevent_core_prepare_shared_storage(char *out, size_t out_size);
```

- `fsevent_core_run` 允许传入 LevelDB 路径、是否开启内置 HTTP 服务等配置；
- `fsevent_core_run_default` 保持命令行工具的原有行为；
- `fsevent_core_prepare_shared_storage` 负责创建 `~/Library/Application Support/FSEventWatcher/LevelDB` 目录，可在主应用/Helper 中统一调用。

## Xcode 集成建议

1. 在 `fsevent.xcodeproj` 中新增两个 target：`FSEventApp`（macOS App）与 `FSEventHelper`（Login Item）。
2. 将 `fsevent/` 目录编译为静态库或 Framework，供两个 target 链接。
3. 主应用需引导用户开启 Full Disk Access / 文件夹权限，并提供开机启动开关；Helper 仅在用户授予权限后通过 `SMAppService.loginItem(identifier:)` 启动。
4. 使用 App Group 或 XPC 在主应用与 Helper 之间共享 LevelDB 路径与监控状态。

## 构建与调试

- 继续可以使用旧的命令行方式编译核心逻辑：
  ```bash
  clang fsevent/main.c fsevent/RepoMap.c fsevent/pb.c fsevent/vbproto_pb.c -I fsevent \
    -framework CoreServices -framework CoreFoundation -lsqlite3 -lleveldb -lpthread -o fsevent
  ```
- Helper target 需链接 Foundation、CoreServices、CoreFoundation 以及 FSEventCore 暴露的头文件。
- GUI target 需要启用 App Sandbox、User Selected Read/Write、SMAppService 权限，并在 `FSEventApp-Bridging-Header.h` 中导入 `FSEventCore.h`。

## 审核提示

- 所有持久化路径应迁移到 App Group 或沙盒可写目录；
- 提供 UI 指引用户授权文件访问、启用/禁用开机启动；
- 确保 Helper 进程在用户撤销授权后停止运行，符合 App Store 审核规范。
