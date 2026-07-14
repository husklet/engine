// HL GPU buffer sharing ABI: the guest↔engine contract for allocating a host-backed,
// zero-copy GPU buffer. A guest opens /dev/dri/renderD128 and issues HL_IOCTL_GPU_ALLOC; the engine
// host-allocates an IOSurface (macOS), and — because guest-VA == host-VA in this in-process JIT — hands
// the IOSurface's base pointer straight back as the guest buffer (no mmap, no copy). The engine also
// returns the IOSurface's global id, which the guest carries to a presentation service in the linux-dmabuf
// modifier so that service can resolve the same IOSurface without a readback.
//
// This whole path is inert unless HL_GPU_IOSURFACE is enabled in the process-local launch options (the host
// sets it), so every existing workload — and the test gate — is byte-for-byte unaffected.
#ifndef HL_GPU_H
#define HL_GPU_H
#include <stdint.h>

// ioctl request number (retained private magic 0xDD; _IOWR-shaped, 32-byte arg). Guest sends this on the
// render-node fd. Chosen to not collide with real DRM ('d'=0x64) ioctls.
#define HL_IOCTL_GPU_ALLOC 0xC020DD01u

// Formats (match DRM fourccs' intent). 0 = BGRA8888 (little-endian ARGB), the wl_shm/Metal default.
#define HL_GPU_FMT_BGRA8888 0u

struct hl_gpu_alloc {
    uint32_t width;  // in
    uint32_t height; // in
    uint32_t format; // in  (HL_GPU_FMT_*)
    uint32_t stride; // out (bytes per row)
    uint32_t id;     // out (IOSurface global id carried to the compositor in the dmabuf modifier)
    int32_t fd;      // out (an anonymous fd to satisfy linux-dmabuf params.add; content unused)
    uint64_t ptr;    // out (buffer base — a host VA, directly usable by the guest since VA==VA)
};

// Private dmabuf modifier: modifier_lo = the IOSurface global id; modifier_hi low-16 = this magic
// tag; bit 16 of modifier_hi = "ask the host GPU to RENDER into this surface" (rung 3 first slice — a
// forwarded 1-op render command). The presentation service recognizes the tag, resolves the IOSurface, and (if the
// render bit is set) runs a Metal render pass into it before compositing.
#define HL_DMABUF_MOD_MAGIC 0x6464u /* retained wire value */
#define HL_DMABUF_RENDER_BIT 0x10000u

#endif
