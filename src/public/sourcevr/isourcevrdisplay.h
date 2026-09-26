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
	enum EOutput
	{
		OUTPUT_SIDE_BY_SIDE = 0,	// both eyes in one frame, left eye first
	};

	// True while the two eyes are being shown on a stereoscopic display.
	virtual bool IsDisplayActive() = 0;

	virtual EOutput GetOutput() = 0;
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

inline bool UseVRDisplay()
{
	if ( !UseVR() )
		return false;
	ISourceVRDisplay *pDisplay = SourceVRDisplay();
	return pDisplay != NULL && pDisplay->IsDisplayActive();
}

#endif // ISOURCEVRDISPLAY_H
