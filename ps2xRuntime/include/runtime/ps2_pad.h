#ifndef PS2_PAD_H
#define PS2_PAD_H

#include <cstddef>
#include <cstdint>

class PSPadBackend
{
public:
    PSPadBackend() = default;
    ~PSPadBackend() = default;

    bool readState(int port, int slot, uint8_t *data, size_t size);

    // When off, readState reports no buttons and centred sticks without calling raylib.
    void setHostInputEnabled(bool enabled) { m_hostInputEnabled = enabled; }
    bool hostInputEnabled() const { return m_hostInputEnabled; }

private:
    bool m_hostInputEnabled = true;
};

#endif
