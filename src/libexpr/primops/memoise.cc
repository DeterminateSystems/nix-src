#include "nix/expr/primops.hh"
#include "nix/expr/eval-inline.hh"
#include "nix/expr/root-value.hh"
#include "nix/util/std-hash.hh"

#include <bit>

#include <boost/unordered/concurrent_flat_map.hpp>

namespace nix {

struct MemoiseCache
{
    struct Entry;

    using EntryPtr = std::shared_ptr<const Entry>;

    /**
     * A memoised function call. Entries are immutable once they have
     * been added to the cache.
     */
    struct Entry
    {
        RootValue fun;
        RootValue arg;

        /**
         * A thunk that evaluates `fun arg`.
         */
        RootValue result;

        /**
         * The next entry with the same hash.
         */
        EntryPtr next;

        ~Entry()
        {
            /* Destroy the rest of the list iteratively rather than
               recursively, to avoid a stack overflow on long lists. */
            auto p = std::move(next);
            while (p && p.use_count() == 1)
                p = std::move(const_cast<Entry &>(*p).next);
        }
    };

    /**
     * Map from the hash of a call to the list of entries with that
     * hash. New entries are prepended to the list. Since lookups
     * hold a reference to the head of the list, the list stays alive
     * even if the cache is cleared in the meantime.
     */
    boost::concurrent_flat_map<size_t, EntryPtr> entries;
};

ref<MemoiseCache> makeMemoiseCache()
{
    return make_ref<MemoiseCache>();
}

void clearMemoiseCache(MemoiseCache & cache)
{
    cache.entries.clear();
}

/**
 * Hash a value that has been forced (but not deeply). This must be
 * consistent with `memoEqual()`, i.e. it can only depend on the
 * properties of the value that `memoEqual()` compares without
 * forcing anything.
 */
static size_t memoHash(const Value & v)
{
    size_t h = 0;
    hash_combine(h, v.type());

    switch (v.type()) {
    case nInt:
        hash_combine(h, v.integer().value);
        break;

    case nBool:
        hash_combine(h, v.boolean());
        break;

    case nFloat:
        hash_combine(h, std::bit_cast<uint64_t>(v.fpoint()));
        break;

    case nString:
        hash_combine(h, v.string_view());
        break;

    case nPath:
        hash_combine(h, (const void *) v.pathAccessor(), v.pathStrView());
        break;

    case nNull:
        break;

    case nList:
        hash_combine(h, v.listSize());
        break;

    case nAttrs:
        hash_combine(h, v.attrs()->size());
        for (auto & attr : *v.attrs())
            hash_combine(h, attr.name);
        break;

    case nFunction:
        if (v.isLambda())
            hash_combine(h, (const void *) v.lambda().env, (const void *) v.lambda().fun);
        else if (v.isPrimOp())
            hash_combine(h, (const void *) v.primOp());
        else if (v.isPrimOpApp())
            hash_combine(h, (const void *) v.primOpAppPrimOp());
        break;

    case nExternal:
        hash_combine(h, (const void *) v.external());
        break;

    case nThunk:
    case nFailed:
        unreachable();
    }

    return h;
}

/**
 * Return whether a function applied to `v1` is guaranteed to return
 * the same result as when applied to `v2`. This is stricter than `==`:
 * integers are not equal to floats, string contexts must be equal,
 * derivations are compared like any other attribute set, and lambdas
 * are equal only if their expression and environment pointers are
 * equal. Values are forced only as far as needed to determine
 * (in)equality.
 */
static bool memoEqual(EvalState & state, Value & v1, Value & v2)
{
    /* Identical values are equal, even if they would fail to
       evaluate. Unlike in `eqValues()`, this is not observable, since
       it only causes a previous result of applying the function to
       this very value to be reused. It also avoids needlessly forcing
       values. */
    if (&v1 == &v2)
        return true;

    auto _level = state.addCallDepth(noPos);

    state.forceValue(v1, noPos);
    state.forceValue(v2, noPos);

    if (v1.type() != v2.type())
        return false;

    switch (v1.type()) {
    case nInt:
        return v1.integer() == v2.integer();

    case nBool:
        return v1.boolean() == v2.boolean();

    case nFloat:
        /* Compare the representations, since a function can
           distinguish 0.0 from -0.0. */
        return std::bit_cast<uint64_t>(v1.fpoint()) == std::bit_cast<uint64_t>(v2.fpoint());

    case nString: {
        if (v1.string_view() != v2.string_view())
            return false;
        auto c1 = v1.context();
        auto c2 = v2.context();
        if (!c1 || !c2)
            return (c1 ? c1->size() : 0) == (c2 ? c2->size() : 0);
        return std::ranges::equal(*c1, *c2, [](auto a, auto b) { return a->view() == b->view(); });
    }

    case nPath:
        return v1.pathAccessor() == v2.pathAccessor() && v1.pathStrView() == v2.pathStrView();

    case nNull:
        return true;

    case nList: {
        auto l1 = v1.listView();
        auto l2 = v2.listView();
        if (l1.size() != l2.size())
            return false;
        for (size_t n = 0; n < l1.size(); ++n)
            if (!memoEqual(state, *l1[n], *l2[n]))
                return false;
        return true;
    }

    case nAttrs: {
        auto a1 = v1.attrs();
        auto a2 = v2.attrs();
        if (a1 == a2)
            return true;
        if (a1->size() != a2->size())
            return false;
        /* Compare the names first to avoid needlessly forcing
           attributes. */
        for (auto i = a1->begin(), j = a2->begin(); i != a1->end(); ++i, ++j)
            if (i->name != j->name)
                return false;
        for (auto i = a1->begin(), j = a2->begin(); i != a1->end(); ++i, ++j)
            if (!memoEqual(state, *i->value, *j->value))
                return false;
        return true;
    }

    case nFunction:
        if (v1.isLambda() && v2.isLambda())
            return v1.lambda().env == v2.lambda().env && v1.lambda().fun == v2.lambda().fun;
        if (v1.isPrimOp() && v2.isPrimOp())
            return v1.primOp() == v2.primOp();
        if (v1.isPrimOpApp() && v2.isPrimOpApp())
            return memoEqual(state, *v1.primOpApp().left, *v2.primOpApp().left)
                   && memoEqual(state, *v1.primOpApp().right, *v2.primOpApp().right);
        return false;

    case nExternal:
        return v1.external() == v2.external();

    case nThunk:
    case nFailed:
        unreachable();
    }

    unreachable();
}

/**
 * Like `memoEqual()`, but consider values that fail to evaluate to be
 * unequal. Thus memoisation doesn't cause errors that wouldn't happen
 * without it.
 */
static bool memoEqualNoThrow(EvalState & state, Value & v1, Value & v2)
{
    try {
        return memoEqual(state, v1, v2);
    } catch (Error &) {
        return false;
    }
}

void prim_memoise(EvalState & state, const PosIdx pos, Value ** args, Value & v)
{
    auto & fun = *args[0];
    auto & arg = *args[1];

    state.forceFunction(fun, pos, "while evaluating the first argument passed to builtins.memoise");

    state.forceValue(arg, pos);

    auto & cache = *state.memoiseCache;

    size_t hash = 0;
    hash_combine(hash, memoHash(fun), memoHash(arg));

    using Entry = MemoiseCache::Entry;

    /* Note: this is on the stack, so it's visible to the GC. */
    Value * result = nullptr;

    /* The head of the list when we last compared with its entries. */
    MemoiseCache::EntryPtr seen;

    std::shared_ptr<Entry> newEntry;

    while (true) {
        MemoiseCache::EntryPtr head;
        cache.entries.cvisit(hash, [&](const auto & kv) { head = kv.second; });

        /* Look for an entry with an equal function and argument. Note
           that we can't hold a lock on the cache while doing this,
           since the comparison can cause arbitrary evaluation. (The
           `e` check is needed if the cache was cleared in the
           meantime.) */
        for (auto e = head.get(); e && e != seen.get(); e = e->next.get())
            if (memoEqualNoThrow(state, **e->fun, fun) && memoEqualNoThrow(state, **e->arg, arg)) {
                result = *e->result;
                break;
            }

        if (result) {
            state.nrMemoiseHits++;
            break;
        }

        seen = std::move(head);

        if (!newEntry) {
            /* Copy the function and argument since they might not be
               heap-allocated. */
            auto funCopy = state.allocValue();
            *funCopy = fun;
            auto argCopy = state.allocValue();
            *argCopy = arg;
            auto thunk = state.allocValue();
            thunk->mkApp(funCopy, argCopy);
            newEntry = std::make_shared<Entry>();
            newEntry->fun = RootValue(funCopy);
            newEntry->arg = RootValue(argCopy);
            newEntry->result = RootValue(thunk);
        }

        /* Add the new entry, unless another thread has added entries
           in the meantime that we haven't compared with yet. */
        cache.entries.try_emplace_and_visit(
            hash,
            nullptr,
            [&](auto & kv) {
                kv.second = newEntry;
                result = *newEntry->result;
            },
            [&](auto & kv) {
                if (kv.second == seen) {
                    newEntry->next = kv.second;
                    kv.second = newEntry;
                    result = *newEntry->result;
                }
            });

        if (result) {
            state.nrMemoiseMisses++;
            break;
        }
    }

    /* If another thread is evaluating the result, this waits for it. */
    state.forceValue(*result, pos);
    v = *result;
}

static RegisterPrimOp primop_memoise({
    .name = "__memoise",
    .args = {"f", "x"},
    .doc = R"(
      Return the result of applying the function *f* to *x*, i.e. `f x`, but
      remember the result so that subsequent calls to `builtins.memoise`
      with an equal *f* and *x* return it without calling *f* again. This
      is useful to avoid repeatedly evaluating expensive function calls,
      such as instantiating Nixpkgs multiple times with the same
      arguments:

      ```nix
      builtins.memoise (import <nixpkgs>) { system = "x86_64-linux"; }
      ```

      Equality is structural, like the `==` operator, with the following
      differences:

      - Lambdas are compared by pointer equality of their closure: two
        lambdas are equal only if they were created by evaluating the
        same lambda expression (i.e. at the same location in the source)
        in the same environment (i.e. the same in-memory instance of the
        enclosing scope). For example, given `mk = n: x: x + n`, `mk 1`
        and `mk 1` are not equal, since each call to `mk` creates a new
        environment.
      - Built-in functions are equal if they are the same built-in
        function applied to equal arguments.
      - Integers are never equal to floats.
      - Strings are equal only if they have the same
        [string context](@docroot@/language/string-context.md).
      - Derivations are compared like any other attribute set, rather
        than by their `outPath`.

      Positions are not compared, so if *f* uses
      [`builtins.unsafeGetAttrPos`](#builtins-unsafeGetAttrPos) on *x*, it
      may get the positions of the argument of a previous call.

      `builtins.memoise` is strict in both *f* and *x*, i.e. it evaluates
      them even if `f x` wouldn't evaluate *x*. Beyond that, *x* and the
      arguments of previous calls are evaluated only as far as needed to
      determine whether they're equal. Parts that fail to evaluate are
      considered unequal. Note that comparing large values that are equal
      but not identical can be expensive.

      Currently, memoised results are kept in memory until the end of the
      evaluation. However, memoisation is best-effort: future versions of
      Nix may discard memoised results (e.g. during garbage collection),
      in which case *f* is called again.
    )",
    .impl = prim_memoise,
    .experimentalFeature = Xp::Memoise,
});

} // namespace nix
