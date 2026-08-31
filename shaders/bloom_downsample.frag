#version 460 core

// One level of the bloom pyramid's downsample chain.
//
// uTexelSize is 1 / dimensions(uSource). The first level also applies the
// bright-pass threshold; deeper levels must not, or the glow would be eroded
// again at every step.

layout(location = 0) in vec2 vUv;
layout(location = 0) out vec4 oColor;

uniform sampler2D uSource;
uniform vec2 uTexelSize;
uniform float uThreshold;
uniform float uKnee;
uniform int uApplyThreshold;

// Soft-knee bright pass: fully keeps pixels above uThreshold, fades smoothly
// over a band of width uKnee below it, so the bloom has no hard onset.
vec3 prefilter(vec3 colour)
{
    if (uApplyThreshold == 0)
    {
        return colour;
    }

    float brightness = max(max(colour.r, colour.g), colour.b);
    float knee = max(uKnee, 1e-5);
    float soft = clamp(brightness - uThreshold + knee, 0.0, 2.0 * knee);
    soft = soft * soft / max(4.0 * knee, 1e-5);
    float contribution = max(brightness - uThreshold, soft) / max(brightness, 1e-5);
    return colour * contribution;
}

// Karis average: weight each group by 1/(1+luma) before averaging. This is the
// standard fix for "fireflies" -- a single very bright sub-pixel star would
// otherwise pump a whole bloom kernel and flicker as the camera moves.
float karisWeight(vec3 colour)
{
    return 1.0 / (1.0 + dot(colour, vec3(0.2126, 0.7152, 0.0722)));
}

void main()
{
    vec2 t = uTexelSize;
    vec3 a = texture(uSource, vUv + vec2(-t.x,  t.y)).rgb;
    vec3 b = texture(uSource, vUv + vec2( 0.0,  t.y)).rgb;
    vec3 c = texture(uSource, vUv + vec2( t.x,  t.y)).rgb;
    vec3 d = texture(uSource, vUv + vec2(-t.x,  0.0)).rgb;
    vec3 e = texture(uSource, vUv).rgb;
    vec3 f = texture(uSource, vUv + vec2( t.x,  0.0)).rgb;
    vec3 g = texture(uSource, vUv + vec2(-t.x, -t.y)).rgb;
    vec3 h = texture(uSource, vUv + vec2( 0.0, -t.y)).rgb;
    vec3 i = texture(uSource, vUv + vec2( t.x, -t.y)).rgb;

    if (uApplyThreshold != 0)
    {
        // Three overlapping groups, each Karis-averaged, then blended by the
        // same 3x3 tent weights used below.
        vec3 centre = e;
        vec3 plus = (b + d + f + h) * 0.25;
        vec3 diagonal = (a + c + g + i) * 0.25;
        float wc = karisWeight(centre);
        float wp = karisWeight(plus);
        float wx = karisWeight(diagonal);
        float total = wc * 0.25 + wp * 0.5 + wx * 0.25;
        vec3 filtered = (centre * wc * 0.25 + plus * wp * 0.5 + diagonal * wx * 0.25) / max(total, 1e-6);
        oColor = vec4(prefilter(filtered), 1.0);
        return;
    }

    vec3 downsampled = e * 0.25 + (b + d + f + h) * 0.125 + (a + c + g + i) * 0.0625;
    oColor = vec4(downsampled, 1.0);
}
