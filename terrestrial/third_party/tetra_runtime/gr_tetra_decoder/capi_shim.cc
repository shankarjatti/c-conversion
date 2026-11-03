// Thin C-linkage wrapper around Tetra::TetraDecoder so it can be called
// via ctypes from a GNU Radio Python block, instead of running tetra-kit's
// decoder as a separate OS process reached over UDP:42000.
//
// No changes to tetra-kit's actual protocol decode logic (decoder.cc/.h and
// everything under mac/, llc/, mle/, mm/, cmce/, sndcp/, uplane/, common/) —
// this file only exposes the existing class's constructor and rxSymbol()
// with C linkage so ctypes can find them by name.
#include "../tetra-kit/decoder/decoder.h"

extern "C" {

void* tetrakit_create(int socketFd, int removeFillBits, int logLevel, int wiresharkOutput)
{
    return new Tetra::TetraDecoder(
        socketFd,
        removeFillBits != 0,
        static_cast<Tetra::LogLevel>(logLevel),
        wiresharkOutput != 0
    );
}

// sym: 0 or 1, one raw channel bit per call — same convention as tetra-kit's
// own main.cc (decoder->rxSymbol(rxBuf[cnt])), fed directly from the GNU
// Radio bitstream instead of a UDP socket.
void tetrakit_rx_symbol(void* handle, unsigned char sym)
{
    static_cast<Tetra::TetraDecoder*>(handle)->rxSymbol(sym);
}

void tetrakit_destroy(void* handle)
{
    delete static_cast<Tetra::TetraDecoder*>(handle);
}

} // extern "C"
