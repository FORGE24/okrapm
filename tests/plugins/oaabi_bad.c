#include "oaabi/oaabi.h"

#include <stdio.h>
#include <stdlib.h>

static int PluginInit(void)
{
	const char *Path = getenv("OAABI_MARK");
	if (!Path) return 0;
	FILE *File = fopen(Path, "w");
	if (!File) return 0;
	fputs("bad\n", File);
	fclose(File);
	return 0;
}

static void PluginFini(void)
{
}

const struct OaabiPlugin OaabiPlugin = {
	.Version = 2,
	.Reserved = 0,
	.Init = PluginInit,
	.Fini = PluginFini,
};
