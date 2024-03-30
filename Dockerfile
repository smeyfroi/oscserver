FROM ubuntu:20.04

ENV LANG C.UTF-8

RUN apt-get update \
    && apt-get -y upgrade \
    && DEBIAN_FRONTEND=noninteractive apt-get install -y \
         build-essential git cmake

RUN apt-get update \
    && apt-get -y upgrade \
    && DEBIAN_FRONTEND=noninteractive apt-get install -y \
         neovim

WORKDIR /oscserver
