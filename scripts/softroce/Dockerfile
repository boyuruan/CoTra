FROM ubuntu:22.04

ENV DEBIAN_FRONTEND=noninteractive

# GCC 11. Universe holds OpenBLAS and Boost.
RUN if [ -f /etc/apt/sources.list ]; then \
      sed -i 's/^# \(deb .* universe\)$/\1/' /etc/apt/sources.list; \
    fi \
 && apt-get update \
 && apt-get install -y --no-install-recommends \
      ca-certificates \
      build-essential \
      cmake \
      pkg-config \
      libnuma-dev \
      libboost-program-options-dev \
      libboost-serialization-dev \
      libboost-iostreams-dev \
      libboost-system-dev \
      libboost-filesystem-dev \
      libopenblas-dev \
      liblapacke-dev \
      libaio-dev \
 && rm -rf /var/lib/apt/lists/*

WORKDIR /src
