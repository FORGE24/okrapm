#include <fstream>
#include <cstdlib>

namespace okrapm {
class ExtensionApi;
}

extern "C" bool lunar_plugin_init(okrapm::ExtensionApi *)
{
	const char *Path = std::getenv("LEGACY_PLUGIN_MARK");
	if (!Path) return false;
	std::ofstream Output(Path);
	if (!Output) return false;
	Output << "legacy\n";
	return true;
}
