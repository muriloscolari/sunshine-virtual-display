#!/usr/bin/env bash
#
# Instalador Automatizado: Sunshine Virtual Display para KDE Plasma 6 Wayland
# Configura tela virtual automática (1920x1080) quando o monitor físico estiver desligado.
#

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

echo "=========================================================="
echo "   Instalador: Sunshine Virtual Display (KDE Wayland)    "
echo "=========================================================="
echo ""

# 1. Instala dependências do sistema
echo "[1/6] Verificando dependências do sistema..."
if command -v pacman >/dev/null 2>&1; then
    MISSING_PKGS=()
    if ! command -v krfb-virtualmonitor >/dev/null 2>&1; then
        MISSING_PKGS+=("krfb")
    fi
    if ! command -v kscreen-doctor >/dev/null 2>&1; then
        MISSING_PKGS+=("libkscreen")
    fi

    if [ ${#MISSING_PKGS[@]} -gt 0 ]; then
        echo "Instalando pacotes necessários: ${MISSING_PKGS[*]}..."
        sudo pacman -S --needed --noconfirm "${MISSING_PKGS[@]}"
    else
        echo "Todas as dependências (krfb, libkscreen) já estão instaladas."
    fi
else
    echo "Aviso: Gerenciador pacman não detectado. Certifique-se de que 'krfb' e 'libkscreen' estejam instalados."
fi

# 2. Cria diretórios do usuário
echo "[2/6] Preparando diretórios do usuário..."
mkdir -p "${HOME}/.local/bin"
mkdir -p "${HOME}/.config/systemd/user"
mkdir -p "${HOME}/.config/sunshine"
mkdir -p "${HOME}/.config/systemd/user/app-dev.lizardbyte.app.Sunshine.service.d"

# 3. Copia o script daemon
echo "[3/6] Instalando sunshine-display-daemon.py em ~/.local/bin/..."
cp "${SCRIPT_DIR}/sunshine-display-daemon.py" "${HOME}/.local/bin/sunshine-display-daemon.py"
chmod +x "${HOME}/.local/bin/sunshine-display-daemon.py"

# 4. Configura o Sunshine para usar KWin capture
echo "[4/6] Configurando sunshine.conf (capture = kwin)..."
CONF_FILE="${HOME}/.config/sunshine/sunshine.conf"
if [ -f "$CONF_FILE" ]; then
    if ! grep -q "^capture\s*=" "$CONF_FILE"; then
        echo "capture = kwin" >> "$CONF_FILE"
    else
        sed -i 's/^capture\s*=.*/capture = kwin/' "$CONF_FILE"
    fi
else
    echo "capture = kwin" > "$CONF_FILE"
fi

# Remove o atraso de 5 segundos de fábrica do serviço Sunshine
cat << 'EOF' > "${HOME}/.config/systemd/user/app-dev.lizardbyte.app.Sunshine.service.d/override.conf"
[Service]
ExecStartPre=
EOF

# 5. Instala e ativa a unidade systemd
echo "[5/6] Instalando e ativando o serviço systemd de usuário..."
cp "${SCRIPT_DIR}/sunshine-virtual-display.service" "${HOME}/.config/systemd/user/sunshine-virtual-display.service"

systemctl --user daemon-reload
systemctl --user enable --now sunshine-virtual-display.service

# 6. Reinicia o Sunshine com as novas configurações
echo "[6/6] Reiniciando Sunshine..."
if systemctl --user is-enabled app-dev.lizardbyte.app.Sunshine.service >/dev/null 2>&1 || systemctl --user is-active app-dev.lizardbyte.app.Sunshine.service >/dev/null 2>&1; then
    systemctl --user restart sunshine || true
fi

echo ""
echo "=========================================================="
echo " Instalação concluída com sucesso!                        "
echo "=========================================================="
echo " - Tela virtual 1080p será criada ao desligar o monitor.  "
echo " - A imagem voltará para o monitor físico ao ligá-lo.    "
echo " - Acompanhe os logs em: ~/.config/sunshine/vmon.log      "
echo "=========================================================="
