#!/usr/bin/env python3
"""Validate Hosted HCI payload lengths before the pinned SDK copies them."""
BEGIN='/* BEGIN OPENVELA HCI FRAME LENGTH */'
END='/* END OPENVELA HCI FRAME LENGTH */'
HELPER='''/* BEGIN OPENVELA HCI FRAME LENGTH */
/* Hosted provides the H4 type in the byte immediately before payload.
 * Only command and ACL packets are supported host-to-controller here. */
static int openvela_hci_payload_valid(uint8_t type, const uint8_t *payload,
                                     uint16_t length)
{
	if (!payload || !length || length == UINT16_MAX)
		return 0;
	if (type == 0x01) {
		/* Opcode (2), parameter length (1), parameters. */
		return length >= 3 && length == (unsigned)payload[2] + 3;
	}
	if (type == 0x02) {
		/* Handle/flags (2), little-endian data length (2), opaque ACL. */
		if (length < 4)
			return 0;
		unsigned bytes = (unsigned)payload[2] | ((unsigned)payload[3] << 8);
		return (unsigned)length == bytes + 4;
	}
	return 0;
}
/* END OPENVELA HCI FRAME LENGTH */
'''
CHECK='''	/* Avoid the H4 length increment wrapping and reject malformed payloads
	 * before the SDK allocates/copies a command or ACL buffer. */
	if (!payload || !payload_len || payload_len == UINT16_MAX ||
	    !openvela_hci_payload_valid(payload[-1], payload, payload_len)) {
		ESP_LOGW(TAG, "Invalid HCI frame length/type");
		return;
	}
'''


def patch_source(text):
    anchor='void process_hci_rx_pkt(uint8_t *payload, uint16_t payload_len)'
    log='\tESP_HEXLOGV("bt_rx", payload, payload_len);'
    if BEGIN in text:
        if text.count(HELPER)!=1 or text.count(CHECK+log)!=1:
            raise ValueError('Unexpected existing HCI length guard')
        return text
    if text.count(anchor)!=1 or text.count(log)!=1:
        raise ValueError('Pinned Hosted HCI receive anchor missing')
    return text.replace(anchor,HELPER+'\n'+anchor,1).replace(log,CHECK+log,1)


def unpatch_source(text):
    if BEGIN not in text:
        return text
    patch_source(text)  # Refuse partial or unexpected edits.
    return text.replace(HELPER+'\n','',1).replace(CHECK,'',1)
