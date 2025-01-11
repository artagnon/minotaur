# Minotaur: A SIMD-Oriented Synthesizing Superoptimizer

A description of how Minotaur works can be found at [arXiv:2306.00229](https://arxiv.org/abs/2306.00229).

## Build Minotaur from source code

The project includes LLVM and Alive2 as submodules, which are pinned to the right versions on any commit. The transitive dependencies are `z3`, `re2c`, `libhiredis-dev`, and `redis`.

To build and check Minotaur along with its dependencies:

```sh
  $ cd build
  $ cmake -GNinja ..
  $ ninja check-llvm-unit clang
  $ ninja check-alive
  $ ninja check-minotaur
```

## Use Minotaur

By default, Minotaur requires a redis server to be running.

To run the Minotaur on LLVM IR files:

```sh
  $ llvm/bin/opt -S -load-pass-plugin `pwd`/online.so -passes=minotaur input.ll
```

For C/C++ programs, use `minotaur-cc` or `minotaur-cxx`. Minotaur pass is disabled by default; the pass can be enabled by setting environment variable `ENABLE_MINOTAUR`.

```sh
  $ export ENABLE_MINOTAUR=ON
  $ ./minotaur-cc input.c
  $ ./minotaur-cxx input.cpp
```

On the first run, redis is populated with the synthesis results. Subsequent runs on the same input are instantanous, as results are fetched from redis. To flush redis cache, use `redis-cli flushall`.

Run `cache-infer` to retrieve cuts from the cache. To dump synthesized results from cache, use `cache-dump`.
