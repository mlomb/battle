# Bot tools

Tools for building competitive bots. The project is composed of four crates:

- `bundler`: Bundles C++ and Rust projects into a single file.
- `cgsync`: Watches a project for changes, bundles it and syncs it with the [CG Local extension](https://github.com/jmerle/cg-local-ext).
- `arena`: Allows automated testing of bots. Connects to other instances via P2P to run distributed tests.
- `optimizer`: Uses the `arena` crate to run parameter searches.

# Install binaries

Clone and run:

```
cargo install --path bundler
cargo install --path cgsync
```

# Binaries

## Bundler

Open the terminal in a folder which contains a C++ or Rust project, and run:

```
bundler
```

This will output the final source into the console.

To specify the entry point or save the output to a file, run:

```
bundler --entry main.cpp --output submit.cpp
```

## CGSync

To bundle and sync a project with CGSync, run:

```
cgsync
```

Extension: https://github.com/jmerle/cg-local-ext


# Optimization

## Decorators

* `RealParam`
* `IntegerParam`
* TODO: log?
* TODO: arrays?

## Rust

In Rust, you can add the previous decorators to your code to specify parameters to optimize:

```
/// RealParam
const FOO: f32 = 42.0;
```

Internally, this will turn `FOO` into a parameter that can be read from arguments:

```
...
```

If the parameter is not found, the default value will be used. Requires Rust 1.80.0.
