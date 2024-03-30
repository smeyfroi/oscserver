#!/bin/sh

docker run -ti --rm \
  -v `pwd`:/oscserver \
  -p 8000:8000/tcp \
  oscserver \
  sh bin/_run.sh
