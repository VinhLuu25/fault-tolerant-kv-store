# syntax=docker/dockerfile:1

FROM ubuntu:24.04 AS builder

RUN apt-get update \
    && DEBIAN_FRONTEND=noninteractive apt-get install --no-install-recommends -y \
        build-essential \
        ca-certificates \
        cmake \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /workspace
COPY . .

RUN cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF \
    && cmake --build build --parallel

FROM ubuntu:24.04 AS runtime

RUN apt-get update \
    && DEBIAN_FRONTEND=noninteractive apt-get install --no-install-recommends -y \
        libstdc++6 \
    && rm -rf /var/lib/apt/lists/*

RUN useradd --create-home --uid 10001 ftkv
COPY --from=builder /workspace/build/ftkv_server /usr/local/bin/ftkv_server

USER ftkv
ENTRYPOINT ["ftkv_server"]
