/* SPDX-License-Identifier: Apache-2.0 */
extern void qpk_camera_timing_log(void);

static void qpk_camera_save_jpeg(const uint8_t *image,size_t size)
{
  struct qpk_photo_input input={.format=QPK_PHOTO_JPEG,.data=image,.bytes=size,
    .root_override=QPK_PHOTO_DIR};
  qpk_camera_stage_photo(&input);
}

/* P4 manages PSRAM cache through Espressif's HAL: CONFIG_ARCH_DCACHE is
 * unset, so generic up_clean_dcache() expands to nothing on this build.
 * Use the same real C2M writeback as the LCD driver, for all three pages. */
extern int esp_mipi_dsi_flush_framebuffer(void *addr, size_t len);

static int qpk_camera_clean_frame(uint16_t *pixels, size_t bytes)
{
  uintptr_t address = (uintptr_t)pixels;
  while (bytes)
    {
      size_t chunk = bytes > 16384 ? 16384 : bytes;
      int ret = esp_mipi_dsi_flush_framebuffer((void *)address, chunk);
      if (ret < 0) return ret;
      address += chunk;
      bytes -= chunk;
    }
  return 0;
}

static uint64_t qpk_usb_preview_now(void)
{
#ifdef CONFIG_ESPRESSIF_MIPI_DSI
  return esp_camera_timing_now();
#else
  return qpk_now_us();
#endif
}

/* Runs entirely outside LVGL. Decoder and input buffers belong to this
 * worker and are released before cleanup joins and resumes the desktop. */
static void *qpk_camera_usb_thread(pthread_addr_t arg)
{
  struct qpk_camera_s *camera = arg;
  uint8_t *input = NULL;
  void *decoder = NULL;
  int error = 0;
  struct uvc_camera_start mode = camera->usb_mode;
  if (qpk_camera_cancelled()) goto done;
  if (ioctl(camera->fd, UVCIOC_START, (unsigned long)&mode) < 0)
    { error = -errno; goto done; }
  pthread_mutex_lock(&g_camera_lock);
  camera->usb_mode = mode;
  pthread_mutex_unlock(&g_camera_lock);
  if (qpk_camera_cancelled()) goto done;
  input = malloc(mode.max_frame_size + 512);
  decoder = qpk_mjpeg_create();
  if (!input || !decoder) { error = -ENOMEM; goto done; }
  /* Smooth live view; JPEG photos retain the original negotiated frame. */
  qpk_mjpeg_preview_limits(decoder,320,240);
  uint64_t last_frame = qpk_now_ms();
  unsigned decode_failures = 0;
  uint64_t profile_start = qpk_usb_preview_now(), costs[6] = {0};
  unsigned samples = 0;
  struct pollfd pfd = { .fd = camera->fd, .events = POLLIN };
  while (!qpk_camera_cancelled())
    {
      uint64_t started = qpk_usb_preview_now();
      int ret = poll(&pfd, 1, 100);
      costs[0] += qpk_usb_preview_now() - started;
      if (qpk_camera_cancelled()) break;
      if (ret < 0) { if (errno == EINTR) continue; error = -errno; break; }
      if (pfd.revents & (POLLHUP | POLLERR | POLLNVAL)) { error = -ENODEV; break; }
      if (!ret)
        { if (qpk_now_ms() - last_frame > 6000) { error = -ETIMEDOUT; break; } continue; }
      started = qpk_usb_preview_now();
      ssize_t size = read(camera->fd, input, mode.max_frame_size);
      costs[1] += qpk_usb_preview_now() - started;
      if (size < 0) { if (errno == EAGAIN || errno == EINTR) continue; error = -errno; break; }
      if (!size) continue;
      unsigned next = (camera->fb_page + 1) % QPK_CAMERA_FRAMEBUFFER_COUNT;
      uint16_t *target = (uint16_t *)((uint8_t *)camera->fb_plane.fbmem + next * QPK_CAMERA_FRAME_SIZE);
      /* Pending photos and video retain the original full-canvas path. A
       * shutter arriving during fast preview is handled on the next frame. */
      pthread_mutex_lock(&g_camera_lock);
      bool photo_needs_frame = g_photo_pending;
      pthread_mutex_unlock(&g_camera_lock);
      struct qpk_camera_view_state controls=qpk_camera_view_state();
      bool prepared = controls.zoom==100 && !qpk_video_selected() && !photo_needs_frame;
      started = qpk_usb_preview_now();
      if (prepared) {
        struct camera_reference_rect view=camera_reference_output(QPK_CAMERA_BAR_LEFT,
          mode.width,mode.height,controls.zoom,controls.aspect);
        struct camera_reference_rect crop=camera_reference_crop(QPK_CAMERA_BAR_LEFT,
          mode.width,mode.height,controls.zoom,controls.aspect);
        struct qpk_mjpeg_view options={.canvas_width=QPK_CAMERA_BAR_LEFT,.canvas_height=QPK_CAMERA_RAW_HEIGHT,
          .stride=QPK_CAMERA_RAW_WIDTH,.width=view.w,.height=view.h,.crop_x=crop.x,.crop_y=crop.y,
          .crop_width=crop.w,.crop_height=crop.h,.style=controls.style};
        ret = qpk_mjpeg_decode_preview(decoder,input,size,mode.max_frame_size+512,
          target+view.y*QPK_CAMERA_RAW_WIDTH+view.x,mode.width,mode.height,&options);
      } else ret = qpk_mjpeg_decode_region(decoder, input, size, mode.max_frame_size + 512,
                             target, QPK_CAMERA_BAR_LEFT, QPK_CAMERA_RAW_HEIGHT, QPK_CAMERA_RAW_WIDTH,
                             mode.width, mode.height);
      costs[2] += qpk_usb_preview_now() - started;
      if (ret < 0)
        { if (++decode_failures == 10) { error = ret; break; } continue; }
      decode_failures = 0;
      started = qpk_usb_preview_now();
      if (!prepared) qpk_video_frame_region(target,QPK_CAMERA_BAR_LEFT,QPK_CAMERA_RAW_HEIGHT,QPK_CAMERA_RAW_WIDTH,mode.width,mode.height);
      last_frame = qpk_now_ms();
      camera->captured_frames++;
      if (qpk_camera_cancelled()) break;
      /* The frame is saved before any reference UI is composed into it. */
      int photo_status = qpk_camera_bar_status();
      if (!prepared && photo_status == 1)
        qpk_camera_capture_ring(target, qpk_camera_rgb565(0xf6f7ff));
      /* The untouched JPEG is lossless to the incoming stream at 1x. For a
       * zoomed shot, save the matching RGB565 crop before drawing any UI. */
      if (!prepared) {
        if(qpk_camera_zoom_percent()==100&&qpk_camera_style()==0&&qpk_camera_aspect()==0)qpk_camera_save_jpeg(input,qpk_mjpeg_frame_size(decoder));
        else qpk_camera_save_region(target,QPK_CAMERA_BAR_LEFT,mode.width,mode.height);
      }
      photo_status = qpk_camera_bar_status();
      /* The reference's guides and shutter extend into the decoded image.
       * Re-compose after every frame, not just when the old right rail changes. */
      qpk_camera_bar_draw(target,QPK_CAMERA_BAR_LEFT,prepared?&controls:NULL,mode.width,mode.height);
      qpk_wake_overlay_blit(target,QPK_CAMERA_RAW_WIDTH,QPK_CAMERA_RAW_HEIGHT,QPK_CAMERA_RAW_WIDTH);
      costs[3] += qpk_usb_preview_now() - started;
      size_t dirty_bytes = QPK_CAMERA_FRAME_SIZE;
      started = qpk_usb_preview_now();
      ret = qpk_camera_clean_frame(target, dirty_bytes);
      costs[4] += qpk_usb_preview_now() - started;
      if (ret < 0) { error = ret; break; }
      camera->fb_plane.yoffset = next * QPK_CAMERA_RAW_HEIGHT;
      started = qpk_usb_preview_now();
      if (qpk_camera_present(camera->fb_fd,&camera->fb_plane) < 0)
        { camera->pan_errors++; error = -errno; break; }
      costs[5] += qpk_usb_preview_now() - started;
      camera->fb_page = next;
      pthread_mutex_lock(&g_camera_lock);
      camera->frames++;
      pthread_mutex_unlock(&g_camera_lock);
      if (camera->frames == 1) printf("[qpk] USB MJPEG first frame %ux%u\n", mode.width, mode.height);
      samples++;
      uint64_t now = qpk_usb_preview_now(), elapsed = now - profile_start;
      if (elapsed >= 5000000)
        {
          struct uvc_camera_stats stats = {0};
          int stats_error = ioctl(camera->fd, UVCIOC_STATS, (unsigned long)&stats) < 0 ? errno : 0;
          unsigned fps10 = (uint64_t)samples * 10000000 / elapsed;
          printf("[usb-preview] fps=%u.%u frames=%lu received=%lu dropped=%lu errors=%lu stats_error=%d avg_us(wait/read/decode/draw/flush/pan)=%lu/%lu/%lu/%lu/%lu/%lu\n",
            fps10/10, fps10%10, (unsigned long)camera->frames,
            (unsigned long)stats.frames, (unsigned long)stats.dropped,
            (unsigned long)stats.errors, stats_error,
            (unsigned long)(costs[0]/samples), (unsigned long)(costs[1]/samples),
            (unsigned long)(costs[2]/samples), (unsigned long)(costs[3]/samples),
            (unsigned long)(costs[4]/samples), (unsigned long)(costs[5]/samples));
          qpk_camera_timing_log();
          memset(costs, 0, sizeof(costs)); samples = 0; profile_start = qpk_usb_preview_now();
        }
    }
done:
  ioctl(camera->fd, UVCIOC_STOP, 0);
  qpk_mjpeg_destroy(decoder);
  free(input);
  pthread_mutex_lock(&g_camera_lock);
  camera->thread_error = error;
  camera->thread_alive = false;
  pthread_mutex_unlock(&g_camera_lock);
  return NULL;
}
