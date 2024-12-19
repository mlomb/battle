```sh
docker run --rm -it -v ${PWD}/target:/target $(docker build -q .) CommandLineInterface.java https://github.com/CodinGame/SomeChallenge
```

```sh
java -jar --add-opens java.base/java.lang=ALL-UNNAMED target/cg-challenge.jar
```
