#ifndef AUTODBG_SYS_PACK_ERROR_HPP
#define AUTODBG_SYS_PACK_ERROR_HPP
/* =============================================================================
 * src/sys/pack/error.hpp —— 打包子系统的错误载体（Plan §3.1 / §5 风险对策）
 *
 * 职责：给 payload / module_loader 一个**不分配堆内存**的错误描述，供启动器打印诊断。
 * 来源：新写（Plan P2）。原型 `tools/PackEXE` 一律 MessageBox，无法回退、无法断言。
 * 与旧实现的差异：把「弹窗」改成「结构体 + 返回值」，调用方决定打印/回退/放弃。
 *
 * ❗为什么不直接用 `const char *detail` 指向模块镜像里的符号名：
 *   `map()` 的失败路径必须**立刻回滚**（F12），回滚后镜像里的名字就失效了。
 *   所以需要「字符串来自模块」时一律用 `AUTODBG_PACK_FAIL_STR` 拷进 `text[64]`，
 *   `detail` 再指向 `text` —— 这样「打印诊断」与「回滚」的先后顺序不再有耦合。
 * ========================================================================== */

namespace autodbg::sys::pack
{
/* 失败发生在哪一步（顺序即 map() 的阶段顺序，便于定位） */
enum class stage
{
    none = 0,
    payload,      /* 载荷结构 / 解压（§2.4） */
    header,       /* DOS/NT 头、机器类型、尺寸 */
    section,      /* 节拷贝 / 页保护 */
    reloc,        /* 重定位 */
    import,       /* 导入解析 */
    exception,    /* x64 展开表注册（F5） */
    entry,        /* 入口 / TLS */
};

struct load_error
{
    stage       where;      /* 失败阶段；none = 没失败 */
    const char *detail;     /* 静态字面量，或指向下面的 text[]；可为 nullptr */
    unsigned    code;       /* Win32 错误码 / SRes / 节索引 / reloc 类型；0 = 无 */
    unsigned    line;       /* 源码行号（排查用） */
    char        text[64];   /* detail 需要在回滚后仍然有效时的落点 */

    void clear()
    {
        where  = stage::none;
        detail = nullptr;
        code   = 0;
        line   = 0;
        text[0] = '\0';
    }
    bool ok() const { return where == stage::none; }

    /* 把 name 拷进 text（截断到 63 字节）并把 detail 指向它 */
    void set_text(const char *name)
    {
        unsigned i = 0;
        if (name != nullptr)
        {
            for (; name[i] != '\0' && i + 1 < sizeof text; i++)
                text[i] = name[i];
        }
        text[i] = '\0';
        detail  = text;
    }
};

/* 记录失败：detail 是静态字面量（不随镜像释放而失效） */
#define AUTODBG_PACK_FAIL(err, st, det, cd) \
    do { \
        if ((err) != nullptr) \
        { \
            (err)->where  = (st); \
            (err)->detail = (det); \
            (err)->code   = (unsigned)(cd); \
            (err)->line   = __LINE__; \
        } \
    } while (0)

/* 记录失败：detail 来自模块镜像 → 先拷进 err->text，之后可以安全回滚 */
#define AUTODBG_PACK_FAIL_STR(err, st, name, cd) \
    do { \
        if ((err) != nullptr) \
        { \
            (err)->where = (st); \
            (err)->code  = (unsigned)(cd); \
            (err)->line  = __LINE__; \
            (err)->set_text(name); \
        } \
    } while (0)
}

#endif /* AUTODBG_SYS_PACK_ERROR_HPP */
