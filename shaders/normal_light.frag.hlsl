// SPDX-FileCopyrightText: 2026 ShadowByte
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Pizza Tycoon HD: relief lighting of one HD picture from its normal map
// (src/engine/fx/NormalLight.cpp), as the custom fragment shader of an
// SDL_GPURenderState on SDL's "gpu" render driver.
//
// The picture keeps the brightness the light pass gives it; the normal map
// only shapes it. The factor applied to each texel is
//     light(N) / light(flat)
// where light(n) = ambient + sun * max(n.S, 0)
//                + sum of colour_i * falloff_i * max(n.L_i, 0)
// so a flat normal (128, 128, 255) leaves the texel exactly as it is, and
// LightPass then lays the light pools over the result as before.
//
// The CPU reference is fx::relief() in NormalLight.cpp; change both together.
//
// Resource layout (SDL_CreateGPUShader, fragment stage):
//   t0/s0, space2: the picture SDL draws (premultiplied)
//   t1/s1, space2: its normal map (render state), the picture's size
//   b0, space3:    NormalLightUniforms
// SPIR-V: set 2 bindings 0 and 1, set 3 binding 0.

[[vk::combinedImageSampler]][[vk::binding(0, 2)]]
Texture2D u_picture : register(t0, space2);
[[vk::combinedImageSampler]][[vk::binding(0, 2)]]
SamplerState u_picture_sampler : register(s0, space2);
[[vk::combinedImageSampler]][[vk::binding(1, 2)]]
Texture2D u_normal : register(t1, space2);
[[vk::combinedImageSampler]][[vk::binding(1, 2)]]
SamplerState u_normal_sampler : register(s1, space2);

// fx::NormalLightUniforms: 20 float4s.
[[vk::binding(0, 3)]]
cbuffer NormalLightUniforms : register(b0, space3)
{
    float4 u_ambient;     // rgb; unused
    float4 u_sun_dir;     // towards the sun: x right, y up, z out; unused
    float4 u_sun_colour;  // rgb (0 = no sun); unused
    float4 u_count;       // point lights in use, unused x3
    float4 u_light_pos[8];    // x, y (target texels, y down), height, radius
    float4 u_light_colour[8]; // rgb; unused
};

struct PSInput {
    float4 v_color : COLOR0;
    float2 v_uv : TEXCOORD0;
    float4 v_position : SV_Position;
};

struct PSOutput {
    float4 o_color : SV_Target;
};

float3 decode_normal(float4 t)
{
    // The map is premultiplied like every picture; transparent texels are flat.
    if (t.a < 0.5) {
        return float3(0.0, 0.0, 1.0);
    }
    const float3 c = t.rgb / t.a * 255.0;
    const float3 n = clamp((c - 128.0) / 127.0, -1.0, 1.0);
    const float len = length(n);
    return len > 1e-4 ? n / len : float3(0.0, 0.0, 1.0);
}

float3 light_at(float3 n, float2 p)
{
    float3 sum = u_ambient.rgb;
    sum += u_sun_colour.rgb * max(dot(n, u_sun_dir.xyz), 0.0);
    const int count = (int)u_count.x;
    for (int i = 0; i < 8; ++i) {
        if (i >= count) {
            break;
        }
        const float4 lp = u_light_pos[i];
        // Screen y grows down, the normal map's y up.
        const float2 d = float2(lp.x - p.x, p.y - lp.y);
        const float d2 = dot(d, d) / max(lp.w * lp.w, 1e-4);
        if (d2 >= 1.0) {
            continue;
        }
        const float falloff = pow(1.0 - d2, 1.5);
        const float3 l = normalize(float3(d, lp.z));
        sum += u_light_colour[i].rgb * (falloff * max(dot(n, l), 0.0));
    }
    return sum;
}

PSOutput main(PSInput input) {
    PSOutput output;
    const float4 texel = u_picture.Sample(u_picture_sampler, input.v_uv);
    const float3 n = decode_normal(u_normal.Sample(u_normal_sampler, input.v_uv));
    const float2 p = input.v_position.xy;
    const float3 lit = light_at(n, p);
    const float3 flat_lit = light_at(float3(0.0, 0.0, 1.0), p);
    // Where there is (almost) no light at all there is nothing to shape.
    const float3 factor = select(flat_lit > 1e-3,
                                 clamp(lit / max(flat_lit, 1e-3), 0.0, 2.0),
                                 float3(1.0, 1.0, 1.0));
    output.o_color = float4(texel.rgb * factor, texel.a) * input.v_color;
    return output;
}
