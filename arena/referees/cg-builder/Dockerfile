FROM maven:3.9.9-ibm-semeru-17-focal

COPY build.sh /build.sh
COPY CommandLineInterface.java /CommandLineInterface.java
COPY pom-build.xml /pom-build.xml
COPY pom-dependencies.xml /pom-dependencies.xml

ENTRYPOINT [ "./build.sh" ]
