#pragma once

#include <string>

namespace Opsis {

struct RuntimeConfig {
	std::string Sysroot;
	std::string DatabaseDirectory;
	bool AllowNonRoot{false};
	std::string ScriptPath;
	std::string BaseDirectory;
};

/**
 * LoadConfig() - 从环境变量填安装根和记录目录。
 *
 * Return: Sysroot 默认是 /。OPSIS_ALLOW_NONROOT 为 1 时允许非 root。
 * 记录目录默认是 Sysroot 下的 var/lib/okrapm/db，也可由 OPSIS_DB_DIR 指定。
 */
RuntimeConfig LoadConfig();

/**
 * RunSource() - 解释并执行一段 OPSIS 源码。
 * @Source: 脚本正文。
 * @FileName: 报错时显示的文件名。
 * @Config: 安装根、记录目录，以及是否允许非 root。
 *
 * Return: 成功返回 0。脚本 Fail 或安装动作失败返回 1。语法或类型错误返回 2。
 */
int RunSource(const std::string &Source, const std::string &FileName, const RuntimeConfig &Config);

/**
 * RunFile() - 读取脚本文件并执行。
 * @Path: 脚本路径。源文件相对路径相对于该脚本所在目录。
 * @Config: 安装配置。函数会把 ScriptPath 设为 Path。
 *
 * Return: 与 RunSource() 相同。文件打不开时返回 2。
 */
int RunFile(const std::string &Path, RuntimeConfig Config);

/**
 * RunPackageScript() - 执行 OAA 包内的 OPSIS 安装脚本。
 * @PackageDir: 已解包的包目录。相对路径相对于这个目录，而不是 scripts/。
 * @Sysroot: 安装根。安装记录默认写到其下的 var/lib/okrapm/db。
 * @AllowNonRoot: 为 true 时允许非 root。环境变量 OPSIS_ALLOW_NONROOT=1 同样允许。
 * 当前用户不是 root 且 Sysroot 不是 / 时也允许，以便开发安装。
 *
 * Return: 包里没有 install.opsis 时返回 0。脚本成功返回 0。Fail 返回 1。语法错误返回 2。
 */
int RunPackageScript(const std::string &PackageDir, const std::string &Sysroot, bool AllowNonRoot);

} // namespace Opsis
