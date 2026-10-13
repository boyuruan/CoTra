FROM ubuntu:22.04

ENV DEBIAN_FRONTEND=noninteractive

# GCC 11. Universe holds OpenBLAS, Boost, memcached, and libmemcached.
RUN if [ -f /etc/apt/sources.list ]; then \
      sed -i 's/^# \(deb .* universe\)$/\1/' /etc/apt/sources.list; \
    fi \
 && apt-get update \
 && apt-get install -y --no-install-recommends \
      ca-certificates \
      build-essential \
      cmake \
      pkg-config \
      rdma-core \
      libibverbs-dev \
      librdmacm-dev \
      libmemcached-dev \
      memcached \
      libnuma-dev \
      libboost-program-options-dev \
      libboost-serialization-dev \
      libboost-iostreams-dev \
      libboost-system-dev \
      libboost-filesystem-dev \
      libopenblas-dev \
      liblapacke-dev \
      libaio-dev \
      iproute2 \
      kmod \
      netcat-openbsd \
 && rm -rf /var/lib/apt/lists/*

# Ubuntu's memcached package creates the user "memcache". The scripts pass -u memcached.
RUN id memcached >/dev/null 2>&1 || \
    useradd --system --no-create-home --shell /bin/false memcached

WORKDIR /src
