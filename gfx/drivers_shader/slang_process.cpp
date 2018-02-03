
#include <fstream>
#include <iostream>
#include <spirv_hlsl.hpp>
#include <stdint.h>

#include "verbosity.h"
#include "glslang_util.h"
#include "slang_preprocess.h"
#include "slang_reflection.h"
#include "slang_process.h"

using namespace spirv_cross;
using namespace std;

template <typename P>
static bool set_unique_map(unordered_map<string, P>& m, const string& name, const P& p)
{
   auto itr = m.find(name);
   if (itr != end(m))
   {
      RARCH_ERR("[slang]: Alias \"%s\" already exists.\n", name.c_str());
      return false;
   }

   m[name] = p;
   return true;
}

template <typename M, typename S>
static string get_semantic_name(const unordered_map<string, M>* map, S semantic, unsigned index)
{
   for (const pair<string, M>& m : *map)
   {
      if (m.second.semantic == semantic && m.second.index == index)
         return m.first;
   }
   return string();
}

static string
get_semantic_name(slang_reflection& reflection, slang_semantic semantic, unsigned index)
{
   static const char* names[] = {
      "MVP",
      "OutputSize",
      "FinalViewportSize",
      "FrameCount",
   };
   if ((int)semantic < sizeof(names) / sizeof(*names))
      return std::string(names[semantic]);

   return get_semantic_name(reflection.semantic_map, semantic, index);
}

static string
get_semantic_name(slang_reflection& reflection, slang_texture_semantic semantic, unsigned index)
{
   static const char* names[] = {
      "Original", "Source", "OriginalHistory", "PassOutput", "PassFeedback",
   };
   if ((int)semantic < (int)SLANG_TEXTURE_SEMANTIC_ORIGINAL_HISTORY)
      return std::string(names[semantic]);
   else if ((int)semantic < sizeof(names) / sizeof(*names))
      return std::string(names[semantic]) + to_string(index);

   return get_semantic_name(reflection.texture_semantic_map, semantic, index);
}

static string get_size_semantic_name(
      slang_reflection& reflection, slang_texture_semantic semantic, unsigned index)
{
   static const char* names[] = {
      "OriginalSize", "SourceSize", "OriginalHistorySize", "PassOutputSize", "PassFeedbackSize",
   };
   if ((int)semantic < (int)SLANG_TEXTURE_SEMANTIC_ORIGINAL_HISTORY)
      return std::string(names[semantic]);
   if ((int)semantic < sizeof(names) / sizeof(*names))
      return std::string(names[semantic]) + to_string(index);

   return get_semantic_name(reflection.texture_semantic_uniform_map, semantic, index);
}

static bool slang_process_reflection(
      const Compiler*        vs_compiler,
      const Compiler*        ps_compiler,
      const ShaderResources& vs_resources,
      const ShaderResources& ps_resources,
      video_shader*          shader_info,
      unsigned               pass_number,
      const semantics_map_t* map,
      pass_semantics_t*      out)
{
   unordered_map<string, slang_texture_semantic_map> texture_semantic_map;
   unordered_map<string, slang_texture_semantic_map> texture_semantic_uniform_map;

   for (unsigned i = 0; i <= pass_number; i++)
   {
      if (!*shader_info->pass[i].alias)
         continue;

      string name = shader_info->pass[i].alias;

      if (!set_unique_map(
                texture_semantic_map, name,
                slang_texture_semantic_map{ SLANG_TEXTURE_SEMANTIC_PASS_OUTPUT, i }))
         return false;

      if (!set_unique_map(
                texture_semantic_uniform_map, name + "Size",
                slang_texture_semantic_map{ SLANG_TEXTURE_SEMANTIC_PASS_OUTPUT, i }))
         return false;

      if (!set_unique_map(
                texture_semantic_map, name + "Feedback",
                slang_texture_semantic_map{ SLANG_TEXTURE_SEMANTIC_PASS_FEEDBACK, i }))
         return false;

      if (!set_unique_map(
                texture_semantic_uniform_map, name + "FeedbackSize",
                slang_texture_semantic_map{ SLANG_TEXTURE_SEMANTIC_PASS_FEEDBACK, i }))
         return false;
   }

   for (unsigned i = 0; i < shader_info->luts; i++)
   {
      if (!set_unique_map(
                texture_semantic_map, shader_info->lut[i].id,
                slang_texture_semantic_map{ SLANG_TEXTURE_SEMANTIC_USER, i }))
         return false;

      if (!set_unique_map(
                texture_semantic_uniform_map, string(shader_info->lut[i].id) + "Size",
                slang_texture_semantic_map{ SLANG_TEXTURE_SEMANTIC_USER, i }))
         return false;
   }

   unordered_map<string, slang_semantic_map> uniform_semantic_map;

   for (unsigned i = 0; i < shader_info->num_parameters; i++)
   {
      if (!set_unique_map(
                uniform_semantic_map, shader_info->parameters[i].id,
                { SLANG_SEMANTIC_FLOAT_PARAMETER, i }))
         return false;
   }

   slang_reflection sl_reflection;
   sl_reflection.pass_number                  = pass_number;
   sl_reflection.texture_semantic_map         = &texture_semantic_map;
   sl_reflection.texture_semantic_uniform_map = &texture_semantic_uniform_map;
   sl_reflection.semantic_map                 = &uniform_semantic_map;

   if (!slang_reflect(*vs_compiler, *ps_compiler, vs_resources, ps_resources, &sl_reflection))
   {
      RARCH_ERR("[slang]: Failed to reflect SPIR-V. Resource usage is inconsistent with "
                "expectations.\n");
      return false;
   }

   out->cbuffers[SLANG_CBUFFER_UBO].stage_mask = sl_reflection.ubo_stage_mask;
   out->cbuffers[SLANG_CBUFFER_UBO].binding    = sl_reflection.ubo_binding;
   out->cbuffers[SLANG_CBUFFER_UBO].size       = (sl_reflection.ubo_size + 0xF) & ~0xF;
   out->cbuffers[SLANG_CBUFFER_PC].stage_mask  = sl_reflection.push_constant_stage_mask;
   out->cbuffers[SLANG_CBUFFER_PC].binding     = sl_reflection.ubo_binding ? 0 : 1;
   out->cbuffers[SLANG_CBUFFER_PC].size        = (sl_reflection.push_constant_size + 0xF) & ~0xF;

   vector<uniform_sem_t> uniforms[SLANG_CBUFFER_MAX];
   vector<texture_sem_t> textures;

   for (int semantic = 0; semantic < SLANG_NUM_BASE_SEMANTICS; semantic++)
   {
      slang_semantic_meta& src = sl_reflection.semantics[semantic];
      if (src.push_constant || src.uniform)
      {
         uniform_sem_t uniform = { map->uniforms[semantic],
                                   src.num_components * (unsigned)sizeof(float) };

         string uniform_id = get_semantic_name(sl_reflection, (slang_semantic)semantic, 0);
         strncpy(uniform.id, uniform_id.c_str(), sizeof(uniform.id));

         if (src.push_constant)
         {
            uniform.offset = src.push_constant_offset;
            uniforms[SLANG_CBUFFER_PC].push_back(uniform);
         }
         else
         {
            uniform.offset = src.ubo_offset;
            uniforms[SLANG_CBUFFER_UBO].push_back(uniform);
         }
      }
   }

   for (int i = 0; i < sl_reflection.semantic_float_parameters.size(); i++)
   {
      slang_semantic_meta& src = sl_reflection.semantic_float_parameters[i];

      if (src.push_constant || src.uniform)
      {
         uniform_sem_t uniform = { &shader_info->parameters[i].current, sizeof(float) };

         string uniform_id = get_semantic_name(sl_reflection, SLANG_SEMANTIC_FLOAT_PARAMETER, i);
         strncpy(uniform.id, uniform_id.c_str(), sizeof(uniform.id));

         if (src.push_constant)
         {
            uniform.offset = src.push_constant_offset;
            uniforms[SLANG_CBUFFER_PC].push_back(uniform);
         }
         else
         {
            uniform.offset = src.ubo_offset;
            uniforms[SLANG_CBUFFER_UBO].push_back(uniform);
         }
      }
   }

   for (int semantic = 0; semantic < SLANG_NUM_TEXTURE_SEMANTICS; semantic++)
   {
      for (int index = 0; index < sl_reflection.semantic_textures[semantic].size(); index++)
      {
         slang_texture_semantic_meta& src = sl_reflection.semantic_textures[semantic][index];

         if (src.stage_mask)
         {
            texture_sem_t texture = {
               (void*)((uintptr_t)map->textures[semantic].image + index * map->textures[semantic].image_stride),
               (void*)((uintptr_t)map->textures[semantic].sampler + index * map->textures[semantic].sampler_stride),
            };
            texture.stage_mask = src.stage_mask;
            texture.binding    = src.binding;
            string id = get_semantic_name(sl_reflection, (slang_texture_semantic)semantic, index);

            strncpy(texture.id, id.c_str(), sizeof(texture.id));

            textures.push_back(texture);

            if (semantic == SLANG_TEXTURE_SEMANTIC_PASS_FEEDBACK)
               shader_info->pass[index].feedback = true;

            if (semantic == SLANG_TEXTURE_SEMANTIC_ORIGINAL_HISTORY &&
                shader_info->history_size < index)
               shader_info->history_size = index;
         }

         if (src.push_constant || src.uniform)
         {
            uniform_sem_t uniform = {
               (void*)((uintptr_t)map->textures[semantic].size + index * map->textures[semantic].size_stride),
               4 * sizeof(float)
            };

            string uniform_id =
                  get_size_semantic_name(sl_reflection, (slang_texture_semantic)semantic, index);

            strncpy(uniform.id, uniform_id.c_str(), sizeof(uniform.id));

            if (src.push_constant)
            {
               uniform.offset = src.push_constant_offset;
               uniforms[SLANG_CBUFFER_PC].push_back(uniform);
            }
            else
            {
               uniform.offset = src.ubo_offset;
               uniforms[SLANG_CBUFFER_UBO].push_back(uniform);
            }
         }
      }
   }

   out->texture_count = textures.size();

   textures.push_back({ NULL });
   out->textures = (texture_sem_t*)malloc(textures.size() * sizeof(*textures.data()));
   memcpy(out->textures, textures.data(), textures.size() * sizeof(*textures.data()));

   for (int i = 0; i < SLANG_CBUFFER_MAX; i++)
   {
      if (uniforms[i].empty())
         continue;

      out->cbuffers[i].uniform_count = uniforms[i].size();

      uniforms[i].push_back({ NULL });
      out->cbuffers[i].uniforms =
            (uniform_sem_t*)malloc(uniforms[i].size() * sizeof(*uniforms[i].data()));
      memcpy(
            out->cbuffers[i].uniforms, uniforms[i].data(),
            uniforms[i].size() * sizeof(*uniforms[i].data()));
   }

   return true;
}

bool slang_process(
      video_shader*          shader_info,
      unsigned               pass_number,
      enum rarch_shader_type dst_type,
      unsigned               version,
      const semantics_map_t* semantics_map,
      pass_semantics_t*      out)
{
   Compiler*          vs_compiler = NULL;
   Compiler*          ps_compiler = NULL;
   video_shader_pass& pass        = shader_info->pass[pass_number];
   glslang_output     output;

   if (!glslang_compile_shader(pass.source.path, &output))
      return false;

   if (!slang_preprocess_parse_parameters(output.meta, shader_info))
      return false;

   if (!*pass.alias && !output.meta.name.empty())
      strncpy(pass.alias, output.meta.name.c_str(), sizeof(pass.alias) - 1);

   out->format = output.meta.rt_format;

   if (out->format == SLANG_FORMAT_UNKNOWN)
   {
      if (pass.fbo.srgb_fbo)
         out->format = SLANG_FORMAT_R8G8B8A8_SRGB;
      else if (pass.fbo.fp_fbo)
         out->format = SLANG_FORMAT_R16G16B16A16_SFLOAT;
      else
         out->format = SLANG_FORMAT_R8G8B8A8_UNORM;
   }

   pass.source.string.vertex   = NULL;
   pass.source.string.fragment = NULL;

   try
   {
      ShaderResources vs_resources;
      ShaderResources ps_resources;
      string          vs_code;
      string          ps_code;

      if (dst_type == RARCH_SHADER_HLSL || dst_type == RARCH_SHADER_CG)
      {
         vs_compiler = new CompilerHLSL(output.vertex);
         ps_compiler = new CompilerHLSL(output.fragment);
      }
      else
      {
         vs_compiler = new CompilerGLSL(output.vertex);
         ps_compiler = new CompilerGLSL(output.fragment);
      }

      vs_resources = vs_compiler->get_shader_resources();
      ps_resources = ps_compiler->get_shader_resources();

      if (!vs_resources.uniform_buffers.empty())
         vs_compiler->set_decoration(vs_resources.uniform_buffers[0].id, spv::DecorationBinding, 0);
      if (!ps_resources.uniform_buffers.empty())
         ps_compiler->set_decoration(ps_resources.uniform_buffers[0].id, spv::DecorationBinding, 0);

      if (!vs_resources.push_constant_buffers.empty())
         vs_compiler->set_decoration(
               vs_resources.push_constant_buffers[0].id, spv::DecorationBinding, 1);
      if (!ps_resources.push_constant_buffers.empty())
         ps_compiler->set_decoration(
               ps_resources.push_constant_buffers[0].id, spv::DecorationBinding, 1);

      if (dst_type == RARCH_SHADER_HLSL || dst_type == RARCH_SHADER_CG)
      {
         CompilerHLSL::Options options;
         CompilerHLSL*         vs = (CompilerHLSL*)vs_compiler;
         CompilerHLSL*         ps = (CompilerHLSL*)ps_compiler;
         options.shader_model     = version;
         vs->set_options(options);
         ps->set_options(options);

         /* not exactly a vertex attribute but this remaps
          * float2 FragCoord :TEXCOORD# to float4 FragCoord : SV_POSITION */
         std::vector<HLSLVertexAttributeRemap> ps_attrib_remap;

         VariableTypeRemapCallback ps_var_remap_cb =
               [&](const SPIRType& type, const std::string& var_name, std::string& name_of_type) {
                  if (var_name == "FragCoord")
                  {
                     name_of_type = "float4";
                  }
               };
         for (Resource& resource : ps_resources.stage_inputs)
         {
            if (ps->get_name(resource.id) == "FragCoord")
            {
               uint32_t location = ps->get_decoration(resource.id, spv::DecorationLocation);
               ps_attrib_remap.push_back({ location, "SV_Position" });
               ps->set_variable_type_remap_callback(ps_var_remap_cb);
            }
         }

         vs_code = vs->compile();
         ps_code = ps->compile(ps_attrib_remap);
      }
      else if (shader_info->type == RARCH_SHADER_GLSL)
      {
         CompilerGLSL::Options options;
         CompilerGLSL*         vs = (CompilerGLSL*)vs_compiler;
         CompilerGLSL*         ps = (CompilerGLSL*)ps_compiler;
         options.version          = version;
         ps->set_options(options);
         vs->set_options(options);

         vs_code = vs->compile();
         ps_code = ps->compile();
      }
      else
         goto error;

      pass.source.string.vertex   = strdup(vs_code.c_str());
      pass.source.string.fragment = strdup(ps_code.c_str());

      if (!slang_process_reflection(
                vs_compiler, ps_compiler, vs_resources, ps_resources, shader_info, pass_number,
                semantics_map, out))
         goto error;

   } catch (const std::exception& e)
   {
      RARCH_ERR("[slang]: SPIRV-Cross threw exception: %s.\n", e.what());
      goto error;
   }

   delete vs_compiler;
   delete ps_compiler;

   return true;

error:
   free(pass.source.string.vertex);
   free(pass.source.string.fragment);

   pass.source.string.vertex   = NULL;
   pass.source.string.fragment = NULL;

   delete vs_compiler;
   delete ps_compiler;

   return false;
}
