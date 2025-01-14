FROM ubuntu:22.04

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y \
    git \
    python3 \
    python3-pip \
    libgl1 \
    libglib2.0-0 \
    pkg-config \
    net-tools \
    libavcodec-dev \
    libavformat-dev \
    libavutil-dev \
    libswscale-dev \
    libavfilter-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /root

RUN git clone https://github.com/predictable-edge/edge-use-case.git

RUN pip3 install ultralytics

WORKDIR /root/edge-use-case

CMD ["/bin/bash"]