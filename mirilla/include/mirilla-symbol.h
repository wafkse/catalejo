/* Seed-keyed aliases for Mirilla-owned C identifiers. */

#ifndef _MIRILLA_SYMBOL_H
#define _MIRILLA_SYMBOL_H

#if defined(MIRILLA_STEALTH_MODE)
#define STEALTH_SYMBOL(symbol, symbol_key) symbol_key
#include "mirilla-symbol.generated.h"
#else
#define STEALTH_SYMBOL(symbol, symbol_key) symbol
#endif

#endif
