#ifndef __DISPLAY_H
#define __DISPLAY_H

#include "ComponentBase.h"
#include "ULA.h"

/* could see one change per 4 cycles, => 9984 changes per frame, but this needs to be a power of 2 */
#define CDISPLAY_VIDEOEVENT_LENGTH	16384

#define CDF_FULLSCREEN	1
#define CDF_OVERLAY		2
#define CDF_MULTIPLEXED	4

/* some IOCTLS */
#define DISPIOCTL_GETSCREEN		0x300
#define DISPIOCTL_SEEDPALETTE	0x301
#define DISPIOCTL_FLIP			0x302
#define DISPIOCTL_GETDBUF		0x303

/* these ones aren't 'public' - they are 2Mhz bus measurements */
/*
http://www.epanorama.net/documents/video/pal_timing.html says:
Line period 64 us (Micro-seconds)
Line blanking 12.05 +- 0.25 us
Line sync 4.7 +- 0.1 us
Front porch: 1.65 +- 0.1 us
Burst start 5.6 +- 0.1 us after sync start.
Burst 10 +- 1 cycles
*/
//46 most likely (?)
#define PIXELS_OFFSET		0
#define DISPLAY_START		(7168+PIXELS_OFFSET)
#define ADDRESS_LOAD_TIME	DISPLAY_START
#define FRAME_TIME			(128*312)
#define CLOCK_INTERRUPT		(155 << 7)

class CDisplay : public CComponentBase
{
	public:
		CDisplay( ElectronConfiguration &cfg );
		virtual ~CDisplay();

		void ToggleFullScreen();
		bool QueryFullScreen();
		void Iconify(bool);
		bool IOCtl(Uint32 Control, void *Parameter, Uint32 TimeStamp);

		void AttachTo(CProcessPool &pool, Uint32 id);
		bool Write(Uint16 Addr, Uint32 TimeStamp, Uint8 Data8, Uint32 Data32);
		Uint8 Read(Uint16 Addr);
		Uint32 Update(Uint32, bool);
		void SetMode(Uint32 TimeStamp, Uint8 Mode);

		void SetMultiplexedPalette(SDL_Color *);

		void SetFlags(Uint32 Flags);
		Uint32 GetFlags();

		void SetInterrupt(Uint32 TimeStamp);

		/* these ones are for when the GUI wants to get on */
		SDL_Surface *GetFrontBuffer();
		void ReleaseFrontBuffer();

		SDL_Surface * GetBufferCopy();

	private:
		/* for GUI */
		SDL_mutex *FrameBufferMutex;

		/* memory collection related */
		Uint16 AddrSource[39936];
		Uint16 VideoOffsets8[39936];
		Uint32 VideoBuffer32[39936];

		/* SDL graphics mode related */
		Uint32 DispFlags;
		bool AllowOverlay, DisplayMultiplexed;

		SDL_Surface *FrameBuffer;
		SDL_Rect ScaleTarget;
		SDL_Overlay *Overlay;
		int XOffset, YOffset;
		int TableShift, TableEntryLength;

		/* Electron graphics mode related */
		bool Icon;
		void UpdateDisplay(bool Catchup);
		void NonAffectingDraw(SDL_Surface *Target);

		void EnactEvent();
		void SumEvents();
		bool TablesDirty;
		int IScanline;

		void FillAddressTable(Uint8 Mode, int index);
		void RefillGraphicsTables();
		void RecalculatePalette();
		bool TryForMode(int w, int h, int bpp, Uint32 flags);
		void GetSurface();
		void FreeSurface();

		int YOffs1, YOffs2, UOffs, VOffs;
		bool Surface;

		Uint32 FrameStart, CurrentTime, LineBase, AddrMode;

		struct VideoEvent
		{
			enum
			{
				MODE, PALETTE, IRQ
			} Type;

			Uint8 Value, Address;
			Uint32 TimeStamp;
		};

		VideoEvent VideoEventQueue[CDISPLAY_VIDEOEVENT_LENGTH];

		Uint32 VideoWritePtr, VideoReadPtr;

		Uint32 *DisplayTables;
		Uint8 *MultiDisplayTables;

		SDL_Color SrcPaletteRGB[256];
		Uint8 PaletteYUV[256][3];
		Uint32 PaletteRGB[256];

		Uint8 PaletteBytes[8];
		Uint32 PaletteColours[256];
		Uint32 YUVTable[8];
		Uint8 CMode, MostRecentMode;
		Uint32 ByteMask;
		bool Blank, ModeChanged;
		Uint32 BlankColour, Black;

		bool EmptyLine[256];

		/* CRC related */
		static Uint32 CRCTable[256];
		Uint32 CRCBuffer[256];
		void SetupCRCTable();

		/* start address */
		Uint16 StartAddr, BackupStartAddr, FrameStartAddr;

		/* ULA */
		CULA *ULA;

		int IdealWidth, IdealHeight;

		/* TO WORK AROUND YET ANOTHER NON-WORKING SDL FEATURE */
		int NativeBytesPerPixel;
};

#endif
