"""Avoid expiring an SD token wait using a sample taken before a task stall.

Pure source transformation; does not deploy, remount, reset, or retry commands.
This fixes a demonstrated scheduling window, not an attribution of a past fault.
"""


def patch_sd_poll_deadline(text):
    before = '''      token = SPI_SEND(spi, 0xff);
      elapsed = ELAPSED_TIME(start);
'''
    after = '''      /* Sample the deadline before clocking the card.  A task can be
       * preempted after SPI_SEND returns 0xff; testing a later timestamp
       * against that stale token would skip the final card-ready check.
       */

      elapsed = ELAPSED_TIME(start);
      token = SPI_SEND(spi, 0xff);
'''
    if text.count(before) != 1:
        raise ValueError('Unsupported mmcsd source: expected one token loop')
    return text.replace(before, after)
