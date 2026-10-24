// Exact display read-out. Reproduces the image that the PCSX2 v2.8.2 oracle hashes at each vsync
// (Oracle::OnGSVSync -> CanonicalImageMD5 over GSDevice::GetCurrent()), for the oracle's
// determinism profile: software renderer (Renderer = 13) at native resolution, deinterlace_mode
// = Off, and every other GS option at its default.
//
// PCSX2 functions mirrored (pcsx2/GS/ unless noted):
//   GS.cpp                       GSvsync(): the CRTC update that runs before every VSync.
//   GSState.cpp                  GSState::GetVideoMode, isReallyInterlaced, and GSPCRTCRegs:
//                                SetVideoMode, EnableDisplays, SetRects, CheckSameSource,
//                                CalculateDisplayOffset, CalculateFramebufferOffset,
//                                NearestToZeroOffset, FrameWrap, FrameRectMatch, GetResolution,
//                                GetFramebufferRect, GetFramebufferSize, RemoveFramebufferOffset
//                                (the software-renderer branches), and the VideoMode* tables.
//   GSState::GIFRegHandlerSCANMSK  m_scanmask_used.
//   Renderers/Common/GSRenderer.cpp  GSRenderer::Merge: which circuits are read, the source and
//                                destination rectangles, the identical-outputs shortcut, the
//                                background colour (BGCOLOR | ALP << 24), blank frames.
//   Renderers/SW/GSRendererSW.cpp  GetOutput: block-aligned read of local memory into 32-bit.
//   GSLocalMemoryMultiISA.cpp / GSBlock.h  ReadTexture24 and ReadTexture16 (Expand24To32,
//                                Expand16To32 with TEXA AEM = 0, TA0 = 0x80 for CT24 else 0,
//                                TA1 = 0x80; the 5-bit channels are shifted, not replicated).
//   Renderers/Common/GSDevice.cpp  GSDevice::Merge; Renderers/OpenGL/GSDeviceOGL.cpp  DoMerge
//                                and DoStretchRect; bin/resources/shaders/opengl/merge.glsl.
//   Oracle.cpp                   OnGSVSync: blank when Merge fails; width x height of the merged
//                                texture; the canonical image is R, G, B, alpha dropped.
//
// The oracle merges through OpenGL on Mesa llvmpipe, so the 8-bit arithmetic follows what
// llvmpipe does (measured over every input, Mesa 26.2.5, LLVM 22.1.8, 256-bit vectors):
//   - copy of circuit 2: the texel unchanged;
//   - circuit 1 blended over it with GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_FUNC_ADD:
//       out = min(255, rnd(src * a) + rnd(dst * (255 - a))),  rnd(t) = (t + 127) / 255,
//     with a = ALP when MMOD = 1, else min(255, 2 * texel alpha) (merge.glsl ps_main0 doubles
//     the alpha, the fixed-function unit clamps it);
//   - nearest sampling with clamp-to-edge: texel floor(s0 + (px + 0.5 - d0) * (sSize / dSize)).
// Different Mesa versions or CPU code paths could in principle round differently (ADR 0005).
//
// Fixed oracle options baked in (GSConfig): PCRTCOffsets = false, PCRTCOverscan = false,
// PCRTCAntiBlur = true, InterlaceMode = Off, upscale multiplier 1. With InterlaceMode Off PCSX2
// never calls GSDevice::Interlace, adds no FFMD interlace offset and does not look at the
// field, ShadeBoost, FXAA and bilinear presentation are off, and the "automatic" deinterlace
// state (game_deinterlacing, scanmask_frame) is not used.
//
// Cases NOT reproduced exactly. The read-out never refuses a frame (the goal is a port that
// behaves like the game, not one that matches PCSX2 pixel for pixel). Each case below gets the
// stated approximation and logs "[gs-readout] approximated ..." once per case (per readout):
//   - EXTWRITE.WRITE = 1 (feedback merge, GetFeedbackOutput, the YUV passes): ignored; the
//     normal merge of the two read circuits is shown;
//   - an unknown video mode (SMODE1.CMOD = 1): the previous video mode is kept (NTSC before
//     the first valid one);
//   - a framebuffer rectangle reaching 2048 (GetFramebufferSize): clamped to 2047;
//   - a read framebuffer that wraps at 2048, or is over 1024 pixels wide or tall (the software
//     renderer's read buffer is 1024 x 1024): clamped, the rest is not read; the source
//     rectangle then samples with clamp-to-edge, so the last column or row is repeated;
//   - a negative framebuffer origin: clamped to 0;
//   - a DISPFB pixel format other than CT32, CT24, CT16 and CT16S (paletted, unknown): no
//     texture for that circuit, as PCSX2 does for an unusable framebuffer, so the circuit shows
//     nothing (black, or the other circuit);
//   - a framebuffer height under 1: no texture for that circuit;
//   - source and destination rectangles whose nearest sampling is ambiguous (a destination
//     pixel centre within 0.001 texel of a texel edge, as in a 4:1 downscale), because llvmpipe
//     breaks such ties by float rounding: the plain formula floor(u) is used;
//   - a destination rectangle with negative extent: not drawn;
//   - a merge target under 1 pixel: a blank frame; over 16384: clamped to 16384;
//   - a GS with no privileged registers or backend (GS::readDisplayExact): a blank frame.
// GS dumps and hardware-renderer-only paths are out of scope.

#include "runtime/gs/gs_display_readout.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <string>

namespace
{
    // GSVideoMode, minus 1 (GSPCRTCRegs::videomode), is the index into the tables below.
    constexpr int kUnknown = 0, kNtsc = 1, kPal = 2, kVesa = 3, kSdtv480p = 4, kHdtv720p = 5, kHdtv1080i = 6;

    struct V4
    {
        int x, y, z, w;
    };
    // VideoModeOffsets: width, height, x offset, y offset.
    constexpr V4 kOffsets[6] = {
        {640, 224, 642, 25}, {640, 256, 676, 36}, {640, 480, 276, 34}, {720, 480, 232, 35}, {1280, 720, 302, 24}, {1920, 540, 238, 40}};
    // VideoModeDividers.
    constexpr V4 kDividers[6] = {
        {3, 0, 2559, 239}, {3, 0, 2559, 287}, {1, 0, 1279, 479}, {1, 0, 1439, 479}, {0, 0, 1279, 719}, {0, 0, 1919, 1079}};

    constexpr int kPsmCT32 = 0, kPsmCT24 = 1, kPsmCT16 = 2, kPsmCT16S = 10;

    uint64_t bits(uint64_t v, int lo, int count)
    {
        return (v >> lo) & ((1ull << count) - 1ull);
    }

    // GSUtil::HasCompatibleBits for the formats a display can use.
    bool compatibleBits(int a, int b)
    {
        if (a == b)
            return true;
        return (a == kPsmCT32 && b == kPsmCT24) || (a == kPsmCT24 && b == kPsmCT32) ||
               (a == kPsmCT16 && b == kPsmCT16S) || (a == kPsmCT16S && b == kPsmCT16);
    }

    int rnd255(int t)
    {
        return (t + 127) / 255;
    }
}

GSDisplayReadout::GSDisplayReadout()
{
    reset();
}

void GSDisplayReadout::reset()
{
    m_videomode = 0;
    m_interlaced = 0;
    m_ffmd = 0;
    m_pcrtcSameSrc = false;
    m_togglingField = false;
    m_circuits[0] = Circuit{};
    m_circuits[1] = Circuit{};
    m_scanmaskUsed = 0;
}

void GSDisplayReadout::noteScanmaskWrite(uint64_t scanmskValue)
{
    if (scanmskValue & 2u)
        m_scanmaskUsed = 2;
}

bool GSDisplayReadout::read(const GSDisplayRegs &regs, const ReadPixelFn &readPixel, bool field, DisplayImage &out)
{
    (void)field; // InterlaceMode Off: Merge does not use the field.
    out = DisplayImage{};
    readImpl(regs, readPixel, out);
    return true;
}

void GSDisplayReadout::approximated(const std::string &key, const std::string &what) const
{
    if (m_approximated.insert(key).second)
        std::cerr << "[gs-readout] approximated " << what << std::endl;
}

bool GSDisplayReadout::isAnalogue() const
{
    const int video = m_videomode + 1;
    return video == kNtsc || video == kPal || video == kHdtv1080i;
}

GSDisplayReadout::Point GSDisplayReadout::nearestToZeroOffset() const
{
    Point r{1, 1};
    if (!m_circuits[0].enabled && !m_circuits[1].enabled)
        return r;
    for (int i = 0; i < 2; i++)
    {
        if (!m_circuits[i].enabled)
        {
            r.x = 1 - i;
            r.y = 1 - i;
            return r;
        }
    }
    if (std::abs(m_circuits[0].displayOffset.x - kOffsets[m_videomode].z) <
        std::abs(m_circuits[1].displayOffset.x - kOffsets[m_videomode].z))
        r.x = 0;
    const int verticalOffset = kOffsets[m_videomode].w * (1 << m_interlaced);
    if (std::abs(m_circuits[0].displayOffset.y - verticalOffset) <
        std::abs(m_circuits[1].displayOffset.y - verticalOffset))
        r.y = 0;
    return r;
}

void GSDisplayReadout::enableDisplays(const GSDisplayRegs &regs, bool smodetoggle)
{
    m_circuits[0].enabled = bits(regs.pmode, 0, 1) != 0;
    m_circuits[1].enabled = bits(regs.pmode, 1, 1) != 0;
    m_interlaced = (bits(regs.smode2, 0, 1) != 0 && isAnalogue()) ? 1 : 0;
    m_ffmd = static_cast<int>(bits(regs.smode2, 1, 1));
    m_togglingField = smodetoggle && isAnalogue();
}

void GSDisplayReadout::setRects(int display, uint64_t displayReg, uint64_t framebufferReg)
{
    Circuit &c = m_circuits[display];
    c.prevFramebufferReg = {c.fbp, c.fbw, c.psm, c.dbx, c.dby};
    c.fbp = static_cast<int>(bits(framebufferReg, 0, 9));
    c.fbw = static_cast<int>(bits(framebufferReg, 9, 6));
    c.psm = static_cast<int>(bits(framebufferReg, 15, 5));
    c.dbx = static_cast<int>(bits(framebufferReg, 32, 11));
    c.dby = static_cast<int>(bits(framebufferReg, 43, 11));

    const uint32_t dx = static_cast<uint32_t>(bits(displayReg, 0, 12));
    const uint32_t dy = static_cast<uint32_t>(bits(displayReg, 12, 11));
    const uint32_t magh = static_cast<uint32_t>(bits(displayReg, 23, 4));
    const uint32_t magv = static_cast<uint32_t>(bits(displayReg, 27, 2));
    const uint32_t dwReg = static_cast<uint32_t>(bits(displayReg, 32, 12));
    const uint32_t dhReg = static_cast<uint32_t>(bits(displayReg, 44, 11));

    // Q-Ball Billiards: both circuits enabled but one not set up.
    if (c.fbw == 0 && dwReg == 0 && dhReg == 0 && magh == 0)
    {
        c.enabled = false;
        return;
    }
    c.magnification = {static_cast<int>(magh) + 1, static_cast<int>(magv) + 1};
    const uint32_t DW = dwReg + 1;
    const uint32_t DH = dhReg + 1;

    const int renderWidth = static_cast<int>(DW / static_cast<uint32_t>(c.magnification.x));
    const int renderHeight = static_cast<int>(DH / static_cast<uint32_t>(c.magnification.y));

    // PCRTCOffsets = false.
    const uint32_t finalW = std::min(static_cast<uint32_t>(renderWidth), DW / static_cast<uint32_t>(kDividers[m_videomode].x + 1));
    const uint32_t finalH = std::min(static_cast<uint32_t>(renderHeight), DH / static_cast<uint32_t>(kDividers[m_videomode].y + 1));

    c.prevFramebufferOffsets = c.framebufferOffsets;
    c.framebufferRect.x = 0;
    c.framebufferRect.y = 0;
    c.framebufferRect.z = renderWidth;
    if (m_ffmd && m_interlaced)
        c.framebufferRect.w = (renderHeight + 1) >> (m_ffmd * m_interlaced);
    else
        c.framebufferRect.w = renderHeight;
    c.framebufferOffsets = {c.dbx, c.dby};

    // is_interlaced_resolution && !interlaced needs InterlaceMode != Off: never here.
    c.displayRect = {0, 0, static_cast<int>(finalW), static_cast<int>(finalH)};
    c.prevDisplayOffset = c.displayOffset;
    c.displayOffset = {static_cast<int>(dx), static_cast<int>(dy)};
}

void GSDisplayReadout::checkSameSource()
{
    if (m_circuits[0].enabled != m_circuits[1].enabled || (!m_circuits[0].enabled && !m_circuits[1].enabled))
    {
        m_pcrtcSameSrc = false;
        return;
    }
    m_pcrtcSameSrc = m_circuits[0].fbp == m_circuits[1].fbp && m_circuits[0].fbw == m_circuits[1].fbw &&
                     compatibleBits(m_circuits[0].psm, m_circuits[1].psm);
}

namespace
{
    struct R4
    {
        int x, y, z, w;
    };
    R4 runion(const R4 &a, const R4 &b)
    {
        return {std::min(a.x, b.x), std::min(a.y, b.y), std::max(a.z, b.z), std::max(a.w, b.w)};
    }
    bool rempty(const R4 &r)
    {
        return !(r.x < r.z && r.y < r.w);
    }
    // GSVector4i::rcontains: this contains v.
    bool rcontains(const R4 &a, const R4 &v)
    {
        return a.x <= v.x && a.y <= v.y && a.z >= v.z && a.w >= v.w;
    }
    bool requal(const R4 &a, const R4 &b)
    {
        return a.x == b.x && a.y == b.y && a.z == b.z && a.w == b.w;
    }
}

bool GSDisplayReadout::frameWrap() const
{
    const R4 a{m_circuits[0].framebufferRect.x, m_circuits[0].framebufferRect.y, m_circuits[0].framebufferRect.z, m_circuits[0].framebufferRect.w};
    const R4 b{m_circuits[1].framebufferRect.x, m_circuits[1].framebufferRect.y, m_circuits[1].framebufferRect.z, m_circuits[1].framebufferRect.w};
    const R4 u = runion(a, b);
    return u.w >= 2048 || u.z >= 2048;
}

GSDisplayReadout::Point GSDisplayReadout::resolution() const
{
    const V4 offsets = kOffsets[m_videomode];
    Point res;
    const Circuit &c0 = m_circuits[0];
    const Circuit &c1 = m_circuits[1];
    if (c0.enabled && c1.enabled)
    {
        const R4 u = runion({c0.displayRect.x, c0.displayRect.y, c0.displayRect.z, c0.displayRect.w},
                            {c1.displayRect.x, c1.displayRect.y, c1.displayRect.z, c1.displayRect.w});
        res = {u.z - u.x, u.w - u.y};
    }
    else if (c0.enabled)
    {
        res = {c0.displayRect.z - c0.displayRect.x, c0.displayRect.w - c0.displayRect.y};
    }
    else
    {
        res = {c1.displayRect.z - c1.displayRect.x, c1.displayRect.w - c1.displayRect.y};
    }
    // is_full_height: InterlaceMode == Off.
    res.x = std::min(res.x, offsets.x);
    res.y = std::min(res.y, offsets.y << 1);
    return res;
}

GSDisplayReadout::Rect GSDisplayReadout::framebufferRect(int display) const
{
    if (display == -1)
    {
        const Rect &a = m_circuits[0].framebufferRect;
        const Rect &b = m_circuits[1].framebufferRect;
        const R4 u = runion({a.x, a.y, a.z, a.w}, {b.x, b.y, b.z, b.w});
        return {u.x, u.y, u.z, u.w};
    }
    return m_circuits[display].framebufferRect;
}

GSDisplayReadout::Point GSDisplayReadout::framebufferSize(int display) const
{
    int maxHeight = kOffsets[m_videomode].y;
    if (!(m_ffmd && m_interlaced))
        maxHeight *= 2;

    if (display == -1)
    {
        const Rect &a = m_circuits[0].framebufferRect;
        const Rect &b = m_circuits[1].framebufferRect;
        R4 combined = runion({a.x, a.y, a.z, a.w}, {b.x, b.y, b.z, b.w});
        if (combined.z >= 2048 || combined.w >= 2048)
        {
            approximated("fb-size-2048", "a framebuffer rectangle reaching 2048 (GetFramebufferSize): clamped to 2047");
            combined.z = std::min(combined.z, 2047);
            combined.w = std::min(combined.w, 2047);
        }

        const int minMag = std::max(1, std::min(m_circuits[0].magnification.y, m_circuits[1].magnification.y));
        const Rect &da = m_circuits[0].displayRect;
        const Rect &db = m_circuits[1].displayRect;
        int offset = runion({da.x, da.y, da.z, da.w}, {db.x, db.y, db.z, db.w}).y;
        if (m_ffmd && m_interlaced)
            offset = (offset - 1) / 2;
        // The hardware renderer widens the read here; the software renderer does not.
        maxHeight += combined.y;
        offset = (maxHeight / minMag) - offset;
        combined.w = std::min(combined.w, offset);
        return {combined.z, combined.w};
    }

    Rect out = m_circuits[display].framebufferRect;
    if (out.z >= 2048 || out.w >= 2048)
    {
        approximated("fb-size-2048", "a framebuffer rectangle reaching 2048 (GetFramebufferSize): clamped to 2047");
        out.z = std::min(out.z, 2047);
        out.w = std::min(out.w, 2047);
    }
    const int minMag = std::max(1, m_circuits[display].magnification.y);
    int offset = m_circuits[display].displayRect.y;
    if (m_ffmd && m_interlaced)
        offset = (offset - 1) / 2;
    maxHeight += out.y;
    offset = (maxHeight / minMag) - offset;
    out.w = std::min(out.w, offset);
    return {out.z, out.w};
}

void GSDisplayReadout::calculateDisplayOffset(bool scanmask)
{
    // PCRTCOffsets = false, PCRTCAntiBlur = true.
    const bool bothEnabled = m_circuits[0].enabled && m_circuits[1].enabled;
    const V4 offsets = kOffsets[m_videomode];
    int intOff[2] = {0, 0};
    const Point zeroDisplay = nearestToZeroOffset();
    Point baseOffset = m_circuits[zeroDisplay.y].displayOffset;

    if (bothEnabled)
    {
        const int blurOffset = std::abs(m_circuits[1].displayOffset.y - m_circuits[0].displayOffset.y);
        if (!scanmask && blurOffset < 4)
        {
            if (m_circuits[1].displayOffset.y > m_circuits[0].displayOffset.y)
                m_circuits[1].displayOffset.y -= blurOffset;
            else
                m_circuits[0].displayOffset.y -= blurOffset;
        }
    }

    for (int i = 0; i < 2; i++)
    {
        if (!m_circuits[i].enabled)
            continue;
        const int offset = (m_circuits[i].displayOffset.y - (offsets.w * (m_interlaced + 1))) / (kDividers[m_videomode].y + 1);
        if (offset > 4)
            continue;
        intOff[i] = offset & 1;
        if (offset < 0)
            intOff[i] = -intOff[i];
        m_circuits[i].displayRect.y += intOff[i];
        m_circuits[i].displayRect.w += intOff[i];
    }

    if (bothEnabled)
    {
        const Point offset = {
            (m_circuits[1 - zeroDisplay.x].displayOffset.x - m_circuits[zeroDisplay.x].displayOffset.x) / (kDividers[m_videomode].x + 1),
            (m_circuits[1 - zeroDisplay.y].displayOffset.y - m_circuits[zeroDisplay.y].displayOffset.y) / (kDividers[m_videomode].y + 1)};
        if (offset.x >= 4 || scanmask)
        {
            m_circuits[1 - zeroDisplay.x].displayRect.x += offset.x;
            m_circuits[1 - zeroDisplay.x].displayRect.z += offset.x;
        }
        if (offset.y >= 4 || scanmask)
        {
            m_circuits[1 - zeroDisplay.y].displayRect.y += offset.y - intOff[1 - zeroDisplay.y];
            m_circuits[1 - zeroDisplay.y].displayRect.w += offset.y - intOff[1 - zeroDisplay.y];
        }
        baseOffset = m_circuits[zeroDisplay.y].displayOffset;
    }

    const int vOffset = (baseOffset.y - (offsets.w * (m_interlaced + 1))) / (kDividers[m_videomode].y + 1);
    if (vOffset <= 4 && vOffset != 0)
    {
        m_circuits[0].displayRect.y += vOffset - intOff[0];
        m_circuits[0].displayRect.w += vOffset - intOff[0];
        m_circuits[1].displayRect.y += vOffset - intOff[1];
        m_circuits[1].displayRect.w += vOffset - intOff[1];
    }
}

void GSDisplayReadout::calculateFramebufferOffset(bool scanmask, uint64_t dispfb0, uint64_t dispfb1)
{
    Point fb0 = m_circuits[0].framebufferOffsets;
    Point fb1 = m_circuits[1].framebufferOffsets;

    if (fb0.x + m_circuits[0].displayRect.z > 2048)
    {
        fb0.x -= 2048;
        m_circuits[0].framebufferOffsets.x = fb0.x;
    }
    if (fb0.y + m_circuits[0].displayRect.w > 2048)
    {
        fb0.y -= 2048;
        m_circuits[0].framebufferOffsets.y = fb0.y;
    }
    if (fb1.x + m_circuits[1].displayRect.z > 2048)
    {
        fb1.x -= 2048;
        m_circuits[1].framebufferOffsets.x = fb1.x;
    }
    if (fb1.y + m_circuits[1].displayRect.w > 2048)
    {
        fb1.y -= 2048;
        m_circuits[1].framebufferOffsets.y = fb1.y;
    }

    // PCRTCAntiBlur = true.
    if (m_pcrtcSameSrc && !scanmask)
    {
        if (std::abs(fb1.x - fb0.x) == 1 && m_circuits[0].displayRect.x == m_circuits[1].displayRect.x)
        {
            if (fb1.x < fb0.x)
                m_circuits[0].framebufferOffsets.x = fb1.x;
            else
                m_circuits[1].framebufferOffsets.x = fb0.x;
        }
    }
    for (int i = 0; i < 2; i++)
    {
        m_circuits[i].framebufferRect.x += m_circuits[i].framebufferOffsets.x;
        m_circuits[i].framebufferRect.z += m_circuits[i].framebufferOffsets.x;
        m_circuits[i].framebufferRect.y += m_circuits[i].framebufferOffsets.y;
        m_circuits[i].framebufferRect.w += m_circuits[i].framebufferOffsets.y;
    }

    if (m_pcrtcSameSrc && !scanmask && std::abs(fb1.y - fb0.y) <= 1)
    {
        const bool alternating0 = static_cast<int>(bits(dispfb0, 43, 11)) != m_circuits[0].prevFramebufferReg.dby;
        const bool alternating1 = static_cast<int>(bits(dispfb1, 43, 11)) != m_circuits[1].prevFramebufferReg.dby;

        int index;
        if (alternating0 && alternating1)
        {
            index = (static_cast<uint32_t>(m_circuits[1].framebufferRect.y) < static_cast<uint32_t>(m_circuits[0].framebufferRect.y)) ? 0 : 1;
        }
        else
        {
            index = alternating1 ? 1 : (alternating0 ? 0 : ((static_cast<uint32_t>(m_circuits[1].framebufferRect.y) < static_cast<uint32_t>(m_circuits[0].framebufferRect.y)) ? 0 : 1));
        }
        const int offset = m_circuits[1 - index].framebufferRect.y - m_circuits[index].framebufferRect.y;
        if (std::abs(offset) <= 4)
        {
            m_circuits[index].framebufferRect.y += offset;
            m_circuits[index].framebufferRect.w += offset;
        }
    }
}

namespace
{
    void blockSize(int psm, int &bsx, int &bsy)
    {
        if (psm == kPsmCT16 || psm == kPsmCT16S)
        {
            bsx = 16;
            bsy = 8;
        }
        else
        {
            bsx = 8;
            bsy = 8;
        }
    }
}

void GSDisplayReadout::removeFramebufferOffset(int display)
{
    // Software renderer: read block-aligned, keep the remainder inside the block.
    if (display >= 0)
    {
        int bsx, bsy;
        blockSize(m_circuits[display].psm, bsx, bsy);
        Rect &r = m_circuits[display].framebufferRect;
        const int ax = r.x & ~(bsx - 1);
        const int ay = r.y & ~(bsy - 1);
        r.z -= ax;
        r.w -= ay;
        r.x -= ax;
        r.y -= ay;
    }
    else
    {
        int bsx, bsy;
        blockSize(m_circuits[1].psm, bsx, bsy);
        const Rect u = framebufferRect(-1);
        const int ax = u.x & ~(bsx - 1);
        const int ay = u.y & ~(bsy - 1);
        for (int i = 0; i < 2; i++)
        {
            Rect &r = m_circuits[i].framebufferRect;
            r.x -= ax;
            r.y -= ay;
            r.z -= ax;
            r.w -= ay;
        }
    }
}

// GSRendererSW::GetOutput. Returns false where PCSX2 returns a null texture.
bool GSDisplayReadout::getOutput(int display, const ReadPixelFn &readPixel, Texture &tex)
{
    const int index = display >= 0 ? display : 1;
    const Circuit &cur = m_circuits[index];
    const Point size = framebufferSize(display);
    const Rect fr = framebufferRect(display);

    if (!(fr.x < fr.z && fr.y < fr.w) || cur.fbw == 0 || size.x < 0 || size.y < 0)
        return false;

    const int psm = cur.psm;
    if (psm != kPsmCT32 && psm != kPsmCT24 && psm != kPsmCT16 && psm != kPsmCT16S)
    {
        approximated("psm-" + std::to_string(psm),
                     "display pixel format " + std::to_string(psm) + ": the circuit shows nothing");
        return false;
    }

    int w = cur.fbw * 64;
    int h = size.y;
    if (h < 1)
    {
        approximated("fb-height", "a framebuffer height under 1: the circuit shows nothing");
        return false;
    }
    if (fr.x < 0 || fr.y < 0)
        approximated("fb-origin", "a negative framebuffer origin: clamped to 0");
    const int frX = std::max(fr.x, 0), frY = std::max(fr.y, 0);

    int bsx, bsy;
    blockSize(psm, bsx, bsy);
    const int offX = (frX & 0x7ff) & ~(bsx - 1);
    const int offXEnd = ((frX & 0x7ff) + (bsx - 1)) & ~(bsx - 1);
    const int offY = (frY & 0x7ff) & ~(bsy - 1);
    const int offYEnd = ((frY & 0x7ff) + (bsy - 1)) & ~(bsy - 1);

    removeFramebufferOffset(display);

    // m_output is 1024 pixels wide and 1024 rows tall; PCSX2 reads w + offXEnd - offX pixels per
    // row, so a wider read would spill into its next row. Reads that wrap at 2048 or exceed
    // that buffer are clamped (the cap of 1024 is the tighter one).
    const int maxW = 1024 - (offXEnd - offX), maxH = 1024 - (offYEnd - offY);
    if (w + offXEnd >= 2048 || h + offYEnd >= 2048 || w > maxW || h > maxH)
    {
        approximated("fb-read-clamp", "a framebuffer read that wraps at 2048 or exceeds the 1024x1024 read buffer: clamped");
        w = std::min({w, maxW, 2047 - offXEnd});
        h = std::min({h, maxH, 2047 - offYEnd});
        if (w < 1 || h < 1)
            return false;
    }

    tex.width = w;
    tex.height = h;
    tex.rgba.assign(static_cast<size_t>(w) * static_cast<size_t>(h), 0u);
    const uint32_t block = static_cast<uint32_t>(cur.fbp) << 5;
    for (int y = 0; y < h; y++)
    {
        for (int x = 0; x < w; x++)
        {
            const uint32_t v = readPixel(static_cast<uint32_t>(psm), block, static_cast<uint32_t>(cur.fbw),
                                         static_cast<uint32_t>(offX + x), static_cast<uint32_t>(offY + y));
            uint32_t px;
            if (psm == kPsmCT32)
            {
                px = v;
            }
            else if (psm == kPsmCT24)
            {
                px = (v & 0xffffffu) | (0x80u << 24); // Expand24To32, AEM = 0, TA0 = 0x80
            }
            else
            {
                // Expand16To32, AEM = 0, TA0 = 0, TA1 = 0x80
                px = (((v & 0x8000u) ? 0x80u : 0u) << 24) | ((v & 0x7c00u) << 9) | ((v & 0x03e0u) << 6) | ((v & 0x001fu) << 3);
            }
            tex.rgba[static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(x)] = px;
        }
    }
    return true;
}

bool GSDisplayReadout::readImpl(const GSDisplayRegs &regs, const ReadPixelFn &readPixel, DisplayImage &out)
{
    // --- GSvsync(): the CRTC update.
    const int cmod = static_cast<int>(bits(regs.smode1, 13, 2));
    const int lc = static_cast<int>(bits(regs.smode1, 3, 7));
    const bool isInterlaced = (regs.smode2 & 1u) != 0;
    int mode;
    switch (cmod)
    {
    case 0:
        if (isInterlaced && lc == 22)
            mode = kHdtv1080i;
        else if (!isInterlaced && lc == 22)
            mode = kHdtv720p;
        else if (!isInterlaced && lc == 32)
            mode = kSdtv480p;
        else
            mode = kVesa;
        break;
    case 2:
        mode = kNtsc;
        break;
    case 3:
        mode = kPal;
        break;
    default:
        mode = kUnknown;
        break;
    }
    if (mode == kUnknown)
    {
        approximated("video-mode", "an unknown video mode (SMODE1.CMOD = " + std::to_string(cmod) + "): the previous mode is kept");
        mode = m_videomode + 1;
    }
    m_videomode = mode - 1;

    const bool reallyInterlaced = (bits(regs.syncv, 0, 10) & 1u) != 0 && cmod != 0;
    enableDisplays(regs, reallyInterlaced);
    setRects(0, regs.display[0], regs.dispfb[0]);
    setRects(1, regs.display[1], regs.dispfb[1]);
    checkSameSource();
    const bool scanmask = m_scanmaskUsed != 0;
    calculateDisplayOffset(scanmask);
    calculateFramebufferOffset(scanmask, regs.dispfb[0], regs.dispfb[1]);

    // --- GSRenderer::Merge.
    if (bits(regs.extwrite, 0, 1) == 1)
        approximated("extwrite", "a feedback merge (EXTWRITE.WRITE = 1): the normal merge is shown");

    if (!m_circuits[0].enabled && !m_circuits[1].enabled)
        return true; // blank

    const int pmodeMmod = static_cast<int>(bits(regs.pmode, 5, 1));
    const int pmodeAmod = static_cast<int>(bits(regs.pmode, 6, 1));
    const int pmodeSlbg = static_cast<int>(bits(regs.pmode, 7, 1));
    const int pmodeAlp = static_cast<int>(bits(regs.pmode, 8, 8));

    Texture texs[2];
    const Texture *tex[2] = {nullptr, nullptr};
    if (m_pcrtcSameSrc && !frameWrap())
    {
        if (getOutput(-1, readPixel, texs[1]))
        {
            tex[0] = &texs[1];
            tex[1] = &texs[1];
        }
    }
    else
    {
        const bool useRc1 = m_circuits[0].enabled && (!(pmodeMmod == 1 && pmodeAlp == 0) || pmodeAmod == 0);
        const Rect &d0 = m_circuits[0].displayRect;
        const Rect &d1 = m_circuits[1].displayRect;
        const bool rc1ContainsRc2 = rcontains({d0.x, d0.y, d0.z, d0.w}, {d1.x, d1.y, d1.z, d1.w});
        const bool rc1OverwritesRc2 = useRc1 && rc1ContainsRc2 && pmodeMmod == 1 && pmodeAlp == 255;
        const bool useRc2 = m_circuits[1].enabled && ((pmodeSlbg == 0 && !rc1OverwritesRc2) || pmodeAmod == 1);
        if (useRc1 && getOutput(0, readPixel, texs[0]))
            tex[0] = &texs[0];
        if (useRc2 && getOutput(1, readPixel, texs[1]))
            tex[1] = &texs[1];
    }

    if (!tex[0] && !tex[1])
        return true; // blank

    // Destination rectangles (scale 1; the scan-mask shuffle in Merge cancels out) and source
    // rectangles (in texels of the texture they read).
    Rect dst[2] = {m_circuits[0].displayRect, m_circuits[1].displayRect};
    Rect src[2] = {m_circuits[0].framebufferRect, m_circuits[1].framebufferRect};

    Point fs = resolution();
    if (fs.x < 1 || fs.y < 1 || fs.x > 16384 || fs.y > 16384)
    {
        approximated("merge-size", "a merge target of " + std::to_string(fs.x) + "x" + std::to_string(fs.y) +
                                       ": a blank frame if under 1 pixel, else clamped to 16384");
        if (fs.x < 1 || fs.y < 1)
            return true; // blank
        fs.x = std::min(fs.x, 16384);
        fs.y = std::min(fs.y, 16384);
    }

    if (tex[0] == tex[1] && requal({src[0].x, src[0].y, src[0].z, src[0].w}, {src[1].x, src[1].y, src[1].z, src[1].w}) &&
        requal({dst[0].x, dst[0].y, dst[0].z, dst[0].w}, {dst[1].x, dst[1].y, dst[1].z, dst[1].w}) && !pmodeSlbg)
        tex[0] = nullptr; // identical outputs: only the copy is drawn

    // --- GSDevice::Merge / DoMerge.
    const uint8_t bgR = static_cast<uint8_t>(bits(regs.bgcolor, 0, 8));
    const uint8_t bgG = static_cast<uint8_t>(bits(regs.bgcolor, 8, 8));
    const uint8_t bgB = static_cast<uint8_t>(bits(regs.bgcolor, 16, 8));

    out.width = static_cast<uint32_t>(fs.x);
    out.height = static_cast<uint32_t>(fs.y);
    out.blank = false;
    out.rgb.resize(static_cast<size_t>(fs.x) * static_cast<size_t>(fs.y) * 3u);
    for (size_t i = 0; i < out.rgb.size(); i += 3)
    {
        out.rgb[i] = bgR;
        out.rgb[i + 1] = bgG;
        out.rgb[i + 2] = bgB;
    }

    // Draws `t` (source rect s) into dRect d: nearest sampling, clamp to edge. `blend` is 0 for
    // the plain copy, 1 for the blended draw.
    auto draw = [&](const Texture &t, const Rect &s, const Rect &d, bool blend)
    {
        const int dw = d.z - d.x;
        const int dh = d.w - d.y;
        if (dw < 0 || dh < 0)
        {
            approximated("dst-negative", "a destination rectangle with negative extent: not drawn");
            return;
        }
        if (dw == 0 || dh == 0)
            return;
        const int x0 = std::max(d.x, 0), x1 = std::min(d.z, fs.x);
        const int y0 = std::max(d.y, 0), y1 = std::min(d.w, fs.y);
        if (x0 >= x1 || y0 >= y1)
            return;

        auto axis = [&](int from, int to, int d0, int dSize, double s0, double sSize, int texSize, std::vector<int> &idx)
        {
            idx.resize(static_cast<size_t>(to - from));
            for (int p = from; p < to; p++)
            {
                const double u = s0 + (static_cast<double>(p) + 0.5 - d0) * (sSize / dSize);
                const int lo = std::clamp(static_cast<int>(std::floor(u - 1e-3)), 0, texSize - 1);
                const int hi = std::clamp(static_cast<int>(std::floor(u + 1e-3)), 0, texSize - 1);
                if (lo != hi)
                    approximated("nearest-tie", "an ambiguous nearest sampling (destination pixel centre on a texel edge): the plain formula is used");
                idx[static_cast<size_t>(p - from)] = std::clamp(static_cast<int>(std::floor(u)), 0, texSize - 1);
            }
        };
        std::vector<int> ix, iy;
        axis(x0, x1, d.x, dw, s.x, s.z - s.x, t.width, ix);
        axis(y0, y1, d.y, dh, s.y, s.w - s.y, t.height, iy);

        for (int y = y0; y < y1; y++)
        {
            const uint32_t *row = &t.rgba[static_cast<size_t>(iy[static_cast<size_t>(y - y0)]) * static_cast<size_t>(t.width)];
            uint8_t *o = &out.rgb[(static_cast<size_t>(y) * static_cast<size_t>(fs.x) + static_cast<size_t>(x0)) * 3u];
            for (int x = x0; x < x1; x++, o += 3)
            {
                const uint32_t px = row[ix[static_cast<size_t>(x - x0)]];
                const int r = px & 0xff, g = (px >> 8) & 0xff, b = (px >> 16) & 0xff, a = px >> 24;
                if (!blend)
                {
                    o[0] = static_cast<uint8_t>(r);
                    o[1] = static_cast<uint8_t>(g);
                    o[2] = static_cast<uint8_t>(b);
                }
                else
                {
                    const int sa = pmodeMmod == 1 ? pmodeAlp : std::min(255, 2 * a);
                    o[0] = static_cast<uint8_t>(std::min(255, rnd255(r * sa) + rnd255(o[0] * (255 - sa))));
                    o[1] = static_cast<uint8_t>(std::min(255, rnd255(g * sa) + rnd255(o[1] * (255 - sa))));
                    o[2] = static_cast<uint8_t>(std::min(255, rnd255(b * sa) + rnd255(o[2] * (255 - sa))));
                }
            }
        }
    };

    if (tex[1] && pmodeSlbg == 0)
        draw(*tex[1], src[1], dst[1], false);
    if (tex[0])
        draw(*tex[0], src[0], dst[0], true);

    if (m_scanmaskUsed)
        m_scanmaskUsed--;
    return true;
}
