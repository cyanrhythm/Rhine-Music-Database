#version 440
#extension GL_GOOGLE_include_directive : enable
#include "opticalCommon.glsl"
layout(binding = 2) uniform sampler2D color;
layout(binding = 3) uniform sampler2D originalActor;
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 fragColor;
vec4 effect() {
    float theme = g.viewport.z;
    // 在已有合成中完成轻量边缘柔化，不增加渲染目标或单独的全屏模糊通道。
    float distanceFromFocus = smoothstep(0.05, 0.48, abs(v_uv.y - 0.52));
    float soften = (1.0 - g.clip.z) * (1.0 - g.viewport.w);
    vec2 texel = (1.0 + 6.0 * distanceFromFocus * soften) / vec2(textureSize(color, 0));
    vec3 hdr = texture(color, rtUV(v_uv)).rgb * 4.0;
    vec3 neighbors = texture(color, rtUV(v_uv + vec2(texel.x, 0.0))).rgb;
    neighbors += texture(color, rtUV(v_uv - vec2(texel.x, 0.0))).rgb;
    neighbors += texture(color, rtUV(v_uv + vec2(0.0, texel.y))).rgb;
    neighbors += texture(color, rtUV(v_uv - vec2(0.0, texel.y))).rgb;
    hdr += neighbors * 2.0;
    hdr += texture(color, rtUV(v_uv + texel)).rgb;
    hdr += texture(color, rtUV(v_uv - texel)).rgb;
    hdr += texture(color, rtUV(v_uv + vec2(texel.x, -texel.y))).rgb;
    hdr += texture(color, rtUV(v_uv + vec2(-texel.x, texel.y))).rgb;
    hdr /= 16.0;
    vec3 rgb = outputColor(aces(hdr, mix(0.76, 0.98, theme)));
    float shade = g.clip.z;
    if (g.viewport.w < 0.5 && g.clip.w < 0.5) {
        vec3 paper = mix(vec3(222, 219, 215) / 255, vec3(17, 24, 27) / 255, theme);
        float top = mix(.72 * (1 - saturate(v_uv.y / .50)), .82 * (1 - saturate(v_uv.y / .36)), theme);
        rgb = mix(rgb, mix(vec3(224, 221, 216) / 255, paper, theme), top * (1 - shade));
        float bottom = mix(.72, .84, theme) * (1 - saturate((1 - v_uv.y) / .35));
        rgb = mix(rgb, paper, bottom * (1 - shade));
        float radius = length((v_uv - vec2(.42, .47)) / vec2(.58, .53)) / sqrt(2.0);
        float darkRadius = length((v_uv - vec2(.48, .48)) / vec2(.58, .53)) / sqrt(2.0);
        float edge = mix(.19 * saturate((radius - .32) / (.85 - .32)),
                         .48 * saturate((darkRadius - .42) / (.90 - .42)), theme);
        rgb = mix(rgb, paper, edge * (1 - shade));
        float side = v_uv.x < .59   ? mix(0.0, .2, saturate((v_uv.x - .4) / .19))
                     : v_uv.x < .68 ? mix(.2, .92, (v_uv.x - .59) / .09)
                                    : mix(.92, 1.0, (v_uv.x - .68) / .32);
        rgb = mix(rgb, paper, side * shade);
    }
    if (opticalLight.w > 0.0) {
        // 只在取出唱片的投影范围内读取原版最终像素，不再给封面和外壳叠加烘焙色。
        float inside = 1.0;
        for (int index = 0; index < 8; ++index) {
            inside *= step(0.0, dot(actorEdges[index].xyz, vec3(v_uv, 1.0)));
        }
        rgb = mix(rgb, texture(originalActor, rtUV(v_uv)).rgb, inside * opticalLight.w);
    }
    return vec4(rgb, 1);
}
void main() {
    fragColor = effect();
}
