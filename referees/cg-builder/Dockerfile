FROM maven:3.9.9-ibm-semeru-17-focal

COPY build.sh /build.sh
COPY pom-build.xml /pom-build.xml
COPY pom-dependencies.xml /pom-dependencies.xml
COPY CLI/ /CLI/

ENTRYPOINT [ "./build.sh" ]
