#version 460 core

// Progressive refinement.
//
// The ray-tracing pass jitters its sample position inside the pixel every
// frame. While the camera and all parameters hold still, this shader keeps a
// running arithmetic mean of those frames:
//
//     mean_n = mean_(n-1) + (sample_n - mean_(n-1)) / n
//
// After a few dozen frames the image is effectively super-sampled: the shadow
// edge, the photon ring and the sub-pixel stars all resolve cleanly, with no
// extra cost while moving. Any camera or parameter change resets the counter,
// so nothing from a stale viewpoint can ghost into the new one.
//
// uHistory is RGBA32F: a 16-bit mean would visibly quantise after a hundred
// or so samples.

layout(location = 0) in vec2 vUv;
layout(location = 0) out vec4 oColor;

uniform sampler2D uHistory;
uniform sampler2D uCurrent;
uniform int uSampleIndex;   // 0 for the first frame after a reset.

void main()
{
    vec3 current = texture(uCurrent, vUv).rgb;
    if (uSampleIndex <= 0)
    {
        oColor = vec4(current, 1.0);
        return;
    }

    vec3 history = texture(uHistory, vUv).rgb;
    float weight = 1.0 / float(uSampleIndex + 1);
    oColor = vec4(mix(history, current, weight), 1.0);
}
