#include "oaabi/oaabi.h"

#include <stdio.h>
#include <stdlib.h>

static int PluginInit(void)
{
	const char *Path = getenv("OAABI_MARK");
	if (!Path) return -1;
	FILE *File = fopen(Path, "w");
	if (!File) return -1;
	fputs("init\n", File);
	fclose(File);
	return 0;
}

static void PluginFini(void)
{
	const char *Path = getenv("OAABI_MARK");
	if (!Path) return;
	FILE *File = fopen(Path, "a");
	if (!File) return;
	fputs("fini\n", File);
	fclose(File);
}

const struct OaabiPlugin OaabiPlugin = {
	.Version = OaabiVersion,
	.Reserved = 0,
	.Init = PluginInit,
	.Fini = PluginFini,
};
