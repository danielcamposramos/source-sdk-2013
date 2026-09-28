// VR Stereo Spectator: side-by-side stereo to red/cyan anaglyph, for any
// colour screen and a pair of red/cyan glasses. A ReShade effect for
// gamescope (--reshade-effect svrtv-anaglyph.fx), 64-bit and outside the game,
// so it works for any game, 32-bit or 64-bit, that outputs side by side.
//
// Techniques (--reshade-technique-idx):
//   0  CRT             the matrix computed for a CRT's phosphors
//   1  modern screens  the matrix computed for an LCD panel
//   2  Identity        diagnostic: passes the side-by-side frame through
//                      unchanged, so the ReShade path runs with no colour
//                      filtering (SBS through gamescope on a 3D TV)
//   3  CRT, top and bottom             as 0, the eyes in the top and bottom halves
//   4  modern screens, top and bottom  as 1, the eyes in the top and bottom halves
//   5  rows, from top and bottom       row-interleaved for passive (film pattern
//                                      retarder) screens: even lines the first
//                                      eye, odd lines the second
//   6  rows, from side by side         the same from the left and right halves
//   7  checkerboard, from side by side for DLP screens: even pixels (x + y) the
//                                      first eye, odd pixels the second
//   8  checkerboard, from top and bottom
//   9  the 3D display, top and bottom: the frame as it is, and a frame the
//      module did not build (the engine's own loading screen) whole in each
//      half, so both eyes see all of it, spinner and progress bar included
//  10  the 3D display, side by side: the same
// Every frame the module builds inside gamescope carries its mark: the two
// bottom-right pixels, green then magenta; every technique paints them over.
// A frame without the mark is flat 2D, drawn by the engine while no view
// renders (run q28); 0-8 show it as it is (both eyes the same, flat), 9 and
// 10 put it whole into both halves (Daniel, 2026-09-28).
// The rows and checkerboard pass each eye's colour through unchanged. They are
// written to the formats' definitions and not yet seen on such a screen.
// Techniques 0 and 1 are least-squares channel mixes (Eric Dubois's method, 2001) for red/
// cyan glasses No. 7003 from REEL3D: CRT as given by Sanders and McAllister
// (the one StereoPhoto Maker uses), modern screens as given by Zhang and
// McAllister; coefficients as collected at
// http://chrisjones.id.au/Dubois/Dubois.html (checked 2026-09-24).
// The mix is done in linear light (the sampler decodes sRGB, the pass encodes
// it again); most tools skip that step.
//
// Layout: left eye in the left half, right eye in the right half (0, 1), or
// left eye on top, right eye below (3, 4), as the module packs them.

// gamescope (3.16) always allocates a uniform buffer of the effect's uniform
// size; with no uniforms that is a zero-size allocation, which NVIDIA refuses
// ("vkAllocateMemory failed", the effect never runs) and RADV crashed on
// (2026-09-24). One uniform avoids it.
uniform float svrtv_unused = 0.0;

texture BackBufferTex : COLOR;
sampler BackBuffer { Texture = BackBufferTex; SRGBTexture = true; };
sampler BackBufferRaw { Texture = BackBufferTex; };

// The module's mark (see above), read as stored.
bool marked()
{
	float3 g = tex2Dfetch(BackBufferRaw, int2(BUFFER_WIDTH - 2, BUFFER_HEIGHT - 1)).rgb;
	float3 m = tex2Dfetch(BackBufferRaw, int2(BUFFER_WIDTH - 1, BUFFER_HEIGHT - 1)).rgb;
	return g.g > 0.9 && g.r < 0.1 && g.b < 0.1 && m.r > 0.9 && m.b > 0.9 && m.g < 0.1;
}
// The frame's colour at uv, the mark painted over: its two pixels take the
// colour of the pixel beside them.
float3 back(float2 uv)
{
	if (uv.y > 1.0 - BUFFER_RCP_HEIGHT && uv.x > 1.0 - 2.0 * BUFFER_RCP_WIDTH)
		uv.x = 1.0 - 2.5 * BUFFER_RCP_WIDTH;
	return tex2D(BackBuffer, uv).rgb;
}

void PostProcessVS(in uint id : SV_VertexID, out float4 position : SV_Position, out float2 texcoord : TEXCOORD)
{
	texcoord.x = (id == 2) ? 2.0 : 0.0;
	texcoord.y = (id == 1) ? 2.0 : 0.0;
	position = float4(texcoord * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
}

// out = ML * left + MR * right, per output channel (rows: red, green, blue).
float3 mix(float3 l, float3 r, float3 lr, float3 lg, float3 lb, float3 rr, float3 rg, float3 rb)
{
	return saturate(float3(dot(lr, l) + dot(rr, r),
	                       dot(lg, l) + dot(rg, r),
	                       dot(lb, l) + dot(rb, r)));
}
float3 mix_eyes(float2 uv, float3 lr, float3 lg, float3 lb, float3 rr, float3 rg, float3 rb)
{
	return mix(back(float2(uv.x * 0.5, uv.y)),
	           back(float2(0.5 + uv.x * 0.5, uv.y)), lr, lg, lb, rr, rg, rb);
}
float3 mix_eyes_tab(float2 uv, float3 lr, float3 lg, float3 lb, float3 rr, float3 rg, float3 rb)
{
	return mix(back(float2(uv.x, uv.y * 0.5)),
	           back(float2(uv.x, 0.5 + uv.y * 0.5)), lr, lg, lb, rr, rg, rb);
}

float4 PS_CRT(float4 pos : SV_Position, float2 uv : TEXCOORD) : SV_Target
{
	if (!marked())
		return float4(back(uv), 1.0);
	return float4(mix_eyes(uv,
		float3( 0.456,  0.500,  0.176), float3(-0.040, -0.038, -0.016), float3(-0.015, -0.021, -0.005),
		float3(-0.043, -0.088, -0.002), float3( 0.378,  0.734, -0.018), float3(-0.072, -0.113,  1.226)), 1.0);
}

float4 PS_ModernScreens(float4 pos : SV_Position, float2 uv : TEXCOORD) : SV_Target
{
	if (!marked())
		return float4(back(uv), 1.0);
	return float4(mix_eyes(uv,
		float3( 0.4154,  0.4710,  0.1669), float3(-0.0458, -0.0484, -0.0257), float3(-0.0547, -0.0615,  0.0128),
		float3(-0.0109, -0.0364, -0.0060), float3( 0.3756,  0.7333,  0.0111), float3(-0.0651, -0.1287,  1.2971)), 1.0);
}

technique CRT
{
	pass { VertexShader = PostProcessVS; PixelShader = PS_CRT; SRGBWriteEnable = true; }
}

technique ModernScreens
{
	pass { VertexShader = PostProcessVS; PixelShader = PS_ModernScreens; SRGBWriteEnable = true; }
}

// Diagnostic: the whole frame, unchanged (matched sRGB decode and encode).
float4 PS_Identity(float4 pos : SV_Position, float2 uv : TEXCOORD) : SV_Target
{
	return float4(back(uv), 1.0);
}

technique Identity
{
	pass { VertexShader = PostProcessVS; PixelShader = PS_Identity; SRGBWriteEnable = true; }
}

float4 PS_CRT_TaB(float4 pos : SV_Position, float2 uv : TEXCOORD) : SV_Target
{
	if (!marked())
		return float4(back(uv), 1.0);
	return float4(mix_eyes_tab(uv,
		float3( 0.456,  0.500,  0.176), float3(-0.040, -0.038, -0.016), float3(-0.015, -0.021, -0.005),
		float3(-0.043, -0.088, -0.002), float3( 0.378,  0.734, -0.018), float3(-0.072, -0.113,  1.226)), 1.0);
}

float4 PS_ModernScreens_TaB(float4 pos : SV_Position, float2 uv : TEXCOORD) : SV_Target
{
	if (!marked())
		return float4(back(uv), 1.0);
	return float4(mix_eyes_tab(uv,
		float3( 0.4154,  0.4710,  0.1669), float3(-0.0458, -0.0484, -0.0257), float3(-0.0547, -0.0615,  0.0128),
		float3(-0.0109, -0.0364, -0.0060), float3( 0.3756,  0.7333,  0.0111), float3(-0.0651, -0.1287,  1.2971)), 1.0);
}

technique CRT_TaB
{
	pass { VertexShader = PostProcessVS; PixelShader = PS_CRT_TaB; SRGBWriteEnable = true; }
}

technique ModernScreens_TaB
{
	pass { VertexShader = PostProcessVS; PixelShader = PS_ModernScreens_TaB; SRGBWriteEnable = true; }
}

// The first eye's and the second eye's colour at this point, from side by
// side or from top and bottom.
float3 eye_sbs(float2 uv, bool second) { return back(float2((second ? 0.5 : 0.0) + uv.x * 0.5, uv.y)); }
float3 eye_tab(float2 uv, bool second) { return back(float2(uv.x, (second ? 0.5 : 0.0) + uv.y * 0.5)); }
// pos is the pixel's centre (row + 0.5): odd rows and odd (x + y) sums give
// a fraction of 0.75 after halving, even ones 0.25.
bool odd_row(float4 pos) { return frac(floor(pos.y) * 0.5) > 0.25; }
bool odd_cell(float4 pos) { return frac((floor(pos.x) + floor(pos.y)) * 0.5) > 0.25; }

float4 PS_Rows_TaB(float4 pos : SV_Position, float2 uv : TEXCOORD) : SV_Target { return float4(marked() ? eye_tab(uv, odd_row(pos)) : back(uv), 1.0); }
float4 PS_Rows_SBS(float4 pos : SV_Position, float2 uv : TEXCOORD) : SV_Target { return float4(marked() ? eye_sbs(uv, odd_row(pos)) : back(uv), 1.0); }
float4 PS_Checkerboard_SBS(float4 pos : SV_Position, float2 uv : TEXCOORD) : SV_Target { return float4(marked() ? eye_sbs(uv, odd_cell(pos)) : back(uv), 1.0); }
float4 PS_Checkerboard_TaB(float4 pos : SV_Position, float2 uv : TEXCOORD) : SV_Target { return float4(marked() ? eye_tab(uv, odd_cell(pos)) : back(uv), 1.0); }

technique Rows_TaB
{
	pass { VertexShader = PostProcessVS; PixelShader = PS_Rows_TaB; SRGBWriteEnable = true; }
}

technique Rows_SBS
{
	pass { VertexShader = PostProcessVS; PixelShader = PS_Rows_SBS; SRGBWriteEnable = true; }
}

technique Checkerboard_SBS
{
	pass { VertexShader = PostProcessVS; PixelShader = PS_Checkerboard_SBS; SRGBWriteEnable = true; }
}

technique Checkerboard_TaB
{
	pass { VertexShader = PostProcessVS; PixelShader = PS_Checkerboard_TaB; SRGBWriteEnable = true; }
}

// The 3D display (9, 10): the module's frames as they are; any other frame
// whole in each half, squashed as the display will stretch it back.
float4 PS_Display_TaB(float4 pos : SV_Position, float2 uv : TEXCOORD) : SV_Target
{
	return float4(back(marked() ? uv : float2(uv.x, frac(uv.y * 2.0))), 1.0);
}
float4 PS_Display_SBS(float4 pos : SV_Position, float2 uv : TEXCOORD) : SV_Target
{
	return float4(back(marked() ? uv : float2(frac(uv.x * 2.0), uv.y)), 1.0);
}

technique Display_TaB
{
	pass { VertexShader = PostProcessVS; PixelShader = PS_Display_TaB; SRGBWriteEnable = true; }
}

technique Display_SBS
{
	pass { VertexShader = PostProcessVS; PixelShader = PS_Display_SBS; SRGBWriteEnable = true; }
}
