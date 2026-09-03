// =============================================================================
// Wavefront ray state
// =============================================================================
//
// The inline tracers keep a ray's whole state in registers and never spill it.
// The wavefront path cannot: it stops every ray after a fixed number of steps,
// compacts the survivors into a dense list and comes back to them, so the state
// has to live in memory between chunks. That round trip is the entire cost of
// the scheme, and it is paid by every live ray on every chunk -- which is why
// this struct is laid out in vec4s and kept as small as the tracer allows.
//
// 96 bytes. Six of its floats exist only for the debug views (minRadius,
// opticalDepth and the four shift fields); dropping them would take it to 64 and
// cut the traffic by a third, at the cost of debug modes 4 to 7.

struct RayState
{
    vec4 positionTransmittance;   // xyz: Kerr-Schild position, spin frame; w: transmittance
    vec4 momentumMinRadius;       // xyz: momentum, spin frame;             w: closest approach
    vec4 radianceOpticalDepth;    // xyz: disk radiance so far;             w: integrated extinction
    vec4 shifts;                  // peakWeight, shiftTotal, shiftDoppler, shiftGravity
    vec4 escapeDirection;         // xyz: sky direction if it escaped;      w: unused
    uvec4 meta;                   // pixel index, steps taken, trace state, unused
};

layout(std430, binding = 1) restrict buffer RayStateBuffer { RayState uRays[]; };
layout(std430, binding = 2) restrict readonly buffer LiveListIn { uint uListIn[]; };
layout(std430, binding = 3) restrict writeonly buffer LiveListOut { uint uListOut[]; };
layout(std430, binding = 4) restrict buffer ControlBuffer { uint uControl[]; };

// Control block. Deliberately a flat uint array: a struct mixing scalars with a
// uvec3 would need its std430 padding reasoned about, and the indirect dispatch
// arguments have to land at a byte offset the CPU can name.
const uint kControlLiveIn   = 0u;   // entries in uListIn
const uint kControlLiveOut  = 1u;   // atomic append target for uListOut
const uint kControlIndirect = 4u;   // uvec3 of workgroup counts, at byte offset 16
const uint kControlHistory  = 8u;   // ring of recent live counts, read back by the CPU

// Workgroup width of the chunk pass. The list is dense, so there is no 2D
// locality left to exploit and nothing a larger group would buy.
const uint kWavefrontLocalSize = 64u;

uint rayIndexFor(uint pixelIndex, int sampleInFrame, int spp)
{
    // Pixel-major, so the shade pass walks one pixel's samples contiguously and
    // sums them in a fixed order. That order has to be reproducible whatever
    // order the compaction happened to leave the rays in.
    return pixelIndex * uint(spp) + uint(sampleInFrame);
}

void unpackRay(RayState st, out KerrRay ray, out TraceResult trace)
{
    ray.position = st.positionTransmittance.xyz;
    ray.momentum = st.momentumMinRadius.xyz;

    trace.state = int(st.meta.z);
    trace.steps = int(st.meta.y);
    trace.minRadius = st.momentumMinRadius.w;
    trace.escapeDirection = st.escapeDirection.xyz;
    trace.diskRadiance = st.radianceOpticalDepth.xyz;
    trace.transmittance = st.positionTransmittance.w;
    trace.opticalDepth = st.radianceOpticalDepth.w;
    trace.peakWeight = st.shifts.x;
    trace.shiftTotal = st.shifts.y;
    trace.shiftDoppler = st.shifts.z;
    trace.shiftGravity = st.shifts.w;
}

RayState packRay(KerrRay ray, TraceResult trace, uint pixelIndex)
{
    RayState st;
    st.positionTransmittance = vec4(ray.position, trace.transmittance);
    st.momentumMinRadius = vec4(ray.momentum, trace.minRadius);
    st.radianceOpticalDepth = vec4(trace.diskRadiance, trace.opticalDepth);
    st.shifts = vec4(trace.peakWeight, trace.shiftTotal, trace.shiftDoppler, trace.shiftGravity);
    st.escapeDirection = vec4(trace.escapeDirection, 0.0);
    st.meta = uvec4(pixelIndex, uint(trace.steps), uint(trace.state), 0u);
    return st;
}
