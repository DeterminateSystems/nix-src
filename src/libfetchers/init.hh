#pragma once

namespace nix::fetchers {

void registerTarballInputScheme();
void registerFileInputScheme();
void registerGitInputScheme();
void registerGitHubInputScheme();
void registerGitLabInputScheme();
void registerSourceHutInputScheme();
void registerPathInputScheme();
void registerIndirectInputScheme();
void registerMercurialInputScheme();

} // namespace nix::fetchers
