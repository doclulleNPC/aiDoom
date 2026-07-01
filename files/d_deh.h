// DeHackEd / BEX / MBF21 patch support (ported from ../winmbf, adapted to aiDoom).
#ifndef __D_DEH__
#define __D_DEH__
void ProcessDehFile(char *filename, char *outfilename, int lumpnum);
void D_ProcessDehInWads(void);   // apply every DEHACKED lump + any -deh <file> args
#endif
