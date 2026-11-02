/* Offline execution of the existing Z-A skinning shader with synthetic fixtures.
 * Requires the raster wrapper produced by tools/skin_compute_probe.py.
 * Uses real Vulkan vertex fetching and a float render target; no Android replay or transform feedback. */
#include <vulkan/vulkan.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>
typedef struct { uint32_t joints[4]; float weights[4], normal[4], tangent[4], position[4]; } InputVertex;
typedef struct { float position[4]; } OutputVertex;
_Static_assert(sizeof(InputVertex)==80, "GLSL std430 input layout");
_Static_assert(sizeof(OutputVertex)==16, "GLSL std430 output layout");
static VkDescriptorType descriptor_type(int i) { return i<3 ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; }

#define CHECK(call) do { VkResult r=(call); if(r!=VK_SUCCESS) { fprintf(stderr,"%s: %d\n",#call,r); return 1; } } while(0)
int main(int argc, char **argv) {
    if(argc!=3) { fprintf(stderr,"usage: skinning_raster_probe skinning.vert.spv position.frag.spv\n"); return 2; }
    FILE *f=fopen(argv[1],"rb"); if(!f) return 2;
    fseek(f,0,SEEK_END); long shader_bytes=ftell(f); rewind(f);
    uint32_t *shader=malloc((size_t)shader_bytes);
    if(shader_bytes<=0 || fread(shader,1,(size_t)shader_bytes,f)!=(size_t)shader_bytes) return 2;
    fclose(f);
    f=fopen(argv[2],"rb"); if(!f) return 2;
    fseek(f,0,SEEK_END); long fragment_bytes=ftell(f); rewind(f);
    uint32_t *fragment=malloc((size_t)fragment_bytes);
    if(fragment_bytes<=0 || fread(fragment,1,(size_t)fragment_bytes,f)!=(size_t)fragment_bytes) return 2;
    fclose(f);
    VkInstance instance;
    VkApplicationInfo app={.sType=VK_STRUCTURE_TYPE_APPLICATION_INFO,.apiVersion=VK_API_VERSION_1_1};
    VkInstanceCreateInfo ici={.sType=VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,.pApplicationInfo=&app};
    CHECK(vkCreateInstance(&ici,0,&instance));
    uint32_t n=1; VkPhysicalDevice physical; CHECK(vkEnumeratePhysicalDevices(instance,&n,&physical));
    VkPhysicalDeviceProperties properties; vkGetPhysicalDeviceProperties(physical,&properties);
    uint32_t nq=0; vkGetPhysicalDeviceQueueFamilyProperties(physical,&nq,0);
    VkQueueFamilyProperties *queues=calloc(nq,sizeof(*queues)); vkGetPhysicalDeviceQueueFamilyProperties(physical,&nq,queues);
    uint32_t family=0; while(family<nq && !(queues[family].queueFlags&VK_QUEUE_GRAPHICS_BIT)) family++;
    if(family==nq) return 2;
    float priority=1; VkDeviceQueueCreateInfo qi={.sType=VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,.queueFamilyIndex=family,.queueCount=1,.pQueuePriorities=&priority};
    VkDeviceCreateInfo di={.sType=VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,.queueCreateInfoCount=1,.pQueueCreateInfos=&qi};
    VkDevice dev; CHECK(vkCreateDevice(physical,&di,0,&dev)); VkQueue queue; vkGetDeviceQueue(dev,family,0,&queue);
    VkPhysicalDeviceMemoryProperties mp; vkGetPhysicalDeviceMemoryProperties(physical,&mp);
    enum { VERTICES=256, TOTAL=16384 };
    VkBuffer buffers[5]; VkDeviceMemory memory[5]; uint32_t *maps[5];
    for(int i=0;i<5;i++) {
        VkBufferCreateInfo bi={.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,.size=TOTAL*4,.usage=VK_BUFFER_USAGE_STORAGE_BUFFER_BIT|VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT|VK_BUFFER_USAGE_VERTEX_BUFFER_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT};
        CHECK(vkCreateBuffer(dev,&bi,0,&buffers[i])); VkMemoryRequirements req; vkGetBufferMemoryRequirements(dev,buffers[i],&req);
        uint32_t type=0; while(type<mp.memoryTypeCount && (!(req.memoryTypeBits&(1u<<type)) || (mp.memoryTypes[type].propertyFlags&(VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))!=(VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))) type++;
        if(type==mp.memoryTypeCount) return 3;
        VkMemoryAllocateInfo ai={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,.allocationSize=req.size,.memoryTypeIndex=type};
        CHECK(vkAllocateMemory(dev,&ai,0,&memory[i])); CHECK(vkBindBufferMemory(dev,buffers[i],memory[i],0)); CHECK(vkMapMemory(dev,memory[i],0,VK_WHOLE_SIZE,0,(void**)&maps[i]));
        for(int j=0;j<TOTAL;j++) maps[i][j]=0;
    }
    InputVertex vertices[VERTICES]; memset(vertices,0,sizeof vertices);
    for(int j=0;j<VERTICES;j++) {
        InputVertex *v=&vertices[j];
        for(int k=0;k<4;k++) v->joints[k]=(j*7+k*103)%256;
        if(j%4==0) { v->weights[0]=1; }
        else if(j%4==1) { v->weights[0]=0.25f; v->weights[1]=0.75f; }
        else { v->weights[0]=0.1f; v->weights[1]=0.2f; v->weights[2]=0.3f; v->weights[3]=0.4f; }
        v->position[0]=(j%13-6)*0.125f; v->position[1]=(j%17)*0.1f; v->position[2]=(j%7-3)*0.25f; v->position[3]=1;
        v->normal[1]=1; v->tangent[0]=1; v->tangent[3]=1;
    }
    vertices[VERTICES-1].joints[0]=255;
    vertices[VERTICES-1].weights[0]=1;
    vertices[VERTICES-1].weights[1]=vertices[VERTICES-1].weights[2]=vertices[VERTICES-1].weights[3]=0;
    /* Exact 48-byte layout in the saved Z-A TF trace: float3 position,
       half4 normal/tangent, u8x4 joint indices and unorm16x4 weights. */
    for(int j=0;j<VERTICES;j++) {
        uint8_t *raw=(uint8_t*)maps[3]+j*48;
        memcpy(raw,vertices[j].position,12);
        uint16_t normal[4]={0,0x3c00,0,0}, tangent[4]={0x3c00,0,0,0x3c00};
        memcpy(raw+12,normal,8); memcpy(raw+20,tangent,8);
        for(int k=0;k<4;k++) {
            raw[36+k]=(uint8_t)vertices[j].joints[k];
            uint16_t w=(uint16_t)lroundf(vertices[j].weights[k]*65535.0f);
            memcpy(raw+40+k*2,&w,2); vertices[j].weights[k]=(float)w/65535.0f;
        }
    }
    VkDescriptorSetLayoutBinding bindings[3]; for(int i=0;i<3;i++) bindings[i]=(VkDescriptorSetLayoutBinding){.binding=i,.descriptorType=VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,.descriptorCount=1,.stageFlags=VK_SHADER_STAGE_VERTEX_BIT};
    VkDescriptorSetLayoutCreateInfo si={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,.bindingCount=3,.pBindings=bindings};
    VkDescriptorSetLayout sl; CHECK(vkCreateDescriptorSetLayout(dev,&si,0,&sl));
    VkPipelineLayoutCreateInfo li={.sType=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,.setLayoutCount=1,.pSetLayouts=&sl,.pushConstantRangeCount=0,.pPushConstantRanges=0};
    VkPipelineLayout layout; CHECK(vkCreatePipelineLayout(dev,&li,0,&layout));
    VkShaderModuleCreateInfo mi={.sType=VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,.codeSize=(size_t)shader_bytes,.pCode=shader}; VkShaderModule module; CHECK(vkCreateShaderModule(dev,&mi,0,&module));
    VkShaderModuleCreateInfo fmi={.sType=VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,.codeSize=(size_t)fragment_bytes,.pCode=fragment};
    VkShaderModule fragment_module; CHECK(vkCreateShaderModule(dev,&fmi,0,&fragment_module));
    VkImageCreateInfo image_info={.sType=VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,.imageType=VK_IMAGE_TYPE_2D,.format=VK_FORMAT_R32G32B32A32_SFLOAT,.extent={VERTICES,1,1},.mipLevels=1,.arrayLayers=1,.samples=VK_SAMPLE_COUNT_1_BIT,.tiling=VK_IMAGE_TILING_OPTIMAL,.usage=VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT};
    VkImage image; CHECK(vkCreateImage(dev,&image_info,0,&image));
    VkMemoryRequirements ir; vkGetImageMemoryRequirements(dev,image,&ir); uint32_t it=0; while(it<mp.memoryTypeCount && !(ir.memoryTypeBits&(1u<<it))) it++;
    if(it==mp.memoryTypeCount) return 3;
    VkMemoryAllocateInfo ima={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,.allocationSize=ir.size,.memoryTypeIndex=it};
    VkDeviceMemory image_memory; CHECK(vkAllocateMemory(dev,&ima,0,&image_memory)); CHECK(vkBindImageMemory(dev,image,image_memory,0));
    VkImageViewCreateInfo ivi={.sType=VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,.image=image,.viewType=VK_IMAGE_VIEW_TYPE_2D,.format=image_info.format,.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}};
    VkImageView view; CHECK(vkCreateImageView(dev,&ivi,0,&view));
    VkAttachmentDescription attachment={.format=image_info.format,.samples=VK_SAMPLE_COUNT_1_BIT,.loadOp=VK_ATTACHMENT_LOAD_OP_CLEAR,.storeOp=VK_ATTACHMENT_STORE_OP_STORE,.stencilLoadOp=VK_ATTACHMENT_LOAD_OP_DONT_CARE,.stencilStoreOp=VK_ATTACHMENT_STORE_OP_DONT_CARE,.initialLayout=VK_IMAGE_LAYOUT_UNDEFINED,.finalLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL};
    VkAttachmentReference reference={0,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass={.pipelineBindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS,.colorAttachmentCount=1,.pColorAttachments=&reference};
    VkSubpassDependency dep[2]={{.srcSubpass=VK_SUBPASS_EXTERNAL,.dstSubpass=0,.srcStageMask=VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,.dstStageMask=VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,.dstAccessMask=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT},
        {.srcSubpass=0,.dstSubpass=VK_SUBPASS_EXTERNAL,.srcStageMask=VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,.dstStageMask=VK_PIPELINE_STAGE_TRANSFER_BIT,.srcAccessMask=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT}};
    VkRenderPassCreateInfo rpi={.sType=VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,.attachmentCount=1,.pAttachments=&attachment,.subpassCount=1,.pSubpasses=&subpass,.dependencyCount=2,.pDependencies=dep};
    VkRenderPass renderpass; CHECK(vkCreateRenderPass(dev,&rpi,0,&renderpass));
    VkFramebufferCreateInfo fi={.sType=VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,.renderPass=renderpass,.attachmentCount=1,.pAttachments=&view,.width=VERTICES,.height=1,.layers=1};
    VkFramebuffer framebuffer; CHECK(vkCreateFramebuffer(dev,&fi,0,&framebuffer));
    VkPipelineShaderStageCreateInfo stages[2]={{.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,.stage=VK_SHADER_STAGE_VERTEX_BIT,.module=module,.pName="main"}, {.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,.stage=VK_SHADER_STAGE_FRAGMENT_BIT,.module=fragment_module,.pName="main"}};
    VkVertexInputBindingDescription binding={0,48,VK_VERTEX_INPUT_RATE_VERTEX};
    VkVertexInputAttributeDescription attributes[5]={{0,0,VK_FORMAT_R32G32B32_SFLOAT,0},{1,0,VK_FORMAT_R16G16B16A16_SFLOAT,12},{3,0,VK_FORMAT_R16G16B16A16_SFLOAT,20},{12,0,VK_FORMAT_R8G8B8A8_UINT,36},{13,0,VK_FORMAT_R16G16B16A16_UNORM,40}};
    VkPipelineVertexInputStateCreateInfo vi={.sType=VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,.vertexBindingDescriptionCount=1,.pVertexBindingDescriptions=&binding,.vertexAttributeDescriptionCount=5,.pVertexAttributeDescriptions=attributes};
    VkPipelineInputAssemblyStateCreateInfo ia={.sType=VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,.topology=VK_PRIMITIVE_TOPOLOGY_POINT_LIST};
    VkViewport viewport={0,0,VERTICES,1,0,1}; VkRect2D scissor={{0,0},{VERTICES,1}};
    VkPipelineViewportStateCreateInfo vp={.sType=VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,.viewportCount=1,.pViewports=&viewport,.scissorCount=1,.pScissors=&scissor};
    VkPipelineRasterizationStateCreateInfo rs={.sType=VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,.polygonMode=VK_POLYGON_MODE_FILL,.cullMode=VK_CULL_MODE_NONE,.frontFace=VK_FRONT_FACE_COUNTER_CLOCKWISE,.lineWidth=1};
    VkPipelineMultisampleStateCreateInfo ms={.sType=VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,.rasterizationSamples=VK_SAMPLE_COUNT_1_BIT};
    VkPipelineColorBlendAttachmentState blend_attachment={.colorWriteMask=15}; VkPipelineColorBlendStateCreateInfo blend={.sType=VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,.attachmentCount=1,.pAttachments=&blend_attachment};
    VkGraphicsPipelineCreateInfo ci={.sType=VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,.stageCount=2,.pStages=stages,.pVertexInputState=&vi,.pInputAssemblyState=&ia,.pViewportState=&vp,.pRasterizationState=&rs,.pMultisampleState=&ms,.pColorBlendState=&blend,.layout=layout,.renderPass=renderpass};
    VkPipeline pipe; CHECK(vkCreateGraphicsPipelines(dev,VK_NULL_HANDLE,1,&ci,0,&pipe));
    VkDescriptorPoolSize size={VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,3}; VkDescriptorPoolCreateInfo dpi={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,.maxSets=1,.poolSizeCount=1,.pPoolSizes=&size}; VkDescriptorPool pool; CHECK(vkCreateDescriptorPool(dev,&dpi,0,&pool));
    VkDescriptorSetAllocateInfo dai={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,.descriptorPool=pool,.descriptorSetCount=1,.pSetLayouts=&sl}; VkDescriptorSet set; CHECK(vkAllocateDescriptorSets(dev,&dai,&set));
    VkDescriptorBufferInfo bi[3]; VkWriteDescriptorSet writes[3]; for(int i=0;i<3;i++) { bi[i]=(VkDescriptorBufferInfo){buffers[i],0,TOTAL*4}; writes[i]=(VkWriteDescriptorSet){.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,.dstSet=set,.dstBinding=i,.descriptorCount=1,.descriptorType=VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,.pBufferInfo=&bi[i]}; } vkUpdateDescriptorSets(dev,3,writes,0,0);
    VkCommandPoolCreateInfo cpi={.sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,.queueFamilyIndex=family,.flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT}; VkCommandPool cp; CHECK(vkCreateCommandPool(dev,&cpi,0,&cp));
    VkCommandBufferAllocateInfo cai={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,.commandPool=cp,.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY,.commandBufferCount=1}; VkCommandBuffer cb; CHECK(vkAllocateCommandBuffers(dev,&cai,&cb));
    int errors=0; float worst=0;
    for(int phase=0;phase<4;phase++) {
        float *bones=(float*)maps[1];
        memset(bones,0,TOTAL*4); memset(maps[4],0xcd,TOTAL*4);
        for(int b=0;b<512;b++) {
            float *m=bones+b*16;
            float angle=phase>=2 ? (b%11-5)*0.07f*(phase-1) : 0;
            float scale=phase>=2 ? 0.7f+(b%7)*0.1f : 1;
            float c=cosf(angle)*scale,s=sinf(angle)*scale;
            m[0]=c; m[1]=s; m[4]=-s; m[5]=c; m[10]=scale; m[15]=1;
            if(phase) { m[12]=(b%13-6)*0.15f*phase; m[13]=(b%5-2)*0.2f*phase; m[14]=(b%9-4)*0.12f*phase; }
        }
        CHECK(vkResetCommandBuffer(cb,0));
        VkCommandBufferBeginInfo begin={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; CHECK(vkBeginCommandBuffer(cb,&begin));
        VkClearValue clear={.color={{0,0,0,0}}}; VkRenderPassBeginInfo rbegin={.sType=VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,.renderPass=renderpass,.framebuffer=framebuffer,.renderArea={{0,0},{VERTICES,1}},.clearValueCount=1,.pClearValues=&clear};
        vkCmdBeginRenderPass(cb,&rbegin,VK_SUBPASS_CONTENTS_INLINE);
        vkCmdBindPipeline(cb,VK_PIPELINE_BIND_POINT_GRAPHICS,pipe); vkCmdBindDescriptorSets(cb,VK_PIPELINE_BIND_POINT_GRAPHICS,layout,0,1,&set,0,0);
        VkDeviceSize offset=0; vkCmdBindVertexBuffers(cb,0,1,&buffers[3],&offset); vkCmdDraw(cb,VERTICES,1,0,0); vkCmdEndRenderPass(cb);
        VkBufferImageCopy copy={.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},.imageExtent={VERTICES,1,1}};
        vkCmdCopyImageToBuffer(cb,image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,buffers[4],1,&copy);
        VkMemoryBarrier barrier={.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER,.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT,.dstAccessMask=VK_ACCESS_HOST_READ_BIT}; vkCmdPipelineBarrier(cb,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&barrier,0,0,0,0);
        CHECK(vkEndCommandBuffer(cb)); VkSubmitInfo submit={.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO,.commandBufferCount=1,.pCommandBuffers=&cb}; CHECK(vkQueueSubmit(queue,1,&submit,VK_NULL_HANDLE)); CHECK(vkQueueWaitIdle(queue));
        OutputVertex *out=(OutputVertex*)maps[4];
        for(int j=0;j<VERTICES;j++) for(int axis=0;axis<3;axis++) {
            const InputVertex *v=&vertices[j]; double expected=0;
            for(int k=0;k<4;k++) {
                const float *m=bones+v->joints[k]*16;
                double value=m[axis+12]; for(int a=0;a<3;a++) value+=(double)m[a*4+axis]*v->position[a];
                expected+=value*v->weights[k];
            }
            float delta=fabsf(out[j].position[axis]-(float)expected); if(delta>worst) worst=delta;
            if(!isfinite(out[j].position[axis]) || delta>0.00005f) {
                if(errors++<8) fprintf(stderr,"phase %d vertex %d axis %d: %.9g expected %.9g\n",phase,j,axis,out[j].position[axis],expected);
            }
        }
    }
    printf("%s on %s: 1024 skinned vertices, identity/translation/rotation/scale, 48-byte vertex fetch (float3, half4, uint8 joints, unorm16 weights), 1/2/4 bone weights, joint 255, four updates of same UBO; max position error %.9g (%d errors)\n",errors?"FAIL":"PASS",properties.deviceName,worst,errors);
    vkDeviceWaitIdle(dev); for(int i=0;i<5;i++) { vkUnmapMemory(dev,memory[i]); vkDestroyBuffer(dev,buffers[i],0); vkFreeMemory(dev,memory[i],0); }
    vkDestroyCommandPool(dev,cp,0); vkDestroyFramebuffer(dev,framebuffer,0); vkDestroyRenderPass(dev,renderpass,0); vkDestroyImageView(dev,view,0); vkDestroyImage(dev,image,0); vkFreeMemory(dev,image_memory,0); vkDestroyShaderModule(dev,fragment_module,0); vkDestroyDescriptorPool(dev,pool,0); vkDestroyPipeline(dev,pipe,0); vkDestroyShaderModule(dev,module,0); vkDestroyPipelineLayout(dev,layout,0); vkDestroyDescriptorSetLayout(dev,sl,0); vkDestroyDevice(dev,0); vkDestroyInstance(instance,0); free(queues); free(shader); free(fragment); return errors?1:0;
}
