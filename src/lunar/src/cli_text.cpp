#include "cli_text.h"

#include <iostream>
#include <unistd.h>

namespace {

bool StdoutIsTerminal()
{
	return isatty(STDOUT_FILENO) == 1;
}

bool StderrIsTerminal()
{
	return isatty(STDERR_FILENO) == 1;
}

std::string Fit(const std::string &Text, std::size_t Limit)
{
	if (Text.size() <= Limit) return Text;
	if (Limit <= 2) return Text.substr(0, Limit);
	return Text.substr(0, Limit - 2) + "..";
}

std::string Pad(const std::string &Text, std::size_t Width)
{
	if (Text.size() >= Width) return Text;
	return Text + std::string(Width - Text.size(), ' ');
}

void Paint(std::ostream &Out, const char *Code, const std::string &Text, bool Enabled)
{
	if (Enabled) Out << "\033[" << Code << "m" << Text << "\033[0m";
	else Out << Text;
}

} // namespace

namespace Cli {

void Error(const std::string &Message)
{
	Paint(std::cerr, "31", "error", StderrIsTerminal());
	std::cerr << "  " << Message << "\n";
}

void TaskBegin(const std::string &Verb, const std::string &Subject)
{
	Paint(std::cout, "1", Verb, StdoutIsTerminal());
	if (!Subject.empty()) std::cout << "  " << Subject;
	std::cout << "\n";
}

void TaskOk(const std::string &Detail)
{
	Paint(std::cout, "32", "ok", StdoutIsTerminal());
	std::cout << "  " << Detail << "\n";
}

void TaskFail(const std::string &Detail)
{
	Paint(std::cerr, "31", "error", StderrIsTerminal());
	std::cerr << "  " << Detail << "\n";
}

void PrintTable(const std::vector<std::string> &Headers,
	const std::vector<std::vector<std::string>> &Rows, int Indent)
{
	const std::size_t DescriptionLimit = 48;
	std::vector<std::size_t> Widths(Headers.size(), 0);
	for (std::size_t Column = 0; Column < Headers.size(); ++Column) {
		Widths[Column] = Headers[Column].size();
	}
	std::vector<std::vector<std::string>> Fitted;
	Fitted.reserve(Rows.size());
	for (const auto &Row : Rows) {
		std::vector<std::string> Cells;
		for (std::size_t Column = 0; Column < Headers.size(); ++Column) {
			std::string Cell = Column < Row.size() ? Row[Column] : "";
			if (Headers[Column] == "DESCRIPTION") Cell = Fit(Cell, DescriptionLimit);
			if (Cell.size() > Widths[Column]) Widths[Column] = Cell.size();
			Cells.push_back(std::move(Cell));
		}
		Fitted.push_back(std::move(Cells));
	}

	std::string Gap(Indent, ' ');
	auto WriteRow = [&](const std::vector<std::string> &Cells, bool Header) {
		std::cout << Gap;
		for (std::size_t Column = 0; Column < Cells.size(); ++Column) {
			if (Column) std::cout << "  ";
			std::string Text = (Column + 1 == Cells.size()) ? Cells[Column] : Pad(Cells[Column], Widths[Column]);
			if (Header) Paint(std::cout, "2", Text, StdoutIsTerminal());
			else std::cout << Text;
		}
		std::cout << "\n";
	};

	WriteRow(Headers, true);
	if (Fitted.empty()) {
		std::cout << Gap << "(none)\n";
		return;
	}
	for (const auto &Row : Fitted) WriteRow(Row, false);
}

void PrintFields(const std::vector<std::pair<std::string, std::string>> &Rows)
{
	std::vector<std::string> Headers{"FIELD", "VALUE"};
	std::vector<std::vector<std::string>> Body;
	for (const auto &Row : Rows) Body.push_back({Row.first, Row.second});
	PrintTable(Headers, Body, 0);
}

void PrintOperations(const okrapm::Transaction &Txn)
{
	if (Txn.operations().empty()) return;
	std::vector<std::vector<std::string>> Rows;
	for (const auto &Op : Txn.operations()) {
		Rows.push_back({
			std::string(1, Op.prefix()),
			Op.target().full_name(),
			Op.target().version().to_string()
		});
	}
	PrintTable({"ACTION", "OBJECT", "VERSION"}, Rows, 2);
}

void Help()
{
	TaskBegin("lunar");
	std::cout << "\n";
	PrintTable({"COMMAND", "PURPOSE"}, {
		{"install <ref...>", "install packages, groups, or .oaa files"},
		{"install-group <name>", "install a group"},
		{"remove <ref...>", "remove packages"},
		{"purge <ref...>", "remove packages and their configuration"},
		{"update [ref...]", "rolling update"},
		{"upgradle <target>", "shift the system baseline"},
		{"sync [repo]", "sync repositories"},
		{"download <ref...>", "download artifacts without installing"},
		{"plan <cmd> <ref>", "show the task table without applying it"},
		{"build <dir>", "build an .oaa from a directory"},
		{"verify <file>", "check an artifact checksum"},
		{"artifact inspect <file>", "show artifact fields"},
		{"artifact extract <f> <d>", "extract an artifact"},
		{"list", "table of installed objects"},
		{"find <pattern>", "find objects by pattern"},
		{"search <query>", "search objects by keyword"},
		{"info <ref>", "fields for one object"},
		{"status", "system state fields"},
		{"members <#group>", "table of group members"},
		{"repo list", "table of repositories"},
		{"repo add <name> <url>", "add a repository"},
		{"repo remove <name>", "remove a repository"},
		{"repo enable <name>", "enable a repository"},
		{"repo disable <name>", "disable a repository"},
		{"snapshot list", "table of snapshots"},
		{"snapshot create [desc]", "create a snapshot"},
		{"rollback [id]", "restore a snapshot"},
		{"transaction list", "table of recent transactions"},
		{"transaction show <id>", "operations inside one transaction"},
		{"ext list", "table of extensions"},
		{"ext load <path>", "load a plugin"},
		{"ext run <name>", "run an extension"},
		{"pipe '<expr>'", "run an object pipeline"},
		{"-r, --root <dir>", "state directory, default /var/lib/lunar"}
	});
}

} // namespace Cli
