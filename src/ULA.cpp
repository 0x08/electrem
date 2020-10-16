/*

	ElectrEm (c) 2000-6 Thomas Harte - an Acorn Electron Emulator

	This is open software, distributed under the GPL 2, see 'Copying' for
	details

	ULA.cpp
	=======

	Less complicated ULA functionality, including:

		- sound emulation
		- ROM paging

*/

#include "ULA.h"
#include "ProcessPool.h"
#include "6502.h"
#include "Display.h"
#include "Tape/Tape.h"
#include "HostMachine/HostMachine.h"

#define ROM_BASIC		8
#define ROM_OS			16
#define ROMAddress(s)	((s == 10) ? 8 : ((s > 10) ? (s-3) : s))

#define SetScratch(v) CPUPtr->SetRepeatedWritePage(v, ScratchAddr, 0x4000)

#include <memory.h>

bool CULA::IOCtl(Uint32 Control, void *Parameter, Uint32 TimeStamp)
{
	switch(Control)
	{
		case IOCTL_PAUSE: AudioMaskBackup = AudioMask; AudioMask = 0; return true;
		case IOCTL_UNPAUSE: AudioMask = AudioMaskBackup; return true;

		case IOCTL_SUPERRESET:
			/* set power on status line */
			Status = ULAIRQ_POWER | 0x80;
			StatusMask = 0;

		case IOCTL_RESET:
		return true;
		
		case IOCTL_SETCONFIG:
			/* take what makes sense of config, return false if there are any changes that can't be made now */
			Volume = ( (ElectronConfiguration *)Parameter)->Volume;
			LowSoundLevel = 128 - ((Volume+1) >> 1);
		return false;	/* IOCTL_SETCONFIG always returns the opposite! */
	}

	return CComponentBase::IOCtl(Control, Parameter, TimeStamp);
}

CULA::CULA(ElectronConfiguration &cfg)
{
	RomStates = 0;
	memset(KeyboardState, 0, 16);
	MRBMode = MRB_UNDEFINED;

	/* build tables for the three types of bus */
	OneMhzBus = new Uint32 *[312];
	TwoMhzBus = new Uint32 *[312];
	HaltingBus = new Uint32 *[312];

	OneMhzRow = new Uint32[128];
	TwoMhzRow = new Uint32[128];
	HaltingRow = new Uint32[128];

	int c = 128;
	while(c--)
	{
		TwoMhzRow[c] = 1;
		OneMhzRow[c] = 2 - (c&1);

		/* set halting for first 80 cycles, for now */
		if(c < 80)
			HaltingRow[c] = 80 - c;
		else
			HaltingRow[c] = OneMhzRow[c];
	}

	c = 312;
	while(c--)
	{
		OneMhzBus[c] = OneMhzRow;
		TwoMhzBus[c] = TwoMhzRow;
		HaltingBus[c] = (c >= 56) ? HaltingRow : OneMhzRow;
	}

	TotalTime = 0;

	/* keyboard */
	KeyProgram = NULL;

	/* initialise audio */
	SDL_AudioSpec WAudioSpec;

	WAudioSpec.freq = 31250;
	WAudioSpec.format = AUDIO_U8;
	WAudioSpec.channels = 1;
	WAudioSpec.samples = 2048;
	WAudioSpec.callback = AudioUpdateFunctionHelper;
	WAudioSpec.userdata = this;

	AudioWritePtr = AudioReadPtr = 0;
	AudioProcessTime = AudioMask = AudioMaskBackup = 0;
	AudioBuffer[CULA_AUDIOEVENT_LENGTH-1].ClockTime = 0;
	AudioReadPtrMutex = NULL;

	if(SDL_OpenAudio(&WAudioSpec, &AudioSpec) >= 0)
	{
		AudioEnabled = true;
		AudioReadPtrMutex = SDL_CreateMutex();

		/* to avoid using a GCC-specific way of declaring a 64bit constant */
		AudioNumerator = 0x7A12; //0x7a12 is 31250
		AudioNumerator <<= 32;
		AudioNumerator /= AudioSpec.freq;
		SDL_PauseAudio(0);
	}
	else
		AudioEnabled = false;

	/* initialise keyboard */
	SetDefaultKeyMap();
}

CULA::~CULA()
{
	delete[] OneMhzBus;
	delete[] TwoMhzBus;
	delete[] HaltingBus;
	delete[] OneMhzRow;
	delete[] TwoMhzRow;
	delete[] HaltingRow;

	SDL_CloseAudio();
	if(AudioReadPtrMutex) SDL_DestroyMutex(AudioReadPtrMutex);
}


/* audio */
void CULA::EnactAudioEvent()
{
	/* enact top event */
	switch(AudioBuffer[ AudioReadPtr ].Type)
	{
		case AudioEvent::NOP:							break;
		case AudioEvent::START:		AudioMask = 0xff;	break;
		case AudioEvent::STOP:		AudioMask = 0x00;	break;
		case AudioEvent::SET_DIVIDER:
		{
			Uint64 AInc = AudioNumerator / (Uint64)(AudioBuffer[ AudioReadPtr ].Value+1);

			/* cheat to filter out 'inaudible' frequencies */
			if(AInc > 0x40000000)
			{
				AudioInc = 0;
				AudioPtr = 0x80000000;
			}
			else
			{
				AudioInc = (Uint32)(AInc);
//				printf("[%d] %d -> %d\n", AudioBuffer[ AudioReadPtr ].ClockTime, AudioBuffer[ AudioReadPtr ].Value, AudioInc);
			}
		}
		break;

	}

	AudioReadPtr = (AudioReadPtr+1)&(CULA_AUDIOEVENT_LENGTH-1);
}

void CULA::AudioUpdateFunctionHelper(void *tptr, Uint8 *TargetBuffer, int TargetLength)
{
	((CULA *)tptr)->AudioUpdateFunction(NULL, TargetBuffer, TargetLength);
}

void CULA::AudioUpdateFunction(void *tptr, Uint8 *TargetBuffer, int TargetLength)
{
	SDL_mutexP(AudioReadPtrMutex);

	if(!AudioMask && (AudioReadPtr == AudioWritePtr))
	{
		/* if audio is disabled and there are no interesting events, then we know the outcome already */
		memset(TargetBuffer, LowSoundLevel, TargetLength);
		AudioProcessTime += TargetLength;
	}
	else
	{
		int SPtr = 0;
		
		/* check if we've run ahead */
		if(AudioProcessTime > AudioBuffer[ AudioReadPtr ].SampleDiff)
			AudioProcessTime = AudioBuffer[ AudioReadPtr ].SampleDiff;

		/* check if we're miles behind */
		if( AudioReadPtr != AudioWritePtr)
		{
			while(1)
			{
				Uint32 TimeDiff = AudioBuffer[ (AudioWritePtr-1)&(CULA_AUDIOEVENT_LENGTH-1) ].ClockTime - AudioBuffer[ AudioReadPtr ].ClockTime;
				if(TimeDiff < 125000) break;
				EnactAudioEvent();
			}
		}

		/* normal processing */
		while(SPtr < TargetLength)
		{
			while(
				( AudioReadPtr != AudioWritePtr) &&
				( AudioProcessTime >= AudioBuffer[ AudioReadPtr ].SampleDiff)
			)
			{
				AudioProcessTime -= AudioBuffer[ AudioReadPtr ].SampleDiff;
				EnactAudioEvent();
			}

			Uint32 SamplesToWrite;
			if(AudioReadPtr == AudioWritePtr)
			{
				SamplesToWrite = TargetLength-SPtr;
			}
			else
			{
				SamplesToWrite = AudioBuffer[ AudioReadPtr ].SampleDiff - AudioProcessTime;

				if(SamplesToWrite > (unsigned)(TargetLength-SPtr))
					SamplesToWrite = TargetLength-SPtr;
			}

			AudioProcessTime += SamplesToWrite;

			while(SamplesToWrite--)
			{
				TargetBuffer[SPtr] = (Uint8)(LowSoundLevel + (( ((AudioPtr&0x80000000) >> 31) * Volume )& AudioMask));
				AudioPtr += AudioInc;
				SPtr++;
			}
		}
	}

	SDL_mutexV(AudioReadPtrMutex);
}

void CULA::WriteAudioEvent(Uint32 TimeStamp)
{
	SDL_mutexP(AudioReadPtrMutex);

	AudioBuffer[AudioWritePtr].ClockTime = TimeStamp;

	Uint64 Difference = TimeStamp - AudioBuffer[(AudioWritePtr-1)&(CULA_AUDIOEVENT_LENGTH-1)].ClockTime;
	Uint64 SDDiff = Difference*(Uint64)AudioSpec.freq + AudioBuffer[(AudioWritePtr-1)&(CULA_AUDIOEVENT_LENGTH-1)].Remainder;

	AudioBuffer[AudioWritePtr].SampleDiff = (Uint32)(SDDiff / 1996800);
	AudioBuffer[AudioWritePtr].Remainder = (Uint32)(SDDiff % 1996800);

	AudioWritePtr = (AudioWritePtr+1)&(CULA_AUDIOEVENT_LENGTH-1);

	SDL_mutexV(AudioReadPtrMutex);
}

Uint32 CULA::Update(Uint32 t, bool)
{
	TotalTime += t;
	UpdateKeyTable();

	if(AudioEnabled)
	{
		if(AudioReadPtr == AudioWritePtr)
		{
			/* generate NOP event to keep sound system alert and awake if it has been four seconds since an event */
			Uint32 BigDiff = TotalTime - AudioBuffer[(AudioWritePtr-1)&(CULA_AUDIOEVENT_LENGTH-1)].ClockTime;

			if(BigDiff > 8000000)
			{
				AudioBuffer[AudioWritePtr].Type = AudioEvent::NOP;
				WriteAudioEvent(TotalTime);
			}
		}
	}

	return CYCLENO_ANY;
}

void CULA::SetROMMode(int slot, int mode)
{
	/* map to physical slots */
//	slot = PhysicalROMSlot(slot);

	slot <<= 1;
	RomStates &= ~(3 << slot);
	RomStates |= mode << slot;
}

int CULA::GetROMMode(int slot)
{
//	slot = PhysicalROMSlot(slot);
	slot <<= 1;
	return (RomStates >> slot)&3;
}

void CULA::SetULARAMTiming(bool Halting)
{
	MemHalting = Halting;
	((C6502 *)PPPtr->GetWellDefinedComponent(COMPONENT_CPU))->SetMemoryLayout(0);

	switch(MRBMode)
	{
		default: 
		break;

		case MRB_OFF:
			((C6502 *)PPPtr->GetWellDefinedComponent(COMPONENT_CPU))->SetExecCyclePage(0x0000, Halting ? HaltingBus : OneMhzBus, 0x8000);
		break;
		
		case MRB_TURBO:
			((C6502 *)PPPtr->GetWellDefinedComponent(COMPONENT_CPU))->SetExecCyclePage(0x4000, Halting ? HaltingBus : OneMhzBus, 0x4000);
		break;

		case MRB_SHADOW:
			((C6502 *)PPPtr->GetWellDefinedComponent(COMPONENT_CPU))->SetExecCyclePage(0x3000, Halting ? HaltingBus : OneMhzBus, 0x5000);
		break;
	}
}

#define DoPage()\
	CPUPtr->SetReadPage(TargAddr, SrcAddr, Length);\
	if(ReadOnly)\
		SetScratch(TargAddr);\
	else\
		CPUPtr->SetWritePage(TargAddr, SrcAddr, Length);

void CULA::Page(Uint16 TargAddr, Uint32 SrcAddr, Uint16 Length, bool ReadOnly)
{
	C6502 *CPUPtr = (C6502 *)PPPtr->GetWellDefinedComponent(COMPONENT_CPU);

	CPUPtr->SetMemoryLayout(0);
	DoPage()

	if(MRBMode == MRB_SHADOW)
	{
		CPUPtr->SetMemoryLayout(1);
		DoPage()

		CPUPtr->SetMemoryLayout(2);
		DoPage()
	}
}

void CULA::SetMemoryModel(MRBModes Mode)
{
	if(Mode == MRBMode) return;

	C6502 *CPUPtr = (C6502 *)PPPtr->GetWellDefinedComponent(COMPONENT_CPU);
	if(((MRBMode == MRB_SHADOW) ^ (Mode == MRB_SHADOW)) || (MRBMode == MRB_UNDEFINED))
	{
		PPPtr->ReleaseTrapAddress(PPNum, 0xfc7f, 0xffff);
		int c;

		/* setup memory layout */
			/*
				32kb RAM + 256b scratch + (OS(3) + 13)*16 + 64 JIM

				= 360704 bytes
			*/
		CPUPtr->SetMemoryTotal(360704 + ((Mode == MRB_SHADOW) ? 32768 : 0));
		RamAddr = CPUPtr->GetStorage((Mode == MRB_SHADOW) ? 65536 : 32768);
		c = 16;
		while(c--)
			RomAddrs[c] = RamAddr;
		if(!InstallROM("%ROMPATH%/basic.rom", ROM_BASIC))
			PPPtr->DebugMessage(PPDEBUG_BASICFAILED);
		ScratchAddr = CPUPtr->GetStorage(256);

		switch(Mode)
		{
			default:break;
			case MRB_OFF:
			case MRB_TURBO:
			case MRB_4Mhz:
				/* memory layout is the same for all of these - only timing differs */
				if(!InstallROM("%ROMPATH%/os.rom", ROM_OS))
					PPPtr->DebugMessage(PPDEBUG_OSFAILED);

				CPUPtr->EstablishMemoryLayouts(1);
				CPUPtr->SetMemoryLayout(0);

					/* page RAM */
					CPUPtr->SetReadPage(0, RamAddr, 0x8000);
					CPUPtr->SetWritePage(0, RamAddr, 0x8000);

					/* page BASIC */
					CPUPtr->SetReadPage(0x8000, RomAddrs[ROM_BASIC], 0x4000);
					SetScratch(0x8000);

					/* page OS */
					CPUPtr->SetReadPage(0xc000, RomAddrs[ROMAddress(ROM_OS)], 0x4000);
					SetScratch(0xc000);

				c = 8;
				while(c--)
					CPUPtr->SetMemoryView(c, 0);
			break;

			case MRB_SHADOW:
				if(!InstallROM("%ROMPATH%/os300.rom", ROM_OS))
					PPPtr->DebugMessage(PPDEBUG_OSFAILED);

				CPUPtr->EstablishMemoryLayouts(3);

				// normal memory layout
				CPUPtr->SetMemoryLayout(0);

					// RAM
					CPUPtr->SetReadPage(0x0000, RamAddr, 0x8000);
					CPUPtr->SetWritePage(0x0000, RamAddr, 0x8000);

					// page BASIC
					CPUPtr->SetReadPage(0x8000, RomAddrs[ROM_BASIC], 0x4000);
					SetScratch(0x8000);

					// page OS
					CPUPtr->SetReadPage(0xc000, RomAddrs[ROMAddress(ROM_OS)], 0x4000);
					SetScratch(0xc000);

				// shadow memory layout
				CPUPtr->SetMemoryLayout(1);

					// shadow RAM
					CPUPtr->SetReadPage(0, RamAddr + 0x8000, 0x3000);
					CPUPtr->SetWritePage(0, RamAddr + 0x8000, 0x3000);

					// page RAM
					CPUPtr->SetReadPage(0x3000, RamAddr + 0x3000, 0x5000);
					CPUPtr->SetWritePage(0x3000, RamAddr + 0x3000, 0x5000);

					// page BASIC
					CPUPtr->SetReadPage(0x8000, RomAddrs[ROM_BASIC], 0x4000);
					SetScratch(0x8000);

					// page OS
					CPUPtr->SetReadPage(0xc000, RomAddrs[ROMAddress(ROM_OS)], 0x4000);
					SetScratch(0xc000);;

				// shadow memory layout
				CPUPtr->SetMemoryLayout(2);

					// page RAM
					CPUPtr->SetReadPage(0, RamAddr + 0x8000, 0x8000);
					CPUPtr->SetWritePage(0, RamAddr + 0x8000, 0x8000);

					// page BASIC
					CPUPtr->SetReadPage(0x8000, RomAddrs[ROM_BASIC], 0x4000);
					SetScratch(0x8000);

					// page OS
					CPUPtr->SetReadPage(0xc000, RomAddrs[ROMAddress(ROM_OS)], 0x4000);
					SetScratch(0xc000);

				c = 8;
				while(c--)
					CPUPtr->SetMemoryView(c, (c == 6) ? 1 : 2);
			break;
		}

		switch(MRBMode = Mode)
		{
			default:break;
			case MRB_OFF:
				CPUPtr->SetExecCyclePage(0x0000, OneMhzBus, 0x8000);
				CPUPtr->SetExecCyclePage(0x8000, TwoMhzBus, 0x8000);
				CPUPtr->SetExecCyclePage(0xfe00, OneMhzBus, 0x0100);
			break;

			case MRB_TURBO:
				CPUPtr->SetExecCyclePage(0x0000, TwoMhzBus, 0x4000);
				CPUPtr->SetExecCyclePage(0x4000, OneMhzBus, 0x8000);
				CPUPtr->SetExecCyclePage(0x8000, TwoMhzBus, 0x8000);
				CPUPtr->SetExecCyclePage(0xfe00, OneMhzBus, 0x0100);
			break;

			case MRB_4Mhz:
				CPUPtr->SetExecCyclePage(0x0000, TwoMhzBus, 0x8000);
				CPUPtr->SetExecCyclePage(0x8000, TwoMhzBus, 0x8000);
				CPUPtr->SetExecCyclePage(0xfe00, TwoMhzBus, 0x0100);
			break;

			case MRB_SHADOW:
				PPPtr->ClaimTrapAddress(PPNum, 0xfc7f, 0xffff);

				CPUPtr->SetMemoryLayout(0);
					CPUPtr->SetExecCyclePage(0x0000, OneMhzBus, 0x8000);
					CPUPtr->SetExecCyclePage(0x8000, TwoMhzBus, 0x8000);
					CPUPtr->SetExecCyclePage(0xfe00, OneMhzBus, 0x0100);

				CPUPtr->SetMemoryLayout(1);
					CPUPtr->SetExecCyclePage(0x0000, TwoMhzBus, 0x3000);
					CPUPtr->SetExecCyclePage(0x3000, OneMhzBus, 0x5000);
					CPUPtr->SetExecCyclePage(0x8000, TwoMhzBus, 0x8000);
					CPUPtr->SetExecCyclePage(0xfe00, OneMhzBus, 0x0100);

				CPUPtr->SetMemoryLayout(2);
					CPUPtr->SetExecCyclePage(0x0000, TwoMhzBus, 0x8000);
					CPUPtr->SetExecCyclePage(0x8000, TwoMhzBus, 0x8000);
					CPUPtr->SetExecCyclePage(0xfe00, OneMhzBus, 0x0100);
			break;
		}
	}
	else
	{
		if(MRBMode != MRB_SHADOW)
		{
			switch(MRBMode = Mode)
			{
				default:break;
				case MRB_OFF:
					CPUPtr->SetExecCyclePage(0x0000, OneMhzBus, 0x8000);
					CPUPtr->SetExecCyclePage(0x8000, TwoMhzBus, 0x8000);
					CPUPtr->SetExecCyclePage(0xfe00, OneMhzBus, 0x0100);
				break;

				case MRB_TURBO:
					CPUPtr->SetExecCyclePage(0x0000, TwoMhzBus, 0x4000);
					CPUPtr->SetExecCyclePage(0x4000, OneMhzBus, 0x8000);
					CPUPtr->SetExecCyclePage(0x8000, TwoMhzBus, 0x8000);
					CPUPtr->SetExecCyclePage(0xfe00, OneMhzBus, 0x0100);
				break;

				case MRB_4Mhz:
					CPUPtr->SetExecCyclePage(0x0000, TwoMhzBus, 0x8000);
					CPUPtr->SetExecCyclePage(0x8000, TwoMhzBus, 0x8000);
					CPUPtr->SetExecCyclePage(0xfe00, TwoMhzBus, 0x0100);
				break;
			}
		}
		else
		{
			switch(MRBMode = Mode)
			{
				default:break;

				case MRB_SHADOW:
					PPPtr->ClaimTrapAddress(PPNum, 0xfc7f, 0xffff);

					CPUPtr->SetMemoryLayout(0);
						CPUPtr->SetExecCyclePage(0x0000, OneMhzBus, 0x8000);
						CPUPtr->SetExecCyclePage(0x8000, TwoMhzBus, 0x8000);
						CPUPtr->SetExecCyclePage(0xfe00, OneMhzBus, 0x0100);

					CPUPtr->SetMemoryLayout(1);
						CPUPtr->SetExecCyclePage(0x0000, TwoMhzBus, 0x3000);
						CPUPtr->SetExecCyclePage(0x3000, OneMhzBus, 0x5000);
						CPUPtr->SetExecCyclePage(0x8000, TwoMhzBus, 0x8000);
						CPUPtr->SetExecCyclePage(0xfe00, OneMhzBus, 0x0100);

					CPUPtr->SetMemoryLayout(2);
						CPUPtr->SetExecCyclePage(0x0000, TwoMhzBus, 0x8000);
						CPUPtr->SetExecCyclePage(0x8000, TwoMhzBus, 0x8000);
						CPUPtr->SetExecCyclePage(0xfe00, OneMhzBus, 0x0100);
				break;
			}
		}
	}
	SetULARAMTiming(MemHalting);
}

void CULA::AdjustInterrupts(Uint32 TimeStamp, Uint8 ANDMask, Uint8 ORMask)
{
	Status = (Status&ANDMask) | ORMask;

	if(Status & StatusMask)
	{
//		printf("Interrupt high %d, %02x\n", TimeStamp, Status);
		Status |= ULAIRQ_MASTER;
		PPPtr->IOCtl(IOCTL_SETIRQ, this, TimeStamp);
		
//		if(ORMask == ULAIRQ_DISPLAY)
//			printf("------ %d\n", TimeStamp);
	}
	else
	{
//		printf("Interrupt low %d\n", TimeStamp);
		Status &= ~ULAIRQ_MASTER;
		PPPtr->IOCtl(IOCTL_SETIRQ, NULL, TimeStamp);
	}
}

bool CULA::InterruptMeaningful(Uint8 v)
{
	return (StatusMask&v) ? true : false;
}

void CULA::AttachTo(CProcessPool &pool, Uint32 id)
{
	CComponentBase::AttachTo(pool, id);

	pool.ClaimTrapAddress(id, 0xfe00, 0xff0f);
	pool.ClaimTrapAddress(id, 0xfe05, 0xff0f);
	pool.ClaimTrapAddress(id, 0xfe06, 0xff0f);
	pool.ClaimTrapAddress(id, 0xfe07, 0xff0f);

	pool.SetTrapAddressSet(1);
	pool.ClaimTrapAddressSet(id, 0x8000, 0xc000);
	pool.SetTrapAddressSet(0);
	RomStates = 0;

	while(
		( AudioReadPtr != AudioWritePtr)
	)
		EnactAudioEvent();

	AudioBuffer[(AudioWritePtr-1)&(CULA_AUDIOEVENT_LENGTH-1)].ClockTime = TotalTime = ProgramTime = 0;
	/* set into a defined start state */
//	ClockDividerBackup = 0xff;
//	Write(0xfe06, 0, 0, 0);
//	ControlBackup = 0xff;
//	Write(0xfe07, 0, 0, 0);

	/* page BASIC (so that SOMETHING is paged) */
//	PagedRom = 10;
//	Keyboard = false;
//	Page(0x8000, MEM_ROM(8), 0x4000);
}

bool CULA::Write(Uint16 Addr, Uint32 TimeStamp, Uint8 Data8, Uint32 Data32)
{
	if(Addr == 0xfc7f)
	{
		/* Slogger MRB */
		C6502 *CPU = (C6502 *)PPPtr->GetWellDefinedComponent(COMPONENT_CPU);
		CPU->SetMemoryLayout(1);

		if(!(Data8&0x80))
		{
			CPU->SetMemoryView(0, 2);
			CPU->SetMemoryView(1, 2);
			CPU->SetMemoryView(2, 2);
			CPU->SetMemoryView(3, 2);
			CPU->SetMemoryView(4, 2);
			CPU->SetMemoryView(5, 2);
			CPU->SetMemoryView(7, 2);
		}
		else
		{
			CPU->SetMemoryView(0, 0);
			CPU->SetMemoryView(1, 0);
			CPU->SetMemoryView(2, 0);
			CPU->SetMemoryView(3, 0);
			CPU->SetMemoryView(4, 0);
			CPU->SetMemoryView(5, 0);
			CPU->SetMemoryView(7, 0);
		}
	}
	else
		switch(Addr&0xff0f)
		{
			default :
//				fprintf(stderr, "Unhandled write of %02x to %04x\n", Data8, Addr);
			return CComponentBase::Write(Addr, TimeStamp, Data8, Data32);

			case 0xfe00:
//				printf("Mask: %02x\n", Data8&(ULAIRQ_RTC|ULAIRQ_DISPLAY));
				Data8&=~0x83;
				if(StatusMask != Data8)
				{
					StatusMask = Data8;
					AdjustInterrupts(TimeStamp);
				}
			break;

//			case 0x01:
//				printf("%d: %02x\n", TimeStamp, Data8);
//			break;

			case 0xfe05:{	//paging & IRQ control

				/* deal with IRQ clearing */
					Uint8 IRQClearMask = 0;

					if(Data8&0x10) IRQClearMask |= ULAIRQ_DISPLAY;
					if(Data8&0x20) IRQClearMask |= ULAIRQ_RTC;
					if(Data8&0x40) IRQClearMask |= ULAIRQ_HTONE;

					if(Data8&0x70)
						AdjustInterrupts(TimeStamp, IRQClearMask^0xff);

				/* deal with paging maybe */
				if(!(Data8&0xf0))
				{
					LastPageWrite = Data8;
					if(((PagedRom&12) != 8) || (Data8&8))
					{
						if(PagedRom != (Data8&15))
						{
							PagedRom = Data8&15;

							if((Data8&8) || ((PagedRom&14) != 10))
							{
								LastPage = Data8&0xf;
								int ROMNo = ROMAddress(LastPage);
								int Mode = GetROMMode(ROMNo);

								if(((Data8&14) != 8))// && Mode)
								{
									//printf("r\n");
									// deinstall keyboard if necessary
									if(Keyboard)
									{
										PPPtr->SetTrapAddressSet(0);
										Keyboard = false;
									}

									if(!Mode)
									{
										LastPage = 10;	// 10 & 11 are the BASIC ROM
										ROMNo = ROM_BASIC;
									}
	
									Page(0x8000, RomAddrs[ROMNo], 0x4000, Mode != ROMMODE_RAM);
								}
								else
								{
									// install keyboard
									if(!Keyboard)
									{
										PPPtr->SetTrapAddressSet(1);

										C6502 *CPUPtr = (C6502 *)PPPtr->GetWellDefinedComponent(COMPONENT_CPU);
										SetScratch(0x8000);

										Keyboard = true;
									}
								}
							}
						}
					}
				}
			}break;

			case 0xfe06: //clock divider
				if(Data8 != ClockDividerBackup)
				{
					AudioBuffer[AudioWritePtr].Value = ClockDividerBackup = Data8;
					AudioBuffer[AudioWritePtr].Type = AudioEvent::SET_DIVIDER;
					WriteAudioEvent(TimeStamp);
				}
			break;

			case 0xfe07: //clock mode, display mode, tape motor, caps lock LED
				// [X, [clock mode: 2 bits], [display mode: 3 bits], cassette motor, caps LED]
				/* determine CapsLED, as may use it one day! */
				CapsLED = (Data8&0x80) ? true: false;

				Uint8 Difference = ControlBackup^Data8;
				ControlBackup = Data8;

				if(Difference&0x38)
				{
					Uint8 VidMode = (Data8 >> 3)&7;
					if(VidMode == 7) VidMode = 4;
					((CDisplay *)PPPtr->GetWellDefinedComponent(COMPONENT_DISPLAY))->SetMode(TimeStamp, VidMode);

					/* and do a multiplexing ROM page */
					/*((C6502 *)PPPtr->GetWellDefinedComponent(COMPONENT_CPU))->SetMemoryLayout(0);
					switch(VidMode)
					{
						case 0:
						case 3:
						case 4:
						case 6:
//							Page(0xc000, MEM_OS32(0), 0x4000);
						break;

						case 1:
						case 5:
//							Page(0xc000, MEM_OS32(1), 0x4000);
						break;						

						case 2:
//							Page(0xc000, MEM_OS32(2), 0x4000);
						break;
					}*/
				}

				/* affect sound output as necessary */
				if(Difference&0x06)
				{
					if((Data8&6) == 2)
						AudioBuffer[AudioWritePtr].Type = AudioEvent::START;
					else
						AudioBuffer[AudioWritePtr].Type = AudioEvent::STOP;

					WriteAudioEvent(TimeStamp);
				}

				/* affect tape mode as necessary */
//				if(Difference&0x46)
				{
//					printf("%02x: %d\n", Data8&0x06, TimeStamp);
					TapeModes TapeMode;
					switch(Data8&0x06)
					{
						default:
						case 6:
						case 2:
							TapeMode = TM_OFF;
						break;
						case 0: TapeMode = TM_INPUT;	break;
						case 4: TapeMode = TM_OUTPUT;	break;
					}

					return ((CTape *)PPPtr->GetWellDefinedComponent(COMPONENT_TAPE))->SetMode(TimeStamp, TapeMode, (Data8&0x40) ? true : false);
				}

			break;
		}

	return false;
}

bool CULA::Read(Uint16 Addr, Uint32 TimeStamp, Uint8 &Data8, Uint32 &Data32)
{
	Data8 = 0; Data32 = 0;

	if(Addr < 0xc000)
	{
		/* keyboard read */
		Data8 = 0xf0;
		Data8 |=	((Addr&0x0001) ? 0 : KeyboardState[0]) |
					((Addr&0x0002) ? 0 : KeyboardState[1]) |
					((Addr&0x0004) ? 0 : KeyboardState[2]) |
					((Addr&0x0008) ? 0 : KeyboardState[3]) |
					((Addr&0x0010) ? 0 : KeyboardState[4]) |
					((Addr&0x0020) ? 0 : KeyboardState[5]) |
					((Addr&0x0040) ? 0 : KeyboardState[6]) |
					((Addr&0x0080) ? 0 : KeyboardState[7]) |
					((Addr&0x0100) ? 0 : KeyboardState[8]) |
					((Addr&0x0200) ? 0 : KeyboardState[9]) |
					((Addr&0x0400) ? 0 : KeyboardState[10]) |
					((Addr&0x0800) ? 0 : KeyboardState[11]) |
					((Addr&0x1000) ? 0 : KeyboardState[12]) |
					((Addr&0x2000) ? 0 : KeyboardState[13]);
	}
	else
		switch(Addr&0xff0f)
		{
			default :
//				printf("Unhandled read from %04x\n", Addr);
			return CComponentBase::Read(Addr, TimeStamp, Data8, Data32);

			case 0xfe00: //interrupt status
				Data8 = Status;

				/* master line mirroring seems to occur? */
				if((ControlBackup&6) != 4)
				{
					Data8 |= (Data8&ULAIRQ_RTC) << 2;
				}

				Status &= ~ULAIRQ_POWER;
			break;
		}

	return false;
}

bool CULA::InstallROM(char *name, int slot)
{
	slot = ROMAddress(slot);

	char *Name = GetHost() -> ResolveFileName(name);
	if(!Name) return false;

	gzFile rom;
	Uint8 TData[16384];

	rom = gzopen(Name, "rb");
	delete[] Name;
	if(!rom) return false;

	gzread(rom, TData, 16384);
	gzclose(rom);
	
	if(slot == ROMAddress(ROM_OS))
	{
		TData[0xfcc0 - 0xc000] = 'l';
		TData[0xfcc1 - 0xc000] = 'e';
	}

	if(RomAddrs[slot] == RamAddr)
		RomAddrs[slot] = ((C6502 *)PPPtr->GetWellDefinedComponent(COMPONENT_CPU))->GetStorage(16384);
	((C6502 *)PPPtr->GetWellDefinedComponent(COMPONENT_CPU))->WriteMemoryBlock(RomAddrs[slot], 0, 16384, TData);
	SetROMMode(slot, ROMMODE_ROM);

//	printf("%s stored to rom %d - addr %d\n", name, slot, RomAddrs[slot]);

	/*
		int c = 16384;
		while(c--)
		{
			Uint8 Col1, Col2;

			Col1 = (MemoryPool8[MEM_OS+c]&0x01) | ((MemoryPool8[MEM_OS+c]>>1)&0x02) | ((MemoryPool8[MEM_OS+c]>>2)&0x04) | ((MemoryPool8[MEM_OS+c]>>3)&0x08);
			Col2 = ((MemoryPool8[MEM_OS+c]>>1)&0x01) | ((MemoryPool8[MEM_OS+c]>>2)&0x02) | ((MemoryPool8[MEM_OS+c]>>3)&0x04) | ((MemoryPool8[MEM_OS+c] >> 4)&0x08);

			MemoryPool32[MEM_OS32(2) + c] = Col1 | (Col2 << 4) | (Col1 << 16) | (Col2 << 20);
			MemoryPool32[MEM_OS32(0) + c] = 
					((MemoryPool8[MEM_OS+c]&0x80) ? 0x10000000 : 0) |
					((MemoryPool8[MEM_OS+c]&0x40) ? 0x01000000 : 0) |
					((MemoryPool8[MEM_OS+c]&0x20) ? 0x00100000 : 0) |
					((MemoryPool8[MEM_OS+c]&0x10) ? 0x00010000 : 0) |
					((MemoryPool8[MEM_OS+c]&0x08) ? 0x00001000 : 0) |
					((MemoryPool8[MEM_OS+c]&0x04) ? 0x00000100 : 0) |
					((MemoryPool8[MEM_OS+c]&0x02) ? 0x00000010 : 0) |
					((MemoryPool8[MEM_OS+c]&0x01) ? 0x00000001 : 0);
			MemoryPool32[MEM_OS32(1) + c] = ((MemoryPool8[MEM_OS+c]<<9)&0x1000) | ((MemoryPool8[MEM_OS+c]<<6)&0x2000) |
											((MemoryPool8[MEM_OS+c]<<6)&0x0100) | ((MemoryPool8[MEM_OS+c]<<3)&0x0200) |
											((MemoryPool8[MEM_OS+c]<<3)&0x0010) | ((MemoryPool8[MEM_OS+c]<<0)&0x0020) |
											((MemoryPool8[MEM_OS+c]<<0)&0x0001) | ((MemoryPool8[MEM_OS+c]>>3)&0x0002);

			MemoryPool8[MEM_OS32(1) + c] =
			MemoryPool8[MEM_OS32(2) + c] =
			MemoryPool8[MEM_OS32(0) + c];
		}

*/

	return true;
}

int CULA::GetMappedAddr(int area)
{
	switch(area)
	{
		case MEM_RAM: return RamAddr;
		case MEM_SHADOW: return RamAddr + 32768;
		default: return 0;
	}
}

Uint8 CULA::QueryRegister(ULAREG reg)
{
	switch(reg)
	{
		case ULAREG_INTSTATUS: return Status;
		case ULAREG_INTCONTROL: return StatusMask;
		case ULAREG_LASTPAGED: return Keyboard ? 8 : LastPage;	// keyboard occupies ROMs 8 & 9
		case ULAREG_PAGEREGISTER: return LastPageWrite;
	}
	
	return 0;
}

Uint8 CULA::QueryRegAddr(Uint16 Addr)
{
	switch(Addr)
	{
		case 0xfe06: return ClockDividerBackup;
		case 0xfe07: return ControlBackup;
	}
	return 0;
}
