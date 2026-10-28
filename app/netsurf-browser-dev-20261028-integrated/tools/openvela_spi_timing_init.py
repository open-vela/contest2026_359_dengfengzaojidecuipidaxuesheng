"""Initialize the pinned P4 SPI HAL output before partial clock calculation.

This patch addresses an observed indeterminate field, not a claim that it
caused the recorded SD token timeout. It does not deploy or reset hardware.
"""


def patch_spi_timing(text):
    before = '  spi_hal_timing_conf_t temp_timing_conf;'
    after = '''  spi_hal_timing_conf_t temp_timing_conf =
    {
      .rx_sample_point = SPI_SAMPLING_POINT_PHASE_0
    };'''
    if text.count(before) != 1:
        raise ValueError('Unsupported SPI source: expected one clock local')
    return text.replace(before, after)
