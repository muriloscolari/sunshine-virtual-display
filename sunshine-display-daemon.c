/*
 * Sunshine Virtual Display Daemon para KDE Plasma 6 (Wayland)
 * Escrito em C para consumo mínimo de recursos (RAM < 2MB, 0% CPU).
 *
 * Monitora continuamente o estado dos monitores:
 * - Se o monitor físico for desligado: cria tela virtual 1920x1080 para Sunshine/Moonlight.
 * - Se o monitor físico for ligado: ativa o monitor físico como principal no KWin ANTES
 *   de encerrar a tela virtual, garantindo estabilidade e impedindo que a sessão feche.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdarg.h>
#include <unistd.h>
#include <time.h>
#include <dirent.h>
#include <signal.h>
#include <errno.h>
#include <poll.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <linux/netlink.h>

#define VMON_NAME "sunshine-vmon"
#define VMON_OUTPUT "Virtual-sunshine-vmon"
#define RES_W "1920"
#define RES_H "1080"
#define FPS "60"

static volatile sig_atomic_t g_keep_running = 1;
static pid_t g_vmon_pid = -1;
static char g_last_physical_output[64] = "DP-1";
static char g_runtime_dir[256] = "";
static char g_log_path[512] = "";

static void log_msg(const char *fmt, ...) {
    time_t now = time(NULL);
    struct tm tm_buf;
    localtime_r(&now, &tm_buf);
    char time_str[64];
    strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", &tm_buf);

    FILE *fp = NULL;
    if (g_log_path[0] != '\0') {
        fp = fopen(g_log_path, "a");
    }

    va_list args;
    va_start(args, fmt);
    if (fp) {
        fprintf(fp, "[%s] ", time_str);
        vfprintf(fp, fmt, args);
        fprintf(fp, "\n");
        fflush(fp);
        fclose(fp);
    } else {
        printf("[%s] ", time_str);
        vprintf(fmt, args);
        printf("\n");
        fflush(stdout);
    }
    va_end(args);
}

static void sigterm_handler(int sig) {
    (void)sig;
    g_keep_running = 0;
}

/* Verifica e recolhe processo filho da tela virtual de forma síncrona (sem race condition de SIGCHLD) */
static void reap_vmon_if_exited(void) {
    if (g_vmon_pid > 1) {
        int status;
        pid_t w = waitpid(g_vmon_pid, &status, WNOHANG);
        if (w == g_vmon_pid || (w == -1 && errno == ECHILD)) {
            g_vmon_pid = -1;
        }
    }
}

/* Detecta dinamicamente o socket wayland ativo (ex: wayland-0, wayland-1) */
static void detect_wayland_display(char *out_display, size_t max_len) {
    const char *env_w = getenv("WAYLAND_DISPLAY");
    if (env_w && env_w[0] != '\0') {
        char test_path[512];
        snprintf(test_path, sizeof(test_path), "%s/%s", g_runtime_dir, env_w);
        if (access(test_path, F_OK) == 0) {
            snprintf(out_display, max_len, "%s", env_w);
            return;
        }
    }

    DIR *dir = opendir(g_runtime_dir);
    if (!dir) {
        snprintf(out_display, max_len, "wayland-0");
        return;
    }

    char best[256] = "";
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strncmp(entry->d_name, "wayland-", 8) == 0) {
            size_t nlen = strlen(entry->d_name);
            if (nlen > 5 && strcmp(entry->d_name + nlen - 5, ".lock") == 0) {
                continue;
            }
            if (strcmp(entry->d_name, best) > 0) {
                snprintf(best, sizeof(best), "%s", entry->d_name);
            }
        }
    }
    closedir(dir);

    if (best[0] != '\0') {
        snprintf(out_display, max_len, "%s", best);
    } else {
        snprintf(out_display, max_len, "wayland-0");
    }
}

static void refresh_environment(void) {
    char w_display[64];
    detect_wayland_display(w_display, sizeof(w_display));
    setenv("WAYLAND_DISPLAY", w_display, 1);
    setenv("QT_QPA_PLATFORM", "wayland", 1);

    char bus_path[512];
    snprintf(bus_path, sizeof(bus_path), "unix:path=%s/bus", g_runtime_dir);
    setenv("DBUS_SESSION_BUS_ADDRESS", bus_path, 1);
}

/* Aguarda até o compositor Wayland estar pronto e com socket disponível */
static bool wait_for_wayland(int timeout_seconds) {
    for (int i = 0; i < timeout_seconds * 10; i++) {
        char w_display[64];
        detect_wayland_display(w_display, sizeof(w_display));
        char test_path[512];
        snprintf(test_path, sizeof(test_path), "%s/%s", g_runtime_dir, w_display);
        if (access(test_path, F_OK) == 0) {
            refresh_environment();
            return true;
        }
        usleep(100000); /* 100ms */
    }
    return false;
}

/* Executa um comando silenciosamente */
static void run_cmd_quiet(const char *cmd) {
    int ret = system(cmd);
    (void)ret;
}

/*
 * Detecta monitor físico diretamente via kernel sysfs (/sys/class/drm/card*-* /status).
 * Extremamente rápido (<10µs), sem dependência de Wayland/Qt/D-Bus, à prova de falhas.
 */
static bool has_drm_physical_monitor(char *primary_output, size_t max_len) {
    DIR *dir = opendir("/sys/class/drm");
    if (!dir) return false;

    struct dirent *entry;
    bool found = false;

    while ((entry = readdir(dir)) != NULL) {
        if (strncmp(entry->d_name, "card", 4) != 0) continue;
        if (strchr(entry->d_name, '-') == NULL) continue;
        if (strstr(entry->d_name, "Writeback") != NULL) continue;

        char status_path[512];
        snprintf(status_path, sizeof(status_path), "/sys/class/drm/%s/status", entry->d_name);

        FILE *f = fopen(status_path, "r");
        if (!f) continue;

        char status[32] = "";
        if (fgets(status, sizeof(status), f)) {
            size_t slen = strlen(status);
            while (slen > 0 && (status[slen - 1] == '\n' || status[slen - 1] == '\r')) {
                status[--slen] = '\0';
            }
            if (strcmp(status, "connected") == 0) {
                found = true;
                if (primary_output && max_len > 0) {
                    const char *dash = strchr(entry->d_name, '-');
                    if (dash) {
                        snprintf(primary_output, max_len, "%.63s", dash + 1);
                    } else {
                        snprintf(primary_output, max_len, "%.63s", entry->d_name);
                    }
                }
                fclose(f);
                break;
            }
        }
        fclose(f);
    }
    closedir(dir);
    return found;
}

/* Wrapper para checagem de monitor físico */
static bool has_physical_monitor(void) {
    char detected[64] = "";
    if (has_drm_physical_monitor(detected, sizeof(detected))) {
        if (detected[0] != '\0') {
            snprintf(g_last_physical_output, sizeof(g_last_physical_output), "%s", detected);
        }
        return true;
    }
    return false;
}

static void start_vmon(void) {
    reap_vmon_if_exited();
    if (g_vmon_pid > 1 && kill(g_vmon_pid, 0) == 0) {
        return;
    }

    log_msg("Monitor físico DESLIGADO detectado! Criando tela virtual %sx%s...", RES_W, RES_H);
    run_cmd_quiet("pkill -f 'krfb-virtualmonitor.*" VMON_NAME "' >/dev/null 2>&1");

    if (!wait_for_wayland(5)) {
        log_msg("Aviso: Servidor Wayland não respondeu a tempo para criar tela virtual.");
        return;
    }

    pid_t pid = fork();
    if (pid == 0) {
        /* Processo filho */
        int devnull = open("/dev/null", O_RDWR);
        if (devnull >= 0) {
            dup2(devnull, STDIN_FILENO);
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
            close(devnull);
        }
        char *args[] = {
            "krfb-virtualmonitor",
            "--desktopfile", "org.kde.krfb.virtualmonitor",
            "--resolution", RES_W "x" RES_H,
            "--scale", "1",
            "--name", VMON_NAME,
            "--password", "sunshine_dummy_pwd",
            "--port", "5912",
            NULL
        };
        execvp("krfb-virtualmonitor", args);
        _exit(1);
    } else if (pid > 1) {
        g_vmon_pid = pid;
        usleep(800000); /* 0.8s para o KWin registrar a nova tela virtual */

        refresh_environment();
        char doctor_cmd[512];
        snprintf(doctor_cmd, sizeof(doctor_cmd),
                 "kscreen-doctor output.%s.enable output.%s.priority.1 output.%s.mode.%sx%s@%s >/dev/null 2>&1",
                 VMON_OUTPUT, VMON_OUTPUT, VMON_OUTPUT, RES_W, RES_H, FPS);
        run_cmd_quiet(doctor_cmd);

        log_msg("Tela virtual %s ativa como tela principal (PID %d).", VMON_OUTPUT, (int)g_vmon_pid);

        /* Reinicia o Sunshine para vincular à nova tela virtual */
        run_cmd_quiet("systemctl --user restart sunshine >/dev/null 2>&1");
        log_msg("Sunshine pronto para conexões na tela virtual.");
    }
}

static void terminate_vmon_process(void) {
    pid_t target_pid = g_vmon_pid;
    g_vmon_pid = -1;

    /*
     * CRÍTICO: Jamais chamar kill() se target_pid <= 1.
     * No POSIX, kill(-1, SIGKILL) mata TODOS os processos do usuário (derrubando a sessão)!
     */
    if (target_pid > 1) {
        kill(target_pid, SIGTERM);
        int status;
        bool exited = false;
        for (int i = 0; i < 15; i++) {
            pid_t w = waitpid(target_pid, &status, WNOHANG);
            if (w == target_pid || (w == -1 && errno == ECHILD)) {
                exited = true;
                break;
            }
            usleep(100000);
        }
        if (!exited) {
            kill(target_pid, SIGKILL);
            waitpid(target_pid, &status, 0);
        }
    }
    run_cmd_quiet("pkill -f 'krfb-virtualmonitor.*" VMON_NAME "' >/dev/null 2>&1");
}

static void stop_vmon(void) {
    log_msg("Monitor físico (%s) LIGADO detectado! Aguardando estabilização do sinal DisplayPort...", g_last_physical_output);

    /*
     * Aguarda 2.5 segundos para que a placa AMD e o cabo DisplayPort completem o
     * DPCD link training e a negociação DDC/EDID sem colisões de KMS no driver.
     */
    usleep(2500000);

    refresh_environment();

    /*
     * PASSO 1: Ativa o monitor físico e define-o como prioridade 1 ENQUANTO a tela virtual
     * AINDA ESTÁ VIVA. Dessa forma o KWin nunca fica com 0 telas ativas.
     */
    char doctor_cmd[512];
    snprintf(doctor_cmd, sizeof(doctor_cmd),
             "kscreen-doctor output.%.60s.enable output.%.60s.priority.1 >/dev/null 2>&1",
             g_last_physical_output, g_last_physical_output);

    bool activated = false;
    for (int attempt = 1; attempt <= 4; attempt++) {
        int ret = system(doctor_cmd);
        if (WIFEXITED(ret) && WEXITSTATUS(ret) == 0) {
            activated = true;
            log_msg("Monitor físico %s ativado com sucesso como tela principal (tentativa %d).", g_last_physical_output, attempt);
            break;
        }
        log_msg("Aguardando KWin aceitar ativação do monitor físico %s (tentativa %d)...", g_last_physical_output, attempt);
        sleep(1);
    }

    if (!activated) {
        log_msg("Aviso: Falha ao ativar monitor físico %s após tentativas. Mantendo tela virtual ativa por segurança.", g_last_physical_output);
        return;
    }

    /* Aguarda 1.0s para o KWin renderizar as janelas e barras no monitor físico */
    usleep(1000000);

    /*
     * PASSO 2: Encerra o processo da tela virtual com segurança total (PID > 1).
     */
    terminate_vmon_process();
    usleep(300000);

    log_msg("Tela virtual encerrada. Imagem transferida para o monitor físico %s com sucesso.", g_last_physical_output);
}

static void cleanup(void) {
    terminate_vmon_process();
    log_msg("Daemon encerrado.");
}

int main(int argc, char *argv[]) {
    uid_t uid = getuid();
    const char *rt = getenv("XDG_RUNTIME_DIR");
    if (rt && rt[0] != '\0') {
        snprintf(g_runtime_dir, sizeof(g_runtime_dir), "%s", rt);
    } else {
        snprintf(g_runtime_dir, sizeof(g_runtime_dir), "/run/user/%u", (unsigned)uid);
        setenv("XDG_RUNTIME_DIR", g_runtime_dir, 1);
    }

    const char *home = getenv("HOME");
    if (home) {
        snprintf(g_log_path, sizeof(g_log_path), "%s/.config/sunshine/vmon.log", home);
    }

    /* Subcomandos rápidos */
    if (argc > 1) {
        if (strcmp(argv[1], "--status") == 0) {
            bool phys = has_physical_monitor();
            printf("Monitor físico ativo: %s\n", phys ? "SIM" : "NÃO");
            printf("Última saída física detectada: %s\n", g_last_physical_output);
            return 0;
        }
        if (strcmp(argv[1], "--version") == 0) {
            printf("sunshine-display-daemon (C edition) v2.2\n");
            return 0;
        }
        if (strcmp(argv[1], "--help") == 0) {
            printf("Uso: %s [--status | --version | --help | --test-iterations N]\n", argv[0]);
            return 0;
        }
    }

    int max_test_iterations = 0;
    if (argc > 2 && strcmp(argv[1], "--test-iterations") == 0) {
        max_test_iterations = atoi(argv[2]);
    }

    /* Configura sinais (sem SIGCHLD assíncrono para evitar race condition no g_vmon_pid) */
    struct sigaction sa_term;
    memset(&sa_term, 0, sizeof(sa_term));
    sa_term.sa_handler = sigterm_handler;
    sigaction(SIGTERM, &sa_term, NULL);
    sigaction(SIGINT, &sa_term, NULL);

    log_msg("Serviço sunshine-virtual-display (C edition) iniciado e ativo.");

    /* Aguarda o Wayland estar pronto antes de prosseguir */
    wait_for_wayland(5);

    bool was_physical_active = has_physical_monitor();
    if (!was_physical_active) {
        log_msg("Iniciando com monitor físico desligado. Criando tela virtual imediatamente...");
        start_vmon();
    } else {
        log_msg("Iniciando com monitor físico ativo (%s).", g_last_physical_output);
    }

    /* Socket Netlink para eventos de hotplug do kernel uevent */
    int nl_fd = socket(AF_NETLINK, SOCK_RAW, NETLINK_KOBJECT_UEVENT);
    if (nl_fd >= 0) {
        struct sockaddr_nl sa_nl;
        memset(&sa_nl, 0, sizeof(sa_nl));
        sa_nl.nl_family = AF_NETLINK;
        sa_nl.nl_groups = 1; /* Notificações do kernel */
        if (bind(nl_fd, (struct sockaddr *)&sa_nl, sizeof(sa_nl)) < 0) {
            close(nl_fd);
            nl_fd = -1;
        }
    }

    struct pollfd pfd;
    pfd.fd = nl_fd;
    pfd.events = POLLIN;

    int iteration = 0;
    char nl_buf[2048];

    while (g_keep_running) {
        if (max_test_iterations > 0 && ++iteration > max_test_iterations) {
            break;
        }

        reap_vmon_if_exited();

        /* Espera evento do kernel ou timeout de 1 segundo */
        int ret = 0;
        if (nl_fd >= 0) {
            ret = poll(&pfd, 1, 1000);
            if (ret > 0 && (pfd.revents & POLLIN)) {
                /* Consome buffer do netlink */
                ssize_t n = recv(nl_fd, nl_buf, sizeof(nl_buf) - 1, MSG_DONTWAIT);
                if (n > 0) {
                    nl_buf[n] = '\0';
                    /* Se for uevent irrelevante (não drm/card), ignora */
                    if (strstr(nl_buf, "drm") == NULL && strstr(nl_buf, "card") == NULL) {
                        continue;
                    }
                }
            }
        } else {
            sleep(1);
        }

        if (!g_keep_running) {
            break;
        }

        bool physical_active = has_physical_monitor();
        if (physical_active != was_physical_active) {
            if (!physical_active) {
                start_vmon();
            } else {
                stop_vmon();
            }
            was_physical_active = physical_active;
        }
    }

    if (nl_fd >= 0) {
        close(nl_fd);
    }
    cleanup();
    return 0;
}
