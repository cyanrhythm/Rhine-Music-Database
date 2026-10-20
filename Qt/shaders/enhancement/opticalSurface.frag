#version 440
#extension GL_GOOGLE_include_directive : enable
#include "opticalCommon.glsl"
layout(location = 0) in vec2 v_surfaceUv;
layout(location = 1) in vec2 v_uv;
layout(location = 2) in vec3 v_lightUv;
layout(location = 3) in float v_theme;
layout(location = 4) in float v_viewZ;
layout(location = 5) in float v_quality;
layout(location = 6) in float v_lightMask;
layout(location = 7) in vec2 v_normalUv;
layout(std140, binding = 1) uniform SurfaceBlock {
    Surface m;
};
layout(binding = 2) uniform sampler2D surfaceAtlas;
layout(binding = 3) uniform sampler2D lightAtlas;
layout(binding = 4) uniform sampler2D label;
layout(binding = 5) uniform sampler2D cover;
layout(binding = 6) uniform sampler2D normalAtlas;
layout(location = 0) out vec4 fragColor;
/** 只插值烘焙光场的相邻高度切片；没有实时光源求值、光线追踪或折射背景采样。 */
vec3 bakedIllumination() {
    float lower = floor(v_lightUv.z);
    float upper = min(lower + 1.0, 7.0);
    vec3 first = texture(lightAtlas, vec2(v_lightUv.x, (v_lightUv.y + lower) / 8.0)).rgb;
    vec3 second = texture(lightAtlas, vec2(v_lightUv.x, (v_lightUv.y + upper) / 8.0)).rgb;
    return mix(first, second, fract(v_lightUv.z)) * v_lightMask * opticalLight.x * opticalLight.y;
}
void main() {
    if (m.flags.x == 1.0) {
        vec4 ink = texture(label, vec2(v_uv.x, 1.0 - v_uv.y));
        vec3 rgb = linearColor(ink.rgb);
        float amount = 1.0 - smoothstep(.12, .65, dot(rgb, vec3(.2126, .7152, .0722)));
        rgb = mix(rgb, mix(vec3(.023, .032, .037), vec3(.78, .78, .71), amount), v_theme);
        fragColor = vec4(rgb, ink.a * v_quality);
        return;
    }
    if (m.flags.x == 2.0) {
        float coverage = fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(.06711056, .00583715))));
        if (v_quality <= coverage) {
            discard;
        }
    }
    vec4 baked = texture(surfaceAtlas, v_surfaceUv);
    vec4 normalResponse = texture(normalAtlas, v_normalUv);
    if (v_theme > 0.0) {
        vec4 night = texture(surfaceAtlas, v_surfaceUv + vec2(0.0, 0.5));
        baked = mix(baked, night, v_theme);
        normalResponse = mix(normalResponse, texture(normalAtlas, v_normalUv + vec2(0.0, 0.5)), v_theme);
    }
    bool shell = m.flags.x == 3.0 || m.flags.x == 4.0 || m.flags.x == 5.0;
    vec3 color = baked.rgb * normalResponse.rgb + vec3(normalResponse.a);
    if (shell) {
        // 选中壳体同样接收内部烘焙光，再叠加自身柔光，保持光源亮于受照邻壳。
        color += bakedIllumination();
        color += emissionColor.rgb * baked.a * m.emission.x * opticalLight.x * opticalLight.y;
    } else {
        color = mix(m.color.rgb, m.darkColor.rgb, v_theme) * 0.8 + baked.rgb * 0.08;
    }
    if (m.flags.x == 6.0) {
        vec2 discUv = vec2(v_uv.x, 1.0 - v_uv.y);
        vec2 centered = discUv - vec2(.5);
        discUv = mat2(m.extra.z, -m.extra.w, m.extra.w, m.extra.z) * centered + vec2(.5);
        color = linearColor(texture(cover, discUv).rgb) * 0.8;
    }
    color = mix(color, g.fogColor.xyz, smoothstep(g.fog.x, g.fog.y, v_viewZ));
    fragColor = vec4(color, m.color.a);
}
