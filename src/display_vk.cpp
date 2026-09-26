// The window on the Vulkan backend: a swapchain, ImGui's Vulkan renderer,
// and a small full-screen pass that frames the canvas.
//
// GL puts the canvas on screen with glBlitFramebuffer under a scissor.
// Vulkan's blit honours no scissor, so here the same framing is a fragment
// shader instead: each window pixel is mapped back to an image pixel with the
// ViewXform's arithmetic, sampled nearest past 1:1 and bilinear below it, and
// the split view is two scissored draws of the same pass.
#include "display.h"
#include "gpu_vk.h"

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_vulkan.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

namespace display {
namespace {

// The framing shader. Push constants carry where image pixel (0,0) lands on
// screen and how many screen pixels one image pixel covers.
const char* kFrameVert = R"(#version 450
void main() {
    // One triangle that covers the viewport.
    vec2 p = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
)";

const char* kFrameFrag = R"(#version 450
layout(binding = 0) uniform sampler2D uImage;
layout(push_constant) uniform Frame {
    vec2 origin;     // window position of image pixel (0,0), framebuffer px
    vec2 imageSize;
    vec4 background;
    float scale;     // framebuffer px per image px
    float nearest;   // past 1:1, show texels rather than invent detail
} f;
layout(location = 0) out vec4 outColor;
void main() {
    vec2 ip = (gl_FragCoord.xy - f.origin) / f.scale;
    if (any(lessThan(ip, vec2(0.0))) || any(greaterThanEqual(ip, f.imageSize))) {
        outColor = f.background;
        return;
    }
    vec3 c = (f.nearest > 0.5) ? texelFetch(uImage, ivec2(ip), 0).rgb
                               : textureLod(uImage, ip / f.imageSize, 0.0).rgb;
    outColor = vec4(clamp(c, 0.0, 1.0), 1.0);
}
)";

struct FramePush {
    float origin[2];
    float imageSize[2];
    float background[4];
    float scale;
    float nearest;
    float pad[2];
};

struct State {
    GLFWwindow* win = nullptr;
    bool glfw = false;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_B8G8R8A8_UNORM;
    VkExtent2D extent{0, 0};
    uint32_t minImages = 2;
    std::vector<VkImage> images;
    std::vector<VkImageView> views;
    std::vector<VkFramebuffer> framebuffers;
    std::vector<VkSemaphore> renderDone;   // one per swapchain image
    VkSemaphore acquired = VK_NULL_HANDLE;
    VkRenderPass renderPass = VK_NULL_HANDLE;
    VkDescriptorPool imguiPool = VK_NULL_HANDLE;
    VkDescriptorSetLayout frameSetLayout = VK_NULL_HANDLE;
    VkPipelineLayout frameLayout = VK_NULL_HANDLE;
    VkPipeline framePipeline = VK_NULL_HANDLE;
    PFN_vkCmdPushDescriptorSetKHR pushDescriptorSet = nullptr;
    bool imgui = false;
    bool stale = false;
};

State g;

VkDevice dev() { return gpu::vk::device().device; }

void check(VkResult r, const char* what) {
    if (r == VK_SUCCESS) return;
    fprintf(stderr, "Vulkan: %s failed (VkResult %d)\n", what, int(r));
    std::abort();
}

void destroySwapchain() {
    for (VkFramebuffer f : g.framebuffers) vkDestroyFramebuffer(dev(), f, nullptr);
    for (VkImageView v : g.views) vkDestroyImageView(dev(), v, nullptr);
    for (VkSemaphore s : g.renderDone) vkDestroySemaphore(dev(), s, nullptr);
    g.framebuffers.clear();
    g.views.clear();
    g.images.clear();
    g.renderDone.clear();
    if (g.swapchain) vkDestroySwapchainKHR(dev(), g.swapchain, nullptr);
    g.swapchain = VK_NULL_HANDLE;
}

bool createSwapchain() {
    const gpu::vk::Device& d = gpu::vk::device();
    int fw = 0, fh = 0;
    glfwGetFramebufferSize(g.win, &fw, &fh);
    if (fw <= 0 || fh <= 0) return false;   // minimised

    VkSurfaceCapabilitiesKHR caps;
    check(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(d.physical, g.surface, &caps),
          "vkGetPhysicalDeviceSurfaceCapabilitiesKHR");
    VkExtent2D extent = caps.currentExtent;
    if (extent.width == 0xffffffffu) {
        extent.width = std::clamp(uint32_t(fw), caps.minImageExtent.width, caps.maxImageExtent.width);
        extent.height = std::clamp(uint32_t(fh), caps.minImageExtent.height, caps.maxImageExtent.height);
    }
    if (extent.width == 0 || extent.height == 0) return false;

    // The overlay is meant to show real GPU cost, so no vsync where the
    // platform lets it go: mailbox, then immediate, then FIFO, which every
    // implementation has.
    uint32_t n = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(d.physical, g.surface, &n, nullptr);
    std::vector<VkPresentModeKHR> modes(n);
    vkGetPhysicalDeviceSurfacePresentModesKHR(d.physical, g.surface, &n, modes.data());
    VkPresentModeKHR mode = VK_PRESENT_MODE_FIFO_KHR;
    for (VkPresentModeKHR want : {VK_PRESENT_MODE_MAILBOX_KHR, VK_PRESENT_MODE_IMMEDIATE_KHR}) {
        if (std::find(modes.begin(), modes.end(), want) != modes.end()) { mode = want; break; }
    }

    uint32_t count = std::max(caps.minImageCount + 1, 2u);
    if (caps.maxImageCount) count = std::min(count, caps.maxImageCount);
    g.minImages = std::max(caps.minImageCount, 2u);

    VkSwapchainCreateInfoKHR si{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    si.surface = g.surface;
    si.minImageCount = count;
    si.imageFormat = g.format;
    si.imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    si.imageExtent = extent;
    si.imageArrayLayers = 1;
    si.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    si.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    si.preTransform = (caps.supportedTransforms & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR)
                          ? VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR : caps.currentTransform;
    // Opaque where offered, otherwise the lowest mode the surface has.
    const uint32_t alphas = caps.supportedCompositeAlpha;
    si.compositeAlpha = (alphas & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR)
                            ? VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR
                            : VkCompositeAlphaFlagBitsKHR(alphas & (~alphas + 1u));
    si.presentMode = mode;
    si.clipped = VK_TRUE;
    si.oldSwapchain = g.swapchain;
    VkSwapchainKHR sc = VK_NULL_HANDLE;
    check(vkCreateSwapchainKHR(dev(), &si, nullptr, &sc), "vkCreateSwapchainKHR");
    destroySwapchain();
    g.swapchain = sc;
    g.extent = extent;

    vkGetSwapchainImagesKHR(dev(), sc, &n, nullptr);
    g.images.resize(n);
    vkGetSwapchainImagesKHR(dev(), sc, &n, g.images.data());
    for (VkImage img : g.images) {
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = img;
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = g.format;
        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkImageView v;
        check(vkCreateImageView(dev(), &vi, nullptr, &v), "vkCreateImageView");
        g.views.push_back(v);
        VkFramebufferCreateInfo fi{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        fi.renderPass = g.renderPass;
        fi.attachmentCount = 1;
        fi.pAttachments = &v;
        fi.width = extent.width;
        fi.height = extent.height;
        fi.layers = 1;
        VkFramebuffer f;
        check(vkCreateFramebuffer(dev(), &fi, nullptr, &f), "vkCreateFramebuffer");
        g.framebuffers.push_back(f);
        VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        VkSemaphore s;
        check(vkCreateSemaphore(dev(), &sci, nullptr, &s), "vkCreateSemaphore");
        g.renderDone.push_back(s);
    }
    if (g.imgui) ImGui_ImplVulkan_SetMinImageCount(g.minImages);
    g.stale = false;
    return true;
}

bool createFramePipeline(std::string* err) {
    std::vector<std::vector<uint32_t>> spirv;
    if (!gpu::vk::compile({{gpu::vk::Stage::Vertex, kFrameVert},
                           {gpu::vk::Stage::Fragment, kFrameFrag}}, &spirv, err))
        return false;

    VkDescriptorSetLayoutBinding b{};
    b.binding = 0;
    b.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    b.descriptorCount = 1;
    b.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo sli{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    sli.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
    sli.bindingCount = 1;
    sli.pBindings = &b;
    check(vkCreateDescriptorSetLayout(dev(), &sli, nullptr, &g.frameSetLayout),
          "vkCreateDescriptorSetLayout");
    VkPushConstantRange pr{VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(FramePush)};
    VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pli.setLayoutCount = 1;
    pli.pSetLayouts = &g.frameSetLayout;
    pli.pushConstantRangeCount = 1;
    pli.pPushConstantRanges = &pr;
    check(vkCreatePipelineLayout(dev(), &pli, nullptr, &g.frameLayout), "vkCreatePipelineLayout");

    VkShaderModule mods[2] = {gpu::vk::createModule(spirv[0]), gpu::vk::createModule(spirv[1])};
    VkPipelineShaderStageCreateInfo st[2]{};
    for (int i = 0; i < 2; ++i) {
        st[i].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        st[i].stage = i == 0 ? VK_SHADER_STAGE_VERTEX_BIT : VK_SHADER_STAGE_FRAGMENT_BIT;
        st[i].module = mods[i];
        st[i].pName = "main";
    }
    VkPipelineVertexInputStateCreateInfo vin{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = 1;
    vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.lineWidth = 1.f;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState att{};
    att.colorWriteMask = 0xf;
    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = 1;
    cb.pAttachments = &att;
    const VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    ds.dynamicStateCount = 2;
    ds.pDynamicStates = dyn;
    VkGraphicsPipelineCreateInfo gi{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    gi.stageCount = 2;
    gi.pStages = st;
    gi.pVertexInputState = &vin;
    gi.pInputAssemblyState = &ia;
    gi.pViewportState = &vp;
    gi.pRasterizationState = &rs;
    gi.pMultisampleState = &ms;
    gi.pColorBlendState = &cb;
    gi.pDynamicState = &ds;
    gi.layout = g.frameLayout;
    gi.renderPass = g.renderPass;
    check(vkCreateGraphicsPipelines(dev(), VK_NULL_HANDLE, 1, &gi, nullptr, &g.framePipeline),
          "vkCreateGraphicsPipelines");
    vkDestroyShaderModule(dev(), mods[0], nullptr);
    vkDestroyShaderModule(dev(), mods[1], nullptr);
    return true;
}

bool openWindow(int width, int height, const char* title, std::string* err) {
#if GLFW_VERSION_MAJOR > 3 || (GLFW_VERSION_MAJOR == 3 && GLFW_VERSION_MINOR >= 4)
    // Hand GLFW the loader this executable links against. Left to itself,
    // GLFW looks for a Vulkan library by name, which on macOS may not be the
    // one that knows where MoltenVK is.
    glfwInitVulkanLoader(vkGetInstanceProcAddr);
#endif
    if (!glfwInit()) { if (err) *err = "glfwInit failed"; return false; }
    g.glfw = true;
    if (!glfwVulkanSupported()) {
        if (err) *err = "GLFW found no Vulkan loader";
        return false;
    }
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    g.win = glfwCreateWindow(width, height, title, nullptr, nullptr);
    if (!g.win) { if (err) *err = "could not create a window"; return false; }

    uint32_t n = 0;
    const char** exts = glfwGetRequiredInstanceExtensions(&n);
    gpu::InitOptions opt;
    for (uint32_t i = 0; i < n; ++i) opt.instanceExtensions.push_back(exts[i]);
    if (!gpu::init(opt, err)) return false;

    const gpu::vk::Device& d = gpu::vk::device();
    if (glfwCreateWindowSurface(d.instance, g.win, nullptr, &g.surface) != VK_SUCCESS) {
        if (err) *err = "could not create a Vulkan surface for the window";
        return false;
    }
    VkBool32 present = VK_FALSE;
    vkGetPhysicalDeviceSurfaceSupportKHR(d.physical, d.queueFamily, g.surface, &present);
    if (!present) {
        if (err) *err = "the chosen GPU queue cannot present to this window";
        return false;
    }

    // Unorm, not sRGB, to match the GL default framebuffer: the canvas holds
    // display values already, and an sRGB target would encode them twice.
    vkGetPhysicalDeviceSurfaceFormatsKHR(d.physical, g.surface, &n, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(n);
    vkGetPhysicalDeviceSurfaceFormatsKHR(d.physical, g.surface, &n, formats.data());
    g.format = formats.empty() ? VK_FORMAT_B8G8R8A8_UNORM : formats[0].format;
    for (const VkSurfaceFormatKHR& f : formats) {
        if ((f.format == VK_FORMAT_B8G8R8A8_UNORM || f.format == VK_FORMAT_R8G8B8A8_UNORM) &&
            f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            g.format = f.format;
            break;
        }
    }

    VkAttachmentDescription a{};
    a.format = g.format;
    a.samples = VK_SAMPLE_COUNT_1_BIT;
    a.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    a.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    a.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    a.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    a.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    a.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    VkAttachmentReference ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sub{};
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 1;
    sub.pColorAttachments = &ref;
    // The acquire semaphore is waited on at colour output, so the layout
    // transition has to wait there too.
    VkSubpassDependency dep{};
    dep.srcSubpass = VK_SUBPASS_EXTERNAL;
    dep.dstSubpass = 0;
    dep.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    VkRenderPassCreateInfo rpi{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    rpi.attachmentCount = 1;
    rpi.pAttachments = &a;
    rpi.subpassCount = 1;
    rpi.pSubpasses = &sub;
    rpi.dependencyCount = 1;
    rpi.pDependencies = &dep;
    check(vkCreateRenderPass(dev(), &rpi, nullptr, &g.renderPass), "vkCreateRenderPass");

    VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    check(vkCreateSemaphore(dev(), &sci, nullptr, &g.acquired), "vkCreateSemaphore");
    g.pushDescriptorSet = reinterpret_cast<PFN_vkCmdPushDescriptorSetKHR>(
        vkGetDeviceProcAddr(dev(), "vkCmdPushDescriptorSetKHR"));
    if (!createFramePipeline(err)) return false;
    createSwapchain();
    return true;
}

// One region of the window showing one texture, framed by `xf`.
void drawImage(VkCommandBuffer cb, gpu::Texture tex, int iw, int ih, int x, int y, int w,
               int h, int sx, int sw, const ViewXform& xf) {
    if (sw <= 0 || !tex) return;
    const float fit = std::min(float(w) / float(iw), float(h) / float(ih));
    const float scale = fit * std::max(xf.zoom, 0.01f);
    const float fx = xf.focusX < 0.f ? float(iw) * 0.5f : xf.focusX;
    const float fy = xf.focusY < 0.f ? float(ih) * 0.5f : xf.focusY;
    const float cx = float(x) + float(w) * 0.5f;
    const float cy = float(y) + float(h) * 0.5f;

    FramePush pc{};
    // Rounded like the GL blit's destination rectangle, so both backends put
    // the image on the same pixels.
    pc.origin[0] = float(std::lround(cx - fx * scale));
    pc.origin[1] = float(std::lround(cy - fy * scale));
    pc.imageSize[0] = float(iw);
    pc.imageSize[1] = float(ih);
    pc.background[0] = 0.09f; pc.background[1] = 0.09f;
    pc.background[2] = 0.10f; pc.background[3] = 1.f;
    pc.scale = scale;
    pc.nearest = scale > 1.f ? 1.f : 0.f;

    VkRect2D sc{{std::max(sx, 0), std::max(y, 0)}, {uint32_t(sw), uint32_t(std::max(h, 0))}};
    vkCmdSetScissor(cb, 0, 1, &sc);
    const VkDescriptorImageInfo ii = gpu::vk::sampled(tex, pc.nearest > 0.5f);
    VkWriteDescriptorSet wr{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    wr.dstBinding = 0;
    wr.descriptorCount = 1;
    wr.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    wr.pImageInfo = &ii;
    g.pushDescriptorSet(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g.frameLayout, 0, 1, &wr);
    vkCmdPushConstants(cb, g.frameLayout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc), &pc);
    vkCmdDraw(cb, 3, 1, 0, 0);
}

} // namespace

bool open(bool interactive, int width, int height, const char* title, std::string* err) {
    if (!interactive) {
        // Nothing to show, so no window and no GLFW: the renderer then runs
        // wherever there is a GPU, with or without a display.
        gpu::InitOptions opt;
        return gpu::init(opt, err);
    }
    return openWindow(width, height, title, err);
}

void close() {
    if (gpu::vk::device().device) {
        gpu::finish();
        vkDeviceWaitIdle(dev());
        destroySwapchain();
        if (g.framePipeline) vkDestroyPipeline(dev(), g.framePipeline, nullptr);
        if (g.frameLayout) vkDestroyPipelineLayout(dev(), g.frameLayout, nullptr);
        if (g.frameSetLayout) vkDestroyDescriptorSetLayout(dev(), g.frameSetLayout, nullptr);
        if (g.imguiPool) vkDestroyDescriptorPool(dev(), g.imguiPool, nullptr);
        if (g.renderPass) vkDestroyRenderPass(dev(), g.renderPass, nullptr);
        if (g.acquired) vkDestroySemaphore(dev(), g.acquired, nullptr);
        if (g.surface) vkDestroySurfaceKHR(gpu::vk::device().instance, g.surface, nullptr);
    }
    gpu::shutdown();
    if (g.win) glfwDestroyWindow(g.win);
    if (g.glfw) glfwTerminate();
    g = State();
}

GLFWwindow* window() { return g.win; }

void imguiInit() {
    const gpu::vk::Device& d = gpu::vk::device();
    const VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 16}};
    VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pi.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    pi.maxSets = 16;
    pi.poolSizeCount = 1;
    pi.pPoolSizes = sizes;
    check(vkCreateDescriptorPool(dev(), &pi, nullptr, &g.imguiPool), "vkCreateDescriptorPool");

    ImGui_ImplGlfw_InitForVulkan(g.win, true);
    ImGui_ImplVulkan_InitInfo ii{};
    ii.ApiVersion = d.apiVersion;
    ii.Instance = d.instance;
    ii.PhysicalDevice = d.physical;
    ii.Device = d.device;
    ii.QueueFamily = d.queueFamily;
    ii.Queue = d.queue;
    ii.DescriptorPool = g.imguiPool;
    ii.RenderPass = g.renderPass;
    ii.MinImageCount = g.minImages;
    ii.ImageCount = std::max<uint32_t>(uint32_t(g.images.size()), g.minImages);
    ii.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
    ImGui_ImplVulkan_Init(&ii);
    g.imgui = true;
}

void imguiNewFrame() {
    ImGui_ImplVulkan_NewFrame();
    ImGui_ImplGlfw_NewFrame();
}

void imguiShutdown() {
    gpu::finish();
    vkDeviceWaitIdle(dev());
    ImGui_ImplVulkan_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    g.imgui = false;
}

void present(const Pipeline& pipe, Rect area, ViewMode mode, const ViewXform& xf,
             ImDrawData* ui) {
    int fw = 0, fh = 0;
    glfwGetFramebufferSize(g.win, &fw, &fh);
    if (fw <= 0 || fh <= 0) return;   // minimised: nothing to draw into
    if (g.stale || !g.swapchain || uint32_t(fw) != g.extent.width ||
        uint32_t(fh) != g.extent.height) {
        gpu::finish();
        vkDeviceWaitIdle(dev());
        if (!createSwapchain()) return;
    }

    uint32_t index = 0;
    VkResult r = vkAcquireNextImageKHR(dev(), g.swapchain, UINT64_MAX, g.acquired,
                                       VK_NULL_HANDLE, &index);
    if (r == VK_ERROR_OUT_OF_DATE_KHR) { g.stale = true; return; }
    if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) check(r, "vkAcquireNextImageKHR");

    VkCommandBuffer cb = gpu::vk::commands();
    VkClearValue clear{};
    clear.color.float32[0] = 0.09f; clear.color.float32[1] = 0.09f;
    clear.color.float32[2] = 0.10f; clear.color.float32[3] = 1.f;
    VkRenderPassBeginInfo rbi{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rbi.renderPass = g.renderPass;
    rbi.framebuffer = g.framebuffers[index];
    rbi.renderArea = {{0, 0}, g.extent};
    rbi.clearValueCount = 1;
    rbi.pClearValues = &clear;
    vkCmdBeginRenderPass(cb, &rbi, VK_SUBPASS_CONTENTS_INLINE);

    const int iw = pipe.width(), ih = pipe.height();
    if (iw > 0 && ih > 0 && area.w > 0 && area.h > 0) {
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g.framePipeline);
        const VkViewport vp{0.f, 0.f, float(g.extent.width), float(g.extent.height), 0.f, 1.f};
        vkCmdSetViewport(cb, 0, 1, &vp);
        // Scissors must stay inside the framebuffer.
        const int x0 = std::clamp(area.x, 0, int(g.extent.width));
        const int x1 = std::clamp(area.x + area.w, 0, int(g.extent.width));
        const int y0 = std::clamp(area.y, 0, int(g.extent.height));
        const int y1 = std::clamp(area.y + area.h, 0, int(g.extent.height));
        const int w = x1 - x0, h = y1 - y0;
        switch (mode) {
            case ViewMode::Source:
                drawImage(cb, pipe.sourceTexture(), iw, ih, area.x, y0, area.w, h, x0, w, xf);
                break;
            case ViewMode::Split: {
                // A wipe, as on GL: both halves share one framing.
                const int cut = std::clamp(int(float(area.w) * xf.wipe), 0, area.w);
                const int split = std::clamp(area.x + cut, x0, x1);
                drawImage(cb, pipe.sourceTexture(), iw, ih, area.x, y0, area.w, h, x0,
                          split - x0, xf);
                drawImage(cb, pipe.canvasTexture(), iw, ih, area.x, y0, area.w, h, split,
                          x1 - split, xf);
                break;
            }
            default:
                drawImage(cb, pipe.canvasTexture(), iw, ih, area.x, y0, area.w, h, x0, w, xf);
                break;
        }
    }
    if (ui) ImGui_ImplVulkan_RenderDrawData(ui, cb);
    vkCmdEndRenderPass(cb);

    gpu::vk::submit(g.acquired, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                    g.renderDone[index]);

    VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &g.renderDone[index];
    pi.swapchainCount = 1;
    pi.pSwapchains = &g.swapchain;
    pi.pImageIndices = &index;
    r = vkQueuePresentKHR(gpu::vk::device().queue, &pi);
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) g.stale = true;
    else if (r != VK_SUCCESS) check(r, "vkQueuePresentKHR");
}

} // namespace display
