FROM ubuntu:24.04

RUN apt-get update -yqq && \
    apt-get install -yqq \
        g++ \
        cmake \
        git \
        make \
        libwolfssl-dev \
        libprotobuf-dev \
        protobuf-compiler \
        libflatbuffers-dev \
        flatbuffers-compiler \
    && rm -rf /var/lib/apt/lists/*

COPY ./ /async_net

WORKDIR /async_net

RUN cmake -B build \
    -DCMAKE_BUILD_TYPE=Release \
    -DASYNC_NET_WITH_SSL=ON \
    -DASYNC_NET_WITH_GRPC=OFF \
    -DASYNC_NET_WITH_FRPC=OFF \
    -DASYNC_NET_WITH_HTTP3=OFF \
    && cmake --build build --target te_bench -j$(nproc)

EXPOSE 8080

CMD ["./build/benchmarks/async_net/te_bench", "8080"]
