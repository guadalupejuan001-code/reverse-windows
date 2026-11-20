#pragma once

inline constexpr char scene_vs[] =
    "struct V {float4 p : SV_Position; float4 c : COLOR0;};"
    "V main(float4 p : POSITION0, float4 c : COLOR0) {V v;v.p=p;v.c=c;return v;}";
inline constexpr char scene_ps[] =
    "struct V {float4 p : SV_Position; float4 c : COLOR0;};"
    "float4 main(V i) : SV_Target0 {return i.c;}";
