/*
 * GStreamer
 * Copyright (C) 2015 Matthew Waters <matthew@centricular.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <string.h>

#include "vkswapper.h"

#define GST_CAT_DEFAULT gst_vulkan_swapper_debug
GST_DEBUG_CATEGORY (GST_CAT_DEFAULT);

#define gst_vulkan_swapper_parent_class parent_class
G_DEFINE_TYPE_WITH_CODE (GstVulkanSwapper, gst_vulkan_swapper,
    GST_TYPE_OBJECT, GST_DEBUG_CATEGORY_INIT (GST_CAT_DEFAULT,
        "vulkanswapper", 0, "Vulkan Swapper"));

#define RENDER_GET_LOCK(o) &(GST_VULKAN_SWAPPER (o)->priv->render_lock)
#define RENDER_LOCK(o) g_mutex_lock (RENDER_GET_LOCK(o));
#define RENDER_UNLOCK(o) g_mutex_unlock (RENDER_GET_LOCK(o));

struct _GstVulkanSwapperPrivate
{
  GMutex render_lock;
};

static void _on_window_draw (GstVulkanWindow * window,
    GstVulkanSwapper * swapper);

static gboolean
_get_function_table (GstVulkanSwapper * swapper)
{
  GstVulkanDevice *device = swapper->device;
  GstVulkanInstance *instance = gst_vulkan_device_get_instance (device);

  if (!instance) {
    GST_ERROR_OBJECT (swapper, "Failed to get instance from the device");
    return FALSE;
  }
#define GET_PROC_ADDRESS_REQUIRED(obj, type, name) \
  G_STMT_START { \
    obj->G_PASTE (, name) = G_PASTE(G_PASTE(gst_vulkan_, type), _get_proc_address) (type, "vk" G_STRINGIFY(name)); \
    if (!obj->G_PASTE(, name)) { \
      GST_ERROR_OBJECT (obj, "Failed to find required function vk" G_STRINGIFY(name)); \
      gst_object_unref (instance); \
      return FALSE; \
    } \
  } G_STMT_END

  GET_PROC_ADDRESS_REQUIRED (swapper, instance,
      GetPhysicalDeviceSurfaceSupportKHR);
  GET_PROC_ADDRESS_REQUIRED (swapper, device,
      GetPhysicalDeviceSurfaceCapabilitiesKHR);
  GET_PROC_ADDRESS_REQUIRED (swapper, device,
      GetPhysicalDeviceSurfaceFormatsKHR);
  GET_PROC_ADDRESS_REQUIRED (swapper, device,
      GetPhysicalDeviceSurfacePresentModesKHR);
  GET_PROC_ADDRESS_REQUIRED (swapper, device, CreateSwapchainKHR);
  GET_PROC_ADDRESS_REQUIRED (swapper, device, DestroySwapchainKHR);
  GET_PROC_ADDRESS_REQUIRED (swapper, device, GetSwapchainImagesKHR);
  GET_PROC_ADDRESS_REQUIRED (swapper, device, AcquireNextImageKHR);
  GET_PROC_ADDRESS_REQUIRED (swapper, device, QueuePresentKHR);

  gst_object_unref (instance);

  return TRUE;

#undef GET_PROC_ADDRESS_REQUIRED
}

static GstVideoFormat
_vk_format_to_video_format (VkFormat format)
{
  switch (format) {
      /* double check endianess */
    case VK_FORMAT_R8G8B8A8_UNORM:
      return GST_VIDEO_FORMAT_RGBA;
    case VK_FORMAT_R8G8B8_UNORM:
      return GST_VIDEO_FORMAT_RGB;
    case VK_FORMAT_B8G8R8A8_UNORM:
      return GST_VIDEO_FORMAT_BGRA;
    case VK_FORMAT_B8G8R8_UNORM:
      return GST_VIDEO_FORMAT_BGR;
    default:
      return GST_VIDEO_FORMAT_UNKNOWN;
  }
}

static VkFormat
_vk_format_from_video_format (GstVideoFormat v_format)
{
  switch (v_format) {
    case GST_VIDEO_FORMAT_RGBA:
      return VK_FORMAT_R8G8B8A8_UNORM;
    case GST_VIDEO_FORMAT_RGB:
      return VK_FORMAT_R8G8B8_UNORM;
    case GST_VIDEO_FORMAT_BGRA:
      return VK_FORMAT_B8G8R8A8_UNORM;
    case GST_VIDEO_FORMAT_BGR:
      return VK_FORMAT_B8G8R8_UNORM;
    default:
      return VK_FORMAT_UNDEFINED;
  }
}

static VkColorSpaceKHR
_vk_color_space_from_video_info (GstVideoInfo * v_info)
{
  return VK_COLORSPACE_SRGB_NONLINEAR_KHR;
}

static void
_add_vk_format_to_list (GValue * list, VkFormat format)
{
  GstVideoFormat v_format;
  const gchar *format_str;

  v_format = _vk_format_to_video_format (format);
  if (v_format) {
    GValue item = G_VALUE_INIT;

    g_value_init (&item, G_TYPE_STRING);
    format_str = gst_video_format_to_string (v_format);
    g_value_set_string (&item, format_str);
    gst_value_list_append_value (list, &item);
    g_value_unset (&item);
  }
}

static gboolean
_vulkan_swapper_ensure_surface (GstVulkanSwapper * swapper, GError ** error)
{
  if (!swapper->surface) {
    if (!(swapper->surface =
            gst_vulkan_window_get_surface (swapper->window, error))) {
      return FALSE;
    }
  }

  return TRUE;
}

static gboolean
_vulkan_swapper_retrieve_surface_properties (GstVulkanSwapper * swapper,
    GError ** error)
{
  guint32 i, present_queue = -1, graphics_queue = -1;
  VkPhysicalDevice gpu;
  VkResult err;

  if (swapper->surf_formats)
    return TRUE;

  if (!_vulkan_swapper_ensure_surface (swapper, error))
    return FALSE;

  gpu = gst_vulkan_device_get_physical_device (swapper->device);

  for (i = 0; i < swapper->device->n_queues; i++) {
    gboolean supports_present;

    supports_present =
        gst_vulkan_window_get_presentation_support (swapper->window,
        swapper->device, i);
    if ((swapper->device->
            queue_family_props[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0) {
      if (supports_present) {
        /* found one that supports both */
        graphics_queue = present_queue = i;
        break;
      }
      if (graphics_queue != -1)
        graphics_queue = i;
    } else if (supports_present) {
      if (present_queue != -1)
        present_queue = i;
    }
  }

  if (graphics_queue != present_queue) {
    /* FIXME: add support for separate graphics/present queues */
    g_set_error (error, GST_VULKAN_ERROR,
        VK_ERROR_INITIALIZATION_FAILED,
        "Failed to find a compatible present/graphics queue");
    return FALSE;
  }

  if (!(swapper->queue = gst_vulkan_device_get_queue (swapper->device,
              swapper->device->queue_family_id, graphics_queue, error)))
    return FALSE;

  err =
      swapper->GetPhysicalDeviceSurfaceCapabilitiesKHR (gpu, swapper->surface,
      &swapper->surf_props);
  if (gst_vulkan_error_to_g_error (err, error,
          "GetPhysicalDeviceSurfaceCapabilitiesKHR") < 0)
    return FALSE;

  err =
      swapper->GetPhysicalDeviceSurfaceFormatsKHR (gpu, swapper->surface,
      &swapper->n_surf_formats, NULL);
  if (gst_vulkan_error_to_g_error (err, error,
          "GetPhysicalDeviceSurfaceFormatsKHR") < 0)
    return FALSE;

  swapper->surf_formats = g_new0 (VkSurfaceFormatKHR, swapper->n_surf_formats);
  err =
      swapper->GetPhysicalDeviceSurfaceFormatsKHR (gpu, swapper->surface,
      &swapper->n_surf_formats, swapper->surf_formats);
  if (gst_vulkan_error_to_g_error (err, error,
          "GetPhysicalDeviceSurfaceFormatsKHR") < 0)
    return FALSE;

  err =
      swapper->GetPhysicalDeviceSurfacePresentModesKHR (gpu, swapper->surface,
      &swapper->n_surf_present_modes, NULL);
  if (gst_vulkan_error_to_g_error (err, error,
          "GetPhysicalDeviceSurfacePresentModesKHR") < 0)
    return FALSE;

  swapper->surf_present_modes =
      g_new0 (VkPresentModeKHR, swapper->n_surf_present_modes);
  err =
      swapper->GetPhysicalDeviceSurfacePresentModesKHR (gpu, swapper->surface,
      &swapper->n_surf_present_modes, swapper->surf_present_modes);
  if (gst_vulkan_error_to_g_error (err, error,
          "GetPhysicalDeviceSurfacePresentModesKHR") < 0)
    return FALSE;

  return TRUE;
}

static gboolean
_on_window_close (GstVulkanWindow * window, GstVulkanSwapper * swapper)
{
  g_atomic_int_set (&swapper->to_quit, 1);

  return TRUE;
}

static void
gst_vulkan_swapper_finalize (GObject * object)
{
  GstVulkanSwapper *swapper = GST_VULKAN_SWAPPER (object);
  int i;

  if (swapper->swap_chain_images) {
    for (i = 0; i < swapper->n_swap_chain_images; i++) {
      gst_memory_unref ((GstMemory *) swapper->swap_chain_images[i]);
      swapper->swap_chain_images[i] = NULL;
    }
    g_free (swapper->swap_chain_images);
  }
  swapper->swap_chain_images = NULL;

  if (swapper->swap_chain)
    swapper->DestroySwapchainKHR (swapper->device->device, swapper->swap_chain,
        NULL);
  swapper->swap_chain = VK_NULL_HANDLE;

  if (swapper->queue)
    gst_object_unref (swapper->queue);
  swapper->queue = NULL;

  if (swapper->device)
    gst_object_unref (swapper->device);
  swapper->device = NULL;

  g_signal_handler_disconnect (swapper->window, swapper->draw_id);
  swapper->draw_id = 0;

  g_signal_handler_disconnect (swapper->window, swapper->close_id);
  swapper->close_id = 0;

  if (swapper->window)
    gst_object_unref (swapper->window);
  swapper->window = NULL;

  g_free (swapper->surf_present_modes);
  swapper->surf_present_modes = NULL;

  g_free (swapper->surf_formats);
  swapper->surf_formats = NULL;

  gst_buffer_replace (&swapper->current_buffer, NULL);
  gst_caps_replace (&swapper->caps, NULL);

  g_mutex_clear (&swapper->priv->render_lock);
}

static void
gst_vulkan_swapper_init (GstVulkanSwapper * swapper)
{
  swapper->priv =
      G_TYPE_INSTANCE_GET_PRIVATE (swapper, GST_TYPE_VULKAN_SWAPPER,
      GstVulkanSwapperPrivate);

  g_mutex_init (&swapper->priv->render_lock);
}

static void
gst_vulkan_swapper_class_init (GstVulkanSwapperClass * klass)
{
  g_type_class_add_private (klass, sizeof (GstVulkanSwapperPrivate));

  G_OBJECT_CLASS (klass)->finalize = gst_vulkan_swapper_finalize;
}

GstVulkanSwapper *
gst_vulkan_swapper_new (GstVulkanDevice * device, GstVulkanWindow * window)
{
  GstVulkanSwapper *swapper;

  swapper = g_object_new (GST_TYPE_VULKAN_SWAPPER, NULL);
  swapper->device = gst_object_ref (device);
  swapper->window = gst_object_ref (window);

  if (!_get_function_table (swapper)) {
    gst_object_unref (swapper);
    return NULL;
  }

  swapper->close_id = g_signal_connect (swapper->window, "close",
      (GCallback) _on_window_close, swapper);
  swapper->draw_id = g_signal_connect (swapper->window, "draw",
      (GCallback) _on_window_draw, swapper);

  return swapper;
}

GstCaps *
gst_vulkan_swapper_get_supported_caps (GstVulkanSwapper * swapper,
    GError ** error)
{
  GstStructure *s;
  GstCaps *caps;

  g_return_val_if_fail (GST_IS_VULKAN_SWAPPER (swapper), NULL);

  if (!_vulkan_swapper_retrieve_surface_properties (swapper, error))
    return NULL;

  caps = gst_caps_new_empty_simple ("video/x-raw");
  s = gst_caps_get_structure (caps, 0);

  {
    int i;
    GValue list = G_VALUE_INIT;

    g_value_init (&list, GST_TYPE_LIST);

    if (swapper->n_surf_formats
        && swapper->surf_formats[0].format == VK_FORMAT_UNDEFINED) {
      _add_vk_format_to_list (&list, VK_FORMAT_B8G8R8A8_UNORM);
    } else {
      for (i = 0; i < swapper->n_surf_formats; i++) {
        _add_vk_format_to_list (&list, swapper->surf_formats[i].format);
      }
    }

    gst_structure_set_value (s, "format", &list);
    g_value_unset (&list);
  }
  {
    guint32 max_dim = swapper->device->gpu_props.limits.maxImageDimension2D;

    gst_structure_set (s, "width", GST_TYPE_INT_RANGE, 1, (gint) max_dim,
        "height", GST_TYPE_INT_RANGE, 1, (gint) max_dim, "pixel-aspect-ratio",
        GST_TYPE_FRACTION, 1, 1, "framerate", GST_TYPE_FRACTION_RANGE, 0, 1,
        G_MAXINT, 1, NULL);
  }

  GST_INFO_OBJECT (swapper, "Probed the following caps %" GST_PTR_FORMAT, caps);

  return caps;
}

static gboolean
_swapper_set_image_layout_with_cmd (GstVulkanSwapper * swapper,
    VkCommandBuffer cmd, GstVulkanImageMemory * image,
    VkImageLayout new_image_layout, GError ** error)
{
  VkPipelineStageFlags src_stages = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
  VkPipelineStageFlags dest_stages = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
  VkImageMemoryBarrier image_memory_barrier;
  VkImageMemoryBarrier *pmemory_barrier = &image_memory_barrier;

  gst_vulkan_image_memory_set_layout (image, new_image_layout,
      &image_memory_barrier);

  vkCmdPipelineBarrier (cmd, src_stages, dest_stages, FALSE, 1,
      (const void *const *) &pmemory_barrier);

  return TRUE;
}

static gboolean
_new_fence (GstVulkanDevice * device, VkFence * fence, GError ** error)
{
  VkFenceCreateInfo fence_info;
  VkResult err;

  fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
  fence_info.pNext = NULL;
  fence_info.flags = 0;

  err = vkCreateFence (device->device, &fence_info, NULL, fence);
  if (gst_vulkan_error_to_g_error (err, error, "vkCreateFence") < 0)
    return FALSE;

  return TRUE;
}

static gboolean
_swapper_set_image_layout (GstVulkanSwapper * swapper,
    GstVulkanImageMemory * image, VkImageLayout new_image_layout,
    GError ** error)
{
  VkCommandBuffer cmd;
  VkFence fence;
  VkResult err;

  if (!gst_vulkan_device_create_cmd_buffer (swapper->device, &cmd, error))
    return FALSE;

  if (!_new_fence (swapper->device, &fence, error))
    return FALSE;

  {
    VkCommandBufferBeginInfo cmd_buf_info = { 0, };

    cmd_buf_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    cmd_buf_info.pNext = NULL;
    cmd_buf_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    cmd_buf_info.renderPass = VK_NULL_HANDLE;
    cmd_buf_info.subpass = 0;
    cmd_buf_info.framebuffer = VK_NULL_HANDLE;
    cmd_buf_info.occlusionQueryEnable = FALSE;
    cmd_buf_info.queryFlags = 0;
    cmd_buf_info.pipelineStatistics = 0;

    err = vkBeginCommandBuffer (cmd, &cmd_buf_info);
    if (gst_vulkan_error_to_g_error (err, error, "vkBeginCommandBuffer") < 0)
      return FALSE;
  }

  if (!_swapper_set_image_layout_with_cmd (swapper, cmd, image,
          new_image_layout, error))
    return FALSE;

  err = vkEndCommandBuffer (cmd);
  if (gst_vulkan_error_to_g_error (err, error, "vkEndCommandBuffer") < 0)
    return FALSE;

  {
    VkSubmitInfo submit_info = { 0, };

    submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit_info.pNext = NULL;
    submit_info.waitSemaphoreCount = 0;
    submit_info.pWaitSemaphores = NULL;
    submit_info.commandBufferCount = 1;
    submit_info.pCommandBuffers = &cmd;
    submit_info.signalSemaphoreCount = 0;
    submit_info.pSignalSemaphores = NULL;

    err = vkQueueSubmit (swapper->queue->queue, 1, &submit_info, fence);
    if (gst_vulkan_error_to_g_error (err, error, "vkQueueSubmit") < 0)
      return FALSE;
  }

  err = vkWaitForFences (swapper->device->device, 1, &fence, TRUE, -1);
  if (gst_vulkan_error_to_g_error (err, error, "vkWaitForFences") < 0)
    return FALSE;
  vkFreeCommandBuffers (swapper->device->device, swapper->device->cmd_pool, 1,
      &cmd);

  vkDestroyFence (swapper->device->device, fence, NULL);

  return TRUE;
}

static gboolean
_allocate_swapchain (GstVulkanSwapper * swapper, GstCaps * caps,
    GError ** error)
{
  VkSurfaceTransformFlagsKHR preTransform;
  VkCompositeAlphaFlagsKHR alpha_flags;
  VkPresentModeKHR present_mode;
  VkImageUsageFlags usage = 0;
  VkColorSpaceKHR color_space;
  VkImage *swap_chain_images;
  VkExtent2D swapchain_dims;
  guint32 n_images_wanted;
  VkPhysicalDevice gpu;
  VkFormat format;
  VkResult err;
  guint32 i;

  if (!_vulkan_swapper_ensure_surface (swapper, error))
    return FALSE;

  gpu = gst_vulkan_device_get_physical_device (swapper->device);
  err =
      swapper->GetPhysicalDeviceSurfaceCapabilitiesKHR (gpu,
      swapper->surface, &swapper->surf_props);
  if (gst_vulkan_error_to_g_error (err, error,
          "GetPhysicalDeviceSurfaceCapabilitiesKHR") < 0)
    return FALSE;

  /* width and height are either both -1, or both not -1. */
  if (swapper->surf_props.currentExtent.width == -1) {
    /* If the surface size is undefined, the size is set to
     * the size of the images requested. */
    swapchain_dims.width = 320;
    swapchain_dims.height = 240;
  } else {
    /* If the surface size is defined, the swap chain size must match */
    swapchain_dims = swapper->surf_props.currentExtent;
  }

  /* If mailbox mode is available, use it, as is the lowest-latency non-
   * tearing mode.  If not, try IMMEDIATE which will usually be available,
   * and is fastest (though it tears).  If not, fall back to FIFO which is
   * always available. */
  present_mode = VK_PRESENT_MODE_FIFO_KHR;
  for (gsize i = 0; i < swapper->n_surf_present_modes; i++) {
    if (swapper->surf_present_modes[i] == VK_PRESENT_MODE_MAILBOX_KHR) {
      present_mode = VK_PRESENT_MODE_MAILBOX_KHR;
      break;
    }
    if ((present_mode != VK_PRESENT_MODE_MAILBOX_KHR) &&
        (swapper->surf_present_modes[i] == VK_PRESENT_MODE_IMMEDIATE_KHR)) {
      present_mode = VK_PRESENT_MODE_IMMEDIATE_KHR;
    }
  }

  /* Determine the number of VkImage's to use in the swap chain (we desire to
   * own only 1 image at a time, besides the images being displayed and
   * queued for display): */
  n_images_wanted = swapper->surf_props.minImageCount + 1;
  if ((swapper->surf_props.maxImageCount > 0) &&
      (n_images_wanted > swapper->surf_props.maxImageCount)) {
    /* Application must settle for fewer images than desired: */
    n_images_wanted = swapper->surf_props.maxImageCount;
  }

  if (swapper->surf_props.
      supportedTransforms & VK_SURFACE_TRANSFORM_NONE_BIT_KHR) {
    preTransform = VK_SURFACE_TRANSFORM_NONE_BIT_KHR;
  } else {
    preTransform = swapper->surf_props.currentTransform;
  }

  format =
      _vk_format_from_video_format (GST_VIDEO_INFO_FORMAT (&swapper->v_info));
  color_space = _vk_color_space_from_video_info (&swapper->v_info);

#if 0
  /* FIXME: unsupported by LunarG's driver */
  g_print ("alpha flags 0x%x\n",
      (guint) swapper->surf_props.supportedCompositeAlpha);

  if ((swapper->surf_props.supportedCompositeAlpha &
          VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR) != 0) {
    alpha_flags = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
  } else if ((swapper->surf_props.supportedCompositeAlpha &
          VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR) != 0) {
    alpha_flags = VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR;
  } else {
    g_set_error (error, GST_VULKAN_ERROR,
        VK_ERROR_INITIALIZATION_FAILED,
        "Incorrect alpha flags available for the swap images");
    return FALSE;
  }
#else
  alpha_flags = 0;
#endif

  if ((swapper->surf_props.supportedUsageFlags &
          VK_IMAGE_USAGE_TRANSFER_DST_BIT) != 0) {
    usage |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  } else {
    g_set_error (error, GST_VULKAN_ERROR,
        VK_ERROR_INITIALIZATION_FAILED,
        "Incorrect usage flags available for the swap images");
    return FALSE;
  }
  if ((swapper->
          surf_props.supportedUsageFlags & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT)
      != 0) {
    usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
  } else {
    g_set_error (error, GST_VULKAN_ERROR,
        VK_ERROR_INITIALIZATION_FAILED,
        "Incorrect usage flags available for the swap images");
    return FALSE;
  }

  {
    VkSwapchainCreateInfoKHR swap_chain_info = { 0, };
    VkSwapchainKHR old_swap_chain;

    old_swap_chain = swapper->swap_chain;

    swap_chain_info.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    swap_chain_info.pNext = NULL;
    swap_chain_info.surface = swapper->surface;
    swap_chain_info.minImageCount = n_images_wanted;
    swap_chain_info.imageFormat = format;
    swap_chain_info.imageColorSpace = color_space;
    swap_chain_info.imageExtent = swapchain_dims;
    swap_chain_info.imageArrayLayers = 1;
    swap_chain_info.imageUsage = usage;
    swap_chain_info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    swap_chain_info.queueFamilyIndexCount = 0;
    swap_chain_info.pQueueFamilyIndices = NULL;
    swap_chain_info.preTransform = preTransform;
    swap_chain_info.presentMode = present_mode;
    swap_chain_info.compositeAlpha = alpha_flags;
    swap_chain_info.clipped = TRUE;
    swap_chain_info.oldSwapchain = swapper->swap_chain;

    err =
        swapper->CreateSwapchainKHR (swapper->device->device, &swap_chain_info,
        NULL, &swapper->swap_chain);
    if (gst_vulkan_error_to_g_error (err, error, "vkCreateSwapchainKHR") < 0)
      return FALSE;

    if (old_swap_chain != VK_NULL_HANDLE) {
      swapper->DestroySwapchainKHR (swapper->device->device, old_swap_chain,
          NULL);
    }
  }

  err =
      swapper->GetSwapchainImagesKHR (swapper->device->device,
      swapper->swap_chain, &swapper->n_swap_chain_images, NULL);
  if (gst_vulkan_error_to_g_error (err, error, "vkGetSwapchainImagesKHR") < 0)
    return FALSE;

  swap_chain_images = g_new0 (VkImage, swapper->n_swap_chain_images);
  err =
      swapper->GetSwapchainImagesKHR (swapper->device->device,
      swapper->swap_chain, &swapper->n_swap_chain_images, swap_chain_images);
  if (gst_vulkan_error_to_g_error (err, error, "vkGetSwapchainImagesKHR") < 0) {
    g_free (swap_chain_images);
    return FALSE;
  }

  swapper->swap_chain_images =
      g_new0 (GstVulkanImageMemory *, swapper->n_swap_chain_images);
  for (i = 0; i < swapper->n_swap_chain_images; i++) {
    swapper->swap_chain_images[i] = (GstVulkanImageMemory *)
        gst_vulkan_image_memory_wrapped (swapper->device, swap_chain_images[i],
        format, swapchain_dims.width, swapchain_dims.height,
        VK_IMAGE_TILING_OPTIMAL, usage, NULL, NULL);

    if (!_swapper_set_image_layout (swapper, swapper->swap_chain_images[i],
            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, error)) {
      g_free (swap_chain_images);
      return FALSE;
    }
  }

  g_free (swap_chain_images);
  return TRUE;
}

static gboolean
_swapchain_resize (GstVulkanSwapper * swapper, GError ** error)
{
  int i;

  if (!swapper->queue) {
    if (!_vulkan_swapper_retrieve_surface_properties (swapper, error)) {
      return FALSE;
    }
  }

  if (swapper->swap_chain_images) {
    for (i = 0; i < swapper->n_swap_chain_images; i++) {
      if (swapper->swap_chain_images[i])
        gst_memory_unref ((GstMemory *) swapper->swap_chain_images[i]);
    }
    g_free (swapper->swap_chain_images);
  }

  return _allocate_swapchain (swapper, swapper->caps, error);
}

gboolean
gst_vulkan_swapper_set_caps (GstVulkanSwapper * swapper, GstCaps * caps,
    GError ** error)
{
  if (!gst_video_info_from_caps (&swapper->v_info, caps)) {
    g_set_error (error, GST_VULKAN_ERROR,
        VK_ERROR_INITIALIZATION_FAILED,
        "Failed to geto GstVideoInfo from caps");
    return FALSE;
  }

  gst_caps_replace (&swapper->caps, caps);

  return _swapchain_resize (swapper, error);
}

struct cmd_data
{
  VkCommandBuffer cmd;
  VkFence fence;
  GDestroyNotify notify;
  gpointer data;
};

static gboolean
_build_render_buffer_cmd (GstVulkanSwapper * swapper, guint32 swap_idx,
    GstBuffer * buffer, struct cmd_data *cmd_data, GError ** error)
{
  VkImageSubresource subres = { 0, };
  GstVulkanImageMemory *swap_mem, *staging;
  GstMapInfo staging_map_info;
  VkSubresourceLayout layout;
  GstVideoFrame vframe;
  guint8 *src, *dest;
  VkCommandBuffer cmd;
  guint32 wt, ht;
  VkResult err;
  gsize h;

  GST_VK_IMAGE_SUBRESOURCE (subres, VK_IMAGE_ASPECT_COLOR_BIT, 0, 0);

  g_return_val_if_fail (swap_idx < swapper->n_swap_chain_images, FALSE);
  swap_mem = swapper->swap_chain_images[swap_idx];

  if (!gst_vulkan_device_create_cmd_buffer (swapper->device, &cmd, error))
    return FALSE;

  if (!gst_video_frame_map (&vframe, &swapper->v_info, buffer, GST_MAP_READ)) {
    g_set_error (error, GST_VULKAN_ERROR, VK_ERROR_MEMORY_MAP_FAILED,
        "Failed to map buffer");
    return FALSE;
  }

  staging =
      (GstVulkanImageMemory *) gst_vulkan_image_memory_alloc (swapper->device,
      swap_mem->create_info.format, GST_VIDEO_FRAME_COMP_WIDTH (&vframe, 0),
      GST_VIDEO_FRAME_COMP_HEIGHT (&vframe, 0), VK_IMAGE_TILING_LINEAR,
      VK_IMAGE_USAGE_TRANSFER_SRC_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);

  if (!staging) {
    g_set_error (error, GST_VULKAN_ERROR, VK_ERROR_MEMORY_MAP_FAILED,
        "Failed to create staging memory");
    gst_video_frame_unmap (&vframe);
    return FALSE;
  }

  if (!gst_memory_map ((GstMemory *) staging, &staging_map_info, GST_MAP_WRITE)) {
    g_set_error (error, GST_VULKAN_ERROR, VK_ERROR_MEMORY_MAP_FAILED,
        "Failed to map swap image");
    gst_video_frame_unmap (&vframe);
    gst_memory_unref ((GstMemory *) staging);
    return FALSE;
  }

  vkGetImageSubresourceLayout (swapper->device->device, staging->image, &subres,
      &layout);

  /* FIXME: multi-planar formats */
  dest = staging_map_info.data;
  dest += layout.offset;
  src = vframe.data[0];
  for (h = 0; h < GST_VIDEO_FRAME_COMP_HEIGHT (&vframe, 0); h++) {
    /* FIXME: memcpy */
    memcpy (dest, src, GST_VIDEO_FRAME_PLANE_STRIDE (&vframe, 0));
    dest += layout.rowPitch;
    src += GST_VIDEO_FRAME_PLANE_STRIDE (&vframe, 0);
    g_assert (dest - staging_map_info.data - layout.offset <= layout.size);
  }
  gst_video_frame_unmap (&vframe);
  gst_memory_unmap ((GstMemory *) staging, &staging_map_info);

  {
    VkCommandBufferBeginInfo cmd_buf_info = { 0, };

    cmd_buf_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    cmd_buf_info.pNext = NULL;
    cmd_buf_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    cmd_buf_info.renderPass = VK_NULL_HANDLE;
    cmd_buf_info.subpass = 0;
    cmd_buf_info.framebuffer = VK_NULL_HANDLE;
    cmd_buf_info.occlusionQueryEnable = FALSE;
    cmd_buf_info.queryFlags = 0;
    cmd_buf_info.pipelineStatistics = 0;

    err = vkBeginCommandBuffer (cmd, &cmd_buf_info);
    if (gst_vulkan_error_to_g_error (err, error, "vkBeginCommandBuffer") < 0)
      return FALSE;
  }

  if (!_swapper_set_image_layout_with_cmd (swapper, cmd, swap_mem,
          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, error)) {
    return FALSE;
  }

  if (!_swapper_set_image_layout_with_cmd (swapper, cmd, staging,
          VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, error)) {
    return FALSE;
  }

  /* FIXME: center rect */
#if 0
  /* XXX: doesn't work with LunarG's example driver. According to LunarG,
   * it's not implemented */
  {
    VkImageBlit blit_image = { 0, };

    GST_VK_IMAGE_SUBRESOURCE_LAYERS (blit_image.srcSubresource,
        VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1);
    GST_VK_OFFSET3D (blit_image.srcOffset, 0, 0, 0);
    GST_VK_EXTENT3D (blit_image.srcExtent,
        GST_VIDEO_INFO_WIDTH (&swapper->v_info),
        GST_VIDEO_INFO_HEIGHT (&swapper->v_info), 1);
    GST_VK_IMAGE_SUBRESOURCE_LAYERS (blit_image.dstSubresource,
        VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1);
    GST_VK_OFFSET3D (blit_image.dstOffset, 0, 0, 0);
    GST_VK_EXTENT3D (blit_image.dstExtent, swap_mem->create_info.extent.width,
        swap_mem->create_info.extent.height, 1);

    /* FIXME: copy */
    vkCmdBlitImage (cmd, staging->image,
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, swap_mem->image,
        VK_IMAGE_LAYOUT_GENERAL, 1, &blit_image, VK_TEX_FILTER_LINEAR);
  }
#else
  wt = MIN (swap_mem->create_info.extent.width,
      GST_VIDEO_INFO_WIDTH (&swapper->v_info));
  ht = MIN (swap_mem->create_info.extent.height,
      GST_VIDEO_INFO_HEIGHT (&swapper->v_info));

  {
    VkImageCopy copy_image = { 0, };

    GST_VK_IMAGE_SUBRESOURCE_LAYERS (copy_image.srcSubresource,
        VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1);
    GST_VK_OFFSET3D (copy_image.srcOffset, 0, 0, 0);
    GST_VK_IMAGE_SUBRESOURCE_LAYERS (copy_image.dstSubresource,
        VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1);
    GST_VK_OFFSET3D (copy_image.dstOffset, 0, 0, 0);
    GST_VK_EXTENT3D (copy_image.extent, wt, ht, 1);

    /* FIXME: copy */
    vkCmdCopyImage (cmd, staging->image,
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, swap_mem->image,
        VK_IMAGE_LAYOUT_GENERAL, 1, &copy_image);
  }
#endif

  if (!_swapper_set_image_layout_with_cmd (swapper, cmd, staging,
          VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, error)) {
    return FALSE;
  }

  err = vkEndCommandBuffer (cmd);
  if (gst_vulkan_error_to_g_error (err, error, "vkEndCommandBuffer") < 0)
    return FALSE;

  cmd_data->cmd = cmd;
  cmd_data->notify = (GDestroyNotify) gst_memory_unref;
  cmd_data->data = staging;

  if (!_new_fence (swapper->device, &cmd_data->fence, error)) {
    return FALSE;
  }

  return TRUE;
}

static gboolean
_render_buffer_unlocked (GstVulkanSwapper * swapper,
    GstBuffer * buffer, GError ** error)
{
  VkSemaphore semaphore = { 0, };
  VkSemaphoreCreateInfo semaphore_info = { 0, };
  VkPresentInfoKHR present;
  struct cmd_data cmd_data = { 0, };
  guint32 swap_idx;
  VkResult err, present_err;

  semaphore_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
  semaphore_info.pNext = NULL;
  semaphore_info.flags = VK_FENCE_CREATE_SIGNALED_BIT;

  if (!buffer) {
    g_set_error (error, GST_VULKAN_ERROR,
        VK_ERROR_INITIALIZATION_FAILED, "Invalid buffer");
    goto error;
  }

  if (g_atomic_int_get (&swapper->to_quit)) {
    g_set_error (error, GST_VULKAN_ERROR, VK_ERROR_SURFACE_LOST_KHR,
        "Output window was closed");
    goto error;
  }

  gst_buffer_replace (&swapper->current_buffer, buffer);

reacquire:
  err = vkCreateSemaphore (swapper->device->device, &semaphore_info,
      NULL, &semaphore);
  if (gst_vulkan_error_to_g_error (err, error, "vkCreateSemaphore") < 0)
    goto error;

  err =
      swapper->AcquireNextImageKHR (swapper->device->device,
      swapper->swap_chain, -1, semaphore, VK_NULL_HANDLE, &swap_idx);
  /* TODO: Deal with the VK_SUBOPTIMAL_KHR and VK_ERROR_OUT_OF_DATE_KHR */
  if (err == VK_ERROR_OUT_OF_DATE_KHR) {
    GST_DEBUG_OBJECT (swapper, "out of date frame acquired");

    vkDestroySemaphore (swapper->device->device, semaphore, NULL);
    if (!_swapchain_resize (swapper, error))
      goto error;
    goto reacquire;
  } else if (gst_vulkan_error_to_g_error (err, error,
          "vkAcquireNextImageKHR") < 0) {
    goto error;
  }

  if (!_build_render_buffer_cmd (swapper, swap_idx, buffer, &cmd_data, error))
    goto error;

  {
    VkSubmitInfo submit_info = { 0, };

    submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit_info.pNext = NULL;
    submit_info.waitSemaphoreCount = 0;
    submit_info.pWaitSemaphores = NULL;
    submit_info.commandBufferCount = 1;
    submit_info.pCommandBuffers = &cmd_data.cmd;
    submit_info.signalSemaphoreCount = 0;
    submit_info.pSignalSemaphores = NULL;

    err =
        vkQueueSubmit (swapper->queue->queue, 1, &submit_info, cmd_data.fence);
    if (gst_vulkan_error_to_g_error (err, error, "vkQueueSubmit") < 0) {
      return FALSE;
    }
  }

  present.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
  present.pNext = NULL;
  present.waitSemaphoreCount = 1;
  present.pWaitSemaphores = &semaphore;
  present.swapchainCount = 1;
  present.pSwapchains = &swapper->swap_chain;
  present.pImageIndices = &swap_idx;
  present.pResults = &present_err;

  err = swapper->QueuePresentKHR (swapper->queue->queue, &present);
  if (err == VK_ERROR_OUT_OF_DATE_KHR) {
    GST_DEBUG_OBJECT (swapper, "out of date frame submitted");

    if (!_swapchain_resize (swapper, error))
      goto error;

    vkDestroySemaphore (swapper->device->device, semaphore, NULL);
    if (cmd_data.cmd)
      vkFreeCommandBuffers (swapper->device->device, swapper->device->cmd_pool,
          1, &cmd_data.cmd);
    cmd_data.cmd = VK_NULL_HANDLE;
    if (cmd_data.fence)
      vkDestroyFence (swapper->device->device, cmd_data.fence, NULL);
    cmd_data.fence = VK_NULL_HANDLE;
    if (cmd_data.notify)
      cmd_data.notify (cmd_data.data);
    cmd_data.notify = VK_NULL_HANDLE;

    goto reacquire;
  } else if (gst_vulkan_error_to_g_error (err, error, "vkQueuePresentKHR") < 0)
    goto error;

  err = vkWaitForFences (swapper->device->device, 1, &cmd_data.fence, TRUE, -1);
  if (gst_vulkan_error_to_g_error (err, error, "vkWaitForFences") < 0)
    goto error;

  if (semaphore)
    vkDestroySemaphore (swapper->device->device, semaphore, NULL);
  if (cmd_data.cmd)
    vkFreeCommandBuffers (swapper->device->device, swapper->device->cmd_pool,
        1, &cmd_data.cmd);
  if (cmd_data.fence)
    vkDestroyFence (swapper->device->device, cmd_data.fence, NULL);
  if (cmd_data.notify)
    cmd_data.notify (cmd_data.data);
  return TRUE;

error:
  {
    if (semaphore)
      vkDestroySemaphore (swapper->device->device, semaphore, NULL);
    if (cmd_data.cmd)
      vkFreeCommandBuffers (swapper->device->device, swapper->device->cmd_pool,
          1, &cmd_data.cmd);
    if (cmd_data.fence)
      vkDestroyFence (swapper->device->device, cmd_data.fence, NULL);
    if (cmd_data.notify)
      cmd_data.notify (cmd_data.data);
    return FALSE;
  }
}

gboolean
gst_vulkan_swapper_render_buffer (GstVulkanSwapper * swapper,
    GstBuffer * buffer, GError ** error)
{
  gboolean ret;

  RENDER_LOCK (swapper);
  ret = _render_buffer_unlocked (swapper, buffer, error);
  RENDER_UNLOCK (swapper);

  return ret;
}

static void
_on_window_draw (GstVulkanWindow * window, GstVulkanSwapper * swapper)
{
  GError *error = NULL;

  RENDER_LOCK (swapper);
  if (!swapper->current_buffer) {
    RENDER_UNLOCK (swapper);
    return;
  }

  /* TODO: perform some rate limiting of the number of redraw events */
  if (!_render_buffer_unlocked (swapper, swapper->current_buffer, &error))
    GST_ERROR_OBJECT (swapper, "Failed to redraw buffer %p %s",
        swapper->current_buffer, error->message);
  RENDER_UNLOCK (swapper);
}
