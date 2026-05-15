have_cxa_throw = false

can_interpose_cxa_throw_test_code = '''
#include <string>
#include <unistd.h>

#define CXA_THROW_ON_LOGIC_ERROR() _exit(0)
#include "interpose-cxa-throw.cc"

int main()
{
    const char * volatile p = nullptr;
    std::string s(p);
    return 1;
}
'''

can_interpose_cxa_throw_result = cxx.run(
  can_interpose_cxa_throw_test_code,
  args : [ '-ldl' ],
  include_directories : include_directories('.'),
  name : 'can interpose __cxa_throw (catches libstdc++ throws)',
)
can_interpose_cxa_throw = can_interpose_cxa_throw_result.compiled() and can_interpose_cxa_throw_result.returncode() == 0

if can_interpose_cxa_throw
  interpose_cxa_throw_lib = static_library(
    'interpose-cxa-throw',
    'interpose-cxa-throw.cc',
    dependencies : cxx.find_library('dl', required : false),
  )

  cxa_throw_dep = declare_dependency(
    link_whole : interpose_cxa_throw_lib,
  )

  have_cxa_throw = true
else
  can_wrap_cxa_throw_test_code = '''
  #include <string>
  #include <unistd.h>

  #define CXA_THROW_ON_LOGIC_ERROR() _exit(0)
  #include "wrap-cxa-throw.cc"

  int main()
  {
      const char * volatile p = nullptr;
      std::string s(p);
      return 1;
  }
  '''

  wrap_cxa_throw_args = [ '-Wl,--wrap=__cxa_throw' ]

  can_wrap_cxa_throw_result = cxx.run(
    can_wrap_cxa_throw_test_code,
    args : wrap_cxa_throw_args,
    include_directories : include_directories('.'),
    name : 'can wrap __cxa_throw (catches libstdc++ throws)',
  )
  can_wrap_cxa_throw = can_wrap_cxa_throw_result.compiled() and can_wrap_cxa_throw_result.returncode() == 0

  if can_wrap_cxa_throw
    wrap_cxa_throw_lib = static_library(
      'wrap-cxa-throw',
      'wrap-cxa-throw.cc',
    )

    cxa_throw_dep = declare_dependency(
      link_whole : wrap_cxa_throw_lib,
      link_args : wrap_cxa_throw_args + [ '-Wl,-u,__wrap___cxa_throw' ],
    )

    have_cxa_throw = true
  endif
endif
