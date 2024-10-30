# bundler

A tool to bundle C++ and Rust projects into a single source unit for submission.

## Usage

Open the terminal in a folder which contains a C++ or Rust project (see [What is a project?](#what-is-a-project)), and run:

```sh
bundler
```

This will output the final source code into the console. You can redirect it to a file (`>`) or use `--output`:

```sh
bundler --output submit.cpp
```

## What is a project?

To build a project, the bundler must find an entry point:

- **C++**: a `.cpp` file.
- **Rust**: a `Cargo.toml` file.

The entry point can be defined using `--entry` pointing to an entry file. However, it is practical to point to a folder containing the entry file. By default `--entry` points to the current folder.

When the entry point is a folder, the bundler will look for a `main.cpp` or `Cargo.toml` in the folder specified by `--entry`.

```sh
bundler --entry src
bundler --entry src/main.cpp
```

## How it works

TODO

## Optimization

## Decorators

* `RealParam`
* `IntegerParam`
* TODO: log?
* TODO: arrays?

### Rust

In Rust, you can add the previous decorators to your code to specify parameters to optimize:

```rust
/// RealParam
const FOO: LazyCell<f32> = LazyCell::new(|| 42.0);
```

Internally, this will turn `FOO` into a parameter that can be read from arguments:

```rust
#[doc = " RealParam"]
const FOO: LazyCell<f32> = LazyCell::new(|| {
    std::env::args()
        .skip(1)
        .collect::<Vec<_>>()
        .chunks_exact(2)
        .find(|item| item[0] == "FOO")
        .map(|item| item[1].parse().unwrap())
        .unwrap_or_else(|| 42.0)
});
```

If the parameter is not found, the default value will be used. Requires Rust 1.80.0.

### C++

```cpp

```

↓

```cpp

```

## Future work

- [ ] Basic dead code elimination to reduce size. Probably too hard, maybe for C++ macros and basic heuristics?
- [ ] Cleaning like
    - [ ] Remove duplicate includes (e.g. `#include <iostream>` twice)
- [ ] Add support for other compiled languages?
