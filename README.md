# OkraPM / Lunar

Okra 的全部文档在总仓库 `git@github.com:OkraLinux/DOCS.git`。本文件只保留这个仓库的构建和命令入口。

OkraPM 是 Okra Rolling Linux 的用户态软件包管理工具链。项目提供：

- `lunar`：依赖解析、事务安装、软件仓库同步和系统升级；
- `oaa`：OAA（Okra Application Artifact）包的构建、校验和安装入口；
- `.oaa`、`.okra`：基于 tar 归档的应用包和传统 Okra 包格式。

## 构建

依赖 CMake 3.16 或更高版本、C++17 编译器以及系统 `tar` 工具。

```bash
cmake -S . -B build
cmake --build build -j2
ctest --test-dir build --output-on-failure
```

构建完成后，Lunar 可执行文件位于 `build/src/lunar/lunar`。

OAA Shell 工具不需要编译，可以直接从 `oaatools/oaatools` 使用，或安装到系统：

```bash
make -C oaatools
sudo make -C oaatools install
```

安装后提供 `oaa` 命令，并将工具安装到 `/usr/bin` 和 `/usr/lib/okrapm`。

## 最小使用体验

### 校验 OAA 包

```bash
oaa verify package.oaa
```

校验包括：

1. `.sha256` sidecar 文件（如果存在）；
2. `meta.yaml` 中的 `checksum` 字段（如果存在）；
3. tar 归档是否可以读取。

也可以使用 Lunar：

```bash
lunar verify package.oaa
```

### 安装本地包

```bash
oaa install package.oaa
```

本地包安装会执行 SHA256 校验，并检查 payload 中的文件是否已经存在。检测到文件冲突时，安装会失败，不会覆盖已有文件。

OAA 包通常使用以下布局：

```text
package/
├── meta.yaml
├── rootfs/
│   ├── usr/bin/
│   └── etc/
└── scripts/
    ├── pre-install
    └── post-install
```

传统 `.okra` 包也可以通过相同入口安装：

```bash
lunar install package.okra
```

非 root 环境下，安装文件默认写入 Lunar 数据目录的 `rootfs/` 子目录。可以使用环境变量指定安装根目录：

```bash
LUNAR_INSTALL_ROOT=/tmp/rootfs lunar install package.oaa
```

### 卸载包

```bash
oaa remove package-name
lunar remove package-name
```

卸载操作会更新 Lunar 系统状态。安装事务失败时，已写入的本地文件会被删除，系统状态也不会提交。

## 依赖、事务和回滚

Lunar 会从本地或远程仓库解析依赖，并按依赖顺序生成事务计划。安装、删除和升级都会经过事务提交；提交前自动创建系统快照。

预览事务而不执行：

```bash
lunar plan install package-name
lunar plan remove package-name
lunar plan update
```

查看已安装软件：

```bash
lunar list
lunar status
lunar info package-name
```

管理快照：

```bash
lunar snapshot list
lunar snapshot create "before upgrade"
lunar rollback <snapshot-id>
```

## 软件仓库

同步所有已配置仓库：

```bash
lunar sync
```

仓库管理：

```bash
lunar repo list
lunar repo add <name> <url>
lunar repo enable <name>
lunar repo disable <name>
lunar repo remove <name>
```

本地仓库和远程仓库都由 Lunar 管理。远程仓库同步后，包元数据可用于搜索、依赖解析和安装。

## 更新和系统升级

滚动更新已安装软件：

```bash
lunar update
```

升级指定软件：

```bash
lunar update package-name
```

系统基线升级命令为：

```bash
lunar upgradle okra.systemversion
```

`upgrade` 是常见用户习惯写法；当前正式命令名为 `upgradle`。

## OAA 工具命令

```bash
oaa new <directory>
oaa build <directory> -o package.oaa
oaa inspect package.oaa
oaa list package.oaa
oaa extract package.oaa <destination>
oaa sha256 package.oaa
oaa verify package.oaa
```

使用 `oaa install` 和 `oaa remove` 时，工具默认查找 `lunar`。如果 Lunar 不在 `PATH` 中，可以指定路径：

```bash
LUNAR_BIN=/path/to/lunar oaa install package.oaa
```

## 数据目录

默认 Lunar 数据目录为 `/var/lib/lunar`。开发和测试时可使用：

```bash
LUNAR_DATA_DIR=/tmp/lunar lunar list
```

也可以通过命令行指定：

```bash
lunar --root /tmp/lunar status
```

数据目录主要包含：

- `system.db`：已安装对象和系统状态；
- `repos/`：本地仓库数据；
- `cache/downloads/`：下载缓存；
- `snapshots/`：系统快照；
- `rootfs/`：非 root 环境下的本地安装根目录。

## 相关组件

- C++ 核心库：`lib/okrapmlib/`
- Lunar CLI：`src/lunar/`
- OAA 工具链：`oaatools/`
- 综合测试：`tests/test_all.cpp`
