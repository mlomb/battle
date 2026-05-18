FROM rust:latest AS builder

WORKDIR /build
COPY Cargo.toml Cargo.lock ./
COPY crates/ crates/

RUN cargo build --release -p battle

# -----------------------------------------------------------------------------

FROM ubuntu:26.04

ENV DEBIAN_FRONTEND=noninteractive

ARG JAVA_VERSION=25
ARG ZIG_VERSION=0.16.0
ARG RUST_VERSION=1.95.0

# Base utilities + Java (for .jar referees) + gcc (linker for cargo at bot-compile time)
RUN apt-get update && apt-get install -y --no-install-recommends \
    ca-certificates \
    curl \
    xz-utils \
    gcc \
    openjdk-${JAVA_VERSION}-jre-headless \
    && rm -rf /var/lib/apt/lists/*

# Zig (for C++ compilation via `zig c++`)
RUN curl -fsSL "https://ziglang.org/download/${ZIG_VERSION}/zig-x86_64-linux-${ZIG_VERSION}.tar.xz" \
    | tar -xJ -C /usr/local && \
    ln -s /usr/local/zig-x86_64-linux-${ZIG_VERSION}/zig /usr/local/bin/zig

# Rust toolchain (for `cargo build` at bot-compile time)
ENV RUSTUP_HOME=/usr/local/rustup \
    CARGO_HOME=/usr/local/cargo \
    PATH=/usr/local/cargo/bin:$PATH
RUN curl -fsSL https://sh.rustup.rs | sh -s -- -y --no-modify-path --profile minimal --default-toolchain ${RUST_VERSION}

# Copy the compiled battle binary from the builder
COPY --from=builder /build/target/release/battle /usr/local/bin/battle

EXPOSE 13670
ENV WORKER_THREADS=0

CMD battle worker --threads ${WORKER_THREADS}
