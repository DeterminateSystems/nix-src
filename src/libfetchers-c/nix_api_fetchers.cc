#include <nix/fetchers/fetchers.hh>

#include "nix_api_fetchers.h"
#include "nix_api_fetchers_internal.hh"
#include "nix_api_util_internal.h"

extern "C" {

nix_err nix_libfetchers_init(nix_c_context * context)
{
    if (context)
        context->last_err_code = NIX_OK;
    try {
        nix::initFetchers();
    }
    NIXC_CATCH_ERRS
}

nix_fetchers_settings * nix_fetchers_settings_new(nix_c_context * context)
{
    try {
        auto fetchersSettings = nix::make_ref<nix::fetchers::Settings>(nix::fetchers::Settings{});
        return new nix_fetchers_settings{
            .settings = fetchersSettings,
        };
    }
    NIXC_CATCH_ERRS_NULL
}

void nix_fetchers_settings_free(nix_fetchers_settings * settings)
{
    delete settings;
}

} // extern "C"
