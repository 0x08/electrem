#ifndef __PLUS1_H
#define __PLUS1_H

#include <stdio.h>

#include "../ComponentBase.h"
#include "../ProcessPool.h"

#define PLUS1_PRINTBUFFERSIZE	256

class CPlus1: public CComponentBase
{
	public:
		CPlus1();
		virtual ~CPlus1();

		void AttachTo(CProcessPool &pool, Uint32 id);
		bool Write(Uint16 Addr, Uint32 TimeStamp, Uint8 Data8, Uint32 Data32);
		bool Read(Uint16 Addr, Uint32 TimeStamp, Uint8 &Data8, Uint32 &Data32);
		Uint32 Update(Uint32 TotalCycles, bool Catchup);

		/* functions for pointing the Plus 1 towards a target for printer output */
		bool SetPrinterTarget(char *fname);
		void CloseFile();

	private:
		/* dumb function for adding a character to the printer output, called by Write */
		void PrintChar(char c);

		/* printer internal stuff */
		void FlushPrintBuffer();
		char PrintBuffer[PLUS1_PRINTBUFFERSIZE];
		unsigned int PrinterBufferPointer;

		FILE *PrinterTarget;
		bool RawPrinterFeed, ESCChar, UnderlineOn;
		SDL_mutex *PrinterFileMutex;

		/* ADC internal stuff */
		Uint32 ADCCyclesLeft;
		Uint8 ADCValue;
		Uint8 GetADCChannel(int channel);
		Uint8 GetADCButtons();
};

#endif
