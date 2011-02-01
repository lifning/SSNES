/*  SSNES - A Super Nintendo Entertainment System (SNES) Emulator frontend for libsnes.
 *  Copyright (C) 2010-2011 - Hans-Kristian Arntzen
 *
 *  Some code herein may be based on code found in BSNES.
 * 
 *  SSNES is free software: you can redistribute it and/or modify it under the terms
 *  of the GNU General Public License as published by the Free Software Found-
 *  ation, either version 3 of the License, or (at your option) any later version.
 *
 *  SSNES is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 *  without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
 *  PURPOSE.  See the GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along with SSNES.
 *  If not, see <http://www.gnu.org/licenses/>.
 */

#include "shader_cg.h"
#include <Cg/cg.h>
#include <Cg/cgGL.h>
#include "general.h"

// Used when we call deactivate() since just unbinding the program didn't seem to work... :(
static const char* stock_cg_program =
      "void main_vertex"
      "("
      "	float4 position	: POSITION,"
      "	float4 color	: COLOR,"
      "	float2 texCoord : TEXCOORD0,"
      ""
      "  uniform float4x4 modelViewProj,"
      ""
      "	out float4 oPosition : POSITION,"
      "	out float4 oColor    : COLOR,"
      "	out float2 otexCoord : TEXCOORD"
      ")"
      "{"
      "	oPosition = mul(modelViewProj, position);"
      "	oColor = color;"
      "	otexCoord = texCoord;"
      "}"
      ""
      ""
      "struct output "
      "{"
      "  float4 color    : COLOR;"
      "};"
      ""
      "output main_fragment(float2 texCoord : TEXCOORD0, uniform sampler2D decal : TEXUNIT0) "
      "{"
      "   output OUT;"
      "   OUT.color = tex2D(decal, texCoord);"
      "   return OUT;"
      "}";


static CGcontext cgCtx;
static CGprogram cgFPrg;
static CGprogram cgVPrg;
static CGprogram cgSFPrg;
static CGprogram cgSVPrg;
static CGprofile cgFProf;
static CGprofile cgVProf;
static CGparameter cg_video_size, cg_texture_size, cg_output_size;
static CGparameter cg_Vvideo_size, cg_Vtexture_size, cg_Voutput_size; // Vertexes
static CGparameter cg_mvp_matrix;
static bool cg_active = false;

void gl_cg_set_proj_matrix(void)
{
   if (cg_active)
      cgGLSetStateMatrixParameter(cg_mvp_matrix, CG_GL_MODELVIEW_PROJECTION_MATRIX, CG_GL_MATRIX_IDENTITY);
}

void gl_cg_set_params(unsigned width, unsigned height, 
      unsigned tex_width, unsigned tex_height,
      unsigned out_width, unsigned out_height)
{
   if (cg_active)
   {
      cgGLSetParameter2f(cg_video_size, width, height);
      cgGLSetParameter2f(cg_texture_size, tex_width, tex_height);
      cgGLSetParameter2f(cg_output_size, out_width, out_height);

      cgGLSetParameter2f(cg_Vvideo_size, width, height);
      cgGLSetParameter2f(cg_Vtexture_size, tex_width, tex_height);
      cgGLSetParameter2f(cg_Voutput_size, out_width, out_height);
   }
}

void gl_cg_deinit(void)
{
   if (cg_active)
      cgDestroyContext(cgCtx);
}

bool gl_cg_init(const char *path)
{
   SSNES_LOG("Loading Cg file: %s\n", path);
   cgCtx = cgCreateContext();
   if (cgCtx == NULL)
   {
      SSNES_ERR("Failed to create Cg context\n");
      return false;
   }
   cgFProf = cgGLGetLatestProfile(CG_GL_FRAGMENT);
   cgVProf = cgGLGetLatestProfile(CG_GL_VERTEX);
   if (cgFProf == CG_PROFILE_UNKNOWN || cgVProf == CG_PROFILE_UNKNOWN)
   {
      SSNES_ERR("Invalid profile type\n");
      return false;
   }
   cgGLSetOptimalOptions(cgFProf);
   cgGLSetOptimalOptions(cgVProf);
   cgFPrg = cgCreateProgramFromFile(cgCtx, CG_SOURCE, path, cgFProf, "main_fragment", 0);
   cgVPrg = cgCreateProgramFromFile(cgCtx, CG_SOURCE, path, cgVProf, "main_vertex", 0);
   cgSFPrg = cgCreateProgram(cgCtx, CG_SOURCE, stock_cg_program, cgFProf, "main_fragment", 0);
   cgSVPrg = cgCreateProgram(cgCtx, CG_SOURCE, stock_cg_program, cgVProf, "main_vertex", 0);
   if (cgFPrg == NULL || cgVPrg == NULL || cgSFPrg == NULL || cgSVPrg == NULL)
   {
      CGerror err = cgGetError();
      SSNES_ERR("CG error: %s\n", cgGetErrorString(err));
      return false;
   }
   cgGLLoadProgram(cgFPrg);
   cgGLLoadProgram(cgVPrg);
   cgGLLoadProgram(cgSFPrg);
   cgGLLoadProgram(cgSVPrg);
   cgGLEnableProfile(cgFProf);
   cgGLEnableProfile(cgVProf);
   cgGLBindProgram(cgFPrg);
   cgGLBindProgram(cgVPrg);

   cg_video_size = cgGetNamedParameter(cgFPrg, "IN.video_size");
   cg_texture_size = cgGetNamedParameter(cgFPrg, "IN.texture_size");
   cg_output_size = cgGetNamedParameter(cgFPrg, "IN.output_size");
   cg_Vvideo_size = cgGetNamedParameter(cgVPrg, "IN.video_size");
   cg_Vtexture_size = cgGetNamedParameter(cgVPrg, "IN.texture_size");
   cg_Voutput_size = cgGetNamedParameter(cgVPrg, "IN.output_size");
   cg_mvp_matrix = cgGetNamedParameter(cgVPrg, "modelViewProj");
   cgGLSetStateMatrixParameter(cg_mvp_matrix, CG_GL_MODELVIEW_PROJECTION_MATRIX, CG_GL_MATRIX_IDENTITY);
   cg_mvp_matrix = cgGetNamedParameter(cgSVPrg, "modelViewProj");
   cgGLSetStateMatrixParameter(cg_mvp_matrix, CG_GL_MODELVIEW_PROJECTION_MATRIX, CG_GL_MATRIX_IDENTITY);
   cg_active = true;
   return true;
}

void gl_cg_activate(void)
{
   if (cg_active)
   {
      cgGLBindProgram(cgFPrg);
      cgGLBindProgram(cgVPrg);
   }
}

// Just load a dummy shader.
void gl_cg_deactivate(void)
{
   if (cg_active)
   {
      cgGLBindProgram(cgSFPrg);
      cgGLBindProgram(cgSVPrg);
   }
}