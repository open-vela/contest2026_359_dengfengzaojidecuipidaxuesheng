"""Capture P4 fault registers before the existing ISR clears them and aborts."""
def apply(source):
    before='''    uint32_t intr_events = psram_ctrlr_ll_get_intr_raw(PSRAM_CTRLR_LL_MSPI_ID_SYSTEM);
    psram_ctrlr_ll_clear_intr(PSRAM_CTRLR_LL_MSPI_ID_SYSTEM, intr_events);

    ESP_DRAM_LOGE(TAG, "MSPI PSRAM error");'''
    after='''    uint32_t intr_events = psram_ctrlr_ll_get_intr_raw(PSRAM_CTRLR_LL_MSPI_ID_SYSTEM);
    /* These registers are cleared with the interrupt, so retain them first. */
    uint32_t axi_error = SPIMEM2.mem_axi_err_addr.val;
    uint32_t pms_error = SPIMEM2.mem_pms_reject.val;
    uint32_t ecc_error = SPIMEM2.mem_ecc_err_addr.val;
    psram_ctrlr_ll_clear_intr(PSRAM_CTRLR_LL_MSPI_ID_SYSTEM, intr_events);

    ESP_DRAM_LOGE(TAG, "MSPI PSRAM error events=0x%08" PRIx32 " axi=0x%08" PRIx32
                 " pms=0x%08" PRIx32 " ecc=0x%08" PRIx32,
                 intr_events, axi_error, pms_error, ecc_error);'''
    if source.count(before)!=1:raise ValueError('unsupported PSRAM error ISR')
    return source.replace(before,after)
