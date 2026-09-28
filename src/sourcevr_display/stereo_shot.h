//========= Stereo screenshots for the stereoscopic display module =========//
//
// One stereo pair, written in the formats people and devices read (Daniel,
// 2026-09-28): the master is Valve's own stereo screenshot type, the one
// SteamVR's compositor writes and Steam files as a stereo shot (side by side,
// left eye on the left, full size per eye, taken before any lens), with the
// left eye as its 2D preview. Beside it: JPS (side by side, right eye on the
// left, the cross-view convention JPS viewers expect), MPO (CIPA DC-007, both
// views typed Disparity, numbered from the left, with the recommended base
// viewpoint, convergence angle and baseline length), and, when the player
// uses anaglyph, the anaglyph they saw (the same colour mixes as the effect).
//
// No engine dependencies: the module reads the eyes back and hands them over;
// this writes the files.
//===========================================================================//

#ifndef STEREO_SHOT_H
#define STEREO_SHOT_H

#include <stddef.h>
#include <time.h>

enum StereoShotAnaglyph { STEREO_SHOT_NO_ANAGLYPH = 0, STEREO_SHOT_ANAGLYPH_CRT, STEREO_SHOT_ANAGLYPH_MODERN };

struct StereoShot {
	int w, h;                        // one eye
	const unsigned char *eye[2];     // left, right: BGRA, top row first, w * 4 bytes a row
	const char *base;                // path without extension: base_stereo.png, base_left.png, base.jps, base.mpo, base_anaglyph.jpg
	int anaglyph;                    // StereoShotAnaglyph
	double baselineMetres;           // distance between the two viewpoints
	double convergenceDegrees;       // angle between the lines of sight (0: parallel cameras)
	const char *make, *model;        // Exif: what wrote the file (never a camera it is not)
	time_t when;
};

struct StereoShotFiles {
	char stereo[1100];      // Valve's stereo type: side by side, left eye on the left (PNG)
	char preview[1100];     // the left eye (PNG), the 2D view in the library
	char jps[1100];
	char mpo[1100];
	char anaglyph[1100];    // empty unless requested
};

// Writes every file; false with a message in err when any failed (the rest
// are still written).
bool stereo_shot_write(const StereoShot &shot, StereoShotFiles *files, char *err, size_t errSize);

#endif // STEREO_SHOT_H
