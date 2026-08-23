/**
 * @file toolchain_stubs.c
 * @brief 工具链兼容性 stub 函数
 *
 * Arduino 框架预编译库 (3.20017) 引用的部分符号在 GCC 14.2 工具链
 * 中不存在，提供 stub 以通过链接。
 *
 * stub 一律弱符号：espressif32@7.0.1 默认 GCC 8.4 工具链的 libgcc/
 * newlib 已提供同名强定义时弱 stub 自动让位，两代工具链均可链接
 * （开源可构建性，2026-08-23）。
 */

/* _Unwind_SetEnableExceptionFdeSorting —
 * ESP-IDF startup.c 中引用，GCC 14 的 libgcc 不再提供。
 * 功能：启用 FDE 异常排序，嵌入式环境不需要。 */
__attribute__((weak))
void _Unwind_SetEnableExceptionFdeSorting(int enable)
{
    (void)enable;
}

/* _cleanup_r —
 * newlib 的 reentrant cleanup，框架 libnewlib.a 引用但当前
 * 工具链的 newlib 版本未导出此符号。 */
__attribute__((weak))
void _cleanup_r(void *reent_ptr)
{
    (void)reent_ptr;
}
