# arena

TODO

## Environment definition

...

```yaml
referee: cg-winter-2024-sprawl

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

> [!NOTE]
> If your IDE supports the [yaml-language-server](https://github.com/redhat-developer/yaml-language-server) (use the [YAML extension](https://marketplace.visualstudio.com/items?itemName=redhat.vscode-yaml) for VSCode), then you can enable schema validation by adding the following line at the top of the `env.yaml` file:
> ```yaml
> # yaml-language-server: $schema=https://raw.githubusercontent.com/mlomb/bot-tools/refs/heads/main/arena/env.schema.json
> ```
> In VSCode, you can associate the schema globally editing your user or workspace settings:
> ```json
> "yaml.schemas": {
>  "https://raw.githubusercontent.com/mlomb/bot-tools/refs/heads/main/arena/env.schema.json": "env.yaml",
> }
> ```


## Referees

The referee is the program that implements the game rules. It starts each of the agents and runs the game.


### Available referees

| Platform | Season | Contest | **Key** | Players |
|----------|--------|---------|---------|-----------|
| CodinGame | Winter 2024 | [Cellularena](https://www.codingame.com/contests/winter-challenge-2024) | `cg-winter-2024-sprawl` | 2 |
| CodinGame | Fall 2023 | [Seabed Security](https://www.codingame.com/multiplayer/bot-programming/seabed-security) | `cg-fall-2023-fish` | 2 |

### Custom referee

Currently, you can't specify 

Eventually, the idea is that you can define a referee the same way you can define agents.

## Tournament

```bash
arena tournament
```

## Parameter optimization

TODO! We must implement parameters in the bundler first

## FAQ

<details>
<summary>Why make snapshots of code, instead of saving binaries?</summary>
<br>

You may think this is obvious. Well, it is, however I known some people (including myself) that used to save binaries instead of code, even knowing that is a bad idea. Just plain laziness I guess (or the lack of a bundler).

Storing code snapshots allows you to go back and inspect older versions: have them in version control, inspect them, restore them, diff them, compile them in other platforms, etc.

Binaries give you one advantage though: you can share binaries with other people without sharing the code. The arena supports binaries for this reason, however I do not adivse this, since some may consider it cheating.
</details>

## TODO

- [ ] Handle all kind of errors better. Mostly related to crashes, timeouts, lost P2P packets, etc.
- [ ] **Add parameter optimization mode**
- [ ] Write `Database` to disk (something like `(time)-(info).arenadb`), then load to:
    - [ ] Resume tournament/optimization
    - [ ] Open UI to view results (`arena view xyz.arenadb`)
