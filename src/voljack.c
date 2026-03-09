#define _POSIX_C_SOURCE 200809L
#include <jack/jack.h>

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#include <time.h>

#define DEFAULT_CHANNELS 2
#define DEFAULT_MIN_DB   (-INFINITY)
#define DEFAULT_MAX_DB   (10.0f)
#define DEFAULT_START_DB (-INFINITY)
#define DEFAULT_STEP_DB  (1.0f)

#define CONFIG_DIR_REL   "/.config/voljack"
#define CONFIG_FILE_REL  "/.config/voljack/config"
#define DEFAULT_SOCKET   "/tmp/voljack.sock"

typedef struct {
	int channels;
	float min_db;
	float max_db;
	float start_db;
	float step_db;
	char socket_path[PATH_MAX];
} config_t;

typedef struct {
	jack_client_t *client;
	jack_port_t **in_ports;
	jack_port_t **out_ports;

	volatile sig_atomic_t running;

	int channels;

	// control state
	volatile sig_atomic_t db_is_neg_inf;
	volatile float current_db;
	volatile float target_gain;
	float current_gain;

	float min_db;
	float max_db;
	float step_db;

	char socket_path[PATH_MAX];
	int server_fd;
} app_t;

static app_t g_app = {0};

static void trim(char *s) {
	if (!s) return;

	char *p = s;
	while (*p && isspace((unsigned char)*p)) p++;
	if (p != s) memmove(s, p, strlen(p) + 1);

	size_t len = strlen(s);
	while (len > 0 && isspace((unsigned char)s[len - 1])) {
		s[len - 1] = '\0';
		len--;
	}
}

static int mkdir_p(const char *dir) {
	char tmp[PATH_MAX];
	size_t len = strlen(dir);
	if (len >= sizeof(tmp)) return -1;

	strcpy(tmp, dir);

	for (char *p = tmp + 1; *p; p++) {
		if (*p == '/') {
			*p = '\0';
			if (mkdir(tmp, 0755) != 0 && errno != EEXIST) return -1;
			*p = '/';
		}
	}
	if (mkdir(tmp, 0755) != 0 && errno != EEXIST) return -1;
	return 0;
}

static int parse_int_str(const char *s, int *out) {
	char *end = NULL;
	errno = 0;
	long v = strtol(s, &end, 10);
	if (errno || end == s || *end != '\0' || v < 1 || v > 4096) return -1;
	*out = (int)v;
	return 0;
}

static int parse_float_str(const char *s, float *out) {
	char *end = NULL;
	errno = 0;
	float v = strtof(s, &end);
	if (errno || end == s || *end != '\0') return -1;
	*out = v;
	return 0;
}

static int parse_db_value(const char *s, float *out, int *is_neg_inf) {
	if (strcasecmp(s, "-inf") == 0) {
		*out = -INFINITY;
		*is_neg_inf = 1;
		return 0;
	}
	*is_neg_inf = 0;
	return parse_float_str(s, out);
}

static void format_db(char *buf, size_t n, int is_neg_inf, float db) {
	if (is_neg_inf) snprintf(buf, n, "-inf");
	else snprintf(buf, n, "%.2f", db);
}

static float db_to_gain(int is_neg_inf, float db) {
	if (is_neg_inf) return 0.0f;
	return powf(10.0f, db / 20.0f);
}

static float clampf_local(float v, float lo, float hi) {
	if (v < lo) return lo;
	if (v > hi) return hi;
	return v;
}

static void apply_db(app_t *app, int is_neg_inf, float db) {
	if (is_neg_inf) {
		app->db_is_neg_inf = 1;
		app->current_db = -INFINITY;
		app->target_gain = 0.0f;
		return;
	}

	db = clampf_local(db, app->min_db, app->max_db);
	app->db_is_neg_inf = 0;
	app->current_db = db;
	app->target_gain = db_to_gain(0, db);
}

static void set_initial_db(app_t *app, int is_neg_inf, float db) {
	apply_db(app, is_neg_inf, db);
	app->current_gain = app->target_gain;
}

static void config_defaults(config_t *cfg) {
	cfg->channels = DEFAULT_CHANNELS;
	cfg->min_db = DEFAULT_MIN_DB;
	cfg->max_db = DEFAULT_MAX_DB;
	cfg->start_db = DEFAULT_START_DB;
	cfg->step_db = DEFAULT_STEP_DB;
	snprintf(cfg->socket_path, sizeof(cfg->socket_path), "%s", DEFAULT_SOCKET);
}

static int get_paths(char *config_dir, size_t config_dir_n,
		char *config_file, size_t config_file_n) {
	const char *home = getenv("HOME");
	if (!home || !*home) return -1;

	if (snprintf(config_dir, config_dir_n, "%s%s", home, CONFIG_DIR_REL) >= (int)config_dir_n) return -1;
	if (snprintf(config_file, config_file_n, "%s%s", home, CONFIG_FILE_REL) >= (int)config_file_n) return -1;
	return 0;
}

static int save_config(const config_t *cfg) {
	char config_dir[PATH_MAX];
	char config_file[PATH_MAX];
	if (get_paths(config_dir, sizeof(config_dir), config_file, sizeof(config_file)) != 0) return -1;
	if (mkdir_p(config_dir) != 0) return -1;

	FILE *f = fopen(config_file, "w");
	if (!f) return -1;

	fprintf(f, "IO=%d\n", cfg->channels);

	if (isinf(cfg->min_db) && cfg->min_db < 0) fprintf(f, "MIN_DB=-inf\n");
	else fprintf(f, "MIN_DB=%.2f\n", cfg->min_db);

	fprintf(f, "MAX_DB=%.2f\n", cfg->max_db);

	if (isinf(cfg->start_db) && cfg->start_db < 0) fprintf(f, "START_DB=-inf\n");
	else fprintf(f, "START_DB=%.2f\n", cfg->start_db);

	fprintf(f, "STEP_DB=%.2f\n", cfg->step_db);
	fprintf(f, "SOCKET=%s\n", cfg->socket_path);

	fclose(f);
	return 0;
}

static int load_config(config_t *cfg, int *exists) {
	char config_dir[PATH_MAX];
	char config_file[PATH_MAX];
	*exists = 0;

	if (get_paths(config_dir, sizeof(config_dir), config_file, sizeof(config_file)) != 0) return -1;

	FILE *f = fopen(config_file, "r");
	if (!f) {
		if (errno == ENOENT) return 0;
		return -1;
	}
	*exists = 1;

	char line[512];
	while (fgets(line, sizeof(line), f)) {
		trim(line);
		if (line[0] == '\0' || line[0] == '#') continue;

		char *eq = strchr(line, '=');
		if (!eq) continue;
		*eq = '\0';

		char *key = line;
		char *val = eq + 1;
		trim(key);
		trim(val);

		if (strcmp(key, "IO") == 0 || strcmp(key, "CHANNELS") == 0) {
			int v;
			if (parse_int_str(val, &v) == 0) cfg->channels = v;
		} else if (strcmp(key, "MIN_DB") == 0) {
			float v;
			int neg_inf;
			if (parse_db_value(val, &v, &neg_inf) == 0)
				cfg->min_db = neg_inf ? -INFINITY : v;
		} else if (strcmp(key, "MAX_DB") == 0) {
			float v;
			if (parse_float_str(val, &v) == 0) cfg->max_db = v;
		} else if (strcmp(key, "START_DB") == 0) {
			float v;
			int neg_inf;
			if (parse_db_value(val, &v, &neg_inf) == 0)
				cfg->start_db = neg_inf ? -INFINITY : v;
		} else if (strcmp(key, "STEP_DB") == 0) {
			float v;
			if (parse_float_str(val, &v) == 0) cfg->step_db = v;
		} else if (strcmp(key, "SOCKET") == 0) {
			snprintf(cfg->socket_path, sizeof(cfg->socket_path), "%s", val);
		}
	}

	fclose(f);

	if (cfg->max_db < cfg->min_db && !isinf(cfg->min_db)) {
		float t = cfg->max_db;
		cfg->max_db = cfg->min_db;
		cfg->min_db = t;
	}

	return 0;
}

static int prompt_first_run_config(config_t *cfg) {
	char buf[128];
	fprintf(stderr, "No config found.\n");
	fprintf(stderr, "Enter channel count: ");
	fflush(stderr);

	if (!fgets(buf, sizeof(buf), stdin)) return -1;
	trim(buf);

	int ch = 0;
	if (parse_int_str(buf, &ch) != 0) {
		fprintf(stderr, "Invalid channel count.\n");
		return -1;
	}

	cfg->channels = ch;
	return save_config(cfg);
}

static int process_cb(jack_nframes_t nframes, void *arg) {
	app_t *app = (app_t *)arg;

	float gain = app->current_gain;
	float target = app->target_gain;
	float step = 0.0f;

	if (nframes > 1) {
		step = (target - gain) / (float)(nframes - 1);
	} else if (nframes == 1) {
		step = target - gain;
	}

	for (int ch = 0; ch < app->channels; ch++) {
		jack_default_audio_sample_t *in =
			(jack_default_audio_sample_t *)jack_port_get_buffer(app->in_ports[ch], nframes);
		jack_default_audio_sample_t *out =
			(jack_default_audio_sample_t *)jack_port_get_buffer(app->out_ports[ch], nframes);

		float g = gain;
		for (jack_nframes_t i = 0; i < nframes; i++) {
			out[i] = in[i] * g;
			g += step;
		}
	}

	app->current_gain = target;
	return 0;
}

static void shutdown_cb(void *arg) {
	(void)arg;
	g_app.running = 0;
}

static void signal_handler(int sig) {
	(void)sig;
	g_app.running = 0;
}

static int connect_control_socket(const char *path) {
	int fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd < 0) return -1;

	struct sockaddr_un addr;
	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	if (strlen(path) >= sizeof(addr.sun_path)) {
		close(fd);
		errno = ENAMETOOLONG;
		return -1;
	}
	snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path);

	if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
		close(fd);
		return -1;
	}
	return fd;
}

static int setup_control_server(app_t *app) {
	app->server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (app->server_fd < 0) return -1;

	unlink(app->socket_path);

	struct sockaddr_un addr;
	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;

	if (strlen(app->socket_path) >= sizeof(addr.sun_path)) {
		close(app->server_fd);
		app->server_fd = -1;
		errno = ENAMETOOLONG;
		return -1;
	}

	snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", app->socket_path);

	if (bind(app->server_fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) return -1;
	if (listen(app->server_fd, 8) != 0) return -1;

	int flags = fcntl(app->server_fd, F_GETFL, 0);
	if (flags >= 0) fcntl(app->server_fd, F_SETFL, flags | O_NONBLOCK);

	return 0;
}

static void send_reply(int fd, const char *msg) {
	(void)!write(fd, msg, strlen(msg));
}

static void handle_command(app_t *app, const char *cmd, int fd) {
	char tmp[256];
	snprintf(tmp, sizeof(tmp), "%s", cmd);
	trim(tmp);

	if (strcmp(tmp, "get") == 0) {
		char dbbuf[64], out[128];
		format_db(dbbuf, sizeof(dbbuf), app->db_is_neg_inf, app->current_db);
		snprintf(out, sizeof(out), "%s dB\n", dbbuf);
		send_reply(fd, out);
		return;
	}

	if (strncmp(tmp, "set ", 4) == 0) {
		char *arg = tmp + 4;
		trim(arg);

		float db;
		int neg_inf;
		if (parse_db_value(arg, &db, &neg_inf) != 0) {
			send_reply(fd, "ERR invalid value\n");
			return;
		}

		if (!neg_inf && db < app->min_db) db = app->min_db;
		if (!neg_inf && db > app->max_db) db = app->max_db;

		apply_db(app, neg_inf, db);
		send_reply(fd, "OK\n");
		return;
	}

	if (strncmp(tmp, "inc", 3) == 0) {
		float step = app->step_db;
		if (tmp[3] != '\0') {
			char *arg = tmp + 3;
			trim(arg);
			if (*arg) {
				if (parse_float_str(arg, &step) != 0) {
					send_reply(fd, "ERR invalid step\n");
					return;
				}
			}
		}

		if (app->db_is_neg_inf) apply_db(app, 0, app->min_db);
		else apply_db(app, 0, app->current_db + step);

		send_reply(fd, "OK\n");
		return;
	}

	if (strncmp(tmp, "dec", 3) == 0) {
		float step = app->step_db;
		if (tmp[3] != '\0') {
			char *arg = tmp + 3;
			trim(arg);
			if (*arg) {
				if (parse_float_str(arg, &step) != 0) {
					send_reply(fd, "ERR invalid step\n");
					return;
				}
			}
		}

		if (app->db_is_neg_inf) {
			send_reply(fd, "OK\n");
			return;
		}

		float next = app->current_db - step;
		if (!isinf(app->min_db) && next < app->min_db) next = app->min_db;
		apply_db(app, 0, next);
		send_reply(fd, "OK\n");
		return;
	}

	send_reply(fd, "ERR unknown command\n");
}

static void poll_control_socket(app_t *app) {
	for (;;) {
		int fd = accept(app->server_fd, NULL, NULL);
		if (fd < 0) {
			if (errno == EAGAIN || errno == EWOULDBLOCK) return;
			return;
		}

		char buf[256];
		ssize_t n = read(fd, buf, sizeof(buf) - 1);
		if (n > 0) {
			buf[n] = '\0';
			handle_command(app, buf, fd);
		}
		close(fd);
	}
}

static int send_client_command(const char *socket_path, const char *cmd) {
	int fd = connect_control_socket(socket_path);
	if (fd < 0) {
		fprintf(stderr, "Could not connect to running voljack at %s\n", socket_path);
		return 1;
	}

	if (write(fd, cmd, strlen(cmd)) < 0 || write(fd, "\n", 1) < 0) {
		close(fd);
		return 1;
	}

	char buf[256];
	ssize_t n = read(fd, buf, sizeof(buf) - 1);
	if (n > 0) {
		buf[n] = '\0';
		fputs(buf, stdout);
	}

	close(fd);
	return 0;
}

static void usage(const char *argv0) {
	fprintf(stderr,
			"Usage:\n"
			"  %s [-c N] [--min-db X] [--max-db X] [--start-db X] [--step-db X] [--socket PATH] [--name NAME]\n"
			"  %s get [--socket PATH]\n"
			"  %s set <dB|-inf> [--socket PATH]\n"
			"  %s inc [STEP_DB] [--socket PATH]\n"
			"  %s dec [STEP_DB] [--socket PATH]\n",
			argv0, argv0, argv0, argv0, argv0);
}

int main(int argc, char **argv) {
	config_t cfg;
	config_defaults(&cfg);

	int cfg_exists = 0;
	if (load_config(&cfg, &cfg_exists) != 0) {
		fprintf(stderr, "Failed to load config\n");
		return 1;
	}

	// detect command mode early
	if (argc >= 2 &&
			(strcmp(argv[1], "get") == 0 ||
			 strcmp(argv[1], "set") == 0 ||
			 strcmp(argv[1], "inc") == 0 ||
			 strcmp(argv[1], "dec") == 0)) {

		const char *cmd = argv[1];
		const char *socket_path = cfg.socket_path;

		for (int i = 2; i < argc; i++) {
			if (strcmp(argv[i], "--socket") == 0 && i + 1 < argc) {
				socket_path = argv[++i];
			}
		}

		if (strcmp(cmd, "get") == 0) {
			return send_client_command(socket_path, "get");
		} else if (strcmp(cmd, "set") == 0) {
			if (argc < 3) {
				usage(argv[0]);
				return 1;
			}
			char line[256];
			snprintf(line, sizeof(line), "set %s", argv[2]);
			return send_client_command(socket_path, line);
		} else if (strcmp(cmd, "inc") == 0) {
			char line[256] = "inc";
			if (argc >= 3 && argv[2][0] != '-') {
				snprintf(line, sizeof(line), "inc %s", argv[2]);
			}
			return send_client_command(socket_path, line);
		} else if (strcmp(cmd, "dec") == 0) {
			char line[256] = "dec";
			if (argc >= 3 && argv[2][0] != '-') {
				snprintf(line, sizeof(line), "dec %s", argv[2]);
			}
			return send_client_command(socket_path, line);
		}
	}

	bool channel_override = false;
	const char *client_name = "voljack";

	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "-c") == 0 && i + 1 < argc) {
			int ch;
			if (parse_int_str(argv[++i], &ch) != 0) {
				fprintf(stderr, "Invalid channel count\n");
				return 1;
			}
			cfg.channels = ch;
			channel_override = true;
		} else if (strcmp(argv[i], "--min-db") == 0 && i + 1 < argc) {
			float v; int neg_inf;
			if (parse_db_value(argv[++i], &v, &neg_inf) != 0) return 1;
			cfg.min_db = neg_inf ? -INFINITY : v;
		} else if (strcmp(argv[i], "--max-db") == 0 && i + 1 < argc) {
			if (parse_float_str(argv[++i], &cfg.max_db) != 0) return 1;
		} else if (strcmp(argv[i], "--start-db") == 0 && i + 1 < argc) {
			float v; int neg_inf;
			if (parse_db_value(argv[++i], &v, &neg_inf) != 0) return 1;
			cfg.start_db = neg_inf ? -INFINITY : v;
		} else if (strcmp(argv[i], "--step-db") == 0 && i + 1 < argc) {
			if (parse_float_str(argv[++i], &cfg.step_db) != 0) return 1;
		} else if (strcmp(argv[i], "--socket") == 0 && i + 1 < argc) {
			snprintf(cfg.socket_path, sizeof(cfg.socket_path), "%s", argv[++i]);
		} else if (strcmp(argv[i], "--name") == 0 && i + 1 < argc) {
			client_name = argv[++i];
		} else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
			usage(argv[0]);
			return 0;
		}
	}

	if (!cfg_exists && !channel_override) {
		if (prompt_first_run_config(&cfg) != 0) return 1;
	}

	if (cfg.max_db < cfg.min_db && !isinf(cfg.min_db)) {
		fprintf(stderr, "MAX_DB must be >= MIN_DB\n");
		return 1;
	}

	app_t app = {0};
	app.running = 1;
	app.channels = cfg.channels;
	app.min_db = cfg.min_db;
	app.max_db = cfg.max_db;
	app.step_db = cfg.step_db;
	app.server_fd = -1;
	snprintf(app.socket_path, sizeof(app.socket_path), "%s", cfg.socket_path);

	if (isinf(cfg.start_db) && cfg.start_db < 0) {
		set_initial_db(&app, 1, -INFINITY);
	} else {
		set_initial_db(&app, 0, cfg.start_db);
	}

	g_app = app;

	jack_status_t status = 0;
	g_app.client = jack_client_open(client_name, JackNullOption, &status);
	if (!g_app.client) {
		fprintf(stderr, "Failed to connect to JACK\n");
		return 1;
	}

	g_app.in_ports = calloc((size_t)g_app.channels, sizeof(*g_app.in_ports));
	g_app.out_ports = calloc((size_t)g_app.channels, sizeof(*g_app.out_ports));
	if (!g_app.in_ports || !g_app.out_ports) {
		fprintf(stderr, "Out of memory\n");
		jack_client_close(g_app.client);
		return 1;
	}

	for (int i = 0; i < g_app.channels; i++) {
		char name[64];

		snprintf(name, sizeof(name), "in%d", i);
		g_app.in_ports[i] = jack_port_register(
				g_app.client, name, JACK_DEFAULT_AUDIO_TYPE, JackPortIsInput, 0);
		if (!g_app.in_ports[i]) {
			fprintf(stderr, "Failed to register input port %d\n", i);
			return 1;
		}

		snprintf(name, sizeof(name), "out%d", i);
		g_app.out_ports[i] = jack_port_register(
				g_app.client, name, JACK_DEFAULT_AUDIO_TYPE, JackPortIsOutput, 0);
		if (!g_app.out_ports[i]) {
			fprintf(stderr, "Failed to register output port %d\n", i);
			return 1;
		}
	}

	jack_set_process_callback(g_app.client, process_cb, &g_app);
	jack_on_shutdown(g_app.client, shutdown_cb, &g_app);

	if (jack_activate(g_app.client) != 0) {
		fprintf(stderr, "Failed to activate JACK client\n");
		return 1;
	}

	if (setup_control_server(&g_app) != 0) {
		fprintf(stderr, "Failed to open control socket: %s\n", g_app.socket_path);
		return 1;
	}

	signal(SIGINT, signal_handler);
	signal(SIGTERM, signal_handler);

	char dbbuf[64];
	format_db(dbbuf, sizeof(dbbuf), g_app.db_is_neg_inf, g_app.current_db);
	fprintf(stderr,
			"voljack running: channels=%d volume=%s dB gain=%f socket=%s\n",
			g_app.channels, dbbuf, g_app.current_gain, g_app.socket_path);

	while (g_app.running) {
		poll_control_socket(&g_app);
		struct timespec ts = {0, 20000000};
		nanosleep(&ts, NULL);
	}

	if (g_app.server_fd >= 0) close(g_app.server_fd);
	unlink(g_app.socket_path);
	jack_client_close(g_app.client);
	free(g_app.in_ports);
	free(g_app.out_ports);
	return 0;
}
