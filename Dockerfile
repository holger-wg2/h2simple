FROM debian:buster-slim as builder
ENV DEBIAN_FRONTEND noninteractive

RUN apt-get update && apt-get install -yq g++ \ 
	make \
	binutils \
	autoconf \
	automake \
	autotools-dev \
	libtool pkg-config \
	zlib1g-dev \
	libcunit1-dev \
	libssl-dev \
	libxml2-dev \
	libev-dev \
	libevent-dev \
	libjansson-dev \
	libc-ares-dev \
	libjemalloc-dev \
	libsystemd-dev \
	cython \
	python3-dev \
	python-setuptools \
	nghttp2 \
	libnghttp2-dev \
	git && \
	apt-get clean

RUN git clone https://github.com/holger-wg2/h2simple.git /root/h2simple
ARG NGHTTP2_INCDIR=/usr/include/nghttp2
ARG NGHTTP2_LIBDIR=/lib/x86_64-linux-gnu
RUN cd /root/h2simple && NGHTTP2_INCDIR=${NGHTTP2_INCDIR} NGHTTP2_LIBDIR=${NGHTTP2_LIBDIR} make

FROM debian:buster-slim as h2-simple
ENV DEBIAN_FRONTEND noninteractive
RUN apt-get update && apt-get install  -yq --no-install-recommends \
    nghttp2 && \
    apt-get clean
COPY --from=builder /root/h2simple/h2cli /usr/local/bin/
COPY --from=builder /root/h2simple/h2svr /usr/local/bin/ 
