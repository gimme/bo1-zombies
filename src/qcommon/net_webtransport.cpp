#include "net_webtransport.h"

#include <stdlib.h>
#include <string.h>

bool WT_FramePacket(const unsigned __int8 *packet, int length, unsigned __int16 *nextId, wtEmitDatagram_t emit, void *context)
{
    unsigned __int8 datagram[WT_MAX_DATAGRAM];

    if ( length < 0 || length > WT_MAX_PACKET )
        return false;
    if ( length + 1 <= WT_MAX_DATAGRAM )
    {
        datagram[0] = WT_WHOLE;
        memcpy(&datagram[1], packet, length);
        emit(context, datagram, length + 1);
        return true;
    }

    int count = (length + WT_FRAGMENT_SIZE - 1) / WT_FRAGMENT_SIZE;
    unsigned __int16 id = (*nextId)++;
    for ( int index = 0; index < count; ++index )
    {
        int start = index * WT_FRAGMENT_SIZE;
        int size = length - start < WT_FRAGMENT_SIZE ? length - start : WT_FRAGMENT_SIZE;
        datagram[0] = WT_FRAGMENT;
        datagram[1] = (unsigned __int8)id;
        datagram[2] = (unsigned __int8)(id >> 8);
        datagram[3] = (unsigned __int8)index;
        datagram[4] = (unsigned __int8)count;
        memcpy(&datagram[WT_FRAGMENT_HEADER], &packet[start], size);
        emit(context, datagram, WT_FRAGMENT_HEADER + size);
    }
    return true;
}

static void WT_FreePartial(wtPartialPacket_t *p)
{
    free(p->data);
    memset(p, 0, sizeof(*p));
}

const unsigned __int8 *WT_ReceiveDatagram(wtReassembly_t *r, const unsigned __int8 *datagram, int length, int *packetLength)
{
    free(r->completed);
    r->completed = NULL;

    if ( length < 1 )
        return NULL;
    if ( datagram[0] == WT_WHOLE )
    {
        *packetLength = length - 1;
        return &datagram[1];
    }
    if ( datagram[0] != WT_FRAGMENT || length <= WT_FRAGMENT_HEADER )
        return NULL;

    unsigned __int16 id = (unsigned __int16)(datagram[1] | (datagram[2] << 8));
    int index = datagram[3];
    int count = datagram[4];
    int size = length - WT_FRAGMENT_HEADER;
    if ( count < 2 || count > WT_MAX_FRAGMENTS || index >= count )
        return NULL;
    // every piece but the last is full
    if ( index < count - 1 ? size != WT_FRAGMENT_SIZE : size > WT_FRAGMENT_SIZE )
        return NULL;

    wtPartialPacket_t *p = NULL;
    for ( int i = 0; i < 2 && !p; ++i )
    {
        if ( r->partial[i].inUse && r->partial[i].id == id )
            p = &r->partial[i];
    }
    if ( p && p->count != count )
        return NULL;
    if ( !p )
    {
        // a new packet: a free slot, else the one started first
        p = !r->partial[0].inUse ? &r->partial[0] : !r->partial[1].inUse ? &r->partial[1]
            : (int)(r->partial[0].age - r->partial[1].age) <= 0 ? &r->partial[0] : &r->partial[1];
        WT_FreePartial(p);
        p->data = (unsigned __int8 *)malloc(count * WT_FRAGMENT_SIZE);
        if ( !p->data )
            return NULL;
        p->id = id;
        p->count = (unsigned __int8)count;
        p->inUse = 1;
        p->age = r->clock++;
    }

    unsigned __int64 bit = 1ull << index;
    if ( p->received & bit )
        return NULL; // a duplicate
    p->received |= bit;
    memcpy(&p->data[index * WT_FRAGMENT_SIZE], &datagram[WT_FRAGMENT_HEADER], size);
    p->length += size;
    if ( p->received != (count == 64 ? ~0ull : (1ull << count) - 1) )
        return NULL;
    if ( p->length > WT_MAX_PACKET )
    {
        WT_FreePartial(p);
        return NULL;
    }

    r->completed = p->data;
    *packetLength = p->length;
    p->data = NULL;
    WT_FreePartial(p);
    return r->completed;
}

void WT_ClearReassembly(wtReassembly_t *r)
{
    WT_FreePartial(&r->partial[0]);
    WT_FreePartial(&r->partial[1]);
    free(r->completed);
    memset(r, 0, sizeof(*r));
}
