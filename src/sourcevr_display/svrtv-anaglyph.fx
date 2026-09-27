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
//   9  rows, from full top and bottom          (1920x2160)
//  10  checkerboard, from full side by side    (3840x1080)
//  11  checkerboard, from full top and bottom  (1920x2160)
// gamescope runs the effect on the game's buffer and then scales it to the
// screen (with -S stretch), halving the doubled axis of a full format; 9-11
// set each pair of buffer pixels that becomes one screen pixel to the same eye,
// so the pattern survives the scaling (rows from full side by side are 6).
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
	return mix(tex2D(BackBuffer, float2(uv.x * 0.5, uv.y)).rgb,
	           tex2D(BackBuffer, float2(0.5 + uv.x * 0.5, uv.y)).rgb, lr, lg, lb, rr, rg, rb);
}
float3 mix_eyes_tab(float2 uv, float3 lr, float3 lg, float3 lb, float3 rr, float3 rg, float3 rb)
{
	return mix(tex2D(BackBuffer, float2(uv.x, uv.y * 0.5)).rgb,
	           tex2D(BackBuffer, float2(uv.x, 0.5 + uv.y * 0.5)).rgb, lr, lg, lb, rr, rg, rb);
}

float4 PS_CRT(float4 pos : SV_Position, float2 uv : TEXCOORD) : SV_Target
{
	return float4(mix_eyes(uv,
		float3( 0.456,  0.500,  0.176), float3(-0.040, -0.038, -0.016), float3(-0.015, -0.021, -0.005),
		float3(-0.043, -0.088, -0.002), float3( 0.378,  0.734, -0.018), float3(-0.072, -0.113,  1.226)), 1.0);
}

float4 PS_ModernScreens(float4 pos : SV_Position, float2 uv : TEXCOORD) : SV_Target
{
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
	return float4(tex2D(BackBuffer, uv).rgb, 1.0);
}

technique Identity
{
	pass { VertexShader = PostProcessVS; PixelShader = PS_Identity; SRGBWriteEnable = true; }
}

float4 PS_CRT_TaB(float4 pos : SV_Position, float2 uv : TEXCOORD) : SV_Target
{
	return float4(mix_eyes_tab(uv,
		float3( 0.456,  0.500,  0.176), float3(-0.040, -0.038, -0.016), float3(-0.015, -0.021, -0.005),
		float3(-0.043, -0.088, -0.002), float3( 0.378,  0.734, -0.018), float3(-0.072, -0.113,  1.226)), 1.0);
}

float4 PS_ModernScreens_TaB(float4 pos : SV_Position, float2 uv : TEXCOORD) : SV_Target
{
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
float3 eye_sbs(float2 uv, bool second) { return tex2D(BackBuffer, float2((second ? 0.5 : 0.0) + uv.x * 0.5, uv.y)).rgb; }
float3 eye_tab(float2 uv, bool second) { return tex2D(BackBuffer, float2(uv.x, (second ? 0.5 : 0.0) + uv.y * 0.5)).rgb; }
// pos is the pixel's centre (row + 0.5): odd rows and odd (x + y) sums give
// a fraction of 0.75 after halving, even ones 0.25.
bool odd_row(float4 pos) { return frac(floor(pos.y) * 0.5) > 0.25; }
bool odd_cell(float4 pos) { return frac((floor(pos.x) + floor(pos.y)) * 0.5) > 0.25; }

float4 PS_Rows_TaB(float4 pos : SV_Position, float2 uv : TEXCOORD) : SV_Target { return float4(eye_tab(uv, odd_row(pos)), 1.0); }
float4 PS_Rows_SBS(float4 pos : SV_Position, float2 uv : TEXCOORD) : SV_Target { return float4(eye_sbs(uv, odd_row(pos)), 1.0); }
float4 PS_Checkerboard_SBS(float4 pos : SV_Position, float2 uv : TEXCOORD) : SV_Target { return float4(eye_sbs(uv, odd_cell(pos)), 1.0); }
float4 PS_Checkerboard_TaB(float4 pos : SV_Position, float2 uv : TEXCOORD) : SV_Target { return float4(eye_tab(uv, odd_cell(pos)), 1.0); }

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

// Full formats: one screen pixel is two buffer pixels along the doubled axis.
bool odd_row_full_tab(float4 pos) { return frac(floor(pos.y * 0.5) * 0.5) > 0.25; }
bool odd_cell_full_sbs(float4 pos) { return frac((floor(pos.x * 0.5) + floor(pos.y)) * 0.5) > 0.25; }
bool odd_cell_full_tab(float4 pos) { return frac((floor(pos.x) + floor(pos.y * 0.5)) * 0.5) > 0.25; }

float4 PS_Rows_TaBFull(float4 pos : SV_Position, float2 uv : TEXCOORD) : SV_Target { return float4(eye_tab(uv, odd_row_full_tab(pos)), 1.0); }
float4 PS_Checkerboard_SBSFull(float4 pos : SV_Position, float2 uv : TEXCOORD) : SV_Target { return float4(eye_sbs(uv, odd_cell_full_sbs(pos)), 1.0); }
float4 PS_Checkerboard_TaBFull(float4 pos : SV_Position, float2 uv : TEXCOORD) : SV_Target { return float4(eye_tab(uv, odd_cell_full_tab(pos)), 1.0); }

technique Rows_TaBFull
{
	pass { VertexShader = PostProcessVS; PixelShader = PS_Rows_TaBFull; SRGBWriteEnable = true; }
}

technique Checkerboard_SBSFull
{
	pass { VertexShader = PostProcessVS; PixelShader = PS_Checkerboard_SBSFull; SRGBWriteEnable = true; }
}

technique Checkerboard_TaBFull
{
	pass { VertexShader = PostProcessVS; PixelShader = PS_Checkerboard_TaBFull; SRGBWriteEnable = true; }
}
