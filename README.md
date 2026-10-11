# Sunshine Virtual Display para KDE Plasma 6 (Wayland)

Solução automatizada para streaming com **Sunshine + Moonlight** quando o monitor físico do computador estiver desligado (headless).

## Como Funciona

1. **Monitor Físico Desligado:**
   - O daemon detecta que não há monitor físico ativo no KWin.
   - Cria imediatamente uma tela virtual **1920x1080 a 60Hz** usando o `krfb-virtualmonitor`.
   - O Sunshine conecta à tela virtual sem exibir erros de falta de tela (`no wl_output found`).
   - Você conecta pelo Moonlight (celular, tablet, notebook) com imagem perfeita 1080p.

2. **Monitor Físico Ligado:**
   - O daemon detecta que o monitor físico foi ligado no botão/tomada.
   - Reativa o monitor físico com prioridade máxima.
   - Desliga a tela virtual.
   - A imagem é transferida instantaneamente para o monitor físico.

---

## Como Instalar em uma Máquina Nova

Basta clonar ou copiar esta pasta para o computador novo e rodar:

```bash
cd ~/Documentos/sunshine-virtual-display
chmod +x install.sh
./install.sh
```

O script `install.sh` fará tudo sozinho:
- Instala as dependências necessárias (`krfb`, `libkscreen`).
- Instala o script `sunshine-display-daemon.py` em `~/.local/bin/`.
- Configura o Sunshine para usar captura nativa do KWin (`capture = kwin`).
- Remove o atraso de inicialização padrão do Sunshine (`sleep 5`).
- Cria e ativa o serviço do systemd do usuário (`sunshine-virtual-display.service`).
- Reinicia o Sunshine pronto para uso.

---

## Arquivos do Projeto

- `install.sh`: Script que instala e configura tudo automaticamente.
- `uninstall.sh`: Script para remover a configuração e o serviço.
- `sunshine-display-daemon.py`: O daemon Python que monitora o estado dos monitores.
- `sunshine-virtual-display.service`: Unidade systemd do usuário para iniciar o daemon automaticamente com a sessão gráfica.

---

## Comandos Úteis

- **Ver status do serviço:**
  ```bash
  systemctl --user status sunshine-virtual-display.service
  ```
- **Acompanhar logs de alternância de tela em tempo real:**
  ```bash
  tail -f ~/.config/sunshine/vmon.log
  ```
- **Ver status do Sunshine:**
  ```bash
  systemctl --user status sunshine
  ```
