# Bot tools

Tools for building competitive bots. The project is composed of four crates:

- `bundler`: Bundles C++ and Rust projects into a single file for submission. See [bundler/README.md](bundler/README.md).
- `cgsync`: Watches a project for changes, bundles it and syncs it with the [CG Local extension](https://github.com/jmerle/cg-local-ext).
- `arena`: Allows automated testing of bots. Connects to other instances via P2P to run distributed tests.
- `optimizer`: Uses the `arena` crate to run parameter searches.

The tools are designed to "just work" and aim to have the best DX possible.

# Install binaries

Clone and run:

```
cargo install --path bundler
cargo install --path cgsync
```

## CGSync

To bundle and sync a project with CGSync, run:

```
cgsync
```

Extension: https://github.com/jmerle/cg-local-ext

