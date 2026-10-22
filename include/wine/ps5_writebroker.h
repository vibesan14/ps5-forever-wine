/* PS5 title write broker protocol.
 *
 * The console limits how fast an application writes to the SSD ("SSD write
 * throttling": 2400 MiB per launch, refilled at 2 MiB/s, then 25, 10, 1 and
 * 0.5 MiB/s), and a whole Battle.net session stalled about 0.95 s per write
 * once its budget was spent. A payload process is not an application and
 * is not throttled. The write broker is such a payload: a title's Wine sends
 * it regular-file writes over 127.0.0.1 and it performs them on the same
 * files, which are the same files the title has open. Only the title's user
 * data is brokered: it lives at WB_USER_ROOT, the same path for both, while
 * the app's own folder (/app0) may be anywhere the loader mounted it from.
 *
 * Connection: the title connects to WB_PORT and sends struct wb_hello with
 * WB_VERSION and the token the broker wrote to WB_TOKEN_FILE; the broker
 * answers struct wb_reply, result 0. For another version it answers
 * -EPROTONOSUPPORT and then accepts only WB_QUIT_MAGIC, so the title can
 * tell a broker left running by an older app from its own and replace it.
 * (Version 1 brokers close the connection instead.)
 * Then any number of writes: struct wb_write, path_length bytes of the
 * title's path (below WB_USER_ROOT, no NUL), length bytes of data; the
 * broker answers struct wb_reply with the bytes written or -errno. The
 * broker only writes existing regular files below WB_USER_ROOT and never
 * creates, truncates or removes one.
 * The Wine client lazily opens at most 16 pooled connections per process,
 * each locked across a synchronous request and reply; no client worker is
 * added. The broker uses one worker and a 1 MiB buffer per live connection
 * (at most 16 workers / 16 MiB for a fully used Wine process's pool).
 * Wine thread-ID routing drops the two fixed alignment bits before taking
 * the pool index, allowing consecutive Wine threads to use all 16 slots.
 * In place of a write, WB_ICON_MAGIC (path_length and length 0) asks the
 * broker to copy WB_ICON_FILE, a 512x512 PNG, over the home screen's copies
 * of the title's icon (/user/app/WB_TITLE_ID/... and /user/appmeta/...),
 * which a title cannot reach; it answers how many it replaced, or -errno.
 * WB_STATFS_MAGIC instead asks for filesystem space below WB_USER_ROOT:
 * path_length bytes of path, with offset, length and reserved all zero.
 * It answers struct wb_statfs_reply (including on refusal), with the real
 * statfs counters or -errno; negative f_bavail is clamped to zero.
 * Protocol version 6 identifies brokers that perform bounded, no-follow
 * permission repair over their fixed launcher-data roots before creating
 * the authentication token or accepting connections.
 * Malformed requests close the connection.
 */

#ifndef __WINE_PS5_WRITEBROKER_H
#define __WINE_PS5_WRITEBROKER_H

#include <stddef.h>
#include <stdint.h>

#define WB_PORT        9141
#define WB_MAGIC       0x31524257u   /* "WBR1" */
/* 2: WB_USER_ROOT, versioned hello. 3: user data only, token in WB_USER_ROOT.
 * 4: WB_ICON_MAGIC. 5: WB_STATFS_MAGIC. 6: startup permission repair. */
#define WB_VERSION     6
#define WB_TOKEN_BYTES 16
#define WB_MAX_PATH    1024
#define WB_MAX_WRITE   (16u << 20)
/* The title's user data (Wine prefix with the games) lives outside the app
 * folder, which a store update replaces. */
#define WB_USER_ROOT   "/data/anocore/"
#define WB_TOKEN_FILE  WB_USER_ROOT ".writebroker-token"
#define WB_QUIT_MAGIC  0x54495551u   /* "QUIT" in place of a wb_write: the broker exits */
#define WB_ICON_MAGIC  0x4e4f4349u   /* "ICON" in place of a wb_write */
#define WB_STATFS_MAGIC 0x53465453u  /* "STFS" in place of a wb_write */
#define WB_ICON_FILE   WB_USER_ROOT "icon0.png"
#define WB_TITLE_ID    "PPSA99130"

/* Length of WB_USER_ROOT when a brokered path is below it, or 0 when the
 * path is outside it, names only the root, or climbs out of it with "..". */
static inline size_t wb_path_root( const char *path )
{
    size_t length = __builtin_strlen( WB_USER_ROOT );
    const char *p;

    if (!path || __builtin_strncmp( path, WB_USER_ROOT, length )) return 0;
    if (!path[length] || path[length] == '/') return 0;
    for (p = path + length; *p; p++)
        if ((p == path + length || p[-1] == '/') && p[0] == '.' && p[1] == '.' && (!p[2] || p[2] == '/'))
            return 0;
    return length;
}

struct wb_hello
{
    uint32_t magic;
    uint32_t version;              /* WB_VERSION */
    unsigned char token[WB_TOKEN_BYTES];
};

struct wb_write
{
    uint32_t magic;
    uint32_t path_length;
    uint64_t offset;
    uint32_t length;
    uint32_t reserved;
};


struct wb_reply
{
    uint32_t magic;
    int32_t  result;
};

struct wb_statfs_reply
{
    uint32_t magic;
    int32_t result;
    uint64_t bsize;
    uint64_t blocks;
    uint64_t bfree;
    uint64_t bavail;
};


_Static_assert( sizeof(struct wb_hello) == 24, "wb_hello wire size" );
_Static_assert( sizeof(struct wb_write) == 24, "wb_write wire size" );
_Static_assert( sizeof(struct wb_reply) == 8, "wb_reply wire size" );
_Static_assert( sizeof(struct wb_statfs_reply) == 40, "wb_statfs_reply wire size" );

#endif /* __WINE_PS5_WRITEBROKER_H */
