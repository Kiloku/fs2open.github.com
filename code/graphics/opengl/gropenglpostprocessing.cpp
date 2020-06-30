
#include "gropenglpostprocessing.h"

#include "ShaderProgram.h"
#include "SmaaAreaTex.h"
#include "SmaaSearchTex.h"
#include "freespace.h"
#include "gropengl.h"
#include "gropengldraw.h"
#include "gropenglshader.h"
#include "gropenglstate.h"

#include "cmdline/cmdline.h"
#include "def_files/def_files.h"
#include "graphics/util/uniform_structs.h"
#include "io/timer.h"
#include "lighting/lighting.h"
#include "mod_table/mod_table.h"
#include "nebula/neb.h"
#include "parse/parselo.h"
#include "ship/ship.h"
#include "tracing/tracing.h"

extern bool PostProcessing_override;
extern int opengl_check_framebuffer();

// Needed to track where the FXAA shaders are
// In case we don't find the shaders at all, this override is needed
bool fxaa_unavailable = false;
bool zbuffer_saved    = false;

// lightshaft parameters
bool ls_on           = false;
bool ls_force_off    = false;
float ls_density     = 0.5f;
float ls_weight      = 0.02f;
float ls_falloff     = 1.0f;
float ls_intensity   = 0.5f;
float ls_cpintensity = 0.5f * 50 * 0.02f;
int ls_samplenum     = 50;

const int MAX_MIP_BLUR_LEVELS = 4;

enum class PostEffectUniformType {
	Invalid,
	NoiseAmount,
	Saturation,
	Brightness,
	Contrast,
	FilmGrain,
	TvStripes,
	Cutoff,
	Tint,
	Dither,
};

struct post_effect_t {
	SCP_string name;
	PostEffectUniformType uniform_type = PostEffectUniformType::Invalid;
	SCP_string define_name;

	float intensity{0.0f};
	float default_intensity{0.0f};
	float div{1.0f};
	float add{0.0f};

	vec3d rgb = vmd_zero_vector;

	bool always_on{false};
};

SCP_vector<post_effect_t> Post_effects;

static int Post_initialized = 0;

bool Post_in_frame = false;

static int Post_active_shader_index = -1;

static GLuint Bloom_framebuffer = 0;
static GLuint Bloom_textures[2] = { 0 };

static GLuint Post_framebuffer_id[2] = { 0 };

static int Post_texture_width = 0;
static int Post_texture_height = 0;

static GLuint Smaa_edge_detection_fb = 0;
static GLuint Smaa_edges_tex         = 0;

static GLuint Smaa_blending_weight_fb = 0;
static GLuint Smaa_blend_tex          = 0;

static GLuint Smaa_neighborhood_blending_fb = 0;
static GLuint Smaa_output_tex               = 0;

static GLuint Smaa_search_tex = 0;
static GLuint Smaa_area_tex   = 0;

void opengl_post_pass_tonemap()
{
	GR_DEBUG_SCOPE("Tonemapping");
	TRACE_SCOPE(tracing::Tonemapping);

	opengl_shader_set_current(gr_opengl_maybe_create_shader(SDR_TYPE_POST_PROCESS_TONEMAPPING, 0));

	Current_shader->program->Uniforms.setTextureUniform("tex", 0);

	opengl_set_generic_uniform_data<graphics::generic_data::tonemapping_data>(
		[](graphics::generic_data::tonemapping_data* data) { data->exposure = 4.0f; });

	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, Scene_ldr_texture, 0);

	GL_state.Texture.Enable(0, GL_TEXTURE_2D, Scene_color_texture);

	opengl_draw_full_screen_textured(0.0f, 0.0f, Scene_texture_u_scale, Scene_texture_u_scale);
}

void opengl_post_pass_bloom()
{
	GR_DEBUG_SCOPE("Bloom");
	TRACE_SCOPE(tracing::Bloom);

	// we need the scissor test disabled
	GLboolean scissor_test = GL_state.ScissorTest(GL_FALSE);

	// ------  begin bright pass ------
	int width, height;
	{
		GR_DEBUG_SCOPE("Bloom bright pass");
		TRACE_SCOPE(tracing::BloomBrightPass);

		GL_state.BindFrameBuffer(Bloom_framebuffer);
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, Bloom_textures[0], 0);

		// width and height are 1/2 for the bright pass
		width = Post_texture_width >> 1;
		height = Post_texture_height >> 1;

		glViewport(0, 0, width, height);

		glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
		glClear(GL_COLOR_BUFFER_BIT);

		opengl_shader_set_current(gr_opengl_maybe_create_shader(SDR_TYPE_POST_PROCESS_BRIGHTPASS, 0));

		Current_shader->program->Uniforms.setTextureUniform("tex", 0);

		GL_state.Texture.Enable(0, GL_TEXTURE_2D, Scene_color_texture);

		opengl_draw_full_screen_textured(0.0f, 0.0f, 1.0f, 1.0f);
	}
	// ------ end bright pass ------

	// ------ begin blur pass ------

	GL_state.Texture.Enable(0, GL_TEXTURE_2D, Bloom_textures[0]);

	glGenerateMipmap(GL_TEXTURE_2D);

	for ( int iteration = 0; iteration < 2; iteration++) {
		for (int pass = 0; pass < 2; pass++) {
			GR_DEBUG_SCOPE("Bloom iteration step");
			TRACE_SCOPE(tracing::BloomIterationStep);

			GLuint source_tex = Bloom_textures[pass];
			GLuint dest_tex = Bloom_textures[1 - pass];

			if (pass) {
				opengl_shader_set_current(gr_opengl_maybe_create_shader(SDR_TYPE_POST_PROCESS_BLUR, SDR_FLAG_BLUR_HORIZONTAL));
			} else {
				opengl_shader_set_current(gr_opengl_maybe_create_shader(SDR_TYPE_POST_PROCESS_BLUR, SDR_FLAG_BLUR_VERTICAL));
			}

			Current_shader->program->Uniforms.setTextureUniform("tex", 0);

			GL_state.Texture.Enable(0, GL_TEXTURE_2D, source_tex);

			for (int mipmap = 0; mipmap < MAX_MIP_BLUR_LEVELS; ++mipmap) {
				int bloom_width  = width >> mipmap;
				int bloom_height = height >> mipmap;

				opengl_set_generic_uniform_data<graphics::generic_data::blur_data>(
					[&](graphics::generic_data::blur_data* data) {
						data->texSize = (pass) ? 1.0f / i2fl(bloom_width) : 1.0f / i2fl(bloom_height);
						data->level   = mipmap;
					});

				glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, dest_tex, mipmap);

				glViewport(0, 0, bloom_width, bloom_height);

				opengl_draw_full_screen_textured(0.0f, 0.0f, 1.0f, 1.0f);
			}
		}
	}

	// composite blur to the color texture
	{
		GR_DEBUG_SCOPE("Bloom composite step");
		TRACE_SCOPE(tracing::BloomCompositeStep);

		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, Scene_color_texture, 0);

		opengl_shader_set_current(gr_opengl_maybe_create_shader(SDR_TYPE_POST_PROCESS_BLOOM_COMP, 0));

		Current_shader->program->Uniforms.setTextureUniform("tex", 0);

		opengl_set_generic_uniform_data<graphics::generic_data::bloom_composition_data>(
			[](graphics::generic_data::bloom_composition_data* data) {
				data->levels          = MAX_MIP_BLUR_LEVELS;
				data->bloom_intensity = Cmdline_bloom_intensity / 100.0f;
			});

		GL_state.Texture.Enable(0, GL_TEXTURE_2D, Bloom_textures[0]);

		GL_state.SetAlphaBlendMode(ALPHA_BLEND_ADDITIVE);

		glViewport(0, 0, gr_screen.max_w, gr_screen.max_h);

		opengl_draw_full_screen_textured(0.0f, 0.0f, 1.0f, 1.0f);

		GL_state.SetAlphaBlendMode(ALPHA_BLEND_NONE);
	}

	// ------ end blur pass --------

	// reset viewport, scissor test and exit
	GL_state.ScissorTest(scissor_test);
}

void gr_opengl_post_process_begin()
{
	if ( !Post_initialized ) {
		return;
	}

	if (Post_in_frame) {
		return;
	}

	if (PostProcessing_override) {
		return;
	}

	GL_state.PushFramebufferState();
	GL_state.BindFrameBuffer(Post_framebuffer_id[0]);

	glDrawBuffer(GL_COLOR_ATTACHMENT0);

	glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

	Post_in_frame = true;
}

SCP_vector<shader_type> get_aa_shader_types(AntiAliasMode aa_mode) {
	auto stypes = SCP_vector<shader_type>();

	if (gr_is_fxaa_mode(aa_mode)) {
		stypes.push_back(shader_type::SDR_TYPE_POST_PROCESS_FXAA);
	}
	else if (gr_is_smaa_mode(aa_mode)) {
		stypes.push_back(shader_type::SDR_TYPE_POST_PROCESS_SMAA_BLENDING_WEIGHT);
		stypes.push_back(shader_type::SDR_TYPE_POST_PROCESS_SMAA_EDGE);
		stypes.push_back(shader_type::SDR_TYPE_POST_PROCESS_SMAA_NEIGHBORHOOD_BLENDING);
	}

	return stypes;
}

void recompile_aa_shader() {
	mprintf(("Recompiling AA shader(s)...\n"));

	for (auto sdr : get_aa_shader_types(Gr_aa_mode_last_frame)) {
		opengl_delete_shader( gr_opengl_maybe_create_shader(sdr, 0) );
	}

	for (auto sdr : get_aa_shader_types(Gr_aa_mode)) {
		gr_opengl_maybe_create_shader(sdr, 0);
	}

	Gr_aa_mode_last_frame = Gr_aa_mode;
}

void opengl_post_pass_fxaa()
{
	GR_DEBUG_SCOPE("FXAA");
	TRACE_SCOPE(tracing::FXAA);

	// If the preset changed, recompile the shader
	if (Gr_aa_mode_last_frame != Gr_aa_mode) {
		recompile_aa_shader();
	}

	// We only want to draw to ATTACHMENT0
	glDrawBuffer(GL_COLOR_ATTACHMENT0);
	GL_state.ColorMask(true, true, true, true);

	// Do a prepass to convert the main shaders' RGBA output into RGBL
	opengl_shader_set_current(gr_opengl_maybe_create_shader(SDR_TYPE_POST_PROCESS_FXAA_PREPASS, 0));

	// basic/default uniforms
	Current_shader->program->Uniforms.setTextureUniform("tex", 0);

	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, Scene_luminance_texture, 0);

	GL_state.Texture.Enable(0, GL_TEXTURE_2D, Scene_ldr_texture);

	opengl_draw_full_screen_textured(0.0f, 0.0f, Scene_texture_u_scale, Scene_texture_u_scale);

	// set and configure post shader ..
	opengl_shader_set_current(gr_opengl_maybe_create_shader(SDR_TYPE_POST_PROCESS_FXAA, 0));

	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, Scene_ldr_texture, 0);

	// basic/default uniforms
	Current_shader->program->Uniforms.setTextureUniform("tex0", 0);

	opengl_set_generic_uniform_data<graphics::generic_data::fxaa_data>([](graphics::generic_data::fxaa_data* data) {
		data->rt_w = i2fl(Post_texture_width);
		data->rt_h = i2fl(Post_texture_height);
	});

	GL_state.Texture.Enable(0, GL_TEXTURE_2D, Scene_luminance_texture);

	opengl_draw_full_screen_textured(0.0f, 0.0f, Scene_texture_u_scale, Scene_texture_u_scale);

	opengl_shader_set_current();
}

static void smaa_detect_edges()
{
	GR_DEBUG_SCOPE("SMAA Detect Edges");
	TRACE_SCOPE(tracing::SMAAEdgeDetection);

	GL_state.BindFrameBuffer(Smaa_edge_detection_fb);

	GLfloat clearValues[] = {0.0f, 0.0f, 0.0f, 1.0f};
	glClearBufferfv(GL_COLOR, 0, clearValues);

	// Do the edge detection step
	opengl_shader_set_current(gr_opengl_maybe_create_shader(SDR_TYPE_POST_PROCESS_SMAA_EDGE, 0));

	// basic/default uniforms
	Current_shader->program->Uniforms.setTextureUniform("colorTex", 0);

	GL_state.Texture.Enable(0, GL_TEXTURE_2D, Scene_ldr_texture);

	opengl_draw_full_screen_textured(0.0f, 0.0f, Scene_texture_u_scale, Scene_texture_u_scale);
}

static void smaa_calculate_blending_weights()
{
	GR_DEBUG_SCOPE("SMAA Blending Weights calculation");
	TRACE_SCOPE(tracing::SMAACalculateBlendingWeights);

	GL_state.BindFrameBuffer(Smaa_blending_weight_fb);

	GLfloat clearValues[] = {0.0f, 0.0f, 0.0f, 1.0f};
	glClearBufferfv(GL_COLOR, 0, clearValues);

	// Do the edge detection step
	opengl_shader_set_current(gr_opengl_maybe_create_shader(SDR_TYPE_POST_PROCESS_SMAA_BLENDING_WEIGHT, 0));

	// basic/default uniforms
	Current_shader->program->Uniforms.setTextureUniform("edgesTex", 0);
	Current_shader->program->Uniforms.setTextureUniform("areaTex", 1);
	Current_shader->program->Uniforms.setTextureUniform("searchTex", 2);

	GL_state.Texture.Enable(0, GL_TEXTURE_2D, Smaa_edges_tex);
	GL_state.Texture.Enable(1, GL_TEXTURE_2D, Smaa_area_tex);
	GL_state.Texture.Enable(2, GL_TEXTURE_2D, Smaa_search_tex);

	opengl_draw_full_screen_textured(0.0f, 0.0f, Scene_texture_u_scale, Scene_texture_u_scale);
}

static void smaa_neighborhood_blending()
{
	GR_DEBUG_SCOPE("SMAA Neighborhood Blending");
	TRACE_SCOPE(tracing::SMAANeighborhoodBlending);

	GL_state.BindFrameBuffer(Smaa_neighborhood_blending_fb);

	GLfloat clearValues[] = {0.0f, 0.0f, 0.0f, 1.0f};
	glClearBufferfv(GL_COLOR, 0, clearValues);

	// Do the edge detection step
	opengl_shader_set_current(gr_opengl_maybe_create_shader(SDR_TYPE_POST_PROCESS_SMAA_NEIGHBORHOOD_BLENDING, 0));

	// basic/default uniforms
	Current_shader->program->Uniforms.setTextureUniform("colorTex", 0);
	Current_shader->program->Uniforms.setTextureUniform("blendTex", 1);

	GL_state.Texture.Enable(0, GL_TEXTURE_2D, Scene_ldr_texture);
	GL_state.Texture.Enable(1, GL_TEXTURE_2D, Smaa_blend_tex);

	opengl_draw_full_screen_textured(0.0f, 0.0f, Scene_texture_u_scale, Scene_texture_u_scale);
}

void smaa_resolve()
{
	GR_DEBUG_SCOPE("SMAA Resolve");
	TRACE_SCOPE(tracing::SMAAResolve);

	opengl_shader_set_passthrough(true, false);
	GL_state.Texture.Enable(0, GL_TEXTURE_2D, Smaa_output_tex);

	// Copy SMAA output back to the original framebuffer
	if (GL_rendering_to_texture) {
		opengl_draw_textured_quad(0.0f, 0.0f, 0.0f, 0.0f, (float)gr_screen.max_w, (float)gr_screen.max_h,
		                          Scene_texture_u_scale, Scene_texture_v_scale);
	} else {
		opengl_draw_textured_quad(0.0f, 0.0f, 0.0f, Scene_texture_v_scale, (float)gr_screen.max_w,
		                          (float)gr_screen.max_h, Scene_texture_u_scale, 0.0f);
	}
}

void opengl_post_pass_smaa()
{
	GR_DEBUG_SCOPE("SMAA");
	TRACE_SCOPE(tracing::SMAA);

		// If the preset changed, recompile the shader
	if (Gr_aa_mode_last_frame != Gr_aa_mode) {
		recompile_aa_shader();
	}

	GL_state.PushFramebufferState();

	GL_state.ColorMask(true, true, true, true);

	// All SMAA stages share the same shader data so we only need this once
	opengl_set_generic_uniform_data<graphics::generic_data::smaa_data>([](graphics::generic_data::smaa_data* data) {
		data->smaa_rt_metrics.x = i2fl(Post_texture_width);
		data->smaa_rt_metrics.y = i2fl(Post_texture_height);
	});

	smaa_detect_edges();

	smaa_calculate_blending_weights();

	smaa_neighborhood_blending();

	GL_state.PopFramebufferState();

	smaa_resolve();
}

extern GLuint Shadow_map_depth_texture;
extern GLuint Scene_depth_texture;
extern GLuint Cockpit_depth_texture;
extern GLuint Scene_position_texture;
extern GLuint Scene_normal_texture;
extern GLuint Scene_specular_texture;
extern bool stars_sun_has_glare(int index);
extern float Sun_spot;
void opengl_post_lightshafts()
{
	GR_DEBUG_SCOPE("Lightshafts");
	TRACE_SCOPE(tracing::Lightshafts);

	opengl_shader_set_current(gr_opengl_maybe_create_shader(SDR_TYPE_POST_PROCESS_LIGHTSHAFTS, 0));

	float x, y;

	// should we even be here?
	if ( !Game_subspace_effect && ls_on && !ls_force_off ) {
		int n_lights = light_get_global_count();

		for ( int idx = 0; idx<n_lights; idx++ ) {
			vec3d light_dir;
			vec3d local_light_dir;
			light_get_global_dir(&light_dir, idx);
			vm_vec_rotate(&local_light_dir, &light_dir, &Eye_matrix);

			if ( !stars_sun_has_glare(idx) ) {
				continue;
			}

			float dot;
			if ( (dot = vm_vec_dot(&light_dir, &Eye_matrix.vec.fvec)) > 0.7f ) {

				x = asinf(vm_vec_dot(&light_dir, &Eye_matrix.vec.rvec)) / PI * 1.5f +
					0.5f; // cant get the coordinates right but this works for the limited glare fov
				y = asinf(vm_vec_dot(&light_dir, &Eye_matrix.vec.uvec)) / PI * 1.5f * gr_screen.clip_aspect + 0.5f;

				opengl_set_generic_uniform_data<graphics::generic_data::lightshaft_data>(
					[&](graphics::generic_data::lightshaft_data* data) {
						data->sun_pos.x = x;
						data->sun_pos.y = y;

						data->density      = ls_density;
						data->falloff      = ls_falloff;
						data->weight       = ls_weight;
						data->intensity    = Sun_spot * ls_intensity;
						data->cp_intensity = Sun_spot * ls_cpintensity;
					});

				Current_shader->program->Uniforms.setTextureUniform("scene", 0);
				Current_shader->program->Uniforms.setTextureUniform("cockpit", 1);

				GL_state.Texture.Enable(0, GL_TEXTURE_2D, Scene_depth_texture);
				GL_state.Texture.Enable(1, GL_TEXTURE_2D, Cockpit_depth_texture);
				GL_state.Blend(GL_TRUE);
				GL_state.SetAlphaBlendMode(ALPHA_BLEND_ADDITIVE);

				opengl_draw_full_screen_textured(0.0f, 0.0f, Scene_texture_u_scale, Scene_texture_u_scale);

				GL_state.Blend(GL_FALSE);
				break;
			}
		}
	}
}

void gr_opengl_post_process_end()
{
	GR_DEBUG_SCOPE("Draw scene texture");
	TRACE_SCOPE(tracing::DrawSceneTexture);

	// state switch just the once (for bloom pass and final render-to-screen)
	GLboolean depth = GL_state.DepthTest(GL_FALSE);
	GLboolean depth_mask = GL_state.DepthMask(GL_FALSE);
	GLboolean blend = GL_state.Blend(GL_FALSE);
	GLboolean cull = GL_state.CullFace(GL_FALSE);

	GL_state.Texture.SetShaderMode(GL_TRUE);

	GL_state.PushFramebufferState();

	// do bloom, hopefully ;)
	opengl_post_pass_bloom();

	// do tone mapping
	opengl_post_pass_tonemap();

	// Do Post processing AA
	if (!GL_rendering_to_texture) {
		if (gr_is_smaa_mode(Gr_aa_mode)) {
			opengl_post_pass_smaa();
		} else if (gr_is_fxaa_mode(Gr_aa_mode) && !fxaa_unavailable) {
			opengl_post_pass_fxaa();
		}
	}

	// render lightshafts
	opengl_post_lightshafts();

	GR_DEBUG_SCOPE("Draw post effects");
	TRACE_SCOPE(tracing::DrawPostEffects);

	// now write to the previous buffer
	GL_state.PopFramebufferState();

	glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
	glClear(GL_COLOR_BUFFER_BIT);

	// set and configure post shader ...
	int flags = 0;
	for ( int i = 0; i < (int)Post_effects.size(); i++) {
		if (Post_effects[i].always_on) {
			flags |= (1 << i);
		}
	}

	int post_sdr_handle = Post_active_shader_index;

	if (post_sdr_handle < 0) {
		// no active shader index? use the always on shader.
		post_sdr_handle = gr_opengl_maybe_create_shader(SDR_TYPE_POST_PROCESS_MAIN, flags);
	}

	opengl_shader_set_current(post_sdr_handle);

	// basic/default uniforms
	Current_shader->program->Uniforms.setTextureUniform("tex", 0);
	GL_state.Texture.Enable(0, GL_TEXTURE_2D, Scene_ldr_texture);

	Current_shader->program->Uniforms.setTextureUniform("depth_tex", 1);
	GL_state.Texture.Enable(1, GL_TEXTURE_2D, Scene_depth_texture);

	opengl_set_generic_uniform_data<graphics::generic_data::post_data>([&](graphics::generic_data::post_data* data) {
		data->timer = i2fl(timer_get_milliseconds() % 100 + 1);

		for (size_t idx = 0; idx < Post_effects.size(); idx++) {
			if (GL_shader[post_sdr_handle].flags & (1 << idx)) {
				float value = Post_effects[idx].intensity;

				switch (Post_effects[idx].uniform_type) {
				case PostEffectUniformType::Invalid:
					// Invalid name specified, do nothing
					break;
				case PostEffectUniformType::NoiseAmount:
					data->noise_amount = value;
					break;
				case PostEffectUniformType::Saturation:
					data->saturation = value;
					break;
				case PostEffectUniformType::Brightness:
					data->brightness = value;
					break;
				case PostEffectUniformType::Contrast:
					data->contrast = value;
					break;
				case PostEffectUniformType::FilmGrain:
					data->film_grain = value;
					break;
				case PostEffectUniformType::TvStripes:
					data->tv_stripes = value;
					break;
				case PostEffectUniformType::Cutoff:
					data->cutoff = value;
					break;
				case PostEffectUniformType::Dither:
					data->dither = value;
					break;
				case PostEffectUniformType::Tint:
					data->tint = Post_effects[idx].rgb;
					break;
				}
			}
		}
	});

	// now render it to the screen ...
	GL_state.PopFramebufferState();

	opengl_draw_full_screen_textured(0.0f, 0.0f, Scene_texture_u_scale, Scene_texture_u_scale);

	//Shadow Map debug window
//#define SHADOW_DEBUG
#ifdef SHADOW_DEBUG
	opengl_shader_set_current( &GL_post_shader[8] );
	GL_state.Texture.SetActiveUnit(0);
//	GL_state.Texture.SetTarget(GL_TEXTURE_2D);
	GL_state.Texture.SetTarget(GL_TEXTURE_2D_ARRAY);
//	GL_state.Texture.Enable(Shadow_map_depth_texture);
	extern GLuint Shadow_map_texture;
	extern GLuint Post_shadow_texture_id;
	GL_state.Texture.Enable(Shadow_map_texture);
	glUniform1iARB( opengl_shader_get_uniform("shadow_map"), 0);
	glUniform1iARB( opengl_shader_get_uniform("index"), 0);
	//opengl_draw_textured_quad(-1.0f, -1.0f, 0.0f, 0.0f, -0.5f, -0.5f, Scene_texture_u_scale, Scene_texture_u_scale);
	//opengl_draw_textured_quad(-1.0f, -1.0f, 0.0f, 0.0f, -0.5f, -0.5f, 0.5f, 0.5f);
	opengl_draw_textured_quad(-1.0f, -1.0f, 0.0f, 0.0f, -0.5f, -0.5f, 1.0f, 1.0f);
	glUniform1iARB( opengl_shader_get_uniform("index"), 1);
	//opengl_draw_textured_quad(-1.0f, -0.5f, 0.5f, 0.0f, -0.5f, 0.0f, 0.75f, 0.25f);
	opengl_draw_textured_quad(-1.0f, -0.5f, 0.0f, 0.0f, -0.5f, 0.0f, 1.0f, 1.0f);
	glUniform1iARB( opengl_shader_get_uniform("index"), 2);
	opengl_draw_textured_quad(-0.5f, -1.0f, 0.0f, 0.0f, 0.0f, -0.5f, 1.0f, 1.0f);
	glUniform1iARB( opengl_shader_get_uniform("index"), 3);
	opengl_draw_textured_quad(-0.5f, -1.0f, 0.0f, 0.0f, 0.0f, -0.5f, 1.0f, 1.0f);
	opengl_shader_set_current();
#endif

	/*GL_state.Texture.SetActiveUnit(0);
	GL_state.Texture.SetTarget(GL_TEXTURE_2D);
	GL_state.Texture.Enable(Scene_depth_texture);

	
	*/
	// Done!
	/*GL_state.Texture.SetActiveUnit(0);
	GL_state.Texture.SetTarget(GL_TEXTURE_2D);
	GL_state.Texture.Enable(Scene_effect_texture);

	opengl_draw_textured_quad(0.0f, -1.0f, 0.0f, 0.0f, 1.0f, 0.0f, Scene_texture_u_scale, Scene_texture_u_scale);

	GL_state.Texture.SetActiveUnit(0);
	GL_state.Texture.SetTarget(GL_TEXTURE_2D);
	GL_state.Texture.Enable(Scene_normal_texture);

	opengl_draw_textured_quad(-1.0f, -0.0f, 0.0f, 0.0f, 0.0f, 1.0f, Scene_texture_u_scale, Scene_texture_u_scale);

	GL_state.Texture.SetActiveUnit(0);
	GL_state.Texture.SetTarget(GL_TEXTURE_2D);
	GL_state.Texture.Enable(Scene_specular_texture);

	opengl_draw_textured_quad(0.0f, -0.0f, 0.0f, 0.0f, 1.0f, 1.0f, Scene_texture_u_scale, Scene_texture_u_scale);
	*/

	GL_state.Texture.SetShaderMode(GL_FALSE);

	// reset state
	GL_state.DepthTest(depth);
	GL_state.DepthMask(depth_mask);
	GL_state.Blend(blend);
	GL_state.CullFace(cull);

	opengl_shader_set_current();

	Post_in_frame = false;
}

void get_post_process_effect_names(SCP_vector<SCP_string> &names)
{
	size_t idx;

	for (idx = 0; idx < Post_effects.size(); idx++) {
		names.push_back(Post_effects[idx].name);
	}
}

void gr_opengl_post_process_set_effect(const char *name, int value, const vec3d *rgb)
{
	if ( !Post_initialized ) {
		return;
	}

	if (name == NULL) {
		return;
	}

	size_t idx;
	int sflags = 0;

	if(!stricmp("lightshafts",name))
	{
		ls_intensity = value / 100.0f;
		ls_on = !!value;
		return;
	}

	for (idx = 0; idx < Post_effects.size(); idx++) {
		const char *eff_name = Post_effects[idx].name.c_str();

		if ( !stricmp(eff_name, name) ) {
			Post_effects[idx].intensity = (value / Post_effects[idx].div) + Post_effects[idx].add;
			if ((rgb != nullptr) && !(vmd_zero_vector == *rgb)) {
				Post_effects[idx].rgb = *rgb;
			}
			break;
		}
	}

	// figure out new flags
	for (idx = 0; idx < Post_effects.size(); idx++) {
		if ( Post_effects[idx].always_on || (Post_effects[idx].intensity != Post_effects[idx].default_intensity) ) {
			sflags |= (1<<idx);
		}
	}

	Post_active_shader_index = gr_opengl_maybe_create_shader(SDR_TYPE_POST_PROCESS_MAIN, sflags);
}

void gr_opengl_post_process_set_defaults()
{
	size_t idx;

	if ( !Post_initialized ) {
		return;
	}

	// reset all effects to their default values
	for (idx = 0; idx < Post_effects.size(); idx++) {
		Post_effects[idx].intensity = Post_effects[idx].default_intensity;
	}

	Post_active_shader_index = -1;
}

extern GLuint Cockpit_depth_texture;
void gr_opengl_post_process_save_zbuffer()
{
	GR_DEBUG_SCOPE("Save z-Buffer");
	if (Post_initialized)
	{
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, Cockpit_depth_texture, 0);
		gr_zbuffer_clear(TRUE);
		zbuffer_saved = true;
	}
	else
	{
		// If we can't save the z-buffer then just clear it so cockpits are still rendered correctly when
		// post-processing isn't available/enabled.
		gr_zbuffer_clear(TRUE);
	}
}
void gr_opengl_post_process_restore_zbuffer()
{
	GR_DEBUG_SCOPE("Restore z-Buffer");

	if (zbuffer_saved) {
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, Scene_depth_texture, 0);

		zbuffer_saved = false;
	}
}

static PostEffectUniformType mapUniformNameToType(const SCP_string& uniform_name)
{
	if (!stricmp(uniform_name.c_str(), "noise_amount")) {
		return PostEffectUniformType::NoiseAmount;
	} else if (!stricmp(uniform_name.c_str(), "saturation")) {
		return PostEffectUniformType::Saturation;
	} else if (!stricmp(uniform_name.c_str(), "brightness")) {
		return PostEffectUniformType::Brightness;
	} else if (!stricmp(uniform_name.c_str(), "contrast")) {
		return PostEffectUniformType::Contrast;
	} else if (!stricmp(uniform_name.c_str(), "film_grain")) {
		return PostEffectUniformType::FilmGrain;
	} else if (!stricmp(uniform_name.c_str(), "tv_stripes")) {
		return PostEffectUniformType::TvStripes;
	} else if (!stricmp(uniform_name.c_str(), "cutoff")) {
		return PostEffectUniformType::Cutoff;
	} else if (!stricmp(uniform_name.c_str(), "dither")) {
		return PostEffectUniformType::Dither;
	} else if (!stricmp(uniform_name.c_str(), "tint")) {
		return PostEffectUniformType::Tint;
	} else {
		error_display(0, "Unknown uniform name '%s'!", uniform_name.c_str());
		return PostEffectUniformType::Invalid;
	}
}

static bool opengl_post_init_table()
{
	bool warned = false;

	try
	{
		if (cf_exists_full("post_processing.tbl", CF_TYPE_TABLES))
			read_file_text("post_processing.tbl", CF_TYPE_TABLES);
		else
			read_file_text_from_default(defaults_get_file("post_processing.tbl"));

		reset_parse();


		if (optional_string("#Effects")) {
			while (!required_string_one_of(3, "$Name:", "#Ship Effects", "#End")) {
				post_effect_t eff;

				required_string("$Name:");
				stuff_string(eff.name, F_NAME);

				required_string("$Uniform:");

				SCP_string tbuf;
				stuff_string(tbuf, F_NAME);
				eff.uniform_type = mapUniformNameToType(tbuf);

				required_string("$Define:");
				stuff_string(eff.define_name, F_NAME);

				required_string("$AlwaysOn:");
				stuff_boolean(&eff.always_on);

				required_string("$Default:");
				stuff_float(&eff.default_intensity);
				eff.intensity = eff.default_intensity;

				required_string("$Div:");
				stuff_float(&eff.div);

				required_string("$Add:");
				stuff_float(&eff.add);

				if (optional_string("$RGB:")) {
					stuff_vec3d(&eff.rgb);
				}

				// Post_effects index is used for flag checks, so we can't have more than 32
				if (Post_effects.size() < 32) {
					Post_effects.push_back(eff);
				}
				else if (!warned) {
					mprintf(("WARNING: post_processing.tbl can only have a max of 32 effects! Ignoring extra...\n"));
					warned = true;
				}
			}
		}

		//Built-in per-ship effects
		ship_effect se1;
		strcpy_s(se1.name, "FS1 Ship select");
		se1.shader_effect = 0;
		se1.disables_rendering = false;
		se1.invert_timer = false;
		Ship_effects.push_back(se1);

		if (optional_string("#Ship Effects")) {
			while (!required_string_one_of(3, "$Name:", "#Light Shafts", "#End")) {
				ship_effect se;
				char tbuf[NAME_LENGTH] = { 0 };

				required_string("$Name:");
				stuff_string(tbuf, F_NAME, NAME_LENGTH);
				strcpy_s(se.name, tbuf);

				required_string("$Shader Effect:");
				stuff_int(&se.shader_effect);

				required_string("$Disables Rendering:");
				stuff_boolean(&se.disables_rendering);

				required_string("$Invert timer:");
				stuff_boolean(&se.invert_timer);

				Ship_effects.push_back(se);
			}
		}

		if (optional_string("#Light Shafts")) {
			required_string("$AlwaysOn:");
			stuff_boolean(&ls_on);
			required_string("$Density:");
			stuff_float(&ls_density);
			required_string("$Falloff:");
			stuff_float(&ls_falloff);
			required_string("$Weight:");
			stuff_float(&ls_weight);
			required_string("$Intensity:");
			stuff_float(&ls_intensity);
			required_string("$Sample Number:");
			stuff_int(&ls_samplenum);

			ls_cpintensity = ls_weight;
			for (int i = 1; i < ls_samplenum; i++)
				ls_cpintensity += ls_weight * pow(ls_falloff, i);
			ls_cpintensity *= ls_intensity;
		}

		required_string("#End");

		return true;
	}
	catch (const parse::ParseException& e)
	{
		mprintf(("Unable to parse 'post_processing.tbl'!  Error message = %s.\n", e.what()));
		return false;
	}
}

static void set_fxaa_defines(SCP_stringstream& sflags)
{
	// Since we require OpenGL 3.2 we always have support for GLSL 130
	sflags << "#define FXAA_GLSL_120 0\n";
	sflags << "#define FXAA_GLSL_130 1\n";

	if (GLSL_version >= 400) {
		// The gather function became part of the standard with GLSL 4.00
		sflags << "#define FXAA_GATHER4_ALPHA 1\n";
	}

	switch (Gr_aa_mode) {
	case AntiAliasMode::None:
		sflags << "#define FXAA_QUALITY_PRESET 10\n";
		sflags << "#define FXAA_QUALITY_EDGE_THRESHOLD (1.0/6.0)\n";
		sflags << "#define FXAA_QUALITY_EDGE_THRESHOLD_MIN (1.0/12.0)\n";
		sflags << "#define FXAA_QUALITY_SUBPIX 0.33\n";
		break;
	case AntiAliasMode::FXAA_Low:
		sflags << "#define FXAA_QUALITY_PRESET 12\n";
		sflags << "#define FXAA_QUALITY_EDGE_THRESHOLD (1.0/8.0)\n";
		sflags << "#define FXAA_QUALITY_EDGE_THRESHOLD_MIN (1.0/16.0)\n";
		sflags << "#define FXAA_QUALITY_SUBPIX 0.33\n";
		break;
	case AntiAliasMode::FXAA_Medium:
		sflags << "#define FXAA_QUALITY_PRESET 26\n";
		sflags << "#define FXAA_QUALITY_EDGE_THRESHOLD (1.0/12.0)\n";
		sflags << "#define FXAA_QUALITY_EDGE_THRESHOLD_MIN (1.0/24.0)\n";
		sflags << "#define FXAA_QUALITY_SUBPIX 0.33\n";
		break;
	case AntiAliasMode::FXAA_High:
		sflags << "#define FXAA_QUALITY_PRESET 39\n";
		sflags << "#define FXAA_QUALITY_EDGE_THRESHOLD (1.0/15.0)\n";
		sflags << "#define FXAA_QUALITY_EDGE_THRESHOLD_MIN (1.0/32.0)\n";
		sflags << "#define FXAA_QUALITY_SUBPIX 0.33\n";
		break;
	default:
		UNREACHABLE("Unhandled FXAA mode!");
	}
}
void set_smaa_defines(SCP_stringstream& sflags)
{
	// Define what GLSL version we use
	if (GLSL_version >= 400) {
		sflags << "#define SMAA_GLSL_4\n";
	} else {
		sflags << "#define SMAA_GLSL_3\n";
	}

	switch (Gr_aa_mode) {
	case AntiAliasMode::SMAA_Low:
		sflags << "#define SMAA_PRESET_LOW\n";
		break;
	case AntiAliasMode::SMAA_Medium:
		sflags << "#define SMAA_PRESET_MEDIUM\n";
		break;
	case AntiAliasMode::SMAA_High:
		sflags << "#define SMAA_PRESET_HIGH\n";
		break;
	case AntiAliasMode::SMAA_Ultra:
		sflags << "#define SMAA_PRESET_ULTRA\n";
		break;
	default:
		UNREACHABLE("Unhandled SMAA mode!");
	}
}
void opengl_post_shader_header(SCP_stringstream& sflags, shader_type shader_t, int flags)
{
	if (shader_t == SDR_TYPE_POST_PROCESS_MAIN) {
		for (size_t idx = 0; idx < Post_effects.size(); idx++) {
			if (flags & (1 << idx)) {
				sflags << "#define ";
				sflags << Post_effects[idx].define_name.c_str();
				sflags << "\n";
			}
		}
	} else if (shader_t == SDR_TYPE_POST_PROCESS_LIGHTSHAFTS) {
		char temp[64];
		sprintf(temp, "#define SAMPLE_NUM %d\n", ls_samplenum);
		sflags << temp;
	} else if (shader_t == SDR_TYPE_POST_PROCESS_FXAA) {
		set_fxaa_defines(sflags);
	} else if (shader_t == SDR_TYPE_POST_PROCESS_SMAA_EDGE || shader_t == SDR_TYPE_POST_PROCESS_SMAA_BLENDING_WEIGHT ||
	           shader_t == SDR_TYPE_POST_PROCESS_SMAA_NEIGHBORHOOD_BLENDING) {
		set_smaa_defines(sflags);
	}
}

bool opengl_post_init_shaders()
{
	int idx;
	int flags = 0;

	// figure out which flags we need for the main post process shader
	for (idx = 0; idx < (int)Post_effects.size(); idx++) {
		if (Post_effects[idx].always_on) {
			flags |= (1 << idx);
		}
	}

	if ( gr_opengl_maybe_create_shader(SDR_TYPE_POST_PROCESS_MAIN, flags) < 0 ) {
		// only the main shader is actually required for post-processing
		return false;
	}

	if ( gr_opengl_maybe_create_shader(SDR_TYPE_POST_PROCESS_BRIGHTPASS, 0) < 0 ||
		gr_opengl_maybe_create_shader(SDR_TYPE_POST_PROCESS_BLUR, SDR_FLAG_BLUR_HORIZONTAL) < 0 ||
		gr_opengl_maybe_create_shader(SDR_TYPE_POST_PROCESS_BLUR, SDR_FLAG_BLUR_VERTICAL) < 0 ||
		gr_opengl_maybe_create_shader(SDR_TYPE_POST_PROCESS_BLOOM_COMP, 0) < 0) {
		// disable bloom if we don't have those shaders available
		Cmdline_bloom_intensity = 0;
	}

	if (gr_is_fxaa_mode(Gr_aa_mode))
	{
		gr_opengl_maybe_create_shader(SDR_TYPE_POST_PROCESS_FXAA, 0);
		gr_opengl_maybe_create_shader(SDR_TYPE_POST_PROCESS_FXAA_PREPASS, 0);
	}

	if (gr_is_smaa_mode(Gr_aa_mode)) {
		// Precompile the SMAA shaders if enabled
		gr_opengl_maybe_create_shader(SDR_TYPE_POST_PROCESS_SMAA_EDGE, 0);
		gr_opengl_maybe_create_shader(SDR_TYPE_POST_PROCESS_SMAA_BLENDING_WEIGHT, 0);
		gr_opengl_maybe_create_shader(SDR_TYPE_POST_PROCESS_SMAA_NEIGHBORHOOD_BLENDING, 0);
	}

	return true;
}

void opengl_setup_bloom_textures()
{
	// two more framebuffers, one each for the two different sized bloom textures
	glGenFramebuffers(1, &Bloom_framebuffer);

	// need to generate textures for bloom too
	glGenTextures(2, Bloom_textures);

	// half size
	int width = Post_texture_width >> 1;
	int height = Post_texture_height >> 1;

	for (int tex = 0; tex < 2; tex++) {
		GL_state.Texture.SetActiveUnit(0);
		GL_state.Texture.SetTarget(GL_TEXTURE_2D);
		GL_state.Texture.Enable(Bloom_textures[tex]);

		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, width, height, 0, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, NULL);

		glGenerateMipmap(GL_TEXTURE_2D);

		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, MAX_MIP_BLUR_LEVELS-1);
	}

	GL_state.BindFrameBuffer(0);
}

void setup_smaa_edges_resources()
{
	glGenTextures(1, &Smaa_edges_tex);

	GL_state.Texture.SetActiveUnit(0);
	GL_state.Texture.SetTarget(GL_TEXTURE_2D);
	GL_state.Texture.Enable(Smaa_edges_tex);

	opengl_set_object_label(GL_TEXTURE, Smaa_edges_tex, "SMAA Edge detection texture");

	if (GLAD_GL_ARB_texture_storage) {
		glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, Post_texture_width, Post_texture_height);
	} else {
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, Post_texture_width, Post_texture_height, 0, GL_BGRA, GL_UNSIGNED_BYTE,
		             nullptr);
	}
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);

	glGenFramebuffers(1, &Smaa_edge_detection_fb);
	GL_state.BindFrameBuffer(Smaa_edge_detection_fb);
	opengl_set_object_label(GL_FRAMEBUFFER, Smaa_edge_detection_fb, "SMAA Edge detection framebuffer");

	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, Smaa_edges_tex, 0);

	glDrawBuffer(GL_COLOR_ATTACHMENT0);
}

void setup_smaa_blending_weight_resources()
{
	glGenTextures(1, &Smaa_blend_tex);

	GL_state.Texture.SetActiveUnit(0);
	GL_state.Texture.SetTarget(GL_TEXTURE_2D);
	GL_state.Texture.Enable(Smaa_blend_tex);

	opengl_set_object_label(GL_TEXTURE, Smaa_blend_tex, "SMAA Blending weight calculation texture");

	if (GLAD_GL_ARB_texture_storage) {
		glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, Post_texture_width, Post_texture_height);
	} else {
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, Post_texture_width, Post_texture_height, 0, GL_BGRA, GL_UNSIGNED_BYTE,
		             nullptr);
	}
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);

	glGenFramebuffers(1, &Smaa_blending_weight_fb);
	GL_state.BindFrameBuffer(Smaa_blending_weight_fb);
	opengl_set_object_label(GL_FRAMEBUFFER, Smaa_blending_weight_fb, "SMAA Blending widght calculation framebuffer");

	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, Smaa_blend_tex, 0);

	glDrawBuffer(GL_COLOR_ATTACHMENT0);
}

void setup_smaa_neighborhood_blending_resources()
{
	glGenTextures(1, &Smaa_output_tex);

	GL_state.Texture.SetActiveUnit(0);
	GL_state.Texture.SetTarget(GL_TEXTURE_2D);
	GL_state.Texture.Enable(Smaa_output_tex);

	opengl_set_object_label(GL_TEXTURE, Smaa_output_tex, "SMAA output texture");

	if (GLAD_GL_ARB_texture_storage) {
		glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, Post_texture_width, Post_texture_height);
	} else {
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, Post_texture_width, Post_texture_height, 0, GL_BGRA, GL_UNSIGNED_BYTE,
		             nullptr);
	}
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);

	glGenFramebuffers(1, &Smaa_neighborhood_blending_fb);
	GL_state.BindFrameBuffer(Smaa_neighborhood_blending_fb);
	opengl_set_object_label(GL_FRAMEBUFFER, Smaa_neighborhood_blending_fb, "SMAA neighborhood blending framebuffer");

	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, Smaa_output_tex, 0);

	glDrawBuffer(GL_COLOR_ATTACHMENT0);
}

static GLuint load_smaa_texture(GLsizei width, GLsizei height, GLenum format, const uint8_t* pixels, const char* name)
{
	GLuint tex;
	glGenTextures(1, &tex);

	GL_state.Texture.SetActiveUnit(0);
	GL_state.Texture.SetTarget(GL_TEXTURE_2D);
	GL_state.Texture.Enable(tex);

	opengl_set_object_label(GL_TEXTURE, tex, name);

	if (GLAD_GL_ARB_texture_storage) {
		glTexStorage2D(GL_TEXTURE_2D, 1, format, width, height);
	} else {
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
		glTexImage2D(GL_TEXTURE_2D, 0, format, Post_texture_width, Post_texture_height, 0, GL_BGRA, GL_UNSIGNED_BYTE,
		             nullptr);
	}
	glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height, format == GL_RG8 ? GL_RG : GL_RED, GL_UNSIGNED_BYTE, pixels);

	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);

	return tex;
}

static void setup_smaa_resources()
{
	GL_state.PushFramebufferState();

	Smaa_area_tex = load_smaa_texture(AREATEX_WIDTH, AREATEX_HEIGHT, GL_RG8, areaTexBytes, "SMAA Area Texture");
	Smaa_search_tex =
	    load_smaa_texture(SEARCHTEX_WIDTH, SEARCHTEX_HEIGHT, GL_R8, searchTexBytes, "SMAA Search Texture");

	setup_smaa_edges_resources();

	setup_smaa_blending_weight_resources();

	setup_smaa_neighborhood_blending_resources();

	GL_state.PopFramebufferState();
}

// generate and test the framebuffer and textures that we are going to use
static bool opengl_post_init_framebuffer()
{
	bool rval = false;

	// clamp size, if needed
	Post_texture_width = gr_screen.max_w;
	Post_texture_height = gr_screen.max_h;

	if (Post_texture_width > GL_max_renderbuffer_size) {
		Post_texture_width = GL_max_renderbuffer_size;
	}

	if (Post_texture_height > GL_max_renderbuffer_size) {
		Post_texture_height = GL_max_renderbuffer_size;
	}

	opengl_setup_bloom_textures();

	if (Gr_aa_mode != AntiAliasMode::None) {
		setup_smaa_resources();
	}

	GL_state.BindFrameBuffer(0);

	rval = true;

	if ( opengl_check_for_errors("post_init_framebuffer()") ) {
		rval = false;
	}

	return rval;
}



void opengl_post_process_shutdown_bloom()
{
	if ( Bloom_textures[0] ) {
		glDeleteTextures(1, &Bloom_textures[0]);
		Bloom_textures[0] = 0;
	}

	if ( Bloom_textures[1] ) {
		glDeleteTextures(1, &Bloom_textures[1]);
		Bloom_textures[1] = 0;
	}

	if ( Bloom_framebuffer > 0 ) {
		glDeleteFramebuffers(1, &Bloom_framebuffer);
		Bloom_framebuffer = 0;
	}
}

void opengl_post_process_init()
{
	Post_initialized = 0;

	//We need to read the tbl first. This is mostly for FRED's benefit, as otherwise the list of post effects for the sexp doesn't get updated.
	if ( !opengl_post_init_table() ) {
		mprintf(("  Unable to read post-processing table! Disabling post-processing...\n\n"));
		Gr_post_processing_enabled = false;
		return;
	}

	if ( !Gr_post_processing_enabled ) {
		return;
	}

	if ( !Scene_texture_initialized ) {
		return;
	}

	if ( Cmdline_no_fbo ) {
		Gr_post_processing_enabled = false;
		return;
	}

	if ( !opengl_post_init_shaders() ) {
		mprintf(("  Unable to initialize post-processing shaders! Disabling post-processing...\n\n"));
		Gr_post_processing_enabled = false;
		return;
	}

	if ( !opengl_post_init_framebuffer() ) {
		mprintf(("  Unable to initialize post-processing framebuffer! Disabling post-processing...\n\n"));
		Gr_post_processing_enabled = false;
		return;
	}

	Post_initialized = 1;
}

void opengl_post_process_shutdown()
{
	if ( !Post_initialized ) {
		return;
	}

	if (Post_framebuffer_id[0]) {
		glDeleteFramebuffers(1, &Post_framebuffer_id[0]);
		Post_framebuffer_id[0] = 0;

		if (Post_framebuffer_id[1]) {
			glDeleteFramebuffers(1, &Post_framebuffer_id[1]);
			Post_framebuffer_id[1] = 0;
		}
	}

	Post_effects.clear();

	opengl_post_process_shutdown_bloom();

	Post_in_frame = false;
	Post_active_shader_index = 0;

	Post_initialized = 0;
}
