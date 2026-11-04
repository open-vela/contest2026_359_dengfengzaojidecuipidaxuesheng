/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdatomic.h>
#define GLASS_STORE_URL_CAP 192
bool glass_store_http_base_valid(const char *url);
int glass_store_http_fetch(const char *base,const char *path,size_t maximum,
                           atomic_bool *cancel,char **out);
