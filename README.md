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
 - `unfuckify compile_commands.json ../src/main.cpp`