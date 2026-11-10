#ifndef _HIDE_H_
#define _HIDE_H_

#include <windows.h>
#include <stdbool.h>

/* P3.A.7：hide.c → hide.cpp 后，本头被两个 C++ TU 共享（bindings.cpp / hide.cpp）→
 * 必须加 extern "C" 护卫，否则 fix_peb/patch_antidebug_api 会因链接规范不一致而链接失败。 */
#ifdef __cplusplus
extern "C"
{
#endif

void fix_peb(HANDLE hProcess, BOOL setToNull);
void patch_antidebug_api(HANDLE hProcess);

#ifdef __cplusplus
}
#endif

#endif // _HIDE_H_
