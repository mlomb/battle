# arena

TODO

## Environment definition

...

```yaml
referee: cg-2024-summer-olympics

min_agents: 2
max_agents: 2

agents:
  latest:
    src: ../bots/main.cpp
  v15:
    src: ../bots/v15.cpp
  other:
    win_bin: ../bots/other.exe
```

## Referees

A

## Tournament

```bash
arena tournament
```

## Parameter optimization

TODO!

## FAQ

<details>
<summary>Why make snapshots of code, instead of saving binaries?</summary>
<br>

You may think this is obvious. Well, it is, however I known some people (including myself) that used to save binaries instead of code, even knowing that is a bad idea. Just plain laziness I guess (or the lack of a bundler).

Storing code snapshots allows you to go back and inspect older versions: have them in version control, inspect them, restore them, diff them, compile them in other platforms, etc.

Binaries give you one advantage though: you can share binaries with other people without sharing the code. The arena supports binaries for this reason, however I do not adivse this, since some may consider it cheating.
</details>

# TODO

- [ ] **Add parameter optimization mode**
- [ ] Write `Database` to disk (something like `(time)-(info).arenadb`), then load to:
    - [ ] Resume tournament/optimization
    - [ ] Open UI to view results (`arena view xyz.arenadb`)
