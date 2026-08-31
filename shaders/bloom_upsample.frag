#version 460 core

// One level of the bloom pyramid's upsample chain.
//
// Render this at the *finer* level's resolution with alpha blending enabled
// (GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA). The shader writes the tent-filtered
// coarse level with alpha = uBlendWeight, so the blend unit computes
//
//     fine = mix(fine, blur(coarse), uBlendWeight)
//
// A *mix* rather than a straight add is what keeps the pyramid energy-bounded:
// summing six levels would otherwise multiply the total brightness by roughly
// six and turn every bright star into a sheet of haze.
//
// uLowTexelSize is 1 / dimensions(uLow); uSampleScale widens the tent for a
// softer, more cinematic falloff.

layout(location = 0) in vec2 vUv;
layout(location = 0) out vec4 oColor;

uniform sampler2D uLow;
uniform vec2 uLowTexelSize;
uniform float uSampleScale;
uniform float uBlendWeight;

vec3 tent9(sampler2D source, vec2 uv, vec2 texel)
{
    vec3 sum = vec3(0.0);
    sum +=       texture(source, uv + texel * vec2(-1.0, -1.0)).rgb;
    sum += 2.0 * texture(source, uv + texel * vec2( 0.0, -1.0)).rgb;
    sum +=       texture(source, uv + texel * vec2( 1.0, -1.0)).rgb;
    sum += 2.0 * texture(source, uv + texel * vec2(-1.0,  0.0)).rgb;
    sum += 4.0 * texture(source, uv).rgb;
    sum += 2.0 * texture(source, uv + texel * vec2( 1.0,  0.0)).rgb;
    sum +=       texture(source, uv + texel * vec2(-1.0,  1.0)).rgb;
    sum += 2.0 * texture(source, uv + texel * vec2( 0.0,  1.0)).rgb;
    sum +=       texture(source, uv + texel * vec2( 1.0,  1.0)).rgb;
    return sum * (1.0 / 16.0);
}

void main()
{
    oColor = vec4(tent9(uLow, vUv, uLowTexelSize * max(uSampleScale, 0.1)),
                  clamp(uBlendWeight, 0.0, 1.0));
}
