/* SPDX-License-Identifier: Apache-2.0 */
#ifndef GLASS_I18N_H
#define GLASS_I18N_H
#include <stdbool.h>

enum glass_language { GLASS_LANGUAGE_ZH_CN, GLASS_LANGUAGE_EN, GLASS_LANGUAGE_COUNT };
/* UI-thread API. Only preferences I/O runs on a worker. User text must never
 * be passed through this lookup; mark application-owned UI strings explicitly. */
const char *glass_tr(const char *source);
enum glass_language glass_i18n_language(void);
const char *glass_i18n_code(void);
const char *glass_i18n_name(unsigned language);
void glass_i18n_start(void);
bool glass_i18n_poll(void);
int glass_i18n_set_language(unsigned language);
/* Consumed once: 1 saved, negative errno on failure, 0 no notification. */
int glass_i18n_save_status(void);
#endif
