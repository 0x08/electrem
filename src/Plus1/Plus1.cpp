/*

	ElectrEm (c) 2000-6 Thomas Harte - an Acorn Electron Emulator

	This is open software, distributed under the GPL 2, see 'Copying' for
	details

	Plus1.cpp
	=========

	Contains what is emulated, so far, of the Plus 1. Mostly just an
	Epson FX -> RTF filter.

*/

#include "Plus1.h"
#include "../HostMachine/HostMachine.h"

CPlus1::CPlus1()
{
	PrinterTarget = NULL;
	PrinterFileMutex = SDL_CreateMutex();
	SDL_mutexP(PrinterFileMutex);
	SDL_mutexV(PrinterFileMutex);
	PrinterBufferPointer = 0;
	ADCCyclesLeft = 0;
	ADCValue = 128;
}

CPlus1::~CPlus1()
{
	CloseFile();
	SDL_DestroyMutex(PrinterFileMutex);
}

void CPlus1::AttachTo(CProcessPool &pool, Uint32 id)
{
	CComponentBase::AttachTo(pool, id);

	// claim Plus 1 status register
	pool.ClaimTrapAddress(id, 0xfc72, 0xffff);

	// centronics interface register
	pool.ClaimTrapAddress(id, 0xfc71, 0xffff);

	// ADC (joysticks) interface register
	pool.ClaimTrapAddress(id, 0xfc70, 0xffff);
}

bool CPlus1::Write(Uint16 Addr, Uint32 TimeStamp, Uint8 Data8, Uint32 Data32)
{
	switch(Addr)
	{
		case 0xfc71:
			PrintChar(Data8);
		return true;
		case 0xfc70:
			/* get ADCValue here */
			switch(Data8&0xc)
			{
				case 0x8:
				case 0:
					//differential
					if(Data8&1)
						ADCValue = GetADCChannel((Data8&2) | 1) - GetADCChannel(Data8&2);
					else
						ADCValue = GetADCChannel(Data8&2) - GetADCChannel((Data8&2) | 1);
				break;

				case 0x04:
					ADCValue = GetADCChannel(Data8&3);
				break;

				case 0xc:
					ADCValue = GetADCChannel(Data8&3) - GetADCChannel(3);
				break;
			}
			
			/* actually won't be ready for 40 us... which is 80 cycles on the 2 Mhz bus*/
			ADCCyclesLeft = 80;
		return true;
	}

	return false;
}

Uint8 CPlus1::GetADCChannel(int channel)
{
	return 128;
}

Uint8 CPlus1::GetADCButtons()
{
	return 0x30;	/* active low! */
}

Uint32 CPlus1::Update(Uint32 TotalCycles, bool Catchup)
{
	if(ADCCyclesLeft > TotalCycles)
		ADCCyclesLeft -= TotalCycles;
	else
		ADCCyclesLeft = 0;

	return CYCLENO_ANY;
}

bool CPlus1::Read(Uint16 Addr, Uint32 TimeStamp, Uint8 &Data8, Uint32 &Data32)
{
	switch(Addr)
	{
		case 0xfc72:
			/* status register */
			Data8 = 0x3f | (ADCCyclesLeft ? 0x40 : 0);	/* printer is free, no fire buttons pressed, check ADC */
		return true;
		case 0xfc70:
			Data8 = ADCValue;
		return true;
	}
	return false;
}

/* printer output functions */
bool CPlus1::SetPrinterTarget(char *fname)
{
	CloseFile();

	/* get file access */
	SDL_mutexP(PrinterFileMutex);

	if(PrinterTarget = fopen(fname, "wb"))
	{
		/* decide whether this is an RTF or not */
		char *RTFExts[] = { "rtf", NULL };
		RawPrinterFeed = !GetHost() -> ExtensionIncluded(fname, RTFExts);

		if(!RawPrinterFeed)
		{
			/* open RTF */
			fprintf(PrinterTarget, "{\\rtf1 {\\fonttbl\\f0\\fmodern\\fcharset77 Courier;} \\f0 ");
			ESCChar = false;
			UnderlineOn = false;
		}
	}

	/* release file access */
	SDL_mutexV(PrinterFileMutex);

	return PrinterTarget ? true : false;
}

void CPlus1::CloseFile()
{
	FlushPrintBuffer();

	/* get file access */
	SDL_mutexP(PrinterFileMutex);

	if(PrinterTarget)
	{
		if(!RawPrinterFeed)
			fputc('}', PrinterTarget);

		fclose(PrinterTarget);
		PrinterTarget = NULL;
	}

	/* release file access */
	SDL_mutexV(PrinterFileMutex);
}

void CPlus1::PrintChar(char c)
{
	PrintBuffer[PrinterBufferPointer] = c;
	PrinterBufferPointer++;
	if(PrinterBufferPointer == PLUS1_PRINTBUFFERSIZE)
		FlushPrintBuffer();
}

void CPlus1::FlushPrintBuffer()
{
	if(PrinterBufferPointer)
	{
		/* get file access */
		SDL_mutexP(PrinterFileMutex);
		
		if(PrinterTarget)
		{
			if(RawPrinterFeed)
			{
				/* raw text output */
				fwrite(PrintBuffer, 1, PrinterBufferPointer, PrinterTarget);
			}
			else
			{
				/* RTF output */
				unsigned int c = 0;
				while(c < PrinterBufferPointer)
				{
					if(ESCChar)
					{
						//escape character...
						switch(PrintBuffer[c])
						{
							default: break;
							case 'E': fputs("\\b ", PrinterTarget);		break; // bold on 
							case 'F': fputs("\\b0 ", PrinterTarget);	break; // bold off
							case '4': fputs("\\i ", PrinterTarget);		break; // italics on 
							case '5': fputs("\\i0 ", PrinterTarget);	break; // italics off
							case '-':
								if(UnderlineOn)
									fputs("\\ul0 ", PrinterTarget);
								else
									fputs("\\ul ", PrinterTarget);
								UnderlineOn ^= true;
							break; // underline on/off toggle
							
							case 'M': break; //12 cpi - so font size = 12 points
							case 'P': break; //10 cpi - so font size = 
							case 'g': break; //15 cpi - so font size = 
							
//							case 0x53: break; //superscript
//							case 
						}
						ESCChar = false;
					}
					else
						switch(PrintBuffer[c])
						{
							default: fputc(PrintBuffer[c], PrinterTarget); break;
							case 0xc: fputs("\\page ", PrinterTarget);	break; // form feed (-> page break)
							case '\r': fputs("\\line ", PrinterTarget); break;
							case '\\': fputs("\\\\", PrinterTarget); break;
							case '{': fputs("\\{", PrinterTarget); break;
							case '}': fputs("\\}", PrinterTarget); break;
							case 0x27: ESCChar = true; break;
						}
					c++;
				}
//				fwrite(PrintBuffer, 1, PrinterBufferPointer, PrinterTarget);
			}
		}

		/* release file access */
		SDL_mutexV(PrinterFileMutex);
		
		PrinterBufferPointer = 0;
	}
}
