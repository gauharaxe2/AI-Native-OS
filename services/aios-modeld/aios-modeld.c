#define _GNU_SOURCE

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#define LINE_MAX 8192
#define HTTP_MAX 16384

static char g_model_dir[256] = "/var/lib/aios/models";
static char g_sock_path[108] = "/run/aios/model.sock";
static char g_audit_sock[108] = "/run/aios/audit.sock";
static char g_llama[256] = "/usr/bin/llama-server";
static char g_model_path[512];
static char g_model_name[256];
static char g_state[16] = "missing";
static int g_predict = 24;
static pid_t g_server = -1;

static const char *base_name(const char *path)
{
	const char *slash = strrchr(path, '/');
	return slash ? slash + 1 : path;
}

static void copy_env(char *dst, size_t n, const char *key, const char *fallback)
{
	const char *value = getenv(key);
	if (value == NULL || value[0] == '\0')
		value = fallback;
	snprintf(dst, n, "%s", value);
}

static int json_escape(const char *in, char *out, size_t outlen)
{
	size_t o = 0;
	if (in == NULL)
		in = "";
	while (*in) {
		char extra = 0;
		if (*in == '"' || *in == '\\')
			extra = *in;
		else if (*in == '\n')
			extra = 'n';
		if (extra != 0) {
			if (o + 2 >= outlen)
				return -1;
			out[o++] = '\\';
			out[o++] = extra;
		} else {
			if (o + 1 >= outlen)
				return -1;
			out[o++] = *in;
		}
		in++;
	}
	out[o] = '\0';
	return 0;
}

static int json_unescape(const char *in, char *out, size_t outlen)
{
	size_t o = 0;
	while (*in && *in != '"') {
		char c = *in++;
		if (c == '\\') {
			if (*in == '\0')
				return -1;
			if (*in == 'n')
				c = '\n';
			else if (*in == '"' || *in == '\\' || *in == '/')
				c = *in;
			else
				return -1;
			in++;
		}
		if (o + 1 >= outlen)
			return -1;
		out[o++] = c;
	}
	out[o] = '\0';
	return 0;
}

static void find_model(void)
{
	DIR *dir;
	struct dirent *ent;

	g_model_path[0] = '\0';
	g_model_name[0] = '\0';
	dir = opendir(g_model_dir);
	if (dir == NULL) {
		snprintf(g_state, sizeof g_state, "missing");
		return;
	}
	while ((ent = readdir(dir)) != NULL) {
		size_t len = strlen(ent->d_name);
		if (len > 5 && strcmp(ent->d_name + len - 5, ".gguf") == 0) {
			snprintf(g_model_name, sizeof g_model_name, "%s", ent->d_name);
			snprintf(g_model_path, sizeof g_model_path, "%s/%s", g_model_dir, ent->d_name);
			break;
		}
	}
	closedir(dir);
	if (g_model_path[0] == '\0')
		snprintf(g_state, sizeof g_state, "missing");
}

static int server_alive(void)
{
	int status;
	if (g_server <= 0)
		return 0;
	if (waitpid(g_server, &status, WNOHANG) == 0)
		return 1;
	g_server = -1;
	snprintf(g_state, sizeof g_state, "failed");
	return 0;
}

static void start_server(void)
{
	pid_t pid;
	int logfd;

	if (g_model_path[0] == '\0') {
		snprintf(g_state, sizeof g_state, "missing");
		return;
	}
	if (access(g_llama, X_OK) != 0) {
		snprintf(g_state, sizeof g_state, "failed");
		return;
	}
	logfd = open("/var/log/llama-server.log", O_WRONLY | O_CREAT | O_TRUNC, 0644);
	pid = fork();
	if (pid < 0) {
		snprintf(g_state, sizeof g_state, "failed");
		if (logfd >= 0)
			close(logfd);
		return;
	}
	if (pid == 0) {
		if (logfd >= 0) {
			dup2(logfd, STDOUT_FILENO);
			dup2(logfd, STDERR_FILENO);
			close(logfd);
		}
		execl(g_llama, "llama-server",
			"-m", g_model_path,
			"--host", "127.0.0.1",
			"--port", "8081",
			"-t", "1",
			"-c", "256",
			"-ngl", "0",
			"--no-warmup",
			(char *)NULL);
		_exit(127);
	}
	if (logfd >= 0)
		close(logfd);
	g_server = pid;
	snprintf(g_state, sizeof g_state, "loading");
}

static int tcp_exchange(const char *request, char *resp, size_t resplen, int timeout_sec)
{
	int fd;
	struct sockaddr_in addr;
	struct timeval tv;
	size_t off = 0;
	size_t len = strlen(request);

	fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0)
		return -1;
	memset(&addr, 0, sizeof addr);
	addr.sin_family = AF_INET;
	addr.sin_port = htons(8081);
	addr.sin_addr.s_addr = htonl(0x7f000001);
	tv.tv_sec = timeout_sec;
	tv.tv_usec = 0;
	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
	setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
	if (connect(fd, (struct sockaddr *)&addr, sizeof addr) != 0) {
		close(fd);
		return -1;
	}
	while (off < len) {
		ssize_t w = write(fd, request + off, len - off);
		if (w < 0) {
			if (errno == EINTR)
				continue;
			close(fd);
			return -1;
		}
		off += (size_t)w;
	}
	off = 0;
	while (off + 1 < resplen) {
		ssize_t r = read(fd, resp + off, resplen - 1 - off);
		if (r < 0) {
			if (errno == EINTR)
				continue;
			break;
		}
		if (r == 0)
			break;
		off += (size_t)r;
	}
	resp[off] = '\0';
	close(fd);
	return off > 0 ? 0 : -1;
}

static int health_ok(void)
{
	char resp[1024];
	const char *req =
		"GET /health HTTP/1.1\r\n"
		"Host: 127.0.0.1\r\n"
		"Connection: close\r\n\r\n";
	if (tcp_exchange(req, resp, sizeof resp, 2) != 0)
		return 0;
	return strstr(resp, " 200 ") != NULL && strstr(resp, "\"status\":\"ok\"") != NULL;
}

static void refresh_state(void)
{
	if (strcmp(g_state, "missing") == 0)
		return;
	if (!server_alive()) {
		if (strcmp(g_state, "loading") == 0 || strcmp(g_state, "ready") == 0)
			snprintf(g_state, sizeof g_state, "failed");
		return;
	}
	if (health_ok())
		snprintf(g_state, sizeof g_state, "ready");
	else if (strcmp(g_state, "ready") != 0)
		snprintf(g_state, sizeof g_state, "loading");
}

static int audit_local(const char *prompt, char *err, size_t errlen)
{
	int fd;
	struct sockaddr_un addr;
	char target[161];
	char escaped[400];
	char payload[512];
	char line[1024];
	size_t n = 0;
	size_t len;

	snprintf(target, sizeof target, "%s", prompt);
	if (json_escape(target, escaped, sizeof escaped) != 0) {
		snprintf(err, errlen, "prompt could not be logged");
		return -1;
	}
	snprintf(payload, sizeof payload,
		"{\"op\":\"request\",\"actor\":\"user\",\"action\":\"model.local\",\"target\":\"%s\"}\n",
		escaped);
	fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd < 0) {
		snprintf(err, errlen, "audit socket failed");
		return -1;
	}
	memset(&addr, 0, sizeof addr);
	addr.sun_family = AF_UNIX;
	snprintf(addr.sun_path, sizeof addr.sun_path, "%s", g_audit_sock);
	if (connect(fd, (struct sockaddr *)&addr, sizeof addr) != 0) {
		close(fd);
		snprintf(err, errlen, "auditd is not running");
		return -1;
	}
	len = strlen(payload);
	if (write(fd, payload, len) != (ssize_t)len) {
		close(fd);
		snprintf(err, errlen, "audit write failed");
		return -1;
	}
	while (n + 1 < sizeof line) {
		char c;
		ssize_t r = read(fd, &c, 1);
		if (r <= 0)
			break;
		if (c == '\n') {
			line[n] = '\0';
			break;
		}
		line[n++] = c;
	}
	close(fd);
	if (n == 0) {
		snprintf(err, errlen, "audit gave no reply");
		return -1;
	}
	if (strstr(line, "\"state\":\"allowed\"") == NULL) {
		snprintf(err, errlen, "model.local was not allowed");
		return -1;
	}
	return 0;
}

static int infer(const char *user_prompt, char *answer, size_t answerlen, char *err, size_t errlen)
{
	char escaped_prompt[2048];
	char body[3072];
	char header[4096];
	char resp[HTTP_MAX];
	const char *content;
	char formatted[1800];

	if (strcmp(g_state, "ready") != 0) {
		snprintf(err, errlen, "model is %s", g_state);
		return -1;
	}
	snprintf(formatted, sizeof formatted,
		"<|im_start|>system\nYou are a helpful assistant.<|im_end|>\n"
		"<|im_start|>user\n%s<|im_end|>\n<|im_start|>assistant\n",
		user_prompt);
	if (json_escape(formatted, escaped_prompt, sizeof escaped_prompt) != 0) {
		snprintf(err, errlen, "prompt too long");
		return -1;
	}
	snprintf(body, sizeof body,
		"{\"prompt\":\"%s\",\"n_predict\":%d,\"temperature\":0.2,"
		"\"stream\":false,\"stop\":[\"<|im_end|>\"]}",
		escaped_prompt, g_predict);
	snprintf(header, sizeof header,
		"POST /completion HTTP/1.1\r\n"
		"Host: 127.0.0.1\r\n"
		"Content-Type: application/json\r\n"
		"Content-Length: %zu\r\n"
		"Connection: close\r\n\r\n%s",
		strlen(body), body);
	if (tcp_exchange(header, resp, sizeof resp, 300) != 0) {
		snprintf(err, errlen, "llama-server did not answer");
		return -1;
	}
	content = strstr(resp, "\"content\":\"");
	if (content == NULL) {
		snprintf(err, errlen, "model reply had no content");
		return -1;
	}
	content += strlen("\"content\":\"");
	if (json_unescape(content, answer, answerlen) != 0) {
		snprintf(err, errlen, "could not read model reply");
		return -1;
	}
	if (answer[0] == '\0') {
		snprintf(err, errlen, "model reply was empty");
		return -1;
	}
	return 0;
}

static void send_line(int fd, const char *text)
{
	size_t len = strlen(text);
	size_t off = 0;
	char buf[LINE_MAX];
	if (len + 2 >= sizeof buf)
		return;
	memcpy(buf, text, len);
	buf[len++] = '\n';
	buf[len] = '\0';
	while (off < len) {
		ssize_t w = write(fd, buf + off, len - off);
		if (w < 0) {
			if (errno == EINTR)
				continue;
			return;
		}
		off += (size_t)w;
	}
}

static void reply_error(int fd, const char *msg)
{
	char esc[512];
	char line[640];
	if (json_escape(msg, esc, sizeof esc) != 0)
		snprintf(esc, sizeof esc, "error");
	snprintf(line, sizeof line, "{\"ok\":false,\"error\":\"%s\"}", esc);
	send_line(fd, line);
}

static void handle_status(int fd)
{
	char esc[512];
	char line[700];
	refresh_state();
	if (json_escape(g_model_name, esc, sizeof esc) != 0)
		esc[0] = '\0';
	snprintf(line, sizeof line,
		"{\"ok\":true,\"state\":\"%s\",\"model\":\"%s\"}", g_state, esc);
	send_line(fd, line);
}

static void handle_ask(int fd, const char *prompt)
{
	char err[256];
	char answer[2048];
	char esc[4096];
	char line[LINE_MAX];

	refresh_state();
	if (audit_local(prompt, err, sizeof err) != 0) {
		reply_error(fd, err);
		return;
	}
	if (infer(prompt, answer, sizeof answer, err, sizeof err) != 0) {
		reply_error(fd, err);
		return;
	}
	if (json_escape(answer, esc, sizeof esc) != 0) {
		reply_error(fd, "answer too long");
		return;
	}
	snprintf(line, sizeof line, "{\"ok\":true,\"answer\":\"%s\"}", esc);
	send_line(fd, line);
}

static void handle_line(int fd, char *line)
{
	char *op;
	char *prompt;

	op = strstr(line, "\"op\":\"");
	if (op == NULL) {
		reply_error(fd, "missing op");
		return;
	}
	op += strlen("\"op\":\"");
	if (strncmp(op, "status\"", 7) == 0) {
		handle_status(fd);
		return;
	}
	if (strncmp(op, "ask\"", 4) == 0) {
		char text[800];
		prompt = strstr(line, "\"prompt\":\"");
		if (prompt == NULL) {
			reply_error(fd, "missing prompt");
			return;
		}
		prompt += strlen("\"prompt\":\"");
		if (json_unescape(prompt, text, sizeof text) != 0) {
			reply_error(fd, "bad prompt");
			return;
		}
		handle_ask(fd, text);
		return;
	}
	reply_error(fd, "unknown op");
}

static int daemon_main(void)
{
	int listen_fd;
	struct sockaddr_un addr;
	const char *predict;

	find_model();
	predict = getenv("AIOS_MODEL_PREDICT");
	if (predict != NULL && predict[0] != '\0') {
		int n = atoi(predict);
		if (n > 0 && n < 256)
			g_predict = n;
	}
	if (g_model_path[0] != '\0')
		start_server();

	if (strlen(g_sock_path) >= sizeof addr.sun_path)
		return 1;
	listen_fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (listen_fd < 0)
		return 1;
	memset(&addr, 0, sizeof addr);
	addr.sun_family = AF_UNIX;
	snprintf(addr.sun_path, sizeof addr.sun_path, "%s", g_sock_path);
	unlink(g_sock_path);
	if (bind(listen_fd, (struct sockaddr *)&addr, sizeof addr) != 0)
		return 1;
	chmod(g_sock_path, 0660);
	if (listen(listen_fd, 4) != 0)
		return 1;

	for (;;) {
		struct pollfd pfd;
		int cfd;
		char buf[LINE_MAX];
		size_t n = 0;

		pfd.fd = listen_fd;
		pfd.events = POLLIN;
		if (poll(&pfd, 1, 1000) < 0) {
			if (errno == EINTR)
				continue;
			return 1;
		}
		refresh_state();
		if (!(pfd.revents & POLLIN))
			continue;
		cfd = accept(listen_fd, NULL, NULL);
		if (cfd < 0)
			continue;
		while (n + 1 < sizeof buf) {
			char c;
			ssize_t r = read(cfd, &c, 1);
			if (r <= 0)
				break;
			if (c == '\n') {
				buf[n] = '\0';
				handle_line(cfd, buf);
				break;
			}
			buf[n++] = c;
		}
		close(cfd);
	}
}

static int rpc(const char *payload, char *line, size_t linelen)
{
	int fd;
	struct sockaddr_un addr;
	size_t n = 0;
	size_t len = strlen(payload);

	fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd < 0)
		return -1;
	memset(&addr, 0, sizeof addr);
	addr.sun_family = AF_UNIX;
	snprintf(addr.sun_path, sizeof addr.sun_path, "%s", g_sock_path);
	if (connect(fd, (struct sockaddr *)&addr, sizeof addr) != 0) {
		close(fd);
		return -1;
	}
	if (write(fd, payload, len) != (ssize_t)len || write(fd, "\n", 1) != 1) {
		close(fd);
		return -1;
	}
	while (n + 1 < linelen) {
		char c;
		ssize_t r = read(fd, &c, 1);
		if (r <= 0)
			break;
		if (c == '\n') {
			line[n] = '\0';
			close(fd);
			return 0;
		}
		line[n++] = c;
	}
	close(fd);
	return -1;
}

static int cmd_status(void)
{
	char line[1024];
	char state[32];
	char model[256];
	const char *p;

	if (rpc("{\"op\":\"status\"}", line, sizeof line) != 0) {
		fprintf(stderr, "aios-model: scheduler is not running\n");
		return 1;
	}
	p = strstr(line, "\"state\":\"");
	if (p == NULL)
		return 1;
	p += strlen("\"state\":\"");
	if (json_unescape(p, state, sizeof state) != 0)
		return 1;
	model[0] = '\0';
	p = strstr(line, "\"model\":\"");
	if (p != NULL) {
		p += strlen("\"model\":\"");
		json_unescape(p, model, sizeof model);
	}
	printf("state: %s\n", state);
	printf("model: %s\n", model[0] ? model : "none");
	return strcmp(state, "failed") == 0 ? 1 : 0;
}

static int cmd_ask(const char *prompt)
{
	char payload[1200];
	char escaped[900];
	char line[LINE_MAX];
	char answer[2048];
	const char *p;

	if (json_escape(prompt, escaped, sizeof escaped) != 0) {
		fprintf(stderr, "aios-model: prompt too long\n");
		return 1;
	}
	snprintf(payload, sizeof payload, "{\"op\":\"ask\",\"prompt\":\"%s\"}", escaped);
	if (rpc(payload, line, sizeof line) != 0) {
		fprintf(stderr, "aios-model: scheduler is not running\n");
		return 1;
	}
	if (strstr(line, "\"ok\":false") != NULL) {
		p = strstr(line, "\"error\":\"");
		if (p != NULL) {
			char err[256];
			p += strlen("\"error\":\"");
			if (json_unescape(p, err, sizeof err) == 0)
				fprintf(stderr, "aios-model: %s\n", err);
		}
		return 1;
	}
	p = strstr(line, "\"answer\":\"");
	if (p == NULL) {
		fprintf(stderr, "aios-model: bad reply\n");
		return 1;
	}
	p += strlen("\"answer\":\"");
	if (json_unescape(p, answer, sizeof answer) != 0)
		return 1;
	printf("%s\n", answer);
	return 0;
}

int main(int argc, char **argv)
{
	copy_env(g_model_dir, sizeof g_model_dir, "AIOS_MODEL_DIR", "/var/lib/aios/models");
	copy_env(g_sock_path, sizeof g_sock_path, "AIOS_MODEL_SOCK", "/run/aios/model.sock");
	copy_env(g_audit_sock, sizeof g_audit_sock, "AIOS_AUDIT_SOCK", "/run/aios/audit.sock");
	copy_env(g_llama, sizeof g_llama, "AIOS_LLAMA_SERVER", "/usr/bin/llama-server");

	if (strcmp(base_name(argv[0]), "aios-modeld") == 0 && argc == 1)
		return daemon_main();
	if (argc >= 2 && strcmp(argv[1], "status") == 0)
		return cmd_status();
	if (argc >= 3 && strcmp(argv[1], "ask") == 0)
		return cmd_ask(argv[2]);
	fprintf(stderr, "usage: aios-model status | aios-model ask <prompt>\n");
	return 2;
}
