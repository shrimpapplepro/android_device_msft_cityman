/*
 * IMX230 sensor library for the OnePlus 2 (msm8994, VFE46) mm-camera stack on cityman.
 *
 * The only IMX230 library is Motorola's (clark, msm8992). Its sensor_lib_t was
 * customised and does not match the OnePlus 2 sensor framework, which rejects it
 * in sensor_get_capabilities(). This library loads Motorola's (renamed to
 * libmmcamera_mot_imx230_moto.so), takes the OnePlus ov13860 struct as a template
 * for the framework's layout and copies Motorola's IMX230 data across.
 *
 * Offsets were measured from the two binaries (relocations + values):
 *   [0x000,0xc70)          identical layout (slave info, output, reg addrs, aec, settings)
 *   out_info[10] @0xc70    OP2 0x30/entry, Moto 0x38/entry (two extra Moto words)
 *   [0xe50,0x1424)         OP2 == Moto[0xea0,...) (+0x50: the out_info difference)
 *   chromatix[10] @0x1424  OP2 13 names/entry, Moto 11 @0x1474 (OP2 adds 2 panorama)
 *   0x162c                 chromatix entry count, same offset in both
 *   0x1630/0x1634          calculate_exposure / fill_exposure_array (OP2 7-arg fill)
 *   0x1638..0x1640         optional OP2 callbacks (NULL-checked by the framework)
 *   0x1644..               Moto equivalents sit 0xc lower
 */
#include <dlfcn.h>
#include <stdint.h>
#include <string.h>
#include <log/log.h>

#undef LOG_TAG
#define LOG_TAG "imx230_adapter"

#define OP2_SIZE        0x1798
#define N_RES           10
#define OP2_OUT         0x0c70
#define OP2_OUT_SZ      0x30
#define MOT_OUT_SZ      0x38
#define OP2_MID         0x0e50
#define MOT_MID         0x0ea0
#define OP2_CHROMA      0x1424
#define MOT_CHROMA      0x1474
#define OP2_CHROMA_SZ   0x34
#define MOT_CHROMA_SZ   0x2c
#define CHROMA_COUNT    0x162c
#define EXP_CALC        0x1630
#define EXP_FILL        0x1634

/* IMX230 SMIA++ registers */
#define REG_FRAME_LENGTH  0x0340
#define REG_COARSE_TIME   0x0202
#define REG_ANALOG_GAIN   0x0204
#define MAX_ANALOG_CODE   480          /* 512 - 512/16: 16x analog */

struct reg_array { uint16_t addr; uint16_t data; uint32_t delay; };
struct reg_setting {                   /* layout from the OP2 ov13860 fill function */
  struct reg_array *regs;              /* +0  */
  uint16_t size;                       /* +4  */
  uint32_t addr_type;                  /* +8  2 = word address */
  uint32_t data_type;                  /* +12 1 = byte data */
  uint16_t delay;                      /* +16 */
};
struct exp_info {                      /* as written by the OP2 ov13860 calc function */
  uint32_t reg_gain;                   /* +0  */
  uint32_t line_count;                 /* +4  */
  uint32_t sensor_digital_gain;        /* +8  (ov13860 sets 1) */
  float sensor_real_gain;              /* +12 */
  float reserved;                      /* +16 */
  float digital_gain;                  /* +20 ISP applies the remainder */
};

static uint8_t lib[OP2_SIZE] __attribute__((aligned(8)));

static uint16_t real_to_reg(float gain) {
  if (gain < 1.0f) gain = 1.0f;
  int code = (int)(512.0f - 512.0f / gain);
  if (code < 0) code = 0;
  if (code > MAX_ANALOG_CODE) code = MAX_ANALOG_CODE;
  return (uint16_t)code;
}

static float reg_to_real(uint16_t code) { return 512.0f / (512.0f - (float)code); }

static int32_t imx230_calculate_exposure(float real_gain, uint32_t line_count, struct exp_info *e) {
  if (!e) return -1;
  e->reg_gain = real_to_reg(real_gain);
  e->sensor_real_gain = reg_to_real((uint16_t)e->reg_gain);
  e->line_count = line_count;
  e->sensor_digital_gain = 1;
  e->digital_gain = real_gain / e->sensor_real_gain;
  return 0;
}

/* OP2 7-argument form: gain (low 16 = analog code), ?, line_count, frame_length,
 * ?, hdr flags, reg_setting. Only analog gain and exposure are programmed here. */
static int32_t imx230_fill_exposure_array(uint32_t gain, uint32_t unused1, uint32_t line_count,
    uint32_t fl_lines, uint32_t unused4, uint32_t hdr, struct reg_setting *rs) {
  (void)unused1; (void)unused4; (void)hdr;
  if (!rs || !rs->regs) return -1;
  uint16_t g = (uint16_t)(gain & 0xffff);
  struct reg_array *r = rs->regs;
  int n = 0;
  r[n++] = (struct reg_array){REG_FRAME_LENGTH,     (fl_lines >> 8) & 0xff, 0};
  r[n++] = (struct reg_array){REG_FRAME_LENGTH + 1,  fl_lines & 0xff,       0};
  r[n++] = (struct reg_array){REG_COARSE_TIME,      (line_count >> 8) & 0xff, 0};
  r[n++] = (struct reg_array){REG_COARSE_TIME + 1,   line_count & 0xff,     0};
  r[n++] = (struct reg_array){REG_ANALOG_GAIN,      (g >> 8) & 0xff,       0};
  r[n++] = (struct reg_array){REG_ANALOG_GAIN + 1,   g & 0xff,             0};
  rs->size = (uint16_t)n;
  rs->addr_type = 2;
  rs->data_type = 1;
  rs->delay = 0;
  return 0;
}

static void *open_sym(const char *libname, const char *sym) {
  void *h = dlopen(libname, RTLD_NOW | RTLD_LOCAL);
  if (!h) { ALOGE("dlopen %s: %s", libname, dlerror()); return NULL; }
  void *(*fn)(void) = (void *(*)(void))dlsym(h, sym);
  if (!fn) { ALOGE("dlsym %s:%s: %s", libname, sym, dlerror()); return NULL; }
  return fn();
}

#define P(base, off) (*(const char **)((base) + (off)))
#define U32(base, off) (*(uint32_t *)((base) + (off)))

void *sensor_open_lib(void) {
  const uint8_t *tmpl = open_sym("libmmcamera_ov13860.so", "sensor_open_lib");
  const uint8_t *mot = open_sym("libmmcamera_mot_imx230_moto.so", "sensor_open_lib");
  if (!tmpl || !mot) return NULL;

  memcpy(lib, tmpl, OP2_SIZE);                 /* OP2 layout + unknown tail fields */
  memcpy(lib, mot, OP2_OUT);                   /* identical region */
  for (int i = 0; i < N_RES; i++) {
    uint8_t *o = lib + OP2_OUT + i * OP2_OUT_SZ;
    memcpy(o, mot + OP2_OUT + i * MOT_OUT_SZ, OP2_OUT_SZ);
    U32(o, 0x2c) = 0;                          /* is_pdaf_supported: PDAF off for now */
    /* Moto's 120fps HFR mode has min_fps == max_fps == 120; the OP2 daemon pairs that
     * min with the 30fps preview max and Snap dies on Range(120, 30). OP2 uses 10. */
    if (U32(o, 0x1c) == 2 /* HFR */) *(float *)(o + 0x14) = 10.0f;
  }
  memcpy(lib + OP2_MID, mot + MOT_MID, OP2_CHROMA - OP2_MID);
  for (int i = 0; i < N_RES; i++) {
    uint8_t *o = lib + OP2_CHROMA + i * OP2_CHROMA_SZ;
    const uint8_t *m = mot + MOT_CHROMA + i * MOT_CHROMA_SZ;
    for (int j = 0; j < 11; j++) P(o, j * 4) = P(m, j * 4);
    P(o, 11 * 4) = P(m, 2 * 4);                /* panorama -> snapshot */
    P(o, 12 * 4) = P(m, 6 * 4);                /* cpp_panorama -> cpp_snapshot */
  }
  U32(lib, CHROMA_COUNT) = U32(mot, CHROMA_COUNT);
  *(void **)(lib + EXP_CALC) = (void *)imx230_calculate_exposure;
  *(void **)(lib + EXP_FILL) = (void *)imx230_fill_exposure_array;
  *(void **)(lib + 0x1638) = NULL;
  *(void **)(lib + 0x163c) = NULL;
  *(void **)(lib + 0x1640) = NULL;
  *(void **)(lib + 0x1684) = NULL;             /* PDAF stats parser (ov13860's) */
  U32(lib, 0x1644) = U32(mot, 0x1638);         /* 5344: Moto field 0xc lower */
  U32(lib, 0x1648) = U32(mot, 0x163c);

  ALOGI("built OP2 sensor_lib for %s: %u modes, %u chromatix sets, %ux%u",
        (const char *)lib, U32(lib, 0x0c4c), U32(lib, CHROMA_COUNT),
        *(uint16_t *)(lib + OP2_OUT), *(uint16_t *)(lib + OP2_OUT + 2));
  return lib;
}
