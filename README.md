# Bot tools

Tools for building competitive bots. The project is composed of three crates:

| Crate | Docs | Description |
|---|---|---|
| `bundler`  | [bundler/README.md](bundler) | Bundles C++ and Rust projects into a single file for submission |
| `cgsync` | [cgsync/README.md](cgsync) | Watches a project for changes, bundles it (with `bundler`) and syncs it with the [CG Local extension](https://github.com/jmerle/cg-local-ext) |
| `arena` | [arena/README.md](arena) | Automated testing of bots. Parameter optimization. Allows distributed compute using P2P |

The tools are designed to "just work" and aim to have good DX. **Still work in progress.**

# Install binaries

Clone and run:

```
cargo install --path bundler
cargo install --path cgsync
cargo install --path arena
```

# TLDR: Example workflow

This section explains the expected workflow to use the tools. For more information and complex use cases, refer to the READMEs of each tool.

[link-cws]: https://chromewebstore.google.com/detail/cg-local/ihakjfajoihlncbnggmcmmeabclpfdgo
[link-amo]: https://addons.mozilla.org/en-US/firefox/addon/cg-local/

<details>
<summary>CodinGame</summary>
<br>
A new CodinGame contest huh?
<br>

1. Prepare a new **project folder** for you bot. It can be a C++ or Rust project.
  - **C++**: make sure the folder contains a `main.cpp` file.
  - **Rust**: make sure the folder contains a `Cargo.toml` file.


2. Make sure you have the [CG Local Extension](https://github.com/jmerle/cg-local-ext#install) installed in your browser:  
- [**Chrome** extension][link-cws] [<img valign="middle" src="https://img.shields.io/chrome-web-store/v/ihakjfajoihlncbnggmcmmeabclpfdgo.svg?label=%20">][link-cws]  
- [**Firefox** add-on][link-amo] [<img valign="middle" src="https://img.shields.io/amo/v/cg-local.svg?label=%20">][link-amo]  
This will allow you to sync local code with the CodinGame IDE effortlessly.

3. Run `cgsync` in the project folder. This will watch your code and sync it with the browser IDE. **After the command is run, click the extension to initiate the connection.**

4. Develop your bot.

> [!NOTE]  
> To use the `arena` command to test your bot, make sure a referee is available (see the [arena README](arena)).

5. arena...
</details>
