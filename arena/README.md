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

## Tournament

```bash
arena tournament
```

## Parameter optimization

TODO! We must implement parameters in the bundler first

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

