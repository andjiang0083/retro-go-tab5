/* Modifications copyright (c) 2026 andjiang0083
 * The upstream file of this name comes from retro-go
 * (https://github.com/ducalex/retro-go, GPLv2); this port keeps it
 * under GPL-2.0-only. See CREDITS.md in the repository root.
 */
#pragma once

#include <string.h>
#include <stdbool.h>

#define _(String) rg_gettext(String)

typedef enum
{
    RG_LANG_EN = 0,
    RG_LANG_FR,
    RG_LANG_ZH,
  //RG_LANG_ES,

    RG_LANG_MAX
} rg_language_t;

// Lookup function
const char *rg_gettext(const char *msg);
int rg_localization_get_language_id(void);
bool rg_localization_set_language_id(int language_id);
const char *rg_localization_get_language_name(int language_id);
