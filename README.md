# Bot tools

Tools for building competitive bots. The project is composed of four crates:

- `bundler`: Bundles C++ and Rust projects into a single file.
- `cgsync`: Watches a project for changes, bundles it and syncs it with the [CG Local extension](https://github.com/jmerle/cg-local-ext).
- `testbed`: Allows automated testing of bots. Connects to other instances via P2P to run distributed tests.
- `optimizer`: Uses the `testbed` crate to run parameter searches.

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
