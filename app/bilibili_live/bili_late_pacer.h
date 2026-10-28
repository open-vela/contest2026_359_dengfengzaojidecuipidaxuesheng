/* SPDX-License-Identifier: Apache-2.0 */
#ifndef BILI_LATE_PACER_H
#define BILI_LATE_PACER_H
#include <stdbool.h>
#include <stdint.h>
struct bili_late_pacer {uint64_t next_ms;unsigned period_ms;};
/* Keep a deadline phase instead of restarting the period at each late frame.
 * Otherwise a decode interval just below the period drops every other frame.
 * A pause/long stall reanchors the deadline, so missed frames never burst.
 */
static inline bool bili_late_drop(struct bili_late_pacer *p,uint64_t now,
                                bool late,unsigned period)
{
  if(!period)period=1;
  if(!late || !p->next_ms || p->period_ms!=period){
    p->next_ms=now;p->period_ms=period;
  }
  if(now<p->next_ms)return true;
  if(now-p->next_ms>=period)p->next_ms=now+period;
  else p->next_ms+=period;
  return false;
}
#endif
