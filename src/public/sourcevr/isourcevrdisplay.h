//===========================================================================//
//
// Purpose: Optional companion to ISourceVirtualReality for VR modules that
//          drive a stereoscopic display (3D TV, projector, monitor)
//          instead of, or as well as, a headset.
//
//          It is served through the module's own QueryInterface, so the
//          existing SourceVirtualReality001 interface is unchanged. A module
//          without it answers NULL, and UseVRDisplay() is false.
//
//===========================================================================//

#ifndef ISOURCEVRDISPLAY_H
#define ISOURCEVRDISPLAY_H

#ifdef _WIN32
#pragma once
#endif

#include "sourcevr/isourcevirtualreality.h"

#define SOURCE_VR_DISPLAY_INTERFACE_VERSION "SourceVirtualRealityDisplay001"

abstract_class ISourceVRDisplay
{
public:
	// True while the stereoscopic display is the headset: the eyes are
	// rendered for it (play, with no headset connected). False when a real
	// headset is connected and the display only mirrors its eyes (spectate):
	// then the client keeps every headset behaviour (its 640x480 UI panel,
	// the in-world HUD), and the module composes the display's view itself.
	virtual bool IsDisplayTheHeadset() = 0;
};

//-----------------------------------------------------------------------------
// The display interface of the loaded VR module, or NULL.
//-----------------------------------------------------------------------------
inline ISourceVRDisplay *SourceVRDisplay()
{
	if ( !g_pSourceVR )
		return NULL;
	return (ISourceVRDisplay *)g_pSourceVR->QueryInterface( SOURCE_VR_DISPLAY_INTERFACE_VERSION );
}

// True while the game renders for a stereoscopic display instead of a headset.
inline bool UseVRDisplay()
{
	if ( !UseVR() )
		return false;
	ISourceVRDisplay *pDisplay = SourceVRDisplay();
	return pDisplay != NULL && pDisplay->IsDisplayTheHeadset();
}

#endif // ISOURCEVRDISPLAY_H
