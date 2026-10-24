#ifndef PS2_GS_DISPLAY_READOUT_H
#define PS2_GS_DISPLAY_READOUT_H

// Exact display read-out: the image PCSX2 v2.8.2 (software renderer, native resolution) hands to
// its output stage for one vsync, as 8-bit RGB. See gs_display_readout.cpp for the PCSX2
// functions it mirrors and the cases it approximates.

#include <cstdint>
#include <functional>
#include <set>
#include <string>
#include <vector>

struct DisplayImage
{
    uint32_t width = 0;
    uint32_t height = 0;
    // True when PCSX2 would show nothing: both read circuits off, or no usable framebuffer.
    // width and height are then 0 and rgb is empty.
    bool blank = true;
    // width * height * 3 bytes: R, G, B per pixel, rows top to bottom, no padding, alpha dropped.
    std::vector<uint8_t> rgb;
};

// The privileged GS registers the read-out looks at.
struct GSDisplayRegs
{
    uint64_t pmode = 0;
    uint64_t smode1 = 0;
    uint64_t smode2 = 0;
    uint64_t syncv = 0;
    uint64_t dispfb[2] = {0, 0};
    uint64_t display[2] = {0, 0};
    uint64_t extbuf = 0;
    uint64_t extwrite = 0;
    uint64_t bgcolor = 0;
};

// Holds the CRTC state PCSX2 carries from one vsync to the next (GSState::GSPCRTCRegs). Call
// read() at every vsync, in order: its anti-blur handling compares each vsync's DISPFB with the
// previous one's. One instance per GS; not thread safe.
class GSDisplayReadout
{
public:
    // Reads one local-memory pixel, as GS::ReadVram: the raw value in `psm` (32 bits for CT32,
    // the low 24 bits for CT24, 16 bits for CT16 and CT16S), at block address `block`
    // (FBP << 5), buffer width `fbw` (in 64-pixel units), pixel x and y.
    using ReadPixelFn = std::function<uint32_t(uint32_t psm, uint32_t block, uint32_t fbw, uint32_t x, uint32_t y)>;

    GSDisplayReadout();

    void reset();

    // A write to GS SCANMSK (GSState::GIFRegHandlerSCANMSK): MSK bit 1 arms the scan-mask
    // frame counter for the next two vsyncs.
    void noteScanmaskWrite(uint64_t scanmskValue);

    // Runs PCSX2's GSvsync() CRTC update and GSRenderer::Merge() for the current registers and
    // local memory. `field` is the current field (CSR.FIELD); the oracle's deinterlace mode is
    // Off, where PCSX2's merge does not look at it, so it does not change the image.
    //
    // Always returns true with `out` filled in (blank or not). A state it does not reproduce
    // exactly gets a reasonable image, and the first time each such case is met it prints
    // "[gs-readout] approximated ..." to stderr (the choices are listed in the .cpp header).
    bool read(const GSDisplayRegs &regs, const ReadPixelFn &readPixel, bool field, DisplayImage &out);

private:
    struct Rect
    {
        int x = 0, y = 0, z = 0, w = 0; // left, top, right, bottom
    };
    struct Point
    {
        int x = 0, y = 0;
    };
    struct FramebufferReg
    {
        int fbp = 0, fbw = 0, psm = 0, dbx = 0, dby = 0;
    };
    struct Circuit
    {
        bool enabled = false;
        int fbp = 0, fbw = 0, psm = 0, dby = 0, dbx = 0;
        FramebufferReg prevFramebufferReg;
        Point prevDisplayOffset;
        Point displayOffset;
        Rect displayRect;
        Point magnification;
        Point prevFramebufferOffsets;
        Point framebufferOffsets;
        Rect framebufferRect;
    };
    // A texture GetOutput() produced: RGBA, width x height.
    struct Texture
    {
        int width = 0;
        int height = 0;
        std::vector<uint32_t> rgba;
    };

    // Logs "[gs-readout] approximated <what>" the first time `key` is seen.
    void approximated(const std::string &key, const std::string &what) const;

    bool readImpl(const GSDisplayRegs &regs, const ReadPixelFn &readPixel, DisplayImage &out);

    bool isAnalogue() const;
    Point nearestToZeroOffset() const;
    void enableDisplays(const GSDisplayRegs &regs, bool smodetoggle);
    void setRects(int display, uint64_t displayReg, uint64_t framebufferReg);
    void checkSameSource();
    bool frameWrap() const;
    Point resolution() const;
    Rect framebufferRect(int display) const;
    Point framebufferSize(int display) const;
    void calculateDisplayOffset(bool scanmask);
    void calculateFramebufferOffset(bool scanmask, uint64_t dispfb0, uint64_t dispfb1);
    void removeFramebufferOffset(int display);
    bool getOutput(int display, const ReadPixelFn &readPixel, Texture &tex);

    int m_videomode = 0;
    int m_interlaced = 0;
    int m_ffmd = 0;
    bool m_pcrtcSameSrc = false;
    bool m_togglingField = false;
    Circuit m_circuits[2];
    int m_scanmaskUsed = 0;
    mutable std::set<std::string> m_approximated; // cases already logged; kept across reset()
};

#endif
