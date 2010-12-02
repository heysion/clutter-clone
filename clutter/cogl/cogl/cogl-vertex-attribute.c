/*
 * Cogl
 *
 * An object oriented GL/GLES Abstraction/Utility Layer
 *
 * Copyright (C) 2010 Intel Corporation.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library. If not, see
 * <http://www.gnu.org/licenses/>.
 *
 *
 *
 * Authors:
 *   Robert Bragg <robert@linux.intel.com>
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "cogl-context.h"
#include "cogl-object-private.h"
#include "cogl-journal-private.h"
#include "cogl-vertex-attribute.h"
#include "cogl-vertex-attribute-private.h"
#include "cogl-pipeline.h"
#include "cogl-pipeline-private.h"
#include "cogl-pipeline-opengl-private.h"
#include "cogl-texture-private.h"
#include "cogl-framebuffer-private.h"
#include "cogl-indices-private.h"

#include <string.h>
#include <stdio.h>

#if defined (HAVE_COGL_GL)

#define glGenBuffers ctx->drv.pf_glGenBuffers
#define glBindBuffer ctx->drv.pf_glBindBuffer
#define glBufferData ctx->drv.pf_glBufferData
#define glBufferSubData ctx->drv.pf_glBufferSubData
#define glGetBufferSubData ctx->drv.pf_glGetBufferSubData
#define glDeleteBuffers ctx->drv.pf_glDeleteBuffers
#define glMapBuffer ctx->drv.pf_glMapBuffer
#define glUnmapBuffer ctx->drv.pf_glUnmapBuffer
#define glClientActiveTexture ctx->drv.pf_glClientActiveTexture
#ifndef GL_ARRAY_BUFFER
#define GL_ARRAY_BUFFER GL_ARRAY_BUFFER_ARB
#endif

#define glVertexAttribPointer ctx->drv.pf_glVertexAttribPointer
#define glEnableVertexAttribArray ctx->drv.pf_glEnableVertexAttribArray
#define glDisableVertexAttribArray ctx->drv.pf_glDisableVertexAttribArray
#define MAY_HAVE_PROGRAMABLE_GL

#define glDrawRangeElements(mode, start, end, count, type, indices) \
  ctx->drv.pf_glDrawRangeElements (mode, start, end, count, type, indices)

#else /* GLES 1/2 */

/* GLES doesn't have glDrawRangeElements, so we simply pretend it does
 * but that it makes no use of the start, end constraints: */
#define glDrawRangeElements(mode, start, end, count, type, indices) \
  glDrawElements (mode, count, type, indices)

/* This isn't defined in the GLES headers */
#ifndef GL_UNSIGNED_INT
#define GL_UNSIGNED_INT 0x1405
#endif

#ifdef HAVE_COGL_GLES2

#include "../gles/cogl-gles2-wrapper.h"
#define MAY_HAVE_PROGRAMABLE_GL

#endif /* HAVE_COGL_GLES2 */

#endif

static void _cogl_vertex_attribute_free (CoglVertexAttribute *attribute);

COGL_OBJECT_DEFINE (VertexAttribute, vertex_attribute);

#if 0
gboolean
validate_gl_attribute (const char *name,
                       int n_components,
                       CoglVertexAttributeNameID *name_id,
                       gboolean *normalized,
                       unsigned int *texture_unit)
{
  name = name + 3; /* skip past "gl_" */

  *normalized = FALSE;
  *texture_unit = 0;

  if (strcmp (name, "Vertex") == 0)
    {
      if (G_UNLIKELY (n_components == 1))
        {
          g_critical ("glVertexPointer doesn't allow 1 component vertex "
                      "positions so we currently only support \"gl_Vertex\" "
                      "attributes where n_components == 2, 3 or 4");
          return FALSE;
        }
      *name_id = COGL_VERTEX_ATTRIBUTE_NAME_ID_POSITION_ARRAY;
    }
  else if (strcmp (name, "Color") == 0)
    {
      if (G_UNLIKELY (n_components != 3 && n_components != 4))
        {
          g_critical ("glColorPointer expects 3 or 4 component colors so we "
                      "currently only support \"gl_Color\" attributes where "
                      "n_components == 3 or 4");
          return FALSE;
        }
      *name_id = COGL_VERTEX_ATTRIBUTE_NAME_ID_COLOR_ARRAY;
      *normalized = TRUE;
    }
  else if (strncmp (name, "MultiTexCoord", strlen ("MultiTexCoord")) == 0)
    {
      if (sscanf (gl_attribute, "MultiTexCoord%u", texture_unit) != 1)
	{
	  g_warning ("gl_MultiTexCoord attributes should include a\n"
		     "texture unit number, E.g. gl_MultiTexCoord0\n");
	  unit = 0;
	}
      *name_id = COGL_VERTEX_ATTRIBUTE_NAME_ID_TEXTURE_COORD_ARRAY;
    }
  else if (strncmp (name, "Normal") == 0)
    {
      if (G_UNLIKELY (n_components != 3))
        {
          g_critical ("glNormalPointer expects 3 component normals so we "
                      "currently only support \"gl_Normal\" attributes where "
                      "n_components == 3");
          return FALSE;
        }
      *name_id = COGL_VERTEX_ATTRIBUTE_NAME_ID_NORMAL_ARRAY;
      *normalized = TRUE;
    }
  else
    {
      g_warning ("Unknown gl_* attribute name gl_%s\n", name);
      return FALSE;
    }

  return TRUE;
}
#endif

gboolean
validate_cogl_attribute (const char *name,
                         int n_components,
                         CoglVertexAttributeNameID *name_id,
                         gboolean *normalized,
                         unsigned int *texture_unit)
{
  name = name + 5; /* skip "cogl_" */

  *normalized = FALSE;
  *texture_unit = 0;

  if (strcmp (name, "position_in") == 0)
    {
      if (G_UNLIKELY (n_components == 1))
        {
          g_critical ("glVertexPointer doesn't allow 1 component vertex "
                      "positions so we currently only support \"cogl_vertex\" "
                      "attributes where n_components == 2, 3 or 4");
          return FALSE;
        }
      *name_id = COGL_VERTEX_ATTRIBUTE_NAME_ID_POSITION_ARRAY;
    }
  else if (strcmp (name, "color_in") == 0)
    {
      if (G_UNLIKELY (n_components != 3 && n_components != 4))
        {
          g_critical ("glColorPointer expects 3 or 4 component colors so we "
                      "currently only support \"cogl_color\" attributes where "
                      "n_components == 3 or 4");
          return FALSE;
        }
      *name_id = COGL_VERTEX_ATTRIBUTE_NAME_ID_COLOR_ARRAY;
    }
  else if (strcmp (name, "tex_coord_in") == 0)
    *name_id = COGL_VERTEX_ATTRIBUTE_NAME_ID_TEXTURE_COORD_ARRAY;
  else if (strncmp (name, "tex_coord", strlen ("tex_coord")) == 0)
    {
      if (sscanf (name, "tex_coord%u_in", texture_unit) != 1)
	{
	  g_warning ("Texture coordinate attributes should either be named "
                     "\"cogl_tex_coord\" or named with a texture unit index "
                     "like \"cogl_tex_coord2_in\"\n");
          return FALSE;
	}
      *name_id = COGL_VERTEX_ATTRIBUTE_NAME_ID_TEXTURE_COORD_ARRAY;
    }
  else if (strcmp (name, "normal") == 0)
    {
      if (G_UNLIKELY (n_components != 3))
        {
          g_critical ("glNormalPointer expects 3 component normals so we "
                      "currently only support \"cogl_normal\" attributes "
                      "where n_components == 3");
          return FALSE;
        }
      *name_id = COGL_VERTEX_ATTRIBUTE_NAME_ID_NORMAL_ARRAY;
      *normalized = TRUE;
    }
  else
    {
      g_warning ("Unknown cogl_* attribute name cogl_%s\n", name);
      return FALSE;
    }

  return TRUE;
}

CoglVertexAttribute *
cogl_vertex_attribute_new (CoglVertexArray *array,
                           const char *name,
                           gsize stride,
                           gsize offset,
                           int n_components,
                           CoglVertexAttributeType type)
{
  CoglVertexAttribute *attribute = g_slice_new (CoglVertexAttribute);
  gboolean status;

  attribute->array = cogl_object_ref (array);
  attribute->name = g_strdup (name);
  attribute->stride = stride;
  attribute->offset = offset;
  attribute->n_components = n_components;
  attribute->type = type;
  attribute->immutable_ref = 0;

  if (strncmp (name, "cogl_", 5) == 0)
    status = validate_cogl_attribute (attribute->name,
                                      n_components,
                                      &attribute->name_id,
                                      &attribute->normalized,
                                      &attribute->texture_unit);
#if 0
  else if (strncmp (name, "gl_", 3) == 0)
    status = validate_gl_attribute (attribute->name,
                                    n_components,
                                    &attribute->name_id,
                                    &attribute->normalized,
                                    &attribute->texture_unit);
#endif
  else
    {
      attribute->name_id = COGL_VERTEX_ATTRIBUTE_NAME_ID_CUSTOM_ARRAY;
      attribute->normalized = FALSE;
      attribute->texture_unit = 0;
      status = TRUE;
    }

  if (!status)
    {
      _cogl_vertex_attribute_free (attribute);
      return NULL;
    }

  return _cogl_vertex_attribute_object_new (attribute);
}

gboolean
cogl_vertex_attribute_get_normalized (CoglVertexAttribute *attribute)
{
  g_return_val_if_fail (cogl_is_vertex_attribute (attribute), FALSE);

  return attribute->normalized;
}

static void
warn_about_midscene_changes (void)
{
  static gboolean seen = FALSE;
  if (!seen)
    {
      g_warning ("Mid-scene modification of attributes has "
                 "undefined results\n");
      seen = TRUE;
    }
}

void
cogl_vertex_attribute_set_normalized (CoglVertexAttribute *attribute,
                                      gboolean normalized)
{
  g_return_if_fail (cogl_is_vertex_attribute (attribute));

  if (G_UNLIKELY (attribute->immutable_ref))
    warn_about_midscene_changes ();

  attribute->normalized = normalized;
}

CoglVertexArray *
cogl_vertex_attribute_get_array (CoglVertexAttribute *attribute)
{
  g_return_val_if_fail (cogl_is_vertex_attribute (attribute), NULL);

  return attribute->array;
}

void
cogl_vertex_attribute_set_array (CoglVertexAttribute *attribute,
                                 CoglVertexArray *array)
{
  g_return_if_fail (cogl_is_vertex_attribute (attribute));

  if (G_UNLIKELY (attribute->immutable_ref))
    warn_about_midscene_changes ();

  cogl_object_ref (array);

  cogl_object_unref (attribute->array);
  attribute->array = array;
}

CoglVertexAttribute *
_cogl_vertex_attribute_immutable_ref (CoglVertexAttribute *vertex_attribute)
{
  g_return_val_if_fail (cogl_is_vertex_attribute (vertex_attribute), NULL);

  vertex_attribute->immutable_ref++;
  _cogl_buffer_immutable_ref (COGL_BUFFER (vertex_attribute->array));
  return vertex_attribute;
}

void
_cogl_vertex_attribute_immutable_unref (CoglVertexAttribute *vertex_attribute)
{
  g_return_if_fail (cogl_is_vertex_attribute (vertex_attribute));
  g_return_if_fail (vertex_attribute->immutable_ref > 0);

  vertex_attribute->immutable_ref--;
  _cogl_buffer_immutable_unref (COGL_BUFFER (vertex_attribute->array));
}

static void
_cogl_vertex_attribute_free (CoglVertexAttribute *attribute)
{
  g_free (attribute->name);
  cogl_object_unref (attribute->array);

  g_slice_free (CoglVertexAttribute, attribute);
}

typedef struct
{
  int unit;
  CoglPipelineFlushOptions options;
  guint32 fallback_layers;
} ValidateLayerState;

static gboolean
validate_layer_cb (CoglPipeline *pipeline,
                   int layer_index,
                   void *user_data)
{
  CoglHandle texture =
    _cogl_pipeline_get_layer_texture (pipeline, layer_index);
  ValidateLayerState *state = user_data;
  gboolean status = TRUE;

  /* invalid textures will be handled correctly in
   * _cogl_pipeline_flush_layers_gl_state */
  if (texture == COGL_INVALID_HANDLE)
    goto validated;

  /* Give the texture a chance to know that we're rendering
     non-quad shaped primitives. If the texture is in an atlas it
     will be migrated */
  _cogl_texture_ensure_non_quad_rendering (texture);

  /* We need to ensure the mipmaps are ready before deciding
   * anything else about the texture because the texture storate
   * could completely change if it needs to be migrated out of the
   * atlas and will affect how we validate the layer.
   */
  _cogl_pipeline_pre_paint_for_layer (pipeline, layer_index);

  if (!_cogl_texture_can_hardware_repeat (texture))
    {
      g_warning ("Disabling layer %d of the current source material, "
                 "because texturing with the vertex buffer API is not "
                 "currently supported using sliced textures, or textures "
                 "with waste\n", layer_index);

      /* XXX: maybe we can add a mechanism for users to forcibly use
       * textures with waste where it would be their responsability to use
       * texture coords in the range [0,1] such that sampling outside isn't
       * required. We can then use a texture matrix (or a modification of
       * the users own matrix) to map 1 to the edge of the texture data.
       *
       * Potentially, given the same guarantee as above we could also
       * support a single sliced layer too. We would have to redraw the
       * vertices once for each layer, each time with a fiddled texture
       * matrix.
       */
      state->fallback_layers |= (1 << state->unit);
      state->options.flags |= COGL_PIPELINE_FLUSH_FALLBACK_MASK;
    }

validated:
  state->unit++;
  return status;
}

static CoglHandle
enable_gl_state (CoglVertexAttribute **attributes,
                 ValidateLayerState *state)
{
  int i;
#ifdef MAY_HAVE_PROGRAMABLE_GL
  GLuint generic_index = 0;
#endif
  unsigned long enable_flags = 0;
  gboolean skip_gl_color = FALSE;
  CoglPipeline *source;
  CoglPipeline *copy = NULL;

  _COGL_GET_CONTEXT (ctx, COGL_INVALID_HANDLE);

  source = cogl_get_source ();

  _cogl_bitmask_clear_all (&ctx->temp_bitmask);

  for (i = 0; attributes[i]; i++)
    {
      CoglVertexAttribute *attribute = attributes[i];
      CoglVertexArray *vertex_array;
      CoglBuffer *buffer;
      void *base;

      vertex_array = cogl_vertex_attribute_get_array (attribute);
      buffer = COGL_BUFFER (vertex_array);
      base = _cogl_buffer_bind (buffer, COGL_BUFFER_BIND_TARGET_VERTEX_ARRAY);

      switch (attribute->name_id)
        {
        case COGL_VERTEX_ATTRIBUTE_NAME_ID_COLOR_ARRAY:
          enable_flags |= COGL_ENABLE_COLOR_ARRAY;
          /* GE (glEnableClientState (GL_COLOR_ARRAY)); */
          GE (glColorPointer (attribute->n_components,
                              attribute->type,
                              attribute->stride,
                              base + attribute->offset));

          if (!_cogl_pipeline_get_real_blend_enabled (source))
            {
              CoglPipelineBlendEnable blend_enable =
                COGL_PIPELINE_BLEND_ENABLE_ENABLED;
              copy = cogl_pipeline_copy (source);
              _cogl_pipeline_set_blend_enabled (copy, blend_enable);
              source = copy;
            }
          skip_gl_color = TRUE;
          break;
        case COGL_VERTEX_ATTRIBUTE_NAME_ID_NORMAL_ARRAY:
          /* FIXME: go through cogl cache to enable normal array */
          GE (glEnableClientState (GL_NORMAL_ARRAY));
          GE (glNormalPointer (attribute->type,
                               attribute->stride,
                               base + attribute->offset));
          break;
        case COGL_VERTEX_ATTRIBUTE_NAME_ID_TEXTURE_COORD_ARRAY:
          GE (glClientActiveTexture (GL_TEXTURE0 +
                                     attribute->texture_unit));
          GE (glEnableClientState (GL_TEXTURE_COORD_ARRAY));
          GE (glTexCoordPointer (attribute->n_components,
                                 attribute->type,
                                 attribute->stride,
                                 base + attribute->offset));
          _cogl_bitmask_set (&ctx->temp_bitmask,
                             attribute->texture_unit, TRUE);
          break;
        case COGL_VERTEX_ATTRIBUTE_NAME_ID_POSITION_ARRAY:
          enable_flags |= COGL_ENABLE_VERTEX_ARRAY;
          /* GE (glEnableClientState (GL_VERTEX_ARRAY)); */
          GE (glVertexPointer (attribute->n_components,
                               attribute->type,
                               attribute->stride,
                               base + attribute->offset));
          break;
        case COGL_VERTEX_ATTRIBUTE_NAME_ID_CUSTOM_ARRAY:
          {
#ifdef MAY_HAVE_PROGRAMABLE_GL
            /* FIXME: go through cogl cache to enable generic array */
            GE (glEnableVertexAttribArray (generic_index++));
            GE (glVertexAttribPointer (generic_index,
                                       attribute->n_components,
                                       attribute->type,
                                       attribute->normalized,
                                       attribute->stride,
                                       base + attribute->offset));
#endif
          }
          break;
        default:
          g_warning ("Unrecognised attribute type 0x%08x", attribute->type);
        }

      _cogl_buffer_unbind (buffer);
    }

  /* Disable any tex coord arrays that we didn't use */
  _cogl_disable_other_texcoord_arrays (&ctx->temp_bitmask);

  if (G_UNLIKELY (state->options.flags))
    {
      /* If we haven't already created a derived pipeline... */
      if (!copy)
        {
          copy = cogl_pipeline_copy (source);
          source = copy;
        }
      _cogl_pipeline_apply_overrides (source, &state->options);

      /* TODO:
       * overrides = cogl_pipeline_get_data (pipeline,
       *                                     last_overrides_key);
       * if (overrides)
       *   {
       *     age = cogl_pipeline_get_age (pipeline);
       *     XXX: actually we also need to check for legacy_state
       *     and blending overrides for use of glColorPointer...
       *     if (overrides->ags != age ||
       *         memcmp (&overrides->options, &options,
       *                 sizeof (options) != 0)
       *       {
       *         cogl_object_unref (overrides->weak_pipeline);
       *         g_slice_free (Overrides, overrides);
       *         overrides = NULL;
       *       }
       *   }
       * if (!overrides)
       *   {
       *     overrides = g_slice_new (Overrides);
       *     overrides->weak_pipeline =
       *       cogl_pipeline_weak_copy (cogl_get_source ());
       *     _cogl_pipeline_apply_overrides (overrides->weak_pipeline,
       *                                     &options);
       *
       *     cogl_pipeline_set_data (pipeline, last_overrides_key,
       *                             weak_overrides,
       *                             free_overrides_cb,
       *                             NULL);
       *   }
       * source = overrides->weak_pipeline;
       */
    }

  if (G_UNLIKELY (ctx->legacy_state_set))
    {
      /* If we haven't already created a derived pipeline... */
      if (!copy)
        {
          copy = cogl_pipeline_copy (source);
          source = copy;
        }
      _cogl_pipeline_apply_legacy_state (source);
    }

  _cogl_pipeline_flush_gl_state (source, skip_gl_color);

  if (ctx->enable_backface_culling)
    enable_flags |= COGL_ENABLE_BACKFACE_CULLING;

  _cogl_enable (enable_flags);
  _cogl_flush_face_winding ();

  return source;
}

/* FIXME: we shouldn't be disabling state after drawing we should
 * just disable the things not needed after enabling state. */
static void
disable_gl_state (CoglVertexAttribute **attributes,
                  CoglPipeline *source)
{
#ifdef MAY_HAVE_PROGRAMABLE_GL
  GLuint generic_index = 0;
#endif
  int i;

  _COGL_GET_CONTEXT (ctx, NO_RETVAL);

  if (G_UNLIKELY (source != cogl_get_source ()))
    cogl_object_unref (source);

  for (i = 0; attributes[i]; i++)
    {
      CoglVertexAttribute *attribute = attributes[i];

      switch (attribute->name_id)
        {
        case COGL_VERTEX_ATTRIBUTE_NAME_ID_COLOR_ARRAY:
          /* GE (glDisableClientState (GL_COLOR_ARRAY)); */
          break;
        case COGL_VERTEX_ATTRIBUTE_NAME_ID_NORMAL_ARRAY:
          /* FIXME: go through cogl cache to enable normal array */
          GE (glDisableClientState (GL_NORMAL_ARRAY));
          break;
        case COGL_VERTEX_ATTRIBUTE_NAME_ID_TEXTURE_COORD_ARRAY:
          /* The enabled state of the texture coord arrays is
             cached in ctx->enabled_texcoord_arrays so we don't
             need to do anything here. The array will be disabled
             by the next drawing primitive if it is not
             required */
          break;
        case COGL_VERTEX_ATTRIBUTE_NAME_ID_POSITION_ARRAY:
          /* GE (glDisableClientState (GL_VERTEX_ARRAY)); */
          break;
        case COGL_VERTEX_ATTRIBUTE_NAME_ID_CUSTOM_ARRAY:
#ifdef MAY_HAVE_PROGRAMABLE_GL
          /* FIXME: go through cogl cache to enable generic array */
          GE (glDisableVertexAttribArray (generic_index++));
#endif
          break;
        default:
          g_warning ("Unrecognised attribute type 0x%08x", attribute->type);
        }
    }
}

#ifdef COGL_ENABLE_DEBUG
static int
get_index (void *indices,
           CoglIndicesType type,
           int _index)
{
  if (!indices)
    return _index;

  switch (type)
    {
    case COGL_INDICES_TYPE_UNSIGNED_BYTE:
      return ((guint8 *)indices)[_index];
    case COGL_INDICES_TYPE_UNSIGNED_SHORT:
      return ((guint16 *)indices)[_index];
    case COGL_INDICES_TYPE_UNSIGNED_INT:
      return ((guint32 *)indices)[_index];
    }

  g_return_val_if_reached (0);
}

static void
add_line (void *vertices,
          void *indices,
          CoglIndicesType indices_type,
          CoglVertexAttribute *attribute,
          int start,
          int end,
          CoglP3Vertex *lines,
          int *n_line_vertices)
{
  int start_index = get_index (indices, indices_type, start);
  int end_index = get_index (indices, indices_type, end);
  float *v0 = (float *)((guint8 *)vertices + start_index * attribute->stride);
  float *v1 = (float *)((guint8 *)vertices + end_index * attribute->stride);
  float *o = (float *)(&lines[*n_line_vertices]);
  int i;

  for (i = 0; i < attribute->n_components; i++)
    *(o++) = *(v0++);
  for (;i < 3; i++)
    *(o++) = 0;

  for (i = 0; i < attribute->n_components; i++)
    *(o++) = *(v1++);
  for (;i < 3; i++)
    *(o++) = 0;

  *n_line_vertices += 2;
}

static CoglP3Vertex *
get_wire_lines (CoglVertexAttribute *attribute,
                CoglVerticesMode mode,
                int n_vertices_in,
                int *n_vertices_out,
                CoglIndices *_indices)
{
  CoglVertexArray *vertex_array = cogl_vertex_attribute_get_array (attribute);
  void *vertices;
  CoglIndexArray *index_array;
  void *indices;
  CoglIndicesType indices_type;
  int i;
  int n_lines;
  CoglP3Vertex *out;

  vertices = cogl_buffer_map (COGL_BUFFER (vertex_array),
                              COGL_BUFFER_ACCESS_READ, 0);
  if (_indices)
    {
      index_array = cogl_indices_get_array (_indices);
      indices = cogl_buffer_map (COGL_BUFFER (index_array),
                                 COGL_BUFFER_ACCESS_READ, 0);
      indices_type = cogl_indices_get_type (_indices);
    }
  else
    indices = NULL;

  *n_vertices_out = 0;

  if (mode == COGL_VERTICES_MODE_TRIANGLES &&
      (n_vertices_in % 3) == 0)
    {
      n_lines = n_vertices_in;
      out = g_new (CoglP3Vertex, n_lines * 2);
      for (i = 0; i < n_vertices_in; i += 3)
        {
          add_line (vertices, indices, indices_type, attribute,
                    i, i+1, out, n_vertices_out);
          add_line (vertices, indices, indices_type, attribute,
                    i+1, i+2, out, n_vertices_out);
          add_line (vertices, indices, indices_type, attribute,
                    i+2, i, out, n_vertices_out);
        }
    }
  else if (mode == COGL_VERTICES_MODE_TRIANGLE_FAN &&
           n_vertices_in >= 3)
    {
      n_lines = 2 * n_vertices_in - 3;
      out = g_new (CoglP3Vertex, n_lines * 2);

      add_line (vertices, indices, indices_type, attribute,
                0, 1, out, n_vertices_out);
      add_line (vertices, indices, indices_type, attribute,
                1, 2, out, n_vertices_out);
      add_line (vertices, indices, indices_type, attribute,
                0, 2, out, n_vertices_out);

      for (i = 3; i < n_vertices_in; i++)
        {
          add_line (vertices, indices, indices_type, attribute,
                    i - 1, i, out, n_vertices_out);
          add_line (vertices, indices, indices_type, attribute,
                    0, i, out, n_vertices_out);
        }
    }
  else if (mode == COGL_VERTICES_MODE_TRIANGLE_STRIP &&
           n_vertices_in >= 3)
    {
      n_lines = 2 * n_vertices_in - 3;
      out = g_new (CoglP3Vertex, n_lines * 2);

      add_line (vertices, indices, indices_type, attribute,
                0, 1, out, n_vertices_out);
      add_line (vertices, indices, indices_type, attribute,
                1, 2, out, n_vertices_out);
      add_line (vertices, indices, indices_type, attribute,
                0, 2, out, n_vertices_out);

      for (i = 3; i < n_vertices_in; i++)
        {
          add_line (vertices, indices, indices_type, attribute,
                    i - 1, i, out, n_vertices_out);
          add_line (vertices, indices, indices_type, attribute,
                    i - 2, i, out, n_vertices_out);
        }
    }
    /* In the journal we are a bit sneaky and actually use GL_QUADS
     * which isn't actually a valid CoglVerticesMode! */
#ifdef HAVE_COGL_GL
  else if (mode == GL_QUADS && (n_vertices_in % 4) == 0)
    {
      n_lines = n_vertices_in;
      out = g_new (CoglP3Vertex, n_lines * 2);

      for (i = 0; i < n_vertices_in; i += 4)
        {
          add_line (vertices, indices, indices_type, attribute,
                    i, i + 1, out, n_vertices_out);
          add_line (vertices, indices, indices_type, attribute,
                    i + 1, i + 2, out, n_vertices_out);
          add_line (vertices, indices, indices_type, attribute,
                    i + 2, i + 3, out, n_vertices_out);
          add_line (vertices, indices, indices_type, attribute,
                    i + 3, i, out, n_vertices_out);
        }
    }
#endif

  if (vertices)
    cogl_buffer_unmap (COGL_BUFFER (vertex_array));
  if (indices)
    cogl_buffer_unmap (COGL_BUFFER (index_array));
  return out;
}

static void
draw_wireframe (CoglVerticesMode mode,
                int first_vertex,
                int n_vertices,
                CoglVertexAttribute **attributes,
                CoglIndices *indices)
{
  CoglVertexAttribute *position = NULL;
  int i;
  int n_line_vertices;
  static CoglPipeline *wire_pipeline;
  CoglVertexAttribute *wire_attribute[2];
  CoglP3Vertex *lines;
  CoglVertexArray *array;

  for (i = 0; attributes[i]; i++)
    {
      if (strcmp (attributes[i]->name, "cogl_position_in") == 0)
        {
          position = attributes[i];
          break;
        }
    }
  if (!position)
    return;

  lines = get_wire_lines (position,
                          mode,
                          n_vertices,
                          &n_line_vertices,
                          indices);
  array = cogl_vertex_array_new (sizeof (CoglP3Vertex) * n_line_vertices,
                                 lines);
  wire_attribute[0] =
    cogl_vertex_attribute_new (array, "cogl_position_in",
                               sizeof (CoglP3Vertex),
                               0,
                               3,
                               COGL_VERTEX_ATTRIBUTE_TYPE_FLOAT);
  wire_attribute[1] = NULL;
  cogl_object_unref (array);

  if (!wire_pipeline)
    {
      wire_pipeline = cogl_pipeline_new ();
      cogl_pipeline_set_color4ub (wire_pipeline,
                                  0x00, 0xff, 0x00, 0xff);
    }

  cogl_push_source (wire_pipeline);

  /* temporarily disable the wireframe to avoid recursion! */
  cogl_debug_flags &= ~COGL_DEBUG_WIREFRAME;
  _cogl_draw_vertex_attributes_array (COGL_VERTICES_MODE_LINES,
                                      0,
                                      n_line_vertices,
                                      wire_attribute);
  cogl_debug_flags |= COGL_DEBUG_WIREFRAME;

  cogl_pop_source ();

  cogl_object_unref (wire_attribute[0]);
}
#endif

static void
_cogl_draw_vertex_attributes_array_real (CoglVerticesMode mode,
                                         int first_vertex,
                                         int n_vertices,
                                         CoglVertexAttribute **attributes,
                                         ValidateLayerState *state)
{
  CoglPipeline *source = enable_gl_state (attributes, state);

  GE (glDrawArrays ((GLenum)mode, first_vertex, n_vertices));

  /* FIXME: we shouldn't be disabling state after drawing we should
   * just disable the things not needed after enabling state. */
  disable_gl_state (attributes, source);

#ifdef COGL_ENABLE_DEBUG
  if (G_UNLIKELY (cogl_debug_flags & COGL_DEBUG_WIREFRAME))
    draw_wireframe (mode, first_vertex, n_vertices, attributes, NULL);
#endif
}

/* This can be used by the CoglJournal to draw attributes skipping the
 * implicit journal flush, the framebuffer flush and pipeline
 * validation. */
void
_cogl_draw_vertex_attributes_array (CoglVerticesMode mode,
                                    int first_vertex,
                                    int n_vertices,
                                    CoglVertexAttribute **attributes)
{
  ValidateLayerState state;

  state.unit = 0;
  state.options.flags = 0;
  state.fallback_layers = 0;

  _cogl_draw_vertex_attributes_array_real (mode, first_vertex, n_vertices,
                                           attributes, &state);
}

void
cogl_draw_vertex_attributes_array (CoglVerticesMode mode,
                                   int first_vertex,
                                   int n_vertices,
                                   CoglVertexAttribute **attributes)
{
  ValidateLayerState state;

  _COGL_GET_CONTEXT (ctx, NO_RETVAL);

  _cogl_journal_flush ();

  state.unit = 0;
  state.options.flags = 0;
  state.fallback_layers = 0;

  cogl_pipeline_foreach_layer (cogl_get_source (),
                               validate_layer_cb,
                               &state);

  /* NB: _cogl_framebuffer_flush_state may disrupt various state (such
   * as the pipeline state) when flushing the clip stack, so should
   * always be done first when preparing to draw. We need to do this
   * before setting up the array pointers because setting up the clip
   * stack can cause some drawing which would change the array
   * pointers. */
  _cogl_framebuffer_flush_state (_cogl_get_framebuffer (), 0);

  _cogl_draw_vertex_attributes_array_real (mode, first_vertex, n_vertices,
                                           attributes, &state);
}

void
cogl_draw_vertex_attributes (CoglVerticesMode mode,
                             int first_vertex,
                             int n_vertices,
                             ...)
{
  va_list ap;
  int n_attributes;
  CoglVertexAttribute *attribute;
  CoglVertexAttribute **attributes;
  int i;

  va_start (ap, n_vertices);
  for (n_attributes = 0; va_arg (ap, CoglVertexAttribute *); n_attributes++)
    ;
  va_end (ap);

  attributes = g_alloca (sizeof (CoglVertexAttribute *) * (n_attributes + 1));
  attributes[n_attributes] = NULL;

  va_start (ap, n_vertices);
  for (i = 0; (attribute = va_arg (ap, CoglVertexAttribute *)); i++)
    attributes[i] = attribute;
  va_end (ap);

  cogl_draw_vertex_attributes_array (mode, first_vertex, n_vertices,
                                     attributes);
}

static size_t
sizeof_index_type (CoglIndicesType type)
{
  switch (type)
    {
    case COGL_INDICES_TYPE_UNSIGNED_BYTE:
      return 1;
    case COGL_INDICES_TYPE_UNSIGNED_SHORT:
      return 2;
    case COGL_INDICES_TYPE_UNSIGNED_INT:
      return 4;
    }
  g_return_val_if_reached (0);
}

static void
_cogl_draw_indexed_vertex_attributes_array_real (CoglVerticesMode mode,
                                                 int first_vertex,
                                                 int n_vertices,
                                                 CoglIndices *indices,
                                                 CoglVertexAttribute **attributes,
                                                 ValidateLayerState *state)
{
  CoglPipeline *source = enable_gl_state (attributes, state);
  CoglBuffer *buffer;
  void *base;
  size_t array_offset;
  size_t index_size;
  GLenum indices_gl_type = 0;

  _COGL_GET_CONTEXT (ctx, NO_RETVAL);

  buffer = COGL_BUFFER (cogl_indices_get_array (indices));
  base = _cogl_buffer_bind (buffer, COGL_BUFFER_BIND_TARGET_INDEX_ARRAY);
  array_offset = cogl_indices_get_offset (indices);
  index_size = sizeof_index_type (cogl_indices_get_type (indices));

  switch (cogl_indices_get_type (indices))
    {
    case COGL_INDICES_TYPE_UNSIGNED_BYTE:
      indices_gl_type = GL_UNSIGNED_BYTE;
      break;
    case COGL_INDICES_TYPE_UNSIGNED_SHORT:
      indices_gl_type = GL_UNSIGNED_SHORT;
      break;
    case COGL_INDICES_TYPE_UNSIGNED_INT:
      indices_gl_type = GL_UNSIGNED_INT;
      break;
    }

  GE (glDrawElements ((GLenum)mode,
                      n_vertices,
                      indices_gl_type,
                      base + array_offset + index_size * first_vertex));

  _cogl_buffer_unbind (buffer);

  /* FIXME: we shouldn't be disabling state after drawing we should
   * just disable the things not needed after enabling state. */
  disable_gl_state (attributes, source);

#ifdef COGL_ENABLE_DEBUG
  if (G_UNLIKELY (cogl_debug_flags & COGL_DEBUG_WIREFRAME))
    draw_wireframe (mode, first_vertex, n_vertices, attributes, indices);
#endif
}

void
_cogl_draw_indexed_vertex_attributes_array (CoglVerticesMode mode,
                                            int first_vertex,
                                            int n_vertices,
                                            CoglIndices *indices,
                                            CoglVertexAttribute **attributes)
{
  ValidateLayerState state;

  state.unit = 0;
  state.options.flags = 0;
  state.fallback_layers = 0;

  _cogl_draw_indexed_vertex_attributes_array_real (mode,
                                                   first_vertex,
                                                   n_vertices,
                                                   indices,
                                                   attributes,
                                                   &state);
}

void
cogl_draw_indexed_vertex_attributes_array (CoglVerticesMode mode,
                                           int first_vertex,
                                           int n_vertices,
                                           CoglIndices *indices,
                                           CoglVertexAttribute **attributes)
{
  ValidateLayerState state;

  _COGL_GET_CONTEXT (ctx, NO_RETVAL);

  _cogl_journal_flush ();

  state.unit = 0;
  state.options.flags = 0;
  state.fallback_layers = 0;

  cogl_pipeline_foreach_layer (cogl_get_source (),
                               validate_layer_cb,
                               &state);

  /* NB: _cogl_framebuffer_flush_state may disrupt various state (such
   * as the pipeline state) when flushing the clip stack, so should
   * always be done first when preparing to draw. We need to do this
   * before setting up the array pointers because setting up the clip
   * stack can cause some drawing which would change the array
   * pointers. */
  _cogl_framebuffer_flush_state (_cogl_get_framebuffer (), 0);

  _cogl_draw_indexed_vertex_attributes_array_real (mode,
                                                   first_vertex,
                                                   n_vertices,
                                                   indices,
                                                   attributes,
                                                   &state);
}

void
cogl_draw_indexed_vertex_attributes (CoglVerticesMode mode,
                                     int first_vertex,
                                     int n_vertices,
                                     CoglIndices *indices,
                                     ...)
{
  va_list ap;
  int n_attributes;
  CoglVertexAttribute **attributes;
  int i;
  CoglVertexAttribute *attribute;

  va_start (ap, indices);
  for (n_attributes = 0; va_arg (ap, CoglVertexAttribute *); n_attributes++)
    ;
  va_end (ap);

  attributes = g_alloca (sizeof (CoglVertexAttribute *) * (n_attributes + 1));
  attributes[n_attributes] = NULL;

  va_start (ap, indices);
  for (i = 0; (attribute = va_arg (ap, CoglVertexAttribute *)); i++)
    attributes[i] = attribute;
  va_end (ap);

  cogl_draw_indexed_vertex_attributes_array (mode,
                                             first_vertex,
                                             n_vertices,
                                             indices,
                                             attributes);
}

