# Rondo server: Node 22 + yt-dlp (standalone, on a volume) + deno + ffmpeg
FROM node:22-bookworm-slim AS build
WORKDIR /app
COPY server/package*.json ./
RUN npm ci
COPY server/ ./
RUN npm run build && npm prune --omit=dev

FROM node:22-bookworm-slim
RUN apt-get update \
 && apt-get install -y --no-install-recommends ffmpeg ca-certificates curl unzip wget \
 && rm -rf /var/lib/apt/lists/*
# deno: JavaScript runtime yt-dlp needs for YouTube challenge solving
RUN curl -fsSL https://deno.land/install.sh | DENO_INSTALL=/usr/local sh
WORKDIR /app
COPY --from=build /app/node_modules ./node_modules
COPY --from=build /app/dist ./dist
COPY --from=build /app/package.json ./
# the admin page (docs/dev/server-api.md) and its font, the same Roboto the device and its dashboard use
COPY --from=build /app/admin ./admin
COPY app/app/src/main/assets/fonts/Roboto-Regular.ttf ./admin/fonts/Roboto-Regular.ttf
COPY infra/entrypoint.sh /entrypoint.sh
RUN chmod +x /entrypoint.sh
ENV YTDLP_BIN=/data/bin/yt-dlp PORT=3000
EXPOSE 3000
ENTRYPOINT ["/entrypoint.sh"]
