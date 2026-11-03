# Rondo's updater (server/src/updater/): moves the Rondo server's container onto its newest image
# when the admin page asks. Node and the compiled updater only, no packages; it talks to Docker over
# the socket the compose files give it. Built with the server image by .github/workflows/release.yml.
FROM node:22-bookworm-slim AS build
WORKDIR /app
COPY server/package*.json ./
RUN npm ci
COPY server/ ./
RUN npm run build

FROM node:22-alpine
WORKDIR /updater
COPY --from=build /app/dist/updater ./
RUN echo '{"type":"module"}' > package.json
ENV PORT=8080 TARGET=rondo TOKEN_FILE=/run/updater/token
EXPOSE 8080
CMD ["node", "main.js"]
