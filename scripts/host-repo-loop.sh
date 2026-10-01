#!/bin/bash
# Repeat the host software-source loop against a temporary HTTP repository.
# Success installs GNU.gcc and its dependency GNU.make. Failure deletes files
# already copied and leaves system.db uncommitted.

set -euo pipefail

SourceRoot="$(cd "$(dirname "$0")/.." && pwd)"
LunarBin="${1:-${LUNAR_BIN:-$SourceRoot/build/src/lunar/lunar}}"
ServerPy="$SourceRoot/repo-server/server.py"
WorkRoot="$(mktemp -d /tmp/host-repo-loop.XXXXXX)"
ServerPid=""
Port=""

# Die() - 打印失败原因并退出。
# @Message: 写到标准错误的说明。
# Return: 不返回。退出码是 1。
Die() {
	echo "host-repo-loop: $1" >&2
	exit 1
}

# Cleanup() - 停掉软件源并删掉临时目录。
# Return: 0。
Cleanup() {
	if [ -n "$ServerPid" ]; then
		kill "$ServerPid" 2>/dev/null || true
		wait "$ServerPid" 2>/dev/null || true
		ServerPid=""
	fi
	rm -rf "$WorkRoot"
}
trap Cleanup EXIT

# BuildArtifact() - 把包目录打成 gzip 的 .oaa。
# @PackageDir: 含有 meta.yaml 的目录。
# @OutputPath: 要写出的归档路径。
# Return: 0 表示 tar 成功，非 0 表示打包失败。
BuildArtifact() {
	local PackageDir="$1"
	local OutputPath="$2"
	tar -czf "$OutputPath" -C "$PackageDir" .
}

# WriteGoodPackages() - 写出带 payload 的 GNU.make 和 GNU.gcc。
# @RepoRoot: 软件源根目录，归档放到其中的 artifacts/。
# Return: 0。目录创建或打包失败时退出。
WriteGoodPackages() {
	local RepoRoot="$1"
	local Stage="$WorkRoot/stage-good"
	mkdir -p "$RepoRoot/artifacts" \
		"$Stage/make/files/usr/bin" \
		"$Stage/gcc/files/usr/bin"

	cat > "$Stage/make/meta.yaml" <<'EOF'
name: make
namespace: GNU
version: 4.4.1
description: "GNU Make"
architecture: x86_64
maintainer: "OkraLinux Team <maintainer@okralinux.cn>"
files:
  - /usr/bin/make
EOF
	printf 'echo host-repo-make\n' > "$Stage/make/files/usr/bin/make"

	cat > "$Stage/gcc/meta.yaml" <<'EOF'
name: gcc
namespace: GNU
version: 16.2.1
description: "GNU Compiler Collection"
architecture: x86_64
maintainer: "OkraLinux Team <maintainer@okralinux.cn>"
dependencies:
  - GNU.make
files:
  - /usr/bin/gcc
EOF
	printf 'echo host-repo-gcc\n' > "$Stage/gcc/files/usr/bin/gcc"

	BuildArtifact "$Stage/make" "$RepoRoot/artifacts/GNU.make@4.4.1.oaa"
	BuildArtifact "$Stage/gcc" "$RepoRoot/artifacts/GNU.gcc@16.2.1.oaa"
}

# WriteBrokenGcc() - 写出可以同步、但安装时没有 payload 的 GNU.gcc。
# @RepoRoot: 软件源根目录。
# Return: 0。GNU.make 仍带 payload，GNU.gcc 只有 meta.yaml。
WriteBrokenGcc() {
	local RepoRoot="$1"
	local Stage="$WorkRoot/stage-broken"
	mkdir -p "$RepoRoot/artifacts" "$Stage/make/files/usr/bin" "$Stage/gcc"

	cat > "$Stage/make/meta.yaml" <<'EOF'
name: make
namespace: GNU
version: 4.4.1
description: "GNU Make"
architecture: x86_64
maintainer: "OkraLinux Team <maintainer@okralinux.cn>"
files:
  - /usr/bin/make
EOF
	printf 'echo host-repo-make\n' > "$Stage/make/files/usr/bin/make"

	cat > "$Stage/gcc/meta.yaml" <<'EOF'
name: gcc
namespace: GNU
version: 16.2.1
description: "GNU Compiler Collection"
architecture: x86_64
maintainer: "OkraLinux Team <maintainer@okralinux.cn>"
dependencies:
  - GNU.make
files:
  - /usr/bin/gcc
EOF

	BuildArtifact "$Stage/make" "$RepoRoot/artifacts/GNU.make@4.4.1.oaa"
	BuildArtifact "$Stage/gcc" "$RepoRoot/artifacts/GNU.gcc@16.2.1.oaa"
}

# StartServer() - 在本机端口上提供一个软件源。
# @RepoRoot: 含 artifacts/ 的目录。
# Return: 0 表示 /index.yaml 已经能读到 GNU.make。超时则退出。
StartServer() {
	local RepoRoot="$1"
	if [ -n "$ServerPid" ]; then
		kill "$ServerPid" 2>/dev/null || true
		wait "$ServerPid" 2>/dev/null || true
		ServerPid=""
	fi
	python3 "$ServerPy" --root "$RepoRoot" --bind 127.0.0.1 --port "$Port" \
		>"$WorkRoot/server.log" 2>&1 &
	ServerPid=$!
	local Attempt
	for Attempt in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20; do
		if curl -fsS "http://127.0.0.1:${Port}/index.yaml" 2>/dev/null | grep -q 'name: make'; then
			return 0
		fi
		if ! kill -0 "$ServerPid" 2>/dev/null; then
			Die "repo server exited early; log: $WorkRoot/server.log"
		fi
		sleep 0.1
	done
	Die "repo server did not publish index.yaml on port $Port"
}

# AssertOrder() - 确认事务计划里先安装 GNU.make，再安装 GNU.gcc。
# @PlanText: lunar plan 的标准输出。
# Return: 0 表示顺序正确，否则退出。
AssertOrder() {
	local PlanText="$1"
	local MakeLine GccLine
	MakeLine="$(printf '%s\n' "$PlanText" | grep -n '+[[:space:]]*GNU\.make' | head -n 1 | cut -d: -f1)"
	GccLine="$(printf '%s\n' "$PlanText" | grep -n '+[[:space:]]*GNU\.gcc' | head -n 1 | cut -d: -f1)"
	if [ -z "$MakeLine" ] || [ -z "$GccLine" ]; then
		Die "plan did not list GNU.make and GNU.gcc"
	fi
	if [ "$MakeLine" -ge "$GccLine" ]; then
		Die "GNU.make was not ordered before GNU.gcc"
	fi
}

if [ ! -x "$LunarBin" ]; then
	Die "lunar binary not found: $LunarBin"
fi
if [ ! -f "$ServerPy" ]; then
	Die "repo server not found: $ServerPy"
fi
command -v python3 >/dev/null 2>&1 || Die "python3 is required"
command -v curl >/dev/null 2>&1 || Die "curl is required"
command -v tar >/dev/null 2>&1 || Die "tar is required"

Port="$(python3 -c 'import socket; s=socket.socket(); s.bind(("127.0.0.1", 0)); print(s.getsockname()[1]); s.close()')"

GoodRepo="$WorkRoot/repo-good"
GoodData="$WorkRoot/data-good"
GoodRoot="$WorkRoot/root-good"
WriteGoodPackages "$GoodRepo"
StartServer "$GoodRepo"

"$LunarBin" --root "$GoodData" repo add okra "http://127.0.0.1:${Port}" remote
"$LunarBin" --root "$GoodData" sync okra
grep -q 'name=make' "$GoodData/repos/okra/index.db"
grep -q 'name=gcc' "$GoodData/repos/okra/index.db"

PlanText="$("$LunarBin" --root "$GoodData" plan install GNU.gcc)"
AssertOrder "$PlanText"

export LUNAR_INSTALL_ROOT="$GoodRoot"
"$LunarBin" --root "$GoodData" install GNU.gcc
unset LUNAR_INSTALL_ROOT

grep -q 'host-repo-make' "$GoodRoot/usr/bin/make"
grep -q 'host-repo-gcc' "$GoodRoot/usr/bin/gcc"
grep -q 'name=make' "$GoodData/system.db"
grep -q 'name=gcc' "$GoodData/system.db"
grep -q 'ns=GNU' "$GoodData/system.db"

BadRepo="$WorkRoot/repo-bad"
BadData="$WorkRoot/data-bad"
BadRoot="$WorkRoot/root-bad"
WriteBrokenGcc "$BadRepo"
StartServer "$BadRepo"

"$LunarBin" --root "$BadData" repo add okra "http://127.0.0.1:${Port}" remote
"$LunarBin" --root "$BadData" sync okra

export LUNAR_INSTALL_ROOT="$BadRoot"
set +e
"$LunarBin" --root "$BadData" install GNU.gcc
InstallStatus=$?
set -e
unset LUNAR_INSTALL_ROOT

if [ "$InstallStatus" -eq 0 ]; then
	Die "install of payload-less GNU.gcc succeeded"
fi
if [ -f "$BadRoot/usr/bin/make" ]; then
	Die "GNU.make payload remained after failed install"
fi
if [ -f "$BadData/system.db" ] && grep -q 'name=make' "$BadData/system.db"; then
	Die "system.db recorded GNU.make after failed install"
fi
if [ -f "$BadData/system.db" ] && grep -q 'name=gcc' "$BadData/system.db"; then
	Die "system.db recorded GNU.gcc after failed install"
fi

echo "host-repo-loop: sync, install GNU.gcc (with GNU.make), and rollback passed"
