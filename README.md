unfuckify
=========

Fixes readability of C++ sourcecode by resolving `auto` and replacing it with
the real type.


Why
---

`auto` is the worst thing to have happened to C++.

Source code is written once and read many times, so spend the two seconds it
takes to write out the type of the variable. Then people that just want to read
the code don't have to go look up the return type of everything you call.


Usage
-----

To use just run pass `-DCMAKE_EXPORT_COMPILE_COMMANDS=ON` to cmake when
building the project you want to fix, and then run `unfuckify
path/to/buildfolder/compile_commands.json` and either the name of a file or
`--all` to fix all files in the project.

By default it will write the fixed source to `foo.cpp-fixed`, pass `--replace`
to replace the existing file.

This uses libclang to parse and resolve the types, so if your project builds
with clang this should work as well.


Example
-------

In a random CMake based project:
 - `mkdir build && cd build`
 - `CC="clang" CXX="clang++" cmake -DCMAKE_EXPORT_COMPILE_COMMANDS=ON -GNinja ..`
 - `ninja` (to make sure generated files are available)
 - `unfuckify --replace compile_commands.json ../src/main.cpp`


Why the offensive name
----------------------

Like all great projects this was written in a moment of frustration and anger.

Official upstream suggestions for alternative names, if you want to package
this for a distro that doesn't allow swearing:
 - Antonyms of autonomy (courtesy of wordhippo.com):
    - Heteronomy
    - Bondage
    - Subservience
    - Subjugation
    - Other BDSM terms, apparently
 - Auto Annihilator
 - Auto-b-Gone
 - auto-was-a-mistake-please-help
 - N1984, The Name Should Have Given It Away
 - no-n1984


Known issues
------------

`auto` is very magic and hides a _lot_ of complexity, so trying to resolve the
actual types is hard.

If libclang is unable to give a proper type for something (like lambdas), we can't replace it.

There are some issues with libclang's handling of tokens vs. cursors, e. g. it
will give us the fully qualified type (including const, \*, &, etc.), but the
extent we get only covers `auto` itself.

We should handle the cases here properly now, but no guarantees that I have catched all cases. So make sure you review the code it changes.

Cases it fails on:
 - `auto **`

TODO
----

 - Find a proper function signature and create an appropriate std::function for lambdas.
