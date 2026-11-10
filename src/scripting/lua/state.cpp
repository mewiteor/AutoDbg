/* =============================================================================
 * src/scripting/lua/state.cpp —— Lua 状态与脚本加载（P4.1，从 bindings.cpp 原样搬出）
 *
 * 内容：`g_wfilepath` 定义 / `generate_templete()` / `my_loadfile()` / `run_script_file()`。
 * 与旧实现的差异仅两处（其余逐字）：
 *   ① `_tmain` 里「建状态 → 注册 → 烘焙库 → 加载 → 跑脚本 → 打印错误 → 关闭」这段收进
 *      `run_script_file()`（逐字搬入，只是不再嵌在入口函数里）；`_tmain` 改为调它。
 *   ② 文件头与包含调整（`datas.h` 的 `all_bytecodes`、`registry.hpp`、`core/util.hpp`）。
 * ========================================================================== */
#include "state.hpp"

#include "bindings.hpp" /* `HWB_SIZE_STR`（模板文本用）与 `lua_*` 声明 */
#include "registry.hpp" /* register_lua_api（P4.2 搬出） */

#include "core/types.hpp" /* MAX_FILEPATH / TCHAR（类型统一后只有这一份） */
#include "core/util.hpp"

#include "common/handle.hpp" /* P6.1：HANDLE RAII */

#include <intrin.h> /* ZeroMem/CopyMem 宏（__stosb/__movsb）——`RemoveAllFiles` 用到 */
#include <shlwapi.h>
#include <strsafe.h>

/* 选择性 using：旧调用点（`Error*` / `utf16_to_utf8` / `utf8_to_utf16_n` / 宏 `E`/`alloc_`）
 * 原先来自 legacy_lib.h（C 版，全局命名空间），现在分属 `autodbg::core`（Plan 5.7）。 */
using autodbg::core::Error;
using autodbg::core::ErrorF;
using autodbg::core::ErrorN;
using autodbg::core::ErrorS;
using autodbg::core::utf8_to_utf16;
using autodbg::core::utf16_to_utf8;
using autodbg::core::utf8_to_utf16_n;
using autodbg::common::unique_handle; /* P6.1 */

/* extern "C" 花括号形式 = 定义（Q7：脚本路径属 Lua 层状态，不进会话） */
extern "C"
{
LPCTSTR g_wfilepath;
}

extern "C"
{
#include <datas.h> /* all_bytecodes：烘焙进 exe 的 Lua 库 */
}

void generate_templete()
{
    TCHAR path[MAX_FILEPATH];
    if (!g_wfilepath)
        return;
    StringCchCopy(path, _countof(path), g_wfilepath);
    LPTSTR s = PathFindFileName(path);
    StringCchCopy(s, _countof(path) - (s - path), TEXT("cdebugger.d.lua"));
    FILE *f = _tfopen(path, TEXT("w"));
    if (!f)
        return;

    fputs("---@meta\n"
          "\n",
          f);

    fputs("---@class JsonxLib\n"
          "---@field encode fun(value: any):string\n"
          "---@field decode fun(text:string):any\n"
          "\n"
          "---@type JsonxLib\n"
          "jsonx = nil\n"
          "\n",
          f);

    fputs("---@type fun(value:any, options?:table):string\n"
          "inspect = nil\n"
          "\n",
          f);

    fputs("---@class MosesOperator\n"
          "---@field add fun(a:number, b:number):number\n"
          "---@field sub fun(a:number, b:number):number\n"
          "---@field mul fun(a:number, b:number):number\n"
          "---@field div fun(a:number, b:number):number\n"
          "---@field mod fun(a:number, b:number):number\n"
          "---@field exp fun(a:number, b:number):number\n"
          "---@field pow fun(a:number, b:number):number\n"
          "---@field unm fun(a:number):number\n"
          "---@field neg fun(a:number):number\n"
          "---@field floordiv fun(a:number, b:number):number\n"
          "---@field intdiv fun(a:number, b:number):number\n"
          "---@field eq fun(a:any, b:any):boolean\n"
          "---@field neq fun(a:any, b:any):boolean\n"
          "---@field lt fun(a:number, b:number):boolean\n"
          "---@field gt fun(a:number, b:number):boolean\n"
          "---@field le fun(a:number, b:number):boolean\n"
          "---@field ge fun(a:number, b:number):boolean\n"
          "---@field land fun(a:any, b:any):any\n"
          "---@field lor fun(a:any, b:any):any\n"
          "---@field lnot fun(a:any):any\n"
          "---@field concat fun(a:string, b:string):string\n"
          "---@field length fun(a:any):number\n"
          "---@field len fun(a:any):number\n"
          "\n"
          "---@class MosesLib\n"
          "---@field operator MosesOperator\n"
          "---@field op MosesOperator\n"
          "--- Table functions\n"
          "---@field clear fun(t:table):table\n"
          "---@field each fun(t:table, f:fun(v:any, k:any))\n"
          "---@field forEach fun(t:table, f:fun(v:any, k:any))\n"
          "---@field eachi fun(t:table, f:fun(v:any, k:any))\n"
          "---@field forEachi fun(t:table, f:fun(v:any, k:any))\n"
          "---@field at fun(t:table, ...:any):table\n"
          "---@field adjust fun(t:table, key:any, f:fun(v:any)|any):table\n"
          "---@field update fun(t:table, key:any, f:fun(v:any)|any):table\n"
          "---@field count fun(t:table, val?:any):number\n"
          "---@field countf fun(t:table, f:fun(v:any, k:any):boolean):number\n"
          "---@field allEqual fun(t:table, comp?:fun(a:any, b:any):boolean):boolean\n"
          "---@field alleq fun(t:table, comp?:fun(a:any, b:any):boolean):boolean\n"
          "---@field cycle fun(t:table, n?:number):fun():any\n"
          "---@field loop fun(t:table, n?:number):fun():any\n"
          "---@field map fun(t:table, f:fun(v:any, k:any):any):table\n"
          "---@field collect fun(t:table, f:fun(v:any, k:any):any):table\n"
          "---@field mapi fun(t:table, f:fun(v:any, k:any):any):table\n"
          "---@field reduce fun(t:table, f:fun(state:any, value:any):any, state?:any):any\n"
          "---@field inject fun(t:table, f:fun(state:any, value:any):any, state?:any):any\n"
          "---@field foldl fun(t:table, f:fun(state:any, value:any):any, state?:any):any\n"
          "---@field best fun(t:table, f:fun(state:any, value:any):boolean):any\n"
          "---@field reduceBy fun(t:table, f:fun(state:any, value:any):any, pred:fun(v:any, k:any):boolean, state?:any):any\n"
          "---@field reduceRight fun(t:table, f:fun(state:any, value:any):any, state?:any):any\n"
          "---@field injectr fun(t:table, f:fun(state:any, value:any):any, state?:any):any\n"
          "---@field foldr fun(t:table, f:fun(state:any, value:any):any, state?:any):any\n"
          "---@field mapReduce fun(t:table, f:fun(state:any, value:any):any, state?:any):table\n"
          "---@field mapr fun(t:table, f:fun(state:any, value:any):any, state?:any):table\n"
          "---@field mapReduceRight fun(t:table, f:fun(state:any, value:any):any, state?:any):table\n"
          "---@field maprr fun(t:table, f:fun(state:any, value:any):any, state?:any):table\n"
          "---@field include fun(t:table, value:any|fun(v:any, value:any):boolean):boolean\n"
          "---@field any fun(t:table, value:any|fun(v:any, value:any):boolean):boolean\n"
          "---@field some fun(t:table, value:any|fun(v:any, value:any):boolean):boolean\n"
          "---@field contains fun(t:table, value:any|fun(v:any, value:any):boolean):boolean\n"
          "---@field detect fun(t:table, value:any|fun(v:any, value:any):boolean):any\n"
          "---@field where fun(t:table, props:table):table|nil\n"
          "---@field findWhere fun(t:table, props:table):any\n"
          "---@field select fun(t:table, f:fun(v:any, k:any):boolean):table\n"
          "---@field filter fun(t:table, f:fun(v:any, k:any):boolean):table\n"
          "---@field reject fun(t:table, f:fun(v:any, k:any):boolean):table\n"
          "---@field discard fun(t:table, f:fun(v:any, k:any):boolean):table\n"
          "---@field all fun(t:table, f:fun(v:any, k:any):boolean):boolean\n"
          "---@field every fun(t:table, f:fun(v:any, k:any):boolean):boolean\n"
          "---@field invoke fun(t:table, method:any):table\n"
          "---@field pluck fun(t:table, key:any):table\n"
          "---@field max fun(t:table, transform?:fun(v:any, k:any):any):any\n"
          "---@field min fun(t:table, transform?:fun(v:any, k:any):any):any\n"
          "---@field same fun(a:table, b:table):boolean\n"
          "---@field sort fun(t:table, comp?:fun(a:any, b:any):boolean):table\n"
          "---@field sortedk fun(t:table, comp?:fun(a:any, b:any):boolean):fun():any, any\n"
          "---@field sortedv fun(t:table, comp?:fun(a:any, b:any):boolean):fun():any, any\n"
          "---@field sortBy fun(t:table, transform?:fun(v:any):any|string, comp?:fun(a:any, b:any):boolean):table\n"
          "---@field groupBy fun(t:table, iter:fun(v:any, k:any):any):table\n"
          "---@field countBy fun(t:table, iter:fun(v:any, k:any):any):table\n"
          "---@field size fun(...:any):number\n"
          "---@field containsKeys fun(t:table, other:table):boolean\n"
          "---@field sameKeys fun(tA:table, tB:table):boolean\n"
          "--- Array functions\n"
          "---@field sample fun(array:table, n?:number, seed?:number):table\n"
          "---@field sampleProb fun(array:table, prob:number, seed?:number):table\n"
          "---@field nsorted fun(array:table, n?:number, comp?:fun(a:any, b:any):boolean):table\n"
          "---@field shuffle fun(array:table, seed?:number):table\n"
          "---@field pack fun(...:any):table\n"
          "---@field find fun(array:table, value:any, from?:number):number|nil\n"
          "---@field reverse fun(array:table):table\n"
          "---@field fill fun(array:table, value:any, i?:number, j?:number):table\n"
          "---@field zeros fun(n:number):table\n"
          "---@field ones fun(n:number):table\n"
          "---@field vector fun(value:any, n:number):table\n"
          "---@field selectWhile fun(array:table, f:fun(v:any, k:any):boolean):table\n"
          "---@field takeWhile fun(array:table, f:fun(v:any, k:any):boolean):table\n"
          "---@field dropWhile fun(array:table, f:fun(v:any, k:any):boolean):table\n"
          "---@field rejectWhile fun(array:table, f:fun(v:any, k:any):boolean):table\n"
          "---@field sortedIndex fun(array:table, value:any, comp?:fun(a:any, b:any):boolean, sort?:boolean):number\n"
          "---@field indexOf fun(array:table, value:any):number|nil\n"
          "---@field lastIndexOf fun(array:table, value:any):number|nil\n"
          "---@field findIndex fun(array:table, pred:fun(v:any, k:any):boolean):number|nil\n"
          "---@field findLastIndex fun(array:table, pred:fun(v:any, k:any):boolean):number|nil\n"
          "---@field addTop fun(array:table, ...:any):table\n"
          "---@field prepend fun(array:table, ...:any):table\n"
          "---@field push fun(array:table, ...:any):table\n"
          "---@field shift fun(array:table, n?:number):...\n"
          "---@field pop fun(array:table, n?:number):...\n"
          "---@field unshift fun(array:table, n?:number):...\n"
          "---@field pull fun(array:table, ...:any):table\n"
          "---@field remove fun(array:table, ...:any):table\n"
          "---@field removeRange fun(array:table, start?:number, finish?:number):table\n"
          "---@field rmRange fun(array:table, start?:number, finish?:number):table\n"
          "---@field chop fun(array:table, start?:number, finish?:number):table\n"
          "---@field chunk fun(array:table, f?:fun(v:any, k:any):any):table\n"
          "---@field slice fun(array:table, start?:number, finish?:number):table\n"
          "---@field sub fun(array:table, start?:number, finish?:number):table\n"
          "---@field first fun(array:table, n?:number):table\n"
          "---@field head fun(array:table, n?:number):table\n"
          "---@field take fun(array:table, n?:number):table\n"
          "---@field initial fun(array:table, n?:number):table\n"
          "---@field last fun(array:table, n?:number):table\n"
          "---@field rest fun(array:table, index?:number):table\n"
          "---@field tail fun(array:table, index?:number):table\n"
          "---@field nth fun(array:table, index:number):any\n"
          "---@field compact fun(array:table):table\n"
          "---@field flatten fun(array:table, shallow?:boolean):table\n"
          "---@field difference fun(array:table, array2?:table):table\n"
          "---@field without fun(array:table, array2?:table):table\n"
          "---@field diff fun(array:table, array2?:table):table\n"
          "---@field union fun(...:table):table\n"
          "---@field intersection fun(...:table):table\n"
          "---@field disjoint fun(...:table):boolean\n"
          "---@field symmetricDifference fun(array:table, array2:table):table\n"
          "---@field symdiff fun(array:table, array2:table):table\n"
          "---@field xor fun(array:table, array2:table):table\n"
          "---@field unique fun(array:table):table\n"
          "---@field uniq fun(array:table):table\n"
          "---@field isunique fun(array:table):boolean\n"
          "---@field isuniq fun(array:table):boolean\n"
          "---@field duplicates fun(array:table):table\n"
          "---@field zip fun(...:table):table\n"
          "---@field transpose fun(...:table):table\n"
          "---@field zipWith fun(f:fun(...):any, ...:table):table\n"
          "---@field transposeWith fun(f:fun(...):any, ...:table):table\n"
          "---@field append fun(array:table, other:table):table\n"
          "---@field interleave fun(...:table):table\n"
          "---@field interpose fun(array:table, value:any):table\n"
          "---@field intersperse fun(array:table, value:any):table\n"
          "---@field range fun(from?:number, to?:number, step?:number):table\n"
          "---@field rep fun(value:any, n:number):table\n"
          "---@field powerset fun(array:table):table\n"
          "---@field partition fun(array:table, n?:number, pad?:any):fun():table\n"
          "---@field part fun(array:table, n?:number, pad?:any):fun():table\n"
          "---@field overlapping fun(array:table, n?:number, pad?:any):fun():table\n"
          "---@field aperture fun(array:table, n?:number):fun():table\n"
          "---@field sliding fun(array:table, n?:number):fun():table\n"
          "---@field pairwise fun(array:table):fun():table\n"
          "---@field permutation fun(array:table):fun():table\n"
          "---@field perm fun(array:table):fun():table\n"
          "---@field concat fun(array:table, sep?:string, i?:number, j?:number):string\n"
          "---@field join fun(array:table, sep?:string, i?:number, j?:number):string\n"
          "---@field xprod fun(array:table, array2:table):table\n"
          "---@field xpairs fun(value:any, array:table):table\n"
          "---@field xpairsRight fun(value:any, array:table):table\n"
          "---@field sum fun(array:table):number\n"
          "---@field product fun(array:table):number\n"
          "---@field mean fun(array:table):number\n"
          "---@field average fun(array:table):number\n"
          "---@field median fun(array:table):number|nil\n"
          "--- Utility functions\n"
          "---@field noop fun()\n"
          "---@field identity fun(value:any):any\n"
          "---@field call fun(f:fun(...):any, ...:any):any\n"
          "---@field constant fun(value:any):fun():any\n"
          "---@field always fun(value:any):fun():any\n"
          "---@field applySpec fun(specs:table):fun(...):table\n"
          "---@field thread fun(value:any, ...:any):any\n"
          "---@field threadRight fun(value:any, ...:any):any\n"
          "---@field dispatch fun(...:fun(...):any):fun(...):any\n"
          "---@field memoize fun(f:fun(key:any):any):fun(key:any):any\n"
          "---@field cache fun(f:fun(key:any):any):fun(key:any):any\n"
          "---@field unfold fun(f:fun(seed:any):any, any, seed:any):table\n"
          "---@field once fun(f:fun(...):any):fun(...):any\n"
          "---@field before fun(f:fun(...):any, count:number):fun(...):any\n"
          "---@field after fun(f:fun(...):any, count:number):fun(...):any\n"
          "---@field compose fun(...:fun(...):any):fun(...):any\n"
          "---@field pipe fun(value:any, ...:fun(...):any):any\n"
          "---@field complement fun(f:fun(...):boolean):fun(...):boolean\n"
          "---@field juxtapose fun(value:any, ...:fun(any):any):...\n"
          "---@field juxt fun(value:any, ...:fun(any):any):...\n"
          "---@field wrap fun(f:fun(...):any, wrapper:fun(f:fun(...):any, ...):any):fun(...):any\n"
          "---@field times fun(iter:fun(i:number):any, n?:number):table\n"
          "---@field bind fun(f:fun(...):any, v:any):fun(...):any\n"
          "---@field bind2 fun(f:fun(...):any, v:any):fun(...):any\n"
          "---@field bindn fun(f:fun(...):any, ...:any):fun(...):any\n"
          "---@field bindall fun(obj:table, ...:string):table\n"
          "---@field cond fun(conds:table):fun(...):any\n"
          "---@field both fun(...:fun(...):boolean):fun(...):boolean\n"
          "---@field either fun(...:fun(...):boolean):fun(...):boolean\n"
          "---@field neither fun(...:fun(...):boolean):fun(...):boolean\n"
          "---@field uniqueId fun(template?:string|fun(id:number):any):any\n"
          "---@field uid fun(template?:string|fun(id:number):any):any\n"
          "---@field iterator fun(f:fun(value:any):any, value:any, n?:number):fun():any\n"
          "---@field iter fun(f:fun(value:any):any, value:any, n?:number):fun():any\n"
          "---@field skip fun(iter:fun():any, n?:number):fun():any\n"
          "---@field tabulate fun(...:any):table\n"
          "---@field iterlen fun(...:any):number\n"
          "---@field castArray fun(value:any):table\n"
          "---@field flip fun(f:fun(...):any):fun(...):any\n"
          "---@field nthArg fun(n:number):fun(...):any\n"
          "---@field unary fun(f:fun(...):any):fun(...):any\n"
          "---@field ary fun(f:fun(...):any, n?:number):fun(...):any\n"
          "---@field nAry fun(f:fun(...):any, n?:number):fun(...):any\n"
          "---@field noarg fun(f:fun():any):fun():any\n"
          "---@field rearg fun(f:fun(...):any, indexes:table):fun(...):any\n"
          "---@field over fun(...:fun(...):any):fun(...):table\n"
          "---@field overEvery fun(...:fun(...):boolean):fun(...):boolean\n"
          "---@field overSome fun(...:fun(...):boolean):fun(...):boolean\n"
          "---@field overArgs fun(f:fun(...):any, ...:fun(v:any):any):fun(...):any\n"
          "---@field converge fun(f:fun(...):any, g:fun(...):any, h:fun(...):any):fun(...):any\n"
          "---@field partial fun(f:fun(...):any, ...:any):fun(...):any\n"
          "---@field partialRight fun(f:fun(...):any, ...:any):fun(...):any\n"
          "---@field curry fun(f:fun(...):any, n_args?:number):fun(...):any\n"
          "---@field time fun(f:fun(...):any, ...:any):number, ...\n"
          "--- Object functions\n"
          "---@field keys fun(obj:table):table\n"
          "---@field values fun(obj:table):table\n"
          "---@field path fun(obj:table, ...:any):any\n"
          "---@field spreadPath fun(obj:table, ...:any):table\n"
          "---@field flattenPath fun(obj:table, ...:any):table\n"
          "---@field kvpairs fun(obj:table):table\n"
          "---@field toObj fun(kvpairs:table):table\n"
          "---@field invert fun(obj:table):table\n"
          "---@field mirror fun(obj:table):table\n"
          "---@field property fun(key:any):fun(obj:table):any\n"
          "---@field propertyOf fun(obj:table):fun(key:any):any\n"
          "---@field toBoolean fun(value:any):boolean\n"
          "---@field extend fun(destObj:table, ...:table):table\n"
          "---@field functions fun(obj?:table, recurseMt?:boolean):table\n"
          "---@field methods fun(obj?:table, recurseMt?:boolean):table\n"
          "---@field clone fun(obj:table, shallow?:boolean):table\n"
          "---@field tap fun(obj:table, f:fun(obj:table)):table\n"
          "---@field has fun(obj:table, key:any):boolean\n"
          "---@field pick fun(obj:table, ...:string):table\n"
          "---@field choose fun(obj:table, ...:string):table\n"
          "---@field omit fun(obj:table, ...:string):table\n"
          "---@field drop fun(obj:table, ...:string):table\n"
          "---@field template fun(obj:table, template?:table):table\n"
          "---@field defaults fun(obj:table, template?:table):table\n"
          "---@field isEqual fun(objA:any, objB:any, useMt?:boolean):boolean\n"
          "---@field compare fun(objA:any, objB:any, useMt?:boolean):boolean\n"
          "---@field matches fun(objA:any, objB:any, useMt?:boolean):boolean\n"
          "---@field result fun(obj:table, method:any):any\n"
          "---@field isTable fun(t:any):boolean\n"
          "---@field isCallable fun(obj:any):boolean\n"
          "---@field isArray fun(obj:any):boolean\n"
          "---@field isIterable fun(obj:any):boolean\n"
          "---@field type fun(obj:any):string\n"
          "---@field isEmpty fun(obj?:any):boolean\n"
          "---@field isString fun(obj:any):boolean\n"
          "---@field isFunction fun(obj:any):boolean\n"
          "---@field isNil fun(obj:any):boolean\n"
          "---@field isNumber fun(obj:any):boolean\n"
          "---@field isNaN fun(obj:any):boolean\n"
          "---@field isFinite fun(obj:any):boolean\n"
          "---@field isBoolean fun(obj:any):boolean\n"
          "---@field isInteger fun(obj:any):boolean\n"
          "--- Chain & import\n"
          "---@field chain fun(value:any):MosesChain\n"
          "---@field import fun(context?:table, noConflict?:boolean):table\n"
          "\n"
          "---@class MosesChain\n"
          "---@field value fun(self):any\n"
          "\n"
          "---@type MosesLib\n"
          "moses = nil\n"
          "\n",
          f);

    fputs("---@class LuaVariant : userdata\n"
          "---@field bin string 返回内存中二进制字符序列\n"
          "---@field hex string 16进制字符串表示\n"
          "---@field val string 10进制字符串表示\n"
          "---@field rawval number 数值\n"
          "---@field type string 类型\n"
          "local LuaVariant = {}\n"
          "\n"
          "---@type fun(type: string): LuaVariant\n"
          "---@param type string support i8、u8、i16、u16、i32、u32、i64、u64、f32、f64\n"
          "---@return LuaVariant\n"
          "function LuaVariant:as(type) end\n"
          "\n"
          "---@type fun(type: string): LuaVariant\n"
          "---@param type string support i8、u8、i16、u16、i32、u32、i64、u64、f32、f64\n"
          "---@return LuaVariant\n"
          "function LuaVariant:to(type) end\n"
          "\n",
          f);

    fputs("---\n"
          "---U8 创建无符号8位LuaVariant，参数可为数字或16进制字符串\n"
          "---\n"
          "---@type fun(value: string|number): LuaVariant\n"
          "function U8(value) end\n"
          "\n",
          f);

    fputs("---\n"
          "---I8 创建有符号8位LuaVariant\n"
          "---\n"
          "---@type fun(value: string|number): LuaVariant\n"
          "function I8(value) end\n"
          "\n",
          f);

    fputs("---\n"
          "---U16 创建无符号16位LuaVariant，参数可为数字或16进制字符串\n"
          "---\n"
          "---@type fun(value: string|number): LuaVariant\n"
          "function U16(value) end\n"
          "\n",
          f);

    fputs("---\n"
          "---I16 创建有符号16位LuaVariant\n"
          "---\n"
          "---@type fun(value: string|number): LuaVariant\n"
          "function I16(value) end\n"
          "\n",
          f);

    fputs("---\n"
          "---U32 创建无符号32位LuaVariant，参数可为数字或16进制字符串\n"
          "---\n"
          "---@type fun(value: string|number): LuaVariant\n"
          "function U32(value) end\n"
          "\n",
          f);

    fputs("---\n"
          "---I32 创建有符号32位LuaVariant\n"
          "---\n"
          "---@type fun(value: string|number): LuaVariant\n"
          "function I32(value) end\n"
          "\n",
          f);

    fputs("---\n"
          "---U64 创建无符号64位LuaVariant\n"
          "---\n"
          "---@type fun(value: string|number): LuaVariant\n"
          "function U64(value) end\n"
          "\n",
          f);

    fputs("---\n"
          "---I64 创建有符号64位LuaVariant\n"
          "---\n"
          "---@type fun(value: string|number): LuaVariant\n"
          "function I64(value) end\n"
          "\n",
          f);

    fputs("---\n"
          "---F32 创建32位浮点LuaVariant\n"
          "---\n"
          "---@type fun(value: string|number): LuaVariant\n"
          "function F32(value) end\n"
          "\n",
          f);

    fputs("---\n"
          "---F64 创建64位浮点LuaVariant\n"
          "---\n"
          "---@type fun(value: string|number): LuaVariant\n"
          "function F64(value) end\n"
          "\n",
          f);

    fputs("---@class Var : userdata\n"
          "local Var = {}\n"
          "\n",
          f);

    fputs("---\n"
          "---V 创建指向目标进程内存的Var指针对象\n"
          "---Var支持: \n"
          "--- 1. var[LuaVariant|number]解引用\n"
          "--- 2. var:off(a)偏移\n"
          "--- 3. var:to(type[, offset])、var:tos(type, count[, offset])、var:sto(type, count[, offset])、类型化读取\n"
          "--- 4. var:write(type[, offset])、var:writes(type, count[, offset])、var:swrite(type, count[, offset])、类型化读取\n"
          "---\n"
          "---@type fun(addr: LuaVariant): Var\n"
          "function V(addr) end\n"
          "\n",
          f);

    fputs("---\n"
          "---@type fun(a: LuaVariant|integer): LuaVariant 偏移\n"
          "---@param a LuaVariant|integer 解析偏移\n"
          "---@return LuaVariant\n"
          "function Var:off(a) end\n"
          "\n",
          f);

    fputs("---\n"
          "---@type fun(type: string, offset: (LuaVariant|integer)?): LuaVariant 读取类型化数据\n"
          "---@param type string 类型标识，支持:\n"
          "---                   'c'=char 'b'=uchar 'h'=short 'H'=ushort 'i'=int 'I'=uint\n"
          "---                   'q'=longlong 'Q'=ulonglong 'z'=size_t 'f'=float 'd'=double\n"
          "---@param offset (LuaVariant|integer)? 解析偏移，可选\n"
          "---@return LuaVariant\n"
          "function Var:to(type, offset) end\n"
          "\n",
          f);

    fputs("---\n"
          "---@type fun(type: string, count: LuaVariant|integer, offset: (LuaVariant|integer)?): LuaVariant[] 读取类型化数据数组\n"
          "---@param type string 类型标识，支持:\n"
          "---                   'c'=char 'b'=uchar 'h'=short 'H'=ushort 'i'=int 'I'=uint\n"
          "---                   'q'=longlong 'Q'=ulonglong 'z'=size_t 'f'=float 'd'=double\n"
          "---@param count LuaVariant|integer 解析数量\n"
          "---@param offset (LuaVariant|integer)? 解析偏移，可选\n"
          "---@return LuaVariant[]\n"
          "function Var:tos(type, count, offset) end\n"
          "\n",
          f);

    fputs("---\n"
          "---@type fun(type: string, count: LuaVariant|integer, offset: (LuaVariant|integer)?): LuaVariant[] 读取字符串数据\n"
          "---@param type string 类型标识，支持:\n"
          "---                   's'=utf8字符串 'b'=字节数据",
          f);
#ifdef _UNICODE
    fputs(" 'w'=utf16-le字符串", f);
#endif
    fputs("\n"
          "---@param count LuaVariant|integer 解析字符串长度数量\n"
          "---@param offset (LuaVariant|integer)? 解析偏移，可选\n"
          "---@return string\n"
          "function Var:sto(type, count, offset) end\n"
          "\n",
          f);

    fputs("---\n"
          "---@type fun(val: LuaVariant|number, offset: (LuaVariant|integer)?) 写入类型化数据\n"
          "---@param val LuaVariant|number 待写入的值\n"
          "---@param offset (LuaVariant|integer)? 解析偏移，可选\n"
          "function Var:write(val, offset) end\n"
          "\n",
          f);

    fputs("---\n"
          "---@type fun(vals: (LuaVariant|number)[], offset: (LuaVariant|integer)?) 写入类型化数据数组\n"
          "---@param vals (LuaVariant|number)[] 待写入的值数组\n"
          "---@param offset (LuaVariant|integer)? 解析偏移，可选\n"
          "function Var:writes(vals, offset) end\n"
          "\n",
          f);

    fputs("---\n"
          "---@type fun(val: string, offset: (LuaVariant|integer)?) 写入字节数组数据\n"
          "---@param val string 字节数组\n"
          "---@param offset (LuaVariant|integer)? 解析偏移，可选\n"
          "function Var:swrite(val, offset) end\n"
          "\n",
          f);

    fputs("---\n"
          "---Run 启动调试进程并进入调试循环（阻塞直到进程退出或按键退出）\n"
          "---按Shift+Q分离进程不终止，按Q终止进程\n"
          "---\n"
          "---@class ExceptSInfo\n"
          "---@field type string|LuaVariant|nil 内存访问异常类型：读取、写入、DEP\n"
          "---@field address LuaVariant? 内存访问异常数据地址\n"
          "---@field nt_status LuaVariant? 内存访问异常NTSTATUS\n"
          "---@field obj_ptr LuaVariant? c++异常对象指针 \n"
          "---@field obj_class_name string? c++异常对象类型名 \n"
          "---@field obj_type_info_ptr LuaVariant? c++异常对象类型信息地址 \n"
          "---@field module_hinstance LuaVariant|string|nil c++异常模块句柄、名称\n"
          "---@field args LuaVariant[]? 一般异常参数\n"
          "---\n"
          "---@class ExceptRecord\n"
          "---@field code LuaVariant 异常代码\n"
          "---@field code_name string? 异常代码名称\n"
          "---@field code_info string? 异常代码信息\n"
          "---@field flag LuaVariant 异常标志\n"
          "---@field non_continuable boolean 是否为不可持续异常?\n"
          "---@field address LuaVariant 异常地址\n"
          "---@field information ExceptSInfo 异常信息\n"
          "---\n"
          "---@class ExceptInfo\n"
          "---@field pid LuaVariant\n"
          "---@field tid LuaVariant\n"
          "---@field first_chance boolean 第一次触发此异常?\n"
          "---@field records ExceptRecord[] 异常记录\n"
          "---\n"
          "---@class OnEnvWithCode\n"
          "---@field pid LuaVariant\n"
          "---@field tid LuaVariant\n"
          "---@field code LuaVariant\n"
          "---\n"
          "---@class OnEnvWithCodeAndAddr\n"
          "---@field pid LuaVariant\n"
          "---@field tid LuaVariant\n"
          "---@field code LuaVariant\n"
          "---@field addr LuaVariant\n"
          "---\n"
          "---@class OnEnvWithAddr\n"
          "---@field pid LuaVariant\n"
          "---@field tid LuaVariant\n"
          "---@field addr LuaVariant\n"
          "---\n"
          "---@class OnCreateProcess\n"
          "---@field pid LuaVariant\n"
          "---@field tid LuaVariant\n"
          "---@field addr LuaVariant\n"
          "---@field base LuaVariant\n"
          "---@field path string\n"
          "---\n"
          "---@class OnLoadDll\n"
          "---@field pid LuaVariant\n"
          "---@field tid LuaVariant\n"
          "---@field base LuaVariant\n"
          "---@field path string\n"
          "---\n"
          "---@class OnUnloadDll\n"
          "---@field pid LuaVariant\n"
          "---@field tid LuaVariant\n"
          "---@field base LuaVariant\n"
          "---\n"
          "---@class OnOutputDebugString\n"
          "---@field pid LuaVariant\n"
          "---@field tid LuaVariant\n"
          "---@field str string\n"
          "---\n"
          "---@class OnRip\n"
          "---@field pid LuaVariant\n"
          "---@field tid LuaVariant\n"
          "---\n"
          "---@class Position\n"
          "---@field x LuaVariant\n"
          "---@field y LuaVariant\n"
          "---\n"
          "---@class OnKeyPressEventState\n"
          "---@field mouse_moved boolean|nil\n"
          "---@field double_click boolean|nil\n"
          "---@field mouse_wheeled boolean|nil\n"
          "---@field mouse_hwheeled boolean|nil\n"
          "---\n"
          "---@class OnKeyPressBtnState\n"
          "---@field from_left_1st_button_pressed boolean|nil\n"
          "---@field rightmost_button_pressed boolean|nil\n"
          "---@field from_left_2nd_button_pressed boolean|nil\n"
          "---@field from_left_3rd_button_pressed boolean|nil\n"
          "---@field from_left_4th_button_pressed boolean|nil\n"
          "---\n"
          "---@class OnKeyPressState\n"
          "---@field left_alt boolean|nil\n"
          "---@field right_alt boolean|nil\n"
          "---@field left_ctrl boolean|nil\n"
          "---@field right_ctrl boolean|nil\n"
          "---@field shift boolean|nil\n"
          "---@field numlock_on boolean|nil\n"
          "---@field scrolllock_on boolean|nil\n"
          "---@field capslock_on boolean|nil\n"
          "---@field enhanced_key boolean|nil\n"
          "---@field nls_dbcschar boolean|nil\n"
          "---@field nls_alphanumeric boolean|nil\n"
          "---@field nls_katakana boolean|nil\n"
          "---@field nls_hiragana boolean|nil\n"
          "---@field nls_roman boolean|nil\n"
          "---@field nls_ime_conversion boolean|nil\n"
          "---@field altnumpad_bit boolean|nil\n"
          "---@field nls_ime_disable boolean|nil\n"
          "---\n"
          "---@class OnConsoleEvent\n"
          "---@field type string key,mouse,window_buffer_size,menu,focus\n"
          "---\n"
          "---key\n"
          "---@field down boolean\n"
          "---@field repeat_count LuaVariant\n"
          "---@field vk_code string|LuaVariant\n"
          "---@field vs_code LuaVariant\n"
          "---@field char string\n"
          "---@field key_state OnKeyPressState\n"
          "---\n"
          "---mouse\n"
          "---@field pos Position\n"
          "---@field btn_state OnKeyPressBtnState\n"
          "---@field key_state OnKeyPressState\n"
          "---@field event_flag OnKeyPressEventState\n"
          "---\n"
          "---window_buffer_size\n"
          "---@field width LuaVariant\n"
          "---@field height LuaVariant\n"
          "---\n"
          "---menu\n"
          "---@field cmdid LuaVariant\n"
          "---\n"
          "---focus\n"
          "---@field set_focus boolean\n"
          "---\n"
          "---@class RunOpt\n"
          "---@field imagepath string? 程序路径\n"
          "---@field cmdline string? 命令行（含imagepath时可省略imagepath）\n"
          "---@field cwd string? 工作目录\n"
          "---@field is_new_console boolean? 是否新建控制台窗口\n"
          "---@field use_scylla_hide boolean? 是否使用ScyllaHide反反调试\n"
          "---@field callback fun(pid: LuaVariant, tid: LuaVariant)? 入口断点回调\n"
          "---@field exception_callback fun(info: ExceptInfo)? 异常回调\n"
          "---@field on_any_debug_event fun(opt: OnEnvWithCode)? Debug 任意调试事件回调\n"
          "---@field on_exception fun(opt: OnEnvWithCodeAndAddr)? Debug 任意异常事件回调\n"
          "---@field on_breakpoint fun(opt: OnEnvWithAddr)? Debug 任意断点事件回调\n"
          "---@field on_single_step fun(opt: OnEnvWithAddr)? Debug 任意单步事件回调\n"
          "---@field on_create_thread fun(opt: OnEnvWithAddr)? Debug 创建线程事件回调\n"
          "---@field on_create_process fun(opt: OnCreateProcess)? Debug 创建进程事件回调\n"
          "---@field on_exit_thread fun(opt: OnEnvWithCode)? Debug 线程退出事件回调\n"
          "---@field on_exit_process fun(opt: OnEnvWithCode)? Debug 进程退出事件回调\n"
          "---@field on_load_dll fun(opt: OnLoadDll)? Debug 加载dll事件回调\n"
          "---@field on_unload_dll fun(opt: OnUnloadDll)? Debug 卸载dll事件回调\n"
          "---@field on_output_debug_string fun(opt: OnOutputDebugString)? Debug 输出调试字符串事件回调\n"
          "---@field on_rip fun(opt: OnRip)? Debug RIP 事件回调\n"
          "---@field on_console_event fun(opt: OnConsoleEvent)? 终端事件回调\n"
          "---\n"
          "---@type fun(opt: RunOpt)\n"
          "function Run(opt) end\n"
          "\n",
          f);

    fputs("---\n"
          "---Attach 附加调试进程并进入调试循环\n"
          "---按Shift+Q分离进程不终止，按Q终止进程\n"
          "---\n"
          "---@class AttachOpt\n"
          "---@field pid integer? 程序pid\n"
          "---@field callback fun(pid: LuaVariant, tid: LuaVariant)? 入口断点回调\n"
          "---@field exception_callback fun(info: ExceptInfo)? 异常回调\n"
          "---@field on_any_debug_event fun(opt: OnEnvWithCode)? Debug 任意调试事件回调\n"
          "---@field on_exception fun(opt: OnEnvWithCodeAndAddr)? Debug 任意异常事件回调\n"
          "---@field on_breakpoint fun(opt: OnEnvWithAddr)? Debug 任意断点事件回调\n"
          "---@field on_single_step fun(opt: OnEnvWithAddr)? Debug 任意单步事件回调\n"
          "---@field on_create_thread fun(opt: OnEnvWithAddr)? Debug 创建线程事件回调\n"
          "---@field on_create_process fun(opt: OnCreateProcess)? Debug 创建进程事件回调\n"
          "---@field on_exit_thread fun(opt: OnEnvWithCode)? Debug 线程退出事件回调\n"
          "---@field on_exit_process fun(opt: OnEnvWithCode)? Debug 进程退出事件回调\n"
          "---@field on_load_dll fun(opt: OnLoadDll)? Debug 加载dll事件回调\n"
          "---@field on_unload_dll fun(opt: OnUnloadDll)? Debug 卸载dll事件回调\n"
          "---@field on_output_debug_string fun(opt: OnOutputDebugString)? Debug 输出调试字符串事件回调\n"
          "---@field on_rip fun(opt: OnRip)? Debug RIP 事件回调\n"
          "---@field on_console_event fun(opt: OnConsoleEvent)? 终端事件回调\n"
          "---\n"
          "---@type fun(callback: function): nil\n"
          "function Attach(callback) end\n"
          "---@type fun(opt: AttachOpt): nil\n"
          "function Attach(opt) end\n"
          "\n",
          f);

    fputs("---\n"
          "---Detach 脱离调试进程\n"
          "---\n"
          "---@type fun()\n"
          "function Detach() end\n"
          "\n",
          f);

    fputs("---\n"
          "---Kill 杀死调试进程\n"
          "---\n"
          "---@type fun()\n"
          "function Kill() end\n"
          "\n",
          f);

    fputs("---\n"
          "---ForceKill 强制杀死调试进程\n"
          "---\n"
          "---@type fun()\n"
          "function ForceKill() end\n"
          "\n",
          f);

    fputs("---\n"
          "---SetBreakpoint 设置一个断点，如果模块已加载则立即写入0xCC\n"
          "---\n"
          "---@class BpOpt\n"
          "---@field modname string 模块名（不含路径）\n"
          "---@field rva LuaVariant 断点RVA\n"
          "---@field is_single boolean? 是否为一次性断点（触发后自动删除）\n"
          "---@field callback fun(info: BpInfo)? 断点回调\n"
          "---\n"
          "---@class BpInfo\n"
          "---@field ea LuaVariant 断点地址\n"
          "---@field modname string 模块名\n"
          "---@field rva LuaVariant RVA\n"
          "---@field pid LuaVariant 进程ID\n"
          "---@field tid LuaVariant 线程ID\n"
          "---\n"
          "---@type fun(opt: BpOpt)\n"
          "function SetBreakpoint(opt) end\n"
          "\n",
          f);

    fputs("---\n"
          "---SetBreakpoints 批量设置断点\n"
          "---格式: {[modname] = {[rva: LuaVariant] = {callback?, is_single?}}}\n"
          "---\n"
          "---@type fun(breakpoints: table)\n"
          "function SetBreakpoints(breakpoints) end\n"
          "\n",
          f);

    fputs("---\n"
          "---DelBreakpoint 删除断点并恢复原始字节\n"
          "---\n"
          "---@type fun(ea: LuaVariant)\n"
          "function DelBreakpoint(ea) end\n"
          "---@type fun(modname: string, rva: LuaVariant)\n"
          "function DelBreakpoint(modname, rva) end\n"
          "\n",
          f);

    fputs("---\n"
          "---SetHardwareBreakpoint 设置硬件断点\n"
          "---\n"
          "---@class HwBpOpt\n"
          "---@field ea LuaVariant 断点地址\n"
          "---@field index integer? 硬件断点dr寄存器序号，支持0~3\n"
          "---@field type string 硬件断点类型，支持e: 执行、w: 写入、r: 读写\n"
          "---@field size integer 硬件断点大小，支持" HWB_SIZE_STR "\n"
          "---@field callback fun(info: HwBpInfo)? 硬件断点回调\n"
          "---\n"
          "---@class HwBpInfo\n"
          "---@field ea LuaVariant 断点地址\n"
          "---@field pid LuaVariant 进程ID\n"
          "---@field tid LuaVariant 线程ID\n"
          "---\n"
          "---@type fun(opt: HwBpOpt)\n"
          "function SetHardwareBreakpoint(opt) end\n"
          "\n",
          f);

    fputs("---\n"
          "---DelHardwareBreakpoint 删除断点并恢复原始字节\n"
          "---\n"
          "---@type fun(index: integer?)\n"
          "function DelHardwareBreakpoint(index) end\n"
          "\n",
          f);

    fputs("---\n"
          "---EnableBreakpoint 启用已禁用的断点\n"
          "---\n"
          "---@type fun(ea: LuaVariant)\n"
          "function EnableBreakpoint(ea) end\n"
          "---@type fun(modname: string, rva: LuaVariant)\n"
          "function EnableBreakpoint(modname, rva) end\n"
          "\n",
          f);

    fputs("---\n"
          "---DisableBreakpoint 禁用断点（恢复原始字节但不删除记录）\n"
          "---\n"
          "---@type fun(ea: LuaVariant)\n"
          "function DisableBreakpoint(ea) end\n"
          "---@type fun(modname: string, rva: LuaVariant)\n"
          "function DisableBreakpoint(modname, rva) end\n"
          "\n",
          f);

    fputs("---\n"
          "---ReadMemory 读取目标进程内存\n"
          "---\n"
          "---@type fun(addr: UZ, size: UZ): table\n"
          "function ReadMemory(addr, size) end\n"
          "\n",
          f);

    fputs("---\n"
          "---WriteMemory 修改目标进程内存\n"
          "---\n"
          "---@type fun(addr: UZ, new_data: table, is_up_intrin: boolean?)\n"
          "function WriteMemory(addr, new_data, is_up_intrin) end\n"
          "\n",
          f);

    fputs("---\n"
          "---GetContext 读取线程上下文\n"
          "---@class XMM\n"
          "---@field Low LuaVariant\n"
          "---@field High LuaVariant\n"
          "---\n"
          "---@class Context\n"
          "---@field ContextFlags LuaVariant\n"
          "---@field SegGs LuaVariant\n"
          "---@field SegFs LuaVariant\n"
          "---@field SegEs LuaVariant\n"
          "---@field SegDs LuaVariant\n"
          "---@field SegCs LuaVariant\n"
          "---@field SegSs LuaVariant\n"
          "---@field EFlags LuaVariant\n"
          "---@field Dr0 LuaVariant\n"
          "---@field Dr1 LuaVariant\n"
          "---@field Dr2 LuaVariant\n"
          "---@field Dr3 LuaVariant\n"
          "---@field Dr6 LuaVariant\n"
          "---@field Dr7 LuaVariant\n"
          "---@field Cax LuaVariant\n"
          "---@field Cbx LuaVariant\n"
          "---@field Ccx LuaVariant\n"
          "---@field Cdx LuaVariant\n"
          "---@field Csi LuaVariant\n"
          "---@field Cdi LuaVariant\n"
          "---@field Csp LuaVariant\n"
          "---@field Cbp LuaVariant\n"
          "---@field Cip LuaVariant\n"
#ifdef _WIN64
          "---@field P1Home LuaVariant\n"
          "---@field P2Home LuaVariant\n"
          "---@field P3Home LuaVariant\n"
          "---@field P4Home LuaVariant\n"
          "---@field P5Home LuaVariant\n"
          "---@field P6Home LuaVariant\n"
          "---@field MxCsr LuaVariant\n"
          "---@field R8 LuaVariant\n"
          "---@field R9 LuaVariant\n"
          "---@field R10 LuaVariant\n"
          "---@field R11 LuaVariant\n"
          "---@field R12 LuaVariant\n"
          "---@field R13 LuaVariant\n"
          "---@field R14 LuaVariant\n"
          "---@field R15 LuaVariant\n"
          "---@field Xmm0 XMM\n"
          "---@field Xmm1 XMM\n"
          "---@field Xmm2 XMM\n"
          "---@field Xmm3 XMM\n"
          "---@field Xmm4 XMM\n"
          "---@field Xmm5 XMM\n"
          "---@field Xmm6 XMM\n"
          "---@field Xmm7 XMM\n"
          "---@field Xmm8 XMM\n"
          "---@field Xmm9 XMM\n"
          "---@field Xmm10 XMM\n"
          "---@field Xmm11 XMM\n"
          "---@field Xmm12 XMM\n"
          "---@field Xmm13 XMM\n"
          "---@field Xmm14 XMM\n"
          "---@field Xmm15 XMM\n"
          "---@field VectorControla LuaVariant\n"
          "---@field DebugControla LuaVariant\n"
          "---@field LastBranchToRipa LuaVariant\n"
          "---@field LastBranchFromRipa LuaVariant\n"
          "---@field LastExceptionToRipa LuaVariant\n"
          "---@field LastExceptionFromRipa LuaVariant\n"
#endif
          "---\n"
          "---@type fun(tid: LuaVariant): Context\n"
          "function GetContext(tid) end\n"
          "\n",
          f);

    fputs("---\n"
          "---SetContext 修改线程上下文\n"
          "---\n"
          "---@type fun(tid: LuaVariant, ctx: Context)\n"
          "function SetContext(tid, ctx) end\n"
          "\n",
          f);

    fputs("---\n"
          "---Ea2ModnameRva 将有效地址转换为模块名和RVA\n"
          "---地址不在任何已知模块中则返回nil,nil\n"
          "---\n"
          "---@type fun(ea: LuaVariant): string?, LuaVariant?\n"
          "function Ea2ModnameRva(ea) end\n"
          "\n",
          f);

    fputs("---\n"
          "---GetModBase 获取模块基址\n"
          "---无效模块则返回nil\n"
          "---\n"
          "---@type fun(name: string): LuaVariant?\n"
          "function GetModBase(name) end\n"
          "\n",
          f);

    fputs("---\n"
          "---StackTrace 获取线程调用堆栈\n"
          "---\n"
          "---@class StackFrame\n"
          "---@field ea LuaVariant 有效地址\n"
          "---@field modname string? 模块名\n"
          "---@field rva LuaVariant? RVA\n"
          "---\n"
          "---@type fun(tid: LuaVariant): StackFrame, ...\n"
          "function StackTrace(tid) end\n"
          "\n",
          f);

    fputs("---\n"
          "---LoadStr 读取字符串\n"
          "---\n"
          "---@type fun(addr: LuaVariant): string?\n"
          "function LoadStr(addr) end\n"
          "\n",
          f);

    fputs("---\n"
          "---Hide 隐藏调试器,以防检测\n"
          "---\n"
          "---@type fun()\n"
          "function Hide() end\n"
          "\n",
          f);

    fputs("---\n"
          "---FindFuncRva 查找指定dll中指定函数的rva\n"
          "---\n"
          "---@type fun(dllname: string, funcname: string): LuaVariant?\n"
          "function FindFuncRva(dllname, funcname) end\n"
          "\n",
          f);

    fputs("---\n"
          "---GetCppClassName 读取字符串\n"
          "---\n"
          "---@type fun(addr: LuaVariant): string?\n"
          "function GetCppClassName(addr) end\n"
          "\n",
          f);

    fputs("---\n"
          "---GetMemoryRegion 获取内存布局\n"
          "---\n"
          "---@class MemoryRegion\n"
          "---@field BaseAddress LuaVariant BaseAddress\n"
          "---@field AllocationBase LuaVariant AllocationBase\n"
          "---@field AllocationProtect LuaVariant AllocationProtect\n"
          "---@field RegionSize LuaVariant RegionSize\n"
          "---@field State LuaVariant State\n"
          "---@field Protect LuaVariant Protect\n"
          "---@field Type LuaVariant Type\n"
          "---\n"
          "---@type fun():MemoryRegion[] \n"
          "function GetMemoryRegion() end\n"
          "\n",
          f);

    fputs("---\n"
          "---FindMemory 查找内存\n"
          "---\n"
          "---@type fun(pattern: string): (LuaVariant,string), ... \n"
          "function FindMemory(pattern) end\n"
          "\n",
          f);

    fputs("---\n"
          "---ResetDir 清空文件夹\n"
          "---\n"
          "---@type fun(to_del_dir: string): boolean \n"
          "function ResetDir(to_del_dir) end\n"
          "\n",
          f);

    fputs("---\n"
          "---BsonEncode bson编码\n"
          "---\n"
          "---@type fun(obj: table): string \n"
          "function BsonEncode(obj) end\n"
          "\n",
          f);

    fputs("---\n"
          "---BsonDecode bson解码\n"
          "---\n"
          "---@type fun(value: string): table \n"
          "function BsonDecode(value) end\n"
          "\n",
          f);

    /* S4（plan_server.md §7.1）：GUI 自动化表 `gui` 的 LuaLS 骨架。
     * 只列签名与坐标口径；语义见 nvim/README.md 与 plan_server.md §7。 */
    fputs("---@class AutoDbgGui GUI 自动化（pyautogui 等价物）\n"
          "---坐标一律是屏幕绝对像素；hwnd 用整数；失败返回 nil, errmsg\n"
          "gui = {}\n"
          "\n"
          "---@return integer w, integer h 主屏宽高\n"
          "function gui.size() end\n"
          "---@return integer r, integer g, integer b\n"
          "function gui.pixel(x, y) end\n"
          "---@param opts table? {x=,y=,w=,h=,hwnd=,scale=,format=\"png\"|\"bmp\"|\"raw\"}\n"
          "---@return table|nil shot { w=, h=, format=, data=<二进制> }, string? err\n"
          "function gui.screenshot(opts) end\n"
          "---@return integer|nil bytes, string? err\n"
          "function gui.screenshot_to(path, opts) end\n"
          "---@return integer x, integer y\n"
          "function gui.position() end\n"
          "---@return boolean ok\n"
          "function gui.move(x, y, duration_ms) end\n"
          "---gui.click([x, y][, \"left\"|\"right\"|\"middle\"][, times])\n"
          "---@return boolean ok\n"
          "function gui.click(...) end\n"
          "---@return boolean ok\n"
          "function gui.down(button) end\n"
          "---@return boolean ok\n"
          "function gui.up(button) end\n"
          "---@return boolean ok\n"
          "function gui.drag(x, y, button, duration_ms) end\n"
          "---@return boolean ok\n"
          "function gui.scroll(delta, x, y) end\n"
          "---@return boolean ok\n"
          "function gui.write(text, interval_ms) end\n"
          "---@param key string 如 \"enter\" / \"f5\" / \"a\" / \"ctrl+c\"\n"
          "---@return boolean ok\n"
          "function gui.press(key, times) end\n"
          "---@return boolean ok\n"
          "function gui.hotkey(...) end\n"
          "---@return boolean ok\n"
          "function gui.key_down(key) end\n"
          "---@return boolean ok\n"
          "function gui.key_up(key) end\n"
          "---@return table[] { hwnd, pid, title, text, class, x, y, w, h, visible, enabled }\n"
          "function gui.windows() end\n"
          "---@return table[] 同上，多一个 id（GetDlgCtrlID）\n"
          "function gui.children(hwnd) end\n"
          "---@param spec string|table 标题子串或 {title=,class=,pid=}\n"
          "---@return integer|nil hwnd, string? err\n"
          "function gui.find(spec) end\n"
          "---@return integer hwnd\n"
          "function gui.active() end\n"
          "---@return boolean ok\n"
          "function gui.activate(hwnd) end\n"
          "---@return boolean ok\n"
          "function gui.focus(hwnd) end\n"
          "---@return integer x, integer y, integer w, integer h\n"
          "function gui.rect(hwnd) end\n"
          "---@return boolean ok\n"
          "function gui.set_rect(hwnd, x, y, w, h) end\n"
          "---@return boolean ok\n"
          "function gui.show(hwnd, cmd) end\n"
          "---@return boolean ok\n"
          "function gui.close(hwnd) end\n"
          "---@return integer|nil hwnd, string? err\n"
          "function gui.wait(spec, timeout_ms) end\n"
          "---@return string\n"
          "function gui.text(hwnd) end\n"
          "---@return boolean ok\n"
          "function gui.set_text(hwnd, text) end\n"
          "---@return boolean ok\n"
          "function gui.post(hwnd, msg, wparam, lparam) end\n"
          "---@return integer result\n"
          "function gui.send(hwnd, msg, wparam, lparam) end\n"
          "---@return boolean ok\n"
          "function gui.click_control(hwnd) end\n"
          "---@return nil\n"
          "function gui.sleep(ms) end\n"
          "---@return integer pid\n"
          "function gui.foreground_pid() end\n"
          "---@return boolean ok\n"
          "function gui.dpi_aware() end\n"
          "---@return boolean ok\n"
          "function gui.cursor_locked(lock) end\n"
          "\n",
          f);

    fclose(f);
    StringCchCopy(s, _countof(path) - (s - path), TEXT(".luarc.json"));
    f = _tfopen(path, TEXT("w"));
    if (!f)
        return;
    fputs("{\"workspace.library\":[\"./cdebugger.d.lua\"],\"runtime.version\":\"", f);
#ifndef JIT
    fputs("Lua 5.5", f);
#else
    fputs("Luajit 2.1", f);
#endif
    fputs("\"}", f);
    fclose(f);
}

int my_loadfile(lua_State *L, LPCTSTR path)
{
    /* P6.1：句柄用 RAII（原先 6 条 return 路径上散着 6 处 CloseHandle） */
    unique_handle hFile(CreateFile(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL));
    if (!hFile)
    {
        E("CreateFile");
        return -1;
    }
    ULARGE_INTEGER fileSize;
    fileSize.LowPart = GetFileSize(hFile.get(), &fileSize.HighPart);
    if (fileSize.LowPart == INVALID_FILE_SIZE)
    {
        DWORD code = GetLastError();
        if (code)
        {
            ERR("GetFileSize", code);
            return -1;
        }
    }
    if (!fileSize.QuadPart)
    {
        ErrorF(TEXT("\"%s\" size == 0\n"), path);
        return -1;
    }
#ifndef _WIN64
    if (fileSize.HighPart)
    {
        ErrorF(TEXT("\"%s\" size to large\n"), path);
        return -1;
    }
#endif
    unique_handle hMapping(CreateFileMapping(hFile.get(), NULL, PAGE_READONLY, 0, 0, NULL));
    if (!hMapping)
    {
        E("CreateFileMapping");
        return -1;
    }
    LPCSTR buf = (LPCSTR)MapViewOfFile(hMapping.get(), FILE_MAP_READ, 0, 0, 0);
    if (!buf)
    {
        E("MapViewOfFile");
        return -1;
    }
    LPSSTR name = tchar_to_utf8(path);
#ifdef _WIN64
    int r = luaL_loadbufferx(L, buf, fileSize.QuadPart, name, "t");
#else
    int r = luaL_loadbufferx(L, buf, fileSize.LowPart, name, "t");
#endif
    UnmapViewOfFile(buf);
    free_from_tchar(name);
    return r;
}

/* ---------------------------------------------------------------- 脚本目录工具（Q7）
 * `RemoveAllFiles` / `lua_reset_dir` 从 bindings.cpp 搬来：它们与调试无关（只操作脚本目录），
 * 按 Plan 5.7 / Q7 归「脚本状态」层。逻辑逐字保留（含失败时的 `E(...)` 与布尔返回）。 */

static bool RemoveAllFiles(LPCTSTR dir)
{
    WIN32_FIND_DATA wfd;
    ZeroMem(wfd);
    size_t n = _tcslen(dir);
    TCHAR  buf[0x8000];
    CopyMemT(buf, dir, n);
    if (buf[n] != TEXT('/') && buf[n] != TEXT('\\'))
        buf[n++] = TEXT('\\');
    CopyTStr(buf + n, TEXT("*"));
    HANDLE hFindFile = FindFirstFile(buf, &wfd);
    if (INVALID_HANDLE_VALUE == hFindFile)
    {
        E("FindFirstFile(%s)", buf);
        return false;
    }
    do
    {
        if (!_tcsicmp(wfd.cFileName, TEXT(".")))
            continue;
        if (!_tcsicmp(wfd.cFileName, TEXT("..")))
            continue;
        size_t m = _tcslen(wfd.cFileName);
        CopyMemT(buf + n, wfd.cFileName, m + 1);
        if (wfd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
        {
            if (!RemoveAllFiles(buf))
            {
                CloseHandle(hFindFile);
                return false;
            }
        }
        else
        {
            if (!DeleteFile(buf))
            {
                E("DeleteFile(%s)", buf);
                CloseHandle(hFindFile);
                return false;
            }
        }
    } while (FindNextFile(hFindFile, &wfd));
    buf[--n] = 0;
    if (!RemoveDirectory(buf))
    {
        E("RemoveDirectory(%s)", buf);
        CloseHandle(hFindFile);
        return false;
    }
    CloseHandle(hFindFile);
    return true;
}

int lua_reset_dir(lua_State *L)
{
    if (lua_gettop(L) != 1)
    {
        lua_pushliteral(L, "ResetDir(dir: string): nil");
        return lua_error(L);
    }
    LPXSTR root = utf8_to_tchar(lua_tostring(L, 1));
    if (PathFileExists(root))
    {
        if (PathIsDirectory(root))
        {
            if (!RemoveAllFiles(root))
            {
                free_from_tchar(root);
                lua_pushboolean(L, 0);
                return 1;
            }
        }
        else
        {
            if (!DeleteFile(root))
            {
                E("DeleteFile(%s)", root);
                free_from_tchar(root);
                lua_pushboolean(L, 0);
                return 1;
            }
        }
    }
    if (!CreateDirectory(root, NULL))
    {
        E("CreateDirectory(%s)", root);
        free_from_tchar(root);
        lua_pushboolean(L, 0);
    }
    else
    {
        free_from_tchar(root);
        lua_pushboolean(L, 1);
    }
    return 1;
}

/* 从 `_tmain` 搬来的引导 + 运行段（逐字；只把整段收进一个函数） */
/* =============================================================================
 * create_state_with_libs —— 建状态 → 注册 API → 烘焙内核库（S0，`plan_server.md` §5.1）
 *
 * 从 `run_script_file()` 的前半段**原样提出**（逐字一致）：server 的 `/run` 加载的是
 * **内存里的源码**（不落盘，D26），但状态初始化必须与文件路径完全相同，
 * 否则全局库/注册表会分叉。返回 nullptr = `luaL_newstate()` 失败。
 * ========================================================================== */
lua_State *create_state_with_libs(void)
{
    lua_State *L = luaL_newstate();
    if (L == nullptr)
        return nullptr;
    luaL_openlibs(L);
    register_lua_api(L); /* P4.2：注册表已搬到 registry.cpp（含 Var / FindMemoryContext 元表与 lv_init） */

    for (size_t i = 0; i < all_bytecodes_count; i++)
    {
        if (luaL_loadbuffer(L, (const char *)all_bytecodes[i].bytecode, all_bytecodes[i].len, all_bytecodes[i].path))
        {
            ErrorS(TEXT("加载 "));
            Error(all_bytecodes[i].name);
            ErrorS(TEXT(" 失败: "));
            size_t n;
            LPCSTR err = lua_tolstring(L, -1, &n);
#ifdef _UNICODE
            int    nn   = (int)n + 1;
            LPWSTR werr = utf8_to_utf16_n(err, &nn);
            ErrorN(werr, nn);
            free_(werr);
#else

            ErrorN(err, n);
#endif
            ErrorS("\n");
        }
        else
        {
            luaL_checktype(L, -1, LUA_TFUNCTION);
            lua_pcall(L, 0, 1, 0);
            lua_setglobal(L, all_bytecodes[i].name);
        }
    }
    return L;
}

int run_script_file(int argc, LPTSTR argv[])
{
    lua_State *L = create_state_with_libs();
    if (L == nullptr)
    {
        ErrorS(TEXT("luaL_newstate 失败\n"));
        return -1;
    }

    int r = my_loadfile(L, g_wfilepath);
    if (r > 0)
        ErrorS(L"加载\"");
    else
    {
        for (int i = 2; i < argc; i++)
        {
#ifdef _UNICODE
            int   n;
            LPSTR s = utf16_to_utf8(argv[i], &n);
            lua_pushlstring(L, s, n);
            free_(s);
#else
            lua_pushstring(L, argv[i]);
#endif
        }
        r = lua_pcall(L, argc > 2 ? argc - 2 : 0, LUA_MULTRET, 0);
        if (r > 0)
            ErrorS(L"运行\"");
    }
    if (r > 0)
    {
        Error(g_wfilepath);
        ErrorS(TEXT("\"失败: "));
        size_t n;
        LPCSTR err = lua_tolstring(L, -1, &n);
#ifdef _UNICODE
        int    nn   = (int)n + 1;
        LPWSTR werr = utf8_to_utf16_n(err, &nn);
        ErrorN(werr, nn);
        free_(werr);
#else

        ErrorN(err, n);
#endif
        ErrorS("\n");
    }
    lua_close(L);
    return r;
}
