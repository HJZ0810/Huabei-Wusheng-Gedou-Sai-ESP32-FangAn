#!/bin/sh
# 单车中继部署：仅管理 CombatBot 自身文件；不修改同机已有网站配置。
# 先通过 server-manager 上传 release.tar.gz/config.private.json 到受限暂存目录。
# 新域名须提前完成 HTTP-01 证书签发；升级时保留全部旧发布和私有配置备份。
set -eu
staging=${1:?staging directory required}
release_id=${2:?release id required}
case "$staging" in /tmp/combatbot-v4-*) ;; *) exit 1;; esac
case "$release_id" in *[!A-Za-z0-9._-]*|'') exit 1;; esac
release=/opt/combatbot/releases/$release_id
backup=/var/backups/combatbot/$release_id
for file in "$staging/release.tar.gz" "$staging/config.private.json" /etc/letsencrypt/live/combatbot.luo-jin-ai.com/fullchain.pem; do test -f "$file"; done
test ! -e "$release"
test ! -e "$backup"
id combatbot >/dev/null 2>&1 || useradd --system --home-dir /nonexistent --shell /usr/sbin/nologin combatbot
install -d -m 0755 "$release"
tar -xzf "$staging/release.tar.gz" -C "$release"
cd "$release/server"
npm ci --ignore-scripts --omit=dev
node --test test/relay.test.mjs
chown -R root:root "$release"
chmod -R go-w "$release"
install -d -m 0700 "$backup"
for file in /etc/combatbot/config.private.json /etc/combatbot/relay.env /etc/systemd/system/combatbot.service /etc/nginx/conf.d/combatbot-domain.conf; do
    if test -f "$file"; then cp -a "$file" "$backup/$(basename "$file")"; fi
done
if test -L /opt/combatbot/current; then readlink /opt/combatbot/current > "$backup/previous-release"; fi
rollback() {
    rollback_code=$?
    trap - EXIT
    if test "$rollback_code" -eq 0; then return; fi
    systemctl stop combatbot.service || true
    for file in /etc/combatbot/config.private.json /etc/combatbot/relay.env /etc/systemd/system/combatbot.service /etc/nginx/conf.d/combatbot-domain.conf; do
        original="$backup/$(basename "$file")"
        if test -f "$original"; then cp -a "$original" "$file"; else rm -f "$file"; fi
    done
    if test -f "$backup/previous-release"; then
        ln -s "$(cat "$backup/previous-release")" /opt/combatbot/.rollback-$release_id
        mv -Tf /opt/combatbot/.rollback-$release_id /opt/combatbot/current
    fi
    systemctl daemon-reload
    if test -f "$backup/combatbot.service"; then systemctl restart combatbot.service || true; fi
    if nginx -t; then systemctl reload nginx; fi
    echo "Deployment failed; previous configuration restored. Backup: $backup" >&2
    exit "$rollback_code"
}
trap rollback EXIT
install -d -o root -g combatbot -m 0750 /etc/combatbot
install -o root -g combatbot -m 0640 "$staging/config.private.json" /etc/combatbot/config.private.json
cat > /etc/combatbot/relay.env <<'ENV'
COMBATBOT_CONFIG=/etc/combatbot/config.private.json
COMBATBOT_PUBLIC_ORIGIN=https://combatbot.luo-jin-ai.com
COMBATBOT_BASE_PATH=
COMBATBOT_HOST=127.0.0.1
COMBATBOT_PORT=8090
ENV
chown root:combatbot /etc/combatbot/relay.env
chmod 0640 /etc/combatbot/relay.env
ln -s "$release" /opt/combatbot/.next-$release_id
mv -Tf /opt/combatbot/.next-$release_id /opt/combatbot/current
install -m 0644 "$release/server/deploy/combatbot.service" /etc/systemd/system/combatbot.service
systemctl daemon-reload
systemctl enable combatbot.service
systemctl restart combatbot.service
curl --retry 5 --retry-connrefused --retry-delay 1 -fsS http://127.0.0.1:8090/api/info
install -m 0644 "$release/server/deploy/combatbot-domain.conf" /etc/nginx/conf.d/combatbot-domain.conf
nginx -t
systemctl reload nginx
install -d -m 0755 /etc/letsencrypt/renewal-hooks/deploy
printf '#!/bin/sh\nset -eu\nnginx -t\nsystemctl reload nginx\n' > /etc/letsencrypt/renewal-hooks/deploy/combatbot-nginx.sh
chmod 0755 /etc/letsencrypt/renewal-hooks/deploy/combatbot-nginx.sh
systemctl is-active combatbot.service
trap - EXIT
printf '\nService installed; verify real HTTPS/WSS with scripts/public-smoke.mjs. Backup: %s\n' "$backup"
