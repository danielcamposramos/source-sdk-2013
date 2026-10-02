// The 3D section of Half-Life 2's GamepadUI options (gamepadui/options.res,
// from hl2/hl2_pak_dir.vpk). Insert it in the "Video" tab's "items", right
// after the "DisplayMode" item, and load the result as a loose override, e.g.
//   Half-Life 2/hl2/custom/stereo3d-menu/gamepadui/options.res
// The items bind the module's convars; none is "instantapply", so every
// change waits for the Apply button, which reloads 3D once with all of them.
// One entry per setting: a hidden entry bound to the same convar wrote its
// old value back over the visible one on Apply (run q25, native). The 3D
// output wheel shows only inside gamescope (vr_display_gamescope, set by the
// module at start); natively the output is always the 3D display.
// When the system draws the eyes (vr_display_system: the desktop shows them in
// its own 3D mode), the format and the output are the desktop's, so the menu
// shows the render resolution per eye instead (applied at the next start).
// Swap eyes shows in every case (Daniel, 2026-10-02).
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
				"depends_on"	"vr_display_packs"

				"options"
				{
					"2"		"Top and bottom, full (recommended)"
					"3"		"Side by side, full"
					"1"		"Top and bottom"
					"0"		"Side by side"
				}
			}

			"Stereo3DRender"
			{
				"text"			"Render resolution per eye (next start)"
				"type"			"wheelywheel"
				"convar"		"vr_display_render"
				"depends_on"	"vr_display_system"

				"options"
				{
					"2"		"1920x1080 (recommended for 3D TVs)"
					"1"		"1600x900"
					"0"		"1280x720 (faster)"
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
