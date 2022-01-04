#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <math.h>

#include "lgfx_qoi.h"

#ifndef MIN
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif

// color spaces
#define QOI_SRGB   0
#define QOI_LINEAR 1

#define QOI_HEADER_SIZE 14
#define QOI_PIXELS_MAX ((unsigned int)400000000)

// QOI header
static const uint32_t qoi_sig = 0x716F6966; // {'q','o','i','f'};
// QOI footer
static const uint8_t qoi_padding[8] = {0,0,0,0,0,0,0,1};


typedef union
{
  struct { unsigned char r, g, b, a; } rgba;
  unsigned int v;
} qoi_rgba_t;


typedef struct __attribute__((packed)) _qoi_desc_t
{
  unsigned int width;
  unsigned int height;
  unsigned char channels;
  unsigned char colorspace;
} qoi_desc_t;


typedef enum
{
  QOI_OP_INDEX  = 0x00, /* 00xxxxxx */
  QOI_OP_DIFF   = 0x40, /* 01xxxxxx */
  QOI_OP_LUMA   = 0x80, /* 10xxxxxx */
  QOI_OP_RUN    = 0xc0, /* 11xxxxxx */
  QOI_OP_RGB    = 0xfe, /* 11111110 */
  QOI_OP_RGBA   = 0xff, /* 11111111 */
  QOI_MASK_2    = 0xc0  /* 11000000 */
} qoi_flag_t;


typedef enum
{
  QOI_STATE_OP_INDEX  = 0x00, /* 00xxxxxx */
  QOI_STATE_OP_DIFF   = 0x40, /* 01xxxxxx */
  QOI_STATE_OP_LUMA   = 0x80, /* 10xxxxxx */
  QOI_STATE_OP_RUN    = 0xc0, /* 11xxxxxx */
  QOI_STATE_OP_RGB    = 0xfe, /* 11111110 */
  QOI_STATE_OP_RGBA   = 0xff, /* 11111111 */
} qoi_state_t;


// typedef struct _qoi_t qoi_t; // declared in qoi.h
struct _qoi_t
{
  qoi_rgba_t* pixelBuffer;
  qoi_rgba_t px;

  uint32_t drawing_x;
  uint32_t drawing_y;

  void *user_data;
  qoi_draw_callback_t draw_callback;
  qoi_init_callback_t init_callback;

  qoi_desc_t desc;
  uint8_t repeat;
  qoi_rgba_t *index;
};

#ifdef QOI_DEBUG
#define debug_printf(...) fprintf(stderr, __VA_ARGS__)
#define QOI_ERROR(...) (fprintf(stderr, __VA_ARGS__), -2)
#else
#define debug_printf(...) ((void)0)
#define QOI_ERROR(...) ( -2 )
#endif


static inline uint8_t QOI_COLOR_HASH( const qoi_rgba_t *c )
{
  return 0x3F & (c->rgba.r*3 + c->rgba.g*5 + c->rgba.b*7 + c->rgba.a*11);
}


static uint8_t read_uint8(const uint8_t *p)
{
  return *p;
}

static uint32_t read_uint32(const uint8_t *p)
{
  return (p[0] << 24)
       | (p[1] << 16)
       | (p[2] <<  8)
       | (p[3] <<  0)
  ;
}

static void write_uint32(uint8_t *bytes, int *p, uint32_t v)
{
  bytes[(*p)++] = (uint8_t)(v >> 24);
  bytes[(*p)++] = (uint8_t)(v >> 16);
  bytes[(*p)++] = (uint8_t)(v >>  8);
  bytes[(*p)++] = (uint8_t)v;
}

int lgfx_qoi_feed(qoi_t *qoi, const uint8_t *buf, size_t len)
{
  size_t consume = 0;

  if ( len < sizeof(qoi_padding) ) { return -2; }
  len -= sizeof(qoi_padding);

  if (qoi->desc.width == 0)
  {
    if ( len < QOI_HEADER_SIZE ) { return -2; }

    if (qoi_sig != read_uint32(buf)) { return QOI_ERROR("Incorrect QOI signature"); }

    qoi->desc.width       = read_uint32(buf +  4);
    qoi->desc.height      = read_uint32(buf +  8);
    qoi->desc.channels    = read_uint8(buf + 12);
    qoi->desc.colorspace  = read_uint8(buf + 13);
    qoi->px.rgba.a = 255;

    if( qoi->desc.width == 0 || qoi->desc.height == 0 || qoi->desc.colorspace > 1 ) return QOI_ERROR("Incorrect QOI signature");
    if (qoi->desc.channels != 0 && qoi->desc.channels != 3 && qoi->desc.channels != 4) return QOI_ERROR("Bad channels count");
    // if( qoi->desc.height >= QOI_PIXELS_MAX / qoi->desc.width ) return QOI_ERROR("Image too big");

    qoi->pixelBuffer = (qoi_rgba_t*)malloc(qoi->desc.width * sizeof(qoi_rgba_t));
    if (qoi->pixelBuffer == NULL) { return QOI_ERROR("Insufficient memory"); }

    if (qoi->init_callback) qoi->init_callback(qoi, qoi->desc.width, qoi->desc.height);

    consume = QOI_HEADER_SIZE;
  }

  uint32_t x = qoi->drawing_x;

  while ( consume < len )
  {
    if (qoi->repeat > 0)
    {
      uint32_t xe = x + qoi->repeat;
      if (xe > qoi->desc.width) { xe = qoi->desc.width; }
      qoi->repeat -= xe - x;
      do
      {
        qoi->pixelBuffer[x] = qoi->px;
      } while (++x != xe);
    }
    else
    {
      uint8_t b1 = buf[consume];

      if (b1 == QOI_OP_RGB)
      {
        qoi->px.rgba.r = buf[++consume];
        qoi->px.rgba.g = buf[++consume];
        qoi->px.rgba.b = buf[++consume];
      }
      else if (b1 == QOI_OP_RGBA)
      {
        qoi->px.rgba.r = buf[++consume];
        qoi->px.rgba.g = buf[++consume];
        qoi->px.rgba.b = buf[++consume];
        qoi->px.rgba.a = buf[++consume];
      }
      else if ((b1 & QOI_MASK_2) == QOI_OP_INDEX)
      {
        qoi->px = qoi->index[b1];
      }
      else if ((b1 & QOI_MASK_2) == QOI_OP_DIFF)
      {
        qoi->px.rgba.r += ((b1 >> 4) & 0x03) - 2;
        qoi->px.rgba.g += ((b1 >> 2) & 0x03) - 2;
        qoi->px.rgba.b += ( b1       & 0x03) - 2;
      }
      else if ((b1 & QOI_MASK_2) == QOI_OP_LUMA)
      {
        uint8_t b2 = buf[++consume];
        int vg = (b1 & 0x3f) - 32;
        qoi->px.rgba.r += vg - 8 + ((b2 >> 4));
        qoi->px.rgba.g += vg;
        qoi->px.rgba.b += vg - 8 +  (b2 & 0x0f);
      }
      else // if ((b1 & QOI_MASK_2) == QOI_OP_RUN)
      {
        qoi->repeat = (b1 & 0x3f);
      }

      ++consume;
      qoi->index[QOI_COLOR_HASH(&qoi->px)] = qoi->px;

      if (qoi->desc.channels != 4) {
        qoi->px.rgba.a = 255;
      }
      qoi->pixelBuffer[x++] = qoi->px;
    }

    if (x < qoi->desc.width) { continue; }
    x = 0;

    if (qoi->draw_callback)
    {
      qoi->draw_callback(qoi, 0, qoi->drawing_y, 1, qoi->desc.width, (const uint8_t*)qoi->pixelBuffer);
    }
    if (++qoi->drawing_y >= qoi->desc.height)
    {
      return -1;
    }
  }
  qoi->drawing_x = x;
  return consume;
}


void lgfx_qoi_reset(qoi_t *qoi)
{
  if (!qoi) return;
  if (qoi->pixelBuffer != NULL) { free(qoi->pixelBuffer);  qoi->pixelBuffer = NULL; }
}


qoi_t *lgfx_qoi_new()
{
  qoi_t *qoi = (qoi_t *)calloc(1, sizeof(qoi_t) );
  if (!qoi) return NULL;
  qoi->index = calloc( 64, sizeof( qoi_rgba_t ) );
  if (!qoi->index) {
    free( qoi );
    return NULL;
  }
  return qoi;
}


void lgfx_qoi_destroy(qoi_t *qoi)
{
  if (qoi) {
    lgfx_qoi_reset(qoi);
    free(qoi->index);
    free(qoi);
  }
}

void lgfx_qoi_set_init_callback(qoi_t *qoi, qoi_init_callback_t callback)
{
  if (!qoi) return;
  qoi->init_callback = callback;
}


void lgfx_qoi_set_draw_callback(qoi_t *qoi, qoi_draw_callback_t callback)
{
  if (!qoi) return;
  qoi->draw_callback = callback;
}


void lgfx_qoi_set_user_data(qoi_t *qoi, void *user_data)
{
  if (!qoi) return;
  qoi->user_data = user_data;
}


void *lgfx_qoi_get_user_data(qoi_t *qoi)
{
  if (!qoi) return NULL;
  return qoi->user_data;
}


// Qoi Encoder


static uint8_t* writeBuffer;
static size_t writeBufferSize = 4096;
static uint32_t writeBufferPos = 0;
lfgx_qoi_writer_func bytes_writer;

static int8_t enc_write_uint8( uint8_t v )
{
  writeBuffer[writeBufferPos++] = v;
  if( writeBufferPos == writeBufferSize )  { // buffer full, write!
    if( bytes_writer ) bytes_writer( writeBuffer, writeBufferSize );
    writeBufferPos = 0;
  }
  return 1;
}


static int8_t enc_write_uint32( uint32_t v )
{
  enc_write_uint8( (uint8_t)(v >> 24) );
  enc_write_uint8( (uint8_t)(v >> 16) );
  enc_write_uint8( (uint8_t)(v >>  8) );
  enc_write_uint8( (uint8_t)v );
  return 4;
}


size_t lgfx_qoi_encoder_write_cb(const void *lineBuffer, uint32_t bufferLen, int w, int h, int num_chans, int flip, lgfx_qoi_encoder_get_row_func get_row, lfgx_qoi_writer_func write_bytes, void *qoienc)
{
  qoi_desc_t desc;
  desc.width      = w;
  desc.height     = h;
  desc.channels   = num_chans;
  desc.colorspace = QOI_SRGB; // QOI_SRGB=0, QOI_LINEAR=1
  writeBufferSize = bufferLen;
  size_t res = lgfx_qoi_encode(lineBuffer, &desc, flip, get_row, write_bytes, qoienc);
  return res;
}


void *lgfx_qoi_encoder_write_fb(const void *lineBuffer, int w, int h, int num_chans, size_t *out_len, int flip, lgfx_qoi_encoder_get_row_func get_row, void *qoienc)
{
  qoi_desc_t desc;
  desc.width      = w;
  desc.height     = h;
  desc.channels   = num_chans;
  desc.colorspace = QOI_SRGB; // QOI_SRGB=0, QOI_LINEAR=1
  writeBufferSize = desc.width * desc.height * (desc.channels + 1) + QOI_HEADER_SIZE + sizeof(qoi_padding);
  size_t res = lgfx_qoi_encode(lineBuffer, &desc, flip, get_row, NULL, qoienc);
  *out_len = res;
  return (void*)writeBuffer;
}



size_t lgfx_qoi_encode(const void *lineBuffer, const qoi_desc_t *desc, int flip, lgfx_qoi_encoder_get_row_func get_row, lfgx_qoi_writer_func write_bytes, void *qoienc)
{
  int i, max_size, p, repeat;
  int px_len, px_end, px_pos, channels;
  uint8_t *pixels = (uint8_t*)lineBuffer;

  qoi_rgba_t *qoi_index = calloc( 64, sizeof( qoi_rgba_t ) );
  qoi_rgba_t px, px_prev;

  if (lineBuffer == NULL)                            { ESP_LOGE("[qoi]", "Bad lineBuffer");    return 0; }
  if( qoi_index == NULL )                            { ESP_LOGE("[qoi]", "OOM");               return 0; }
  if (desc == NULL )                                 { ESP_LOGE("[qoi]", "Bad desc");          return 0; }
  if (desc->width == 0 || desc->height == 0 )        { ESP_LOGE("[qoi]", "Bad w/h");           return 0; }
  if (desc->channels < 3 || desc->channels > 4 )     { ESP_LOGE("[qoi]", "Bad bpp");           return 0; }
  if (desc->colorspace > 1 )                         { ESP_LOGE("[qoi]", "Bad colorspace");    return 0; }
  if (desc->height >= QOI_PIXELS_MAX / desc->width ) { ESP_LOGE("[qoi]", "Too big");           return 0; }

  p = 0;
  writeBufferPos = 0;
  writeBuffer = (uint8_t*)malloc(writeBufferSize);
  if (!writeBuffer) {
    ESP_LOGE("[qoi]", "Can't malloc %d bytes", max_size);
    return 0;
  }

  bytes_writer = write_bytes;

  p += enc_write_uint32( qoi_sig);
  p += enc_write_uint32( desc->width);
  p += enc_write_uint32( desc->height);

  p += enc_write_uint8( desc->channels );
  p += enc_write_uint8( desc->colorspace );

  uint32_t lineBufferLen = desc->width * desc->channels;

  repeat = 0;
  px_prev.rgba.r = 0;
  px_prev.rgba.g = 0;
  px_prev.rgba.b = 0;
  px_prev.rgba.a = 255;
  px = px_prev;

  px_len = desc->width * desc->height * desc->channels;
  px_end = px_len - desc->channels;
  channels = desc->channels;

  for (px_pos = 0; px_pos < px_len; px_pos += channels) {

    uint32_t bufferPos = px_pos%lineBufferLen;
    uint32_t ypos      = px_pos/lineBufferLen;
    if( get_row && bufferPos == 0 ) get_row( pixels, flip, desc->width, 1, ypos, qoienc );

    if (channels == 4) {
      px = *(qoi_rgba_t *)(pixels + bufferPos);
    } else {
      px.rgba.r = pixels[bufferPos + 0];
      px.rgba.g = pixels[bufferPos + 1];
      px.rgba.b = pixels[bufferPos + 2];
    }

    if (px.v == px_prev.v) {
      repeat++;
      if (repeat == 62 || px_pos == px_end) {
        p += enc_write_uint8( (uint8_t)(QOI_OP_RUN | (repeat - 1)) );
        repeat = 0;
      }
    } else {
      int index_pos;

      if (repeat > 0) {
        p += enc_write_uint8( (uint8_t)(QOI_OP_RUN | (repeat - 1)));
        repeat = 0;
      }

      index_pos = QOI_COLOR_HASH(&px);

      if (qoi_index[index_pos].v == px.v) {
        p += enc_write_uint8( (uint8_t)(QOI_OP_INDEX | index_pos) );
      } else {
        qoi_index[index_pos] = px;

        if (px.rgba.a == px_prev.rgba.a) {
          signed char vr = px.rgba.r - px_prev.rgba.r;
          signed char vg = px.rgba.g - px_prev.rgba.g;
          signed char vb = px.rgba.b - px_prev.rgba.b;

          signed char vg_r = vr - vg;
          signed char vg_b = vb - vg;

          if (
            vr > -3 && vr < 2 &&
            vg > -3 && vg < 2 &&
            vb > -3 && vb < 2
          ) {
            p += enc_write_uint8( (uint8_t)(QOI_OP_DIFF + ((vr + 2) << 4) + ((vg + 2) << 2) + (vb + 2)) );
          } else if (
            vg_r >  -9 && vg_r <  8 &&
            vg   > -33 && vg   < 32 &&
            vg_b >  -9 && vg_b <  8
          ) {
            p += enc_write_uint8( (uint8_t)(QOI_OP_LUMA     | (vg   + 32)) );
            p += enc_write_uint8( (uint8_t)((vg_r + 8) << 4 | (vg_b +  8)) );
          } else {
            p += enc_write_uint8( QOI_OP_RGB );
            p += enc_write_uint8( px.rgba.r  );
            p += enc_write_uint8( px.rgba.g  );
            p += enc_write_uint8( px.rgba.b  );
          }
        } else {
          p += enc_write_uint8( QOI_OP_RGBA );
          p += enc_write_uint8( px.rgba.r   );
          p += enc_write_uint8( px.rgba.g   );
          p += enc_write_uint8( px.rgba.b   );
          p += enc_write_uint8( px.rgba.a   );
        }
      }
    }
    px_prev = px;
  }

  for (i = 0; i < (int)sizeof(qoi_padding); i++) {
    p += enc_write_uint8( qoi_padding[i] );
  }

  if( write_bytes ) {
    if( writeBufferPos>0 ) write_bytes( writeBuffer, writeBufferPos );
    free( writeBuffer );
  }

  free( qoi_index );

  return p;
}

