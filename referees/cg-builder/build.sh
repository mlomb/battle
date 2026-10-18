#!/bin/bash
# Usage: build.sh <CLI file> <git repository> [project subfolder] [output name]

git clone $2 /repo

# some repositories keep the maven project in a subfolder
project="/repo/${3:-.}"

# edit pom.xml
sed -i "/<\/dependencies>/e cat /pom-dependencies.xml" "$project/pom.xml"
sed -i "/<\/project>/e cat /pom-build.xml" "$project/pom.xml"

echo "Modified pom.xml:"
cat "$project/pom.xml"

# remove unnecessary assets
rm -rf "$project/src/main/resources"

# copy CLI files
cli_dir="$project/src/main/java/com/codingame/gameengine/runner"
mkdir -p "$cli_dir"
cp CLI/$1 "$cli_dir/CommandLineInterface.java"

# gather info
artifactId=$(mvn -f "$project/pom.xml" help:evaluate -Dexpression=project.artifactId -q -DforceStdout)
version=$(mvn -f "$project/pom.xml" help:evaluate -Dexpression=project.version -q -DforceStdout)

target="$project/target/$artifactId-$version-jar-with-dependencies.jar"
output="/target/${4:-cg-$artifactId}.jar"

# tests are not part of the referee, skip them (they may not even compile with a recent JDK)
mvn -f "$project/pom.xml" -Dmaven.test.skip=true package
mv "$target" "$output"

echo "Output: $output"
