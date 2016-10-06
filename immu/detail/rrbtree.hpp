
#pragma once

#include <immu/config.hpp>
#include <immu/detail/vnode.hpp>

#include <cassert>
#include <memory>
#include <numeric>

namespace immu {
namespace detail {

template <typename T,
          int B,
          typename MemoryPolicy>
struct rrbtree
{
    using heap_policy = typename MemoryPolicy::heap;
    using refcount    = typename MemoryPolicy::refcount;

    using node_t = vnode<T, B, MemoryPolicy>;
    using heap   = typename node_t::heap;

    std::size_t size;
    unsigned    shift;
    node_t*     root;
    node_t*     tail;

    static const rrbtree empty;

    rrbtree(std::size_t sz, unsigned sh, node_t* r, node_t* t)
        : size{sz}, shift{sh}, root{r}, tail{t}
    {
        assert(check_tree());
    }

    rrbtree(const rrbtree& other)
        : rrbtree{other.size, other.shift, other.root, other.tail}
    {
        inc();
    }

    rrbtree(rrbtree&& other)
        : rrbtree{empty}
    {
        swap(*this, other);
    }

    rrbtree& operator=(const rrbtree& other)
    {
        auto next{other};
        swap(*this, next);
        return *this;
    }

    rrbtree& operator=(rrbtree&& other)
    {
        swap(*this, other);
        return *this;
    }

    friend void swap(rrbtree& x, rrbtree& y)
    {
        using std::swap;
        swap(x.size,  y.size);
        swap(x.shift, y.shift);
        swap(x.root,  y.root);
        swap(x.tail,  y.tail);
    }

    ~rrbtree()
    {
        dec();
    }

    void inc() const
    {
        root->inc();
        tail->inc();
    }

    struct dec_traversal
    {
        bool predicate(node_t* p)
        { return p->dec(); }
        void visit_inner(node_t* p)
        { node_t::delete_inner(p); }
        void visit_leaf(node_t* p, unsigned n)
        { node_t::delete_leaf(p, n); }
    };

    void dec() const
    {
        traverse(dec_traversal{});
    }

    static void dec_node(node_t* n, unsigned shift, std::size_t size)
    {
        traverse(dec_traversal{}, n, shift, size);
    }

    static void dec_node(node_t* n, unsigned shift)
    {
        traverse(dec_traversal{}, n, shift);
    }

    auto tail_size() const
    {
        return size - tail_offset();
    }

    auto tail_offset() const
    {
        assert(!root->sizes() || root->slots());
        return
            root->sizes()   ? root->sizes() [root->slots() - 1] :
            size            ? (size - 1) & ~mask<B>
            /* otherwise */ : 0;
    }

    template <typename Step, typename State>
    struct reduce_traversal
    {
        Step fn;
        State acc;

        bool predicate(node_t*) { return true; }
        void visit_inner(node_t* n) {}
        void visit_leaf(node_t* n, unsigned elems)
        {
            acc = std::accumulate(n->leaf(),
                                  n->leaf() + elems,
                                  acc,
                                  fn);
        }
    };

    template <typename Step, typename State>
    State reduce(Step step, State init) const
    {
        auto t = reduce_traversal<Step, State>{step, init};
        traverse(t);
        return t.acc;
    }

    template <typename Traversal>
    static void traverse_node_relaxed(Traversal&& t,
                                      node_t* node,
                                      unsigned level,
                                      std::size_t size = 0)
    {
        assert(level > 0);
        assert(size || node->sizes());
        if (auto s = node->sizes()) {
            if (t.predicate(node)) {
                auto count = node->slots();
                auto next = level - B;
                if (next == 0) {
                    auto c = node->inner();
                    auto last_s = 0u;
                    for (auto i = 0u; i < count; ++i) {
                        if (t.predicate(c[i]))
                            t.visit_leaf(c[i], s[i] - last_s);
                        last_s = s[i];
                    }
                } else {
                    auto c = node->inner();
                    auto last_s = 0u;
                    for (auto i = 0u; i < count; ++i) {
                        traverse_node_relaxed(t, c[i], next, s[i] - last_s);
                        last_s = s[i];
                    }
                }
                t.visit_inner(node);
            }
        } else {
            traverse_node_regular(t, node, level, size);
        }
    }

    template <typename Traversal>
    static void traverse_node_regular(Traversal&& t,
                                      node_t* node,
                                      unsigned level,
                                      std::size_t size)
    {
        assert(level > 0);
        assert(size > 0);
        if (t.predicate(node)) {
            auto next = level - B;
            auto last = ((size - 1) >> level) & mask<B>;
            if (next == 0) {
                auto i = node->inner();
                for (auto e = i + last; i != e; ++i)
                    if (t.predicate(*i))
                        t.visit_leaf(*i, branches<B>);
                if (t.predicate(*i)) {
                    auto last_size = size & mask<B>;
                    t.visit_leaf(*i, last_size ? last_size : branches<B>);
                }
            } else {
                auto i = node->inner();
                for (auto e = i + last; i != e; ++i)
                    traverse_node_full(t, *i, next);
                traverse_node_regular(t, *i, next, size);
            }
            t.visit_inner(node);
        }
    }

    template <typename Traversal>
    static void traverse_node_full(Traversal&& t, node_t* node, unsigned level)
    {
        assert(level > 0);
        if (t.predicate(node)) {
            auto next = level - B;
            if (next == 0) {
                for (auto i = node->inner(), e = i + branches<B>; i != e; ++i)
                    if (t.predicate(*i))
                        t.visit_leaf(*i, branches<B>);
            } else {
                for (auto i = node->inner(), e = i + branches<B>; i != e; ++i)
                    traverse_node_full(t, *i, next);
            }
            t.visit_inner(node);
        }
    }

    template <typename Traversal>
    void traverse(Traversal&& t) const
    {
        auto to = tail_offset();
        auto ts = size - to;
        if (ts < size)
            traverse_node_relaxed(t, root, shift, to);
        else if (t.predicate(root))
            t.visit_inner(root);
        if (t.predicate(tail))
            t.visit_leaf(tail, ts);
    }

    template <typename Traversal>
    static void traverse(Traversal&& t,
                         node_t* n,
                         unsigned shift)
    {
        traverse_node_relaxed(t, n, shift);
    }

    template <typename Traversal>
    static void traverse(Traversal&& t,
                         node_t* n,
                         unsigned shift,
                         std::size_t size)
    {
        traverse_node_relaxed(t, n, shift, size);
    }

    const T* array_for(std::size_t& index) const
    {
        assert(index < size);
        auto tail_off = tail_offset();
        if (index >= tail_offset()) {
            index -= tail_off;
            return tail->leaf();
        } else {
            auto node = root;
            for (auto level = shift; level; level -= B) {
                auto sizes = node->sizes();
                if (sizes) {
                    auto node_index = (index >> level) & mask<B>;
                    while (sizes[node_index] <= index) ++node_index;
                    if (node_index) index -= sizes[node_index - 1];
                    node = node->inner() [node_index];
                } else {
                    do {
                        node = node->inner() [(index >> level) & mask<B>];
                    } while (level -= B);
                    return node->leaf();
                }
            }
            return node->leaf();
        }
    }

    node_t* make_path(unsigned level, node_t* node) const
    {
        assert(node->kind() == node_t::leaf_kind);
        return level == 0
            ? node
            : node_t::make_inner(make_path(level - B, std::move(node)));
    }

    node_t* push_tail_relaxed(unsigned level,
                              node_t* parent,
                              node_t* tail,
                              unsigned tail_size) const
    {
        if (auto sizes      = parent->sizes()) {
            auto idx        = parent->slots() - 1;
            auto children   = idx ? sizes[idx] - sizes[idx - 1] : sizes[idx];
            auto new_idx    = children == 1 << level || level == B
                ? idx + 1 : idx;
            if (new_idx >= branches<B>)
                return nullptr;
            auto new_child  =
                level == B       ? tail :
                idx == new_idx   ? push_tail_relaxed(level - B,
                                                     parent->inner()[idx],
                                                     tail,
                                                     tail_size)
                /* otherwise */  : make_path(level - B, tail);
            // if there was no space on the leftmost branch, we see if
            // there is an empty slot to its left and we just put it there
            if (!new_child) {
                if (idx == new_idx && idx + 1 < branches<B>) {
                    ++new_idx;
                    new_child = make_path(level - B, tail);
                } else
                    return nullptr;
            }
            auto new_parent = node_t::copy_inner_r(parent, new_idx);
            new_parent->inner()[new_idx] = new_child;
            new_parent->sizes()[new_idx] = sizes[idx] + tail_size;
            new_parent->slots() = new_idx + 1;
            return new_parent;
        } else {
            return push_tail_regular(level, parent, tail);
        }
    }

    node_t* push_tail_regular(unsigned level,
                              node_t* parent,
                              node_t* tail) const
    {
        auto idx        = ((size - branches<B> - 1) >> level) & mask<B>;
        auto new_idx    = ((size - 1) >> level) & mask<B>;
        auto new_parent = node_t::copy_inner(parent, new_idx);
        new_parent->inner()[new_idx] =
            level == B       ? tail :
            idx == new_idx   ? push_tail_regular(level - B,
                                                 parent->inner()[idx],
                                                 tail)
            /* otherwise */  : make_path(level - B, tail);
        assert(new_idx < branches<B>);
        return new_parent;
    }

    std::tuple<unsigned, node_t*>
    push_tail(node_t* root, unsigned shift, std::size_t size,
              node_t* tail, unsigned tail_size) const
    {
        if (auto sizes = root->sizes()) {
            auto new_root = push_tail_relaxed(shift, root, tail, tail_size);
            if (new_root)
                return { shift, new_root };
            else {
                auto new_path = make_path(shift, tail);
                auto new_root = node_t::make_inner_r(root->inc(),
                                                     size,
                                                     new_path,
                                                     tail_size);
                return { shift + B, new_root };
            }
        } else if (size >> B >= 1u << shift) {
            auto new_path = make_path(shift, tail);
            auto new_root = node_t::make_inner(root->inc(), new_path);
            return { shift + B, new_root };
        } else {
            auto new_root = push_tail_regular(shift, root, tail);
            return { shift, new_root };
        }
    }

    rrbtree push_back(T value) const
    {
        auto ts = tail_size();
        if (ts < branches<B>) {
            auto new_tail = node_t::copy_leaf_emplace(tail, ts,
                                                      std::move(value));
            return { size + 1, shift, root->inc(), new_tail };
        } else {
            using std::get;
            auto new_tail = node_t::make_leaf(std::move(value));
            auto tail_off = tail_offset();
            auto new_root = push_tail(root, shift, tail_off,
                                      tail->inc(), size - tail_off);
            return { size + 1, get<0>(new_root), get<1>(new_root), new_tail };
        }
    }

    const T& get(std::size_t index) const
    {
        auto arr = array_for(index);
        return arr [index & mask<B>];
    }

    template <typename FnT>
    rrbtree update(std::size_t idx, FnT&& fn) const
    {
        auto tail_off = tail_offset();
        if (idx >= tail_off) {
            auto new_tail  = node_t::copy_leaf(tail, size - tail_off);
            auto& item     = new_tail->leaf() [idx & mask<B>];
            auto new_value = std::forward<FnT>(fn) (std::move(item));
            item = std::move(new_value);
            return { size, shift, root->inc(), new_tail };
        } else {
            auto new_root = do_update(shift,
                                      root,
                                      idx,
                                      tail_offset(),
                                      std::forward<FnT>(fn));
            return { size, shift, new_root, tail->inc() };
        }
    }

    template <typename FnT>
    node_t* do_update_leaf(node_t* node, std::size_t idx, FnT&& fn) const
    {
        auto new_node  = node_t::copy_leaf(node, branches<B>);
        auto& item     = new_node->leaf() [idx & mask<B>];
        auto new_value = std::forward<FnT>(fn) (std::move(item));
        item = std::move(new_value);
        return new_node;
    }

    template <typename FnT>
    node_t* do_update(unsigned level,
                      node_t* node,
                      std::size_t idx,
                      std::size_t count,
                      FnT&& fn) const
    {
        auto sizes = node->sizes();
        if (level == 0) {
            return do_update_leaf(node, idx, std::forward<FnT>(fn));
        } else if (sizes) {
            auto offset   = (idx >> level) & mask<B>;
            while (sizes[offset] <= idx) ++offset;
            auto new_node = node_t::copy_inner_r(node, node->slots());
            node->inner()[offset]->dec_unsafe();
            new_node->inner()[offset] =
                do_update(level - B,
                          node->inner()[offset],
                          offset ? idx - sizes[offset - 1] : idx,
                          sizes[idx],
                          std::forward<FnT>(fn));
            return new_node;
        } else {
            return do_update_regular(level, node, idx, count,
                                     std::forward<FnT>(fn));
        }
    }

    template <typename FnT>
    node_t* do_update_full(unsigned level,
                           node_t* node,
                           std::size_t idx,
                           FnT&& fn) const
    {
        if (level == 0) {
            return do_update_leaf(node, idx, std::forward<FnT>(fn));
        } else {
            auto offset   = (idx >> level) & mask<B>;
            auto new_node = node_t::copy_inner(node, branches<B>);
            node->inner()[offset]->dec_unsafe();
            new_node->inner()[offset] =
                do_update_full(level - B,
                               node->inner()[offset],
                               idx,
                               std::forward<FnT>(fn));
            return new_node;
        }
    }

    template <typename FnT>
    node_t* do_update_regular(unsigned level,
                              node_t* node,
                              std::size_t idx,
                              std::size_t size,
                              FnT&& fn) const
    {
        if (level == 0) {
            return do_update_leaf(node, idx, std::forward<FnT>(fn));
        } else {
            auto offset     = (idx >> level) & mask<B>;
            auto end_offset = ((size - 1) >> level) & mask<B>;
            auto new_node   = node_t::copy_inner(node, end_offset + 1);
            node->inner()[offset]->dec_unsafe();
            new_node->inner()[offset] =
                offset == end_offset
                ? do_update_regular(level - B,
                                    node->inner()[offset],
                                    idx,
                                    size,
                                    std::forward<FnT>(fn))
                : do_update_full(level - B,
                                 node->inner()[offset],
                                 idx,
                                 std::forward<FnT>(fn));
            return new_node;
        }
    }

    rrbtree assoc(std::size_t idx, T value) const
    {
        return update(idx, [&] (auto&&) {
                return std::move(value);
            });
    }

    static std::tuple<unsigned, node_t*, unsigned, node_t*>
    slice_right_leaf(node_t* n, std::size_t size, std::size_t new_size)
    {
        assert(new_size > 0 && size > 0);
        auto old_tail_size = ((size - 1) & mask<B>) + 1;
        auto tail_size     = ((new_size - 1) & mask<B>) + 1;
        IMMU_TRACE("slice_right_leaf: " << size << " -> " << new_size);
        IMMU_TRACE("                : " << old_tail_size << " -> " << tail_size);
        auto new_tail      = tail_size == old_tail_size
            ? n->inc()
            : node_t::copy_leaf(n, tail_size);
        return { 0, nullptr, tail_size, new_tail };
    }

    static std::tuple<unsigned, node_t*, unsigned, node_t*>
    slice_right(node_t* n, unsigned shift, std::size_t size,
                std::size_t new_size, bool collapse)
    {
        if (shift == 0) {
            return slice_right_leaf(n, size, new_size);
        } else if (auto sizes = n->sizes()) {
            assert(shift > 0);
            auto idx = ((new_size - 1) >> shift) & mask<B>;
            while (sizes[idx] < new_size) ++idx;
            if (collapse && idx == 0) {
                return slice_right(n->inner()[idx], shift - B,
                                   sizes[idx], new_size,
                                   collapse);
            } else {
                using std::get;
                auto lsize = idx ? sizes[idx - 1] : 0;
                auto subs  = slice_right(n->inner()[idx], shift - B,
                                         sizes[idx] - lsize,
                                         new_size - lsize,
                                         false);
                auto next = get<1>(subs);
                auto tail_size = get<2>(subs);
                auto tail = get<3>(subs);
                if (next) {
                    auto newn = node_t::copy_inner_r(n, idx);
                    newn->inner()[idx] = next;
                    newn->sizes()[idx] = new_size - tail_size;
                    newn->slots()++;
                    return { shift, newn, tail_size, tail };
                } else if (idx == 0) {
                    return { shift, nullptr, tail_size, tail };
                } else if (idx == 1 && collapse && shift > B) {
                    auto newn = n->inner()[0];
                    return { shift - B, newn->inc(), tail_size, tail };
                } else {
                    return { shift, node_t::copy_inner_r(n, idx),
                             tail_size, tail };
                }
            }
        } else {
            return slice_right_regular(n, shift, size, new_size, collapse);
        }
    }

    static std::tuple<unsigned, node_t*, unsigned, node_t*>
    slice_right_regular(node_t* n, unsigned shift, std::size_t size,
                        std::size_t new_size, bool collapse)
    {
        if (shift == 0) {
            return slice_right_leaf(n, size, new_size);
        } else {
            assert(new_size);
            auto idx = ((new_size - 1) >> shift) & mask<B>;
            if (collapse && idx == 0) {
                return slice_right_regular(n->inner()[idx], shift - B,
                                           size, new_size, collapse);
            } else {
                using std::get;
                auto subs = slice_right_regular(n->inner()[idx], shift - B,
                                                size, new_size, false);
                auto next = get<1>(subs);
                auto tail_size = get<2>(subs);
                auto tail = get<3>(subs);
                if (next) {
                    auto newn = node_t::copy_inner(n, idx);
                    newn->inner()[idx] = next;
                    return { shift, newn, tail_size, tail };
                } else if (idx == 0) {
                    return { shift, nullptr, tail_size, tail };
                } else if (idx == 1 && collapse && shift > B) {
                    auto newn = n->inner()[0];
                    return { shift - B, newn->inc(), tail_size, tail };
                } else {
                    return { shift, node_t::copy_inner(n, idx),
                             tail_size, tail };
                }
            }
        }
    }

    rrbtree take(std::size_t new_size) const
    {
        auto tail_off = tail_offset();
        if (new_size == 0) {
            return empty;
        } else if (new_size >= size) {
            return *this;
        } else if (new_size > tail_off) {
            auto new_tail = node_t::copy_leaf(tail, new_size - tail_off);
            return { new_size, shift, root->inc(), new_tail };
        } else {
            using std::get;
            auto r = slice_right(root, shift, tail_off, new_size, true);
            auto new_shift = get<0>(r);
            auto new_root  = get<1>(r);
            auto new_tail  = get<3>(r);
            if (new_root) {
                assert(compute_shift(new_root) == get<0>(r));
                assert(check_node(new_root, new_shift, new_size - get<2>(r)));
                return { new_size, new_shift, new_root, new_tail };
            } else {
                return { new_size, B, empty.root->inc(), new_tail };
            }
        }
    }

    static std::tuple<unsigned, node_t*>
    slice_left_leaf(node_t* n, std::size_t size, std::size_t elems)
    {
        assert(elems < size);
        assert(size <= branches<B>);
        return { 0, node_t::copy_leaf(n, elems, size) };
    }

    static std::tuple<unsigned, node_t*>
    slice_left(node_t* n, unsigned shift,
               std::size_t size, std::size_t elems,
               bool collapse)
    {
        assert(elems <= size);
        if (shift == 0) {
            return slice_left_leaf(n, size, elems);
        } else if (auto sizes = n->sizes()) {
            auto idx = (elems >> shift) & mask<B>;
            while (sizes[idx] <= elems) ++idx;
            auto lsize = idx ? sizes[idx - 1] : 0;
            if (collapse && shift > B && idx == n->slots() - 1) {
                return slice_left(n->inner()[idx], shift - B,
                                  sizes[idx] - lsize,
                                  elems - lsize,
                                  collapse);
            } else {
                using std::get;
                auto subs = slice_left(n->inner()[idx], shift - B,
                                       sizes[idx] - lsize,
                                       elems - lsize,
                                       false);
                auto newn = node_t::make_inner_r();
                auto news = newn->sizes();
                newn->slots() = n->slots() - idx;
                news[0] = sizes[idx] - elems;
                for (auto i = 1; i < newn->slots(); ++i)
                    news[i] = (sizes[idx+i] - sizes[idx+i-1]) + news[i-1];
                newn->inner()[0] = get<1>(subs);
                std::uninitialized_copy(n->inner() + idx + 1,
                                        n->inner() + n->slots(),
                                        newn->inner() + 1);
                node_t::inc_nodes(newn->inner() + 1, newn->slots() - 1);
                return { shift, newn };
            }
        } else {
            return slice_left_regular(n, shift, size, elems, collapse);
        }
    }

    static std::tuple<unsigned, node_t*>
    slice_left_regular(node_t* n, unsigned shift,
                       std::size_t size, std::size_t elems,
                       bool collapse)
    {
        assert(elems <= size);
        assert(size > 0);
        if (shift == 0) {
            return slice_left_leaf(n, size, elems);
        } else {
            auto idx    = (elems >> shift) & mask<B>;
            auto lidx   = ((size - 1) >> shift) & mask<B>;
            auto count  = lidx + 1;
            auto lsize  = idx << shift;
            auto csize  = idx == lidx ? size - lsize : 1 << shift;
            auto celems = elems - lsize;
            if (collapse && shift > B && idx == lidx) {
                return slice_left_regular(n->inner()[idx],
                                          shift - B,
                                          csize,
                                          celems,
                                          collapse);
            } else {
                using std::get;
                auto subs = slice_left_regular(n->inner()[idx],
                                               shift - B,
                                               csize,
                                               celems,
                                               false);
                auto newn = node_t::make_inner_r();
                auto news = newn->sizes();
                newn->slots() = count - idx;
                news[0] = csize - celems;
                for (auto i = 1; i < newn->slots() - 1; ++i)
                    news[i] = news[i - 1] + (1 << shift);
                news[newn->slots() - 1] = size - elems;
                newn->inner()[0] = get<1>(subs);
                std::uninitialized_copy(n->inner() + idx + 1,
                                        n->inner() + count,
                                        newn->inner() + 1);
                node_t::inc_nodes(newn->inner() + 1, newn->slots() - 1);
                return { shift, newn };
            }
        }
    }

    rrbtree drop(std::size_t elems) const
    {
        if (elems == 0) {
            return *this;
        } else if (elems >= size) {
            return empty;
        } else if (elems == tail_offset()) {
            return { size - elems, B, empty.root->inc(), tail->inc() };
        } else if (elems > tail_offset()) {
            auto tail_off = tail_offset();
            auto new_tail = node_t::copy_leaf(tail, elems - tail_off,
                                              size - tail_off);
            return { size - elems, B, empty.root->inc(), new_tail };
        } else {
            using std::get;
            auto r = slice_left(root, shift, tail_offset(), elems, true);
            auto new_root  = get<1>(r);
            auto new_shift = get<0>(r);
            return { size - elems, new_shift, new_root, tail->inc() };
        }
        return *this;
    }

    rrbtree concat(const rrbtree& r) const
    {
        using std::get;
        if (size == 0)
            return r;
        else if (r.size == 0)
            return *this;
        else if (r.tail_offset() == 0) {
            // just concat the tail, similar to push_back
            auto tail_offst = tail_offset();
            auto tail_size  = size - tail_offst;
            if (tail_size == branches<B>) {
                auto new_root = push_tail(root, shift, tail_offst,
                                          tail->inc(), tail_size);
                return { size + r.size, get<0>(new_root), get<1>(new_root),
                         r.tail->inc() };
            } else if (tail_size + r.size <= branches<B>) {
                auto new_tail = node_t::copy_leaf(tail, tail_size,
                                                  r.tail, r.size);
                return { size + r.size, shift, root->inc(), new_tail };
            } else {
                auto remaining = branches<B> - tail_size;
                auto add_tail  = node_t::copy_leaf(tail, tail_size,
                                                   r.tail, remaining);
                auto new_tail  = node_t::copy_leaf(r.tail, remaining, r.size);
                auto new_root  = push_tail(root, shift, tail_offst,
                                           add_tail, branches<B>);
                return { size + r.size, get<0>(new_root), get<1>(new_root),
                         new_tail };
            }
        } else {
            auto tail_offst = tail_offset();
            auto with_tail  = push_tail(root, shift, tail_offst,
                                        tail->inc(), size - tail_offst);
            auto lshift     = get<0>(with_tail);
            auto lroot      = get<1>(with_tail);
            assert(check_node(lroot, lshift, size));
            auto concated   = concat_sub_tree(
                size, lshift, lroot,
                r.tail_offset(), r.shift, r.root,
                true);
            auto new_shift  = get<0>(concated);
            auto new_root   = get<1>(concated);
            assert(new_shift == compute_shift(new_root));
            assert(check_node(new_root, new_shift, size + r.tail_offset()));
            dec_node(lroot, lshift, size);
            return { size + r.size, new_shift, new_root, r.tail->inc() };
        }
    }

    static std::tuple<unsigned, node_t*> concat_sub_tree(
        std::size_t lsize, unsigned lshift, node_t* lnode,
        std::size_t rsize, unsigned rshift, node_t* rnode,
        bool is_top)
    {
        IMMU_TRACE("concat_sub_tree: " << lshift << " <> " << rshift);
        using std::get;
        if (lshift > rshift) {
            auto lidx = lnode->sizes()
                ? lnode->slots() - 1
                : ((lsize - 1) >> lshift) & mask<B>;
            auto llsize = lnode->sizes()
                ? lnode->sizes()[lidx] - (lidx ? lnode->sizes()[lidx - 1] : 0)
                : lsize - (lidx << lshift);
            auto cnode  = get<1>(
                concat_sub_tree(
                    llsize, lshift - B, lnode->inner() [lidx],
                    rsize, rshift, rnode,
                    false));
            auto result = concat_rebalance(
                lsize, lnode,
                llsize, cnode,
                0, nullptr,
                lshift, is_top);
            dec_node(cnode, lshift);
            return result;
        } else if (lshift < rshift) {
            auto rrsize = rnode->sizes()
                ? rnode->sizes()[0]
                : std::min(rsize, std::size_t{1} << rshift);
            auto cnode  = get<1>(
                concat_sub_tree(
                    lsize, lshift, lnode,
                    rrsize, rshift - B, rnode->inner() [0],
                    false));
            auto result = concat_rebalance(
                0, nullptr,
                rrsize, cnode,
                rsize, rnode,
                rshift, is_top);
            dec_node(cnode, rshift);
            return result;
        } else if (lshift == 0) {
            assert(lsize);
            assert(rsize);
            auto lslots = (((lsize - 1) >> lshift) & mask<B>) + 1;
            auto rslots = (((rsize - 1) >> lshift) & mask<B>) + 1;
            if (is_top && lslots + rslots <= branches<B>)
                return { 0, node_t::copy_leaf(lnode, lslots, rnode, rslots) };
            else {
                return { B, node_t::make_inner_r(lnode->inc(), lslots,
                                                 rnode->inc(), rslots) };
            }
        } else {
            auto lidx = lnode->sizes()
                ? lnode->slots() - 1
                : ((lsize - 1) >> lshift) & mask<B>;
            auto llsize = lnode->sizes()
                ? lnode->sizes()[lidx] - (lidx ? lnode->sizes()[lidx - 1] : 0)
                : lsize - (lidx << lshift);
            auto rrsize = rnode->sizes()
                ? rnode->sizes()[0]
                : std::min(rsize, std::size_t{1} << rshift);
            auto cnode  = get<1>(
                concat_sub_tree(
                    llsize, lshift - B, lnode->inner() [lidx],
                    rrsize, rshift - B, rnode->inner() [0],
                    false));
            auto result = concat_rebalance(
                lsize, lnode,
                llsize + rrsize, cnode,
                rsize, rnode,
                lshift, is_top);
            dec_node(cnode, lshift);
            return result;
        }
    }

    struct leaf_policy
    {
        using data_t = T;
        static node_t* make() { return node_t::make_leaf(); }
        static data_t* data(node_t* n) { return n->leaf(); }
        static std::size_t* sizes(node_t*) { return nullptr; }
        static std::size_t size(node_t*, unsigned slots) { return slots; }
        static void copied(node_t*, unsigned, std::size_t,
                           unsigned, unsigned,
                           node_t*, unsigned) {}
    };

    struct inner_r_policy
    {
        using data_t = node_t*;
        static node_t* make() { return node_t::make_inner_r(); }
        static data_t* data(node_t* n) { return n->inner(); }
        static std::size_t* sizes(node_t* n) { return n->sizes(); }
        static std::size_t size(node_t* n, unsigned slots) {
            return n->sizes()[slots - 1];
        }
        static void copied(node_t* from, unsigned shift, std::size_t size,
                           unsigned from_offset, unsigned copy_count,
                           node_t* to, unsigned to_offset)
        {
            to->slots() += copy_count;
            node_t::inc_nodes(to->inner() + to_offset, copy_count);
            auto to_sizes  = to->sizes();
            if (auto sizes = from->sizes()) {
                auto last_from_size = from_offset  ? sizes[from_offset - 1] : 0;
                auto last_to_size   = to_offset ? to_sizes[to_offset - 1] : 0;
                for (auto i = 0; i < copy_count; ++i) {
                    last_to_size = to_sizes [to_offset + i] =
                        sizes[from_offset + i] -
                        last_from_size +
                        last_to_size;
                    last_from_size = sizes[from_offset + i];
                }
            } else {
                auto lidx = ((size - 1) >> shift) & mask<B>;
                auto end = from_offset + copy_count;
                if (end < lidx) {
                    for (auto idx = from_offset; idx < end; ++idx)
                        to_sizes[idx] = (idx + 1) << (shift - B);
                } else {
                    for (auto idx = from_offset; idx < lidx; ++idx)
                        to_sizes[idx] = (idx + 1) << (shift - B);
                    to_sizes[lidx] = size;
                }
            }
        }
    };

    template <typename ChildrenPolicy>
    struct node_merger
    {
        using policy = ChildrenPolicy;
        using data_t = typename policy::data_t;

        unsigned* slots;

        node_t*     result    = node_t::make_inner_r();
        node_t*     parent    = result;
        node_t*     to        = nullptr;
        unsigned    to_offset = 0;
        std::size_t size_sum  = 0;

        node_merger(unsigned* s)
            : slots{s}
        {}

        std::tuple<unsigned, node_t*>
        finish(unsigned shift, bool is_top)
        {
            assert(!to);
            if (parent != result) {
                assert(result->slots() == 2);
                auto sizes = result->sizes();
                sizes [1] = sizes[0] + size_sum;
                return { shift + B, result };
            } else if (is_top) {
                return { shift, result };
            } else {
                return { shift, node_t::make_inner_r(result, size_sum) };
            }
        }

        void merge(node_t* node, unsigned shift, std::size_t size,
                   unsigned nslots, unsigned offset, unsigned endoff)
        {
            auto add_child = [&] (node_t* n, std::size_t size)
            {
                ++slots;
                if (parent->slots() == branches<B>) {
                    assert(result == parent);
                    auto new_parent = node_t::make_inner_r();
                    result = node_t::make_inner_r(parent, size_sum, new_parent);
                    parent = new_parent;
                    size_sum = 0;
                }
                auto idx = parent->slots()++;
                parent->inner() [idx] = n;
                parent->sizes() [idx] = size_sum += size;
                assert(!idx || parent->inner() [idx] != parent->inner() [idx - 1]);
            };

            if (!node) return;
            auto sizes  = node->sizes();
            auto nshift = shift - B;
            for (auto idx = offset; idx + endoff < nslots; ++idx) {
                auto from = node->inner() [idx];
                auto from_data  = policy::data(from);
                auto from_size  =
                    sizes            ? sizes[idx] - (idx ? sizes[idx - 1] : 0) :
                    idx < nslots - 1 ? 1 << shift
                    /* otherwise */  : size - (idx << shift);
                assert(from_size);
                auto from_slots = static_cast<unsigned>(
                    policy::sizes(from) ? from->slots()
                    /* otherwise */     : ((from_size - 1) >> nshift) + 1);
                if (!to && *slots == from_slots) {
                    add_child(from->inc(), from_size);
                } else {
                    auto from_offset = 0u;
                    do {
                        if (!to) {
                            to = policy::make();
                            to_offset  = 0;
                        }
                        auto to_copy = std::min(
                            from_slots - from_offset,
                            *slots - to_offset);
                        auto data = policy::data(to);
                        assert(to_copy);
                        std::uninitialized_copy(
                            from_data + from_offset,
                            from_data + from_offset + to_copy,
                            data + to_offset);
                        policy::copied(
                            from, nshift, from_size, from_offset,
                            to_copy, to, to_offset);
                        to_offset   += to_copy;
                        from_offset += to_copy;
                        if (*slots == to_offset) {
                            add_child(to, policy::size(to, to_offset));
                            to = nullptr;
                        }
                    } while (from_slots != from_offset);
                }
            }
        }
    };

    struct concat_rebalance_plan_t
    {
        unsigned slots [3 * branches<B>];
        unsigned total_slots = 0u;
        unsigned count = 0u;
    };

    static std::tuple<unsigned, node_t*>
    concat_rebalance(std::size_t lsize, node_t* lnode,
                     std::size_t csize, node_t* cnode,
                     std::size_t rsize, node_t* rnode,
                     unsigned shift,
                     bool is_top)
    {
        assert(cnode);
        assert(lnode || rnode);

        // list all children and their slot counts
        unsigned all_slots [3 * branches<B>];
        auto total_all_slots = 0u;
        auto all_n = 0u;

        auto add_slots = [&] (node_t* node, unsigned shift, std::size_t size,
                              unsigned offset, unsigned endoff, unsigned& slots)
        {
            if (node) {
                auto nshift = shift - B;
                if (auto sizes = node->sizes()) {
                    slots = node->slots();
                    auto last_size = offset ? sizes[offset - 1] : 0;
                    for (auto i = offset; i + endoff < slots; ++i) {
                        auto nsize = sizes[i] - last_size;
                        total_all_slots += all_slots[all_n++] =
                            nshift == 0 || !node->inner()[i]->sizes()
                            ? ((nsize - 1) >> nshift) + 1
                            : node->inner()[i]->slots();
                        last_size = sizes[i];
                        assert(all_slots[all_n-1]);
                    }
                } else {
                    assert(size);
                    auto lidx = ((size - 1) >> shift) & mask<B>;
                    slots = lidx + 1;
                    auto i = offset;
                    for (; i + endoff < lidx; ++i)
                        total_all_slots += all_slots[all_n++] = branches<B>;
                    if (i + endoff < slots) {
                        total_all_slots += all_slots[all_n++] =
                            i == lidx
                            ? (((size - 1) >> nshift) & mask<B>) + 1
                            : branches<B>;
                        assert(all_slots[all_n-1]);
                    }
                }
            }
        };

        auto lslots = 0u;
        auto cslots = 0u;
        auto rslots = 0u;
        add_slots(lnode, shift, lsize, 0, 1, lslots);
        add_slots(cnode, shift, csize, 0, 0, cslots);
        add_slots(rnode, shift, rsize, 1, 0, rslots);

        // plan rebalance
        const auto RRB_EXTRAS = 2;
        const auto RRB_INVARIANT = 1;

        IMMU_TRACE("all_slots: " <<
                   pretty_print_array(all_slots, all_n));

        assert(total_all_slots > 0);
        const auto optimal_slots = ((total_all_slots - 1) / branches<B>) + 1;
        IMMU_TRACE("optimal_slots: " << optimal_slots <<
                   " // " << total_all_slots);

        auto shuffled_n = all_n;
        auto i = 0u;
        while (shuffled_n >= optimal_slots + RRB_EXTRAS) {
            // skip ok nodes
            while (all_slots[i] > branches<B> - RRB_INVARIANT) i++;
            // short node, redistribute
            auto remaining = all_slots[i];
            do {
                auto min_size = std::min(remaining + all_slots[i+1],
                                         branches<B, unsigned>);
                all_slots[i] = min_size;
                remaining += all_slots[i+1] - min_size;
                ++i;
            } while (remaining > 0);
            // remove node
            std::move(all_slots + i + 1, all_slots + shuffled_n, all_slots + i);
            --shuffled_n;
            --i;
        }

        IMMU_TRACE("    // " << pretty_print_array(all_slots, shuffled_n));

        // actually rebalance the nodes
        auto merge_all = [&] (auto& merger)
        {
            merger.merge(lnode, shift, lsize, lslots, 0, 1);
            merger.merge(cnode, shift, csize, cslots, 0, 0);
            merger.merge(rnode, shift, rsize, rslots, 1, 0);
            assert(!merger.to);
            assert(merger.slots == all_slots + shuffled_n);
            return merger.finish(shift, is_top);
        };

        if (shift == B) {
            auto merger = node_merger<leaf_policy>{all_slots};
            return merge_all(merger);
        } else {
            auto merger = node_merger<inner_r_policy>{all_slots};
            return merge_all(merger);
        }
    }

#if IMMU_TAGGED_VNODE
    static unsigned compute_shift(node_t* node)
    {
        if (node->kind() == node_t::leaf_kind)
            return 0;
        else
            return B + compute_shift(node->inner() [0]);
    }
#endif

    bool check_tree() const
    {
#if IMMU_DEBUG_DEEP_CHECK
        assert(shift >= B);
        assert(tail_offset() <= size);
        assert(check_root());
        assert(check_tail());
#endif
        return true;
    }

    bool check_tail() const
    {
#if IMMU_DEBUG_DEEP_CHECK
        if (tail_size() > 0)
            check_node(tail, 0, tail_size());
#endif
        return true;
    }

    bool check_root() const
    {
#if IMMU_DEBUG_DEEP_CHECK
        if (tail_offset() > 0)
            check_node(root, shift, tail_offset());
        else {
            assert(root->kind() == node_t::inner_kind);
            assert(shift == B);
        }
#endif
        return true;
    }

    bool check_node(node_t* node, unsigned shift, std::size_t size) const
    {
#if IMMU_DEBUG_DEEP_CHECK
        assert(size > 0);
        if (shift == 0) {
            assert(node->kind() == node_t::leaf_kind);
            assert(size <= branches<B>);
        } else if (auto sizes = node->sizes()) {
            auto count = node->slots();
            assert(count > 0);
            assert(count <= branches<B>);
            assert(sizes[count - 1] == size);
            for (auto i = 1; i < count; ++i)
                assert(sizes[i - 1] < sizes[i]);
            auto last_size = std::size_t{};
            for (auto i = 0; i < count; ++i) {
                assert(check_node(node->inner()[i],
                                  shift - B,
                                  sizes[i] - last_size));
                last_size = sizes[i];
            }
        } else {
            assert(size <= branches<B> << shift);
            auto count = (size >> shift)
                + (size - ((size >> shift) << shift) > 0);
            assert(count <= branches<B>);
            if (count) {
                for (auto i = 1; i < count - 1; ++i)
                    assert(check_node(node->inner()[i],
                                      shift - B,
                                      1 << shift));
                assert(check_node(node->inner()[count - 1],
                                  shift - B,
                                  size - ((count - 1) << shift)));
            }
        }
#endif // IMMU_DEBUG_DEEP_CHECK
        return true;
    }

#if IMMU_DEBUG_PRINT
    void debug_print() const
    {
        std::cerr
            << "--" << std::endl
            << "{" << std::endl
            << "  size  = " << size << std::endl
            << "  shift = " << shift << std::endl
            << "  root  = " << std::endl;
        debug_print_node(root, shift, tail_offset());
        std::cerr << "  tail  = " << std::endl;
        debug_print_node(tail, 0, tail_size());
        std::cerr << "}" << std::endl;
    }

    void debug_print_indent(unsigned indent) const
    {
        while (indent --> 0)
            std::cerr << ' ';
    }

    void debug_print_node(node_t* node,
                          unsigned shift,
                          std::size_t size,
                          unsigned indent = 8) const
    {
        const auto indent_step = 4;

        if (shift == 0) {
            debug_print_indent(indent);
            std::cerr << "- {" << size << "} "
                      << pretty_print_array(node->leaf(), size)
                      << std::endl;
        } else if (auto sizes = node->sizes()) {
            auto count = node->slots();
            debug_print_indent(indent);
            std::cerr << "# {" << size << "} "
                      << pretty_print_array(node->sizes(), node->slots())
                      << std::endl;
            auto last_size = std::size_t{};
            for (auto i = 0; i < count; ++i) {
                debug_print_node(node->inner()[i],
                                 shift - B,
                                 sizes[i] - last_size,
                                 indent + indent_step);
                last_size = sizes[i];
            }
        } else {
            debug_print_indent(indent);
            std::cerr << "+ {" << size << "}" << std::endl;
            auto count = (size >> shift)
                + (size - ((size >> shift) << shift) > 0);
            if (count) {
                for (auto i = 0; i < count - 1; ++i)
                    debug_print_node(node->inner()[i],
                                     shift - B,
                                     1 << shift,
                                     indent + indent_step);
                debug_print_node(node->inner()[count - 1],
                                 shift - B,
                                 size - ((count - 1) << shift),
                                 indent + indent_step);
            }
        }
    }
#endif // IMMU_DEBUG_PRINT
};

template <typename T, int B, typename MP>
const rrbtree<T, B, MP> rrbtree<T, B, MP>::empty = {
    0,
    B,
    node_t::make_inner(),
    node_t::make_leaf()
};

} /* namespace detail */
} /* namespace immu */