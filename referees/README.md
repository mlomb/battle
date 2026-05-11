# Referees repository

Referees are provided for convenience.

## Available referees

| Platform | Contest | Reference | Fast |
|-|-|-|-|
| CodinGame | Spring 2024 - Olymbits | ✅ `cg-spring-2024-olymbits-ref` | ✅ `cg-spring-2024-olymbits` |

TODO: write full list

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
