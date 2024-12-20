# arena

The arena crate allows automated testing of bots.

[GIF terminal]

### Features

- Supports both Linux and Windows
- Agents can be defined either by source code or executables
  - source code is automatically bundled (see [bundler](../bundler)) and compiled
  - executables are chosen based on the platform
- Run tournaments between agents
  - round-robin (all vs all)
  - gauntlet (one vs all)
  - ~~matchmaking~~ (TODO)
- Computation can be...
  - paralellized using multiple threads
  - distributed using P2P workers 🚀
- It has an interactive mode that guides you through the process of creating the tournament you want
- Referees included and ready to be used

Check out the [TODO list](#todo) for upcoming features.

## Usage

Create an [environment file](ENV.md) (`env.yaml`) in your project folder. Then run `arena` to be guided through the process of creating a tournament or jump to the [tournament section](#tournament).

If you want to distribute the computation, look at the [execution mode](#execution-mode-local-vs-p2p) section.

## Tournament

> [!NOTE]
> I recommend you to set up your env file and let the `arena` command guide you instead of constructing the command yourself. It will check for problems in your env file too.

The base command for running tournaments is `arena tournament`.

You **must** specify the following options:

- `--format`: The format of the tournament.
    - `round-robin`: All agents play against each other.
    - `gauntlet`: First agent passed plays against all others.
    - `matchmaking`: TODO
- `-a <name>` or `--agent <name>`: The agents to include in the tournament. The name must match agents defined in the environment file. You can specify as many agents you want. Agents for a match will be chosen based on the tournament format.
    - e.g. `--agent latest --agent v1 --agent v2`
    - e.g. `-a latest -a v1 -a v2`

You **may** specify the following options:

- `--N <number>` or `--games <number>`: The number of matches to run. By default, it is `0`, which means it will run matches until stopped.
- `--threads <number>` or `--network`: The execution mode. See [execution mode](#execution-mode-local-vs-p2p).

## Parameter optimization

TODO! We must implement parameters in the bundler first

## Execution mode (local vs. P2P)

You need to decide wether you want to run matches in the same process or distribute them using P2P:

1. `--threads` (default): Matches are run in the same process, and you can specify the number of threads to use.
2. `--network`: No matches will run in the process and instead will wait until a P2P worker is available to receive matches.

To start a P2P worker, run `arena worker`. Yes, it is that simple. You can specify the number of threads as well (`arena worker --threads 4`).

**Note that only nodes in the same network can discover each other for now.**

Note that by default, `--threads` has a value of `0`, which means it will use the number of **physical cores** in the machine **minus two**.

## FAQ

<details>
<summary>How is source code compiled?</summary>
<br>

> **C++**: using the `cc` crate, that autodetects the MSVC/g++ compiler.  
> **Rust**: invoking `cargo build --release`.
> 
> You may want to look at [source_builder.rs](./src/exec/source_builder.rs).

</details>

<details>
<summary>Why make snapshots of code, instead of saving binaries?</summary>
<br>

> You may think this is obvious. Well, it is, however I known some people (including myself) that used to save binaries instead of code, even knowing that is a bad idea. Just plain laziness I guess (or the lack of a bundler).
> 
> Storing code snapshots allows you to go back and inspect older versions: have them in version control, inspect them, restore them, diff them, compile them in other platforms, etc.
> 
> Binaries give you one advantage though: you can share binaries with other people without sharing the code. The arena supports binaries for this reason, however I do not adivse this, since some may consider it cheating.
</details>

## TODO

- [ ] Handle all kind of errors better. Mostly related to crashes, timeouts, lost P2P packets, etc.
- [ ] **Add parameter optimization mode**
- [ ] Write `Database` to disk (something like `(time)-(info).arenadb`), then load to:
    - [ ] Resume tournament/optimization
    - [ ] Open UI to view results (`arena view xyz.arenadb`)
    - or maybe have a more human readable format?
- [ ] Add `--players` flag to control how many players should play in a match. $min\_players <= players <= max\_players$
