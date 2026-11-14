// SPDX-FileCopyrightText: 2026 ShadowByte
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Pizza Tycoon HD: the final picture pass (src/engine/fx/PostFx.cpp).
//
// Runs as the custom fragment shader of an SDL_GPURenderState while the
// composited frame is drawn to the window, on SDL's "gpu" render driver.
// The maths is mirrored, step for step, by the CPU reference in
// src/engine/fx/Grade.cpp (fx::grade_texel), which the tests check; change
// both together.
//
// Resource layout (SDL_CreateGPUShader, fragment stage):
//   t0/s0, space2: the frame SDL draws (bound by SDL itself)
//   t1/s1, space2: the bloom source, the frame at 1/8 size (render state)
//   b0, space3:    PostFxUniforms (SDL_SetGPURenderStateFragmentUniforms)
// SPIR-V: set 2 bindings 0 and 1 (combined image samplers), set 3 binding 0.
//
// Bloom: the frame is reduced to 1/8 size by three 2x2 box halvings on the
// fixed-function path (linear filtering), and here a 9-tap tent filter over
// that copy widens it further; the soft threshold is applied to the blurred
// light (an approximation of threshold-then-blur that needs no extra pass).

[[vk::combinedImageSampler]][[vk::binding(0, 2)]]
Texture2D u_frame : register(t0, space2);
[[vk::combinedImageSampler]][[vk::binding(0, 2)]]
SamplerState u_frame_sampler : register(s0, space2);
[[vk::combinedImageSampler]][[vk::binding(1, 2)]]
Texture2D u_bloom : register(t1, space2);
[[vk::combinedImageSampler]][[vk::binding(1, 2)]]
SamplerState u_bloom_sampler : register(s1, space2);

// Every member is a float4 so HLSL, SPIR-V and MSL agree on the layout
// (fx::PostFxUniforms on the C++ side, 8 x 16 bytes).
[[vk::binding(0, 3)]]
cbuffer PostFxUniforms : register(b0, space3)
{
    float4 u_tone;      // exposure, contrast, saturation, unused
    float4 u_balance;   // white balance multipliers r, g, b; unused
    float4 u_lift;      // rgb; unused
    float4 u_gamma;     // rgb (> 0); unused
    float4 u_gain;      // rgb; unused
    float4 u_bloom_p;   // strength, threshold, texel w, texel h (of bloom)
    float4 u_frame_p;   // vignette, crt, scanlines (source rows), mask cols
    float4 u_output;    // split u, linear io (0/1), SDR white, HDR headroom
};

struct PSInput {
    float4 v_color : COLOR0;
    float2 v_uv : TEXCOORD0;
};

struct PSOutput {
    float4 o_color : SV_Target;
};

static const float3 luma709 = float3(0.2126, 0.7152, 0.0722);
static const float pi = 3.14159265;

float3 to_srgb(float3 c)
{
    c = max(c, 0.0);
    return select(c <= 0.0031308, c * 12.92,
                  1.055 * pow(c, 1.0 / 2.4) - 0.055);
}

float3 to_linear(float3 c)
{
    c = max(c, 0.0);
    return select(c <= 0.04045, c / 12.92, pow((c + 0.055) / 1.055, 2.4));
}

float3 bloom_light(float2 uv)
{
    const float2 t = u_bloom_p.zw;
    float3 sum = u_bloom.Sample(u_bloom_sampler, uv).rgb * 4.0;
    sum += u_bloom.Sample(u_bloom_sampler, uv + float2(t.x, 0)).rgb * 2.0;
    sum += u_bloom.Sample(u_bloom_sampler, uv - float2(t.x, 0)).rgb * 2.0;
    sum += u_bloom.Sample(u_bloom_sampler, uv + float2(0, t.y)).rgb * 2.0;
    sum += u_bloom.Sample(u_bloom_sampler, uv - float2(0, t.y)).rgb * 2.0;
    sum += u_bloom.Sample(u_bloom_sampler, uv + t).rgb;
    sum += u_bloom.Sample(u_bloom_sampler, uv - t).rgb;
    sum += u_bloom.Sample(u_bloom_sampler, uv + float2(t.x, -t.y)).rgb;
    sum += u_bloom.Sample(u_bloom_sampler, uv + float2(-t.x, t.y)).rgb;
    return sum / 16.0;
}

PSOutput main(PSInput input) {
    PSOutput output;
    const float4 texel = u_frame.Sample(u_frame_sampler, input.v_uv);
    const bool linear_io = u_output.y > 0.5;

    // The 1994 side of the F8 comparison stays as it is.
    if (input.v_uv.x < u_output.x) {
        const float white = linear_io ? u_output.z : 1.0;
        output.o_color = float4(texel.rgb * white, texel.a) * input.v_color;
        return output;
    }

    float3 c = linear_io ? to_srgb(texel.rgb) : texel.rgb;

    // Bloom: light above the threshold spills around bright things.
    if (u_bloom_p.x > 0.0) {
        float3 b = bloom_light(input.v_uv);
        if (linear_io) {
            b = to_srgb(b);
        }
        const float threshold = u_bloom_p.y;
        const float level = max(max(b.r, b.g), b.b);
        const float over = saturate((level - threshold) / max(1.0 - threshold, 1e-4));
        c += b * (over * over * u_bloom_p.x);
    }

    // Exposure and white balance.
    c *= u_tone.x * u_balance.rgb;
    // Contrast around mid grey.
    c = (c - 0.5) * u_tone.y + 0.5;
    // Saturation around the luma.
    const float l = dot(c, luma709);
    c = l + (c - l) * u_tone.z;
    // Lift / gamma / gain.
    c = u_gain.rgb * (c + u_lift.rgb * (1.0 - c));
    c = pow(max(c, 0.0), 1.0 / u_gamma.rgb);

    // Vignette: darker towards the corners.
    if (u_frame_p.x > 0.0) {
        const float2 d = input.v_uv - 0.5;
        const float r = sqrt(dot(d, d)) * 1.41421356;
        c *= 1.0 - u_frame_p.x * smoothstep(0.45, 1.0, r);
    }

    // CRT: a dark gap between the original rows and a soft aperture mask.
    if (u_frame_p.y > 0.0) {
        const float row = frac(input.v_uv.y * u_frame_p.z);
        const float scan = 0.5 + 0.5 * cos(2.0 * pi * (row - 0.5));
        c *= lerp(1.0, 0.72 + 0.28 * scan, u_frame_p.y);
        const float col = frac(input.v_uv.x * u_frame_p.w / 3.0) * 3.0;
        const float3 mask = col < 1.0 ? float3(1.0, 0.82, 0.82)
                          : col < 2.0 ? float3(0.82, 1.0, 0.82)
                                      : float3(0.82, 0.82, 1.0);
        c *= lerp(float3(1.0, 1.0, 1.0), mask * 1.12, u_frame_p.y * 0.5);
    }

    if (linear_io) {
        // HDR: SDR white sits at the display's paper white, and highlights
        // (bloom, gain) may use the headroom above it.
        c = to_linear(c) * u_output.z;
        c = min(c, u_output.z * u_output.w);
    } else {
        c = saturate(c);
    }
    output.o_color = float4(c, texel.a) * input.v_color;
    return output;
}
