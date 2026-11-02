/* Real Vulkan test: GPU merge must preserve newer CPU words and buffer guards. */
#include <vulkan/vulkan.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "shaders/storage_merge_spv.h"
#define CHECK(call) do { VkResult r=(call); if(r!=VK_SUCCESS) { fprintf(stderr,"%s: %d\n",#call,r); return 1; } } while(0)
int main(void) {
    VkInstance instance;
    VkApplicationInfo app={.sType=VK_STRUCTURE_TYPE_APPLICATION_INFO,.apiVersion=VK_API_VERSION_1_1};
    VkInstanceCreateInfo ici={.sType=VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,.pApplicationInfo=&app};
    CHECK(vkCreateInstance(&ici,0,&instance));
    uint32_t n=1; VkPhysicalDevice physical; CHECK(vkEnumeratePhysicalDevices(instance,&n,&physical));
    VkPhysicalDeviceProperties properties; vkGetPhysicalDeviceProperties(physical,&properties);
    uint32_t nq=0; vkGetPhysicalDeviceQueueFamilyProperties(physical,&nq,0);
    VkQueueFamilyProperties *queues=calloc(nq,sizeof(*queues)); vkGetPhysicalDeviceQueueFamilyProperties(physical,&nq,queues);
    uint32_t family=0; while(family<nq && !(queues[family].queueFlags&VK_QUEUE_COMPUTE_BIT)) family++;
    if(family==nq) return 2;
    float priority=1; VkDeviceQueueCreateInfo qi={.sType=VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,.queueFamilyIndex=family,.queueCount=1,.pQueuePriorities=&priority};
    VkDeviceCreateInfo di={.sType=VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,.queueCreateInfoCount=1,.pQueueCreateInfos=&qi};
    VkDevice dev; CHECK(vkCreateDevice(physical,&di,0,&dev)); VkQueue queue; vkGetDeviceQueue(dev,family,0,&queue);
    VkPhysicalDeviceMemoryProperties mp; vkGetPhysicalDeviceMemoryProperties(physical,&mp);
    enum { WORDS=515, OFFSET=4, TOTAL=WORDS+OFFSET+7 };
    VkBuffer buffers[3]; VkDeviceMemory memory[3]; uint32_t *maps[3];
    for(int i=0;i<3;i++) {
        VkBufferCreateInfo bi={.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,.size=TOTAL*4,.usage=VK_BUFFER_USAGE_STORAGE_BUFFER_BIT};
        CHECK(vkCreateBuffer(dev,&bi,0,&buffers[i])); VkMemoryRequirements req; vkGetBufferMemoryRequirements(dev,buffers[i],&req);
        uint32_t type=0; while(type<mp.memoryTypeCount && (!(req.memoryTypeBits&(1u<<type)) || (mp.memoryTypes[type].propertyFlags&(VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))!=(VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))) type++;
        if(type==mp.memoryTypeCount) return 3;
        VkMemoryAllocateInfo ai={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,.allocationSize=req.size,.memoryTypeIndex=type};
        CHECK(vkAllocateMemory(dev,&ai,0,&memory[i])); CHECK(vkBindBufferMemory(dev,buffers[i],memory[i],0)); CHECK(vkMapMemory(dev,memory[i],0,VK_WHOLE_SIZE,0,(void**)&maps[i]));
        for(int j=0;j<TOTAL;j++) maps[i][j]=0xcafebabe;
    }
    for(int j=0;j<WORDS;j++) { maps[0][OFFSET+j]=10000+j; maps[2][j]=j; maps[1][j]= j%3 ? j : 777+j; }
    VkDescriptorSetLayoutBinding bindings[3]; for(int i=0;i<3;i++) bindings[i]=(VkDescriptorSetLayoutBinding){.binding=i,.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,.descriptorCount=1,.stageFlags=VK_SHADER_STAGE_COMPUTE_BIT};
    VkDescriptorSetLayoutCreateInfo si={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,.bindingCount=3,.pBindings=bindings};
    VkDescriptorSetLayout sl; CHECK(vkCreateDescriptorSetLayout(dev,&si,0,&sl));
    VkPushConstantRange pc={VK_SHADER_STAGE_COMPUTE_BIT,0,8}; VkPipelineLayoutCreateInfo li={.sType=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,.setLayoutCount=1,.pSetLayouts=&sl,.pushConstantRangeCount=1,.pPushConstantRanges=&pc};
    VkPipelineLayout layout; CHECK(vkCreatePipelineLayout(dev,&li,0,&layout));
    VkShaderModuleCreateInfo mi={.sType=VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,.codeSize=sizeof storage_merge_spv,.pCode=storage_merge_spv}; VkShaderModule module; CHECK(vkCreateShaderModule(dev,&mi,0,&module));
    VkComputePipelineCreateInfo ci={.sType=VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,.layout=layout,.stage={.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,.stage=VK_SHADER_STAGE_COMPUTE_BIT,.module=module,.pName="main"}};
    VkPipeline pipe; CHECK(vkCreateComputePipelines(dev,VK_NULL_HANDLE,1,&ci,0,&pipe));
    VkDescriptorPoolSize size={VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,3}; VkDescriptorPoolCreateInfo dpi={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,.maxSets=1,.poolSizeCount=1,.pPoolSizes=&size}; VkDescriptorPool pool; CHECK(vkCreateDescriptorPool(dev,&dpi,0,&pool));
    VkDescriptorSetAllocateInfo dai={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,.descriptorPool=pool,.descriptorSetCount=1,.pSetLayouts=&sl}; VkDescriptorSet set; CHECK(vkAllocateDescriptorSets(dev,&dai,&set));
    VkDescriptorBufferInfo bi[3]; VkWriteDescriptorSet writes[3]; for(int i=0;i<3;i++) { bi[i]=(VkDescriptorBufferInfo){buffers[i],0,TOTAL*4}; writes[i]=(VkWriteDescriptorSet){.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,.dstSet=set,.dstBinding=i,.descriptorCount=1,.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,.pBufferInfo=&bi[i]}; } vkUpdateDescriptorSets(dev,3,writes,0,0);
    VkCommandPoolCreateInfo cpi={.sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,.queueFamilyIndex=family}; VkCommandPool cp; CHECK(vkCreateCommandPool(dev,&cpi,0,&cp));
    VkCommandBufferAllocateInfo cai={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,.commandPool=cp,.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY,.commandBufferCount=1}; VkCommandBuffer cb; CHECK(vkAllocateCommandBuffers(dev,&cai,&cb));
    VkCommandBufferBeginInfo begin={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; CHECK(vkBeginCommandBuffer(cb,&begin));
    vkCmdBindPipeline(cb,VK_PIPELINE_BIND_POINT_COMPUTE,pipe); vkCmdBindDescriptorSets(cb,VK_PIPELINE_BIND_POINT_COMPUTE,layout,0,1,&set,0,0);
    uint32_t push[2]={OFFSET,WORDS}; vkCmdPushConstants(cb,layout,VK_SHADER_STAGE_COMPUTE_BIT,0,8,push); vkCmdDispatch(cb,(WORDS+255)/256,1,1);
    VkMemoryBarrier barrier={.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER,.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT,.dstAccessMask=VK_ACCESS_HOST_READ_BIT}; vkCmdPipelineBarrier(cb,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&barrier,0,0,0,0);
    CHECK(vkEndCommandBuffer(cb)); VkSubmitInfo submit={.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO,.commandBufferCount=1,.pCommandBuffers=&cb}; CHECK(vkQueueSubmit(queue,1,&submit,VK_NULL_HANDLE)); CHECK(vkQueueWaitIdle(queue));
    int errors=0; for(int j=0;j<TOTAL;j++) { uint32_t expected=0xcafebabe; if(j>=OFFSET && j<OFFSET+WORDS) { int k=j-OFFSET; expected=k%3 ? 10000+k : 777+k; } if(maps[0][j]!=expected) { if(errors++<8) fprintf(stderr,"word %d: %u expected %u\n",j,maps[0][j],expected); } }
    printf("%s on %s: 515 words, preserved CPU updates, merged GPU updates, offset and tail guards (%d errors)\n",errors?"FAIL":"PASS",properties.deviceName,errors);
    vkDeviceWaitIdle(dev); for(int i=0;i<3;i++) { vkUnmapMemory(dev,memory[i]); vkDestroyBuffer(dev,buffers[i],0); vkFreeMemory(dev,memory[i],0); }
    vkDestroyCommandPool(dev,cp,0); vkDestroyDescriptorPool(dev,pool,0); vkDestroyPipeline(dev,pipe,0); vkDestroyShaderModule(dev,module,0); vkDestroyPipelineLayout(dev,layout,0); vkDestroyDescriptorSetLayout(dev,sl,0); vkDestroyDevice(dev,0); vkDestroyInstance(instance,0); free(queues); return errors?1:0;
}
