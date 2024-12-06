#!/bin/bash

git clone $1 /repo

# edit pom.xml
sed -i "/<\/dependencies>/e cat /pom-dependencies.xml" /repo/pom.xml
sed -i "/<\/project>/e cat /pom-build.xml" /repo/pom.xml

echo "Modified pom.xml:"
cat /repo/pom.xml

# remove unnecessary assets
rm -rf /repo/src/main/resources

# copy CLI files
cli_dir=/repo/src/main/java/com/codingame/gameengine/runner
mkdir -p $cli_dir
cp /CommandLineInterface.java $cli_dir/CommandLineInterface.java

# gather info
artifactId=$(mvn -f /repo/pom.xml help:evaluate -Dexpression=project.artifactId -q -DforceStdout)
version=$(mvn -f /repo/pom.xml help:evaluate -Dexpression=project.version -q -DforceStdout)

target=/repo/target/$artifactId-$version-jar-with-dependencies.jar
output=/target/$artifactId.jar

mvn -f /repo/pom.xml package
mv $target $output

echo "Output: $output"


