/*
 * The "C" locale for libc++. Its locale.cpp (pulled in by iostreams in cspot
 * and bell) calls FreeBSD's *_l functions, which the title's libc lacks. The
 * console has no locales, so each one ignores the locale argument and uses
 * the plain function, and ctype answers for 7-bit ASCII.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <limits.h>
#include <locale.h>
#include <nl_types.h>
#include <runetype.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <wchar.h>
#include <wctype.h>
#include <xlocale.h>

/* ---- Locale objects ------------------------------------------------------------------------ */

static int c_locale_object;

locale_t newlocale(int mask, const char *name, locale_t base)
{
    (void)mask;
    (void)name;
    (void)base;
    return (locale_t)&c_locale_object;
}

int freelocale(locale_t locale)
{
    (void)locale;
    return 0;
}

struct lconv *localeconv_l(locale_t locale)
{
    (void)locale;
    static char empty[] = "";
    static char point[] = ".";
    static struct lconv c_lconv = {
        .decimal_point = point,
        .thousands_sep = empty,
        .grouping = empty,
        .int_curr_symbol = empty,
        .currency_symbol = empty,
        .mon_decimal_point = empty,
        .mon_thousands_sep = empty,
        .mon_grouping = empty,
        .positive_sign = empty,
        .negative_sign = empty,
        .int_frac_digits = CHAR_MAX,
        .frac_digits = CHAR_MAX,
        .p_cs_precedes = CHAR_MAX,
        .p_sep_by_space = CHAR_MAX,
        .n_cs_precedes = CHAR_MAX,
        .n_sep_by_space = CHAR_MAX,
        .p_sign_posn = CHAR_MAX,
        .n_sign_posn = CHAR_MAX,
        .int_p_cs_precedes = CHAR_MAX,
        .int_n_cs_precedes = CHAR_MAX,
        .int_p_sep_by_space = CHAR_MAX,
        .int_n_sep_by_space = CHAR_MAX,
        .int_p_sign_posn = CHAR_MAX,
        .int_n_sign_posn = CHAR_MAX,
    };
    return &c_lconv;
}

/* ---- ctype --------------------------------------------------------------------------------- */

static unsigned long ascii_type(int c)
{
    unsigned long type = 0;
    if (c < 0 || c > 127)
        return 0;
    if (c < 32 || c == 127)
        type |= _CTYPE_C;
    if (c == ' ' || c == '\t')
        type |= _CTYPE_B;
    if (c == ' ' || (c >= '\t' && c <= '\r'))
        type |= _CTYPE_S;
    if (c >= 32 && c < 127)
        type |= _CTYPE_R | 1UL << _CTYPE_SWS;
    if (c > 32 && c < 127)
        type |= _CTYPE_G;
    if (c >= '0' && c <= '9')
        type |= _CTYPE_D | _CTYPE_X | _CTYPE_N | (unsigned long)(c - '0');
    else if ((c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))
        type |= _CTYPE_X | (unsigned long)((c | 0x20) - 'a' + 10);
    if (c >= 'a' && c <= 'z')
        type |= _CTYPE_A | _CTYPE_L;
    else if (c >= 'A' && c <= 'Z')
        type |= _CTYPE_A | _CTYPE_U;
    else if (c > 32 && c < 127 && !(c >= '0' && c <= '9'))
        type |= _CTYPE_P;
    return type;
}

static _RuneLocale c_runes;
static int c_runes_ready;

_RuneLocale *__runes_for_locale(locale_t locale, int *mb_sole_byte)
{
    (void)locale;
    if (!c_runes_ready)
    {
        /* Idempotent: two threads racing here write the same values. */
        memcpy(c_runes.__magic, _RUNE_MAGIC_1, sizeof c_runes.__magic);
        strcpy(c_runes.__encoding, "NONE");
        c_runes.__invalid_rune = 0xFFFD;
        for (int c = 0; c < _CACHED_RUNES; c++)
        {
            c_runes.__runetype[c] = ascii_type(c);
            c_runes.__maplower[c] = c >= 'A' && c <= 'Z' ? c + 32 : c;
            c_runes.__mapupper[c] = c >= 'a' && c <= 'z' ? c - 32 : c;
        }
        c_runes_ready = 1;
    }
    if (mb_sole_byte != NULL)
        *mb_sole_byte = 1;
    return &c_runes;
}

unsigned long ___runetype_l(__ct_rune_t c, locale_t locale)
{
    (void)locale;
    return ascii_type(c);
}

__ct_rune_t ___tolower_l(__ct_rune_t c, locale_t locale)
{
    (void)locale;
    return c >= 'A' && c <= 'Z' ? c + 32 : c;
}

__ct_rune_t ___toupper_l(__ct_rune_t c, locale_t locale)
{
    (void)locale;
    return c >= 'a' && c <= 'z' ? c - 32 : c;
}

int ___mb_cur_max_l(locale_t locale)
{
    (void)locale;
    return 1;
}

int iswctype_l(wint_t c, wctype_t type, locale_t locale)
{
    (void)locale;
    return (ascii_type((int)c) & type) != 0;
}

/* ---- Message catalogues: none -------------------------------------------------------------- */

nl_catd catopen(const char *name, int flag)
{
    (void)name;
    (void)flag;
    return (nl_catd)-1;
}

char *catgets(nl_catd catalog, int set, int number, const char *fallback)
{
    (void)catalog;
    (void)set;
    (void)number;
    return (char *)fallback;
}

int catclose(nl_catd catalog)
{
    (void)catalog;
    return 0;
}

/* ---- Conversions: the plain functions ------------------------------------------------------ */

double strtod_l(const char *restrict text, char **restrict end, locale_t locale)
{
    (void)locale;
    return strtod(text, end);
}

float strtof_l(const char *restrict text, char **restrict end, locale_t locale)
{
    (void)locale;
    return strtof(text, end);
}

long double strtold_l(const char *restrict text, char **restrict end, locale_t locale)
{
    (void)locale;
    return strtold(text, end);
}

long long strtoll_l(const char *restrict text, char **restrict end, int base, locale_t locale)
{
    (void)locale;
    return strtoll(text, end, base);
}

unsigned long long strtoull_l(const char *restrict text, char **restrict end, int base,
                              locale_t locale)
{
    (void)locale;
    return strtoull(text, end, base);
}

int snprintf_l(char *restrict out, size_t size, locale_t locale, const char *restrict format,
               ...)
{
    (void)locale;
    va_list arguments;
    va_start(arguments, format);
    int result = vsnprintf(out, size, format, arguments);
    va_end(arguments);
    return result;
}

int asprintf_l(char **out, locale_t locale, const char *format, ...)
{
    (void)locale;
    va_list arguments;
    va_start(arguments, format);
    va_list copy;
    va_copy(copy, arguments);
    int length = vsnprintf(NULL, 0, format, copy);
    va_end(copy);
    *out = NULL;
    if (length >= 0)
    {
        *out = malloc((size_t)length + 1);
        if (*out != NULL)
            vsnprintf(*out, (size_t)length + 1, format, arguments);
        else
            length = -1;
    }
    va_end(arguments);
    return length;
}

int sscanf_l(const char *restrict text, locale_t locale, const char *restrict format, ...)
{
    (void)locale;
    va_list arguments;
    va_start(arguments, format);
    int result = vsscanf(text, format, arguments);
    va_end(arguments);
    return result;
}

size_t strftime_l(char *restrict out, size_t size, const char *restrict format,
                  const struct tm *restrict time, locale_t locale)
{
    (void)locale;
    return strftime(out, size, format, time);
}

int strcoll_l(const char *left, const char *right, locale_t locale)
{
    (void)locale;
    return strcmp(left, right);
}

size_t strxfrm_l(char *restrict out, const char *restrict text, size_t size, locale_t locale)
{
    (void)locale;
    size_t length = strlen(text);
    if (size > 0)
    {
        size_t copy = length < size - 1 ? length : size - 1;
        memcpy(out, text, copy);
        out[copy] = '\0';
    }
    return length;
}

int wcscoll_l(const wchar_t *left, const wchar_t *right, locale_t locale)
{
    (void)locale;
    return wcscmp(left, right);
}

size_t wcsxfrm_l(wchar_t *restrict out, const wchar_t *restrict text, size_t size,
                 locale_t locale)
{
    (void)locale;
    size_t length = wcslen(text);
    if (size > 0)
    {
        size_t copy = length < size - 1 ? length : size - 1;
        wmemcpy(out, text, copy);
        out[copy] = L'\0';
    }
    return length;
}

/* Multibyte: the C locale's single bytes. */

wint_t btowc_l(int c, locale_t locale)
{
    (void)locale;
    return c == EOF ? WEOF : (wint_t)(unsigned char)c;
}

int wctob_l(wint_t c, locale_t locale)
{
    (void)locale;
    return c < 256 ? (int)c : EOF;
}

size_t mbrtowc_l(wchar_t *restrict out, const char *restrict text, size_t size,
                 mbstate_t *restrict state, locale_t locale)
{
    (void)state;
    (void)locale;
    if (text == NULL)
        return 0;
    if (size == 0)
        return (size_t)-2;
    if (out != NULL)
        *out = (wchar_t)(unsigned char)*text;
    return *text != '\0' ? 1 : 0;
}

size_t mbrlen_l(const char *restrict text, size_t size, mbstate_t *restrict state,
                locale_t locale)
{
    return mbrtowc_l(NULL, text, size, state, locale);
}

int mbtowc_l(wchar_t *restrict out, const char *restrict text, size_t size, locale_t locale)
{
    if (text == NULL)
        return 0;
    size_t result = mbrtowc_l(out, text, size, NULL, locale);
    return result > 1 ? -1 : (int)result;
}

size_t wcrtomb_l(char *restrict out, wchar_t c, mbstate_t *restrict state, locale_t locale)
{
    (void)state;
    (void)locale;
    if (out == NULL)
        return 1;
    if ((unsigned)c > 255)
        return (size_t)-1;
    *out = (char)c;
    return 1;
}

size_t mbsnrtowcs_l(wchar_t *restrict out, const char **restrict text, size_t count,
                    size_t size, mbstate_t *restrict state, locale_t locale)
{
    (void)state;
    (void)locale;
    const char *source = *text;
    size_t done = 0;
    while (done < count && (out == NULL || done < size))
    {
        unsigned char c = (unsigned char)source[done];
        if (out != NULL)
            out[done] = c;
        if (c == '\0')
        {
            if (out != NULL)
                *text = NULL;
            return done;
        }
        done++;
    }
    if (out != NULL)
        *text = source + done;
    return done;
}

size_t mbsrtowcs_l(wchar_t *restrict out, const char **restrict text, size_t size,
                   mbstate_t *restrict state, locale_t locale)
{
    return mbsnrtowcs_l(out, text, (size_t)-1, size, state, locale);
}

size_t wcsnrtombs_l(char *restrict out, const wchar_t **restrict text, size_t count,
                    size_t size, mbstate_t *restrict state, locale_t locale)
{
    (void)state;
    (void)locale;
    const wchar_t *source = *text;
    size_t done = 0;
    while (done < count && (out == NULL || done < size))
    {
        wchar_t c = source[done];
        if ((unsigned)c > 255)
            return (size_t)-1;
        if (out != NULL)
            out[done] = (char)c;
        if (c == L'\0')
        {
            if (out != NULL)
                *text = NULL;
            return done;
        }
        done++;
    }
    if (out != NULL)
        *text = source + done;
    return done;
}
