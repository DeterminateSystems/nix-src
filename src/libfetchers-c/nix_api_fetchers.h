#ifndef NIX_API_FETCHERS_H
#define NIX_API_FETCHERS_H
/** @defgroup libfetchers libfetchers
 * @brief Bindings to the Nix fetchers library
 * @{
 */
/** @file
 * @brief Main entry for the libfetchers C bindings
 */

#include "nix_api_util.h"

#ifdef __cplusplus
extern "C" {
#endif
// cffi start

// Type definitions
/**
 * @brief Shared settings object
 */
typedef struct nix_fetchers_settings nix_fetchers_settings;

/**
 * @brief Initialize the Nix fetchers.
 * @ingroup libfetcher_init
 *
 * This function must be called at least once,
 * at some point before using a fetcher for the first time.
 * This function can be called multiple times, and it idempotent.
 *
 * @param[out] context Optional, stores error information
 * @return NIX_OK if the initialization was successful, an error code otherwise.
 */
nix_err nix_libfetchers_init(nix_c_context * context);

nix_fetchers_settings * nix_fetchers_settings_new(nix_c_context * context);

void nix_fetchers_settings_free(nix_fetchers_settings * settings);

#ifdef __cplusplus
} // extern "C"
#endif

#endif // NIX_API_FETCHERS_H
