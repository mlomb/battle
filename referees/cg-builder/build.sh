#!/bin/bash
# Usage: build.sh <CLI file> <git repository> [project subfolder] [output name] [resources to keep...]
set -e

git clone $2 /repo

# a repository that does not build as published gets patches/<output name>.patch (e.g. Code of Kutulu)
if [ -f "/patches/$4.patch" ]; then
    git -C /repo apply "/patches/$4.patch"
fi

# some repositories keep the maven project in a subfolder
project="/repo/${3:-.}"

# edit pom.xml
sed -i "/<\/dependencies>/e cat /pom-dependencies.xml" "$project/pom.xml"

echo "Modified pom.xml:"
cat "$project/pom.xml"

# remove unnecessary assets, except the files the referee reads, listed after the output name (e.g. cardlist.txt)
resources="$project/src/main/resources"
keep=$(mktemp -d)
for f in "${@:5}"; do
    mkdir -p "$keep/$(dirname "$f")"
    cp "$resources/$f" "$keep/$f"
done
rm -rf "$resources"
mv "$keep" "$resources"

# a referee that only knows its own leagues plays the one given (e.g. LEAGUE_LEVEL=2), read by CommandLineInterface-v2
if [ -n "$LEAGUE_LEVEL" ]; then
    mkdir -p "$resources/cg-builder"
    echo "$LEAGUE_LEVEL" > "$resources/cg-builder/league.level"
fi

# copy CLI files
cli_dir="$project/src/main/java/com/codingame/gameengine/runner"
mkdir -p "$cli_dir"
cp CLI/$1 "$cli_dir/CommandLineInterface.java"

# gather info
artifactId=$(mvn -f "$project/pom.xml" help:evaluate -Dexpression=project.artifactId -q -DforceStdout)
output="/target/${4:-cg-$artifactId}.jar"

# compile with the project's own build (e.g. its Kotlin compiler; tests are not part of the referee), then pack the
# classes and their dependencies into one jar
mvn -f "$project/pom.xml" compile dependency:copy-dependencies -DincludeScope=runtime
classes=$(mktemp -d)
for dependency in "$project"/target/dependency/*.jar; do
    (cd "$classes" && jar xf "$dependency")
done
cp -r "$project/target/classes/." "$classes"
rm -f "$classes/META-INF/MANIFEST.MF"
jar cfe "$output" com.codingame.gameengine.runner.CommandLineInterface -C "$classes" .

echo "Output: $output"
