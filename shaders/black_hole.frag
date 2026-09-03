#version 460 core

// Fragment entry point for the HDR ray-tracing pass.
//
// The pass itself lives in black_hole_common.glsl, which this and
// black_hole.comp both include: there is exactly one copy of the tracer, and
// the two entry points differ only in where a pixel's coordinate comes from and
// where the colour goes.

layout(location = 0) out vec4 oHdrColor;
layout(location = 0) in vec2 vUv;

#include "black_hole_common.glsl"

void main()
{
    oHdrColor = renderPixel(vUv);
}
