#include "nix/fetchers/filtering-source-accessor.hh"
#include "nix/util/sync.hh"
#include "nix/util/util.hh"
#include "nix/util/strings.hh"

#include <boost/unordered/concurrent_flat_map.hpp>
#include <boost/unordered/unordered_flat_set.hpp>

namespace nix {

std::optional<std::filesystem::path> FilteringSourceAccessor::getPhysicalPath(const CanonPath & path)
{
    checkAccess(path);
    return next->getPhysicalPath(prefix / path);
}

void FilteringSourceAccessor::readFile(const CanonPath & path, Sink & sink, fun<void(uint64_t)> sizeCallback)
{
    checkAccess(path);
    return next->readFile(prefix / path, sink, sizeCallback);
}

bool FilteringSourceAccessor::pathExists(const CanonPath & path)
{
    return isAllowed(path) && next->pathExists(prefix / path);
}

std::optional<SourceAccessor::Stat> FilteringSourceAccessor::maybeLstat(const CanonPath & path)
{
    return isAllowed(path) ? next->maybeLstat(prefix / path) : std::nullopt;
}

SourceAccessor::Stat FilteringSourceAccessor::lstat(const CanonPath & path)
{
    checkAccess(path);
    return next->lstat(prefix / path);
}

SourceAccessor::DirEntries FilteringSourceAccessor::readDirectory(const CanonPath & path)
{
    checkAccess(path);
    auto entries = next->readDirectory(prefix / path);
    if (!isAllowedRecursively(path))
        std::erase_if(entries, [&](auto & entry) { return !isAllowed(path / entry.first); });
    return entries;
}

std::string FilteringSourceAccessor::readLink(const CanonPath & path)
{
    checkAccess(path);
    return next->readLink(prefix / path);
}

std::string FilteringSourceAccessor::showPath(const CanonPath & path)
{
    return displayPrefix + next->showPath(prefix / path) + displaySuffix;
}

std::pair<CanonPath, std::optional<std::string>> FilteringSourceAccessor::getFingerprint(const CanonPath & path)
{
    if (fingerprint)
        return {path, fingerprint};
    return next->getFingerprint(prefix / path);
}

std::shared_ptr<const Provenance> FilteringSourceAccessor::getProvenance(const CanonPath & path)
{
    if (provenance)
        return SourceAccessor::getProvenance(path);
    return next->getProvenance(prefix / path);
}

void FilteringSourceAccessor::checkAccess(const CanonPath & path)
{
    if (!isAllowed(path))
        throw makeNotAllowedError(path);
}

struct AllowListSourceAccessorImpl : AllowListSourceAccessor
{
private:
    void anchor() override {};
public:
    struct Prefixes
    {
        typedef boost::unordered_flat_set<std::string, StringViewHash, std::equal_to<>> Set;

        /**
         * The allowed prefixes, i.e. paths that are allowed along with
         * all their descendants.
         */
        Set prefixes;

        /**
         * The strict ancestors of the allowed prefixes. These are
         * allowed (so that the prefixes can be reached), but their
         * other descendants are not.
         */
        Set ancestors;

        void add(const CanonPath & prefix)
        {
            for (auto p = prefix.parent(); p; p = p->parent())
                /* If `p` is already present, then so are its ancestors. */
                if (!ancestors.insert(p->abs()).second)
                    break;
            prefixes.insert(prefix.abs());
        }

        /**
         * Return whether `path` or one of its ancestors is an allowed
         * prefix.
         */
        bool isWithinPrefix(std::string_view path) const
        {
            if (prefixes.empty())
                return false;
            if (prefixes.contains("/"))
                return true;
            /* Check the ancestors of `path` starting with the shortest,
               since allowed prefixes tend to be short (e.g. store
               paths). */
            for (size_t pos = 1;;) {
                auto slash = path.find('/', pos);
                if (prefixes.contains(path.substr(0, slash)))
                    return true;
                if (slash == path.npos)
                    return false;
                pos = slash + 1;
            }
        }

        bool isAllowed(std::string_view path) const
        {
            return ancestors.contains(path) || isWithinPrefix(path);
        }
    };

    SharedSync<Prefixes> allowedPrefixes;

    /**
     * Paths that are allowed, but not their descendants. This is
     * immutable, so it doesn't need synchronisation.
     */
    const std::unordered_set<CanonPath> allowedPaths;

    AllowListSourceAccessorImpl(
        ref<SourceAccessor> next,
        const std::set<CanonPath> & allowedPrefixes,
        const std::unordered_set<CanonPath> & allowedPaths,
        MakeNotAllowedError && makeNotAllowedError)
        : AllowListSourceAccessor(SourcePath(next), std::move(makeNotAllowedError))
        , allowedPaths(allowedPaths)
    {
        auto state(this->allowedPrefixes.lock());
        for (auto & prefix : allowedPrefixes)
            state->add(prefix);
    }

    bool isAllowed(const CanonPath & path) override
    {
        if (!allowedPaths.empty() && allowedPaths.contains(path))
            return true;
        return allowedPrefixes.readLock()->isAllowed(path.abs());
    }

    bool isAllowedRecursively(const CanonPath & path) override
    {
        return allowedPrefixes.readLock()->isWithinPrefix(path.abs());
    }

    void allowPrefix(CanonPath prefix) override
    {
        allowedPrefixes.lock()->add(prefix);
    }
};

ref<AllowListSourceAccessor> AllowListSourceAccessor::create(
    ref<SourceAccessor> next,
    const std::set<CanonPath> & allowedPrefixes,
    const std::unordered_set<CanonPath> & allowedPaths,
    MakeNotAllowedError && makeNotAllowedError)
{
    return make_ref<AllowListSourceAccessorImpl>(next, allowedPrefixes, allowedPaths, std::move(makeNotAllowedError));
}

CachingFilteringSourceAccessor::CachingFilteringSourceAccessor(
    const SourcePath & src, MakeNotAllowedError && makeNotAllowedError)
    : FilteringSourceAccessor(src, std::move(makeNotAllowedError))
    , cache(make_ref<boost::concurrent_flat_map<CanonPath, bool>>())
{
}

bool CachingFilteringSourceAccessor::isAllowed(const CanonPath & path)
{
    if (auto allowed = getConcurrent(*cache, path))
        return *allowed;
    auto res = isAllowedUncached(path);
    cache->emplace(path, res);
    return res;
}

} // namespace nix
