#include "opsis/interpreter.h"

#include <iostream>
#include <string>

namespace {

/**
 * PrintUsage() - 把 opsis 的命令行用法写到标准错误。
 *
 * Return: 无。
 */
void PrintUsage()
{
	std::cerr << "Usage: opsis [--sysroot DIR] [--db DIR] [--allow-nonroot] <script.opsis>\n"
		<< "       opsis pack [--output FILE] [--namespace NS] [--name NAME]\n"
		<< "                  [--version VER] [--build-dir DIR] <script.opsis>\n"
		<< "\n"
		<< "Interpret a C#-like OPSIS script. pack builds a direct .oaa from compile products.\n"
		<< "OPSIS_SYSROOT, OPSIS_DB_DIR, OPSIS_ALLOW_NONROOT, OPSIS_PKG_* and OPSIS_BUILD_DIR apply.\n";
}

} // namespace

int main(int Argc, char **Argv)
{
	Opsis::RuntimeConfig Config = Opsis::LoadConfig();
	std::string Script;

	for (int Index = 1; Index < Argc; ++Index) {
		std::string Arg = Argv[Index];
		if (Arg == "-h" || Arg == "--help") {
			PrintUsage();
			return 0;
		}
		if (Arg == "--allow-nonroot") {
			Config.AllowNonRoot = true;
			continue;
		}
		if (Arg == "--sysroot" && Index + 1 < Argc) {
			Config.Sysroot = Argv[++Index];
			continue;
		}
		if (Arg == "--db" && Index + 1 < Argc) {
			Config.DatabaseDirectory = Argv[++Index];
			continue;
		}
		if (Arg == "--output" && Index + 1 < Argc) {
			Config.PackageOutput = Argv[++Index];
			continue;
		}
		if (Arg == "--namespace" && Index + 1 < Argc) {
			Config.PackageNamespace = Argv[++Index];
			continue;
		}
		if (Arg == "--name" && Index + 1 < Argc) {
			Config.PackageName = Argv[++Index];
			continue;
		}
		if (Arg == "--version" && Index + 1 < Argc) {
			Config.PackageVersion = Argv[++Index];
			continue;
		}
		if (Arg == "--description" && Index + 1 < Argc) {
			Config.PackageDescription = Argv[++Index];
			continue;
		}
		if (Arg == "--build-dir" && Index + 1 < Argc) {
			Config.BuildDirectory = Argv[++Index];
			continue;
		}
		if (Arg == "pack" && Script.empty()) {
			Config.PackOnly = true;
			Config.EntryName = "MAIN";
			continue;
		}
		if (!Arg.empty() && Arg[0] == '-') {
			std::cerr << "opsis: unknown option " << Arg << "\n";
			PrintUsage();
			return 2;
		}
		if (!Script.empty()) {
			std::cerr << "opsis: extra argument " << Arg << "\n";
			return 2;
		}
		Script = Arg;
	}

	if (Script.empty()) {
		PrintUsage();
		return 2;
	}
	return Opsis::RunFile(Script, Config);
}
