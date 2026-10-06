// Small helper kernels for the Metal renderer. Resource indices follow the
// convention tools/glsl2msl.py gives the translated shaders:
//   [[texture(0..7)]]  sampled/read textures
//   [[texture(8+n)]]   storage (write) textures
//   [[buffer(0..)]]    storage buffers
//   [[buffer(12)]]     per-dispatch constants
#include <metal_stdlib>
using namespace metal;

kernel void util_clear_u32(device uint* buf [[buffer(0)]],
                           constant uint& count [[buffer(12)]],
                           uint gid [[thread_position_in_grid]]) {
    if (gid < count) buf[gid] = 0u;
}

kernel void util_clear_tex(texture2d<float, access::write> dst [[texture(8)]],
                           uint2 gid [[thread_position_in_grid]]) {
    if (gid.x >= dst.get_width() || gid.y >= dst.get_height()) return;
    dst.write(float4(0.0), gid);
}

// Same-size copy between float-ish textures of any format (formats need not
// match, unlike a blit).
kernel void util_copy_tex(texture2d<float, access::read> src [[texture(0)]],
                          texture2d<float, access::write> dst [[texture(8)]],
                          uint2 gid [[thread_position_in_grid]]) {
    if (gid.x >= dst.get_width() || gid.y >= dst.get_height()) return;
    if (gid.x >= src.get_width() || gid.y >= src.get_height()) return;
    dst.write(src.read(gid), gid);
}

// Live input: Syphon frame (BGRA8, bottom-row-first) -> the pipeline's RGBA8
// source texture (top-row-first).
kernel void util_ingest(texture2d<float, access::read> src [[texture(0)]],
                        texture2d<float, access::write> dst [[texture(8)]],
                        constant uint& flipY [[buffer(12)]],
                        uint2 gid [[thread_position_in_grid]]) {
    if (gid.x >= dst.get_width() || gid.y >= dst.get_height()) return;
    uint2 sp = gid;
    if (flipY != 0u) sp.y = src.get_height() - 1u - gid.y;
    if (sp.x >= src.get_width() || sp.y >= src.get_height()) return;
    dst.write(src.read(sp), gid);
}

// Canvas (RGBA16F) -> BGRA8 for publishing and readback.
kernel void util_egress(texture2d<float, access::read> src [[texture(0)]],
                        texture2d<float, access::write> dst [[texture(8)]],
                        uint2 gid [[thread_position_in_grid]]) {
    if (gid.x >= dst.get_width() || gid.y >= dst.get_height()) return;
    float3 c = saturate(src.read(gid).rgb);
    dst.write(float4(c, 1.0), gid);
}
