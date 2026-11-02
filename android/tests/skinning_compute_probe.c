/* Offline execution of the existing Z-A skinning shader with synthetic fixtures.
 * Requires the compute wrapper produced by tools/skin_compute_probe.py.
 * Uses real Vulkan; does not exercise the Android replay or transform feedback. */
#include <vulkan/vulkan.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>
typedef struct { uint32_t joints[4]; float weights[4], normal[4], tangent[4], position[4]; } InputVertex;
typedef struct { float position[4]; uint32_t packed[4]; } OutputVertex;
_Static_assert(sizeof(InputVertex)==80, "GLSL std430 input layout");
_Static_assert(sizeof(OutputVertex)==32, "GLSL std430 output layout");
static VkDescriptorType descriptor_type(int i) { return i<3 ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; }

#define CHECK(call) do { VkResult r=(call); if(r!=VK_SUCCESS) { fprintf(stderr,"%s: %d\n",#call,r); return 1; } } while(0)
int main(int argc, char **argv) {
    if(argc!=2) { fprintf(stderr,"usage: skinning_compute_probe skinning.comp.spv\n"); return 2; }
    FILE *f=fopen(argv[1],"rb"); if(!f) return 2;
    fseek(f,0,SEEK_END); long shader_bytes=ftell(f); rewind(f);
    uint32_t *shader=malloc((size_t)shader_bytes);
    if(shader_bytes<=0 || fread(shader,1,(size_t)shader_bytes,f)!=(size_t)shader_bytes) return 2;
    fclose(f);
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
    enum { VERTICES=256, TOTAL=16384 };
    VkBuffer buffers[5]; VkDeviceMemory memory[5]; uint32_t *maps[5];
    for(int i=0;i<5;i++) {
        VkBufferCreateInfo bi={.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,.size=TOTAL*4,.usage=VK_BUFFER_USAGE_STORAGE_BUFFER_BIT|VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT};
        CHECK(vkCreateBuffer(dev,&bi,0,&buffers[i])); VkMemoryRequirements req; vkGetBufferMemoryRequirements(dev,buffers[i],&req);
        uint32_t type=0; while(type<mp.memoryTypeCount && (!(req.memoryTypeBits&(1u<<type)) || (mp.memoryTypes[type].propertyFlags&(VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))!=(VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))) type++;
        if(type==mp.memoryTypeCount) return 3;
        VkMemoryAllocateInfo ai={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,.allocationSize=req.size,.memoryTypeIndex=type};
        CHECK(vkAllocateMemory(dev,&ai,0,&memory[i])); CHECK(vkBindBufferMemory(dev,buffers[i],memory[i],0)); CHECK(vkMapMemory(dev,memory[i],0,VK_WHOLE_SIZE,0,(void**)&maps[i]));
        for(int j=0;j<TOTAL;j++) maps[i][j]=0;
    }
    InputVertex *vertices=(InputVertex*)maps[3];
    for(int j=0;j<VERTICES;j++) {
        InputVertex *v=&vertices[j];
        for(int k=0;k<4;k++) v->joints[k]=(j*7+k*103)%512;
        if(j%4==0) { v->weights[0]=1; }
        else if(j%4==1) { v->weights[0]=0.25f; v->weights[1]=0.75f; }
        else { v->weights[0]=0.1f; v->weights[1]=0.2f; v->weights[2]=0.3f; v->weights[3]=0.4f; }
        v->position[0]=(j%13-6)*0.125f; v->position[1]=(j%17)*0.1f; v->position[2]=(j%7-3)*0.25f; v->position[3]=1;
        v->normal[1]=1; v->tangent[0]=1; v->tangent[3]=1;
    }
    vertices[VERTICES-1].joints[0]=511;
    vertices[VERTICES-1].weights[0]=1;
    vertices[VERTICES-1].weights[1]=vertices[VERTICES-1].weights[2]=vertices[VERTICES-1].weights[3]=0;
    VkDescriptorSetLayoutBinding bindings[5]; for(int i=0;i<5;i++) bindings[i]=(VkDescriptorSetLayoutBinding){.binding=i,.descriptorType=descriptor_type(i),.descriptorCount=1,.stageFlags=VK_SHADER_STAGE_COMPUTE_BIT};
    VkDescriptorSetLayoutCreateInfo si={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,.bindingCount=5,.pBindings=bindings};
    VkDescriptorSetLayout sl; CHECK(vkCreateDescriptorSetLayout(dev,&si,0,&sl));
    VkPipelineLayoutCreateInfo li={.sType=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,.setLayoutCount=1,.pSetLayouts=&sl,.pushConstantRangeCount=0,.pPushConstantRanges=0};
    VkPipelineLayout layout; CHECK(vkCreatePipelineLayout(dev,&li,0,&layout));
    VkShaderModuleCreateInfo mi={.sType=VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,.codeSize=(size_t)shader_bytes,.pCode=shader}; VkShaderModule module; CHECK(vkCreateShaderModule(dev,&mi,0,&module));
    VkComputePipelineCreateInfo ci={.sType=VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,.layout=layout,.stage={.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,.stage=VK_SHADER_STAGE_COMPUTE_BIT,.module=module,.pName="main"}};
    VkPipeline pipe; CHECK(vkCreateComputePipelines(dev,VK_NULL_HANDLE,1,&ci,0,&pipe));
    VkDescriptorPoolSize sizes[2]={{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,2},{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,3}}; VkDescriptorPoolCreateInfo dpi={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,.maxSets=1,.poolSizeCount=2,.pPoolSizes=sizes}; VkDescriptorPool pool; CHECK(vkCreateDescriptorPool(dev,&dpi,0,&pool));
    VkDescriptorSetAllocateInfo dai={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,.descriptorPool=pool,.descriptorSetCount=1,.pSetLayouts=&sl}; VkDescriptorSet set; CHECK(vkAllocateDescriptorSets(dev,&dai,&set));
    VkDescriptorBufferInfo bi[5]; VkWriteDescriptorSet writes[5]; for(int i=0;i<5;i++) { bi[i]=(VkDescriptorBufferInfo){buffers[i],0,TOTAL*4}; writes[i]=(VkWriteDescriptorSet){.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,.dstSet=set,.dstBinding=i,.descriptorCount=1,.descriptorType=descriptor_type(i),.pBufferInfo=&bi[i]}; } vkUpdateDescriptorSets(dev,5,writes,0,0);
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
        vkCmdBindPipeline(cb,VK_PIPELINE_BIND_POINT_COMPUTE,pipe); vkCmdBindDescriptorSets(cb,VK_PIPELINE_BIND_POINT_COMPUTE,layout,0,1,&set,0,0);
        vkCmdDispatch(cb,VERTICES/64,1,1);
        VkMemoryBarrier barrier={.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER,.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT,.dstAccessMask=VK_ACCESS_HOST_READ_BIT}; vkCmdPipelineBarrier(cb,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&barrier,0,0,0,0);
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
    printf("%s on %s: 1024 skinned vertices, identity/translation/rotation/scale, 1/2/4 bone weights, joint 511, four updates of same UBO; max position error %.9g (%d errors)\n",errors?"FAIL":"PASS",properties.deviceName,worst,errors);
    vkDeviceWaitIdle(dev); for(int i=0;i<5;i++) { vkUnmapMemory(dev,memory[i]); vkDestroyBuffer(dev,buffers[i],0); vkFreeMemory(dev,memory[i],0); }
    vkDestroyCommandPool(dev,cp,0); vkDestroyDescriptorPool(dev,pool,0); vkDestroyPipeline(dev,pipe,0); vkDestroyShaderModule(dev,module,0); vkDestroyPipelineLayout(dev,layout,0); vkDestroyDescriptorSetLayout(dev,sl,0); vkDestroyDevice(dev,0); vkDestroyInstance(instance,0); free(queues); free(shader); return errors?1:0;
}
