// VR Stereo Spectator: sourcevr for 3D televisions.
//
// A replacement for the Source engine's sourcevr module (interface
// "SourceVirtualReality001", Source SDK 2013). The engine's own VR path
// renders each eye with the geometry this module supplies, into the
// viewport this module chooses; this module presents a stereoscopic
// television as the "headset": fixed pose, no lens distortion, both eyes
// packed side by side or top and bottom in the game's own window, which the
// television unpacks.
//
// Geometry: parallel eye cameras with an off-centre frustum, so both eyes
// share one screen plane at the convergence distance (no toe-in, no vertical
// parallax). Source coordinates are x forward, y left, z up; the projection
// convention follows mathlib's MatrixBuildPerspectiveX (view looks down -z,
// depth 0..1).
//
// Configuration: KEY=VALUE lines in svrtv.ini next to this module (so a
// benchmark suite can switch steps by rewriting one file), overridden by the
// same names in the environment (Steam launch options accept
// "VAR=value %command%"):
//   SVRTV_LAYOUT       sbs or tab; unset, the module stays inert and the game
//                      runs in 2D exactly as with Valve's module
//   SVRTV_WIDTH/HEIGHT output size in pixels (default 1920x1080; match -w/-h)
//   SVRTV_ASPECT       displayed aspect, default WIDTH/HEIGHT
//   SVRTV_SEPARATION   eye separation in game units (default 2.5, about 64 mm)
//   SVRTV_CONVERGENCE  distance of the screen plane in game units (default 120)
//   SVRTV_SWAP         1 swaps the eyes
//   SVRTV_LOG          path of a log file (default: stderr only)
//   SVRTV_ANGLELOG     1 records the engine's view angles and a timestamp for
//                      every frame, in memory, written to svrtv-angles.tsv next
//                      to this module when the game exits (diagnostics)
//   SVRTV_LOADINGSCREEN 0 leaves loading screens as the game draws them
//                      (default 1: whole in both eyes inside gamescope,
//                      stereo art natively)
//   SVRTV_CALLTRACE    1 logs every gap over 200 ms between the calls the
//                      module receives, and the calls made while the engine
//                      draws its loading image (diagnostics)
//   SVRTV_SCREENSHOTS  0 leaves screenshots to Steam and the engine as in 2D
//                      (default 1: while 3D is on, every screenshot is a
//                      stereo one, from both eyes at their full size, before
//                      packing and anaglyph; see stereo_shot.h)
//
// In the game, 3D is switched on by any of: -stereo3d on the command line (a
// launch option, in the saved format; -stereo3d tab|sbs forces one), the saved settings the
// video options bind (vr_display_3d 0/1, vr_display_layout 0 side by side /
// 1 top and bottom, vr_display_output 0 the 3D display / 1 anaglyph for CRTs
// / 2 anaglyph for modern screens, applied together on Apply), or
// SVRTV_LAYOUT. Inside gamescope the anaglyph outputs switch gamescope's own
// effect at run time; outside it they fall back to the 3D display.
//
// Builds for 32-bit (today's native HL2) and 64-bit Source games alike.

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdint.h>
#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>
#include <vector>

#include "sourcevr/isourcevirtualreality.h"
#include "materialsystem/imaterialsystem.h"
#include "materialsystem/itexture.h"
#include "materialsystem/imaterial.h"
#include "materialsystem/imaterialvar.h"
#include "materialsystem/materialsystem_config.h"
#include "cdll_int.h"
#include "tier1/tier1.h"
#include "tier1/convar.h"
#include "tier0/icommandline.h"
#include "sourcevr/isourcevrdisplay.h"
#include "vgui/IInput.h"
#include "vgui/ISurface.h"
#include "steam/isteamremotestorage.h"   // PublishedFileId_t, used by isteamscreenshots.h
#include "steam/isteamscreenshots.h"
#include "stereo_shot.h"
#ifdef _WIN32
#include "winlite.h"
#include <direct.h>
#else
#include <dlfcn.h>
#include <sys/time.h>
#include <sys/stat.h>

// dlsym under its original symbol version, which every glibc still exports;
// the default one (GLIBC_2.34) is newer than Steam's runtimes.
#if defined(__x86_64__)
__asm__(".symver dlsym,dlsym@GLIBC_2.2.5");
__asm__(".symver dlopen,dlopen@GLIBC_2.2.5");
#else
__asm__(".symver dlsym,dlsym@GLIBC_2.0");
__asm__(".symver dlopen,dlopen@GLIBC_2.1");
#endif
#endif

namespace {

// 3D formats: how the two eyes are packed into the frame. The values are the
// ones vr_display_layout saves, so the order is fixed.
enum Format {
	FMT_SBS = 0,       // side by side, each eye half the width
	FMT_TAB,           // top and bottom, each eye half the height (the default)
	FMT_TAB_FULL,      // top and bottom, each eye the full 2D size
	FMT_SBS_FULL,      // side by side, each eye the full 2D size
	FMT_FP1080,        // HDMI 1.4 frame packing, 1920x2205 (a 45-line gap)
	FMT_FP720,         // HDMI 1.4 frame packing, 1280x1470 (a 30-line gap)
	FMT_COUNT
};
static const char *g_formatNames[FMT_COUNT] = { "sbs", "tab", "tabfull", "sbsfull", "fp1080", "fp720" };
int format_from_name(const char *name)
{
	for (int i = 0; name && i < FMT_COUNT; i++)
		if (!strcmp(name, g_formatNames[i]))
			return i;
	return -1;
}

// 3D outputs: what the screen needs. The ones past OUT_DISPLAY combine the
// eyes pixel by pixel, which gamescope's effect does. OUT_SYSTEM hands both
// eyes to the system (KWin with the driver_stereodisplay package), which
// sends them to the screen's 3D mode or mixes them as anaglyph: the frame is
// two full 2D-size eyes side by side, packed for nothing (Daniel, 2026-10-01:
// games are side by side, as VR is).
enum Output { OUT_DISPLAY = 0, OUT_ANAGLYPH_CRT, OUT_ANAGLYPH_MODERN, OUT_ROWS, OUT_CHECKERBOARD, OUT_SYSTEM, OUT_COUNT };

struct Config {
	bool enabled;   // 3D requested (SVRTV_LAYOUT set); otherwise fully inert
	bool layoutGiven; // the format came with the request (SVRTV_LAYOUT, -stereo3d <format>)
	int format;       // Format
	int output;       // Output
	int outputForced; // SVRTV_OUTPUT: the output for this run, never saved (-1: the saved one)
	bool sizeGiven;   // SVRTV_WIDTH/HEIGHT set; otherwise the 2D size is the video mode's
	int width, height;
	double aspect;
	double separation;
	double convergence;
	bool swap;
	bool hudcopy;   // test: paste the HUD sheet with a plain copy
	bool hudwide;   // HUD across the full width (default; SVRTV_HUD43=1 keeps the sheet's 4:3)
	char hudtex[64];   // test: texture the HUD paste samples
	char hudmat[64];   // material the HUD paste draws with
	double hudband;   // HUD bottom band moved to the top (fraction of the sheet; 0 = off)
	int dump;         // diagnostics: write the HUD sheet and the frame to files at this frame
	int eyew, eyeh;   // eye render size (SVRTV_EYE=WxH); 0: its half of the frame
	bool latecopy;    // both eyes into the frame together at the end of the frame
	int dumpevery;    // diagnostics: after SVRTV_DUMP, a frame every n frames
	bool calltrace;   // diagnostics: log the gaps between the calls the module receives
	bool loading;     // gamescope: frames marked, loading screens whole in both eyes
	bool shots;       // stereo screenshots while 3D is on
	bool xhair;       // the module draws the crosshair on the 2D layer (client's off)
	bool bluroff;     // motion blur off while VR is on, the player's setting restored after
	int aniso;        // mat_forceaniso while VR is on (0 leaves it), the player's setting restored after
	char onvr[512];   // console commands issued when VR starts
	bool mouselog;    // diagnostics: log the system pointer next to the UI cursor
	bool anglelog;    // diagnostics: view angles and time of every frame
	bool confine;     // keep the system pointer on the UI's area when the window is bigger
	FILE *log;
};

Config g_cfg;

// svrtv.ini next to this module: up to 32 KEY=VALUE lines.
char g_ini[32][2][128];
int g_nini;
char g_dir[1024];   // this module's directory, with the trailing slash

#ifdef _WIN32
// This module's own path, from the loader.
bool module_path(char *out, size_t size)
{
	HMODULE self = NULL;
	if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
			(LPCSTR)&module_path, &self))
		return false;
	DWORD n = GetModuleFileNameA(self, out, (DWORD)size);
	return n > 0 && n < size;
}
#else
// Hex without strtoul/sscanf, which current glibc redirects to its
// C23 variants (GLIBC_2.38) under _GNU_SOURCE.
uintptr_t parse_hex(const char **c)
{
	uintptr_t v = 0;
	for (;; (*c)++) {
		char h = **c;
		int d = (h >= '0' && h <= '9') ? h - '0' : (h >= 'a' && h <= 'f') ? h - 'a' + 10 : -1;
		if (d < 0)
			return v;
		v = v * 16 + d;
	}
}

// Finds this module's own path in /proc/self/maps (plain stdio, so no
// newer-glibc symbol such as dladdr@GLIBC_2.34 is pulled in).
bool module_path(char *out, size_t size)
{
	FILE *m = fopen("/proc/self/maps", "r");
	if (!m)
		return false;
	uintptr_t self = (uintptr_t)&module_path;
	char line[1200];
	bool found = false;
	while (fgets(line, sizeof(line), m)) {
		const char *c = line;
		uintptr_t lo = parse_hex(&c);
		if (*c++ != '-')
			continue;
		uintptr_t hi = parse_hex(&c);
		if (self < lo || self >= hi)
			continue;
		char *p = strchr(line, '/');
		if (p) {
			p[strcspn(p, "\n")] = 0;
			snprintf(out, size, "%s", p);
			found = true;
		}
		break;
	}
	fclose(m);
	return found;
}
#endif

// GamepadUI (the menu of Steam Deck and Big Picture, or -gamepadui) lays
// its menus out in the window's real pixels and ignores the client's 640x480
// UI size for VR, so in 3D its menus showed cropped (Daniel, 2026-09-26).
bool gamepadui_loaded()
{
	// Decided at start the way the launcher and engine decide it: -gamepadui,
	// or a Steam Deck (SteamDeck=1), unless -nogamepadui. The module makes its
	// UI sheet before the client loads gamepadui.so, so looking for the
	// loaded library came too early (run q04).
	static int loaded = -1;
	if (loaded < 0) {
		const char *deck = getenv("SteamDeck");
		loaded = !CommandLine()->CheckParm("-nogamepadui") &&
			(CommandLine()->CheckParm("-gamepadui") || (deck && atoi(deck) == 1));
	}
	return loaded != 0;
}

void load_ini()
{
	char path[1024];
	if (!module_path(path, sizeof(path)))
		return;
	char *slash = strrchr(path, '/');
#ifdef _WIN32
	char *backslash = strrchr(path, '\\');
	if (backslash && (!slash || backslash > slash))
		slash = backslash;
#endif
	if (!slash)
		return;
	slash[1] = 0;
	snprintf(g_dir, sizeof(g_dir), "%s", path);
	snprintf(slash + 1, sizeof(path) - (slash + 1 - path), "svrtv.ini");
	FILE *f = fopen(path, "r");
	if (!f)
		return;
	char line[300];
	while (g_nini < 32 && fgets(line, sizeof(line), f)) {
		char *eq = strchr(line, '=');
		if (line[0] == '#' || !eq)
			continue;
		*eq = 0;
		char *val = eq + 1;
		val[strcspn(val, "\r\n")] = 0;
		snprintf(g_ini[g_nini][0], 128, "%s", line);
		snprintf(g_ini[g_nini][1], 128, "%s", val);
		g_nini++;
	}
	fclose(f);
}

// The environment wins over svrtv.ini.
const char *setting(const char *name)
{
	const char *v = getenv(name);
	if (v && *v)
		return v;
	for (int i = 0; i < g_nini; i++)
		if (!strcmp(g_ini[i][0], name))
			return g_ini[i][1];
	return NULL;
}

double env_double(const char *name, double def)
{
	const char *v = setting(name);
	return (v && *v) ? atof(v) : def;
}

void logf(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	fputs("[vr-stereo-spectator] ", stderr);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	if (g_cfg.log) {
		va_start(ap, fmt);
		vfprintf(g_cfg.log, fmt, ap);
		va_end(ap);
		fflush(g_cfg.log);
	}
}

// Angle log (SVRTV_ANGLELOG=1): one row per frame, taken where the engine
// starts a frame (SampleTrackingState, before either eye renders). Kept in
// memory so the measurement does not disturb the timing it measures; written
// when the game exits, or when the buffer fills. Built to compare mouse-turn
// smoothness natively and through gamescope (2026-09-25).
struct AngleRow { double t; float yaw, pitch; int frame; };
enum { ANGLE_MAX = 262144 };
static AngleRow g_ang[ANGLE_MAX];
static int g_nang;
static int g_angleWrites;

static void write_angles()
{
	if (g_nang <= 0)
		return;
	char path[1200];
	snprintf(path, sizeof(path), "%ssvrtv-angles.tsv", g_dir);
	FILE *f = fopen(path, g_angleWrites ? "a" : "w");
	if (!f)
		return;
	if (!g_angleWrites)
		fputs("frame\tt_ms\tdt_ms\tyaw\tpitch\tdyaw\tdyaw_per_s\n", f);
	for (int i = 0; i < g_nang; i++) {
		const AngleRow &r = g_ang[i];
		double dt = 0, dyaw = 0;
		if (i > 0) {
			dt = (r.t - g_ang[i - 1].t) * 1000.0;
			dyaw = r.yaw - g_ang[i - 1].yaw;
			while (dyaw > 180.0) dyaw -= 360.0;
			while (dyaw < -180.0) dyaw += 360.0;
		}
		fprintf(f, "%d\t%.3f\t%.3f\t%.4f\t%.4f\t%.4f\t%.2f\n", r.frame, (r.t - g_ang[0].t) * 1000.0,
		        dt, r.yaw, r.pitch, dyaw, dt > 0 ? dyaw / (dt / 1000.0) : 0.0);
	}
	fclose(f);
	logf("angle log: %d frames written to %s\n", g_nang, path);
	g_angleWrites++;
	g_nang = 0;
}

#ifdef _WIN32
// MSVC has no destructor attribute: a static object's destructor runs at
// the same point, when the module is unloaded or the game exits.
struct AngleLogAtExit
{
	~AngleLogAtExit() { write_angles(); }
} g_angleLogAtExit;
#else
__attribute__((destructor)) static void write_angles_at_exit()
{
	write_angles();
}
#endif

void load_config()
{
	load_ini();
	const char *layout = setting("SVRTV_LAYOUT");
	g_cfg.enabled = layout && *layout;
	g_cfg.format = FMT_TAB;
	g_cfg.layoutGiven = false;
	if (g_cfg.enabled && format_from_name(layout) >= 0) {
		g_cfg.format = format_from_name(layout);
		g_cfg.layoutGiven = true;
	}
	g_cfg.output = OUT_DISPLAY;
	// The bench's output for one run (SVRTV_OUTPUT=system or a number): the
	// menu's saved output is left as the player set it.
	const char *output = setting("SVRTV_OUTPUT");
	g_cfg.outputForced = -1;
	if (output && !strcmp(output, "system"))
		g_cfg.outputForced = OUT_SYSTEM;
	else if (output && *output >= '0' && *output <= '9' && atoi(output) < OUT_COUNT)
		g_cfg.outputForced = atoi(output);
	if (g_cfg.outputForced >= 0)
		g_cfg.output = g_cfg.outputForced;
	// The launch option, e.g. Steam's "3D SBS Vulkan": -vulkan -stereo3d, in
	// the saved format (read at startup); -stereo3d <format> forces one
	// (sbs, tab, tabfull, sbsfull, fp1080, fp720).
	const char *layout3d = NULL;
	if (CommandLine()->CheckParm("-stereo3d", &layout3d)) {
		g_cfg.enabled = true;
		if (format_from_name(layout3d) >= 0) {
			g_cfg.format = format_from_name(layout3d);
			g_cfg.layoutGiven = true;
		}
	}
	// The 2D size: the game's own video mode (read when 3D starts), unless
	// the bench gives one.
	g_cfg.sizeGiven = setting("SVRTV_WIDTH") && setting("SVRTV_HEIGHT");
	g_cfg.width = (int)env_double("SVRTV_WIDTH", 1920);
	g_cfg.height = (int)env_double("SVRTV_HEIGHT", 1080);
	g_cfg.aspect = env_double("SVRTV_ASPECT", (double)g_cfg.width / g_cfg.height);
	g_cfg.separation = env_double("SVRTV_SEPARATION", 2.5);
	g_cfg.convergence = env_double("SVRTV_CONVERGENCE", 120.0);
	g_cfg.swap = env_double("SVRTV_SWAP", 0) != 0;
	g_cfg.hudcopy = env_double("SVRTV_HUDCOPY", 0) != 0;
	g_cfg.hudwide = env_double("SVRTV_HUD43", 0) == 0;
	// HL2's health and ammo row fills sheet rows 432-467 of 480, 12 rows
	// above the bottom edge (dump, 2026-09-24); a band of 60 rows (0.125)
	// puts it 12 rows below the top edge, the same margin (Daniel's ask).
	g_cfg.hudband = env_double("SVRTV_HUDTOP", 0.125);
	if (g_cfg.hudband < 0 || g_cfg.hudband > 0.45)
		g_cfg.hudband = 0;
	g_cfg.dump = (int)env_double("SVRTV_DUMP", 0);
	g_cfg.mouselog = env_double("SVRTV_MOUSELOG", 0) != 0;
	g_cfg.anglelog = env_double("SVRTV_ANGLELOG", 0) != 0;
	g_cfg.confine = env_double("SVRTV_CONFINE", 1) != 0;
	g_cfg.latecopy = env_double("SVRTV_LATECOPY", 0) != 0;
	g_cfg.dumpevery = (int)env_double("SVRTV_DUMPEVERY", 0);
	g_cfg.eyew = g_cfg.eyeh = 0;
	const char *ov = setting("SVRTV_ONVR");
	// A television has no head tracking: the view follows the game (mode 7,
	// the SDK's HMM_SHOOTMOVELOOKMOUSE); then Valve's own VR settings for
	// Half-Life 2 (hl2/cfg/sourcevr_hl2.cfg, which this build never runs).
	snprintf(g_cfg.onvr, sizeof(g_cfg.onvr), "%s", ov ? ov :
		"vr_moveaim_mode 7;vr_moveaim_mode_zoom 7;vr_first_person_uses_world_model 0;hud_draw_fixed_reticle 0;r_flashlightscissor 0");
	const char *es = setting("SVRTV_EYE");
	if (es && *es) {
		const char *c = es;
		int w = 0, h = 0;
		while (*c >= '0' && *c <= '9') w = w * 10 + (*c++ - '0');
		if (*c == 'x' || *c == 'X') {
			c++;
			while (*c >= '0' && *c <= '9') h = h * 10 + (*c++ - '0');
		}
		if (w >= 64 && h >= 64 && w <= g_cfg.width && h <= g_cfg.height) {
			g_cfg.eyew = w;
			g_cfg.eyeh = h;
		}
	}
	// The client's own crosshair is painted for a 640x480 screen and lands
	// off-centre in the eyes, so the module draws HL2's crosshair on the 2D
	// layer, once per eye (SVRTV_CROSSHAIR=0 leaves the client's).
	g_cfg.xhair = env_double("SVRTV_CROSSHAIR", 1) != 0;
	// Diagnostics: every gap over 200 ms between the calls the module
	// receives, and the calls made while the engine draws its loading
	// image (does the module get any frame during a load?).
	g_cfg.calltrace = env_double("SVRTV_CALLTRACE", 0) != 0;
	// Loading screens: whole in both eyes inside gamescope (mark_frame),
	// stereo art natively (loading_startup); 0 leaves them as the game draws
	// them.
	g_cfg.loading = env_double("SVRTV_LOADINGSCREEN", 1) != 0;
	// Screenshots while 3D is on: both eyes, full size, in Valve's stereo
	// type plus JPS, MPO and the anaglyph when that is the output (Daniel,
	// 2026-09-28: "video capture in 3D is a must", screenshots first).
	g_cfg.shots = env_double("SVRTV_SCREENSHOTS", 1) != 0;
	// Source keeps motion blur's previous view in statics shared by both eyes
	// (viewpostprocess.cpp), so in stereo each eye blurs differently during a
	// turn; with it on, the mouse felt wrecked (Daniel, run p20, 2026-09-26).
	g_cfg.bluroff = env_double("SVRTV_BLUROFF", 1) != 0;
	g_cfg.aniso = (int)env_double("SVRTV_ANISO", 16);
	const char *hm = setting("SVRTV_HUDMAT");
	snprintf(g_cfg.hudmat, sizeof(g_cfg.hudmat), "%s", (hm && *hm) ? hm : "vgui/icon_con_grey");
	const char *ht = setting("SVRTV_HUDTEX");
	snprintf(g_cfg.hudtex, sizeof(g_cfg.hudtex), "%s", ht ? ht : "");
	// A relative log path is taken from this module's directory: Steam runs
	// the game in a runtime container that sees the game's folders but not
	// necessarily the caller's.
	const char *lp = setting("SVRTV_LOG");
	if (lp && *lp) {
		char full[1200];
#ifdef _WIN32
		snprintf(full, sizeof(full), "%s%s", (lp[0] == '/' || lp[0] == '\\' || (lp[0] && lp[1] == ':')) ? "" : g_dir, lp);
#else
		snprintf(full, sizeof(full), "%s%s", lp[0] == '/' ? "" : g_dir, lp);
#endif
		g_cfg.log = fopen(full, "a");
		// An absolute path outside what the container shares cannot be
		// opened; log next to the module instead.
		if (!g_cfg.log) {
			snprintf(full, sizeof(full), "%ssvrtv.log", g_dir);
			g_cfg.log = fopen(full, "a");
		}
	}
	if (g_cfg.convergence <= 0)
		g_cfg.convergence = 120.0;
	logf("config (%d lines from svrtv.ini): enabled=%d layout=%s %dx%d aspect=%.4f separation=%.3f convergence=%.1f swap=%d\n",
	     g_nini, (int)g_cfg.enabled, g_formatNames[g_cfg.format], g_cfg.width, g_cfg.height, g_cfg.aspect,
	     g_cfg.separation, g_cfg.convergence, (int)g_cfg.swap);
}

// +1 for the left eye, -1 for the right eye. The cameras never swap;
// SVRTV_SWAP only changes which half each eye is packed into.
double eye_sign(ISourceVirtualReality::VREye eye)
{
	return (eye == ISourceVirtualReality::VREye_Left) ? 1.0 : -1.0;
}

void set_identity(VMatrix &m)
{
	for (int i = 0; i < 4; i++)
		for (int j = 0; j < 4; j++)
			m.m[i][j] = (i == j) ? 1.0f : 0.0f;
}

static void display_changed(IConVar *var, const char *oldValue, float oldFloat);

// Saved with the player's settings. The video options bind them and apply
// them together on Apply; so does the console.
static ConVar vr_display_3d("vr_display_3d", "0", FCVAR_ARCHIVE,
	"Stereo 3D on a 3D display: 0 disabled, 1 enabled (engage the display's own 3D mode)",
	true, 0, true, 1, display_changed);
// Top and bottom by default: each eye keeps the full width, the axis stereo
// depth lives on (Daniel's choice, 2026-09-26).
static ConVar vr_display_layout("vr_display_layout", "2", FCVAR_ARCHIVE,
	"3D format: 2 top and bottom full (recommended: full-size eyes, supersampled), 3 side by side full, 1 top and bottom, 0 side by side (lighter), 4 frame packing 1080p, 5 frame packing 720p",
	true, 0, true, FMT_COUNT - 1, display_changed);
static ConVar vr_display_output("vr_display_output", "0", FCVAR_ARCHIVE,
	"3D output: 0 the 3D display, 1 red/cyan anaglyph for CRTs, 2 red/cyan anaglyph for modern screens, 3 row-interleaved (passive screens), 4 checkerboard (DLP), 5 the system (both eyes at full size; the desktop sends them to the screen)",
	true, 0, true, OUT_COUNT - 1, display_changed);
static ConVar vr_display_swap("vr_display_swap", "0", FCVAR_ARCHIVE,
	"Swap the eyes: 0 left eye first, 1 right eye first",
	true, 0, true, 1, display_changed);
// Whether the game runs inside gamescope, for the video options: some outputs
// exist only there. Set by the module at start, not saved.
static ConVar vr_display_gamescope("vr_display_gamescope", "0", FCVAR_DONTRECORD,
	"1 when the game runs inside gamescope (set by the VR module)");
static ConVar vr_display_native("vr_display_native", "1", FCVAR_DONTRECORD,
	"1 when the game runs outside gamescope (set by the VR module)");
// The system output (version B: the desktop shows the eyes in its own 3D
// mode) has its own menu: stereo on or off, the render resolution and the
// eye order; the 3D format and output are the desktop's (Daniel, 2026-10-02).
// The menu shows an entry only when a convar is true, so the module sets one
// for each family. Not saved.
static ConVar vr_display_system("vr_display_system", "0", FCVAR_DONTRECORD,
	"1 when the system draws the eyes (set by the VR module)");
static ConVar vr_display_packs("vr_display_packs", "1", FCVAR_DONTRECORD,
	"1 when the game packs the 3D format itself (set by the VR module)");
// The render resolution per eye for the system output: the frame holds two
// side by side and the desktop scales it to its 3D mode, so above the output
// is sharper (supersampled) and below it is faster. The game's screen is set
// at start, so it applies at the next start (the launch reads it).
static ConVar vr_display_render("vr_display_render", "2", FCVAR_ARCHIVE,
	"Render resolution per eye, system output (next start): 0 1280x720, 1 1600x900, 2 1920x1080 (3D TVs)",
	true, 0, true, 2);


// Stereo screenshots (Daniel, 2026-09-28). While 3D is on the module takes
// over Steam's screenshot key, as Team Fortress 2's client already does
// (clientmode_tf.cpp: HookScreenshots, OnScreenshotRequested): Steam then
// asks the game for the picture, and the module files a stereo screenshot of
// Valve's own type (k_EVRScreenshotType_Stereo, the one SteamVR writes), with
// JPS, MPO and the anaglyph beside it (stereo_shot.cpp). Source already takes
// its VR screenshots without the lens (view.cpp: bDoUndistort =
// !engine->IsTakingScreenshot()); these are also both eyes. Steam's flat API
// is looked up in the game's own steam_api at run time, as SDL is: no link.
typedef ISteamScreenshots *(*SteamScreenshotsFn)(void);
typedef void (*HookScreenshotsFn)(ISteamScreenshots *, bool);
typedef ScreenshotHandle (*AddVRScreenshotFn)(ISteamScreenshots *, EVRScreenshotType, const char *, const char *);
typedef bool (*SetLocationFn)(ISteamScreenshots *, ScreenshotHandle, const char *);
typedef void (*RegisterCallbackFn)(CCallbackBase *, int);
typedef void (*UnregisterCallbackFn)(CCallbackBase *);

static std::atomic<int> g_shotRequests(0);   // Steam's key pressed while the module holds it
class ShotRequested : public CCallbackBase {
public:
	void Run(void *) { g_shotRequests++; }
	void Run(void *p, bool, SteamAPICall_t) { Run(p); }
	int GetCallbackSizeBytes() { return (int)sizeof(ScreenshotRequested_t); }
};
static ShotRequested g_shotRequested;

// A screenshot read back on the game's thread, written on its own.
struct ShotJob {
	StereoShot shot;
	unsigned char *eyes[2];
	char base[1024];
	char location[128];
	char model[64];
	bool steam;          // asked for by Steam's key: goes into Steam's library
	bool ok;
	char err[512];
	StereoShotFiles files;
};
static std::mutex g_shotMutex;
static std::vector<ShotJob *> g_shotDone;
static std::atomic<int> g_shotsWriting(0);
static void shot_write(ShotJob *job)
{
	job->ok = stereo_shot_write(job->shot, &job->files, job->err, sizeof(job->err));
	free(job->eyes[0]);
	free(job->eyes[1]);
	job->eyes[0] = job->eyes[1] = NULL;
	std::lock_guard<std::mutex> lock(g_shotMutex);
	g_shotDone.push_back(job);
	g_shotsWriting--;
}

class CSourceVRTelevision : public ISourceVirtualReality, public ISourceVRDisplay
{
public:
	CSourceVRTelevision() : m_active(false), m_clientCrosshair(false), m_lastCall(0), m_lastLoadingLog(0), m_lastWhat(""), m_fovX(75.0f), m_ms(NULL), m_factory(NULL), m_triedTargets(false)
	{
		m_rt[0] = m_rt[1] = NULL;
		m_shotRT = NULL;
		m_hudMatUsed = NULL;
		m_hudSheetUsed = NULL;
		m_hudTranslucent = true;
		m_engineShotPending = false;
		m_steamLooked = false;
		m_steamLib = NULL;
		m_shotsApi = NULL;
		m_hookShots = NULL;
		m_addVRShot = NULL;
		m_setLocation = NULL;
		m_unregister = NULL;
		m_shotsHooked = false;
		m_shown[0] = m_shown[1] = false;
		for (int i = 0; i < 8; i++)
			m_traced[i] = false;
		m_hudLogs = 0;
		m_hud = NULL;
		m_hudCopied = false;
		m_matReady = false;
		m_frame = 0;
		m_fovLogs = 0;
		m_dumps = 0;
		m_engine = NULL;
		m_xhairReady = false;
		m_input = NULL;
		m_sdlLooked = false;
		m_sdlLib = NULL;
		m_getKeyFocus = NULL;
		m_setMouseRect = NULL;
		m_warp = NULL;
		m_triedMouseRect = false;
		m_confined = NULL;
		m_getState = NULL;
		m_getFocus = NULL;
		m_getSize = NULL;
		m_getRel = NULL;
		m_mouseLogs = 0;
		m_lastMouse[0] = m_lastMouse[1] = m_lastMouse[2] = m_lastMouse[3] = -2;
		m_startupDone = false;
		m_switchOff = false;
		m_uiFull = false;
		m_selfSet = false;
		m_relayout = 0;
		m_modeCheckFrame = 0;
		m_applyQueued = false;
		m_effectSet = false;
		m_xTried = false;
		m_xdpy = NULL;
		m_uiMade = false;
		m_nTargets = 0;
		m_loadingRT = NULL;
		for (int i = 0; i <= LOADING_BGS; i++)
			m_loadingMat[i] = NULL;
		m_loadingDirty = false;
	}

	// The menu's families: the system's entries, or the game's own formats
	// and, inside gamescope, its outputs.
	void menu_flags()
	{
		bool system = g_cfg.output == OUT_SYSTEM;
		bool gamescope = getenv("GAMESCOPE_WAYLAND_DISPLAY") != NULL;
		vr_display_system.SetValue(system ? 1 : 0);
		vr_display_packs.SetValue(system ? 0 : 1);
		vr_display_gamescope.SetValue(gamescope && !system ? 1 : 0);
	}

	// IAppSystem
	// The engine connects the module like any app system; its factory
	// reaches the material system, which the render targets need.
	bool Connect(CreateInterfaceFn factory)
	{
		trace(5, "Connect");
		m_factory = factory;
		ConnectTier1Libraries(&factory, 1);
		ConVar_Register(0);
		bool gamescope = getenv("GAMESCOPE_WAYLAND_DISPLAY") != NULL;
		vr_display_native.SetValue(gamescope ? 0 : 1);
		menu_flags();
		// Loading screens: the files GameUI reads once, at start-up, set now
		// for the state the game starts in; the source materials (re)written.
		if (g_cfg.loading && loading_available()) {
			loading_sources();
			loading_startup();
		}
		return true;
	}
	void Disconnect()
	{
		shots_hook(false);
		if (m_unregister)
			m_unregister(&g_shotRequested);
		for (int i = 0; i < 200 && g_shotsWriting > 0; i++)
			std::this_thread::sleep_for(std::chrono::milliseconds(50));
		shot_finish();
		ConVar_Unregister();
		DisconnectTier1Libraries();
	}
	void *QueryInterface(const char *name)
	{
		if (name && !strcmp(name, SOURCE_VIRTUAL_REALITY_INTERFACE_VERSION))
			return static_cast<ISourceVirtualReality *>(this);
		if (name && !strcmp(name, SOURCE_VR_DISPLAY_INTERFACE_VERSION))
			return static_cast<ISourceVRDisplay *>(this);
		return NULL;
	}

	// ISourceVRDisplay: the client asks this before its display-only
	// behaviour (the UI at the window's size, the HUD on the screen plane, the
	// muzzle flash placement). The display is the headset whenever 3D is on:
	// the spectator mode (a real headset, the display mirroring it) will
	// answer false here, so the headset keeps its own UI and HUD.
	bool IsDisplayTheHeadset()
	{
		bool display = g_cfg.enabled && m_active;
		// A client that asks this lays its UI out at the window's size and
		// places its crosshair as in 2D, by the game's own rules: in first
		// person and in the in-eye spectator view, not in chase or fixed
		// cameras. The module's own crosshair is only for clients that do
		// not ask (today's Half-Life 2), so it hands the crosshair back.
		if (display && !m_clientCrosshair) {
			m_clientCrosshair = true;
			crosshair_restore();
			logf("crosshair: the client's own (display-aware client)\n");
		}
		return display;
	}
	InitReturnVal_t Init() { return INIT_OK; }
	void Shutdown() {}

	// Without SVRTV_LAYOUT the module behaves like Valve's with no headset:
	// no device, never VR. (Reporting a device and forcing VR mode always made
	// the client switch to VR at startup in 2D runs, and crash; see below.)
	bool ShouldRunInVR() { beat("ShouldRunInVR"); return g_cfg.enabled && m_active; }
	bool IsHmdConnected() { beat("IsHmdConnected"); return g_cfg.enabled; }

	void GetViewportBounds(VREye eye, int *x, int *y, int *w, int *h)
	{
		beat("GetViewportBounds");
		// The left eye takes the first half: left in side-by-side, top in
		// top-and-bottom, as HDMI 1.4 packs them.
		// Any output may be NULL: the client's Activate() asks only for the
		// size (client_virtualreality.cpp, GetViewportBounds(eye, NULL, NULL,
		// &w, &h)). Writing through those crashed Half-Life 2 at startup.
		int vx, vy, vw, vh;
		ensure_targets();
		if (m_rt[0] && m_rt[1]) {
			// Each eye renders into its own target, from its corner.
			eye_size(eye, &vw, &vh);
			vx = vy = 0;
		} else
			half(eye, &vx, &vy, &vw, &vh);
		if (x) *x = vx;
		if (y) *y = vy;
		if (w) *w = vw;
		if (h) *h = vh;
	}

	// The size each eye renders at: its half of the frame, or SVRTV_EYE.
	// In VR mode the client paints the crosshair straight into the eye at the
	// centre of a 640x480 screen (dump, 2026-09-24: eye pixel ~314,240), so a
	// 640x480 eye puts it in the true centre; the copy stretches the eye to
	// its half.
	void eye_size(VREye eye, int *w, int *h)
	{
		if (g_cfg.eyew > 0 && g_cfg.eyeh > 0) {
			*w = g_cfg.eyew;
			*h = g_cfg.eyeh;
		} else if (full_eyes()) {
			// The full formats render each eye at the whole 2D size and
			// shrink it into its half of the frame: supersampled, sharper.
			*w = g_cfg.width;
			*h = g_cfg.height;
		} else
			half(eye, NULL, NULL, w, h);
	}

	// The format the frame is packed in. The pixel outputs (anaglyph, rows,
	// checkerboard) only carry the eyes to gamescope, so frame packing, which
	// needs the HDMI 3D signal, travels as full top and bottom there.
	int packing()
	{
		if (g_cfg.output == OUT_SYSTEM)
			return FMT_SBS;
		if (g_cfg.output != OUT_DISPLAY && (g_cfg.format == FMT_FP1080 || g_cfg.format == FMT_FP720))
			return FMT_TAB;
		// The frame is always the screen the game already has: the full
		// formats pack their full-size eyes into the half layout, so no video
		// mode or gamescope screen is ever resized for them (a resized
		// gamescope screen needed -S stretch, which breaks the mouse, q23).
		if (g_cfg.format == FMT_TAB_FULL)
			return FMT_TAB;
		if (g_cfg.format == FMT_SBS_FULL)
			return FMT_SBS;
		return g_cfg.format;
	}
	// Full formats: eyes rendered at the whole 2D size (Daniel: render in high
	// resolution, even if it is shrunk into the half formats' layout).
	bool full_eyes()
	{
		int f = g_cfg.format;
		return g_cfg.output == OUT_SYSTEM || f == FMT_TAB_FULL || f == FMT_SBS_FULL
			|| (g_cfg.output != OUT_DISPLAY && (f == FMT_FP1080 || f == FMT_FP720));
	}
	bool packed_tab() { return packing() == FMT_TAB; }

	// The output frame's size: the screen the game has. For the system it
	// holds two 2D-size eyes side by side; the game starts at that size.
	void frame_size(int *w, int *h)
	{
		int W = g_cfg.width, H = g_cfg.height;
		if (g_cfg.output == OUT_SYSTEM) {
			*w = 2 * W;
			*h = H;
			return;
		}
		switch (packing()) {
		case FMT_FP1080: *w = 1920; *h = 2205; break;
		case FMT_FP720: *w = 1280; *h = 1470; break;
		default: *w = W; *h = H; break;
		}
	}

	// Where an eye goes in the output frame. The left eye takes the first
	// place (left, or top), as HDMI 1.4 packs them; frame packing leaves its
	// gap (active space) between the eyes.
	void half(VREye eye, int *x, int *y, int *w, int *h)
	{
		bool first = (eye == VREye_Left) != g_cfg.swap;
		// the halves of the frame (the 2D size, or two of it for the system)
		int W, H;
		frame_size(&W, &H);
		int vx = 0, vy = 0, vw = W, vh = H;
		switch (packing()) {
		case FMT_SBS: vw = W / 2; vx = first ? 0 : W / 2; break;
		case FMT_TAB: vh = H / 2; vy = first ? 0 : H / 2; break;
		case FMT_FP1080: vw = 1920; vh = 1080; vy = first ? 0 : 1125; break;
		case FMT_FP720: vw = 1280; vh = 720; vy = first ? 0 : 750; break;
		}
		if (x) *x = vx;
		if (y) *y = vy;
		if (w) *w = vw;
		if (h) *h = vh;
	}

	// No lenses, so "distortion processing" is just putting the eye's picture
	// into its half of the frame. The client calls it after each eye, except
	// while a screenshot is being taken; CompositeHud comes after it in every
	// frame, screenshots included, and shows the eye if this did not.
	bool DoDistortionProcessing(VREye eye)
	{
		beat("DoDistortionProcessing");
		trace(0, "DoDistortionProcessing");
		if (eye == VREye_Left)
			loading_update();
		if (!g_cfg.latecopy)
			show(eye);
		return true;
	}

	// The HUD and menus are painted into the client's "_rt_gui" target
	// (640x480); the client works out where that sheet sits in each eye's
	// view (normalised device coordinates) and asks the module to paste it.
	// The client's own in-world HUD materials do the blending.
	// Late copy (SVRTV_LATECOPY=1): both eyes go into the frame together at
	// the end of the frame, left then right, instead of each right after its
	// render. Under gamescope, anaglyph showed static geometry swimming in
	// depth during camera turns only, as if the two halves came from
	// different frames (2026-09-24); writing them together narrows that.
	bool CompositeHud(VREye eye, float ndc[4], bool blackout, bool undistort, bool translucent)
	{
		beat("CompositeHud");
		trace(1, "CompositeHud");
		bool ok;
		if (!g_cfg.latecopy)
			ok = composite_eye(eye, ndc, translucent);
		else if (eye == VREye_Left)
			return true;
		else {
			m_shown[0] = m_shown[1] = false;
			composite_eye(VREye_Left, ndc, translucent);
			ok = composite_eye(VREye_Right, ndc, translucent);
		}
		if (eye == VREye_Right) {
			mark_frame();
			shot_frame();
		}
		return ok;
	}

	// Loading screens (Daniel, 2026-09-28): the engine draws them while no
	// view renders, straight to the screen, once across the whole frame
	// (run q28), so each eye got half of one flat image, spinner and progress
	// bar included (ghosts in top and bottom, a clash in side by side).
	// Natively nothing reaches the module then; inside gamescope every frame
	// passes through its effect. So there, every frame the module builds
	// carries a mark, its two bottom-right pixels green then magenta, and the
	// effect (svrtv-anaglyph.fx) puts any frame without it, flat 2D, whole
	// into both halves, and paints the mark over.
	void mark_frame()
	{
		if (!g_cfg.loading || !in_gamescope() || !m_ms || g_cfg.output == OUT_SYSTEM)
			return;
		int fw, fh;
		frame_size(&fw, &fh);
		CMatRenderContextPtr ctx(m_ms);
		ctx->PushRenderTargetAndViewport(NULL, fw - 2, fh - 1, 1, 1);
		ctx->ClearColor4ub(0, 255, 0, 255);
		ctx->ClearBuffers(true, false);
		ctx->PopRenderTargetAndViewport();
		ctx->PushRenderTargetAndViewport(NULL, fw - 1, fh - 1, 1, 1);
		ctx->ClearColor4ub(255, 0, 255, 255);
		ctx->ClearBuffers(true, false);
		ctx->PopRenderTargetAndViewport();
	}

	// Stereo screenshots: Steam's flat API in the game's own steam_api.
	void *steam_api(const char *name)
	{
#ifdef _WIN32
		if (!m_steamLib)
			m_steamLib = (void *)GetModuleHandleA(sizeof(void *) == 8 ? "steam_api64.dll" : "steam_api.dll");
		return m_steamLib ? (void *)GetProcAddress((HMODULE)m_steamLib, name) : NULL;
#else
		if (!m_steamLib)
			m_steamLib = dlopen("libsteam_api.so", RTLD_NOW | RTLD_NOLOAD);
		return m_steamLib ? dlsym(m_steamLib, name) : dlsym(RTLD_DEFAULT, name);
#endif
	}
	bool steam_shots()
	{
		if (m_steamLooked)
			return m_shotsApi != NULL;
		m_steamLooked = true;
		SteamScreenshotsFn get = (SteamScreenshotsFn)steam_api("SteamAPI_SteamScreenshots_v003");
		m_hookShots = (HookScreenshotsFn)steam_api("SteamAPI_ISteamScreenshots_HookScreenshots");
		m_addVRShot = (AddVRScreenshotFn)steam_api("SteamAPI_ISteamScreenshots_AddVRScreenshotToLibrary");
		m_setLocation = (SetLocationFn)steam_api("SteamAPI_ISteamScreenshots_SetLocation");
		RegisterCallbackFn reg = (RegisterCallbackFn)steam_api("SteamAPI_RegisterCallback");
		m_unregister = (UnregisterCallbackFn)steam_api("SteamAPI_UnregisterCallback");
		m_shotsApi = get ? get() : NULL;
		if (m_shotsApi && m_hookShots && m_addVRShot && reg)
			reg(&g_shotRequested, ScreenshotRequested_t::k_iCallback);
		else {
			m_shotsApi = NULL;
			m_unregister = NULL;
		}
		logf("stereo screenshots: Steam's screenshot interface %s\n",
		     m_shotsApi ? "found" : "not found (the engine's screenshot key still makes them)");
		return m_shotsApi != NULL;
	}
	// While 3D is on, Steam's screenshot key is the module's; handed back
	// when 3D stops (Steam takes its own 2D screenshots again).
	void shots_hook(bool on)
	{
		if (!g_cfg.shots || on == m_shotsHooked)
			return;
		if (on && !steam_shots())
			return;
		if (!m_shotsApi)
			return;
		m_hookShots(m_shotsApi, on);
		m_shotsHooked = on;
		if (!on)
			g_shotRequests = 0;
		logf("stereo screenshots: Steam's screenshot key %s\n",
		     on ? "taken (every screenshot in 3D is a stereo one)" : "handed back");
	}
	// Once a frame, after both eyes: a screenshot asked for by Steam's key,
	// or by the engine's own, then the finished ones. The engine's own is
	// followed, never joined: reading the eyes back during the engine's
	// screenshot frame crashed the game four seconds later (run q35a; with the
	// module's screenshots off, the same key did not, run q35c), so the module
	// takes its pair on the first frame after the engine has finished.
	void shot_frame()
	{
		if (g_cfg.shots) {
			IVEngineClient *e = engine();
			bool engineShot = e && e->IsTakingScreenshot();
			int requests = g_shotRequests.exchange(0);
			if (requests > 0 && m_shotsHooked)
				shot_take(true);
			if (engineShot)
				m_engineShotPending = true;
			else if (m_engineShotPending) {
				m_engineShotPending = false;
				shot_take(false);
			}
		}
		shot_finish();
	}
	// <game>/screenshots/stereo3d/<map>_<date>_<time>_<frame>
	void shot_base(char *out, size_t n, char *location, size_t ln)
	{
		IVEngineClient *e = engine();
		const char *gd = e ? e->GetGameDirectory() : NULL;
		char dir[1024];
		if (gd && *gd)
			snprintf(dir, sizeof(dir), "%s/screenshots/stereo3d/", gd);
		else
			snprintf(dir, sizeof(dir), "%sscreenshots/stereo3d/", g_dir);
		const char *level = e ? e->GetLevelName() : NULL;   // maps/<name>.bsp
		char map[128] = "menu";
		if (level && *level) {
			const char *b = strrchr(level, '/');
			snprintf(map, sizeof(map), "%s", b ? b + 1 : level);
			char *dot = strrchr(map, '.');
			if (dot)
				*dot = 0;
		}
		snprintf(location, ln, "%s", map);
		time_t now = time(NULL);
		struct tm tmv;
#ifdef _WIN32
		localtime_s(&tmv, &now);
#else
		localtime_r(&now, &tmv);
#endif
		char stamp[32];
		strftime(stamp, sizeof(stamp), "%Y%m%d_%H%M%S", &tmv);
		snprintf(out, n, "%s%s_%s_%d", dir, map, stamp, m_frame);
		make_dirs(out);
	}
	// Both eyes as rendered (full size in the full formats), each with this
	// frame's HUD painted over it on the screen plane as the player saw it
	// (established solution 1), read back before packing and before any
	// anaglyph; the files are written on their own thread.
	void shot_take(bool steam)
	{
		int w, h;
		eye_size(VREye_Left, &w, &h);
		if (!m_ms || !m_rt[0] || !m_rt[1] || !m_shotRT) {
			logf("stereo screenshot: NOT taken (no eye targets%s)\n", m_shotRT ? "" : ", no screenshot target");
			return;
		}
		ShotJob *job = new ShotJob;
		memset(job, 0, sizeof(*job));
		size_t bytes = (size_t)w * h * 4;
		job->eyes[0] = (unsigned char *)malloc(bytes);
		job->eyes[1] = (unsigned char *)malloc(bytes);
		if (!job->eyes[0] || !job->eyes[1]) {
			free(job->eyes[0]);
			free(job->eyes[1]);
			delete job;
			logf("stereo screenshot: NOT taken (out of memory for %dx%d)\n", w, h);
			return;
		}
		CMatRenderContextPtr ctx(m_ms);
		for (int i = 0; i < 2; i++) {
			Rect_t src = { 0, 0, w, h };
			Rect_t dst = { 0, 0, w, h };
			ctx->PushRenderTargetAndViewport(m_shotRT, 0, 0, w, h);
			ctx->CopyTextureToRenderTargetEx(0, m_rt[i], &src, &dst);
			if (m_hudMatUsed && m_hudSheetUsed) {
				int x0, y0, x1, y1;
				hud_rect(0, 0, w, h, &x0, &y0, &x1, &y1);
				if (x1 > x0 && y1 > y0)
					paint_hud(ctx, m_hudMatUsed, m_hudSheetUsed, x0, y0, x1 - x0, y1 - y0, m_hudTranslucent);
			}
			ctx->ReadPixels(0, 0, w, h, job->eyes[i], IMAGE_FORMAT_BGRA8888);
			ctx->PopRenderTargetAndViewport();
		}
		shot_base(job->base, sizeof(job->base), job->location, sizeof(job->location));
		const char *game = CommandLine()->ParmValue("-game", "hl2");
		const char *slash = strrchr(game, '/');
		snprintf(job->model, sizeof(job->model), "%s", slash ? slash + 1 : game);
		job->steam = steam;
		StereoShot &s = job->shot;
		s.w = w;
		s.h = h;
		// The swap switch changes which half each eye fills, not which eye
		// is which: these are always the left and right eyes.
		s.eye[0] = job->eyes[0];
		s.eye[1] = job->eyes[1];
		s.base = job->base;
		s.anaglyph = g_cfg.output == OUT_ANAGLYPH_CRT ? STEREO_SHOT_ANAGLYPH_CRT
		           : g_cfg.output == OUT_ANAGLYPH_MODERN ? STEREO_SHOT_ANAGLYPH_MODERN
		           : STEREO_SHOT_NO_ANAGLYPH;
		// Game units are inches (the separation's default, 2.5, is about
		// 64 mm); the eyes are parallel, their views shifted to meet at the
		// screen plane, so the lines of sight never converge.
		s.baselineMetres = g_cfg.separation * 0.0254;
		s.convergenceDegrees = 0.0;
		s.make = "sourcevr_display";
		s.model = job->model;
		s.when = time(NULL);
		g_shotsWriting++;
		std::thread(shot_write, job).detach();
		logf("stereo screenshot: %dx%d per eye, asked by %s, writing %s\n", w, h,
		     steam ? "Steam's key" : "the engine's screenshot", job->base);
	}
	// Finished screenshots: logged, and the ones Steam asked for go into its
	// library as stereo screenshots (preview: the left eye).
	void shot_finish()
	{
		std::vector<ShotJob *> done;
		{
			std::lock_guard<std::mutex> lock(g_shotMutex);
			done.swap(g_shotDone);
		}
		for (size_t i = 0; i < done.size(); i++) {
			ShotJob *job = done[i];
			if (!job->ok)
				logf("stereo screenshot: FAILED (%s)\n", job->err);
			else
				logf("stereo screenshot: written %s, %s, %s, %s%s%s\n", job->files.stereo, job->files.preview,
				     job->files.jps, job->files.mpo, job->files.anaglyph[0] ? ", " : "", job->files.anaglyph);
			if (job->ok && job->steam && m_shotsApi && m_addVRShot) {
				ScreenshotHandle hnd = m_addVRShot(m_shotsApi, k_EVRScreenshotType_Stereo, job->files.preview, job->files.stereo);
				if (hnd != INVALID_SCREENSHOT_HANDLE && m_setLocation)
					m_setLocation(m_shotsApi, hnd, job->location);
				logf("stereo screenshot: Steam library %s (handle %u)\n",
				     hnd != INVALID_SCREENSHOT_HANDLE ? "added as a stereo screenshot" : "REFUSED it", (unsigned)hnd);
			}
			delete job;
		}
	}

	// Where the HUD sheet goes in an eye's area (its half of the frame, or a
	// whole eye in a stereo screenshot). A television is not a headset: the
	// HUD fills the screen as in 2D, at the same place in both eyes, so it
	// sits on the screen plane (zero parallax). The client's own placement
	// (ndc, a floating panel sized for a headset) covered only the middle
	// (Daniel, 2026-09-24). The sheet is 4:3; keep that shape: full height,
	// centred.
	void hud_rect(int hx, int hy, int hw, int hh, int *x0, int *y0, int *x1, int *y1)
	{
		double wf = g_cfg.hudwide ? 1.0 : (4.0 / 3.0) / g_cfg.aspect;
		if (wf > 1.0)
			wf = 1.0;
		*x0 = hx + (int)(hw * (1.0 - wf) / 2.0);
		*x1 = *x0 + (int)(hw * wf);
		*y0 = hy;
		*y1 = hy + hh;
	}
	// Paints the HUD sheet (and the module's crosshair, for clients that do
	// not place their own) into the current target at x0, y0, w by h.
	void paint_hud(IMatRenderContext *ctx, IMaterial *mat, ITexture *sheet, int x0, int y0, int w, int h, bool translucent)
	{
		int tw = sheet->GetActualWidth(), th = sheet->GetActualHeight();
		// The client passes translucent = false while the mouse cursor is
		// visible: a menu or dialog is open. Those keep HL2's layout (moving
		// the bottom band sent Save/Cancel to the top; Daniel, 2026-09-24).
		if (g_cfg.hudband > 0 && translucent) {
			// HL2 keeps health and ammo in the bottom band, where the ammo
			// panel lands over the gun, which in 3D is confusing and tiring
			// (Daniel, 2026-09-24). That band goes to the top and the rest of
			// the sheet moves down by the band's height, whole (swapping the
			// two bands cut the weapon selection, drawn at the top, in two).
			int sb = (int)(th * g_cfg.hudband), db = (int)(h * g_cfg.hudband);
			ctx->DrawScreenSpaceRectangle(mat, x0, y0 + db, w, h - db, 0, 0, tw - 1, th - sb - 1, tw, th);
			ctx->DrawScreenSpaceRectangle(mat, x0, y0, w, db, 0, th - sb, tw - 1, th - 1, tw, th);
		} else
			ctx->DrawScreenSpaceRectangle(mat, x0, y0, w, h, 0, 0, tw - 1, th - 1, tw, th);
		if (g_cfg.xhair && !m_clientCrosshair)
			draw_crosshair(ctx, x0, y0, w, h);
	}

	bool composite_eye(VREye eye, float ndc[4], bool translucent)
	{
		show(eye);
		if (!m_ms)
			return false;
		ITexture *gui = m_ms->FindTexture("_rt_gui", NULL, false);
		if (gui && gui->IsError())
			gui = NULL;
		// Not the client's own vgui/inworldui: it is set up while _rt_gui does
		// not exist yet and then draws the purple-black error pattern whatever
		// texture it is given (2026-09-24: even the engine's _rt_FullFrameFB).
		// vgui/icon_con_grey is the same kind of material (UnlitGeneric,
		// translucent, ignorez), set up normally; it is the server browser's
		// connection icon, which single-player never shows, so in 3D mode the
		// module takes it over for the HUD.
		IMaterial *mat = m_ms->FindMaterial(g_cfg.hudmat, TEXTURE_GROUP_VGUI, false);
		if (!gui || !mat || mat->IsErrorMaterial())
			return false;
		// With the engine's threaded renderer, a material first used from the
		// render thread without a main-thread precache draws as the error
		// material. The client precaches everything it draws; the module
		// holds a reference and asks for the precache once.
		if (!m_matReady) {
			m_matReady = true;
			bool was = mat->IsPrecached();
			mat->IncrementReferenceCount();
			m_ms->CacheUsedMaterials();
			logf("hud material %s: precached %d -> %d\n", mat->GetName(), (int)was, (int)mat->IsPrecached());
		}
		// Sample a fresh copy of the sheet (see make_targets), copied once a
		// frame, before the first eye's paste.
		if (m_hud && !m_hudCopied) {
			m_hudCopied = true;
			int gw = gui->GetActualWidth(), gh = gui->GetActualHeight();
			Rect_t src = { 0, 0, gw, gh };
			Rect_t dst = { 0, 0, m_hud->GetActualWidth(), m_hud->GetActualHeight() };
			CMatRenderContextPtr c(m_ms);
			c->PushRenderTargetAndViewport(m_hud);
			c->CopyTextureToRenderTargetEx(0, gui, &src, &dst);
			c->PopRenderTargetAndViewport();
		}
		ITexture *sheet = m_hud ? m_hud : gui;
		// Test switch: sample another texture instead (SVRTV_HUDTEX=name,
		// e.g. _rt_FullFrameFB or _rt_svrtv_left), to tell a material problem
		// from a texture problem.
		if (g_cfg.hudtex[0]) {
			ITexture *t = m_ms->FindTexture(g_cfg.hudtex, NULL, false);
			if (t && !t->IsError())
				sheet = t;
		}
		bool found = false;
		IMaterialVar *base = mat->FindVar("$basetexture", &found, false);
		ITexture *before = (found && base) ? base->GetTextureValue() : NULL;
		if (found && base && before != sheet)
			base->SetTextureValue(sheet);
		int hx, hy, hw, hh;
		half(eye, &hx, &hy, &hw, &hh);
		(void)ndc;
		int x0, y0, x1, y1;
		hud_rect(hx, hy, hw, hh, &x0, &y0, &x1, &y1);
		if (m_hudLogs < 4) {
			m_hudLogs++;
			ITexture *after = (found && base) ? base->GetTextureValue() : NULL;
			logf("hud pointers: gui %p sheet %p before %p after %p\n", (void *)gui, (void *)sheet, (void *)before, (void *)after);
			logf("hud: eye %d material %s (error %d) $basetexture found %d before %s after %s; gui %s %dx%d error %d; ndc %.3f %.3f %.3f %.3f -> rect %d,%d %dx%d translucent %d\n",
			     (int)eye, mat->GetName(), (int)mat->IsErrorMaterial(), (int)found,
			     before ? before->GetName() : "-", after ? after->GetName() : "-",
			     gui->GetName(), gui->GetActualWidth(), gui->GetActualHeight(), (int)gui->IsError(),
			     ndc[0], ndc[1], ndc[2], ndc[3], x0, y0, x1 - x0, y1 - y0, (int)translucent);
		}
		if (x1 <= x0 || y1 <= y0)
			return false;
		int tw = sheet->GetActualWidth(), th = sheet->GetActualHeight();
		CMatRenderContextPtr ctx(m_ms);
		if (g_cfg.hudcopy) {
			// Test path: a plain copy of the sheet, no material, no blending.
			Rect_t src = { 0, 0, tw, th };
			Rect_t dst = { x0, y0, x1 - x0, y1 - y0 };
			ctx->PushRenderTargetAndViewport(NULL);
			ctx->CopyTextureToRenderTargetEx(0, gui, &src, &dst);
			ctx->PopRenderTargetAndViewport();
			return true;
		}
		int fw, fh;
		frame_size(&fw, &fh);
		ctx->PushRenderTargetAndViewport(NULL, 0, 0, fw, fh);
		paint_hud(ctx, mat, sheet, x0, y0, x1 - x0, y1 - y0, translucent);
		ctx->PopRenderTargetAndViewport();
		// What a stereo screenshot paints over each eye: this frame's HUD.
		m_hudMatUsed = mat;
		m_hudSheetUsed = sheet;
		m_hudTranslucent = translucent;
		if (g_cfg.dump && m_frame == g_cfg.dump) {
			if (eye == VREye_Left)
				dump(ctx, sheet, "svrtv-hud.tga");
			else
				dump(ctx, NULL, "svrtv-frame.tga");
		}
		// A series of frames (SVRTV_DUMPEVERY=n, from SVRTV_DUMP on, 12 at
		// most), to compare the two eyes of the same frame in motion.
		if (g_cfg.dumpevery > 0 && g_cfg.dump && eye == VREye_Right && m_frame > g_cfg.dump &&
		    (m_frame - g_cfg.dump) % g_cfg.dumpevery == 0 && m_dumps < 12) {
			char name[64];
			snprintf(name, sizeof(name), "svrtv-frame-%06d.tga", m_frame);
			dump(ctx, NULL, name);
			m_dumps++;
		}
		return true;
	}

	// The engine's client interface, from the factory passed to Connect()
	// (VEngineClient013: the same slots in the 2013 and 2025 SDK headers).
	// One angle-log row for this frame (SVRTV_ANGLELOG).
	void record_angles()
	{
		IVEngineClient *e = engine();
		if (!e)
			return;
		if (g_nang >= ANGLE_MAX)
			write_angles();
		QAngle a;
		e->GetViewAngles(a);
		AngleRow &r = g_ang[g_nang++];
		r.t = clock_s();
		r.yaw = a[YAW];
		r.pitch = a[PITCH];
		r.frame = m_frame;
	}

	// Seconds, for logs that only use differences: tier0's high-resolution
	// clock on Windows, gettimeofday elsewhere.
	static double clock_s()
	{
#ifdef _WIN32
		return Plat_FloatTime();
#else
		struct timeval tv;
		gettimeofday(&tv, NULL);
		return tv.tv_sec + tv.tv_usec / 1e6;
#endif
	}
	// SVRTV_CALLTRACE: each call the engine or client makes, at its start.
	void beat(const char *what)
	{
		if (!g_cfg.calltrace)
			return;
		double t = clock_s();
		IVEngineClient *e = engine();
		bool loading = e && e->IsDrawingLoadingImage();
		if (m_lastCall > 0 && t - m_lastCall > 0.2)
			logf("call gap %.3f s: after %s, then %s (loading image: %d)\n", t - m_lastCall, m_lastWhat, what, (int)loading);
		if (loading && t - m_lastLoadingLog > 1.0) {
			m_lastLoadingLog = t;
			logf("call while the loading image is drawn: %s\n", what);
		}
		m_lastCall = t;
		m_lastWhat = what;
	}

	IVEngineClient *engine()
	{
		if (!m_engine && m_factory)
			m_engine = (IVEngineClient *)m_factory(VENGINE_CLIENT_INTERFACE_VERSION_13, NULL);
		return m_engine;
	}

	// Queues console commands, ";"-separated.
	void command(const char *cmds)
	{
		IVEngineClient *e = engine();
		if (!e || !cmds || !*cmds)
			return;
		char line[600];
		snprintf(line, sizeof(line), "%s\n", cmds);
		e->ClientCmd_Unrestricted(line);
		logf("console: %s", line);
	}

	// crosshair is a saved setting: when the module turns it off it leaves a
	// marker next to itself, so a later start (2D included) turns it back on
	// even if the game quit while in VR.
	void marker_path(char *out, size_t n) { snprintf(out, n, "%ssvrtv-crosshair-off", g_dir); }
	void crosshair_off()
	{
		command("crosshair 0");
		char p[1200];
		marker_path(p, sizeof(p));
		FILE *f = fopen(p, "w");
		if (f)
			fclose(f);
	}
	void crosshair_restore()
	{
		char p[1200];
		marker_path(p, sizeof(p));
		FILE *f = fopen(p, "r");
		if (!f)
			return;
		fclose(f);
		command("crosshair 1");
		remove(p);
	}

	// Motion blur follows the same pattern: the player's own value (the
	// video settings' "MotionBlur") is kept in a marker next to the module and
	// put back when VR stops, or at the next start if the game quit in VR.
	void blur_marker(char *out, size_t n) { snprintf(out, n, "%ssvrtv-blur-restore", g_dir); }
	void blur_off()
	{
		char p[1200];
		blur_marker(p, sizeof(p));
		FILE *f = fopen(p, "r");
		if (f) {
			fclose(f);   // already off, the player's value already kept
		} else {
			int was = 1;
#ifdef _WIN32
			// Windows keeps the video settings outside the game folder, so
			// the live value is read before it is turned off.
			ConVarRef blur("mat_motion_blur_enabled");
			if (blur.IsValid())
				was = blur.GetBool() ? 1 : 0;
#else
			char vc[1200], line[256];
			snprintf(vc, sizeof(vc), "%s../hl2/videoconfig_linux.cfg", g_dir);
			FILE *v = fopen(vc, "r");
			if (v) {
				while (fgets(line, sizeof(line), v)) {
					const char *k = strstr(line, "\"MotionBlur\"");
					if (!k)
						continue;
					const char *q = strchr(k + 12, '"');
					if (q)
						was = atoi(q + 1) != 0;
				}
				fclose(v);
			}
#endif
			f = fopen(p, "w");
			if (f) {
				fprintf(f, "%d\n", was);
				fclose(f);
			}
			logf("motion blur was %d\n", was);
		}
		command("mat_motion_blur_enabled 0");
	}
	// The video options show the settings, so while 3D is on they read what
	// is showing whatever turned it on (the menu showed "Disabled" in a
	// -stereo3d session, Daniel, 2026-09-26). When -stereo3d or the bench
	// turned it on, a marker keeps the player's own values, which go back
	// when 3D stops or at the next start: the launch option applies to its
	// own launch.
	void launch_3d_mark()
	{
		if (!g_pCVar)   // not connected to the engine (a test harness)
			return;
		int layout = g_cfg.format;
		if (vr_display_3d.GetInt() == 1 && vr_display_layout.GetInt() == layout)
			return;
		char p[1200];
		snprintf(p, sizeof(p), "%ssvrtv-launch-3d", g_dir);
		FILE *f = fopen(p, "r");
		if (f) {
			fclose(f);   // already marked: the player's first values are kept
		} else if ((f = fopen(p, "w")) != NULL) {
			fprintf(f, "%d %d\n", vr_display_3d.GetInt(), vr_display_layout.GetInt());
			fclose(f);
		}
		m_selfSet = true;
		vr_display_3d.SetValue(1);
		vr_display_layout.SetValue(layout);
		m_selfSet = false;
	}
	// The player changed the settings: from now on the values are theirs.
	void launch_3d_forget()
	{
		char p[1200];
		snprintf(p, sizeof(p), "%ssvrtv-launch-3d", g_dir);
		remove(p);
	}
	void launch_3d_unmark()
	{
		if (!g_pCVar)
			return;
		char p[1200];
		snprintf(p, sizeof(p), "%ssvrtv-launch-3d", g_dir);
		FILE *f = fopen(p, "r");
		if (!f)
			return;
		int was3d = 0, wasLayout = 0;
		if (fscanf(f, "%d %d", &was3d, &wasLayout) != 2)
			was3d = wasLayout = 0;
		fclose(f);
		remove(p);
		m_selfSet = true;
		vr_display_3d.SetValue(was3d);
		vr_display_layout.SetValue(wasLayout);
		m_selfSet = false;
	}

	// Anaglyph through gamescope: gamescope reads its ReShade effect and
	// technique from two properties on the root window of the game's X
	// display (GAMESCOPE_RESHADE_EFFECT, GAMESCOPE_RESHADE_TECHNIQUE_IDX), so
	// the game switches it while running. libX11 is the game's own, loaded
	// at run time. Techniques of svrtv-anaglyph.fx: 0 CRT and 1 modern
	// screens from side by side, 3 and 4 the same from top and bottom.
	typedef void *(*XOpenDisplayFn)(const char *);
	typedef unsigned long (*XInternAtomFn)(void *, const char *, int);
	typedef unsigned long (*XDefaultRootWindowFn)(void *);
	typedef int (*XChangePropertyFn)(void *, unsigned long, unsigned long, unsigned long, int, int, const unsigned char *, int);
	typedef int (*XDeletePropertyFn)(void *, unsigned long, unsigned long);
	typedef int (*XFlushFn)(void *);
#ifdef _WIN32
	// gamescope is Linux's (SteamOS's) compositor. A Windows game under
	// Proton inside gamescope cannot reach its X display from here, so the
	// Windows module offers the native menu only.
	bool in_gamescope() { return false; }
#else
	bool in_gamescope() { return getenv("GAMESCOPE_WAYLAND_DISPLAY") != NULL; }
#endif
	bool x11()
	{
#ifdef _WIN32
		return false;
#else
		if (m_xdpy)
			return true;
		if (m_xTried)
			return false;
		m_xTried = true;
		void *lib = dlopen("libX11.so.6", RTLD_NOW);
		if (!lib) {
			logf("output: libX11 not found\n");
			return false;
		}
		XOpenDisplayFn open = (XOpenDisplayFn)dlsym(lib, "XOpenDisplay");
		m_xInternAtom = (XInternAtomFn)dlsym(lib, "XInternAtom");
		m_xRoot = (XDefaultRootWindowFn)dlsym(lib, "XDefaultRootWindow");
		m_xChange = (XChangePropertyFn)dlsym(lib, "XChangeProperty");
		m_xDelete = (XDeletePropertyFn)dlsym(lib, "XDeleteProperty");
		m_xFlush = (XFlushFn)dlsym(lib, "XFlush");
		if (!open || !m_xInternAtom || !m_xRoot || !m_xChange || !m_xDelete || !m_xFlush)
			return false;
		m_xdpy = open(NULL);
		logf("output: X display %s\n", m_xdpy ? "open" : "not open");
		return m_xdpy != NULL;
#endif
	}
	// gamescope finds effects in its ReShade folder; the module brings its
	// own copy from next to itself.
	// Loading screens, natively (Daniel, 2026-09-28: stereo art only). The
	// engine draws the loading screen once across the whole frame while no
	// view renders (run q28), and natively nothing reaches the module then.
	// GameUI builds it from the chapter's picture, named in
	// scripts/chapterbackgrounds.txt, plus a spinner and a progress bar, and
	// reads the list and the colours once, at start-up (runs q30-q32). So:
	// - while 3D runs, the module saves a stereo copy of each picture beside
	//   the originals (console/svrtv1_tab_<name> or svrtv1_sbs_<name>, drawn
	//   by the game's renderer and read back): the picture once per eye in
	//   the frame's packing, and the game's lambda logo at the centre, half
	//   the largest centred square, popped in front of the screen;
	// - at start-up, when the game will start in 3D and the copies for its
	//   packing exist, a chapter list in the 3D menu's custom folder points
	//   at them; otherwise it goes, and the loading screens are Valve's.
	// GameUI's spinner and progress bar are still drawn once across the frame
	// (run q34: the spinner in one eye's corner, the bar crossing from one
	// eye into the other, no ghost; hiding them through the client scheme
	// did not reach them). Daniel: acceptable, loading screens are a small
	// part of the time next to the whole game in depth.
	// Valve's files are never touched. A 3D or format change during a session
	// reaches the loading screens at the next start. Inside gamescope none of
	// this: the effect puts the whole loading screen into both eyes
	// (mark_frame).
	enum { LOADING_BGS = 7, LOADING_W = 2048, LOADING_H = 1024 };
	void loading_path(char *out, size_t n, const char *sub)
	{
		snprintf(out, n, "%s../hl2/custom/svrtv-3d-menu/%s", g_dir, sub);
	}
	bool loading_file(const char *sub)
	{
		char p[1200];
		loading_path(p, sizeof(p), sub);
		FILE *f = fopen(p, "rb");
		if (!f)
			return false;
		fclose(f);
		return true;
	}
	bool loading_available() { return loading_file("gamepadui/options.res"); }
	static void make_dirs(char *path)   // every folder of a file's path
	{
		for (char *c = path + 1; *c; c++)
			if (*c == '/' || *c == '\\') {
				char k = *c;
				*c = 0;
#ifdef _WIN32
				_mkdir(path);
#else
				mkdir(path, 0755);
#endif
				*c = k;
			}
	}
	void loading_write(const char *sub, const char *text)
	{
		char p[1200];
		loading_path(p, sizeof(p), sub);
		make_dirs(p);
		FILE *f = fopen(p, "w");
		if (f) {
			fputs(text, f);
			fclose(f);
		}
	}
	// An uncompressed BGRA8888 VTF (7.1, one level) from read-back pixels.
	void loading_vtf(const char *sub, const unsigned char *px, int w, int h)
	{
		char p[1200];
		loading_path(p, sizeof(p), sub);
		make_dirs(p);
		FILE *f = fopen(p, "wb");
		if (!f)
			return;
		unsigned char hdr[64];
		memset(hdr, 0, sizeof(hdr));
		unsigned int u;
		unsigned short s;
		float one = 1.0f;
		memcpy(hdr, "VTF", 4);
		u = 7; memcpy(hdr + 4, &u, 4);
		u = 1; memcpy(hdr + 8, &u, 4);
		u = 64; memcpy(hdr + 12, &u, 4);       // header size
		s = (unsigned short)w; memcpy(hdr + 16, &s, 2);
		s = (unsigned short)h; memcpy(hdr + 18, &s, 2);
		u = 0x4 | 0x8 | 0x100 | 0x200; memcpy(hdr + 20, &u, 4);   // CLAMPS|CLAMPT|NOMIP|NOLOD
		s = 1; memcpy(hdr + 24, &s, 2);        // frames
		memcpy(hdr + 48, &one, 4);             // bump map scale
		u = IMAGE_FORMAT_BGRA8888; memcpy(hdr + 52, &u, 4);
		hdr[56] = 1;                           // mip levels
		u = 0xFFFFFFFF; memcpy(hdr + 57, &u, 4);   // no low-resolution image
		fwrite(hdr, 1, sizeof(hdr), f);
		fwrite(px, 1, (size_t)w * h * 4, f);
		fclose(f);
	}
	static const char *loading_prefix(int pk) { return pk == FMT_SBS ? "svrtv1_sbs_" : "svrtv1_tab_"; }
	bool loading_ready(const char *prefix)
	{
		for (int i = 1; i <= LOADING_BGS; i++) {
			char sub[160];
			snprintf(sub, sizeof(sub), "materials/console/%sbackground%02d_widescreen.vtf", prefix, i);
			if (!loading_file(sub))
				return false;
		}
		return true;
	}
	// The chapter list: Valve's, each background renamed to its stereo copy;
	// NULL removes it.
	void loading_chapters(const char *prefix)
	{
		char dst[1200];
		loading_path(dst, sizeof(dst), "scripts/chapterbackgrounds.txt");
		if (!prefix) {
			if (remove(dst) == 0)
				logf("loading screens: chapter list removed (Valve's pictures)\n");
			return;
		}
		char src[1200];
		snprintf(src, sizeof(src), "%s../hl2/scripts/chapterbackgrounds.txt", g_dir);
		FILE *in = fopen(src, "r");
		if (!in) {
			logf("loading screens: %s not found\n", src);
			return;
		}
		make_dirs(dst);
		FILE *out = fopen(dst, "w");
		if (!out) {
			fclose(in);
			return;
		}
		char line[512];
		while (fgets(line, sizeof(line), in)) {
			char *b = strstr(line, "\"background");
			if (b) {
				fwrite(line, 1, b + 1 - line, out);
				fputs(prefix, out);
				fputs(b + 1, out);
			} else
				fputs(line, out);
		}
		fclose(in);
		fclose(out);
		logf("loading screens: chapter list points at the %s pictures\n", prefix);
	}
	// "9" from a line like: vr_display_3d "9"
	static int saved_int(const char *text, const char *name, int def)
	{
		const char *p = text;
		size_t n = strlen(name);
		while ((p = strstr(p, name)) != NULL) {
			if ((p == text || p[-1] == '\n') && (p[n] == ' ' || p[n] == '\t')) {
				const char *q = strchr(p + n, '"');
				if (q)
					return atoi(q + 1);
			}
			p += n;
		}
		return def;
	}
	// At start-up (Connect, before the game reads its files): the stereo art
	// when the game will start in 3D, natively, in a packing whose copies
	// exist; Valve's loading screens otherwise.
	void loading_startup()
	{
		// The game's folder from the launch line (-game, relative to the game's
		// root or absolute): at Connect the engine has not set its own yet.
		char gamedir[1024];
		const char *game = CommandLine()->ParmValue("-game", "hl2");
		bool absolute = game[0] == '/' || game[0] == '\\' || (game[0] && game[1] == ':');
		if (absolute)
			snprintf(gamedir, sizeof(gamedir), "%s", game);
		else
			snprintf(gamedir, sizeof(gamedir), "%s../%s", g_dir, game);
		bool on = g_cfg.enabled;
		int fmt = g_cfg.layoutGiven ? g_cfg.format : FMT_TAB_FULL;
		char cfg[1200];
		snprintf(cfg, sizeof(cfg), "%s/cfg/config.cfg", gamedir);
		FILE *f = fopen(cfg, "rb");
		if (f) {
			static char text[262144];
			size_t n = fread(text, 1, sizeof(text) - 1, f);
			text[n] = 0;
			fclose(f);
			on = on || saved_int(text, "vr_display_3d", 0) > 0;
			if (!g_cfg.layoutGiven)
				fmt = saved_int(text, "vr_display_layout", FMT_TAB_FULL);
			logf("loading screens: saved state from %s: 3D %d, format %d\n", cfg, (int)on, fmt);
		} else
			logf("loading screens: no saved state (%s)\n", cfg);
		int pk = fmt == FMT_TAB || fmt == FMT_TAB_FULL ? FMT_TAB : fmt == FMT_SBS || fmt == FMT_SBS_FULL ? FMT_SBS : -1;
		const char *prefix = pk >= 0 ? loading_prefix(pk) : NULL;
		if (on && prefix && !in_gamescope() && loading_ready(prefix))
			loading_chapters(prefix);
		else {
			loading_chapters(NULL);
			logf("loading screens: Valve's (3D at start %d, packing %d, gamescope %d)\n", (int)on, pk, (int)in_gamescope());
		}
	}
	IMaterial *loading_material(int i)   // 0..6 the pictures, LOADING_BGS the logo
	{
		if (!m_loadingMat[i]) {
			char name[64];
			if (i < LOADING_BGS)
				snprintf(name, sizeof(name), "svrtv/loading_bg%02d", i + 1);
			else
				snprintf(name, sizeof(name), "svrtv/loading_logo");
			IMaterial *m = m_ms->FindMaterial(name, TEXTURE_GROUP_VGUI, false);
			if (!m || m->IsErrorMaterial())
				return NULL;
			m->IncrementReferenceCount();
			m_loadingMat[i] = m;
		}
		return m_loadingMat[i];
	}
	// Draws and saves every picture of this packing that has no stereo copy
	// yet (a name's content never changes: the version is in the prefix),
	// for the next start.
	void loading_make()
	{
		if (!m_loadingRT || !m_ms)
			return;
		int pk = packing();
		if (pk != FMT_TAB && pk != FMT_SBS)
			return;
		const char *prefix = loading_prefix(pk);
		// The eye's own view, normalised: the logo's box and its shift
		// (crossed parallax: the left eye's copy right, the right eye's left).
		float sw = (float)g_cfg.width, sh = (float)g_cfg.height, side = 0.5f * (sw < sh ? sw : sh);
		float lw = side / sw, lh = side / sh, pop = 0.00625f;
		IMaterial *logo = loading_material(LOADING_BGS);
		unsigned char *px = NULL;
		int made = 0, kept = 0;
		for (int i = 0; i < LOADING_BGS; i++) {
			char sub[160], text[512];
			snprintf(sub, sizeof(sub), "materials/console/%sbackground%02d_widescreen.vtf", prefix, i + 1);
			if (loading_file(sub)) {
				kept++;
				continue;
			}
			IMaterial *bg = loading_material(i);
			if (!bg)
				continue;
			if (!px && !(px = (unsigned char *)malloc((size_t)LOADING_W * LOADING_H * 4)))
				break;
			CMatRenderContextPtr ctx(m_ms);
			ctx->PushRenderTargetAndViewport(m_loadingRT, 0, 0, LOADING_W, LOADING_H);
			ctx->ClearColor4ub(0, 0, 0, 255);
			ctx->ClearBuffers(true, false);
			for (int half = 0; half < 2; half++) {
				int hx = 0, hy = 0, hw = LOADING_W, hh = LOADING_H;
				if (pk == FMT_TAB) { hh = LOADING_H / 2; hy = half * hh; }
				else { hw = LOADING_W / 2; hx = half * hw; }
				ctx->DrawScreenSpaceRectangle(bg, hx, hy, hw, hh, 0, 0, 511, 511, 512, 512);
				if (logo) {
					bool left = (half == 0) != g_cfg.swap;   // the first place is the left eye unless swapped
					float x = 0.5f - lw / 2 + (left ? pop : -pop), y = 0.5f - lh / 2;
					ctx->DrawScreenSpaceRectangle(logo, hx + (int)(x * hw), hy + (int)(y * hh),
						(int)(lw * hw), (int)(lh * hh), 0, 0, 255, 255, 256, 256);
				}
			}
			ctx->ReadPixels(0, 0, LOADING_W, LOADING_H, px, IMAGE_FORMAT_BGRA8888);
			ctx->PopRenderTargetAndViewport();
			for (size_t k = 3; k < (size_t)LOADING_W * LOADING_H * 4; k += 4)
				px[k] = 255;
			loading_vtf(sub, px, LOADING_W, LOADING_H);
			for (int wide = 0; wide < 2; wide++) {
				snprintf(sub, sizeof(sub), "materials/console/%sbackground%02d%s.vmt", prefix, i + 1, wide ? "_widescreen" : "");
				snprintf(text, sizeof(text),
					"\"UnlitGeneric\"\n{\n\t\"$basetexture\" \"console/%sbackground%02d_widescreen\"\n\t\"$vertexcolor\" 1\n"
					"\t\"$vertexalpha\" 1\n\t\"$ignorez\" 1\n\t\"$no_fullbright\" 1\n\t\"$nolod\" 1\n}\n", prefix, i + 1);
				loading_write(sub, text);
			}
			made++;
		}
		free(px);
		logf("loading screens: %d stereo pictures made, %d already there (%s)\n", made, kept, prefix);
	}
	// The materials the module draws from: the chapter pictures under their
	// own names and the logo.
	void loading_sources()
	{
		for (int i = 1; i <= LOADING_BGS; i++) {
			char sub[128], text[512];
			snprintf(sub, sizeof(sub), "materials/svrtv/loading_bg%02d.vmt", i);
			snprintf(text, sizeof(text),
				"\"UnlitGeneric\"\n{\n\t\"$basetexture\" \"console/background%02d_widescreen\"\n"
				"\t\"$ignorez\" 1\n\t\"$nolod\" 1\n\t\"$nomip\" 1\n}\n", i);
			loading_write(sub, text);
		}
		loading_write("materials/svrtv/loading_logo.vmt",
			"\"UnlitGeneric\"\n{\n\t\"$basetexture\" \"gamepadui/game_logo\"\n\t\"$translucent\" 1\n"
			"\t\"$ignorez\" 1\n\t\"$nomip\" 1\n}\n");
	}
	// From the eyes' first call in a frame, once after 3D starts natively: the
	// copies for this packing, for the next start.
	void loading_update()
	{
		if (!g_cfg.loading || !m_loadingRT || !m_loadingDirty)
			return;
		m_loadingDirty = false;
		if (!in_gamescope())
			loading_make();
	}

	void install_effect()
	{
#ifndef _WIN32
		const char *xdg = getenv("XDG_DATA_HOME"), *home = getenv("HOME");
		char dir[1100], src[1200], dst[1200];
		if (xdg && *xdg)
			snprintf(dir, sizeof(dir), "%s/gamescope/reshade/Shaders", xdg);
		else if (home && *home)
			snprintf(dir, sizeof(dir), "%s/.local/share/gamescope/reshade/Shaders", home);
		else
			return;
		for (char *c = dir + 1; *c; c++)
			if (*c == '/') {
				*c = 0;
				mkdir(dir, 0755);
				*c = '/';
			}
		mkdir(dir, 0755);
		snprintf(src, sizeof(src), "%ssvrtv-anaglyph.fx", g_dir);
		snprintf(dst, sizeof(dst), "%s/svrtv-anaglyph.fx", dir);
		FILE *in = fopen(src, "rb");
		if (!in)
			return;
		FILE *out = fopen(dst, "wb");
		if (out) {
			char buf[4096];
			size_t n;
			while ((n = fread(buf, 1, sizeof(buf), in)) > 0)
				fwrite(buf, 1, n, out);
			fclose(out);
		}
		fclose(in);
#endif
	}
	void gamescope_effect(int technique)
	{
		if (!x11())
			return;
		unsigned long root = m_xRoot(m_xdpy);
		unsigned long effect = m_xInternAtom(m_xdpy, "GAMESCOPE_RESHADE_EFFECT", 0);
		unsigned long index = m_xInternAtom(m_xdpy, "GAMESCOPE_RESHADE_TECHNIQUE_IDX", 0);
		if (technique < 0) {
			m_xDelete(m_xdpy, root, effect);
		} else {
			install_effect();
			long idx = technique;
			const char *name = "svrtv-anaglyph.fx";
			m_xChange(m_xdpy, root, index, 6 /* XA_CARDINAL */, 32, 0 /* PropModeReplace */, (const unsigned char *)&idx, 1);
			m_xChange(m_xdpy, root, effect, 31 /* XA_STRING */, 8, 0, (const unsigned char *)name, (int)strlen(name));
		}
		m_xFlush(m_xdpy);
		m_effectSet = technique >= 0;
		logf("output: gamescope effect %s (technique %d)\n", technique < 0 ? "off" : "svrtv-anaglyph.fx", technique);
	}
	void apply_output()
	{
		int out = g_cfg.output;
		if (!in_gamescope()) {
			if (out != OUT_DISPLAY && out != OUT_SYSTEM)
				command("echo \"3D output: this output needs gamescope; showing the 3D display format instead\"");
			return;
		}
		// The system draws the eyes itself: gamescope's effect has nothing to do
		// (and its ReShade aborted gamescope on the 3840x1080 frame, run sysb 5a).
		if ((out == OUT_DISPLAY && !g_cfg.loading) || out == OUT_SYSTEM) {
			if (m_effectSet)
				gamescope_effect(-1);
			return;
		}
		// svrtv-anaglyph.fx techniques, from side by side / from top and bottom.
		// The full formats pack into the same screen, so they use the same ones.
		// The 3D display itself gets 10/9: the frames as they are, loading
		// screens whole in both halves (mark_frame); the system the same.
		static const int techniques[OUT_COUNT][2] = { { 10, 9 }, { 0, 3 }, { 1, 4 }, { 6, 5 }, { 7, 8 }, { 10, 9 } };
		gamescope_effect(techniques[out][packed_tab() ? 1 : 0]);
	}
	void clear_output()
	{
		if (m_effectSet)
			gamescope_effect(-1);
	}

	// The full formats and frame packing need a video mode of the frame's
	// size (3840x1080, 1920x2160, 1920x2205, 1280x1470). The module sets it
	// when 3D starts and puts the player's own back when 3D stops; a marker
	// keeps it if the game quits in 3D, or the frame size would be saved as
	// the player's resolution.
	IMaterialSystem *material_system()
	{
		if (!m_ms && m_factory)
			m_ms = (IMaterialSystem *)m_factory(MATERIAL_SYSTEM_INTERFACE_VERSION_OLD, NULL);
		return m_ms;
	}
	bool mode_now(int *w, int *h, bool *windowed)
	{
		IMaterialSystem *ms = material_system();
		if (!ms)
			return false;
		const MaterialSystem_Config_t &cfg = ms->GetCurrentConfigForVideoCard();
		*w = cfg.m_VideoMode.m_Width;
		*h = cfg.m_VideoMode.m_Height;
		*windowed = cfg.Windowed();
		return *w > 0 && *h > 0;
	}
	bool saved_mode(int *w, int *h, int *windowed)
	{
		char p[1200];
		snprintf(p, sizeof(p), "%ssvrtv-restore-mode", g_dir);
		FILE *f = fopen(p, "r");
		if (!f)
			return false;
		bool ok = fscanf(f, "%d %d %d", w, h, windowed) == 3;
		fclose(f);
		return ok && *w > 0 && *h > 0;
	}
	// The 2D size is the player's own video mode (the one kept in the marker
	// while a frame-size mode is set).
	void size_from_mode()
	{
		if (g_cfg.sizeGiven)
			return;
		int w, h, win;
		bool windowed;
		if (saved_mode(&w, &h, &win) || mode_now(&w, &h, &windowed)) {
			// for the system the video mode holds two 2D-size eyes side by side
			g_cfg.width = g_cfg.output == OUT_SYSTEM ? w / 2 : w;
			g_cfg.height = h;
		}
	}
	// Only native frame packing needs a video mode of its own (the HDMI 3D
	// modes, once the kernel lists them); every other format uses the screen
	// as it is.
	bool needs_frame_mode()
	{
		int f = g_cfg.format;
		return !in_gamescope() && g_cfg.output == OUT_DISPLAY && (f == FMT_FP1080 || f == FMT_FP720);
	}
	void frame_mode_on()
	{
		if (!needs_frame_mode())
			return;
		int fw, fh, w, h;
		bool windowed;
		frame_size(&fw, &fh);
		if (!mode_now(&w, &h, &windowed) || (w == fw && h == fh))
			return;
		int sw, sh, swin;
		if (!saved_mode(&sw, &sh, &swin)) {
			char p[1200];
			snprintf(p, sizeof(p), "%ssvrtv-restore-mode", g_dir);
			FILE *f = fopen(p, "w");
			if (f) {
				fprintf(f, "%d %d %d\n", w, h, windowed ? 1 : 0);
				fclose(f);
			}
		}
		char cmd[96];
		snprintf(cmd, sizeof(cmd), "mat_setvideomode %d %d %d", fw, fh, windowed ? 1 : 0);
		command(cmd);
		m_modeCheckFrame = m_frame + 60;
	}
	// A moment after asking for a frame-size mode, the back buffer says
	// whether it was granted; logged only, so the run shows it as it is.
	void check_frame_mode()
	{
		if (!m_modeCheckFrame || m_frame < m_modeCheckFrame || !material_system())
			return;
		m_modeCheckFrame = 0;
		int fw, fh, bw, bh;
		frame_size(&fw, &fh);
		m_ms->GetBackBufferDimensions(bw, bh);
		logf("frame mode %dx%d %s: back buffer %dx%d\n", fw, fh, (bw == fw && bh == fh) ? "granted" : "NOT granted", bw, bh);
	}
	void frame_mode_off()
	{
		int w, h, windowed;
		if (!saved_mode(&w, &h, &windowed))
			return;
		char p[1200];
		snprintf(p, sizeof(p), "%ssvrtv-restore-mode", g_dir);
		remove(p);
		char cmd[96];
		snprintf(cmd, sizeof(cmd), "mat_setvideomode %d %d %d", w, h, windowed);
		command(cmd);
	}

	// Anisotropic filtering 16x while 3D is on: slanted surfaces stay sharp
	// in both eyes (Daniel, 2026-09-25). The player's own value is kept in a
	// marker next to the module and put back when 3D stops, or at the next
	// start if the game quit in 3D, like the crosshair and motion blur.
	void set_for_3d(const char *name, int value)
	{
		if (!g_pCVar)   // not connected to the engine (a test harness)
			return;
		char p[1200];
		snprintf(p, sizeof(p), "%ssvrtv-restore-%s", g_dir, name);
		FILE *f = fopen(p, "r");
		if (f) {
			fclose(f);   // already set, the player's value already kept
		} else {
			ConVarRef var(name);
			if (!var.IsValid())
				return;
			f = fopen(p, "w");
			if (f) {
				fprintf(f, "%d\n", var.GetInt());
				fclose(f);
			}
		}
		char cmd[128];
		snprintf(cmd, sizeof(cmd), "%s %d", name, value);
		command(cmd);
	}
	void restore_after_3d(const char *name)
	{
		char p[1200];
		snprintf(p, sizeof(p), "%ssvrtv-restore-%s", g_dir, name);
		FILE *f = fopen(p, "r");
		if (!f)
			return;
		int was = 0;
		bool ok = fscanf(f, "%d", &was) == 1;
		fclose(f);
		if (ok) {
			char cmd[128];
			snprintf(cmd, sizeof(cmd), "%s %d", name, was);
			command(cmd);
		}
		remove(p);
	}

	void blur_restore()
	{
		char p[1200];
		blur_marker(p, sizeof(p));
		FILE *f = fopen(p, "r");
		if (!f)
			return;
		int was = 1;
		if (fscanf(f, "%d", &was) != 1)
			was = 1;
		fclose(f);
		char cmd[64];
		snprintf(cmd, sizeof(cmd), "mat_motion_blur_enabled %d", was ? 1 : 0);
		command(cmd);
		remove(p);
	}

	// HL2's classic crosshair (hud_textures.txt "crosshair_default":
	// sprites/crosshairs, x 0 y 48, 24x24 of the 128x128 sheet), drawn with
	// sprites/crosshairs_tluc, which blends by the texture's alpha. The
	// quick-info dot (sprites/qi_center) is additive, DXT1 with no alpha: its
	// look depends on what is behind it, which differs between the eyes
	// (Daniel, 2026-09-24), and it cannot be drawn into the transparent HUD
	// sheet (a black square). Drawn once per eye, at the centre of the HUD
	// rectangle, so both eyes get the same pixels and it sits on the screen
	// plane.
	void draw_crosshair(IMatRenderContext *ctx, int x0, int y0, int w, int h)
	{
		IMaterial *m = m_ms->FindMaterial("sprites/crosshairs_tluc", TEXTURE_GROUP_VGUI, false);
		if (!m || m->IsErrorMaterial())
			return;
		if (!m_xhairReady) {
			m_xhairReady = true;
			m->IncrementReferenceCount();
			m_ms->CacheUsedMaterials();
			logf("crosshair material %s: precached %d\n", m->GetName(), (int)m->IsPrecached());
		}
		int cw = w * 24 / 640, ch = h * 24 / 480;
		ctx->DrawScreenSpaceRectangle(m, x0 + (w - cw) / 2, y0 + (h - ch) / 2, cw, ch, 0, 48, 23, 71, 128, 128);
	}

	// The game's SDL2, found in the running process (Steam's runtime loads
	// libSDL2-2.0.so.0 for the launcher).
	typedef unsigned int (*SdlGetMouseState)(int *, int *);
	typedef void *(*SdlGetFocus)(void);
	typedef void (*SdlGetWindowSize)(void *, int *, int *);
	typedef int (*SdlGetRelative)(void);
	void *sdl(const char *name)
	{
		// The launcher loads SDL privately, so look it up by name in the
		// already-loaded library, never loading a second copy.
#ifdef _WIN32
		if (!m_sdlLib)
			m_sdlLib = (void *)GetModuleHandleA("SDL2.dll");
		void *f = m_sdlLib ? (void *)GetProcAddress((HMODULE)m_sdlLib, name) : NULL;
#else
		if (!m_sdlLib)
			m_sdlLib = dlopen("libSDL2-2.0.so.0", RTLD_NOW | RTLD_NOLOAD);
		void *f = m_sdlLib ? dlsym(m_sdlLib, name) : dlsym(RTLD_DEFAULT, name);
#endif
		if (!f && m_mouseLogs < 400)
			logf("SDL: %s not found\n", name);
		return f;
	}

	// The game's UI cursor uses window pixels 1:1 and the UI is laid out at
	// the 2D size, so when the window is bigger than the UI (the full formats,
	// or a gamescope screen bigger than the game) the cursor can walk off what
	// the eyes show (mouse log, 2026-09-24: the cursor left the 640x480 sheet
	// down to y 819; run q20: the menu out of reach in a 3840x2160 gamescope
	// screen). Confine the system pointer to the UI's area then, through the
	// game's own SDL2. Established solution 2 (README).
	struct SdlRect { int x, y, w, h; };
	typedef int (*SdlSetMouseRect)(void *, const SdlRect *);
	typedef void (*SdlWarp)(void *, int, int);
	void *sdl_window()
	{
		if (!m_getFocus)
			m_getFocus = (SdlGetFocus)sdl("SDL_GetMouseFocus");
		if (!m_getKeyFocus)
			m_getKeyFocus = (SdlGetFocus)sdl("SDL_GetKeyboardFocus");
		void *w = m_getFocus ? m_getFocus() : NULL;
		if (!w && m_getKeyFocus)
			w = m_getKeyFocus();
		return w;
	}
	bool needs_fence()
	{
		// Never inside gamescope: its relative mouse pins a fenced pointer
		// (runs q15/q16, and again q20). Established solution.
		if (in_gamescope())
			return false;
		// The system keeps the pointer on the left eye itself (KWin maps it
		// there); the fence held it in the window, so no other window could be
		// reached (run sysb native, 2026-10-01).
		if (g_cfg.output == OUT_SYSTEM)
			return false;
		int fw, fh;
		frame_size(&fw, &fh);
		int W = g_cfg.width, H = g_cfg.height;
		return fw > W || fh > H;
	}
	void confine_mouse(bool on)
	{
		if (!g_cfg.confine)
			return;
		if (on && !needs_fence())
			on = false;
		if (!on && !m_confined)
			return;
		if (!m_setMouseRect && !m_triedMouseRect) {
			m_triedMouseRect = true;
			m_setMouseRect = (SdlSetMouseRect)sdl("SDL_SetWindowMouseRect");
			m_warp = (SdlWarp)sdl("SDL_WarpMouseInWindow");
			if (!m_getState)
				m_getState = (SdlGetMouseState)sdl("SDL_GetMouseState");
			if (!m_getRel)
				m_getRel = (SdlGetRelative)sdl("SDL_GetRelativeMouseMode");
		}
		void *win = sdl_window();
		if (!win)
			return;
		int W = g_cfg.width, H = g_cfg.height;
		if (m_setMouseRect) {
			if (on && win == m_confined)
				return;
			SdlRect rc = { 0, 0, W, H };
			int rv = m_setMouseRect(win, on ? &rc : NULL);
			m_confined = on ? win : NULL;
			logf("mouse %s to %dx%d (SDL_SetWindowMouseRect: %d)\n", on ? "confined" : "released", W, H, rv);
			return;
		}
		// Older SDL: pull the pointer back each frame while the cursor is
		// free (menus); relative mode (play) needs nothing.
		m_confined = on ? win : NULL;
		if (on && m_warp && m_getState && !(m_getRel && m_getRel())) {
			int x, y;
			m_getState(&x, &y);
			if (x > W - 1 || y > H - 1)
				m_warp(win, x > W - 1 ? W - 1 : x, y > H - 1 ? H - 1 : y);
		}
	}

	// Diagnostics: the system pointer (SDL, window pixels) next to the game's
	// UI cursor (VGUI), when either moves.
	void log_mouse()
	{
		if (m_mouseLogs >= 400)
			return;
		if (!m_sdlLooked) {
			m_sdlLooked = true;
			m_getState = (SdlGetMouseState)sdl("SDL_GetMouseState");
			m_getFocus = (SdlGetFocus)sdl("SDL_GetMouseFocus");
			m_getSize = (SdlGetWindowSize)sdl("SDL_GetWindowSize");
			m_getRel = (SdlGetRelative)sdl("SDL_GetRelativeMouseMode");
		}
		SdlGetMouseState get_state = m_getState;
		SdlGetFocus get_focus = m_getFocus;
		SdlGetWindowSize get_size = m_getSize;
		SdlGetRelative get_rel = m_getRel;
		if (!m_input && m_factory) {
			m_input = (vgui::IInput *)m_factory(VGUI_INPUT_INTERFACE_VERSION, NULL);
			logf("VGUI input %s: %s\n", VGUI_INPUT_INTERFACE_VERSION, m_input ? "found" : "not found");
		}
		int sx = -1, sy = -1, ww = 0, wh = 0, ux = -1, uy = -1, rel = -1;
		if (get_state)
			get_state(&sx, &sy);
		void *win = get_focus ? get_focus() : NULL;
		if (win && get_size)
			get_size(win, &ww, &wh);
		if (get_rel)
			rel = get_rel();
		if (m_input)
			m_input->GetCursorPos(ux, uy);
		if (sx == m_lastMouse[0] && sy == m_lastMouse[1] && ux == m_lastMouse[2] && uy == m_lastMouse[3])
			return;
		m_lastMouse[0] = sx; m_lastMouse[1] = sy; m_lastMouse[2] = ux; m_lastMouse[3] = uy;
		m_mouseLogs++;
		logf("mouse frame %d: window %d,%d of %dx%d relative %d | ui %d,%d\n", m_frame, sx, sy, ww, wh, rel, ux, uy);
	}

	// Diagnostics: a render target (NULL: the frame) to a 32-bit TGA next to
	// the module.
	void dump(IMatRenderContext *ctx, ITexture *t, const char *name)
	{
		int fw, fh;
		frame_size(&fw, &fh);
		int w = t ? t->GetActualWidth() : fw;
		int h = t ? t->GetActualHeight() : fh;
		unsigned char *px = (unsigned char *)malloc((size_t)w * h * 4);
		if (!px)
			return;
		memset(px, 0, (size_t)w * h * 4);
		ctx->PushRenderTargetAndViewport(t);
		ctx->ReadPixels(0, 0, w, h, px, IMAGE_FORMAT_BGRA8888);
		ctx->PopRenderTargetAndViewport();
		char path[1200];
		snprintf(path, sizeof(path), "%s%s", g_dir, name);
		FILE *f = fopen(path, "wb");
		if (f) {
			// Uncompressed true-colour TGA, top-left origin.
			unsigned char hdr[18] = { 0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0,
				(unsigned char)(w & 255), (unsigned char)(w >> 8),
				(unsigned char)(h & 255), (unsigned char)(h >> 8), 32, 0x28 };
			fwrite(hdr, 1, sizeof(hdr), f);
			fwrite(px, 1, (size_t)w * h * 4, f);
			fclose(f);
		}
		logf("dump %s: %dx%d at frame %d\n", name, w, h, m_frame);
		free(px);
	}

	// Copies an eye's target into its half of the frame, once per frame.
	void show(VREye eye)
	{
		int i = (eye == VREye_Left) ? 0 : 1;
		if (!m_ms || !m_rt[i] || m_shown[i])
			return;
		m_shown[i] = true;
		int hx, hy, hw, hh;
		half(eye, &hx, &hy, &hw, &hh);
		int ew, eh;
		eye_size(eye, &ew, &eh);
		Rect_t src = { 0, 0, ew, eh };
		Rect_t dst = { hx, hy, hw, hh };
		CMatRenderContextPtr ctx(m_ms);
		ctx->PushRenderTargetAndViewport(NULL);
		ctx->CopyTextureToRenderTargetEx(0, m_rt[i], &src, &dst);
		ctx->PopRenderTargetAndViewport();
	}

	// Fixed pose: the head is where the game camera is.
	VMatrix GetMideyePose()
	{
		VMatrix m;
		set_identity(m);
		return m;
	}

	// The UI sheet is the window's own size: a television shows the HUD and
	// menus as in 2D, sharp and whole, and the pointer maps 1:1. The client's
	// 640x480 VR UI size exists for a headset's floating panel (#8297 items 4
	// and 6).
	int ui_width() { return g_cfg.width; }
	int ui_height() { return g_cfg.height; }
	// The client's Activate() sets the UI to 640x480 (a headset's floating
	// panel) after the module's; the module sets it back to the window's size
	// as soon as the client next calls in (GetRenderTarget), still inside its
	// Activate(), so nothing lays itself out at 640x480 (GamepadUI kept such a
	// layout, run q06).
	void ui_full_size()
	{
		if (m_uiFull || !m_factory)
			return;
		m_uiFull = true;
		vgui::ISurface *surface = (vgui::ISurface *)m_factory(VGUI_SURFACE_INTERFACE_VERSION, NULL);
		bool ok = surface && surface->ForceScreenSizeOverride(true, g_cfg.width, g_cfg.height);
		logf("ui: laid out at %dx%d%s (%s)\n", g_cfg.width, g_cfg.height, gamepadui_loaded() ? ", GamepadUI" : "",
		     surface ? (ok ? "ok" : "refused") : "no surface");
	}

	// Called with the game's field of view each frame; keep it for the
	// projection.
	bool SampleTrackingState(float playerGameFov, float)
	{
		beat("SampleTrackingState");
		// Called once per frame before the eyes render: a new frame.
		m_frame++;
		if (g_cfg.mouselog && (m_frame % 30) == 0)
			log_mouse();
		if (g_cfg.anglelog)
			record_angles();
		if (m_active) {
			confine_mouse(true);
			ui_full_size();
			check_frame_mode();
		}
		m_shown[0] = m_shown[1] = false;
		m_hudCopied = false;
		// The client passes 0 while the view has the game's default field of
		// view, and otherwise the view's FOV divided by vr_zoom_multiplier (a
		// headset HUD adjustment). A display needs the real FOV: the default
		// on 0, the value times the multiplier otherwise. Keeping the last
		// value on 0 left a changed FOV stuck (HL2:DM at 90 read 45, run r01).
		float fov = playerGameFov;
		if (g_pCVar) {
			static ConVarRef defaultFov("default_fov"), zoomMultiplier("vr_zoom_multiplier");
			if (fov <= 1.0f)
				fov = defaultFov.IsValid() ? defaultFov.GetFloat() : 75.0f;
			else if (zoomMultiplier.IsValid() && zoomMultiplier.GetFloat() > 0.0f)
				fov *= zoomMultiplier.GetFloat();
		}
		if (fov > 1.0f && fov < 179.0f) {
			if (fov != m_fovX && m_fovLogs < 8) {
				m_fovLogs++;
				logf("game fov %.3f (passed %.3f) at frame %d\n", fov, playerGameFov, m_frame);
			}
			m_fovX = fov;
		}
		return true;
	}

	bool GetDisplayBounds(VRRect_t *r)
	{
		beat("GetDisplayBounds");
		r->nX = 0;
		r->nY = 0;
		frame_size(&r->nWidth, &r->nHeight);
		return true;
	}

	bool GetEyeProjectionMatrix(VMatrix *out, VREye eye, float zNear, float zFar, float fovScale)
	{
		beat("GetEyeProjectionMatrix");
		// Source's fov is defined for a 4:3 screen; widen it to the displayed
		// aspect as the engine does for widescreen, then apply the zoom scale.
		double t = tan(m_fovX * M_PI / 360.0) * (g_cfg.aspect / (4.0 / 3.0));
		if (fovScale > 0.0f)
			t *= fovScale;
		// Tangent bounds of the mid-eye window, shifted for this eye so the
		// window at the convergence distance is shared by both eyes. The left
		// eye sits to the left of the mid-eye, so its window centre lies to its
		// right.
		double shift = eye_sign(eye) * (g_cfg.separation / 2.0) / g_cfg.convergence;
		double l = -t + shift, r = t + shift;
		double b = -t / g_cfg.aspect, tp = t / g_cfg.aspect;

		VMatrix &m = *out;
		for (int i = 0; i < 4; i++)
			for (int j = 0; j < 4; j++)
				m.m[i][j] = 0.0f;
		m.m[0][0] = (float)(2.0 / (r - l));
		m.m[0][2] = (float)((r + l) / (r - l));
		m.m[1][1] = (float)(2.0 / (tp - b));
		m.m[1][2] = (float)((tp + b) / (tp - b));
		m.m[2][2] = zFar / (zNear - zFar);
		m.m[2][3] = zNear * zFar / (zNear - zFar);
		m.m[3][2] = -1.0f;
		return true;
	}

	// Mid-eye to eye: a sideways shift of half the separation. Source's +y is
	// left, so the left eye moves +y and the right eye -y.
	VMatrix GetMidEyeFromEye(VREye eye)
	{
		beat("GetMidEyeFromEye");
		VMatrix m;
		set_identity(m);
		m.m[1][3] = (float)(eye_sign(eye) * g_cfg.separation / 2.0);
		return m;
	}

	int GetVRModeAdapter() { return 0; }
	bool WillDriftInYaw() { return false; }

	// One colour target per eye, the size of its half of the frame; depth is
	// the frame's own (MATERIAL_RT_DEPTH_SHARED, which needs a target no
	// larger than the frame). The engine calls this inside its render-target
	// allocation. Without targets, each eye renders straight into its half.
	void CreateRenderTargets(IMaterialSystem *ms)
	{
		trace(2, "CreateRenderTargets");
		if (!g_cfg.enabled || !ms)
			return;
		m_ms = ms;
		m_triedTargets = true;
		make_targets();
	}

	// This engine never calls CreateRenderTargets (2026-09-24 log: the
	// client asks for GetRenderTarget first). So the targets are made on
	// first use, inside the allocation bracket render targets need.
	void ensure_targets()
	{
		if (select_targets())
			return;
		if (!g_cfg.enabled || !m_active)
			return;
		m_triedTargets = true;
		if (!m_ms && m_factory) {
			m_ms = (IMaterialSystem *)m_factory(MATERIAL_SYSTEM_INTERFACE_VERSION_OLD, NULL);
			logf("material system %s: %s\n", MATERIAL_SYSTEM_INTERFACE_VERSION_OLD, m_ms ? "found" : "not found");
		}
		if (!m_ms)
			return;
		m_ms->BeginRenderTargetAllocation();
		make_targets();
		m_ms->EndRenderTargetAllocation();
	}

	// Eye targets, one pair per eye size (format and rendering resolution),
	// made on first use and kept.
	bool select_targets()
	{
		int w, h;
		eye_size(VREye_Left, &w, &h);
		for (int i = 0; i < m_nTargets; i++)
			if (m_targets[i].w == w && m_targets[i].h == h) {
				m_rt[0] = m_targets[i].rt[0];
				m_rt[1] = m_targets[i].rt[1];
				m_shotRT = m_targets[i].shot;
				return true;
			}
		return false;
	}

	void make_targets()
	{
		IMaterialSystem *ms = m_ms;
		int w, h;
		eye_size(VREye_Left, &w, &h);
		for (int i = 0; i < 2; i++) {
			char name[64];
			snprintf(name, sizeof(name), "_rt_svrtv_%s_%dx%d", i ? "right" : "left", w, h);
			m_rt[i] = ms->CreateNamedRenderTargetTextureEx(name, w, h, RT_SIZE_LITERAL,
				ms->GetBackBufferFormat(), MATERIAL_RT_DEPTH_SHARED,
				0x4 | 0x8 | 0x100 | 0x200,   // TEXTUREFLAGS_CLAMPS|CLAMPT|NOMIP|NOLOD
				0);
			logf("render target %s: %dx%d made\n", name,
			     m_rt[i] ? m_rt[i]->GetActualWidth() : 0, m_rt[i] ? m_rt[i]->GetActualHeight() : 0);
		}
		m_shotRT = NULL;
		if (g_cfg.shots && m_rt[0] && m_rt[1]) {
			char name[64];
			snprintf(name, sizeof(name), "_rt_svrtv_shot_%dx%d", w, h);
			m_shotRT = ms->CreateNamedRenderTargetTextureEx(name, w, h, RT_SIZE_LITERAL,
				ms->GetBackBufferFormat(), MATERIAL_RT_DEPTH_NONE,
				0x4 | 0x8 | 0x100 | 0x200, 0);
			logf("render target %s: %s\n", name, m_shotRT ? "made" : "NOT made");
		}
		if (!m_rt[0] || !m_rt[1])
			m_rt[0] = m_rt[1] = NULL;
		else if (m_nTargets < MAX_TARGETS) {
			m_targets[m_nTargets].w = w;
			m_targets[m_nTargets].h = h;
			m_targets[m_nTargets].rt[0] = m_rt[0];
			m_targets[m_nTargets].rt[1] = m_rt[1];
			m_targets[m_nTargets].shot = m_shotRT;
			m_nTargets++;
		}
		if (m_uiMade)
			return;
		m_uiMade = true;

		// The HUD sheet. The client paints the HUD and menus into "_rt_gui"
		// (640x480, with alpha for the in-world HUD material) and this game
		// makes it only when VR was set up at start; without it there is no
		// HUD in 3D and the loading screen sits in a corner.
		ITexture *gui = ms->FindTexture("_rt_gui", NULL, false);
		if (!gui || gui->IsError()) {
			gui = ms->CreateNamedRenderTargetTextureEx("_rt_gui", ui_width(), ui_height(), RT_SIZE_LITERAL,
				IMAGE_FORMAT_RGBA8888, MATERIAL_RT_DEPTH_SHARED,
				0x4 | 0x8 | 0x100 | 0x200, 0);
			logf("render target _rt_gui: made %dx%d\n",
			     gui ? gui->GetActualWidth() : 0, gui ? gui->GetActualHeight() : 0);
		} else
			logf("render target _rt_gui: present %dx%d\n", gui->GetActualWidth(), gui->GetActualHeight());

		// A fresh copy of the sheet for the HUD material to sample. When
		// _rt_gui starts life as the engine's error placeholder, the object
		// works as a render target (the client paints the HUD into it; a
		// plain copy of it shows the HUD) but a material sampling it still
		// gets the purple-black error texture, even after a Refresh()
		// (2026-09-24). A target under a new name has no such history.
		m_hud = ms->CreateNamedRenderTargetTextureEx("_rt_svrtv_gui", ui_width(), ui_height(), RT_SIZE_LITERAL,
			IMAGE_FORMAT_RGBA8888, MATERIAL_RT_DEPTH_NONE,
			0x4 | 0x8 | 0x100 | 0x200, 0);
		logf("render target _rt_svrtv_gui: made %dx%d\n",
		     m_hud ? m_hud->GetActualWidth() : 0, m_hud ? m_hud->GetActualHeight() : 0);

		// Where the native loading screens' stereo pictures are drawn.
		if (g_cfg.loading && !in_gamescope() && loading_available()) {
			m_loadingRT = ms->CreateNamedRenderTargetTextureEx("_rt_svrtv_loading", LOADING_W, LOADING_H, RT_SIZE_LITERAL,
				ms->GetBackBufferFormat(), MATERIAL_RT_DEPTH_NONE,
				0x4 | 0x8 | 0x100 | 0x200, 0);
			logf("loading screens: target %dx%d %s\n", LOADING_W, LOADING_H, m_loadingRT ? "made" : "NOT made");
		}
	}
	void ShutdownRenderTargets()
	{
		trace(3, "ShutdownRenderTargets");
		m_rt[0] = m_rt[1] = NULL;
		m_shotRT = NULL;
		m_nTargets = 0;
	}
	ITexture *GetRenderTarget(VREye eye, EWhichRenderTarget which)
	{
		beat("GetRenderTarget");
		trace(4, "GetRenderTarget");
		ensure_targets();
		// The client asks for the UI's render target right after setting the
		// UI to 640x480 in its Activate(); resizing here leaves no frame in
		// which a GamepadUI panel can lay itself out at 640x480 (run q06: the
		// options panel kept a 640x480 layout with full-size text).
		if (m_active)
			ui_full_size();
		if (which != RT_Color)
			return NULL;
		return m_rt[eye == VREye_Left ? 0 : 1];
	}
	void GetRenderTargetFrameBufferDimensions(int &w, int &h)
	{
		frame_size(&w, &h);
	}

	bool Activate()
	{
		if (!g_cfg.enabled)
			return false;
		m_active = true;
		size_from_mode();
		logf("activated: %s, %dx%d 2D\n", g_cfg.output == OUT_SYSTEM ? "system (side by side, full)" : g_formatNames[g_cfg.format], g_cfg.width, g_cfg.height);
		command(g_cfg.onvr);
		if (g_cfg.xhair && !m_clientCrosshair)
			crosshair_off();
		if (g_cfg.bluroff)
			blur_off();
		if (g_cfg.aniso > 0)
			set_for_3d("mat_forceaniso", g_cfg.aniso);
		launch_3d_mark();
		frame_mode_on();
		apply_output();
		m_loadingDirty = true;   // the native loading pictures for this packing
		shots_hook(true);
		return true;
	}
	void Deactivate()
	{
		m_active = false;
		logf("deactivated\n");
		shots_hook(false);
		m_uiFull = false;
		confine_mouse(false);
		clear_output();
		frame_mode_off();
		crosshair_restore();
		blur_restore();
		restore_after_3d("mat_forceaniso");
		launch_3d_unmark();
		if (m_switchOff) {
			m_switchOff = false;
			g_cfg.enabled = false;
		}
		if (m_relayout) {
			g_cfg.format = m_relayout - 1;
			m_relayout = 0;
			command("vr_activate");
		}
	}

	// "VR because Steam said so": skips the client's headset-adapter checks.
	// The client asks once startup is complete (after config.cfg), which is
	// also when a crosshair left off by a VR session is turned back on.
	bool ShouldForceVRMode()
	{
		// config.cfg has run by now, so the saved switch is known; a value
		// left by a launch-option session that quit in 3D goes first. Only on
		// the first call, at startup: the client asks again from its own
		// Activate(), after the module has marked 3D on (run q05).
		if (!m_startupDone) {
			launch_3d_unmark();
			if (g_pCVar) {
				g_cfg.output = g_cfg.outputForced >= 0 ? g_cfg.outputForced : vr_display_output.GetInt();
				g_cfg.swap = vr_display_swap.GetBool();
				menu_flags();
			}
		}
		if (!g_cfg.enabled && vr_display_3d.GetInt() > 0) {
			g_cfg.enabled = true;
			g_cfg.format = vr_display_layout.GetInt();
		} else if (g_cfg.enabled && !g_cfg.layoutGiven && !m_startupDone)
			g_cfg.format = vr_display_layout.GetInt();   // -stereo3d alone: the saved format
		// A video mode left by a session that quit in a full format: kept when
		// 3D starts in one again, put back otherwise.
		if (!m_startupDone && !(g_cfg.enabled && needs_frame_mode()))
			frame_mode_off();
		m_startupDone = true;
		if (!g_cfg.enabled || !g_cfg.xhair)
			crosshair_restore();
		if (!g_cfg.enabled || !g_cfg.bluroff)
			blur_restore();
		if (!g_cfg.enabled || g_cfg.aniso <= 0)
			restore_after_3d("mat_forceaniso");
		return g_cfg.enabled;
	}

	// A setting changed while the game runs. The video options write the
	// three on Apply one after another and give no signal when done, so the
	// reload runs once, on the next engine frame (a console command), after
	// the last. Before startup completes it is config.cfg loading the saved
	// values, which ShouldForceVRMode() reads.
	void queue_apply()
	{
		if (m_selfSet || !m_startupDone || m_applyQueued)
			return;
		m_applyQueued = true;
		command("vr_display_apply");
	}
	// Apply (Daniel's design): read all three settings and apply them at
	// once, with one full reload of the 3D path: the client's own deactivate,
	// then activate with the new values (instant, run q07).
	void apply_settings()
	{
		m_applyQueued = false;
		launch_3d_forget();
		bool on = vr_display_3d.GetInt() > 0;
		int format = vr_display_layout.GetInt();
		g_cfg.output = g_cfg.outputForced >= 0 ? g_cfg.outputForced : vr_display_output.GetInt();
		g_cfg.swap = vr_display_swap.GetBool();
		menu_flags();
		if (g_cfg.enabled && m_active) {
			// Deactivate() below activates again with the new settings, or ends
			// 3D; the client's Deactivate() first asks ShouldRunInVR(), so 3D
			// stays on until it has run.
			m_relayout = on ? format + 1 : 0;
			m_switchOff = !on;
			command("vr_deactivate");
		} else if (on) {
			g_cfg.enabled = true;
			g_cfg.format = format;
			command("vr_activate");   // Activate() applies the output
		}
	}
	void SetShouldForceVRMode() {}

private:
	// Logs the first call of each traced method: which parts of the
	// interface the engine and client actually use, and in what order.
	void trace(int n, const char *what)
	{
		if (n < 0 || n >= 8 || m_traced[n])
			return;
		m_traced[n] = true;
		logf("first call: %s\n", what);
	}

	bool m_active;
	bool m_clientCrosshair;   // the client places the crosshair (it asked for the display interface)
	ITexture *m_loadingRT;
	IMaterial *m_loadingMat[LOADING_BGS + 1];
	bool m_loadingDirty;   // the native loading pictures to make
	double m_lastCall, m_lastLoadingLog;   // SVRTV_CALLTRACE
	const char *m_lastWhat;
	float m_fovX;
	IMaterialSystem *m_ms;
	CreateInterfaceFn m_factory;
	bool m_triedTargets;
	enum { MAX_TARGETS = 8 };
	struct EyeTargets { int w, h; ITexture *rt[2]; ITexture *shot; };
	EyeTargets m_targets[MAX_TARGETS];   // one pair per eye size
	int m_nTargets;
	bool m_uiMade;              // HUD sheets made (shared by both layouts)
	bool m_applyQueued;         // vr_display_apply queued, not yet run
	bool m_effectSet;           // a gamescope effect is set by the module
	bool m_xTried;
	void *m_xdpy;
	XInternAtomFn m_xInternAtom;
	XDefaultRootWindowFn m_xRoot;
	XChangePropertyFn m_xChange;
	XDeletePropertyFn m_xDelete;
	XFlushFn m_xFlush;
	int m_modeCheckFrame;       // the frame at which to log whether the frame mode was granted (0: none)
	int m_relayout;             // Apply while on: the format + 1 to activate again with, in Deactivate()
	ITexture *m_rt[2];
	ITexture *m_shotRT;         // eye-sized: where a stereo screenshot gets its HUD
	IMaterial *m_hudMatUsed;    // this frame's HUD paste, repeated in a screenshot
	ITexture *m_hudSheetUsed;
	bool m_hudTranslucent;
	bool m_engineShotPending;   // the engine took a screenshot: the module's pair follows once it is done
	bool m_steamLooked;
	void *m_steamLib;
	ISteamScreenshots *m_shotsApi;
	HookScreenshotsFn m_hookShots;
	AddVRScreenshotFn m_addVRShot;
	SetLocationFn m_setLocation;
	UnregisterCallbackFn m_unregister;
	bool m_shotsHooked;         // Steam's screenshot key is the module's
	bool m_shown[2];
	bool m_traced[8];
	int m_hudLogs;
	ITexture *m_hud;
	bool m_hudCopied;
	bool m_matReady;
	int m_frame;
	int m_fovLogs;
	int m_dumps;
	IVEngineClient *m_engine;
	bool m_xhairReady;
	vgui::IInput *m_input;
	bool m_sdlLooked;
	void *m_sdlLib;
	SdlGetFocus m_getKeyFocus;
	SdlSetMouseRect m_setMouseRect;
	SdlWarp m_warp;
	bool m_triedMouseRect;
	void *m_confined;
	SdlGetMouseState m_getState;
	SdlGetFocus m_getFocus;
	SdlGetWindowSize m_getSize;
	SdlGetRelative m_getRel;
	int m_mouseLogs;
	int m_lastMouse[4];
	bool m_startupDone;
	bool m_selfSet;   // the module resetting vr_display_3d itself (launch_3d_unmark)
	bool m_uiFull;      // GamepadUI: the UI set to the window size for this activation
	bool m_switchOff;   // vr_display_3d 0: 3D ends once the client has deactivated
};

CSourceVRTelevision g_television;

static void display_changed(IConVar *var, const char *oldValue, float oldFloat)
{
	g_television.queue_apply();
}

static void display_apply(const CCommand &args)
{
	g_television.apply_settings();
}
static ConCommand vr_display_apply("vr_display_apply", display_apply,
	"Applies vr_display_3d, vr_display_layout, vr_display_output and vr_display_swap together (the video options' Apply)");

} // namespace

// The Source interface factory, as tier1's EXPOSE_SINGLE_INTERFACE would
// export it, without linking tier1.
#ifdef _WIN32
extern "C" __declspec(dllexport)
#else
extern "C" __attribute__((visibility("default")))
#endif
void *CreateInterface(const char *name, int *returnCode)
{
	static bool configured = false;
	if (!configured) {
		load_config();
		configured = true;
	}
	if (name && !strcmp(name, SOURCE_VIRTUAL_REALITY_INTERFACE_VERSION)) {
		if (returnCode)
			*returnCode = IFACE_OK;
		logf("CreateInterface(%s): television\n", name);
		return &g_television;
	}
	if (returnCode)
		*returnCode = IFACE_FAILED;
	return NULL;
}


