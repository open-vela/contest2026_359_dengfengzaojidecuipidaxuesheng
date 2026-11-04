#include <assert.h>
#include <string.h>
#include <stdio.h>
#include "route_plan.h"
int main(void){
    routes_t r={0};int y=-1;
    assert(routes_validate(&r)==0);
    assert(routes_connect(&r,0,20,&y)==0 && y==0);
    assert(routes_connect(&r,20,40,&y)==0 && y==0);
    assert(routes_connect(&r,1,21,&y)==0 && y==1);
    assert(routes_connect(&r,2,22,&y)==0 && y==2);
    assert(routes_connect(&r,0,1,&y)==0 && y==0);
    assert(r.net[1]==0);assert(routes_net_of(&r,21)==0);
    assert(routes_remove(&r,40)==0 && routes_net_of(&r,40)==-1);
    // Power collisions and direct MCU supply connections are forbidden.
    routes_t old=r;
    assert(routes_connect(&r,49,50,NULL)==J_FORBIDDEN);
    assert(memcmp(&r,&old,sizeof r)==0);
    assert(routes_connect(&r,49,40,NULL)==J_FORBIDDEN);
    assert(memcmp(&r,&old,sizeof r)==0);
    assert(routes_connect(&r,50,51,NULL)==J_FORBIDDEN);
    assert(memcmp(&r,&old,sizeof r)==0);
    assert(routes_connect(&r,50,40,NULL)==J_FORBIDDEN);
    assert(memcmp(&r,&old,sizeof r)==0);
    // The default safe policy prevents shorting two ESP push-pull drivers.
    assert(routes_connect(&r,40,41,NULL)==J_FORBIDDEN);
    assert(memcmp(&r,&old,sizeof r)==0);
    // exact 8 independent nets: on the ninth, fail
    memset(&r,0,sizeof r);
    for(unsigned i=0;i<8;i++) assert(routes_connect(&r,2*i,2*i+1,NULL)==0);
    assert(routes_connect(&r,30,31,NULL)==J_OVERFLOW);
    // No shared nodes even if duplicate manually provided
    memset(&r,0,sizeof r);r.net[0]=1;r.net[1]=1;
    assert(routes_validate(&r)==J_BAD_ARG);
    // mapping of all 52 nodes: all distinct physical X and chip
    bool used[4][16]={0};
    for(unsigned i=0;i<52;i++){
        unsigned c=mapped_chip(i),x=mapped_x(i);
        assert(c<4 && x<16 && !used[c][x]);used[c][x]=true;
    }
    // Explicit schematic anchors: CN1 sensor pins, EXT1 and U5 supplies.
    assert(mapped_chip(19)==1 && mapped_x(19)==3); // CN1-39 -> U3 X3
    assert(mapped_chip(18)==1 && mapped_x(18)==2); // CN1-37 -> U3 X2
    assert(mapped_chip(16)==1 && mapped_x(16)==0); // CN1-33 -> U3 X0
    assert(mapped_chip(40)==2 && mapped_x(40)==8); // GPIO1 -> U4 X8
    assert(mapped_chip(49)==3 && mapped_x(49)==1); // 5V -> U5 X1
    assert(mapped_chip(50)==3 && mapped_x(50)==2); // 3.3V -> U5 X2
    assert(mapped_chip(51)==3 && mapped_x(51)==3); // GND -> U5 X3
    puts("route allocator and pin mapping: PASS");
}
