#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/time.h>
#include "librtmp/rtmp.h"
#include "librtmp/log.h"
#include "rtmppush.h"

#define RTMP_TRY_CONNECT_TIMEOUT 2000

static uint32_t get_tick_count(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

static int h264_parse_nalu_header(uint8_t *data, int len)
{
    int  counter, i;
    for (counter = 0, i = 0; i < len; i++) {
        if (data[i] == 0) counter++;
        else if (counter >= 2 && data[i] == 0x01) return i;
        else counter = 0;
    }
    return -1;
}

int h264_parse_key_sps_pps(uint8_t *data, int len, int *key, uint8_t **sps_buf, int *sps_len, uint8_t **pps_buf, int *pps_len)
{
    uint8_t *sbuf, *pbuf;
    int slen = 0, plen = 0, type, i;

#if 0
    printf("%02x %02x %02x %02x %02x %02x %02x %02x\n", data[0 ], data[1 ], data[2 ], data[3 ], data[4 ], data[5 ], data[6 ], data[7 ]);
    printf("%02x %02x %02x %02x %02x %02x %02x %02x\n", data[8 ], data[9 ], data[10], data[11], data[12], data[13], data[14], data[15]);
    printf("%02x %02x %02x %02x %02x %02x %02x %02x\n", data[16], data[17], data[18], data[19], data[20], data[21], data[22], data[23]);
    printf("%02x %02x %02x %02x %02x %02x %02x %02x\n", data[24], data[25], data[26], data[27], data[28], data[29], data[30], data[31]);
    printf("\n");
#endif

    if (key    ) *key     = 0;
    if (sps_buf) *sps_buf = NULL;
    if (sps_len) *sps_len = 0;
    if (pps_buf) *pps_buf = NULL;
    if (pps_len) *pps_len = 0;
    i = h264_parse_nalu_header(data, len);
    if (i > 0 && i+1 < len && (data[i+1] & 0x1f) == 7) { // find sps
        len -= i + 1;
        data+= i + 1;
        i = h264_parse_nalu_header(data, len);
        if (i > 0) {
            sbuf = data;
            slen = i - 3;
            len -= i + 1;
            data+= i + 1;
        } else {
            goto find_frame_data;
        }
        if (len > 2 && (data[0] & 0x1f) == 8) { // find pps
            i = h264_parse_nalu_header(data, len);
            if (i > 0) {
                pbuf = data;
                plen = i - 2;
                len -= i - 2;
                data+= i - 2;
            } else {
                goto find_frame_data;
            }
        }
        if (sps_buf) *sps_buf = sbuf;
        if (sps_len) *sps_len = slen;
        if (pps_buf) *pps_buf = pbuf;
        if (pps_len) *pps_len = plen;
        if (key    ) *key     = 1;
    }

find_frame_data:
    while (1) {
        i = h264_parse_nalu_header(data, len);
        if (i < 0) break;
        data += i;
        len  -= i;
        if (len >= 2) {
            type = data[1] & 0x1f;
            if (type >= 1 && type <= 5) {
                return len - 1;
            }
        }
    }
    return 0;
}

typedef struct {
    RTMP    *rtmp;
    char    *buf_h264;
    int      len_h264;
    char    *buf_alaw;
    int      len_alaw;
    char    *buf_aac ;
    int      len_aac ;
    uint8_t  aac_dec_spec[2];
    uint32_t acc_sync_counter;
    char     url[256];
    #define FLAG_EXIT (1 << 0)
    uint32_t flags;
    uint32_t try_connect_tick;
    pthread_mutex_t lock;
    pthread_t       hthread;
} RTMPPUSHER;

static void* rtmp_tryconnect_proc(void *param)
{
    RTMPPUSHER *pusher  = param;
    uint32_t    curtick = 0;
    int         ret     = 0;

    while (!(pusher->flags & FLAG_EXIT)) {
        curtick = get_tick_count();
        if (RTMP_IsConnected(pusher->rtmp) == 2 || (pusher->try_connect_tick && (int32_t)curtick - pusher->try_connect_tick < RTMP_TRY_CONNECT_TIMEOUT)) { usleep(100 * 1000); continue; }
        pusher->try_connect_tick = curtick ? curtick : 1;

        pthread_mutex_lock(&pusher->lock);
//      printf("rtmp try connect ...\n");

        if (!RTMP_SetupURL(pusher->rtmp, pusher->url)) {
//          printf("RTMP_SetupURL failed !\n");
            ret = -1; goto done;
        }

        RTMP_EnableWrite(pusher->rtmp); // !!important, RTMP_EnableWrite must be called before RTMP_Connect
        if (!RTMP_Connect(pusher->rtmp, NULL) || !RTMP_ConnectStream(pusher->rtmp, 0)) {
//          printf("RTMP_Connect failed !\n");
            ret = -1; goto done;
        }
//      printf("rtmp connect ok !\n");
done:
        pthread_mutex_unlock(&pusher->lock);
    }

//  printf("rtmp_tryconnect_proc exited !\n");
    return NULL;
}

void* rtmp_push_init(char *url, uint8_t *aac_dec_spec)
{
    RTMPPUSHER *pusher = calloc(1, sizeof(RTMPPUSHER));
    if (pusher) {
//      RTMP_LogSetLevel(RTMP_LOGDEBUG);
        pusher->rtmp = RTMP_Alloc();
        if (!pusher->rtmp) { rtmp_push_exit(pusher); return NULL; }
        RTMP_Init(pusher->rtmp);

        strncpy(pusher->url, url, sizeof(pusher->url));
        if (aac_dec_spec) pusher->aac_dec_spec[0] = aac_dec_spec[0];
        if (aac_dec_spec) pusher->aac_dec_spec[1] = aac_dec_spec[1];
        pthread_mutex_init(&pusher->lock, NULL);
        pthread_create(&pusher->hthread, NULL, rtmp_tryconnect_proc, pusher);
        return pusher;
    }
    return NULL;
}

void rtmp_push_exit(void *ctxt)
{
    RTMPPUSHER *pusher = (RTMPPUSHER*)ctxt;
    if (!ctxt) return;

    pthread_mutex_lock(&pusher->lock);
    pusher->flags |= FLAG_EXIT;
    pthread_mutex_unlock(&pusher->lock);
    pthread_join(pusher->hthread, NULL);
    pthread_mutex_destroy(&pusher->lock);

    RTMP_Close(pusher->rtmp);
    RTMP_Free (pusher->rtmp);
    free(pusher->buf_h264);
    free(pusher->buf_alaw);
    free(pusher->buf_aac );
    free(pusher);
}

#define RTMP_HEAD_SIZE (sizeof(RTMPPacket) + RTMP_MAX_HEADER_SIZE)
static void send_sps_pps(RTMPPUSHER *pusher, uint8_t *spsbuf, int spslen, uint8_t *ppsbuf, int ppslen, uint32_t pts)
{
    char        pktbuf[RTMP_HEAD_SIZE+16+spslen+ppslen];
    RTMPPacket *packet =(RTMPPacket*)pktbuf;
    char       *body   = pktbuf + RTMP_HEAD_SIZE;
    int         i      = 0;

    memset(packet, 0, sizeof(RTMPPacket));
    body[i++] = 0x17;
    body[i++] = 0x00;
    body[i++] = 0x00;
    body[i++] = 0x00;
    body[i++] = 0x00;
    body[i++] = 0x01;
    body[i++] = spsbuf[1];
    body[i++] = spsbuf[2];
    body[i++] = spsbuf[3];
    body[i++] = 0xff;
    body[i++] = 0xe1;
    body[i++] = (spslen >> 8) & 0xff;
    body[i++] = (spslen >> 0) & 0xff;
    memcpy(body+i, spsbuf, spslen);
    i += spslen;

    body[i++] = 0x01;
    body[i++] = (ppslen >> 8) & 0xff;
    body[i++] = (ppslen >> 0) & 0xff;
    memcpy(body+i, ppsbuf, ppslen);
    i += ppslen;

    packet->m_headerType  = RTMP_PACKET_SIZE_MEDIUM;
    packet->m_body        = body;
    packet->m_nBodySize   = i;
    packet->m_packetType  = RTMP_PACKET_TYPE_VIDEO;
    packet->m_nChannel    = 0x04;
    packet->m_nTimeStamp  = pts;
    packet->m_nInfoField2 = pusher->rtmp->m_stream_id;
    pthread_mutex_lock(&pusher->lock);
    RTMP_SendPacket(pusher->rtmp, packet, TRUE);
    pthread_mutex_unlock(&pusher->lock);
}

static void send_h264_data(RTMPPUSHER *pusher, uint8_t *data, int len, int key, uint32_t pts)
{
    RTMPPacket *packet;
    char       *body  ;
    int         i = 0;

    if (pusher->len_h264 < RTMP_HEAD_SIZE + 9 + len) {
        pusher->len_h264 = RTMP_HEAD_SIZE + 9 + len;
        if (pusher->buf_h264) free(pusher->buf_h264);
        pusher->buf_h264 = malloc(pusher->len_h264);
//      printf("buf_h264 reallocated !\n");
    }
    if (!pusher->buf_h264) {
        printf("pusher->buf_h264 is null !\n");
        return;
    }

    packet = (RTMPPacket*)pusher->buf_h264;
    body   = pusher->buf_h264 + RTMP_HEAD_SIZE;

    memset(packet, 0, sizeof(RTMPPacket));
    body[i++] =  key ? 0x17 : 0x27;
    body[i++] =  0x01;
    body[i++] =  0x00;
    body[i++] =  0x00;
    body[i++] =  0x00;
    body[i++] = (len >> 24) & 0xff;
    body[i++] = (len >> 16) & 0xff;
    body[i++] = (len >> 8 ) & 0xff;
    body[i++] = (len >> 0 ) & 0xff;
    memcpy(body+i, data, len);

    packet->m_headerType      = RTMP_PACKET_SIZE_MEDIUM;
    packet->m_body            = body;
    packet->m_nBodySize       = len + 9;
    packet->m_packetType      = RTMP_PACKET_TYPE_VIDEO;
    packet->m_nChannel        = 0x04;
    packet->m_nTimeStamp      = pts;
    packet->m_nInfoField2     = pusher->rtmp->m_stream_id;
    pthread_mutex_lock(&pusher->lock);
    RTMP_SendPacket(pusher->rtmp, packet, TRUE);
    pthread_mutex_unlock(&pusher->lock);
}

void rtmp_push_h264(void *ctxt, uint8_t *data, int len)
{
    RTMPPUSHER *pusher = (RTMPPUSHER*)ctxt;
    uint8_t    *spsbuf, *ppsbuf;
    int         spslen,  ppslen;
    int         newlen, key = 0;
    uint32_t    pts = get_tick_count();
    if (!pusher || !pusher->rtmp || RTMP_IsConnected(pusher->rtmp) != 2) return;
    newlen = h264_parse_key_sps_pps(data, len, &key, &spsbuf, &spslen, &ppsbuf, &ppslen);
    if (key) send_sps_pps(pusher, spsbuf, spslen, ppsbuf, ppslen, pts);
    if (newlen) send_h264_data(pusher, data + (len - newlen), newlen, key, pts);
}

void rtmp_push_alaw(void *ctxt, uint8_t *data, int len)
{
    RTMPPUSHER *pusher = (RTMPPUSHER*)ctxt;
    RTMPPacket *packet;
    char       *body  ;
    uint32_t    pts = get_tick_count();

    if (!pusher || !pusher->rtmp || RTMP_IsConnected(pusher->rtmp) != 2) return;
    if (pusher->len_alaw < RTMP_HEAD_SIZE + 1 + len) {
        pusher->len_alaw = RTMP_HEAD_SIZE + 1 + len;
        if (pusher->buf_alaw) free(pusher->buf_alaw);
        pusher->buf_alaw = malloc(pusher->len_alaw);
//      printf("buf_alaw reallocated !\n");
    }
    if (!pusher->buf_alaw) {
        printf("pusher->buf_alaw is null !\n");
        return;
    }

    packet = (RTMPPacket*)pusher->buf_alaw;
    body   = pusher->buf_alaw + RTMP_HEAD_SIZE;

    memset(packet, 0, sizeof(RTMPPacket));
    body[0] = 0x76;
    memcpy(body+1, data, len);

    packet->m_headerType      = RTMP_PACKET_SIZE_MEDIUM;
    packet->m_body            = body;
    packet->m_nBodySize       = len + 1;
    packet->m_packetType      = RTMP_PACKET_TYPE_AUDIO;
    packet->m_nChannel        = 0x05;
    packet->m_nTimeStamp      = pts;
    packet->m_nInfoField2     = pusher->rtmp->m_stream_id;
    pthread_mutex_lock(&pusher->lock);
    RTMP_SendPacket(pusher->rtmp, packet, TRUE);
    pthread_mutex_unlock(&pusher->lock);
}

static void send_aac_dec_spec(RTMPPUSHER *pusher, uint8_t *spec)
{
    char        pktbuf[RTMP_HEAD_SIZE + 2 + 2];
    RTMPPacket *packet =(RTMPPacket*)pktbuf;
    char       *body   = pktbuf + RTMP_HEAD_SIZE;

    memset(packet, 0, sizeof(RTMPPacket));
    body[0] = 0xAE;
    body[1] = 0x00;
    memcpy(body + 2, spec, 2);

    packet->m_headerType  = RTMP_PACKET_SIZE_MEDIUM;
    packet->m_body        = body;
    packet->m_nBodySize   = 2 + 2;
    packet->m_packetType  = RTMP_PACKET_TYPE_AUDIO;
    packet->m_nChannel    = 0x05;
    packet->m_nInfoField2 = pusher->rtmp->m_stream_id;
    pthread_mutex_lock(&pusher->lock);
    RTMP_SendPacket(pusher->rtmp, packet, TRUE);
    pthread_mutex_unlock(&pusher->lock);
}

static void send_aac_data(void *ctxt, uint8_t *data, int len, uint32_t pts)
{
    RTMPPUSHER *pusher = (RTMPPUSHER*)ctxt;
    RTMPPacket *packet;
    char       *body  ;

    if (pusher->len_aac < RTMP_HEAD_SIZE + 2 + len) {
        pusher->len_aac = RTMP_HEAD_SIZE + 2 + len;
        if (pusher->buf_aac) free(pusher->buf_aac);
        pusher->buf_aac = malloc(pusher->len_aac);
//      printf("buf_aac reallocated !\n");
    }
    if (!pusher->buf_aac) {
        printf("pusher->buf_aac is null !\n");
        return;
    }

    packet = (RTMPPacket*)pusher->buf_aac;
    body   = pusher->buf_aac + RTMP_HEAD_SIZE;

    memset(packet, 0, sizeof(RTMPPacket));
    body[0] = 0xAE;
    body[1] = 0x01;
    memcpy(body+2, data, len);

    packet->m_headerType      = RTMP_PACKET_SIZE_MEDIUM;
    packet->m_body            = body;
    packet->m_nBodySize       = len + 2;
    packet->m_packetType      = RTMP_PACKET_TYPE_AUDIO;
    packet->m_nChannel        = 0x05;
    packet->m_nTimeStamp      = pts;
    packet->m_nInfoField2     = pusher->rtmp->m_stream_id;
    pthread_mutex_lock(&pusher->lock);
    RTMP_SendPacket(pusher->rtmp, packet, TRUE);
    pthread_mutex_unlock(&pusher->lock);
}

void rtmp_push_aac(void *ctxt, uint8_t *data, int len)
{
    RTMPPUSHER *pusher = (RTMPPUSHER*)ctxt;
    uint32_t    pts = get_tick_count();
    if (!pusher || !pusher->rtmp || RTMP_IsConnected(pusher->rtmp) != 2) return;
    if ((pusher->acc_sync_counter++ & 0xF) == 0) {
        send_aac_dec_spec(pusher, pusher->aac_dec_spec);
    }
    send_aac_data(pusher, data, len, pts);
}

void rtmp_push_url(void *ctxt, char *url)
{
    RTMPPUSHER *pusher = (RTMPPUSHER*)ctxt;
    if (!pusher) return;
    pthread_mutex_lock(&pusher->lock);
    if (pusher->rtmp) RTMP_Close(pusher->rtmp);
    strncpy(pusher->url, url, sizeof(pusher->url));
    pthread_mutex_unlock(&pusher->lock);
}

