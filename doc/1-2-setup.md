# Dale

[Previous](./1-1-introduction.md) | [Next](./1-3-types.md)

## 1.2 Setup

The Dale compiler is written in C++. It does not depend on any C++11
features, so most versions of clang/gcc should suffice.

### External dependencies

  * LLVM (3.2-3.5)
  * libffi

### Tested platforms

  * Linux (Debian), x86 and x86-64
  * OS X 10.9.2 (Mavericks)

### Building

From the 'dale' directory, for an out-of-tree (recommended) build:

        mkdir ../build
        cd ../build
        cmake ../dale
        make
        make tests
        make install
    
cmake needs to know the location of the ffi.h header. If this isn't
found in a default location, then it has to be provided during the
third step: e.g.

        cmake ../dale -DFFI_INCLUDE_DIR=/usr/include/i386-linux-gnu/

By default, the installation prefix is `/usr/local/`. The compiler
executable (`dalec`) is installed to `/usr/local/bin`, its manpage is
installed to `/usr/local/share/man`, and all libraries are installed
to `/usr/local/lib/dale`.

The tests are written using Perl. `IPC::Run` is the only non-core
dependency required for testing: it can be installed by running `cpan
IPC::Run`, or by way of the system package manager.

### Building on OS X

Internally, the compiler uses the system's C compiler (`cc`) for
assembly and linking. If the version of LLVM being used produces
assembly that can't be handled by the system's compiler, then errors
about 'unknown directives' may be seen. The easiest way to fix this is
to make sure that `cc` maps to a version of `clang` that refers to the
same version of LLVM as that set via DLLVM\_CONFIG. With Homebrew,
this is done like so:

        brew install llvm --with-clang
        ln -s /usr/local/Cellar/llvm/{version}/bin/clang /usr/local/bin/cc
        export PATH=/usr/local/bin:$PATH

prior to running a build as per the previous instructions.

### Hello world

Copy the following into a file called `hello-world.dt`:

        (import cstdio)
    
        (def main (fn extern-c int (void)
          (printf "Hello, world\n")))
    
Compile it and run it:

        user@main:~$ dalec hello-world.dt
        user@main:~$ ./a.out
        Hello, world
        user@main:~$

The compiler manpage has more detail on supported flags, but most
things are as per other languages' compilers (`-c`, `-o`, etc.).

[Previous](./1-1-introduction.md) | [Next](./1-3-types.md)
