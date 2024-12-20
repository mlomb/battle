# Bot tools

Tools for building competitive bots. The project is composed of three crates:

| Crate | Docs | Description |
|---|---|---|
| `bundler`  | [bundler/README.md](bundler) | Bundles C++ and Rust projects into a single file for submission |
| `cgsync` | [cgsync/README.md](cgsync) | Watches a project for changes, bundles it (with `bundler`) and syncs it with the [CG Local extension](https://github.com/jmerle/cg-local-ext) |
| `arena` | [arena/README.md](arena) | Automated testing of bots. Parameter optimization. Allows distributed compute using P2P |

The tools are designed to "just work" and aim to have good DX.

🛠️ **Still work in progress.**

# Install binaries

Clone and run:

```
cargo install --path bundler
cargo install --path cgsync
cargo install --path arena
```

# Example workflow

This section explains the _expected/recommended_ workflow to use the tools. For more information and complex use cases, refer to the READMEs of each tool.

[link-cws]: https://chromewebstore.google.com/detail/cg-local/ihakjfajoihlncbnggmcmmeabclpfdgo
[link-amo]: https://addons.mozilla.org/en-US/firefox/addon/cg-local/

<details>
<summary>CodinGame</summary>
<br>
A new CodinGame contest huh?
<br>

1. Prepare a new **project folder** for you bot, you can use any build system you want. It can be a C++ or Rust project (see [What is a project?](bundler/README.md#what-is-a-project)).

2. Make sure you have the [CG Local Extension](https://github.com/jmerle/cg-local-ext#install) installed in your browser:  
    - [**Chrome** extension][link-cws] [<img valign="middle" src="https://img.shields.io/chrome-web-store/v/ihakjfajoihlncbnggmcmmeabclpfdgo.svg?label=%20">][link-cws]  
    - [**Firefox** add-on][link-amo] [<img valign="middle" src="https://img.shields.io/amo/v/cg-local.svg?label=%20">][link-amo]  

3. Run `cgsync` in the project folder. This will watch your code and sync it with the browser IDE. **After the command is run, click the extension to initiate the connection.**

4. Develop your bot.

5. Now, you want to test changes to your bot. You can create snapshots of your code running `bundler --output versions/v1.cpp` in the project folder. This gives you a self-contained file with the current code.

_In the future, you will be able to define parameters in the code and testing will be a lot easier._

6. Once you have multiple snapshots (versions or features), create a new **arena environment file** named `env.yaml` in the project folder. For example:  
```yml
referee: cg-fall-2023-fish # make sure to use the correct referee (read note below)

agents:
  latest: # name of the agent
    src: main.cpp
  v1:
    src: versions/v1.cpp
  some-feature:
    src: versions/sf.cpp
  # ... add every agent you want
```

7. Run the `arena` command in the folder that contains the `env.yaml` file. It will guide you through the process of constructing the proper command to run the tournament you want. For example:  
```sh
arena tournament --format round-robin -a latest -a v1
```
</details>

> [!NOTE]
> To use the `arena` command to test your bot, make sure a referee is available (see [arena/README.md](arena/README.md#available-referees)). TLDR, you have to wait until I push a build of the referee (and trust me) or build it yourself.
