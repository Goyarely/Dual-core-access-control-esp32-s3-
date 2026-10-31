#include "face_engine.h"

#include <string.h>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "human_face_detect.hpp"
#include "dl_image.hpp"

static const char *TAG = "face_engine";

static HumanFaceDetect *s_det = nullptr;
static uint8_t *s_rgb888 = nullptr;
static int s_rgb888_px = 0;

/* 检测前统一转 RGB888：摄像头 RGB565 是大端，直接喂给 esp-dl 容易踩字节序问题 */
static uint8_t *to_rgb888(const uint8_t *src, int w, int h)
{
    const int px = w * h;
    if (s_rgb888 == nullptr || s_rgb888_px != px) {
        if (s_rgb888) heap_caps_free(s_rgb888);
        s_rgb888 = (uint8_t *)heap_caps_malloc((size_t)px * 3, MALLOC_CAP_SPIRAM);
        s_rgb888_px = px;
    }
    if (s_rgb888 == nullptr) return nullptr;

    for (int i = 0; i < px; i++) {
        uint16_t v = (uint16_t)((src[2 * i] << 8) | src[2 * i + 1]);   /* 大端 */
        uint8_t r = (v >> 11) & 0x1F;
        uint8_t g = (v >> 5) & 0x3F;
        uint8_t b = v & 0x1F;
        s_rgb888[3 * i + 0] = (uint8_t)((r << 3) | (r >> 2));
        s_rgb888[3 * i + 1] = (uint8_t)((g << 2) | (g >> 4));
        s_rgb888[3 * i + 2] = (uint8_t)((b << 3) | (b >> 2));
    }
    return s_rgb888;
}

extern "C" esp_err_t face_engine_init(void)
{
    if (s_det == nullptr) {
        s_det = new HumanFaceDetect(HumanFaceDetect::MSRMNP_S8_V1, false);
        /* 课程取值：MSR 粗检 0.3 / MNP 精检 0.4；
         * 阈值最终要按 M5 的正负样本分布重标（写进 docs/test_report） */
        s_det->set_score_thr(0.3f, 0);
        s_det->set_score_thr(0.4f, 1);
        ESP_LOGI(TAG, "MSRMNP_S8_V1 ready (thr 0.3/0.4)");
    }
    return ESP_OK;
}

extern "C" int face_engine_detect(const uint8_t *buf, int w, int h, face_box_t *out, int max_boxes)
{
    if (s_det == nullptr || buf == nullptr || out == nullptr || max_boxes <= 0) {
        return 0;
    }

    uint8_t *rgb = to_rgb888(buf, w, h);
    if (rgb == nullptr) return 0;

    dl::image::img_t img = {};
    img.data = rgb;
    img.width = (uint16_t)w;
    img.height = (uint16_t)h;
    img.pix_type = dl::image::DL_IMAGE_PIX_TYPE_RGB888;

    std::list<dl::detect::result_t> &res = s_det->run(img);

    int n = 0;
    for (auto &r : res) {
        if (n >= max_boxes) break;
        out[n].x1 = r.box[0];
        out[n].y1 = r.box[1];
        out[n].x2 = r.box[2];
        out[n].y2 = r.box[3];
        out[n].score = r.score;
        n++;
    }
    return n;
}
