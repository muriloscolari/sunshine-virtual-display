# Sunshine Virtual Display para KDE Plasma 6 (Wayland) — C Edition

Solução automatizada e de alta performance escrita em **C** para streaming com **Sunshine + Moonlight** quando o monitor físico do computador estiver desligado (headless).

## Destaques da Versão em C

- **Consumo de Memória Mínimo:** Consome **menos de 2 MB de RAM** em segundo plano (contra ~20MB do Python).
- **Zero Ciclos de CPU Ociosos:** Integração com eventos Netlink do kernel Linux e `poll()`, reagindo instantaneamente a eventos de hotplug do monitor.
- **Detecção Dinâmica do Wayland:** Detecta automaticamente o socket Wayland da sessão (`wayland-0`, `wayland-1`, etc.).
- **Sem Vazamentos de Memória:** Código estritamente verificado com **Valgrind** e **AddressSanitizer** (0 leaks, 0 errors).

---

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

Basta clonar o repositório e executar o script de instalação:

```bash
git clone https://github.com/muriloscolari/sunshine-virtual-display.git
cd sunshine-virtual-display
./install.sh
```

O script `install.sh` fará tudo sozinho:
- Instala as dependências necessárias (`krfb`, `libkscreen`, `cjson`, `gcc`).
- Compila o binário em C com otimizações (`-O2`).
- Instala o binário em `~/.local/bin/sunshine-display-daemon`.
- Configura o Sunshine para captura nativa do KWin (`capture = kwin`).
- Remove o atraso padrão de inicialização de 5s do Sunshine.
- Registra e inicia o serviço systemd de usuário (`sunshine-virtual-display.service`).

---

## Comandos Úteis

- **Ver status atual das telas:**
  ```bash
  sunshine-display-daemon --status
  ```
- **Ver status do serviço systemd:**
  ```bash
  systemctl --user status sunshine-virtual-display.service
  ```
- **Acompanhar logs de alternância de tela em tempo real:**
  ```bash
  tail -f ~/.config/sunshine/vmon.log
  ```
- **Compilar manualmente via Makefile:**
  ```bash
  make
  make test   # Executa bateria de testes com AddressSanitizer
  ```
