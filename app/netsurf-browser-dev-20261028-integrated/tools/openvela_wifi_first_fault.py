"""P4 late-command guard and bounded first-fault RAM recorder.

Apply after openvela_wifi_tx_errors. No deployment or device operations.
"""
from openvela_wifi_tx_errors import span, once

TYPES = r'''
/* Fixed RAM recorder for this singleton SDMMC controller.  valid is written
 * last: 0 = recording, 1 = freezing, 2 = complete.  Only boot clears it.
 * Descriptor words are CPU cache views; recording never invalidates caches.
 */

#define SDIO_TRACE_SCHED_COUNT 16
#define SDIO_TRACE_PHASE_COUNT 6

enum sdio_trace_phase_e
{
  SDIO_TRACE_ARM,
  SDIO_TRACE_DMA_BEGIN,
  SDIO_TRACE_DMA_READY,
  SDIO_TRACE_SUBMIT,
  SDIO_TRACE_IRQ,
  SDIO_TRACE_RETURN
};

enum sdio_trace_reason_e
{
  SDIO_TRACE_TIMEOUT = 1,
  SDIO_TRACE_LATE_SUBMIT,
  SDIO_TRACE_COMMAND_BUSY,
  SDIO_TRACE_DMA_SETUP,
  SDIO_TRACE_IRQ_ERROR,
  SDIO_TRACE_CACHE_ERROR,
  SDIO_TRACE_WDOG_ERROR
};

struct sdio_sched_trace_s
{
  uint64_t us;
  int32_t pid;
  uint8_t cpu;
  uint8_t priority;
  uint8_t resumed;
  uint8_t state;
};

struct sdio_first_fault_s
{
  uint32_t magic;
  uint32_t version;
  uint32_t bytes;
  uint32_t valid;
  uint32_t sequence;
  uint32_t active;
  int32_t owner_pid;
  uint32_t owner_cpu;
  uint64_t phase_us[SDIO_TRACE_PHASE_COUNT];
  uint32_t phase_tick[SDIO_TRACE_PHASE_COUNT];
  uint32_t phase_cpu[SDIO_TRACE_PHASE_COUNT];
  uint64_t off_since_us;
  uint64_t off_total_us;
  uint64_t off_max_us;
  uint32_t owner_suspended;
  uint32_t sched_count;
  struct sdio_sched_trace_s sched[SDIO_TRACE_SCHED_COUNT];
  uint32_t reason;
  int32_t error;
  uint64_t fault_us;
  uint32_t fault_tick;
  uint32_t fault_cpu;
  int32_t fault_pid;
  uint32_t requested_cmd;
  uint32_t requested_arg;
  uint32_t submitted;
  uint32_t deadline;
  uint32_t waitevents;
  uint32_t wkupevent;
  uint32_t remaining;
  uint32_t buffer;
  uint32_t bytes_requested;
  uint32_t write;
  uint32_t rintsts;
  uint32_t idsts;
  uint32_t status;
  uint32_t cmd;
  uint32_t cmdarg;
  uint32_t ctrl;
  uint32_t bmod;
  uint32_t intmask;
  uint32_t idinten;
  uint32_t dbaddr;
  uint32_t dscaddr;
  uint32_t bufaddr;
  uint32_t descriptor_cpu[4];
};
'''

HELPERS = r'''
/* This symbol is intentionally retained for ELF-assisted RAM extraction.
 * The live record itself freezes; no large history copy is needed in ISR.
 */

static volatile struct sdio_first_fault_s g_sdio_first;
static spinlock_t g_sdio_trace_lock = SP_UNLOCKED;

static void esp32p4_trace_phase(enum sdio_trace_phase_e phase)
{
  irqstate_t flags = enter_critical_section();

  if (g_sdio_first.active && g_sdio_first.valid == 0)
    {
      /* Keep the first IRQ entry, rather than a later interrupt. */

      if (phase != SDIO_TRACE_IRQ || g_sdio_first.phase_us[phase] == 0)
        {
          g_sdio_first.phase_us[phase] = esp_timer_get_time();
          g_sdio_first.phase_tick[phase] = clock_systime_ticks();
          g_sdio_first.phase_cpu[phase] = up_cpu_index();
        }

      if (phase == SDIO_TRACE_SUBMIT)
        {
          g_sdio_first.submitted = 1;
        }
      else if (phase == SDIO_TRACE_RETURN)
        {
          g_sdio_first.active = 0;
        }
    }

  leave_critical_section(flags);
}

static void esp32p4_trace_begin(void)
{
  irqstate_t flags = enter_critical_section();
  unsigned int i;

  if (g_sdio_first.valid == 0)
    {
      g_sdio_first.magic = 0x53444631;
      g_sdio_first.version = 1;
      g_sdio_first.bytes = sizeof(g_sdio_first);
      g_sdio_first.sequence++;
      g_sdio_first.active = 1;
      g_sdio_first.owner_pid = nxsched_getpid();
      g_sdio_first.owner_cpu = up_cpu_index();
      g_sdio_first.owner_suspended = 0;
      g_sdio_first.off_since_us = 0;
      g_sdio_first.off_total_us = 0;
      g_sdio_first.off_max_us = 0;
      g_sdio_first.sched_count = 0;
      g_sdio_first.requested_cmd = 0;
      g_sdio_first.requested_arg = 0;
      g_sdio_first.submitted = 0;
      for (i = 0; i < SDIO_TRACE_PHASE_COUNT; i++)
        {
          g_sdio_first.phase_us[i] = 0;
          g_sdio_first.phase_tick[i] = 0;
          g_sdio_first.phase_cpu[i] = 0;
        }

      g_sdio_first.phase_us[SDIO_TRACE_ARM] = esp_timer_get_time();
      g_sdio_first.phase_tick[SDIO_TRACE_ARM] = clock_systime_ticks();
      g_sdio_first.phase_cpu[SDIO_TRACE_ARM] = up_cpu_index();
    }

  leave_critical_section(flags);
}

/* Called at the real scheduler suspend/resume hooks, not inferred from
 * elapsed I/O time.  No allocation, logging, task lookup or peripheral I/O.
 */

void esp32p4_sdio_trace_sched(int pid, int priority, int state, bool resumed)
{
  irqstate_t flags;
  uint64_t now;
  uint64_t gap;
  unsigned int index;

  flags = enter_critical_section();
  if (!g_sdio_first.active || g_sdio_first.valid != 0)
    {
      leave_critical_section(flags);
      return;
    }

  now = esp_timer_get_time();
  index = g_sdio_first.sched_count++ % SDIO_TRACE_SCHED_COUNT;
  g_sdio_first.sched[index].us = now;
  g_sdio_first.sched[index].pid = pid;
  g_sdio_first.sched[index].cpu = up_cpu_index();
  g_sdio_first.sched[index].priority = priority;
  g_sdio_first.sched[index].state = state;
  g_sdio_first.sched[index].resumed = resumed;
  if (pid == g_sdio_first.owner_pid)
    {
      if (!resumed && !g_sdio_first.owner_suspended)
        {
          g_sdio_first.owner_suspended = 1;
          g_sdio_first.off_since_us = now;
        }
      else if (resumed && g_sdio_first.owner_suspended)
        {
          gap = now - g_sdio_first.off_since_us;
          g_sdio_first.off_total_us += gap;
          if (gap > g_sdio_first.off_max_us)
            {
              g_sdio_first.off_max_us = gap;
            }

          g_sdio_first.owner_suspended = 0;
        }
    }

  leave_critical_section(flags);
}

static void esp32p4_trace_request(uint32_t cmd, uint32_t arg)
{
  irqstate_t flags = enter_critical_section();

  if (g_sdio_first.active && g_sdio_first.valid == 0)
    {
      g_sdio_first.requested_cmd = cmd;
      g_sdio_first.requested_arg = arg;
    }

  leave_critical_section(flags);
}

static void esp32p4_trace_fault(struct esp32p4_dev_s *priv,
                                enum sdio_trace_reason_e reason, int error)
{
  irqstate_t flags = enter_critical_section();

  if (g_sdio_first.active && g_sdio_first.valid == 0)
    {
      g_sdio_first.valid = 1;
      g_sdio_first.reason = reason;
      g_sdio_first.error = error;
      g_sdio_first.fault_us = esp_timer_get_time();
      g_sdio_first.fault_tick = clock_systime_ticks();
      g_sdio_first.fault_cpu = up_cpu_index();
      g_sdio_first.fault_pid = nxsched_getpid();
      g_sdio_first.deadline = priv->submit_deadline;
      g_sdio_first.waitevents = priv->waitevents;
      g_sdio_first.wkupevent = priv->wkupevent;
      g_sdio_first.remaining = priv->remaining;
      g_sdio_first.buffer = (uintptr_t)priv->buffer;
      g_sdio_first.bytes_requested = priv->remaining;
      g_sdio_first.write = priv->wrdir;
      g_sdio_first.rintsts = esp32p4_getreg(ESP32P4_SDMMC_RINTSTS);
      g_sdio_first.idsts = esp32p4_getreg(ESP32P4_SDMMC_IDSTS);
      g_sdio_first.status = esp32p4_getreg(ESP32P4_SDMMC_STATUS);
      g_sdio_first.cmd = esp32p4_getreg(ESP32P4_SDMMC_CMD);
      g_sdio_first.cmdarg = esp32p4_getreg(ESP32P4_SDMMC_CMDARG);
      g_sdio_first.ctrl = esp32p4_getreg(ESP32P4_SDMMC_CTRL);
      g_sdio_first.bmod = esp32p4_getreg(ESP32P4_SDMMC_BMOD);
      g_sdio_first.intmask = esp32p4_getreg(ESP32P4_SDMMC_INTMASK);
      g_sdio_first.idinten = esp32p4_getreg(ESP32P4_SDMMC_IDINTEN);
      g_sdio_first.dbaddr = esp32p4_getreg(ESP32P4_SDMMC_DBADDR);
      g_sdio_first.dscaddr = esp32p4_getreg(ESP32P4_SDMMC_DSCADDR);
      g_sdio_first.bufaddr = esp32p4_getreg(ESP32P4_SDMMC_BUFADDR);
#ifdef CONFIG_ESP32P4_SDMMC_DMA
      g_sdio_first.descriptor_cpu[0] = priv->dma_desc[0].des0;
      g_sdio_first.descriptor_cpu[1] = priv->dma_desc[0].des1;
      g_sdio_first.descriptor_cpu[2] = priv->dma_desc[0].des2;
      g_sdio_first.descriptor_cpu[3] = priv->dma_desc[0].des3;
#endif

      /* The private trace lock serializes concurrent producers.
       * valid=2 is the publication marker for an external RAM reader.
       */

      g_sdio_first.valid = 2;
    }

  leave_critical_section(flags);
}

static bool esp32p4_submit_expired(struct esp32p4_dev_s *priv)
{
  return priv->submit_expired ||
         (priv->submit_timed &&
          clock_compare(priv->submit_deadline, clock_systime_ticks()));
}
'''

# Keep diagnostics independent of the scheduler's global critical lock.
# No helper nests this lock or invokes scheduler/lock tracing callbacks.
HELPERS = HELPERS.replace('enter_critical_section()',
    'spin_lock_irqsave_notrace(&g_sdio_trace_lock)').replace(
    'leave_critical_section(flags)',
    'spin_unlock_irqrestore_notrace(&g_sdio_trace_lock, flags)')

CIU = r'''static int esp32p4_ciu_sendcmd(struct esp32p4_dev_s *priv,
                              uint32_t cmd, uint32_t arg)
{
  clock_t watchtime = clock_systime_ticks();
  irqstate_t flags;
  bool data = (cmd & SDMMC_CMD_DATAXFREXPTD) != 0;

  for (; ; )
    {
      flags = enter_critical_section();
      if (data && esp32p4_submit_expired(priv))
        {
          esp32p4_trace_fault(priv, SDIO_TRACE_LATE_SUBMIT, -ETIMEDOUT);
          leave_critical_section(flags);
          return 1;
        }

      if ((esp32p4_getreg(ESP32P4_SDMMC_CMD) & SDMMC_CMD_STARTCMD) == 0)
        {
          /* The deadline check and the two command writes are atomic with
           * respect to timer expiry and task scheduling on both CPUs.
           * No cache sync, polling or blocking call is inside this span.
           */

          if (data)
            {
              esp32p4_trace_phase(SDIO_TRACE_SUBMIT);
            }

          esp32p4_putreg(arg, ESP32P4_SDMMC_CMDARG);
          esp32p4_putreg(cmd | SDMMC_CMD_STARTCMD | SDMMC_CMD_USE_HOLE,
                         ESP32P4_SDMMC_CMD);
          leave_critical_section(flags);
          return 0;
        }

      leave_critical_section(flags);
      if (clock_systime_ticks() - watchtime > SDCARD_CMDTIMEOUT)
        {
          if (data)
            {
              esp32p4_trace_fault(priv, SDIO_TRACE_COMMAND_BUSY, -ETIMEDOUT);
            }

          return 1;
        }
    }
}'''


def replace_fn(text, sig, change):
    a, b = span(text, sig)
    return text[:a] + change(text[a:b]) + text[b:]


def patch_p4(text):
    text = once(text, '#include <nuttx/clock.h>', '#include <nuttx/clock.h>\n#include <nuttx/sched.h>\n#include <nuttx/spinlock.h>')
    text = once(text, '#include "esp_cache.h"', '#include "esp_cache.h"\n#include "esp_timer.h"')
    # This port's verified configuration always uses IDMAC.
    text = once(text, '/* This structure defines the state', TYPES.lstrip() + '\n/* This structure defines the state')
    text = once(text, '  /* Callback support */', '''  clock_t submit_deadline;
  bool submit_timed;
  bool submit_expired;

  /* Callback support */''')
    a, b = span(text, 'static int esp32p4_ciu_sendcmd(')
    text = text[:a] + HELPERS.lstrip() + '\n' + CIU + text[b:]
    text = text.replace('esp32p4_ciu_sendcmd(cmd, 0)', 'esp32p4_ciu_sendcmd(priv, cmd, 0)')
    text = text.replace('esp32p4_ciu_sendcmd(regval, arg)', 'esp32p4_ciu_sendcmd(priv, regval, arg)')
    def arm(s):
        s = once(s, '  uint32_t waitmask;', '  uint32_t waitmask;\n  irqstate_t flags;')
        s = once(s, '  /* Discard the previous failure cause', '''  flags = enter_critical_section();
  priv->submit_timed = false;
  priv->submit_expired = false;
  priv->submit_deadline = 0;
  esp32p4_trace_begin();

  /* Discard the previous failure cause''')
        s = once(s, '          priv->wkupevent = SDIOWAIT_TIMEOUT;\n          return;', '''          priv->wkupevent = SDIOWAIT_TIMEOUT;
          priv->submit_expired = true;
          esp32p4_trace_fault(priv, SDIO_TRACE_TIMEOUT, -ETIMEDOUT);
          leave_critical_section(flags);
          return;''')
        s = once(s, '          mcerr("ERROR: wd_start failed: %d\\n", ret);', '''          priv->submit_expired = true;
          priv->wkupevent = SDIOWAIT_ERROR;
          esp32p4_trace_fault(priv, SDIO_TRACE_WDOG_ERROR, ret);
          nxsem_post(&priv->waitsem);''')
        s = once(s, '    }\n}', '''      else
        {
          priv->submit_deadline = priv->waitwdog.expired;
          priv->submit_timed = true;
        }
    }

  leave_critical_section(flags);
}''')
        return s
    text = replace_fn(text, 'static void esp32p4_waitenable(', arm)
    def timeout(s):
        s = once(s, '  /* There is always race conditions',
            '  irqstate_t flags = enter_critical_section();\n\n  /* There is always race conditions')
        s = once(s,
        '      esp32p4_endwait(priv, SDIOWAIT_TIMEOUT);', '''      priv->submit_expired = true;
      esp32p4_trace_fault(priv, SDIO_TRACE_TIMEOUT, -ETIMEDOUT);
      esp32p4_endwait(priv, SDIOWAIT_TIMEOUT);''')
        s = once(s, '      mcerr("Timeout: remaining: %d\\n", priv->remaining);\n', '')
        return s[:-1] + '\n  leave_critical_section(flags);\n}'
    text = replace_fn(text, 'static void esp32p4_eventtimeout(', timeout)
    def send(s):
        return once(s, '  /* Write the SD card CMD */', '''  if ((regval & SDMMC_CMD_DATAXFREXPTD) != 0)
    {
      esp32p4_trace_request(cmd, arg);
    }

  /* Write the SD card CMD */''')
    text = replace_fn(text, 'static int esp32p4_sendcmd(', send)
    for sig in ('static int esp32p4_dmarecvsetup(', 'static int esp32p4_dmasendsetup('):
        def setup(s):
            s = once(s, '  ret = esp32p4_fill_dma_desc(priv);', '''  esp32p4_trace_phase(SDIO_TRACE_DMA_BEGIN);
  ret = esp32p4_fill_dma_desc(priv);''')
            s = once(s, '      return ret;', '''      esp32p4_trace_fault(priv, SDIO_TRACE_DMA_SETUP, ret);
      return ret;''')
            s = once(s, '  return OK;', '  esp32p4_trace_phase(SDIO_TRACE_DMA_READY);\n  return OK;')
            return s
        text = replace_fn(text, sig, setup)
    def irq(s):
        s = once(s, '      /* Clear pending status */', '''      esp32p4_trace_phase(SDIO_TRACE_IRQ);
      if ((enabled & (priv->xfrmask | priv->waitmask) &
           (SDMMC_INT_DCRC | SDMMC_INT_DRTO | SDMMC_INT_HTO |
            SDMMC_INT_FRUN | SDMMC_INT_HLE | SDMMC_INT_SBE |
            SDMMC_INT_EBE | SDCARD_INT_RESPERR)) != 0)
        {
          esp32p4_trace_fault(priv, SDIO_TRACE_IRQ_ERROR, -EIO);
        }

      /* Clear pending status */''')
        return once(s, '      priv->lastidsts = pending;', '''      esp32p4_trace_phase(SDIO_TRACE_IRQ);
      esp32p4_trace_fault(priv, SDIO_TRACE_IRQ_ERROR, -EIO);
      priv->lastidsts = pending;''')
    text = replace_fn(text, 'static int esp32p4_interrupt(', irq)
    def wait(s):
        s = once(s, '          wkupevent |= SDIOWAIT_ERROR;', '''          esp32p4_trace_fault(priv, SDIO_TRACE_CACHE_ERROR, syncret);
          wkupevent |= SDIOWAIT_ERROR;''')
        return once(s, '  return wkupevent;', '''  esp32p4_trace_phase(SDIO_TRACE_RETURN);
  return wkupevent;''')
    text = replace_fn(text, 'static sdio_eventset_t esp32p4_eventwait(', wait)
    return text


def patch_sched(text):
    text = once(text, '#include <nuttx/sched_note.h>', '''#include <nuttx/sched_note.h>

#ifdef CONFIG_ESP32P4_SDMMC
void esp32p4_sdio_trace_sched(int pid, int priority, int state,
                              bool resumed);
#endif''')
    for name, resumed in [('suspend', 'false'), ('resume', 'true')]:
        sig = 'void nxsched_' + name + '_scheduler(FAR struct tcb_s *tcb)\n{'
        text = once(text, sig, sig + '''
#ifdef CONFIG_ESP32P4_SDMMC
  esp32p4_sdio_trace_sched(tcb->pid, tcb->sched_priority,
                           tcb->task_state, ''' + resumed + ''');
#endif
''')
    return text
