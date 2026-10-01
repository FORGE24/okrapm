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
#include <iterator>
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

void TestMoreSyntax()
{
	fs::path Root = fs::temp_directory_path() / "opsis-more-syntax";
	fs::remove_all(Root);
	fs::path Sysroot = Root / "sysroot";
	fs::create_directories(Sysroot);
	setenv("OPSIS_TEST_MARK", "yes", 1);
	std::string Source = R"OPSIS(
public class Package {
	public static int Abs(int Number) {
		if (Number < 0) {
			return 0 - Number;
		}
		return Number;
	}

	public static void Install() {
		int Number = 0;
		Number += 2;
		++Number;
		if (Number != 3) {
			Fail("update");
		}
		string Word = "make";
		Word += "-ok";
		if (Word != "make-ok" || Word.Length != 7 || Length(Word) != 7) {
			Fail("concat");
		}
		string Label = Number == 3 ? "yes" : "no";
		if (Label != "yes") {
			Fail("ternary");
		}
		int Kind = 2;
		if (Kind == 1) {
			Label = "a";
		} else if (Kind == 2) {
			Label = "b";
		} else {
			Label = "c";
		}
		if (Label != "b") {
			Fail("else if");
		}
		var Missing = null;
		if (Missing != null) {
			Fail("null");
		}
		int Sum = 0;
		for (int Index = 0; Index < 5; Index++) {
			if (Index == 1) {
				continue;
			}
			if (Index == 4) {
				break;
			}
			Sum += Index;
		}
		if (Sum != 5) {
			Fail("break");
		}
		string[] Names = { "make", "gcc" };
		Names[0] = "cmake";
		if (Names.Length != 2 || Names[0] != "cmake" || Names[1] != "gcc") {
			Fail("array");
		}
		int Seen = 0;
		foreach (string Name in Names) {
			if (!Contains(Name, "a") && !EndsWith(Name, "cc")) {
				Fail("foreach");
			}
			Seen++;
		}
		if (Seen != 2 || !StartsWith("cmake", "c") || Substring("make-ok", 5) != "ok") {
			Fail("string");
		}
		int[] Numbers = { 1, 2, 3 };
		int Total = 0;
		foreach (int Item in Numbers) {
			Total += Item;
		}
		if (Total != 6 || Abs(-4) != 4) {
			Fail("call");
		}
		if (CompareVersion("16.2.1", "16.10") >= 0 || CompareVersion("4.4.1", "4.4.1") != 0) {
			Fail("version");
		}
		if (Environment("OPSIS_TEST_MARK") != "yes") {
			Fail("env");
		}
		WriteFile("/etc/mark", "one");
		AppendFile("/etc/mark", "\ntwo");
		if (ReadFile("/etc/mark") != "one\ntwo" || !FileExists("/etc/mark")) {
			Fail("read");
		}
		MakeDirectory("/opt/share", "0755");
		if (!DirectoryExists("/opt/share")) {
			Fail("dir");
		}
		Symlink("../etc/mark", "/opt/share/mark");
		MovePath("/etc/mark", "/etc/mark2");
		if (FileExists("/etc/mark") || !FileExists("/etc/mark2")) {
			Fail("move");
		}
		SetMode("/etc/mark2", "0640");
		RemoveDirectory("/opt");
		if (!FileExists("/etc/mark2") || DirectoryExists("/opt")) {
			Fail("remove");
		}
		RecordInstalled("make", "4.4.1", "missing.yaml");
		if (!IsInstalled("make")) {
			Fail("installed");
		}
		if (Run("/bin/true") != 0) {
			Fail("run");
		}
		UpdateDesktopDatabase();
		UpdateMimeDatabase();
		UpdateIconCache();
	}
}
)OPSIS";
	Opsis::RuntimeConfig Config = TestConfig(Sysroot);
	int Status = Opsis::RunSource(Source, "more.opsis", Config);
	assert(Status == 0);
	struct stat Stat {};
	assert(stat((Sysroot / "opt").c_str(), &Stat) != 0);
	assert(stat((Sysroot / "etc/mark2").c_str(), &Stat) == 0);
	assert((Stat.st_mode & 0777) == 0640);
	{
		std::ifstream Version(Sysroot / "var/lib/okrapm/db/make/version");
		std::string Text;
		std::getline(Version, Text);
		assert(Text == "4.4.1");
	}
	Status = Opsis::RunSource("void Install() { RemoveDirectory(\"/\"); }\n", "root.opsis", Config);
	assert(Status == 1);
	assert(fs::is_directory(Sysroot));
	Status = Opsis::RunSource("void Install() { WriteFile(\"/../../outside\", \"x\"); }\n", "out.opsis", Config);
	assert(Status == 1);
	assert(!fs::exists(Root / "outside"));
	Status = Opsis::RunSource("void Install() { break; }\n", "break.opsis", Config);
	assert(Status == 2);
	unsetenv("OPSIS_TEST_MARK");
	fs::remove_all(Root);
	std::cout << "[PASS] TestMoreSyntax\n";
}

void TestDirectPackage()
{
	fs::path Root = fs::temp_directory_path() / "opsis-direct-pack";
	fs::remove_all(Root);
	fs::path Build = Root / "build";
	fs::path Share = Build / "share";
	fs::create_directories(Share);
	WriteFile(Build / "tool", "tool-bin\n");
	WriteFile(Share / "note.txt", "note\n");
	fs::permissions(Build / "tool", fs::perms::owner_all, fs::perm_options::add);

	Opsis::RuntimeConfig Config = TestConfig(Root / "missing-sysroot");
	Config.PackOnly = true;
	Config.PackageNamespace = "Okra";
	Config.PackageName = "tool";
	Config.PackageVersion = "1.2.0";
	Config.PackageDescription = "direct tool";
	Config.PackageOutput = (Root / "Okra.tool.oaa").string();
	Config.BuildDirectory = Build.string();

	std::string Source = R"OPSIS(
public class Package {
	public void Main() {
		BeginPackage();
		PackFile("tool", "/usr/bin/tool", "0755");
		PackDirectory("share", "/usr/share/tool");
		AddDependency("GNU.make");
		FinishPackage();
	}
}
)OPSIS";
	int Status = Opsis::RunSource(Source, "pack.opsis", Config);
	assert(Status == 0);
	assert(!fs::exists(Root / "missing-sysroot"));
	assert(fs::is_regular_file(Root / "Okra.tool.oaa"));
	assert(fs::is_regular_file(Root / "Okra.tool.oaa.sha256"));

	auto Meta = okrapm::ArtifactExtractor::inspect((Root / "Okra.tool.oaa").string());
	assert(Meta.has_value());
	assert(Meta->ns == "Okra");
	assert(Meta->name == "tool");
	assert(Meta->version.to_string() == "1.2.0");
	assert(Meta->dependencies.size() == 1);
	assert(Meta->dependencies[0] == "GNU.make");

	fs::path Extracted = Root / "extracted";
	fs::create_directories(Extracted);
	std::string Unpack = "tar -xf '" + (Root / "Okra.tool.oaa").string() + "' -C '" + Extracted.string() + "'";
	assert(std::system(Unpack.c_str()) == 0);
	{
		std::ifstream Bin(Extracted / "rootfs/usr/bin/tool");
		std::string Text;
		std::getline(Bin, Text);
		assert(Text == "tool-bin");
	}
	{
		std::ifstream Note(Extracted / "rootfs/usr/share/tool/note.txt");
		std::string Text;
		std::getline(Note, Text);
		assert(Text == "note");
	}
	struct stat Stat {};
	assert(stat((Extracted / "rootfs/usr/bin/tool").c_str(), &Stat) == 0);
	assert((Stat.st_mode & 0777) == 0755);
	assert(fs::is_regular_file(Extracted / "scripts/install.opsis"));
	assert(fs::is_regular_file(Extracted / "meta.yaml"));

	Status = Opsis::RunSource(
		"public class Package {\n"
		"  public void Main() {\n"
		"    BeginPackage();\n"
		"    PackFile(\"tool\", \"/../../outside\", \"0755\");\n"
		"    FinishPackage();\n"
		"  }\n"
		"}\n",
		"escape.opsis", Config);
	assert(Status == 1);
	assert(!fs::exists(Root / "outside"));
	fs::remove_all(Root);
	std::cout << "[PASS] TestDirectPackage\n";
}

void TestRemoveAndUpgrade()
{
	fs::path Root = fs::temp_directory_path() / "opsis-lifecycle";
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
		"        WriteFile(\"/usr/share/mark\", \"install\");\n"
		"        RecordInstalled(\"tool\", \"1.0.0\", \"meta.yaml\");\n"
		"    }\n"
		"}\n");
	WriteFile(Package / "scripts/remove.opsis",
		"public class Package {\n"
		"    public void Remove() {\n"
		"        RemoveFile(\"/usr/bin/tool\");\n"
		"        WriteFile(\"/var/gone\", \"yes\");\n"
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
	assert(fs::is_regular_file(Sysroot / "usr/bin/tool"));

	auto Removed = Core.remove({"app.tool"});
	assert(Removed.success);
	assert(!fs::exists(Sysroot / "usr/bin/tool"));
	{
		std::ifstream Gone(Sysroot / "var/gone");
		std::string Text;
		std::getline(Gone, Text);
		assert(Text == "yes");
	}
	assert(!Core.info("app.tool").has_value());

	fs::path FailPackage = Root / "failpkg";
	WriteFile(FailPackage / "files/usr/bin/tool", "tool-ok\n");
	WriteFile(FailPackage / "meta.yaml",
		"name: tool\nnamespace: app\nversion: 1.0.0\ndescription: \"tool\"\n");
	WriteFile(FailPackage / "scripts/install.opsis",
		"public class Package {\n"
		"    public void Install() {\n"
		"        RecordInstalled(\"tool\", \"1.0.0\", \"meta.yaml\");\n"
		"    }\n"
		"}\n");
	WriteFile(FailPackage / "scripts/remove.opsis",
		"public class Package {\n"
		"    public void Remove() {\n"
		"        Fail(\"stop\");\n"
		"    }\n"
		"}\n");
	Options.output_path = (Root / "fail.oaa").string();
	auto FailBuilt = okrapm::ArtifactBuilder::build(FailPackage.string(), Options);
	assert(FailBuilt.has_value());
	assert(Core.install({*FailBuilt}).success);
	assert(!Core.remove({"app.tool"}).success);
	assert(fs::is_regular_file(Sysroot / "usr/bin/tool"));
	assert(Core.info("app.tool").has_value());

	fs::path Next = Root / "next";
	WriteFile(Next / "files/usr/bin/tool", "tool-ok\n");
	WriteFile(Next / "meta.yaml",
		"name: tool\nnamespace: app\nversion: 2.0.0\ndescription: \"tool\"\n");
	WriteFile(Next / "scripts/install.opsis",
		"public class Package {\n"
		"    public void Install() {\n"
		"        Fail(\"install-on-upgrade\");\n"
		"    }\n"
		"}\n");
	WriteFile(Next / "scripts/upgrade.opsis",
		"public class Package {\n"
		"    public void Upgrade() {\n"
		"        WriteFile(\"/usr/share/mark\", \"upgrade\");\n"
		"        RecordInstalled(\"tool\", \"2.0.0\", \"meta.yaml\");\n"
		"    }\n"
		"}\n");
	Options.output_path = (Root / "next.oaa").string();
	auto NextBuilt = okrapm::ArtifactBuilder::build(Next.string(), Options);
	assert(NextBuilt.has_value());
	assert(Core.install({*NextBuilt}).success);
	{
		std::ifstream Mark(Sysroot / "usr/share/mark");
		std::string Text;
		std::getline(Mark, Text);
		assert(Text == "upgrade");
	}
	assert(Core.info("app.tool")->version().to_string() == "2.0.0");

	unsetenv("LUNAR_INSTALL_ROOT");
	fs::remove_all(Root);
	std::cout << "[PASS] TestRemoveAndUpgrade\n";
}

std::string ReadLine(const fs::path &Path)
{
	std::ifstream Input(Path);
	std::string Text;
	std::getline(Input, Text);
	return Text;
}

void TestEntryContract()
{
	fs::path Root = fs::temp_directory_path() / "opsis-entries";
	fs::remove_all(Root);
	fs::path Sys = Root / "sys";
	fs::create_directories(Sys);
	WriteFile(Root / "lib/note.opsis",
		"public class Note {\n"
		"    public void Mark() {\n"
		"        WriteFile(\"/var/note\", \"helper\");\n"
		"    }\n"
		"}\n");
	WriteFile(Root / "package.opsis",
		"import \"lib/note.opsis\";\n"
		"public class DatabaseInitializer {\n"
		"    public void Prepare() {\n"
		"        WriteFile(\"/var/db-ready\", \"yes\");\n"
		"    }\n"
		"}\n"
		"public class Package {\n"
		"    public int MAIN(BuildContext ctx) {\n"
		"        WriteFile(\"/var/scene\", \"main\");\n"
		"        return 0;\n"
		"    }\n"
		"    public int INSTALL(InstallContext ctx) {\n"
		"        DatabaseInitializer.Prepare();\n"
		"        Note.Mark();\n"
		"        WriteFile(\"/var/scene\", \"install\");\n"
		"        return 0;\n"
		"    }\n"
		"    public int UPDATE(UpdateContext ctx) {\n"
		"        if (Runtime.DefaultUpdate(ctx) != 0) return 1;\n"
		"        WriteFile(\"/var/scene\", ctx.OldVersion + \"->\" + ctx.NewVersion);\n"
		"        return 0;\n"
		"    }\n"
		"    public int REMOVE(RemoveContext ctx) {\n"
		"        WriteFile(\"/var/scene\", \"remove\");\n"
		"        return 0;\n"
		"    }\n"
		"}\n");

	auto RunScene = [&](const std::string &Entry, const std::string &Old, const std::string &New) {
		Opsis::RuntimeConfig Config = TestConfig(Sys);
		Config.EntryName = Entry;
		Config.BaseDirectory = Root.string();
		Config.OldVersion = Old;
		Config.NewVersion = New;
		Config.PackageNamespace = "Okra";
		Config.PackageName = "postgres";
		Config.PackageVersion = New.empty() ? Old : New;
		Config.Reason = Entry;
		return Opsis::RunFile((Root / "package.opsis").string(), Config);
	};
	assert(RunScene("MAIN", "", "1.0.0") == 0);
	assert(ReadLine(Sys / "var/scene") == "main");
	assert(RunScene("INSTALL", "", "1.0.0") == 0);
	assert(ReadLine(Sys / "var/scene") == "install");
	assert(ReadLine(Sys / "var/note") == "helper");
	assert(ReadLine(Sys / "var/db-ready") == "yes");
	assert(RunScene("UPDATE", "2.0.0", "3.0.0") == 0);
	assert(ReadLine(Sys / "var/scene") == "2.0.0->3.0.0");
	assert(RunScene("REMOVE", "3.0.0", "") == 0);
	assert(ReadLine(Sys / "var/scene") == "remove");
	WriteFile(Sys / "var/scene", "keep\n");
	assert(RunScene("VERIFY", "", "") == 0);
	assert(ReadLine(Sys / "var/scene") == "keep");
	assert(Opsis::RunSource(
		"public class Package {\n"
		"    public int INSTALL(InstallContext ctx) { return 7; }\n"
		"}\n",
		"bad-entry.opsis", TestConfig(Sys)) == 1);

	fs::path Package = Root / "pkg";
	fs::path Sysroot = Root / "sysroot";
	fs::path Data = Root / "lunar";
	fs::create_directories(Sysroot);
	WriteFile(Package / "files/usr/bin/tool", "tool-ok\n");
	WriteFile(Package / "meta.yaml",
		"name: widget\nnamespace: app\nversion: 1.0.0\narchitecture: x86_64\ndescription: \"widget\"\n");
	WriteFile(Package / "lib/note.opsis",
		"public class Note {\n"
		"    public void Mark() { WriteFile(\"/var/note\", \"from-package\"); }\n"
		"}\n");
	WriteFile(Package / "package.opsis",
		"import \"lib/note.opsis\";\n"
		"public class Package {\n"
		"    public int MAIN(BuildContext ctx) { Fail(\"main-on-install\"); return 1; }\n"
		"    public int INSTALL(InstallContext ctx) {\n"
		"        Note.Mark();\n"
		"        WriteFile(\"/usr/share/scene\", \"install \" + ctx.Version);\n"
		"        return 0;\n"
		"    }\n"
		"    public int UPDATE(UpdateContext ctx) {\n"
		"        WriteFile(\"/usr/share/scene\", ctx.OldPackage + \" => \" + ctx.NewPackage);\n"
		"        return 0;\n"
		"    }\n"
		"    public int REMOVE(RemoveContext ctx) {\n"
		"        RemoveFile(\"/usr/bin/tool\");\n"
		"        WriteFile(\"/var/removed\", ctx.OldVersion);\n"
		"        return 0;\n"
		"    }\n"
		"}\n");
	WriteFile(Package / "scripts/install.opsis",
		"public class Legacy {\n"
		"    public void INSTALL() { Fail(\"file-is-not-the-entry\"); }\n"
		"}\n");

	okrapm::ArtifactBuilder::BuildOptions Options;
	Options.compression = "gzip";
	Options.output_path = (Root / "widget.oaa").string();
	auto Built = okrapm::ArtifactBuilder::build(Package.string(), Options);
	assert(Built.has_value());
	unsetenv("OPSIS_DB_DIR");
	setenv("LUNAR_INSTALL_ROOT", Sysroot.c_str(), 1);
	okrapm::LunarCore Core(Data.string());
	assert(Core.install({*Built}).success);
	assert(ReadLine(Sysroot / "usr/share/scene") == "install 1.0.0");
	assert(ReadLine(Sysroot / "var/note") == "from-package");

	fs::path Next = Root / "next";
	WriteFile(Next / "files/usr/bin/tool", "tool-ok\n");
	WriteFile(Next / "meta.yaml",
		"name: widget\nnamespace: app\nversion: 2.0.0\narchitecture: x86_64\ndescription: \"widget\"\n");
	WriteFile(Next / "package.opsis",
		"public class Package {\n"
		"    public int UPDATE(UpdateContext ctx) {\n"
		"        WriteFile(\"/usr/share/scene\", ctx.OldVersion + \"->\" + ctx.NewVersion);\n"
		"        return 0;\n"
		"    }\n"
		"    public int INSTALL(InstallContext ctx) { Fail(\"install-on-update\"); return 1; }\n"
		"    public int REMOVE(RemoveContext ctx) {\n"
		"        RemoveFile(\"/usr/bin/tool\");\n"
		"        WriteFile(\"/var/removed\", ctx.OldVersion);\n"
		"        return 0;\n"
		"    }\n"
		"}\n");
	Options.output_path = (Root / "next.oaa").string();
	auto NextBuilt = okrapm::ArtifactBuilder::build(Next.string(), Options);
	assert(NextBuilt.has_value());
	assert(Core.install({*NextBuilt}).success);
	assert(ReadLine(Sysroot / "usr/share/scene") == "1.0.0->2.0.0");
	assert(Core.remove({"app.widget"}).success);
	assert(!fs::exists(Sysroot / "usr/bin/tool"));
	assert(ReadLine(Sysroot / "var/removed") == "2.0.0");
	assert(!Core.info("app.widget").has_value());

	unsetenv("LUNAR_INSTALL_ROOT");
	fs::remove_all(Root);
	std::cout << "[PASS] TestEntryContract\n";
}

void TestLanguageForms()
{
	fs::path Root = fs::temp_directory_path() / "opsis-language";
	fs::remove_all(Root);
	fs::path Sys = Root / "sys";
	fs::create_directories(Sys);
	std::string Program =
		"public abstract class PackageLifecycle {\n"
		"    private string Mark = \"/var/mark\";\n"
		"    public virtual int INSTALL(InstallContext ctx) {\n"
		"        WriteFile(Mark, \"default-install\");\n"
		"        return Runtime.DefaultInstall(ctx);\n"
		"    }\n"
		"    public virtual int UPDATE(UpdateContext ctx) {\n"
		"        return Runtime.DefaultUpdate(ctx);\n"
		"    }\n"
		"}\n"
		"public class Package : PackageLifecycle {\n"
		"    private string Prefix = \"/usr\";\n"
		"    public override int UPDATE(UpdateContext ctx) {\n"
		"        if (ctx.OldVersion.Major == 1) {\n"
		"            throw new LegacyDatabaseException(ctx.OldVersion);\n"
		"        }\n"
		"        switch (ctx.OldVersion.Major) {\n"
		"            case 2:\n"
		"                WriteFile(Prefix + \"/share/step\", \"v2\");\n"
		"                goto case 3;\n"
		"            case 3:\n"
		"                WriteFile(Mark, ctx.OldVersion + \"->\" + ctx.NewVersion);\n"
		"                break;\n"
		"            default:\n"
		"                WriteFile(Mark, \"other\");\n"
		"                break;\n"
		"        }\n"
		"        return 0;\n"
		"    }\n"
		"}\n";
	auto RunScene = [&](const std::string &Entry, const std::string &Old, const std::string &New) {
		Opsis::RuntimeConfig Config = TestConfig(Sys);
		Config.EntryName = Entry;
		Config.OldVersion = Old;
		Config.NewVersion = New;
		Config.PackageVersion = New.empty() ? Old : New;
		return Opsis::RunSource(Program, "lifecycle.opsis", Config);
	};
	assert(RunScene("INSTALL", "", "1.0.0") == 0);
	assert(ReadLine(Sys / "var/mark") == "default-install");
	assert(RunScene("UPDATE", "2.0.0", "3.0.0") == 0);
	assert(ReadLine(Sys / "usr/share/step") == "v2");
	assert(ReadLine(Sys / "var/mark") == "2.0.0->3.0.0");
	assert(RunScene("UPDATE", "9.1.0", "9.2.0") == 0);
	assert(ReadLine(Sys / "var/mark") == "other");
	assert(RunScene("UPDATE", "1.4.0", "2.0.0") == 1);

	Opsis::RuntimeConfig Config = TestConfig(Sys);
	assert(Opsis::RunSource(
		"public class Package {\n"
		"    private string Prefix = \"/usr\";\n"
		"    private void Shift() { Prefix = \"/opt\"; }\n"
		"    public int INSTALL(InstallContext ctx) {\n"
		"        Shift();\n"
		"        WriteFile(\"/var/prefix\", Prefix);\n"
		"        return 0;\n"
		"    }\n"
		"}\n",
		"fields.opsis", Config) == 0);
	assert(ReadLine(Sys / "var/prefix") == "/opt");

	assert(Opsis::RunSource(
		"public class Package {\n"
		"    public int INSTALL(InstallContext ctx) {\n"
		"        switch (1) {\n"
		"            case 1:\n"
		"            case 2:\n"
		"                WriteFile(\"/var/stack\", \"two\");\n"
		"                break;\n"
		"        }\n"
		"        return 0;\n"
		"    }\n"
		"}\n",
		"stack.opsis", Config) == 0);
	assert(ReadLine(Sys / "var/stack") == "two");

	assert(Opsis::RunSource(
		"public class Package {\n"
		"    public int INSTALL(InstallContext ctx) {\n"
		"        switch (1) {\n"
		"            case 1:\n"
		"                WriteFile(\"/var/fell\", \"yes\");\n"
		"            case 2:\n"
		"                break;\n"
		"        }\n"
		"        return 0;\n"
		"    }\n"
		"}\n",
		"fall.opsis", Config) == 2);

	assert(Opsis::RunSource(
		"public class Base {\n"
		"    public int INSTALL(InstallContext ctx) { return 0; }\n"
		"}\n"
		"public class Package : Base {\n"
		"    public int INSTALL(InstallContext ctx) { return 0; }\n"
		"}\n",
		"hide.opsis", Config) == 2);

	assert(Opsis::RunSource(
		"public class Package {\n"
		"    public void INSTALL() { goto case 1; }\n"
		"}\n",
		"goto.opsis", Config) == 2);

	fs::remove_all(Root);
	std::cout << "[PASS] TestLanguageForms\n";
}

void TestTransactionRollback()
{
	fs::path Root = fs::temp_directory_path() / "opsis-rollback";
	fs::remove_all(Root);
	fs::path Sysroot = Root / "sysroot";
	fs::path Data = Root / "lunar";
	fs::create_directories(Sysroot);
	fs::path Package = Root / "pkg";
	WriteFile(Package / "files/usr/bin/tool", "old\n");
	WriteFile(Package / "files/usr/share/keep", "stay\n");
	WriteFile(Package / "meta.yaml",
		"name: tool\nnamespace: app\nversion: 1.0.0\ndescription: \"tool\"\n");
	WriteFile(Package / "scripts/install.opsis",
		"public class Package {\n"
		"    public void INSTALL() { WriteFile(\"/usr/share/arch\", Context.Architecture); }\n"
		"}\n");
	WriteFile(Package / "scripts/remove.opsis",
		"public class Package {\n"
		"    public void REMOVE() { WriteFile(\"/var/gone\", \"v1-remove\"); }\n"
		"}\n");
	okrapm::ArtifactBuilder::BuildOptions Options;
	Options.compression = "gzip";
	Options.output_path = (Root / "tool.oaa").string();
	auto Built = okrapm::ArtifactBuilder::build(Package.string(), Options);
	assert(Built.has_value());
	unsetenv("OPSIS_DB_DIR");
	setenv("LUNAR_INSTALL_ROOT", Sysroot.c_str(), 1);
	okrapm::LunarCore Core(Data.string());
	assert(Core.install({*Built}).success);
	assert(ReadLine(Sysroot / "usr/bin/tool") == "old");
	assert(ReadLine(Sysroot / "usr/share/arch") == Opsis::HostArchitecture());
	assert(fs::is_regular_file(Data / "pkg-scripts/app/tool/scripts/remove.opsis"));

	fs::path Next = Root / "next";
	WriteFile(Next / "files/usr/bin/tool", "new\n");
	WriteFile(Next / "files/usr/bin/extra", "extra\n");
	WriteFile(Next / "meta.yaml",
		"name: tool\nnamespace: app\nversion: 2.0.0\narchitecture: riscv64\ndescription: \"tool\"\n");
	WriteFile(Next / "scripts/upgrade.opsis",
		"public class Package {\n"
		"    public void UPDATE() { Fail(\"update-failed\"); }\n"
		"}\n");
	WriteFile(Next / "scripts/remove.opsis",
		"public class Package {\n"
		"    public void REMOVE() { WriteFile(\"/var/gone\", \"v2-remove\"); }\n"
		"}\n");
	Options.output_path = (Root / "next.oaa").string();
	auto NextBuilt = okrapm::ArtifactBuilder::build(Next.string(), Options);
	assert(NextBuilt.has_value());
	assert(!Core.install({*NextBuilt}).success);
	assert(ReadLine(Sysroot / "usr/bin/tool") == "old");
	assert(ReadLine(Sysroot / "usr/share/keep") == "stay");
	assert(!fs::exists(Sysroot / "usr/bin/extra"));
	assert(Core.info("app.tool")->version().to_string() == "1.0.0");
	{
		std::ifstream Script(Data / "pkg-scripts/app/tool/scripts/remove.opsis");
		std::string Text((std::istreambuf_iterator<char>(Script)), std::istreambuf_iterator<char>());
		assert(Text.find("v1-remove") != std::string::npos);
	}

	fs::path Saved = Root / "savedb";
	fs::copy(Data / "system.db", Saved);
	fs::remove(Data / "system.db");
	fs::create_directory(Data / "system.db");
	fs::path StoreBreak = Root / "store";
	WriteFile(StoreBreak / "files/usr/bin/tool", "new\n");
	WriteFile(StoreBreak / "files/usr/bin/extra", "extra\n");
	WriteFile(StoreBreak / "meta.yaml",
		"name: tool\nnamespace: app\nversion: 2.0.0\ndescription: \"tool\"\n");
	WriteFile(StoreBreak / "scripts/upgrade.opsis",
		"public class Package {\n"
		"    public void UPDATE() { WriteFile(\"/usr/share/marker\", \"ran\"); }\n"
		"}\n");
	WriteFile(StoreBreak / "scripts/remove.opsis",
		"public class Package {\n"
		"    public void REMOVE() { WriteFile(\"/var/gone\", \"v2-remove\"); }\n"
		"}\n");
	Options.output_path = (Root / "store.oaa").string();
	auto StoreBuilt = okrapm::ArtifactBuilder::build(StoreBreak.string(), Options);
	assert(StoreBuilt.has_value());
	assert(!Core.install({*StoreBuilt}).success);
	assert(ReadLine(Sysroot / "usr/bin/tool") == "old");
	assert(!fs::exists(Sysroot / "usr/bin/extra"));
	{
		std::ifstream Script(Data / "pkg-scripts/app/tool/scripts/remove.opsis");
		std::string Text((std::istreambuf_iterator<char>(Script)), std::istreambuf_iterator<char>());
		assert(Text.find("v1-remove") != std::string::npos);
	}
	fs::remove_all(Data / "system.db");
	fs::copy(Saved, Data / "system.db");

	fs::path Dotted = Root / "dotted";
	WriteFile(Dotted / "files/usr/bin/dotted", "ok\n");
	WriteFile(Dotted / "meta.yaml",
		"name: foo..bar\nnamespace: app\nversion: 1.0.0\ndescription: \"dotted\"\n");
	WriteFile(Dotted / "scripts/install.opsis",
		"public class Package { public void INSTALL() {} }\n");
	Options.output_path = (Root / "dotted.oaa").string();
	auto DottedBuilt = okrapm::ArtifactBuilder::build(Dotted.string(), Options);
	assert(DottedBuilt.has_value());
	assert(Core.install({*DottedBuilt}).success);
	assert(fs::is_directory(Data / "pkg-scripts/app/foo..bar"));

	fs::path Hidden = Root / "hidden";
	WriteFile(Hidden / "files/usr/bin/hidden", "no\n");
	WriteFile(Hidden / "meta.yaml",
		"name: .hidden\nnamespace: app\nversion: 1.0.0\ndescription: \"hidden\"\n");
	Options.output_path = (Root / "hidden.oaa").string();
	auto HiddenBuilt = okrapm::ArtifactBuilder::build(Hidden.string(), Options);
	assert(HiddenBuilt.has_value());
	assert(!Core.install({*HiddenBuilt}).success);
	assert(!fs::exists(Sysroot / "usr/bin/hidden"));
	assert(!Core.info("app..hidden").has_value());

	fs::path Arch = Root / "arch";
	WriteFile(Arch / "meta.yaml",
		"name: archpkg\nnamespace: app\nversion: 1.0.0\narchitecture: riscv64\ndescription: \"arch\"\n");
	WriteFile(Arch / "scripts/install.opsis",
		"public class Package {\n"
		"    public void INSTALL() { WriteFile(\"/usr/share/declared\", Context.Architecture); }\n"
		"}\n");
	Options.output_path = (Root / "arch.oaa").string();
	auto ArchBuilt = okrapm::ArtifactBuilder::build(Arch.string(), Options);
	assert(ArchBuilt.has_value());
	assert(Core.install({*ArchBuilt}).success);
	assert(ReadLine(Sysroot / "usr/share/declared") == "riscv64");

	unsetenv("LUNAR_INSTALL_ROOT");
	fs::remove_all(Root);
	std::cout << "[PASS] TestTransactionRollback\n";
}

} // namespace

int main()
{
	TestInstallScript();
	TestFailureRollsNoRecord();
	TestEscapeAndSyntax();
	TestLunarCommitRunsOpsis();
	TestMoreSyntax();
	TestDirectPackage();
	TestRemoveAndUpgrade();
	TestEntryContract();
	TestLanguageForms();
	TestTransactionRollback();
	std::cout << "OPSIS tests passed\n";
	return 0;
}
