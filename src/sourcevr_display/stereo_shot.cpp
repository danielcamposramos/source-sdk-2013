//========= Stereo screenshots for the stereoscopic display module =========//
//
// See stereo_shot.h. The MPO layout is the one tools/bravia_mpo.py writes in
// sony-bravia-linux, which two Sony BRAVIA sets switched into 3D on their own
// (2026-09-19): SOI, JFIF, Exif, then the MPF segment (index IFD, attribute
// IFD, MP entries) in the first image, and an attribute IFD in the second.
// Added here, after the entries, are the tags CIPA DC-007 (2026 edition,
// Table 8) recommends for Disparity images: Convergence Angle (SRATIONAL,
// degrees) and Baseline Length (RATIONAL, metres), both 0 at the base
// viewpoint.
//===========================================================================//

#include "stereo_shot.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STB_IMAGE_WRITE_STATIC
#define STBI_WRITE_NO_STDIO
#include "../thirdparty/stb/stb_image_write.h"

typedef std::vector<unsigned char> Bytes;

static const int JPEG_QUALITY = 95;   // over 90: stb keeps full-resolution colour

// CIPA DC-007 MP Type codes and flags.
static const unsigned MP_TYPE_DISPARITY = 0x020002;   // Multi-frame Image: Disparity
static const unsigned MP_REPRESENTATIVE = 0x20000000;

static void put16le(Bytes &b, unsigned v) { b.push_back(v & 255); b.push_back((v >> 8) & 255); }
static void put32le(Bytes &b, unsigned v) { put16le(b, v & 0xFFFF); put16le(b, v >> 16); }
static void put16be(Bytes &b, unsigned v) { b.push_back((v >> 8) & 255); b.push_back(v & 255); }
static void put(Bytes &b, const void *p, size_t n) { b.insert(b.end(), (const unsigned char *)p, (const unsigned char *)p + n); }

static void collect(void *context, void *data, int size)
{
	put(*(Bytes *)context, data, (size_t)size);
}

static bool save(const char *path, const Bytes &b)
{
	FILE *f = fopen(path, "wb");
	if (!f)
		return false;
	bool ok = fwrite(b.data(), 1, b.size(), f) == b.size();
	return fclose(f) == 0 && ok;
}

// One eye (BGRA) into an RGB image, at column x0 of a picture stride bytes wide.
static void to_rgb(const unsigned char *bgra, int w, int h, unsigned char *dst, int stride, int x0)
{
	for (int y = 0; y < h; y++) {
		const unsigned char *s = bgra + (size_t)y * w * 4;
		unsigned char *d = dst + (size_t)y * stride + (size_t)x0 * 3;
		for (int x = 0; x < w; x++, s += 4, d += 3) {
			d[0] = s[2];
			d[1] = s[1];
			d[2] = s[0];
		}
	}
}

static bool jpeg(Bytes &out, const unsigned char *rgb, int w, int h)
{
	return stbi_write_jpg_to_func(collect, &out, w, h, 3, rgb, JPEG_QUALITY) != 0;
}

static bool png(Bytes &out, const unsigned char *rgb, int w, int h)
{
	return stbi_write_png_to_func(collect, &out, w, h, 3, rgb, w * 3) != 0;
}

// Where a segment may go: after SOI and any leading APP0/APP1.
static size_t after_app01(const Bytes &j)
{
	size_t i = 2;
	while (i + 4 <= j.size() && j[i] == 0xFF && (j[i + 1] == 0xE0 || j[i + 1] == 0xE1))
		i += 2 + ((size_t)j[i + 2] << 8 | j[i + 3]);
	return i;
}

static void insert(Bytes &j, size_t at, const Bytes &seg)
{
	j.insert(j.begin() + at, seg.begin(), seg.end());
}

// A minimal, honest Exif block: what wrote the file and when, as real MPO
// files are Exif files (DC-007 builds on Exif). TIFF, little endian.
static Bytes exif_app1(const char *make, const char *model, time_t when)
{
	char dt[20];
	struct tm tmv;
#ifdef _WIN32
	localtime_s(&tmv, &when);
#else
	localtime_r(&when, &tmv);
#endif
	strftime(dt, sizeof(dt), "%Y:%m:%d %H:%M:%S", &tmv);
	size_t nMake = strlen(make) + 1, nModel = strlen(model) + 1, nDt = 20;
	size_t pad = (nMake & 1) + (nModel & 1);
	// IFD0: Make, Model, DateTime, ExifIFD; then the Exif IFD: ExifVersion,
	// DateTimeOriginal; then the values.
	unsigned ifd0 = 8, ifd0Size = 2 + 4 * 12 + 4;
	unsigned exifIfd = ifd0 + ifd0Size, exifSize = 2 + 2 * 12 + 4;
	unsigned data = exifIfd + exifSize;
	unsigned oMake = data, oModel = oMake + (unsigned)(nMake + (nMake & 1));
	unsigned oDt = oModel + (unsigned)(nModel + (nModel & 1)), oDt2 = oDt + (unsigned)nDt;
	Bytes t;
	put(t, "II", 2); put16le(t, 42); put32le(t, ifd0);
	put16le(t, 4);
	put16le(t, 0x010F); put16le(t, 2); put32le(t, (unsigned)nMake); put32le(t, oMake);
	put16le(t, 0x0110); put16le(t, 2); put32le(t, (unsigned)nModel); put32le(t, oModel);
	put16le(t, 0x0132); put16le(t, 2); put32le(t, (unsigned)nDt); put32le(t, oDt);
	put16le(t, 0x8769); put16le(t, 4); put32le(t, 1); put32le(t, exifIfd);
	put32le(t, 0);
	put16le(t, 2);
	put16le(t, 0x9000); put16le(t, 7); put32le(t, 4); put(t, "0230", 4);
	put16le(t, 0x9003); put16le(t, 2); put32le(t, (unsigned)nDt); put32le(t, oDt2);
	put32le(t, 0);
	put(t, make, nMake); if (nMake & 1) t.push_back(0);
	put(t, model, nModel); if (nModel & 1) t.push_back(0);
	put(t, dt, nDt);
	put(t, dt, nDt);
	(void)pad;
	Bytes seg;
	seg.push_back(0xFF); seg.push_back(0xE1);
	put16be(seg, (unsigned)(2 + 6 + t.size()));
	put(seg, "Exif\0\0", 6);
	put(seg, t.data(), t.size());
	return seg;
}

static Bytes app2(const Bytes &tiff)
{
	Bytes seg;
	seg.push_back(0xFF); seg.push_back(0xE2);
	put16be(seg, (unsigned)(2 + 4 + tiff.size()));
	put(seg, "MPF\0", 4);
	put(seg, tiff.data(), tiff.size());
	return seg;
}

static void rational(Bytes &b, double v, bool isSigned)
{
	// Four decimal places: 0.0635 m is 635/10000.
	long n = (long)floor(v * 10000.0 + 0.5);
	if (!isSigned && n < 0)
		n = 0;
	put32le(b, (unsigned)n);
	put32le(b, 10000);
}

// An MP Attribute IFD (MPFVersion, MPIndividualNum, BaseViewpointNum,
// ConvergenceAngle, BaselineLength) at offset 'at' of its TIFF; its two
// rationals go at 'values'.
static void attr_ifd(Bytes &t, unsigned individual, unsigned values)
{
	put16le(t, 5);
	put16le(t, 0xB000); put16le(t, 7); put32le(t, 4); put(t, "0100", 4);
	put16le(t, 0xB101); put16le(t, 4); put32le(t, 1); put32le(t, individual);
	put16le(t, 0xB204); put16le(t, 4); put32le(t, 1); put32le(t, 1);
	put16le(t, 0xB205); put16le(t, 10); put32le(t, 1); put32le(t, values);
	put16le(t, 0xB206); put16le(t, 5); put32le(t, 1); put32le(t, values + 8);
	put32le(t, 0);
}
static const unsigned ATTR_IFD_SIZE = 2 + 5 * 12 + 4;
static const unsigned INDEX_IFD_SIZE = 2 + 3 * 12 + 4;

// The first image's MPF: index IFD, attribute IFD, MP entries, rationals.
static Bytes mpf_first(const unsigned sizes[2], const unsigned offsets[2])
{
	unsigned attrAt = 8 + INDEX_IFD_SIZE;
	unsigned entriesAt = attrAt + ATTR_IFD_SIZE;
	unsigned valuesAt = entriesAt + 2 * 16;
	Bytes t;
	put(t, "II", 2); put16le(t, 42); put32le(t, 8);
	put16le(t, 3);
	put16le(t, 0xB000); put16le(t, 7); put32le(t, 4); put(t, "0100", 4);
	put16le(t, 0xB001); put16le(t, 4); put32le(t, 1); put32le(t, 2);
	put16le(t, 0xB002); put16le(t, 7); put32le(t, 2 * 16); put32le(t, entriesAt);
	put32le(t, attrAt);
	attr_ifd(t, 1, valuesAt);
	const unsigned attrs[2] = { MP_REPRESENTATIVE | MP_TYPE_DISPARITY, MP_TYPE_DISPARITY };
	for (int i = 0; i < 2; i++) {
		put32le(t, attrs[i]);
		put32le(t, sizes[i]);
		put32le(t, offsets[i]);
		put16le(t, 0);
		put16le(t, 0);
	}
	rational(t, 0.0, true);    // the base viewpoint: its own reference
	rational(t, 0.0, false);
	return app2(t);
}

static Bytes mpf_second(double convergence, double baseline)
{
	Bytes t;
	put(t, "II", 2); put16le(t, 42); put32le(t, 8);
	attr_ifd(t, 2, 8 + ATTR_IFD_SIZE);
	rational(t, convergence, true);
	rational(t, baseline, false);
	return app2(t);
}

static Bytes build_mpo(Bytes left, Bytes right, double convergence, double baseline)
{
	insert(right, after_app01(right), mpf_second(convergence, baseline));
	size_t ins = after_app01(left);
	unsigned zero[2] = { 0, 0 };
	size_t segLen = mpf_first(zero, zero).size();
	size_t tiffAt = ins + 4 + 4;   // the MP endian field: after FFE2, length and "MPF\0"
	unsigned sizes[2] = { (unsigned)(left.size() + segLen), (unsigned)right.size() };
	unsigned offsets[2] = { 0, (unsigned)(sizes[0] - tiffAt) };
	insert(left, ins, mpf_first(sizes, offsets));
	left.insert(left.end(), right.begin(), right.end());
	return left;
}

// sRGB <-> linear, as the effect samples (SRGBTexture) and writes
// (SRGBWriteEnable).
static float srgb_to_linear(float c) { return c <= 0.04045f ? c / 12.92f : powf((c + 0.055f) / 1.055f, 2.4f); }
static float linear_to_srgb(float c) { return c <= 0.0031308f ? c * 12.92f : 1.055f * powf(c, 1.0f / 2.4f) - 0.055f; }

// Eric Dubois's least-squares mixes, the effect's techniques 0 (CRT) and 1
// (modern screens): out = ML * left + MR * right, rows red, green, blue.
static const float DUBOIS[2][6][3] = {
	{ { 0.456f, 0.500f, 0.176f }, { -0.040f, -0.038f, -0.016f }, { -0.015f, -0.021f, -0.005f },
	  { -0.043f, -0.088f, -0.002f }, { 0.378f, 0.734f, -0.018f }, { -0.072f, -0.113f, 1.226f } },
	{ { 0.4154f, 0.4710f, 0.1669f }, { -0.0458f, -0.0484f, -0.0257f }, { -0.0547f, -0.0615f, 0.0128f },
	  { -0.0109f, -0.0364f, -0.0060f }, { 0.3756f, 0.7333f, 0.0111f }, { -0.0651f, -0.1287f, 1.2971f } },
};

static void anaglyph(const unsigned char *l, const unsigned char *r, int w, int h, int profile, unsigned char *out)
{
	static float lin[256];
	static unsigned char enc[4096];
	static bool ready = false;
	if (!ready) {
		for (int i = 0; i < 256; i++)
			lin[i] = srgb_to_linear(i / 255.0f);
		for (int i = 0; i < 4096; i++)
			enc[i] = (unsigned char)floorf(linear_to_srgb(i / 4095.0f) * 255.0f + 0.5f);
		ready = true;
	}
	const float (*m)[3] = DUBOIS[profile == STEREO_SHOT_ANAGLYPH_MODERN ? 1 : 0];
	size_t n = (size_t)w * h;
	for (size_t i = 0; i < n; i++, l += 3, r += 3, out += 3) {
		float L[3] = { lin[l[0]], lin[l[1]], lin[l[2]] };
		float R[3] = { lin[r[0]], lin[r[1]], lin[r[2]] };
		for (int c = 0; c < 3; c++) {
			float v = m[c][0] * L[0] + m[c][1] * L[1] + m[c][2] * L[2]
			        + m[3 + c][0] * R[0] + m[3 + c][1] * R[1] + m[3 + c][2] * R[2];
			v = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
			out[c] = enc[(int)(v * 4095.0f + 0.5f)];
		}
	}
}

static void failed(char *err, size_t n, const char *what, const char *path)
{
	if (err && n && !err[0])
		snprintf(err, n, "%s: %s", what, path);
}

bool stereo_shot_write(const StereoShot &s, StereoShotFiles *files, char *err, size_t errSize)
{
	if (err && errSize)
		err[0] = 0;
	memset(files, 0, sizeof(*files));
	int w = s.w, h = s.h;
	if (w <= 0 || h <= 0 || !s.eye[0] || !s.eye[1] || !s.base) {
		failed(err, errSize, "nothing to write", s.base ? s.base : "-");
		return false;
	}
	snprintf(files->stereo, sizeof(files->stereo), "%s_stereo.png", s.base);
	snprintf(files->preview, sizeof(files->preview), "%s_left.png", s.base);
	snprintf(files->jps, sizeof(files->jps), "%s.jps", s.base);
	snprintf(files->mpo, sizeof(files->mpo), "%s.mpo", s.base);
	if (s.anaglyph != STEREO_SHOT_NO_ANAGLYPH)
		snprintf(files->anaglyph, sizeof(files->anaglyph), "%s_anaglyph.jpg", s.base);

	bool ok = true;
	size_t eyeBytes = (size_t)w * h * 3;
	unsigned char *pair = (unsigned char *)malloc(eyeBytes * 2);   // side by side, 2w wide
	unsigned char *eye[2] = { (unsigned char *)malloc(eyeBytes), (unsigned char *)malloc(eyeBytes) };
	if (!pair || !eye[0] || !eye[1]) {
		free(pair); free(eye[0]); free(eye[1]);
		failed(err, errSize, "out of memory", s.base);
		return false;
	}
	for (int i = 0; i < 2; i++)
		to_rgb(s.eye[i], w, h, eye[i], w * 3, 0);

	// Valve's stereo type: left eye on the left.
	to_rgb(s.eye[0], w, h, pair, w * 6, 0);
	to_rgb(s.eye[1], w, h, pair, w * 6, w);
	Bytes b;
	if (!png(b, pair, 2 * w, h) || !save(files->stereo, b)) { ok = false; failed(err, errSize, "stereo PNG", files->stereo); }
	b.clear();
	if (!png(b, eye[0], w, h) || !save(files->preview, b)) { ok = false; failed(err, errSize, "preview PNG", files->preview); }

	// JPS: right eye on the left.
	to_rgb(s.eye[1], w, h, pair, w * 6, 0);
	to_rgb(s.eye[0], w, h, pair, w * 6, w);
	b.clear();
	if (!jpeg(b, pair, 2 * w, h) || !save(files->jps, b)) { ok = false; failed(err, errSize, "JPS", files->jps); }

	// MPO: each view its own JPEG with Exif, then the MPF segments.
	Bytes jl, jr;
	if (jpeg(jl, eye[0], w, h) && jpeg(jr, eye[1], w, h)) {
		Bytes ex = exif_app1(s.make ? s.make : "sourcevr", s.model ? s.model : "stereo screenshot", s.when);
		insert(jl, after_app01(jl), ex);
		insert(jr, after_app01(jr), ex);
		if (!save(files->mpo, build_mpo(jl, jr, s.convergenceDegrees, s.baselineMetres))) { ok = false; failed(err, errSize, "MPO", files->mpo); }
	} else { ok = false; failed(err, errSize, "MPO JPEG", files->mpo); }

	if (files->anaglyph[0]) {
		anaglyph(eye[0], eye[1], w, h, s.anaglyph, pair);
		b.clear();
		if (!jpeg(b, pair, w, h) || !save(files->anaglyph, b)) { ok = false; failed(err, errSize, "anaglyph", files->anaglyph); }
	}
	free(pair);
	free(eye[0]);
	free(eye[1]);
	return ok;
}
