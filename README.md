> [!WARNING]
> This repository and crate are still under development, APIs will change, CLI arguments will change, and there is no timeline on sight. I usually work on it prior / during a contest.

> [!NOTE]
> At this point, the project is not really open to big contributions. The shape of the repository is still evolving. Please open an issue first to discuss your idea.
> 
> Regarding AI use for this repository, please read rust-lang's [LLM policy](https://blog.rust-lang.org/inside-rust/2026/08/05/rust-langrust-is-adopting-an-llm-policy/).

# `battle` bot tools

[![](https://img.shields.io/crates/v/battle)](https://crates.io/crates/battle)
[![codecov](https://codecov.io/gh/mlomb/battle/graph/badge.svg?token=748JIN3FVS)](https://codecov.io/gh/mlomb/battle)

Tools for building competitive bots, e.g. for [CodinGame](https://codingame.com) contests.

The project is composed of [multiple Rust crates](https://github.com/mlomb/battle/tree/main/crates), conveniently accesible under the same CLI tool called `battle`. The tools are designed to "just work" and aim to have good DX. 🛠️ Still a lot to do.

## Install

From [crates.io](https://crates.io/crates/battle):

```
cargo install battle
```

or clone and run:

```sh
cargo install --path crates/battle
```

To update, install the crate again or pull the latest changes and run the command again.

## Usage / Cheatsheet

### `battle bundle`

A tool to bundle C++ and Rust projects into a single source unit for submission.

| Example | Note |
| --- | --- |
| `battle bundle` | It will look for a C++ or Rust project (see [What is a project?](https://github.com/mlomb/battle/tree/main/crates/bundler/README.md#what-is-a-project)) and print a single file to stdout. |
| `battle build v5.cpp` | Specify entrypoint manually |
| `battle build main.cpp --output submission.cpp` | Output to a file instead of stdout. |

### `battle build`

A tool to bundle and build your bot and check compilation issues.

| Example | Note |
| --- | --- |
| `battle build` | Same as `bundle`, it will try to find an entrypoint automaticaly. |

### `battle cgsync`

Watches your project, bundles on every change, and pushes the result to the CodinGame browser IDE through the [CG Local extension](https://github.com/jmerle/cg-local-ext). See [cgsync](https://github.com/mlomb/battle/tree/main/crates/cgsync).

| Example | Note |
| --- | --- |
| `battle cgsync` | Auto-detects the project in the current folder. Click the browser extension to attach after starting. |
| `battle cgsync src/main.cpp` | Specify the entrypoint manually (file or folder). |

### `battle worker`

Starts a worker node that the distributed runner uses to play games.

| Example | Note |
| --- | --- |
| `battle worker` | Listens on the default port using *physical CPUs − 2* threads. |
| `battle worker --threads 32 --port 8080` | Override the thread count and listening port. |

### `battle play`

Plays one or more games between bots through the worker pool, streaming results as they finish.

| Example | Note |
| --- | --- |
| `battle play -r cg-fall-2023-fish -a v1.cpp -a v2.cpp` | Play a single game with the given referee and agents (repeat `-a` per agent). |
| `battle play -r ... -a v1.cpp -a v2.cpp -n 100` | Play `-n` games in total. |

### `battle referee-diff`

Runs the same game on a reference and a candidate referee and stops at the first score / status mismatch. Useful when porting or optimizing a referee while preserving outcomes.

| Example | Note |
| --- | --- |
| `battle referee-diff --reference ref-impl --candidate cand-impl -a main.cpp` | Compare both referees using the given agent(s). Use a non-trivial, non-deterministic agent for meaningful comparisons. |
| `battle referee-diff ... --max-games 50` | Run up to `--max-games` identical games before declaring success (default `10`). |

### `battle wrap`

Records and later plays back a command's stdin / stdout / stderr. See [wrapcmd](https://github.com/mlomb/battle/tree/main/crates/wrapcmd).

| Example | Note |
| --- | --- |
| `battle wrap capture out.txt -- ./bot` | Run `./bot` transparently while writing a transcript to `out.txt`. |
| `battle wrap playback out.txt` | Plays back the transcript: writes the recorded stdout/stderr and validates that stdin matches. |


## Tips

* Make aliases for common commands: `battle build` -> `bb`
* Pass the referee as an environment variable to skip writing it each time: `-r some-game` -> `BATTLE_REFEREE=some-game`
* Keep your worker list in an environment variable: `BATTLE_WORKERS=100.100.1.1,100.100.1.2,etc`
