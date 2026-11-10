/* =============================================================================
 * src/common/containers.cpp —— 容器的**非模板核心**（Plan 4.5：模板前端 + 非模板核心）
 *
 * 这样做的收益：无论 `IntrusiveList<T>` / `ArrayList<T>` 实例化多少种 T，
 * 实际代码只有这一份（模板前端内联后只是几条指令），体积可控。
 * ========================================================================== */
#include "containers.hpp"
#include <string.h> /* memcpy / memmove（零 CRT：见 src/sys/msvcrt.def） */
#include <intrin.h> /* __stosb（zero_bytes） */
#include <windows.h> /* PBYTE */

namespace autodbg::common
{
namespace detail
{
/* 节点约定：首字段是「指向下一个节点的指针」 */
struct Node
{
    void *next;
};

void zero_bytes(void *p, size_t n) noexcept
{
    __stosb((PBYTE)p, 0, n);
}

void *intrusive_pop_front(void **head) noexcept
{
    void *n = *head;
    if (n) *head = static_cast<Node *>(n)->next;
    return n;
}

void intrusive_push_front(void **head, void *node) noexcept
{
    static_cast<Node *>(node)->next = *head;
    *head                           = node;
}

bool intrusive_remove(void **head, void *node) noexcept
{
    if (!head || !node) return false;
    Node *target = static_cast<Node *>(node);

    /* 头结点 */
    Node *cur = static_cast<Node *>(*head);
    if (cur == target)
    {
        *head              = cur->next;
        target->next       = nullptr;
        return true;
    }
    /* 其余结点（单链表，O(n)） */
    while (cur && cur->next)
    {
        Node *nxt = static_cast<Node *>(cur->next);
        if (nxt == target)
        {
            cur->next    = nxt->next;
            target->next = nullptr;
            return true;
        }
        cur = nxt;
    }
    return false;
}

size_t intrusive_count(const void *head) noexcept
{
    size_t n = 0;
    for (const Node *p = static_cast<const Node *>(head); p; p = static_cast<const Node *>(p->next)) n++;
    return n;
}

void array_init(ArrayCore *a, size_t elem_size) noexcept
{
    a->data      = nullptr;
    a->count     = 0;
    a->capacity  = 0;
    a->elem_size = elem_size ? elem_size : 1;
}

void array_clear(ArrayCore *a) noexcept
{
    release(a->data);
    a->data     = nullptr;
    a->count    = 0;
    a->capacity = 0;
}

bool array_reserve(ArrayCore *a, size_t needed) noexcept
{
    if (needed <= a->capacity) return true;
    /* 增长策略：至少 4，且按 2 倍增长（与常见实现一致，摊还 O(1)） */
    size_t cap = a->capacity ? a->capacity : 4;
    while (cap < needed) cap *= 2;
    a->data     = resize(a->data, cap * a->elem_size);
    a->capacity = cap;
    return true; /* resize 失败即 fail-fast，不会走到这里 */
}

void *array_push_back(ArrayCore *a, const void *elem) noexcept
{
    array_reserve(a, a->count + 1);
    void *slot = static_cast<char *>(a->data) + a->count * a->elem_size;
    if (elem) memcpy(slot, elem, a->elem_size);
    a->count++;
    return slot;
}

bool array_erase(ArrayCore *a, size_t index) noexcept
{
    if (index >= a->count) return false;
    char *base = static_cast<char *>(a->data);
    if (index + 1 < a->count)
        memmove(base + index * a->elem_size, base + (index + 1) * a->elem_size, (a->count - index - 1) * a->elem_size);
    a->count--;
    return true;
}
}
}
