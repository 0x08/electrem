#import "Preferences.h"
#include "../../GUIEvents.h"
#include <SDL.h>
#include "../../Configuration/ElectronConfiguration.h"
@implementation Preferences

// Called when this class is loaded from a nib file, sets up the controls on the preferences window.
- (void)awakeFromNib
{
	NSUserDefaults * savedPrefs = [NSUserDefaults standardUserDefaults];

	// Set the toggles
	[ fastTapeToggle setState:[savedPrefs boolForKey:@"FastTape" ] ];
	[ multiplexedToggle setState:[savedPrefs boolForKey:@"MultiplexedDisplay" ] ];
	[ firstByteToggle setState:[savedPrefs boolForKey:@"FirstByte" ] ];
	[ plus1Toggle setState:[savedPrefs boolForKey:@"Plus1" ] ];
	[ plus3Toggle setState:[savedPrefs boolForKey:@"Plus3" ] ];
	[ autoconfigure setState:[savedPrefs boolForKey:@"Autoconfigure" ] ];
	[ autoload setState:[savedPrefs boolForKey:@"Autoload" ] ];
	[ drive1wptoggle setState:[savedPrefs boolForKey:@"Drive1WriteProtect" ] ];
	[ drive2wptoggle setState:[savedPrefs boolForKey:@"Drive2WriteProtect" ] ];
	[ startFullscreenToggle setState:[savedPrefs boolForKey:@"StartFullScreen" ] ];
	[ saveStateToggle setState:[savedPrefs boolForKey:@"PersistentState" ] ];

	float vol = (float)[savedPrefs integerForKey:@"Volume" ];
	[ volume setFloatValue:vol ];

	// Set the Slogger mode
	int mode = [ savedPrefs integerForKey:@"SloggerMRB" ];
	if ( mode > 0 )
	{
		[sloggerToggle setState:YES];
		[sloggerMode setState:YES atRow: (mode - 1) column: 0];
	}

	// Update the enable state based on the options selected
	[ self updateEnable ];

	// Make the window visible and bring it forward
	[prefWindow makeKeyAndOrderFront:nil];
}

// Update certain controls to enable/disable them. This prevents you from selecting a slogger mode when
// slogger is turned off, and from using a Plus1/3 when First Byte is on (and vice-versa)
- (void) updateEnable
{
	// Don't allow Plus 3 to be used if First Byte is on
/*	if ( [firstByteToggle state ] != 0 )
	{
		[ plus1Toggle setEnabled:NO ];
		[ plus3Toggle setEnabled:NO ];
		[ plus1Toggle setState:0 ];
		[ plus3Toggle setState:0 ];
	}
	else
	{
		[ plus1Toggle setEnabled:YES ];
		[ plus3Toggle setEnabled:YES ];
	}*/
	
	if( [plus3Toggle state] )
	{
		[ drive1wptoggle setEnabled:YES ];
		[ drive2wptoggle setEnabled:YES ];
	}
	else
	{
		[ drive1wptoggle setEnabled:NO ];
		[ drive2wptoggle setEnabled:NO ];
	}

	// Disable first byte option if plus 3 is enabled
//	[ firstByteToggle setEnabled: ( [plus1Toggle state] != 1 && [plus3Toggle state] != 1 ) ];	

	// Enable the slogger mode options if slogger is turned on
	[sloggerMode setEnabled: ( [sloggerToggle state ] != 0 ) ];

	// enable the hardware configuration if autoload is turned on
	if([autoload state])
		[ autoconfigure setEnabled:YES ];
	else
	{
		[ autoconfigure setEnabled:NO ];
		[ autoconfigure setState:NO ];
	}	
}

- (IBAction)toggleOption:(id)sender
{
	// Update the toggle buttons based on the selection
	[ self updateEnable ];

	// Store any changes
	NSUserDefaults * savedPrefs = [NSUserDefaults standardUserDefaults];
	[savedPrefs setBool:[ plus1Toggle state] forKey:@"Plus1" ];
	[savedPrefs setBool:[ plus3Toggle state] forKey:@"Plus3" ];
	[savedPrefs setBool:[ firstByteToggle state] forKey:@"FirstByte" ];
	[savedPrefs setBool:[ fastTapeToggle state]  forKey:@"FastTape" ];
	[savedPrefs setBool:[ multiplexedToggle state] forKey:@"MultiplexedDisplay" ];
	[savedPrefs setBool:[ autoconfigure state] forKey:@"Autoconfigure" ];
	[savedPrefs setBool:[ autoload state] forKey:@"Autoload" ];
	[savedPrefs setBool:[ drive1wptoggle state] forKey:@"Drive1WriteProtect" ];
	[savedPrefs setBool:[ drive2wptoggle state] forKey:@"Drive2WriteProtect" ];
	[savedPrefs setBool:[ startFullscreenToggle state] forKey:@"StartFullScreen" ];
	[savedPrefs setBool:[ saveStateToggle state ] forKey:@"PersistentState"];

	int vol = (int)[volume floatValue];
	[savedPrefs setInteger:vol forKey:@"Volume" ];

	int mode = 0;
	if ( [sloggerToggle state] != 0 )
	{
		mode = [sloggerMode selectedRow ];
		mode ++;
	}

	[ savedPrefs setInteger:mode forKey:@"SloggerMRB" ];

	// asign configuration
    SDL_Event evt;
	evt.type = SDL_USEREVENT;
	evt.user.code = GUIEVT_ASSIGNCONFIG;

	ElectronConfiguration *Cfg = new ElectronConfiguration;
	Cfg->Plus1 = [ plus1Toggle state];
	Cfg->FirstByte = [ firstByteToggle state];
	Cfg->FastTape = [ fastTapeToggle state];
	Cfg->Autoload = [ autoload state];
	Cfg->Autoconfigure = [ autoconfigure state];
	Cfg->PersistentState = [ saveStateToggle state];
	switch(mode)
	{
		default:
		case 0: Cfg->MRBMode = MRB_OFF; break;
		case 1: Cfg->MRBMode = MRB_TURBO; break;
		case 2: Cfg->MRBMode = MRB_SHADOW; break;
		case 3: Cfg->MRBMode = MRB_4Mhz; break;
	}
	Cfg->Plus3.Enabled = [ plus3Toggle state];
	Cfg->Plus3.Drive1WriteProtect = [ drive1wptoggle state];
	Cfg->Plus3.Drive2WriteProtect = [ drive2wptoggle state];

	Cfg->Display.DisplayMultiplexed = [multiplexedToggle state];
	Cfg->Display.StartFullScreen = [startFullscreenToggle state];
	Cfg->Volume = vol;

	evt.user.data1 = Cfg;
	/* may have to wait here if event queue is full */
	while( SDL_PushEvent( &evt ) == -1)
		SDL_Delay(10);

	// Write the changes to disk
	[savedPrefs synchronize];
}

@end
