//
// Created by scaled
//
// Based on image-source.c from OBS Studio: https://github.com/obsproject/obs-studio
// Also included some code from Spectralizer plugin: https://github.com/univrsal/spectralizer
//

#include <obs-module.h>
#include <graphics/image-file.h>
#include <util/platform.h>
#include <util/dstr.h>
#include <sys/stat.h>
#include <media-io/audio-math.h>
#include <math.h>

#define obs_log(level, format, ...) blog(level, "[image-reaction] " format, ##__VA_ARGS__)
#define IR_PI 3.14159265f
#define IR_BOUNCE_DURATION 0.22f
#define IR_STATE_COUNT 3

struct image_reaction_source {
	obs_source_t *source;
	char source_name[255];

	char *files[IR_STATE_COUNT];
	bool persistent;
	bool linear_alpha;
	bool active;

	gs_image_file_ex_t images[IR_STATE_COUNT];

	obs_weak_source_t *audio_source;

	int state;
	float threshold;
	float loud_threshold;
	float smoothness;
	float average;

	uint64_t hold_time_ns;
	uint64_t last_sound_time;
	uint64_t last_loud_time;

	uint64_t last_time;
	uint64_t capture_check_time;

	bool anim_reset[IR_STATE_COUNT];
	bool anim_reset_trigger;

	float bounce_amount;
	float bounce_timer;
	bool bounce_active;
	bool bounce_trigger;

	float breathing_amount;
	float breathing_speed;
	float breath_phase;
};

#define MAX(a, b) ((a) > (b) ? (a) : (b))

static const char *image_reaction_source_get_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_module_text("ImageReactionSource");
}

static void image_reaction_source_load(struct image_reaction_source *context)
{
	for (int i = 0; i < IR_STATE_COUNT; i++) {
		char *file = context->files[i];
		gs_image_file_ex_t *if4 = &context->images[i];

		obs_enter_graphics();
		gs_image_file_ex_free(if4);
		obs_leave_graphics();

		if (file && *file) {
			obs_log(LOG_DEBUG, "loading texture '%s'", file);
			gs_image_file_ex_init(if4, file,
					      context->linear_alpha ? GS_IMAGE_ALPHA_PREMULTIPLY_SRGB
								    : GS_IMAGE_ALPHA_PREMULTIPLY);

			obs_enter_graphics();
			gs_image_file_ex_init_texture(if4);
			obs_leave_graphics();

			if (!if4->loaded)
				obs_log(LOG_WARNING, "failed to load texture '%s'", file);
		}
	}
}

static void image_reaction_source_unload(struct image_reaction_source *context)
{
	obs_enter_graphics();
	for (int i = 0; i < IR_STATE_COUNT; i++)
		gs_image_file_ex_free(&context->images[i]);
	obs_leave_graphics();
}

static void audio_capture(void *param, obs_source_t *src, const struct audio_data *data, bool muted)
{
	(void)src;
	struct image_reaction_source *context = param;
	uint64_t now = os_gettime_ns();
	int new_state = 0;

	if (muted) {
		context->average = 0.0f;
		context->last_sound_time = 0;
		context->last_loud_time = 0;
	} else {
		uint32_t samples_count = data->frames;
		float *samples = (float *)data->data[0];
		float average_local = 0.0f;

		if (samples_count > 0 && samples != NULL) {
			for (uint32_t i = 0; i < samples_count; i++)
				average_local += fabsf(samples[i]) / (float)samples_count;
		}

		context->average += context->smoothness * (average_local - context->average);

		bool has_file3 = context->files[2] && *context->files[2];
		bool instant_loud = has_file3 && (context->average > context->loud_threshold);
		bool instant_sound = context->average > context->threshold;

		if (instant_loud) {
			context->last_loud_time = now;
			context->last_sound_time = now;
		} else if (instant_sound) {
			context->last_sound_time = now;
		}

		bool hold_loud = has_file3 && context->hold_time_ns > 0 && context->last_loud_time > 0 &&
				 (now - context->last_loud_time <= context->hold_time_ns);
		bool hold_sound = context->hold_time_ns > 0 && context->last_sound_time > 0 &&
				  (now - context->last_sound_time <= context->hold_time_ns);

		if (instant_loud || hold_loud)
			new_state = 2;
		else if (instant_sound || hold_sound)
			new_state = 1;
	}

	if (new_state != context->state) {
		if (new_state > context->state)
			context->bounce_trigger = true;
		context->state = new_state;
		context->anim_reset_trigger = true;
	}
}

static void image_reaction_source_update(void *data, obs_data_t *settings)
{
	struct image_reaction_source *context = data;
	const char *file_keys[IR_STATE_COUNT] = {"file1", "file2", "file3"};
	const char *reset_keys[IR_STATE_COUNT] = {"anim_reset_1", "anim_reset_2", "anim_reset_3"};

	for (int i = 0; i < IR_STATE_COUNT; i++) {
		const char *file = obs_data_get_string(settings, file_keys[i]);
		if (context->files[i])
			bfree(context->files[i]);
		context->files[i] = bstrdup(file);
		context->anim_reset[i] = obs_data_get_bool(settings, reset_keys[i]);
	}

	const bool unload = obs_data_get_bool(settings, "unload");
	const bool linear_alpha = obs_data_get_bool(settings, "linear_alpha");
	const float threshold = (float)obs_data_get_double(settings, "threshold");
	const float loud_threshold = (float)obs_data_get_double(settings, "loud_threshold");
	const float smoothness = (float)obs_data_get_double(settings, "smoothness");
	const double hold_time_ms = obs_data_get_double(settings, "hold_time_ms");

	context->persistent = !unload;
	context->linear_alpha = linear_alpha;
	context->threshold = db_to_mul(threshold);
	context->loud_threshold = db_to_mul(loud_threshold);
	context->smoothness = powf(0.1f, smoothness);
	context->hold_time_ns = hold_time_ms > 0.0 ? (uint64_t)(hold_time_ms * 1000000.0) : 0;

	context->bounce_amount = (float)obs_data_get_double(settings, "bounce_amount");
	context->breathing_amount = (float)obs_data_get_double(settings, "breathing_amount");
	context->breathing_speed = (float)obs_data_get_double(settings, "breathing_speed");

	/* Load the image if the source is persistent or showing */
	if (context->persistent || obs_source_showing(context->source))
		image_reaction_source_load(data);
	else
		image_reaction_source_unload(data);

	const char *cfg_source_name = obs_data_get_string(settings, "audio_source");

	obs_weak_source_t *old = NULL;

	if (cfg_source_name[0] == '\0') {
		if (context->audio_source) {
			old = context->audio_source;
			context->audio_source = NULL;
		}
		context->source_name[0] = '\0';
	} else {
		if (context->source_name[0] == '\0' || strcmp(context->source_name, cfg_source_name) != 0) {
			if (context->audio_source) {
				old = context->audio_source;
				context->audio_source = NULL;
			}
			snprintf(context->source_name, sizeof(context->source_name), "%s", cfg_source_name);
			context->capture_check_time = os_gettime_ns() - 3000000000ULL;
		}
	}

	if (old) {
		obs_source_t *old_source = obs_weak_source_get_source(old);
		if (old_source) {
			obs_log(LOG_INFO, "Removed audio capture from '%s'", obs_source_get_name(old_source));
			obs_source_remove_audio_capture_callback(old_source, audio_capture, context);
			obs_source_release(old_source);
		}
		obs_weak_source_release(old);
	}
}

static void image_reaction_source_defaults(obs_data_t *settings)
{
	obs_data_set_default_bool(settings, "unload", false);
	obs_data_set_default_bool(settings, "linear_alpha", false);
	obs_data_set_default_string(settings, "audio_source", "");
	obs_data_set_default_double(settings, "threshold", -40.0);
	obs_data_set_default_double(settings, "loud_threshold", -15.0);
	obs_data_set_default_double(settings, "smoothness", 1.0);
	obs_data_set_default_double(settings, "hold_time_ms", 150.0);
	obs_data_set_default_double(settings, "bounce_amount", 12.0);
	obs_data_set_default_double(settings, "breathing_amount", 4.0);
	obs_data_set_default_double(settings, "breathing_speed", 1.5);
}

static void image_reaction_source_show(void *data)
{
	struct image_reaction_source *context = data;

	if (!context->persistent)
		image_reaction_source_load(context);
}

static void image_reaction_source_hide(void *data)
{
	struct image_reaction_source *context = data;

	if (!context->persistent)
		image_reaction_source_unload(context);
}

static void *image_reaction_source_create(obs_data_t *settings, obs_source_t *source)
{
	struct image_reaction_source *context = bzalloc(sizeof(struct image_reaction_source));
	context->source = source;

	context->source_name[0] = '\0';
	context->state = 0;

	image_reaction_source_update(context, settings);
	return context;
}

static void image_reaction_source_destroy(void *data)
{
	struct image_reaction_source *context = data;

	image_reaction_source_unload(context);

	for (int i = 0; i < IR_STATE_COUNT; i++) {
		if (context->files[i])
			bfree(context->files[i]);
	}

	if (context->audio_source) {
		obs_source_t *source = obs_weak_source_get_source(context->audio_source);
		if (source) {
			obs_log(LOG_INFO, "Removed audio capture from '%s'", obs_source_get_name(source));
			obs_source_remove_audio_capture_callback(source, audio_capture, context);
			obs_source_release(source);
		}
		obs_weak_source_release(context->audio_source);
	}

	bfree(context);
}

static uint32_t image_reaction_source_getwidth(void *data)
{
	struct image_reaction_source *context = data;
	uint32_t w = MAX(context->images[0].cx, context->images[1].cx);
	return MAX(w, context->images[2].cx);
}

static uint32_t image_reaction_source_getheight(void *data)
{
	struct image_reaction_source *context = data;
	uint32_t h = MAX(context->images[0].cy, context->images[1].cy);
	return MAX(h, context->images[2].cy);
}

static gs_image_file_ex_t *get_active_image(struct image_reaction_source *context)
{
	int idx = context->state;
	if (idx < 0 || idx >= IR_STATE_COUNT)
		idx = 0;

	if (context->images[idx].texture)
		return &context->images[idx];
	if (idx == 2 && context->images[1].texture)
		return &context->images[1];
	return &context->images[0];
}

static void image_reaction_source_render(void *data, gs_effect_t *effect)
{
	struct image_reaction_source *context = data;

	const bool previous = gs_framebuffer_srgb_enabled();
	gs_enable_framebuffer_srgb(true);
	gs_blend_state_push();
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);

	gs_image_file_ex_t *if4 = get_active_image(context);
	if (if4->texture) {
		float bounce_offset = 0.0f;
		if (context->bounce_active && context->bounce_amount > 0.0f) {
			float progress = context->bounce_timer / IR_BOUNCE_DURATION;
			if (progress >= 0.0f && progress <= 1.0f)
				bounce_offset = sinf(progress * IR_PI) * context->bounce_amount;
		}

		float breath_offset = sinf(context->breath_phase) * context->breathing_amount;
		float offset_y = breath_offset - bounce_offset;

		gs_matrix_push();
		if (offset_y != 0.0f)
			gs_matrix_translate3f(0.0f, offset_y, 0.0f);

		gs_eparam_t *const param = gs_effect_get_param_by_name(effect, "image");
		gs_effect_set_texture_srgb(param, if4->texture);

		gs_draw_sprite(if4->texture, 0, if4->cx, if4->cy);
		gs_matrix_pop();
	}

	gs_blend_state_pop();

	gs_enable_framebuffer_srgb(previous);
}

static void image_reaction_tick(void *data, float seconds)
{
	struct image_reaction_source *context = data;

	/* Update natural breathing & bounce animation */
	if (context->breathing_amount > 0.0f) {
		context->breath_phase += seconds * context->breathing_speed * 2.0f;
		if (context->breath_phase > 2.0f * IR_PI)
			context->breath_phase -= 2.0f * IR_PI;
	} else {
		context->breath_phase = 0.0f;
	}

	if (context->bounce_trigger) {
		context->bounce_trigger = false;
		if (context->bounce_amount > 0.0f) {
			context->bounce_active = true;
			context->bounce_timer = 0.0f;
		}
	}

	if (context->bounce_active) {
		context->bounce_timer += seconds;
		if (context->bounce_timer >= IR_BOUNCE_DURATION) {
			context->bounce_active = false;
			context->bounce_timer = 0.0f;
		}
	}

	/* Update / refresh audio capturing */
	char *new_name = NULL;
	if (context->source_name[0] != '\0' && !context->audio_source) {
		uint64_t t = os_gettime_ns();

		if (t - context->capture_check_time > 3000000000ULL) {
			new_name = context->source_name;
			context->capture_check_time = t;
		}
	}

	if (new_name != NULL) {
		obs_source_t *capture = obs_get_source_by_name(new_name);
		obs_weak_source_t *weak_capture = capture ? obs_source_get_weak_source(capture) : NULL;

		if (context->source_name[0] != '\0' && new_name == context->source_name) {
			context->audio_source = weak_capture;
			weak_capture = NULL;
		}

		if (capture) {
			obs_log(LOG_INFO, "Added audio capture to '%s'", obs_source_get_name(capture));
			obs_source_add_audio_capture_callback(capture, audio_capture, context);
			obs_weak_source_release(weak_capture);
			obs_source_release(capture);
		}
	}

	/* Update GIFs */
	uint64_t frame_time = obs_get_video_frame_time();
	if (obs_source_active(context->source)) {
		if (!context->active) {
			for (int i = 0; i < IR_STATE_COUNT; i++) {
				if (context->images[i].is_animated_gif) {
					context->last_time = frame_time;
					break;
				}
			}
			context->active = true;
		}
	} else {
		if (context->active) {
			for (int i = 0; i < IR_STATE_COUNT; i++) {
				gs_image_file_ex_t *if4 = &context->images[i];
				if (if4->is_animated_gif) {
					if4->cur_frame = 0;
					if4->cur_loop = 0;
					if4->cur_time = 0;

					obs_enter_graphics();
					gs_image_file_ex_update_texture(if4);
					obs_leave_graphics();
				}
			}

			context->active = false;
		}
	}

	for (int i = 0; i < IR_STATE_COUNT; i++) {
		gs_image_file_ex_t *if4 = &context->images[i];
		bool anim_reset = context->anim_reset[i];

		if (context->last_time && if4->is_animated_gif) {
			if (anim_reset && context->anim_reset_trigger) {
				if4->cur_frame = 0;
				if4->cur_loop = 0;
				if4->cur_time = 0;

				obs_enter_graphics();
				gs_image_file_ex_update_texture(if4);
				obs_leave_graphics();
			} else {
				uint64_t elapsed = frame_time - context->last_time;
				bool updated = gs_image_file_ex_tick(if4, elapsed);

				if (updated) {
					obs_enter_graphics();
					gs_image_file_ex_update_texture(if4);
					obs_leave_graphics();
				}
			}
		}
	}
	context->anim_reset_trigger = false;

	context->last_time = frame_time;
}

static const char *image_filter = "All formats (*.bmp *.tga *.png *.jpeg *.jpg *.gif *.psd *.webp);;"
				  "BMP Files (*.bmp);;"
				  "Targa Files (*.tga);;"
				  "PNG Files (*.png);;"
				  "JPEG Files (*.jpeg *.jpg);;"
				  "GIF Files (*.gif);;"
				  "PSD Files (*.psd);;"
				  "WebP Files (*.webp);;"
				  "All Files (*.*)";

static bool add_source(void *param, obs_source_t *src)
{
	obs_property_t *list = param;

	uint32_t caps = obs_source_get_output_flags(src);

	if ((caps & OBS_SOURCE_AUDIO) == 0)
		return true;
	const char *name = obs_source_get_name(src);
	obs_property_list_add_string(list, name, name);
	return true;
}

static obs_properties_t *image_reaction_source_properties(void *data)
{
	struct image_reaction_source *s = data;
	struct dstr path = {0};

	obs_properties_t *props = obs_properties_create();

	if (s && s->files[0] && *s->files[0]) {
		const char *slash;

		dstr_copy(&path, s->files[0]);
		dstr_replace(&path, "\\", "/");
		slash = strrchr(path.array, '/');
		if (slash)
			dstr_resize(&path, slash - path.array + 1);
	}

	obs_properties_add_path(props, "file1", obs_module_text("Reaction1"), OBS_PATH_FILE, image_filter, path.array);
	obs_properties_add_bool(props, "anim_reset_1", obs_module_text("AnimReset1"));
	obs_properties_add_path(props, "file2", obs_module_text("Reaction2"), OBS_PATH_FILE, image_filter, path.array);
	obs_properties_add_bool(props, "anim_reset_2", obs_module_text("AnimReset2"));
	obs_properties_add_path(props, "file3", obs_module_text("Reaction3"), OBS_PATH_FILE, image_filter, path.array);
	obs_properties_add_bool(props, "anim_reset_3", obs_module_text("AnimReset3"));
	dstr_free(&path);

	obs_properties_add_bool(props, "unload", obs_module_text("UnloadWhenNotShowing"));
	obs_properties_add_bool(props, "linear_alpha", obs_module_text("LinearAlpha"));
	obs_property_t *sources_list = obs_properties_add_list(props, "audio_source", obs_module_text("AudioSource"),
							       OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(sources_list, "", "");

	obs_property_t *p =
		obs_properties_add_float_slider(props, "threshold", obs_module_text("Threshold"), -60.0, 0.0, 0.1);
	obs_property_float_set_suffix(p, " dB");

	p = obs_properties_add_float_slider(props, "loud_threshold", obs_module_text("LoudThreshold"), -60.0, 0.0, 0.1);
	obs_property_float_set_suffix(p, " dB");

	obs_properties_add_float_slider(props, "smoothness", obs_module_text("Smoothness"), 0.0, 5.0, 0.1);

	p = obs_properties_add_float_slider(props, "hold_time_ms", obs_module_text("HoldTime"), 0.0, 2000.0, 10.0);
	obs_property_float_set_suffix(p, " ms");

	p = obs_properties_add_float_slider(props, "bounce_amount", obs_module_text("BounceAmount"), 0.0, 100.0, 1.0);
	obs_property_float_set_suffix(p, " px");

	p = obs_properties_add_float_slider(props, "breathing_amount", obs_module_text("BreathingAmount"), 0.0, 50.0,
					    0.5);
	obs_property_float_set_suffix(p, " px");

	obs_properties_add_float_slider(props, "breathing_speed", obs_module_text("BreathingSpeed"), 0.1, 5.0, 0.1);

	obs_enum_sources(add_source, sources_list);

	return props;
}

uint64_t image_reaction_source_get_memory_usage(void *data)
{
	struct image_reaction_source *s = data;
	uint64_t total = 0;
	for (int i = 0; i < IR_STATE_COUNT; i++)
		total += s->images[i].mem_usage;
	return total;
}

static struct obs_source_info image_reaction_source_info = {
	.id = "image_reaction_source",
	.type = OBS_SOURCE_TYPE_INPUT,
	.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_SRGB,
	.get_name = image_reaction_source_get_name,
	.create = image_reaction_source_create,
	.destroy = image_reaction_source_destroy,
	.update = image_reaction_source_update,
	.get_defaults = image_reaction_source_defaults,
	.show = image_reaction_source_show,
	.hide = image_reaction_source_hide,
	.get_width = image_reaction_source_getwidth,
	.get_height = image_reaction_source_getheight,
	.video_render = image_reaction_source_render,
	.video_tick = image_reaction_tick,
	.get_properties = image_reaction_source_properties,
	.icon_type = OBS_ICON_TYPE_IMAGE,
};

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("image-reaction", "en-US")
MODULE_EXPORT const char *obs_module_description(void)
{
	return "Image reaction source";
}

bool obs_module_load(void)
{
	obs_register_source(&image_reaction_source_info);
	return true;
}
