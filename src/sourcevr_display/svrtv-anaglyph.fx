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
// Techniques 0 and 1 are least-squares channel mixes (Eric Dubois's method, 2001) for red/
// cyan glasses No. 7003 from REEL3D: CRT as given by Sanders and McAllister
// (the one StereoPhoto Maker uses), modern screens as given by Zhang and
// McAllister; coefficients as collected at
// http://chrisjones.id.au/Dubois/Dubois.html (checked 2026-09-24).
// The mix is done in linear light (the sampler decodes sRGB, the pass encodes
// it again); most tools skip that step.
//
// Layout: left eye in the left half, right eye in the right half, as the
// module packs them (SVRTV_LAYOUT=sbs).

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
float3 mix_eyes(float2 uv, float3 lr, float3 lg, float3 lb, float3 rr, float3 rg, float3 rb)
{
	float3 l = tex2D(BackBuffer, float2(uv.x * 0.5, uv.y)).rgb;
	float3 r = tex2D(BackBuffer, float2(0.5 + uv.x * 0.5, uv.y)).rgb;
	return saturate(float3(dot(lr, l) + dot(rr, r),
	                       dot(lg, l) + dot(rg, r),
	                       dot(lb, l) + dot(rb, r)));
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
