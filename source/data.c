#include <stdio.h>
#include <string.h>
#include "data.h"

char dataDir[40] = "romfs:/";

void dataInit(void)
{
	FILE* f = fopen(SD_DATA "index.bin", "rb");
	if (f) { fclose(f); snprintf(dataDir, sizeof dataDir, "%s", SD_DATA); }
}

const char* dataPath(const char* name)
{
	static char buf[4][128];
	static int k;
	char* b = buf[k++ & 3];
	snprintf(b, sizeof buf[0], "%s%s", dataDir, name);
	return b;
}
