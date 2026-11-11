#define _GNU_SOURCE

#include "sha256.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#define LINE_MAX 4096
#define MAX_CLIENTS 16
#define MAX_PENDING 16
#define GENESIS "0000000000000000000000000000000000000000000000000000000000000000"

static char g_log_path[256] = "/var/lib/aios/audit.log";
static char g_sock_path[108] = "/run/aios/audit.sock";
static char g_policy_path[256] = "/etc/aios/policy.conf";
static char g_backup_dir[256] = "/var/lib/aios/backups";
static char g_prev[65];
static unsigned g_next_id = 1;

struct record {
	unsigned id;
	char timestamp[80];
	char actor[64];
	char approver[64];
	char action[64];
	char target[256];
	char risk[16];
	char decision[16];
	char result[512];
	char undo[1024];
	unsigned ref;
	char prev[65];
	char hash[65];
};

struct client {
	int fd;
	char buf[LINE_MAX];
	size_t len;
	unsigned waiting_for;
};

struct pending {
	int used;
	unsigned id;
	int client_fd;
	char actor[64];
	char action[64];
	char target[256];
	char risk[16];
};

static struct client g_clients[MAX_CLIENTS];
static struct pending g_pending[MAX_PENDING];

static void die(const char *msg)
{
	fprintf(stderr, "aios-auditd: %s\n", msg);
	exit(1);
}

static int sha256_selfcheck(void)
{
	char hex[65];

	sha256_hex("", 0, hex);
	if (strcmp(hex, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855") != 0)
		return -1;
	sha256_hex("abc", 3, hex);
	if (strcmp(hex, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad") != 0)
		return -1;
	return 0;
}

static const char *base_name(const char *path)
{
	const char *slash = strrchr(path, '/');
	return slash ? slash + 1 : path;
}

static void copy_env(char *dst, size_t dstlen, const char *env, const char *fallback)
{
	const char *value = getenv(env);
	if (value == NULL || value[0] == '\0')
		value = fallback;
	snprintf(dst, dstlen, "%s", value);
}

static int mkdir_p(const char *path)
{
	char tmp[512];
	size_t len;
	char *p;

	len = strlen(path);
	if (len == 0 || len >= sizeof tmp)
		return -1;
	memcpy(tmp, path, len + 1);
	for (p = tmp + 1; *p; p++) {
		if (*p == '/') {
			*p = '\0';
			if (mkdir(tmp, 0755) < 0 && errno != EEXIST)
				return -1;
			*p = '/';
		}
	}
	if (mkdir(tmp, 0755) < 0 && errno != EEXIST)
		return -1;
	return 0;
}

static int mkdir_parent(const char *file)
{
	char tmp[512];
	char *slash;

	snprintf(tmp, sizeof tmp, "%s", file);
	slash = strrchr(tmp, '/');
	if (slash == NULL)
		return 0;
	*slash = '\0';
	if (tmp[0] == '\0')
		return 0;
	return mkdir_p(tmp);
}

static int json_escape(const char *in, char *out, size_t outlen)
{
	size_t o = 0;

	if (in == NULL)
		in = "";
	while (*in) {
		const char *extra = NULL;
		char ch[2];

		if (*in == '"' || *in == '\\') {
			ch[0] = '\\';
			ch[1] = *in;
			extra = ch;
		} else if (*in == '\n') {
			ch[0] = '\\';
			ch[1] = 'n';
			extra = ch;
		}
		if (extra != NULL) {
			if (o + 2 >= outlen)
				return -1;
			out[o++] = extra[0];
			out[o++] = extra[1];
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

static int json_get(const char *json, const char *key, char *out, size_t outlen)
{
	char pat[80];
	const char *p;
	size_t o = 0;

	snprintf(pat, sizeof pat, "\"%s\"", key);
	p = strstr(json, pat);
	if (p == NULL)
		return -1;
	p += strlen(pat);
	while (*p == ' ' || *p == '\t')
		p++;
	if (*p != ':')
		return -1;
	p++;
	while (*p == ' ' || *p == '\t')
		p++;
	if (*p == '"') {
		p++;
		while (*p && *p != '"') {
			char c = *p++;
			if (c == '\\') {
				if (*p == '\0')
					return -1;
				if (*p == 'n')
					c = '\n';
				else if (*p == '"' || *p == '\\' || *p == '/')
					c = *p;
				else
					return -1;
				p++;
			}
			if (o + 1 >= outlen)
				return -1;
			out[o++] = c;
		}
		out[o] = '\0';
		return 0;
	}
	while (*p && *p != ',' && *p != '}' && *p != ' ') {
		if (o + 1 >= outlen)
			return -1;
		out[o++] = *p++;
	}
	out[o] = '\0';
	return o == 0 ? -1 : 0;
}

static int canon_hash(const struct record *r, char out[65])
{
	char canon[LINE_MAX];
	int n;

	n = snprintf(canon, sizeof canon,
		"%u\n%s\n%s\n%s\n%s\n%s\n%s\n%s\n%s\n%s\n%u\n%s",
		r->id, r->timestamp, r->actor, r->approver, r->action, r->target,
		r->risk, r->decision, r->result, r->undo, r->ref, r->prev);
	if (n < 0 || (size_t)n >= sizeof canon)
		return -1;
	sha256_hex(canon, (size_t)n, out);
	return 0;
}

static int parse_record(const char *line, struct record *r)
{
	char idbuf[32];
	char refbuf[32];

	memset(r, 0, sizeof *r);
	if (json_get(line, "id", idbuf, sizeof idbuf) != 0)
		return -1;
	if (json_get(line, "timestamp", r->timestamp, sizeof r->timestamp) != 0)
		return -1;
	if (json_get(line, "actor", r->actor, sizeof r->actor) != 0)
		return -1;
	if (json_get(line, "approver", r->approver, sizeof r->approver) != 0)
		return -1;
	if (json_get(line, "action", r->action, sizeof r->action) != 0)
		return -1;
	if (json_get(line, "target", r->target, sizeof r->target) != 0)
		return -1;
	if (json_get(line, "risk", r->risk, sizeof r->risk) != 0)
		return -1;
	if (json_get(line, "decision", r->decision, sizeof r->decision) != 0)
		return -1;
	if (json_get(line, "result", r->result, sizeof r->result) != 0)
		return -1;
	if (json_get(line, "undo", r->undo, sizeof r->undo) != 0)
		return -1;
	if (json_get(line, "ref", refbuf, sizeof refbuf) != 0)
		return -1;
	if (json_get(line, "prev", r->prev, sizeof r->prev) != 0)
		return -1;
	if (json_get(line, "hash", r->hash, sizeof r->hash) != 0)
		return -1;
	r->id = (unsigned)strtoul(idbuf, NULL, 10);
	r->ref = (unsigned)strtoul(refbuf, NULL, 10);
	return 0;
}

static int load_and_verify(struct record *last, unsigned *count, char *err, size_t errlen)
{
	FILE *file;
	char line[LINE_MAX];
	char expect_prev[65];
	unsigned n = 0;

	memset(last, 0, sizeof *last);
	*count = 0;
	memcpy(expect_prev, GENESIS, 65);
	file = fopen(g_log_path, "r");
	if (file == NULL) {
		if (errno == ENOENT)
			return 0;
		snprintf(err, errlen, "cannot read audit log");
		return -1;
	}
	while (fgets(line, sizeof line, file) != NULL) {
		struct record rec;
		char got[65];
		size_t len = strlen(line);

		if (len == 0 || (len == 1 && line[0] == '\n'))
			continue;
		if (line[len - 1] != '\n') {
			snprintf(err, errlen, "truncated audit line");
			fclose(file);
			return -1;
		}
		line[len - 1] = '\0';
		if (parse_record(line, &rec) != 0) {
			snprintf(err, errlen, "unreadable audit record");
			fclose(file);
			return -1;
		}
		if (strcmp(rec.prev, expect_prev) != 0 || canon_hash(&rec, got) != 0 ||
		    strcmp(got, rec.hash) != 0) {
			snprintf(err, errlen, "chain broken at id %u", rec.id);
			fclose(file);
			return -1;
		}
		memcpy(expect_prev, rec.hash, 65);
		*last = rec;
		n++;
	}
	fclose(file);
	*count = n;
	return 0;
}

static int write_record(const struct record *r)
{
	char actor[128], approver[128], action[128], target[512];
	char risk[40], decision[40], result[512], undo[1024];
	char line[LINE_MAX];
	int fd;
	int n;
	size_t off;
	size_t len;

	if (json_escape(r->actor, actor, sizeof actor) != 0 ||
	    json_escape(r->approver, approver, sizeof approver) != 0 ||
	    json_escape(r->action, action, sizeof action) != 0 ||
	    json_escape(r->target, target, sizeof target) != 0 ||
	    json_escape(r->risk, risk, sizeof risk) != 0 ||
	    json_escape(r->decision, decision, sizeof decision) != 0 ||
	    json_escape(r->result, result, sizeof result) != 0 ||
	    json_escape(r->undo, undo, sizeof undo) != 0)
		return -1;
	n = snprintf(line, sizeof line,
		"{\"id\":%u,\"timestamp\":\"%s\",\"actor\":\"%s\",\"approver\":\"%s\","
		"\"action\":\"%s\",\"target\":\"%s\",\"risk\":\"%s\",\"decision\":\"%s\","
		"\"result\":\"%s\",\"undo\":\"%s\",\"ref\":%u,\"prev\":\"%s\",\"hash\":\"%s\"}\n",
		r->id, r->timestamp, actor, approver, action, target, risk, decision,
		result, undo, r->ref, r->prev, r->hash);
	if (n < 0 || (size_t)n >= sizeof line)
		return -1;
	fd = open(g_log_path, O_WRONLY | O_CREAT | O_APPEND, 0644);
	if (fd < 0)
		return -1;
	len = (size_t)n;
	off = 0;
	while (off < len) {
		ssize_t w = write(fd, line + off, len - off);
		if (w < 0) {
			if (errno == EINTR)
				continue;
			close(fd);
			return -1;
		}
		off += (size_t)w;
	}
	if (fsync(fd) != 0) {
		close(fd);
		return -1;
	}
	close(fd);
	return 0;
}

static void stamp(struct record *r)
{
	struct timespec ts;
	struct tm tm;

	clock_gettime(CLOCK_REALTIME, &ts);
	gmtime_r(&ts.tv_sec, &tm);
	snprintf(r->timestamp, sizeof r->timestamp, "%04d-%02d-%02dT%02d:%02d:%02dZ",
		tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
		tm.tm_hour, tm.tm_min, tm.tm_sec);
}

static int commit_record(struct record *r)
{
	r->id = g_next_id;
	snprintf(r->prev, sizeof r->prev, "%s", g_prev);
	if (canon_hash(r, r->hash) != 0)
		return -1;
	if (write_record(r) != 0)
		return -1;
	snprintf(g_prev, sizeof g_prev, "%s", r->hash);
	g_next_id++;
	return 0;
}

static int path_ok(const char *path)
{
	const char *p;

	if (strncmp(path, "/tmp/", 5) != 0 || path[5] == '\0')
		return 0;
	if (strlen(path) >= 180)
		return 0;
	for (p = path; *p; p++) {
		if ((unsigned char)*p < 32 || *p == '\\')
			return 0;
	}
	for (p = path; (p = strchr(p, '/')) != NULL; p++) {
		if (p[1] == '.' && p[2] == '.' && (p[3] == '/' || p[3] == '\0'))
			return 0;
		if (p[1] == '\0')
			return 0;
	}
	return 1;
}

static int copy_file(const char *src, const char *dst)
{
	char buf[4096];
	int in;
	int out;
	ssize_t n;
	size_t total = 0;

	in = open(src, O_RDONLY | O_NOFOLLOW);
	if (in < 0)
		return -1;
	out = open(dst, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW, 0644);
	if (out < 0) {
		close(in);
		return -1;
	}
	while ((n = read(in, buf, sizeof buf)) > 0) {
		ssize_t off = 0;
		total += (size_t)n;
		if (total > 1024 * 1024) {
			close(in);
			close(out);
			unlink(dst);
			errno = EFBIG;
			return -1;
		}
		while (off < n) {
			ssize_t w = write(out, buf + off, (size_t)(n - off));
			if (w < 0) {
				close(in);
				close(out);
				return -1;
			}
			off += w;
		}
	}
	close(in);
	if (n < 0 || fsync(out) != 0) {
		close(out);
		return -1;
	}
	close(out);
	return 0;
}

static int execute_action(unsigned id, const char *action, const char *target,
	char *result, size_t result_len, char *undo, size_t undo_len)
{
	char backup[512];
	struct stat st;

	snprintf(result, result_len, "error");
	snprintf(undo, undo_len, "irreversible");
	snprintf(backup, sizeof backup, "%s/%u", g_backup_dir, id);

	if (strcmp(action, "file.create") == 0) {
		int fd;
		char body[128];
		int n;
		int existed = 0;

		if (!path_ok(target)) {
			snprintf(result, result_len, "rejected bad path");
			return -1;
		}
		if (lstat(target, &st) == 0) {
			if (!S_ISREG(st.st_mode)) {
				snprintf(result, result_len, "rejected not a regular file");
				return -1;
			}
			if (copy_file(target, backup) != 0) {
				snprintf(result, result_len, "error backup failed");
				return -1;
			}
			existed = 1;
		}
		fd = open(target, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW, 0644);
		if (fd < 0) {
			snprintf(result, result_len, "error create failed");
			return -1;
		}
		n = snprintf(body, sizeof body, "created by aios-auditd id=%u\n", id);
		if (n < 0 || write(fd, body, (size_t)n) != n || fsync(fd) != 0) {
			close(fd);
			snprintf(result, result_len, "error write failed");
			return -1;
		}
		close(fd);
		snprintf(result, result_len, "created %s", target);
		if (existed)
			snprintf(undo, undo_len, "restore:%s:%s", backup, target);
		else
			snprintf(undo, undo_len, "delete:%s", target);
		return 0;
	}

	if (strcmp(action, "file.delete") == 0) {
		if (!path_ok(target)) {
			snprintf(result, result_len, "rejected bad path");
			return -1;
		}
		if (lstat(target, &st) != 0 || !S_ISREG(st.st_mode)) {
			snprintf(result, result_len, "error not a regular file");
			return -1;
		}
		if (copy_file(target, backup) != 0) {
			snprintf(result, result_len, "error backup failed");
			return -1;
		}
		if (unlink(target) != 0) {
			snprintf(result, result_len, "error unlink failed");
			return -1;
		}
		snprintf(result, result_len, "deleted %s", target);
		snprintf(undo, undo_len, "restore:%s:%s", backup, target);
		return 0;
	}

	if (strcmp(action, "system.poweroff") == 0) {
		snprintf(result, result_len, "not executed: phase 2 does not power off");
		return -1;
	}

	snprintf(result, result_len, "unknown action");
	return -1;
}

static int apply_undo(const char *undo, char *result, size_t result_len)
{
	if (strncmp(undo, "delete:", 7) == 0) {
		const char *path = undo + 7;
		if (!path_ok(path) || unlink(path) != 0) {
			snprintf(result, result_len, "error undo delete failed");
			return -1;
		}
		snprintf(result, result_len, "removed %s", path);
		return 0;
	}
	if (strncmp(undo, "restore:", 8) == 0) {
		const char *rest = undo + 8;
		const char *sep = strstr(rest, ":/tmp/");
		char backup[512];
		size_t blen;

		if (sep == NULL) {
			snprintf(result, result_len, "error bad undo record");
			return -1;
		}
		blen = (size_t)(sep - rest);
		if (blen == 0 || blen >= sizeof backup) {
			snprintf(result, result_len, "error bad undo record");
			return -1;
		}
		memcpy(backup, rest, blen);
		backup[blen] = '\0';
		if (!path_ok(sep + 1) || copy_file(backup, sep + 1) != 0) {
			snprintf(result, result_len, "error undo restore failed");
			return -1;
		}
		snprintf(result, result_len, "restored %s", sep + 1);
		return 0;
	}
	snprintf(result, result_len, "irreversible");
	return -1;
}

struct policy {
	char risk[8][16];
	char risk_decision[8][16];
	int nrisk;
	char action[32][64];
	char action_risk[32][16];
	char action_decision[32][16];
	int naction;
};

static int load_policy(struct policy *pol, char *err, size_t errlen)
{
	FILE *file;
	char line[256];

	memset(pol, 0, sizeof *pol);
	file = fopen(g_policy_path, "r");
	if (file == NULL) {
		snprintf(err, errlen, "cannot read policy");
		return -1;
	}
	while (fgets(line, sizeof line, file) != NULL) {
		char kind[32], a[64], b[32], c[32];
		int n;

		if (line[0] == '#' || line[0] == '\n')
			continue;
		n = sscanf(line, "%31s %63s %31s %31s", kind, a, b, c);
		if (n < 3)
			continue;
		if (strcmp(kind, "risk") == 0 && pol->nrisk < 8) {
			snprintf(pol->risk[pol->nrisk], 16, "%s", a);
			snprintf(pol->risk_decision[pol->nrisk], 16, "%s", b);
			pol->nrisk++;
		} else if (strcmp(kind, "action") == 0 && pol->naction < 32) {
			snprintf(pol->action[pol->naction], 64, "%s", a);
			snprintf(pol->action_risk[pol->naction], 16, "%s", b);
			if (n >= 4)
				snprintf(pol->action_decision[pol->naction], 16, "%s", c);
			pol->naction++;
		}
	}
	fclose(file);
	return 0;
}

static int classify(const char *action, char *risk, size_t risklen,
	char *decision, size_t decisionlen, char *err, size_t errlen)
{
	struct policy pol;
	int i;
	const char *found_risk = NULL;
	const char *found_decision = NULL;

	if (load_policy(&pol, err, errlen) != 0)
		return -1;
	for (i = 0; i < pol.naction; i++) {
		if (strcmp(pol.action[i], action) == 0) {
			found_risk = pol.action_risk[i];
			if (pol.action_decision[i][0] != '\0')
				found_decision = pol.action_decision[i];
			break;
		}
	}
	if (found_risk == NULL) {
		snprintf(risk, risklen, "high");
		snprintf(decision, decisionlen, "deny");
		snprintf(err, errlen, "unknown action");
		return 0;
	}
	snprintf(risk, risklen, "%s", found_risk);
	if (found_decision == NULL) {
		for (i = 0; i < pol.nrisk; i++) {
			if (strcmp(pol.risk[i], found_risk) == 0) {
				found_decision = pol.risk_decision[i];
				break;
			}
		}
	}
	if (found_decision == NULL) {
		snprintf(decision, decisionlen, "deny");
		snprintf(err, errlen, "no decision for risk");
		return 0;
	}
	if (strcmp(found_decision, "allow") != 0 &&
	    strcmp(found_decision, "ask") != 0 &&
	    strcmp(found_decision, "deny") != 0) {
		snprintf(decision, decisionlen, "deny");
		snprintf(err, errlen, "bad policy decision");
		return 0;
	}
	snprintf(decision, decisionlen, "%s", found_decision);
	err[0] = '\0';
	return 0;
}

static int find_record(unsigned id, struct record *out)
{
	FILE *file;
	char line[LINE_MAX];

	file = fopen(g_log_path, "r");
	if (file == NULL)
		return -1;
	while (fgets(line, sizeof line, file) != NULL) {
		struct record rec;
		size_t len = strlen(line);
		if (len == 0 || line[len - 1] != '\n')
			continue;
		line[len - 1] = '\0';
		if (parse_record(line, &rec) != 0)
			continue;
		if (rec.id == id) {
			*out = rec;
			fclose(file);
			return 0;
		}
	}
	fclose(file);
	return -1;
}

static void fill_common(struct record *r, const char *actor, const char *approver,
	const char *action, const char *target, const char *risk,
	const char *decision, const char *result, const char *undo, unsigned ref)
{
	memset(r, 0, sizeof *r);
	stamp(r);
	snprintf(r->actor, sizeof r->actor, "%s", actor ? actor : "");
	snprintf(r->approver, sizeof r->approver, "%s", approver ? approver : "");
	snprintf(r->action, sizeof r->action, "%s", action ? action : "");
	snprintf(r->target, sizeof r->target, "%s", target ? target : "");
	snprintf(r->risk, sizeof r->risk, "%s", risk ? risk : "");
	snprintf(r->decision, sizeof r->decision, "%s", decision);
	snprintf(r->result, sizeof r->result, "%s", result);
	snprintf(r->undo, sizeof r->undo, "%s", undo);
	r->ref = ref;
}

static void send_all(int fd, const char *text)
{
	size_t len = strlen(text);
	size_t off = 0;

	while (off < len) {
		ssize_t w = write(fd, text + off, len - off);
		if (w < 0) {
			if (errno == EINTR)
				continue;
			return;
		}
		off += (size_t)w;
	}
}

static void reply(int fd, const char *fmt, ...)
{
	char buf[LINE_MAX];
	int n;
	va_list ap;

	va_start(ap, fmt);
	n = vsnprintf(buf, sizeof buf - 2, fmt, ap);
	va_end(ap);
	if (n < 0 || (size_t)n >= sizeof buf - 2)
		return;
	buf[n++] = '\n';
	buf[n] = '\0';
	send_all(fd, buf);
}

static void reply_error(int fd, const char *msg)
{
	char esc[256];
	if (json_escape(msg, esc, sizeof esc) != 0)
		snprintf(esc, sizeof esc, "error");
	reply(fd, "{\"ok\":false,\"error\":\"%s\"}", esc);
}

static void reply_state(int fd, const char *state, unsigned id, unsigned ref,
	const char *action, const char *target, const char *risk,
	const char *result, const char *undo)
{
	char eaction[128], etarget[512], erisk[40], eresult[512], eundo[1024];

	if (json_escape(action ? action : "", eaction, sizeof eaction) != 0 ||
	    json_escape(target ? target : "", etarget, sizeof etarget) != 0 ||
	    json_escape(risk ? risk : "", erisk, sizeof erisk) != 0 ||
	    json_escape(result ? result : "", eresult, sizeof eresult) != 0 ||
	    json_escape(undo ? undo : "", eundo, sizeof eundo) != 0) {
		reply_error(fd, "reply too long");
		return;
	}
	reply(fd,
		"{\"ok\":true,\"state\":\"%s\",\"id\":%u,\"ref\":%u,"
		"\"action\":\"%s\",\"target\":\"%s\",\"risk\":\"%s\","
		"\"result\":\"%s\",\"undo\":\"%s\"}",
		state, id, ref, eaction, etarget, erisk, eresult, eundo);
}

static struct pending *alloc_pending(void)
{
	int i;
	for (i = 0; i < MAX_PENDING; i++) {
		if (!g_pending[i].used)
			return &g_pending[i];
	}
	return NULL;
}

static struct pending *find_pending(unsigned id)
{
	int i;
	for (i = 0; i < MAX_PENDING; i++) {
		if (g_pending[i].used && g_pending[i].id == id)
			return &g_pending[i];
	}
	return NULL;
}

static void drop_client_fd(int fd);

static int finish_pending(struct pending *p, const char *approver, int allow,
	struct record *out)
{
	struct record rec;
	char result[512];
	char undo[1024];
	const char *decision;
	const char *state;
	int cfd;

	if (allow) {
		unsigned id = g_next_id;
		if (execute_action(id, p->action, p->target, result, sizeof result,
				undo, sizeof undo) != 0)
			snprintf(undo, sizeof undo, "irreversible");
		decision = "allowed";
		state = "allowed";
	} else {
		snprintf(result, sizeof result, "denied by user");
		snprintf(undo, sizeof undo, "irreversible");
		decision = "denied";
		state = "denied";
	}
	fill_common(&rec, p->actor, approver, p->action, p->target, p->risk,
		decision, result, undo, p->id);
	cfd = p->client_fd;
	p->client_fd = -1;
	p->used = 0;
	if (commit_record(&rec) != 0) {
		if (cfd >= 0) {
			reply_error(cfd, "failed to append audit record");
			drop_client_fd(cfd);
		}
		return -1;
	}
	if (cfd >= 0) {
		reply_state(cfd, state, rec.id, rec.ref, rec.action, rec.target,
			rec.risk, rec.result, rec.undo);
		drop_client_fd(cfd);
	}
	*out = rec;
	return 0;
}

static int name_ok(const char *s, size_t max)
{
	size_t n = 0;
	if (s == NULL || s[0] == '\0')
		return 0;
	while (s[n] != '\0') {
		unsigned char c = (unsigned char)s[n];
		if (!(isalnum(c) || c == '.' || c == '_' || c == '-'))
			return 0;
		n++;
		if (n > max)
			return 0;
	}
	return 1;
}

static void handle_request(int fd, const char *actor, const char *action, const char *target)
{
	struct record rec;
	char risk[16], decision[16], err[128];
	char result[512], undo[1024];

	if (!name_ok(actor, 32) || !name_ok(action, 32)) {
		reply_error(fd, "bad actor or action");
		drop_client_fd(fd);
		return;
	}
	if (target == NULL || target[0] == '\0' || strlen(target) >= 180 || strchr(target, '\n')) {
		reply_error(fd, "bad target");
		drop_client_fd(fd);
		return;
	}
	if (classify(action, risk, sizeof risk, decision, sizeof decision, err, sizeof err) != 0) {
		reply_error(fd, err);
		drop_client_fd(fd);
		return;
	}
	if (err[0] != '\0' && strcmp(decision, "deny") == 0 && strcmp(err, "unknown action") == 0) {
		fill_common(&rec, actor, "", action, target, risk, "denied", err, "irreversible", 0);
		if (commit_record(&rec) != 0) {
			reply_error(fd, "failed to append audit record");
			drop_client_fd(fd);
			return;
		}
		reply_state(fd, "denied", rec.id, 0, action, target, risk, err, "irreversible");
		drop_client_fd(fd);
		return;
	}
	if (strcmp(decision, "deny") == 0) {
		fill_common(&rec, actor, "", action, target, risk, "denied",
			"denied by policy", "irreversible", 0);
		if (commit_record(&rec) != 0) {
			reply_error(fd, "failed to append audit record");
			drop_client_fd(fd);
			return;
		}
		reply_state(fd, "denied", rec.id, 0, action, target, risk,
			"denied by policy", "irreversible");
		drop_client_fd(fd);
		return;
	}
	if (strcmp(decision, "ask") == 0) {
		struct pending *slot = alloc_pending();
		if (slot == NULL) {
			reply_error(fd, "too many pending requests");
			drop_client_fd(fd);
			return;
		}
		fill_common(&rec, actor, "", action, target, risk, "pending",
			"waiting", "irreversible", 0);
		if (commit_record(&rec) != 0) {
			reply_error(fd, "failed to append audit record");
			drop_client_fd(fd);
			return;
		}
		slot->used = 1;
		slot->id = rec.id;
		slot->client_fd = fd;
		snprintf(slot->actor, sizeof slot->actor, "%s", actor);
		snprintf(slot->action, sizeof slot->action, "%s", action);
		snprintf(slot->target, sizeof slot->target, "%s", target);
		snprintf(slot->risk, sizeof slot->risk, "%s", risk);
		reply_state(fd, "pending", rec.id, 0, action, target, risk, "waiting", "irreversible");
		return;
	}

	{
		unsigned id = g_next_id;
		if (execute_action(id, action, target, result, sizeof result, undo, sizeof undo) != 0)
			snprintf(undo, sizeof undo, "irreversible");
		fill_common(&rec, actor, "", action, target, risk, "allowed", result, undo, 0);
		if (commit_record(&rec) != 0) {
			reply_error(fd, "failed to append audit record");
			drop_client_fd(fd);
			return;
		}
		reply_state(fd, "allowed", rec.id, 0, action, target, risk, result, undo);
		drop_client_fd(fd);
	}
}

static void handle_resolve(int fd, unsigned id, const char *actor, int allow)
{
	struct pending *p;

	if (!name_ok(actor, 32)) {
		reply_error(fd, "bad actor");
		drop_client_fd(fd);
		return;
	}
	p = find_pending(id);
	if (p == NULL) {
		reply_error(fd, "no pending request with that id");
		drop_client_fd(fd);
		return;
	}
	{
		struct record rec;
		if (finish_pending(p, actor, allow, &rec) != 0)
			reply_error(fd, "failed to append audit record");
		else
			reply_state(fd, rec.decision, rec.id, rec.ref, rec.action,
				rec.target, rec.risk, rec.result, rec.undo);
	}
	drop_client_fd(fd);
}

static void handle_undo(int fd, unsigned id, const char *actor)
{
	struct record orig;
	struct record rec;
	char result[512];

	if (!name_ok(actor, 32)) {
		reply_error(fd, "bad actor");
		drop_client_fd(fd);
		return;
	}
	if (find_record(id, &orig) != 0) {
		reply_error(fd, "no such audit id");
		drop_client_fd(fd);
		return;
	}
	if (apply_undo(orig.undo, result, sizeof result) != 0) {
		fill_common(&rec, actor, "", "audit.undo", orig.target, "low", "denied",
			result, "irreversible", id);
		if (commit_record(&rec) != 0) {
			reply_error(fd, "failed to append audit record");
			drop_client_fd(fd);
			return;
		}
		reply_state(fd, "denied", rec.id, id, "audit.undo", orig.target, "low",
			result, "irreversible");
		drop_client_fd(fd);
		return;
	}
	fill_common(&rec, actor, "", "audit.undo", orig.target, "low", "allowed",
		result, "irreversible", id);
	if (commit_record(&rec) != 0) {
		reply_error(fd, "undo applied but audit append failed");
		drop_client_fd(fd);
		return;
	}
	reply_state(fd, "allowed", rec.id, id, "audit.undo", orig.target, "low",
		result, "irreversible");
	drop_client_fd(fd);
}

static void handle_line(int fd, const char *line)
{
	char op[32], actor[64], action[64], target[256], idbuf[32];

	if (json_get(line, "op", op, sizeof op) != 0) {
		reply_error(fd, "missing op");
		drop_client_fd(fd);
		return;
	}
	if (strcmp(op, "status") == 0) {
		reply(fd, "{\"ok\":true,\"state\":\"running\"}");
		drop_client_fd(fd);
		return;
	}
	if (strcmp(op, "request") == 0) {
		if (json_get(line, "actor", actor, sizeof actor) != 0 ||
		    json_get(line, "action", action, sizeof action) != 0 ||
		    json_get(line, "target", target, sizeof target) != 0) {
			reply_error(fd, "request needs actor, action, and target");
			drop_client_fd(fd);
			return;
		}
		handle_request(fd, actor, action, target);
		return;
	}
	if (strcmp(op, "approve") == 0 || strcmp(op, "deny") == 0) {
		if (json_get(line, "actor", actor, sizeof actor) != 0 ||
		    json_get(line, "id", idbuf, sizeof idbuf) != 0) {
			reply_error(fd, "needs actor and id");
			drop_client_fd(fd);
			return;
		}
		handle_resolve(fd, (unsigned)strtoul(idbuf, NULL, 10), actor,
			strcmp(op, "approve") == 0);
		return;
	}
	if (strcmp(op, "undo") == 0) {
		if (json_get(line, "actor", actor, sizeof actor) != 0 ||
		    json_get(line, "id", idbuf, sizeof idbuf) != 0) {
			reply_error(fd, "needs actor and id");
			drop_client_fd(fd);
			return;
		}
		handle_undo(fd, (unsigned)strtoul(idbuf, NULL, 10), actor);
		return;
	}
	reply_error(fd, "unknown op");
	drop_client_fd(fd);
}

static int client_slot(int fd)
{
	int i;
	for (i = 0; i < MAX_CLIENTS; i++) {
		if (g_clients[i].fd == fd)
			return i;
	}
	return -1;
}

static void drop_client_fd(int fd)
{
	int slot = client_slot(fd);
	int i;
	if (slot >= 0) {
		g_clients[slot].fd = -1;
		g_clients[slot].len = 0;
		g_clients[slot].waiting_for = 0;
	}
	for (i = 0; i < MAX_PENDING; i++) {
		if (g_pending[i].used && g_pending[i].client_fd == fd)
			g_pending[i].client_fd = -1;
	}
	close(fd);
}

static void on_client_data(int fd)
{
	int slot = client_slot(fd);
	ssize_t n;
	char tmp[512];

	if (slot < 0)
		return;
	n = read(fd, tmp, sizeof tmp);
	if (n < 0) {
		if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
			return;
		drop_client_fd(fd);
		return;
	}
	if (n == 0) {
		drop_client_fd(fd);
		return;
	}
	if (g_clients[slot].len + (size_t)n >= sizeof g_clients[slot].buf) {
		reply_error(fd, "line too long");
		drop_client_fd(fd);
		return;
	}
	memcpy(g_clients[slot].buf + g_clients[slot].len, tmp, (size_t)n);
	g_clients[slot].len += (size_t)n;
	g_clients[slot].buf[g_clients[slot].len] = '\0';
	{
		char *nl = strchr(g_clients[slot].buf, '\n');
		if (nl == NULL)
			return;
		*nl = '\0';
		handle_line(fd, g_clients[slot].buf);
		if (g_clients[slot].fd == fd) {
			g_clients[slot].len = 0;
			g_clients[slot].buf[0] = '\0';
		}
	}
}

static int daemon_main(void)
{
	struct record last;
	unsigned count = 0;
	char err[128];
	int listen_fd;
	struct sockaddr_un addr;
	int i;

	if (sha256_selfcheck() != 0)
		die("sha256 self-check failed");
	if (mkdir_parent(g_log_path) != 0 || mkdir_parent(g_sock_path) != 0 ||
	    mkdir_p(g_backup_dir) != 0)
		die("cannot create audit directories");
	if (strlen(g_sock_path) >= sizeof addr.sun_path)
		die("socket path too long");
	if (load_and_verify(&last, &count, err, sizeof err) != 0) {
		fprintf(stderr, "aios-auditd: %s\n", err);
		return 1;
	}
	if (count == 0) {
		memcpy(g_prev, GENESIS, 65);
		g_next_id = 1;
	} else {
		snprintf(g_prev, sizeof g_prev, "%s", last.hash);
		g_next_id = last.id + 1;
	}

	signal(SIGPIPE, SIG_IGN);
	listen_fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (listen_fd < 0)
		die("socket");
	memset(&addr, 0, sizeof addr);
	addr.sun_family = AF_UNIX;
	snprintf(addr.sun_path, sizeof addr.sun_path, "%s", g_sock_path);
	unlink(g_sock_path);
	if (bind(listen_fd, (struct sockaddr *)&addr, sizeof addr) != 0)
		die("bind");
	chmod(g_sock_path, 0660);
	if (listen(listen_fd, 8) != 0)
		die("listen");

	for (i = 0; i < MAX_CLIENTS; i++) {
		g_clients[i].fd = -1;
		g_clients[i].len = 0;
		g_clients[i].waiting_for = 0;
	}

	for (;;) {
		struct pollfd pfds[1 + MAX_CLIENTS];
		int map[1 + MAX_CLIENTS];
		int np = 0;
		int ready;

		pfds[np].fd = listen_fd;
		pfds[np].events = POLLIN;
		map[np] = -1;
		np++;
		for (i = 0; i < MAX_CLIENTS; i++) {
			if (g_clients[i].fd < 0)
				continue;
			pfds[np].fd = g_clients[i].fd;
			pfds[np].events = POLLIN;
			map[np] = i;
			np++;
		}
		ready = poll(pfds, (nfds_t)np, -1);
		if (ready < 0) {
			if (errno == EINTR)
				continue;
			die("poll");
		}
		if (pfds[0].revents & POLLIN) {
			int cfd = accept(listen_fd, NULL, NULL);
			int slot = -1;
			if (cfd >= 0) {
				for (i = 0; i < MAX_CLIENTS; i++) {
					if (g_clients[i].fd < 0) {
						slot = i;
						break;
					}
				}
				if (slot < 0) {
					reply_error(cfd, "too many clients");
					close(cfd);
				} else {
					int flags = fcntl(cfd, F_GETFL, 0);
					if (flags >= 0)
						fcntl(cfd, F_SETFL, flags | O_NONBLOCK);
					g_clients[slot].fd = cfd;
					g_clients[slot].len = 0;
					g_clients[slot].waiting_for = 0;
				}
			}
		}
		for (i = 1; i < np; i++) {
			if ((pfds[i].revents & POLLIN) && g_clients[map[i]].fd == pfds[i].fd)
				on_client_data(pfds[i].fd);
		}
		for (i = 1; i < np; i++) {
			if ((pfds[i].revents & (POLLERR | POLLHUP | POLLNVAL)) &&
			    g_clients[map[i]].fd == pfds[i].fd)
				drop_client_fd(pfds[i].fd);
		}
	}
}

static int print_reply(const char *json)
{
	char ok[16], state[32], id[32], ref[32], result[300];
	char action[64], target[300], undo[500], risk[16], err[300];

	ok[0] = state[0] = id[0] = ref[0] = result[0] = '\0';
	action[0] = target[0] = undo[0] = risk[0] = err[0] = '\0';
	if (json_get(json, "ok", ok, sizeof ok) != 0) {
		fprintf(stderr, "aios-audit: bad reply\n");
		return 1;
	}
	if (strcmp(ok, "false") == 0) {
		if (json_get(json, "error", err, sizeof err) != 0)
			snprintf(err, sizeof err, "request failed");
		fprintf(stderr, "aios-audit: %s\n", err);
		return 1;
	}
	json_get(json, "state", state, sizeof state);
	json_get(json, "id", id, sizeof id);
	json_get(json, "ref", ref, sizeof ref);
	json_get(json, "result", result, sizeof result);
	json_get(json, "action", action, sizeof action);
	json_get(json, "target", target, sizeof target);
	json_get(json, "undo", undo, sizeof undo);
	json_get(json, "risk", risk, sizeof risk);
	if (ref[0] == '\0')
		snprintf(ref, sizeof ref, "0");
	if (strcmp(state, "pending") == 0) {
		printf("pending id=%s action=%s target=%s risk=%s\n", id, action, target, risk);
		fflush(stdout);
		return 0;
	}
	if (strcmp(state, "allowed") == 0) {
		printf("allowed id=%s ref=%s result=%s undo=%s\n", id, ref, result, undo);
		fflush(stdout);
		return 0;
	}
	if (strcmp(state, "denied") == 0) {
		printf("denied id=%s ref=%s result=%s\n", id, ref, result);
		fflush(stdout);
		return 0;
	}
	if (strcmp(state, "running") == 0) {
		printf("running\n");
		fflush(stdout);
		return 0;
	}
	fprintf(stderr, "aios-audit: bad reply\n");
	return 1;
}

static int rpc(const char *payload, int expect_followup)
{
	int fd;
	struct sockaddr_un addr;
	char line[LINE_MAX];

	if (strlen(g_sock_path) >= sizeof addr.sun_path) {
		fprintf(stderr, "aios-audit: socket path too long\n");
		return 1;
	}
	fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd < 0) {
		fprintf(stderr, "aios-audit: cannot open socket\n");
		return 1;
	}
	memset(&addr, 0, sizeof addr);
	addr.sun_family = AF_UNIX;
	snprintf(addr.sun_path, sizeof addr.sun_path, "%s", g_sock_path);
	if (connect(fd, (struct sockaddr *)&addr, sizeof addr) != 0) {
		fprintf(stderr, "aios-audit: auditd is not running\n");
		close(fd);
		return 1;
	}
	send_all(fd, payload);
	send_all(fd, "\n");
	{
		size_t n = 0;
		while (n + 1 < sizeof line) {
			char c;
			ssize_t r = read(fd, &c, 1);
			if (r < 0) {
				if (errno == EINTR)
					continue;
				fprintf(stderr, "aios-audit: read failed\n");
				close(fd);
				return 1;
			}
			if (r == 0) {
				fprintf(stderr, "aios-audit: short reply\n");
				close(fd);
				return 1;
			}
			if (c == '\n') {
				line[n] = '\0';
				break;
			}
			line[n++] = c;
		}
		if (n + 1 >= sizeof line) {
			fprintf(stderr, "aios-audit: reply too long\n");
			close(fd);
			return 1;
		}
	}
	if (print_reply(line) != 0) {
		close(fd);
		return 1;
	}
	if (expect_followup && strstr(line, "\"state\":\"pending\"") != NULL) {
		size_t n = 0;
		while (n + 1 < sizeof line) {
			char c;
			ssize_t r = read(fd, &c, 1);
			if (r <= 0) {
				fprintf(stderr, "aios-audit: pending request closed\n");
				close(fd);
				return 1;
			}
			if (c == '\n') {
				line[n] = '\0';
				break;
			}
			line[n++] = c;
		}
		close(fd);
		return print_reply(line);
	}
	close(fd);
	return 0;
}

static int cmd_list(void)
{
	FILE *file;
	char line[LINE_MAX];
	unsigned n = 0;

	file = fopen(g_log_path, "r");
	if (file == NULL) {
		if (errno == ENOENT) {
			printf("audit log empty\n");
			return 0;
		}
		fprintf(stderr, "aios-audit: cannot read audit log\n");
		return 1;
	}
	while (fgets(line, sizeof line, file) != NULL) {
		struct record rec;
		size_t len = strlen(line);
		if (len == 0 || line[len - 1] != '\n')
			continue;
		line[len - 1] = '\0';
		if (parse_record(line, &rec) != 0) {
			fprintf(stderr, "aios-audit: unreadable record\n");
			fclose(file);
			return 1;
		}
		printf("%u %s %s %s %s\n", rec.id, rec.decision, rec.action, rec.target, rec.actor);
		n++;
	}
	fclose(file);
	if (n == 0)
		printf("audit log empty\n");
	return 0;
}

static int cmd_show(unsigned id)
{
	struct record rec;
	if (find_record(id, &rec) != 0) {
		fprintf(stderr, "aios-audit: no such audit id\n");
		return 1;
	}
	printf("id: %u\n", rec.id);
	printf("timestamp: %s\n", rec.timestamp);
	printf("actor: %s\n", rec.actor);
	printf("approver: %s\n", rec.approver);
	printf("action: %s\n", rec.action);
	printf("target: %s\n", rec.target);
	printf("risk: %s\n", rec.risk);
	printf("decision: %s\n", rec.decision);
	printf("result: %s\n", rec.result);
	printf("undo: %s\n", rec.undo);
	printf("ref: %u\n", rec.ref);
	printf("prev: %s\n", rec.prev);
	printf("hash: %s\n", rec.hash);
	return 0;
}

static int cmd_verify(void)
{
	struct record last;
	unsigned count = 0;
	char err[128];

	if (load_and_verify(&last, &count, err, sizeof err) != 0) {
		printf("verify failed: %s\n", err);
		return 1;
	}
	printf("verify ok records=%u\n", count);
	return 0;
}

static void client_usage(void)
{
	fprintf(stderr,
		"usage: aios-audit request <action> <target>\n"
		"       aios-audit approve <id>\n"
		"       aios-audit deny <id>\n"
		"       aios-audit undo <id>\n"
		"       aios-audit list\n"
		"       aios-audit show <id>\n"
		"       aios-audit verify\n"
		"       aios-audit status\n");
}

static int client_main(int argc, char **argv)
{
	char payload[LINE_MAX];
	char etarget[512];

	if (argc < 2) {
		client_usage();
		return 2;
	}
	if (strcmp(argv[1], "list") == 0)
		return cmd_list();
	if (strcmp(argv[1], "verify") == 0)
		return cmd_verify();
	if (strcmp(argv[1], "show") == 0) {
		if (argc != 3) {
			client_usage();
			return 2;
		}
		return cmd_show((unsigned)strtoul(argv[2], NULL, 10));
	}
	if (strcmp(argv[1], "status") == 0)
		return rpc("{\"op\":\"status\"}", 0);
	if (strcmp(argv[1], "request") == 0) {
		if (argc != 4) {
			client_usage();
			return 2;
		}
		if (json_escape(argv[3], etarget, sizeof etarget) != 0) {
			fprintf(stderr, "aios-audit: target too long\n");
			return 1;
		}
		snprintf(payload, sizeof payload,
			"{\"op\":\"request\",\"actor\":\"agent\",\"action\":\"%s\",\"target\":\"%s\"}",
			argv[2], etarget);
		return rpc(payload, 1);
	}
	if (strcmp(argv[1], "approve") == 0 || strcmp(argv[1], "deny") == 0 ||
	    strcmp(argv[1], "undo") == 0) {
		unsigned id;
		if (argc != 3) {
			client_usage();
			return 2;
		}
		id = (unsigned)strtoul(argv[2], NULL, 10);
		snprintf(payload, sizeof payload,
			"{\"op\":\"%s\",\"actor\":\"user\",\"id\":%u}", argv[1], id);
		return rpc(payload, 0);
	}
	client_usage();
	return 2;
}

int main(int argc, char **argv)
{
	copy_env(g_log_path, sizeof g_log_path, "AIOS_AUDIT_LOG", "/var/lib/aios/audit.log");
	copy_env(g_sock_path, sizeof g_sock_path, "AIOS_AUDIT_SOCK", "/run/aios/audit.sock");
	copy_env(g_policy_path, sizeof g_policy_path, "AIOS_AUDIT_POLICY", "/etc/aios/policy.conf");
	copy_env(g_backup_dir, sizeof g_backup_dir, "AIOS_AUDIT_BACKUP", "/var/lib/aios/backups");

	if (strcmp(base_name(argv[0]), "aios-auditd") == 0 && argc == 1)
		return daemon_main();
	return client_main(argc, argv);
}
