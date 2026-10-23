/*
 * Sunshine Virtual Display Daemon para KDE Plasma 6 (Wayland)
 * Escrito em C para consumo mínimo de recursos (RAM < 2MB, 0% CPU).
 *
 * Monitora continuamente o estado dos monitores:
 * - Se o monitor físico for desligado: cria imediatamente tela virtual 1920x1080.
 * - Se o monitor físico for ligado: reativa o monitor físico e desliga a tela virtual.
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
#include <cjson/cJSON.h>

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

static void sigchld_handler(int sig) {
    (void)sig;
    int saved_errno = errno;
    pid_t pid;
    while ((pid = waitpid(-1, NULL, WNOHANG)) > 0) {
        if (pid == g_vmon_pid) {
            g_vmon_pid = -1;
        }
    }
    errno = saved_errno;
}

static void sigterm_handler(int sig) {
    (void)sig;
    g_keep_running = 0;
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

/* Executa um comando silenciosamente */
static void run_cmd_quiet(const char *cmd) {
    int ret = system(cmd);
    (void)ret;
}

/* Executa kscreen-doctor -j e analisa as saídas físicas e virtuais */
static bool get_kwin_state(char physical_list[][64], int *num_phys, int max_phys) {
    *num_phys = 0;
    refresh_environment();

    FILE *fp = popen("kscreen-doctor -j 2>/dev/null", "r");
    if (!fp) {
        log_msg("Erro ao executar kscreen-doctor");
        return false;
    }

    size_t cap = 8192;
    size_t len = 0;
    char *buf = malloc(cap);
    if (!buf) {
        pclose(fp);
        return false;
    }

    char chunk[1024];
    while (fgets(chunk, sizeof(chunk), fp)) {
        size_t n = strlen(chunk);
        if (len + n + 1 > cap) {
            cap *= 2;
            char *new_buf = realloc(buf, cap);
            if (!new_buf) {
                free(buf);
                pclose(fp);
                return false;
            }
            buf = new_buf;
        }
        memcpy(buf + len, chunk, n);
        len += n;
        buf[len] = '\0';
    }
    pclose(fp);

    if (len == 0) {
        free(buf);
        return false;
    }

    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root) {
        return false;
    }

    cJSON *outputs = cJSON_GetObjectItem(root, "outputs");
    if (cJSON_IsArray(outputs)) {
        int count = cJSON_GetArraySize(outputs);
        for (int i = 0; i < count; i++) {
            cJSON *item = cJSON_GetArrayItem(outputs, i);
            cJSON *name = cJSON_GetObjectItem(item, "name");
            cJSON *conn = cJSON_GetObjectItem(item, "connected");

            if (name && cJSON_IsString(name) && conn && cJSON_IsTrue(conn)) {
                if (strstr(name->valuestring, "Virtual") == NULL) {
                    if (*num_phys < max_phys) {
                        snprintf(physical_list[*num_phys], 64, "%s", name->valuestring);
                        (*num_phys)++;
                    }
                }
            }
        }
    }

    cJSON_Delete(root);
    return true;
}

static bool has_physical_monitor(void) {
    char phys[16][64];
    int count = 0;
    if (get_kwin_state(phys, &count, 16)) {
        if (count > 0) {
            snprintf(g_last_physical_output, sizeof(g_last_physical_output), "%s", phys[0]);
            return true;
        }
    }
    return false;
}

static void start_vmon(void) {
    if (g_vmon_pid > 0 && kill(g_vmon_pid, 0) == 0) {
        return;
    }

    log_msg("Monitor físico DESLIGADO detectado! Criando tela virtual %sx%s...", RES_W, RES_H);
    run_cmd_quiet("pkill -f 'krfb-virtualmonitor.*" VMON_NAME "' 2>/dev/null");

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
    } else if (pid > 0) {
        g_vmon_pid = pid;
        usleep(600000); /* 0.6s para o KWin registrar */

        char doctor_cmd[512];
        snprintf(doctor_cmd, sizeof(doctor_cmd),
                 "kscreen-doctor output.%s.enable output.%s.priority.1 output.%s.mode.%sx%s@%s 2>/dev/null",
                 VMON_OUTPUT, VMON_OUTPUT, VMON_OUTPUT, RES_W, RES_H, FPS);
        run_cmd_quiet(doctor_cmd);

        log_msg("Tela virtual %s ativa como tela principal (PID %d).", VMON_OUTPUT, (int)g_vmon_pid);

        /* Reinicia o Sunshine para vincular à nova tela virtual se ele não estiver com display */
        run_cmd_quiet("systemctl --user restart sunshine 2>/dev/null");
        log_msg("Sunshine pronto para conexões na tela virtual.");
    }
}

static void stop_vmon(void) {
    log_msg("Monitor físico (%s) LIGADO detectado! Aguardando estabilização do hardware...", g_last_physical_output);

    /* Aguarda 2 segundos para o DisplayPort concluir link training naturalmente */
    sleep(2);

    /* Encerra a tela virtual de forma limpa */
    if (g_vmon_pid > 0) {
        kill(g_vmon_pid, SIGTERM);
        int status;
        for (int i = 0; i < 10; i++) {
            if (waitpid(g_vmon_pid, &status, WNOHANG) > 0) {
                break;
            }
            usleep(100000);
        }
        if (kill(g_vmon_pid, 0) == 0) {
            kill(g_vmon_pid, SIGKILL);
            waitpid(g_vmon_pid, NULL, 0);
        }
        g_vmon_pid = -1;
    }
    run_cmd_quiet("pkill -f 'krfb-virtualmonitor.*" VMON_NAME "' 2>/dev/null");
    usleep(400000);

    /* Garante prioridade no monitor físico sem forçar atomic enable redundante */
    char doctor_cmd[256];
    snprintf(doctor_cmd, sizeof(doctor_cmd), "kscreen-doctor output.%.60s.priority.1 2>/dev/null", g_last_physical_output);
    run_cmd_quiet(doctor_cmd);

    log_msg("Tela virtual encerrada. Imagem transferida para o monitor físico %s com sucesso.", g_last_physical_output);
}

static void cleanup(void) {
    if (g_vmon_pid > 0) {
        kill(g_vmon_pid, SIGTERM);
        usleep(200000);
        waitpid(g_vmon_pid, NULL, WNOHANG);
        g_vmon_pid = -1;
    }
    run_cmd_quiet("pkill -f 'krfb-virtualmonitor.*" VMON_NAME "' 2>/dev/null");
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
            printf("sunshine-display-daemon (C edition) v2.0\n");
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

    /* Configura sinais */
    struct sigaction sa_chld;
    memset(&sa_chld, 0, sizeof(sa_chld));
    sa_chld.sa_handler = sigchld_handler;
    sa_chld.sa_flags = SA_RESTART | SA_NOCLDSTOP;
    sigaction(SIGCHLD, &sa_chld, NULL);

    struct sigaction sa_term;
    memset(&sa_term, 0, sizeof(sa_term));
    sa_term.sa_handler = sigterm_handler;
    sigaction(SIGTERM, &sa_term, NULL);
    sigaction(SIGINT, &sa_term, NULL);

    log_msg("Serviço sunshine-virtual-display (C edition) iniciado e ativo.");

    bool was_physical_active = has_physical_monitor();
    if (!was_physical_active) {
        log_msg("Iniciando com monitor físico desligado. Criando tela virtual imediatamente...");
        start_vmon();
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

        /* Espera evento do kernel ou timeout de 1 segundo */
        int ret = 0;
        if (nl_fd >= 0) {
            ret = poll(&pfd, 1, 1000);
            if (ret > 0 && (pfd.revents & POLLIN)) {
                /* Consome buffer do netlink */
                while (recv(nl_fd, nl_buf, sizeof(nl_buf), MSG_DONTWAIT) > 0);
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
                sleep(1); /* Aguarda estabilização do sinal de vídeo */
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
