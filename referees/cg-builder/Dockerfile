# some old referees only compile with the javac of their time: --build-arg MAVEN_IMAGE=maven:3.9.9-eclipse-temurin-8-focal
ARG MAVEN_IMAGE=maven:3.9.9-ibm-semeru-17-focal
FROM ${MAVEN_IMAGE}

COPY build.sh /build.sh
COPY pom-dependencies.xml /pom-dependencies.xml
COPY CLI/ /CLI/
COPY patches/ /patches/
# pre-SDK referees: docker run --entrypoint /legacy/build.sh ...
COPY legacy/ /legacy/

ENTRYPOINT [ "./build.sh" ]
