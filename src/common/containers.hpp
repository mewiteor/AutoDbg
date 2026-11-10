#ifndef AUTODBG_COMMON_CONTAINERS_HPP
#define AUTODBG_COMMON_CONTAINERS_HPP
/* =============================================================================
 * src/common/containers.hpp —— 轻量容器（Plan.md 4.3 / P2.4）
 *
 * 两个容器，都是「**模板前端 + 非模板核心**」（Plan 4.5 的体积纪律）：
 *   * `IntrusiveList<T>`：侵入式链表，**零额外分配**，直接替换现有 `next` 链表。
 *       要求：`T` 的**首个成员**必须是指向下一个节点的指针（现有
 *       `ThreadInfo` / `BreakpointInfo` / `PEInfo` 都满足：第一个字段就是 `next`）。
 *   * `ArrayList<T>`：按字节增长的核心 + 模板前端，用于 `FuncInfo` 数组 /
 *       可增长缓冲（替代手工 `realloc`）。
 *
 * 约束：
 *   * 不引入 `std::`；分配统一走 `autodbg::common::alloc/release/resize`；
 *   * `ArrayList` 的 `push_back` 使用按字节拷贝 → 要求 `T` 可平凡复制
 *     （本项目容器只承载 POD/兼容 C 的结构体）。
 *
 * 迭代：直接配合 `for (T* p = list.head(); p; p = p->next)` 使用；
 *   暂不提供迭代器（保持模板代码最薄，等真的需要再加）。
 * ========================================================================== */
#include "allocator.hpp"
#include <stddef.h>

namespace autodbg::common
{
    namespace detail
    {
        /* ---- 侵入式链表核心：节点首字段为 next 指针 ---- */
        void  *intrusive_pop_front(void **head) noexcept;
        void   intrusive_push_front(void **head, void *node) noexcept;
        bool   intrusive_remove(void **head, void *node) noexcept;
        size_t intrusive_count(const void *head) noexcept;

        /* ---- 动态数组核心（按字节操作） ---- */
        struct ArrayCore
        {
            void  *data;
            size_t count;
            size_t capacity;
            size_t elem_size;
        };

        void  array_init(ArrayCore *a, size_t elem_size) noexcept;
        void  array_clear(ArrayCore *a) noexcept;
        bool  array_reserve(ArrayCore *a, size_t needed) noexcept;
        void *array_push_back(ArrayCore *a, const void *elem) noexcept; /* 返回新元素指针，或 nullptr（预留失败） */
        bool  array_erase(ArrayCore *a, size_t index) noexcept;

        /* 把 n 字节清零（`__stosb`；放在非模板核心以保持体积纪律） */
        void zero_bytes(void *p, size_t n) noexcept;
    } // namespace detail

    /* ============================ IntrusiveList ============================ */
    template<class T>
    class IntrusiveList
    {
        public:
        IntrusiveList() noexcept
            : m_head(nullptr)
        {
        }

        IntrusiveList(const IntrusiveList &)            = delete;
        IntrusiveList &operator=(const IntrusiveList &) = delete;

        bool empty() const noexcept
        {
            return m_head == nullptr;
        }

        T *head() const noexcept
        {
            return static_cast<T *>(m_head);
        }

        size_t count() const noexcept
        {
            return detail::intrusive_count(m_head);
        }

        void push_front(T *node) noexcept
        {
            detail::intrusive_push_front(&m_head, node);
        }

        T *pop_front() noexcept
        {
            return static_cast<T *>(detail::intrusive_pop_front(&m_head));
        }

        /* 从链表中摘除 node（O(n) 查找；命中返回 true）。**不释放 node** */
        bool remove(T *node) noexcept
        {
            return detail::intrusive_remove(&m_head, node);
        }

        private:
        void *m_head;
    };

    /* ============================== ArrayList ============================== */
    template<class T>
    class ArrayList
    {
        public:
        ArrayList() noexcept
        {
            detail::array_init(&m_core, sizeof(T));
        }

        ~ArrayList() noexcept
        {
            detail::array_clear(&m_core);
        }

        ArrayList(const ArrayList &)            = delete;
        ArrayList &operator=(const ArrayList &) = delete;

        size_t size() const noexcept
        {
            return m_core.count;
        }

        bool empty() const noexcept
        {
            return m_core.count == 0;
        }

        T *data() noexcept
        {
            return static_cast<T *>(m_core.data);
        }

        const T *data() const noexcept
        {
            return static_cast<const T *>(m_core.data);
        }

        T &operator[](size_t i) noexcept
        {
            return data()[i];
        }

        const T &operator[](size_t i) const noexcept
        {
            return data()[i];
        }

        bool push_back(const T &elem) noexcept
        {
            return detail::array_push_back(&m_core, &elem) != nullptr;
        }

        bool reserve(size_t needed) noexcept
        {
            return detail::array_reserve(&m_core, needed);
        }

        bool erase(size_t index) noexcept
        {
            return detail::array_erase(&m_core, index);
        }

        void clear() noexcept
        {
            detail::array_clear(&m_core);
        }

        private:
        detail::ArrayCore m_core;
    };

    /* ======================= OwnedIntrusiveList（变长节点 + 自动释放） =======================
     * 与上面 `IntrusiveList<T>` 的关系：「同一个侵入式链表，但**拥有节点**」——
     *   · 析构/`clear()` 会 `common::release()` 每个节点；
     *   · `create_front(extra_bytes)` 按 `sizeof(T) + extra_bytes` 分配（给**尾数组**用）。
     *
     * 回答「能不能支持变长节点（`new char[size]` + placement new）」：
     *   · **能**，但**不需要 placement new** —— 本项目的节点全是 POD
     *     （`PEInfo`/`BreakpointInfo`/`ThreadInfo`），无构造函数可跑；
     *     分配后直接写字段即可（旧代码就是 `alloc_(sizeof(X) + n*sizeof(TCHAR))`）。
     *   · placement new 只在节点有**非平凡构造**时才必要；本项目禁用标准库、也不引入
     *     非 POD 节点 → 不必引入它（顺带避开一个坑：`<new>` 是被禁的标准库头，
     *     真要用手得自己声明 `void* operator new(size_t, void*)`）。
     *   · 尾数组的元素个数由调用方自己算（与旧代码一致）：
     *     `TCHAR Path[1]` 这种写法下，要装 n 个字符就传 `n * sizeof(TCHAR)`。
     *
     * 用途：把「裸指针链表 + 手工 `alloc_`/`free_` + 会话析构里手写清理」收敛成 RAII，
     *   同时保留现有遍历写法（`for (T *p = list.head(); p; p = p->next)`）。 */
    template<class T>
    class OwnedIntrusiveList
    {
        public:
        OwnedIntrusiveList() noexcept
            : m_head(nullptr)
        {
        }

        ~OwnedIntrusiveList() noexcept
        {
            clear();
        }

        OwnedIntrusiveList(const OwnedIntrusiveList &)            = delete;
        OwnedIntrusiveList &operator=(const OwnedIntrusiveList &) = delete;

        bool empty() const noexcept
        {
            return m_head == nullptr;
        }

        T *head() const noexcept
        {
            return static_cast<T *>(m_head);
        }

        size_t count() const noexcept
        {
            return detail::intrusive_count(m_head);
        }

        /* 分配 + **先清零、再入链**（顺序关键：清零会把 `next` 一起抹掉 ✗）。
         * 语义：返回一个「整块已清零」的节点（含尾数组）——调用方直接填字段即可。
         * ❗旧代码写的是 `ZeroMem(*info)`（宏里 `sizeof(s)` 只拿到指针宽度 = 4 字节 ✗）
         *   + 然后 `info->next = head` 手动把 `next` 设回 → 本方法把这两步合成一步且做得正确。 */
        T *create_front(size_t extra_bytes = 0) noexcept
        {
            void *raw = alloc(sizeof(T) + extra_bytes);
            detail::zero_bytes(raw, sizeof(T) + extra_bytes);
            detail::intrusive_push_front(&m_head, raw);
            return static_cast<T *>(raw);
        }

        /* 接管一个**已分配**的节点（析构/clear 时会被 release）。 */
        void push_front(T *node) noexcept
        {
            detail::intrusive_push_front(&m_head, node);
        }

        /* 摘除但不释放（所有权交回调用方） */
        bool remove(T *node) noexcept
        {
            return detail::intrusive_remove(&m_head, node);
        }

        /* 摘除并释放 */
        bool remove_and_free(T *node) noexcept
        {
            if (!detail::intrusive_remove(&m_head, node))
                return false;
            release(node);
            return true;
        }

        /* 释放全部节点并置空 */
        void clear() noexcept
        {
            while (m_head)
            {
                T *p = static_cast<T *>(detail::intrusive_pop_front(&m_head));
                release(p);
            }
        }

        private:
        void *m_head;
    };

    template<typename K, typename V>
    class RBTree
    {
        enum Color : unsigned char
        {
            RED = 1,
            BLACK
        };

        struct Node
        {
            K     key;
            V     val;
            Color c;
            Node *l, *r, *p;

            Node(const K &k, const V &v, Color co = RED)
                : key(k)
                , val(v)
                , c(co)
                , l(nullptr)
                , r(nullptr)
                , p(nullptr)
            {
            }
        };

        Node *root = nullptr;

        void rotL(Node *x)
        {
            Node *y = x->r;
            x->r    = y->l;
            if (y->l)
                y->l->p = x;
            y->p = x->p;
            if (!x->p)
                root = y;
            else if (x == x->p->l)
                x->p->l = y;
            else
                x->p->r = y;
            y->l = x;
            x->p = y;
        }

        void rotR(Node *x)
        {
            Node *y = x->l;
            x->l    = y->r;
            if (y->r)
                y->r->p = x;
            y->p = x->p;
            if (!x->p)
                root = y;
            else if (x == x->p->r)
                x->p->r = y;
            else
                x->p->l = y;
            y->r = x;
            x->p = y;
        }

        void fixInsert(Node *z)
        {
            while (z && z != root && z->p->c == RED)
            {
                if (z->p == z->p->p->l)
                {
                    Node *y = z->p->p->r;
                    if (y && y->c == RED)
                    {
                        z->p->c = y->c = BLACK;
                        z->p->p->c     = RED;
                        z              = z->p->p;
                    }
                    else
                    {
                        if (z == z->p->r)
                        {
                            z = z->p;
                            rotL(z);
                        }
                        z->p->c    = BLACK;
                        z->p->p->c = RED;
                        rotR(z->p->p);
                    }
                }
                else
                {
                    Node *y = z->p->p->l;
                    if (y && y->c == RED)
                    {
                        z->p->c = y->c = BLACK;
                        z->p->p->c     = RED;
                        z              = z->p->p;
                    }
                    else
                    {
                        if (z == z->p->l)
                        {
                            z = z->p;
                            rotR(z);
                        }
                        z->p->c    = BLACK;
                        z->p->p->c = RED;
                        rotL(z->p->p);
                    }
                }
            }
            if (root)
                root->c = BLACK;
        }

        void transplant(Node *u, Node *v)
        {
            if (!u->p)
                root = v;
            else if (u == u->p->l)
                u->p->l = v;
            else
                u->p->r = v;
            if (v)
                v->p = u->p;
        }

        static Node *treeMin(Node *x)
        {
            while (x && x->l) x = x->l;
            return x;
        }

        static Node *treeMax(Node *x)
        {
            while (x && x->r) x = x->r;
            return x;
        }

        static Node *successor(Node *x)
        {
            if (x->r)
                return treeMin(x->r);
            Node *y = x->p;
            while (y && x == y->r)
            {
                x = y;
                y = y->p;
            }
            return y;
        }

        static Node *predecessor(Node *x)
        {
            if (x->l)
                return treeMax(x->l);
            Node *y = x->p;
            while (y && x == y->l)
            {
                x = y;
                y = y->p;
            }
            return y;
        }

        void fixDelete(Node *x, Node *xp)
        {
            while (x != root && (!x || x->c == BLACK))
            {
                if (x == xp->l)
                {
                    Node *s = xp->r;
                    if (s && s->c == RED)
                    {
                        s->c  = BLACK;
                        xp->c = RED;
                        rotL(xp);
                        s = xp->r;
                    }
                    bool ll = !s->l || s->l->c == BLACK;
                    bool lr = !s->r || s->r->c == BLACK;
                    if (ll && lr)
                    {
                        if (s)
                            s->c = RED;
                        x  = xp;
                        xp = x ? x->p : nullptr;
                    }
                    else
                    {
                        if (lr)
                        {
                            if (s->l)
                                s->l->c = BLACK;
                            s->c = RED;
                            rotR(s);
                            s = xp->r;
                        }
                        s->c  = xp->c;
                        xp->c = BLACK;
                        if (s->r)
                            s->r->c = BLACK;
                        rotL(xp);
                        x = root;
                    }
                }
                else
                {
                    Node *s = xp->l;
                    if (s && s->c == RED)
                    {
                        s->c  = BLACK;
                        xp->c = RED;
                        rotR(xp);
                        s = xp->l;
                    }
                    bool rl = !s->r || s->r->c == BLACK;
                    bool rr = !s->l || s->l->c == BLACK;
                    if (rl && rr)
                    {
                        if (s)
                            s->c = RED;
                        x  = xp;
                        xp = x ? x->p : nullptr;
                    }
                    else
                    {
                        if (rr)
                        {
                            if (s->r)
                                s->r->c = BLACK;
                            s->c = RED;
                            rotL(s);
                            s = xp->l;
                        }
                        s->c  = xp->c;
                        xp->c = BLACK;
                        if (s->l)
                            s->l->c = BLACK;
                        rotR(xp);
                        x = root;
                    }
                }
            }
            if (x)
                x->c = BLACK;
        }

        static void clear(Node *n)
        {
            if (!n)
                return;
            clear(n->l);
            clear(n->r);
            delete n;
        }

        Node *findNode(const K &k)
        {
            Node *x = root;
            while (x)
            {
                if (k < x->key)
                    x = x->l;
                else if (x->key < k)
                    x = x->r;
                else
                    return x;
            }
            return nullptr;
        }

        public:
        struct Iterator
        {
            Node *node;

            Iterator()
                : node(nullptr)
            {
            }

            Iterator(Node *n)
                : node(n)
            {
            }

            bool operator!=(const Iterator &o) const
            {
                return node != o.node;
            }

            bool operator==(const Iterator &o) const
            {
                return node == o.node;
            }

            Iterator &operator++()
            {
                node = node ? successor(node) : nullptr;
                return *this;
            }

            Iterator operator++(int)
            {
                Iterator tmp = *this;
                ++(*this);
                return tmp;
            }

            Iterator &operator--()
            {
                node = node ? predecessor(node) : nullptr;
                return *this;
            }

            Iterator operator--(int)
            {
                Iterator tmp = *this;
                --(*this);
                return tmp;
            }

            struct Pair
            {
                K &first;
                V &second;
            };

            Pair operator*()
            {
                return {node->key, node->val};
            }

            K &key()
            {
                return node->key;
            }

            V &value()
            {
                return node->val;
            }
        };

        ~RBTree()
        {
            clear(root);
        }

        Iterator begin()
        {
            return Iterator(treeMin(root));
        }

        Iterator end()
        {
            return Iterator(nullptr);
        }

        Iterator find(const K &k)
        {
            return Iterator(findNode(k));
        }

        Iterator insert(const K &k, const V &v)
        {
            Node *y = nullptr, *x = root;
            while (x)
            {
                y = x;
                x = k < x->key ? x->l : x->r;
            }
            Node *z = new Node(k, v);
            z->p    = y;
            if (!y)
                root = z;
            else if (k < y->key)
                y->l = z;
            else
                y->r = z;
            fixInsert(z);
            return Iterator(z);
        }

        void remove(Iterator it)
        {
            Node *z = it.node;
            if (!z)
                return;
            Node *y = z, *x = nullptr, *xp = nullptr;
            Color yc = y->c;
            if (!z->l)
            {
                x  = z->r;
                xp = z->p;
                transplant(z, z->r);
            }
            else if (!z->r)
            {
                x  = z->l;
                xp = z->p;
                transplant(z, z->l);
            }
            else
            {
                y  = treeMin(z->r);
                yc = y->c;
                x  = y->r;
                if (y->p == z)
                {
                    xp = y;
                    if (x)
                        x->p = y;
                }
                else
                {
                    xp = y->p;
                    transplant(y, y->r);
                    y->r    = z->r;
                    y->r->p = y;
                }
                transplant(z, y);
                y->l    = z->l;
                y->l->p = y;
                y->c    = z->c;
            }
            delete z;
            if (yc == BLACK && xp)
                fixDelete(x, xp);
        }

        operator bool() const
        {
            return !!root;
        }
    };
} // namespace autodbg::common

#endif /* AUTODBG_COMMON_CONTAINERS_HPP */
