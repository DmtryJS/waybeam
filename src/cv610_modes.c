#include "cv610_modes.h"

/* The mode tables are the single source of truth for capture geometry, frame
 * rate, MIPI RAW bit depth and the sensor clock a mode's line timing assumes.
 * Everything derives from them: cv610_validate_config() rejects anything that
 * does not resolve, cv610_prepare() takes the bit depth and clock from the
 * resolved mode, and /api/v1/modes is generated from them.
 *
 * The table index is what a config's sensor.mode names, and what
 * /api/v1/modes lists; the sensor plugin never sees it.  isp_setup() publishes
 * pub.sns_mode = 0 ("auto") and the plugin's cmos_isp_set_image_mode() picks
 * its own img_mode from the geometry and rate the ISP hands it: IMX662 keys on
 * the rate alone (all of its modes are 1080p), IMX335 on an exact width/height
 * match plus a rate inside that mode's min..max, with a non-zero sns_mode
 * accepted as an explicit hint that nothing publishes today.  So a table row
 * is load-bearing only through what it publishes as geometry and rate: two
 * rows of one part must not be confusable by the plugin (same geometry is fine
 * only when the rates separate them), and the row order is kept aligned with
 * the plugin's *_MODE enum so that plumbing the index into pub.sns_mode stays
 * a one-line change -- a flexible window crop, which has no fixed geometry to
 * match on, would need exactly that.  Only fixed modes are listed; the
 * plugin's img_mode 5 is deliberately absent until that bring-up is wired
 * through.
 */

/* ---- IMX662: 1920x1080, four rates --------------------------------------
 *
 * All four entries are hardware-verified on the IMX662 bring-up board.
 *
 * The clock column is the one that bites.  The sensor keeps whatever line
 * timing it was programmed with regardless of the MCLK it is actually fed,
 * so a mismatch is silent: frames simply arrive at fps * actual / expected
 * while /fps/live and /api/v1/modes both still report the nominal rate.
 * Measured 43.6 fps for the 60 fps mode when it ran on the 100 fps mode's
 * 27 MHz clock.  Only 100 fps takes the sensor's 24 MHz INCK profile
 * overclocked to 27 MHz (INCK_SEL 0x04); the rest run 37.125 MHz. */
static const Cv610SensorMode g_imx662_modes[] = {
	{ 1920, 1080,  30, 12, 37125000u, "1080p30 RAW12" },
	{ 1920, 1080,  60, 12, 37125000u, "1080p60 RAW12" },
	{ 1920, 1080,  90, 10, 37125000u, "1080p90 RAW10" },
	{ 1920, 1080, 100, 10, 27000000u, "1080p100 RAW10" },
};

static const Cv610SensorMode *table_for(Cv610SnsType type, size_t *count)
{
	switch (type) {
	case SNS_TYPE_IMX662:
	default:
		/* Unknown falls back to IMX662: a caller that resolved its config
		 * has already rejected an unknown name, and returning NULL here
		 * would turn a handled error into a crash. */
		if (count)
			*count = sizeof(g_imx662_modes) / sizeof(g_imx662_modes[0]);
		return g_imx662_modes;
	}
}

const Cv610SensorMode *cv610_mode_table(Cv610SnsType type, size_t *count)
{
	return table_for(type, count);
}

/* VPSS scales, it does not invent detail: upscaling past the captured size
 * costs encoder bandwidth for no information.  Refuse it rather than let a
 * config quietly waste the link. */
#define CV610_OUT_ALIGN 8u
#define CV610_OUT_MIN   128u

const Cv610SensorMode *cv610_mode_for_fps(Cv610SnsType type, uint32_t fps)
{
	const Cv610SensorMode *table;
	size_t count = 0;
	size_t i;

	table = table_for(type, &count);
	for (i = 0; i < count; ++i) {
		if (table[i].fps == fps)
			return &table[i];
	}
	return NULL;
}

const Cv610SensorMode *cv610_mode_select(Cv610SnsType type, int forced_mode,
	uint32_t target_fps, int *out_index)
{
	const Cv610SensorMode *table;
	const Cv610SensorMode *pick;
	size_t count = 0;
	size_t pick_i = 0;
	size_t i;

	if (out_index)
		*out_index = -1;

	table = table_for(type, &count);

	/* A forced index is the whole answer: it exists or the config is wrong.
	 * Parity with find_best_mode(), which fails bring-up rather than
	 * quietly running a mode the operator did not ask for. */
	if (forced_mode >= 0) {
		if ((size_t)forced_mode >= count)
			return NULL;
		if (out_index)
			*out_index = forced_mode;
		return &table[forced_mode];
	}
	/* Nothing to select against.  Rejecting keeps the behaviour this
	 * function replaced — a bare cv610_mode_for_fps(0) returned NULL too —
	 * rather than inventing a rate for a config that names none. */
	if (target_fps == 0)
		return NULL;

	pick = cv610_mode_for_fps(type, target_fps);
	if (pick != NULL) {
		if (out_index)
			*out_index = (int)(pick - table);
		return pick;
	}

	/* Slowest mode still faster than the target: never deliver fewer frames
	 * than asked for while a mode that can keep up exists. */
	for (i = 0; i < count; ++i) {
		const Cv610SensorMode *m = &table[i];

		if (m->fps > target_fps && (pick == NULL || m->fps < pick->fps)) {
			pick = m;
			pick_i = i;
		}
	}
	/* Target above every mode: clamp to the fastest. */
	if (pick == NULL) {
		for (i = 0; i < count; ++i) {
			if (pick == NULL || table[i].fps > pick->fps) {
				pick = &table[i];
				pick_i = i;
			}
		}
	}
	if (out_index)
		*out_index = (int)pick_i;
	return pick;
}

const char *cv610_mode_check_output(const Cv610SensorMode *mode,
	uint32_t width, uint32_t height)
{
	if (mode == NULL)
		return "no sensor mode selected";
	/* Only a fully unset geometry means "the mode's own size".  A half-set
	 * one (1920x0) is a typo, not a request, and must not be completed into
	 * something the caller did not ask for. */
	if (width == 0 && height == 0)
		return NULL;
	if (width == 0 || height == 0)
		return "CV610 video0.size needs both a width and a height";
	if (width % CV610_OUT_ALIGN != 0 || height % CV610_OUT_ALIGN != 0)
		return "CV610 video0.size must be a multiple of 8";
	if (width < CV610_OUT_MIN || height < CV610_OUT_MIN)
		return "CV610 video0.size must be at least 128x128";
	if (width > mode->width || height > mode->height)
		return "CV610 video0.size cannot exceed the sensor mode's capture size";
	return NULL;
}

void cv610_mode_resolve_output(const Cv610SensorMode *mode,
	uint32_t req_width, uint32_t req_height,
	uint32_t *out_width, uint32_t *out_height)
{
	uint32_t w = 0;
	uint32_t h = 0;

	if (mode != NULL) {
		int given = (req_width != 0 && req_height != 0);

		w = given ? req_width : mode->width;
		h = given ? req_height : mode->height;
	}
	if (out_width)
		*out_width = w;
	if (out_height)
		*out_height = h;
}
