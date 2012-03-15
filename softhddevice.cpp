///
///	@file softhddevice.cpp	@brief A software HD device plugin for VDR.
///
///	Copyright (c) 2011, 2012 by Johns.  All Rights Reserved.
///
///	Contributor(s):
///
///	License: AGPLv3
///
///	This program is free software: you can redistribute it and/or modify
///	it under the terms of the GNU Affero General Public License as
///	published by the Free Software Foundation, either version 3 of the
///	License.
///
///	This program is distributed in the hope that it will be useful,
///	but WITHOUT ANY WARRANTY; without even the implied warranty of
///	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
///	GNU Affero General Public License for more details.
///
///	$Id$
//////////////////////////////////////////////////////////////////////////////

#include <vdr/interface.h>
#include <vdr/plugin.h>
#include <vdr/player.h>
#include <vdr/osd.h>
#include <vdr/dvbspu.h>
#include <vdr/shutdown.h>

#ifdef HAVE_CONFIG
#include "config.h"
#endif

#include "softhddev.h"
#include "softhddevice.h"
extern "C"
{
#include "video.h"
    extern const char *X11DisplayName;		///< x11 display name

    extern void AudioPoller(void);
    extern void CodecSetAudioPassthrough(int);
    extern void CodecSetAudioDownmix(int);
}

//////////////////////////////////////////////////////////////////////////////

    /// vdr-plugin version number.
    /// Makefile extracts the version number for generating the file name
    /// for the distribution archive.
static const char *const VERSION = "0.5.0"
#ifdef GIT_REV
    "-GIT" GIT_REV
#endif
    ;

    /// vdr-plugin description.
static const char *const DESCRIPTION =
trNOOP("A software and GPU emulated HD device");

    /// vdr-plugin text of main menu entry
static const char *MAINMENUENTRY = trNOOP("SoftHdDevice");

    /// single instance of softhddevice plugin device.
static class cSoftHdDevice *MyDevice;

//////////////////////////////////////////////////////////////////////////////

#define RESOLUTIONS 4			///< number of resolutions

    /// resolutions names
static const char *const Resolution[RESOLUTIONS] = {
    "576i", "720p", "1080i_fake", "1080i"
};

static char ConfigMakePrimary;		///< config primary wanted
static char ConfigHideMainMenuEntry;	///< config hide main menu entry

static uint32_t ConfigVideoBackground;	///< config video background color
static int ConfigVideoSkipLines;	///< config skip lines top/bottom
static char ConfigVideoStudioLevels;	///< config use studio levels
static char ConfigVideo60HzMode;	///< config use 60Hz display mode
static char ConfigVideoSoftStartSync;	///< config use softstart sync

    /// config deinterlace
static int ConfigVideoDeinterlace[RESOLUTIONS];

    /// config skip chroma
static int ConfigVideoSkipChromaDeinterlace[RESOLUTIONS];

    /// config inverse telecine
static int ConfigVideoInverseTelecine[RESOLUTIONS];

    /// config denoise
static int ConfigVideoDenoise[RESOLUTIONS];

    /// config sharpen
static int ConfigVideoSharpen[RESOLUTIONS];

    /// config scaling
static int ConfigVideoScaling[RESOLUTIONS];

static int ConfigVideoAudioDelay;	///< config audio delay
static int ConfigAudioPassthrough;	///< config audio pass-through
static int ConfigAudioDownmix;		///< config audio downmix

static int ConfigAutoCropInterval;	///< auto crop detection interval
static int ConfigAutoCropDelay;		///< auto crop detection delay
static int ConfigAutoCropTolerance;	///< auto crop detection tolerance

static char ConfigSuspendClose;		///< suspend should close devices
static char ConfigSuspendX11;		///< suspend should stop x11

static volatile int DoMakePrimary;	///< switch primary device to this

#define SUSPEND_EXTERNAL	-1	///< play external suspend mode
#define NOT_SUSPENDED		0	///< not suspend mode
#define SUSPEND_NORMAL		1	///< normal suspend mode
#define SUSPEND_DETACHED	2	///< detached suspend mode
static char SuspendMode;		///< suspend mode

//////////////////////////////////////////////////////////////////////////////

//////////////////////////////////////////////////////////////////////////////
//	C Callbacks
//////////////////////////////////////////////////////////////////////////////

/**
**	Soft device plugin remote class.
*/
class cSoftRemote:public cRemote
{
  public:

    /**
    **	Soft device remote class constructor.
    **
    **	@param name	remote name
    */
    cSoftRemote(const char *name):cRemote(name)
    {
    }

    /**
    **	Put keycode into vdr event queue.
    **
    **	@param code	key code
    **	@param repeat	flag key repeated
    **	@param release	flag key released
    */
    bool Put(const char *code, bool repeat = false, bool release = false) {
	return cRemote::Put(code, repeat, release);
    }
};

/**
**	Feed key press as remote input (called from C part).
**
**	@param keymap	target keymap "XKeymap" name
**	@param key	pressed/released key name
**	@param repeat	repeated key flag
**	@param release	released key flag
*/
extern "C" void FeedKeyPress(const char *keymap, const char *key, int repeat,
    int release)
{
    cRemote *remote;
    cSoftRemote *csoft;

    if (!keymap || !key) {
	return;
    }
    // find remote
    for (remote = Remotes.First(); remote; remote = Remotes.Next(remote)) {
	if (!strcmp(remote->Name(), keymap)) {
	    break;
	}
    }
    // if remote not already exists, create it
    if (remote) {
	csoft = (cSoftRemote *) remote;
    } else {
	dsyslog("[softhddev]%s: remote '%s' not found\n", __FUNCTION__,
	    keymap);
	csoft = new cSoftRemote(keymap);
    }

    //dsyslog("[softhddev]%s %s, %s\n", __FUNCTION__, keymap, key);
    if (key[1]) {			// no single character
	csoft->Put(key, repeat, release);
    } else if (!csoft->Put(key, repeat, release)) {
	cRemote::Put(KBDKEY(key[0]));	// feed it for edit mode
    }
}

//////////////////////////////////////////////////////////////////////////////
//	OSD
//////////////////////////////////////////////////////////////////////////////

/**
**	Soft device plugin OSD class.
*/
class cSoftOsd:public cOsd
{
  public:
    static volatile char Dirty;		///< flag force redraw everything

     cSoftOsd(int, int, uint);		///< constructor
     virtual ~ cSoftOsd(void);		///< destructor
    virtual void Flush(void);		///< commits all data to the hardware
    virtual void SetActive(bool);	///< sets OSD to be the active one
};

volatile char cSoftOsd::Dirty;		///< flag force redraw everything

/**
**	Sets this OSD to be the active one.
**
**	@param on	true on, false off
**
**	@note only needed as workaround for text2skin plugin with
**	undrawn areas.
*/
void cSoftOsd::SetActive(bool on)
{
    //dsyslog("[softhddev]%s: %d\n", __FUNCTION__, on);

    if (Active() == on) {
	return;				// already active, no action
    }
    cOsd::SetActive(on);
    if (on) {
	Dirty = 1;
    } else {
	OsdClose();
    }
}

/**
**	Constructor OSD.
**
**	Initializes the OSD with the given coordinates.
**
**	@param left	x-coordinate of osd on display
**	@param top	y-coordinate of osd on display
**	@param level	level of the osd (smallest is shown)
*/
cSoftOsd::cSoftOsd(int left, int top, uint level)
:cOsd(left, top, level)
{
    /* FIXME: OsdWidth/OsdHeight not correct!
       dsyslog("[softhddev]%s: %dx%d+%d+%d, %d\n", __FUNCTION__, OsdWidth(),
       OsdHeight(), left, top, level);
     */

    SetActive(true);
}

/**
**	OSD Destructor.
**
**	Shuts down the OSD.
*/
cSoftOsd::~cSoftOsd(void)
{
    //dsyslog("[softhddev]%s:\n", __FUNCTION__);
    SetActive(false);
    // done by SetActive: OsdClose();

#ifdef USE_YAEPG
    // support yaepghd, video window
    if (vidWin.bpp) {			// restore fullsized video
	int width;
	int height;
	double video_aspect;

	::GetOsdSize(&width, &height, &video_aspect);
	// works osd relative
	VideoSetOutputPosition(0, 0, width, height);
    }
#endif
}

/**
**	Actually commits all data to the OSD hardware.
*/
void cSoftOsd::Flush(void)
{
    cPixmapMemory *pm;

    if (!Active()) {
	return;
    }
#ifdef USE_YAEPG
    // support yaepghd, video window
    if (vidWin.bpp) {
	dsyslog("[softhddev]%s: %dx%d+%d+%d\n", __FUNCTION__, vidWin.Width(),
	    vidWin.Height(), vidWin.x1, vidWin.y2);

	// FIXME: vidWin is OSD relative not video window.
	VideoSetOutputPosition(Left() + vidWin.x1, Top() + vidWin.y1,
	    vidWin.Width(), vidWin.Height());
    }
#endif

    if (!IsTrueColor()) {
	static char warned;
	cBitmap *bitmap;
	int i;

	if (!warned) {
	    dsyslog("[softhddev]%s: FIXME: should be truecolor\n",
		__FUNCTION__);
	    warned = 1;
	}
	// draw all bitmaps
	for (i = 0; (bitmap = GetBitmap(i)); ++i) {
	    uint8_t *argb;
	    int x;
	    int y;
	    int w;
	    int h;
	    int x1;
	    int y1;
	    int x2;
	    int y2;

	    // get dirty bounding box
	    if (Dirty) {		// forced complete update
		x1 = 0;
		y1 = 0;
		x2 = bitmap->Width() - 1;
		y2 = bitmap->Height() - 1;
	    } else if (!bitmap->Dirty(x1, y1, x2, y2)) {
		continue;		// nothing dirty continue
	    }
	    // convert and upload only dirty areas
	    w = x2 - x1 + 1;
	    h = y2 - y1 + 1;
	    if (1) {			// just for the case it makes trouble
		int width;
		int height;
		double video_aspect;

		::GetOsdSize(&width, &height, &video_aspect);
		if (w > width) {
		    w = width;
		    x2 = x1 + width - 1;
		}
		if (h > height) {
		    h = height;
		    y2 = y1 + height - 1;
		}
	    }
#ifdef DEBUG
	    if (w > bitmap->Width() || h > bitmap->Height()) {
		esyslog(tr("[softhddev]: dirty area too big\n"));
		abort();
	    }
#endif
	    argb = (uint8_t *) malloc(w * h * sizeof(uint32_t));
	    for (y = y1; y <= y2; ++y) {
		for (x = x1; x <= x2; ++x) {
		    ((uint32_t *) argb)[x - x1 + (y - y1) * w] =
			bitmap->GetColor(x, y);
		}
	    }
	    OsdDrawARGB(Left() + bitmap->X0() + x1, Top() + bitmap->Y0() + y1,
		w, h, argb);

	    bitmap->Clean();
	    // FIXME: reuse argb
	    free(argb);
	}
	cSoftOsd::Dirty = 0;
	return;
    }

    LOCK_PIXMAPS;
    while ((pm = RenderPixmaps())) {
	int x;
	int y;
	int w;
	int h;

	x = Left() + pm->ViewPort().X();
	y = Top() + pm->ViewPort().Y();
	w = pm->ViewPort().Width();
	h = pm->ViewPort().Height();

	/*
	   dsyslog("[softhddev]%s: draw %dx%d+%d+%d %p\n", __FUNCTION__, w, h,
	   x, y, pm->Data());
	 */

	OsdDrawARGB(x, y, w, h, pm->Data());

	delete pm;
    }
}

//////////////////////////////////////////////////////////////////////////////
//	OSD provider
//////////////////////////////////////////////////////////////////////////////

/**
**	Soft device plugin OSD provider class.
*/
class cSoftOsdProvider:public cOsdProvider
{
  private:
    static cOsd *Osd;
  public:
    virtual cOsd * CreateOsd(int, int, uint);
    virtual bool ProvidesTrueColor(void);
    cSoftOsdProvider(void);
};

cOsd *cSoftOsdProvider::Osd;		///< single osd

/**
**	Create a new OSD.
**
**	@param left	x-coordinate of OSD
**	@param top	y-coordinate of OSD
**	@param level	layer level of OSD
*/
cOsd *cSoftOsdProvider::CreateOsd(int left, int top, uint level)
{
    //dsyslog("[softhddev]%s: %d, %d, %d\n", __FUNCTION__, left, top, level);

    return Osd = new cSoftOsd(left, top, level);
}

/**
**	Check if this OSD provider is able to handle a true color OSD.
**
**	@returns true we are able to handle a true color OSD.
*/
bool cSoftOsdProvider::ProvidesTrueColor(void)
{
    return true;
}

/**
**	Create cOsdProvider class.
*/
cSoftOsdProvider::cSoftOsdProvider(void)
:  cOsdProvider()
{
    //dsyslog("[softhddev]%s:\n", __FUNCTION__);
}

//////////////////////////////////////////////////////////////////////////////
//	cMenuSetupPage
//////////////////////////////////////////////////////////////////////////////

/**
**	Soft device plugin menu setup page class.
*/
class cMenuSetupSoft:public cMenuSetupPage
{
  protected:
    ///
    /// local copies of global setup variables:
    /// @{
    int MakePrimary;
    int HideMainMenuEntry;
    uint32_t Background;
    uint32_t BackgroundAlpha;
    int SkipLines;
    int StudioLevels;
    int _60HzMode;
    int SoftStartSync;
    int Scaling[RESOLUTIONS];
    int Deinterlace[RESOLUTIONS];
    int SkipChromaDeinterlace[RESOLUTIONS];
    int InverseTelecine[RESOLUTIONS];
    int Denoise[RESOLUTIONS];
    int Sharpen[RESOLUTIONS];
    int AudioDelay;
    int AudioPassthrough;
    int AudioDownmix;
    int AutoCropInterval;
    int AutoCropDelay;
    int AutoCropTolerance;
    int SuspendClose;
    int SuspendX11;
    /// @}
  protected:
     virtual void Store(void);
  public:
     cMenuSetupSoft(void);
};

/**
**	Create a seperator item.
**
**	@param label	text inside separator
*/
static inline cOsdItem *SeparatorItem(const char *label)
{
    cOsdItem *item;

    item = new cOsdItem(cString::sprintf("* %s: ", label));
    item->SetSelectable(false);

    return item;
}

/**
**	Constructor setup menu.
*/
cMenuSetupSoft::cMenuSetupSoft(void)
{
    static const char *const deinterlace[] = {
	"Bob", "Weave/None", "Temporal", "TemporalSpatial", "Software Bob",
	"Software Spatial",
    };
    static const char *const scaling[] = {
	"Normal", "Fast", "HQ", "Anamorphic"
    };
    static const char *const passthrough[] = {
	"None", "AC-3"
    };
    static const char *const resolution[RESOLUTIONS] = {
	"576i", "720p", "fake 1080i", "1080i"
    };
    int i;

    // cMenuEditBoolItem cMenuEditBitItem cMenuEditNumItem
    // cMenuEditStrItem cMenuEditStraItem cMenuEditIntItem
    MakePrimary = ConfigMakePrimary;
    Add(new cMenuEditBoolItem(tr("Make primary device"), &MakePrimary,
	    trVDR("no"), trVDR("yes")));
    HideMainMenuEntry = ConfigHideMainMenuEntry;
    Add(new cMenuEditBoolItem(tr("Hide main menu entry"), &HideMainMenuEntry,
	    trVDR("no"), trVDR("yes")));
    //
    //	video
    //
    Add(SeparatorItem(tr("Video")));

    // no unsigned int menu item supported, split background color/alpha
    Background = ConfigVideoBackground >> 8;
    BackgroundAlpha = ConfigVideoBackground & 0xFF;
    Add(new cMenuEditIntItem(tr("video background color (RGB)"),
	    (int *)&Background, 0, 0x00FFFFFF));
    Add(new cMenuEditIntItem(tr("video background color (Alpha)"),
	    (int *)&BackgroundAlpha, 0, 0xFF));
    SkipLines = ConfigVideoSkipLines;
    Add(new cMenuEditIntItem(tr("Skip lines top+bot (pixel)"), &SkipLines, 0,
	    64));
    StudioLevels = ConfigVideoStudioLevels;
    Add(new cMenuEditBoolItem(tr("Use studio levels (vdpau only)"),
	    &StudioLevels, trVDR("no"), trVDR("yes")));
    _60HzMode = ConfigVideo60HzMode;
    Add(new cMenuEditBoolItem(tr("60hz display mode"), &_60HzMode, trVDR("no"),
	    trVDR("yes")));
    SoftStartSync = ConfigVideoSoftStartSync;
    Add(new cMenuEditBoolItem(tr("soft start a/v sync"), &SoftStartSync,
	    trVDR("no"), trVDR("yes")));

    for (i = 0; i < RESOLUTIONS; ++i) {
	Add(SeparatorItem(resolution[i]));
	Scaling[i] = ConfigVideoScaling[i];
	Add(new cMenuEditStraItem(tr("Scaling"), &Scaling[i], 4, scaling));
	Deinterlace[i] = ConfigVideoDeinterlace[i];
	Add(new cMenuEditStraItem(tr("Deinterlace"), &Deinterlace[i], 6,
		deinterlace));
	SkipChromaDeinterlace[i] = ConfigVideoSkipChromaDeinterlace[i];
	Add(new cMenuEditBoolItem(tr("SkipChromaDeinterlace (vdpau)"),
		&SkipChromaDeinterlace[i], trVDR("no"), trVDR("yes")));
	InverseTelecine[i] = ConfigVideoInverseTelecine[i];
	Add(new cMenuEditBoolItem(tr("Inverse Telecine (vdpau)"),
		&InverseTelecine[i], trVDR("no"), trVDR("yes")));
	Denoise[i] = ConfigVideoDenoise[i];
	Add(new cMenuEditIntItem(tr("Denoise (0..1000) (vdpau)"), &Denoise[i],
		0, 1000, tr("off"), tr("max")));
	Sharpen[i] = ConfigVideoSharpen[i];
	Add(new cMenuEditIntItem(tr("Sharpen (-1000..1000) (vdpau)"),
		&Sharpen[i], -1000, 1000, tr("blur max"), tr("sharpen max")));
    }
    //
    //	audio
    //
    Add(SeparatorItem(tr("Audio")));
    AudioDelay = ConfigVideoAudioDelay;
    Add(new cMenuEditIntItem(tr("Audio delay (ms)"), &AudioDelay, -1000,
	    1000));
    AudioPassthrough = ConfigAudioPassthrough;
    Add(new cMenuEditStraItem(tr("Audio pass-through"), &AudioPassthrough, 2,
	    passthrough));
    AudioDownmix = ConfigAudioDownmix;
    Add(new cMenuEditBoolItem(tr("Enable AC-3 downmix"), &AudioDownmix,
	    trVDR("no"), trVDR("yes")));
    //
    //	auto-crop
    //
    Add(SeparatorItem(tr("Auto-crop")));
    AutoCropInterval = ConfigAutoCropInterval;
    Add(new cMenuEditIntItem(tr("autocrop interval (frames)"),
	    &AutoCropInterval, 0, 200, tr("off")));
    AutoCropDelay = ConfigAutoCropDelay;
    Add(new cMenuEditIntItem(tr("autocrop delay (n * interval)"),
	    &AutoCropDelay, 0, 200));
    AutoCropTolerance = ConfigAutoCropTolerance;
    Add(new cMenuEditIntItem(tr("autocrop tolerance (pixel)"),
	    &AutoCropTolerance, 0, 32));
    //
    //	suspend
    //
    Add(SeparatorItem(tr("Suspend")));
    SuspendClose = ConfigSuspendClose;
    Add(new cMenuEditBoolItem(tr("suspend closes video+audio"), &SuspendClose,
	    trVDR("no"), trVDR("yes")));
    SuspendX11 = ConfigSuspendX11;
    Add(new cMenuEditBoolItem(tr("suspend stops x11"), &SuspendX11,
	    trVDR("no"), trVDR("yes")));
}

/**
**	Store setup.
*/
void cMenuSetupSoft::Store(void)
{
    int i;

    SetupStore("MakePrimary", ConfigMakePrimary = MakePrimary);
    SetupStore("HideMainMenuEntry", ConfigHideMainMenuEntry =
	HideMainMenuEntry);

    ConfigVideoBackground = Background << 8 | (BackgroundAlpha & 0xFF);
    SetupStore("Background", ConfigVideoBackground);
    VideoSetBackground(ConfigVideoBackground);
    SetupStore("SkipLines", ConfigVideoSkipLines = SkipLines);
    VideoSetSkipLines(ConfigVideoSkipLines);
    SetupStore("StudioLevels", ConfigVideoStudioLevels = StudioLevels);
    VideoSetStudioLevels(ConfigVideoStudioLevels);
    SetupStore("60HzMode", ConfigVideo60HzMode = _60HzMode);
    VideoSet60HzMode(ConfigVideo60HzMode);
    SetupStore("SoftStartSync", ConfigVideoSoftStartSync = SoftStartSync);
    VideoSetSoftStartSync(ConfigVideoSoftStartSync);

    for (i = 0; i < RESOLUTIONS; ++i) {
	char buf[128];

	snprintf(buf, sizeof(buf), "%s.%s", Resolution[i], "Scaling");
	SetupStore(buf, ConfigVideoScaling[i] = Scaling[i]);
	snprintf(buf, sizeof(buf), "%s.%s", Resolution[i], "Deinterlace");
	SetupStore(buf, ConfigVideoDeinterlace[i] = Deinterlace[i]);
	snprintf(buf, sizeof(buf), "%s.%s", Resolution[i],
	    "SkipChromaDeinterlace");
	SetupStore(buf, ConfigVideoSkipChromaDeinterlace[i] =
	    SkipChromaDeinterlace[i]);
	snprintf(buf, sizeof(buf), "%s.%s", Resolution[i], "InverseTelecine");
	SetupStore(buf, ConfigVideoInverseTelecine[i] = InverseTelecine[i]);
	snprintf(buf, sizeof(buf), "%s.%s", Resolution[i], "Denoise");
	SetupStore(buf, ConfigVideoDenoise[i] = Denoise[i]);
	snprintf(buf, sizeof(buf), "%s.%s", Resolution[i], "Sharpen");
	SetupStore(buf, ConfigVideoSharpen[i] = Sharpen[i]);
    }
    VideoSetScaling(ConfigVideoScaling);
    VideoSetDeinterlace(ConfigVideoDeinterlace);
    VideoSetSkipChromaDeinterlace(ConfigVideoSkipChromaDeinterlace);
    VideoSetInverseTelecine(ConfigVideoInverseTelecine);
    VideoSetDenoise(ConfigVideoDenoise);
    VideoSetSharpen(ConfigVideoSharpen);

    SetupStore("AudioDelay", ConfigVideoAudioDelay = AudioDelay);
    VideoSetAudioDelay(ConfigVideoAudioDelay);
    SetupStore("AudioPassthrough", ConfigAudioPassthrough = AudioPassthrough);
    CodecSetAudioPassthrough(ConfigAudioPassthrough);
    SetupStore("AudioDownmix", ConfigAudioDownmix = AudioDownmix);
    CodecSetAudioDownmix(ConfigAudioDownmix);

    SetupStore("AutoCrop.Interval", ConfigAutoCropInterval = AutoCropInterval);
    SetupStore("AutoCrop.Delay", ConfigAutoCropDelay = AutoCropDelay);
    SetupStore("AutoCrop.Tolerance", ConfigAutoCropTolerance =
	AutoCropTolerance);
    VideoSetAutoCrop(ConfigAutoCropInterval, ConfigAutoCropDelay,
	ConfigAutoCropTolerance);

    SetupStore("Suspend.Close", ConfigSuspendClose = SuspendClose);
    SetupStore("Suspend.X11", ConfigSuspendX11 = SuspendX11);
}

//////////////////////////////////////////////////////////////////////////////
//	cPlayer
//////////////////////////////////////////////////////////////////////////////

/**
**	Dummy player for suspend mode.
*/
class cSoftHdPlayer:public cPlayer
{
  protected:
  public:
    cSoftHdPlayer(void);
    virtual ~ cSoftHdPlayer();
};

cSoftHdPlayer::cSoftHdPlayer(void)
{
}

cSoftHdPlayer::~cSoftHdPlayer()
{
    Detach();
}

//////////////////////////////////////////////////////////////////////////////
//	cControl
//////////////////////////////////////////////////////////////////////////////

/**
**	Dummy control class for suspend mode.
*/
class cSoftHdControl:public cControl
{
  public:
    static cSoftHdPlayer *Player;	///< dummy player
    virtual void Hide(void)		///< hide control
    {
    }
    virtual eOSState ProcessKey(eKeys);	///< process input events

    cSoftHdControl(void);		///< control constructor

    virtual ~ cSoftHdControl();		///< control destructor
};

cSoftHdPlayer *cSoftHdControl::Player;	///< dummy player instance

/**
**	Handle a key event.
**
**	@param key	key pressed
*/
eOSState cSoftHdControl::ProcessKey(eKeys key)
{
    if (SuspendMode == SUSPEND_NORMAL && (!ISMODELESSKEY(key)
	    || key == kMenu || key == kBack || key == kStop)) {
	if (Player) {
	    delete Player;

	    Player = NULL;
	}
	Resume();
	SuspendMode = NOT_SUSPENDED;
	return osEnd;
    }
    return osContinue;
}

/**
**	Player control constructor.
*/
cSoftHdControl::cSoftHdControl(void)
:  cControl(Player = new cSoftHdPlayer)
{
}

/**
**	Player control destructor.
*/
cSoftHdControl::~cSoftHdControl()
{
    if (Player) {
	delete Player;

	Player = NULL;
    }

    dsyslog("[softhddev]%s: dummy player stopped\n", __FUNCTION__);
}

//////////////////////////////////////////////////////////////////////////////
//	cOsdMenu
//////////////////////////////////////////////////////////////////////////////

/**
**	Soft device plugin menu class.
*/
class cSoftHdMenu:public cOsdMenu
{
    int HotkeyState;			///< current hot-key state
    int HotkeyCode;			///< current hot-key code
  public:
     cSoftHdMenu(const char *, int = 0, int = 0, int = 0, int = 0, int = 0);
     virtual ~ cSoftHdMenu();
    virtual eOSState ProcessKey(eKeys);
};

/**
**	Soft device menu constructor.
*/
cSoftHdMenu::cSoftHdMenu(const char *title, int c0, int c1, int c2, int c3,
    int c4)
:cOsdMenu(title, c0, c1, c2, c3, c4)
{
    HotkeyState = 0;

    SetHasHotkeys();
    Add(new cOsdItem(hk(tr("Suspend SoftHdDevice")), osUser1));
}

/**
**	Soft device menu destructor.
*/
cSoftHdMenu::~cSoftHdMenu()
{
}

/**
**	Handle hot key commands.
**
**	@param code	numeric hot key code
*/
static void HandleHotkey(int code)
{
    switch (code) {
	case 10:			// disable pass-through
	    CodecSetAudioPassthrough(ConfigAudioPassthrough = 0);
	    break;
	case 11:			// enable pass-through
	    CodecSetAudioPassthrough(ConfigAudioPassthrough = 1);
	    break;
	case 12:			// toggle pass-through
	    CodecSetAudioPassthrough(ConfigAudioPassthrough ^= 1);
	    break;
	default:
	    esyslog(tr("[softhddev]: hot key %d is not supported\n"), code);
	    break;
    }
}

/**
**	Handle key event.
**
**	@param key	key event
*/
eOSState cSoftHdMenu::ProcessKey(eKeys key)
{
    eOSState state;

    //dsyslog("[softhddev]%s: %x\n", __FUNCTION__, key);

    switch (HotkeyState) {
	case 0:			// initial state, waiting for hot key
	    if (key == kBlue) {
		HotkeyState = 1;
		return osContinue;
	    }
	    break;
	case 1:
	    if (k0 <= key && key <= k9) {
		HotkeyCode = key - k0;
		HotkeyState = 2;
		return osContinue;
	    }
	    HotkeyState = 0;
	    break;
	case 2:
	    if (k0 <= key && key <= k9) {
		HotkeyCode *= 10;
		HotkeyCode += key - k0;
		HotkeyState = 0;
		dsyslog("[softhddev]%s: hot-key %d\n", __FUNCTION__,
		    HotkeyCode);
		HandleHotkey(HotkeyCode);
		return osEnd;
	    }
	    if (key == kOk) {
		HotkeyState = 0;
		dsyslog("[softhddev]%s: hot-key %d\n", __FUNCTION__,
		    HotkeyCode);
		HandleHotkey(HotkeyCode);
		return osEnd;
	    }
	    HotkeyState = 0;
	    break;
    }

    // call standard function
    state = cOsdMenu::ProcessKey(key);

    switch (state) {
	case osUser1:
	    // not already suspended
	    if (SuspendMode == NOT_SUSPENDED && !cSoftHdControl::Player) {
		cControl::Launch(new cSoftHdControl);
		cControl::Attach();
		Suspend(ConfigSuspendClose, ConfigSuspendClose,
		    ConfigSuspendX11);
		SuspendMode = SUSPEND_NORMAL;
		if (ShutdownHandler.GetUserInactiveTime()) {
		    dsyslog("[softhddev]%s: set user inactive\n",
			__FUNCTION__);
		    ShutdownHandler.SetUserInactive();
		}
	    }
	    return osEnd;
	default:
	    break;
    }
    return state;
}

//////////////////////////////////////////////////////////////////////////////
//	cDevice
//////////////////////////////////////////////////////////////////////////////

class cSoftHdDevice:public cDevice
{
  public:
    cSoftHdDevice(void);
    virtual ~ cSoftHdDevice(void);

    virtual bool HasDecoder(void) const;
    virtual bool CanReplay(void) const;
    virtual bool SetPlayMode(ePlayMode);
    virtual void TrickSpeed(int);
    virtual void Clear(void);
    virtual void Play(void);
    virtual void Freeze(void);
    virtual void Mute(void);
    virtual void StillPicture(const uchar *, int);
    virtual bool Poll(cPoller &, int = 0);
    virtual bool Flush(int = 0);
    virtual int64_t GetSTC(void);
    virtual void SetVideoDisplayFormat(eVideoDisplayFormat);
    virtual void SetVideoFormat(bool);
    virtual void GetVideoSize(int &, int &, double &);
    virtual void GetOsdSize(int &, int &, double &);
    virtual int PlayVideo(const uchar *, int);
    virtual int PlayAudio(const uchar *, int, uchar);
#ifdef USE_TS_VIDEO
    virtual int PlayTsVideo(const uchar *, int);
#endif
#if !defined(USE_AUDIO_THREAD) || !defined(NO_TS_AUDIO)
    virtual int PlayTsAudio(const uchar *, int);
#endif
    virtual void SetAudioChannelDevice(int);
    virtual int GetAudioChannelDevice(void);
    virtual void SetDigitalAudioDevice(bool);
    virtual void SetAudioTrackDevice(eTrackType);
    virtual void SetVolumeDevice(int);

// Image Grab facilities

    virtual uchar *GrabImage(int &, bool, int, int, int);

#if 0
// SPU facilities
  private:
    cDvbSpuDecoder * spuDecoder;
  public:
    virtual cSpuDecoder * GetSpuDecoder(void);
#endif

  protected:
    virtual void MakePrimaryDevice(bool);
};

cSoftHdDevice::cSoftHdDevice(void)
{
    //dsyslog("[softhddev]%s\n", __FUNCTION__);

#if 0
    spuDecoder = NULL;
#endif
}

cSoftHdDevice::~cSoftHdDevice(void)
{
    //dsyslog("[softhddev]%s:\n", __FUNCTION__);
}

/**
**	Informs a device that it will be the primary device.
**
**	@param on	flag if becoming or loosing primary
*/
void cSoftHdDevice::MakePrimaryDevice(bool on)
{
    dsyslog("[softhddev]%s: %d\n", __FUNCTION__, on);

    cDevice::MakePrimaryDevice(on);
    if (on) {
	new cSoftOsdProvider();

	if (SuspendMode == SUSPEND_DETACHED) {
	    Resume();
	    SuspendMode = NOT_SUSPENDED;
	}
    } else if (SuspendMode == NOT_SUSPENDED) {
	Suspend(1, 1, 0);
	SuspendMode = SUSPEND_DETACHED;
    }
}

#if 0

cSpuDecoder *cSoftHdDevice::GetSpuDecoder(void)
{
    dsyslog("[softhddev]%s:\n", __FUNCTION__);

    if (IsPrimaryDevice() && !spuDecoder) {
	spuDecoder = new cDvbSpuDecoder();
    }
    return spuDecoder;
}

#endif

/**
**	Tells whether this device has a MPEG decoder.
*/
bool cSoftHdDevice::HasDecoder(void) const
{
    return true;
}

/**
**	Returns true if this device can currently start a replay session.
*/
bool cSoftHdDevice::CanReplay(void) const
{
    return true;
}

/**
**	Sets the device into the given play mode.
**
**	@param play_mode	new play mode (Audio/Video/External...)
*/
bool cSoftHdDevice::SetPlayMode(ePlayMode play_mode)
{
    dsyslog("[softhddev]%s: %d\n", __FUNCTION__, play_mode);

    switch (play_mode) {
	case pmAudioVideo:
	    break;
	case pmAudioOnly:
	case pmAudioOnlyBlack:
	    break;
	case pmVideoOnly:
	    break;
	case pmNone:
	    return true;
	case pmExtern_THIS_SHOULD_BE_AVOIDED:
	    dsyslog("[softhddev] play mode external\n");
	    Suspend(1, 1, 0);
	    SuspendMode = SUSPEND_EXTERNAL;
	    return true;
	default:
	    dsyslog("[softhddev] playmode not implemented... %d\n", play_mode);
	    break;
    }

    if (SuspendMode != NOT_SUSPENDED) {
	if (SuspendMode != SUSPEND_EXTERNAL) {
	    return false;
	}
	Resume();
	SuspendMode = NOT_SUSPENDED;
    }

    return::SetPlayMode(play_mode);
}

/**
**	Gets the current System Time Counter, which can be used to
**	synchronize audio, video and subtitles.
*/
int64_t cSoftHdDevice::GetSTC(void)
{
    //dsyslog("[softhddev]%s:\n", __FUNCTION__);

    return::VideoGetClock();
}

/**
**	Set trick play speed.
**
**	Every single frame shall then be displayed the given number of
**	times.
**
**	@param speed	trick speed
*/
void cSoftHdDevice::TrickSpeed(int speed)
{
    dsyslog("[softhddev]%s: %d\n", __FUNCTION__, speed);

    ::TrickSpeed(speed);
}

/**
**	Clears all video and audio data from the device.
*/
void cSoftHdDevice::Clear(void)
{
    dsyslog("[softhddev]%s:\n", __FUNCTION__);

    cDevice::Clear();
    ::Clear();
}

/**
**	Sets the device into play mode (after a previous trick mode)
*/
void cSoftHdDevice::Play(void)
{
    dsyslog("[softhddev]%s:\n", __FUNCTION__);

    cDevice::Play();
    ::Play();
}

/**
**	Puts the device into "freeze frame" mode.
*/
void cSoftHdDevice::Freeze(void)
{
    dsyslog("[softhddev]%s:\n", __FUNCTION__);

    cDevice::Freeze();
    ::Freeze();
}

/**
**	Turns off audio while replaying.
*/
void cSoftHdDevice::Mute(void)
{
    dsyslog("[softhddev]%s:\n", __FUNCTION__);

    cDevice::Mute();
    ::Mute();
}

/**
**	Display the given I-frame as a still picture.
**
**	@param data	pes or ts data of a frame
**	@param length	length of data area
*/
void cSoftHdDevice::StillPicture(const uchar * data, int length)
{
    dsyslog("[softhddev]%s: %s %p %d\n", __FUNCTION__,
	data[0] == 0x47 ? "ts" : "pes", data, length);

    if (data[0] == 0x47) {		// ts sync
	cDevice::StillPicture(data, length);
	return;
    }

    ::StillPicture(data, length);
}

/**
**	Check if the device is ready for further action.
**
**	@param poller		file handles (unused)
**	@param timeout_ms	timeout in ms to become ready
*/
bool cSoftHdDevice::Poll(
    __attribute__ ((unused)) cPoller & poller, int timeout_ms)
{
    //dsyslog("[softhddev]%s: %d\n", __FUNCTION__, timeout_ms);

    return::Poll(timeout_ms);
}

/**
**	Flush the device output buffers.
**
**	@param timeout_ms	timeout in ms to become ready
*/
bool cSoftHdDevice::Flush(int timeout_ms)
{
    dsyslog("[softhddev]%s: %d ms\n", __FUNCTION__, timeout_ms);

    return::Flush(timeout_ms);
}

// ----------------------------------------------------------------------------

/**
**	Sets the video display format to the given one (only useful if this
**	device has an MPEG decoder).
*/
void cSoftHdDevice:: SetVideoDisplayFormat(eVideoDisplayFormat
    video_display_format)
{
    static int last = -1;

    dsyslog("[softhddev]%s: %d\n", __FUNCTION__, video_display_format);

    cDevice::SetVideoDisplayFormat(video_display_format);

    // called on every channel switch, no need to kill osd...
    if (last != video_display_format) {
	last = video_display_format;

	::VideoSetDisplayFormat(video_display_format);
	cSoftOsd::Dirty = 1;
    }
}

/**
**	Sets the output video format to either 16:9 or 4:3 (only useful
**	if this device has an MPEG decoder).
**
**	Should call SetVideoDisplayFormat.
**
**	@param video_format16_9	flag true 16:9.
*/
void cSoftHdDevice::SetVideoFormat(bool video_format16_9)
{
    dsyslog("[softhddev]%s: %d\n", __FUNCTION__, video_format16_9);

    // FIXME: 4:3 / 16:9 video format not supported.

    SetVideoDisplayFormat(eVideoDisplayFormat(Setup.VideoDisplayFormat));
}

/**
**	Returns the width, height and video_aspect ratio of the currently
**	displayed video material.
**
**	@note the size is used to scale the subtitle.
*/
void cSoftHdDevice::GetVideoSize(int &width, int &height, double &video_aspect)
{
    ::GetOsdSize(&width, &height, &video_aspect);
}

/**
**	Returns the width, height and pixel_aspect ratio the OSD.
**
**	FIXME: Called every second, for nothing (no OSD displayed)?
*/
void cSoftHdDevice::GetOsdSize(int &width, int &height, double &pixel_aspect)
{
    ::GetOsdSize(&width, &height, &pixel_aspect);
}

// ----------------------------------------------------------------------------

/**
**	Play a audio packet.
**
**	@param data	exactly one complete PES packet (which is incomplete)
**	@param length	length of PES packet
**	@param id	type of audio data this packet holds
*/
int cSoftHdDevice::PlayAudio(const uchar * data, int length, uchar id)
{
    //dsyslog("[softhddev]%s: %p %p %d %d\n", __FUNCTION__, this, data, length, id);

    return::PlayAudio(data, length, id);
}

void cSoftHdDevice::SetAudioTrackDevice(
    __attribute__ ((unused)) eTrackType type)
{
    //dsyslog("[softhddev]%s:\n", __FUNCTION__);
}

void cSoftHdDevice::SetDigitalAudioDevice( __attribute__ ((unused)) bool on)
{
    //dsyslog("[softhddev]%s: %s\n", __FUNCTION__, on ? "true" : "false");
}

void cSoftHdDevice::SetAudioChannelDevice( __attribute__ ((unused))
    int audio_channel)
{
    //dsyslog("[softhddev]%s: %d\n", __FUNCTION__, audio_channel);
}

int cSoftHdDevice::GetAudioChannelDevice(void)
{
    //dsyslog("[softhddev]%s:\n", __FUNCTION__);
    return 0;
}

/**
**	Sets the audio volume on this device (Volume = 0...255).
**
**	@param volume	device volume
*/
void cSoftHdDevice::SetVolumeDevice(int volume)
{
    dsyslog("[softhddev]%s: %d\n", __FUNCTION__, volume);

    ::SetVolumeDevice(volume);
}

// ----------------------------------------------------------------------------

/**
**	Play a video packet.
**
**	@param data	exactly one complete PES packet (which is incomplete)
**	@param length	length of PES packet
*/
int cSoftHdDevice::PlayVideo(const uchar * data, int length)
{
    //dsyslog("[softhddev]%s: %p %d\n", __FUNCTION__, data, length);

    return::PlayVideo(data, length);
}

#ifdef USE_TS_VIDEO

///
///	Play a TS video packet.
///
///	@param data	ts data buffer
///	@param length	ts packet length (188)
///
int cSoftHdDevice::PlayTsVideo(const uchar * data, int length)
{
}

#endif

#if !defined(USE_AUDIO_THREAD) || !defined(NO_TS_AUDIO)

///
///	Play a TS audio packet.
///
///	@param data	ts data buffer
///	@param length	ts packet length (188)
///
int cSoftHdDevice::PlayTsAudio(const uchar * data, int length)
{
#ifndef NO_TS_AUDIO
    return::PlayTsAudio(data, length);
#else
    AudioPoller();

    return cDevice::PlayTsAudio(data, length);
#endif
}

#endif

/**
**	Grabs the currently visible screen image.
**
**	@param size	size of the returned data
**	@param jpeg	flag true, create JPEG data
**	@param quality	JPEG quality
**	@param width	number of horizontal pixels in the frame
**	@param height	number of vertical pixels in the frame
*/
uchar *cSoftHdDevice::GrabImage(int &size, bool jpeg, int quality, int width,
    int height)
{
    dsyslog("[softhddev]%s: %d, %d, %d, %dx%d\n", __FUNCTION__, size, jpeg,
	quality, width, height);

    return::GrabImage(&size, jpeg, quality, width, height);
}

/**
**	Call rgb to jpeg for C Plugin.
*/
extern "C" uint8_t * CreateJpeg(uint8_t * image, int *size, int quality,
    int width, int height)
{
    return (uint8_t *) RgbToJpeg((uchar *) image, width, height, *size,
	quality);
}

//////////////////////////////////////////////////////////////////////////////
//	cPlugin
//////////////////////////////////////////////////////////////////////////////

class cPluginSoftHdDevice:public cPlugin
{
  public:
    cPluginSoftHdDevice(void);
    virtual ~ cPluginSoftHdDevice(void);
    virtual const char *Version(void);
    virtual const char *Description(void);
    virtual const char *CommandLineHelp(void);
    virtual bool ProcessArgs(int, char *[]);
    virtual bool Initialize(void);
    virtual bool Start(void);
    virtual void Stop(void);
    // virtual void Housekeeping(void);
    virtual void MainThreadHook(void);
    virtual const char *MainMenuEntry(void);
    virtual cOsdObject *MainMenuAction(void);
    virtual cMenuSetupPage *SetupMenu(void);
    virtual bool SetupParse(const char *, const char *);
//    virtual bool Service(const char *, void * = NULL);
    virtual const char **SVDRPHelpPages(void);
    virtual cString SVDRPCommand(const char *, const char *, int &);
};

cPluginSoftHdDevice::cPluginSoftHdDevice(void)
{
    // Initialize any member variables here.
    // DON'T DO ANYTHING ELSE THAT MAY HAVE SIDE EFFECTS, REQUIRE GLOBAL
    // VDR OBJECTS TO EXIST OR PRODUCE ANY OUTPUT!
    //dsyslog("[softhddev]%s:\n", __FUNCTION__);
}

cPluginSoftHdDevice::~cPluginSoftHdDevice(void)
{
    // Clean up after yourself!
    //dsyslog("[softhddev]%s:\n", __FUNCTION__);

    ::SoftHdDeviceExit();
}

/**
**	Return plugin version number.
**
**	@returns version number as constant string.
*/
const char *cPluginSoftHdDevice::Version(void)
{
    return VERSION;
}

const char *cPluginSoftHdDevice::Description(void)
{
    return tr(DESCRIPTION);
}

/**
**	Return a string that describes all known command line options.
*/
const char *cPluginSoftHdDevice::CommandLineHelp(void)
{
    return::CommandLineHelp();
}

/**
**	Process the command line arguments.
*/
bool cPluginSoftHdDevice::ProcessArgs(int argc, char *argv[])
{
    //dsyslog("[softhddev]%s:\n", __FUNCTION__);

    return::ProcessArgs(argc, argv);
}

bool cPluginSoftHdDevice::Initialize(void)
{
    // Start any background activities the plugin shall perform.
    //dsyslog("[softhddev]%s:\n", __FUNCTION__);

    MyDevice = new cSoftHdDevice();

    return true;
}

/**
**	 Start any background activities the plugin shall perform.
*/
bool cPluginSoftHdDevice::Start(void)
{
    //dsyslog("[softhddev]%s:\n", __FUNCTION__);

    if (!MyDevice->IsPrimaryDevice()) {
	isyslog("[softhddev] softhddevice is not the primary device!");
	if (ConfigMakePrimary) {
	    // Must be done in the main thread
	    dsyslog("[softhddev] makeing softhddevice %d the primary device!",
		MyDevice->DeviceNumber());
	    DoMakePrimary = MyDevice->DeviceNumber() + 1;
	} else {
	    isyslog("[softhddev] softhddevice %d is not the primary device!",
		MyDevice->DeviceNumber());
	}
    }

    if (!::Start()) {
	cControl::Launch(new cSoftHdControl);
	cControl::Attach();
	SuspendMode = SUSPEND_NORMAL;
    }

    return true;
}

void cPluginSoftHdDevice::Stop(void)
{
    //dsyslog("[softhddev]%s:\n", __FUNCTION__);

    ::Stop();
}

#if 0

/**
**	Perform any cleanup or other regular tasks.
*/
void cPluginSoftHdDevice::Housekeeping(void)
{
    dsyslog("[softhddev]%s:\n", __FUNCTION__);

    // ::Housekeeping();
}

#endif

/**
**	Create main menu entry.
*/
const char *cPluginSoftHdDevice::MainMenuEntry(void)
{
    //dsyslog("[softhddev]%s:\n", __FUNCTION__);

    return ConfigHideMainMenuEntry ? NULL : tr(MAINMENUENTRY);
}

/**
**	Perform the action when selected from the main VDR menu.
*/
cOsdObject *cPluginSoftHdDevice::MainMenuAction(void)
{
    //dsyslog("[softhddev]%s:\n", __FUNCTION__);

    return new cSoftHdMenu("SoftHdDevice");
}

/**
**	Called for every plugin once during every cycle of VDR's main program
**	loop.
*/
void cPluginSoftHdDevice::MainThreadHook(void)
{
    //dsyslog("[softhddev]%s:\n", __FUNCTION__);

    if (DoMakePrimary) {
	dsyslog("[softhddev]%s: switching primary device to %d\n",
	    __FUNCTION__, DoMakePrimary);
	cDevice::SetPrimaryDevice(DoMakePrimary);
	DoMakePrimary = 0;
    }
    // check if user is inactive, automatic enter suspend mode
    if (SuspendMode == NOT_SUSPENDED && ShutdownHandler.IsUserInactive()) {
	// don't overwrite already suspended suspend mode
	cControl::Launch(new cSoftHdControl);
	cControl::Attach();
	Suspend(ConfigSuspendClose, ConfigSuspendClose, ConfigSuspendX11);
	SuspendMode = SUSPEND_NORMAL;
    }

    ::MainThreadHook();
}

/**
**	Return our setup menu.
*/
cMenuSetupPage *cPluginSoftHdDevice::SetupMenu(void)
{
    //dsyslog("[softhddev]%s:\n", __FUNCTION__);

    return new cMenuSetupSoft;
}

/**
**	Parse setup parameters
**
**	@param name	paramter name (case sensetive)
**	@param value	value as string
**
**	@returns true if the parameter is supported.
*/
bool cPluginSoftHdDevice::SetupParse(const char *name, const char *value)
{
    int i;

    //dsyslog("[softhddev]%s: '%s' = '%s'\n", __FUNCTION__, name, value);

    if (!strcasecmp(name, "MakePrimary")) {
	ConfigMakePrimary = atoi(value);
	return true;
    }
    if (!strcasecmp(name, "HideMainMenuEntry")) {
	ConfigHideMainMenuEntry = atoi(value);
	return true;
    }
    if (!strcasecmp(name, "Background")) {
	VideoSetBackground(ConfigVideoBackground = strtoul(value, NULL, 0));
	return true;
    }
    if (!strcasecmp(name, "SkipLines")) {
	VideoSetSkipLines(ConfigVideoSkipLines = atoi(value));
	return true;
    }
    if (!strcasecmp(name, "StudioLevels")) {
	VideoSetStudioLevels(ConfigVideoStudioLevels = atoi(value));
	return true;
    }
    if (!strcasecmp(name, "60HzMode")) {
	VideoSet60HzMode(ConfigVideo60HzMode = atoi(value));
	return true;
    }
    if (!strcasecmp(name, "SoftStartSync")) {
	VideoSetSoftStartSync(ConfigVideoSoftStartSync = atoi(value));
	return true;
    }
    for (i = 0; i < RESOLUTIONS; ++i) {
	char buf[128];

	snprintf(buf, sizeof(buf), "%s.%s", Resolution[i], "Scaling");
	if (!strcasecmp(name, buf)) {
	    ConfigVideoScaling[i] = atoi(value);
	    VideoSetScaling(ConfigVideoScaling);
	    return true;
	}
	snprintf(buf, sizeof(buf), "%s.%s", Resolution[i], "Deinterlace");
	if (!strcasecmp(name, buf)) {
	    ConfigVideoDeinterlace[i] = atoi(value);
	    VideoSetDeinterlace(ConfigVideoDeinterlace);
	    return true;
	}
	snprintf(buf, sizeof(buf), "%s.%s", Resolution[i],
	    "SkipChromaDeinterlace");
	if (!strcasecmp(name, buf)) {
	    ConfigVideoSkipChromaDeinterlace[i] = atoi(value);
	    VideoSetSkipChromaDeinterlace(ConfigVideoSkipChromaDeinterlace);
	    return true;
	}
	snprintf(buf, sizeof(buf), "%s.%s", Resolution[i], "InverseTelecine");
	if (!strcasecmp(name, buf)) {
	    ConfigVideoInverseTelecine[i] = atoi(value);
	    VideoSetInverseTelecine(ConfigVideoInverseTelecine);
	    return true;
	}
	snprintf(buf, sizeof(buf), "%s.%s", Resolution[i], "Denoise");
	if (!strcasecmp(name, buf)) {
	    ConfigVideoDenoise[i] = atoi(value);
	    VideoSetDenoise(ConfigVideoDenoise);
	    return true;
	}
	snprintf(buf, sizeof(buf), "%s.%s", Resolution[i], "Sharpen");
	if (!strcasecmp(name, buf)) {
	    ConfigVideoSharpen[i] = atoi(value);
	    VideoSetSharpen(ConfigVideoSharpen);
	    return true;
	}
    }

    if (!strcasecmp(name, "AudioDelay")) {
	VideoSetAudioDelay(ConfigVideoAudioDelay = atoi(value));
	return true;
    }
    if (!strcasecmp(name, "AudioPassthrough")) {
	CodecSetAudioPassthrough(ConfigAudioPassthrough = atoi(value));
	return true;
    }
    if (!strcasecmp(name, "AudioDownmix")) {
	CodecSetAudioDownmix(ConfigAudioDownmix = atoi(value));
	return true;
    }

    if (!strcasecmp(name, "AutoCrop.Interval")) {
	VideoSetAutoCrop(ConfigAutoCropInterval =
	    atoi(value), ConfigAutoCropDelay, ConfigAutoCropTolerance);
	return true;
    }
    if (!strcasecmp(name, "AutoCrop.Delay")) {
	VideoSetAutoCrop(ConfigAutoCropInterval, ConfigAutoCropDelay =
	    atoi(value), ConfigAutoCropTolerance);
	return true;
    }
    if (!strcasecmp(name, "AutoCrop.Tolerance")) {
	VideoSetAutoCrop(ConfigAutoCropInterval, ConfigAutoCropDelay,
	    ConfigAutoCropTolerance = atoi(value));
	return true;
    }

    if (!strcasecmp(name, "Suspend.Close")) {
	ConfigSuspendClose = atoi(value);
	return true;
    }
    if (!strcasecmp(name, "Suspend.X11")) {
	ConfigSuspendX11 = atoi(value);
	return true;
    }
    return false;
}

#if 0

bool cPluginSoftHdDevice::Service(const char *Id, void *Data)
{
    dsyslog("[softhddev]%s:\n", __FUNCTION__);

    return false;
}

#endif

//----------------------------------------------------------------------------
//	cPlugin SVDRP
//----------------------------------------------------------------------------

/**
**	SVDRP commands help text.
**	FIXME: translation?
*/
static const char *SVDRPHelpText[] = {
    "SUSP\n" "   Suspend plugin.\n\n"
	"    The plugin is suspended to save energie. Depending on the setup\n"
	"    'softhddevice.Suspend.Close = 0' only the video and audio output\n"
	"    is stopped or with 'softhddevice.Suspend.Close = 1' the video\n"
	"    and audio devices are closed.\n"
	"    If 'softhddevice.Suspend.X11 = 1' is set and the X11 server was\n"
	"    started by the plugin, the X11 server would also be closed.\n"
	"    (Stopping X11 while suspended isn't supported yet)\n",
    "RESU\n" "   Resume plugin.\n\n"
	"    Resume the suspended plugin. The plugin could be suspended by\n"
	"    the command line option '-s' or by a previous SUSP command.\n"
	"    If the x11 server was stopped by the plugin, it will be\n"
	"    restarted.",
    "DETA\n" "   Detach plugin.\n\n"
	"    The plugin will be detached from the audio, video and DVB\n"
	"    devices.  Other programs or plugins can use them now.\n",
    "ATTA <-d display>\n" "   Attach plugin.\n\n"
	"    Attach the plugin to audio, video and DVB devices.\n"
	"    Use -d display (f.e. -d :0.0) to use another X11 display.\n",
    "PRIM <n>\n" "    Make <n> the primary device.\n\n"
	"    <n> is the number of device. Without number softhddevice becomes\n"
	"    the primary device. If becoming primary, the plugin is attached\n"
	"    to the devices. If loosing primary, the plugin is detached from\n"
	"    the devices.",
    "HOTK key\n" "    Execute hotkey.\n\n"
	"    key is the hotkey number, following are supported:\n"
	"    10: disable audio pass-through\n"
	"    11: enable audio pass-through\n"
	"    12: toggle audio pass-through\n",
    NULL
};

/**
**	Return SVDRP commands help pages.
**
**	return a pointer to a list of help strings for all of the plugin's
**	SVDRP commands.
*/
const char **cPluginSoftHdDevice::SVDRPHelpPages(void)
{
    return SVDRPHelpText;
}

/**
**	Handle SVDRP commands.
**
**	@param command		SVDRP command
**	@param option		all command arguments
**	@param reply_code	reply code
*/
cString cPluginSoftHdDevice::SVDRPCommand(const char *command,
    const char *option, __attribute__ ((unused)) int &reply_code)
{
    if (!strcasecmp(command, "SUSP")) {
	if (cSoftHdControl::Player) {	// already suspended
	    return "SoftHdDevice already suspended";
	}
	if (SuspendMode != NOT_SUSPENDED) {
	    return "SoftHdDevice already detached";
	}
	cControl::Launch(new cSoftHdControl);
	cControl::Attach();
	Suspend(ConfigSuspendClose, ConfigSuspendClose, ConfigSuspendX11);
	SuspendMode = SUSPEND_NORMAL;
	return "SoftHdDevice is suspended";
    }
    if (!strcasecmp(command, "RESU")) {
	if (SuspendMode == NOT_SUSPENDED) {
	    return "SoftHdDevice already resumed";
	}
	if (SuspendMode != SUSPEND_NORMAL) {
	    return "can't resume SoftHdDevice";
	}
	if (ShutdownHandler.GetUserInactiveTime()) {
	    ShutdownHandler.SetUserInactiveTimeout();
	}
	if (cSoftHdControl::Player) {	// suspended
	    cControl::Shutdown();	// not need, if not suspended
	}
	Resume();
	SuspendMode = NOT_SUSPENDED;
	return "SoftHdDevice is resumed";
    }
    if (!strcasecmp(command, "DETA")) {
	if (SuspendMode == SUSPEND_DETACHED) {
	    return "SoftHdDevice already detached";
	}
	if (cSoftHdControl::Player) {	// already suspended
	    return "can't suspend SoftHdDevice already suspended";
	}
	cControl::Launch(new cSoftHdControl);
	cControl::Attach();
	Suspend(1, 1, 0);
	SuspendMode = SUSPEND_DETACHED;
	return "SoftHdDevice is detached";
    }
    if (!strcasecmp(command, "ATTA")) {
	if (SuspendMode != SUSPEND_DETACHED) {
	    return "can't attach SoftHdDevice not detached";
	}
	if ( !strncmp(option, "-d ", 3) ) {
	    // FIXME: loose memory here
	    X11DisplayName = strdup(option + 3);
	} else if ( !strncmp(option, "-d", 2) ) {
	    // FIXME: loose memory here
	    X11DisplayName = strdup(option + 2);
	}
	if (ShutdownHandler.GetUserInactiveTime()) {
	    ShutdownHandler.SetUserInactiveTimeout();
	}
	if (cSoftHdControl::Player) {	// suspended
	    cControl::Shutdown();	// not need, if not suspended
	}
	Resume();
	SuspendMode = NOT_SUSPENDED;
	return "SoftHdDevice is attached";
    }
    if (!strcasecmp(command, "HOTK")) {
	int hotk;

	hotk = strtol(option, NULL, 0);
	HandleHotkey(hotk);
	return "hot-key executed";
    }
    if (!strcasecmp(command, "PRIM")) {
	int primary;

	primary = strtol(option, NULL, 0);
	if (!primary && MyDevice) {
	    primary = MyDevice->DeviceNumber() + 1;
	}
	dsyslog("[softhddev] switching primary device to %d\n", primary);
	DoMakePrimary = primary;
	return "switching primary device requested";
    }
    return NULL;
}

VDRPLUGINCREATOR(cPluginSoftHdDevice);	// Don't touch this!
