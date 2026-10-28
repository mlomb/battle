#!/bin/bash
# Builds the jar of a pre-SDK CodinGame referee: one Java file with `class Referee extends MultiReferee` (the old
# AbstractReferee protocol), run by the SDK 4.5.0 runner through LegacyRefereeAgent (see its javadoc).
#
# Usage: legacy/build.sh <git repository> <commit> <path of the referee in the repository> <output name> [extras...]
#
# The referee's file is copied as is, as src/main/java/Referee.java (the default package). The optional extras after
# the output name are:
#   - files of the builder's legacy/ folder (e.g. the base classes for a repository that only has the game's class:
#     bases/base-2017-cotc.java), copied next to it;
#   - *.imports files of legacy/: import blocks (e.g. imports/java-2017.imports), prepended to Referee.java in the
#     order given, for a game class published without its imports (Code4Life, Wondev Woman);
#   - NAME=VALUE: the initial value of the referee's static field NAME becomes VALUE (a number, true/false or an
#     identifier), for a referee whose league is a constant that selects a lower league (Wondev Woman:
#     GAME_VERSION=3). Exactly one `static ... NAME = ...;` declaration must match.
# A patch named after the output, patches/<output name>.patch (as for build.sh), is applied to the repository right
# after the checkout, before the copy: e.g. Coders Strike Back's rules of CodinGame's live game.
# Without .imports and NAME=VALUE extras and without a patch, Referee.java is the referee's file byte for byte.
set -e

if [ $# -lt 4 ]; then
    echo "usage: $0 <git repository> <commit> <path of the referee in the repository> <output name> [extras...]" >&2
    exit 1
fi

git clone "$1" /repo
git -C /repo checkout --detach "$2"
echo "Referee: $1 @ $(git -C /repo rev-parse HEAD), $3"
if [ -f "/patches/$4.patch" ]; then
    git -C /repo apply "/patches/$4.patch" || exit 1
    echo "Patch: patches/$4.patch"
fi

project=/legacy-project
mkdir -p "$project"
cp /legacy/pom.xml "$project/pom.xml"
cp -r /legacy/src "$project/src"
imports=()
overrides=()
for f in "${@:5}"; do
    case "$f" in
    *=*) overrides+=("$f") ;;
    *.imports) imports+=("/legacy/$f") ;;
    *) cp "/legacy/$f" "$project/src/main/java/" || exit 1 ;;
    esac
done
referee="$project/src/main/java/Referee.java"
cat "${imports[@]}" "/repo/$3" > "$referee"
for o in "${overrides[@]}"; do
    name="${o%%=*}"
    value="${o#*=}"
    if ! [[ "$name" =~ ^[A-Za-z_][A-Za-z0-9_]*$ && "$value" =~ ^[A-Za-z0-9_.+-]+$ ]]; then
        echo "invalid override: $o" >&2
        exit 1
    fi
    pattern="^(([^;=]*[^A-Za-z0-9_])?static[[:space:]][^;=(]*[[:space:]]$name[[:space:]]*=[[:space:]]*)[^;]*;"
    count=$(grep -cE "$pattern" "$referee" || true)
    if [ "$count" != 1 ]; then
        echo "override $o: $count declarations of a static field $name" >&2
        exit 1
    fi
    echo "Override: $(grep -E "$pattern" "$referee" | sed -E 's/^[[:space:]]+//') -> $value"
    sed -E -i "s/$pattern/\\1$value;/" "$referee"
done

mvn -f "$project/pom.xml" package
mv "$project/target/legacy-referee-1.0-jar-with-dependencies.jar" "/target/$4.jar"

echo "Output: /target/$4.jar"
