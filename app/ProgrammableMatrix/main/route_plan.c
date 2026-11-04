#include "route_plan.h"
#include <string.h>

int routes_validate(const routes_t *r)
{
    if (!r) return J_BAD_ARG;
    uint64_t seen=0;
    const uint64_t valid=(1ULL<<BOARD_NODES)-1ULL;
    const uint64_t power=(1ULL<<BOARD_POWER5)|(1ULL<<BOARD_POWER33)|(1ULL<<BOARD_GND);
    const uint64_t extmask=((1ULL<<BOARD_EXT)-1ULL)<<40;
    for (unsigned y=0;y<BOARD_NETS;y++) {
        uint64_t s=r->net[y];
        if (s & ~valid) return J_BAD_ARG;
        if (s & seen) return J_BAD_ARG; // each X belongs to one net
        seen |= s;
        if (__builtin_popcountll(s & power)>1) return J_FORBIDDEN;
        if ((s & power) && (s & extmask)) return J_FORBIDDEN;
        // Fail-safe policy: do not tie two independent MCU GPIOs together.
        // If needed later, make an explicit hardware-aware override with
        // output-driver contention checking rather than silently allowing it.
        if (__builtin_popcountll(s & extmask)>1) return J_FORBIDDEN;
    }
    return J_OK;
}
int routes_net_of(const routes_t *r, unsigned a)
{
    if (!r||a>=BOARD_NODES) return J_BAD_ARG;
    for (unsigned y=0;y<BOARD_NETS;y++)
        if ((r->net[y]>>a)&1ULL) return (int)y;
    return -1;
}
int routes_connect(routes_t *r, unsigned a, unsigned b, int *out_net)
{
    if (!r || a>=BOARD_NODES || b>=BOARD_NODES || a==b) return J_BAD_ARG;
    int va=routes_validate(r); if(va!=J_OK) return va;
    int x=routes_net_of(r,a),y=routes_net_of(r,b);
    if (x<0 && y<0) {
        for (unsigned i=0;i<BOARD_NETS;i++) if (!r->net[i]) { x=(int)i;break; }
        if (x<0) return J_OVERFLOW;
    } else if (x<0) x=y;
    routes_t next=*r;
    if(y>=0 && y!=x) {
        next.net[x] |= next.net[y];
        next.net[y] = 0;
    }
    next.net[x] |= (1ULL<<a)|(1ULL<<b);
    int rc=routes_validate(&next);
    if(rc!=J_OK) return rc;
    *r=next;
    if(out_net)*out_net=x;
    return J_OK;
}
int routes_remove(routes_t *r, unsigned a)
{
    if(!r || a>=BOARD_NODES) return J_BAD_ARG;
    int rc=routes_validate(r); if(rc!=J_OK) return rc;
    for (unsigned y=0;y<BOARD_NETS;y++) r->net[y] &= ~(1ULL<<a);
    return J_OK;
}
