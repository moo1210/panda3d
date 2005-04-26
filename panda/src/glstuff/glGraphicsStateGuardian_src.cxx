// Filename: glGraphicsStateGuardian_src.cxx
// Created by:  drose (02Feb99)
//
////////////////////////////////////////////////////////////////////
//
// PANDA 3D SOFTWARE
// Copyright (c) 2001 - 2004, Disney Enterprises, Inc.  All rights reserved
//
// All use of this software is subject to the terms of the Panda 3d
// Software license.  You should have received a copy of this license
// along with this source code; you will also find a current copy of
// the license at http://etc.cmu.edu/panda3d/docs/license/ .
//
// To contact the maintainers of this program write to
// panda3d-general@lists.sourceforge.net .
//
////////////////////////////////////////////////////////////////////

#include "config_util.h"
#include "displayRegion.h"
#include "renderBuffer.h"
#include "geom.h"
#include "geomIssuer.h"
#include "qpgeomVertexData.h"
#include "qpgeomTriangles.h"
#include "qpgeomTristrips.h"
#include "qpgeomTrifans.h"
#include "qpgeomLines.h"
#include "qpgeomLinestrips.h"
#include "qpgeomPoints.h"
#include "qpgeomVertexReader.h"
#include "graphicsWindow.h"
#include "lens.h"
#include "perspectiveLens.h"
#include "directionalLight.h"
#include "pointLight.h"
#include "spotlight.h"
#include "planeNode.h"
#include "textureAttrib.h"
#include "lightAttrib.h"
#include "cullFaceAttrib.h"
#include "transparencyAttrib.h"
#include "alphaTestAttrib.h"
#include "depthTestAttrib.h"
#include "depthWriteAttrib.h"
#include "colorWriteAttrib.h"
#include "texMatrixAttrib.h"
#include "texGenAttrib.h"
#include "materialAttrib.h"
#include "renderModeAttrib.h"
#include "rescaleNormalAttrib.h"
#include "fogAttrib.h"
#include "depthOffsetAttrib.h"
#include "shadeModelAttrib.h"
#include "fog.h"
#include "clockObject.h"
#include "string_utils.h"
#include "nodePath.h"
#include "dcast.h"
#include "pvector.h"
#include "vector_string.h"
#include "string_utils.h"
#include "pnmImage.h"
#include "config_gobj.h"
#include "mutexHolder.h"
#include "indirectLess.h"
#ifdef DO_PSTATS
#include "pStatTimer.h"
#endif
#ifdef HAVE_CGGL
#include "cgShaderAttrib.h"
#endif

#include <algorithm>

TypeHandle CLP(GraphicsStateGuardian)::_type_handle;

PStatCollector CLP(GraphicsStateGuardian)::_load_display_list_pcollector("Draw:Transfer data:Display lists");
PStatCollector CLP(GraphicsStateGuardian)::_primitive_batches_display_list_pcollector("Primitive batches:Display lists");
PStatCollector CLP(GraphicsStateGuardian)::_vertices_display_list_pcollector("Vertices:Display lists");

static void
issue_vertex_gl(const Geom *geom, Geom::VertexIterator &viterator, 
                GraphicsStateGuardianBase *) {
  const Vertexf &vertex = geom->get_next_vertex(viterator);
  // GLCAT.spam() << "Issuing vertex " << vertex << "\n";
  GLP(Vertex3fv)(vertex.get_data());
}

static void
issue_normal_gl(const Geom *geom, Geom::NormalIterator &niterator, 
                GraphicsStateGuardianBase *) {
  const Normalf &normal = geom->get_next_normal(niterator);
  // GLCAT.spam() << "Issuing normal " << normal << "\n";
  GLP(Normal3fv)(normal.get_data());
}

static void
issue_texcoord_single_gl(const Geom *geom, 
                         Geom::MultiTexCoordIterator &tciterator, 
                         GraphicsStateGuardianBase *) {
  const TexCoordf &texcoord = geom->get_next_multitexcoord(tciterator, 0);
  // GLCAT.spam() << "Issuing texcoord " << texcoord << " on unit 0 (single-texture mode)\n";
  GLP(TexCoord2fv)(texcoord.get_data());
}

static void
issue_texcoord_multi_gl(const Geom *geom, 
                        Geom::MultiTexCoordIterator &tciterator, 
                        GraphicsStateGuardianBase *gsgbase) {
  // We avoid DCAST here because we don't really need it, and it's
  // nice not to have to pay that overhead on each vertex.
  CLP(GraphicsStateGuardian) *gsg = (CLP(GraphicsStateGuardian) *)gsgbase;
  for (int i = 0; i < tciterator._num_stages; i++) {
    const TexCoordf &texcoord = geom->get_next_multitexcoord(tciterator, i);
    int stage_index = tciterator._stage_index[i];
    // GLCAT.spam() << "Issuing texcoord " << texcoord << " on unit " << stage_index << "\n";
    gsg->_glMultiTexCoord2fv(GL_TEXTURE0 + stage_index, texcoord.get_data());
  }
}

static void
issue_color_gl(const Geom *geom, Geom::ColorIterator &citerator,
               GraphicsStateGuardianBase *) {
  const Colorf &color = geom->get_next_color(citerator);
  //  GLCAT.spam() << "Issuing color " << color << "\n";
  GLP(Color4fv)(color.get_data());
}

static void
issue_scaled_color_gl(const Geom *geom, Geom::ColorIterator &citerator,
                           GraphicsStateGuardianBase *gsg) {
  const CLP(GraphicsStateGuardian) *glgsg = DCAST(CLP(GraphicsStateGuardian), gsg);
  const Colorf &color = geom->get_next_color(citerator);
  glgsg->issue_scaled_color(color);
}

// The following noop functions are assigned to the corresponding
// glext function pointers in the class, in case the functions are not
// defined by the GL, just so it will always be safe to call the
// extension functions.

static void APIENTRY
null_glPointParameterfv(GLenum, const GLfloat *) {
}

static void APIENTRY
null_glDrawRangeElements(GLenum mode, GLuint start, GLuint end, 
                         GLsizei count, GLenum type, const GLvoid *indices) {
  // If we don't support glDrawRangeElements(), just use the original
  // glDrawElements() instead.
  GLP(DrawElements)(mode, count, type, indices);
}

static void APIENTRY
null_glActiveTexture(GLenum gl_texture_stage) {
  // If we don't support multitexture, we'd better not try to request
  // a texture beyond the first texture stage.
  nassertv(gl_texture_stage == GL_TEXTURE0);
}

static void APIENTRY
null_glBlendEquation(GLenum) {
}

static void APIENTRY
null_glBlendColor(GLclampf, GLclampf, GLclampf, GLclampf) {
}

////////////////////////////////////////////////////////////////////
//     Function: uchar_bgr_to_rgb
//  Description: Recopies the given array of pixels, converting from
//               BGR to RGB arrangement.
////////////////////////////////////////////////////////////////////
static void
uchar_bgr_to_rgb(unsigned char *dest, const unsigned char *source, 
                 int num_pixels) {
  for (int i = 0; i < num_pixels; i++) {
    dest[0] = source[2];
    dest[1] = source[1];
    dest[2] = source[0];
    dest += 3;
    source += 3;
  }
}

////////////////////////////////////////////////////////////////////
//     Function: uchar_bgra_to_rgba
//  Description: Recopies the given array of pixels, converting from
//               BGRA to RGBA arrangement.
////////////////////////////////////////////////////////////////////
static void
uchar_bgra_to_rgba(unsigned char *dest, const unsigned char *source, 
                   int num_pixels) {
  for (int i = 0; i < num_pixels; i++) {
    dest[0] = source[2];
    dest[1] = source[1];
    dest[2] = source[0];
    dest[3] = source[3];
    dest += 4;
    source += 4;
  }
}

////////////////////////////////////////////////////////////////////
//     Function: ushort_bgr_to_rgb
//  Description: Recopies the given array of pixels, converting from
//               BGR to RGB arrangement.
////////////////////////////////////////////////////////////////////
static void
ushort_bgr_to_rgb(unsigned short *dest, const unsigned short *source, 
                  int num_pixels) {
  for (int i = 0; i < num_pixels; i++) {
    dest[0] = source[2];
    dest[1] = source[1];
    dest[2] = source[0];
    dest += 3;
    source += 3;
  }
}

////////////////////////////////////////////////////////////////////
//     Function: ushort_bgra_to_rgba
//  Description: Recopies the given array of pixels, converting from
//               BGRA to RGBA arrangement.
////////////////////////////////////////////////////////////////////
static void
ushort_bgra_to_rgba(unsigned short *dest, const unsigned short *source, 
                    int num_pixels) {
  for (int i = 0; i < num_pixels; i++) {
    dest[0] = source[2];
    dest[1] = source[1];
    dest[2] = source[0];
    dest[3] = source[3];
    dest += 4;
    source += 4;
  }
}

////////////////////////////////////////////////////////////////////
//     Function: reduce_image
//  Description: Reduces an image to an acceptable size by sampling
//               pixels.  This is just a cheap and dirty trick to load
//               an image that the GL says is too large.
//
//               The implementation copies byte_chunk bytes every
//               byte_chunk * stride bytes.
////////////////////////////////////////////////////////////////////
static PTA_uchar
reduce_image(CPTA_uchar orig_image, int byte_chunk, int stride) {
  size_t orig_image_size = orig_image.size();
  size_t new_image_size = orig_image_size / stride;
  PTA_uchar new_image = PTA_uchar::empty_array(new_image_size);
  const unsigned char *from = orig_image.p();
  unsigned char *to = new_image.p();

  const unsigned char *from_end = from + orig_image_size;
  const unsigned char *to_end = to + new_image_size;

  while (from + byte_chunk <= from_end) {
    nassertr(to + byte_chunk <= to_end, new_image);
    memcpy(to, from, byte_chunk);
    from += stride * byte_chunk;
    to += byte_chunk;
  }

  return new_image;
}

////////////////////////////////////////////////////////////////////
//     Function: fix_component_ordering
//  Description: Reverses the order of the components within the
//               image, to convert (for instance) GL_BGR to GL_RGB.
//               Returns the CPTA_uchar representing the converted
//               image, or the original image if it is unchanged.
////////////////////////////////////////////////////////////////////
static PTA_uchar
fix_component_ordering(CPTA_uchar orig_image, GLenum external_format, 
                       Texture *tex) {
  size_t orig_image_size = orig_image.size();
  PTA_uchar new_image = (PTA_uchar &)orig_image;

  switch (external_format) {
  case GL_RGB:
    switch (tex->get_component_type()) {
    case Texture::T_unsigned_byte:
      new_image = PTA_uchar::empty_array(orig_image_size);
      uchar_bgr_to_rgb(new_image, orig_image, orig_image_size / 3);
      break;

    case Texture::T_unsigned_short:
      new_image = PTA_uchar::empty_array(orig_image_size);
      ushort_bgr_to_rgb((unsigned short *)new_image.p(), 
                        (const unsigned short *)orig_image.p(), 
                        orig_image_size / 6);
      break;

    default:
      break;
    }
    break;

  case GL_RGBA:
    switch (tex->get_component_type()) {
    case Texture::T_unsigned_byte:
      new_image = PTA_uchar::empty_array(orig_image_size);
      uchar_bgra_to_rgba(new_image, orig_image, orig_image_size / 4);
      break;

    case Texture::T_unsigned_short:
      new_image = PTA_uchar::empty_array(orig_image_size);
      ushort_bgra_to_rgba((unsigned short *)new_image.p(), 
                          (const unsigned short *)orig_image.p(), 
                          orig_image_size / 8);
      break;

    default:
      break;
    }
    break;

  default:
    break;
  }

  return new_image;
}

////////////////////////////////////////////////////////////////////
//     Function: GLGraphicsStateGuardian::Constructor
//       Access: Public
//  Description:
////////////////////////////////////////////////////////////////////
CLP(GraphicsStateGuardian)::
CLP(GraphicsStateGuardian)(const FrameBufferProperties &properties) :
  GraphicsStateGuardian(properties, CS_yup_right) 
{
  _error_count = 0;
#ifdef HAVE_CGGL
  _cg_shader = (CgShader *)NULL;
#endif
}

////////////////////////////////////////////////////////////////////
//     Function: GLGraphicsStateGuardian::Destructor
//       Access: Public
//  Description:
////////////////////////////////////////////////////////////////////
CLP(GraphicsStateGuardian)::
~CLP(GraphicsStateGuardian)() {
  close_gsg();
}

////////////////////////////////////////////////////////////////////
//     Function: GLGraphicsStateGuardian::reset
//       Access: Public, Virtual
//  Description: Resets all internal state as if the gsg were newly
//               created.
////////////////////////////////////////////////////////////////////
void CLP(GraphicsStateGuardian)::
reset() {
  free_pointers();
  GraphicsStateGuardian::reset();

  // Output the vendor and version strings.
  get_gl_version();

  // Save the extensions tokens.
  save_extensions((const char *)GLP(GetString)(GL_EXTENSIONS));
  get_extra_extensions();
  report_extensions();

  _supported_geom_rendering = 
    qpGeom::GR_indexed_point |
    qpGeom::GR_point | qpGeom::GR_point_uniform_size |
    qpGeom::GR_triangle_strip | qpGeom::GR_triangle_fan |
    qpGeom::GR_flat_last_vertex;

  _supports_point_parameters = false;

  if (is_at_least_version(1, 4)) {
    _supports_point_parameters = true;
    _glPointParameterfv = (PFNGLPOINTPARAMETERFVPROC)
      get_extension_func(GLPREFIX_QUOTED, "PointParameterfv");

  } else if (has_extension("GL_ARB_point_parameters")) {
    _supports_point_parameters = true;
    _glPointParameterfv = (PFNGLPOINTPARAMETERFVPROC)
      get_extension_func(GLPREFIX_QUOTED, "PointParameterfvARB");
  }
  if (_supports_point_parameters) {
    if (_glPointParameterfv == NULL) {
      GLCAT.warning()
        << "glPointParameterfv advertised as supported by OpenGL runtime, but could not get pointers to extension functions.\n";
      _supports_point_parameters = false;
    }
  }
  if (_supports_point_parameters) {
    _supported_geom_rendering |= qpGeom::GR_point_perspective;
  } else {
    _glPointParameterfv = null_glPointParameterfv;
  }

  _supports_point_sprite = has_extension("GL_ARB_point_sprite");
  if (_supports_point_sprite) {
    // It appears that the point_sprite extension doesn't support
    // texture transforms on the generated texture coordinates.  How
    // inconsistent.  Because of this, we don't advertise
    // GR_point_sprite_tex_matrix.
    _supported_geom_rendering |= qpGeom::GR_point_sprite;
  }

  _supports_vertex_blend = has_extension("GL_ARB_vertex_blend");

  if (_supports_vertex_blend) {
    _glWeightPointerARB = (PFNGLWEIGHTPOINTERARBPROC)
      get_extension_func(GLPREFIX_QUOTED, "WeightPointerARB");
    _glVertexBlendARB = (PFNGLVERTEXBLENDARBPROC)
      get_extension_func(GLPREFIX_QUOTED, "VertexBlendARB");

    if (_glWeightPointerARB == NULL || _glVertexBlendARB == NULL) {
      GLCAT.warning()
        << "Vertex blending advertised as supported by OpenGL runtime, but could not get pointers to extension functions.\n";
      _supports_vertex_blend = false;
    }
  }

  if (_supports_vertex_blend) {
    GLP(Enable)(GL_WEIGHT_SUM_UNITY_ARB);

    GLint max_vertex_units;
    GLP(GetIntegerv)(GL_MAX_VERTEX_UNITS_ARB, &max_vertex_units);
    _max_vertex_transforms = max_vertex_units;
    GLCAT.debug()
      << "max vertex transforms = " << _max_vertex_transforms << "\n";
  }

  _supports_matrix_palette = has_extension("GL_ARB_matrix_palette");

  if (_supports_matrix_palette) {
    _glCurrentPaletteMatrixARB = (PFNGLCURRENTPALETTEMATRIXARBPROC)
      get_extension_func(GLPREFIX_QUOTED, "CurrentPaletteMatrixARB");
    _glMatrixIndexPointerARB = (PFNGLMATRIXINDEXPOINTERARBPROC)
      get_extension_func(GLPREFIX_QUOTED, "MatrixIndexPointerARB");

    if (_glCurrentPaletteMatrixARB == NULL || _glMatrixIndexPointerARB == NULL) {
      GLCAT.warning()
        << "Matrix palette advertised as supported by OpenGL runtime, but could not get pointers to extension functions.\n";
      _supports_matrix_palette = false;
    }
  }

  /*
    The matrix_palette support in this module is completely untested
    (because I don't happen to have a card handy whose driver supports
    this extension), so I have this ConfigVariable set to
    unconditionally set this flag off for now, to protect the unwary.
    When we have shown that the code works, we should remove this bit.
    In the meantime, you must put both "matrix-palette 1" and
    "gl-matrix-palette 1" in your Config.prc to exercise the new
    code. */
  if (!ConfigVariableBool("gl-matrix-palette", false, PRC_DESC("Temporary hack variable protecting untested code.  See glGraphicsStateGuardian_src.cxx."))) {
    if (_supports_matrix_palette) {
      GLCAT.debug() << "Forcing off matrix palette support.\n";
    }
    _supports_matrix_palette = false;
  }

  if (_supports_matrix_palette) {
    GLint max_palette_matrices;
    GLP(GetIntegerv)(GL_MAX_PALETTE_MATRICES_ARB, &max_palette_matrices);
    _max_vertex_transform_indices = max_palette_matrices;
    GLCAT.debug()
      << "max vertex transform indices = " << _max_vertex_transform_indices << "\n";
  }

  _supports_draw_range_elements = false;

  if (is_at_least_version(1, 2)) {
    _supports_draw_range_elements = true;
    _glDrawRangeElements = (PFNGLDRAWRANGEELEMENTSPROC)
      get_extension_func(GLPREFIX_QUOTED, "DrawRangeElements");

  } else if (has_extension("GL_EXT_draw_range_elements")) {
    _supports_draw_range_elements = true;
    _glDrawRangeElements = (PFNGLDRAWRANGEELEMENTSPROC)
      get_extension_func(GLPREFIX_QUOTED, "DrawRangeElementsEXT");
  }
  if (_supports_draw_range_elements) {
    if (_glDrawRangeElements == NULL) {
      GLCAT.warning()
        << "glDrawRangeElements advertised as supported by OpenGL runtime, but could not get pointers to extension functions.\n";
      _supports_draw_range_elements = false;
    }
  }
  if (!_supports_draw_range_elements) {
    _glDrawRangeElements = null_glDrawRangeElements;
  }

  _supports_3d_texture = 
    has_extension("GL_EXT_texture3D") || is_at_least_version(1, 2);

  if (is_at_least_version(1, 2)) {
    _supports_3d_texture = true;

    _glTexImage3D = (PFNGLTEXIMAGE3DPROC)
      get_extension_func(GLPREFIX_QUOTED, "TexImage3D");
    _glTexSubImage3D = (PFNGLTEXSUBIMAGE3DPROC)
      get_extension_func(GLPREFIX_QUOTED, "TexSubImage3D");

  } else if (has_extension("GL_EXT_texture3D")) {
    _supports_3d_texture = true;

    _glTexImage3D = (PFNGLTEXIMAGE3DPROC)
      get_extension_func(GLPREFIX_QUOTED, "TexImage3DEXT");
    _glTexSubImage3D = (PFNGLTEXSUBIMAGE3DPROC)
      get_extension_func(GLPREFIX_QUOTED, "TexSubImage3DEXT");
  }

  if (_supports_3d_texture) {
    if (_glTexImage3D == NULL || _glTexSubImage3D == NULL) {
      GLCAT.warning()
        << "3-D textures advertised as supported by OpenGL runtime, but could not get pointers to extension functions.\n";
      _supports_3d_texture = false;
    }
  }

  _supports_cube_map = 
    has_extension("GL_ARB_texture_cube_map") || is_at_least_version(1, 3);

  _supports_bgr = 
    has_extension("GL_EXT_bgra") || is_at_least_version(1, 2);
  _supports_rescale_normal = 
    has_extension("GL_EXT_rescale_normal") || is_at_least_version(1, 2);

  _supports_multisample = 
    has_extension("GL_ARB_multisample");

  _supports_generate_mipmap = 
    has_extension("GL_SGIS_generate_mipmap") || is_at_least_version(1, 4);

  _supports_multitexture = false;
    
  if (is_at_least_version(1, 3)) {
    _supports_multitexture = true;

    _glActiveTexture = (PFNGLACTIVETEXTUREPROC)
      get_extension_func(GLPREFIX_QUOTED, "ActiveTexture");
    _glClientActiveTexture = (PFNGLACTIVETEXTUREPROC)
      get_extension_func(GLPREFIX_QUOTED, "ClientActiveTexture");
    _glMultiTexCoord2fv = (PFNGLMULTITEXCOORD2FVPROC)
      get_extension_func(GLPREFIX_QUOTED, "MultiTexCoord2fv");

  } else if (has_extension("GL_ARB_multitexture")) {
    _supports_multitexture = true;

    _glActiveTexture = (PFNGLACTIVETEXTUREPROC)
      get_extension_func(GLPREFIX_QUOTED, "ActiveTextureARB");
    _glClientActiveTexture = (PFNGLACTIVETEXTUREPROC)
      get_extension_func(GLPREFIX_QUOTED, "ClientActiveTextureARB");
    _glMultiTexCoord2fv = (PFNGLMULTITEXCOORD2FVPROC)
      get_extension_func(GLPREFIX_QUOTED, "MultiTexCoord2fvARB");
  }

  if (_supports_multitexture) {
    if (_glActiveTexture == NULL || _glClientActiveTexture == NULL ||
        _glMultiTexCoord2fv == NULL) {
      GLCAT.warning()
        << "Multitexture advertised as supported by OpenGL runtime, but could not get pointers to extension functions.\n";
      _supports_multitexture = false;
    }
  }
  if (!_supports_multitexture) {
    _glActiveTexture = null_glActiveTexture;
    _glClientActiveTexture = null_glActiveTexture;
  }

  _supports_buffers = false;
    
  if (is_at_least_version(1, 5)) {
    _supports_buffers = true;

    _glGenBuffers = (PFNGLGENBUFFERSPROC)
      get_extension_func(GLPREFIX_QUOTED, "GenBuffers");
    _glBindBuffer = (PFNGLBINDBUFFERPROC)
      get_extension_func(GLPREFIX_QUOTED, "BindBuffer");
    _glBufferData = (PFNGLBUFFERDATAPROC)
      get_extension_func(GLPREFIX_QUOTED, "BufferData");
    _glBufferSubData = (PFNGLBUFFERSUBDATAPROC)
      get_extension_func(GLPREFIX_QUOTED, "BufferSubData");
    _glDeleteBuffers = (PFNGLDELETEBUFFERSPROC)
      get_extension_func(GLPREFIX_QUOTED, "DeleteBuffers");

  } else if (has_extension("GL_ARB_vertex_buffer_object")) {
    _supports_buffers = true;

    _glGenBuffers = (PFNGLGENBUFFERSPROC)
      get_extension_func(GLPREFIX_QUOTED, "GenBuffersARB");
    _glBindBuffer = (PFNGLBINDBUFFERPROC)
      get_extension_func(GLPREFIX_QUOTED, "BindBufferARB");
    _glBufferData = (PFNGLBUFFERDATAPROC)
      get_extension_func(GLPREFIX_QUOTED, "BufferDataARB");
    _glBufferSubData = (PFNGLBUFFERSUBDATAPROC)
      get_extension_func(GLPREFIX_QUOTED, "BufferSubDataARB");
    _glDeleteBuffers = (PFNGLDELETEBUFFERSPROC)
      get_extension_func(GLPREFIX_QUOTED, "DeleteBuffersARB");
  }

  if (_supports_buffers) {
    if (_glGenBuffers == NULL || _glBindBuffer == NULL ||
        _glBufferData == NULL || _glBufferSubData == NULL ||
        _glDeleteBuffers == NULL) {
      GLCAT.warning()
        << "Buffers advertised as supported by OpenGL runtime, but could not get pointers to extension functions.\n";
      _supports_buffers = false;
    }
  }

  _glBlendEquation = NULL;
  bool supports_blend_equation = false;
  if (is_at_least_version(1, 2)) {
    supports_blend_equation = true;
    _glBlendEquation = (PFNGLBLENDEQUATIONPROC)
      get_extension_func(GLPREFIX_QUOTED, "BlendEquation");
  } else if (has_extension("GL_EXT_blend_minmax")) {
    supports_blend_equation = true;
    _glBlendEquation = (PFNGLBLENDEQUATIONPROC)
      get_extension_func(GLPREFIX_QUOTED, "BlendEquationEXT");
  }
  if (supports_blend_equation && _glBlendEquation == NULL) {
    GLCAT.warning()
      << "BlendEquation advertised as supported by OpenGL runtime, but could not get pointers to extension function.\n";
  }
  if (_glBlendEquation == NULL) {
    _glBlendEquation = null_glBlendEquation;
  }

  _glBlendColor = NULL;
  bool supports_blend_color = false;
  if (is_at_least_version(1, 2)) {
    supports_blend_color = true;
    _glBlendColor = (PFNGLBLENDCOLORPROC)
      get_extension_func(GLPREFIX_QUOTED, "BlendColor");
  } else if (has_extension("GL_EXT_blend_color")) {
    supports_blend_color = true;
    _glBlendColor = (PFNGLBLENDCOLORPROC)
      get_extension_func(GLPREFIX_QUOTED, "BlendColorEXT");
  }
  if (supports_blend_color && _glBlendColor == NULL) {
    GLCAT.warning()
      << "BlendColor advertised as supported by OpenGL runtime, but could not get pointers to extension function.\n";
  }
  if (_glBlendColor == NULL) {
    _glBlendColor = null_glBlendColor;
  }

  _edge_clamp = GL_CLAMP;
  if (has_extension("GL_SGIS_texture_edge_clamp") ||
      is_at_least_version(1, 2)) {
    _edge_clamp = GL_CLAMP_TO_EDGE;
  }

  _border_clamp = GL_CLAMP;
  if (has_extension("GL_ARB_texture_border_clamp") ||
      is_at_least_version(1, 3)) {
    _border_clamp = GL_CLAMP_TO_BORDER;
  }

  _mirror_repeat = GL_REPEAT;
  if (has_extension("GL_ARB_texture_mirrored_repeat") ||
      is_at_least_version(1, 4)) {
    _mirror_repeat = GL_MIRRORED_REPEAT;
  }

  _mirror_clamp = GL_CLAMP;
  _mirror_edge_clamp = _edge_clamp;
  _mirror_border_clamp = _border_clamp;
  if (has_extension("GL_EXT_texture_mirror_clamp")) {
    _mirror_clamp = GL_MIRROR_CLAMP_EXT;
    _mirror_edge_clamp = GL_MIRROR_CLAMP_TO_EDGE_EXT;
    _mirror_border_clamp = GL_MIRROR_CLAMP_TO_BORDER_EXT;
  }

  if (_supports_multisample) {
    GLint sample_buffers;
    GLP(GetIntegerv)(GL_SAMPLE_BUFFERS, &sample_buffers);
    if (sample_buffers != 1) {
      // Even if the API supports multisample, we might have ended up
      // with a framebuffer that doesn't have any multisample bits.
      // (It's also possible the graphics card doesn't provide any
      // framebuffers with multisample.)  In this case, we don't
      // really support the multisample API's, since they won't do
      // anything.
      _supports_multisample = false;
    }
  }

  GLint max_texture_size;
  GLint max_3d_texture_size;
  GLint max_cube_map_size;
  
  GLP(GetIntegerv)(GL_MAX_TEXTURE_SIZE, &max_texture_size);
  _max_texture_dimension = max_texture_size;

  if (_supports_3d_texture) {
    GLP(GetIntegerv)(GL_MAX_3D_TEXTURE_SIZE, &max_3d_texture_size);
    _max_3d_texture_dimension = max_3d_texture_size;
  } else {
    _max_3d_texture_dimension = 0;
  }

  if (_supports_cube_map) {
    GLP(GetIntegerv)(GL_MAX_CUBE_MAP_TEXTURE_SIZE, &max_cube_map_size);
    _max_cube_map_dimension = max_cube_map_size;
  } else {
    _max_cube_map_dimension = 0;
  }

  if (GLCAT.is_debug()) {
    GLCAT.debug()
      << "max texture dimension = " << _max_texture_dimension
      << ", max 3d texture = " << _max_3d_texture_dimension
      << ", max cube map = " << _max_cube_map_dimension << "\n";
    GLint max_elements_vertices, max_elements_indices;
    GLP(GetIntegerv)(GL_MAX_ELEMENTS_VERTICES, &max_elements_vertices);
    GLP(GetIntegerv)(GL_MAX_ELEMENTS_INDICES, &max_elements_indices);
    GLCAT.debug()
      << "max_elements_vertices = " << max_elements_vertices
      << ", max_elements_indices = " << max_elements_indices << "\n";
    if (_supports_buffers) {
      if (vertex_buffers) {
        GLCAT.debug()
          << "vertex buffer objects are supported.\n";
      } else {
        GLCAT.debug()
          << "vertex buffer objects are supported (but not enabled).\n";
      }
    } else {
      GLCAT.debug()
        << "vertex buffer objects are NOT supported.\n";
    }
  }

  report_my_gl_errors();

  _auto_rescale_normal = false;

  // All GL implementations have the following buffers.
  _buffer_mask = (RenderBuffer::T_color |
                  RenderBuffer::T_depth |
                  RenderBuffer::T_stencil |
                  RenderBuffer::T_accum);

  // If we don't have double-buffering, don't attempt to write to the
  // back buffer.
  GLboolean has_back;
  GLP(GetBooleanv)(GL_DOUBLEBUFFER, &has_back);
  if (!has_back) {
    _buffer_mask &= ~RenderBuffer::T_back;
  }

  // Ensure the initial state is what we say it should be (in some
  // cases, we don't want the GL default settings; in others, we have
  // to force the point with some drivers that aren't strictly
  // compliant w.r.t. initial settings).
  GLP(FrontFace)(GL_CCW);
  GLP(Disable)(GL_LINE_SMOOTH);
  GLP(Disable)(GL_POINT_SMOOTH);
  GLP(Disable)(GL_POLYGON_SMOOTH);

  if (_supports_multisample) {
    GLP(Disable)(GL_MULTISAMPLE);
  }


  // Set up all the enabled/disabled flags to GL's known initial
  // values: everything off.
  _multisample_mode = 0;
  _line_smooth_enabled = false;
  _point_smooth_enabled = false;
  _polygon_smooth_enabled = false;
  _scissor_enabled = false;
  _stencil_test_enabled = false;
  _blend_enabled = false;
  _depth_test_enabled = false;
  _fog_enabled = false;
  _alpha_test_enabled = false;
  _polygon_offset_enabled = false;
  _flat_shade_model = false;
  _decal_level = 0;
  _tex_gen_point_sprite = false;
  
  // Dither is on by default in GL; let's turn it off
  GLP(Disable)(GL_DITHER);
  _dithering_enabled = false;

  _texgen_forced_normal = false;

#ifdef HAVE_CGGL
  _cg_shader = (CgShader *)NULL;
#endif

  // Count the max number of lights
  GLint max_lights;
  GLP(GetIntegerv)(GL_MAX_LIGHTS, &max_lights);
  _max_lights = max_lights;

  if (GLCAT.is_debug()) {
    GLCAT.debug()
      << "max lights = " << _max_lights << "\n";
  }

  // Count the max number of clipping planes
  GLint max_clip_planes;
  GLP(GetIntegerv)(GL_MAX_CLIP_PLANES, &max_clip_planes);
  _max_clip_planes = max_clip_planes;

  if (GLCAT.is_debug()) {
    GLCAT.debug()
      << "max clip planes = " << _max_clip_planes << "\n";
  }

  _projection_mat = LMatrix4f::ident_mat();

  if (_supports_multitexture) {
    GLint max_texture_stages;
    GLP(GetIntegerv)(GL_MAX_TEXTURE_UNITS, &max_texture_stages);
    _max_texture_stages = max_texture_stages;

    if (GLCAT.is_debug()) {
      GLCAT.debug()
        << "max texture stages = " << _max_texture_stages << "\n";
    }
  }
  _current_texture = DCAST(TextureAttrib, TextureAttrib::make_all_off());
  _current_tex_mat = DCAST(TexMatrixAttrib, TexMatrixAttrib::make());
  _needs_tex_mat = false;
  _current_tex_gen = DCAST(TexGenAttrib, TexGenAttrib::make());
  _needs_tex_gen = false;
  _tex_gen_modifies_mat = false;
  _last_max_stage_index = 0;
  _current_vbuffer_index = 0;
  _current_ibuffer_index = 0;
  _auto_antialias_mode = false;
  _render_mode = RenderModeAttrib::M_filled;
  _point_size = 1.0f;
  _point_perspective = false;

  _transform_stale = false;
  _vertex_blending_enabled = false;

  report_my_gl_errors();

  // Make sure the GL state matches all of our initial attribute
  // states.
  CPT(RenderAttrib) dta = DepthTestAttrib::make(DepthTestAttrib::M_less);
  CPT(RenderAttrib) dwa = DepthWriteAttrib::make(DepthWriteAttrib::M_on);
  CPT(RenderAttrib) cfa = CullFaceAttrib::make(CullFaceAttrib::M_cull_clockwise);
  CPT(RenderAttrib) ta = TextureAttrib::make_off();

  dta->issue(this);
  dwa->issue(this);
  cfa->issue(this);
  ta->issue(this);

  Material empty;
  apply_material(&empty);

  if (CLP(cheap_textures)) {
    GLCAT.info()
      << "Setting GLP(Hint)() for fastest textures.\n";
    GLP(Hint)(GL_PERSPECTIVE_CORRECTION_HINT, GL_FASTEST);
  }

  // use per-vertex fog if per-pixel fog requires SW renderer
  GLP(Hint)(GL_FOG_HINT, GL_DONT_CARE);

  GLint num_red_bits;
  GLP(GetIntegerv)(GL_RED_BITS, &num_red_bits);
  if (num_red_bits < 8) {
    GLP(Enable)(GL_DITHER);
    _dithering_enabled = true;
    if (GLCAT.is_debug()) {
      GLCAT.debug() 
        << "frame buffer depth = " << num_red_bits
        << " bits/channel, enabling dithering\n";
    }
  }

  _error_count = 0;

  report_my_gl_errors();
}


////////////////////////////////////////////////////////////////////
//     Function: GLGraphicsStateGuardian::clear
//       Access: Public, Virtual
//  Description: Clears all of the indicated buffers to their assigned
//               colors.
////////////////////////////////////////////////////////////////////
void CLP(GraphicsStateGuardian)::
do_clear(const RenderBuffer &buffer) {
  nassertv(buffer._gsg == this);
  int buffer_type = buffer._buffer_type;
  GLbitfield mask = 0;
  CPT(RenderState) state = RenderState::make_empty();

  if (buffer_type & RenderBuffer::T_color) {
    GLP(ClearColor)(_color_clear_value[0],
                    _color_clear_value[1],
                    _color_clear_value[2],
                    _color_clear_value[3]);
    state = state->add_attrib(ColorWriteAttrib::make(ColorWriteAttrib::M_on));
    mask |= GL_COLOR_BUFFER_BIT;

    set_draw_buffer(buffer);
  }

  if (buffer_type & RenderBuffer::T_depth) {
    GLP(ClearDepth)(_depth_clear_value);
    mask |= GL_DEPTH_BUFFER_BIT;

    // In order to clear the depth buffer, the depth mask must enable
    // writing to the depth buffer.
    state = state->add_attrib(DepthWriteAttrib::make(DepthWriteAttrib::M_on));
  }

  if (buffer_type & RenderBuffer::T_stencil) {
    GLP(ClearStencil)(_stencil_clear_value != false);
    mask |= GL_STENCIL_BUFFER_BIT;
  }

  if (buffer_type & RenderBuffer::T_accum) {
    GLP(ClearAccum)(_accum_clear_value[0],
                    _accum_clear_value[1],
                    _accum_clear_value[2],
                    _accum_clear_value[3]);
    mask |= GL_ACCUM_BUFFER_BIT;
  }

#ifdef GSG_VERBOSE
  GLCAT.spam() << "glClear(";
  if (mask & GL_COLOR_BUFFER_BIT) {
    GLCAT.spam(false) << "GL_COLOR_BUFFER_BIT|";
  }
  if (mask & GL_DEPTH_BUFFER_BIT) {
    GLCAT.spam(false) << "GL_DEPTH_BUFFER_BIT|";
  }
  if (mask & GL_STENCIL_BUFFER_BIT) {
    GLCAT.spam(false) << "GL_STENCIL_BUFFER_BIT|";
  }
  if (mask & GL_ACCUM_BUFFER_BIT) {
    GLCAT.spam(false) << "GL_ACCUM_BUFFER_BIT|";
  }
  GLCAT.spam(false) << ")" << endl;
#endif

  modify_state(state);

  GLP(Clear)(mask);
  report_my_gl_errors();
}

////////////////////////////////////////////////////////////////////
//     Function: GLGraphicsStateGuardian::prepare_display_region
//       Access: Public, Virtual
//  Description: Prepare a display region for rendering (set up
//               scissor region and viewport)
////////////////////////////////////////////////////////////////////
void CLP(GraphicsStateGuardian)::
prepare_display_region() {
  if (_current_display_region == (DisplayRegion*)0L) {
    GLCAT.error()
      << "Invalid NULL display region in prepare_display_region()\n";
    enable_scissor(false);
    _viewport_width = 1;
    _viewport_height = 1;

  } else if (_current_display_region != _actual_display_region) {
    _actual_display_region = _current_display_region;

    int l, b, w, h;
    _actual_display_region->get_region_pixels(l, b, w, h);
    _viewport_width = w;
    _viewport_height = h;
    GLint x = GLint(l);
    GLint y = GLint(b);
    GLsizei width = GLsizei(w);
    GLsizei height = GLsizei(h);

    enable_scissor(true);
    GLP(Scissor)(x, y, width, height);
    GLP(Viewport)(x, y, width, height);
  }
  report_my_gl_errors();

  do_point_size();
}

////////////////////////////////////////////////////////////////////
//     Function: GLGraphicsStateGuardian::prepare_lens
//       Access: Public, Virtual
//  Description: Makes the current lens (whichever lens was most
//               recently specified with set_scene()) active, so
//               that it will transform future rendered geometry.
//               Normally this is only called from the draw process,
//               and usually it is called by set_scene().
//
//               The return value is true if the lens is acceptable,
//               false if it is not.
////////////////////////////////////////////////////////////////////
bool CLP(GraphicsStateGuardian)::
prepare_lens() {
  if (_current_lens == (Lens *)NULL) {
    return false;
  }

  if (!_current_lens->is_linear()) {
    return false;
  }

  const LMatrix4f &lens_mat = _current_lens->get_projection_mat();

  // The projection matrix must always be right-handed Y-up, even if
  // our coordinate system of choice is otherwise, because certain GL
  // calls (specifically glTexGen(GL_SPHERE_MAP)) assume this kind of
  // a coordinate system.  Sigh.  In order to implement a Z-up (or
  // other arbitrary) coordinate system, we'll use a Y-up projection
  // matrix, and store the conversion to our coordinate system of
  // choice in the modelview matrix.
  _projection_mat =
    LMatrix4f::convert_mat(CS_yup_right, _current_lens->get_coordinate_system()) *
    lens_mat;

  if (_scene_setup->get_inverted()) {
    // If the scene is supposed to be inverted, then invert the
    // projection matrix.
    static LMatrix4f invert_mat = LMatrix4f::scale_mat(1.0f, -1.0f, 1.0f);
    _projection_mat *= invert_mat;
  }

#ifdef GSG_VERBOSE
  GLCAT.spam()
    << "glMatrixMode(GL_PROJECTION): " << _projection_mat << endl;
#endif
  GLP(MatrixMode)(GL_PROJECTION);
  GLP(LoadMatrixf)(_projection_mat.get_data());
  report_my_gl_errors();

  do_point_size();

  return true;
}

////////////////////////////////////////////////////////////////////
//     Function: GraphicsStateGuardian::begin_frame
//       Access: Public, Virtual
//  Description: Called before each frame is rendered, to allow the
//               GSG a chance to do any internal cleanup before
//               beginning the frame.
//
//               The return value is true if successful (in which case
//               the frame will be drawn and end_frame() will be
//               called later), or false if unsuccessful (in which
//               case nothing will be drawn and end_frame() will not
//               be called).
////////////////////////////////////////////////////////////////////
bool CLP(GraphicsStateGuardian)::
begin_frame() {
  if (!GraphicsStateGuardian::begin_frame()) {
    return false;
  }

#ifdef DO_PSTATS
  _vertices_display_list_pcollector.clear_level();
  _primitive_batches_display_list_pcollector.clear_level();
#endif

  _actual_display_region = NULL;

  report_my_gl_errors();
  return true;
}

////////////////////////////////////////////////////////////////////
//     Function: GLGraphicsStateGuardian::end_frame
//       Access: Public, Virtual
//  Description: Called after each frame is rendered, to allow the
//               GSG a chance to do any internal cleanup after
//               rendering the frame, and before the window flips.
////////////////////////////////////////////////////////////////////
void CLP(GraphicsStateGuardian)::
end_frame() {
  GraphicsStateGuardian::end_frame();

  // Now is a good time to delete any pending display lists.
  {
    MutexHolder holder(_lock);
    if (!_deleted_display_lists.empty()) {
      DeletedDisplayLists::iterator ddli;
      for (ddli = _deleted_display_lists.begin();
           ddli != _deleted_display_lists.end();
           ++ddli) {
        if (GLCAT.is_debug()) {
          GLCAT.debug()
            << "releasing index " << (*ddli) << "\n";
        }
        GLP(DeleteLists)((*ddli), 1);
      }
      _deleted_display_lists.clear();
    }
  }

  {
#ifdef DO_PSTATS
    PStatTimer timer(_flush_pcollector);
#endif
    // Calling glFlush() at the end of the frame is particularly
    // necessary if this is a single-buffered visual, so that the frame
    // will be finished drawing before we return to the application.
    // It's not clear what effect this has on our total frame time.
    GLP(Flush)();
  }

  report_my_gl_errors();
}

////////////////////////////////////////////////////////////////////
//     Function: GLGraphicsStateGuardian::draw_point
//       Access: Public, Virtual
//  Description:
////////////////////////////////////////////////////////////////////
void CLP(GraphicsStateGuardian)::
draw_point(GeomPoint *geom, GeomContext *gc) {
#ifdef GSG_VERBOSE
  GLCAT.spam() << "draw_point()" << endl;
#endif

  setup_antialias_point();

  if (draw_display_list(gc)) {
    return;
  }


#ifdef DO_PSTATS
  PStatTimer timer(_draw_primitive_pcollector);
  _vertices_other_pcollector.add_level(geom->get_num_vertices());
#endif

  issue_scene_graph_color();

  int nprims = geom->get_num_prims();
  Geom::VertexIterator vi = geom->make_vertex_iterator();
  Geom::NormalIterator ni = geom->make_normal_iterator();
  Geom::MultiTexCoordIterator ti;
  geom->setup_multitexcoord_iterator(ti, _current_texture->get_on_stages(),
                                     _current_tex_gen->get_no_texcoords());
  Geom::ColorIterator ci = geom->make_color_iterator();

  GeomIssuer::IssueColor *issue_color;

  if (_color_blend_involves_color_scale || !_color_scale_enabled) {
    issue_color = issue_color_gl;
  } else {
    issue_color = issue_scaled_color_gl;
  }

  GeomIssuer issuer(geom, this,
                    issue_vertex_gl,
                    issue_normal_gl,
                    issue_color,
                    issue_texcoord_single_gl,
                    issue_texcoord_multi_gl,
                    ti);

  // Draw overall
  issuer.issue_color(G_OVERALL, ci);
  issuer.issue_normal(G_OVERALL, ni);

  GLP(Begin)(GL_POINTS);

  for (int i = 0; i < nprims; i++) {
    // Draw per primitive
    issuer.issue_color(G_PER_PRIM, ci);
    issuer.issue_normal(G_PER_PRIM, ni);

    // Draw per vertex, same thing.
    issuer.issue_color(G_PER_VERTEX, ci);
    issuer.issue_normal(G_PER_VERTEX, ni);
    issuer.issue_texcoord(G_PER_VERTEX, ti);
    issuer.issue_vertex(G_PER_VERTEX, vi);
  }

  GLP(End)();
  report_my_gl_errors();
}

////////////////////////////////////////////////////////////////////
//     Function: GLGraphicsStateGuardian::draw_line
//       Access: Public, Virtual
//  Description:
////////////////////////////////////////////////////////////////////
void CLP(GraphicsStateGuardian)::
draw_line(GeomLine *geom, GeomContext *gc) {
#ifdef GSG_VERBOSE
  GLCAT.spam() << "draw_line()" << endl;
#endif

  setup_antialias_line();

  if (draw_display_list(gc)) {
    return;
  }

#ifdef DO_PSTATS
  PStatTimer timer(_draw_primitive_pcollector);
  _vertices_other_pcollector.add_level(geom->get_num_vertices());
#endif

  issue_scene_graph_color();

  int nprims = geom->get_num_prims();
  Geom::VertexIterator vi = geom->make_vertex_iterator();
  Geom::NormalIterator ni = geom->make_normal_iterator();
  Geom::MultiTexCoordIterator ti;
  geom->setup_multitexcoord_iterator(ti, _current_texture->get_on_stages(),
                                     _current_tex_gen->get_no_texcoords());
  Geom::ColorIterator ci = geom->make_color_iterator();

  GeomIssuer::IssueColor *issue_color;

  if (_color_blend_involves_color_scale || !_color_scale_enabled) {
    issue_color = issue_color_gl;
  } else {
    issue_color = issue_scaled_color_gl;
  }

  GeomIssuer issuer(geom, this,
                    issue_vertex_gl,
                    issue_normal_gl,
                    issue_color,
                    issue_texcoord_single_gl,
                    issue_texcoord_multi_gl,
                    ti);

  issue_flat_shading(geom);

  // Draw overall
  issuer.issue_color(G_OVERALL, ci);
  issuer.issue_normal(G_OVERALL, ni);

  GLP(Begin)(GL_LINES);

  for (int i = 0; i < nprims; i++) {
    // Draw per primitive
    issuer.issue_color(G_PER_PRIM, ci);
    issuer.issue_normal(G_PER_PRIM, ni);

    for (int j = 0; j < 2; j++) {
      // Draw per vertex
      issuer.issue_color(G_PER_VERTEX, ci);
      issuer.issue_normal(G_PER_VERTEX, ni);
      issuer.issue_texcoord(G_PER_VERTEX, ti);
      issuer.issue_vertex(G_PER_VERTEX, vi);
    }
  }

  GLP(End)();
  report_my_gl_errors();
}

////////////////////////////////////////////////////////////////////
//     Function: GLGraphicsStateGuardian::draw_linestrip
//       Access: Public, Virtual
//  Description:
////////////////////////////////////////////////////////////////////
void CLP(GraphicsStateGuardian)::
draw_linestrip(GeomLinestrip *geom, GeomContext *gc) {
#ifdef GSG_VERBOSE
  GLCAT.spam() << "draw_linestrip()" << endl;
#endif

  setup_antialias_line();

  if (draw_display_list(gc)) {
    return;
  }

#ifdef DO_PSTATS
  //  PStatTimer timer(_draw_primitive_pcollector);
  // Using PStatTimer may cause a compiler crash.
  _draw_primitive_pcollector.start();
  _vertices_other_pcollector.add_level(geom->get_num_vertices());
#endif

  issue_scene_graph_color();

  int nprims = geom->get_num_prims();
  const int *plen = geom->get_lengths();
  Geom::VertexIterator vi = geom->make_vertex_iterator();
  Geom::NormalIterator ni = geom->make_normal_iterator();
  Geom::MultiTexCoordIterator ti;
  geom->setup_multitexcoord_iterator(ti, _current_texture->get_on_stages(),
                                     _current_tex_gen->get_no_texcoords());
  Geom::ColorIterator ci = geom->make_color_iterator();

  GeomIssuer::IssueColor *issue_color;

  if (_color_blend_involves_color_scale || !_color_scale_enabled) {
    issue_color = issue_color_gl;
  } else {
    issue_color = issue_scaled_color_gl;
  }

  GeomIssuer issuer(geom, this,
                    issue_vertex_gl,
                    issue_normal_gl,
                    issue_color,
                    issue_texcoord_single_gl,
                    issue_texcoord_multi_gl,
                    ti);

  issue_flat_shading(geom);

  // Draw overall
  issuer.issue_color(G_OVERALL, ci);
  issuer.issue_normal(G_OVERALL, ni);

  for (int i = 0; i < nprims; i++) {
    // Draw per primitive
    issuer.issue_color(G_PER_PRIM, ci);
    issuer.issue_normal(G_PER_PRIM, ni);

    int num_verts = *(plen++);
    nassertv(num_verts >= 2);

    GLP(Begin)(GL_LINE_STRIP);

    // Per-component attributes for the first line segment?
    issuer.issue_color(G_PER_COMPONENT, ci);
    issuer.issue_normal(G_PER_COMPONENT, ni);

    // Draw the first 2 vertices
    int v;
    for (v = 0; v < 2; v++) {
      issuer.issue_color(G_PER_VERTEX, ci);
      issuer.issue_normal(G_PER_VERTEX, ni);
      issuer.issue_texcoord(G_PER_VERTEX, ti);
      issuer.issue_vertex(G_PER_VERTEX, vi);
    }

    // Now draw each of the remaining vertices.  Each vertex from
    // this point on defines a new line segment.
    for (v = 2; v < num_verts; v++) {
      // Per-component attributes?
      issuer.issue_color(G_PER_COMPONENT, ci);
      issuer.issue_normal(G_PER_COMPONENT, ni);

      // Per-vertex attributes
      issuer.issue_color(G_PER_VERTEX, ci);
      issuer.issue_normal(G_PER_VERTEX, ni);
      issuer.issue_texcoord(G_PER_VERTEX, ti);
      issuer.issue_vertex(G_PER_VERTEX, vi);
    }
    GLP(End)();
  }
  report_my_gl_errors();
  DO_PSTATS_STUFF(_draw_primitive_pcollector.stop());
}

////////////////////////////////////////////////////////////////////
//     Function: GLGraphicsStateGuardian::draw_sprite
//       Access: Public, Virtual
//  Description: CSN, 7/11/00
////////////////////////////////////////////////////////////////////

// this class exists because an alpha sort is necessary for correct
// sprite rendering, and we can't simply sort the vertex arrays as
// each vertex may or may not have corresponding information in the
// x/y texel-world-ratio and rotation arrays.
class WrappedSprite {
public:
  Vertexf _v;
  Colorf _c;
  float _x_ratio;
  float _y_ratio;
  float _theta;
};

// this struct exists because the STL can sort faster than i can.
struct draw_sprite_vertex_less {
  INLINE bool operator ()(const WrappedSprite& v0,
                          const WrappedSprite& v1) const {
    return v0._v[2] < v1._v[2]; }
};

void CLP(GraphicsStateGuardian)::
draw_sprite(GeomSprite *geom, GeomContext *) {
  // this is a little bit of a mess, but it's ok.  Here's the deal:
  // we want to draw, and draw quickly, an arbitrarily large number
  // of sprites all facing the screen.  Performing the billboard math
  // for ~1000 sprites is way too slow.  Ideally, we want one
  // matrix transformation that will handle everything, and this is
  // just about what ends up happening. We're getting the front-facing
  // effect by setting up a new frustum (of the same z-depth as the
  // current one) that is very small in x and y.  This way regularly
  // rendered triangles that might not be EXACTLY facing the camera
  // will certainly look close enough.  Then, we transform to camera-space
  // by hand and apply the inverse frustum to the transformed point.
  // For some cracked out reason, this actually works.
#ifdef GSG_VERBOSE
  GLCAT.spam() << "draw_sprite()" << endl;
#endif

  setup_antialias_polygon();

  // get the array traversal set up.
  int nprims = geom->get_num_prims();
  if (nprims==0) {
      return;
  }

#ifdef DO_PSTATS
  //  PStatTimer timer(_draw_primitive_pcollector);
  // Using PStatTimer may cause a compiler crash.
  _draw_primitive_pcollector.start();
  _vertices_other_pcollector.add_level(geom->get_num_vertices());
#endif

  Geom::VertexIterator vi = geom->make_vertex_iterator();
  Geom::ColorIterator ci = geom->make_color_iterator();

  // need some interface so user can set 2d dimensions if no texture specified
  float tex_x_size = 1.0f;  
  float tex_y_size = 1.0f;

  Texture *tex = geom->get_texture();
  if(tex != NULL) {
    // set up the texture-rendering state
    modify_state(RenderState::make
                 (TextureAttrib::make(tex),
                  TextureApplyAttrib::make(TextureApplyAttrib::M_modulate)));
    tex_x_size = tex->get_x_size();
    tex_y_size = tex->get_y_size();
  }

  // save the modelview matrix
  const LMatrix4f &modelview_mat = _transform->get_mat();

  // We don't need to mess with the aspect ratio, since we are now
  // using the default projection matrix, which has the right aspect
  // ratio built in.

  // load up our own matrices
  GLP(MatrixMode)(GL_MODELVIEW);
  GLP(LoadIdentity)();

  // precomputation stuff
  float tex_left = geom->get_ll_uv()[0];
  float tex_right = geom->get_ur_uv()[0];
  float tex_bottom = geom->get_ll_uv()[1];
  float tex_top = geom->get_ur_uv()[1];

  float half_width =  0.5f * tex_x_size * fabs(tex_right - tex_left);
  float half_height = 0.5f * tex_y_size * fabs(tex_top - tex_bottom);
  float scaled_width = 0.0f;
  float scaled_height = 0.0f;

  // the user can override alpha sorting if they want
  bool alpha = false;

  if (!geom->get_alpha_disable()) {
    // figure out if alpha's enabled (if not, no reason to sort)
    const TransparencyAttrib *trans = _state->get_transparency();
    if (trans != (const TransparencyAttrib *)NULL) {
      alpha = (trans->get_mode() != TransparencyAttrib::M_none);
    }
  }

  // sort container and iterator
  pvector< WrappedSprite > cameraspace_vector;
  pvector< WrappedSprite >::iterator vec_iter;

  // inner loop vars
  int i;
  Vertexf source_vert, cameraspace_vert;
  float *x_walk = (float *)NULL;
  float *y_walk = (float *)NULL;
  float *theta_walk = (float *)NULL;
  float theta = 0.0f;

  nassertv(geom->get_x_bind_type() != G_PER_VERTEX);
  nassertv(geom->get_y_bind_type() != G_PER_VERTEX);

  // set up the non-built-in bindings
  bool x_overall = (geom->get_x_bind_type() == G_OVERALL);
  bool y_overall = (geom->get_y_bind_type() == G_OVERALL);
  bool theta_overall = (geom->get_theta_bind_type() == G_OVERALL);
  bool color_overall = (geom->get_binding(G_COLOR) == G_OVERALL);
  bool theta_on = !(geom->get_theta_bind_type() == G_OFF);

  // x direction
  if (x_overall)
    scaled_width = geom->_x_texel_ratio[0] * half_width;
  else {
    nassertv(((int)geom->_x_texel_ratio.size() >= geom->get_num_prims()));
    x_walk = &geom->_x_texel_ratio[0];
  }

  // y direction
  if (y_overall)
    scaled_height = geom->_y_texel_ratio[0] * half_height;
  else {
    nassertv(((int)geom->_y_texel_ratio.size() >= geom->get_num_prims()));
    y_walk = &geom->_y_texel_ratio[0];
  }

  // theta
  if (theta_on) {
    if (theta_overall)
      theta = geom->_theta[0];
    else {
      nassertv(((int)geom->_theta.size() >= geom->get_num_prims()));
      theta_walk = &geom->_theta[0];
    }
  }

  /////////////////////////////////////////////////////////////////////
  // INNER LOOP PART 1 STARTS HERE
  // Here we transform each point to cameraspace and fill our sort
  // vector with the final geometric information.
  /////////////////////////////////////////////////////////////////////

  cameraspace_vector.reserve(nprims);   //pre-alloc space for nprims

  // the state is set, start running the prims
  for (i = 0; i < nprims; i++) {
    WrappedSprite ws;

    source_vert = geom->get_next_vertex(vi);

    // this mult converts to y-up cameraspace.
    cameraspace_vert = source_vert * modelview_mat;
    // build the final object that will go into the vector.
    ws._v.set(cameraspace_vert[0],cameraspace_vert[1],cameraspace_vert[2]);

    if (!color_overall)
      ws._c = geom->get_next_color(ci);
    if (!x_overall)
      ws._x_ratio = *x_walk++;
    if (!y_overall)
      ws._y_ratio = *y_walk++;
    if (theta_on) {
      if (!theta_overall)
        ws._theta = *theta_walk++;
    }

    cameraspace_vector.push_back(ws);
  }

  // now the verts are properly sorted by alpha (if necessary).  Of course,
  // the sort is only local, not scene-global, so if you look closely you'll
  // notice that alphas may be screwy.  It's ok though, because this is fast.
  // if you want accuracy, use billboards and take the speed hit.
  if (alpha) {
    sort(cameraspace_vector.begin(), cameraspace_vector.end(),
         draw_sprite_vertex_less());

     if (_dithering_enabled)
         GLP(Disable)(GL_DITHER);
  }

  Vertexf ul, ur, ll, lr;

  if (color_overall)
    GLP(Color4fv)(geom->get_next_color(ci).get_data());

  ////////////////////////////////////////////////////////////////////////////
  // INNER LOOP PART 2 STARTS HERE
  // Now we run through the cameraspace vector and compute the geometry for each
  // tristrip.  This includes scaling as per the ratio arrays, as well as
  // rotating in the z.
  ////////////////////////////////////////////////////////////////////////////

  vec_iter = cameraspace_vector.begin();
  for (; vec_iter != cameraspace_vector.end(); vec_iter++) {
    WrappedSprite& cur_image = *vec_iter;

    // if not G_OVERALL, calculate the scale factors
    if (x_overall == false)
      scaled_width = cur_image._x_ratio * half_width;

    if (y_overall == false)
      scaled_height = cur_image._y_ratio * half_height;

    // if not G_OVERALL, do some trig for this z rotate
    if (theta_on) {
      if (theta_overall == false)
        theta = cur_image._theta;

      // create the rotated points
      LMatrix3f xform_mat = LMatrix3f::rotate_mat(theta) * LMatrix3f::scale_mat(scaled_width, scaled_height);

      ur = (LVector3f( 1,  1, 0) * xform_mat) + cur_image._v;
      ul = (LVector3f(-1,  1, 0) * xform_mat) + cur_image._v;
      lr = (LVector3f( 1, -1, 0) * xform_mat) + cur_image._v;
      ll = (LVector3f(-1, -1, 0) * xform_mat) + cur_image._v;
    }
    else {
      // create the normal points
      ur.set(scaled_width, scaled_height, 0);
      ul.set(-scaled_width, scaled_height, 0);
      lr.set(scaled_width, -scaled_height, 0);
      ll.set(-scaled_width, -scaled_height, 0);

      ur += cur_image._v;
      ul += cur_image._v;
      lr += cur_image._v;
      ll += cur_image._v;
    }

    // set the color
    if (color_overall == false)
      GLP(Color4fv)(cur_image._c.get_data());

    // draw each one as a 2-element tri-strip
    GLP(Begin)(GL_TRIANGLE_STRIP);
    GLP(Normal3f)(0.0f, 0.0f, 1.0f);
    GLP(TexCoord2f)(tex_left, tex_bottom);  GLP(Vertex3fv)(ll.get_data());
    GLP(TexCoord2f)(tex_right, tex_bottom); GLP(Vertex3fv)(lr.get_data());
    GLP(TexCoord2f)(tex_left, tex_top);     GLP(Vertex3fv)(ul.get_data());
    GLP(TexCoord2f)(tex_right, tex_top);    GLP(Vertex3fv)(ur.get_data());
    GLP(End)();
  }

  // restore the matrices
  GLP(LoadMatrixf)(modelview_mat.get_data());

  if(alpha && _dithering_enabled)
     GLP(Enable)(GL_DITHER);

  report_my_gl_errors();
  DO_PSTATS_STUFF(_draw_primitive_pcollector.stop());
}


////////////////////////////////////////////////////////////////////
//     Function: GLGraphicsStateGuardian::draw_polygon
//       Access: Public, Virtual
//  Description:
////////////////////////////////////////////////////////////////////
void CLP(GraphicsStateGuardian)::
draw_polygon(GeomPolygon *geom, GeomContext *gc) {
#ifdef GSG_VERBOSE
  GLCAT.spam() << "draw_polygon()" << endl;
#endif

  setup_antialias_polygon();

  if (draw_display_list(gc)) {
    return;
  }

#ifdef DO_PSTATS
  //  PStatTimer timer(_draw_primitive_pcollector);
  // Using PStatTimer may cause a compiler crash.
  _draw_primitive_pcollector.start();
  _vertices_other_pcollector.add_level(geom->get_num_vertices());
#endif

  issue_scene_graph_color();

  int nprims = geom->get_num_prims();
  const int *plen = geom->get_lengths();
  Geom::VertexIterator vi = geom->make_vertex_iterator();
  Geom::NormalIterator ni = geom->make_normal_iterator();
  Geom::MultiTexCoordIterator ti;
  geom->setup_multitexcoord_iterator(ti, _current_texture->get_on_stages(),
                                     _current_tex_gen->get_no_texcoords());
  Geom::ColorIterator ci = geom->make_color_iterator();

  GeomIssuer::IssueColor *issue_color;

  if (_color_blend_involves_color_scale || !_color_scale_enabled) {
    issue_color = issue_color_gl;
  } else {
    issue_color = issue_scaled_color_gl;
  }

  GeomIssuer issuer(geom, this,
                    issue_vertex_gl,
                    issue_normal_gl,
                    issue_color,
                    issue_texcoord_single_gl,
                    issue_texcoord_multi_gl,
                    ti);

  issue_flat_shading(geom);

  // Draw overall
  issuer.issue_color(G_OVERALL, ci);
  issuer.issue_normal(G_OVERALL, ni);

  for (int i = 0; i < nprims; i++) {
    // Draw per primitive
    issuer.issue_color(G_PER_PRIM, ci);
    issuer.issue_normal(G_PER_PRIM, ni);

    int num_verts = *(plen++);
    nassertv(num_verts >= 3);

    GLP(Begin)(GL_POLYGON);

    // Draw the vertices.
    int v;
    for (v = 0; v < num_verts; v++) {
      // Per-vertex attributes.
      issuer.issue_color(G_PER_VERTEX, ci);
      issuer.issue_normal(G_PER_VERTEX, ni);
      issuer.issue_texcoord(G_PER_VERTEX, ti);
      issuer.issue_vertex(G_PER_VERTEX, vi);
    }
    GLP(End)();
  }
  report_my_gl_errors();
  DO_PSTATS_STUFF(_draw_primitive_pcollector.stop());
}

////////////////////////////////////////////////////////////////////
//     Function: GLGraphicsStateGuardian::draw_tri
//       Access: Public, Virtual
//  Description:
////////////////////////////////////////////////////////////////////
void CLP(GraphicsStateGuardian)::
draw_tri(GeomTri *geom, GeomContext *gc) {
#ifdef GSG_VERBOSE
  GLCAT.spam() << "draw_tri()" << endl;
#endif

  setup_antialias_polygon();

  if (draw_display_list(gc)) {
    return;
  }

#ifdef DO_PSTATS
  //  PStatTimer timer(_draw_primitive_pcollector);
  // Using PStatTimer may cause a compiler crash.
  _draw_primitive_pcollector.start();
  _vertices_tri_pcollector.add_level(geom->get_num_vertices());
#endif

  issue_scene_graph_color();

  int nprims = geom->get_num_prims();
  Geom::VertexIterator vi = geom->make_vertex_iterator();
  Geom::NormalIterator ni = geom->make_normal_iterator();
  Geom::MultiTexCoordIterator ti;
  geom->setup_multitexcoord_iterator(ti, _current_texture->get_on_stages(),
                                     _current_tex_gen->get_no_texcoords());
  Geom::ColorIterator ci = geom->make_color_iterator();

  GeomIssuer::IssueColor *issue_color;

  if (_color_blend_involves_color_scale || !_color_scale_enabled) {
    issue_color = issue_color_gl;
  } else {
    issue_color = issue_scaled_color_gl;
  }

  GeomIssuer issuer(geom, this,
                    issue_vertex_gl,
                    issue_normal_gl,
                    issue_color,
                    issue_texcoord_single_gl,
                    issue_texcoord_multi_gl,
                    ti);

  issue_flat_shading(geom);

  // Draw overall
  issuer.issue_color(G_OVERALL, ci);
  issuer.issue_normal(G_OVERALL, ni);

  GLP(Begin)(GL_TRIANGLES);

  for (int i = 0; i < nprims; i++) {
    // Draw per primitive
    issuer.issue_color(G_PER_PRIM, ci);
    issuer.issue_normal(G_PER_PRIM, ni);

    for (int j = 0; j < 3; j++) {
      // Draw per vertex
      issuer.issue_color(G_PER_VERTEX, ci);
      issuer.issue_normal(G_PER_VERTEX, ni);
      issuer.issue_texcoord(G_PER_VERTEX, ti);
      issuer.issue_vertex(G_PER_VERTEX, vi);
    }
  }

  GLP(End)();
  report_my_gl_errors();
#ifdef DO_PSTATS
  _draw_primitive_pcollector.stop();
#endif
}

////////////////////////////////////////////////////////////////////
//     Function: GLGraphicsStateGuardian::draw_quad
//       Access: Public, Virtual
//  Description:
////////////////////////////////////////////////////////////////////
void CLP(GraphicsStateGuardian)::
draw_quad(GeomQuad *geom, GeomContext *gc) {
#ifdef GSG_VERBOSE
  GLCAT.spam() << "draw_quad()" << endl;
#endif

  setup_antialias_polygon();

  if (draw_display_list(gc)) {
    return;
  }

#ifdef DO_PSTATS
  //  PStatTimer timer(_draw_primitive_pcollector);
  // Using PStatTimer may cause a compiler crash.
  _draw_primitive_pcollector.start();
  _vertices_other_pcollector.add_level(geom->get_num_vertices());
#endif

  issue_scene_graph_color();

  int nprims = geom->get_num_prims();
  Geom::VertexIterator vi = geom->make_vertex_iterator();
  Geom::NormalIterator ni = geom->make_normal_iterator();
  Geom::MultiTexCoordIterator ti;
  geom->setup_multitexcoord_iterator(ti, _current_texture->get_on_stages(),
                                     _current_tex_gen->get_no_texcoords());
  Geom::ColorIterator ci = geom->make_color_iterator();

  GeomIssuer::IssueColor *issue_color;

  if (_color_blend_involves_color_scale || !_color_scale_enabled) {
    issue_color = issue_color_gl;
  } else {
    issue_color = issue_scaled_color_gl;
  }

  GeomIssuer issuer(geom, this,
                    issue_vertex_gl,
                    issue_normal_gl,
                    issue_color,
                    issue_texcoord_single_gl,
                    issue_texcoord_multi_gl,
                    ti);

  issue_flat_shading(geom);

  // Draw overall
  issuer.issue_color(G_OVERALL, ci);
  issuer.issue_normal(G_OVERALL, ni);

  GLP(Begin)(GL_QUADS);

  for (int i = 0; i < nprims; i++) {
    // Draw per primitive
    issuer.issue_color(G_PER_PRIM, ci);
    issuer.issue_normal(G_PER_PRIM, ni);

    for (int j = 0; j < 4; j++) {
      // Draw per vertex
      issuer.issue_color(G_PER_VERTEX, ci);
      issuer.issue_normal(G_PER_VERTEX, ni);
      issuer.issue_texcoord(G_PER_VERTEX, ti);
      issuer.issue_vertex(G_PER_VERTEX, vi);
    }
  }

  GLP(End)();
  report_my_gl_errors();
  DO_PSTATS_STUFF(_draw_primitive_pcollector.stop());
}

////////////////////////////////////////////////////////////////////
//     Function: GLGraphicsStateGuardian::draw_tristrip
//       Access: Public, Virtual
//  Description:
////////////////////////////////////////////////////////////////////
void CLP(GraphicsStateGuardian)::
draw_tristrip(GeomTristrip *geom, GeomContext *gc) {
  
#ifdef GSG_VERBOSE
  GLCAT.spam() << "draw_tristrip()" << endl;
#endif

  setup_antialias_polygon();

  if (draw_display_list(gc)) {
    return;
  }

#ifdef DO_PSTATS
  //  PStatTimer timer(_draw_primitive_pcollector);
  // Using PStatTimer may cause a compiler crash.
  _draw_primitive_pcollector.start();
  if (geom->get_coords_index().is_null()) {
    _vertices_tristrip_pcollector.add_level(geom->get_num_vertices());
  } else {
    _vertices_indexed_tristrip_pcollector.add_level(geom->get_num_vertices());
  }
#endif

  issue_scene_graph_color();

  int nprims = geom->get_num_prims();
  const int *plen = geom->get_lengths();
  Geom::VertexIterator vi = geom->make_vertex_iterator();
  Geom::NormalIterator ni = geom->make_normal_iterator();
  Geom::MultiTexCoordIterator ti;
  geom->setup_multitexcoord_iterator(ti, _current_texture->get_on_stages(),
                                     _current_tex_gen->get_no_texcoords());
  Geom::ColorIterator ci = geom->make_color_iterator();

  GeomIssuer::IssueColor *issue_color;

  if (_color_blend_involves_color_scale || !_color_scale_enabled) {
    issue_color = issue_color_gl;
  } else {
    issue_color = issue_scaled_color_gl;
  }

  GeomIssuer issuer(geom, this,
                    issue_vertex_gl,
                    issue_normal_gl,
                    issue_color,
                    issue_texcoord_single_gl,
                    issue_texcoord_multi_gl,
                    ti);

  issue_flat_shading(geom);

  // Draw overall
  issuer.issue_color(G_OVERALL, ci);
  issuer.issue_normal(G_OVERALL, ni);

  for (int i = 0; i < nprims; i++) {
    // Draw per primitive
    issuer.issue_color(G_PER_PRIM, ci);
    issuer.issue_normal(G_PER_PRIM, ni);

    int num_verts = *(plen++);
    nassertv(num_verts >= 3);

    GLP(Begin)(GL_TRIANGLE_STRIP);

    // Per-component attributes for the first triangle?
    issuer.issue_color(G_PER_COMPONENT, ci);
    issuer.issue_normal(G_PER_COMPONENT, ni);

    // Draw the first three vertices.
    int v;
    for (v = 0; v < 3; v++) {
      issuer.issue_color(G_PER_VERTEX, ci);
      issuer.issue_normal(G_PER_VERTEX, ni);
      issuer.issue_texcoord(G_PER_VERTEX, ti);
      issuer.issue_vertex(G_PER_VERTEX, vi);
    }

    // Now draw each of the remaining vertices.  Each vertex from
    // this point on defines a new triangle.
    for (v = 3; v < num_verts; v++) {
      // Per-component attributes?
      issuer.issue_color(G_PER_COMPONENT, ci);
      issuer.issue_normal(G_PER_COMPONENT, ni);

      // Per-vertex attributes.
      issuer.issue_color(G_PER_VERTEX, ci);
      issuer.issue_normal(G_PER_VERTEX, ni);
      issuer.issue_texcoord(G_PER_VERTEX, ti);
      issuer.issue_vertex(G_PER_VERTEX, vi);
    }
    GLP(End)();
  }

  report_my_gl_errors();
  DO_PSTATS_STUFF(_draw_primitive_pcollector.stop());
  
}

////////////////////////////////////////////////////////////////////
//     Function: GLGraphicsStateGuardian::draw_trifan
//       Access: Public, Virtual
//  Description:
////////////////////////////////////////////////////////////////////
void CLP(GraphicsStateGuardian)::
draw_trifan(GeomTrifan *geom, GeomContext *gc) {
#ifdef GSG_VERBOSE
  GLCAT.spam() << "draw_trifan()" << endl;
#endif

  setup_antialias_polygon();

  if (draw_display_list(gc)) {
    return;
  }

#ifdef DO_PSTATS
  //  PStatTimer timer(_draw_primitive_pcollector);
  // Using PStatTimer may cause a compiler crash.
  _draw_primitive_pcollector.start();
  _vertices_trifan_pcollector.add_level(geom->get_num_vertices());
#endif

  issue_scene_graph_color();

  int nprims = geom->get_num_prims();
  const int *plen = geom->get_lengths();
  Geom::VertexIterator vi = geom->make_vertex_iterator();
  Geom::NormalIterator ni = geom->make_normal_iterator();
  Geom::MultiTexCoordIterator ti;
  geom->setup_multitexcoord_iterator(ti, _current_texture->get_on_stages(),
                                     _current_tex_gen->get_no_texcoords());
  Geom::ColorIterator ci = geom->make_color_iterator();

  GeomIssuer::IssueColor *issue_color;

  if (_color_blend_involves_color_scale || !_color_scale_enabled) {
    issue_color = issue_color_gl;
  } else {
    issue_color = issue_scaled_color_gl;
  }

  GeomIssuer issuer(geom, this,
                    issue_vertex_gl,
                    issue_normal_gl,
                    issue_color,
                    issue_texcoord_single_gl,
                    issue_texcoord_multi_gl,
                    ti);

  issue_flat_shading(geom);

  // Draw overall
  issuer.issue_color(G_OVERALL, ci);
  issuer.issue_normal(G_OVERALL, ni);

  for (int i = 0; i < nprims; i++) {
    // Draw per primitive
    issuer.issue_color(G_PER_PRIM, ci);
    issuer.issue_normal(G_PER_PRIM, ni);

    int num_verts = *(plen++);
    nassertv(num_verts >= 3);

    GLP(Begin)(GL_TRIANGLE_FAN);

    // Per-component attributes for the first triangle?
    issuer.issue_color(G_PER_COMPONENT, ci);
    issuer.issue_normal(G_PER_COMPONENT, ni);

    // Draw the first three vertices.
    int v;
    for (v = 0; v < 3; v++) {
      issuer.issue_color(G_PER_VERTEX, ci);
      issuer.issue_normal(G_PER_VERTEX, ni);
      issuer.issue_texcoord(G_PER_VERTEX, ti);
      issuer.issue_vertex(G_PER_VERTEX, vi);
    }

    // Now draw each of the remaining vertices.  Each vertex from
    // this point on defines a new triangle.
    for (v = 3; v < num_verts; v++) {
      // Per-component attributes?
      issuer.issue_color(G_PER_COMPONENT, ci);
      issuer.issue_normal(G_PER_COMPONENT, ni);

      // Per-vertex attributes.
      issuer.issue_color(G_PER_VERTEX, ci);
      issuer.issue_normal(G_PER_VERTEX, ni);
      issuer.issue_texcoord(G_PER_VERTEX, ti);
      issuer.issue_vertex(G_PER_VERTEX, vi);
    }
    GLP(End)();
  }
  report_my_gl_errors();
  DO_PSTATS_STUFF(_draw_primitive_pcollector.stop());
}


////////////////////////////////////////////////////////////////////
//     Function: GLGraphicsStateGuardian::draw_sphere
//       Access: Public, Virtual
//  Description:
////////////////////////////////////////////////////////////////////
void CLP(GraphicsStateGuardian)::
draw_sphere(GeomSphere *geom, GeomContext *gc) {
#ifdef GSG_VERBOSE
  GLCAT.spam() << "draw_sphere()" << endl;
#endif

  setup_antialias_polygon();

  if (draw_display_list(gc)) {
    return;
  }

#ifdef DO_PSTATS
  //  PStatTimer timer(_draw_primitive_pcollector);
  // Using PStatTimer may cause a compiler crash.
  _draw_primitive_pcollector.start();
  _vertices_other_pcollector.add_level(geom->get_num_vertices());
#endif

  issue_scene_graph_color();

  int nprims = geom->get_num_prims();
  Geom::VertexIterator vi = geom->make_vertex_iterator();
  Geom::MultiTexCoordIterator ti;
  geom->setup_multitexcoord_iterator(ti, _current_texture->get_on_stages(),
                                     _current_tex_gen->get_no_texcoords());
  Geom::ColorIterator ci = geom->make_color_iterator();

  GeomIssuer::IssueColor *issue_color;

  if (_color_blend_involves_color_scale || !_color_scale_enabled) {
    issue_color = issue_color_gl;
  } else {
    issue_color = issue_scaled_color_gl;
  }

  GeomIssuer issuer(geom, this,
                    issue_vertex_gl,
                    issue_normal_gl,
                    issue_color,
                    issue_texcoord_single_gl,
                    issue_texcoord_multi_gl,
                    ti);

  if (wants_normals()) {
    if (_flat_shade_model) {
      modify_state(get_smooth_state());
    }
  }

  // Draw overall
  issuer.issue_color(G_OVERALL, ci);

  GLUquadricObj *sph = GLUP(NewQuadric)();
  GLUP(QuadricNormals)(sph, wants_normals() ? (GLenum)GLU_SMOOTH : (GLenum)GLU_NONE);
  GLUP(QuadricTexture)(sph, wants_texcoords() ? (GLenum)GL_TRUE : (GLenum)GL_FALSE);
  GLUP(QuadricOrientation)(sph, (GLenum)GLU_OUTSIDE);
  GLUP(QuadricDrawStyle)(sph, (GLenum)GLU_FILL);
  //GLUP(QuadricDrawStyle)(sph, (GLenum)GLU_LINE);

  for (int i = 0; i < nprims; i++) {
    // Draw per primitive
    issuer.issue_color(G_PER_PRIM, ci);

    for (int j = 0; j < 2; j++) {
      // Draw per vertex
      issuer.issue_color(G_PER_VERTEX, ci);
    }
    Vertexf center = geom->get_next_vertex(vi);
    Vertexf edge = geom->get_next_vertex(vi);
    LVector3f v = edge - center;
    float r = sqrt(dot(v, v));

    // Since GLUP(Sphere) doesn't have a center parameter, we have to use
    // a matrix transform.

    GLP(MatrixMode)(GL_MODELVIEW);
    GLP(PushMatrix)();
    GLP(MultMatrixf)(LMatrix4f::translate_mat(center).get_data());

    // Now render the sphere using GLU calls.
    GLUP(Sphere)(sph, r, 16, 10);

    GLP(MatrixMode)(GL_MODELVIEW);
    GLP(PopMatrix)();
  }

  GLUP(DeleteQuadric)(sph);
  report_my_gl_errors();
  DO_PSTATS_STUFF(_draw_primitive_pcollector.stop());
}

////////////////////////////////////////////////////////////////////
//     Function: GLGraphicsStateGuardian::begin_draw_primitives
//       Access: Public, Virtual
//  Description: Called before a sequence of draw_primitive()
//               functions are called, this should prepare the vertex
//               data for rendering.  It returns true if the vertices
//               are ok, false to abort this group of primitives.
////////////////////////////////////////////////////////////////////
bool CLP(GraphicsStateGuardian)::
begin_draw_primitives(const qpGeom *geom, const qpGeomMunger *munger,
                      const qpGeomVertexData *vertex_data) {
  if (!GraphicsStateGuardian::begin_draw_primitives(geom, munger, vertex_data)) {
    return false;
  }
  nassertr(_vertex_data != (qpGeomVertexData *)NULL, false);

  _geom_display_list = 0;

  if (_auto_antialias_mode) {
    switch (geom->get_primitive_type()) {
    case qpGeomPrimitive::PT_polygons:
      setup_antialias_polygon();
      break;
    case qpGeomPrimitive::PT_points:
      setup_antialias_point();
      break;
    case qpGeomPrimitive::PT_lines:
      setup_antialias_line();
      break;
    case qpGeomPrimitive::PT_none:
      break;
    }
  }

  const qpGeomVertexAnimationSpec &animation = 
    vertex_data->get_format()->get_animation();
  bool hardware_animation = (animation.get_animation_type() == qpGeom::AT_hardware);
  if (hardware_animation) {
    // Set up the transform matrices for vertex blending.
    GLP(Enable)(GL_VERTEX_BLEND_ARB);
    _glVertexBlendARB(animation.get_num_transforms() - 1);
    
    const TransformTable *table = vertex_data->get_transform_table();
    if (table != (TransformTable *)NULL) {
      if (animation.get_indexed_transforms()) {
        // We are loading the indexed matrix palette.  The ARB decided
        // to change this interface from that for the list of
        // nonindexed matrices, to make it easier to load an arbitrary
        // number of matrices.
        GLP(Enable)(GL_MATRIX_PALETTE_ARB);

        GLP(MatrixMode)(GL_MATRIX_PALETTE_ARB);

        for (int i = 0; i < table->get_num_transforms(); ++i) {
          LMatrix4f mat;
          table->get_transform(i)->mult_matrix(mat, _transform->get_mat());
          _glCurrentPaletteMatrixARB(i);
          GLP(LoadMatrixf)(mat.get_data());
        }

        // Presumably loading the matrix palette does not step on the
        // GL_MODELVIEW matrix?

      } else {
        // We are loading the list of nonindexed matrices.  This is a
        // little clumsier.

        if (_supports_matrix_palette) {
          GLP(Disable)(GL_MATRIX_PALETTE_ARB);
        }

        // GL_MODELVIEW0 and 1 are different than the rest.
        int i = 0;
        if (i < table->get_num_transforms()) {
          LMatrix4f mat;
          table->get_transform(i)->mult_matrix(mat, _transform->get_mat());
          GLP(MatrixMode)(GL_MODELVIEW0_ARB);
          GLP(LoadMatrixf)(mat.get_data());
          ++i;
        }
        if (i < table->get_num_transforms()) {
          LMatrix4f mat;
          table->get_transform(i)->mult_matrix(mat, _transform->get_mat());
          GLP(MatrixMode)(GL_MODELVIEW1_ARB);
          GLP(LoadMatrixf)(mat.get_data());
          ++i;
        }
        while (i < table->get_num_transforms()) {
          LMatrix4f mat;
          table->get_transform(i)->mult_matrix(mat, _transform->get_mat());
          GLP(MatrixMode)(GL_MODELVIEW2_ARB + i - 2);
          GLP(LoadMatrixf)(mat.get_data());
          ++i;
        }
        
        // Setting the GL_MODELVIEW0 matrix steps on the world matrix,
        // so we have to set a flag to reload the world matrix later.
        _transform_stale = true;
      }
    }
    _vertex_blending_enabled = true;
    
  } else {
    // We're not using vertex blending.
    if (_vertex_blending_enabled) {
      GLP(Disable)(GL_VERTEX_BLEND_ARB);
      if (_supports_matrix_palette) {
        GLP(Disable)(GL_MATRIX_PALETTE_ARB);
      }
      _vertex_blending_enabled = false;
    }

    if (_transform_stale) {
      GLP(MatrixMode)(GL_MODELVIEW);
      GLP(LoadMatrixf)(_transform->get_mat().get_data());
    }
  }

  if (_vertex_data->is_vertex_transformed()) {
    // If the vertex data claims to be already transformed into clip
    // coordinates, wipe out the current projection and modelview
    // matrix (so we don't attempt to transform it again).
    GLP(MatrixMode)(GL_PROJECTION);
    GLP(PushMatrix)();
    GLP(LoadIdentity)();
    GLP(MatrixMode)(GL_MODELVIEW);
    GLP(PushMatrix)();
    GLP(LoadIdentity)();
  }

  if (geom->get_usage_hint() == qpGeom::UH_static && 
      _vertex_data->get_usage_hint() == qpGeom::UH_static &&
      display_lists && (!hardware_animation || display_list_animation)) {
    // If the geom claims to be totally static, try to build it into
    // a display list.

    // Before we compile or call a display list, make sure the current
    // buffers are unbound, or the nVidia drivers may crash.
    if (_current_vbuffer_index != 0) {
      _glBindBuffer(GL_ARRAY_BUFFER, 0);
      _current_vbuffer_index = 0;
    }
    if (_current_ibuffer_index != 0) {
      _glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
      _current_ibuffer_index = 0;
    }

    GeomContext *gc = ((qpGeom *)geom)->prepare_now(get_prepared_objects(), this);
    nassertr(gc != (GeomContext *)NULL, false);
    CLP(GeomContext) *ggc = DCAST(CLP(GeomContext), gc);
    const CLP(GeomMunger) *gmunger = DCAST(CLP(GeomMunger), _munger);
    UpdateSeq modified = max(geom->get_modified(), _vertex_data->get_modified());
    if (ggc->get_display_list(_geom_display_list, gmunger, modified)) {
      // If it hasn't been modified, just play the display list again.
      if (GLCAT.is_spam()) {
        GLCAT.spam()
          << "calling display list " << _geom_display_list << "\n";
      }

      GLP(CallList)(_geom_display_list);
#ifdef DO_PSTATS
      _vertices_display_list_pcollector.add_level(ggc->_num_verts);
      _primitive_batches_display_list_pcollector.add_level(1);
#endif

      // And now we don't need to do anything else for this geom.
      _geom_display_list = 0;
      end_draw_primitives();
      return false;
    }

    // Since we start this collector explicitly, we have to be sure to
    // stop it again.
    _load_display_list_pcollector.start();

    if (GLCAT.is_debug()) {
      GLCAT.debug()
        << "compiling display list " << _geom_display_list << "\n";
    }

    // If it has been modified, or this is the first time, then we
    // need to build the display list up.
    if (CLP(compile_and_execute)) {
      GLP(NewList)(_geom_display_list, GL_COMPILE_AND_EXECUTE);
    } else {
      GLP(NewList)(_geom_display_list, GL_COMPILE);
    }

#ifdef DO_PSTATS
    // Count up the number of vertices used by primitives in the Geom,
    // for PStats reporting.
    ggc->_num_verts = 0;
    for (int i = 0; i < geom->get_num_primitives(); i++) {
      ggc->_num_verts += geom->get_primitive(i)->get_num_vertices();
    }
#endif
  }

  const qpGeomVertexArrayData *array_data;
  int num_values;
  qpGeom::NumericType numeric_type;
  int start;
  int stride;

  if (_vertex_data->get_vertex_info(array_data, num_values, numeric_type, 
                                    start, stride)) {
    const unsigned char *client_pointer = setup_array_data(array_data);
    GLP(VertexPointer)(num_values, get_numeric_type(numeric_type), 
                       stride, client_pointer + start);
    GLP(EnableClientState)(GL_VERTEX_ARRAY);
  }

  if (_vertex_data->get_normal_info(array_data, numeric_type, 
                                    start, stride)) {
    const unsigned char *client_pointer = setup_array_data(array_data);
    GLP(NormalPointer)(get_numeric_type(numeric_type), stride, 
                       client_pointer + start);
    GLP(EnableClientState)(GL_NORMAL_ARRAY);
  } else {
    GLP(DisableClientState)(GL_NORMAL_ARRAY);
  }

  if (_vertex_data->get_color_info(array_data, num_values, numeric_type, 
                                   start, stride) &&
      numeric_type != qpGeom::NT_packed_dabc) {
    const unsigned char *client_pointer = setup_array_data(array_data);
    GLP(ColorPointer)(num_values, get_numeric_type(numeric_type), 
                      stride, client_pointer + start);
    GLP(EnableClientState)(GL_COLOR_ARRAY);
  } else {
    GLP(DisableClientState)(GL_COLOR_ARRAY);

    // Since we don't have per-vertex color, the implicit color is
    // white.
    GLP(Color4f)(1.0f, 1.0f, 1.0f, 1.0f);
  }

  // Now set up each of the active texture coordinate stages--or at
  // least those for which we're not generating texture coordinates
  // automatically.
  const Geom::ActiveTextureStages &active_stages = 
    _current_texture->get_on_stages();
  const Geom::NoTexCoordStages &no_texcoords = 
    _current_tex_gen->get_no_texcoords();

  int max_stage_index = (int)active_stages.size();
  int stage_index = 0;
  while (stage_index < max_stage_index) {
    _glClientActiveTexture(GL_TEXTURE0 + stage_index);
    TextureStage *stage = active_stages[stage_index];
    if (no_texcoords.find(stage) == no_texcoords.end()) {
      // This stage is not one of the stages that doesn't need
      // texcoords issued for it.
      const InternalName *name = stage->get_texcoord_name();

      if (_vertex_data->get_array_info(name, array_data, num_values, 
                                       numeric_type, start, stride)) {
        // The vertex data does have texcoords for this stage.
        const unsigned char *client_pointer = setup_array_data(array_data);
        GLP(TexCoordPointer)(num_values, get_numeric_type(numeric_type), 
                             stride, client_pointer + start);
        GLP(EnableClientState)(GL_TEXTURE_COORD_ARRAY);

      } else {
        // The vertex data doesn't have texcoords for this stage (even
        // though they're needed).
        GLP(DisableClientState)(GL_TEXTURE_COORD_ARRAY);
      }
    } else {
      // No texcoords are needed for this stage.
      GLP(DisableClientState)(GL_TEXTURE_COORD_ARRAY);
    }

    ++stage_index;
  }

  // Be sure also to disable any texture stages we had enabled before.
  while (stage_index < _last_max_stage_index) {
    _glClientActiveTexture(GL_TEXTURE0 + stage_index);
    GLP(DisableClientState)(GL_TEXTURE_COORD_ARRAY);
    ++stage_index;
  }
  _last_max_stage_index = max_stage_index;

  if (_supports_vertex_blend) {
    if (hardware_animation) {
      // Issue the weights and/or transform indices for vertex blending.
      if (_vertex_data->get_array_info(InternalName::get_transform_weight(),
                                       array_data, num_values, numeric_type, 
                                       start, stride)) {
        const unsigned char *client_pointer = setup_array_data(array_data);
        _glWeightPointerARB(num_values, get_numeric_type(numeric_type), 
                            stride, client_pointer + start);
        GLP(EnableClientState)(GL_WEIGHT_ARRAY_ARB);
      } else {
        GLP(DisableClientState)(GL_WEIGHT_ARRAY_ARB);
      }

      if (animation.get_indexed_transforms()) {
        // Issue the matrix palette indices.
        if (_vertex_data->get_array_info(InternalName::get_transform_index(),
                                         array_data, num_values, numeric_type, 
                                         start, stride)) {
          const unsigned char *client_pointer = setup_array_data(array_data);
          _glMatrixIndexPointerARB(num_values, get_numeric_type(numeric_type), 
                              stride, client_pointer + start);
          GLP(EnableClientState)(GL_MATRIX_INDEX_ARRAY_ARB);
        } else {
          GLP(DisableClientState)(GL_MATRIX_INDEX_ARRAY_ARB);
        }
      }

    } else {
      GLP(DisableClientState)(GL_WEIGHT_ARRAY_ARB);
      if (_supports_matrix_palette) {
        GLP(DisableClientState)(GL_MATRIX_INDEX_ARRAY_ARB);
      }
    }
  }

  return true;
}

////////////////////////////////////////////////////////////////////
//     Function: GLGraphicsStateGuardian::draw_triangles
//       Access: Public, Virtual
//  Description: Draws a series of disconnected triangles.
////////////////////////////////////////////////////////////////////
void CLP(GraphicsStateGuardian)::
draw_triangles(const qpGeomTriangles *primitive) {
  _vertices_tri_pcollector.add_level(primitive->get_num_vertices());
  _primitive_batches_tri_pcollector.add_level(1);

  if (primitive->is_indexed()) {
    const unsigned char *client_pointer = setup_primitive(primitive);
    
    _glDrawRangeElements(GL_TRIANGLES, 
                         primitive->get_min_vertex(),
                         primitive->get_max_vertex(),
                         primitive->get_num_vertices(),
                         get_numeric_type(primitive->get_index_type()), 
                         client_pointer);
  } else {
    GLP(DrawArrays)(GL_TRIANGLES,
                    primitive->get_first_vertex(),
                    primitive->get_num_vertices());
  }

  report_my_gl_errors();
}

////////////////////////////////////////////////////////////////////
//     Function: GLGraphicsStateGuardian::draw_tristrips
//       Access: Public, Virtual
//  Description: Draws a series of triangle strips.
////////////////////////////////////////////////////////////////////
void CLP(GraphicsStateGuardian)::
draw_tristrips(const qpGeomTristrips *primitive) {
  if (connect_triangle_strips && _render_mode != RenderModeAttrib::M_wireframe) {
    // One long triangle strip, connected by the degenerate vertices
    // that have already been set up within the primitive.
    _vertices_tristrip_pcollector.add_level(primitive->get_num_vertices());
    _primitive_batches_tristrip_pcollector.add_level(1);
    if (primitive->is_indexed()) {
      const unsigned char *client_pointer = setup_primitive(primitive);
      _glDrawRangeElements(GL_TRIANGLE_STRIP, 
                           primitive->get_min_vertex(),
                           primitive->get_max_vertex(),
                           primitive->get_num_vertices(),
                           get_numeric_type(primitive->get_index_type()), 
                           client_pointer);
    } else {
      GLP(DrawArrays)(GL_TRIANGLE_STRIP,
                      primitive->get_first_vertex(),
                      primitive->get_num_vertices());
    }

  } else {
    // Send the individual triangle strips, stepping over the
    // degenerate vertices.
    CPTA_int ends = primitive->get_ends();
    
    _primitive_batches_tristrip_pcollector.add_level(ends.size());
    if (primitive->is_indexed()) {
      const unsigned char *client_pointer = setup_primitive(primitive);
      int index_stride = primitive->get_index_stride();
      qpGeomVertexReader mins(primitive->get_mins(), 0);
      qpGeomVertexReader maxs(primitive->get_maxs(), 0);
      nassertv(primitive->get_mins()->get_num_rows() == (int)ends.size() && 
               primitive->get_maxs()->get_num_rows() == (int)ends.size());

      unsigned int start = 0;
      for (size_t i = 0; i < ends.size(); i++) {
        _vertices_tristrip_pcollector.add_level(ends[i] - start);
        _glDrawRangeElements(GL_TRIANGLE_STRIP, 
                             mins.get_data1i(), maxs.get_data1i(), 
                             ends[i] - start,
                             get_numeric_type(primitive->get_index_type()), 
                             client_pointer + start * index_stride);
        start = ends[i] + 2;
      }
    } else {
      unsigned int start = 0;
      int first_vertex = primitive->get_first_vertex();
      for (size_t i = 0; i < ends.size(); i++) {
        _vertices_tristrip_pcollector.add_level(ends[i] - start);
        GLP(DrawArrays)(GL_TRIANGLE_STRIP, first_vertex + start, 
                        ends[i] - start);
        start = ends[i] + 2;
      }
    }
  }
    
  report_my_gl_errors();
}

////////////////////////////////////////////////////////////////////
//     Function: GLGraphicsStateGuardian::draw_trifans
//       Access: Public, Virtual
//  Description: Draws a series of triangle fans.
////////////////////////////////////////////////////////////////////
void CLP(GraphicsStateGuardian)::
draw_trifans(const qpGeomTrifans *primitive) {
  // Send the individual triangle fans.  There's no connecting fans
  // with degenerate vertices, so no worries about that.
  CPTA_int ends = primitive->get_ends();

  _primitive_batches_trifan_pcollector.add_level(ends.size());
  if (primitive->is_indexed()) {
    const unsigned char *client_pointer = setup_primitive(primitive);
    int index_stride = primitive->get_index_stride();
    qpGeomVertexReader mins(primitive->get_mins(), 0);
    qpGeomVertexReader maxs(primitive->get_maxs(), 0);
    nassertv(primitive->get_mins()->get_num_rows() == (int)ends.size() && 
             primitive->get_maxs()->get_num_rows() == (int)ends.size());

    unsigned int start = 0;
    for (size_t i = 0; i < ends.size(); i++) {
      _vertices_trifan_pcollector.add_level(ends[i] - start);
      _glDrawRangeElements(GL_TRIANGLE_FAN, 
                           mins.get_data1i(), maxs.get_data1i(), ends[i] - start,
                           get_numeric_type(primitive->get_index_type()), 
                           client_pointer + start * index_stride);
      start = ends[i];
    }
  } else {
    unsigned int start = 0;
    int first_vertex = primitive->get_first_vertex();
    for (size_t i = 0; i < ends.size(); i++) {
      _vertices_trifan_pcollector.add_level(ends[i] - start);
      GLP(DrawArrays)(GL_TRIANGLE_FAN, first_vertex + start,
                      ends[i] - start);
      start = ends[i];
    }
  }
    
  report_my_gl_errors();
}

////////////////////////////////////////////////////////////////////
//     Function: GLGraphicsStateGuardian::draw_lines
//       Access: Public, Virtual
//  Description: Draws a series of disconnected line segments.
////////////////////////////////////////////////////////////////////
void CLP(GraphicsStateGuardian)::
draw_lines(const qpGeomLines *primitive) {
  _vertices_other_pcollector.add_level(primitive->get_num_vertices());
  _primitive_batches_other_pcollector.add_level(1);

  if (primitive->is_indexed()) {
    const unsigned char *client_pointer = setup_primitive(primitive);
    _glDrawRangeElements(GL_LINES, 
                         primitive->get_min_vertex(),
                         primitive->get_max_vertex(),
                         primitive->get_num_vertices(),
                         get_numeric_type(primitive->get_index_type()), 
                         client_pointer);
  } else {
    GLP(DrawArrays)(GL_LINES,
                    primitive->get_first_vertex(),
                    primitive->get_num_vertices());
  }

  report_my_gl_errors();
}

////////////////////////////////////////////////////////////////////
//     Function: GLGraphicsStateGuardian::draw_linestrips
//       Access: Public, Virtual
//  Description: Draws a series of line strips.
////////////////////////////////////////////////////////////////////
void CLP(GraphicsStateGuardian)::
draw_linestrips(const qpGeomLinestrips *primitive) {
}

////////////////////////////////////////////////////////////////////
//     Function: GLGraphicsStateGuardian::draw_points
//       Access: Public, Virtual
//  Description: Draws a series of disconnected points.
////////////////////////////////////////////////////////////////////
void CLP(GraphicsStateGuardian)::
draw_points(const qpGeomPoints *primitive) {
  _vertices_other_pcollector.add_level(primitive->get_num_vertices());
  _primitive_batches_other_pcollector.add_level(1);

  if (primitive->is_indexed()) {
    const unsigned char *client_pointer = setup_primitive(primitive);
    _glDrawRangeElements(GL_POINTS, 
                         primitive->get_min_vertex(),
                         primitive->get_max_vertex(),
                         primitive->get_num_vertices(),
                         get_numeric_type(primitive->get_index_type()), 
                         client_pointer);
  } else {
    GLP(DrawArrays)(GL_POINTS,
                    primitive->get_first_vertex(),
                    primitive->get_num_vertices());
  }

  report_my_gl_errors();
}

////////////////////////////////////////////////////////////////////
//     Function: GLGraphicsStateGuardian::end_draw_primitives()
//       Access: Public, Virtual
//  Description: Called after a sequence of draw_primitive()
//               functions are called, this should do whatever cleanup
//               is appropriate.
////////////////////////////////////////////////////////////////////
void CLP(GraphicsStateGuardian)::
end_draw_primitives() {
  if (_geom_display_list != 0) {
    // If we were building a display list, close it now.
    GLP(EndList)();
    _load_display_list_pcollector.stop();

    if (!CLP(compile_and_execute)) {
      GLP(CallList)(_geom_display_list);
    }      
    _primitive_batches_display_list_pcollector.add_level(1);
  }
  _geom_display_list = 0;

  // Clean up the vertex blending state.
  if (_vertex_blending_enabled) {
    GLP(Disable)(GL_VERTEX_BLEND_ARB);
    if (_supports_matrix_palette) {
      GLP(Disable)(GL_MATRIX_PALETTE_ARB);
    }
    _vertex_blending_enabled = false;
  }
  
  if (_transform_stale) {
    GLP(MatrixMode)(GL_MODELVIEW);
    GLP(LoadMatrixf)(_transform->get_mat().get_data());
  }

  if (_vertex_data->is_vertex_transformed()) {
    // Restore the matrices that we pushed above.
    GLP(MatrixMode)(GL_PROJECTION);
    GLP(PopMatrix)();
    GLP(MatrixMode)(GL_MODELVIEW);
    GLP(PopMatrix)();
  }

  GraphicsStateGuardian::end_draw_primitives();
}

////////////////////////////////////////////////////////////////////
//     Function: GLGraphicsStateGuardian::prepare_texture
//       Access: Public, Virtual
//  Description: Creates a new retained-mode representation of the
//               given texture, and returns a newly-allocated
//               TextureContext pointer to reference it.  It is the
//               responsibility of the calling function to later
//               call release_texture() with this same pointer (which
//               will also delete the pointer).
//
//               This function should not be called directly to
//               prepare a texture.  Instead, call Texture::prepare().
////////////////////////////////////////////////////////////////////
TextureContext *CLP(GraphicsStateGuardian)::
prepare_texture(Texture *tex) {
  CLP(TextureContext) *gtc = new CLP(TextureContext)(tex);
  GLP(GenTextures)(1, &gtc->_index);

  bind_texture(gtc);
  GLP(PrioritizeTextures)(1, &gtc->_index, &gtc->_priority);
  specify_texture(tex);
  apply_texture_immediate(gtc, tex);

  report_my_gl_errors();
  return gtc;
}

////////////////////////////////////////////////////////////////////
//     Function: GLGraphicsStateGuardian::apply_texture
//       Access: Public, Virtual
//  Description: Makes the texture the currently available texture for
//               rendering.
////////////////////////////////////////////////////////////////////
void CLP(GraphicsStateGuardian)::
apply_texture(TextureContext *tc) {
  CLP(TextureContext) *gtc = DCAST(CLP(TextureContext), tc);

  add_to_texture_record(gtc);
  bind_texture(gtc);

  int dirty = gtc->get_dirty_flags();
  if ((dirty & (Texture::DF_wrap | Texture::DF_filter | Texture::DF_border)) != 0) {
    // We need to re-specify the texture properties.
    specify_texture(gtc->_texture);
  }
  if ((dirty & (Texture::DF_image | Texture::DF_mipmap | Texture::DF_border)) != 0) {
    // We need to re-apply the image.
    apply_texture_immediate(gtc, gtc->_texture);
  }

  gtc->clear_dirty_flags();

  report_my_gl_errors();
}

////////////////////////////////////////////////////////////////////
//     Function: GLGraphicsStateGuardian::release_texture
//       Access: Public, Virtual
//  Description: Frees the GL resources previously allocated for the
//               texture.  This function should never be called
//               directly; instead, call Texture::release() (or simply
//               let the Texture destruct).
////////////////////////////////////////////////////////////////////
void CLP(GraphicsStateGuardian)::
release_texture(TextureContext *tc) {
  CLP(TextureContext) *gtc = DCAST(CLP(TextureContext), tc);

  GLP(DeleteTextures)(1, &gtc->_index);
  report_my_gl_errors();

  gtc->_index = 0;
  delete gtc;
}

////////////////////////////////////////////////////////////////////
//     Function: GLGraphicsStateGuardian::prepare_geom
//       Access: Public, Virtual
//  Description: Creates a new retained-mode representation of the
//               given geom, and returns a newly-allocated
//               GeomContext pointer to reference it.  It is the
//               responsibility of the calling function to later
//               call release_geom() with this same pointer (which
//               will also delete the pointer).
//
//               This function should not be called directly to
//               prepare a geom.  Instead, call Geom::prepare().
////////////////////////////////////////////////////////////////////
GeomContext *CLP(GraphicsStateGuardian)::
prepare_geom(Geom *geom) {
  // Temporary test until the experimental Geom rewrite becomes the
  // actual Geom implementation.
  if (geom->is_of_type(qpGeom::get_class_type())) {
    CLP(GeomContext) *ggc = new CLP(GeomContext)(geom);
    return ggc;

  } else {
    // Original Geom display list implementation.  Slightly broken,
    // since it doesn't work well with scene graph color
    // manipulations.

    if (!_vertex_colors_enabled) {
      // We can't build a display list (or play back a display list) if
      // its color is overridden with a scene graph color.  Maybe if we
      // take advantage of the OpenGL color matrix we can do this, but
      // for now we'll just ignore it.
      return NULL;
    }

    if (geom->is_dynamic()) {
      // If the Geom is dynamic in some way, we shouldn't try to
      // display-list it.
      return NULL;
    }

    CLP(GeomContext) *ggc = new CLP(GeomContext)(geom);
    ggc->_deprecated_index = GLP(GenLists)(1);
    if (GLCAT.is_debug()) {
      GLCAT.debug()
        << "preparing " << *geom << ", index " << ggc->_deprecated_index << "\n";
    }
    if (ggc->_deprecated_index == 0) {
      GLCAT.error()
        << "Ran out of display list indices.\n";
      delete ggc;
      return NULL;
    }

    // We need to temporarily force normals and UV's on, so the display
    // list will have them built in.
    //force_texcoords(); 
    force_normals();

#ifdef DO_PSTATS
    // Count up the number of vertices we're about to render, by
    // checking the PStats vertex counters now, and at the end.  This is
    // kind of hacky, but this is debug code.
    float num_verts_before =
      _vertices_tristrip_pcollector.get_level() +
      _vertices_trifan_pcollector.get_level() +
      _vertices_tri_pcollector.get_level() +
      _vertices_other_pcollector.get_level();
#endif

    // Now define the display list.
    GLP(NewList)(ggc->_deprecated_index, GL_COMPILE);
    geom->draw_immediate(this, NULL);
    GLP(EndList)();

#ifdef DO_PSTATS
    float num_verts_after =
      _vertices_tristrip_pcollector.get_level() +
      _vertices_trifan_pcollector.get_level() +
      _vertices_tri_pcollector.get_level() +
      _vertices_other_pcollector.get_level();
    float num_verts = num_verts_after - num_verts_before;
    ggc->_num_verts = (int)(num_verts + 0.5);
#endif

    undo_force_normals();
    //undo_force_texcoords();

    report_my_gl_errors();
    return ggc;
  }
}

////////////////////////////////////////////////////////////////////
//     Function: GLGraphicsStateGuardian::release_geom
//       Access: Public, Virtual
//  Description: Frees the GL resources previously allocated for the
//               geom.  This function should never be called
//               directly; instead, call Geom::release() (or simply
//               let the Geom destruct).
////////////////////////////////////////////////////////////////////
void CLP(GraphicsStateGuardian)::
release_geom(GeomContext *gc) {
  CLP(GeomContext) *ggc = DCAST(CLP(GeomContext), gc);
  ggc->release_display_lists();
  report_my_gl_errors();

  delete ggc;
}

////////////////////////////////////////////////////////////////////
//     Function: GLGraphicsStateGuardian::record_deleted_display_list
//       Access: Public
//  Description: This is intended to be called only from the
//               GLGeomContext destructor.  It saves the indicated
//               display list index in the list to be deleted at the
//               end of the frame.
////////////////////////////////////////////////////////////////////
void CLP(GraphicsStateGuardian)::
record_deleted_display_list(GLuint index) {
  MutexHolder holder(_lock);
  _deleted_display_lists.push_back(index);
}

////////////////////////////////////////////////////////////////////
//     Function: GLGraphicsStateGuardian::prepare_vertex_buffer
//       Access: Public, Virtual
//  Description: Creates a new retained-mode representation of the
//               given data, and returns a newly-allocated
//               VertexBufferContext pointer to reference it.  It is the
//               responsibility of the calling function to later
//               call release_vertex_buffer() with this same pointer (which
//               will also delete the pointer).
//
//               This function should not be called directly to
//               prepare a buffer.  Instead, call Geom::prepare().
////////////////////////////////////////////////////////////////////
VertexBufferContext *CLP(GraphicsStateGuardian)::
prepare_vertex_buffer(qpGeomVertexArrayData *data) {
  if (_supports_buffers) {
    CLP(VertexBufferContext) *gvbc = new CLP(VertexBufferContext)(data);
    _glGenBuffers(1, &gvbc->_index);

    if (GLCAT.is_debug()) {
      GLCAT.debug()
        << "creating vertex buffer " << gvbc->_index << ": "
        << data->get_num_rows() << " vertices " 
        << *data->get_array_format() << "\n";
    }
    
    report_my_gl_errors();
    return gvbc;
  }

  return NULL;
}

////////////////////////////////////////////////////////////////////
//     Function: GLGraphicsStateGuardian::apply_vertex_buffer
//       Access: Public
//  Description: Makes the data the currently available data for
//               rendering.
////////////////////////////////////////////////////////////////////
void CLP(GraphicsStateGuardian)::
apply_vertex_buffer(VertexBufferContext *vbc) {
  nassertv(_supports_buffers);

  CLP(VertexBufferContext) *gvbc = DCAST(CLP(VertexBufferContext), vbc);

  if (_current_vbuffer_index != gvbc->_index) {
    _glBindBuffer(GL_ARRAY_BUFFER, gvbc->_index);
    _current_vbuffer_index = gvbc->_index;
    add_to_vertex_buffer_record(gvbc);
  }
  
  if (gvbc->was_modified()) {
    PStatTimer timer(_load_vertex_buffer_pcollector);
    int num_bytes = gvbc->get_data()->get_data_size_bytes();
    if (GLCAT.is_spam()) {
      GLCAT.spam()
        << "copying " << num_bytes
        << " bytes into vertex buffer " << gvbc->_index << "\n";
    }
    if (gvbc->changed_size() || gvbc->changed_usage_hint()) {
      _glBufferData(GL_ARRAY_BUFFER, num_bytes,
                    gvbc->get_data()->get_data(), 
                    get_usage(gvbc->get_data()->get_usage_hint()));

    } else {
      _glBufferSubData(GL_ARRAY_BUFFER, 0, num_bytes,
                       gvbc->get_data()->get_data());
    }
    _data_transferred_pcollector.add_level(num_bytes);
    add_to_total_buffer_record(gvbc);
    gvbc->mark_loaded();
  }

  report_my_gl_errors();
}

////////////////////////////////////////////////////////////////////
//     Function: GLGraphicsStateGuardian::release_vertex_buffer
//       Access: Public, Virtual
//  Description: Frees the GL resources previously allocated for the
//               data.  This function should never be called
//               directly; instead, call Data::release() (or simply
//               let the Data destruct).
////////////////////////////////////////////////////////////////////
void CLP(GraphicsStateGuardian)::
release_vertex_buffer(VertexBufferContext *vbc) {
  nassertv(_supports_buffers);
  
  CLP(VertexBufferContext) *gvbc = DCAST(CLP(VertexBufferContext), vbc);

  if (GLCAT.is_debug()) {
    GLCAT.debug()
      << "deleting vertex buffer " << gvbc->_index << "\n";
  }
  _glDeleteBuffers(1, &gvbc->_index);
  report_my_gl_errors();

  gvbc->_index = 0;

  delete gvbc;
}

////////////////////////////////////////////////////////////////////
//     Function: GLGraphicsStateGuardian::setup_array_data
//       Access: Public
//  Description: Internal function to bind a buffer object for the
//               indicated data array, if appropriate, or to unbind a
//               buffer object if it should be rendered from client
//               memory.
//
//               If the buffer object is bound, this function returns
//               NULL (reprsenting the start of the buffer object in
//               server memory); if the buffer object is not bound,
//               this function returns the pointer to the data array
//               in client memory, that is, the data array passed in.
////////////////////////////////////////////////////////////////////
const unsigned char *CLP(GraphicsStateGuardian)::
setup_array_data(const qpGeomVertexArrayData *data) {
  if (!_supports_buffers) {
    // No support for buffer objects; always render from client.
    return data->get_data();
  }
  if (!vertex_buffers || _geom_display_list != 0 ||
      data->get_usage_hint() == qpGeom::UH_client) {
    // The array specifies client rendering only, or buffer objects
    // are configured off.
    if (_current_vbuffer_index != 0) {
      _glBindBuffer(GL_ARRAY_BUFFER, 0);
      _current_vbuffer_index = 0;
    }
    return data->get_data();
  }

  // Prepare the buffer object and bind it.
  VertexBufferContext *vbc = ((qpGeomVertexArrayData *)data)->prepare_now(get_prepared_objects(), this);
  nassertr(vbc != (VertexBufferContext *)NULL, data->get_data());
  apply_vertex_buffer(vbc);

  // NULL is the OpenGL convention for the first byte of the buffer object.
  return NULL;
}

////////////////////////////////////////////////////////////////////
//     Function: GLGraphicsStateGuardian::prepare_index_buffer
//       Access: Public, Virtual
//  Description: Creates a new retained-mode representation of the
//               given data, and returns a newly-allocated
//               IndexBufferContext pointer to reference it.  It is the
//               responsibility of the calling function to later
//               call release_index_buffer() with this same pointer (which
//               will also delete the pointer).
//
//               This function should not be called directly to
//               prepare a buffer.  Instead, call Geom::prepare().
////////////////////////////////////////////////////////////////////
IndexBufferContext *CLP(GraphicsStateGuardian)::
prepare_index_buffer(qpGeomPrimitive *data) {
  if (_supports_buffers) {
    CLP(IndexBufferContext) *gibc = new CLP(IndexBufferContext)(data);
    _glGenBuffers(1, &gibc->_index);

    if (GLCAT.is_debug()) {
      GLCAT.debug()
        << "creating index buffer " << gibc->_index << ": "
        << data->get_num_vertices() << " indices (" 
        << data->get_vertices()->get_array_format()->get_column(0)->get_numeric_type()
        << ")\n";
    }
    
    report_my_gl_errors();
    return gibc;
  }

  return NULL;
}

////////////////////////////////////////////////////////////////////
//     Function: GLGraphicsStateGuardian::apply_index_buffer
//       Access: Public
//  Description: Makes the data the currently available data for
//               rendering.
////////////////////////////////////////////////////////////////////
void CLP(GraphicsStateGuardian)::
apply_index_buffer(IndexBufferContext *ibc) {
  nassertv(_supports_buffers);

  CLP(IndexBufferContext) *gibc = DCAST(CLP(IndexBufferContext), ibc);

  if (_current_ibuffer_index != gibc->_index) {
    _glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, gibc->_index);
    _current_ibuffer_index = gibc->_index;
    add_to_index_buffer_record(gibc);
  }
  
  if (gibc->was_modified()) {
    PStatTimer timer(_load_index_buffer_pcollector);
    int num_bytes = gibc->get_data()->get_data_size_bytes();
    if (GLCAT.is_spam()) {
      GLCAT.spam()
        << "copying " << num_bytes
        << " bytes into index buffer " << gibc->_index << "\n";
    }
    if (gibc->changed_size() || gibc->changed_usage_hint()) {
      _glBufferData(GL_ELEMENT_ARRAY_BUFFER, num_bytes,
                    gibc->get_data()->get_data(), 
                    get_usage(gibc->get_data()->get_usage_hint()));

    } else {
      _glBufferSubData(GL_ELEMENT_ARRAY_BUFFER, 0, num_bytes,
                       gibc->get_data()->get_data());
    }
    _data_transferred_pcollector.add_level(num_bytes);
    add_to_total_buffer_record(gibc);
    gibc->mark_loaded();
  }

  report_my_gl_errors();
}

////////////////////////////////////////////////////////////////////
//     Function: GLGraphicsStateGuardian::release_index_buffer
//       Access: Public, Virtual
//  Description: Frees the GL resources previously allocated for the
//               data.  This function should never be called
//               directly; instead, call Data::release() (or simply
//               let the Data destruct).
////////////////////////////////////////////////////////////////////
void CLP(GraphicsStateGuardian)::
release_index_buffer(IndexBufferContext *ibc) {
  nassertv(_supports_buffers);
  
  CLP(IndexBufferContext) *gibc = DCAST(CLP(IndexBufferContext), ibc);

  if (GLCAT.is_debug()) {
    GLCAT.debug()
      << "deleting index buffer " << gibc->_index << "\n";
  }
  _glDeleteBuffers(1, &gibc->_index);
  report_my_gl_errors();

  gibc->_index = 0;

  delete gibc;
}

////////////////////////////////////////////////////////////////////
//     Function: GLGraphicsStateGuardian::setup_primitive
//       Access: Public
//  Description: Internal function to bind a buffer object for the
//               indicated primitive's index list, if appropriate, or
//               to unbind a buffer object if it should be rendered
//               from client memory.
//
//               If the buffer object is bound, this function returns
//               NULL (reprsenting the start of the buffer object in
//               server memory); if the buffer object is not bound,
//               this function returns the pointer to the data array
//               in client memory, that is, the data array passed in.
////////////////////////////////////////////////////////////////////
const unsigned char *CLP(GraphicsStateGuardian)::
setup_primitive(const qpGeomPrimitive *data) {
  if (!_supports_buffers) {
    // No support for buffer objects; always render from client.
    return data->get_data();
  }
  if (!vertex_buffers || _geom_display_list != 0 ||
      data->get_usage_hint() == qpGeom::UH_client) {
    // The array specifies client rendering only, or buffer objects
    // are configured off.
    if (_current_ibuffer_index != 0) {
      _glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
      _current_ibuffer_index = 0;
    }
    return data->get_data();
  }

  // Prepare the buffer object and bind it.
  IndexBufferContext *ibc = ((qpGeomPrimitive *)data)->prepare_now(get_prepared_objects(), this);
  nassertr(ibc != (IndexBufferContext *)NULL, data->get_data());
  apply_index_buffer(ibc);

  // NULL is the OpenGL convention for the first byte of the buffer object.
  return NULL;
}

////////////////////////////////////////////////////////////////////
//     Function: GLGraphicsStateGuardian::get_geom_munger
//       Access: Public, Virtual
//  Description: Creates a new GeomMunger object to munge vertices
//               appropriate to this GSG for the indicated state.
////////////////////////////////////////////////////////////////////
CPT(qpGeomMunger) CLP(GraphicsStateGuardian)::
get_geom_munger(const RenderState *state) {
  PT(CLP(GeomMunger)) munger = new CLP(GeomMunger)(this, state);
  return qpGeomMunger::register_munger(munger);
}

////////////////////////////////////////////////////////////////////
//     Function: GLGraphicsStateGuardian::framebuffer_copy_to_texture
//       Access: Public, Virtual
//  Description: Copy the pixels within the indicated display
//               region from the framebuffer into texture memory.
//
//               If z > -1, it is the cube map index into which to
//               copy.
////////////////////////////////////////////////////////////////////
void CLP(GraphicsStateGuardian)::
framebuffer_copy_to_texture(Texture *tex, int z, const DisplayRegion *dr,
                            const RenderBuffer &rb) {
  nassertv(tex != NULL && dr != NULL);
  set_read_buffer(rb);

  int xo, yo, w, h;
  dr->get_region_pixels(xo, yo, w, h);

  tex->set_x_size(w);
  tex->set_y_size(h);

  if (tex->get_match_framebuffer_format()) {
    FrameBufferProperties properties = get_properties();
    int mode = properties.get_frame_buffer_mode();
    if (mode & FrameBufferProperties::FM_alpha) {
      tex->set_format(Texture::F_rgba);
    } else {
      tex->set_format(Texture::F_rgb);
    }
  }

  TextureContext *tc = tex->prepare_now(get_prepared_objects(), this);
  nassertv(tc != (TextureContext *)NULL);
  bind_texture(tc);

  if (z >= 0) {
    // Copy to a cube map face.
    nassertv(z < 6);
    nassertv(tex->get_texture_type() == Texture::TT_cube_map);

    if (_supports_cube_map) {
      // We cleverly defined the cube map faces to fall in the same
      // order as the GL constants are defined, so we can just make this
      // simple addition to get to the right GL constant.
      GLP(CopyTexImage2D)(GL_TEXTURE_CUBE_MAP_POSITIVE_X + z, 0,
                          get_internal_image_format(tex->get_format()),
                          xo, yo, w, h, 0);
    }

  } else {
    // Copy to a regular texture.
    nassertv(tex->get_texture_type() == Texture::TT_2d_texture);
    GLP(CopyTexImage2D)(GL_TEXTURE_2D, 0,
                        get_internal_image_format(tex->get_format()),
                        xo, yo, w, h, 0);
  }

  // Clear the internal texture state, since we've just monkeyed with it.
  modify_state(get_untextured_state());
}

////////////////////////////////////////////////////////////////////
//     Function: GLGraphicsStateGuardian::framebuffer_copy_to_ram
//       Access: Public, Virtual
//  Description: Copy the pixels within the indicated display region
//               from the framebuffer into system memory, not texture
//               memory.  Returns true on success, false on failure.
//
//               This completely redefines the ram image of the
//               indicated texture.
////////////////////////////////////////////////////////////////////
bool CLP(GraphicsStateGuardian)::
framebuffer_copy_to_ram(Texture *tex, int z, const DisplayRegion *dr,
                        const RenderBuffer &rb) {
  nassertr(tex != NULL && dr != NULL, false);
  set_read_buffer(rb);
  GLP(PixelStorei)(GL_PACK_ALIGNMENT, 1);

  // Bug fix for RE, RE2, and VTX - need to disable texturing in order
  // for GLP(ReadPixels)() to work
  // NOTE: reading the depth buffer is *much* slower than reading the
  // color buffer
  modify_state(get_untextured_state());

  int xo, yo, w, h;
  dr->get_region_pixels(xo, yo, w, h);

  const FrameBufferProperties &properties = get_properties();

  Texture::ComponentType component_type;
  if (properties.get_color_bits() <= 24) {
    component_type = Texture::T_unsigned_byte;
  } else {
    component_type = Texture::T_unsigned_short;
  }

  Texture::Format format;
  if (properties.get_frame_buffer_mode() & FrameBufferProperties::FM_alpha) {
    format = Texture::F_rgba;
  } else {
    format = Texture::F_rgb;
  }

  Texture::TextureType texture_type;
  if (z >= 0) {
    texture_type = Texture::TT_cube_map;
  } else {
    texture_type = Texture::TT_2d_texture;
  }

  if (tex->get_x_size() != w || tex->get_y_size() != h ||
      tex->get_component_type() != component_type ||
      tex->get_format() != format ||
      tex->get_texture_type() != texture_type) {
    // Re-setup the texture; its properties have changed.
    tex->setup_texture(texture_type, w, h, 1, component_type, format);
  }

  GLenum external_format = get_external_image_format(format);

#ifdef GSG_VERBOSE
  GLCAT.debug()
    << "glReadPixels(" << xo << ", " << yo << ", " << w << ", " << h << ", ";
  switch (external_format) {
  case GL_DEPTH_COMPONENT:
    GLCAT.debug(false) << "GL_DEPTH_COMPONENT, ";
    break;
  case GL_RGB:
    GLCAT.debug(false) << "GL_RGB, ";
    break;
  case GL_RGBA:
    GLCAT.debug(false) << "GL_RGBA, ";
    break;
  case GL_BGR:
    GLCAT.debug(false) << "GL_BGR, ";
    break;
  case GL_BGRA:
    GLCAT.debug(false) << "GL_BGRA, ";
    break;
  default:
    GLCAT.debug(false) << "unknown, ";
    break;
  }
  switch (get_component_type(component_type)) {
  case GL_UNSIGNED_BYTE:
    GLCAT.debug(false) << "GL_UNSIGNED_BYTE, ";
    break;
  case GL_UNSIGNED_SHORT:
    GLCAT.debug(false) << "GL_UNSIGNED_SHORT, ";
    break;
  case GL_FLOAT:
    GLCAT.debug(false) << "GL_FLOAT, ";
    break;
  default:
    GLCAT.debug(false) << "unknown, ";
    break;
  }
  GLCAT.debug(false)
    << ")" << endl;
#endif

  unsigned char *image = tex->modify_ram_image();
  if (z >= 0) {
    nassertr(z < tex->get_z_size(), false);
    image += z * tex->get_expected_ram_page_size();
  }

  GLP(ReadPixels)(xo, yo, w, h,
                  external_format, get_component_type(component_type),
                  image);

  // We may have to reverse the byte ordering of the image if GL
  // didn't do it for us.  This assumes we render out the six faces of
  // a cube map in ascending order, since we can't do this until we
  // have rendered the last face.
  if (!_supports_bgr && (z == -1 || z == 5)) {
    tex->set_ram_image(fix_component_ordering(tex->get_ram_image(), 
                                              external_format, tex));
  }

  report_my_gl_errors();
  return true;
}

////////////////////////////////////////////////////////////////////
//     Function: GLGraphicsStateGuardian::apply_material
//       Access: Public, Virtual
//  Description:
////////////////////////////////////////////////////////////////////
void CLP(GraphicsStateGuardian)::apply_material(const Material *material) {
  GLenum face = material->get_twoside() ? GL_FRONT_AND_BACK : GL_FRONT;

  GLP(Materialfv)(face, GL_SPECULAR, material->get_specular().get_data());
  GLP(Materialfv)(face, GL_EMISSION, material->get_emission().get_data());
  GLP(Materialf)(face, GL_SHININESS, material->get_shininess());

  if (material->has_ambient() && material->has_diffuse()) {
    // The material has both an ambient and diffuse specified.  This
    // means we do not need glMaterialColor().
    GLP(Disable)(GL_COLOR_MATERIAL);
    GLP(Materialfv)(face, GL_AMBIENT, material->get_ambient().get_data());
    GLP(Materialfv)(face, GL_DIFFUSE, material->get_diffuse().get_data());

  } else if (material->has_ambient()) {
    // The material specifies an ambient, but not a diffuse component.
    // The diffuse component comes from the object's color.
    GLP(Materialfv)(face, GL_AMBIENT, material->get_ambient().get_data());
    GLP(ColorMaterial)(face, GL_DIFFUSE);
    GLP(Enable)(GL_COLOR_MATERIAL);

  } else if (material->has_diffuse()) {
    // The material specifies a diffuse, but not an ambient component.
    // The ambient component comes from the object's color.
    GLP(Materialfv)(face, GL_DIFFUSE, material->get_diffuse().get_data());
    GLP(ColorMaterial)(face, GL_AMBIENT);
    GLP(Enable)(GL_COLOR_MATERIAL);

  } else {
    // The material specifies neither a diffuse nor an ambient
    // component.  Both components come from the object's color.
    GLP(ColorMaterial)(face, GL_AMBIENT_AND_DIFFUSE);
    GLP(Enable)(GL_COLOR_MATERIAL);
  }

  GLP(LightModeli)(GL_LIGHT_MODEL_LOCAL_VIEWER, material->get_local());
  GLP(LightModeli)(GL_LIGHT_MODEL_TWO_SIDE, material->get_twoside());
  report_my_gl_errors();
}

////////////////////////////////////////////////////////////////////
//     Function: GLGraphicsStateGuardian::apply_fog
//       Access: Public, Virtual
//  Description:
////////////////////////////////////////////////////////////////////
void CLP(GraphicsStateGuardian)::
apply_fog(Fog *fog) {
  Fog::Mode fmode = fog->get_mode();
  GLP(Fogi)(GL_FOG_MODE, get_fog_mode_type(fmode));

  if (fmode == Fog::M_linear) {
    float onset, opaque;
    fog->get_linear_range(onset, opaque);
    GLP(Fogf)(GL_FOG_START, onset);
    GLP(Fogf)(GL_FOG_END, opaque);

  } else {
    // Exponential fog is always camera-relative.
    GLP(Fogf)(GL_FOG_DENSITY, fog->get_exp_density());
  }

  GLP(Fogfv)(GL_FOG_COLOR, fog->get_color().get_data());
  report_my_gl_errors();
}

////////////////////////////////////////////////////////////////////
//     Function: GLGraphicsStateGuardian::issue_transform
//       Access: Public, Virtual
//  Description: Sends the indicated transform matrix to the graphics
//               API to be applied to future vertices.
////////////////////////////////////////////////////////////////////
void CLP(GraphicsStateGuardian)::
issue_transform(const TransformState *transform) {
#ifdef GSG_VERBOSE
  GLCAT.spam()
    << "glLoadMatrix(GL_MODELVIEW): " << transform->get_mat() << endl;
#endif
  DO_PSTATS_STUFF(_transform_state_pcollector.add_level(1));
  GLP(MatrixMode)(GL_MODELVIEW);
  GLP(LoadMatrixf)(transform->get_mat().get_data());
  _transform_stale = false;

  _transform = transform;
  if (_auto_rescale_normal) {
    do_auto_rescale_normal();
  }

  report_my_gl_errors();
}

////////////////////////////////////////////////////////////////////
//     Function: GLGraphicsStateGuardian::issue_tex_matrix
//       Access: Public, Virtual
//  Description:
////////////////////////////////////////////////////////////////////
void CLP(GraphicsStateGuardian)::
issue_tex_matrix(const TexMatrixAttrib *attrib) {
  // We don't apply the texture matrix right away, since we might yet
  // get a TextureAttrib that changes the set of TextureStages we have
  // active.  Instead, we simply set a flag that indicates we need to
  // re-issue the texture matrix after all of the other attribs are
  // done being issued.
  _current_tex_mat = attrib;
  _needs_tex_mat = true;
}

////////////////////////////////////////////////////////////////////
//     Function: GLGraphicsStateGuardian::issue_tex_gen
//       Access: Public, Virtual
//  Description:
////////////////////////////////////////////////////////////////////
void CLP(GraphicsStateGuardian)::
issue_tex_gen(const TexGenAttrib *attrib) {
  // We don't apply the texture coordinate generation commands right
  // away, since we might yet get a TextureAttrib that changes the set
  // of TextureStages we have active.  Instead, we simply set a flag
  // that indicates we need to re-issue the TexGenAttrib after all of
  // the other attribs are done being issued.
  _current_tex_gen = attrib;
  _needs_tex_gen = true;
}

////////////////////////////////////////////////////////////////////
//     Function: GLGraphicsStateGuardian::issue_shade_model
//       Access: Public, Virtual
//  Description:
////////////////////////////////////////////////////////////////////
void CLP(GraphicsStateGuardian)::
issue_shade_model(const ShadeModelAttrib *attrib) {
  switch (attrib->get_mode()) {
  case ShadeModelAttrib::M_smooth:
    GLP(ShadeModel)(GL_SMOOTH);
    _flat_shade_model = false;
    break;

  case ShadeModelAttrib::M_flat:
    GLP(ShadeModel)(GL_FLAT);
    _flat_shade_model = true;
    break;
  }
}




////////////////////////////////////////////////////////////////////
//     Function: GLGraphicsStateGuardian::issue_cg_shader_bind
//       Access: Public, Virtual
//  Description: Bind shader of current node
//               and unbind the shader of the previous node
//               Create a new GLCgShaderContext if this shader
//               object is coming in for the first time
//               Also maintain the map of CgShader objects to
//               respective GLCgShaderContexts
////////////////////////////////////////////////////////////////////
#ifdef HAVE_CGGL
void CLP(GraphicsStateGuardian)::
issue_cg_shader_bind(const CgShaderAttrib *attrib) {

  if (attrib->is_off()) { //Current node has no shaders
    if (_cg_shader != (CgShader *) NULL) {
      _gl_cg_shader_contexts[_cg_shader]->un_bind();// Prev node had shaders
    }    
    _cg_shader = attrib->get_cg_shader();//Store current node.. here NULL 
  } else {// Current node has shaders
    if (_cg_shader != (CgShader *) NULL) {
      _gl_cg_shader_contexts[_cg_shader]->un_bind();// Prev node had shaders
    }
    _cg_shader = attrib->get_cg_shader();//Store current node  
    CGSHADERCONTEXTS::const_iterator csci;
    csci = _gl_cg_shader_contexts.find(_cg_shader);
    if (csci != _gl_cg_shader_contexts.end()) { // Already have context?
      (*csci).second->bind(this); // Bind the current shader
    } else {// First time CgShader object...need to make a new GLCgShaderContext
      PT(CLP(CgShaderContext)) csc = new CLP(CgShaderContext)(_cg_shader);
      _cg_shader->load_shaders(); // Profiles created lets load from HD
      csc->load_shaders(); // Programs loaded, compile and download to GPU
      CGSHADERCONTEXTS::value_type shader_and_context(_cg_shader, csc);
      _gl_cg_shader_contexts.insert(shader_and_context);
      csc->bind(this);// Bind the new shader
    }
  }
}
#endif

////////////////////////////////////////////////////////////////////
//     Function: GLGraphicsStateGuardian::issue_material
//       Access: Public, Virtual
//  Description:
////////////////////////////////////////////////////////////////////
void CLP(GraphicsStateGuardian)::
issue_material(const MaterialAttrib *attrib) {
  const Material *material = attrib->get_material();
  if (material != (const Material *)NULL) {
    apply_material(material);
  } else {
    // Apply a default material when materials are turned off.
    Material empty;
    apply_material(&empty);
  }
  report_my_gl_errors();
}

////////////////////////////////////////////////////////////////////
//     Function: GLGraphicsStateGuardian::issue_render_mode
//       Access: Public, Virtual
//  Description:
////////////////////////////////////////////////////////////////////
void CLP(GraphicsStateGuardian)::
issue_render_mode(const RenderModeAttrib *attrib) {
  _render_mode = attrib->get_mode();
  _point_size = attrib->get_thickness();
  _point_perspective = attrib->get_perspective();

  switch (_render_mode) {
  case RenderModeAttrib::M_unchanged:
  case RenderModeAttrib::M_filled:
    GLP(PolygonMode)(GL_FRONT_AND_BACK, GL_FILL);
    break;

  case RenderModeAttrib::M_wireframe:
    GLP(PolygonMode)(GL_FRONT_AND_BACK, GL_LINE);
    break;

  case RenderModeAttrib::M_point:
    GLP(PolygonMode)(GL_FRONT_AND_BACK, GL_POINT);
    break;

  default:
    GLCAT.error()
      << "Unknown render mode " << (int)_render_mode << endl;
  }

  // The thickness affects both the line width and the point size.
  GLP(LineWidth)(_point_size);
  GLP(PointSize)(_point_size);
  report_my_gl_errors();

  do_point_size();
}

////////////////////////////////////////////////////////////////////
//     Function: GLGraphicsStateGuardian::issue_antialias
//       Access: Public, Virtual
//  Description:
////////////////////////////////////////////////////////////////////
void CLP(GraphicsStateGuardian)::
issue_antialias(const AntialiasAttrib *attrib) {
  if (attrib->get_mode_type() == AntialiasAttrib::M_auto) {
    // In this special mode, we must enable antialiasing on a
    // case-by-case basis, because we enable it differently for
    // polygons and for points and lines.
    _auto_antialias_mode = true;

  } else {
    // Otherwise, explicitly enable or disable according to the bits
    // that are set.  But if multisample is requested and supported,
    // don't use the other bits at all (they will be ignored by GL
    // anyway).
    _auto_antialias_mode = false;
    unsigned short mode = attrib->get_mode();

    if (_supports_multisample &&
        (mode & AntialiasAttrib::M_multisample) != 0) {
      enable_multisample_antialias(true);

    } else {
      enable_multisample_antialias(false);
      enable_line_smooth((mode & AntialiasAttrib::M_line) != 0);
      enable_point_smooth((mode & AntialiasAttrib::M_point) != 0);
      enable_polygon_smooth((mode & AntialiasAttrib::M_polygon) != 0);
    }
  }

  switch (attrib->get_mode_quality()) {
  case AntialiasAttrib::M_faster:
    GLP(Hint)(GL_LINE_SMOOTH_HINT, GL_FASTEST);
    GLP(Hint)(GL_POINT_SMOOTH_HINT, GL_FASTEST);
    GLP(Hint)(GL_POLYGON_SMOOTH_HINT, GL_FASTEST);
    break;

  case AntialiasAttrib::M_better:
    GLP(Hint)(GL_LINE_SMOOTH_HINT, GL_NICEST);
    GLP(Hint)(GL_POINT_SMOOTH_HINT, GL_NICEST);
    GLP(Hint)(GL_POLYGON_SMOOTH_HINT, GL_NICEST);
    break;

  default:
    GLP(Hint)(GL_LINE_SMOOTH_HINT, GL_DONT_CARE);
    GLP(Hint)(GL_POINT_SMOOTH_HINT, GL_DONT_CARE);
    GLP(Hint)(GL_POLYGON_SMOOTH_HINT, GL_DONT_CARE);
    break;
  }

  report_my_gl_errors();
}

////////////////////////////////////////////////////////////////////
//     Function: GLGraphicsStateGuardian::issue_rescale_normal
//       Access: Public, Virtual
//  Description:
////////////////////////////////////////////////////////////////////
void CLP(GraphicsStateGuardian)::
issue_rescale_normal(const RescaleNormalAttrib *attrib) {
  RescaleNormalAttrib::Mode mode = attrib->get_mode();

  _auto_rescale_normal = false;

  switch (mode) {
  case RescaleNormalAttrib::M_none:
    GLP(Disable)(GL_NORMALIZE);
    if (_supports_rescale_normal) {
      GLP(Disable)(GL_RESCALE_NORMAL);
    }
    break;

  case RescaleNormalAttrib::M_rescale:
    if (_supports_rescale_normal) {
      GLP(Enable)(GL_RESCALE_NORMAL);
      GLP(Disable)(GL_NORMALIZE);
    } else {
      GLP(Enable)(GL_NORMALIZE);
    }
    break;

  case RescaleNormalAttrib::M_normalize:
    GLP(Enable)(GL_NORMALIZE);
    if (_supports_rescale_normal) {
      GLP(Disable)(GL_RESCALE_NORMAL);
    }
    break;

  case RescaleNormalAttrib::M_auto:
    _auto_rescale_normal = true;
    do_auto_rescale_normal();
    break;

  default:
    GLCAT.error()
      << "Unknown rescale_normal mode " << (int)mode << endl;
  }
  report_my_gl_errors();
}

////////////////////////////////////////////////////////////////////
//     Function: GLGraphicsStateGuardian::issue_texture_apply
//       Access: Public, Virtual
//  Description:
////////////////////////////////////////////////////////////////////
void CLP(GraphicsStateGuardian)::
issue_texture_apply(const TextureApplyAttrib *) {
  // This attrib is no longer used; it is replaced by the parameters
  // within TextureStage.
  return;
}

////////////////////////////////////////////////////////////////////
//     Function: GLGraphicsStateGuardian::issue_color_write
//       Access: Public, Virtual
//  Description:
////////////////////////////////////////////////////////////////////
void CLP(GraphicsStateGuardian)::
issue_color_write(const ColorWriteAttrib *attrib) {
  // If we did not override this function, the default implementation
  // would achieve turning off color writes by changing the blend mode
  // in set_blend_mode().  However, since GL does support an easy way
  // to disable writes to the color buffer, we can take advantage of
  // it here.
  if (CLP(color_mask)) {
    ColorWriteAttrib::Mode mode = attrib->get_mode();
    if (mode == ColorWriteAttrib::M_off) {
      GLP(ColorMask)(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
    } else {
      GLP(ColorMask)(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    }
    report_my_gl_errors();

  } else {
    // Some implementations don't seem to handle GLP(ColorMask)() very
    // robustly, however, so we provide this fallback.
    GraphicsStateGuardian::issue_color_write(attrib);
  }
}

// PandaCompareFunc - 1 + 0x200 === GL_NEVER, etc.  order is sequential
#define PANDA_TO_GL_COMPAREFUNC(PANDACMPFUNC) (PANDACMPFUNC-1 +0x200)

////////////////////////////////////////////////////////////////////
//     Function: GLGraphicsStateGuardian::issue_depth_test
//       Access: Public, Virtual
//  Description:
////////////////////////////////////////////////////////////////////
void CLP(GraphicsStateGuardian)::
issue_depth_test(const DepthTestAttrib *attrib) {
  DepthTestAttrib::PandaCompareFunc mode = attrib->get_mode();
  if (mode == DepthTestAttrib::M_none) {
    enable_depth_test(false);
  } else {
    enable_depth_test(true);
    GLP(DepthFunc)(PANDA_TO_GL_COMPAREFUNC(mode));
  }
  report_my_gl_errors();
}

////////////////////////////////////////////////////////////////////
//     Function: GLGraphicsStateGuardian::issue_alpha_test
//       Access: Public, Virtual
//  Description:
////////////////////////////////////////////////////////////////////
void CLP(GraphicsStateGuardian)::
issue_alpha_test(const AlphaTestAttrib *attrib) {
  AlphaTestAttrib::PandaCompareFunc mode = attrib->get_mode();
  if (mode == AlphaTestAttrib::M_none) {
    enable_alpha_test(false);
  } else {
    assert(GL_NEVER==(AlphaTestAttrib::M_never-1+0x200));
    GLP(AlphaFunc)(PANDA_TO_GL_COMPAREFUNC(mode), attrib->get_reference_alpha());
    enable_alpha_test(true);
  }
}

////////////////////////////////////////////////////////////////////
//     Function: GLGraphicsStateGuardian::issue_depth_write
//       Access: Public, Virtual
//  Description:
////////////////////////////////////////////////////////////////////
void CLP(GraphicsStateGuardian)::
issue_depth_write(const DepthWriteAttrib *attrib) {
  DepthWriteAttrib::Mode mode = attrib->get_mode();
  if (mode == DepthWriteAttrib::M_off) {
    GLP(DepthMask)(GL_FALSE);
  } else {
    GLP(DepthMask)(GL_TRUE);
  }
  report_my_gl_errors();
}

////////////////////////////////////////////////////////////////////
//     Function: GLGraphicsStateGuardian::issue_cull_face
//       Access: Public, Virtual
//  Description:
////////////////////////////////////////////////////////////////////
void CLP(GraphicsStateGuardian)::
issue_cull_face(const CullFaceAttrib *attrib) {
  CullFaceAttrib::Mode mode = attrib->get_effective_mode();

  switch (mode) {
  case CullFaceAttrib::M_cull_none:
    GLP(Disable)(GL_CULL_FACE);
    break;
  case CullFaceAttrib::M_cull_clockwise:
    GLP(Enable)(GL_CULL_FACE);
    GLP(CullFace)(GL_BACK);
    break;
  case CullFaceAttrib::M_cull_counter_clockwise:
    GLP(Enable)(GL_CULL_FACE);
    GLP(CullFace)(GL_FRONT);
    break;
  default:
    GLCAT.error()
      << "invalid cull face mode " << (int)mode << endl;
    break;
  }
  report_my_gl_errors();
}

////////////////////////////////////////////////////////////////////
//     Function: GLGraphicsStateGuardian::issue_fog
//       Access: Public, Virtual
//  Description:
////////////////////////////////////////////////////////////////////
void CLP(GraphicsStateGuardian)::
issue_fog(const FogAttrib *attrib) {
  if (!attrib->is_off()) {
    enable_fog(true);
    Fog *fog = attrib->get_fog();
    nassertv(fog != (Fog *)NULL);
    apply_fog(fog);
  } else {
    enable_fog(false);
  }
  report_my_gl_errors();
}

////////////////////////////////////////////////////////////////////
//     Function: GLGraphicsStateGuardian::issue_depth_offset
//       Access: Public, Virtual
//  Description:
////////////////////////////////////////////////////////////////////
void CLP(GraphicsStateGuardian)::
issue_depth_offset(const DepthOffsetAttrib *attrib) {
  int offset = attrib->get_offset();

  if (offset != 0) {
    // The relationship between these two parameters is a little
    // unclear and poorly explained in the GL man pages.
    GLP(PolygonOffset)((GLfloat) -offset, (GLfloat) -offset);
    enable_polygon_offset(true);

  } else {
    enable_polygon_offset(false);
  }

  report_my_gl_errors();
}

////////////////////////////////////////////////////////////////////
//     Function: GLGraphicsStateGuardian::bind_light
//       Access: Public, Virtual
//  Description: Called the first time a particular light has been
//               bound to a given id within a frame, this should set
//               up the associated hardware light with the light's
//               properties.
////////////////////////////////////////////////////////////////////
void CLP(GraphicsStateGuardian)::
bind_light(PointLight *light_obj, const NodePath &light, int light_id) {
  GLenum id = get_light_id(light_id);
  static const Colorf black(0.0f, 0.0f, 0.0f, 1.0f);
  GLP(Lightfv)(id, GL_AMBIENT, black.get_data());
  GLP(Lightfv)(id, GL_DIFFUSE, light_obj->get_color().get_data());
  GLP(Lightfv)(id, GL_SPECULAR, light_obj->get_specular_color().get_data());

  // Position needs to specify x, y, z, and w
  // w == 1 implies non-infinite position
  const LMatrix4f &light_mat = light.get_mat(_scene_setup->get_scene_root());
  LPoint3f pos = light_obj->get_point() * light_mat;

  LPoint4f fpos(pos[0], pos[1], pos[2], 1.0f);
  GLP(Lightfv)(id, GL_POSITION, fpos.get_data());

  // GL_SPOT_DIRECTION is not significant when cutoff == 180

  // Exponent == 0 implies uniform light distribution
  GLP(Lightf)(id, GL_SPOT_EXPONENT, 0.0f);

  // Cutoff == 180 means uniform point light source
  GLP(Lightf)(id, GL_SPOT_CUTOFF, 180.0f);

  const LVecBase3f &att = light_obj->get_attenuation();
  GLP(Lightf)(id, GL_CONSTANT_ATTENUATION, att[0]);
  GLP(Lightf)(id, GL_LINEAR_ATTENUATION, att[1]);
  GLP(Lightf)(id, GL_QUADRATIC_ATTENUATION, att[2]);

  report_my_gl_errors();
}

////////////////////////////////////////////////////////////////////
//     Function: GLGraphicsStateGuardian::bind_light
//       Access: Public, Virtual
//  Description: Called the first time a particular light has been
//               bound to a given id within a frame, this should set
//               up the associated hardware light with the light's
//               properties.
////////////////////////////////////////////////////////////////////
void CLP(GraphicsStateGuardian)::
bind_light(DirectionalLight *light_obj, const NodePath &light, int light_id) {
  GLenum id = get_light_id( light_id );
  static const Colorf black(0.0f, 0.0f, 0.0f, 1.0f);
  GLP(Lightfv)(id, GL_AMBIENT, black.get_data());
  GLP(Lightfv)(id, GL_DIFFUSE, light_obj->get_color().get_data());
  GLP(Lightfv)(id, GL_SPECULAR, light_obj->get_specular_color().get_data());

  // Position needs to specify x, y, z, and w.
  // w == 0 implies light is at infinity
  const LMatrix4f &light_mat = light.get_mat(_scene_setup->get_scene_root());
  LVector3f dir = light_obj->get_direction() * light_mat;
  LPoint4f fdir(-dir[0], -dir[1], -dir[2], 0);
  GLP(Lightfv)(id, GL_POSITION, fdir.get_data());

  // GL_SPOT_DIRECTION is not significant when cutoff == 180
  // In this case, position x, y, z specifies direction

  // Exponent == 0 implies uniform light distribution
  GLP(Lightf)(id, GL_SPOT_EXPONENT, 0.0f);

  // Cutoff == 180 means uniform point light source
  GLP(Lightf)(id, GL_SPOT_CUTOFF, 180.0f);

  // Default attenuation values (only spotlight and point light can
  // modify these)
  GLP(Lightf)(id, GL_CONSTANT_ATTENUATION, 1.0f);
  GLP(Lightf)(id, GL_LINEAR_ATTENUATION, 0.0f);
  GLP(Lightf)(id, GL_QUADRATIC_ATTENUATION, 0.0f);

  report_my_gl_errors();
}

////////////////////////////////////////////////////////////////////
//     Function: GLGraphicsStateGuardian::bind_light
//       Access: Public, Virtual
//  Description: Called the first time a particular light has been
//               bound to a given id within a frame, this should set
//               up the associated hardware light with the light's
//               properties.
////////////////////////////////////////////////////////////////////
void CLP(GraphicsStateGuardian)::
bind_light(Spotlight *light_obj, const NodePath &light, int light_id) {
  Lens *lens = light_obj->get_lens();
  nassertv(lens != (Lens *)NULL);

  GLenum id = get_light_id(light_id);
  static const Colorf black(0.0f, 0.0f, 0.0f, 1.0f);
  GLP(Lightfv)(id, GL_AMBIENT, black.get_data());
  GLP(Lightfv)(id, GL_DIFFUSE, light_obj->get_color().get_data());
  GLP(Lightfv)(id, GL_SPECULAR, light_obj->get_specular_color().get_data());

  // Position needs to specify x, y, z, and w
  // w == 1 implies non-infinite position
  const LMatrix4f &light_mat = light.get_mat(_scene_setup->get_scene_root());
  LPoint3f pos = lens->get_nodal_point() * light_mat;
  LVector3f dir = lens->get_view_vector() * light_mat;

  LPoint4f fpos(pos[0], pos[1], pos[2], 1.0f);
  GLP(Lightfv)(id, GL_POSITION, fpos.get_data());
  GLP(Lightfv)(id, GL_SPOT_DIRECTION, dir.get_data());

  GLP(Lightf)(id, GL_SPOT_EXPONENT, light_obj->get_exponent());
  GLP(Lightf)(id, GL_SPOT_CUTOFF, lens->get_hfov());

  const LVecBase3f &att = light_obj->get_attenuation();
  GLP(Lightf)(id, GL_CONSTANT_ATTENUATION, att[0]);
  GLP(Lightf)(id, GL_LINEAR_ATTENUATION, att[1]);
  GLP(Lightf)(id, GL_QUADRATIC_ATTENUATION, att[2]);

  report_my_gl_errors();
}

////////////////////////////////////////////////////////////////////
//     Function: GLGraphicsStateGuardian::wants_texcoords
//       Access: Public, Virtual
//  Description:
////////////////////////////////////////////////////////////////////
bool CLP(GraphicsStateGuardian)::
wants_texcoords() const {
  return true;
}

////////////////////////////////////////////////////////////////////
//     Function: GLGraphicsStateGuardian::compute_distance_to
//       Access: Public, Virtual
//  Description: This function may only be called during a render
//               traversal; it will compute the distance to the
//               indicated point, assumed to be in eye coordinates,
//               from the camera plane.
////////////////////////////////////////////////////////////////////
float CLP(GraphicsStateGuardian)::
compute_distance_to(const LPoint3f &point) const {
  // In the case of a CLP(GraphicsStateGuardian), we know that the
  // modelview matrix already includes the relative transform from the
  // camera, as well as a to-y-up conversion.  Thus, the distance to
  // the camera plane is simply the -z distance.

  return -point[2];
}

////////////////////////////////////////////////////////////////////
//     Function: report_errors_loop
//       Access: Protected, Static
//  Description: The internal implementation of report_errors().
//               Don't call this function; use report_errors()
//               instead.  The return value is true if everything is
//               ok, or false if we should shut down.
////////////////////////////////////////////////////////////////////
bool CLP(GraphicsStateGuardian)::
report_errors_loop(int line, const char *source_file, GLenum error_code,
                   int &error_count) {
#ifndef NDEBUG
  static const int max_gl_errors_reported = 20;

  while ((error_count < max_gl_errors_reported) && 
         (error_code != GL_NO_ERROR)) {
    const GLubyte *error_string = GLUP(ErrorString)(error_code);
    if (error_string != (const GLubyte *)NULL) {
      GLCAT.error()
        << "at " << line << " of " << source_file << ": " 
        << error_string << "\n";
    } else {
      GLCAT.error()
        << "at " << line << " of " << source_file << ": " 
        << "GL error " << (int)error_code << "\n";
    }
    error_code = GLP(GetError)();
    error_count++;
  }

#endif
  return (error_code == GL_NO_ERROR);
}

////////////////////////////////////////////////////////////////////
//     Function: GLGraphicsStateGuardian::show_gl_string
//       Access: Protected
//  Description: Outputs the result of glGetString() on the indicated
//               tag.
////////////////////////////////////////////////////////////////////
void CLP(GraphicsStateGuardian)::
show_gl_string(const string &name, GLenum id) {
  if (GLCAT.is_debug()) {
    const GLubyte *text = GLP(GetString)(id);
    if (text == (const GLubyte *)NULL) {
      GLCAT.debug()
        << "Unable to query " << name << "\n";
    } else {
      GLCAT.debug()
        << name << " = " << (const char *)text << "\n";
    }
  }
}

////////////////////////////////////////////////////////////////////
//     Function: GLGraphicsStateGuardian::get_gl_version
//       Access: Protected, Virtual
//  Description: Queries the runtime version of OpenGL in use.
////////////////////////////////////////////////////////////////////
void CLP(GraphicsStateGuardian)::
get_gl_version() {
  show_gl_string("GL_VENDOR", GL_VENDOR);
  show_gl_string("GL_RENDERER", GL_RENDERER);

  _gl_version_major = 0;
  _gl_version_minor = 0;
  _gl_version_release = 0;

  const GLubyte *text = GLP(GetString)(GL_VERSION);
  if (text == (const GLubyte *)NULL) {
    GLCAT.debug()
      << "Unable to query GL_VERSION\n";
  } else {
    string input((const char *)text);
    size_t space = input.find(' ');
    if (space != string::npos) {
      input = input.substr(0, space);
    }

    vector_string components;
    tokenize(input, components, ".");
    if (components.size() >= 1) {
      string_to_int(components[0], _gl_version_major);
    }
    if (components.size() >= 2) {
      string_to_int(components[1], _gl_version_minor);
    }
    if (components.size() >= 3) {
      string_to_int(components[2], _gl_version_release);
    }

    GLCAT.debug()
      << "GL_VERSION = " << (const char *)text << ", decoded to "
      << _gl_version_major << "." << _gl_version_minor 
      << "." << _gl_version_release << "\n";
  }
}

////////////////////////////////////////////////////////////////////
//     Function: GLGraphicsStateGuardian::save_extensions
//       Access: Protected
//  Description: Separates the string returned by GL_EXTENSIONS (or
//               glx or wgl extensions) into its individual tokens
//               and saves them in the _extensions member.
////////////////////////////////////////////////////////////////////
void CLP(GraphicsStateGuardian)::
save_extensions(const char *extensions) {
  if (extensions != (const char *)NULL) {
    vector_string tokens;
    extract_words(extensions, tokens);
    
    vector_string::iterator ti;
    for (ti = tokens.begin(); ti != tokens.end(); ++ti) {
      _extensions.insert(*ti);
    }
  }
}

////////////////////////////////////////////////////////////////////
//     Function: GLGraphicsStateGuardian::get_extra_extensions
//       Access: Protected, Virtual
//  Description: This may be redefined by a derived class (e.g. glx or
//               wgl) to get whatever further extensions strings may
//               be appropriate to that interface, in addition to the
//               GL extension strings return by glGetString().
////////////////////////////////////////////////////////////////////
void CLP(GraphicsStateGuardian)::
get_extra_extensions() {
}

////////////////////////////////////////////////////////////////////
//     Function: GLGraphicsStateGuardian::report_extensions
//       Access: Protected
//  Description: Outputs the list of GL extensions to notify, if debug
//               mode is enabled.
////////////////////////////////////////////////////////////////////
void CLP(GraphicsStateGuardian)::
report_extensions() const {
  if (GLCAT.is_debug()) {
    GLCAT.debug()
      << "GL Extensions:\n";
    pset<string>::const_iterator ei;
    for (ei = _extensions.begin(); ei != _extensions.end(); ++ei) {
      GLCAT.debug() << (*ei) << "\n";
    }
  }
}

////////////////////////////////////////////////////////////////////
//     Function: GLGraphicsStateGuardian::has_extension
//       Access: Protected
//  Description: Returns true if the indicated extension is reported
//               by the GL system, false otherwise.  The extension
//               name is case-sensitive.
////////////////////////////////////////////////////////////////////
bool CLP(GraphicsStateGuardian)::
has_extension(const string &extension) const {
  return (_extensions.find(extension) != _extensions.end());
}

////////////////////////////////////////////////////////////////////
//     Function: GLGraphicsStateGuardian::is_at_least_version
//       Access: Public
//  Description: Returns true if the runtime GL version number is at
//               least the indicated value, false otherwise.
////////////////////////////////////////////////////////////////////
bool CLP(GraphicsStateGuardian)::
is_at_least_version(int major_version, int minor_version, 
                    int release_version) const {
  if (_gl_version_major < major_version) {
    return false;
  }
  if (_gl_version_minor < minor_version) {
    return false;
  }
  if (_gl_version_release < release_version) {
    return false;
  }
  return true;
}

////////////////////////////////////////////////////////////////////
//     Function: GLGraphicsStateGuardian::get_extension_func
//       Access: Public, Virtual
//  Description: Returns the pointer to the GL extension function with
//               the indicated name.  It is the responsibility of the
//               caller to ensure that the required extension is
//               defined in the OpenGL runtime prior to calling this;
//               it is an error to call this for a function that is
//               not defined.
////////////////////////////////////////////////////////////////////
void *CLP(GraphicsStateGuardian)::
get_extension_func(const char *, const char *) {
  return NULL;
}


////////////////////////////////////////////////////////////////////
//     Function: GLGraphicsStateGuardian::set_draw_buffer
//       Access: Protected
//  Description: Sets up the GLP(DrawBuffer) to render into the buffer
//               indicated by the RenderBuffer object.  This only sets
//               up the color bits; it does not affect the depth,
//               stencil, accum layers.
////////////////////////////////////////////////////////////////////
void CLP(GraphicsStateGuardian)::
set_draw_buffer(const RenderBuffer &rb) {
  switch (rb._buffer_type & RenderBuffer::T_color) {
  case RenderBuffer::T_front:
    GLP(DrawBuffer)(GL_FRONT);
    break;

  case RenderBuffer::T_back:
    GLP(DrawBuffer)(GL_BACK);
    break;

  case RenderBuffer::T_right:
    GLP(DrawBuffer)(GL_RIGHT);
    break;

  case RenderBuffer::T_left:
    GLP(DrawBuffer)(GL_LEFT);
    break;

  case RenderBuffer::T_front_right:
    GLP(DrawBuffer)(GL_FRONT_RIGHT);
    break;

  case RenderBuffer::T_front_left:
    GLP(DrawBuffer)(GL_FRONT_LEFT);
    break;

  case RenderBuffer::T_back_right:
    GLP(DrawBuffer)(GL_BACK_RIGHT);
    break;

  case RenderBuffer::T_back_left:
    GLP(DrawBuffer)(GL_BACK_LEFT);
    break;

  default:
    GLP(DrawBuffer)(GL_FRONT_AND_BACK);
  }
  report_my_gl_errors();
}

////////////////////////////////////////////////////////////////////
//     Function: GLGraphicsStateGuardian::set_read_buffer
//       Access: Protected
//  Description: Sets up the GLP(ReadBuffer) to render into the buffer
//               indicated by the RenderBuffer object.  This only sets
//               up the color bits; it does not affect the depth,
//               stencil, accum layers.
////////////////////////////////////////////////////////////////////
void CLP(GraphicsStateGuardian)::
set_read_buffer(const RenderBuffer &rb) {
  switch (rb._buffer_type & RenderBuffer::T_color) {
  case RenderBuffer::T_front:
    GLP(ReadBuffer)(GL_FRONT);
    break;

  case RenderBuffer::T_back:
    GLP(ReadBuffer)(GL_BACK);
    break;

  case RenderBuffer::T_right:
    GLP(ReadBuffer)(GL_RIGHT);
    break;

  case RenderBuffer::T_left:
    GLP(ReadBuffer)(GL_LEFT);
    break;

  case RenderBuffer::T_front_right:
    GLP(ReadBuffer)(GL_FRONT_RIGHT);
    break;

  case RenderBuffer::T_front_left:
    GLP(ReadBuffer)(GL_FRONT_LEFT);
    break;

  case RenderBuffer::T_back_right:
    GLP(ReadBuffer)(GL_BACK_RIGHT);
    break;

  case RenderBuffer::T_back_left:
    GLP(ReadBuffer)(GL_BACK_LEFT);
    break;

  default:
    GLP(ReadBuffer)(GL_FRONT_AND_BACK);
  }
  report_my_gl_errors();
}


////////////////////////////////////////////////////////////////////
//     Function: GLGraphicsStateGuardian::bind_texture
//       Access: Protected
//  Description:
////////////////////////////////////////////////////////////////////
void CLP(GraphicsStateGuardian)::
bind_texture(TextureContext *tc) {
  CLP(TextureContext) *gtc = DCAST(CLP(TextureContext), tc);
  report_my_gl_errors();

#ifdef GSG_VERBOSE
  Texture *tex = tc->_texture;
  GLCAT.spam()
    << "glBindTexture(): " << tex->get_name() << "(" << (int)gtc->_index
    << ")" << endl;
#endif

  GLenum target = get_texture_target(tc->_texture->get_texture_type());
  if (target != GL_NONE) {
    GLP(BindTexture)(target, gtc->_index);
  }

  report_my_gl_errors();
}

////////////////////////////////////////////////////////////////////
//     Function: GLGraphicsStateGuardian::specify_texture
//       Access: Protected
//  Description:
////////////////////////////////////////////////////////////////////
void CLP(GraphicsStateGuardian)::
specify_texture(Texture *tex) {
  GLenum target = get_texture_target(tex->get_texture_type());
  if (target == GL_NONE) {
    // Unsupported target (e.g. 3-d texturing on GL 1.1).
    return;
  }

  GLP(TexParameteri)(target, GL_TEXTURE_WRAP_S,
                     get_texture_wrap_mode(tex->get_wrap_u()));
  if (target != GL_TEXTURE_1D) {
    GLP(TexParameteri)(target, GL_TEXTURE_WRAP_T,
                       get_texture_wrap_mode(tex->get_wrap_v()));
  }
  if (target == GL_TEXTURE_3D) {
    GLP(TexParameteri)(target, GL_TEXTURE_WRAP_R,
                       get_texture_wrap_mode(tex->get_wrap_w()));
  }

  Colorf border_color = tex->get_border_color();
  GLP(TexParameterfv)(target, GL_TEXTURE_BORDER_COLOR,
                      border_color.get_data());

  Texture::FilterType minfilter = tex->get_minfilter();
  Texture::FilterType magfilter = tex->get_magfilter();
  bool uses_mipmaps = tex->uses_mipmaps() && !CLP(ignore_mipmaps);

#ifndef NDEBUG
  if (CLP(force_mipmaps)) {
    minfilter = Texture::FT_linear_mipmap_linear;
    magfilter = Texture::FT_linear;
    uses_mipmaps = true;
  }
#endif

  if (_supports_generate_mipmap && 
      (auto_generate_mipmaps || !tex->might_have_ram_image())) {
    // If the hardware can automatically generate mipmaps, ask it to
    // do so now, but only if the texture requires them.
    GLP(TexParameteri)(target, GL_GENERATE_MIPMAP, uses_mipmaps);

  } else if (!tex->might_have_ram_image()) {
    // If the hardware can't automatically generate mipmaps, but it's
    // a dynamically generated texture (that is, the RAM image isn't
    // available so it didn't pass through the CPU), then we'd better
    // not try to enable mipmap filtering, since we can't generate
    // mipmaps.
    uses_mipmaps = false;
  }
 
  GLP(TexParameteri)(target, GL_TEXTURE_MIN_FILTER,
                     get_texture_filter_type(minfilter, !uses_mipmaps));
  GLP(TexParameteri)(target, GL_TEXTURE_MAG_FILTER,
                     get_texture_filter_type(magfilter, true));

  report_my_gl_errors();
}

#ifndef NDEBUG
////////////////////////////////////////////////////////////////////
//     Function: compute_gl_image_size
//  Description: Calculates how many bytes GL will expect to read for
//               a texture image, based on the number of pixels and
//               the GL format and type.  This is only used for
//               debugging.
////////////////////////////////////////////////////////////////////
static int
compute_gl_image_size(int x_size, int y_size, int z_size, 
                      int external_format, int type) {
  int num_components = 0;
  switch (external_format) {
  case GL_COLOR_INDEX:
  case GL_STENCIL_INDEX:
  case GL_DEPTH_COMPONENT:
  case GL_RED:
  case GL_GREEN:
  case GL_BLUE:
  case GL_ALPHA:
  case GL_LUMINANCE:
    num_components = 1;
    break;

  case GL_LUMINANCE_ALPHA:
    num_components = 2;
    break;

  case GL_BGR:
  case GL_RGB:
    num_components = 3;
    break;

  case GL_BGRA:
  case GL_RGBA:
    num_components = 4;
    break;
  }

  int pixel_width = 0;
  switch (type) {
  case GL_UNSIGNED_BYTE:
    pixel_width = 1 * num_components;
    break;

  case GL_UNSIGNED_SHORT:
    pixel_width = 2 * num_components;
    break;

  case GL_UNSIGNED_BYTE_3_3_2:
    nassertr(num_components == 3, 0);
    pixel_width = 1;
    break;

  case GL_FLOAT:
    pixel_width = 4 * num_components;
    break;
  }

  return x_size * y_size * z_size * pixel_width;
}
#endif  // NDEBUG

////////////////////////////////////////////////////////////////////
//     Function: GLGraphicsStateGuardian::apply_texture_immediate
//       Access: Protected
//  Description: Sends the texture image to GL.  This can be used to
//               render a texture in immediate mode, or as part of the
//               process of creating a GL texture object.
//
//               The return value is true if successful, or false if
//               the texture has no image.
////////////////////////////////////////////////////////////////////
bool CLP(GraphicsStateGuardian)::
apply_texture_immediate(CLP(TextureContext) *gtc, Texture *tex) {
  CPTA_uchar image = tex->get_ram_image();
  if (image.is_null()) {
    return false;
  }

  int width = tex->get_x_size();
  int height = tex->get_y_size();
  int depth = tex->get_z_size();

  GLint internal_format = get_internal_image_format(tex->get_format());
  GLint external_format = get_external_image_format(tex->get_format());
  GLenum component_type = get_component_type(tex->get_component_type());

  // Ensure that the texture fits within the GL's specified limits.
  int max_dimension;
  switch (tex->get_texture_type()) {
  case Texture::TT_3d_texture:
    max_dimension = _max_3d_texture_dimension;
    break;

  case Texture::TT_cube_map:
    max_dimension = _max_cube_map_dimension;
    break;

  default:
    max_dimension = _max_texture_dimension;
  }

  if (max_dimension == 0) {
    // Guess this GL doesn't support cube mapping/3d textures.
    report_my_gl_errors();
    return false;
  }

  int texel_size = tex->get_num_components() * tex->get_component_width();

  // If it doesn't fit, we have to reduce it on-the-fly.  This is kind
  // of expensive and it doesn't look great; it would have been better
  // if the user had specified max-texture-dimension to reduce the
  // texture at load time instead.  Of course, the user doesn't always
  // know ahead of time what the hardware limits are.
  if (max_dimension > 0) {
    if (width > max_dimension) {
      int byte_chunk = texel_size;
      int stride = 1;
      int new_width = width;
      while (new_width > max_dimension) {
        stride <<= 1;
        new_width >>= 1;
      }
      GLCAT.info()
        << "Reducing width of " << tex->get_name()
        << " from " << width << " to " << new_width << "\n";
      image = reduce_image(image, byte_chunk, stride);
      width = new_width;
    }
    if (height > max_dimension) {
      int byte_chunk = width * texel_size;
      int stride = 1;
      int new_height = height;
      while (new_height > max_dimension) {
        stride <<= 1;
        new_height >>= 1;
      }
      GLCAT.info()
        << "Reducing height of " << tex->get_name()
        << " from " << height << " to " << new_height << "\n";
      image = reduce_image(image, byte_chunk, stride);
      height = new_height;
    }
    if (depth > max_dimension) {
      int byte_chunk = height * width * texel_size;
      int stride = 1;
      int new_depth = depth;
      while (new_depth > max_dimension) {
        stride <<= 1;
        new_depth >>= 1;
      }
      GLCAT.info()
        << "Reducing depth of " << tex->get_name()
        << " from " << depth << " to " << new_depth << "\n";
      image = reduce_image(image, byte_chunk, stride);
      depth = new_depth;
    }
  }

  if (!_supports_bgr) {
    // If the GL doesn't claim to support BGR, we may have to reverse
    // the component ordering of the image.
    image = fix_component_ordering(image, external_format, tex);
  }

#ifndef NDEBUG
  int wanted_size = 
    compute_gl_image_size(width, height, depth, external_format, component_type);
  nassertr(wanted_size == (int)image.size(), false);
#endif  // NDEBUG

  GLP(PixelStorei)(GL_UNPACK_ALIGNMENT, 1);

  bool uses_mipmaps = (tex->uses_mipmaps() && !CLP(ignore_mipmaps)) || CLP(force_mipmaps);

#ifndef NDEBUG
  if (CLP(force_mipmaps)) {
    uses_mipmaps = true;
  }
#endif

  bool success = true;

  if (tex->get_texture_type() == Texture::TT_cube_map) {
    // A cube map must load six different 2-d images (which are stored
    // as the six pages of the system ram image).
    if (!_supports_cube_map) {
      report_my_gl_errors();
      return false;
    }

    size_t page_size = height * width * texel_size;
    const unsigned char *image_base = image;
    
    success = success && upload_texture_image
      (gtc, uses_mipmaps, GL_TEXTURE_CUBE_MAP_POSITIVE_X,
       internal_format, width, height, depth, external_format, component_type,
       image_base);
    image_base += page_size;
    
    success = success && upload_texture_image
      (gtc, uses_mipmaps, GL_TEXTURE_CUBE_MAP_NEGATIVE_X,
       internal_format, width, height, depth, external_format, component_type,
       image_base);
    image_base += page_size;
    
    success = success && upload_texture_image
      (gtc, uses_mipmaps, GL_TEXTURE_CUBE_MAP_POSITIVE_Y,
       internal_format, width, height, depth, external_format, component_type,
       image_base);
    image_base += page_size;
    
    success = success && upload_texture_image
      (gtc, uses_mipmaps, GL_TEXTURE_CUBE_MAP_NEGATIVE_Y,
       internal_format, width, height, depth, external_format, component_type,
       image_base);
    image_base += page_size;
    
    success = success && upload_texture_image
      (gtc, uses_mipmaps, GL_TEXTURE_CUBE_MAP_POSITIVE_Z,
       internal_format, width, height, depth, external_format, component_type,
       image_base);
    image_base += page_size;
    
    success = success && upload_texture_image
      (gtc, uses_mipmaps, GL_TEXTURE_CUBE_MAP_NEGATIVE_Z,
       internal_format, width, height, depth, external_format, component_type,
       image_base);
    image_base += page_size;
    
    nassertr((size_t)(image_base - image) == image.size(), false);

  } else {
    // Any other kind of texture can be loaded all at once.
    success = upload_texture_image
      (gtc, uses_mipmaps, get_texture_target(tex->get_texture_type()),
       internal_format, width, height, depth, external_format, component_type,
       image);
  }

  if (success) {
    gtc->_already_applied = true;
    gtc->_internal_format = internal_format;
    gtc->_width = width;
    gtc->_height = height;
    gtc->_depth = depth;

#ifndef NDEBUG
    if (uses_mipmaps && CLP(save_mipmaps)) {
      save_mipmap_images(tex);
    }
#endif

    report_my_gl_errors();
    return true;
  }

  report_my_gl_errors();
  return false;
}

////////////////////////////////////////////////////////////////////
//     Function: GLGraphicsStateGuardian::upload_texture_image
//       Access: Protected
//  Description: Loads a texture image, or one page of a cube map
//               image, from system RAM to texture memory.
////////////////////////////////////////////////////////////////////
bool CLP(GraphicsStateGuardian)::
upload_texture_image(CLP(TextureContext) *gtc, 
                     bool uses_mipmaps, 
                     GLenum target, GLint internal_format, 
                     int width, int height, int depth,
                     GLint external_format, GLenum component_type, 
                     const unsigned char *image) {
  if (target == GL_NONE) {
    // Unsupported target (e.g. 3-d texturing on GL 1.1).
    return false;
  }
  PStatTimer timer(_load_texture_pcollector);

  if (uses_mipmaps) {
#ifndef NDEBUG
    if (CLP(show_mipmaps) && target == GL_TEXTURE_2D) {
      build_phony_mipmaps(gtc->_texture);
      report_my_gl_errors();
      return true;
      
    } else 
#endif 
      if (!_supports_generate_mipmap || !auto_generate_mipmaps) {
        // We only need to build the mipmaps by hand if the GL
        // doesn't support generating them automatically.
        bool success = true;
#ifdef DO_PSTATS
        _data_transferred_pcollector.add_level(get_external_texture_bytes(width, height, depth, external_format, component_type) * 4 / 3);
#endif
        switch (target) {
        case GL_TEXTURE_1D:
          GLUP(Build1DMipmaps)(target, internal_format, width,
                               external_format, component_type, image);
          break;

        case GL_TEXTURE_3D:
#ifdef GLU_VERSION_1_3
          GLUP(Build3DMipmaps)(target, internal_format,
                               width, height, depth,
                               external_format, component_type, image);
#else  // GLU_VERSION_1_3
          // Prior to GLU 1.3, there was no gluBuild3DMipmaps() call.
          // Just fall through and load the texture without mipmaps.
          GLP(TexParameteri)(target, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
          success = false;
#endif  // GLU_VERSION_1_3
          break;

        default:
          GLUP(Build2DMipmaps)(target, internal_format,
                               width, height,
                               external_format, component_type, image);
        }

        report_my_gl_errors();
        if (success) {
          return true;
        }
      }
  }

  if (!gtc->_already_applied || 
      gtc->_internal_format != internal_format ||
      gtc->_width != width ||
      gtc->_height != height ||
      gtc->_depth != depth) {
    // We need to reload a new image.
#ifdef DO_PSTATS
    _data_transferred_pcollector.add_level(get_external_texture_bytes(width, height, depth, external_format, component_type));
#endif
    switch (target) {
    case GL_TEXTURE_1D:
      GLP(TexImage1D)(target, 0, internal_format,
                      width, 0,
                      external_format, component_type, image);
      break;

    case GL_TEXTURE_3D:
      if (_supports_3d_texture) {
        _glTexImage3D(target, 0, internal_format,
                      width, height, depth, 0,
                      external_format, component_type, image);
      } else {
        report_my_gl_errors();
        return false;
      }
      break;

    default:
      GLP(TexImage2D)(target, 0, internal_format,
                      width, height, 0,
                      external_format, component_type, image);
    }

  } else {
    // We can reload the image over the previous image, possibly
    // saving on texture memory fragmentation.
    switch (target) {
    case GL_TEXTURE_1D:
      GLP(TexSubImage1D)(target, 0, 0, width, 
                         external_format, component_type, image);
      break;

    case GL_TEXTURE_3D:
      if (_supports_3d_texture) {
        _glTexSubImage3D(target, 0, 0, 0, 0, width, height, depth,
                         external_format, component_type, image);
      } else {
        report_my_gl_errors();
        return false;
      }
      break;

    default:
      GLP(TexSubImage2D)(target, 0, 0, 0, width, height,
                         external_format, component_type, image);
      break;
    }
  }

  // Report the error message explicitly if the GL texture creation
  // failed.
  GLenum error_code = GLP(GetError)();
  if (error_code != GL_NO_ERROR) {
    const GLubyte *error_string = GLUP(ErrorString)(error_code);
    GLCAT.error()
      << "GL texture creation failed for " << gtc->_texture->get_name();
    if (error_string != (const GLubyte *)NULL) {
      GLCAT.error(false)
        << " : " << error_string;
    }
    GLCAT.error(false)
      << "\n";
  }

  return true;
}


////////////////////////////////////////////////////////////////////
//     Function: GLGraphicsStateGuardian::get_numeric_type
//       Access: Protected, Static
//  Description: Maps from the Geom's internal numeric type symbols
//               to GL's.
////////////////////////////////////////////////////////////////////
GLenum CLP(GraphicsStateGuardian)::
get_numeric_type(qpGeom::NumericType numeric_type) {
  switch (numeric_type) {
  case qpGeom::NT_uint16:
    return GL_UNSIGNED_SHORT;

  case qpGeom::NT_uint32:
    return GL_UNSIGNED_INT;

  case qpGeom::NT_uint8:
  case qpGeom::NT_packed_dcba:
  case qpGeom::NT_packed_dabc:
    return GL_UNSIGNED_BYTE;
    
  case qpGeom::NT_float32:
    return GL_FLOAT;
  }

  GLCAT.error()
    << "Invalid NumericType value (" << (int)numeric_type << ")\n";
  return GL_UNSIGNED_BYTE;
}

////////////////////////////////////////////////////////////////////
//     Function: GLGraphicsStateGuardian::get_texture_target
//       Access: Protected
//  Description: Maps from the Texture's texture type symbols to
//               GL's.
////////////////////////////////////////////////////////////////////
GLenum CLP(GraphicsStateGuardian)::
get_texture_target(Texture::TextureType texture_type) const {
  switch (texture_type) {
  case Texture::TT_1d_texture:
    return GL_TEXTURE_1D;

  case Texture::TT_2d_texture:
    return GL_TEXTURE_2D;

  case Texture::TT_3d_texture:
    if (_supports_3d_texture) {
      return GL_TEXTURE_3D;
    } else {
      return GL_NONE;
    }

  case Texture::TT_cube_map:
    if (_supports_cube_map) {
      return GL_TEXTURE_CUBE_MAP;
    } else {
      return GL_NONE;
    }
  }

  GLCAT.error() << "Invalid Texture::TextureType value!\n";
  return GL_TEXTURE_2D;
}

////////////////////////////////////////////////////////////////////
//     Function: GLGraphicsStateGuardian::get_texture_wrap_mode
//       Access: Protected, Static
//  Description: Maps from the Texture's internal wrap mode symbols to
//               GL's.
////////////////////////////////////////////////////////////////////
GLenum CLP(GraphicsStateGuardian)::
get_texture_wrap_mode(Texture::WrapMode wm) {
  if (CLP(ignore_clamp)) {
    return GL_REPEAT;
  }
  switch (wm) {
  case Texture::WM_clamp:
    return _edge_clamp;

  case Texture::WM_repeat:
    return GL_REPEAT;

  case Texture::WM_mirror:
    return _mirror_repeat;

  case Texture::WM_mirror_once:
    return _mirror_border_clamp;

  case Texture::WM_border_color:
    return _border_clamp;

  case Texture::WM_invalid:
    break;
  }
  GLCAT.error() << "Invalid Texture::WrapMode value!\n";
  return _edge_clamp;
}

////////////////////////////////////////////////////////////////////
//     Function: GLGraphicsStateGuardian::get_texture_filter_type
//       Access: Protected, Static
//  Description: Maps from the Texture's internal filter type symbols
//               to GL's.
////////////////////////////////////////////////////////////////////
GLenum CLP(GraphicsStateGuardian)::
get_texture_filter_type(Texture::FilterType ft, bool ignore_mipmaps) {
  if (CLP(ignore_filters)) {
    return GL_NEAREST;

  } else if (ignore_mipmaps) {
    switch (ft) {
    case Texture::FT_nearest_mipmap_nearest:
    case Texture::FT_nearest:
      return GL_NEAREST;
    case Texture::FT_linear:
    case Texture::FT_linear_mipmap_nearest:
    case Texture::FT_nearest_mipmap_linear:
    case Texture::FT_linear_mipmap_linear:
      return GL_LINEAR;
    case Texture::FT_invalid:
      break;
    }

  } else {
    switch (ft) {
    case Texture::FT_nearest:
      return GL_NEAREST;
    case Texture::FT_linear:
      return GL_LINEAR;
    case Texture::FT_nearest_mipmap_nearest:
      return GL_NEAREST_MIPMAP_NEAREST;
    case Texture::FT_linear_mipmap_nearest:
      return GL_LINEAR_MIPMAP_NEAREST;
    case Texture::FT_nearest_mipmap_linear:
      return GL_NEAREST_MIPMAP_LINEAR;
    case Texture::FT_linear_mipmap_linear:
      return GL_LINEAR_MIPMAP_LINEAR;
    case Texture::FT_invalid:
      break;
    }
  }
  GLCAT.error() << "Invalid Texture::FilterType value!\n";
  return GL_NEAREST;
}

////////////////////////////////////////////////////////////////////
//     Function: GLGraphicsStateGuardian::get_component_type
//       Access: Protected, Static
//  Description: Maps from the Texture's internal ComponentType symbols
//               to GL's.
////////////////////////////////////////////////////////////////////
GLenum CLP(GraphicsStateGuardian)::
get_component_type(Texture::ComponentType component_type) {
  switch (component_type) {
  case Texture::T_unsigned_byte:
    return GL_UNSIGNED_BYTE;
  case Texture::T_unsigned_short:
    return GL_UNSIGNED_SHORT;
  case Texture::T_float:
    return GL_FLOAT;

  default:
    GLCAT.error() << "Invalid Texture::Type value!\n";
    return GL_UNSIGNED_BYTE;
  }
}

////////////////////////////////////////////////////////////////////
//     Function: GLGraphicsStateGuardian::get_external_image_format
//       Access: Protected
//  Description: Maps from the Texture's Format symbols
//               to GL's.
////////////////////////////////////////////////////////////////////
GLint CLP(GraphicsStateGuardian)::
get_external_image_format(Texture::Format format) const {
  switch (format) {
  case Texture::F_color_index:
    return GL_COLOR_INDEX;
  case Texture::F_stencil_index:
    return GL_STENCIL_INDEX;
  case Texture::F_depth_component:
    return GL_DEPTH_COMPONENT;
  case Texture::F_red:
    return GL_RED;
  case Texture::F_green:
    return GL_GREEN;
  case Texture::F_blue:
    return GL_BLUE;
  case Texture::F_alpha:
    return GL_ALPHA;
  case Texture::F_rgb:
  case Texture::F_rgb5:
  case Texture::F_rgb8:
  case Texture::F_rgb12:
  case Texture::F_rgb332:
    return _supports_bgr ? GL_BGR : GL_RGB;
  case Texture::F_rgba:
  case Texture::F_rgbm:
  case Texture::F_rgba4:
  case Texture::F_rgba5:
  case Texture::F_rgba8:
  case Texture::F_rgba12:
    return _supports_bgr ? GL_BGRA : GL_RGBA;
  case Texture::F_luminance:
    return GL_LUMINANCE;
  case Texture::F_luminance_alphamask:
  case Texture::F_luminance_alpha:
    return GL_LUMINANCE_ALPHA;
  }
  GLCAT.error()
    << "Invalid Texture::Format value in get_external_image_format(): "
    << (int)format << "\n";
  return GL_RGB;
}

////////////////////////////////////////////////////////////////////
//     Function: GLGraphicsStateGuardian::get_internal_image_format
//       Access: Protected, Static
//  Description: Maps from the Texture's Format symbols to a
//               suitable internal format for GL textures.
////////////////////////////////////////////////////////////////////
GLint CLP(GraphicsStateGuardian)::
get_internal_image_format(Texture::Format format) {
  switch (format) {
  case Texture::F_rgba:
  case Texture::F_rgbm:
    return GL_RGBA;
  case Texture::F_rgba4:
    return GL_RGBA4;
  case Texture::F_rgba8:
    return GL_RGBA8;
  case Texture::F_rgba12:
    return GL_RGBA12;

  case Texture::F_rgb:
    return GL_RGB;
  case Texture::F_rgb5:
    return GL_RGB5;
  case Texture::F_rgba5:
    return GL_RGB5_A1;
  case Texture::F_rgb8:
    return GL_RGB8;
  case Texture::F_rgb12:
    return GL_RGB12;
  case Texture::F_rgb332:
    return GL_R3_G3_B2;

  case Texture::F_alpha:
    return GL_ALPHA;

  case Texture::F_red:
  case Texture::F_green:
  case Texture::F_blue:
  case Texture::F_luminance:
    return GL_LUMINANCE;
  case Texture::F_luminance_alpha:
  case Texture::F_luminance_alphamask:
    return GL_LUMINANCE_ALPHA;

  default:
    GLCAT.error()
      << "Invalid image format in get_internal_image_format(): "
      << (int)format << "\n";
    return GL_RGB;
  }
}

////////////////////////////////////////////////////////////////////
//     Function: GLGraphicsStateGuardian::get_external_texture_bytes
//       Access: Protected, Static
//  Description: Computes the number of bytes that should be in the
//               "external", or local, texture buffer before
//               transferring to OpenGL.  This is just used for
//               sending data to PStats.
////////////////////////////////////////////////////////////////////
int CLP(GraphicsStateGuardian)::
get_external_texture_bytes(int width, int height, int depth,
                           GLint external_format, GLenum component_type) {
  int num_components;
  switch (external_format) {
  case GL_COLOR_INDEX:
  case GL_STENCIL_INDEX:
  case GL_DEPTH_COMPONENT:
  case GL_RED:
  case GL_GREEN:
  case GL_BLUE:
  case GL_ALPHA:
  case GL_LUMINANCE:
    num_components = 1;
    break;

  case GL_LUMINANCE_ALPHA:
    num_components = 2;
    break;

  case GL_BGR: 
  case GL_RGB:
    num_components = 3;
    break;

  case GL_BGRA: 
  case GL_RGBA:
    num_components = 4;
    break;
    
  default:
    GLCAT.error()
      << "Unexpected external_format in get_external_texture_bytes(): "
      << hex << external_format << dec << "\n";
    num_components = 3;
  }

  int component_width;
  switch (component_type) {
  case GL_UNSIGNED_BYTE:
    component_width = 1;
    break;

  case GL_UNSIGNED_SHORT:
    component_width = 2;
    break;

  case GL_FLOAT:
    component_width = 4;
    break;

  default:
    GLCAT.error()
      << "Unexpected component_type in get_external_texture_bytes(): "
      << hex << component_type << dec << "\n";
    component_width = 1;
  }

  return width * height * depth * num_components * component_width;
}

////////////////////////////////////////////////////////////////////
//     Function: GLGraphicsStateGuardian::get_texture_apply_mode_type
//       Access: Protected, Static
//  Description: Maps from the texture stage's mode types
//               to the corresponding OpenGL ids
////////////////////////////////////////////////////////////////////
GLint CLP(GraphicsStateGuardian)::
get_texture_apply_mode_type(TextureStage::Mode am) {
  switch (am) {
  case TextureStage::M_modulate: return GL_MODULATE;
  case TextureStage::M_decal: return GL_DECAL;
  case TextureStage::M_blend: return GL_BLEND;
  case TextureStage::M_replace: return GL_REPLACE;
  case TextureStage::M_add: return GL_ADD;
  case TextureStage::M_combine: return GL_COMBINE;
  case TextureStage::M_blend_color_scale: return GL_BLEND;
  }

  GLCAT.error()
    << "Invalid TextureStage::Mode value" << endl;
  return GL_MODULATE;
}

////////////////////////////////////////////////////////////////////
//     Function: GLGraphicsStateGuardian::get_texture_combine_type
//       Access: Protected, Static
//  Description: Maps from the texture stage's CombineMode types
//               to the corresponding OpenGL ids
////////////////////////////////////////////////////////////////////
GLint CLP(GraphicsStateGuardian)::
get_texture_combine_type(TextureStage::CombineMode cm) {
  switch (cm) {
  case TextureStage::CM_undefined: // fall through
  case TextureStage::CM_replace: return GL_REPLACE;
  case TextureStage::CM_modulate: return GL_MODULATE;
  case TextureStage::CM_add: return GL_ADD;
  case TextureStage::CM_add_signed: return GL_ADD_SIGNED;
  case TextureStage::CM_interpolate: return GL_INTERPOLATE;
  case TextureStage::CM_subtract: return GL_SUBTRACT;
  case TextureStage::CM_dot3_rgb: return GL_DOT3_RGB;
  case TextureStage::CM_dot3_rgba: return GL_DOT3_RGBA;
  }
  GLCAT.error()
    << "Invalid TextureStage::CombineMode value" << endl;
  return GL_REPLACE;
}

////////////////////////////////////////////////////////////////////
//     Function: GLGraphicsStateGuardian::get_texture_src_type
//       Access: Protected, Static
//  Description: Maps from the texture stage's CombineSource types
//               to the corresponding OpenGL ids
////////////////////////////////////////////////////////////////////
GLint CLP(GraphicsStateGuardian)::
get_texture_src_type(TextureStage::CombineSource cs) {
  switch (cs) {
  case TextureStage::CS_undefined: // fall through
  case TextureStage::CS_texture: return GL_TEXTURE;
  case TextureStage::CS_constant: return GL_CONSTANT;
  case TextureStage::CS_primary_color: return GL_PRIMARY_COLOR;
  case TextureStage::CS_previous: return GL_PREVIOUS;
  case TextureStage::CS_constant_color_scale: return GL_CONSTANT;
  }

  GLCAT.error()
    << "Invalid TextureStage::CombineSource value" << endl;
  return GL_TEXTURE;
}

////////////////////////////////////////////////////////////////////
//     Function: GLGraphicsStateGuardian::get_texture_operand_type
//       Access: Protected, Static
//  Description: Maps from the texture stage's CombineOperand types
//               to the corresponding OpenGL ids
////////////////////////////////////////////////////////////////////
GLint CLP(GraphicsStateGuardian)::
get_texture_operand_type(TextureStage::CombineOperand co) {
  switch (co) {
  case TextureStage::CO_undefined: // fall through
  case TextureStage::CO_src_alpha: return GL_SRC_ALPHA;
  case TextureStage::CO_one_minus_src_alpha: return GL_ONE_MINUS_SRC_ALPHA;
  case TextureStage::CO_src_color: return GL_SRC_COLOR;
  case TextureStage::CO_one_minus_src_color: return GL_ONE_MINUS_SRC_COLOR;
  }

  GLCAT.error()
    << "Invalid TextureStage::CombineOperand value" << endl;
  return GL_SRC_COLOR;
}

////////////////////////////////////////////////////////////////////
//     Function: GLGraphicsStateGuardian::get_fog_mode_type
//       Access: Protected, Static
//  Description: Maps from the fog types to gl version
////////////////////////////////////////////////////////////////////
GLenum CLP(GraphicsStateGuardian)::
get_fog_mode_type(Fog::Mode m) {
  switch(m) {
  case Fog::M_linear: return GL_LINEAR;
  case Fog::M_exponential: return GL_EXP;
  case Fog::M_exponential_squared: return GL_EXP2;
    /*
      case Fog::M_spline: return GL_FOG_FUNC_SGIS;
    */

  default:
    GLCAT.error() << "Invalid Fog::Mode value" << endl;
    return GL_EXP;
  }
}

////////////////////////////////////////////////////////////////////
//     Function: GLGraphicsStateGuardian::get_blend_equation_type
//       Access: Protected, Static
//  Description: Maps from ColorBlendAttrib::Mode to glBlendEquation
//               value.
////////////////////////////////////////////////////////////////////
GLenum CLP(GraphicsStateGuardian)::
get_blend_equation_type(ColorBlendAttrib::Mode mode) {
  switch (mode) {
  case ColorBlendAttrib::M_none:
  case ColorBlendAttrib::M_add:
    return GL_FUNC_ADD;
    
  case ColorBlendAttrib::M_subtract:
    return GL_FUNC_SUBTRACT;
    
  case ColorBlendAttrib::M_inv_subtract:
    return GL_FUNC_REVERSE_SUBTRACT;
    
  case ColorBlendAttrib::M_min:
    return GL_MIN;
    
  case ColorBlendAttrib::M_max:
    return GL_MAX;
  }    

  GLCAT.error()
    << "Unknown color blend mode " << (int)mode << endl;
  return GL_FUNC_ADD;
}

////////////////////////////////////////////////////////////////////
//     Function: GLGraphicsStateGuardian::get_blend_func
//       Access: Protected, Static
//  Description: Maps from ColorBlendAttrib::Operand to glBlendFunc
//               value.
////////////////////////////////////////////////////////////////////
GLenum CLP(GraphicsStateGuardian)::
get_blend_func(ColorBlendAttrib::Operand operand) {
  switch (operand) {
  case ColorBlendAttrib::O_zero:
    return GL_ZERO;

  case ColorBlendAttrib::O_one:
    return GL_ONE;

  case ColorBlendAttrib::O_incoming_color:
    return GL_SRC_COLOR;

  case ColorBlendAttrib::O_one_minus_incoming_color:
    return GL_ONE_MINUS_SRC_COLOR;

  case ColorBlendAttrib::O_fbuffer_color:
    return GL_DST_COLOR;

  case ColorBlendAttrib::O_one_minus_fbuffer_color:
    return GL_ONE_MINUS_DST_COLOR;

  case ColorBlendAttrib::O_incoming_alpha:
    return GL_SRC_ALPHA;

  case ColorBlendAttrib::O_one_minus_incoming_alpha:
    return GL_ONE_MINUS_SRC_ALPHA;

  case ColorBlendAttrib::O_fbuffer_alpha:
    return GL_DST_ALPHA;

  case ColorBlendAttrib::O_one_minus_fbuffer_alpha:
    return GL_ONE_MINUS_DST_ALPHA;

  case ColorBlendAttrib::O_constant_color:
  case ColorBlendAttrib::O_color_scale:
    return GL_CONSTANT_COLOR;

  case ColorBlendAttrib::O_one_minus_constant_color:
  case ColorBlendAttrib::O_one_minus_color_scale:
    return GL_ONE_MINUS_CONSTANT_COLOR;

  case ColorBlendAttrib::O_constant_alpha:
  case ColorBlendAttrib::O_alpha_scale:
    return GL_CONSTANT_ALPHA;

  case ColorBlendAttrib::O_one_minus_constant_alpha:
  case ColorBlendAttrib::O_one_minus_alpha_scale:
    return GL_ONE_MINUS_CONSTANT_ALPHA;

  case ColorBlendAttrib::O_incoming_color_saturate:
    return GL_SRC_ALPHA_SATURATE;
  }

  GLCAT.error()
    << "Unknown color blend operand " << (int)operand << endl;
  return GL_ZERO;
}

////////////////////////////////////////////////////////////////////
//     Function: GLGraphicsStateGuardian::get_usage
//       Access: Public, Static
//  Description: Maps from UsageHint to the GL symbol.
////////////////////////////////////////////////////////////////////
GLenum CLP(GraphicsStateGuardian)::
get_usage(qpGeom::UsageHint usage_hint) {
  switch (usage_hint) {
  case qpGeom::UH_stream:
    return GL_STREAM_DRAW;

  case qpGeom::UH_static:
  case qpGeom::UH_unspecified:
    return GL_STATIC_DRAW;

  case qpGeom::UH_dynamic:
    return GL_DYNAMIC_DRAW;

  case qpGeom::UH_client:
    break;
  }

  GLCAT.error()
    << "Unexpected usage_hint " << (int)usage_hint << endl;
  return GL_STATIC_DRAW;
}


////////////////////////////////////////////////////////////////////
//     Function: GLGraphicsStateGuardian::print_gfx_visual
//       Access: Public
//  Description: Prints a description of the current visual selected.
////////////////////////////////////////////////////////////////////
void CLP(GraphicsStateGuardian)::
print_gfx_visual() {
  GLint i;
  GLboolean j;
  cout << "Graphics Visual Info (# bits of each):" << endl;

  cout << "RGBA: ";
  GLP(GetIntegerv)( GL_RED_BITS, &i ); cout << i << " ";
  GLP(GetIntegerv)( GL_GREEN_BITS, &i ); cout << i << " ";
  GLP(GetIntegerv)( GL_BLUE_BITS, &i ); cout << i << " ";
  GLP(GetIntegerv)( GL_ALPHA_BITS, &i ); cout << i << endl;

  cout << "Accum RGBA: ";
  GLP(GetIntegerv)( GL_ACCUM_RED_BITS, &i ); cout << i << " ";
  GLP(GetIntegerv)( GL_ACCUM_GREEN_BITS, &i ); cout << i << " ";
  GLP(GetIntegerv)( GL_ACCUM_BLUE_BITS, &i ); cout << i << " ";
  GLP(GetIntegerv)( GL_ACCUM_ALPHA_BITS, &i ); cout << i << endl;

  GLP(GetIntegerv)( GL_INDEX_BITS, &i ); cout << "Color Index: " << i << endl;

  GLP(GetIntegerv)( GL_DEPTH_BITS, &i ); cout << "Depth: " << i << endl;
  GLP(GetIntegerv)( GL_ALPHA_BITS, &i ); cout << "Alpha: " << i << endl;
  GLP(GetIntegerv)( GL_STENCIL_BITS, &i ); cout << "Stencil: " << i << endl;

  GLP(GetBooleanv)( GL_DOUBLEBUFFER, &j ); cout << "DoubleBuffer? "
                                             << (int)j << endl;

  GLP(GetBooleanv)( GL_STEREO, &j ); cout << "Stereo? " << (int)j << endl;

  if (_supports_multisample) {
    GLP(GetBooleanv)( GL_MULTISAMPLE, &j ); cout << "Multisample? " << (int)j << endl;
    GLP(GetIntegerv)( GL_SAMPLES, &i ); cout << "Samples: " << i << endl;
  }

  GLP(GetBooleanv)( GL_BLEND, &j ); cout << "Blend? " << (int)j << endl;
  GLP(GetBooleanv)( GL_POINT_SMOOTH, &j ); cout << "Point Smooth? "
                                             << (int)j << endl;
  GLP(GetBooleanv)( GL_LINE_SMOOTH, &j ); cout << "Line Smooth? "
                                            << (int)j << endl;

  GLP(GetIntegerv)( GL_AUX_BUFFERS, &i ); cout << "Aux Buffers: " << i << endl;
}

////////////////////////////////////////////////////////////////////
//     Function: GLGraphicsStateGuardian::issue_scaled_color
//       Access: Public
//  Description: Transform the color by the current color matrix, and
//               calls the appropriate glColor function.
////////////////////////////////////////////////////////////////////
void CLP(GraphicsStateGuardian)::
issue_scaled_color(const Colorf &color) const {
  Colorf transformed
    (color[0] * _current_color_scale[0],
     color[1] * _current_color_scale[1],
     color[2] * _current_color_scale[2],
     color[3] * _current_color_scale[3]);

  GLP(Color4fv)(transformed.get_data());
}

////////////////////////////////////////////////////////////////////
//     Function: GLGraphicsStateGuardian::slot_new_light
//       Access: Protected, Virtual
//  Description: This will be called by the base class before a
//               particular light id will be used for the first time.
//               It is intended to allow the derived class to reserve
//               any additional resources, if required, for the new
//               light; and also to indicate whether the hardware
//               supports this many simultaneous lights.
//
//               The return value should be true if the additional
//               light is supported, or false if it is not.
////////////////////////////////////////////////////////////////////
bool CLP(GraphicsStateGuardian)::
slot_new_light(int light_id) {
  return (light_id < _max_lights);
}

////////////////////////////////////////////////////////////////////
//     Function: GLGraphicsStateGuardian::enable_lighting
//       Access: Protected, Virtual
//  Description: Intended to be overridden by a derived class to
//               enable or disable the use of lighting overall.  This
//               is called by issue_light() according to whether any
//               lights are in use or not.
////////////////////////////////////////////////////////////////////
void CLP(GraphicsStateGuardian)::
enable_lighting(bool enable) {
  if (enable) {
    GLP(Enable)(GL_LIGHTING);
  } else {
    GLP(Disable)(GL_LIGHTING);
  }
}

////////////////////////////////////////////////////////////////////
//     Function: GLGraphicsStateGuardian::set_ambient_light
//       Access: Protected, Virtual
//  Description: Intended to be overridden by a derived class to
//               indicate the color of the ambient light that should
//               be in effect.  This is called by issue_light() after
//               all other lights have been enabled or disabled.
////////////////////////////////////////////////////////////////////
void CLP(GraphicsStateGuardian)::
set_ambient_light(const Colorf &color) {
  GLP(LightModelfv)(GL_LIGHT_MODEL_AMBIENT, color.get_data());
}

////////////////////////////////////////////////////////////////////
//     Function: GLGraphicsStateGuardian::enable_light
//       Access: Protected, Virtual
//  Description: Intended to be overridden by a derived class to
//               enable the indicated light id.  A specific Light will
//               already have been bound to this id via bind_light().
////////////////////////////////////////////////////////////////////
void CLP(GraphicsStateGuardian)::
enable_light(int light_id, bool enable) {
  if (enable) {
    GLP(Enable)(get_light_id(light_id));
  } else {
    GLP(Disable)(get_light_id(light_id));
  }
}

////////////////////////////////////////////////////////////////////
//     Function: GLGraphicsStateGuardian::begin_bind_lights
//       Access: Protected, Virtual
//  Description: Called immediately before bind_light() is called,
//               this is intended to provide the derived class a hook
//               in which to set up some state (like transform) that
//               might apply to several lights.
//
//               The sequence is: begin_bind_lights() will be called,
//               then one or more bind_light() calls, then
//               end_bind_lights().
////////////////////////////////////////////////////////////////////
void CLP(GraphicsStateGuardian)::
begin_bind_lights() {
  // We need to temporarily load a new matrix so we can define the
  // light in a known coordinate system.  We pick the transform of the
  // root.  (Alternatively, we could leave the current transform where
  // it is and compute the light position relative to that transform
  // instead of relative to the root, by composing with the matrix
  // computed by _transform->invert_compose(render_transform).  But I
  // think loading a completely new matrix is simpler.)
  GLP(MatrixMode)(GL_MODELVIEW);
  GLP(PushMatrix)();
  GLP(LoadMatrixf)(_scene_setup->get_render_transform()->get_mat().get_data());
}

////////////////////////////////////////////////////////////////////
//     Function: GLGraphicsStateGuardian::end_bind_lights
//       Access: Protected, Virtual
//  Description: Called after before bind_light() has been called one
//               or more times (but before any geometry is issued or
//               additional state is changed), this is intended to
//               clean up any temporary changes to the state that may
//               have been made by begin_bind_lights().
////////////////////////////////////////////////////////////////////
void CLP(GraphicsStateGuardian)::
end_bind_lights() {
  GLP(MatrixMode)(GL_MODELVIEW);
  GLP(PopMatrix)();
}

////////////////////////////////////////////////////////////////////
//     Function: GLGraphicsStateGuardian::slot_new_clip_plane
//       Access: Protected, Virtual
//  Description: This will be called by the base class before a
//               particular clip plane id will be used for the first
//               time.  It is intended to allow the derived class to
//               reserve any additional resources, if required, for
//               the new clip plane; and also to indicate whether the
//               hardware supports this many simultaneous clipping
//               planes.
//
//               The return value should be true if the additional
//               plane is supported, or false if it is not.
////////////////////////////////////////////////////////////////////
bool CLP(GraphicsStateGuardian)::
slot_new_clip_plane(int plane_id) {
  return (plane_id < _max_clip_planes);
}

////////////////////////////////////////////////////////////////////
//     Function: GLGraphicsStateGuardian::enable_clip_plane
//       Access: Protected, Virtual
//  Description: Intended to be overridden by a derived class to
//               enable the indicated clip_plane id.  A specific
//               PlaneNode will already have been bound to this id via
//               bind_clip_plane().
////////////////////////////////////////////////////////////////////
void CLP(GraphicsStateGuardian)::
enable_clip_plane(int plane_id, bool enable) {
  if (enable) {
    GLP(Enable)(get_clip_plane_id(plane_id));
  } else {
    GLP(Disable)(get_clip_plane_id(plane_id));
  }
}

////////////////////////////////////////////////////////////////////
//     Function: GLGraphicsStateGuardian::begin_bind_clip_planes
//       Access: Protected, Virtual
//  Description: Called immediately before bind_clip_plane() is called,
//               this is intended to provide the derived class a hook
//               in which to set up some state (like transform) that
//               might apply to several clip_planes.
//
//               The sequence is: begin_bind_clip_planes() will be called,
//               then one or more bind_clip_plane() calls, then
//               end_bind_clip_planes().
////////////////////////////////////////////////////////////////////
void CLP(GraphicsStateGuardian)::
begin_bind_clip_planes() {
  // We need to temporarily load a new matrix so we can define the
  // clip_plane in a known coordinate system.  We pick the transform of the
  // root.  (Alternatively, we could leave the current transform where
  // it is and compute the clip_plane position relative to that transform
  // instead of relative to the root, by composing with the matrix
  // computed by _transform->invert_compose(render_transform).  But I
  // think loading a completely new matrix is simpler.)
  GLP(MatrixMode)(GL_MODELVIEW);
  GLP(PushMatrix)();
  GLP(LoadMatrixf)(_scene_setup->get_render_transform()->get_mat().get_data());
}

////////////////////////////////////////////////////////////////////
//     Function: GLGraphicsStateGuardian::bind_clip_plane
//       Access: Protected, Virtual
//  Description: Called the first time a particular clip_plane has been
//               bound to a given id within a frame, this should set
//               up the associated hardware clip_plane with the clip_plane's
//               properties.
////////////////////////////////////////////////////////////////////
void CLP(GraphicsStateGuardian)::
bind_clip_plane(const NodePath &plane, int plane_id) {
  GLenum id = get_clip_plane_id(plane_id);

  const LMatrix4f &plane_mat = plane.get_mat(_scene_setup->get_scene_root());
  const PlaneNode *plane_node;
  DCAST_INTO_V(plane_node, plane.node());
  Planef xformed_plane = plane_node->get_plane() * plane_mat;

  Planed double_plane(LCAST(double, xformed_plane));
  GLP(ClipPlane)(id, double_plane.get_data());

  report_my_gl_errors();
}

////////////////////////////////////////////////////////////////////
//     Function: GLGraphicsStateGuardian::end_bind_clip_planes
//       Access: Protected, Virtual
//  Description: Called after before bind_clip_plane() has been called one
//               or more times (but before any geometry is issued or
//               additional state is changed), this is intended to
//               clean up any temporary changes to the state that may
//               have been made by begin_bind_clip_planes().
////////////////////////////////////////////////////////////////////
void CLP(GraphicsStateGuardian)::
end_bind_clip_planes() {
  GLP(MatrixMode)(GL_MODELVIEW);
  GLP(PopMatrix)();
}

////////////////////////////////////////////////////////////////////
//     Function: GLGraphicsStateGuardian::set_blend_mode
//       Access: Protected, Virtual
//  Description: Called after any of the things that might change
//               blending state have changed, this function is
//               responsible for setting the appropriate color
//               blending mode based on the current properties.
////////////////////////////////////////////////////////////////////
void CLP(GraphicsStateGuardian)::
set_blend_mode() {
  // If color_write_mode is off, we disable writing to the color using
  // blending.  This case is only used if we can't use GLP(ColorMask) to
  // disable the color writing for some reason (usually a driver
  // problem).
  if (_color_write_mode == ColorWriteAttrib::M_off) {
    enable_multisample_alpha_one(false);
    enable_multisample_alpha_mask(false);
    enable_blend(true);
    _glBlendEquation(GL_FUNC_ADD);
    GLP(BlendFunc)(GL_ZERO, GL_ONE);
   return;
  }

  // Is there a color blend set?
  if (_color_blend_mode != ColorBlendAttrib::M_none) {
    enable_multisample_alpha_one(false);
    enable_multisample_alpha_mask(false);
    enable_blend(true);
    _glBlendEquation(get_blend_equation_type(_color_blend_mode));
    GLP(BlendFunc)(get_blend_func(_color_blend->get_operand_a()),
                   get_blend_func(_color_blend->get_operand_b()));

    if (_color_blend_involves_color_scale) {
      // Apply the current color scale to the blend mode.
      _glBlendColor(_current_color_scale[0], _current_color_scale[1], 
                    _current_color_scale[2], _current_color_scale[3]);
      
    } else {
      Colorf c = _color_blend->get_color();
      _glBlendColor(c[0], c[1], c[2], c[3]);
    }
    return;
  }

  // No color blend; is there a transparency set?
  switch (_transparency_mode) {
  case TransparencyAttrib::M_none:
  case TransparencyAttrib::M_binary:
    break;
    
  case TransparencyAttrib::M_alpha:
  case TransparencyAttrib::M_dual:
    enable_multisample_alpha_one(false);
    enable_multisample_alpha_mask(false);
    enable_blend(true);
    _glBlendEquation(GL_FUNC_ADD);
    GLP(BlendFunc)(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    return;
    
  case TransparencyAttrib::M_multisample:
    // We need to enable *both* of these in M_multisample case.
    enable_multisample_alpha_one(true);
    enable_multisample_alpha_mask(true);
    enable_blend(false);
    return;
    
  case TransparencyAttrib::M_multisample_mask:
    enable_multisample_alpha_one(false);
    enable_multisample_alpha_mask(true);
    enable_blend(false);
    return;
    
  default:
    GLCAT.error()
      << "invalid transparency mode " << (int)_transparency_mode << endl;
    break;
  }

  if (_line_smooth_enabled || _point_smooth_enabled) {
    // If we have either of these turned on, we also need to have
    // blend mode enabled in order to see it.
    enable_multisample_alpha_one(false);
    enable_multisample_alpha_mask(false);
    enable_blend(true);
    _glBlendEquation(GL_FUNC_ADD);
    GLP(BlendFunc)(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    return;
  }

  // For best polygon smoothing, we need:
  // (1) a frame buffer that supports alpha
  // (2) sort polygons front-to-back
  // (3) glBlendFunc(GL_SRC_ALPHA_SATURATE, GL_ONE);
  //
  // Since these modes have other implications for the application, we
  // don't attempt to do this by default.  If you really want good
  // polygon smoothing (and you don't have multisample support), do
  // all this yourself.

  // Nothing's set, so disable blending.
  enable_multisample_alpha_one(false);
  enable_multisample_alpha_mask(false);
  enable_blend(false);
}

////////////////////////////////////////////////////////////////////
//     Function: GLGraphicsStateGuardian::finish_modify_state
//       Access: Protected, Virtual
//  Description: Called after the GSG state has been modified via
//               modify_state() or set_state(), this hook is provided
//               for the derived class to do any further state setup
//               work.
////////////////////////////////////////////////////////////////////
void CLP(GraphicsStateGuardian)::
finish_modify_state() {
  GraphicsStateGuardian::finish_modify_state();

  // Apply the texture, if it needs to be reapplied.
  if (_texture_stale) {
    _texture_stale = false;
    do_issue_texture();
  }

  // If one of the previously-loaded TexGen modes modified the texture
  // matrix, then if either state changed, we have to change both of
  // them now.
  if (_tex_gen_modifies_mat &&
      (_needs_tex_mat || _needs_tex_gen)) {
    _needs_tex_mat = true;
    _needs_tex_gen = true;
  }

  // Apply the texture matrix, if needed.
  if (_needs_tex_mat) {
    _needs_tex_mat = false;

    int num_stages = _current_texture->get_num_on_stages();
    nassertv(num_stages <= _max_texture_stages);
    
    for (int i = 0; i < num_stages; i++) {
      TextureStage *stage = _current_texture->get_on_stage(i);
      _glActiveTexture(GL_TEXTURE0 + i);
      
      GLP(MatrixMode)(GL_TEXTURE);
      if (_current_tex_mat->has_stage(stage)) {
        GLP(LoadMatrixf)(_current_tex_mat->get_mat(stage).get_data());
      } else {
        GLP(LoadIdentity)();

        // For some reason, the glLoadIdentity() call doesn't work on
        // my Dell laptop's IBM OpenGL driver, when used in
        // conjunction with glTexGen(), below.  But explicitly loading
        // an identity matrix does work.  But this buggy-driver
        // workaround might have other performance implications, so I
        // leave it out.
        //GLP(LoadMatrixf)(LMatrix4f::ident_mat().get_data());
      }
    }
    report_my_gl_errors();
  }

  if (_needs_tex_gen) {
    _needs_tex_gen = false;
    bool force_normal = false;

    int num_stages = _current_texture->get_num_on_stages();
    nassertv(num_stages <= _max_texture_stages);
    
    // These are passed in for the four OBJECT_PLANE or EYE_PLANE
    // values; they effectively define an identity matrix that maps
    // the spatial coordinates one-for-one to UV's.  If you want a
    // mapping other than identity, use a TexMatrixAttrib (or a
    // TexProjectorEffect).
    static const float s_data[4] = { 1, 0, 0, 0 };
    static const float t_data[4] = { 0, 1, 0, 0 };
    static const float r_data[4] = { 0, 0, 1, 0 };
    static const float q_data[4] = { 0, 0, 0, 1 };

    _tex_gen_modifies_mat = false;

    bool got_point_sprites = false;
    
    for (int i = 0; i < num_stages; i++) {
      TextureStage *stage = _current_texture->get_on_stage(i);
      _glActiveTexture(GL_TEXTURE0 + i);
      GLP(Disable)(GL_TEXTURE_GEN_S);
      GLP(Disable)(GL_TEXTURE_GEN_T);
      GLP(Disable)(GL_TEXTURE_GEN_R);
      GLP(Disable)(GL_TEXTURE_GEN_Q);
      if (_supports_point_sprite) {
        GLP(TexEnvi)(GL_POINT_SPRITE_ARB, GL_COORD_REPLACE_ARB, GL_FALSE);
      }

      TexGenAttrib::Mode mode = _current_tex_gen->get_mode(stage);
      switch (mode) {
      case TexGenAttrib::M_off:
        break;
        
      case TexGenAttrib::M_eye_sphere_map:
        GLP(TexGeni)(GL_S, GL_TEXTURE_GEN_MODE, GL_SPHERE_MAP);
        GLP(TexGeni)(GL_T, GL_TEXTURE_GEN_MODE, GL_SPHERE_MAP);
        GLP(Enable)(GL_TEXTURE_GEN_S);
        GLP(Enable)(GL_TEXTURE_GEN_T);
        force_normal = true;
        break;
        
      case TexGenAttrib::M_eye_cube_map:
      case TexGenAttrib::M_world_cube_map:
        if (_supports_cube_map) {
          if (mode != TexGenAttrib::M_eye_cube_map) {
            // We dynamically transform normals from eye space to
            // world space by applying the appropriate rotation
            // transform to the current texture matrix.  Although it's
            // tempting to try, we can't safely convert to object
            // space, since this method doesn't get called with each
            // different object.
            CPT(TransformState) transform = _scene_setup->get_render_transform();
            transform = transform->invert_compose(TransformState::make_identity());
            LMatrix4f mat = transform->get_mat();
            mat.set_row(3, LVecBase3f(0.0f, 0.0f, 0.0f));
            GLP(MatrixMode)(GL_TEXTURE);
            GLP(MultMatrixf)(mat.get_data());

            // Now we need to reset the texture matrix next time
            // around to undo this.
            _tex_gen_modifies_mat = true;
          }

          GLP(TexGeni)(GL_S, GL_TEXTURE_GEN_MODE, GL_REFLECTION_MAP);
          GLP(TexGeni)(GL_T, GL_TEXTURE_GEN_MODE, GL_REFLECTION_MAP);
          GLP(TexGeni)(GL_R, GL_TEXTURE_GEN_MODE, GL_REFLECTION_MAP);
          GLP(Enable)(GL_TEXTURE_GEN_S);
          GLP(Enable)(GL_TEXTURE_GEN_T);
          GLP(Enable)(GL_TEXTURE_GEN_R);
          force_normal = true;
        }
        break;
        
      case TexGenAttrib::M_eye_normal:
      case TexGenAttrib::M_world_normal:
        if (_supports_cube_map) {
          if (mode != TexGenAttrib::M_eye_normal) {
            // We dynamically transform normals from eye space to
            // world space by applying the appropriate rotation
            // transform to the current texture matrix.  Although it's
            // tempting to try, we can't safely convert to object
            // space, since this method doesn't get called with each
            // different object.
            CPT(TransformState) transform = _scene_setup->get_render_transform();
            transform = transform->invert_compose(TransformState::make_identity());
            LMatrix4f mat = transform->get_mat();
            mat.set_row(3, LVecBase3f(0.0f, 0.0f, 0.0f));
            GLP(MatrixMode)(GL_TEXTURE);
            GLP(MultMatrixf)(mat.get_data());

            // Now we need to reset the texture matrix next time
            // around to undo this.
            _tex_gen_modifies_mat = true;
          }

          GLP(TexGeni)(GL_S, GL_TEXTURE_GEN_MODE, GL_NORMAL_MAP);
          GLP(TexGeni)(GL_T, GL_TEXTURE_GEN_MODE, GL_NORMAL_MAP);
          GLP(TexGeni)(GL_R, GL_TEXTURE_GEN_MODE, GL_NORMAL_MAP);
          GLP(Enable)(GL_TEXTURE_GEN_S);
          GLP(Enable)(GL_TEXTURE_GEN_T);
          GLP(Enable)(GL_TEXTURE_GEN_R);
          force_normal = true;
        }
        break;

      case TexGenAttrib::M_object_position:
        GLP(TexGeni)(GL_S, GL_TEXTURE_GEN_MODE, GL_OBJECT_LINEAR);
        GLP(TexGeni)(GL_T, GL_TEXTURE_GEN_MODE, GL_OBJECT_LINEAR);
        GLP(TexGeni)(GL_R, GL_TEXTURE_GEN_MODE, GL_OBJECT_LINEAR);
        GLP(TexGeni)(GL_Q, GL_TEXTURE_GEN_MODE, GL_OBJECT_LINEAR);
        
        GLP(TexGenfv)(GL_S, GL_OBJECT_PLANE, s_data);
        GLP(TexGenfv)(GL_T, GL_OBJECT_PLANE, t_data);
        GLP(TexGenfv)(GL_R, GL_OBJECT_PLANE, r_data);
        GLP(TexGenfv)(GL_Q, GL_OBJECT_PLANE, q_data);
        
        GLP(Enable)(GL_TEXTURE_GEN_S);
        GLP(Enable)(GL_TEXTURE_GEN_T);
        GLP(Enable)(GL_TEXTURE_GEN_R);
        GLP(Enable)(GL_TEXTURE_GEN_Q);
        break;

      case TexGenAttrib::M_eye_position:
        // To represent eye position correctly, we need to temporarily
        // load the coordinate-system transform.
        GLP(MatrixMode)(GL_MODELVIEW);
        GLP(PushMatrix)();
        GLP(LoadMatrixf)(_scene_setup->get_cs_transform()->get_mat().get_data());

        GLP(TexGeni)(GL_S, GL_TEXTURE_GEN_MODE, GL_EYE_LINEAR);
        GLP(TexGeni)(GL_T, GL_TEXTURE_GEN_MODE, GL_EYE_LINEAR);
        GLP(TexGeni)(GL_R, GL_TEXTURE_GEN_MODE, GL_EYE_LINEAR);
        GLP(TexGeni)(GL_Q, GL_TEXTURE_GEN_MODE, GL_EYE_LINEAR);
        
        GLP(TexGenfv)(GL_S, GL_EYE_PLANE, s_data);
        GLP(TexGenfv)(GL_T, GL_EYE_PLANE, t_data);
        GLP(TexGenfv)(GL_R, GL_EYE_PLANE, r_data);
        GLP(TexGenfv)(GL_Q, GL_EYE_PLANE, q_data);
        
        GLP(Enable)(GL_TEXTURE_GEN_S);
        GLP(Enable)(GL_TEXTURE_GEN_T);
        GLP(Enable)(GL_TEXTURE_GEN_R);
        GLP(Enable)(GL_TEXTURE_GEN_Q);

        GLP(MatrixMode)(GL_MODELVIEW);
        GLP(PopMatrix)();
        break;

      case TexGenAttrib::M_world_position:
        // We achieve world position coordinates by using the eye
        // position mode, and loading the transform of the root
        // node--thus putting the "eye" at the root.
        {
          GLP(MatrixMode)(GL_MODELVIEW);
          GLP(PushMatrix)();
          CPT(TransformState) root_transform = _scene_setup->get_render_transform();
          GLP(LoadMatrixf)(root_transform->get_mat().get_data());
          GLP(TexGeni)(GL_S, GL_TEXTURE_GEN_MODE, GL_EYE_LINEAR);
          GLP(TexGeni)(GL_T, GL_TEXTURE_GEN_MODE, GL_EYE_LINEAR);
          GLP(TexGeni)(GL_R, GL_TEXTURE_GEN_MODE, GL_EYE_LINEAR);
          GLP(TexGeni)(GL_Q, GL_TEXTURE_GEN_MODE, GL_EYE_LINEAR);
        
          GLP(TexGenfv)(GL_S, GL_EYE_PLANE, s_data);
          GLP(TexGenfv)(GL_T, GL_EYE_PLANE, t_data);
          GLP(TexGenfv)(GL_R, GL_EYE_PLANE, r_data);
          GLP(TexGenfv)(GL_Q, GL_EYE_PLANE, q_data);
          
          GLP(Enable)(GL_TEXTURE_GEN_S);
          GLP(Enable)(GL_TEXTURE_GEN_T);
          GLP(Enable)(GL_TEXTURE_GEN_R);
          GLP(Enable)(GL_TEXTURE_GEN_Q);
          
          GLP(MatrixMode)(GL_MODELVIEW);
          GLP(PopMatrix)();
        }
        break;

      case TexGenAttrib::M_point_sprite:
        nassertv(_supports_point_sprite);
        GLP(TexEnvi)(GL_POINT_SPRITE_ARB, GL_COORD_REPLACE_ARB, GL_TRUE);
        got_point_sprites = true;
        break;
      }
    }

    if (got_point_sprites != _tex_gen_point_sprite) {
      _tex_gen_point_sprite = got_point_sprites;
      if (_tex_gen_point_sprite) {
        GLP(Enable)(GL_POINT_SPRITE_ARB);
      } else {
        GLP(Disable)(GL_POINT_SPRITE_ARB);
      }
    }

    // Certain texgen modes (sphere_map, cube_map) require forcing the
    // normal to be sent to the GL while the texgen mode is in effect.
    if (force_normal != _texgen_forced_normal) {
      if (force_normal) {
        force_normals();
      } else  {
        undo_force_normals();
      }
      _texgen_forced_normal = force_normal;
    }

    report_my_gl_errors();
  }
}

////////////////////////////////////////////////////////////////////
//     Function: GLGraphicsStateGuardian::free_pointers
//       Access: Protected, Virtual
//  Description: Frees some memory that was explicitly allocated
//               within the glgsg.
////////////////////////////////////////////////////////////////////
void CLP(GraphicsStateGuardian)::
free_pointers() {
}

////////////////////////////////////////////////////////////////////
//     Function: GLGraphicsStateGuardian::get_untextured_state
//       Access: Protected, Static
//  Description: Returns a RenderState object that represents
//               texturing off.
////////////////////////////////////////////////////////////////////
CPT(RenderState) CLP(GraphicsStateGuardian)::
get_untextured_state() {
  static CPT(RenderState) state;
  if (state == (RenderState *)NULL) {
    state = RenderState::make(TextureAttrib::make_off());
  }
  return state;
}

////////////////////////////////////////////////////////////////////
//     Function: GLGraphicsStateGuardian::get_smooth_state
//       Access: Protected, Static
//  Description: Returns a RenderState object that represents
//               smooth, per-vertex shading.
////////////////////////////////////////////////////////////////////
CPT(RenderState) CLP(GraphicsStateGuardian)::
get_smooth_state() {
  static CPT(RenderState) state;
  if (state == (RenderState *)NULL) {
    state = RenderState::make(ShadeModelAttrib::make(ShadeModelAttrib::M_smooth));
  }
  return state;
}

////////////////////////////////////////////////////////////////////
//     Function: GLGraphicsStateGuardian::get_flat_state
//       Access: Protected, Static
//  Description: Returns a RenderState object that represents
//               flat, per-primitive shading.
////////////////////////////////////////////////////////////////////
CPT(RenderState) CLP(GraphicsStateGuardian)::
get_flat_state() {
  static CPT(RenderState) state;
  if (state == (RenderState *)NULL) {
    state = RenderState::make(ShadeModelAttrib::make(ShadeModelAttrib::M_flat));
  }
  return state;
}

////////////////////////////////////////////////////////////////////
//     Function: GLGraphicsStateGuardian::do_auto_rescale_normal
//       Access: Protected
//  Description: Issues the appropriate GL commands to either rescale
//               or normalize the normals according to the current
//               transform.
////////////////////////////////////////////////////////////////////
void CLP(GraphicsStateGuardian)::
do_auto_rescale_normal() {
  if (_transform->has_uniform_scale()) {
    if (IS_NEARLY_EQUAL(_transform->get_uniform_scale(), 1.0f)) {
      // If there's no scale at all, don't do anything.
      GLP(Disable)(GL_NORMALIZE);
      if (_supports_rescale_normal) {
        GLP(Disable)(GL_RESCALE_NORMAL);
      }
      
    } else {
      // There's a uniform scale; use the rescale feature if available.
      if (_supports_rescale_normal) {
        GLP(Enable)(GL_RESCALE_NORMAL);
        GLP(Disable)(GL_NORMALIZE);
      } else {
        GLP(Enable)(GL_NORMALIZE);
      }
    }

  } else {
    // If there's a non-uniform scale, normalize everything.
    GLP(Enable)(GL_NORMALIZE);
    if (_supports_rescale_normal) {
      GLP(Disable)(GL_RESCALE_NORMAL);
    }
  }
}

////////////////////////////////////////////////////////////////////
//     Function: GLGraphicsStateGuardian::do_issue_texture
//       Access: Protected
//  Description: This is called by finish_modify_state() when the
//               texture state has changed.
////////////////////////////////////////////////////////////////////
void CLP(GraphicsStateGuardian)::
do_issue_texture() {
  DO_PSTATS_STUFF(_texture_state_pcollector.add_level(1));

  CPT(TextureAttrib) new_texture = _pending_texture->filter_to_max(_max_texture_stages);
  
  int num_stages = new_texture->get_num_on_stages();
  int num_old_stages = _current_texture->get_num_on_stages();

  nassertv(num_stages <= _max_texture_stages && 
           num_old_stages <= _max_texture_stages);

  _texture_involves_color_scale = false;

  int i;
  for (i = 0; i < num_stages; i++) {
    TextureStage *stage = new_texture->get_on_stage(i);
    Texture *texture = new_texture->get_on_texture(stage);
    nassertv(texture != (Texture *)NULL);
    
    if (i >= num_old_stages ||
        stage != _current_texture->get_on_stage(i) ||
        texture != _current_texture->get_on_texture(stage) ||
        stage->involves_color_scale()) {
      // Stage i has changed.  Issue the texture on this stage.
      _glActiveTexture(GL_TEXTURE0 + i);

      GLenum target = get_texture_target(texture->get_texture_type());

      // First, turn off the previous texture mode.
      GLP(Disable)(GL_TEXTURE_1D);
      GLP(Disable)(GL_TEXTURE_2D);
      if (_supports_3d_texture) {
        GLP(Disable)(GL_TEXTURE_3D);
      }
      if (_supports_cube_map) {
        GLP(Disable)(GL_TEXTURE_CUBE_MAP);
      }

      // Then, turn on the current texture mode.
      if (target == GL_NONE) {
        // Unsupported texture mode.
        break;
      }
      GLP(Enable)(target);
      
      TextureContext *tc = texture->prepare_now(_prepared_objects, this);
      apply_texture(tc);

      if (stage->involves_color_scale() && _color_scale_enabled) {
        Colorf color = stage->get_color();
        color.set(color[0] * _current_color_scale[0],
                  color[1] * _current_color_scale[1],
                  color[2] * _current_color_scale[2],
                  color[3] * _current_color_scale[3]);
        _texture_involves_color_scale = true;
        GLP(TexEnvfv)(GL_TEXTURE_ENV, GL_TEXTURE_ENV_COLOR, color.get_data());
      } else {
        GLP(TexEnvfv)(GL_TEXTURE_ENV, GL_TEXTURE_ENV_COLOR, stage->get_color().get_data());
      }

      if (stage->get_mode() == TextureStage::M_decal) {
        if (texture->get_num_components() < 3) {
          // Make a special case for 1- and 2-channel decal textures.
          // OpenGL does not define their use with GL_DECAL for some
          // reason, so implement them using the combiner instead.
          GLP(TexEnvi)(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_COMBINE);
          GLP(TexEnvi)(GL_TEXTURE_ENV, GL_COMBINE_RGB, GL_INTERPOLATE);
          GLP(TexEnvi)(GL_TEXTURE_ENV, GL_RGB_SCALE, 1);
          GLP(TexEnvi)(GL_TEXTURE_ENV, GL_ALPHA_SCALE, 1);
          GLP(TexEnvi)(GL_TEXTURE_ENV, GL_SRC0_RGB, GL_TEXTURE);
          GLP(TexEnvi)(GL_TEXTURE_ENV, GL_OPERAND1_RGB, GL_SRC_COLOR);
          GLP(TexEnvi)(GL_TEXTURE_ENV, GL_SRC1_RGB, GL_PREVIOUS);
          GLP(TexEnvi)(GL_TEXTURE_ENV, GL_OPERAND1_RGB, GL_SRC_COLOR);
          GLP(TexEnvi)(GL_TEXTURE_ENV, GL_SRC2_RGB, GL_TEXTURE);
          GLP(TexEnvi)(GL_TEXTURE_ENV, GL_OPERAND2_RGB, GL_SRC_ALPHA);
          
        } else {
          // Normal 3- and 4-channel decal textures.
          GLP(TexEnvi)(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_DECAL);
        }

      } else if (stage->get_mode() == TextureStage::M_combine) {
        GLP(TexEnvi)(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_COMBINE);
        GLP(TexEnvi)(GL_TEXTURE_ENV, GL_COMBINE_RGB, GL_INTERPOLATE);
        GLP(TexEnvi)(GL_TEXTURE_ENV, GL_RGB_SCALE, stage->get_rgb_scale());
        GLP(TexEnvi)(GL_TEXTURE_ENV, GL_ALPHA_SCALE, stage->get_alpha_scale());
        GLP(TexEnvi)(GL_TEXTURE_ENV, GL_COMBINE_RGB, 
                     get_texture_combine_type(stage->get_combine_rgb_mode()));

        switch (stage->get_num_combine_rgb_operands()) {
        case 3:
          GLP(TexEnvi)(GL_TEXTURE_ENV, GL_SRC2_RGB, 
                       get_texture_src_type(stage->get_combine_rgb_source2()));
          GLP(TexEnvi)(GL_TEXTURE_ENV, GL_OPERAND2_RGB, 
                       get_texture_operand_type(stage->get_combine_rgb_operand2()));
          // fall through

        case 2:
          GLP(TexEnvi)(GL_TEXTURE_ENV, GL_SRC1_RGB, 
                       get_texture_src_type(stage->get_combine_rgb_source1()));
          GLP(TexEnvi)(GL_TEXTURE_ENV, GL_OPERAND1_RGB, 
                       get_texture_operand_type(stage->get_combine_rgb_operand1()));
          // fall through

        case 1:
          GLP(TexEnvi)(GL_TEXTURE_ENV, GL_SRC0_RGB, 
                       get_texture_src_type(stage->get_combine_rgb_source0()));
          GLP(TexEnvi)(GL_TEXTURE_ENV, GL_OPERAND0_RGB, 
                       get_texture_operand_type(stage->get_combine_rgb_operand0()));
          // fall through

        default:
          break;
        }
        GLP(TexEnvi)(GL_TEXTURE_ENV, GL_COMBINE_ALPHA, 
                     get_texture_combine_type(stage->get_combine_alpha_mode()));

        switch (stage->get_num_combine_alpha_operands()) {
        case 3:
          GLP(TexEnvi)(GL_TEXTURE_ENV, GL_SRC2_ALPHA, 
                       get_texture_src_type(stage->get_combine_alpha_source2()));
          GLP(TexEnvi)(GL_TEXTURE_ENV, GL_OPERAND2_ALPHA, 
                       get_texture_operand_type(stage->get_combine_alpha_operand2()));
          // fall through

        case 2:
          GLP(TexEnvi)(GL_TEXTURE_ENV, GL_SRC1_ALPHA, 
                       get_texture_src_type(stage->get_combine_alpha_source1()));
          GLP(TexEnvi)(GL_TEXTURE_ENV, GL_OPERAND1_ALPHA, 
                       get_texture_operand_type(stage->get_combine_alpha_operand1()));
          // fall through

        case 1:
          GLP(TexEnvi)(GL_TEXTURE_ENV, GL_SRC0_ALPHA, 
                       get_texture_src_type(stage->get_combine_alpha_source0()));
          GLP(TexEnvi)(GL_TEXTURE_ENV, GL_OPERAND0_ALPHA, 
                       get_texture_operand_type(stage->get_combine_alpha_operand0()));
          // fall through

        default:
          break;
        }

      } else {
        GLint glmode = get_texture_apply_mode_type(stage->get_mode());
        GLP(TexEnvi)(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, glmode);
      }

      GLP(MatrixMode)(GL_TEXTURE);
      if (_current_tex_mat->has_stage(stage)) {
        GLP(LoadMatrixf)(_current_tex_mat->get_mat(stage).get_data());
      } else {
        GLP(LoadIdentity)();
      }
    }
  }
    
  // Disable the texture stages that are no longer used.
  for (i = num_stages; i < num_old_stages; i++) {
    _glActiveTexture(GL_TEXTURE0 + i);
    GLP(Disable)(GL_TEXTURE_1D);
    GLP(Disable)(GL_TEXTURE_2D);
    if (_supports_3d_texture) {
      GLP(Disable)(GL_TEXTURE_3D);
    }
    if (_supports_cube_map) {
      GLP(Disable)(GL_TEXTURE_CUBE_MAP);
    }
  }

  _current_texture = new_texture;

  // Changing the set of texture stages will require us to reissue the
  // texgen and texmat attribs.
  _needs_tex_gen = true;
  _needs_tex_mat = true;

  report_my_gl_errors();
}

////////////////////////////////////////////////////////////////////
//     Function: GLGraphicsStateGuardian::do_point_size
//       Access: Protected
//  Description: Internally sets the point size parameters after any
//               of the properties have changed that might affect
//               this.
////////////////////////////////////////////////////////////////////
void CLP(GraphicsStateGuardian)::
do_point_size() {
  if (!_point_perspective) {
    // Normal, constant-sized points.  Here _point_size is a width in
    // pixels.
    static LVecBase3f constant(1.0f, 0.0f, 0.0f);
    _glPointParameterfv(GL_POINT_DISTANCE_ATTENUATION, constant.get_data());

  } else {
    // Perspective-sized points.  Here _point_size is a width in 3-d
    // units.  To arrange that, we need to figure out the appropriate
    // scaling factor based on the current viewport and projection
    // matrix.
    LVector3f height(0.0f, _point_size, 1.0f);
    height = height * _projection_mat;
    float s = height[1] * _viewport_height / _point_size;
    LVecBase3f square(0.0f, 0.0f, 1.0f / (s * s));
    _glPointParameterfv(GL_POINT_DISTANCE_ATTENUATION, square.get_data());
  }

  report_my_gl_errors();
}

#ifndef NDEBUG
////////////////////////////////////////////////////////////////////
//     Function: GLGraphicsStateGuardian::build_phony_mipmaps
//       Access: Protected
//  Description: Generates a series of colored mipmap levels to aid in
//               visualizing the mipmap levels as the hardware applies
//               them.
////////////////////////////////////////////////////////////////////
void CLP(GraphicsStateGuardian)::
build_phony_mipmaps(Texture *tex) {
  int x_size = tex->get_x_size();
  int y_size = tex->get_y_size();

  GLCAT.info()
    << "Building phony mipmap levels for " << tex->get_name() << "\n";
  int level = 0;
  while (x_size > 0 && y_size > 0) {
    GLCAT.info(false)
      << "  level " << level << " is " << x_size << " by " << y_size << "\n";
    build_phony_mipmap_level(level, x_size, y_size);

    x_size >>= 1;
    y_size >>= 1;
    level++;
  }

  while (x_size > 0) {
    GLCAT.info(false)
      << "  level " << level << " is " << x_size << " by 1\n";
    build_phony_mipmap_level(level, x_size, 1);

    x_size >>= 1;
    level++;
  }

  while (y_size > 0) {
    GLCAT.info(false)
      << "  level " << level << " is 1 by " << y_size << "\n";
    build_phony_mipmap_level(level, 1, y_size);

    y_size >>= 1;
    level++;
  }
}

////////////////////////////////////////////////////////////////////
//     Function: GLGraphicsStateGuardian::build_phony_mipmap_level
//       Access: Protected
//  Description: Generates a single colored mipmap level.
////////////////////////////////////////////////////////////////////
void CLP(GraphicsStateGuardian)::
build_phony_mipmap_level(int level, int x_size, int y_size) {
  static const int num_levels = 10;
  static const char *level_filenames[num_levels] = {
    "mipmap_level_0.rgb",
    "mipmap_level_1.rgb",
    "mipmap_level_2.rgb",
    "mipmap_level_3.rgb",
    "mipmap_level_4.rgb",
    "mipmap_level_5.rgb",
    "mipmap_level_6.rgb",
    "mipmap_level_7.rgb",
    "mipmap_level_8.rgb",
    "mipmap_level_9.rgb"
  };
  static const RGBColorf level_colors[num_levels] = {
    RGBColorf(1.0f, 1.0f, 1.0f),
    RGBColorf(1.0f, 0.0f, 0.0f),
    RGBColorf(0.0f, 1.0f, 0.0f),
    RGBColorf(0.0f, 0.0f, 1.0f),
    RGBColorf(1.0f, 1.0f, 0.0f),
    RGBColorf(0.0f, 1.0f, 1.0f),
    RGBColorf(1.0f, 0.0f, 1.0f),
    RGBColorf(1.0f, 0.5, 0.0f),
    RGBColorf(0.0f, 1.0f, 0.5),
    RGBColorf(0.83, 0.71, 1.0f)
  };

  level = level % num_levels;
  Filename filename(level_filenames[level]);

  PNMImage image_sized(x_size, y_size);
  PNMImage image_source;
  if (filename.resolve_filename(get_texture_path()) ||
      filename.resolve_filename(get_model_path())) {
    image_source.read(filename);
  }

  if (image_source.is_valid()) {
    image_sized.quick_filter_from(image_source);

  } else {
    GLCAT.info(false)
      << "    " << filename << " cannot be read, making solid color mipmap.\n";
    image_sized.fill(level_colors[level][0],
                     level_colors[level][1],
                     level_colors[level][2]);
  }

  PT(Texture) tex = new Texture;
  if (!tex->load(image_sized)) {
    GLCAT.warning()
      << "Unable to load phony mipmap image.\n";
  } else {
    GLenum internal_format = get_internal_image_format(tex->get_format());
    GLenum external_format = get_external_image_format(tex->get_format());
    GLenum component_type = get_component_type(tex->get_component_type());
    
#ifdef DO_PSTATS
    int num_bytes = 
      get_external_texture_bytes(tex->get_x_size(), tex->get_y_size(), 1,
                                 external_format, component_type);
    _data_transferred_pcollector.add_level(num_bytes);
#endif
    GLP(TexImage2D)(GL_TEXTURE_2D, level, internal_format,
                    tex->get_x_size(), tex->get_y_size(), 0,
                    external_format, component_type, tex->get_ram_image());
  }
}

////////////////////////////////////////////////////////////////////
//     Function: GLGraphicsStateGuardian::save_mipmap_images
//       Access: Protected
//  Description: Saves out each mipmap level of the indicated texture
//               (which must also be the currently active texture in
//               the GL state) as a separate image file to disk.
////////////////////////////////////////////////////////////////////
void CLP(GraphicsStateGuardian)::
save_mipmap_images(Texture *tex) {
  if (tex->get_texture_type() != Texture::TT_2d_texture) {
    // Never mind on unusual texture formats.
    return;
  }

  Filename filename = tex->get_name();
  string name;
  if (filename.empty()) {
    static int index = 0;
    name = "texture" + format_string(index);
    index++;
  } else {
    name = filename.get_basename_wo_extension();
  }

  GLenum external_format = get_external_image_format(tex->get_format());
  GLenum type = get_component_type(tex->get_component_type());

  int x_size = tex->get_x_size();
  int y_size = tex->get_y_size();

  // Specify byte-alignment for the pixels on output.
  GLP(PixelStorei)(GL_PACK_ALIGNMENT, 1);

  int mipmap_level = 0;
  do {
    x_size = max(x_size, 1);
    y_size = max(y_size, 1);

    PT(Texture) mtex = new Texture;
    mtex->setup_2d_texture(x_size, y_size, tex->get_component_type(), 
                           tex->get_format());
    GLP(GetTexImage)(GL_TEXTURE_2D, mipmap_level, external_format, 
                     type, mtex->make_ram_image());
    Filename mipmap_filename = name + "_" + format_string(mipmap_level) + ".rgb";
    nout << "Writing mipmap level " << mipmap_level
         << " (" << x_size << " by " << y_size << ") " 
         << mipmap_filename << "\n";
    mtex->write(mipmap_filename);

    x_size >>= 1;
    y_size >>= 1;
    mipmap_level++;
  } while (x_size > 0 || y_size > 0);
}
#endif  // NDEBUG
