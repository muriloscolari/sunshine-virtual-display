#!/usr/bin/env bash
#
# Desinstalador: Sunshine Virtual Display para KDE Plasma 6 Wayland
#

set -euo pipefail

echo "=========================================================="
echo "   Desinstalador: Sunshine Virtual Display               "
echo "=========================================================="

echo "Parando e desabilitando serviço sunshine-virtual-display..."
systemctl --user stop sunshine-virtual-display.service 2>/dev/null || true
systemctl --user disable sunshine-virtual-display.service 2>/dev/null || true

echo "Removendo arquivos instalados..."
rm -f "${HOME}/.config/systemd/user/sunshine-virtual-display.service"
rm -f "${HOME}/.local/bin/sunshine-display-daemon.py"
rm -f "${HOME}/.config/systemd/user/app-dev.lizardbyte.app.Sunshine.service.d/override.conf"

systemctl --user daemon-reload

pkill -f "krfb-virtualmonitor.*sunshine-vmon" 2>/dev/null || true

echo "Desinstalação concluída com sucesso!"
