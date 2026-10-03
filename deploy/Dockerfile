# Wolfpack phone map — web/ served as-is by Caddy on :3000.
# No build step: the page is the artifact.  Built from the repo root:
#   docker build -f deploy/Dockerfile .
FROM caddy:2.11-alpine
COPY deploy/Caddyfile /etc/caddy/Caddyfile
COPY web/ /srv/
# Node test harnesses and the README don't belong on the public origin
RUN rm -f /srv/test-*.js /srv/README.md
EXPOSE 3000
