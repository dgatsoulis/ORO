// ringprofile_test.cpp - build OroRingProfile.cpp + OroDDS.cpp OUTSIDE Orbiter and dump
// the derived profile, so the C++ port can be diffed against tools/ringprofile.py (the
// spec) before anything reaches the sim. Run tools\ringprofile_test\run.cmd.
//
//   ringprofile_test <texRoot> <Body> <Rkm> <irad> <orad> <n|0> <override 0|1> <out.txt>
//
// Output: one header line, then one line per texel "i bright tau rgba", the format
// ringprofile.py --compare reads.

#include "../../OroRingProfile.h"
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char** argv)
{
	if (argc != 9) { fprintf(stderr, "usage: %s texRoot Body Rkm irad orad n override out.txt\n", argv[0]); return 2; }
	const char* texRoot = argv[1];
	const char* body    = argv[2];
	const double Rkm    = atof(argv[3]);
	const double irad   = atof(argv[4]);
	const double orad   = atof(argv[5]);
	const int    n      = atoi(argv[6]);
	const bool   ovr    = atoi(argv[7]) != 0;

	OroRingProfile p;
	if (!OroRingProfile_Derive(texRoot, body, Rkm, irad, orad, n, ovr, &p)) {
		fprintf(stderr, "derive failed for %s\n", body); return 1;
	}
	FILE* f = NULL;
	if (fopen_s(&f, argv[8], "w") || !f) { fprintf(stderr, "cannot write %s\n", argv[8]); return 1; }
	fprintf(f, "# body=%s n=%d Rkm=%.6f irad=%.9f orad=%.9f override=%d srcB=%s srcT=%s\n",
	        body, p.n, p.Rkm, p.irad, p.orad, p.fromOverride ? 1 : 0, p.srcBright, p.srcTau);
	for (int i = 0; i < p.n; i++)
		fprintf(f, "%d %.9f %.9f %08X\n", i, p.bright[i], p.tau[i], p.rgba[i]);
	fclose(f);
	printf("%s: %d texels, brightness <- %s, tau <- %s%s\n", body, p.n, p.srcBright, p.srcTau,
	       p.fromOverride ? " (override)" : "");
	OroRingProfile_Free(&p);
	return 0;
}
