// The 3D section of Half-Life 2's GamepadUI options (gamepadui/options.res,
// from hl2/hl2_pak_dir.vpk). Insert it in the "Video" tab's "items", right
// after the "DisplayMode" item, and load the result as a loose override, e.g.
//   Half-Life 2/hl2/custom/stereo3d-menu/gamepadui/options.res
// The items bind the module's convars; none is "instantapply", so every
// change waits for the Apply button, which reloads 3D once with all of them.
// "vr_display_native" and "vr_display_gamescope" are set by the module at
// start, so each path offers only what it can show.
			"Stereo3D"
			{
				"text"			"Stereo 3D"
				"type"			"wheelywheel"
				"convar"		"vr_display_3d"

				"options"
				{
					"0"		"#gameui_disabled"
					"1"		"#gameui_enabled"
				}
			}

			"Stereo3DFormat"
			{
				"text"			"3D format"
				"type"			"wheelywheel"
				"convar"		"vr_display_layout"
				"depends_on"	"vr_display_native"

				"options"
				{
					"1"		"Top and bottom (recommended)"
					"0"		"Side by side"
				}
			}

			"Stereo3DFormatGamescope"
			{
				"text"			"3D format"
				"type"			"wheelywheel"
				"convar"		"vr_display_layout"
				"depends_on"	"vr_display_gamescope"

				"options"
				{
					"1"		"Top and bottom (recommended)"
					"0"		"Side by side"
					"2"		"Top and bottom, full"
					"3"		"Side by side, full"
				}
			}

			"Stereo3DOutput"
			{
				"text"			"3D output"
				"type"			"wheelywheel"
				"convar"		"vr_display_output"
				"depends_on"	"vr_display_native"

				"options"
				{
					"0"		"3D display"
				}
			}

			"Stereo3DOutputGamescope"
			{
				"text"			"3D output"
				"type"			"wheelywheel"
				"convar"		"vr_display_output"
				"depends_on"	"vr_display_gamescope"

				"options"
				{
					"0"		"3D display"
					"1"		"Anaglyph (CRT)"
					"2"		"Anaglyph (modern screens)"
					"3"		"Row-interleaved (passive screens)"
					"4"		"Checkerboard (DLP)"
				}
			}

			"Stereo3DSwapEyes"
			{
				"text"			"Swap eyes"
				"type"			"wheelywheel"
				"convar"		"vr_display_swap"
				"depends_on"	"vr_display_3d"

				"options"
				{
					"0"		"#gameui_disabled"
					"1"		"#gameui_enabled"
				}
			}
