# Thin overlay. Factory image is fixed; this layer adds just for in-container recipes.

FROM ghcr.io/mateuszzx/zephyr-workspace:latest

USER root
RUN apt-get update && apt-get install -y --no-install-recommends just \
    && rm -rf /var/lib/apt/lists/*
ENV JUST_INSIDE_CONTAINER=true
USER dev

