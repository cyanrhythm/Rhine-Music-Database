#version 440
#extension GL_GOOGLE_include_directive : enable
#include "opticalCommon.glsl"
layout(location = 0) in vec4 position;
layout(location = 1) in vec4 normal;
layout(location = 2) in vec4 uv;
layout(location = 3) in vec4 model0;
layout(location = 4) in vec4 model1;
layout(location = 5) in vec4 model2;
layout(location = 6) in vec4 model3;
layout(location = 7) in vec4 state;
layout(location = 0) out vec2 v_surfaceUv;
layout(location = 1) out vec2 v_uv;
layout(location = 2) out vec3 v_lightUv;
layout(location = 3) out float v_theme;
layout(location = 4) out float v_viewZ;
layout(location = 5) out float v_quality;
layout(location = 6) out float v_lightMask;
layout(location = 7) out vec2 v_normalUv;
void main() {
    mat4 model = mat4(model0, model1, model2, model3);
    vec4 world = model * position;
    vec4 eye = g.view * world;
    gl_Position = correction * g.projection * eye;
    float face = getFace(normal.xyz);
    vec2 surfaceUv = vec2((position.x + 2.5) / 5.0, position.y / 3.7);
    if (face == 0.0 || face == 3.0) {
        surfaceUv.x = (position.z + 0.25) / 0.5;
    } else if (face == 1.0 || face == 4.0) {
        surfaceUv.y = (position.z + 0.25) / 0.5;
    }
    // 每块图集留半个纹素边界，避免相邻朝向与昼夜行发生串色。
    surfaceUv = (saturate(surfaceUv) * 95.0 + 0.5) / 96.0;
    v_surfaceUv = vec2((face + surfaceUv.x) / 6.0, surfaceUv.y / 2.0);
    vec3 local = (emitterInverse * world).xyz;
    vec3 localNormal = normalize((emitterInverse * model * vec4(normal.xyz, 0)).xyz);
    float lightFace = getFace(localNormal);
    vec2 fieldUv = saturate(vec2((local.x + 9.0) / 18.0, (local.z + 7.0) / 14.0));
    fieldUv = (fieldUv * 63.0 + 0.5) / 64.0;
    v_lightUv = vec3((lightFace + fieldUv.x) / 6.0, fieldUv.y, saturate((local.y + 0.4) / 5.2) * 7.0);
    v_lightMask = step(abs(local.x), 9.0) * step(abs(local.z), 7.0);
    v_uv = uv.xy;
    v_theme = state.z;
    v_viewZ = -eye.z;
    v_quality = state.x;
    vec3 direction = normalize(normal.xyz);
    vec2 encoded = direction.xy / (abs(direction.x) + abs(direction.y) + abs(direction.z));
    if (direction.z < 0.0) {
        encoded = (1.0 - abs(encoded.yx)) * step(vec2(0.0), encoded) * 2.0 - (1.0 - abs(encoded.yx));
    }
    v_normalUv = ((encoded * 0.5 + 0.5) * 63.0 + 0.5) / 64.0;
    v_normalUv.y *= 0.5;
}
