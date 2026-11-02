#pragma once
#include <vulkan/vulkan.h>

static inline uint32_t nvk_subgroup32_choice(VkBool32 supported,
        const VkPhysicalDeviceSubgroupSizeControlProperties *p, int enabled) {
    return enabled && supported && p->minSubgroupSize <= 32 && p->maxSubgroupSize >= 32 &&
        (p->requiredSubgroupSizeStages & VK_SHADER_STAGE_COMPUTE_BIT) ? 32 : 0;
}

/* Optional features are independent: lack of 2D-of-3D must not hide local reads. */
static inline void *nvk_optional_feature_chain(VkPhysicalDeviceImage2DViewOf3DFeaturesEXT *view,
        VkPhysicalDeviceDynamicRenderingLocalReadFeaturesKHR *local, int has_view, int has_local) {
    local->pNext = NULL;
    view->pNext = has_local ? (void *)local : NULL;
    return has_view ? (void *)view : has_local ? (void *)local : NULL;
}
