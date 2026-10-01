#pragma once

#include <string>

namespace Opsis {

struct RuntimeConfig {
	std::string Sysroot;
	std::string DatabaseDirectory;
	bool AllowNonRoot{false};
	std::string ScriptPath;
	std::string BaseDirectory;
	bool PackOnly{false};
	std::string PackageNamespace;
	std::string PackageName;
	std::string PackageVersion;
	std::string PackageDescription;
	std::string PackageArchitecture;
	std::string PackageOutput;
	std::string BuildDirectory;
	std::string EntryName;
	std::string OldVersion;
	std::string NewVersion;
	std::string Reason;
};

/**
 * PackageScene 描述一次生命周期调用带给脚本的包身份。
 * 字段为空时不覆盖 RuntimeConfig 里已有的值。
 * Architecture 为空时，脚本里的 Context.Architecture 使用 x86_64。
 */
struct PackageScene {
	std::string Namespace;
	std::string Name;
	std::string Version;
	std::string OldVersion;
	std::string NewVersion;
	std::string Architecture;
	std::string Reason;
};

/**
 * LoadConfig() - 从环境变量填安装根和记录目录。
 *
 * Return: Sysroot 默认是 /。OPSIS_ALLOW_NONROOT 为 1 时允许非 root。
 * 记录目录默认是 Sysroot 下的 var/lib/okrapm/db，也可由 OPSIS_DB_DIR 指定。
 * OPSIS_PKG_NAMESPACE、OPSIS_PKG_NAME、OPSIS_PKG_VERSION、OPSIS_PKG_DESCRIPTION、
 * OPSIS_PKG_ARCH、OPSIS_PKG_OUTPUT 和 OPSIS_BUILD_DIR 填入直包字段。
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

/**
 * RunLifecycleScript() - 按入口契约执行包程序。
 * @PackageDir: 已解包的包目录。相对路径相对于这个目录。
 * @Entry: 入口契约。MAIN、INSTALL、UPDATE、REMOVE。旧名 Main、Install、Upgrade、Remove 指向同一入口。
 * @Sysroot: 安装根。
 * @AllowNonRoot: 为 true 时允许非 root。规则与 RunPackageScript() 相同。
 * @Scene: 传给 Context 的包身份。UPDATE 使用 OldVersion 和 NewVersion。
 *
 * 运行时按入口找方法，不把文件名当成语言语义。查找顺序是 package.opsis、lifecycle.opsis，
 * 然后才是同名的约定文件（install.opsis、update.opsis、upgrade.opsis、remove.opsis）。
 * 程序里没有这个入口时返回 0，由宿主走默认安装、升级或卸载。
 * 入口返回非 0 的 int 时，本次调用失败。
 *
 * Return: 没有对应入口时返回 0。其余与 RunSource() 相同。
 */
int RunLifecycleScript(const std::string &PackageDir, const std::string &Entry, const std::string &Sysroot,
	bool AllowNonRoot, const PackageScene &Scene = {});

/**
 * RunScriptFile() - 执行一份已经保存下来的生命周期脚本。
 * @ScriptPath: 脚本文件。相对路径相对于该文件所在目录。
 * @Entry: 入口契约，例如 REMOVE。
 * @Sysroot: 安装根。
 * @AllowNonRoot: 为 true 时允许非 root。
 * @Scene: 传给 Context 的包身份。
 *
 * Return: 文件不存在时返回 0。其余与 RunSource() 相同。
 */
int RunScriptFile(const std::string &ScriptPath, const std::string &Entry, const std::string &Sysroot,
	bool AllowNonRoot, const PackageScene &Scene = {});

} // namespace Opsis
