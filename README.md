# Bot tools

Tools for building competitive bots. The project is composed of three crates:

| Crate | Docs | Description |
|---|---|---|
| `bundler`  | [bundler/README.md](bundler) | Bundles C++ and Rust projects into a single file for submission |
| `cgsync` | [cgsync/README.md](cgsync) | Watches a project for changes, bundles it (with `bundler`) and syncs it with the [CG Local extension](https://github.com/jmerle/cg-local-ext) |
| `arena` | TODO | Allows automated testing of bots. Connects to other instances via P2P to run distributed tests |

The tools are designed to "just work" and have the best DX possible.

# Install binaries

Clone and run:

```
cargo install --path bundler
cargo install --path cgsync
cargo install --path arena
```
