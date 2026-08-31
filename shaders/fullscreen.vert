#version 460 core

// A single oversized triangle covers the viewport without a diagonal seam.
// No vertex buffer or vertex attributes are required: draw with
// glDrawArrays(GL_TRIANGLES, 0, 3).

layout(location = 0) out vec2 vUv;

void main()
{
    const vec2 kPositions[3] = vec2[3](
        vec2(-1.0, -1.0),
        vec2( 3.0, -1.0),
        vec2(-1.0,  3.0)
    );

    const vec2 kUvs[3] = vec2[3](
        vec2(0.0, 0.0),
        vec2(2.0, 0.0),
        vec2(0.0, 2.0)
    );

    gl_Position = vec4(kPositions[gl_VertexID], 0.0, 1.0);
    vUv = kUvs[gl_VertexID];
}
