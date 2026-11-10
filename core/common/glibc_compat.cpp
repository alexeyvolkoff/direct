/**
 * Glibc 2.38+ compatibility wrapper.
 * Newer compilers/headers generate references to __isoc23_strtoul / __isoc23_strtol
 * which are not available in older Glibc versions (like 2.31 in Ubuntu 20.04).
 *
 * IMPORTANT: do NOT call strtoul()/strtol() through <cstdlib> here: on glibc >= 2.38
 * the headers redirect them back to __isoc23_*, which would make these wrappers
 * call themselves (infinite tail-call recursion, 100% CPU busy loop). The asm
 * labels below bind directly to the classic libc symbols, bypassing any header
 * redirection.
 */

extern "C" {

unsigned long int __glibc_compat_strtoul(const char *nptr, char **endptr, int base) __asm__("strtoul");
long int __glibc_compat_strtol(const char *nptr, char **endptr, int base) __asm__("strtol");

unsigned long int __isoc23_strtoul(const char *nptr, char **endptr, int base)
{
    return __glibc_compat_strtoul(nptr, endptr, base);
}

long int __isoc23_strtol(const char *nptr, char **endptr, int base)
{
    return __glibc_compat_strtol(nptr, endptr, base);
}

} // extern "C"
