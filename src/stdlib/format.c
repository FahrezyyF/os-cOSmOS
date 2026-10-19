#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdarg.h>
#include "header/stdlib/format.h"

struct FormatOutput {
    char   *buf;
    size_t size;
    size_t len;
};

struct FormatSpec {
    bool left_align;
    bool zero_pad;
    bool plus_sign;
    bool space_sign;
    int  width;
    int  precision;
};

static void emit(struct FormatOutput *out, char c) {
    if (out->len + 1 < out->size)
        out->buf[out->len] = c;
    out->len++;
}

static void emit_repeat(struct FormatOutput *out, char c, int count) {
    for (int i = 0; i < count; i++)
        emit(out, c);
}

static void emit_string(struct FormatOutput *out, const char *s, const struct FormatSpec *spec) {
    if (s == NULL)
        s = "(null)";
    int len = 0;
    while (s[len] != '\0' && (spec->precision < 0 || len < spec->precision))
        len++;
    int pad = spec->width > len ? spec->width - len : 0;
    if (!spec->left_align)
        emit_repeat(out, ' ', pad);
    for (int i = 0; i < len; i++)
        emit(out, s[i]);
    if (spec->left_align)
        emit_repeat(out, ' ', pad);
}

static void emit_number(struct FormatOutput *out, uint32_t value, bool negative, uint32_t base,
                        bool uppercase, const char *prefix, const struct FormatSpec *spec) {
    const char *digits = uppercase ? "0123456789ABCDEF" : "0123456789abcdef";
    char tmp[16];
    int  ndigits = 0;
    if (value == 0 && spec->precision != 0)
        tmp[ndigits++] = '0';
    while (value != 0) {
        tmp[ndigits++] = digits[value % base];
        value /= base;
    }

    char sign = 0;
    if (negative)
        sign = '-';
    else if (spec->plus_sign)
        sign = '+';
    else if (spec->space_sign)
        sign = ' ';

    int prefix_len = 0;
    while (prefix != NULL && prefix[prefix_len] != '\0')
        prefix_len++;

    int zeros = spec->precision > ndigits ? spec->precision - ndigits : 0;
    int total = ndigits + zeros + prefix_len + (sign ? 1 : 0);
    int pad   = spec->width > total ? spec->width - total : 0;

    // '0' flag is ignored when a precision is given (C standard behaviour)
    bool pad_with_zero = spec->zero_pad && !spec->left_align && spec->precision < 0;
    if (pad_with_zero) {
        zeros += pad;
        pad = 0;
    }

    if (!spec->left_align)
        emit_repeat(out, ' ', pad);
    if (sign)
        emit(out, sign);
    for (int i = 0; i < prefix_len; i++)
        emit(out, prefix[i]);
    emit_repeat(out, '0', zeros);
    while (ndigits > 0)
        emit(out, tmp[--ndigits]);
    if (spec->left_align)
        emit_repeat(out, ' ', pad);
}

int vsnprintf(char *buf, size_t size, const char *fmt, va_list ap) {
    struct FormatOutput out = {.buf = buf, .size = size, .len = 0};

    for (const char *p = fmt; *p != '\0'; p++) {
        if (*p != '%') {
            emit(&out, *p);
            continue;
        }
        p++;

        struct FormatSpec spec = {.width = 0, .precision = -1};
        for (;; p++) {
            if (*p == '-')      spec.left_align = true;
            else if (*p == '0') spec.zero_pad   = true;
            else if (*p == '+') spec.plus_sign  = true;
            else if (*p == ' ') spec.space_sign = true;
            else break;
        }

        if (*p == '*') {
            spec.width = va_arg(ap, int);
            if (spec.width < 0) {
                spec.left_align = true;
                spec.width      = -spec.width;
            }
            p++;
        } else {
            while (*p >= '0' && *p <= '9')
                spec.width = spec.width * 10 + (*p++ - '0');
        }

        if (*p == '.') {
            p++;
            spec.precision = 0;
            if (*p == '*') {
                spec.precision = va_arg(ap, int);
                if (spec.precision < 0)
                    spec.precision = -1;
                p++;
            } else {
                while (*p >= '0' && *p <= '9')
                    spec.precision = spec.precision * 10 + (*p++ - '0');
            }
        }

        // Length modifiers: every supported integer is at most 32-bit on i386,
        // so h/hh only truncate and l/z are accepted and ignored.
        int short_level = 0;
        while (*p == 'h' || *p == 'l' || *p == 'z') {
            if (*p == 'h')
                short_level++;
            p++;
        }

        switch (*p) {
            case 'd':
            case 'i': {
                int32_t v = va_arg(ap, int32_t);
                if (short_level == 1) v = (int16_t) v;
                if (short_level >= 2) v = (int8_t) v;
                bool negative = v < 0;
                uint32_t magnitude = negative ? (uint32_t) 0 - (uint32_t) v : (uint32_t) v;
                emit_number(&out, magnitude, negative, 10, false, NULL, &spec);
                break;
            }
            case 'u':
            case 'x':
            case 'X':
            case 'o': {
                uint32_t v = va_arg(ap, uint32_t);
                if (short_level == 1) v = (uint16_t) v;
                if (short_level >= 2) v = (uint8_t) v;
                uint32_t base = (*p == 'u') ? 10 : (*p == 'o') ? 8 : 16;
                emit_number(&out, v, false, base, *p == 'X', NULL, &spec);
                break;
            }
            case 'p': {
                uintptr_t v = (uintptr_t) va_arg(ap, void*);
                struct FormatSpec pspec = spec;
                if (pspec.precision < 0)
                    pspec.precision = 8;
                emit_number(&out, (uint32_t) v, false, 16, false, "0x", &pspec);
                break;
            }
            case 'c': {
                char c = (char) va_arg(ap, int);
                int pad = spec.width > 1 ? spec.width - 1 : 0;
                if (!spec.left_align)
                    emit_repeat(&out, ' ', pad);
                emit(&out, c);
                if (spec.left_align)
                    emit_repeat(&out, ' ', pad);
                break;
            }
            case 's':
                emit_string(&out, va_arg(ap, const char*), &spec);
                break;
            case '%':
                emit(&out, '%');
                break;
            case '\0':
                p--; // dangling '%' at the end of the format string
                break;
            default:
                emit(&out, '%');
                emit(&out, *p);
                break;
        }
    }

    if (size > 0)
        buf[out.len < size ? out.len : size - 1] = '\0';
    return (int) out.len;
}

int snprintf(char *buf, size_t size, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int ret = vsnprintf(buf, size, fmt, ap);
    va_end(ap);
    return ret;
}
