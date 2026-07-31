/**
 * @file vsnprintf_s_compat.c
 * @brief win10 预编译 libmbedcrypto.a (MSVC 构建) 引用了 vsnprintf_s;
 *        MinGW UCRT 只在 stdio_s.h 内联提供, 链接期无外部符号。
 *        此处手工声明避免与 <stdio.h> 的内联定义冲突。host E2E only。
 */
#include <stddef.h>
#include <stdarg.h>

int vsnprintf(char *s, size_t n, const char *fmt, va_list ap);

int vsnprintf_s(char *s, size_t n, size_t max, const char *fmt, va_list ap)
{
    (void)n;
    return vsnprintf(s, max, fmt, ap);
}
