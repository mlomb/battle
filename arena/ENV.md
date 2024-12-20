# Environment definition

An environment file (`env.yaml`) is a [YAML](https://yaml.org) file that defines [the referee](#referees) and [the agents](#agents) that will play the game.

During a contest, you will add and remove agents from this file to test them against each other. Usually, you will snapshot your code each time you submit or make a big change to your bot. This can be done using the [bundler](../bundler) tool (e.g. `bundler --output versions/v3.cpp`).


Take a look at the following `env.yaml` file:

```yaml
referee: cg-winter-2024-sprawl

agents:
  latest:
    src: main.cpp
  v3:
    src: versions/v3.cpp
  starterbot:
    cmd: python starter.py
    files:
        starter.py: ../bots/starter.py
```

In this example, three agents are defined, two by source code and one by a custom command. The referee is selected by key from the [available referees](#available-referees). For more complex definitions, jump to the [referees section](#referees), or jump to the [agents section](#agents) .

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

The referee is the program that implements the game rules. It follows a [protocol to communicate with the arena](./src/referee.rs#L16-L20), and a protocol to communicate with the agents that the agents must respect. It is responsible for starting the agent processes, running the game, and deciding the winner.

### Available referees

For ease of use, some referees are already compiled and ready to be used in [referees/](referees/).

> [!WARNING]
> Running untrusted binaries (referees) is not safe, including the ones located at [referees/](referees/). I encourage you to build the referees yourself. You can find a reproducible script to build CodinGame referees inside the [referees/cg-builder](referees/cg-builder) folder.

| Platform | Season | Contest | **Key** | Players |
|----------|--------|---------|---------|-----------|
| CodinGame | Winter 2024 | [Cellularena](https://www.codingame.com/contests/winter-challenge-2024) | **`cg-winter-2024-sprawl`** | 2 |
| CodinGame | Fall 2023 | [Seabed Security](https://www.codingame.com/multiplayer/bot-programming/seabed-security) | **`cg-fall-2023-fish`** | 2 |

If you want to add referee to the list, please open an issue. I will not merge PRs with binary files.

### Custom referees

Currently, you can't specify custom referees. Eventually, the idea is that you can define a referee the same way you can define agents. After all, a referee is just a program like any other.

You can add your own CodinGame JAR referees to the [referees/](referees/) folder and use them by specifying the key (file name) in the environment file.

## Agents

An agent is a program that respects the referee's protocol so it can play the game.

There are two ways to define an agent:

1. Provide the source code `src` and let the arena compile it for you.
2. Provide a command `cmd` that will run your agent. Parameters are appended at the end of the command.

The following table shows some examples of agent definitions:

<table>
<tr>
<th>Description</th>
<th>Examples</th>
</tr>
<tr>
<td>

Define an agent pointing to a [project entry point](../bundler#what-is-a-project) or bundled source code. **The recommended way.**

</td>
<td>

```yaml
agents:
  latest:
    src: main.cpp
  v15:
    src: versions/v15.cpp
  nntest:
    src: tests/nn.cpp
    files:
      nn.bin: tests/network.bin
  rust_entry:
    src: cratebot/
  rust_bundled:
    src: bundled.rs
```

</td>
</tr>

<tr>
<td>

Define an agent using an arbitrary command.

</td>
<td>

```yaml
agents:
  pybot:
    cmd: python bot.py
    files:
      bot.py: ../bots/bot.py
```

</td>
</tr>

<tr>
<td>

Define an agent using an already compiled binary file.

</td>
<td>

```yaml
agents:
  pybot:
    cmd:
      win: ./bot.exe
      linux: ./bot
    files:
      bot.exe: ./release/bot.exe
      bot: ./release/bot
```

</td>
</tr>
</table>

To every agent, you can pass a `files` key that maps the files that will be copied to the agent's working directory. This is needed when you use a command to run, since the arena can't determine which files are needed. You can also use this to attach assets, like neural networks.
