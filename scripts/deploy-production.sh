#!/bin/bash
# Runs ON xdroplet04, fed over ssh by .woodpecker/deploy-production.yml with
# IMAGE_TAG set to the tagged commit's short SHA.
set -euo pipefail

: "${IMAGE_TAG:?IMAGE_TAG must be set (short SHA of the tagged commit)}"
export IMAGE_TAG

cd /opt/projects/wolfpack
mkdir -p caddy

mv _staged/compose.production.yml ./compose.production.yml
mv _staged/wolfpack.caddy ./caddy/wolfpack.caddy

docker compose -f compose.production.yml pull
docker compose -f compose.production.yml up -d --remove-orphans

cp caddy/wolfpack.caddy /opt/caddy-stack/sites/
docker exec caddy caddy reload --config /etc/caddy/Caddyfile

docker image prune -f
echo "Production deployment complete: meshtastic:${IMAGE_TAG}"
