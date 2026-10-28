"""Supplement the verified first-fault candidate with SD/IRQ provenance.

Diagnostic-only generator; never deploys or accesses hardware.
"""
from openvela_wifi_first_fault import replace_fn
from openvela_wifi_tx_errors import once

RECORDS = r'''
/* Separate frozen windows for SPI token waits and SDMMC waits.  These
 * record software timing, not a hardware completion timestamp.  The
 * singleton P4 port supplies the private, non-recursive trace lock.
 */

#define IO_CAUSE_CPUS 2
#define IO_CAUSE_EVENTS 16

struct io_cause_event_s
{
  uint64_t us;
  int32_t pid;
  int32_t irq;
  uint8_t cpu;
  uint8_t priority;
  uint8_t state;
  uint8_t resumed;
};

struct io_irq_context_s
{
  uint64_t since_us;
  int32_t irq;
  int32_t pid;
  uint32_t depth;
};

struct io_cause_s
{
  uint32_t magic;
  uint32_t version;
  uint32_t bytes;
  uint32_t valid;
  uint32_t active;
  uint32_t sequence;
  uint32_t kind;
  int32_t owner_pid;
  uint32_t owner_cpu;
  uint32_t start_tick;
  uint64_t start_us;
  uint32_t end_tick;
  int32_t error;
  uint64_t end_us;
  uint64_t off_since_us;
  uint64_t off_total_us;
  uint64_t off_max_us;
  uint32_t suspended;
  uint32_t event_count;
  struct io_cause_event_s events[IO_CAUSE_EVENTS];
  uint32_t irq_count[IO_CAUSE_CPUS];
  uint64_t irq_max_us[IO_CAUSE_CPUS];
  int32_t irq_max_id[IO_CAUSE_CPUS];
  int32_t irq_max_pid[IO_CAUSE_CPUS];
  struct io_irq_context_s irq_at_end[IO_CAUSE_CPUS];
  uint32_t token;
  uint32_t polls;
  uint32_t limit_ticks;
  uint32_t speed_requested;
  uint64_t poll_gap_max_us;
  uint64_t spi_call_max_us;
};

static volatile struct io_cause_s g_io_cause[2];
static struct io_irq_context_s g_io_irq[IO_CAUSE_CPUS];
static bool g_io_enabled;

uint64_t esp32p4_io_time(void)
{
  return esp_timer_get_time();
}

void esp32p4_io_begin(unsigned int kind, uint32_t limit, uint32_t speed)
{
  volatile struct io_cause_s *p = &g_io_cause[kind];
  irqstate_t flags = spin_lock_irqsave_notrace(&g_sdio_trace_lock);
  unsigned int cpu;

  if (p->valid == 0)
    {
      p->magic = 0x494f4331;
      p->version = 1;
      p->bytes = sizeof(*p);
      p->active = 1;
      p->sequence++;
      p->kind = kind;
      p->owner_pid = nxsched_getpid();
      p->owner_cpu = up_cpu_index();
      p->start_tick = clock_systime_ticks();
      p->start_us = esp_timer_get_time();
      p->end_tick = 0;
      p->end_us = 0;
      p->error = 0;
      p->off_since_us = 0;
      p->off_total_us = 0;
      p->off_max_us = 0;
      p->suspended = 0;
      p->event_count = 0;
      p->limit_ticks = limit;
      p->speed_requested = speed;
      p->token = 0;
      p->polls = 0;
      p->poll_gap_max_us = 0;
      p->spi_call_max_us = 0;
      for (cpu = 0; cpu < IO_CAUSE_CPUS; cpu++)
        {
          p->irq_count[cpu] = 0;
          p->irq_max_us[cpu] = 0;
          p->irq_max_id[cpu] = -1;
          p->irq_max_pid[cpu] = -1;
        }

      __atomic_store_n(&g_io_enabled, true, __ATOMIC_RELEASE);
    }

  spin_unlock_irqrestore_notrace(&g_sdio_trace_lock, flags);
}

void esp32p4_io_end(unsigned int kind, int error, uint32_t token,
                    uint32_t polls, uint64_t gap, uint64_t spi)
{
  volatile struct io_cause_s *p = &g_io_cause[kind];
  irqstate_t flags = spin_lock_irqsave_notrace(&g_sdio_trace_lock);
  unsigned int cpu;

  if (p->active && p->valid == 0)
    {
      p->end_us = esp_timer_get_time();
      p->end_tick = clock_systime_ticks();
      p->error = error;
      p->token = token;
      p->polls = polls;
      p->poll_gap_max_us = gap;
      p->spi_call_max_us = spi;
      for (cpu = 0; cpu < IO_CAUSE_CPUS; cpu++)
        {
          p->irq_at_end[cpu] = g_io_irq[cpu];
        }

      p->active = 0;
      if (error != 0)
        {
          /* All producers use the same lock.  Publish complete last. */

          p->valid = 2;
        }
    }

  spin_unlock_irqrestore_notrace(&g_sdio_trace_lock, flags);
}

/* Called around actual IRQ dispatch.  Exceptions are deliberately excluded.
 * Track only the outer IRQ span if a fault nests into a handler.  No MMIO,
 * task lookup, cache operation, printing or scheduler-global lock here.
 */

void esp32p4_io_irq(int irq, int pid, bool entering)
{
  irqstate_t flags;
  unsigned int cpu;
  struct io_irq_context_s *c;
  volatile struct io_cause_s *p;
  uint64_t now;
  uint64_t span;
  unsigned int kind;

  if (!__atomic_load_n(&g_io_enabled, __ATOMIC_ACQUIRE))
    {
      return;
    }

  flags = spin_lock_irqsave_notrace(&g_sdio_trace_lock);
  cpu = up_cpu_index();
  c = &g_io_irq[cpu];
  now = esp_timer_get_time();
  if (entering)
    {
      if (c->depth++ == 0)
        {
          c->since_us = now;
          c->irq = irq;
          c->pid = pid;
        }

      for (kind = 0; kind < 2; kind++)
        {
          p = &g_io_cause[kind];
          if (p->active && p->valid == 0)
            {
              p->irq_count[cpu]++;
            }
        }
    }
  else if (c->depth > 0 && --c->depth == 0)
    {
      for (kind = 0; kind < 2; kind++)
        {
          p = &g_io_cause[kind];
          span = now - (c->since_us > p->start_us ?
                        c->since_us : p->start_us);
          if (p->active && p->valid == 0 && span > p->irq_max_us[cpu])
            {
              p->irq_max_us[cpu] = span;
              p->irq_max_id[cpu] = c->irq;
              p->irq_max_pid[cpu] = c->pid;
            }
        }
    }

  spin_unlock_irqrestore_notrace(&g_sdio_trace_lock, flags);
}

static void esp32p4_io_sched(int pid, int priority, int state, bool resumed)
{
  irqstate_t flags;
  uint64_t now;
  unsigned int cpu;
  volatile struct io_cause_s *p;
  volatile struct io_cause_event_s *e;
  uint64_t span;
  unsigned int kind;

  if (!__atomic_load_n(&g_io_enabled, __ATOMIC_ACQUIRE))
    {
      return;
    }

  flags = spin_lock_irqsave_notrace(&g_sdio_trace_lock);
  now = esp_timer_get_time();
  cpu = up_cpu_index();
  for (kind = 0; kind < 2; kind++)
    {
      p = &g_io_cause[kind];
      if (!p->active || p->valid != 0)
        {
          continue;
        }

      e = &p->events[p->event_count++ % IO_CAUSE_EVENTS];
      e->us = now;
      e->pid = pid;
      e->cpu = cpu;
      e->priority = priority;
      e->state = state;
      e->resumed = resumed;
      e->irq = g_io_irq[cpu].depth > 0 ? g_io_irq[cpu].irq : -1;
      if (pid == p->owner_pid)
        {
          if (!resumed && !p->suspended)
            {
              p->suspended = 1;
              p->off_since_us = now;
            }
          else if (resumed && p->suspended)
            {
              span = now - p->off_since_us;
              p->off_total_us += span;
              if (span > p->off_max_us)
                {
                  p->off_max_us = span;
                }

              p->suspended = 0;
            }
        }
    }

  spin_unlock_irqrestore_notrace(&g_sdio_trace_lock, flags);
}
'''


def patch_p4(text):
    marker='static void esp32p4_trace_phase('
    text=once(text,marker,RECORDS.lstrip()+'\n'+marker)
    def begin(s):
        return once(s,'  spin_unlock_irqrestore_notrace(&g_sdio_trace_lock, flags);',
            '  spin_unlock_irqrestore_notrace(&g_sdio_trace_lock, flags);\n  esp32p4_io_begin(1, 0, 0);')
    text=replace_fn(text,'static void esp32p4_trace_begin(',begin)
    def sched(s):
        return once(s,'  flags = spin_lock_irqsave_notrace(&g_sdio_trace_lock);',
            '  esp32p4_io_sched(pid, priority, state, resumed);\n  flags = spin_lock_irqsave_notrace(&g_sdio_trace_lock);')
    text=replace_fn(text,'void esp32p4_sdio_trace_sched(',sched)
    def fault(s):
        return once(s,'  spin_unlock_irqrestore_notrace(&g_sdio_trace_lock, flags);',
            '  spin_unlock_irqrestore_notrace(&g_sdio_trace_lock, flags);\n  esp32p4_io_end(1, error, reason, 0, 0, 0);')
    text=replace_fn(text,'static void esp32p4_trace_fault(',fault)
    def phase(s):
        return once(s,'  spin_unlock_irqrestore_notrace(&g_sdio_trace_lock, flags);',
            '''  spin_unlock_irqrestore_notrace(&g_sdio_trace_lock, flags);
  if (phase == SDIO_TRACE_RETURN)
    {
      esp32p4_io_end(1, 0, 0, 0, 0, 0);
    }''')
    return replace_fn(text,'static void esp32p4_trace_phase(',phase)


def patch_irq(text):
    text=once(text,'#  define IRQ_DISPATCH(irq, regs)    irq_dispatch(irq, regs)',
        '''#  ifdef CONFIG_ESP32P4_SDMMC
void esp32p4_io_irq(int irq, int pid, bool entering);
#    define IRQ_DISPATCH(irq, regs) \\
  do \\
    { \\
      if ((irq) > RISCV_MAX_EXCEPTION) \\
        { \\
          esp32p4_io_irq(irq, this_task()->pid, true); \\
        } \\
      irq_dispatch(irq, regs); \\
      if ((irq) > RISCV_MAX_EXCEPTION) \\
        { \\
          esp32p4_io_irq(irq, this_task()->pid, false); \\
        } \\
    } \\
  while (0)
#  else
#    define IRQ_DISPATCH(irq, regs) irq_dispatch(irq, regs)
#  endif''')
    return text


def patch_sd(text):
    declarations='''#ifdef CONFIG_ESP32P4_SDMMC
uint64_t esp32p4_io_time(void);
void esp32p4_io_begin(unsigned int kind, uint32_t limit, uint32_t speed);
void esp32p4_io_end(unsigned int kind, int error, uint32_t token,
                    uint32_t polls, uint64_t gap, uint64_t spi);
#endif

'''
    text=once(text,'/* This structure represents the state of one card slot */',
        declarations+'/* This structure represents the state of one card slot */')
    def recv(s):
        s=once(s,'  uint8_t token;', '''  uint8_t token;
#ifdef CONFIG_ESP32P4_SDMMC
  uint64_t poll_us;
  uint64_t last_us;
  uint64_t done_us;
  uint64_t gap_us = 0;
  uint64_t spi_us = 0;

  esp32p4_io_begin(0, slot->taccess, slot->spispeed);
  last_us = esp32p4_io_time();
#endif''')
        s=once(s,'      token = SPI_SEND(spi, 0xff);', '''#ifdef CONFIG_ESP32P4_SDMMC
      poll_us = esp32p4_io_time();
#endif
      token = SPI_SEND(spi, 0xff);
#ifdef CONFIG_ESP32P4_SDMMC
      done_us = esp32p4_io_time();
      if (done_us - last_us > gap_us)
        {
          gap_us = done_us - last_us;
        }

      if (done_us - poll_us > spi_us)
        {
          spi_us = done_us - poll_us;
        }

      last_us = done_us;
#endif''')
        s=once(s,'  if (token == MMCSD_SPIDT_STARTBLKSNGL)', '''#ifdef CONFIG_ESP32P4_SDMMC
  esp32p4_io_end(0, token == MMCSD_SPIDT_STARTBLKSNGL ? 0 : -EIO,
                  token, polls, gap_us, spi_us);
#endif

  if (token == MMCSD_SPIDT_STARTBLKSNGL)''')
        return s
    return replace_fn(text,'static int mmcsd_recvblock(',recv)
