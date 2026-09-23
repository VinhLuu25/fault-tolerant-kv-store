# syntax=docker/dockerfile:1

FROM ubuntu:24.04 AS builder

RUN apt-get update \
    && DEBIAN_FRONTEND=noninteractive apt-get install --no-install-recommends -y \
        build-essential \
        ca-certificates \
        cmake \
        libgrpc++-dev \
        libprotobuf-dev \
        protobuf-compiler \
        protobuf-compiler-grpc \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /workspace
COPY . .

RUN cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF \
    && cmake --build build --parallel

FROM ubuntu:24.04 AS runtime

RUN apt-get update \
    && DEBIAN_FRONTEND=noninteractive apt-get install --no-install-recommends -y \
        ca-certificates \
        libgrpc++1.51t64 \
        libprotobuf32t64 \
    && rm -rf /var/lib/apt/lists/* \
    && useradd --create-home --uid 10001 ftkv \
    && install -d -o ftkv -g ftkv /var/lib/ftkv

COPY --from=builder /workspace/build/ftkv_server /usr/local/bin/ftkv_server

USER ftkv
WORKDIR /var/lib/ftkv
EXPOSE 50051
STOPSIGNAL SIGTERM
ENTRYPOINT ["ftkv_server"]
