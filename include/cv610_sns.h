#ifndef CV610_SNS_H
#define CV610_SNS_H

#include <stddef.h>

/* The CV610 backend can stream more than one Sony sensor, and which one is
 * fitted is a fact about the board rather than about the build: the sensor
 * plugin is dlopen'd at bring-up, so one waybeam binary serves any part in the
 * table below without a rebuild.
 *
 * The system already knows the part.  It is the same name the loader uses to
 * decide which sensor modules to insmod and which input clock to feed, it
 * lives in the u-boot env, and rcS exports it as $SENSOR before the daemon
 * starts.  So the backend asks the system (see cv610_sns_read_system_hint())
 * instead of carrying a config field that could disagree with the modules the
 * craft actually loaded, and comes up on the historical IMX662 default only
 * when nothing answers.
 *
 * This header is deliberately free of SDK types so the host-side unit tests
 * (which compile cv610_modes.c) can include it.  The mapping onto the SDK's
 * ot_sensor_id is carried as a plain int and cast at the one place that needs
 * the real type (cv610_pipeline.c). */
typedef enum {
	SNS_TYPE_UNKNOWN = -1,
	SNS_TYPE_IMX662 = 0,   /* 1920x1080, 4 modes */
	SNS_TYPE_IMX335 = 1,   /* 2592x1944 and crops, 5 modes */
	SNS_TYPE_MAX = 2
} Cv610SnsType;

/* Exact-name lookup, case-insensitive.  NULL, "" and an unknown name all
 * return SNS_TYPE_UNKNOWN. */
Cv610SnsType cv610_sns_from_name(const char *name);

/* Where an effective sensor type came from.  cv610_prepare() and the
 * /api/v1/modes handler report this so "why did this craft come up as the
 * default?" is answerable from the camera. */
typedef enum {
	CV610_SRC_ENV = 0,   /* $SENSOR in the daemon's environment */
	CV610_SRC_UBOOT,     /* fw_printenv -n sensor */
	CV610_SRC_IPCINFO,   /* ipcinfo --short-sensor */
	CV610_SRC_DEFAULT    /* historical IMX662 fallback */
} Cv610SnsSource;

/* Read the system's idea of which sensor is fitted: $SENSOR first, then the
 * u-boot env, then ipcinfo.  buf is trimmed and NUL-terminated on success;
 * *src names the source that answered.  Returns 0 when a hint was found, -1
 * otherwise.  fw_printenv/ipcinfo fork, so call this from bring-up only —
 * never per HTTP request (cv610_sns_effective() caches it for the rest). */
int cv610_sns_read_system_hint(char *buf, size_t n, Cv610SnsSource *src);

/* Turn a hint into the type to run.  Pure: the hint is a parameter, so this is
 * unit-testable without forking.
 *
 * Returns 0 when the hint names a part in the table, 1 when there is no hint
 * (the historical default is used), 2 when the hint names a part this binary
 * has no plugin for (the historical default is used, and the caller owes the
 * operator a warning -- refusing to start would leave a craft that the kernel
 * loader is happily running dark).  Never returns SNS_TYPE_UNKNOWN. */
int cv610_sns_resolve_hint(const char *hint, Cv610SnsSource hint_src,
	Cv610SnsType *out, Cv610SnsSource *src);

/* The sensor this process runs, resolved once from the system hint and then
 * cached: validation consults it on every staged write, and the command
 * fallbacks fork.  *out (type), *src (source) and hint (raw hint text, may be
 * NULL) are all optional; the return value is cv610_sns_resolve_hint()'s
 * status.  Never returns SNS_TYPE_UNKNOWN. */
int cv610_sns_effective(Cv610SnsType *out, Cv610SnsSource *src,
	char *hint, size_t hint_n);

/* Drop the cached answer so the next cv610_sns_effective() re-reads the hint.
 * For the host tests, which flip $SENSOR; the daemon resolves once per
 * process. */
void cv610_sns_effective_reset(void);

/* Human-readable source for logs.  Never NULL. */
const char *cv610_sns_source_name(Cv610SnsSource src);

/* Table name ("imx662"/"imx335"), used in logs.  Never NULL for a known type;
 * NULL for SNS_TYPE_UNKNOWN. */
const char *cv610_sns_get_name(Cv610SnsType type);

/* The dlopen'd plugin file's base name (libsns_<suffix>.so). */
const char *cv610_sns_get_suffix(Cv610SnsType type);

/* The exported ot_isp_sns_obj symbol the daemon dlsym()s. */
const char *cv610_sns_get_symbol(Cv610SnsType type);

/* The SDK ot_sensor_id the ISP is bound to, as a plain int (662/335).  -1 for
 * an unknown type. */
int cv610_sns_get_id(Cv610SnsType type);

/* Build "/usr/lib/sensors/libsns_<suffix>.so" into buf.  Returns 0, or -1 on
 * an unknown type or a buf too small. */
int cv610_sns_write_so_path(Cv610SnsType type, char *buf, size_t n);

#endif /* CV610_SNS_H */