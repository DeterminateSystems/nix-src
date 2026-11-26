sources += files(
  'bakedDerivation.cc',
  'context.cc',
  'fetchClosure.cc',
  'fetchMercurial.cc',
  'fetchTree.cc',
  'fromTOML.cc',
  'regex.cc',
)

if wasmtime_required.enabled()
  sources += files('wasm.cc')
endif
