#include "nix/expr/json-to-value.hh"
#include "nix/expr/value.hh"
#include "nix/expr/eval.hh"

#include <limits>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

namespace nix {

namespace {

// for more information, refer to
// https://github.com/nlohmann/json/blob/master/include/nlohmann/detail/input/json_sax.hpp
class JSONSax : nlohmann::json_sax<json>
{
    class JSONState
    {
    protected:
        std::unique_ptr<JSONState> parent;

        /**
         * The value being built by this state, or null. Kept alive by
         * `JSONSax::rootValues`.
         */
        Value * v = nullptr;

        /**
         * Reference to `JSONSax::rootValues`.
         */
        ValueVector & rootValues;

    public:
        virtual std::unique_ptr<JSONState> resolve(EvalState &)
        {
            throw std::logic_error("tried to close toplevel json parser state");
        }

        explicit JSONState(std::unique_ptr<JSONState> && p)
            : parent(std::move(p))
            , rootValues(parent->rootValues)
        {
        }

        JSONState(Value * v, ValueVector & rootValues)
            : v(v)
            , rootValues(rootValues)
        {
        }

        JSONState(JSONState & p) = delete;

        Value & value(EvalState & state)
        {
            if (!v) {
                v = state.allocValue();
                /* Root the value for the duration of the parse (see
                   `JSONSax::rootValues`). */
                rootValues.push_back(v);
            }
            return *v;
        }

        virtual ~JSONState() {}

        virtual void add() {}
    };

    class JSONObjectState : public JSONState
    {
        using JSONState::JSONState;

        /**
         * Note: deliberately not `ValueMap`: the values are rooted via
         * `JSONSax::rootValues`, so there is no need for a GC-visible
         * (uncollectable) container, whose allocations would take the
         * global GC allocation lock.
         */
        std::vector<std::pair<Symbol, Value *>> attrs;

        std::unique_ptr<JSONState> resolve(EvalState & state) override
        {
            /* JSON allows duplicate keys, and the last occurrence
               wins. `Bindings` requires unique keys, so drop all but
               the last entry of every run of equal keys. The stable
               sort keeps equal keys in insertion order. */
            std::stable_sort(
                attrs.begin(), attrs.end(), [](const auto & a, const auto & b) { return a.first < b.first; });
            auto attrs2 = state.buildBindings(attrs.size());
            for (auto i = attrs.begin(); i != attrs.end(); ++i)
                if (std::next(i) == attrs.end() || std::next(i)->first != i->first)
                    attrs2.insert(i->first, i->second);
            parent->value(state).mkAttrs(attrs2.alreadySorted());
            return std::move(parent);
        }

        void add() override
        {
            v = nullptr;
        }
    public:
        void key(string_t & name, EvalState & state)
        {
            forceNoNullByte(name);
            attrs.emplace_back(state.symbols.create(name), &value(state));
        }
    };

    class JSONListState : public JSONState
    {
        /**
         * Note: deliberately not `ValueVector` (see
         * `JSONObjectState::attrs`).
         */
        std::vector<Value *> values;

        std::unique_ptr<JSONState> resolve(EvalState & state) override
        {
            auto list = state.buildList(values.size());
            for (const auto & [n, v2] : enumerate(list))
                v2 = values[n];
            parent->value(state).mkList(list);
            return std::move(parent);
        }

        void add() override
        {
            values.push_back(v);
            v = nullptr;
        }
    public:
        JSONListState(std::unique_ptr<JSONState> && p, std::size_t reserve)
            : JSONState(std::move(p))
        {
            values.reserve(reserve);
        }
    };

    EvalState & state;

    /**
     * Keeps all values allocated during the parse alive. This is the
     * only GC-visible container of the parser: the per-node containers
     * in `JSONObjectState`/`JSONListState` use ordinary allocators,
     * since GC-visible (uncollectable) allocations take the global GC
     * allocation lock, and a big JSON document would otherwise do one
     * or more of them per object/array node — a significant source of
     * lock contention during parallel evaluation.
     */
    ValueVector rootValues;

    std::unique_ptr<JSONState> rs;

public:
    JSONSax(EvalState & state, Value & v)
        : state(state)
        , rs(new JSONState(&v, rootValues)) {};

    bool null() override
    {
        rs->value(state).mkNull();
        rs->add();
        return true;
    }

    bool boolean(bool val) override
    {
        rs->value(state).mkBool(val);
        rs->add();
        return true;
    }

    bool number_integer(number_integer_t val) override
    {
        rs->value(state).mkInt(val);
        rs->add();
        return true;
    }

    bool number_unsigned(number_unsigned_t val_) override
    {
        if (val_ > std::numeric_limits<NixInt::Inner>::max()) {
            throw Error("unsigned json number %1% outside of Nix integer range", val_);
        }
        NixInt::Inner val = val_;
        rs->value(state).mkInt(val);
        rs->add();
        return true;
    }

    bool number_float(number_float_t val, const string_t & s) override
    {
        rs->value(state).mkFloat(val);
        rs->add();
        return true;
    }

    bool string(string_t & val) override
    {
        forceNoNullByte(val);
        rs->value(state).mkString(val, state.mem);
        rs->add();
        return true;
    }

#if NLOHMANN_JSON_VERSION_MAJOR >= 3 && NLOHMANN_JSON_VERSION_MINOR >= 8
    bool binary(binary_t &) override
    {
        // This function ought to be unreachable
        assert(false);
        return true;
    }
#endif

    bool start_object(std::size_t len) override
    {
        rs = std::make_unique<JSONObjectState>(std::move(rs));
        return true;
    }

    bool key(string_t & name) override
    {
        dynamic_cast<JSONObjectState *>(rs.get())->key(name, state);
        return true;
    }

    bool end_object() override
    {
        rs = rs->resolve(state);
        rs->add();
        return true;
    }

    bool end_array() override
    {
        return end_object();
    }

    bool start_array(size_t len) override
    {
        rs = std::make_unique<JSONListState>(std::move(rs), len != std::numeric_limits<size_t>::max() ? len : 128);
        return true;
    }

    bool parse_error(std::size_t, const std::string &, const nlohmann::detail::exception & ex) override
    {
        throw JSONParseError("%s", ex.what());
    }
};

} // namespace

void parseJSON(EvalState & state, const std::string_view & s_, Value & v)
{
    JSONSax parser(state, v);
    bool res = json::sax_parse(s_, &parser);
    if (!res)
        throw JSONParseError("Invalid JSON Value");
}

void JSONParseError::anchor() {}

} // namespace nix
