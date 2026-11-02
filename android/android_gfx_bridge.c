/* First game-facing Android graphics checkpoint. The patched ARM64 game calls
 * these imports through the host gate, as the Metal port does on macOS.
 * This file creates the Vulkan device; the shared NVN layer (hle_nvn.c) runs
 * on top of it through nvn_vk_backend.c. */
#include <android/native_window.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>
#include "nvk_vk_dispatch.h"
#include "android_gfx.h"
#include "nvn_vk_features.h"

extern _Noreturn void runtime_fail(const char *message);
extern ANativeWindow *android_front_acquire_window(void);
extern void *android_nvn_force_override(const char *name);
extern void android_nvn_host_install(void);
extern PFN_vkGetInstanceProcAddr android_vk_loader(void);

typedef void *(*Alloc)(unsigned long, unsigned long, void *);
typedef void (*Free)(void *, void *);
typedef void *(*Realloc)(void *, unsigned long, void *);
typedef struct { Alloc alloc; Free free; Realloc realloc; void *user; } GraphicsAllocator;

static GraphicsAllocator game_allocator;
static GraphicsAllocator devtools_allocator;
static VkInstance instance;
static VkDevice device;
static VkPhysicalDevice physical;
static VkQueue queue;
static uint32_t queue_family;
static VkSurfaceKHR surface;
static ANativeWindow *native_window;

static AndroidGfxCaps caps;
extern int android_vk_2d_view_of_3d;
extern int android_vk_local_read;   /* nvn_vk_image.c */
extern int android_vk_load_op_none; /* nvn_vk_backend.c */
extern int android_vk_transform_feedback; /* nvn_vk_backend.c */
extern int nvk_dyn_ubo_limit;       /* nvn_vk_shader.c */
extern int nvk_xfb_storage_supported;
extern uint32_t nvk_compute_subgroup_size;
extern uint64_t nvk_storage_alignment, nvk_storage_range_max;

int android_gfx_device_ready(void) { return device!=VK_NULL_HANDLE; }
const AndroidGfxCaps *android_gfx_caps(void) { return device?&caps:NULL; }
static AndroidGfxVulkan handles;
const AndroidGfxVulkan *android_gfx_vulkan(void) { return device?&handles:NULL; }

static void graphics_allocator(Alloc alloc, Free free_fn, Realloc realloc_fn, void *user) {
    game_allocator = (GraphicsAllocator){alloc, free_fn, realloc_fn, user};
}

static void graphics_devtools_allocator(Alloc alloc, Free free_fn, Realloc realloc_fn, void *user) {
    devtools_allocator = (GraphicsAllocator){alloc, free_fn, realloc_fn, user};
}

static void graphics_initialize(void *work, unsigned long size) {
    if (!work || !size || instance || device) runtime_fail("invalid or repeated Android graphics initialization");
    VkApplicationInfo app = {.sType=VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName="TOTK Android", .apiVersion=VK_API_VERSION_1_3};
    VkInstanceCreateInfo create = {.sType=VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo=&app};
    const char *instance_extensions[]={VK_KHR_SURFACE_EXTENSION_NAME,VK_KHR_ANDROID_SURFACE_EXTENSION_NAME};
    native_window=android_front_acquire_window();
    if(native_window) {
        create.enabledExtensionCount=2;
        create.ppEnabledExtensionNames=instance_extensions;
    }
    nvk_dispatch_load_global(android_vk_loader());
    if (vkCreateInstance(&create, NULL, &instance) != VK_SUCCESS)
        runtime_fail("Android game Vulkan instance creation failed");
    nvk_dispatch_load_instance(instance, native_window != NULL);
    if(native_window) {
        VkAndroidSurfaceCreateInfoKHR surface_create={.sType=VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR,
            .window=native_window};
        if(vkCreateAndroidSurfaceKHR(instance,&surface_create,NULL,&surface)!=VK_SUCCESS)
            runtime_fail("Android game Vulkan surface creation failed");
    }
    uint32_t count = 0;
    if (vkEnumeratePhysicalDevices(instance, &count, NULL) != VK_SUCCESS || !count)
        runtime_fail("Android game Vulkan physical device missing");
    VkPhysicalDevice candidates[8];
    if (count > 8) count = 8;
    if (vkEnumeratePhysicalDevices(instance, &count, candidates) != VK_SUCCESS)
        runtime_fail("Android game Vulkan device enumeration failed");
    VkPhysicalDeviceProperties properties = {0};
    for (uint32_t i=0; i<count && !physical; ++i) {
        VkPhysicalDeviceProperties candidate_properties;
        vkGetPhysicalDeviceProperties(candidates[i], &candidate_properties);
        if (candidate_properties.apiVersion < VK_API_VERSION_1_3) continue;
        uint32_t families = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(candidates[i], &families, NULL);
        if (!families) continue;
        VkQueueFamilyProperties available[32];
        if (families > 32) families = 32;
        vkGetPhysicalDeviceQueueFamilyProperties(candidates[i], &families, available);
        for (uint32_t j=0; j<families; ++j) if (available[j].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
            VkBool32 present=VK_FALSE;
            if(surface && (vkGetPhysicalDeviceSurfaceSupportKHR(candidates[i],j,surface,&present)!=VK_SUCCESS || !present))
                continue;
            physical = candidates[i]; queue_family = j; properties = candidate_properties; break;
        }
    }
    if (!physical) runtime_fail("Android game needs a Vulkan 1.3 graphics queue");
    float priority = 1.0f;
    VkDeviceQueueCreateInfo q = {.sType=VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex=queue_family, .queueCount=1, .pQueuePriorities=&priority};
    /* Enable every feature the device supports except robustBufferAccess (cost):
       translated shaders may use any capability the hardware offers. */
    /* Optional: NVN may sample one slice of a 3D texture through a 2D view (seen with a full save loaded). */
    uint32_t next_count=0;VkExtensionProperties next_ext[512];int view2d3d=0, feedback_loop=0, local_read=0, xfb=0, eds1=0, eds2=0;
    const char *load_none=NULL;
    if(vkEnumerateDeviceExtensionProperties(physical,NULL,&next_count,NULL)==VK_SUCCESS) {
        if(next_count>512)next_count=512;
        if(vkEnumerateDeviceExtensionProperties(physical,NULL,&next_count,next_ext)>=0)
            for(uint32_t i=0;i<next_count;++i) {
                if(!strcmp(next_ext[i].extensionName,VK_EXT_IMAGE_2D_VIEW_OF_3D_EXTENSION_NAME))view2d3d=1;
                if(!strcmp(next_ext[i].extensionName,VK_EXT_ATTACHMENT_FEEDBACK_LOOP_LAYOUT_EXTENSION_NAME))feedback_loop=1;
                if(!strcmp(next_ext[i].extensionName,VK_KHR_DYNAMIC_RENDERING_LOCAL_READ_EXTENSION_NAME))local_read=1;
                if(!strcmp(next_ext[i].extensionName,VK_EXT_TRANSFORM_FEEDBACK_EXTENSION_NAME))xfb=1;
                if(!strcmp(next_ext[i].extensionName,VK_EXT_EXTENDED_DYNAMIC_STATE_EXTENSION_NAME))eds1=1;
                if(!strcmp(next_ext[i].extensionName,VK_EXT_EXTENDED_DYNAMIC_STATE_2_EXTENSION_NAME))eds2=1;
                if(!strcmp(next_ext[i].extensionName,VK_KHR_LOAD_STORE_OP_NONE_EXTENSION_NAME))load_none=VK_KHR_LOAD_STORE_OP_NONE_EXTENSION_NAME;
                if(!load_none&&!strcmp(next_ext[i].extensionName,VK_EXT_LOAD_STORE_OP_NONE_EXTENSION_NAME))load_none=VK_EXT_LOAD_STORE_OP_NONE_EXTENSION_NAME;
                if(!strcmp(next_ext[i].extensionName,"VK_KHR_maintenance8"))fprintf(stderr,"[android-gfx] VK_KHR_maintenance8 advertised (spec %u)\n",next_ext[i].specVersion);
            }
        fprintf(stderr,"[android-gfx] %u device extensions advertised\n",next_count);
    }
    /* Optional: a fragment shader reads its own pixel of a colour attachment (framebuffer fetch for decals, nvn_vk_draw.c). */
    VkPhysicalDeviceTransformFeedbackFeaturesEXT have_xfb={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TRANSFORM_FEEDBACK_FEATURES_EXT};
    VkPhysicalDeviceDynamicRenderingLocalReadFeaturesKHR have_lr={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_LOCAL_READ_FEATURES_KHR};
    VkPhysicalDeviceImage2DViewOf3DFeaturesEXT have_2d3d={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_2D_VIEW_OF_3D_FEATURES_EXT,.pNext=local_read?&have_lr:NULL};
    void *opt_next=nvk_optional_feature_chain(&have_2d3d,&have_lr,view2d3d,local_read);
    if(xfb){if(local_read)have_lr.pNext=&have_xfb;else if(view2d3d)have_2d3d.pNext=&have_xfb;else opt_next=&have_xfb;}
    VkPhysicalDeviceIndexTypeUint8FeaturesEXT have_u8={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_INDEX_TYPE_UINT8_FEATURES_EXT,
        .pNext=opt_next};
    VkPhysicalDeviceCustomBorderColorFeaturesEXT have_border={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CUSTOM_BORDER_COLOR_FEATURES_EXT,.pNext=&have_u8};
    VkPhysicalDeviceVertexInputDynamicStateFeaturesEXT have_vi={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_INPUT_DYNAMIC_STATE_FEATURES_EXT,.pNext=&have_border};
    VkPhysicalDeviceExtendedDynamicState2FeaturesEXT have_eds2={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_2_FEATURES_EXT,.pNext=&have_vi};
    VkPhysicalDeviceExtendedDynamicStateFeaturesEXT have_eds1={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_FEATURES_EXT,.pNext=&have_eds2};
    VkPhysicalDeviceVulkan13Features have13={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,.pNext=&have_eds1};
    VkPhysicalDeviceVulkan12Features have12={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,.pNext=&have13};
    VkPhysicalDeviceVulkan11Features have11={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES,.pNext=&have12};
    VkPhysicalDeviceFeatures2 have={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,.pNext=&have11};
    vkGetPhysicalDeviceFeatures2(physical,&have);
    nvk_xfb_storage_supported=have.features.vertexPipelineStoresAndAtomics;
    nvk_storage_alignment=properties.limits.minStorageBufferOffsetAlignment;
    nvk_storage_range_max=properties.limits.maxStorageBufferRange;
    VkPhysicalDeviceSubgroupProperties subgroup_ops={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES};
    VkPhysicalDeviceSubgroupSizeControlProperties subgroup={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_SIZE_CONTROL_PROPERTIES,.pNext=&subgroup_ops};
    VkPhysicalDeviceProperties2 subgroup_props={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,.pNext=&subgroup};
    vkGetPhysicalDeviceProperties2(physical,&subgroup_props);
    /* Maxwell SHFL/VOTE/ballot in a stage need SHUFFLE, BALLOT and VOTE there; there is no fallback. */
    fprintf(stderr,"[android-gfx] subgroup size=%u stages=0x%x (vs=%d fs=%d cs=%d) ops=0x%x (basic=%d vote=%d ballot=%d shuffle=%d shuffleRel=%d) quadAllStages=%d\n",
        subgroup_ops.subgroupSize,subgroup_ops.supportedStages,
        !!(subgroup_ops.supportedStages&VK_SHADER_STAGE_VERTEX_BIT),!!(subgroup_ops.supportedStages&VK_SHADER_STAGE_FRAGMENT_BIT),!!(subgroup_ops.supportedStages&VK_SHADER_STAGE_COMPUTE_BIT),
        subgroup_ops.supportedOperations,
        !!(subgroup_ops.supportedOperations&VK_SUBGROUP_FEATURE_BASIC_BIT),!!(subgroup_ops.supportedOperations&VK_SUBGROUP_FEATURE_VOTE_BIT),
        !!(subgroup_ops.supportedOperations&VK_SUBGROUP_FEATURE_BALLOT_BIT),!!(subgroup_ops.supportedOperations&VK_SUBGROUP_FEATURE_SHUFFLE_BIT),
        !!(subgroup_ops.supportedOperations&VK_SUBGROUP_FEATURE_SHUFFLE_RELATIVE_BIT),subgroup_ops.quadOperationsInAllStages);
    const char *subgroup_env=getenv("ZA_CS_SUBGROUP32");
    nvk_compute_subgroup_size=nvk_subgroup32_choice(have13.subgroupSizeControl,&subgroup,!(subgroup_env&&!strcmp(subgroup_env,"0")));
    fprintf(stderr,"[android-gfx] compute subgroup sizes %u..%u: %s; vertex storage writes=%d\n",
        subgroup.minSubgroupSize,subgroup.maxSubgroupSize,nvk_compute_subgroup_size?"request 32 lanes":"driver choice (shader warp emulation retained)",nvk_xfb_storage_supported);
    fprintf(stderr,"[android-gfx] buffer limits: uniform=%u storage=%u; offset alignment uniform=%llu storage=%llu; bufferDeviceAddress=%u\n",
        properties.limits.maxUniformBufferRange,properties.limits.maxStorageBufferRange,
        (unsigned long long)properties.limits.minUniformBufferOffsetAlignment,
        (unsigned long long)properties.limits.minStorageBufferOffsetAlignment,have12.bufferDeviceAddress);
    if(!have_vi.vertexInputDynamicState||!have_u8.indexTypeUint8||!have_border.customBorderColors||!have13.dynamicRendering)
        runtime_fail("Android game Vulkan device lacks a required feature (vertex input dynamic state, uint8 indices, custom border colours, dynamic rendering)");
    if(!have_eds1.extendedDynamicState||!have_eds2.extendedDynamicState2)
        runtime_fail("Android game Vulkan device lacks required extended dynamic state 1/2 features");
    VkPhysicalDeviceFeatures2 use=have;
    use.features.robustBufferAccess=VK_FALSE;
    const VkPhysicalDeviceFeatures *f=&have.features;    VkFormatProperties stencil8;
    vkGetPhysicalDeviceFormatProperties(physical,VK_FORMAT_S8_UINT,&stencil8);
    caps=(AndroidGfxCaps){.min_max_filtering=have12.samplerFilterMinmax,
        .astc=f->textureCompressionASTC_LDR,.bc=f->textureCompressionBC,
        .stencil8=!!(stencil8.optimalTilingFeatures&VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT),
        .geometry_shader=f->geometryShader,.tessellation=f->tessellationShader,.multi_viewport=f->multiViewport,
        .max_anisotropy=properties.limits.maxSamplerAnisotropy};
    fprintf(stderr,"[android-gfx] features: minmax=%d astc=%d bc=%d stencil8=%d geometry=%d tessellation=%d "
        "multiViewport=%d anisotropy=%.0f timeline=%d sync2=%d dynamicRendering=%d EDS1=%d EDS2=%d (all supported features enabled)\n",
        caps.min_max_filtering,caps.astc,caps.bc,caps.stencil8,caps.geometry_shader,caps.tessellation,
        caps.multi_viewport,caps.max_anisotropy,have12.timelineSemaphore,have13.synchronization2,
        have13.dynamicRendering,have_eds1.extendedDynamicState,have_eds2.extendedDynamicState2);
    VkDeviceCreateInfo device_create = {.sType=VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,.pNext=&use,
        .queueCreateInfoCount=1, .pQueueCreateInfos=&q};
    /* External memory fd: NVN pools map exported dma-bufs at game addresses. */
    const char *device_extensions[16]={VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,VK_EXT_VERTEX_INPUT_DYNAMIC_STATE_EXTENSION_NAME,
        VK_EXT_INDEX_TYPE_UINT8_EXTENSION_NAME,VK_EXT_CUSTOM_BORDER_COLOR_EXTENSION_NAME};
    uint32_t extension_count=4;
    if(surface)device_extensions[extension_count++]=VK_KHR_SWAPCHAIN_EXTENSION_NAME;
    android_vk_2d_view_of_3d=view2d3d&&have_2d3d.image2DViewOf3D&&have_2d3d.sampler2DViewOf3D;
    if(view2d3d)device_extensions[extension_count++]=VK_EXT_IMAGE_2D_VIEW_OF_3D_EXTENSION_NAME;
    android_vk_local_read=local_read&&have_lr.dynamicRenderingLocalRead;
    if(local_read)device_extensions[extension_count++]=VK_KHR_DYNAMIC_RENDERING_LOCAL_READ_EXTENSION_NAME;
    fprintf(stderr,"[android-gfx] dynamic rendering local read (framebuffer fetch): %s\n",android_vk_local_read?"supported":"unavailable");
    /* Optional: LOAD_OP_NONE for attachments a pass leaves alone (nvn_vk_backend.c, attachment use prediction). */
    android_vk_load_op_none=load_none!=NULL;
    if(load_none)device_extensions[extension_count++]=load_none;
    fprintf(stderr,"[android-gfx] load op none: %s\n",load_none?load_none:"unavailable");
    android_vk_transform_feedback=xfb&&have_xfb.transformFeedback;
    if(android_vk_transform_feedback)device_extensions[extension_count++]=VK_EXT_TRANSFORM_FEEDBACK_EXTENSION_NAME;
    fprintf(stderr,"[android-gfx] transform feedback: %s\n",android_vk_transform_feedback?"supported":"unavailable");
    if(eds1)device_extensions[extension_count++]=VK_EXT_EXTENDED_DYNAMIC_STATE_EXTENSION_NAME;
    if(eds2)device_extensions[extension_count++]=VK_EXT_EXTENDED_DYNAMIC_STATE_2_EXTENSION_NAME;
    fprintf(stderr,"[android-gfx] extended dynamic state: EDS1 %s, EDS2 %s\n",
        eds1?"advertised":"core only",eds2?"advertised":"core only");
    /* Game uniform buffers as dynamic uniform buffers (descriptor reuse, nvn_vk_shader.c). TOTK_VK_UBO_DYNAMIC=0: none. */
    const char *dyn_env=getenv("TOTK_VK_UBO_DYNAMIC");
    nvk_dyn_ubo_limit=dyn_env&&!strcmp(dyn_env,"0")?0:(int)properties.limits.maxDescriptorSetUniformBuffersDynamic;
    fprintf(stderr,"[android-gfx] dynamic uniform buffers per pipeline layout: %d%s\n",nvk_dyn_ubo_limit,dyn_env&&!strcmp(dyn_env,"0")?" (TOTK_VK_UBO_DYNAMIC=0)":"");
    fprintf(stderr,"[android-gfx] 2D views of 3D images: %s\n",android_vk_2d_view_of_3d?"supported":"unavailable");
    fprintf(stderr,"[android-gfx] attachment feedback loop layout: %s\n",feedback_loop?"advertised":"unavailable");
    device_create.enabledExtensionCount=extension_count;
    device_create.ppEnabledExtensionNames=device_extensions;
    if (vkCreateDevice(physical, &device_create, NULL, &device) != VK_SUCCESS)
        runtime_fail("Android game Vulkan logical device creation failed");
    vkGetDeviceQueue(device, queue_family, 0, &queue);
    if (!queue) runtime_fail("Android game Vulkan graphics queue missing");
    handles=(AndroidGfxVulkan){instance,physical,device,queue,queue_family,surface};
    android_nvn_host_install();
    fprintf(stderr, "[android-gfx] Vulkan device ready: %s, API %u.%u, queue %u; work=%lu bytes, allocators=%d/%d, Android surface=%d\n",
        properties.deviceName, VK_API_VERSION_MAJOR(properties.apiVersion),
        VK_API_VERSION_MINOR(properties.apiVersion), queue_family, size,
        game_allocator.alloc != NULL, devtools_allocator.alloc != NULL,surface!=VK_NULL_HANDLE);
}

void *runtime_gfx_override(const char *name) {
    if (!strcmp(name, "_ZN2nv20SetGraphicsAllocatorEPFPvmmS0_EPFvS0_S0_EPFS0_S0_mS0_ES0_")) return graphics_allocator;
    if (!strcmp(name, "_ZN2nv28SetGraphicsDevtoolsAllocatorEPFPvmmS0_EPFvS0_S0_EPFS0_S0_mS0_ES0_")) return graphics_devtools_allocator;
    if (!strcmp(name, "_ZN2nv18InitializeGraphicsEPvm")) return graphics_initialize;
    /* The shared NVN layer (hle_nvn.c) registers nvnBootstrapLoader itself. */
    if (!strcmp(name, "nvnBootstrapLoader")) return android_nvn_force_override(name);
    return NULL;
}
