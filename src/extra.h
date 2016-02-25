#ifndef __REDIS_EXTRA_H
#define __REDIS_EXTRA_H

typedef struct redisExtra {
    pthread_t thread;
    int enabled;                        /* True if extra thread is running */
    uint64_t next_client_id;            /* Next client unique ID. Incremental. */
    aeEventLoop *el;
    int hz;                             /* extraCron() calls frequency in hertz */
    int port;                           /* TCP listening port */
    int efd[REDIS_BINDADDR_MAX];
    int efd_count;
    char neterr[ANET_ERR_LEN];          /* Error buffer for anet.c */
    time_t unixtime;                    /* Unix time sampled every cron cycle. */
    dict *commands;                     /* Command table */
    clientBufferLimitsConfig client_obuf_limits[REDIS_CLIENT_TYPE_COUNT];
    int tcpkeepalive;                   /* Set SO_KEEPALIVE if non-zero. */
    size_t client_max_querybuf_len;     /* Limit for client query buffer length */
    unsigned int maxclients;            /* Max number of simultaneous clients */
    redisClient *current_client;        /* Current client, only used on crash report */
    list *clients;                      /* List of active clients */
    list *clients_to_close;             /* Clients to close asynchronously */
    time_t stat_starttime;              /* Extra start time */
    long long stat_rejected_conn;       /* Clients rejected because of maxclients */
    long long stat_numconnections;      /* Number of connections received */
    long long stat_net_input_bytes;     /* Bytes read from network. */
    long long stat_net_output_bytes;    /* Bytes written to network. */
    long long stat_numcommands;         /* Number of processed commands */
}redisExtra;

void initExtraConfig(void);
int initExtra(void);
void deinitExtra(void);

void *extraThreadRun(void *arg);
int extraThreadInit(void);

void extraCommand(redisClient *c);

#endif /* __REDIS_EXTRA_H */
