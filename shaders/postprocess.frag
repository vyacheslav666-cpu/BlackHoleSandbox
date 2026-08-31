#version 460 core

// Final HDR composition and display transform.  Input textures remain in
// linear HDR radiance space.  This shader adds bloom before tone mapping,
// then encodes the result for a non-sRGB default framebuffer.

layout(location = 0) in vec2 vUv;
layout(location = 0) out vec4 oColor;

uniform sampler2D uScene;
uniform sampler2D uBloom;
uniform float uExposure;
uniform float uBloomStrength;
// 0 = ACES fitted (default), 1 = Reinhard, 2 = Uncharted 2 filmic.
uniform int uToneMapper;
uniform vec2 uResolution;

vec3 acesFitted(vec3 colour)
{
    // Narkowicz's compact fit of the ACES RRT+ODT appearance transform.
    const float a = 2.51;
    const float b = 0.03;
    const float c = 2.43;
    const float d = 0.59;
    const float e = 0.14;
    return clamp((colour * (a * colour + b)) / (colour * (c * colour + d) + e), 0.0, 1.0);
}

vec3 uncharted2Tonemap(vec3 x)
{
    const float A = 0.15;
    const float B = 0.50;
    const float C = 0.10;
    const float D = 0.20;
    const float E = 0.02;
    const float F = 0.30;
    return ((x * (A * x + C * B) + D * E) / (x * (A * x + B) + D * F)) - E / F;
}

float interleavedGradientNoise(vec2 pixel)
{
    return fract(52.9829189 * fract(dot(pixel, vec2(0.06711056, 0.00583715))));
}

void main()
{
    vec3 hdr = texture(uScene, vUv).rgb;
    float exposure = uExposure > 0.0 ? uExposure : 1.0;
    float bloomStrength = max(uBloomStrength, 0.0);
    hdr = (hdr + texture(uBloom, vUv).rgb * bloomStrength) * exposure;

    vec3 mapped;
    if (uToneMapper == 1)
    {
        mapped = hdr / (vec3(1.0) + hdr);
    }
    else if (uToneMapper == 2)
    {
        vec3 whiteScale = vec3(1.0) / uncharted2Tonemap(vec3(11.2));
        mapped = clamp(uncharted2Tonemap(hdr) * whiteScale, 0.0, 1.0);
    }
    else
    {
        mapped = acesFitted(hdr);
    }

    // Very small blue-noise-like dither prevents smooth star/bloom gradients
    // from banding in an 8-bit default framebuffer.
    vec2 pixel = vUv * max(uResolution, vec2(1.0));
    mapped += (interleavedGradientNoise(pixel) - 0.5) / 255.0;
    mapped = clamp(mapped, 0.0, 1.0);
    oColor = vec4(pow(mapped, vec3(1.0 / 2.2)), 1.0);
}
