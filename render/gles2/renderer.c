#include <assert.h>
#include <drm_fourcc.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <wayland-server-protocol.h>
#include <wayland-util.h>
#include <wlr/render/egl.h>
#include <wlr/render/interface.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_matrix.h>
#include <wlr/types/wlr_linux_dmabuf_v1.h>
#include <wlr/util/log.h>
#include "render/gles2.h"
#include "render/pixel_format.h"

static const GLfloat verts[] = {
	1, 0, // top right
	0, 0, // top left
	1, 1, // bottom right
	0, 1, // bottom left
};

static const struct wlr_renderer_impl renderer_impl;

struct wlr_gles2_renderer *gles2_get_renderer(
		struct wlr_renderer *wlr_renderer) {
	assert(wlr_renderer->impl == &renderer_impl);
	return (struct wlr_gles2_renderer *)wlr_renderer;
}

static struct wlr_gles2_renderer *gles2_get_renderer_in_context(
		struct wlr_renderer *wlr_renderer) {
	struct wlr_gles2_renderer *renderer = gles2_get_renderer(wlr_renderer);
	assert(wlr_egl_is_current(renderer->egl));
	assert(renderer->current_buffer != NULL);
	return renderer;
}

static void destroy_buffer(struct wlr_gles2_buffer *buffer) {
	wl_list_remove(&buffer->link);
	wl_list_remove(&buffer->buffer_destroy.link);

	struct wlr_egl_context prev_ctx;
	wlr_egl_save_context(&prev_ctx);
	wlr_egl_make_current(buffer->renderer->egl);

	push_gles2_debug(buffer->renderer);

	glDeleteFramebuffers(1, &buffer->fbo);
	glDeleteRenderbuffers(1, &buffer->rbo);

	pop_gles2_debug(buffer->renderer);

	wlr_egl_destroy_image(buffer->renderer->egl, buffer->image);

	wlr_egl_restore_context(&prev_ctx);

	free(buffer);
}

static struct wlr_gles2_buffer *get_buffer(struct wlr_gles2_renderer *renderer,
		struct wlr_buffer *wlr_buffer) {
	struct wlr_gles2_buffer *buffer;
	wl_list_for_each(buffer, &renderer->buffers, link) {
		if (buffer->buffer == wlr_buffer) {
			return buffer;
		}
	}
	return NULL;
}

static void handle_buffer_destroy(struct wl_listener *listener, void *data) {
	struct wlr_gles2_buffer *buffer =
		wl_container_of(listener, buffer, buffer_destroy);
	destroy_buffer(buffer);
}

static struct wlr_gles2_buffer *create_buffer(struct wlr_gles2_renderer *renderer,
		struct wlr_buffer *wlr_buffer) {
	struct wlr_gles2_buffer *buffer = calloc(1, sizeof(*buffer));
	if (buffer == NULL) {
		wlr_log_errno(WLR_ERROR, "Allocation failed");
		return NULL;
	}
	buffer->buffer = wlr_buffer;
	buffer->renderer = renderer;

	struct wlr_dmabuf_attributes dmabuf = {0};
	if (!wlr_buffer_get_dmabuf(wlr_buffer, &dmabuf)) {
		goto error_buffer;
	}

	bool external_only;
	buffer->image = wlr_egl_create_image_from_dmabuf(renderer->egl,
		&dmabuf, &external_only);
	if (buffer->image == EGL_NO_IMAGE_KHR) {
		goto error_buffer;
	}

	push_gles2_debug(renderer);

	glGenRenderbuffers(1, &buffer->rbo);
	glBindRenderbuffer(GL_RENDERBUFFER, buffer->rbo);
	renderer->procs.glEGLImageTargetRenderbufferStorageOES(GL_RENDERBUFFER,
		buffer->image);
	glBindRenderbuffer(GL_RENDERBUFFER, 0);

	glGenFramebuffers(1, &buffer->fbo);
	glBindFramebuffer(GL_FRAMEBUFFER, buffer->fbo);
	glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
		GL_RENDERBUFFER, buffer->rbo);
	GLenum fb_status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
	glBindFramebuffer(GL_FRAMEBUFFER, 0);

	pop_gles2_debug(renderer);

	if (fb_status != GL_FRAMEBUFFER_COMPLETE) {
		wlr_log(WLR_ERROR, "Failed to create FBO");
		goto error_image;
	}

	buffer->buffer_destroy.notify = handle_buffer_destroy;
	wl_signal_add(&wlr_buffer->events.destroy, &buffer->buffer_destroy);

	wl_list_insert(&renderer->buffers, &buffer->link);

	wlr_log(WLR_DEBUG, "Created GL FBO for buffer %dx%d",
		wlr_buffer->width, wlr_buffer->height);

	return buffer;

error_image:
	wlr_egl_destroy_image(renderer->egl, buffer->image);
error_buffer:
	free(buffer);
	return NULL;
}

static bool gles2_bind_buffer(struct wlr_renderer *wlr_renderer,
		struct wlr_buffer *wlr_buffer) {
	struct wlr_gles2_renderer *renderer = gles2_get_renderer(wlr_renderer);

	if (renderer->current_buffer != NULL) {
		assert(wlr_egl_is_current(renderer->egl));

		push_gles2_debug(renderer);
		glFlush();
		glBindFramebuffer(GL_FRAMEBUFFER, 0);
		pop_gles2_debug(renderer);

		wlr_buffer_unlock(renderer->current_buffer->buffer);
		renderer->current_buffer = NULL;
	}

	if (wlr_buffer == NULL) {
		wlr_egl_unset_current(renderer->egl);
		return true;
	}

	wlr_egl_make_current(renderer->egl);

	struct wlr_gles2_buffer *buffer = get_buffer(renderer, wlr_buffer);
	if (buffer == NULL) {
		buffer = create_buffer(renderer, wlr_buffer);
	}
	if (buffer == NULL) {
		return false;
	}

	wlr_buffer_lock(wlr_buffer);
	renderer->current_buffer = buffer;

	push_gles2_debug(renderer);
	glBindFramebuffer(GL_FRAMEBUFFER, renderer->current_buffer->fbo);
	pop_gles2_debug(renderer);

	return true;
}

static void gles2_begin(struct wlr_renderer *wlr_renderer, uint32_t width,
		uint32_t height) {
	struct wlr_gles2_renderer *renderer =
		gles2_get_renderer_in_context(wlr_renderer);

	push_gles2_debug(renderer);

	glViewport(0, 0, width, height);
	renderer->viewport_width = width;
	renderer->viewport_height = height;

	// refresh projection matrix
	wlr_matrix_projection(renderer->projection, width, height,
			WL_OUTPUT_TRANSFORM_NORMAL);

	// enable transparency
	glEnable(GL_BLEND);
	glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);

	// XXX: maybe we should save output projection and remove some of the need
	// for users to sling matricies themselves

	pop_gles2_debug(renderer);
}

static void gles2_end(struct wlr_renderer *wlr_renderer) {
	gles2_get_renderer_in_context(wlr_renderer);
	// no-op
}

static void gles2_clear(struct wlr_renderer *wlr_renderer,
		const float color[static 4]) {
	struct wlr_gles2_renderer *renderer =
		gles2_get_renderer_in_context(wlr_renderer);

	push_gles2_debug(renderer);
	glClearColor(color[0], color[1], color[2], color[3]);
	glClear(GL_COLOR_BUFFER_BIT);
	pop_gles2_debug(renderer);
}

static void gles2_scissor(struct wlr_renderer *wlr_renderer,
		struct wlr_box *box) {
	struct wlr_gles2_renderer *renderer =
		gles2_get_renderer_in_context(wlr_renderer);

	push_gles2_debug(renderer);
	if (box != NULL) {
		glScissor(box->x, box->y, box->width, box->height);
		glEnable(GL_SCISSOR_TEST);
	} else {
		glDisable(GL_SCISSOR_TEST);
	}
	pop_gles2_debug(renderer);
}

static const float flip_180[9] = {
	1.0f, 0.0f, 0.0f,
	0.0f, -1.0f, 0.0f,
	0.0f, 0.0f, 1.0f,
};

static bool gles2_render_subtexture_with_matrix(
		struct wlr_renderer *wlr_renderer, struct wlr_texture *wlr_texture,
		const struct wlr_fbox *box, const float matrix[static 9],
		float alpha) {
	struct wlr_gles2_renderer *renderer =
		gles2_get_renderer_in_context(wlr_renderer);
	struct wlr_gles2_texture *texture =
		gles2_get_texture(wlr_texture);

	struct wlr_gles2_tex_shader *shader = NULL;

	switch (texture->target) {
	case GL_TEXTURE_2D:
		if (texture->has_alpha) {
			shader = &renderer->shaders.tex_rgba;
		} else {
			shader = &renderer->shaders.tex_rgbx;
		}
		break;
	case GL_TEXTURE_EXTERNAL_OES:
		shader = &renderer->shaders.tex_ext;

		if (!renderer->exts.egl_image_external_oes) {
			wlr_log(WLR_ERROR, "Failed to render texture: "
				"GL_TEXTURE_EXTERNAL_OES not supported");
			return false;
		}
		break;
	default:
		abort();
	}

	float gl_matrix[9];
	wlr_matrix_multiply(gl_matrix, renderer->projection, matrix);
	wlr_matrix_multiply(gl_matrix, flip_180, gl_matrix);

	// OpenGL ES 2 requires the glUniformMatrix3fv transpose parameter to be set
	// to GL_FALSE
	wlr_matrix_transpose(gl_matrix, gl_matrix);

	push_gles2_debug(renderer);

	glActiveTexture(GL_TEXTURE0);
	glBindTexture(texture->target, texture->tex);

	glTexParameteri(texture->target, GL_TEXTURE_MIN_FILTER, GL_LINEAR);

	glUseProgram(shader->program);

	glUniformMatrix3fv(shader->proj, 1, GL_FALSE, gl_matrix);
	glUniform1i(shader->invert_y, texture->inverted_y);
	glUniform1i(shader->tex, 0);
	glUniform1f(shader->alpha, alpha);

	const GLfloat x1 = box->x / wlr_texture->width;
	const GLfloat y1 = box->y / wlr_texture->height;
	const GLfloat x2 = (box->x + box->width) / wlr_texture->width;
	const GLfloat y2 = (box->y + box->height) / wlr_texture->height;
	const GLfloat texcoord[] = {
		x2, y1, // top right
		x1, y1, // top left
		x2, y2, // bottom right
		x1, y2, // bottom left
	};

	glVertexAttribPointer(shader->pos_attrib, 2, GL_FLOAT, GL_FALSE, 0, verts);
	glVertexAttribPointer(shader->tex_attrib, 2, GL_FLOAT, GL_FALSE, 0, texcoord);

	glEnableVertexAttribArray(shader->pos_attrib);
	glEnableVertexAttribArray(shader->tex_attrib);

	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

	glDisableVertexAttribArray(shader->pos_attrib);
	glDisableVertexAttribArray(shader->tex_attrib);

	glBindTexture(texture->target, 0);

	pop_gles2_debug(renderer);
	return true;
}

static void gles2_render_quad_with_matrix(struct wlr_renderer *wlr_renderer,
		const float color[static 4], const float matrix[static 9]) {
	struct wlr_gles2_renderer *renderer =
		gles2_get_renderer_in_context(wlr_renderer);

	float gl_matrix[9];
	wlr_matrix_multiply(gl_matrix, renderer->projection, matrix);
	wlr_matrix_multiply(gl_matrix, flip_180, gl_matrix);

	// OpenGL ES 2 requires the glUniformMatrix3fv transpose parameter to be set
	// to GL_FALSE
	wlr_matrix_transpose(gl_matrix, gl_matrix);

	push_gles2_debug(renderer);
	glUseProgram(renderer->shaders.quad.program);

	glUniformMatrix3fv(renderer->shaders.quad.proj, 1, GL_FALSE, gl_matrix);
	glUniform4f(renderer->shaders.quad.color, color[0], color[1], color[2], color[3]);

	glVertexAttribPointer(renderer->shaders.quad.pos_attrib, 2, GL_FLOAT, GL_FALSE,
			0, verts);

	glEnableVertexAttribArray(renderer->shaders.quad.pos_attrib);

	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

	glDisableVertexAttribArray(renderer->shaders.quad.pos_attrib);

	pop_gles2_debug(renderer);
}

static void gles2_render_ellipse_with_matrix(struct wlr_renderer *wlr_renderer,
		const float color[static 4], const float matrix[static 9]) {
	struct wlr_gles2_renderer *renderer =
		gles2_get_renderer_in_context(wlr_renderer);

	float gl_matrix[9];
	wlr_matrix_multiply(gl_matrix, renderer->projection, matrix);
	wlr_matrix_multiply(gl_matrix, flip_180, gl_matrix);

	// OpenGL ES 2 requires the glUniformMatrix3fv transpose parameter to be set
	// to GL_FALSE
	wlr_matrix_transpose(gl_matrix, gl_matrix);

	static const GLfloat texcoord[] = {
		1, 0, // top right
		0, 0, // top left
		1, 1, // bottom right
		0, 1, // bottom left
	};

	push_gles2_debug(renderer);
	glUseProgram(renderer->shaders.ellipse.program);

	glUniformMatrix3fv(renderer->shaders.ellipse.proj, 1, GL_FALSE, gl_matrix);
	glUniform4f(renderer->shaders.ellipse.color, color[0], color[1], color[2], color[3]);

	glVertexAttribPointer(renderer->shaders.ellipse.pos_attrib, 2, GL_FLOAT,
			GL_FALSE, 0, verts);
	glVertexAttribPointer(renderer->shaders.ellipse.tex_attrib, 2, GL_FLOAT,
			GL_FALSE, 0, texcoord);

	glEnableVertexAttribArray(renderer->shaders.ellipse.pos_attrib);
	glEnableVertexAttribArray(renderer->shaders.ellipse.tex_attrib);

	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

	glDisableVertexAttribArray(renderer->shaders.ellipse.pos_attrib);
	glDisableVertexAttribArray(renderer->shaders.ellipse.tex_attrib);
	pop_gles2_debug(renderer);
}

static const uint32_t *gles2_get_shm_texture_formats(
		struct wlr_renderer *wlr_renderer, size_t *len) {
	return get_gles2_shm_formats(len);
}

static bool gles2_resource_is_wl_drm_buffer(struct wlr_renderer *wlr_renderer,
		struct wl_resource *resource) {
	struct wlr_gles2_renderer *renderer = gles2_get_renderer(wlr_renderer);

	if (!renderer->egl->exts.bind_wayland_display_wl) {
		return false;
	}

	EGLint fmt;
	return renderer->egl->procs.eglQueryWaylandBufferWL(renderer->egl->display,
		resource, EGL_TEXTURE_FORMAT, &fmt);
}

static void gles2_wl_drm_buffer_get_size(struct wlr_renderer *wlr_renderer,
		struct wl_resource *buffer, int *width, int *height) {
	struct wlr_gles2_renderer *renderer =
		gles2_get_renderer(wlr_renderer);

	if (!renderer->egl->exts.bind_wayland_display_wl) {
		return;
	}

	renderer->egl->procs.eglQueryWaylandBufferWL(renderer->egl->display,
		buffer, EGL_WIDTH, width);
	renderer->egl->procs.eglQueryWaylandBufferWL(renderer->egl->display,
		buffer, EGL_HEIGHT, height);
}

static const struct wlr_drm_format_set *gles2_get_dmabuf_texture_formats(
		struct wlr_renderer *wlr_renderer) {
	struct wlr_gles2_renderer *renderer = gles2_get_renderer(wlr_renderer);
	return wlr_egl_get_dmabuf_texture_formats(renderer->egl);
}

static const struct wlr_drm_format_set *gles2_get_dmabuf_render_formats(
		struct wlr_renderer *wlr_renderer) {
	struct wlr_gles2_renderer *renderer = gles2_get_renderer(wlr_renderer);
	return wlr_egl_get_dmabuf_render_formats(renderer->egl);
}

static uint32_t gles2_preferred_read_format(
		struct wlr_renderer *wlr_renderer) {
	struct wlr_gles2_renderer *renderer =
		gles2_get_renderer_in_context(wlr_renderer);

	push_gles2_debug(renderer);

	GLint gl_format = -1, gl_type = -1;
	glGetIntegerv(GL_IMPLEMENTATION_COLOR_READ_FORMAT, &gl_format);
	glGetIntegerv(GL_IMPLEMENTATION_COLOR_READ_TYPE, &gl_type);

	EGLint alpha_size = -1;
	glBindRenderbuffer(GL_RENDERBUFFER, renderer->current_buffer->rbo);
	glGetRenderbufferParameteriv(GL_RENDERBUFFER,
		GL_RENDERBUFFER_ALPHA_SIZE, &alpha_size);
	glBindRenderbuffer(GL_RENDERBUFFER, 0);

	pop_gles2_debug(renderer);

	const struct wlr_gles2_pixel_format *fmt =
		get_gles2_format_from_gl(gl_format, gl_type, alpha_size > 0);
	if (fmt != NULL) {
		return fmt->drm_format;
	}

	if (renderer->exts.read_format_bgra_ext) {
		return DRM_FORMAT_XRGB8888;
	}
	return DRM_FORMAT_XBGR8888;
}

static bool gles2_read_pixels(struct wlr_renderer *wlr_renderer,
		uint32_t drm_format, uint32_t *flags, uint32_t stride,
		uint32_t width, uint32_t height, uint32_t src_x, uint32_t src_y,
		uint32_t dst_x, uint32_t dst_y, void *data) {
	struct wlr_gles2_renderer *renderer =
		gles2_get_renderer_in_context(wlr_renderer);

	const struct wlr_gles2_pixel_format *fmt =
		get_gles2_format_from_drm(drm_format);
	if (fmt == NULL) {
		wlr_log(WLR_ERROR, "Cannot read pixels: unsupported pixel format");
		return false;
	}

	if (fmt->gl_format == GL_BGRA_EXT && !renderer->exts.read_format_bgra_ext) {
		wlr_log(WLR_ERROR,
			"Cannot read pixels: missing GL_EXT_read_format_bgra extension");
		return false;
	}

	const struct wlr_pixel_format_info *drm_fmt =
		drm_get_pixel_format_info(fmt->drm_format);
	assert(drm_fmt);

	push_gles2_debug(renderer);

	// Make sure any pending drawing is finished before we try to read it
	glFinish();

	glGetError(); // Clear the error flag

	unsigned char *p = (unsigned char *)data + dst_y * stride;
	uint32_t pack_stride = width * drm_fmt->bpp / 8;
	if (pack_stride == stride && dst_x == 0) {
		// Under these particular conditions, we can read the pixels with only
		// one glReadPixels call

		glReadPixels(src_x, src_y, width, height, fmt->gl_format, fmt->gl_type, p);
	} else {
		// Unfortunately GLES2 doesn't support GL_PACK_*, so we have to read
		// the lines out row by row
		for (size_t i = 0; i < height; ++i) {
			uint32_t y = src_y + i;
			glReadPixels(src_x, y, width, 1, fmt->gl_format,
				fmt->gl_type, p + i * stride + dst_x * drm_fmt->bpp / 8);
		}
	}

	pop_gles2_debug(renderer);

	if (flags != NULL) {
		*flags = 0;
	}

	return glGetError() == GL_NO_ERROR;
}

static bool gles2_blit_dmabuf(struct wlr_renderer *wlr_renderer,
		struct wlr_dmabuf_attributes *dst_attr,
		struct wlr_dmabuf_attributes *src_attr) {
	struct wlr_gles2_renderer *renderer = gles2_get_renderer(wlr_renderer);
	if (!renderer->procs.glEGLImageTargetRenderbufferStorageOES) {
		return false;
	}

	struct wlr_egl_context old_context;
	wlr_egl_save_context(&old_context);

	bool r = false;
	struct wlr_texture *src_tex =
		wlr_texture_from_dmabuf(wlr_renderer, src_attr);
	if (!src_tex) {
		goto restore_context_out;
	}

	// TODO: get inverted_y right
	// This is to take into account y-inversion on both buffers rather than
	// just the source buffer.
	bool src_inverted_y =
		!!(src_attr->flags & WLR_DMABUF_ATTRIBUTES_FLAGS_Y_INVERT);
	bool dst_inverted_y =
		!!(dst_attr->flags & WLR_DMABUF_ATTRIBUTES_FLAGS_Y_INVERT);
	struct wlr_gles2_texture *gles2_src_tex = gles2_get_texture(src_tex);
	gles2_src_tex->inverted_y = src_inverted_y ^ dst_inverted_y;

	if (!wlr_egl_make_current(renderer->egl)) {
		goto texture_destroy_out;
	}

	// TODO: The imported buffer should be checked with
	// eglQueryDmaBufModifiersEXT to see if it may be modified.
	bool external_only = false;
	EGLImageKHR image = wlr_egl_create_image_from_dmabuf(renderer->egl, dst_attr,
			&external_only);
	if (image == EGL_NO_IMAGE_KHR) {
		goto texture_destroy_out;
	}

	GLuint rbo = 0;
	glGenRenderbuffers(1, &rbo);
	glBindRenderbuffer(GL_RENDERBUFFER, rbo);
	renderer->procs.glEGLImageTargetRenderbufferStorageOES(GL_RENDERBUFFER,
			image);
	glBindRenderbuffer(GL_RENDERBUFFER, 0);

	GLuint fbo = 0;
	glGenFramebuffers(1, &fbo);
	glBindFramebuffer(GL_FRAMEBUFFER, fbo);
	glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
			GL_RENDERBUFFER, rbo);

	GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
	if (status != GL_FRAMEBUFFER_COMPLETE) {
		goto out;
	}

	// TODO: use ANGLE_framebuffer_blit if available
	float mat[9];
	wlr_matrix_identity(mat);

	wlr_renderer_begin(wlr_renderer, dst_attr->width, dst_attr->height);
	wlr_renderer_clear(wlr_renderer, (float[]){ 0.0, 0.0, 0.0, 0.0 });
	wlr_render_texture_with_matrix(wlr_renderer, src_tex, mat, 1.0f);
	wlr_renderer_end(wlr_renderer);

	r = true;
out:
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	glDeleteFramebuffers(1, &fbo);
	glDeleteRenderbuffers(1, &rbo);
	wlr_egl_destroy_image(renderer->egl, image);
texture_destroy_out:
	wlr_texture_destroy(src_tex);
restore_context_out:
	wlr_egl_restore_context(&old_context);
	return r;
}

static bool gles2_init_wl_display(struct wlr_renderer *wlr_renderer,
		struct wl_display *wl_display) {
	struct wlr_gles2_renderer *renderer =
		gles2_get_renderer(wlr_renderer);

	if (renderer->egl->exts.bind_wayland_display_wl) {
		if (!wlr_egl_bind_display(renderer->egl, wl_display)) {
			wlr_log(WLR_ERROR, "Failed to bind wl_display to EGL");
			return false;
		}
	} else {
		wlr_log(WLR_INFO, "EGL_WL_bind_wayland_display is not supported");
	}

	if (renderer->egl->exts.image_dmabuf_import_ext) {
		if (wlr_linux_dmabuf_v1_create(wl_display, wlr_renderer) == NULL) {
			return false;
		}
	} else {
		wlr_log(WLR_INFO, "EGL_EXT_image_dma_buf_import is not supported");
	}

	return true;
}

static int gles2_get_drm_fd(struct wlr_renderer *wlr_renderer) {
	struct wlr_gles2_renderer *renderer =
		gles2_get_renderer(wlr_renderer);

	if (renderer->drm_fd < 0) {
		renderer->drm_fd = wlr_egl_dup_drm_fd(renderer->egl);
	}

	return renderer->drm_fd;
}

struct wlr_egl *wlr_gles2_renderer_get_egl(struct wlr_renderer *wlr_renderer) {
	struct wlr_gles2_renderer *renderer =
		gles2_get_renderer(wlr_renderer);
	return renderer->egl;
}

static void gles2_destroy(struct wlr_renderer *wlr_renderer) {
	struct wlr_gles2_renderer *renderer = gles2_get_renderer(wlr_renderer);

	wlr_egl_make_current(renderer->egl);

	struct wlr_gles2_buffer *buffer, *buffer_tmp;
	wl_list_for_each_safe(buffer, buffer_tmp, &renderer->buffers, link) {
		destroy_buffer(buffer);
	}

	push_gles2_debug(renderer);
	glDeleteProgram(renderer->shaders.quad.program);
	glDeleteProgram(renderer->shaders.ellipse.program);
	glDeleteProgram(renderer->shaders.tex_rgba.program);
	glDeleteProgram(renderer->shaders.tex_rgbx.program);
	glDeleteProgram(renderer->shaders.tex_ext.program);
	pop_gles2_debug(renderer);

	if (renderer->exts.debug_khr) {
		glDisable(GL_DEBUG_OUTPUT_KHR);
		renderer->procs.glDebugMessageCallbackKHR(NULL, NULL);
	}

	wlr_egl_unset_current(renderer->egl);
	wlr_egl_destroy(renderer->egl);

	if (renderer->drm_fd >= 0) {
		close(renderer->drm_fd);
	}

	free(renderer);
}

static struct wlr_buffer *gles2_get_current_wlr_buffer(struct wlr_renderer *renderer) {
    struct wlr_gles2_renderer *r = gles2_get_renderer(renderer);
    return r->current_buffer ? r->current_buffer->buffer : NULL;
}

static const struct wlr_renderer_impl renderer_impl = {
	.destroy = gles2_destroy,
	.bind_buffer = gles2_bind_buffer,
	.begin = gles2_begin,
	.end = gles2_end,
	.clear = gles2_clear,
	.scissor = gles2_scissor,
	.render_subtexture_with_matrix = gles2_render_subtexture_with_matrix,
	.render_quad_with_matrix = gles2_render_quad_with_matrix,
	.render_ellipse_with_matrix = gles2_render_ellipse_with_matrix,
	.get_shm_texture_formats = gles2_get_shm_texture_formats,
	.resource_is_wl_drm_buffer = gles2_resource_is_wl_drm_buffer,
	.wl_drm_buffer_get_size = gles2_wl_drm_buffer_get_size,
	.get_dmabuf_texture_formats = gles2_get_dmabuf_texture_formats,
	.get_dmabuf_render_formats = gles2_get_dmabuf_render_formats,
	.preferred_read_format = gles2_preferred_read_format,
	.read_pixels = gles2_read_pixels,
	.texture_from_pixels = gles2_texture_from_pixels,
	.texture_from_wl_drm = gles2_texture_from_wl_drm,
	.texture_from_dmabuf = gles2_texture_from_dmabuf,
	.init_wl_display = gles2_init_wl_display,
	.blit_dmabuf = gles2_blit_dmabuf,
	.get_drm_fd = gles2_get_drm_fd,
	.get_current_buffer = gles2_get_current_wlr_buffer,
};

void push_gles2_debug_(struct wlr_gles2_renderer *renderer,
		const char *file, const char *func) {
	if (!renderer->procs.glPushDebugGroupKHR) {
		return;
	}

	int len = snprintf(NULL, 0, "%s:%s", file, func) + 1;
	char str[len];
	snprintf(str, len, "%s:%s", file, func);
	renderer->procs.glPushDebugGroupKHR(GL_DEBUG_SOURCE_APPLICATION_KHR, 1, -1, str);
}

void pop_gles2_debug(struct wlr_gles2_renderer *renderer) {
	if (renderer->procs.glPopDebugGroupKHR) {
		renderer->procs.glPopDebugGroupKHR();
	}
}

static enum wlr_log_importance gles2_log_importance_to_wlr(GLenum type) {
	switch (type) {
	case GL_DEBUG_TYPE_ERROR_KHR:               return WLR_ERROR;
	case GL_DEBUG_TYPE_DEPRECATED_BEHAVIOR_KHR: return WLR_DEBUG;
	case GL_DEBUG_TYPE_UNDEFINED_BEHAVIOR_KHR:  return WLR_ERROR;
	case GL_DEBUG_TYPE_PORTABILITY_KHR:         return WLR_DEBUG;
	case GL_DEBUG_TYPE_PERFORMANCE_KHR:         return WLR_DEBUG;
	case GL_DEBUG_TYPE_OTHER_KHR:               return WLR_DEBUG;
	case GL_DEBUG_TYPE_MARKER_KHR:              return WLR_DEBUG;
	case GL_DEBUG_TYPE_PUSH_GROUP_KHR:          return WLR_DEBUG;
	case GL_DEBUG_TYPE_POP_GROUP_KHR:           return WLR_DEBUG;
	default:                                    return WLR_DEBUG;
	}
}

static void gles2_log(GLenum src, GLenum type, GLuint id, GLenum severity,
		GLsizei len, const GLchar *msg, const void *user) {
	_wlr_log(gles2_log_importance_to_wlr(type), "[GLES2] %s", msg);
}

static GLuint compile_shader(struct wlr_gles2_renderer *renderer,
		GLuint type, const GLchar *src) {
	push_gles2_debug(renderer);

	GLuint shader = glCreateShader(type);
	glShaderSource(shader, 1, &src, NULL);
	glCompileShader(shader);

	GLint ok;
	glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
	if (ok == GL_FALSE) {
		glDeleteShader(shader);
		shader = 0;
	}

	pop_gles2_debug(renderer);
	return shader;
}

static GLuint link_program(struct wlr_gles2_renderer *renderer,
		const GLchar *vert_src, const GLchar *frag_src) {
	push_gles2_debug(renderer);

	GLuint vert = compile_shader(renderer, GL_VERTEX_SHADER, vert_src);
	if (!vert) {
		goto error;
	}

	GLuint frag = compile_shader(renderer, GL_FRAGMENT_SHADER, frag_src);
	if (!frag) {
		glDeleteShader(vert);
		goto error;
	}

	GLuint prog = glCreateProgram();
	glAttachShader(prog, vert);
	glAttachShader(prog, frag);
	glLinkProgram(prog);

	glDetachShader(prog, vert);
	glDetachShader(prog, frag);
	glDeleteShader(vert);
	glDeleteShader(frag);

	GLint ok;
	glGetProgramiv(prog, GL_LINK_STATUS, &ok);
	if (ok == GL_FALSE) {
		glDeleteProgram(prog);
		goto error;
	}

	pop_gles2_debug(renderer);
	return prog;

error:
	pop_gles2_debug(renderer);
	return 0;
}

static bool check_gl_ext(const char *exts, const char *ext) {
	size_t extlen = strlen(ext);
	const char *end = exts + strlen(exts);

	while (exts < end) {
		if (exts[0] == ' ') {
			exts++;
			continue;
		}
		size_t n = strcspn(exts, " ");
		if (n == extlen && strncmp(ext, exts, n) == 0) {
			return true;
		}
		exts += n;
	}
	return false;
}

static void load_gl_proc(void *proc_ptr, const char *name) {
	void *proc = (void *)eglGetProcAddress(name);
	if (proc == NULL) {
		wlr_log(WLR_ERROR, "eglGetProcAddress(%s) failed", name);
		abort();
	}
	*(void **)proc_ptr = proc;
}

extern const GLchar quad_vertex_src[];
extern const GLchar quad_fragment_src[];
extern const GLchar ellipse_fragment_src[];
extern const GLchar tex_vertex_src[];
extern const GLchar tex_fragment_src_rgba[];
extern const GLchar tex_fragment_src_rgbx[];
extern const GLchar tex_fragment_src_external[];

struct wlr_renderer *wlr_gles2_renderer_create(struct wlr_egl *egl) {
	if (!wlr_egl_make_current(egl)) {
		return NULL;
	}

	const char *exts_str = (const char *)glGetString(GL_EXTENSIONS);
	if (exts_str == NULL) {
		wlr_log(WLR_ERROR, "Failed to get GL_EXTENSIONS");
		return NULL;
	}

	struct wlr_gles2_renderer *renderer =
		calloc(1, sizeof(struct wlr_gles2_renderer));
	if (renderer == NULL) {
		return NULL;
	}
	wlr_renderer_init(&renderer->wlr_renderer, &renderer_impl);

	wl_list_init(&renderer->buffers);

	renderer->egl = egl;
	renderer->exts_str = exts_str;
	renderer->drm_fd = -1;

	wlr_log(WLR_INFO, "Using %s", glGetString(GL_VERSION));
	wlr_log(WLR_INFO, "GL vendor: %s", glGetString(GL_VENDOR));
	wlr_log(WLR_INFO, "GL renderer: %s", glGetString(GL_RENDERER));
	wlr_log(WLR_INFO, "Supported GLES2 extensions: %s", exts_str);

	if (!check_gl_ext(exts_str, "GL_EXT_texture_format_BGRA8888")) {
		wlr_log(WLR_ERROR, "BGRA8888 format not supported by GLES2");
		free(renderer);
		return NULL;
	}
	if (!check_gl_ext(exts_str, "GL_EXT_unpack_subimage")) {
		wlr_log(WLR_ERROR, "GL_EXT_unpack_subimage not supported");
		free(renderer);
		return NULL;
	}

	renderer->exts.read_format_bgra_ext =
		check_gl_ext(exts_str, "GL_EXT_read_format_bgra");

	if (check_gl_ext(exts_str, "GL_KHR_debug")) {
		renderer->exts.debug_khr = true;
		load_gl_proc(&renderer->procs.glDebugMessageCallbackKHR,
			"glDebugMessageCallbackKHR");
		load_gl_proc(&renderer->procs.glDebugMessageControlKHR,
			"glDebugMessageControlKHR");
	}

	if (check_gl_ext(exts_str, "GL_OES_EGL_image_external")) {
		renderer->exts.egl_image_external_oes = true;
		load_gl_proc(&renderer->procs.glEGLImageTargetTexture2DOES,
			"glEGLImageTargetTexture2DOES");
	}

	if (check_gl_ext(exts_str, "GL_OES_EGL_image")) {
		renderer->exts.egl_image_oes = true;
		load_gl_proc(&renderer->procs.glEGLImageTargetRenderbufferStorageOES,
			"glEGLImageTargetRenderbufferStorageOES");
	}

	if (renderer->exts.debug_khr) {
		glEnable(GL_DEBUG_OUTPUT_KHR);
		glEnable(GL_DEBUG_OUTPUT_SYNCHRONOUS_KHR);
		renderer->procs.glDebugMessageCallbackKHR(gles2_log, NULL);

		// Silence unwanted message types
		renderer->procs.glDebugMessageControlKHR(GL_DONT_CARE,
			GL_DEBUG_TYPE_POP_GROUP_KHR, GL_DONT_CARE, 0, NULL, GL_FALSE);
		renderer->procs.glDebugMessageControlKHR(GL_DONT_CARE,
			GL_DEBUG_TYPE_PUSH_GROUP_KHR, GL_DONT_CARE, 0, NULL, GL_FALSE);
	}

	push_gles2_debug(renderer);

	GLuint prog;
	renderer->shaders.quad.program = prog =
		link_program(renderer, quad_vertex_src, quad_fragment_src);
	if (!renderer->shaders.quad.program) {
		goto error;
	}
	renderer->shaders.quad.proj = glGetUniformLocation(prog, "proj");
	renderer->shaders.quad.color = glGetUniformLocation(prog, "color");
	renderer->shaders.quad.pos_attrib = glGetAttribLocation(prog, "pos");

	renderer->shaders.ellipse.program = prog =
		link_program(renderer, quad_vertex_src, ellipse_fragment_src);
	if (!renderer->shaders.ellipse.program) {
		goto error;
	}
	renderer->shaders.ellipse.proj = glGetUniformLocation(prog, "proj");
	renderer->shaders.ellipse.color = glGetUniformLocation(prog, "color");
	renderer->shaders.ellipse.pos_attrib = glGetAttribLocation(prog, "pos");
	renderer->shaders.ellipse.tex_attrib = glGetAttribLocation(prog, "texcoord");

	renderer->shaders.tex_rgba.program = prog =
		link_program(renderer, tex_vertex_src, tex_fragment_src_rgba);
	if (!renderer->shaders.tex_rgba.program) {
		goto error;
	}
	renderer->shaders.tex_rgba.proj = glGetUniformLocation(prog, "proj");
	renderer->shaders.tex_rgba.invert_y = glGetUniformLocation(prog, "invert_y");
	renderer->shaders.tex_rgba.tex = glGetUniformLocation(prog, "tex");
	renderer->shaders.tex_rgba.alpha = glGetUniformLocation(prog, "alpha");
	renderer->shaders.tex_rgba.pos_attrib = glGetAttribLocation(prog, "pos");
	renderer->shaders.tex_rgba.tex_attrib = glGetAttribLocation(prog, "texcoord");

	renderer->shaders.tex_rgbx.program = prog =
		link_program(renderer, tex_vertex_src, tex_fragment_src_rgbx);
	if (!renderer->shaders.tex_rgbx.program) {
		goto error;
	}
	renderer->shaders.tex_rgbx.proj = glGetUniformLocation(prog, "proj");
	renderer->shaders.tex_rgbx.invert_y = glGetUniformLocation(prog, "invert_y");
	renderer->shaders.tex_rgbx.tex = glGetUniformLocation(prog, "tex");
	renderer->shaders.tex_rgbx.alpha = glGetUniformLocation(prog, "alpha");
	renderer->shaders.tex_rgbx.pos_attrib = glGetAttribLocation(prog, "pos");
	renderer->shaders.tex_rgbx.tex_attrib = glGetAttribLocation(prog, "texcoord");

	if (renderer->exts.egl_image_external_oes) {
		renderer->shaders.tex_ext.program = prog =
			link_program(renderer, tex_vertex_src, tex_fragment_src_external);
		if (!renderer->shaders.tex_ext.program) {
			goto error;
		}
		renderer->shaders.tex_ext.proj = glGetUniformLocation(prog, "proj");
		renderer->shaders.tex_ext.invert_y = glGetUniformLocation(prog, "invert_y");
		renderer->shaders.tex_ext.tex = glGetUniformLocation(prog, "tex");
		renderer->shaders.tex_ext.alpha = glGetUniformLocation(prog, "alpha");
		renderer->shaders.tex_ext.pos_attrib = glGetAttribLocation(prog, "pos");
		renderer->shaders.tex_ext.tex_attrib = glGetAttribLocation(prog, "texcoord");
	}

	pop_gles2_debug(renderer);

	wlr_egl_unset_current(renderer->egl);

	return &renderer->wlr_renderer;

error:
	glDeleteProgram(renderer->shaders.quad.program);
	glDeleteProgram(renderer->shaders.ellipse.program);
	glDeleteProgram(renderer->shaders.tex_rgba.program);
	glDeleteProgram(renderer->shaders.tex_rgbx.program);
	glDeleteProgram(renderer->shaders.tex_ext.program);

	pop_gles2_debug(renderer);

	if (renderer->exts.debug_khr) {
		glDisable(GL_DEBUG_OUTPUT_KHR);
		renderer->procs.glDebugMessageCallbackKHR(NULL, NULL);
	}

	wlr_egl_unset_current(renderer->egl);

	free(renderer);
	return NULL;
}

bool wlr_gles2_renderer_check_ext(struct wlr_renderer *wlr_renderer,
		const char *ext) {
	struct wlr_gles2_renderer *renderer = gles2_get_renderer(wlr_renderer);
	return check_gl_ext(renderer->exts_str, ext);
}

struct wlr_gles2_buffer *gles2_get_current_buffer(struct wlr_renderer *wlr_renderer) {
	struct wlr_gles2_renderer *renderer = gles2_get_renderer(wlr_renderer);
	return renderer->current_buffer;
}

struct wlr_gles2_buffer *gles2_get_buffer(struct wlr_renderer *wlr_renderer,
		struct wlr_buffer *buffer) {
	struct wlr_gles2_renderer *renderer = gles2_get_renderer(wlr_renderer);
	return get_buffer(renderer, buffer);
}
