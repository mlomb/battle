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

## Parameters

TODO

arrays are converted to ARR[0], ARR[1], ARR[2]

ARR[0] { min=0, max=5, default=3 }

## How it works

### Rust

From the entry point `Cargo.toml`, the bundler will look for the first binary target in the package. It will use the entry source file (`main.rs`) to start the process. If the package contains a library, the bundler will also process the library code (`lib.rs`) and merge it with the binary code. 

The source code is parsed using the `syn` crate to create a syntax tree. The code is then transformed using multiple `syn::VisitMut`, in the following order:

1. **ModInliner**: Starts from the entry source file (e.g. `main.rs`) and recursively goes through all `mod` statements. It looks for the mod source file (`name.rs` or `name/mod.rs`), parses it with `syn` and inlines it: `mod name;` -> `mod { ... };`.
2. **TestRemover**: Removes...
   - modules marked with `#[cfg(test)]`.
   - functions marked with `#[test]`.
3. **ParameterParser**: Parses parameter annotations. TODO
4. **AttributeRemover**: Removes the attributes...
   - `#[doc="..."]`, i.e. comments.
   - `#[wasm_bindgen]`.
   
   Note that the attribute line is removed and not the items, like in 2.
5. **UseTrimmer**: If the package is a library and the entry point is a binary in `src/bin`, the code may refer to code in the library using `use package::...`. Since the code is inlined, the prefix `package::` is removed: `use package::{ ... }` -> `use { ... }`. Note that this solution is not perfect and may generate invalid code in some cases.

The `syn::File` is then converted to string and formatted with `rustfmt`.

### C++

TODO


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

- [ ] Rust: inline `include_str!` and `include_bytes!` calls.
- [ ] Basic dead code elimination to reduce size. Probably too hard, maybe for C++ macros and basic heuristics?
- [ ] Cleaning like
    - [ ] Remove duplicate includes (e.g. `#include <iostream>` twice)
- [ ] Add support for other compiled languages?

