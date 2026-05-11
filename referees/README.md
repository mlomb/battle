# Referees repository

Referees are provided for convenience.

## Available referees

| Platform | Contest | Validated | Reference | Fast |
|-|-|-|-|-|
| CodinGame | Spring 2021 - Mad Pod Racing | ❌ | `cg-spring-2021-ref` | |
| CodinGame | Spring 2022 - Spider Attack | ❌ | `cg-spider-attack-spring-2022-ref` | |
| CodinGame | Fall Challenge 2022 - Keep Off the Grass | ❌ | `cg-fall-challenge-2022-keep-off-the-grass-ref` | |
| CodinGame | Spring 2023 - Ants | ❌ | `cg-spring-2023-ants-ref` | |
| CodinGame | Fall 2023 - Fish | ❌ | `cg-fall-2023-fish-ref` | |
| CodinGame | Winter 2024 - Sprawl | ❌ | `cg-winter-2024-sprawl-ref` | |
| CodinGame | Spring 2024 - Olymbits | ✅ | `cg-spring-2024-olymbits-ref` <br/> [cg-spring-2024-olymbits.jar](cg-jar/cg-spring-2024-olympics.jar) | `cg-spring-2024-olymbits` <br/> [cg-spring-2024-olympics.cpp](cg-cpp/cg-spring-2024-olympics.cpp) |
| CodinGame | Fall Challenge 2024 - Moon City | ❌ | `cg-fall-challenge-2024-moon-city-ref` | |

## CodinGame

For CodinGame contests, there are two types of referees:

* the official Java referee built using Docker (see [cg-builder](./cg-builder/)) and,
* a fast and verified C++ version (see [cg-cpp](./cg-cpp/))
  * _usually generated with a long session of an LLM making use of `referee-diff`_

Most of the time you will want to use the C++ version since it is WAY WAY faster, usually due Java startup taking _seconds_. For Olymbits 2024 I measured 80 seconds vs 2 seconds for 100 games (8 threads).

Java referees are stored in the repository as compiled Jar binaries. C++ referees are stored as source code.
 
> [!WARNING]
> I will never distribute Java referees via [crates.io](https://crates.io), only C++ ones via source code.

If for some reason you want to revalidate an existing referees, I encourage you to rebuild them, don't trust my builds :).

### Validating a referee

To make sure two referees are behaving the same way, you need to make use of the `referee-diff` command:

```sh
battle referee-diff
    --reference cg-spring-2024-olympics-ref
    --candidate wip_referee.cpp
    --max-games 1000
    -a agent1.cpp
    -a agent2.cpp
    -a agent3.cpp
```

You MUST use deterministic agents otherwise it will be impossible to make them match. You also want to use non trivial bots, otherwise we will fail to explore lots of variations.

Remember, we can show if two referees disagree, but we can't prove they won't.
