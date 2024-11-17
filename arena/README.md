# arena

TODO

## Environment

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

## Tournament

```bash
arena tournament
```

# TODO

- [ ] **Add parameter optimization mode**
- [ ] Write `Database` to disk (something like `(time)-(info).arenadb`), then load to:
    - [ ] Resume tournament/optimization
    - [ ] Open UI to view results (`arena view xyz.arenadb`)
