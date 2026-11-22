#pragma once

// WebTransport datagram framing: the WebAssembly build's sockets (web/src/web_net.cpp) send and receive packets this
// way, and so does zombies-hub's server, which relays them to the game's UDP port (zombies-hub's server/framing.go is
// the same). Web clients reach a server over WebTransport datagrams, which carry the same packets UDP does. A packet
// travels in one datagram when it fits and in fragments otherwise:
//
//   whole packet:  0x00, packet
//   fragment:      0x01, id (16 bits, little endian), index, count, piece   (pieces of WT_FRAGMENT_SIZE, the last shorter)
//
// No datagram is longer than WT_MAX_DATAGRAM, which fits a QUIC path at its minimum packet size (1200 bytes) with room
// for QUIC's own headers, so neither side needs to know the path's real limit. Netchan packets take one: the netchan
// makes them to fit (net_chan_mp.h's MAX_PACKETLEN). Out-of-band packets can be up to 64 KB, as over UDP.

#define WT_MAX_DATAGRAM 1100
#define WT_WHOLE 0x00
#define WT_FRAGMENT 0x01
#define WT_FRAGMENT_HEADER 5
#define WT_FRAGMENT_SIZE (WT_MAX_DATAGRAM - WT_FRAGMENT_HEADER)
#define WT_MAX_FRAGMENTS 64 // 64 * WT_FRAGMENT_SIZE covers the largest packet, 0x10000 bytes
#define WT_MAX_PACKET 0x10000

typedef void (*wtEmitDatagram_t)(void *context, const unsigned __int8 *datagram, int length);

// Splits a packet into datagrams and calls emit for each, in order. nextId numbers one sender's fragmented packets.
// Returns false (nothing emitted) when the packet is larger than WT_MAX_PACKET.
bool WT_FramePacket(const unsigned __int8 *packet, int length, unsigned __int16 *nextId, wtEmitDatagram_t emit, void *context);

// A packet being put back together from its fragments. Two at a time, so a fragment that arrives late (datagrams
// can be reordered) does not throw away the packet after it.
struct wtPartialPacket_t
{
    unsigned __int8 *data; // count * WT_FRAGMENT_SIZE bytes, allocated with the first fragment
    unsigned __int64 received; // a bit per fragment index
    int length; // so far
    unsigned __int16 id;
    unsigned __int8 count;
    unsigned __int8 inUse;
    unsigned int age; // reassembly's clock when it started: the oldest goes first
};

struct wtReassembly_t
{
    wtPartialPacket_t partial[2];
    unsigned __int8 *completed; // the last packet returned from fragments, freed by the next call
    unsigned int clock;
};

// Takes one datagram. Returns the packet it completes (in the datagram itself, or in the reassembly's buffer until
// the next call) and its length, or NULL when it is a fragment of a packet still missing pieces or malformed.
const unsigned __int8 *WT_ReceiveDatagram(wtReassembly_t *r, const unsigned __int8 *datagram, int length, int *packetLength);

// Frees what a reassembly holds; it can be used again afterwards.
void WT_ClearReassembly(wtReassembly_t *r);
