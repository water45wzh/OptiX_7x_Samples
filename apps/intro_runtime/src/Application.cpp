/* 
 * Copyright (c) 2013-2020, NVIDIA CORPORATION. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *  * Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *  * Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *  * Neither the name of NVIDIA CORPORATION nor the names of its
 *    contributors may be used to endorse or promote products derived
 *    from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "shaders/app_config.h"

#include "inc/Application.h"
#include "inc/CheckMacros.h"


#ifdef _WIN32
// The cfgmgr32 header is necessary for interrogating driver information in the registry.
#include <cfgmgr32.h>
// For convenience the library is also linked in automatically using the #pragma command.
#pragma comment(lib, "Cfgmgr32.lib")
#else
#include <dlfcn.h>
#endif

#include <algorithm>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <sstream>
#include <string.h>
#include <time.h>
#include <vector>

#include "shaders/vector_math.h"
#include "shaders/system_parameter.h"
#include "shaders/material_parameter.h"
// Only needed for the FLAG_THINWALLED
#include "shaders/per_ray_data.h"

#include <inc/MyAssert.h>


#ifdef _WIN32
// Code based on helper function in optix_stubs.h
static void* optixLoadWindowsDll(void)
{
  const char* optixDllName = "nvoptix.dll";
  void* handle = NULL;

  // Get the size of the path first, then allocate
  unsigned int size = GetSystemDirectoryA(NULL, 0);
  if (size == 0)
  {
    // Couldn't get the system path size, so bail
    return NULL;
  }

  size_t pathSize = size + 1 + strlen(optixDllName);
  char*  systemPath = (char*) malloc(pathSize);

  if (GetSystemDirectoryA(systemPath, size) != size - 1)
  {
    // Something went wrong
    free(systemPath);
    return NULL;
  }

  strcat(systemPath, "\\");
  strcat(systemPath, optixDllName);

  handle = LoadLibraryA(systemPath);

  free(systemPath);

  if (handle)
  {
    return handle;
  }

  // If we didn't find it, go looking in the register store.  Since nvoptix.dll doesn't
  // have its own registry entry, we are going to look for the OpenGL driver which lives
  // next to nvoptix.dll. 0 (null) will be returned if any errors occured.

  static const char* deviceInstanceIdentifiersGUID = "{4d36e968-e325-11ce-bfc1-08002be10318}";
  const ULONG        flags = CM_GETIDLIST_FILTER_CLASS | CM_GETIDLIST_FILTER_PRESENT;
  ULONG              deviceListSize = 0;

  if (CM_Get_Device_ID_List_SizeA(&deviceListSize, deviceInstanceIdentifiersGUID, flags) != CR_SUCCESS)
  {
    return NULL;
  }

  char* deviceNames = (char*) malloc(deviceListSize);

  if (CM_Get_Device_ID_ListA(deviceInstanceIdentifiersGUID, deviceNames, deviceListSize, flags))
  {
    free(deviceNames);
    return NULL;
  }

  DEVINST devID = 0;

  // Continue to the next device if errors are encountered.
  for (char* deviceName = deviceNames; *deviceName; deviceName += strlen(deviceName) + 1)
  {
    if (CM_Locate_DevNodeA(&devID, deviceName, CM_LOCATE_DEVNODE_NORMAL) != CR_SUCCESS)
    {
      continue;
    }

    HKEY regKey = 0;
    if (CM_Open_DevNode_Key(devID, KEY_QUERY_VALUE, 0, RegDisposition_OpenExisting, &regKey, CM_REGISTRY_SOFTWARE) != CR_SUCCESS)
    {
      continue;
    }

    const char* valueName = "OpenGLDriverName";
    DWORD       valueSize = 0;

    LSTATUS     ret = RegQueryValueExA(regKey, valueName, NULL, NULL, NULL, &valueSize);
    if (ret != ERROR_SUCCESS)
    {
      RegCloseKey(regKey);
      continue;
    }

    char* regValue = (char*) malloc(valueSize);
    ret = RegQueryValueExA(regKey, valueName, NULL, NULL, (LPBYTE) regValue, &valueSize);
    if (ret != ERROR_SUCCESS)
    {
      free(regValue);
      RegCloseKey(regKey);
      continue;
    }

    // Strip the OpenGL driver dll name from the string then create a new string with
    // the path and the nvoptix.dll name
    for (int i = valueSize - 1; i >= 0 && regValue[i] != '\\'; --i)
    {
      regValue[i] = '\0';
    }

    size_t newPathSize = strlen(regValue) + strlen(optixDllName) + 1;
    char*  dllPath = (char*) malloc(newPathSize);
    strcpy(dllPath, regValue);
    strcat(dllPath, optixDllName);

    free(regValue);
    RegCloseKey(regKey);

    handle = LoadLibraryA((LPCSTR) dllPath);
    free(dllPath);

    if (handle)
    {
      break;
    }
  }

  free(deviceNames);

  return handle;
}
#endif


Application::Application(GLFWwindow* window,
                         Options const& options)
: m_window(window)
, m_logger(std::cerr)
{
  m_width   = std::max(1, options.getClientWidth());
  m_height  = std::max(1, options.getClientHeight());
  m_interop = options.getInterop();

  m_lightID = options.getLight();
  m_missID  = options.getMiss();
  m_environmentFilename = options.getEnvironment();

  m_isValid = false;

  m_sceneEpsilonFactor = 500;  // Factor on SCENE_EPSILOPN_SCALE (1.0e-7f) used to offset ray tmin interval along the path to reduce self-intersections.
  m_iterationIndex = 0; 
  
  m_pbo = 0;
  m_hdrTexture = 0;

  m_outputBuffer = new float4[m_width * m_height];

  m_present         = false;  // Update once per second. (The first half second shows all frames to get some initial accumulation).
  m_presentNext     = true;
  m_presentAtSecond = 1.0;

  m_frames = 0; // Samples per pixel. 0 == render forever.
    
  m_glslVS = 0;
  m_glslFS = 0;
  m_glslProgram = 0;

#if 1 // Tonemapper defaults
  m_gamma          = 2.2f;
  m_colorBalance   = make_float3(1.0f, 1.0f, 1.0f);
  m_whitePoint     = 1.0f;
  m_burnHighlights = 0.8f;
  m_crushBlacks    = 0.2f;
  m_saturation     = 1.2f;
  m_brightness     = 0.8f;
#else // Neutral tonemapper settings.
  m_gamma          = 1.0f;
  m_colorBalance   = make_float3(1.0f, 1.0f, 1.0f);
  m_whitePoint     = 1.0f;
  m_burnHighlights = 1.0f;
  m_crushBlacks    = 0.0f;
  m_saturation     = 1.0f;
  m_brightness     = 1.0f;
#endif

  m_guiState = GUI_STATE_NONE;

  m_isVisibleGUI = true;

  m_mouseSpeedRatio = 10.0f;

  m_pinholeCamera.setViewport(m_width, m_height);

  m_textureEnvironment = nullptr;
  m_textureAlbedo      = nullptr;
  m_textureCutout      = nullptr;
  
  m_vboAttributes = 0;
  m_vboIndices = 0;
    
  m_positionLocation = -1;
  m_texCoordLocation = -1;
  
  m_cudaGraphicsResource = nullptr;

  m_context = nullptr;

  m_root = 0;
  m_d_ias = 0;

  m_pipeline = nullptr;
  
  m_d_systemParameter = nullptr;
  
  // The Shader Binding Table and data.
  m_d_sbtRecordRaygeneration = 0;
  m_d_sbtRecordException = 0;
  m_d_sbtRecordMiss = 0;

  m_d_sbtRecordCallables = 0;

  m_d_sbtRecordGeometryInstanceData = nullptr;

  // Initialize all renderer system parameters.
  m_systemParameter.topObject          = 0;
  m_systemParameter.outputBuffer       = nullptr;
  m_systemParameter.lightDefinitions   = nullptr;
  m_systemParameter.materialParameters = nullptr;
  m_systemParameter.envTexture         = 0;
  m_systemParameter.envCDF_U           = nullptr;
  m_systemParameter.envCDF_V           = nullptr;
  m_systemParameter.pathLengths        = make_int2(2, 5);
  m_systemParameter.envWidth           = 0;
  m_systemParameter.envHeight          = 0;
  m_systemParameter.envIntegral        = 1.0f;
  m_systemParameter.envRotation        = 0.0f;
  m_systemParameter.iterationIndex     = 0;
  m_systemParameter.sceneEpsilon       = m_sceneEpsilonFactor * SCENE_EPSILON_SCALE;
  m_systemParameter.numLights          = 0;
  m_systemParameter.cameraType         = 0;
  m_systemParameter.cameraPosition     = make_float3(0.0f, 0.0f, 1.0f);
  m_systemParameter.cameraU            = make_float3(1.0f, 0.0f, 0.0f);
  m_systemParameter.cameraV            = make_float3(0.0f, 1.0f, 0.0f);
  m_systemParameter.cameraW            = make_float3(0.0f, 0.0f, -1.0f);

  // Setup ImGui binding.
  ImGui::CreateContext();
  ImGui_ImplGlfwGL3_Init(window, true);

  // This initializes the GLFW part including the font texture.
  ImGui_ImplGlfwGL3_NewFrame();
  ImGui::EndFrame();

#if 1
  // Style the GUI colors to a neutral greyscale with plenty of transparency to concentrate on the image.
  ImGuiStyle& style = ImGui::GetStyle();

  // Change these RGB values to get any other tint.
  const float r = 1.0f;
  const float g = 1.0f;
  const float b = 1.0f;
  
  style.Colors[ImGuiCol_Text]                  = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
  style.Colors[ImGuiCol_TextDisabled]          = ImVec4(0.5f, 0.5f, 0.5f, 1.0f);
  style.Colors[ImGuiCol_WindowBg]              = ImVec4(r * 0.2f, g * 0.2f, b * 0.2f, 0.6f);
  style.Colors[ImGuiCol_ChildWindowBg]         = ImVec4(r * 0.2f, g * 0.2f, b * 0.2f, 1.0f);
  style.Colors[ImGuiCol_PopupBg]               = ImVec4(r * 0.2f, g * 0.2f, b * 0.2f, 1.0f);
  style.Colors[ImGuiCol_Border]                = ImVec4(r * 0.4f, g * 0.4f, b * 0.4f, 0.4f);
  style.Colors[ImGuiCol_BorderShadow]          = ImVec4(r * 0.0f, g * 0.0f, b * 0.0f, 0.4f);
  style.Colors[ImGuiCol_FrameBg]               = ImVec4(r * 0.4f, g * 0.4f, b * 0.4f, 0.4f);
  style.Colors[ImGuiCol_FrameBgHovered]        = ImVec4(r * 0.6f, g * 0.6f, b * 0.6f, 0.6f);
  style.Colors[ImGuiCol_FrameBgActive]         = ImVec4(r * 0.8f, g * 0.8f, b * 0.8f, 0.8f);
  style.Colors[ImGuiCol_TitleBg]               = ImVec4(r * 0.6f, g * 0.6f, b * 0.6f, 0.6f);
  style.Colors[ImGuiCol_TitleBgCollapsed]      = ImVec4(r * 0.2f, g * 0.2f, b * 0.2f, 0.2f);
  style.Colors[ImGuiCol_TitleBgActive]         = ImVec4(r * 0.8f, g * 0.8f, b * 0.8f, 0.8f);
  style.Colors[ImGuiCol_MenuBarBg]             = ImVec4(r * 0.2f, g * 0.2f, b * 0.2f, 1.0f);
  style.Colors[ImGuiCol_ScrollbarBg]           = ImVec4(r * 0.2f, g * 0.2f, b * 0.2f, 0.2f);
  style.Colors[ImGuiCol_ScrollbarGrab]         = ImVec4(r * 0.4f, g * 0.4f, b * 0.4f, 0.4f);
  style.Colors[ImGuiCol_ScrollbarGrabHovered]  = ImVec4(r * 0.6f, g * 0.6f, b * 0.6f, 0.6f);
  style.Colors[ImGuiCol_ScrollbarGrabActive]   = ImVec4(r * 0.8f, g * 0.8f, b * 0.8f, 0.8f);
  style.Colors[ImGuiCol_CheckMark]             = ImVec4(r * 0.8f, g * 0.8f, b * 0.8f, 0.8f);
  style.Colors[ImGuiCol_SliderGrab]            = ImVec4(r * 0.4f, g * 0.4f, b * 0.4f, 0.4f);
  style.Colors[ImGuiCol_SliderGrabActive]      = ImVec4(r * 0.8f, g * 0.8f, b * 0.8f, 0.8f);
  style.Colors[ImGuiCol_Button]                = ImVec4(r * 0.4f, g * 0.4f, b * 0.4f, 0.4f);
  style.Colors[ImGuiCol_ButtonHovered]         = ImVec4(r * 0.6f, g * 0.6f, b * 0.6f, 0.6f);
  style.Colors[ImGuiCol_ButtonActive]          = ImVec4(r * 0.8f, g * 0.8f, b * 0.8f, 0.8f);
  style.Colors[ImGuiCol_Header]                = ImVec4(r * 0.4f, g * 0.4f, b * 0.4f, 0.4f);
  style.Colors[ImGuiCol_HeaderHovered]         = ImVec4(r * 0.6f, g * 0.6f, b * 0.6f, 0.6f);
  style.Colors[ImGuiCol_HeaderActive]          = ImVec4(r * 0.8f, g * 0.8f, b * 0.8f, 0.8f);
  style.Colors[ImGuiCol_Column]                = ImVec4(r * 0.4f, g * 0.4f, b * 0.4f, 0.4f);
  style.Colors[ImGuiCol_ColumnHovered]         = ImVec4(r * 0.6f, g * 0.6f, b * 0.6f, 0.6f);
  style.Colors[ImGuiCol_ColumnActive]          = ImVec4(r * 0.8f, g * 0.8f, b * 0.8f, 0.8f);
  style.Colors[ImGuiCol_ResizeGrip]            = ImVec4(r * 0.6f, g * 0.6f, b * 0.6f, 0.6f);
  style.Colors[ImGuiCol_ResizeGripHovered]     = ImVec4(r * 0.8f, g * 0.8f, b * 0.8f, 0.8f);
  style.Colors[ImGuiCol_ResizeGripActive]      = ImVec4(r * 1.0f, g * 1.0f, b * 1.0f, 1.0f);
  style.Colors[ImGuiCol_CloseButton]           = ImVec4(r * 0.4f, g * 0.4f, b * 0.4f, 0.4f);
  style.Colors[ImGuiCol_CloseButtonHovered]    = ImVec4(r * 0.6f, g * 0.6f, b * 0.6f, 0.6f);
  style.Colors[ImGuiCol_CloseButtonActive]     = ImVec4(r * 0.8f, g * 0.8f, b * 0.8f, 0.8f);
  style.Colors[ImGuiCol_PlotLines]             = ImVec4(r * 0.8f, g * 0.8f, b * 0.8f, 1.0f);
  style.Colors[ImGuiCol_PlotLinesHovered]      = ImVec4(r * 1.0f, g * 1.0f, b * 1.0f, 1.0f);
  style.Colors[ImGuiCol_PlotHistogram]         = ImVec4(r * 0.8f, g * 0.8f, b * 0.8f, 1.0f);
  style.Colors[ImGuiCol_PlotHistogramHovered]  = ImVec4(r * 1.0f, g * 1.0f, b * 1.0f, 1.0f);
  style.Colors[ImGuiCol_TextSelectedBg]        = ImVec4(r * 0.5f, g * 0.5f, b * 0.5f, 1.0f);
  style.Colors[ImGuiCol_ModalWindowDarkening]  = ImVec4(r * 0.2f, g * 0.2f, b * 0.2f, 0.2f);
  style.Colors[ImGuiCol_DragDropTarget]        = ImVec4(r * 1.0f, g * 1.0f, b * 0.0f, 1.0f); // Yellow
  style.Colors[ImGuiCol_NavHighlight]          = ImVec4(r * 1.0f, g * 1.0f, b * 1.0f, 1.0f);
  style.Colors[ImGuiCol_NavWindowingHighlight] = ImVec4(r * 1.0f, g * 1.0f, b * 1.0f, 1.0f);
#endif

  initOpenGL();

  m_moduleFilenames.resize(NUM_MODULE_IDENTIFIERS);

  // Starting with OptiX SDK 7.5.0 and CUDA 11.7 either PTX or OptiX IR input can be used to create modules.
  // Just initialize the m_moduleFilenames depending on the definition of USE_OPTIX_IR.
  // That is added to the project definitions inside the CMake script when OptiX SDK 7.5.0 and CUDA 11.7 or newer are found.
#if defined(USE_OPTIX_IR)
  m_moduleFilenames[MODULE_ID_RAYGENERATION]                    = std::string("./intro_runtime_core/raygeneration.optixir");
  m_moduleFilenames[MODULE_ID_EXCEPTION]                        = std::string("./intro_runtime_core/exception.optixir");
  m_moduleFilenames[MODULE_ID_MISS]                             = std::string("./intro_runtime_core/miss.optixir");
  m_moduleFilenames[MODULE_ID_CLOSESTHIT]                       = std::string("./intro_runtime_core/closesthit.optixir");
  m_moduleFilenames[MODULE_ID_ANYHIT]                           = std::string("./intro_runtime_core/anyhit.optixir");
  m_moduleFilenames[MODULE_ID_LENS_SHADER]                      = std::string("./intro_runtime_core/lens_shader.optixir");
  m_moduleFilenames[MODULE_ID_LIGHT_SAMPLE]                     = std::string("./intro_runtime_core/light_sample.optixir");
  m_moduleFilenames[MODULE_ID_DIFFUSE_REFLECTION]               = std::string("./intro_runtime_core/bsdf_diffuse_reflection.optixir");
  m_moduleFilenames[MODULE_ID_SPECULAR_REFLECTION]              = std::string("./intro_runtime_core/bsdf_specular_reflection.optixir");
  m_moduleFilenames[MODULE_ID_SPECULAR_REFLECTION_TRANSMISSION] = std::string("./intro_runtime_core/bsdf_specular_reflection_transmission.optixir");
#else
  m_moduleFilenames[MODULE_ID_RAYGENERATION]                    = std::string("./intro_runtime_core/raygeneration.ptx");
  m_moduleFilenames[MODULE_ID_EXCEPTION]                        = std::string("./intro_runtime_core/exception.ptx");
  m_moduleFilenames[MODULE_ID_MISS]                             = std::string("./intro_runtime_core/miss.ptx");
  m_moduleFilenames[MODULE_ID_CLOSESTHIT]                       = std::string("./intro_runtime_core/closesthit.ptx");
  m_moduleFilenames[MODULE_ID_ANYHIT]                           = std::string("./intro_runtime_core/anyhit.ptx");
  m_moduleFilenames[MODULE_ID_LENS_SHADER]                      = std::string("./intro_runtime_core/lens_shader.ptx");
  m_moduleFilenames[MODULE_ID_LIGHT_SAMPLE]                     = std::string("./intro_runtime_core/light_sample.ptx");
  m_moduleFilenames[MODULE_ID_DIFFUSE_REFLECTION]               = std::string("./intro_runtime_core/bsdf_diffuse_reflection.ptx");
  m_moduleFilenames[MODULE_ID_SPECULAR_REFLECTION]              = std::string("./intro_runtime_core/bsdf_specular_reflection.ptx");
  m_moduleFilenames[MODULE_ID_SPECULAR_REFLECTION_TRANSMISSION] = std::string("./intro_runtime_core/bsdf_specular_reflection_transmission.ptx");
#endif

  m_isValid = initOptiX();
}


Application::~Application()
{
  if (m_isValid)
  {
    CUDA_CHECK( cudaStreamSynchronize(m_cudaStream) );

    // Delete the textures while the context is still alive.
    delete m_textureEnvironment;
    delete m_textureAlbedo;
    delete m_textureCutout;

    if (m_interop)
    {
      CUDA_CHECK( cudaGraphicsUnregisterResource(m_cudaGraphicsResource) );
      glDeleteBuffers(1, &m_pbo);
    }
    else
    {
      CUDA_CHECK( cudaFree((void*) m_systemParameter.outputBuffer) );
      delete[] m_outputBuffer;
    }
    CUDA_CHECK( cudaFree((void*) m_systemParameter.lightDefinitions) );
    CUDA_CHECK( cudaFree((void*) m_systemParameter.materialParameters) );
    CUDA_CHECK( cudaFree((void*) m_d_systemParameter) );

    for (size_t i = 0; i < m_geometries.size(); ++i)
    {
      CUDA_CHECK( cudaFree((void*) m_geometries[i].indices) );
      CUDA_CHECK( cudaFree((void*) m_geometries[i].attributes) );
      CUDA_CHECK( cudaFree((void*) m_geometries[i].gas) );
    }
    CUDA_CHECK( cudaFree((void*) m_d_ias) );

    CUDA_CHECK( cudaFree((void*) m_d_sbtRecordRaygeneration) );
    CUDA_CHECK( cudaFree((void*) m_d_sbtRecordException) );
    CUDA_CHECK( cudaFree((void*) m_d_sbtRecordMiss) );
    CUDA_CHECK( cudaFree((void*) m_d_sbtRecordCallables) );

    CUDA_CHECK( cudaFree((void*) m_d_sbtRecordGeometryInstanceData) );

    OPTIX_CHECK( m_api.optixPipelineDestroy(m_pipeline) );
    OPTIX_CHECK( m_api.optixDeviceContextDestroy(m_context) );

    CUDA_CHECK( cudaStreamDestroy(m_cudaStream) );
    // FIXME No way to explicitly destroy the CUDA context here using only CUDA Runtime API calls?
	
    glDeleteBuffers(1, &m_vboAttributes);
    glDeleteBuffers(1, &m_vboIndices);

    glDeleteProgram(m_glslProgram);
  }

  ImGui_ImplGlfwGL3_Shutdown();
  ImGui::DestroyContext();
}

bool Application::isValid() const
{
  return m_isValid;
}

void Application::reshape(int width, int height)
{
  if ((width != 0 && height != 0) && // Zero sized interop buffers are not allowed in OptiX.
      (m_width != width || m_height != height))
  {
    m_width  = width;
    m_height = height;

    glViewport(0, 0, m_width, m_height);

    if (m_interop)
    {
      CUDA_CHECK( cudaGraphicsUnregisterResource(m_cudaGraphicsResource) ); // No flags for read-write access during accumulation.

      glBindBuffer(GL_PIXEL_UNPACK_BUFFER, m_pbo);
      glBufferData(GL_PIXEL_UNPACK_BUFFER, m_width * m_height * sizeof(float) * 4, nullptr, GL_DYNAMIC_DRAW);
      glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);

      CUDA_CHECK( cudaGraphicsGLRegisterBuffer(&m_cudaGraphicsResource, m_pbo, cudaGraphicsRegisterFlagsNone) );

      size_t size;

      CUDA_CHECK( cudaGraphicsMapResources(1, &m_cudaGraphicsResource, m_cudaStream) );
      CUDA_CHECK( cudaGraphicsResourceGetMappedPointer((void**) &m_systemParameter.outputBuffer, &size, m_cudaGraphicsResource) ); // DAR Redundant. Must be done on each map anyway.
      CUDA_CHECK( cudaGraphicsUnmapResources(1, &m_cudaGraphicsResource, m_cudaStream) );
      
      MY_ASSERT(m_width * m_height * sizeof(float) * 4 <= size);
    }
    else
    {
      delete[] m_outputBuffer;
      m_outputBuffer = new float4[m_width * m_height];

      CUDA_CHECK( cudaFree((void*) m_systemParameter.outputBuffer) );
      CUDA_CHECK( cudaMalloc((void**) &m_systemParameter.outputBuffer, sizeof(float4) * m_width * m_height) );
    }

    m_pinholeCamera.setViewport(m_width, m_height);

    restartAccumulation();
  }
}

void Application::guiNewFrame()
{
  ImGui_ImplGlfwGL3_NewFrame();
}

void Application::guiReferenceManual()
{
  ImGui::ShowTestWindow();
}

void Application::guiRender()
{
  ImGui::Render();
  ImGui_ImplGlfwGL3_RenderDrawData(ImGui::GetDrawData());
}


void Application::getSystemInformation()
{
  int versionDriver = 0;
  CUDA_CHECK( cudaDriverGetVersion(&versionDriver) ); 
  
  // The version is returned as (1000 * major + 10 * minor).
  int major =  versionDriver / 1000;
  int minor = (versionDriver - major * 1000) / 10;
  std::cout << "Driver Version  = " << major << "." << minor << '\n';
  
  int versionRuntime = 0;
  CUDA_CHECK( cudaRuntimeGetVersion(&versionRuntime) );
  
  // The version is returned as (1000 * major + 10 * minor). 
  major =  versionRuntime / 1000;
  minor = (versionRuntime - major * 1000) / 10;
  std::cout << "Runtime Version = " << major << "." << minor << '\n';
  
  int countDevices = 0;
  CUDA_CHECK( cudaGetDeviceCount(&countDevices) );
  std::cout << "Device Count    = " << countDevices << '\n';

  for (int i = 0; i < countDevices; ++i)
  {
    cudaDeviceProp properties;

    CUDA_CHECK( cudaGetDeviceProperties(&properties, i) );

    m_deviceProperties.push_back(properties);
    
    std::cout << "Device " << i << ": " << properties.name << '\n';
#if 1 // Condensed information    
    std::cout << "  SM " << properties.major << "." << properties.minor << '\n';
    std::cout << "  Total Mem = " << properties.totalGlobalMem << '\n';
    std::cout << "  ClockRate [kHz] = " << properties.clockRate << '\n';
    std::cout << "  MaxThreadsPerBlock = " << properties.maxThreadsPerBlock << '\n';
    std::cout << "  SM Count = " << properties.multiProcessorCount << '\n';
    std::cout << "  Timeout Enabled = " << properties.kernelExecTimeoutEnabled << '\n';
    std::cout << "  TCC Driver = " << properties.tccDriver << '\n';
#else // Dump every property.
    //std::cout << "name[256] = " << properties.name << '\n';
    std::cout << "uuid = " << properties.uuid.bytes << '\n';
    std::cout << "totalGlobalMem = " << properties.totalGlobalMem << '\n';
    std::cout << "sharedMemPerBlock = " << properties.sharedMemPerBlock << '\n';
    std::cout << "regsPerBlock = " << properties.regsPerBlock << '\n';
    std::cout << "warpSize = " << properties.warpSize << '\n';
    std::cout << "memPitch = " << properties.memPitch << '\n';
    std::cout << "maxThreadsPerBlock = " << properties.maxThreadsPerBlock << '\n';
    std::cout << "maxThreadsDim[3] = " << properties.maxThreadsDim[0] << ", " << properties.maxThreadsDim[1] << ", " << properties.maxThreadsDim[0] << '\n';
    std::cout << "maxGridSize[3] = " << properties.maxGridSize[0] << ", " << properties.maxGridSize[1] << ", " << properties.maxGridSize[2] << '\n';
    std::cout << "clockRate = " << properties.clockRate << '\n';
    std::cout << "totalConstMem = " << properties.totalConstMem << '\n';
    std::cout << "major = " << properties.major << '\n';
    std::cout << "minor = " << properties.minor << '\n';
    std::cout << "textureAlignment = " << properties.textureAlignment << '\n';
    std::cout << "texturePitchAlignment = " << properties.texturePitchAlignment << '\n';
    std::cout << "deviceOverlap = " << properties.deviceOverlap << '\n';
    std::cout << "multiProcessorCount = " << properties.multiProcessorCount << '\n';
    std::cout << "kernelExecTimeoutEnabled = " << properties.kernelExecTimeoutEnabled << '\n';
    std::cout << "integrated = " << properties.integrated << '\n';
    std::cout << "canMapHostMemory = " << properties.canMapHostMemory << '\n';
    std::cout << "computeMode = " << properties.computeMode << '\n';
    std::cout << "maxTexture1D = " << properties.maxTexture1D << '\n';
    std::cout << "maxTexture1DMipmap = " << properties.maxTexture1DMipmap << '\n';
    std::cout << "maxTexture1DLinear = " << properties.maxTexture1DLinear << '\n';
    std::cout << "maxTexture2D[2] = " << properties.maxTexture2D[0] << ", " << properties.maxTexture2D[1] << '\n';
    std::cout << "maxTexture2DMipmap[2] = " << properties.maxTexture2DMipmap[0] << ", " << properties.maxTexture2DMipmap[1] << '\n';
    std::cout << "maxTexture2DLinear[3] = " << properties.maxTexture2DLinear[0] << ", " << properties.maxTexture2DLinear[1] << ", " << properties.maxTexture2DLinear[2] << '\n';
    std::cout << "maxTexture2DGather[2] = " << properties.maxTexture2DGather[0] << ", " << properties.maxTexture2DGather[1] << '\n';
    std::cout << "maxTexture3D[3] = " << properties.maxTexture3D[0] << ", " << properties.maxTexture3D[1] << ", " << properties.maxTexture3D[2] << '\n';
    std::cout << "maxTexture3DAlt[3] = " << properties.maxTexture3DAlt[0] << ", " << properties.maxTexture3DAlt[1] << ", " << properties.maxTexture3DAlt[2] << '\n';
    std::cout << "maxTextureCubemap = " << properties.maxTextureCubemap << '\n';
    std::cout << "maxTexture1DLayered[2] = " << properties.maxTexture1DLayered[0] << ", " << properties.maxTexture1DLayered[1] << '\n';
    std::cout << "maxTexture2DLayered[3] = " << properties.maxTexture2DLayered[0] << ", " << properties.maxTexture2DLayered[1] << ", " << properties.maxTexture2DLayered[2] << '\n';
    std::cout << "maxTextureCubemapLayered[2] = " << properties.maxTextureCubemapLayered[0] << ", " << properties.maxTextureCubemapLayered[1] << '\n';
    std::cout << "maxSurface1D = " << properties.maxSurface1D << '\n';
    std::cout << "maxSurface2D[2] = " << properties.maxSurface2D[0] << ", " << properties.maxSurface2D[1] << '\n';
    std::cout << "maxSurface3D[3] = " << properties.maxSurface3D[0] << ", " << properties.maxSurface3D[1] << ", " << properties.maxSurface3D[2] << '\n';
    std::cout << "maxSurface1DLayered[2] = " << properties.maxSurface1DLayered[0] << ", " << properties.maxSurface1DLayered[1] << '\n';
    std::cout << "maxSurface2DLayered[3] = " << properties.maxSurface2DLayered[0] << ", " << properties.maxSurface2DLayered[1] << ", " << properties.maxSurface2DLayered[2] << '\n';
    std::cout << "maxSurfaceCubemap = " << properties.maxSurfaceCubemap << '\n';
    std::cout << "maxSurfaceCubemapLayered[2] = " << properties.maxSurfaceCubemapLayered[0] << ", " << properties.maxSurfaceCubemapLayered[1] << '\n';
    std::cout << "surfaceAlignment = " << properties.surfaceAlignment << '\n';
    std::cout << "concurrentKernels = " << properties.concurrentKernels << '\n';
    std::cout << "ECCEnabled = " << properties.ECCEnabled << '\n';
    std::cout << "pciBusID = " << properties.pciBusID << '\n';
    std::cout << "pciDeviceID = " << properties.pciDeviceID << '\n';
    std::cout << "pciDomainID = " << properties.pciDomainID << '\n';
    std::cout << "tccDriver = " << properties.tccDriver << '\n';
    std::cout << "asyncEngineCount = " << properties.asyncEngineCount << '\n';
    std::cout << "unifiedAddressing = " << properties.unifiedAddressing << '\n';
    std::cout << "memoryClockRate = " << properties.memoryClockRate << '\n';
    std::cout << "memoryBusWidth = " << properties.memoryBusWidth << '\n';
    std::cout << "l2CacheSize = " << properties.l2CacheSize << '\n';
    std::cout << "maxThreadsPerMultiProcessor = " << properties.maxThreadsPerMultiProcessor << '\n';
    std::cout << "streamPrioritiesSupported = " << properties.streamPrioritiesSupported << '\n';
    std::cout << "globalL1CacheSupported = " << properties.globalL1CacheSupported << '\n';
    std::cout << "localL1CacheSupported = " << properties.localL1CacheSupported << '\n';
    std::cout << "sharedMemPerMultiprocessor = " << properties.sharedMemPerMultiprocessor << '\n';
    std::cout << "regsPerMultiprocessor = " << properties.regsPerMultiprocessor << '\n';
    std::cout << "managedMemory = " << properties.managedMemory << '\n';
    std::cout << "isMultiGpuBoard = " << properties.isMultiGpuBoard << '\n';
    std::cout << "multiGpuBoardGroupID = " << properties.multiGpuBoardGroupID << '\n';
    std::cout << "singleToDoublePrecisionPerfRatio = " << properties.singleToDoublePrecisionPerfRatio << '\n';
    std::cout << "pageableMemoryAccess = " << properties.pageableMemoryAccess << '\n';
    std::cout << "concurrentManagedAccess = " << properties.concurrentManagedAccess << '\n';
    std::cout << "computePreemptionSupported = " << properties.computePreemptionSupported << '\n';
    std::cout << "canUseHostPointerForRegisteredMem = " << properties.canUseHostPointerForRegisteredMem << '\n';
    std::cout << "cooperativeLaunch = " << properties.cooperativeLaunch << '\n';
    std::cout << "cooperativeMultiDeviceLaunch = " << properties.cooperativeMultiDeviceLaunch << '\n';
    std::cout << "pageableMemoryAccessUsesHostPageTables = " << properties.pageableMemoryAccessUsesHostPageTables << '\n';
    std::cout << "directManagedMemAccessFromHost = " << properties.directManagedMemAccessFromHost << '\n';
#endif
  }
}

void Application::initOpenGL()
{
  glClearColor(0.0f, 0.0f, 0.0f, 0.0f);

  glViewport(0, 0, m_width, m_height);

  glMatrixMode(GL_PROJECTION);
  glLoadIdentity();

  glMatrixMode(GL_MODELVIEW);
  glLoadIdentity();

  // glPixelStorei(GL_UNPACK_ALIGNMENT, 4); // default, works for BGRA8, RGBA16F, and RGBA32F.

  glDisable(GL_CULL_FACE);  // default
  glDisable(GL_DEPTH_TEST); // default

  if (m_interop)
  {
    // PBO for CUDA-OpenGL interop.
    glGenBuffers(1, &m_pbo);
    MY_ASSERT(m_pbo != 0); 

    // Buffer size must be > 0 or OptiX can't create a buffer from it.
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, m_pbo);
    glBufferData(GL_PIXEL_UNPACK_BUFFER, m_width * m_height * sizeof(float) * 4, (void*) 0, GL_DYNAMIC_DRAW); // RGBA32F from byte offset 0 in the pixel unpack buffer.
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
  }

  glGenTextures(1, &m_hdrTexture);
  MY_ASSERT(m_hdrTexture != 0);

  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, m_hdrTexture);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

  glBindTexture(GL_TEXTURE_2D, 0);

  // DAR Local ImGui code has been changed to push the GL_TEXTURE_BIT so that this works. 
  glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);

  // GLSL shaders objects and program. 
  m_glslVS      = 0;
  m_glslFS      = 0;
  m_glslProgram = 0;

  m_positionLocation   = -1;
  m_texCoordLocation   = -1;

  initGLSL();

  // Two hardcoded triangles in the identity matrix pojection coordinate system with 2D texture coordinates.
  const float attributes[16] = 
  {
    // vertex2f,   texcoord2f
    -1.0f, -1.0f,  0.0f, 0.0f,
     1.0f, -1.0f,  1.0f, 0.0f,
     1.0f,  1.0f,  1.0f, 1.0f,
    -1.0f,  1.0f,  0.0f, 1.0f
  };

  unsigned int indices[6] = 
  {
    0, 1, 2, 
    2, 3, 0
  };

  glGenBuffers(1, &m_vboAttributes);
  MY_ASSERT(m_vboAttributes != 0);

  glGenBuffers(1, &m_vboIndices);
  MY_ASSERT(m_vboIndices != 0);

  // Setup the vertex arrays from the interleaved vertex attributes.
  glBindBuffer(GL_ARRAY_BUFFER, m_vboAttributes);
  glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr) sizeof(float) * 16, (GLvoid const*) attributes, GL_STATIC_DRAW);

  glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m_vboIndices);
  glBufferData(GL_ELEMENT_ARRAY_BUFFER, (GLsizeiptr) sizeof(unsigned int) * 6, (const GLvoid*) indices, GL_STATIC_DRAW);

  glVertexAttribPointer(m_positionLocation, 2, GL_FLOAT, GL_FALSE, sizeof(float) * 4, (GLvoid*) 0);
  //glEnableVertexAttribArray(m_positionLocation);

  glVertexAttribPointer(m_texCoordLocation, 2, GL_FLOAT, GL_FALSE, sizeof(float) * 4, (GLvoid*) (sizeof(float) * 2));
  //glEnableVertexAttribArray(m_texCoordLocation);
}


OptixResult Application::initOptiXFunctionTable()
{
#ifdef _WIN32
  void* handle = optixLoadWindowsDll();
  if (!handle)
  {
    return OPTIX_ERROR_LIBRARY_NOT_FOUND;
  }

  void* symbol = reinterpret_cast<void*>(GetProcAddress((HMODULE) handle, "optixQueryFunctionTable"));
  if (!symbol)
  {
    return OPTIX_ERROR_ENTRY_SYMBOL_NOT_FOUND;
  }
#else
  void* handle = dlopen("libnvoptix.so.1", RTLD_NOW);
  if (!handle)
  {
    return OPTIX_ERROR_LIBRARY_NOT_FOUND;
  }

  void* symbol = dlsym(handle, "optixQueryFunctionTable");
  if (!symbol)
  {
    return OPTIX_ERROR_ENTRY_SYMBOL_NOT_FOUND;
  }
#endif

  OptixQueryFunctionTable_t* optixQueryFunctionTable = reinterpret_cast<OptixQueryFunctionTable_t*>(symbol);

  return optixQueryFunctionTable(OPTIX_ABI_VERSION, 0, 0, 0, &m_api, sizeof(OptixFunctionTable));
}


bool Application::initOptiX()
{
  //getSystemInformation(); // This optionally dumps system information.

  cudaError_t cuErr = cudaFree(0); // Creates a CUDA context.
  if (cuErr != cudaSuccess)
  {
    std::cerr << "ERROR: initOptiX() cudaFree(0) failed: " << cuErr << '\n';
    return false;
  }

  CUresult cuRes = cuCtxGetCurrent(&m_cudaContext);
  if (cuRes != CUDA_SUCCESS)
  {
    std::cerr << "ERROR: initOptiX() cuCtxGetCurrent() failed: " << cuRes << '\n';
    return false;
  }

  cuErr = cudaStreamCreate(&m_cudaStream);
  if (cuErr != cudaSuccess)
  {
    std::cerr << "ERROR: initOptiX() cudaStreamCreate() failed: " << cuErr << '\n';
    return false;
  }

  OptixResult res = initOptiXFunctionTable();
  if (res != OPTIX_SUCCESS)
  {
    std::cerr << "ERROR: initOptiX() initOptiXFunctionTable() failed: " << res << '\n';
    return false;
  }

  OptixDeviceContextOptions options = {};

  options.logCallbackFunction = &Logger::callback;
  options.logCallbackData     = &m_logger;
  options.logCallbackLevel    = 3; // Keep at warning level to suppress the disk cache messages.

  res = m_api.optixDeviceContextCreate(m_cudaContext, &options, &m_context);
  if (res != OPTIX_SUCCESS)
  {
    std::cerr << "ERROR: initOptiX() optixDeviceContextCreate() failed: " << res << '\n';
    return false;
  }

  initRenderer(); // Initialize all the rest.

  return true;
}


void Application::restartAccumulation()
{
  m_iterationIndex  = 0;
  m_presentNext     = true;
  m_presentAtSecond = 1.0;

  CUDA_CHECK( cudaStreamSynchronize(m_cudaStream) );
  CUDA_CHECK( cudaMemcpy((void*) m_d_systemParameter, &m_systemParameter, sizeof(SystemParameter), cudaMemcpyHostToDevice) );

  m_timer.restart();
}


bool Application::render()
{
  bool repaint = false;

  bool cameraChanged = m_pinholeCamera.getFrustum(m_systemParameter.cameraPosition,
                                                  m_systemParameter.cameraU,
                                                  m_systemParameter.cameraV,
                                                  m_systemParameter.cameraW);
  if (cameraChanged)
  {
    restartAccumulation();
  }
  
  // Continue manual accumulation rendering if there is no limit (m_frames == 0) or the number of frames has not been reached.
  if (0 == m_frames || m_iterationIndex < m_frames)
  {
    // Update only the sysParameter.iterationIndex.
    m_systemParameter.iterationIndex = m_iterationIndex++;

    CUDA_CHECK( cudaMemcpy((void*) &m_d_systemParameter->iterationIndex, &m_systemParameter.iterationIndex, sizeof(int), cudaMemcpyHostToDevice) );

    if (m_interop)
    {
      size_t size;

      CUDA_CHECK( cudaGraphicsMapResources(1, &m_cudaGraphicsResource, m_cudaStream) );
      CUDA_CHECK( cudaGraphicsResourceGetMappedPointer((void**) &m_systemParameter.outputBuffer, &size, m_cudaGraphicsResource) ); // The pointer can change on every map!
      CUDA_CHECK( cudaMemcpy((void*) &m_d_systemParameter->outputBuffer, &m_systemParameter.outputBuffer, sizeof(void*), cudaMemcpyHostToDevice) );

      OPTIX_CHECK( m_api.optixLaunch(m_pipeline, m_cudaStream, (CUdeviceptr) m_d_systemParameter, sizeof(SystemParameter), &m_sbt, m_width, m_height, 1) );
      
      CUDA_CHECK( cudaGraphicsUnmapResources(1, &m_cudaGraphicsResource, m_cudaStream) );
    }
    else
    {
      OPTIX_CHECK( m_api.optixLaunch(m_pipeline, m_cudaStream, (CUdeviceptr) m_d_systemParameter, sizeof(SystemParameter), &m_sbt, m_width, m_height, 1) );
    }
  }

  // Only update the texture when a restart happened or one second passed to reduce required bandwidth.
  if (m_presentNext)
  {
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, m_hdrTexture); // Manual accumulation always renders into the m_hdrTexture.

    if (m_interop)
    {
      glBindBuffer(GL_PIXEL_UNPACK_BUFFER, m_pbo);
      glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F, (GLsizei) m_width, (GLsizei) m_height, 0, GL_RGBA, GL_FLOAT, (void*) 0); // RGBA32F from byte offset 0 in the pixel unpack buffer.
    }
    else
    {
      CUDA_CHECK( cudaMemcpy((void*) m_outputBuffer, m_systemParameter.outputBuffer, sizeof(float4) * m_width * m_height , cudaMemcpyDeviceToHost) );
      glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F, (GLsizei) m_width, (GLsizei) m_height, 0, GL_RGBA, GL_FLOAT, m_outputBuffer); // RGBA32F
    }

    repaint = true; // Indicate that there is a new image.

    m_presentNext = m_present;
  }

  double seconds = m_timer.getTime();
#if 1
  // Show the accumulation of the first half second to get some refinement after interaction.
  if (seconds < 0.5)
  {
    m_presentAtSecond = 1.0;
    m_presentNext     = true;
  }
  else 
#endif
  if (m_presentAtSecond < seconds)
  {
    m_presentAtSecond = ceil(seconds);
      
    const double fps = double(m_iterationIndex) / seconds;

    std::ostringstream stream; 
    stream.precision(3); // Precision is # digits in fraction part.
    // m_iterationIndex has already been incremented for the last rendered frame, so it is the actual framecount here.
    stream << std::fixed << m_iterationIndex << " / " << seconds << " = " << fps << " fps";
    std::cout << stream.str() << '\n';

    m_presentNext = true; // Present at least every second.
  }
  
  return repaint;
}

void Application::display()
{
  glBindBuffer(GL_ARRAY_BUFFER, m_vboAttributes);
  glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m_vboIndices);

  glEnableVertexAttribArray(m_positionLocation);
  glEnableVertexAttribArray(m_texCoordLocation);

  glUseProgram(m_glslProgram);

  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, m_hdrTexture);

  glDrawElements(GL_TRIANGLES, (GLsizei) 6, GL_UNSIGNED_INT, (const GLvoid*) 0);

  glUseProgram(0);

  glDisableVertexAttribArray(m_positionLocation);
  glDisableVertexAttribArray(m_texCoordLocation);
}


void Application::checkInfoLog(const char *msg, GLuint object)
{
  GLint  maxLength;
  GLint  length;
  GLchar *infoLog;

  if (glIsProgram(object))
  {
    glGetProgramiv(object, GL_INFO_LOG_LENGTH, &maxLength);
  }
  else
  {
    glGetShaderiv(object, GL_INFO_LOG_LENGTH, &maxLength);
  }
  if (maxLength > 1) 
  {
    infoLog = (GLchar *) malloc(maxLength);
    if (infoLog != NULL)
    {
      if (glIsShader(object))
      {
        glGetShaderInfoLog(object, maxLength, &length, infoLog);
      }
      else
      {
        glGetProgramInfoLog(object, maxLength, &length, infoLog);
      }
      //fprintf(fileLog, "-- tried to compile (len=%d): %s\n", (unsigned int)strlen(msg), msg);
      //fprintf(fileLog, "--- info log contents (len=%d) ---\n", (int) maxLength);
      //fprintf(fileLog, "%s", infoLog);
      //fprintf(fileLog, "--- end ---\n");
      std::cout << infoLog << '\n';
      // Look at the info log string here...
      free(infoLog);
    }
  }
}

void Application::initGLSL()
{
  static const std::string vsSource =
    "#version 330\n"
    "layout(location = 0) in vec2 attrPosition;\n"
    "layout(location = 1) in vec2 attrTexCoord;\n"
    "out vec2 varTexCoord;\n"
    "void main()\n"
    "{\n"
    "  gl_Position = vec4(attrPosition, 0.0, 1.0);\n"
    "  varTexCoord = attrTexCoord;\n"
    "}\n";

  static const std::string fsSource =
    "#version 330\n"
    "uniform sampler2D samplerHDR;\n"
    "uniform vec3  colorBalance;\n"
    "uniform float invWhitePoint;\n"
    "uniform float burnHighlights;\n"
    "uniform float saturation;\n"
    "uniform float crushBlacks;\n"
    "uniform float invGamma;\n"
    "in vec2 varTexCoord;\n"
    "layout(location = 0, index = 0) out vec4 outColor;\n"
    "void main()\n"
    "{\n"
    "  vec3 hdrColor = texture(samplerHDR, varTexCoord).rgb;\n"
    "  vec3 ldrColor = invWhitePoint * colorBalance * hdrColor;\n"
    "  ldrColor *= (ldrColor * burnHighlights + 1.0) / (ldrColor + 1.0);\n"
    "  float luminance = dot(ldrColor, vec3(0.3, 0.59, 0.11));\n"
    "  ldrColor = max(mix(vec3(luminance), ldrColor, saturation), 0.0);\n"
    "  luminance = dot(ldrColor, vec3(0.3, 0.59, 0.11));\n"
    "  if (luminance < 1.0)\n"
    "  {\n"
    "    ldrColor = max(mix(pow(ldrColor, vec3(crushBlacks)), ldrColor, sqrt(luminance)), 0.0);\n"
    "  }\n"
    "  ldrColor = pow(ldrColor, vec3(invGamma));\n"
    "  outColor = vec4(ldrColor, 1.0);\n"
    "}\n";

  GLint vsCompiled = 0;
  GLint fsCompiled = 0;
    
  m_glslVS = glCreateShader(GL_VERTEX_SHADER);
  if (m_glslVS)
  {
    GLsizei len = (GLsizei) vsSource.size();
    const GLchar *vs = vsSource.c_str();
    glShaderSource(m_glslVS, 1, &vs, &len);
    glCompileShader(m_glslVS);
    checkInfoLog(vs, m_glslVS);

    glGetShaderiv(m_glslVS, GL_COMPILE_STATUS, &vsCompiled);
    MY_ASSERT(vsCompiled);
  }

  m_glslFS = glCreateShader(GL_FRAGMENT_SHADER);
  if (m_glslFS)
  {
    GLsizei len = (GLsizei) fsSource.size();
    const GLchar *fs = fsSource.c_str();
    glShaderSource(m_glslFS, 1, &fs, &len);
    glCompileShader(m_glslFS);
    checkInfoLog(fs, m_glslFS);

    glGetShaderiv(m_glslFS, GL_COMPILE_STATUS, &fsCompiled);
    MY_ASSERT(fsCompiled);
  }

  m_glslProgram = glCreateProgram();
  if (m_glslProgram)
  {
    GLint programLinked = 0;

    if (m_glslVS && vsCompiled)
    {
      glAttachShader(m_glslProgram, m_glslVS);
    }
    if (m_glslFS && fsCompiled)
    {
      glAttachShader(m_glslProgram, m_glslFS);
    }

    glLinkProgram(m_glslProgram);
    checkInfoLog("m_glslProgram", m_glslProgram);

    glGetProgramiv(m_glslProgram, GL_LINK_STATUS, &programLinked);
    MY_ASSERT(programLinked);

    if (programLinked)
    {
      glUseProgram(m_glslProgram);

      m_positionLocation = glGetAttribLocation(m_glslProgram, "attrPosition");
      MY_ASSERT(m_positionLocation != -1);

      m_texCoordLocation = glGetAttribLocation(m_glslProgram, "attrTexCoord");
      MY_ASSERT(m_texCoordLocation != -1);
      
      glUniform1i(glGetUniformLocation(m_glslProgram, "samplerHDR"), 0); // Always using texture image unit 0 for the display texture.
      glUniform1f(glGetUniformLocation(m_glslProgram, "invGamma"), 1.0f / m_gamma);
      glUniform3f(glGetUniformLocation(m_glslProgram, "colorBalance"), m_colorBalance.x, m_colorBalance.y, m_colorBalance.z);
      glUniform1f(glGetUniformLocation(m_glslProgram, "invWhitePoint"), m_brightness / m_whitePoint);
      glUniform1f(glGetUniformLocation(m_glslProgram, "burnHighlights"), m_burnHighlights);
      glUniform1f(glGetUniformLocation(m_glslProgram, "crushBlacks"), m_crushBlacks + m_crushBlacks + 1.0f);
      glUniform1f(glGetUniformLocation(m_glslProgram, "saturation"), m_saturation);

      glUseProgram(0);
    }
  }
}


void Application::guiWindow()
{
  if (!m_isVisibleGUI) // Use SPACE to toggle the display of the GUI window.
  {
    return;
  }

  ImGui::SetNextWindowSize(ImVec2(200, 200), ImGuiSetCond_FirstUseEver);

  ImGuiWindowFlags window_flags = 0;
  if (!ImGui::Begin("intro_runtime", nullptr, window_flags)) // No bool flag to omit the close button.
  {
    // Early out if the window is collapsed, as an optimization.
    ImGui::End();
    return;
  }

  ImGui::PushItemWidth(-110); // Right-aligned, keep pixels for the labels.

  if (ImGui::CollapsingHeader("System"))
  {
    if (ImGui::Checkbox("Present", &m_present))
    {
      // No action needed, happens automatically on next frame.
    }
    if (ImGui::Combo("Camera", (int*) &m_systemParameter.cameraType, "Pinhole\0Fisheye\0Spherical\0\0"))
    {
      restartAccumulation();
    }
    if (ImGui::DragInt("Min Path Length", &m_systemParameter.pathLengths.x, 1.0f, 0, 100))
    {
      restartAccumulation();
    }
    if (ImGui::DragInt("Max Path Length", &m_systemParameter.pathLengths.y, 1.0f, 0, 100))
    {
      restartAccumulation();
    }
    if (ImGui::DragFloat("Scene Epsilon", &m_sceneEpsilonFactor, 1.0f, 0.0f, 10000.0f))
    {
      m_systemParameter.sceneEpsilon = m_sceneEpsilonFactor * SCENE_EPSILON_SCALE;
      restartAccumulation();
    }
    if (ImGui::DragFloat("Env Rotation", &m_systemParameter.envRotation, 0.001f, 0.0f, 1.0f))
    {
      restartAccumulation();
    }
    if (ImGui::DragInt("Frames", &m_frames, 1.0f, 0, 10000))
    {
      if (m_frames != 0 && m_frames < m_iterationIndex) // If we already rendered more frames, start again.
      {
        restartAccumulation();
      }
    }
    if (ImGui::DragFloat("Mouse Ratio", &m_mouseSpeedRatio, 0.1f, 0.1f, 1000.0f, "%.1f"))
    {
      m_pinholeCamera.setSpeedRatio(m_mouseSpeedRatio);
    }
  }
  if (ImGui::CollapsingHeader("Tonemapper"))
  {
    if (ImGui::ColorEdit3("Balance", (float*) &m_colorBalance))
    {
      glUseProgram(m_glslProgram);
      glUniform3f(glGetUniformLocation(m_glslProgram, "colorBalance"), m_colorBalance.x, m_colorBalance.y, m_colorBalance.z);
      glUseProgram(0);
    }
    if (ImGui::DragFloat("Gamma", &m_gamma, 0.01f, 0.01f, 10.0f)) // Must not get 0.0f
    {
      glUseProgram(m_glslProgram);
      glUniform1f(glGetUniformLocation(m_glslProgram, "invGamma"), 1.0f / m_gamma);
      glUseProgram(0);
    }
    if (ImGui::DragFloat("White Point", &m_whitePoint, 0.01f, 0.01f, 255.0f, "%.2f", 2.0f)) // Must not get 0.0f
    {
      glUseProgram(m_glslProgram);
      glUniform1f(glGetUniformLocation(m_glslProgram, "invWhitePoint"), m_brightness / m_whitePoint);
      glUseProgram(0);
    }
    if (ImGui::DragFloat("Burn Lights", &m_burnHighlights, 0.01f, 0.0f, 10.0f, "%.2f"))
    {
      glUseProgram(m_glslProgram);
      glUniform1f(glGetUniformLocation(m_glslProgram, "burnHighlights"), m_burnHighlights);
      glUseProgram(0);
    }
    if (ImGui::DragFloat("Crush Blacks", &m_crushBlacks, 0.01f, 0.0f, 1.0f, "%.2f"))
    {
      glUseProgram(m_glslProgram);
      glUniform1f(glGetUniformLocation(m_glslProgram, "crushBlacks"),  m_crushBlacks + m_crushBlacks + 1.0f);
      glUseProgram(0);
    }
    if (ImGui::DragFloat("Saturation", &m_saturation, 0.01f, 0.0f, 10.0f, "%.2f"))
    {
      glUseProgram(m_glslProgram);
      glUniform1f(glGetUniformLocation(m_glslProgram, "saturation"), m_saturation);
      glUseProgram(0);
    }
    if (ImGui::DragFloat("Brightness", &m_brightness, 0.01f, 0.0f, 100.0f, "%.2f", 2.0f))
    {
      glUseProgram(m_glslProgram);
      glUniform1f(glGetUniformLocation(m_glslProgram, "invWhitePoint"), m_brightness / m_whitePoint);
      glUseProgram(0);
    }
  }
  if (ImGui::CollapsingHeader("Materials"))
  {
    bool changed = false;

    // HACK The last material is a black specular reflection for the area light and not editable
    // because this example does not support explicit light sampling of textured or cutout opacity geometry.
    for (int i = 0; i < int(m_guiMaterialParameters.size()) - 1; ++i)
    {
      if (ImGui::TreeNode((void*)(intptr_t) i, "Material %d", i))
      {
        MaterialParameterGUI& parameters = m_guiMaterialParameters[i];

        if (ImGui::Combo("BSDF Type", (int*) &parameters.indexBSDF,
                         "Diffuse Reflection\0Specular Reflection\0Specular Reflection Transmission\0\0"))
        {
          changed = true;
        }
        if (ImGui::ColorEdit3("Albedo", (float*) &parameters.albedo))
        {
          changed = true;
        }
        if (ImGui::Checkbox("Use Albedo Texture", &parameters.useAlbedoTexture))
        {
          changed = true;
        }
        if (ImGui::Checkbox("Use Cutout Texture", &parameters.useCutoutTexture))
        {
          // This chnages the hit group in the Shader Binding Table between opaque and cutout. (Opaque renders faster.)
          updateShaderBindingTable(i);
          changed = true; // This triggers the sysParameter.textureCutout object ID update.
        }
        if (ImGui::Checkbox("Thin-Walled", &parameters.thinwalled)) // Set this to true when using cutout opacity. Refracting materials won't look right with cutouts otherwise.
        {
          changed = true;
        }	
        // Only show material parameters for the BSDFs which are affected.
        if (parameters.indexBSDF == INDEX_BSDF_SPECULAR_REFLECTION_TRANSMISSION)
        {
          if (ImGui::ColorEdit3("Absorption", (float*) &parameters.absorptionColor))
          {
            changed = true;
          }
          if (ImGui::DragFloat("Volume Scale", &parameters.volumeDistanceScale, 0.01f, 0.0f, 100.0f, "%.2f"))
          {
            changed = true;
          }
          if (ImGui::DragFloat("IOR", &parameters.ior, 0.01f, 0.0f, 10.0f, "%.2f"))
          {
            changed = true;
          }
        }
        ImGui::TreePop();
      }
    }
    
    if (changed) // If any of the material parameters changed, simply upload them to the sysMaterialParameters again.
    {
      updateMaterialParameters();
      restartAccumulation();
    }
  }
  if (ImGui::CollapsingHeader("Lights"))
  {
    bool changed = false;
    
    for (int i = 0; i < int(m_lightDefinitions.size()); ++i)
    {
      LightDefinition& light = m_lightDefinitions[i];

      // Allow to change the emission (radiant exitance in Watt/m^2) of the rectangle lights in the scene.
      if (light.type == LIGHT_PARALLELOGRAM)
      {
        if (ImGui::TreeNode((void*)(intptr_t) i, "Light %d", i))
        {
          if (ImGui::DragFloat3("Emission", (float*) &light.emission, 1.0f, 0.0f, 10000.0f, "%.0f"))
          {
            changed = true;
          }
          ImGui::TreePop();
        }
      }
    }
    if (changed) // If any of the light parameters changed, simply upload them to the sysMaterialParameters again.
    {
      CUDA_CHECK( cudaStreamSynchronize(m_cudaStream) );
      CUDA_CHECK( cudaMemcpy((void*) m_systemParameter.lightDefinitions, m_lightDefinitions.data(), sizeof(LightDefinition) * m_lightDefinitions.size(), cudaMemcpyHostToDevice) );

      restartAccumulation();
    }
  }

  ImGui::PopItemWidth();

  ImGui::End();
}

void Application::guiEventHandler()
{
  ImGuiIO const& io = ImGui::GetIO();

  if (ImGui::IsKeyPressed(' ', false)) // Toggle the GUI window display with SPACE key.
  {
    m_isVisibleGUI = !m_isVisibleGUI;
  }

  const ImVec2 mousePosition = ImGui::GetMousePos(); // Mouse coordinate window client rect.
  const int x = int(mousePosition.x);
  const int y = int(mousePosition.y);

  switch (m_guiState)
  {
    case GUI_STATE_NONE:
      if (!io.WantCaptureMouse) // Only allow camera interactions to begin when not interacting with the GUI.
      {
        if (ImGui::IsMouseDown(0)) // LMB down event?
        {
          m_pinholeCamera.setBaseCoordinates(x, y);
          m_guiState = GUI_STATE_ORBIT;
        }
        else if (ImGui::IsMouseDown(1)) // RMB down event?
        {
          m_pinholeCamera.setBaseCoordinates(x, y);
          m_guiState = GUI_STATE_DOLLY;
        }
        else if (ImGui::IsMouseDown(2)) // MMB down event?
        {
          m_pinholeCamera.setBaseCoordinates(x, y);
          m_guiState = GUI_STATE_PAN;
        }
        else if (io.MouseWheel != 0.0f) // Mouse wheel zoom.
        {
          m_pinholeCamera.zoom(io.MouseWheel);
        }
      }
      break;

    case GUI_STATE_ORBIT:
      if (ImGui::IsMouseReleased(0)) // LMB released? End of orbit mode.
      {
        m_guiState = GUI_STATE_NONE;
      }
      else
      {
        m_pinholeCamera.orbit(x, y);
      }
      break;

    case GUI_STATE_DOLLY:
      if (ImGui::IsMouseReleased(1)) // RMB released? End of dolly mode.
      {
        m_guiState = GUI_STATE_NONE;
      }
      else
      {
        m_pinholeCamera.dolly(x, y);
      }
      break;

    case GUI_STATE_PAN:
      if (ImGui::IsMouseReleased(2)) // MMB released? End of pan mode.
      {
        m_guiState = GUI_STATE_NONE;
      }
      else
      {
        m_pinholeCamera.pan(x, y);
      }
      break;
  }
}


// This part is always identical in the generated geometry creation routines.
OptixTraversableHandle Application::createGeometry(std::vector<VertexAttributes> const& attributes, std::vector<unsigned int> const& indices)
{
  CUdeviceptr d_attributes;
  CUdeviceptr d_indices;

  const size_t attributesSizeInBytes = sizeof(VertexAttributes) * attributes.size();

  CUDA_CHECK( cudaMalloc((void**) &d_attributes, attributesSizeInBytes) );
  CUDA_CHECK( cudaMemcpy((void*) d_attributes, attributes.data(), attributesSizeInBytes, cudaMemcpyHostToDevice) );

  const size_t indicesSizeInBytes = sizeof(unsigned int) * indices.size();

  CUDA_CHECK( cudaMalloc((void**) &d_indices, indicesSizeInBytes) );
  CUDA_CHECK( cudaMemcpy((void*) d_indices, indices.data(), indicesSizeInBytes, cudaMemcpyHostToDevice) );

  OptixBuildInput triangleInput = {};

  triangleInput.type = OPTIX_BUILD_INPUT_TYPE_TRIANGLES;

  triangleInput.triangleArray.vertexFormat        = OPTIX_VERTEX_FORMAT_FLOAT3;
  triangleInput.triangleArray.vertexStrideInBytes = sizeof(VertexAttributes);
  triangleInput.triangleArray.numVertices         = (unsigned int) attributes.size();
  triangleInput.triangleArray.vertexBuffers       = &d_attributes;

  triangleInput.triangleArray.indexFormat        = OPTIX_INDICES_FORMAT_UNSIGNED_INT3;
  triangleInput.triangleArray.indexStrideInBytes = sizeof(unsigned int) * 3;

  triangleInput.triangleArray.numIndexTriplets   = (unsigned int) indices.size() / 3;
  triangleInput.triangleArray.indexBuffer        = d_indices;

  unsigned int triangleInputFlags[1] = { OPTIX_GEOMETRY_FLAG_NONE };

  triangleInput.triangleArray.flags         = triangleInputFlags;
  triangleInput.triangleArray.numSbtRecords = 1;

  OptixAccelBuildOptions accelBuildOptions = {};

  accelBuildOptions.buildFlags = OPTIX_BUILD_FLAG_NONE;
  accelBuildOptions.operation  = OPTIX_BUILD_OPERATION_BUILD;

  OptixAccelBufferSizes accelBufferSizes;
  
  OPTIX_CHECK( m_api.optixAccelComputeMemoryUsage(m_context, &accelBuildOptions, &triangleInput, 1, &accelBufferSizes) );

  CUdeviceptr d_gas; // This holds the geometry acceleration structure.

  CUDA_CHECK( cudaMalloc((void**) &d_gas, accelBufferSizes.outputSizeInBytes) );

  CUdeviceptr d_tmp;

  CUDA_CHECK( cudaMalloc((void**) &d_tmp, accelBufferSizes.tempSizeInBytes) );

  OptixTraversableHandle traversableHandle = 0; // This is the GAS handle which gets returned.

  OPTIX_CHECK( m_api.optixAccelBuild(m_context, m_cudaStream, 
                                     &accelBuildOptions, &triangleInput, 1,
                                     d_tmp, accelBufferSizes.tempSizeInBytes,
                                     d_gas, accelBufferSizes.outputSizeInBytes, 
                                     &traversableHandle, nullptr, 0) );

  CUDA_CHECK( cudaStreamSynchronize(m_cudaStream) );

  CUDA_CHECK( cudaFree((void*) d_tmp) );
  
  // Track the GeometryData to be able to set them in the SBT record GeometryInstanceData and free them on exit.
  GeometryData geometry;

  geometry.indices       = d_indices;
  geometry.attributes    = d_attributes;
  geometry.numIndices    = indices.size();
  geometry.numAttributes = attributes.size();
  geometry.gas           = d_gas;

  m_geometries.push_back(geometry);

  return traversableHandle;
}


std::vector<char> Application::readData(std::string const& filename)
{
  std::ifstream inputData(filename, std::ios::binary);

  if (inputData.fail())
  {
    std::cerr << "ERROR: readData() Failed to open file " << filename << '\n';
    return std::vector<char>();
  }

  // Copy the input buffer to a char vector.
  std::vector<char> data(std::istreambuf_iterator<char>(inputData), {});

  if (inputData.fail())
  {
    std::cerr << "ERROR: readData() Failed to read file " << filename << '\n';
    return std::vector<char>();
  }

  return data;
}


// Convert the GUI material parameters to the device side structure and upload them into the m_systemParameter.materialParameters device pointer.
void Application::updateMaterialParameters()
{
  MY_ASSERT((sizeof(MaterialParameter) & 15) == 0); // Verify float4 alignment.

  std::vector<MaterialParameter> materialParameters(m_guiMaterialParameters.size());

  // PERF This could be made faster for GUI interactions on scenes with very many materials when really only copying the changed values.
  for (size_t i = 0; i < m_guiMaterialParameters.size(); ++i)
  {
    MaterialParameterGUI& src = m_guiMaterialParameters[i]; // GUI layout.
    MaterialParameter&    dst = materialParameters[i];      // Device layout.

    dst.indexBSDF     = src.indexBSDF;
    dst.albedo        = src.albedo;
    dst.textureAlbedo = (src.useAlbedoTexture) ? m_textureAlbedo->getTextureObject() : 0;
    dst.textureCutout = (src.useCutoutTexture) ? m_textureCutout->getTextureObject() : 0;
    dst.flags         = (src.thinwalled) ? FLAG_THINWALLED : 0;
    // Calculate the effective absorption coefficient from the GUI parameters. This is one reason why there are two structures.
    // Prevent logf(0.0f) which results in infinity.
    const float x = (0.0f < src.absorptionColor.x) ? -logf(src.absorptionColor.x) : RT_DEFAULT_MAX;
    const float y = (0.0f < src.absorptionColor.y) ? -logf(src.absorptionColor.y) : RT_DEFAULT_MAX;
    const float z = (0.0f < src.absorptionColor.z) ? -logf(src.absorptionColor.z) : RT_DEFAULT_MAX;
    dst.absorption    = make_float3(x, y, z) * src.volumeDistanceScale;
    dst.ior           = src.ior;
  }

  CUDA_CHECK( cudaStreamSynchronize(m_cudaStream) );
  CUDA_CHECK( cudaMemcpy((void*) m_systemParameter.materialParameters, materialParameters.data(), sizeof(MaterialParameter) * materialParameters.size(), cudaMemcpyHostToDevice) );
}


void Application::initMaterials()
{
  Picture* picture = new Picture;

  unsigned int flags = IMAGE_FLAG_2D;

  const std::string filenameCutout = std::string("./slots_alpha.png");
  if (!picture->load(filenameCutout, flags))
  {
    picture->generateRGBA8(2, 2, 1, flags); // This will not have cutouts though.
  }
  m_textureCutout = new Texture();
  m_textureCutout->create(picture, flags);

  const std::string filenameDiffuse = std::string("./NVIDIA_Logo.jpg");
  if (!picture->load(filenameDiffuse, flags))
  {
    picture->generateRGBA8(2, 2, 1, flags); // 2x2 RGBA8 red-green-blue-yellow failure picture.
  }
  m_textureAlbedo = new Texture();
  m_textureAlbedo->create(picture, flags);

  delete picture;

  // Setup GUI material parameters, one for each of the implemented BSDFs.
  MaterialParameterGUI parameters;

  // The order in this array matches the instance ID in the root IAS!
  // Lambert material for the floor.
  parameters.indexBSDF           = INDEX_BSDF_DIFFUSE_REFLECTION; // Index for the direct callables.
  parameters.albedo              = make_float3(0.5f); // Grey. Modulates the albedo texture.
  parameters.useAlbedoTexture    = true;
  parameters.useCutoutTexture    = false;
  parameters.thinwalled          = false;
  parameters.absorptionColor     = make_float3(1.0f);
  parameters.volumeDistanceScale = 1.0f;
  parameters.ior                 = 1.5f;
  m_guiMaterialParameters.push_back(parameters); // 0

  // Water material for the box.
  parameters.indexBSDF           = INDEX_BSDF_SPECULAR_REFLECTION_TRANSMISSION;
  parameters.albedo              = make_float3(1.0f);
  parameters.useAlbedoTexture    = false;
  parameters.useCutoutTexture    = false;
  parameters.thinwalled          = false;
  parameters.absorptionColor     = make_float3(0.75f, 0.75f, 0.95f); // Blue
  parameters.volumeDistanceScale = 1.0f;
  parameters.ior                 = 1.33f; // Water
  m_guiMaterialParameters.push_back(parameters); // 1

  // Glass material for the sphere inside that box to show nested materials!
  parameters.indexBSDF           = INDEX_BSDF_SPECULAR_REFLECTION_TRANSMISSION;
  parameters.albedo              = make_float3(1.0f);
  parameters.useAlbedoTexture    = false;
  parameters.useCutoutTexture    = false;
  parameters.thinwalled          = false;
  parameters.absorptionColor     = make_float3(0.5f, 0.75f, 0.5f); // Green
  parameters.volumeDistanceScale = 1.0f;
  parameters.ior                 = 1.52f; // Flint glass. Higher IOR than the surrounding box.
  m_guiMaterialParameters.push_back(parameters); // 2

  // Lambert material with cutout opacity.
  parameters.indexBSDF           = INDEX_BSDF_DIFFUSE_REFLECTION;
  parameters.albedo              = make_float3(0.75f);
  parameters.useAlbedoTexture    = false;
  parameters.useCutoutTexture    = true;
  parameters.thinwalled          = true; // Materials with cutout opacity should always be thinwalled.
  parameters.absorptionColor     = make_float3(0.980392f, 0.729412f, 0.470588f);
  parameters.volumeDistanceScale = 1.0f;
  parameters.ior                 = 1.5f; // Glass.
  m_guiMaterialParameters.push_back(parameters); // 3

  // Tinted mirror material.
  parameters.indexBSDF           = INDEX_BSDF_SPECULAR_REFLECTION;
  parameters.albedo              = make_float3(0.462745f, 0.72549f, 0.0f);
  parameters.useAlbedoTexture    = false;
  parameters.useCutoutTexture    = false;
  parameters.thinwalled          = false;
  parameters.absorptionColor     = make_float3(0.9f, 0.8f, 0.8f); // Light red.
  parameters.volumeDistanceScale = 1.0f;
  parameters.ior                 = 1.33f; // Water
  m_guiMaterialParameters.push_back(parameters); // 4
  
  // Black BSDF for the light. This last material will not be shown inside the GUI!
  parameters.indexBSDF           = INDEX_BSDF_SPECULAR_REFLECTION;
  parameters.albedo              = make_float3(0.0f);
  parameters.useAlbedoTexture    = false;
  parameters.useCutoutTexture    = false;
  parameters.thinwalled          = false;
  parameters.absorptionColor     = make_float3(1.0f);
  parameters.volumeDistanceScale = 1.0f;
  parameters.ior                 = 1.0f;
  m_guiMaterialParameters.push_back(parameters); // 5
}


void Application::initPipeline()
{
  MY_ASSERT((sizeof(SbtRecordHeader)               % OPTIX_SBT_RECORD_ALIGNMENT) == 0);
  MY_ASSERT((sizeof(SbtRecordGeometryInstanceData) % OPTIX_SBT_RECORD_ALIGNMENT) == 0);

  // INSTANCES

  OptixInstance instance = {};

  OptixTraversableHandle geoPlane = createPlane(1, 1, 1);

  const float trafoPlane[12] =
  {
    8.0f, 0.0f, 0.0f, 0.0f,
    0.0f, 8.0f, 0.0f, 0.0f,
    0.0f, 0.0f, 8.0f, 0.0f
  };

  unsigned int id = static_cast<unsigned int>(m_instances.size());

  memcpy(instance.transform, trafoPlane, sizeof(float) * 12);
  instance.instanceId        = id;
  instance.visibilityMask    = 255;
  instance.sbtOffset         = id * NUM_RAYTYPES; // This controls the SBT instance offset!
  instance.flags             = OPTIX_INSTANCE_FLAG_NONE;
  instance.traversableHandle = geoPlane;
    
  m_instances.push_back(instance); // Plane 

  OptixTraversableHandle geoBox = createBox();

  const float trafoBox[12] =
  {
    1.0f, 0.0f, 0.0f, -2.5f, // Move to the left.
    0.0f, 1.0f, 0.0f, 1.25f, // The box is modeled with unit coordinates in the range [-1, 1], Move it above the floor plane.
    0.0f, 0.0f, 1.0f, 0.0f
  };

  id = static_cast<unsigned int>(m_instances.size());

  memcpy(instance.transform, trafoBox, sizeof(float) * 12);
  instance.instanceId        = id;
  instance.visibilityMask    = 255;
  instance.sbtOffset         = id * NUM_RAYTYPES;
  instance.flags             = OPTIX_INSTANCE_FLAG_NONE;
  instance.traversableHandle = geoBox;
    
  m_instances.push_back(instance); // Box 
  
#if 1
  // This is not instanced to match the original optixIntro_07 example for exact performance comparisons.
  OptixTraversableHandle geoNested = createSphere(180, 90, 1.0f, M_PIf); 

  const float trafoNested[12] =
  {
    0.75f, 0.0f,  0.0f, -2.5f,  // Scale this sphere down and move it into the center of the box.
    0.0f,  0.75f, 0.0f,  1.25f,
    0.0f,  0.0f,  0.75f, 0.0f,
  };

  id = static_cast<unsigned int>(m_instances.size());

  memcpy(instance.transform, trafoNested, sizeof(float) * 12);
  instance.instanceId        = id;
  instance.visibilityMask    = 255;
  instance.sbtOffset         = id * NUM_RAYTYPES;
  instance.flags             = OPTIX_INSTANCE_FLAG_NONE;
  instance.traversableHandle = geoNested;
    
  m_instances.push_back(instance); // Nested sphere.
#endif
  
  OptixTraversableHandle geoSphere = createSphere(180, 90, 1.0f, M_PIf);

  const float trafoSphere[12] =
  {
    1.0f, 0.0f, 0.0f, 0.0f,  // In the center, to the right of the box.
    0.0f, 1.0f, 0.0f, 1.25f, // The sphere is modeled with radius 1.0f. Move it above the floor plane to show shadows.
    0.0f, 0.0f, 1.0f, 0.0f
  };

  id = static_cast<unsigned int>(m_instances.size());

  memcpy(instance.transform, trafoSphere, sizeof(float) * 12);
  instance.instanceId        = id;
  instance.visibilityMask    = 255;
  instance.sbtOffset         = id * NUM_RAYTYPES;
  instance.flags             = OPTIX_INSTANCE_FLAG_NONE;
  instance.traversableHandle = geoSphere;
    
  m_instances.push_back(instance); // Sphere

  OptixTraversableHandle geoTorus = createTorus(180, 180, 0.75f, 0.25f);

  const float trafoTorus[12] =
  {
    1.0f, 0.0f, 0.0f, 2.5f,  // Move it to the right of the sphere.
    0.0f, 1.0f, 0.0f, 1.25f, // The torus has an outer radius of 0.5f. Move it above the floor plane.
    0.0f, 0.0f, 1.0f, 0.0f
  };

  id = static_cast<unsigned int>(m_instances.size());

  memcpy(instance.transform, trafoTorus, sizeof(float) * 12);
  instance.instanceId        = id;
  instance.visibilityMask    = 255;
  instance.sbtOffset         = id * NUM_RAYTYPES;
  instance.flags             = OPTIX_INSTANCE_FLAG_NONE;
  instance.traversableHandle = geoTorus;
    
  m_instances.push_back(instance); // Torus

  createLights();

  CUdeviceptr d_instances;
  
  const size_t instancesSizeInBytes = sizeof(OptixInstance) * m_instances.size();

  CUDA_CHECK( cudaMalloc((void**) &d_instances, instancesSizeInBytes) );
  CUDA_CHECK( cudaMemcpy((void*) d_instances, m_instances.data(), instancesSizeInBytes, cudaMemcpyHostToDevice) );

  OptixBuildInput instanceInput = {};

  instanceInput.type = OPTIX_BUILD_INPUT_TYPE_INSTANCES;
  instanceInput.instanceArray.instances    = d_instances;
  instanceInput.instanceArray.numInstances = (unsigned int) m_instances.size();

  OptixAccelBuildOptions accelBuildOptions = {};

  accelBuildOptions.buildFlags = OPTIX_BUILD_FLAG_NONE;
  accelBuildOptions.operation  = OPTIX_BUILD_OPERATION_BUILD;
  
  OptixAccelBufferSizes iasBufferSizes = {};

  OPTIX_CHECK( m_api.optixAccelComputeMemoryUsage(m_context, &accelBuildOptions, &instanceInput, 1, &iasBufferSizes) );

  CUDA_CHECK( cudaMalloc((void**) &m_d_ias, iasBufferSizes.outputSizeInBytes ) );

  CUdeviceptr d_tmp;
  
  CUDA_CHECK( cudaMalloc((void**) &d_tmp,   iasBufferSizes.tempSizeInBytes) );

  OPTIX_CHECK( m_api.optixAccelBuild(m_context, m_cudaStream,
                                     &accelBuildOptions, &instanceInput, 1,
                                     d_tmp,   iasBufferSizes.tempSizeInBytes,
                                     m_d_ias, iasBufferSizes.outputSizeInBytes,
                                     &m_root, nullptr, 0));

  CUDA_CHECK( cudaStreamSynchronize(m_cudaStream) );

  CUDA_CHECK( cudaFree((void*) d_tmp) );

  CUDA_CHECK( cudaFree((void*) d_instances) ); // Don't need the instances anymore.

  // MODULES

  OptixModuleCompileOptions moduleCompileOptions = {};

  moduleCompileOptions.maxRegisterCount = OPTIX_COMPILE_DEFAULT_MAX_REGISTER_COUNT; // No explicit register limit.
#if USE_MAX_OPTIMIZATION
  moduleCompileOptions.optLevel   = OPTIX_COMPILE_OPTIMIZATION_LEVEL_3; // All optimizations, is the default.
  // Keep generated line info for Nsight Compute profiling. (NVCC_OPTIONS use --generate-line-info in CMakeLists.txt)
#if (OPTIX_VERSION >= 70400)
  moduleCompileOptions.debugLevel = OPTIX_COMPILE_DEBUG_LEVEL_MINIMAL; 
#else
  moduleCompileOptions.debugLevel = OPTIX_COMPILE_DEBUG_LEVEL_LINEINFO;
#endif
#else // DEBUG
  moduleCompileOptions.optLevel   = OPTIX_COMPILE_OPTIMIZATION_LEVEL_0;
  moduleCompileOptions.debugLevel = OPTIX_COMPILE_DEBUG_LEVEL_FULL;
#endif

  OptixPipelineCompileOptions pipelineCompileOptions = {};

  pipelineCompileOptions.usesMotionBlur        = 0;
  pipelineCompileOptions.traversableGraphFlags = OPTIX_TRAVERSABLE_GRAPH_FLAG_ALLOW_SINGLE_LEVEL_INSTANCING;
  pipelineCompileOptions.numPayloadValues      = 2; // I need two to encode a 64-bit pointer to the per ray payload structure.
  pipelineCompileOptions.numAttributeValues    = 2; // The minimum is two, for the barycentrics.
#if USE_MAX_OPTIMIZATION
  pipelineCompileOptions.exceptionFlags        = OPTIX_EXCEPTION_FLAG_NONE;
#else // DEBUG 
  pipelineCompileOptions.exceptionFlags        = OPTIX_EXCEPTION_FLAG_STACK_OVERFLOW | 
                                                 OPTIX_EXCEPTION_FLAG_TRACE_DEPTH |
                                                 OPTIX_EXCEPTION_FLAG_USER |
                                                 OPTIX_EXCEPTION_FLAG_DEBUG;
#endif
  pipelineCompileOptions.pipelineLaunchParamsVariableName = "sysParameter";

  OptixProgramGroupOptions programGroupOptions = {}; // So far this is just a placeholder today.

  // Each source file results in one OptixModule.
  std::vector<OptixModule> modules(NUM_MODULE_IDENTIFIERS);
 
  // Create all modules:
  for (size_t i = 0; i < m_moduleFilenames.size(); ++i)
  {
    // Since OptiX 7.5.0 the program input can either be *.ptx source code or *.optixir binary code.
    // The module filenames are automatically switched between *.ptx or *.optixir extension based on the definition of USE_OPTIX_IR
    std::vector<char> programData = readData(m_moduleFilenames[i]); 
    
    OPTIX_CHECK( m_api.optixModuleCreateFromPTX(m_context, &moduleCompileOptions, &pipelineCompileOptions, programData.data(), programData.size(), nullptr, nullptr, &modules[i]) );
  }

  // Each program gets its own OptixProgramGroupDesc.
  std::vector<OptixProgramGroupDesc> programGroupDescriptions(NUM_PROGRAM_IDENTIFIERS);
  // Null out all entries inside the program group descriptions. 
  // This is important because the following code will only set the required fields.
  memset(programGroupDescriptions.data(), 0, sizeof(OptixProgramGroupDesc) * programGroupDescriptions.size());

  // Setup all program group descriptions.
  OptixProgramGroupDesc* pgd = &programGroupDescriptions[PROGRAM_ID_RAYGENERATION];
  pgd->kind  = OPTIX_PROGRAM_GROUP_KIND_RAYGEN;
  pgd->flags = OPTIX_PROGRAM_GROUP_FLAGS_NONE;
  pgd->raygen.module            = modules[MODULE_ID_RAYGENERATION];
  pgd->raygen.entryFunctionName = "__raygen__pathtracer";

  pgd = &programGroupDescriptions[PROGRAM_ID_EXCEPTION];
  pgd->kind  = OPTIX_PROGRAM_GROUP_KIND_EXCEPTION;
  pgd->flags = OPTIX_PROGRAM_GROUP_FLAGS_NONE;
  pgd->exception.module            = modules[MODULE_ID_EXCEPTION];
  pgd->exception.entryFunctionName = "__exception__all";

  // MISS

  pgd = &programGroupDescriptions[PROGRAM_ID_MISS_RADIANCE];
  pgd->kind  = OPTIX_PROGRAM_GROUP_KIND_MISS;
  pgd->flags = OPTIX_PROGRAM_GROUP_FLAGS_NONE;
  pgd->miss.module = modules[MODULE_ID_MISS];
  switch (m_missID)
  {
    case 0: // Black, not a light.
      pgd->miss.entryFunctionName = "__miss__env_null";
      break;
    case 1: // Constant white environment.
    default:
      pgd->miss.entryFunctionName = "__miss__env_constant";
      break;
    case 2: // Spherical HDR environment light.
      pgd->miss.entryFunctionName = "__miss__env_sphere";
      break;
  }

  pgd = &programGroupDescriptions[PROGRAM_ID_MISS_SHADOW];
  pgd->kind  = OPTIX_PROGRAM_GROUP_KIND_MISS;
  pgd->flags = OPTIX_PROGRAM_GROUP_FLAGS_NONE;
  pgd->miss.module            = nullptr; // Redundant after the memset() above, for code clarity.
  pgd->miss.entryFunctionName = nullptr; // No miss program for shadow rays. 

  // HIT

  pgd = &programGroupDescriptions[PROGRAM_ID_HIT_RADIANCE];
  pgd->kind  = OPTIX_PROGRAM_GROUP_KIND_HITGROUP;
  pgd->flags = OPTIX_PROGRAM_GROUP_FLAGS_NONE;
  pgd->hitgroup.moduleCH            = modules[MODULE_ID_CLOSESTHIT];
  pgd->hitgroup.entryFunctionNameCH = "__closesthit__radiance";

  pgd = &programGroupDescriptions[PROGRAM_ID_HIT_SHADOW];
  pgd->kind  = OPTIX_PROGRAM_GROUP_KIND_HITGROUP;
  pgd->flags = OPTIX_PROGRAM_GROUP_FLAGS_NONE;
  pgd->hitgroup.moduleAH            = modules[MODULE_ID_ANYHIT];
  pgd->hitgroup.entryFunctionNameAH = "__anyhit__shadow";

  pgd = &programGroupDescriptions[PROGRAM_ID_HIT_RADIANCE_CUTOUT];
  pgd->kind  = OPTIX_PROGRAM_GROUP_KIND_HITGROUP;
  pgd->flags = OPTIX_PROGRAM_GROUP_FLAGS_NONE;
  pgd->hitgroup.moduleCH            = modules[MODULE_ID_CLOSESTHIT];
  pgd->hitgroup.entryFunctionNameCH = "__closesthit__radiance";
  pgd->hitgroup.moduleAH            = modules[MODULE_ID_ANYHIT];
  pgd->hitgroup.entryFunctionNameAH = "__anyhit__radiance_cutout";

  pgd = &programGroupDescriptions[PROGRAM_ID_HIT_SHADOW_CUTOUT];
  pgd->kind  = OPTIX_PROGRAM_GROUP_KIND_HITGROUP;
  pgd->flags = OPTIX_PROGRAM_GROUP_FLAGS_NONE;
  pgd->hitgroup.moduleAH            = modules[MODULE_ID_ANYHIT];
  pgd->hitgroup.entryFunctionNameAH = "__anyhit__shadow_cutout";

  // CALLABLES

  pgd = &programGroupDescriptions[PROGRAM_ID_LENS_PINHOLE];
  pgd->kind  = OPTIX_PROGRAM_GROUP_KIND_CALLABLES;
  pgd->flags = OPTIX_PROGRAM_GROUP_FLAGS_NONE;
  pgd->callables.moduleDC = modules[MODULE_ID_LENS_SHADER];
  pgd->callables.entryFunctionNameDC = "__direct_callable__pinhole";

  pgd = &programGroupDescriptions[PROGRAM_ID_LENS_FISHEYE];
  pgd->kind  = OPTIX_PROGRAM_GROUP_KIND_CALLABLES;
  pgd->flags = OPTIX_PROGRAM_GROUP_FLAGS_NONE;
  pgd->callables.moduleDC = modules[MODULE_ID_LENS_SHADER];
  pgd->callables.entryFunctionNameDC = "__direct_callable__fisheye";

  pgd = &programGroupDescriptions[PROGRAM_ID_LENS_SPHERE];
  pgd->kind  = OPTIX_PROGRAM_GROUP_KIND_CALLABLES;
  pgd->flags = OPTIX_PROGRAM_GROUP_FLAGS_NONE;
  pgd->callables.moduleDC = modules[MODULE_ID_LENS_SHADER];
  pgd->callables.entryFunctionNameDC = "__direct_callable__sphere";

  // Two light sampling functions, one for the environment and one for the parallelogram.
  pgd = &programGroupDescriptions[PROGRAM_ID_LIGHT_ENV];
  pgd->kind  = OPTIX_PROGRAM_GROUP_KIND_CALLABLES;
  pgd->flags = OPTIX_PROGRAM_GROUP_FLAGS_NONE;
  pgd->callables.moduleDC = modules[MODULE_ID_LIGHT_SAMPLE];
  switch (m_missID)
  {
    case 0: // Black environment. 
      // This is not a light and doesn't appear in the sysParameter.lightDefinitions and therefore is never called.
      // Put a valid direct callable into this SBT record anyway to have the correct number of callables. Use the light_env_constant function.
      // Fall through.
    case 1: // White environment.
    default:
      pgd->callables.entryFunctionNameDC = "__direct_callable__light_env_constant";
      break;
    case 2:
      pgd->callables.entryFunctionNameDC = "__direct_callable__light_env_sphere";
      break;
  }

  pgd = &programGroupDescriptions[PROGRAM_ID_LIGHT_PARALLELOGRAM];
  pgd->kind  = OPTIX_PROGRAM_GROUP_KIND_CALLABLES;
  pgd->flags = OPTIX_PROGRAM_GROUP_FLAGS_NONE;
  pgd->callables.moduleDC = modules[MODULE_ID_LIGHT_SAMPLE];
  pgd->callables.entryFunctionNameDC = "__direct_callable__light_parallelogram";

  pgd = &programGroupDescriptions[PROGRAM_ID_BRDF_DIFFUSE_SAMPLE];
  pgd->kind  = OPTIX_PROGRAM_GROUP_KIND_CALLABLES;
  pgd->flags = OPTIX_PROGRAM_GROUP_FLAGS_NONE;
  pgd->callables.moduleDC = modules[MODULE_ID_DIFFUSE_REFLECTION];
  pgd->callables.entryFunctionNameDC = "__direct_callable__sample_bsdf_diffuse_reflection";

  pgd = &programGroupDescriptions[PROGRAM_ID_BRDF_DIFFUSE_EVAL];
  pgd->kind  = OPTIX_PROGRAM_GROUP_KIND_CALLABLES;
  pgd->flags = OPTIX_PROGRAM_GROUP_FLAGS_NONE;
  pgd->callables.moduleDC = modules[MODULE_ID_DIFFUSE_REFLECTION];
  pgd->callables.entryFunctionNameDC = "__direct_callable__eval_bsdf_diffuse_reflection";

  pgd = &programGroupDescriptions[PROGRAM_ID_BRDF_SPECULAR_SAMPLE];
  pgd->kind  = OPTIX_PROGRAM_GROUP_KIND_CALLABLES;
  pgd->flags = OPTIX_PROGRAM_GROUP_FLAGS_NONE;
  pgd->callables.moduleDC = modules[MODULE_ID_SPECULAR_REFLECTION];
  pgd->callables.entryFunctionNameDC = "__direct_callable__sample_bsdf_specular_reflection";

  pgd = &programGroupDescriptions[PROGRAM_ID_BRDF_SPECULAR_EVAL];
  pgd->kind  = OPTIX_PROGRAM_GROUP_KIND_CALLABLES;
  pgd->flags = OPTIX_PROGRAM_GROUP_FLAGS_NONE;
  pgd->callables.moduleDC = modules[MODULE_ID_SPECULAR_REFLECTION];
  pgd->callables.entryFunctionNameDC = "__direct_callable__eval_bsdf_specular_reflection"; // black

  pgd = &programGroupDescriptions[PROGRAM_ID_BSDF_SPECULAR_SAMPLE];
  pgd->kind  = OPTIX_PROGRAM_GROUP_KIND_CALLABLES;
  pgd->flags = OPTIX_PROGRAM_GROUP_FLAGS_NONE;
  pgd->callables.moduleDC = modules[MODULE_ID_SPECULAR_REFLECTION_TRANSMISSION];
  pgd->callables.entryFunctionNameDC = "__direct_callable__sample_bsdf_specular_reflection_transmission";

  pgd = &programGroupDescriptions[PROGRAM_ID_BSDF_SPECULAR_EVAL];
  pgd->kind  = OPTIX_PROGRAM_GROUP_KIND_CALLABLES;
  pgd->flags = OPTIX_PROGRAM_GROUP_FLAGS_NONE;
  // Reuse the same black eval function from the specular BRDF.
  pgd->callables.moduleDC = modules[MODULE_ID_SPECULAR_REFLECTION];
  pgd->callables.entryFunctionNameDC = "__direct_callable__eval_bsdf_specular_reflection"; // black

  // Each OptixProgramGroupDesc results on one OptixProgramGroup.
  std::vector<OptixProgramGroup> programGroups(NUM_PROGRAM_IDENTIFIERS);

  // Construct all program groups at once.
  OPTIX_CHECK( m_api.optixProgramGroupCreate(m_context, programGroupDescriptions.data(), (unsigned int) programGroupDescriptions.size(), &programGroupOptions, nullptr, nullptr, programGroups.data()) );

  OptixPipelineLinkOptions pipelineLinkOptions = {};

  pipelineLinkOptions.maxTraceDepth = 2;
#if USE_MAX_OPTIMIZATION
  // Keep generated line info for Nsight Compute profiling. (NVCC_OPTIONS use --generate-line-info in CMakeLists.txt)
#if (OPTIX_VERSION >= 70400)
  pipelineLinkOptions.debugLevel = OPTIX_COMPILE_DEBUG_LEVEL_MINIMAL; 
#else
  pipelineLinkOptions.debugLevel = OPTIX_COMPILE_DEBUG_LEVEL_LINEINFO;
#endif
#else // DEBUG
  pipelineLinkOptions.debugLevel = OPTIX_COMPILE_DEBUG_LEVEL_FULL;
#endif
#if (OPTIX_VERSION == 70000)
  pipelineLinkOptions.overrideUsesMotionBlur = 0; // Does not exist in OptiX 7.1.0.
#endif

  OPTIX_CHECK( m_api.optixPipelineCreate(m_context, &pipelineCompileOptions, &pipelineLinkOptions, programGroups.data(), (unsigned int) programGroups.size(), nullptr, nullptr, &m_pipeline) );

  // STACK SIZES

  OptixStackSizes stackSizesPipeline = {};

  for (size_t i = 0; i < programGroups.size(); ++i)
  {
    OptixStackSizes stackSizes;

    OPTIX_CHECK( m_api.optixProgramGroupGetStackSize(programGroups[i], &stackSizes) );

    stackSizesPipeline.cssRG = std::max(stackSizesPipeline.cssRG, stackSizes.cssRG);
    stackSizesPipeline.cssMS = std::max(stackSizesPipeline.cssMS, stackSizes.cssMS);
    stackSizesPipeline.cssCH = std::max(stackSizesPipeline.cssCH, stackSizes.cssCH);
    stackSizesPipeline.cssAH = std::max(stackSizesPipeline.cssAH, stackSizes.cssAH);
    stackSizesPipeline.cssIS = std::max(stackSizesPipeline.cssIS, stackSizes.cssIS);
    stackSizesPipeline.cssCC = std::max(stackSizesPipeline.cssCC, stackSizes.cssCC);
    stackSizesPipeline.dssDC = std::max(stackSizesPipeline.dssDC, stackSizes.dssDC);
  }
  
  // Temporaries
  const unsigned int cssCCTree           = stackSizesPipeline.cssCC; // Should be 0. No continuation callables in this pipeline. // maxCCDepth == 0
  const unsigned int cssCHOrMSPlusCCTree = std::max(stackSizesPipeline.cssCH, stackSizesPipeline.cssMS) + cssCCTree;

  // Arguments
  const unsigned int directCallableStackSizeFromTraversal = stackSizesPipeline.dssDC; // maxDCDepth == 1 // FromTraversal: DC is invoked from IS or AH.      // Possible stack size optimizations.
  const unsigned int directCallableStackSizeFromState     = stackSizesPipeline.dssDC; // maxDCDepth == 1 // FromState:     DC is invoked from RG, MS, or CH. // Possible stack size optimizations.
  const unsigned int continuationStackSize = stackSizesPipeline.cssRG + cssCCTree + cssCHOrMSPlusCCTree * (std::max(1u, pipelineLinkOptions.maxTraceDepth) - 1u) +
                                             std::min(1u, pipelineLinkOptions.maxTraceDepth) * std::max(cssCHOrMSPlusCCTree, stackSizesPipeline.cssAH + stackSizesPipeline.cssIS);
  // "The maxTraversableGraphDepth responds to the maximum number of traversables visited when calling optixTrace. 
  // Every acceleration structure and motion transform count as one level of traversal."
  // Render Graph is at maximum: IAS -> GAS
  const unsigned int maxTraversableGraphDepth = 2;

  OPTIX_CHECK( m_api.optixPipelineSetStackSize(m_pipeline, directCallableStackSizeFromTraversal, directCallableStackSizeFromState, continuationStackSize, maxTraversableGraphDepth) );

  // Set up Shader Binding Table (SBT)
  // The shader binding table is inherently connected to the scene graph geometry instances in this example.

  // Raygeneration group
  SbtRecordHeader sbtRecordRaygeneration;

  OPTIX_CHECK( m_api.optixSbtRecordPackHeader(programGroups[PROGRAM_ID_RAYGENERATION], &sbtRecordRaygeneration) );

  CUDA_CHECK( cudaMalloc((void**) &m_d_sbtRecordRaygeneration, sizeof(SbtRecordHeader)) );
  CUDA_CHECK( cudaMemcpy((void*) m_d_sbtRecordRaygeneration, &sbtRecordRaygeneration, sizeof(SbtRecordHeader), cudaMemcpyHostToDevice) );

  // Exception
  SbtRecordHeader sbtRecordException;

  OPTIX_CHECK( m_api.optixSbtRecordPackHeader(programGroups[PROGRAM_ID_EXCEPTION], &sbtRecordException) );

  CUDA_CHECK( cudaMalloc((void**) &m_d_sbtRecordException, sizeof(SbtRecordHeader)) );
  CUDA_CHECK( cudaMemcpy((void*) m_d_sbtRecordException, &sbtRecordException, sizeof(SbtRecordHeader), cudaMemcpyHostToDevice) );

  // Miss group
  std::vector<SbtRecordHeader> sbtRecordMiss(NUM_RAYTYPES);

  OPTIX_CHECK( m_api.optixSbtRecordPackHeader(programGroups[PROGRAM_ID_MISS_RADIANCE], &sbtRecordMiss[RAYTYPE_RADIANCE]) );
  OPTIX_CHECK( m_api.optixSbtRecordPackHeader(programGroups[PROGRAM_ID_MISS_SHADOW],   &sbtRecordMiss[RAYTYPE_SHADOW]) );

  CUDA_CHECK( cudaMalloc((void**) &m_d_sbtRecordMiss, sizeof(SbtRecordHeader) * NUM_RAYTYPES) );
  CUDA_CHECK( cudaMemcpy((void*) m_d_sbtRecordMiss, sbtRecordMiss.data(), sizeof(SbtRecordHeader) * NUM_RAYTYPES, cudaMemcpyHostToDevice) );

  // Hit groups for radiance and shadow rays per instance.
  
  MY_ASSERT(NUM_RAYTYPES == 2); // The following code only works for two raytypes.

  // Note that the SBT record data field is uninitialized after these!
  OPTIX_CHECK( m_api.optixSbtRecordPackHeader(programGroups[PROGRAM_ID_HIT_RADIANCE],        &m_sbtRecordHitRadiance) );
  OPTIX_CHECK( m_api.optixSbtRecordPackHeader(programGroups[PROGRAM_ID_HIT_SHADOW],          &m_sbtRecordHitShadow) );
  OPTIX_CHECK( m_api.optixSbtRecordPackHeader(programGroups[PROGRAM_ID_HIT_RADIANCE_CUTOUT], &m_sbtRecordHitRadianceCutout) );
  OPTIX_CHECK( m_api.optixSbtRecordPackHeader(programGroups[PROGRAM_ID_HIT_SHADOW_CUTOUT],   &m_sbtRecordHitShadowCutout) );

  // The real content.
  const int numInstances = static_cast<int>(m_instances.size());

  // In this exmple, each instance has its own SBT hit record. 
  // The additional data in the SBT hit record defines the geometry attributes and topology, material and optional light indices.
  m_sbtRecordGeometryInstanceData.resize(NUM_RAYTYPES * numInstances);

  for (int i = 0; i < numInstances; ++i)
  {
    const int idx = i * NUM_RAYTYPES; // idx == radiance ray, idx + 1 == shadow ray

    if (!m_guiMaterialParameters[i].useCutoutTexture)
    {
      // Only update the header to switch the program hit group. The SBT record data field doesn't change. 
      memcpy(m_sbtRecordGeometryInstanceData[idx    ].header, m_sbtRecordHitRadiance.header, OPTIX_SBT_RECORD_HEADER_SIZE);
      memcpy(m_sbtRecordGeometryInstanceData[idx + 1].header, m_sbtRecordHitShadow.header,   OPTIX_SBT_RECORD_HEADER_SIZE);
    }
    else
    {
      memcpy(m_sbtRecordGeometryInstanceData[idx    ].header, m_sbtRecordHitRadianceCutout.header, OPTIX_SBT_RECORD_HEADER_SIZE);
      memcpy(m_sbtRecordGeometryInstanceData[idx + 1].header, m_sbtRecordHitShadowCutout.header,   OPTIX_SBT_RECORD_HEADER_SIZE);
    }

    m_sbtRecordGeometryInstanceData[idx    ].data.indices       = (int3*)             m_geometries[i].indices;
    m_sbtRecordGeometryInstanceData[idx    ].data.attributes    = (VertexAttributes*) m_geometries[i].attributes;
    m_sbtRecordGeometryInstanceData[idx    ].data.materialIndex = i;
    m_sbtRecordGeometryInstanceData[idx    ].data.lightIndex    = -1;

    m_sbtRecordGeometryInstanceData[idx + 1].data.indices       = (int3*)             m_geometries[i].indices;
    m_sbtRecordGeometryInstanceData[idx + 1].data.attributes    = (VertexAttributes*) m_geometries[i].attributes;
    m_sbtRecordGeometryInstanceData[idx + 1].data.materialIndex = i;
    m_sbtRecordGeometryInstanceData[idx + 1].data.lightIndex    = -1;
  }

  if (m_lightID)
  {
    const int idx = (numInstances - 1) * NUM_RAYTYPES; // HACK The last instance is the parallelogram light.
    const int lightIndex = (m_missID != 0) ? 1 : 0;    // HACK If there is any environment light that is in sysParameter.lightDefinitions[0] and the area light in index [1] then.
    m_sbtRecordGeometryInstanceData[idx    ].data.lightIndex = lightIndex;
    m_sbtRecordGeometryInstanceData[idx + 1].data.lightIndex = lightIndex;
  }

  CUDA_CHECK( cudaMalloc((void**) &m_d_sbtRecordGeometryInstanceData, sizeof(SbtRecordGeometryInstanceData) * NUM_RAYTYPES * numInstances) );
  CUDA_CHECK( cudaMemcpy((void*) m_d_sbtRecordGeometryInstanceData, m_sbtRecordGeometryInstanceData.data(), sizeof(SbtRecordGeometryInstanceData) * NUM_RAYTYPES * numInstances, cudaMemcpyHostToDevice) );

  // CALLABLES

  // The callable programs are at the end of the ProgramIdentifier enums (from PROGRAM_ID_LENS_PINHOLE to PROGRAM_ID_BSDF_SPECULAR_EVAL)
  const int numCallables = static_cast<int>(NUM_PROGRAM_IDENTIFIERS) - static_cast<int>(PROGRAM_ID_LENS_PINHOLE);
  std::vector<SbtRecordHeader> sbtRecordCallables(numCallables);

  for (int i = 0; i < numCallables; ++i)
  {
    OPTIX_CHECK( m_api.optixSbtRecordPackHeader(programGroups[static_cast<int>(PROGRAM_ID_LENS_PINHOLE) + i], &sbtRecordCallables[i]) );
  }

  CUDA_CHECK( cudaMalloc((void**) &m_d_sbtRecordCallables, sizeof(SbtRecordHeader) * sbtRecordCallables.size()) );
  CUDA_CHECK( cudaMemcpy((void*) m_d_sbtRecordCallables, sbtRecordCallables.data(), sizeof(SbtRecordHeader) * sbtRecordCallables.size(), cudaMemcpyHostToDevice) );

  // Setup the OptixShaderBindingTable.
  m_sbt.raygenRecord = m_d_sbtRecordRaygeneration;

  m_sbt.exceptionRecord = m_d_sbtRecordException;

  m_sbt.missRecordBase          = m_d_sbtRecordMiss;
  m_sbt.missRecordStrideInBytes = (unsigned int) sizeof(SbtRecordHeader);
  m_sbt.missRecordCount         = NUM_RAYTYPES;

  m_sbt.hitgroupRecordBase          = reinterpret_cast<CUdeviceptr>(m_d_sbtRecordGeometryInstanceData);
  m_sbt.hitgroupRecordStrideInBytes = (unsigned int) sizeof(SbtRecordGeometryInstanceData);
  m_sbt.hitgroupRecordCount         = NUM_RAYTYPES * numInstances;

  m_sbt.callablesRecordBase          = m_d_sbtRecordCallables;
  m_sbt.callablesRecordStrideInBytes = (unsigned int) sizeof(SbtRecordHeader);
  m_sbt.callablesRecordCount         = (unsigned int) sbtRecordCallables.size();

  // Setup "sysParameter" data.
  m_systemParameter.topObject = m_root;

  if (m_interop)
  {
    CUDA_CHECK( cudaGraphicsGLRegisterBuffer(&m_cudaGraphicsResource, m_pbo, cudaGraphicsRegisterFlagsNone) ); // No flags for read-write access during accumulation.

    size_t size;

    CUDA_CHECK( cudaGraphicsMapResources(1, &m_cudaGraphicsResource, m_cudaStream) );
    CUDA_CHECK( cudaGraphicsResourceGetMappedPointer((void**) &m_systemParameter.outputBuffer, &size, m_cudaGraphicsResource) );
    CUDA_CHECK( cudaGraphicsUnmapResources(1, &m_cudaGraphicsResource, m_cudaStream) );
    
    MY_ASSERT(m_width * m_height * sizeof(float) * 4 <= size);
  }
  else
  {
    CUDA_CHECK( cudaMalloc((void**) &m_systemParameter.outputBuffer, sizeof(float4) * m_width * m_height) ); // No data initialization, that is done at iterationIndex == 0.
  }
  
  MY_ASSERT((sizeof(LightDefinition) & 15) == 0); // Check alignment to float4
  CUDA_CHECK( cudaMalloc((void**) &m_systemParameter.lightDefinitions, sizeof(LightDefinition) * m_lightDefinitions.size()) );
  CUDA_CHECK( cudaMemcpy((void*) m_systemParameter.lightDefinitions, m_lightDefinitions.data(), sizeof(LightDefinition) * m_lightDefinitions.size(), cudaMemcpyHostToDevice) );
  
  CUDA_CHECK( cudaMalloc((void**) &m_systemParameter.materialParameters, sizeof(MaterialParameter) * m_guiMaterialParameters.size()) );
  updateMaterialParameters();

  // Setup the environment texture values. These are all defaults when there is no environment texture filename given.
  m_systemParameter.envTexture  = m_textureEnvironment->getTextureObject();
  m_systemParameter.envCDF_U    = (float*) m_textureEnvironment->getCDF_U();
  m_systemParameter.envCDF_V    = (float*) m_textureEnvironment->getCDF_V();
  m_systemParameter.envWidth    = m_textureEnvironment->getWidth();
  m_systemParameter.envHeight   = m_textureEnvironment->getHeight();
  m_systemParameter.envIntegral = m_textureEnvironment->getIntegral();

  m_systemParameter.pathLengths    = make_int2(2, 10);  // Default max path length set to 10 for the nested materials and to match optixIntro_07 for performance comparison.
  m_systemParameter.sceneEpsilon   = m_sceneEpsilonFactor * SCENE_EPSILON_SCALE;
  m_systemParameter.numLights      = static_cast<unsigned int>(m_lightDefinitions.size());
  m_systemParameter.iterationIndex = 0;
  m_systemParameter.cameraType     = LENS_SHADER_PINHOLE;

  m_pinholeCamera.getFrustum(m_systemParameter.cameraPosition,
                             m_systemParameter.cameraU,
                             m_systemParameter.cameraV,
                             m_systemParameter.cameraW);

  CUDA_CHECK( cudaMalloc((void**) &m_d_systemParameter, sizeof(SystemParameter)) );
  CUDA_CHECK( cudaMemcpy((void*) m_d_systemParameter, &m_systemParameter, sizeof(SystemParameter), cudaMemcpyHostToDevice) );

  // After all required optixSbtRecordPackHeader, optixProgramGroupGetStackSize, and optixPipelineCreate
  // calls have been done, the OptixProgramGroup and OptixModule objects (opaque pointer to struct types)
  // can be destroyed.
  for (auto pg: programGroups)
  {
    OPTIX_CHECK( m_api.optixProgramGroupDestroy(pg) );
  }

  for (auto m: modules)
  {
    OPTIX_CHECK( m_api.optixModuleDestroy(m) );
  }
}

// In contrast to the original optixIntro_07, this example supports dynamic switching of the cutout opacity material parameter.
void Application::updateShaderBindingTable(const int instance)
{
  if (instance < m_instances.size()) // Make sure to only touch existing SBT records.
  {
    const int idx = instance * NUM_RAYTYPES; // idx == radiance ray, idx + 1 == shadow ray

    if (!m_guiMaterialParameters[instance].useCutoutTexture)
    {
      // Only update the header to switch the program hit group. The SBT record data field doesn't change. 
      memcpy(m_sbtRecordGeometryInstanceData[idx    ].header, m_sbtRecordHitRadiance.header, OPTIX_SBT_RECORD_HEADER_SIZE);
      memcpy(m_sbtRecordGeometryInstanceData[idx + 1].header, m_sbtRecordHitShadow.header,   OPTIX_SBT_RECORD_HEADER_SIZE);
    }
    else
    {
      memcpy(m_sbtRecordGeometryInstanceData[idx    ].header, m_sbtRecordHitRadianceCutout.header, OPTIX_SBT_RECORD_HEADER_SIZE);
      memcpy(m_sbtRecordGeometryInstanceData[idx + 1].header, m_sbtRecordHitShadowCutout.header,   OPTIX_SBT_RECORD_HEADER_SIZE);
    }

    // Make sure the SBT isn't changed while the renderer is active.
    CUDA_CHECK( cudaStreamSynchronize(m_cudaStream) ); 
    // Only copy the two SBT entries which changed.
    CUDA_CHECK( cudaMemcpy((void*) &m_d_sbtRecordGeometryInstanceData[idx], &m_sbtRecordGeometryInstanceData[idx], sizeof(SbtRecordGeometryInstanceData) * NUM_RAYTYPES, cudaMemcpyHostToDevice) );
  }
}


void Application::initRenderer()
{
  m_timer.restart();

  const double timeRenderer = m_timer.getTime();

  initMaterials();
  const double timeMaterials = m_timer.getTime();

  initPipeline();
  const double timePipeline = m_timer.getTime();

  std::cout << "initRenderer(): " << timePipeline - timeRenderer << " seconds overall\n";
  std::cout << "{\n";
  std::cout << "  materials  = " << timeMaterials - timeRenderer << " seconds\n";
  std::cout << "  pipeline   = " << timePipeline - timeMaterials << " seconds\n";
  std::cout << "}\n";
}


void Application::createLights()
{
  LightDefinition light;

  // Unused in environment lights. 
  light.position = make_float3(0.0f, 0.0f, 0.0f);
  light.vecU     = make_float3(1.0f, 0.0f, 0.0f);
  light.vecV     = make_float3(0.0f, 1.0f, 0.0f);
  light.normal   = make_float3(0.0f, 0.0f, 1.0f);
  light.area     = 1.0f;
  light.emission = make_float3(1.0f, 1.0f, 1.0f);
  
  m_textureEnvironment = new Texture(); // Allocate an empty environment texture to be able to initialize the sysParameters unconditionally.

  // The environment light is expected in sysParameter.lightDefinitions[0], but since there is only one, 
  // the sysParameter struct contains the data for the spherical HDR environment light when enabled.
  // All other lights are indexed by their position inside the array.
  switch (m_missID)
  {
  case 0: // No environment light at all. Faster than a zero emission constant environment!
  default:
    break;

  case 1: // Constant environment light.
    light.type = LIGHT_ENVIRONMENT;
    light.area = 4.0f * M_PIf; // Unused.

    m_lightDefinitions.push_back(light);
    break;

  case 2: // HDR Environment mapping with loaded texture.
    {
      Picture* picture = new Picture; // Separating image file handling from CUDA texture handling.

      const unsigned int flags = IMAGE_FLAG_2D | IMAGE_FLAG_ENV;
      if (!picture->load(m_environmentFilename, flags))
      {
        picture->generateEnvironment(8, 8); // Generate a white 8x8 RGBA32F dummy environment picture.
      }
      m_textureEnvironment->create(picture, flags);

      delete picture;
    }

    light.type = LIGHT_ENVIRONMENT;
    light.area = 4.0f * M_PIf; // Unused.

    m_lightDefinitions.push_back(light);
    break;
  }

  if (m_lightID)  // Add a square area light over the scene objects.
  {
    light.type     = LIGHT_PARALLELOGRAM;             // A geometric area light with diffuse emission distribution function.
    light.position = make_float3(-2.0f, 4.0f, -2.0f); // Corner position.
    light.vecU     = make_float3(4.0f, 0.0f, 0.0f);   // To the right.
    light.vecV     = make_float3(0.0f, 0.0f, 4.0f);   // To the front. 
    float3 n       = cross(light.vecU, light.vecV);   // Length of the cross product is the area.
    light.area     = length(n);                       // Calculate the world space area of that rectangle, unit is [m^2]
    light.normal   = n / light.area;                  // Normalized normal
    light.emission = make_float3(10.0f);              // Radiant exitance in Watt/m^2.

    m_lightDefinitions.push_back(light);
    
    OptixTraversableHandle geoLight = createParallelogram(light.position, light.vecU, light.vecV, light.normal);

    OptixInstance instance = {};

    // The geometric light is stored in world coordinates for now.
    const float trafoLight[12] =
    {
      1.0f, 0.0f, 0.0f, 0.0f,
      0.0f, 1.0f, 0.0f, 0.0f,
      0.0f, 0.0f, 1.0f, 0.0f
    };

    const unsigned int id = static_cast<unsigned int>(m_instances.size());

    memcpy(instance.transform, trafoLight, sizeof(float) * 12);
    instance.instanceId        = id;
    instance.visibilityMask    = 255;
    instance.sbtOffset         = id * NUM_RAYTYPES;
    instance.flags             = OPTIX_INSTANCE_FLAG_NONE;
    instance.traversableHandle = geoLight;

    m_instances.push_back(instance); // Parallelogram light.
  }
}
