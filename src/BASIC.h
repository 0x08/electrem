#ifndef __BASIC_H
#define __BASIC_H

/* SDL.h defines the Uin8 data type — it isn't used for anything else */
#include "SDL.h"

extern void SetupBASICTables();
extern bool ExportBASIC(char *Filename, Uint8 *Memory);
extern bool ImportBASIC(char *Filename, Uint8 *Memory);
extern char *GetBASICError();

#endif
