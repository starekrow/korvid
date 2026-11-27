# slang's shader_cpu rule, where the target's C library is not the one slangc was built
# against (Android's bionic, from a Linux or macOS host): slangc pastes its C++ prelude into
# each shader with the headers that prelude includes already expanded, as the C library of
# the machine slangc was built on had them (glibc's <math.h>, <stdint.h>, ...). The
# generated file is compiled inside a namespace after renderer-cpu's shader_glue.h has
# included the target's own headers; glibc's copies, under glibc's guards, are not skipped
# by bionic's and do not compile.
#
# This puts back what the prelude says in its non-LLVM branch, the includes themselves:
# everything between the first "#else // SLANG_LLVM" and the "#endif // SLANG_LLVM" after
# it. Included again inside the namespace, each is a no-op behind the target's own guard.
# A file without the two lines is passed through unchanged.
#
#   awk -f cpu_prelude.awk shader.cpu.cpp > out
!done && !skip && /^#else \/\/ SLANG_LLVM/ {
    print
    print "#if SLANG_GCC_FAMILY && __GNUC__ < 6"
    print "#include <cmath>"
    print "#define SLANG_PRELUDE_STD std::"
    print "#else"
    print "#include <math.h>"
    print "#define SLANG_PRELUDE_STD"
    print "#endif"
    print "#include <assert.h>"
    print "#include <stdint.h>"
    print "#include <stdlib.h>"
    print "#include <string.h>"
    skip = 1
    next
}
skip && /^#endif \/\/ SLANG_LLVM/ { skip = 0; done = 1 }
!skip { print }
