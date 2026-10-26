#include "cv610_sns.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* One row per Cv610SnsType, indexed by the enum so a missing initializer is a
 * compile-time hole rather than a silent default. */
struct Cv610SnsInfo {
	const char *name;    /* the system's name for the part */
	const char *suffix;  /* libsns_<suffix>.so */
	const char *symbol;  /* exported ot_isp_sns_obj */
	int         sns_id;  /* ot_sensor_id the ISP binds */
};

static const struct Cv610SnsInfo g_sns_info[SNS_TYPE_MAX] = {
	[SNS_TYPE_IMX662] = {
		"imx662", "imx662", "g_sns_imx662_obj", 662,
	},
};

/* The part a craft with no usable hint comes up as: the one this backend has
 * always served when nothing named another. */
#define CV610_SNS_FALLBACK SNS_TYPE_IMX662

static int name_case_eq(const char *a, const char *b)
{
	if (a == NULL || b == NULL)
		return 0;
	while (*a != '\0' && *b != '\0') {
		char ca = *a;
		char cb = *b;

		if (ca >= 'A' && ca <= 'Z')
			ca = (char)(ca - 'A' + 'a');
		if (cb >= 'A' && cb <= 'Z')
			cb = (char)(cb - 'A' + 'a');
		if (ca != cb)
			return 0;
		a++;
		b++;
	}
	return *a == '\0' && *b == '\0';
}

Cv610SnsType cv610_sns_from_name(const char *name)
{
	int i;

	if (name == NULL || *name == '\0')
		return SNS_TYPE_UNKNOWN;
	for (i = 0; i < SNS_TYPE_MAX; i++) {
		if (name_case_eq(name, g_sns_info[i].name))
			return (Cv610SnsType)i;
	}
	return SNS_TYPE_UNKNOWN;
}

/* Trim trailing whitespace/newlines left by env values or command output. */
static void hint_trim(char *s)
{
	size_t len;

	if (s == NULL)
		return;
	len = strlen(s);
	while (len > 0 && (s[len - 1] == '\n' || s[len - 1] == '\r' ||
			   s[len - 1] == ' ' || s[len - 1] == '\t'))
		s[--len] = '\0';
}

static int hint_from_cmd(const char *cmd, char *buf, size_t n)
{
	FILE *p;
	int got;

	p = popen(cmd, "r");
	if (p == NULL)
		return -1;
	got = fgets(buf, (int)n, p) != NULL;
	(void)pclose(p);
	if (!got)
		return -1;
	hint_trim(buf);
	return buf[0] != '\0' ? 0 : -1;
}

int cv610_sns_read_system_hint(char *buf, size_t n, Cv610SnsSource *src)
{
	const char *env;

	if (buf == NULL || n == 0)
		return -1;
	buf[0] = '\0';
	/* The normal boot path exports SENSOR in rcS before S95waybeam runs, so
	 * this is the cheap case and forks nothing.  A manual restart from ssh
	 * inherits neither, hence the two command fallbacks below. */
	env = getenv("SENSOR");
	if (env != NULL && *env != '\0') {
		snprintf(buf, n, "%s", env);
		hint_trim(buf);
		if (buf[0] != '\0') {
			if (src != NULL)
				*src = CV610_SRC_ENV;
			return 0;
		}
	}
	if (hint_from_cmd("fw_printenv -n sensor 2>/dev/null", buf, n) == 0) {
		if (src != NULL)
			*src = CV610_SRC_UBOOT;
		return 0;
	}
	if (hint_from_cmd("ipcinfo --short-sensor 2>/dev/null", buf, n) == 0) {
		if (src != NULL)
			*src = CV610_SRC_IPCINFO;
		return 0;
	}
	buf[0] = '\0';
	return -1;
}

int cv610_sns_resolve_hint(const char *hint, Cv610SnsSource hint_src,
	Cv610SnsType *out, Cv610SnsSource *src)
{
	Cv610SnsType type = SNS_TYPE_UNKNOWN;

	if (out == NULL || src == NULL)
		return -1;
	if (hint != NULL && *hint != '\0')
		type = cv610_sns_from_name(hint);
	if (type != SNS_TYPE_UNKNOWN) {
		*out = type;
		*src = hint_src;
		return 0;
	}
	/* Nothing usable: keep the craft streaming on the historical default and
	 * let the caller warn.  The hint is a system fact, not an operator
	 * decision, so an unusable one is not a config error. */
	*out = CV610_SNS_FALLBACK;
	*src = CV610_SRC_DEFAULT;
	return (hint != NULL && *hint != '\0') ? 2 : 1;
}

/* ---- the resolved answer -----------------------------------------------
 *
 * Validation consults the effective type on every staged write, and it does so
 * from the very start of the process: main() validates the file on disk inside
 * venc_config_load() (src/venc_config.c:1029), which runs BEFORE
 * backend_execute() reaches cv610_prepare() -- so the runner context that owns
 * everything else about this run does not exist yet and cannot own this.  The
 * command fallbacks behind the resolution fork, so the answer is resolved once
 * and kept.
 *
 * The cache is therefore one struct holding the answer, written once by
 * effective_resolve_locked() and only read afterwards; the mutex is there for
 * the case where the first reader is the HTTP dispatch thread (handle_modes())
 * rather than config loading.  Keeping the answer in one object also keeps the
 * module's other entry points pure: cv610_sns_resolve_hint() stays a function
 * of its arguments, and nothing outside this file can write the answer except
 * cv610_sns_effective_reset(), which exists for the host tests that flip
 * $SENSOR -- the daemon resolves once per process. */
struct Cv610SnsAnswer {
	pthread_mutex_t lock;
	int             ready;   /* guarded by lock */
	int             status;  /* guarded by lock */
	Cv610SnsType    type;
	Cv610SnsSource  src;
	char            hint[32];
};

static struct Cv610SnsAnswer g_eff = {
	.lock = PTHREAD_MUTEX_INITIALIZER,
	.type = CV610_SNS_FALLBACK,
	.src  = CV610_SRC_DEFAULT,
};

static void effective_resolve_locked(void)
{
	Cv610SnsSource hint_src = CV610_SRC_DEFAULT;
	char hint[sizeof(g_eff.hint)] = "";

	(void)cv610_sns_read_system_hint(hint, sizeof(hint), &hint_src);
	g_eff.status = cv610_sns_resolve_hint(hint, hint_src, &g_eff.type,
		&g_eff.src);
	/* Keep the raw hint even when it was unusable: it is what the warning
	 * has to quote. */
	snprintf(g_eff.hint, sizeof(g_eff.hint), "%s", hint);
}

int cv610_sns_effective(Cv610SnsType *out, Cv610SnsSource *src,
	char *hint, size_t hint_n)
{
	int status;

	pthread_mutex_lock(&g_eff.lock);
	if (!g_eff.ready) {
		effective_resolve_locked();
		g_eff.ready = 1;
	}
	status = g_eff.status;
	if (out != NULL)
		*out = g_eff.type;
	if (src != NULL)
		*src = g_eff.src;
	if (hint != NULL && hint_n > 0)
		snprintf(hint, hint_n, "%s", g_eff.hint);
	pthread_mutex_unlock(&g_eff.lock);
	return status;
}

void cv610_sns_effective_reset(void)
{
	pthread_mutex_lock(&g_eff.lock);
	g_eff.ready = 0;
	pthread_mutex_unlock(&g_eff.lock);
}

const char *cv610_sns_source_name(Cv610SnsSource src)
{
	switch (src) {
	case CV610_SRC_ENV:     return "from SENSOR env";
	case CV610_SRC_UBOOT:   return "from u-boot env";
	case CV610_SRC_IPCINFO: return "from ipcinfo";
	case CV610_SRC_DEFAULT: return "default";
	}
	return "unknown";
}

static const struct Cv610SnsInfo *info_for(Cv610SnsType type)
{
	if (type < 0 || type >= SNS_TYPE_MAX)
		return NULL;
	return &g_sns_info[type];
}

const char *cv610_sns_get_name(Cv610SnsType type)
{
	const struct Cv610SnsInfo *info = info_for(type);

	return info ? info->name : NULL;
}

const char *cv610_sns_get_suffix(Cv610SnsType type)
{
	const struct Cv610SnsInfo *info = info_for(type);

	return info ? info->suffix : NULL;
}

const char *cv610_sns_get_symbol(Cv610SnsType type)
{
	const struct Cv610SnsInfo *info = info_for(type);

	return info ? info->symbol : NULL;
}

int cv610_sns_get_id(Cv610SnsType type)
{
	const struct Cv610SnsInfo *info = info_for(type);

	return info ? info->sns_id : -1;
}

int cv610_sns_write_so_path(Cv610SnsType type, char *buf, size_t n)
{
	const char *suffix = cv610_sns_get_suffix(type);

	if (buf == NULL || suffix == NULL)
		return -1;
	if ((size_t)snprintf(buf, n, "/usr/lib/sensors/libsns_%s.so", suffix) >= n)
		return -1;
	return 0;
}