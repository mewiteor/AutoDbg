#ifndef _DBL2STR_H_
#define _DBL2STR_H_

/* Ryu 双精度 → 字符串（实现是 C：`src/common/dbl2str.c`，见 Plan 阶段 5：**保持 C，仅移动**）。
 * P3.B 模块 12：LuaVariant 转成 C++ 后也被 C++ TU 引用 → 加 extern "C" 护卫，
 * 让 `dbl2str` 在所有 TU 里都是同一个 C 符号。 */
#ifdef __cplusplus
extern "C"
{
#endif

void dbl2str(char *buf, double _Value);

#ifdef __cplusplus
}
#endif

#endif // _DBL2STR_H_
