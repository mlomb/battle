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
