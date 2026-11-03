#include "face_engine.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "esp_partition.h"
#include "human_face_detect.hpp"
#include "human_face_recognition.hpp"
#include "dl_image.hpp"

static const char *TAG = "face_engine";

static HumanFaceDetect *s_det = nullptr;
static HumanFaceRecognizer *s_recog = nullptr;
static uint8_t *s_rgb888 = nullptr;
static int s_rgb888_px = 0;

/* RGB565（大端）→ RGB888，**同时逆时针转 90°**
 *
 * 为什么要转：传感器在板上是横装的、屏幕是竖屏，摄像头原始帧里的脸是躺着的。
 * 检测器还能找到脸，但关键点对齐会偏，识别相似度从 0.8+ 掉到 0.69~0.72
 * （2026-10-31 实测 face_lvgl 横屏用时更高）。
 * 转正后喂模型，同时检测框坐标直接就是竖屏预览坐标，UI 侧不用再做反变换。
 *
 * 映射与 bsp_camera_blit(cw=false) 完全一致：dest(x,y) ← src(w-1-y, x) */
static uint8_t *to_rgb888_rot(const uint8_t *src, int w, int h)
{
    const int ow = h;          /* 旋转后宽高互换：320x240 → 240x320 */
    const int oh = w;
    const int px = ow * oh;

    if (s_rgb888 == nullptr || s_rgb888_px != px) {
        if (s_rgb888) heap_caps_free(s_rgb888);
        s_rgb888 = (uint8_t *)heap_caps_malloc((size_t)px * 3, MALLOC_CAP_SPIRAM);
        s_rgb888_px = px;
    }
    if (s_rgb888 == nullptr) return nullptr;

    for (int y = 0; y < oh; y++) {
        uint8_t *d = s_rgb888 + (size_t)y * ow * 3;
        for (int x = 0; x < ow; x++) {
            int sx = w - 1 - y;
            int sy = x;
            const uint8_t *s = src + ((size_t)sy * w + sx) * 2;
            uint16_t v = (uint16_t)((s[0] << 8) | s[1]);   /* 大端 */
            uint8_t r = (v >> 11) & 0x1F;
            uint8_t g = (v >> 5) & 0x3F;
            uint8_t b = v & 0x1F;
            *d++ = (uint8_t)((r << 3) | (r >> 2));
            *d++ = (uint8_t)((g << 2) | (g >> 4));
            *d++ = (uint8_t)((b << 3) | (b >> 2));
        }
    }
    return s_rgb888;
}

/* 组装模型输入：旋转后的尺寸 */
static void fill_img(dl::image::img_t &img, const uint8_t *buf, int w, int h)
{
    img.data = to_rgb888_rot(buf, w, h);
    img.width = (uint16_t)h;    /* 旋转后宽 = 原高 */
    img.height = (uint16_t)w;
    img.pix_type = dl::image::DL_IMAGE_PIX_TYPE_RGB888;
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

    dl::image::img_t img = {};
    fill_img(img, buf, w, h);
    if (img.data == nullptr) return 0;

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

/* ---------------- 识别 / 人脸库 ---------------- */
static esp_err_t mount_storage(void)
{
    static bool mounted = false;
    if (mounted) return ESP_OK;

    const esp_partition_t *part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                                          ESP_PARTITION_SUBTYPE_ANY, "storage");
    if (part == nullptr) {
        ESP_LOGE(TAG, "storage 分区不存在（检查 partitions.csv）");
        return ESP_ERR_NOT_FOUND;
    }

    esp_vfs_fat_mount_config_t cfg = {};
    cfg.format_if_mount_failed = true;
    cfg.max_files = 4;
    cfg.allocation_unit_size = 4096;

    wl_handle_t wl = WL_INVALID_HANDLE;
    esp_err_t err = esp_vfs_fat_spiflash_mount_rw_wl("/storage", "storage", &cfg, &wl);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "挂载 /storage 失败: %s", esp_err_to_name(err));
        return err;
    }
    mounted = true;
    ESP_LOGI(TAG, "/storage 挂载成功");
    return ESP_OK;
}

extern "C" esp_err_t face_recog_init(void)
{
    face_engine_init();
    if (s_recog == nullptr) {
        esp_err_t err = mount_storage();
        if (err != ESP_OK) return err;
        s_recog = new HumanFaceRecognizer("/storage/face.db", HumanFaceFeat::MFN_S8_V1, false);
        ESP_LOGI(TAG, "MFN_S8_V1 ready, %d faces enrolled", (int)s_recog->get_num_feats());
    }
    return ESP_OK;
}

/* 检测一次并返回 img + 结果列表（enroll/recognize 都要用同一份） */
static bool detect_full(const uint8_t *buf, int w, int h,
                        dl::image::img_t &img, std::list<dl::detect::result_t> **res)
{
    if (s_det == nullptr || buf == nullptr) return false;

    fill_img(img, buf, w, h);
    if (img.data == nullptr) return false;

    *res = &s_det->run(img);
    return !(*res)->empty();
}

/* ---------------- 成员枚举 ----------------
 * 库文件本身就是一张按 id 排的稀疏表：
 *   [database_meta 6B][记录 id=1][记录 id=2]...
 *   记录 = uint16 id + feat_len * float32     （id==0 表示该槽已删除）
 * 所以不自己维护索引，直接读文件最可靠——
 * 之前用"自建索引"在清库/删除后 id 必然错位，导致删除总是 ESP_FAIL。 */
#define DB_FILE "/storage/face.db"
#define MEMBER_MAX_IDS 64

typedef struct {
    uint16_t num_feats_total;
    uint16_t num_feats_valid;
    uint16_t feat_len;
} db_meta_t;

extern "C" int face_members(uint16_t *ids, int max)
{
    if (face_recog_init() != ESP_OK) return 0;

    FILE *f = fopen(DB_FILE, "rb");
    if (!f) return 0;

    db_meta_t m = {};
    if (fread(&m, sizeof(m), 1, f) != 1 || m.feat_len == 0 || m.feat_len > 4096) {
        fclose(f);
        return 0;
    }

    const long rec = sizeof(uint16_t) + (long)m.feat_len * sizeof(float);
    int n = 0;
    for (uint16_t i = 0; i < m.num_feats_total && n < max; i++) {
        uint16_t id = 0;
        if (fseek(f, (long)sizeof(m) + rec * i, SEEK_SET) != 0) break;
        if (fread(&id, sizeof(id), 1, f) != 1) break;
        if (id != 0) ids[n++] = id;
    }
    fclose(f);
    return n;
}

extern "C" esp_err_t face_delete_member(uint16_t id)
{
    if (face_recog_init() != ESP_OK) return ESP_FAIL;
    esp_err_t err = s_recog->delete_feat(id);
    int left = (int)s_recog->get_num_feats();

    /* 删空了就把库文件整个删掉：库的 id 是 num_feats_total+1、只增不减，
     * 不清文件的话下次录入会接着 17、18 往上排（实测踩过）。
     * clear_all_feats() 的实现就是 remove(face.db)，下次录入从 1 重新开始。 */
    if (err == ESP_OK && left == 0) {
        s_recog->clear_all_feats();
        ESP_LOGW(TAG, "成员已清空，库文件重置，id 从 1 开始");
    }
    ESP_LOGW(TAG, "delete id=%u -> %s (left %d)", (unsigned)id, esp_err_to_name(err), left);
    return err;
}

extern "C" esp_err_t face_recog_enroll(const uint8_t *buf, int w, int h)
{
    if (face_recog_init() != ESP_OK) return ESP_FAIL;

    dl::image::img_t img = {};
    std::list<dl::detect::result_t> *res = nullptr;
    if (!detect_full(buf, w, h, img, &res)) {
        return ESP_ERR_NOT_FOUND;
    }

    esp_err_t err = s_recog->enroll(img, *res);
    if (err == ESP_OK) {
        ESP_LOGW(TAG, "enroll ok, total %d", (int)s_recog->get_num_feats());
    } else {
        ESP_LOGW(TAG, "enroll fail: %s", esp_err_to_name(err));
    }
    return err;
}

extern "C" esp_err_t face_recog_recognize(const uint8_t *buf, int w, int h,
                                          char *name_out, size_t name_len, float *score_out,
                                          uint16_t *id_out)
{
    if (face_recog_init() != ESP_OK) return ESP_FAIL;

    dl::image::img_t img = {};
    std::list<dl::detect::result_t> *res = nullptr;
    if (!detect_full(buf, w, h, img, &res)) {
        /* 这一帧压根没人脸：和"陌生人"必须区分开，
         * 否则画面一抖就被记一次失败，5 次直接锁门 */
        return ESP_ERR_INVALID_STATE;
    }

    auto out = s_recog->recognize(img, *res);
    /* result_t = {id, similarity}；id == UINT16_MAX 表示陌生人不入库 */
    for (auto &r : out) {
        if (r.id != UINT16_MAX) {
            if (name_out && name_len) {
                snprintf(name_out, name_len, "Person_%u", (unsigned)r.id);
            }
            if (score_out) *score_out = r.similarity;
            if (id_out) *id_out = r.id;
            ESP_LOGW(TAG, "recognize: id=%u sim=%.2f", (unsigned)r.id, r.similarity);
            return ESP_OK;
        }
    }
    return ESP_ERR_NOT_FOUND;
}

extern "C" esp_err_t face_recog_clear(void)
{
    if (face_recog_init() != ESP_OK) return ESP_FAIL;
    esp_err_t err = s_recog->clear_all_feats();
    ESP_LOGW(TAG, "cleared, left %d", (int)s_recog->get_num_feats());
    return err;
}

extern "C" int face_recog_count(void)
{
    return s_recog ? (int)s_recog->get_num_feats() : 0;
}
