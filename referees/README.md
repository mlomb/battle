# Referees repository

Referees are provided for convenience.

## Available referees

| Platform | Contest | Validated | Reference | Fast |
|-|-|-|-|-|
| CodinGame | Botters of the Galaxy | ✅ | `cg-botters-of-the-galaxy-ref` <br/> [cg-botters-of-the-galaxy.jar](cg-jar/cg-botters-of-the-galaxy.jar) | `cg-botters-of-the-galaxy` <br/> [cg-botters-of-the-galaxy.cpp](cg-cpp/cg-botters-of-the-galaxy.cpp) |
| CodinGame | Spring 2021 - Photosynthesis | ✅ | `cg-spring-2021-ref` <br/> [cg-spring-2021.jar](cg-jar/cg-spring-2021.jar) | `cg-spring-2021` <br/> [cg-spring-2021.cpp](cg-cpp/cg-spring-2021.cpp) |
| CodinGame | Spring 2022 - Spider Attack | ✅ | `cg-spider-attack-spring-2022-ref` <br/> [cg-spider-attack-spring-2022.jar](cg-jar/cg-spider-attack-spring-2022.jar) | `cg-spider-attack-spring-2022` <br/> [cg-spider-attack-spring-2022.cpp](cg-cpp/cg-spider-attack-spring-2022.cpp) |
| CodinGame | Fall Challenge 2022 - Keep Off the Grass | ✅ | `cg-fall-challenge-2022-keep-off-the-grass-ref` <br/> [cg-fall-challenge-2022-keep-off-the-grass.jar](cg-jar/cg-fall-challenge-2022-keep-off-the-grass.jar) | `cg-fall-challenge-2022-keep-off-the-grass` <br/> [cg-fall-challenge-2022-keep-off-the-grass.cpp](cg-cpp/cg-fall-challenge-2022-keep-off-the-grass.cpp) |
| CodinGame | Spring 2023 - Ants | ✅ | `cg-spring-2023-ants-ref` <br/> [cg-spring-2023-ants.jar](cg-jar/cg-spring-2023-ants.jar) | `cg-spring-2023-ants` <br/> [cg-spring-2023-ants.cpp](cg-cpp/cg-spring-2023-ants.cpp) |
| CodinGame | Fall 2023 - Fish | ✅ | `cg-fall-2023-fish-ref` <br/> [cg-fall-2023-fish.jar](cg-jar/cg-fall-2023-fish.jar) | `cg-fall-2023-fish` <br/> [cg-fall-2023-fish.cpp](cg-cpp/cg-fall-2023-fish.cpp) |
| CodinGame | Winter 2024 - Sprawl | ✅ | `cg-winter-2024-sprawl-ref` <br/> [cg-winter-2024-sprawl.jar](cg-jar/cg-winter-2024-sprawl.jar) | `cg-winter-2024-sprawl` <br/> [cg-winter-2024-sprawl.cpp](cg-cpp/cg-winter-2024-sprawl.cpp) |
| CodinGame | Spring 2024 - Olymbits | ✅ | `cg-spring-2024-olympics-ref` <br/> [cg-spring-2024-olympics.jar](cg-jar/cg-spring-2024-olympics.jar) | `cg-spring-2024-olympics` <br/> [cg-spring-2024-olympics.cpp](cg-cpp/cg-spring-2024-olympics.cpp) |
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
