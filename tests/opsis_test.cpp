#include "opsis/interpreter.h"
#include "okrapmlib/artifact_engine.h"
#include "okrapmlib/lunar_core.h"

#include <sys/stat.h>
#include <unistd.h>
#include <cstdlib>

#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace fs = std::filesystem;

namespace {

Opsis::RuntimeConfig TestConfig(const fs::path &Root)
{
	Opsis::RuntimeConfig Config;
	Config.Sysroot = Root.string();
	Config.DatabaseDirectory = (Root / "var/lib/okrapm/db").string();
	Config.AllowNonRoot = true;
	return Config;
}

void WriteFile(const fs::path &Path, const std::string &Text)
{
	fs::create_directories(Path.parent_path());
	std::ofstream Output(Path);
	assert(Output);
	Output << Text;
}

void TestInstallScript()
{
	fs::path Root = fs::temp_directory_path() / "opsis-install-test";
	fs::remove_all(Root);
	fs::path ScriptDir = Root / "pkg";
	fs::path Sysroot = Root / "sysroot";
	fs::create_directories(Sysroot);
	WriteFile(ScriptDir / "payload/usr/bin/make", "make-ok\n");
	WriteFile(ScriptDir / "payload/usr/share/doc.txt", "doc-ok\n");
	WriteFile(ScriptDir / "meta.yaml", "name: make\n");

	std::string Source = R"OPSIS(
// Install make into the sysroot.
public class Package {
	public static void Install() {
		string Name = "make";
		int Sum = 0;
		for (int Index = 1; Index < 4; Index = Index + 1) {
			Sum = Sum + Index;
		}
		bool Ok = Sum == 6 && Name == "make";
		if (!Ok) {
			Fail("loop");
		} else {
			Opsis.LogInfo("sum ok");
		}
		int Left = 2;
		while (Left > 0) {
			Left = Left - 1;
		}
		if (Left != 0) {
			Fail("while");
		}
		CheckUser();
		if (DiskFreeKb() < 1) {
			Fail("disk");
		}
		InstallDirectory("payload", "/");
		InstallFile("payload/usr/bin/make", "/usr/bin/make", "0755", "0:0");
		UpdateLdconfig();
		UpdateSystemd();
		RecordInstalled(Name, "4.4.1", "meta.yaml");
	}
}
)OPSIS";

	fs::path Script = ScriptDir / "install.opsis";
	WriteFile(Script, Source);
	Opsis::RuntimeConfig Config = TestConfig(Sysroot);
	Config.ScriptPath = Script.string();
	int Status = Opsis::RunFile(Script.string(), Config);
	assert(Status == 0);
	assert(fs::is_regular_file(Sysroot / "usr/share/doc.txt"));
	{
		std::ifstream Doc(Sysroot / "usr/share/doc.txt");
		std::string Text;
		std::getline(Doc, Text);
		assert(Text == "doc-ok");
	}
	{
		std::ifstream Bin(Sysroot / "usr/bin/make");
		std::string Text;
		std::getline(Bin, Text);
		assert(Text == "make-ok");
	}
	struct stat Stat {};
	assert(stat((Sysroot / "usr/bin/make").c_str(), &Stat) == 0);
	assert((Stat.st_mode & 0777) == 0755);
	{
		std::ifstream Version(Sysroot / "var/lib/okrapm/db/make/version");
		std::string Text;
		std::getline(Version, Text);
		assert(Text == "4.4.1");
	}
	assert(fs::is_regular_file(Sysroot / "var/lib/okrapm/db/make/manifest"));
	assert(fs::is_regular_file(Sysroot / "var/lib/okrapm/db/make/installed_time"));
	fs::remove_all(Root);
	std::cout << "[PASS] TestInstallScript\n";
}

void TestFailureRollsNoRecord()
{
	fs::path Root = fs::temp_directory_path() / "opsis-fail-test";
	fs::remove_all(Root);
	fs::path Sysroot = Root / "sysroot";
	fs::create_directories(Sysroot);
	std::string Source = R"OPSIS(
public class Package {
	public void Install() {
		CheckDiskSpace(9223372036854775807);
		RecordInstalled("should-not", "1", "meta.yaml");
	}
}
)OPSIS";
	Opsis::RuntimeConfig Config = TestConfig(Sysroot);
	int Status = Opsis::RunSource(Source, "fail.opsis", Config);
	assert(Status == 1);
	assert(!fs::exists(Sysroot / "var/lib/okrapm/db/should-not/version"));
	fs::remove_all(Root);
	std::cout << "[PASS] TestFailureRollsNoRecord\n";
}

void TestEscapeAndSyntax()
{
	fs::path Root = fs::temp_directory_path() / "opsis-escape-test";
	fs::remove_all(Root);
	fs::path Sysroot = Root / "sysroot";
	fs::path ScriptDir = Root / "pkg";
	fs::create_directories(Sysroot);
	WriteFile(ScriptDir / "payload/usr/bin/make", "make-ok\n");
	std::string Source = R"OPSIS(
void Install() {
	InstallFile("payload/usr/bin/make", "/../../outside", "0755", "0:0");
}
)OPSIS";
	fs::path Script = ScriptDir / "install.opsis";
	WriteFile(Script, Source);
	Opsis::RuntimeConfig Config = TestConfig(Sysroot);
	int Status = Opsis::RunFile(Script.string(), Config);
	assert(Status == 1);
	assert(!fs::exists(Root / "outside"));

	Status = Opsis::RunSource("void Install( {\n", "bad.opsis", Config);
	assert(Status == 2);

	if (geteuid() != 0) {
		Config.AllowNonRoot = false;
		Status = Opsis::RunSource("void Install() { CheckUser(); }\n", "user.opsis", Config);
		assert(Status == 1);
	}
	fs::remove_all(Root);
	std::cout << "[PASS] TestEscapeAndSyntax\n";
}

void TestLunarCommitRunsOpsis()
{
	fs::path Root = fs::temp_directory_path() / "opsis-lunar-commit";
	fs::remove_all(Root);
	fs::path Package = Root / "pkg";
	fs::path Sysroot = Root / "sysroot";
	fs::path Data = Root / "lunar";
	fs::create_directories(Sysroot);
	WriteFile(Package / "files/usr/bin/tool", "tool-ok\n");
	WriteFile(Package / "meta.yaml",
		"name: tool\nnamespace: app\nversion: 1.0.0\ndescription: \"tool\"\n");
	WriteFile(Package / "scripts/install.opsis",
		"public class Package {\n"
		"    public void Install() {\n"
		"        RecordInstalled(\"tool\", \"1.0.0\", \"meta.yaml\");\n"
		"    }\n"
		"}\n");

	okrapm::ArtifactBuilder::BuildOptions Options;
	Options.compression = "gzip";
	Options.output_path = (Root / "tool.oaa").string();
	auto Built = okrapm::ArtifactBuilder::build(Package.string(), Options);
	assert(Built.has_value());

	unsetenv("OPSIS_DB_DIR");
	setenv("LUNAR_INSTALL_ROOT", Sysroot.c_str(), 1);
	okrapm::LunarCore Core(Data.string());
	auto Installed = Core.install({*Built});
	assert(Installed.success);
	{
		std::ifstream Bin(Sysroot / "usr/bin/tool");
		std::string Text;
		std::getline(Bin, Text);
		assert(Text == "tool-ok");
	}
	{
		std::ifstream Version(Sysroot / "var/lib/okrapm/db/tool/version");
		std::string Text;
		std::getline(Version, Text);
		assert(Text == "1.0.0");
	}
	assert(Core.info("app.tool").has_value());

	fs::path FailPackage = Root / "failpkg";
	fs::path FailRoot = Root / "failroot";
	fs::path FailData = Root / "faildata";
	fs::create_directories(FailRoot);
	WriteFile(FailPackage / "files/usr/bin/tool", "tool-ok\n");
	WriteFile(FailPackage / "meta.yaml",
		"name: tool\nnamespace: app\nversion: 1.0.0\ndescription: \"tool\"\n");
	WriteFile(FailPackage / "scripts/install.opsis",
		"public class Package {\n"
		"    public void Install() {\n"
		"        Fail(\"stop\");\n"
		"    }\n"
		"}\n");
	Options.output_path = (Root / "fail.oaa").string();
	auto FailBuilt = okrapm::ArtifactBuilder::build(FailPackage.string(), Options);
	assert(FailBuilt.has_value());
	setenv("LUNAR_INSTALL_ROOT", FailRoot.c_str(), 1);
	okrapm::LunarCore FailCore(FailData.string());
	auto Failed = FailCore.install({*FailBuilt});
	assert(!Failed.success);
	assert(!fs::exists(FailRoot / "usr/bin/tool"));
	assert(!FailCore.info("app.tool").has_value());
	unsetenv("LUNAR_INSTALL_ROOT");

	fs::remove_all(Root);
	std::cout << "[PASS] TestLunarCommitRunsOpsis\n";
}

} // namespace

int main()
{
	TestInstallScript();
	TestFailureRollsNoRecord();
	TestEscapeAndSyntax();
	TestLunarCommitRunsOpsis();
	std::cout << "OPSIS tests passed\n";
	return 0;
}
