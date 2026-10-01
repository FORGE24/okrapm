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
		<< "\n"
		<< "Interpret a C#-like OPSIS install script.\n"
		<< "OPSIS_SYSROOT, OPSIS_DB_DIR and OPSIS_ALLOW_NONROOT still apply.\n";
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
