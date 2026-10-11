#!/usr/bin/env python3
"""
Sunshine Virtual Display Daemon para KDE Plasma 6 (Wayland)
Monitora continuamente se há algum monitor físico ativo.
- Se o monitor físico estiver desligado: cria instantaneamente a tela virtual 1920x1080 para o Sunshine.
- Se o monitor físico ligar de novo: reativa o monitor físico e desliga a tela virtual.
"""

import os
import sys
import time
import json
import signal
import subprocess
import logging

LOG_PATH = os.path.expanduser("~/.config/sunshine/vmon.log")
os.makedirs(os.path.dirname(LOG_PATH), exist_ok=True)

logging.basicConfig(
    filename=LOG_PATH,
    level=logging.INFO,
    format="[%(asctime)s] %(message)s",
    datefmt="%Y-%m-%d %H:%M:%S"
)

uid = os.getuid()
runtime_dir = os.environ.get("XDG_RUNTIME_DIR", f"/run/user/{uid}")
os.environ["XDG_RUNTIME_DIR"] = runtime_dir
os.environ.setdefault("WAYLAND_DISPLAY", "wayland-0")
os.environ.setdefault("DBUS_SESSION_BUS_ADDRESS", f"unix:path={runtime_dir}/bus")
os.environ.setdefault("QT_QPA_PLATFORM", "wayland")

VMON_NAME = "sunshine-vmon"
VMON_OUTPUT = f"Virtual-{VMON_NAME}"
RES_W = 1920
RES_H = 1080
FPS = 60

vmon_process = None
last_physical_output = "DP-1"

def get_kwin_state():
    """Retorna lista de nomes de displays físicos conectados e virtuais no KWin."""
    physical = []
    virtual = []
    try:
        out = subprocess.check_output(["kscreen-doctor", "-j"], stderr=subprocess.DEVNULL)
        data = json.loads(out)
        for o in data.get("outputs", []):
            name = o.get("name", "")
            connected = o.get("connected", False)
            if "Virtual" in name:
                if connected:
                    virtual.append(name)
            else:
                if connected:
                    physical.append(name)
    except Exception as e:
        logging.error(f"Erro ao ler kscreen-doctor: {e}")
    return physical, virtual

def has_physical_monitor():
    physical, _ = get_kwin_state()
    return len(physical) > 0

def get_physical_output_name():
    global last_physical_output
    physical, _ = get_kwin_state()
    if physical:
        last_physical_output = physical[0]
        return physical[0]
    return last_physical_output

def start_vmon():
    global vmon_process
    if vmon_process and vmon_process.poll() is None:
        return

    logging.info(f"Monitor físico DESLIGADO detectado! Criando tela virtual {RES_W}x{RES_H}...")
    subprocess.run(["pkill", "-f", f"krfb-virtualmonitor.*{VMON_NAME}"], stderr=subprocess.DEVNULL)

    passw = "sunshine_dummy_pwd"
    cmd = [
        "krfb-virtualmonitor",
        "--desktopfile", "org.kde.krfb.virtualmonitor",
        "--resolution", f"{RES_W}x{RES_H}",
        "--scale", "1",
        "--name", VMON_NAME,
        "--password", passw,
        "--port", "5912"
    ]
    try:
        # Inicia a tela virtual instantaneamente sem timeout de portal
        vmon_process = subprocess.Popen(
            cmd,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL
        )
        time.sleep(0.5)

        # Desabilita saídas físicas para garantir que o virtual é a única tela ativa
        phys_list, _ = get_kwin_state()
        disable_args = [f"output.{p}.disable" for p in phys_list]

        cmd_doctor = [
            "kscreen-doctor",
            f"output.{VMON_OUTPUT}.enable",
            f"output.{VMON_OUTPUT}.priority.1",
            f"output.{VMON_OUTPUT}.mode.{RES_W}x{RES_H}@{FPS}"
        ] + disable_args

        subprocess.run(cmd_doctor, stderr=subprocess.DEVNULL)
        logging.info(f"Tela virtual {VMON_OUTPUT} ativa como tela principal (PID {vmon_process.pid}).")

        # Reinicia o Sunshine para anexar os encoders à tela virtual
        subprocess.run(["systemctl", "--user", "restart", "sunshine"], stderr=subprocess.DEVNULL)
        logging.info("Sunshine reiniciado e pronto para conexões na tela virtual.")
    except Exception as e:
        logging.error(f"Erro ao iniciar krfb-virtualmonitor: {e}")

def stop_vmon():
    global vmon_process
    phys = get_physical_output_name()
    logging.info(f"Monitor físico ({phys}) LIGADO detectado! Reativando tela física e desligando virtual...")

    # Reativa a tela física como prioritária
    subprocess.run([
        "kscreen-doctor",
        f"output.{phys}.enable",
        f"output.{phys}.priority.1"
    ], stderr=subprocess.DEVNULL)
    subprocess.run(["kscreen-doctor", "--dpms", "on"], stderr=subprocess.DEVNULL)
    time.sleep(0.3)

    if vmon_process and vmon_process.poll() is None:
        vmon_process.terminate()
        try:
            vmon_process.wait(timeout=1)
        except subprocess.TimeoutExpired:
            vmon_process.kill()
    vmon_process = None
    subprocess.run(["pkill", "-f", f"krfb-virtualmonitor.*{VMON_NAME}"], stderr=subprocess.DEVNULL)

    time.sleep(0.3)
    subprocess.run(["kscreen-doctor", f"output.{phys}.priority.1"], stderr=subprocess.DEVNULL)
    logging.info(f"Tela virtual encerrada. Imagem transferida para o monitor físico {phys} com sucesso.")

    # Reinicia o Sunshine para voltar a transmitir o monitor físico
    subprocess.run(["systemctl", "--user", "restart", "sunshine"], stderr=subprocess.DEVNULL)

def cleanup(*args):
    global vmon_process
    if vmon_process and vmon_process.poll() is None:
        vmon_process.terminate()
    subprocess.run(["pkill", "-f", f"krfb-virtualmonitor.*{VMON_NAME}"], stderr=subprocess.DEVNULL)
    sys.exit(0)

signal.signal(signal.SIGTERM, cleanup)
signal.signal(signal.SIGINT, cleanup)

def main():
    logging.info("Serviço sunshine-virtual-display iniciado e ativo.")
    was_physical_active = has_physical_monitor()

    if not was_physical_active:
        logging.info("Iniciando com monitor físico desligado. Criando tela virtual imediatamente...")
        start_vmon()

    while True:
        try:
            physical_active = has_physical_monitor()

            if physical_active != was_physical_active:
                if not physical_active:
                    # Monitor físico foi desligado!
                    start_vmon()
                else:
                    # Monitor físico foi religado!
                    time.sleep(1.0)
                    stop_vmon()
                was_physical_active = physical_active

            time.sleep(1)
        except Exception as e:
            logging.error(f"Erro no loop do daemon: {e}")
            time.sleep(2)

if __name__ == "__main__":
    main()
