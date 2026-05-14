/*
 * alsatcp — ALSA Loopback ↔ TCP audio streaming daemon
 *
 * Modes:
 *   tx    read ALSA capture → send TCP (connect to remote)
 *   rx    recv TCP → write ALSA playback (listen for connection)
 *   bidi  both simultaneously (two threads, two subdevices)
 *
 * Usage: alsatcp <tx|rx|bidi> [options]
 *   -d DEVICE      ALSA device
 *   -H HOST        Remote host (tx/bidi)
 *   -p PORT        Remote port (tx) or listen port (rx)
 *   -P PORT        Listen port for bidi RX side
 *   -r RATE        Sample rate (default: 48000)
 *   -c CHANNELS    Channels (default: 2)
 *   -f FORMAT      s16|s32|f32 (default: s16)
 *   -B BUFFER_US   ALSA buffer time µs (default: 50000)
 *   -F PERIOD_US   ALSA period time µs (default: 10000)
 *   -R RETRY_MS    TCP reconnect interval ms (default: 2000)
 *   -v             Verbose
 */

#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 600

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <netdb.h>
#include <alsa/asoundlib.h>

/* ── defaults ─────────────────────────────────────────────────────────────── */

#define DEFAULT_TX_DEVICE   "hw:Loopback,1,0"
#define DEFAULT_RX_DEVICE   "hw:Loopback,0,0"
#define DEFAULT_RATE        48000
#define DEFAULT_CHANNELS    2
#define DEFAULT_BUFFER_US   50000
#define DEFAULT_PERIOD_US   10000
#define DEFAULT_RETRY_MS    2000

/* ── types ────────────────────────────────────────────────────────────────── */

typedef enum { MODE_TX, MODE_RX, MODE_BIDI } stream_mode_t;

typedef struct {
    char               *alsa_dev;
    char               *alsa_dev_rx;   /* bidi only */
    char               *remote_host;
    int                 remote_port;
    int                 listen_port;
    unsigned int        rate;
    unsigned int        channels;
    snd_pcm_format_t    format;
    unsigned int        buffer_us;
    unsigned int        period_us;
    int                 retry_ms;
    int                 verbose;
    stream_mode_t       mode;
} config_t;

typedef struct {
    config_t               *cfg;
    int                     sock_fd;
    snd_pcm_t              *pcm;
    void                   *buf;
    snd_pcm_uframes_t       period_frames;
    size_t                  frame_bytes;
    volatile sig_atomic_t  *stop;
} stream_ctx_t;

/* ── globals ──────────────────────────────────────────────────────────────── */

static volatile sig_atomic_t g_stop = 0;

/* ── signal ───────────────────────────────────────────────────────────────── */

static void sig_handler(int sig)
{
    (void)sig;
    g_stop = 1;
}

/* ── ALSA helpers ─────────────────────────────────────────────────────────── */

static int alsa_open(const char *dev, snd_pcm_stream_t stream,
                     const config_t *cfg, snd_pcm_t **pcm,
                     snd_pcm_uframes_t *period_frames, size_t *frame_bytes)
{
    int err;

    err = snd_pcm_open(pcm, dev, stream, 0);
    if (err < 0) {
        fprintf(stderr, "alsatcp: snd_pcm_open(%s): %s\n",
                dev, snd_strerror(err));
        return -1;
    }

    err = snd_pcm_set_params(*pcm,
            cfg->format,
            SND_PCM_ACCESS_RW_INTERLEAVED,
            cfg->channels,
            cfg->rate,
            1,               /* allow soft resampling */
            cfg->buffer_us);
    if (err < 0) {
        fprintf(stderr, "alsatcp: snd_pcm_set_params(%s): %s\n",
                dev, snd_strerror(err));
        snd_pcm_close(*pcm);
        *pcm = NULL;
        return -1;
    }

    /* query actual period size */
    snd_pcm_uframes_t buf_frames;
    snd_pcm_get_params(*pcm, &buf_frames, period_frames);

    *frame_bytes = cfg->channels * snd_pcm_format_physical_width(cfg->format) / 8;

    if (cfg->verbose)
        fprintf(stderr, "alsatcp: opened %s (%s), period=%lu frames, frame=%zu bytes\n",
                dev,
                stream == SND_PCM_STREAM_CAPTURE ? "capture" : "playback",
                *period_frames, *frame_bytes);

    return 0;
}

/* ── network helpers ──────────────────────────────────────────────────────── */

static int send_all(int fd, const void *buf, size_t len)
{
    const char *p = buf;
    while (len > 0) {
        ssize_t n = send(fd, p, len, MSG_NOSIGNAL);
        if (n <= 0) return -1;
        p   += n;
        len -= (size_t)n;
    }
    return 0;
}

static int recv_all(int fd, void *buf, size_t len)
{
    char *p = buf;
    while (len > 0) {
        ssize_t n = recv(fd, p, len, MSG_WAITALL);
        if (n <= 0) return -1;
        p   += n;
        len -= (size_t)n;
    }
    return 0;
}

static int tcp_connect(const char *host, int port,
                       int retry_ms, volatile sig_atomic_t *stop)
{
    char portstr[16];
    snprintf(portstr, sizeof(portstr), "%d", port);

    struct addrinfo hints = {
        .ai_family   = AF_UNSPEC,
        .ai_socktype = SOCK_STREAM,
    };

    while (!*stop) {
        struct addrinfo *res = NULL;
        if (getaddrinfo(host, portstr, &hints, &res) != 0 || !res) {
            fprintf(stderr, "alsatcp: getaddrinfo(%s:%d) failed, retrying\n",
                    host, port);
            usleep((useconds_t)retry_ms * 1000);
            continue;
        }

        int fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
        if (fd < 0) { freeaddrinfo(res); continue; }

        if (connect(fd, res->ai_addr, res->ai_addrlen) == 0) {
            freeaddrinfo(res);
            int one = 1;
            setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof(one));
            setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
            fprintf(stderr, "alsatcp: connected to %s:%d\n", host, port);
            return fd;
        }

        fprintf(stderr, "alsatcp: connect(%s:%d): %s, retrying in %dms\n",
                host, port, strerror(errno), retry_ms);
        close(fd);
        freeaddrinfo(res);
        usleep((useconds_t)retry_ms * 1000);
    }
    return -1;
}

static int tcp_listen_accept(int port, volatile sig_atomic_t *stop)
{
    int lfd = socket(AF_INET6, SOCK_STREAM, 0);
    if (lfd < 0) {
        /* fallback to IPv4 */
        lfd = socket(AF_INET, SOCK_STREAM, 0);
        if (lfd < 0) { perror("socket"); return -1; }

        int one = 1;
        setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

        struct sockaddr_in addr = {
            .sin_family      = AF_INET,
            .sin_port        = htons((uint16_t)port),
            .sin_addr.s_addr = INADDR_ANY,
        };
        if (bind(lfd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
            perror("bind"); close(lfd); return -1;
        }
    } else {
        int one = 1;
        setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        /* IPV6_V6ONLY=0 → accept both IPv4 and IPv6 */
        int zero = 0;
        setsockopt(lfd, IPPROTO_IPV6, IPV6_V6ONLY, &zero, sizeof(zero));

        struct sockaddr_in6 addr = {
            .sin6_family = AF_INET6,
            .sin6_port   = htons((uint16_t)port),
            .sin6_addr   = in6addr_any,
        };
        if (bind(lfd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
            perror("bind"); close(lfd); return -1;
        }
    }

    if (listen(lfd, 1) < 0) { perror("listen"); close(lfd); return -1; }
    fprintf(stderr, "alsatcp: listening on port %d\n", port);

    while (!*stop) {
        int fd = accept(lfd, NULL, NULL);
        if (fd >= 0) {
            close(lfd);
            int one = 1;
            setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
            fprintf(stderr, "alsatcp: accepted connection on port %d\n", port);
            return fd;
        }
        if (errno != EINTR)
            perror("accept");
    }

    close(lfd);
    return -1;
}

/* ── TX thread: ALSA capture → TCP ───────────────────────────────────────── */

static void *tx_thread(void *arg)
{
    stream_ctx_t *ctx = arg;
    config_t     *cfg = ctx->cfg;

    while (!*ctx->stop) {
        /* (re)connect */
        ctx->sock_fd = tcp_connect(cfg->remote_host, cfg->remote_port,
                                   cfg->retry_ms, ctx->stop);
        if (ctx->sock_fd < 0) break;

        /* stream loop */
        while (!*ctx->stop) {
            snd_pcm_sframes_t n = snd_pcm_readi(ctx->pcm, ctx->buf,
                                                  ctx->period_frames);
            if (n < 0) {
                if (cfg->verbose)
                    fprintf(stderr, "alsatcp: tx xrun: %s\n", snd_strerror((int)n));
                n = snd_pcm_recover(ctx->pcm, (int)n, 0);
                if (n < 0) {
                    fprintf(stderr, "alsatcp: tx recover failed: %s\n",
                            snd_strerror((int)n));
                    break;
                }
                continue;
            }
            if (send_all(ctx->sock_fd, ctx->buf,
                         (size_t)n * ctx->frame_bytes) < 0) {
                fprintf(stderr, "alsatcp: tx send failed: %s\n", strerror(errno));
                break;
            }
        }

        close(ctx->sock_fd);
        ctx->sock_fd = -1;
        if (!*ctx->stop)
            fprintf(stderr, "alsatcp: tx disconnected, reconnecting\n");
    }

    return NULL;
}

/* ── RX thread: TCP → ALSA playback ──────────────────────────────────────── */

static void *rx_thread(void *arg)
{
    stream_ctx_t *ctx = arg;
    config_t     *cfg = ctx->cfg;

    while (!*ctx->stop) {
        /* (re)accept */
        ctx->sock_fd = tcp_listen_accept(cfg->listen_port, ctx->stop);
        if (ctx->sock_fd < 0) break;

        snd_pcm_prepare(ctx->pcm);

        /* stream loop */
        while (!*ctx->stop) {
            if (recv_all(ctx->sock_fd, ctx->buf,
                         ctx->period_frames * ctx->frame_bytes) < 0) {
                fprintf(stderr, "alsatcp: rx recv failed (peer closed?)\n");
                break;
            }
            snd_pcm_sframes_t n = snd_pcm_writei(ctx->pcm, ctx->buf,
                                                   ctx->period_frames);
            if (n < 0) {
                if (cfg->verbose)
                    fprintf(stderr, "alsatcp: rx xrun: %s\n", snd_strerror((int)n));
                n = snd_pcm_recover(ctx->pcm, (int)n, 0);
                if (n < 0) {
                    fprintf(stderr, "alsatcp: rx recover failed: %s\n",
                            snd_strerror((int)n));
                    break;
                }
            }
        }

        close(ctx->sock_fd);
        ctx->sock_fd = -1;
        if (!*ctx->stop)
            fprintf(stderr, "alsatcp: rx disconnected, re-listening\n");
    }

    return NULL;
}

/* ── usage ────────────────────────────────────────────────────────────────── */

static void usage(const char *prog)
{
    fprintf(stderr,
        "Usage: %s <tx|rx|bidi> [options]\n"
        "  -d DEVICE     ALSA device (tx default: " DEFAULT_TX_DEVICE
                                    ", rx default: " DEFAULT_RX_DEVICE ")\n"
        "  -D DEVICE     ALSA RX device (bidi only)\n"
        "  -H HOST       Remote host (tx/bidi TX)\n"
        "  -p PORT       Remote port (tx) or listen port (rx)\n"
        "  -P PORT       Listen port (bidi RX side)\n"
        "  -r RATE       Sample rate (default: %d)\n"
        "  -c CHANNELS   Channels (default: %d)\n"
        "  -f FORMAT     s16|s32|f32 (default: s16)\n"
        "  -B BUFFER_US  ALSA buffer µs (default: %d)\n"
        "  -F PERIOD_US  ALSA period µs (default: %d)\n"
        "  -R RETRY_MS   TCP retry ms (default: %d)\n"
        "  -v            Verbose\n",
        prog,
        DEFAULT_RATE, DEFAULT_CHANNELS,
        DEFAULT_BUFFER_US, DEFAULT_PERIOD_US, DEFAULT_RETRY_MS);
}

/* ── main ─────────────────────────────────────────────────────────────────── */

int main(int argc, char *argv[])
{
    if (argc < 2) { usage(argv[0]); return 1; }

    config_t cfg = {
        .alsa_dev    = NULL,
        .alsa_dev_rx = NULL,
        .remote_host = NULL,
        .remote_port = 0,
        .listen_port = 0,
        .rate        = DEFAULT_RATE,
        .channels    = DEFAULT_CHANNELS,
        .format      = SND_PCM_FORMAT_S16_LE,
        .buffer_us   = DEFAULT_BUFFER_US,
        .period_us   = DEFAULT_PERIOD_US,
        .retry_ms    = DEFAULT_RETRY_MS,
        .verbose     = 0,
        .mode        = MODE_TX,
    };

    /* mode */
    if      (strcmp(argv[1], "tx")   == 0) cfg.mode = MODE_TX;
    else if (strcmp(argv[1], "rx")   == 0) cfg.mode = MODE_RX;
    else if (strcmp(argv[1], "bidi") == 0) cfg.mode = MODE_BIDI;
    else { fprintf(stderr, "Unknown mode: %s\n", argv[1]); usage(argv[0]); return 1; }

    /* options */
    int opt;
    optind = 2;
    while ((opt = getopt(argc, argv, "d:D:H:p:P:r:c:f:B:F:R:v")) != -1) {
        switch (opt) {
        case 'd': cfg.alsa_dev    = optarg; break;
        case 'D': cfg.alsa_dev_rx = optarg; break;
        case 'H': cfg.remote_host = optarg; break;
        case 'p': cfg.remote_port = atoi(optarg); cfg.listen_port = atoi(optarg); break;
        case 'P': cfg.listen_port = atoi(optarg); break;
        case 'r': cfg.rate        = (unsigned)atoi(optarg); break;
        case 'c': cfg.channels    = (unsigned)atoi(optarg); break;
        case 'f':
            if      (strcmp(optarg, "s16") == 0) cfg.format = SND_PCM_FORMAT_S16_LE;
            else if (strcmp(optarg, "s32") == 0) cfg.format = SND_PCM_FORMAT_S32_LE;
            else if (strcmp(optarg, "f32") == 0) cfg.format = SND_PCM_FORMAT_FLOAT_LE;
            else { fprintf(stderr, "Unknown format: %s\n", optarg); return 1; }
            break;
        case 'B': cfg.buffer_us   = (unsigned)atoi(optarg); break;
        case 'F': cfg.period_us   = (unsigned)atoi(optarg); break;
        case 'R': cfg.retry_ms    = atoi(optarg); break;
        case 'v': cfg.verbose     = 1; break;
        default:  usage(argv[0]); return 1;
        }
    }

    /* defaults per mode */
    if (!cfg.alsa_dev)
        cfg.alsa_dev = (cfg.mode == MODE_RX) ? DEFAULT_RX_DEVICE : DEFAULT_TX_DEVICE;

    /* validate */
    if ((cfg.mode == MODE_TX || cfg.mode == MODE_BIDI) && !cfg.remote_host) {
        fprintf(stderr, "alsatcp: -H HOST required for tx/bidi\n"); return 1;
    }
    if (cfg.remote_port == 0 && cfg.listen_port == 0) {
        fprintf(stderr, "alsatcp: -p PORT required\n"); return 1;
    }

    /* signals */
    struct sigaction sa = { .sa_handler = sig_handler, .sa_flags = 0 };
    sigemptyset(&sa.sa_mask);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT,  &sa, NULL);
    signal(SIGPIPE, SIG_IGN);

    /* open ALSA */
    stream_ctx_t tx_ctx = { .cfg = &cfg, .sock_fd = -1, .stop = &g_stop };
    stream_ctx_t rx_ctx = { .cfg = &cfg, .sock_fd = -1, .stop = &g_stop };

    if (cfg.mode == MODE_TX || cfg.mode == MODE_BIDI) {
        if (alsa_open(cfg.alsa_dev, SND_PCM_STREAM_CAPTURE, &cfg,
                      &tx_ctx.pcm, &tx_ctx.period_frames, &tx_ctx.frame_bytes) < 0)
            return 1;
        tx_ctx.buf = malloc(tx_ctx.period_frames * tx_ctx.frame_bytes);
        if (!tx_ctx.buf) { perror("malloc"); return 1; }
    }

    if (cfg.mode == MODE_RX || cfg.mode == MODE_BIDI) {
        const char *rx_dev = (cfg.mode == MODE_BIDI && cfg.alsa_dev_rx)
                             ? cfg.alsa_dev_rx : cfg.alsa_dev;
        if (alsa_open(rx_dev, SND_PCM_STREAM_PLAYBACK, &cfg,
                      &rx_ctx.pcm, &rx_ctx.period_frames, &rx_ctx.frame_bytes) < 0)
            return 1;
        rx_ctx.buf = malloc(rx_ctx.period_frames * rx_ctx.frame_bytes);
        if (!rx_ctx.buf) { perror("malloc"); return 1; }
    }

    /* dispatch */
    if (cfg.mode == MODE_TX) {
        tx_thread(&tx_ctx);
    } else if (cfg.mode == MODE_RX) {
        rx_thread(&rx_ctx);
    } else {
        /* bidi: TX connects, RX listens on separate port */
        if (cfg.listen_port == cfg.remote_port)
            fprintf(stderr, "alsatcp: warning: TX and RX on same port — use -P for RX listen port\n");
        pthread_t tx_tid, rx_tid;
        pthread_create(&tx_tid, NULL, tx_thread, &tx_ctx);
        pthread_create(&rx_tid, NULL, rx_thread, &rx_ctx);
        pthread_join(tx_tid, NULL);
        pthread_join(rx_tid, NULL);
    }

    /* cleanup */
    if (tx_ctx.pcm) { snd_pcm_drain(tx_ctx.pcm); snd_pcm_close(tx_ctx.pcm); }
    if (rx_ctx.pcm) { snd_pcm_drain(rx_ctx.pcm); snd_pcm_close(rx_ctx.pcm); }
    if (tx_ctx.sock_fd >= 0) close(tx_ctx.sock_fd);
    if (rx_ctx.sock_fd >= 0) close(rx_ctx.sock_fd);
    free(tx_ctx.buf);
    free(rx_ctx.buf);

    return 0;
}
