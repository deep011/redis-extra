
#include "redis.h"
#include "extra.h"
#include "endianconv.h"

redisExtra rextra;

static void pingExtraCommand(redisClient *c);
static void clientExtraCommand(redisClient *c);
static void infoExtraCommand(redisClient *c);
static void readQueryFromExtraClient(aeEventLoop *el, int fd, void *privdata, int mask);
static void addExtraReplyString(redisClient *c, char *s, size_t len);
static void addExtraReplySds(redisClient *c, sds s);
static void addExtraReplyErrorLength(redisClient *c, char *s, size_t len);
static void addExtraReplyError(redisClient *c, char *err);
static void addExtraReply(redisClient *c, robj *obj);
static void addExtraReplyBulkLen(redisClient *c, robj *obj);
static void addExtraReplyBulk(redisClient *c, robj *obj);
static void addExtraReplyBulkCBuffer(redisClient *c, void *p, size_t len);
static void addExtraReplyLongLongWithPrefix(redisClient *c, long long ll, char prefix);
static void addExtraReplyLongLong(redisClient *c, long long ll);
#ifdef __GNUC__
static void addExtraReplyErrorFormat(redisClient *c, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));
#else
static void addExtraReplyErrorFormat(redisClient *c, const char *fmt, ...);
#endif
static void freeExtraClient(redisClient *c);

static unsigned int dictSdsCaseHash(const void *key) {
    return dictGenCaseHashFunction((unsigned char*)key, sdslen((char*)key));
}

/* A case insensitive version used for the command lookup table and other
 * places where case insensitive non binary-safe comparison is needed. */
static int dictSdsKeyCaseCompare(void *privdata, const void *key1,
        const void *key2)
{
    DICT_NOTUSED(privdata);

    return strcasecmp(key1, key2) == 0;
}

static void dictSdsDestructor(void *privdata, void *val)
{
    DICT_NOTUSED(privdata);

    sdsfree(val);
}

/* Command table. sds string -> command struct pointer. */
static dictType commandTableDictType = {
    dictSdsCaseHash,           /* hash function */
    NULL,                      /* key dup */
    NULL,                      /* val dup */
    dictSdsKeyCaseCompare,     /* key compare */
    dictSdsDestructor,         /* key destructor */
    NULL                       /* val destructor */
};

/* Extra command table.
 *
 * Every entry is composed of the following fields:
 *
 * name: a string representing the command name.
 * function: pointer to the C function implementing the command.
 * arity: number of arguments, it is possible to use -N to say >= N
 * sflags: command flags as string. See below for a table of flags.
 * flags: flags as bitmask. Computed by Redis using the 'sflags' field.
 * get_keys_proc: an optional function to get key arguments from a command.
 *                This is only used when the following three fields are not
 *                enough to specify what arguments are keys.
 * first_key_index: first argument that is a key
 * last_key_index: last argument that is a key
 * key_step: step to get all the keys from first to last argument. For instance
 *           in MSET the step is two since arguments are key,val,key,val,...
 * microseconds: microseconds of total execution time for this command.
 * calls: total number of calls of this command.
 *
 * The flags, microseconds and calls fields are computed by Redis and should
 * always be set to zero.
 *
 * Command flags are expressed using strings where every character represents
 * a flag. Later the populateCommandTable() function will take care of
 * populating the real 'flags' field using this characters.
 *
 * This is the meaning of the flags:
 *
 * w: write command (may modify the key space).
 * r: read command  (will never modify the key space).
 * m: may increase memory usage once called. Don't allow if out of memory.
 * a: admin command, like SAVE or SHUTDOWN.
 * p: Pub/Sub related command.
 * f: force replication of this command, regardless of server.dirty.
 * s: command not allowed in scripts.
 * R: random command. Command is not deterministic, that is, the same command
 *    with the same arguments, with the same key space, may have different
 *    results. For instance SPOP and RANDOMKEY are two random commands.
 * S: Sort command output array if called from script, so that the output
 *    is deterministic.
 * l: Allow command while loading the database.
 * t: Allow command while a slave has stale data but is not allowed to
 *    server this data. Normally no command is accepted in this condition
 *    but just a few.
 * M: Do not automatically propagate the command on MONITOR.
 * k: Perform an implicit ASKING for this command, so the command will be
 *    accepted in cluster mode if the slot is marked as 'importing'.
 * F: Fast command: O(1) or O(log(N)) command that should never delay
 *    its execution as long as the kernel scheduler is giving us time.
 *    Note that commands that may trigger a DEL as a side effect (like SET)
 *    are not fast commands.
 */
struct redisCommand redisExtraCommandTable[] = {
    {"ping",pingExtraCommand,-1,"rtF",0,NULL,0,0,0,0,0},
    {"client",clientExtraCommand,-2,"rs",0,NULL,0,0,0,0,0},
    {"info",infoExtraCommand,-1,"rlt",0,NULL,0,0,0,0,0}
};

static int getLongLongFromObjectOrExtraReply(redisClient *c, robj *o, long long *target, const char *msg) {
    long long value;
    if (getLongLongFromObject(o, &value) != REDIS_OK) {
        if (msg != NULL) {
            addExtraReplyError(c,(char*)msg);
        } else {
            addExtraReplyError(c,"value is not an integer or out of range");
        }
        return REDIS_ERR;
    }
    *target = value;
    return REDIS_OK;
}

/* A Redis "Peer ID" is a colon separated ip:port pair.
 * For IPv4 it's in the form x.y.z.k:port, example: "127.0.0.1:1234".
 * For IPv6 addresses we use [] around the IP part, like in "[::1]:1234".
 * For Unix sockets we use path:0, like in "/tmp/redis:0".
 *
 * A Peer ID always fits inside a buffer of REDIS_PEER_ID_LEN bytes, including
 * the null term.
 *
 * The function returns REDIS_OK on succcess, and REDIS_ERR on failure.
 *
 * On failure the function still populates 'peerid' with the "?:0" string
 * in case you want to relax error checking or need to display something
 * anyway (see anetPeerToString implementation for more info). */
int genExtraClientPeerId(redisClient *client, char *peerid, size_t peerid_len) {
    char ip[REDIS_IP_STR_LEN];
    int port;

    if (client->flags & REDIS_UNIX_SOCKET) {
        /* Unix socket client. */
        snprintf(peerid,peerid_len,"%s","NOTSUPPORT");
        return REDIS_OK;
    } else {
        /* TCP client. */
        int retval = anetPeerToString(client->fd,ip,sizeof(ip),&port);
        formatPeerId(peerid,peerid_len,ip,port);
        return (retval == -1) ? REDIS_ERR : REDIS_OK;
    }
}

/* This function returns the client peer id, by creating and caching it
 * if client->peerid is NULL, otherwise returning the cached value.
 * The Peer ID never changes during the life of the client, however it
 * is expensive to compute. */
static char *getExtraClientPeerId(redisClient *c) {
    char peerid[REDIS_PEER_ID_LEN];

    if (c->peerid == NULL) {
        genExtraClientPeerId(c,peerid,sizeof(peerid));
        c->peerid = sdsnew(peerid);
    }
    return c->peerid;
}

/* Concatenate a string representing the state of a client in an human
 * readable format, into the sds string 's'. */
static sds catExtraClientInfoString(sds s, redisClient *client) {
    char flags[16], events[3], *p;
    int emask;

    p = flags;
    if (client->flags & REDIS_SLAVE) {
        if (client->flags & REDIS_MONITOR)
            *p++ = 'O';
        else
            *p++ = 'S';
    }
    if (client->flags & REDIS_MASTER) *p++ = 'M';
    if (client->flags & REDIS_MULTI) *p++ = 'x';
    if (client->flags & REDIS_BLOCKED) *p++ = 'b';
    if (client->flags & REDIS_DIRTY_CAS) *p++ = 'd';
    if (client->flags & REDIS_CLOSE_AFTER_REPLY) *p++ = 'c';
    if (client->flags & REDIS_UNBLOCKED) *p++ = 'u';
    if (client->flags & REDIS_CLOSE_ASAP) *p++ = 'A';
    if (client->flags & REDIS_UNIX_SOCKET) *p++ = 'U';
    if (client->flags & REDIS_READONLY) *p++ = 'r';
    if (p == flags) *p++ = 'N';
    *p++ = '\0';

    emask = client->fd == -1 ? 0 : aeGetFileEvents(rextra.el,client->fd);
    p = events;
    if (emask & AE_READABLE) *p++ = 'r';
    if (emask & AE_WRITABLE) *p++ = 'w';
    *p = '\0';
    return sdscatfmt(s,
        "id=%U addr=%s fd=%i name=%s age=%I idle=%I flags=%s db=%i sub=%i psub=%i multi=%i qbuf=%U qbuf-free=%U obl=%U oll=%U omem=%U events=%s cmd=%s",
        (unsigned long long) client->id,
        getExtraClientPeerId(client),
        client->fd,
        client->name ? (char*)client->name->ptr : "",
        (long long)(rextra.unixtime - client->ctime),
        (long long)(rextra.unixtime - client->lastinteraction),
        flags,
        client->db?client->db->id:-1,
        (int) dictSize(client->pubsub_channels),
        (int) listLength(client->pubsub_patterns),
        (client->flags & REDIS_MULTI) ? client->mstate.count : -1,
        (unsigned long long) sdslen(client->querybuf),
        (unsigned long long) sdsavail(client->querybuf),
        (unsigned long long) client->bufpos,
        (unsigned long long) listLength(client->reply),
        (unsigned long long) getClientOutputBufferMemoryUsage(client),
        events,
        client->lastcmd ? client->lastcmd->name : "NULL");
}

static sds getAllExtraClientsInfoString(void) {
    listNode *ln;
    listIter li;
    redisClient *client;
    sds o = sdsempty();

    o = sdsMakeRoomFor(o,200*listLength(rextra.clients));
    listRewind(rextra.clients,&li);
    while ((ln = listNext(&li)) != NULL) {
        client = listNodeValue(ln);
        o = catExtraClientInfoString(o,client);
        o = sdscatlen(o,"\n",1);
    }

    return o;
}

/* The PING command. */
static void pingExtraCommand(redisClient *c) {
    /* The command takes zero or one arguments. */
    if (c->argc > 2) {
        addExtraReplyErrorFormat(c,"wrong number of arguments for '%s' command",
            c->cmd->name);
        return;
    }

    if (c->flags & REDIS_PUBSUB) {
        addExtraReply(c,shared.mbulkhdr[2]);
        addExtraReplyBulkCBuffer(c,"pong",4);
        if (c->argc == 1)
            addExtraReplyBulkCBuffer(c,"",0);
        else
            addExtraReplyBulk(c,c->argv[1]);
    } else {
        if (c->argc == 1)
            addExtraReply(c,shared.pong);
        else
            addExtraReplyBulk(c,c->argv[1]);
    }
}

static void clientExtraCommand(redisClient *c) {
    listNode *ln;
    listIter li;
    redisClient *client;

    if (!strcasecmp(c->argv[1]->ptr,"list") && c->argc == 2) {
        /* CLIENT LIST */
        sds o = getAllExtraClientsInfoString();
        addExtraReplyBulkCBuffer(c,o,sdslen(o));
        sdsfree(o);
    } else if (!strcasecmp(c->argv[1]->ptr,"kill")) {
        /* CLIENT KILL <ip:port>
         * CLIENT KILL <option> [value] ... <option> [value] */
        char *addr = NULL;
        int type = -1;
        uint64_t id = 0;
        int skipme = 1;
        int killed = 0, close_this_client = 0;

        if (c->argc == 3) {
            /* Old style syntax: CLIENT KILL <addr> */
            addr = c->argv[2]->ptr;
            skipme = 0; /* With the old form, you can kill yourself. */
        } else if (c->argc > 3) {
            int i = 2; /* Next option index. */

            /* New style syntax: parse options. */
            while(i < c->argc) {
                int moreargs = c->argc > i+1;

                if (!strcasecmp(c->argv[i]->ptr,"id") && moreargs) {
                    long long tmp;

                    if (getLongLongFromObjectOrExtraReply(c,c->argv[i+1],&tmp,NULL)
                        != REDIS_OK) return;
                    id = tmp;
                } else if (!strcasecmp(c->argv[i]->ptr,"type") && moreargs) {
                    type = getClientTypeByName(c->argv[i+1]->ptr);
                    if (type == -1) {
                        addExtraReplyErrorFormat(c,"Unknown client type '%s'",
                            (char*) c->argv[i+1]->ptr);
                        return;
                    }
                } else if (!strcasecmp(c->argv[i]->ptr,"addr") && moreargs) {
                    addr = c->argv[i+1]->ptr;
                } else if (!strcasecmp(c->argv[i]->ptr,"skipme") && moreargs) {
                    if (!strcasecmp(c->argv[i+1]->ptr,"yes")) {
                        skipme = 1;
                    } else if (!strcasecmp(c->argv[i+1]->ptr,"no")) {
                        skipme = 0;
                    } else {
                        addExtraReply(c,shared.syntaxerr);
                        return;
                    }
                } else {
                    addExtraReply(c,shared.syntaxerr);
                    return;
                }
                i += 2;
            }
        } else {
            addExtraReply(c,shared.syntaxerr);
            return;
        }

        if (c->argc == 4 && !strcasecmp(c->argv[2]->ptr,"id") && id == 0) {
            addExtraReplyLongLong(c,0);
            return;
        }

        /* Iterate clients killing all the matching clients. */
        listRewind(rextra.clients,&li);
        while ((ln = listNext(&li)) != NULL) {
            client = listNodeValue(ln);
            if (addr && strcmp(getExtraClientPeerId(client),addr) != 0) continue;
            if (type != -1 &&
                (client->flags & REDIS_MASTER ||
                 getClientType(client) != type)) continue;
            if (id != 0 && client->id != id) continue;
            if (c == client && skipme) continue;

            /* Kill it. */
            if (c == client) {
                close_this_client = 1;
            } else {
                freeExtraClient(client);
            }
            killed++;
        }

        /* Reply according to old/new format. */
        if (c->argc == 3) {
            if (killed == 0)
                addExtraReplyError(c,"No such client");
            else
                addExtraReply(c,shared.ok);
        } else {
            addExtraReplyLongLong(c,killed);
        }

        /* If this client has to be closed, flag it as CLOSE_AFTER_REPLY
         * only after we queued the reply to its output buffers. */
        if (close_this_client) c->flags |= REDIS_CLOSE_AFTER_REPLY;
    } else if (!strcasecmp(c->argv[1]->ptr,"setname") && c->argc == 3) {
        int j, len = sdslen(c->argv[2]->ptr);
        char *p = c->argv[2]->ptr;

        /* Setting the client name to an empty string actually removes
         * the current name. */
        if (len == 0) {
            if (c->name) decrRefCount(c->name);
            c->name = NULL;
            addExtraReply(c,shared.ok);
            return;
        }

        /* Otherwise check if the charset is ok. We need to do this otherwise
         * CLIENT LIST format will break. You should always be able to
         * split by space to get the different fields. */
        for (j = 0; j < len; j++) {
            if (p[j] < '!' || p[j] > '~') { /* ASCII is assumed. */
                addExtraReplyError(c,
                    "Client names cannot contain spaces, "
                    "newlines or special characters.");
                return;
            }
        }
        if (c->name) decrRefCount(c->name);
        c->name = c->argv[2];
        incrRefCount(c->name);
        addExtraReply(c,shared.ok);
    } else if (!strcasecmp(c->argv[1]->ptr,"getname") && c->argc == 2) {
        if (c->name)
            addExtraReplyBulk(c,c->name);
        else
            addExtraReply(c,shared.nullbulk);
    } else {
        addExtraReplyError(c, "Syntax error, try CLIENT (LIST | KILL ip:port | GETNAME | SETNAME connection-name)");
    }
}

/* Create the string returned by the INFO command. This is decoupled
 * by the INFO command itself as we need to report the same information
 * on memory corruption problems. */
static sds genExtraInfoString(char *section) {
    sds info = sdsempty();
    time_t uptime = rextra.unixtime-rextra.stat_starttime;
    int j, numcommands;
    int allsections = 0, defsections = 0;
    int sections = 0;

    if (section == NULL) section = "default";
    allsections = strcasecmp(section,"all") == 0;
    defsections = strcasecmp(section,"default") == 0;

    /* Extra */
    if (allsections || defsections || !strcasecmp(section,"extra")) {
        if (sections++) info = sdscat(info,"\r\n");

        info = sdscatprintf(info,
            "# Extra\r\n"
            "thread_id:%lu\r\n"
            "tcp_port:%d\r\n"
            "uptime_in_seconds:%jd\r\n"
            "uptime_in_days:%jd\r\n"
            "tcpkeepalive:%d\r\n"
            "hz:%d\r\n",
            rextra.thread,
            rextra.port,
            (intmax_t)uptime,
            (intmax_t)(uptime/(3600*24)),
            rextra.tcpkeepalive,
            rextra.hz);
    }

    /* Clients */
    if (allsections || defsections || !strcasecmp(section,"clients")) {
        if (sections++) info = sdscat(info,"\r\n");
        info = sdscatprintf(info,
            "# Clients\r\n"
            "max_clients_can_be_accepted:%u\r\n"
            "client_max_querybuf_len:%zu\r\n"
            "connected_clients:%lu\r\n",
            rextra.maxclients,
            rextra.client_max_querybuf_len,
            listLength(rextra.clients));
    }

    /* Stats */
    if (allsections || defsections || !strcasecmp(section,"stats")) {
        if (sections++) info = sdscat(info,"\r\n");
        info = sdscatprintf(info,
            "# Stats\r\n"
            "total_connections_received:%lld\r\n"
            "total_commands_processed:%lld\r\n"
            "total_net_input_bytes:%lld\r\n"
            "total_net_output_bytes:%lld\r\n"
            "rejected_connections:%lld\r\n",
            rextra.stat_numconnections,
            rextra.stat_numcommands,
            rextra.stat_net_input_bytes,
            rextra.stat_net_output_bytes,
            rextra.stat_rejected_conn);
    }

    /* cmdtime */
    if (allsections || !strcasecmp(section,"commandstats")) {
        if (sections++) info = sdscat(info,"\r\n");
        info = sdscatprintf(info, "# Commandstats\r\n");
        numcommands = sizeof(redisExtraCommandTable)/sizeof(struct redisCommand);
        for (j = 0; j < numcommands; j++) {
            struct redisCommand *c = redisExtraCommandTable+j;

            if (!c->calls) continue;
            info = sdscatprintf(info,
                "cmdstat_%s:calls=%lld,usec=%lld,usec_per_call=%.2f\r\n",
                c->name, c->calls, c->microseconds,
                (c->calls == 0) ? 0 : ((float)c->microseconds/c->calls));
        }
    }

    return info;
}

static void infoExtraCommand(redisClient *c) {
    char *section = c->argc == 2 ? c->argv[1]->ptr : "default";

    if (c->argc > 2) {
        addExtraReply(c,shared.syntaxerr);
        return;
    }
    sds info = genExtraInfoString(section);
    addExtraReplySds(c,sdscatprintf(sdsempty(),"$%lu\r\n",
        (unsigned long)sdslen(info)));
    addExtraReplySds(c,info);
    addExtraReply(c,shared.crlf);
}

static int listMatchObjects(void *a, void *b) {
    return equalStringObjects(a,b);
}

/* To evaluate the output buffer size of a client we need to get size of
 * allocated objects, however we can't used zmalloc_size() directly on sds
 * strings because of the trick they use to work (the header is before the
 * returned pointer), so we use this helper function. */
static size_t zmalloc_size_sds(sds s) {
    return zmalloc_size(s-sizeof(struct sdshdr));
}

/* Return the amount of memory used by the sds string at object->ptr
 * for a string object. */
static size_t getStringObjectSdsUsedMemory(robj *o) {
    redisAssertWithInfo(NULL,o,o->type == REDIS_STRING);
    switch(o->encoding) {
    case REDIS_ENCODING_RAW: return zmalloc_size_sds(o->ptr);
    case REDIS_ENCODING_EMBSTR: return sdslen(o->ptr);
    default: return 0; /* Just integer encoding for now. */
    }
}

/* Create a duplicate of the last object in the reply list when
 * it is not exclusively owned by the reply list. */
static robj *dupLastObjectIfNeeded(list *reply) {
    robj *new, *cur;
    listNode *ln;
    redisAssert(listLength(reply) > 0);
    ln = listLast(reply);
    cur = listNodeValue(ln);
    if (cur->refcount > 1) {
        new = dupStringObject(cur);
        decrRefCount(cur);
        listNodeValue(ln) = new;
    }
    return listNodeValue(ln);
}


static void freeExtraClientArgv(redisClient *c) {
    int j;
    for (j = 0; j < c->argc; j++)
        decrRefCount(c->argv[j]);
    c->argc = 0;
    c->cmd = NULL;
}

static redisClient *createExtraClient(int fd) {    
    redisClient *c = zmalloc(sizeof(redisClient));

    /* passing -1 as fd it is possible to create a non connected client.
     * This is useful since all the Redis commands needs to be executed
     * in the context of a client. When commands are executed in other
     * contexts (for instance a Lua script) we need a non connected client. */
    if (fd != -1) {
        anetNonBlock(NULL,fd);
        anetEnableTcpNoDelay(NULL,fd);
        if (rextra.tcpkeepalive)
            anetKeepAlive(NULL,fd,rextra.tcpkeepalive);
        if (aeCreateFileEvent(rextra.el,fd,AE_READABLE,
            readQueryFromExtraClient, c) == AE_ERR)
        {
            close(fd);
            zfree(c);
            return NULL;
        }
    }

    c->id = rextra.next_client_id++;
    c->fd = fd;
    c->name = NULL;
    c->bufpos = 0;
    c->querybuf = sdsempty();
    c->querybuf_peak = 0;
    c->reqtype = 0;
    c->argc = 0;
    c->argv = NULL;
    c->cmd = c->lastcmd = NULL;
    c->multibulklen = 0;
    c->bulklen = -1;
    c->sentlen = 0;
    c->flags = 0;
    c->ctime = c->lastinteraction = rextra.unixtime;
    c->authenticated = 0;
    c->replstate = REDIS_REPL_NONE;
    c->repl_put_online_on_ack = 0;
    c->reploff = 0;
    c->repl_ack_off = 0;
    c->repl_ack_time = 0;
    c->slave_listening_port = 0;
    c->slave_capa = SLAVE_CAPA_NONE;
    c->reply = listCreate();
    c->reply_bytes = 0;
    c->obuf_soft_limit_reached_time = 0;
    listSetFreeMethod(c->reply,decrRefCountVoid);
    listSetDupMethod(c->reply,dupClientReplyValue);
    c->btype = REDIS_BLOCKED_NONE;
    c->bpop.timeout = 0;
    c->bpop.keys = dictCreate(&setDictType,NULL);
    c->bpop.target = NULL;
    c->bpop.numreplicas = 0;
    c->bpop.reploffset = 0;
    c->woff = 0;
    c->watched_keys = listCreate();
    c->pubsub_channels = dictCreate(&setDictType,NULL);
    c->pubsub_patterns = listCreate();
    c->peerid = NULL;
    listSetFreeMethod(c->pubsub_patterns,decrRefCountVoid);
    listSetMatchMethod(c->pubsub_patterns,listMatchObjects);
    if (fd != -1) listAddNodeTail(rextra.clients,c);
    initClientMultiState(c);
    return c;
}

static void freeExtraClient(redisClient *c) {
    listNode *ln;

    /* If this is marked as current client unset it */
    if (rextra.current_client == c) rextra.current_client = NULL;

    /* Free the query buffer */
    sdsfree(c->querybuf);
    c->querybuf = NULL;

    /* UNWATCH all the keys */
    unwatchAllKeys(c);
    listRelease(c->watched_keys);

    /* Unsubscribe from all the pubsub channels */
    pubsubUnsubscribeAllChannels(c,0);
    pubsubUnsubscribeAllPatterns(c,0);
    dictRelease(c->pubsub_channels);
    listRelease(c->pubsub_patterns);

    /* Close socket, unregister events, and remove list of replies and
     * accumulated arguments. */
    if (c->fd != -1) {
        aeDeleteFileEvent(rextra.el,c->fd,AE_READABLE);
        aeDeleteFileEvent(rextra.el,c->fd,AE_WRITABLE);
        close(c->fd);
    }
    listRelease(c->reply);
    freeExtraClientArgv(c);

    /* Remove from the list of clients */
    if (c->fd != -1) {
        ln = listSearchKey(rextra.clients,c);
        redisAssert(ln != NULL);
        listDelNode(rextra.clients,ln);
    }

    /* If this client was scheduled for async freeing we need to remove it
     * from the queue. */
    if (c->flags & REDIS_CLOSE_ASAP) {
        ln = listSearchKey(rextra.clients_to_close,c);
        redisAssert(ln != NULL);
        listDelNode(rextra.clients_to_close,ln);
    }

    /* Release other dynamically allocated client structure fields,
     * and finally release the client structure itself. */
    if (c->name) decrRefCount(c->name);
    zfree(c->argv);

    freeClientMultiState(c);
    sdsfree(c->peerid);

    zfree(c);

}

/* Schedule a client to free it at a safe time in the extraCron() function.
 * This function is useful when we need to terminate a client but we are in
 * a context where calling freeExtraClient() is not possible, because the client
 * should be valid for the continuation of the flow of the program. */
void freeExtraClientAsync(redisClient *c) {
    if (c->flags & REDIS_CLOSE_ASAP || c->flags & REDIS_LUA_CLIENT) return;
    c->flags |= REDIS_CLOSE_ASAP;
    listAddNodeTail(rextra.clients_to_close,c);
}

/* resetClient prepare the client to process the next command */
static void resetExtraClient(redisClient *c) {

    freeExtraClientArgv(c);
    c->reqtype = 0;
    c->multibulklen = 0;
    c->bulklen = -1;
    /* We clear the ASKING flag as well if we are not inside a MULTI, and
     * if what we just executed is not the ASKING command itself. */
    if (!(c->flags & REDIS_MULTI))
        c->flags &= (~REDIS_ASKING);
}

void sendReplyToExtraClient(aeEventLoop *el, int fd, void *privdata, int mask) {
    redisClient *c = privdata;
    int nwritten = 0, totwritten = 0, objlen;
    size_t objmem;
    robj *o;
    REDIS_NOTUSED(el);
    REDIS_NOTUSED(mask);

    while(c->bufpos > 0 || listLength(c->reply)) {
        if (c->bufpos > 0) {
            nwritten = write(fd,c->buf+c->sentlen,c->bufpos-c->sentlen);
            if (nwritten <= 0) break;
            c->sentlen += nwritten;
            totwritten += nwritten;

            /* If the buffer was sent, set bufpos to zero to continue with
             * the remainder of the reply. */
            if (c->sentlen == c->bufpos) {
                c->bufpos = 0;
                c->sentlen = 0;
            }
        } else {
            o = listNodeValue(listFirst(c->reply));
            objlen = sdslen(o->ptr);
            objmem = getStringObjectSdsUsedMemory(o);

            if (objlen == 0) {
                listDelNode(c->reply,listFirst(c->reply));
                c->reply_bytes -= objmem;
                continue;
            }

            nwritten = write(fd, ((char*)o->ptr)+c->sentlen,objlen-c->sentlen);
            if (nwritten <= 0) break;
            c->sentlen += nwritten;
            totwritten += nwritten;

            /* If we fully sent the object on head go to the next one */
            if (c->sentlen == objlen) {
                listDelNode(c->reply,listFirst(c->reply));
                c->sentlen = 0;
                c->reply_bytes -= objmem;
            }
        }
        /* Note that we avoid to send more than REDIS_MAX_WRITE_PER_EVENT
         * bytes, in a single threaded server it's a good idea to serve
         * other clients as well, even if a very large request comes from
         * super fast link that is always able to accept data (in real world
         * scenario think about 'KEYS *' against the loopback interface).
         *
         * However if we are over the maxmemory limit we ignore that and
         * just deliver as much data as it is possible to deliver. */
        rextra.stat_net_output_bytes += totwritten;
        if (totwritten > REDIS_MAX_WRITE_PER_EVENT) break;
    }
    if (nwritten == -1) {
        if (errno == EAGAIN) {
            nwritten = 0;
        } else {
            //redisLog(REDIS_VERBOSE,
            //    "Error writing to client: %s", strerror(errno));
            freeExtraClient(c);
            return;
        }
    }
    if (totwritten > 0) {
        /* For clients representing masters we don't count sending data
         * as an interaction, since we always send REPLCONF ACK commands
         * that take some time to just fill the socket output buffer.
         * We just rely on data / pings received for timeout detection. */
        if (!(c->flags & REDIS_MASTER)) c->lastinteraction = rextra.unixtime;
    }
    if (c->bufpos == 0 && listLength(c->reply) == 0) {
        c->sentlen = 0;
        aeDeleteFileEvent(rextra.el,c->fd,AE_WRITABLE);

        /* Close connection after entire reply has been sent. */
        if (c->flags & REDIS_CLOSE_AFTER_REPLY) freeExtraClient(c);
    }
}


/* This function is called every time we are going to transmit new data
 * to the client. The behavior is the following:
 *
 * If the client should receive new data (normal clients will) the function
 * returns REDIS_OK, and make sure to install the write handler in our event
 * loop so that when the socket is writable new data gets written.
 *
 * If the client should not receive new data, because it is a fake client
 * (used to load AOF in memory), a master or because the setup of the write
 * handler failed, the function returns REDIS_ERR.
 *
 * The function may return REDIS_OK without actually installing the write
 * event handler in the following cases:
 *
 * 1) The event handler should already be installed since the output buffer
 *    already contained something.
 * 2) The client is a slave but not yet online, so we want to just accumulate
 *    writes in the buffer but not actually sending them yet.
 *
 * Typically gets called every time a reply is built, before adding more
 * data to the clients output buffers. If the function returns REDIS_ERR no
 * data should be appended to the output buffers. */
int prepareExtraClientToWrite(redisClient *c) {
    /* If it's the Lua client we always return ok without installing any
     * handler since there is no socket at all. */
    if (c->flags & REDIS_LUA_CLIENT) return REDIS_OK;

    /* Masters don't receive replies, unless REDIS_MASTER_FORCE_REPLY flag
     * is set. */
    if ((c->flags & REDIS_MASTER) &&
        !(c->flags & REDIS_MASTER_FORCE_REPLY)) return REDIS_ERR;

    if (c->fd <= 0) return REDIS_ERR; /* Fake client for AOF loading. */

    /* Only install the handler if not already installed and, in case of
     * slaves, if the client can actually receive writes. */
    if (c->bufpos == 0 && listLength(c->reply) == 0 &&
        (c->replstate == REDIS_REPL_NONE ||
         (c->replstate == REDIS_REPL_ONLINE && !c->repl_put_online_on_ack)))
    {
        /* Try to install the write handler. */
        if (aeCreateFileEvent(rextra.el, c->fd, AE_WRITABLE,
                sendReplyToExtraClient, c) == AE_ERR)
        {
            freeExtraClientAsync(c);
            return REDIS_ERR;
        }
    }

    /* Authorize the caller to queue in the output buffer of this client. */
    return REDIS_OK;
}

/* The function checks if the client reached output buffer soft or hard
 * limit, and also update the state needed to check the soft limit as
 * a side effect.
 *
 * Return value: non-zero if the client reached the soft or the hard limit.
 *               Otherwise zero is returned. */
int checkExtraClientOutputBufferLimits(redisClient *c) {
    int soft = 0, hard = 0, class;
    unsigned long used_mem = getClientOutputBufferMemoryUsage(c);

    class = getClientType(c);
    if (rextra.client_obuf_limits[class].hard_limit_bytes &&
        used_mem >= rextra.client_obuf_limits[class].hard_limit_bytes)
        hard = 1;
    if (rextra.client_obuf_limits[class].soft_limit_bytes &&
        used_mem >= rextra.client_obuf_limits[class].soft_limit_bytes)
        soft = 1;

    /* We need to check if the soft limit is reached continuously for the
     * specified amount of seconds. */
    if (soft) {
        if (c->obuf_soft_limit_reached_time == 0) {
            c->obuf_soft_limit_reached_time = rextra.unixtime;
            soft = 0; /* First time we see the soft limit reached */
        } else {
            time_t elapsed = rextra.unixtime - c->obuf_soft_limit_reached_time;

            if (elapsed <=
                rextra.client_obuf_limits[class].soft_limit_seconds) {
                soft = 0; /* The client still did not reached the max number of
                             seconds for the soft limit to be considered
                             reached. */
            }
        }
    } else {
        c->obuf_soft_limit_reached_time = 0;
    }
    return soft || hard;
}

/* Asynchronously close a client if soft or hard limit is reached on the
 * output buffer size. The caller can check if the client will be closed
 * checking if the client REDIS_CLOSE_ASAP flag is set.
 *
 * Note: we need to close the client asynchronously because this function is
 * called from contexts where the client can't be freed safely, i.e. from the
 * lower level functions pushing data inside the client output buffers. */
void asyncCloseExtraClientOnOutputBufferLimitReached(redisClient *c) {
    redisAssert(c->reply_bytes < ULONG_MAX-(1024*64));
    if (c->reply_bytes == 0 || c->flags & REDIS_CLOSE_ASAP) return;
    if (checkExtraClientOutputBufferLimits(c)) {
        //sds client = catClientInfoString(sdsempty(),c);

        freeClientAsync(c);
        //redisLog(REDIS_WARNING,"Client %s scheduled to be closed ASAP for overcoming of output buffer limits.", client);
        //sdsfree(client);
    }
}

/* -----------------------------------------------------------------------------
 * Low level functions to add more data to output buffers.
 * -------------------------------------------------------------------------- */

static int _addExtraReplyToBuffer(redisClient *c, char *s, size_t len) {
    size_t available = sizeof(c->buf)-c->bufpos;

    if (c->flags & REDIS_CLOSE_AFTER_REPLY) return REDIS_OK;

    /* If there already are entries in the reply list, we cannot
     * add anything more to the static buffer. */
    if (listLength(c->reply) > 0) return REDIS_ERR;

    /* Check that the buffer has enough space available for this string. */
    if (len > available) return REDIS_ERR;

    memcpy(c->buf+c->bufpos,s,len);
    c->bufpos+=len;
    return REDIS_OK;
}

static void _addExtraReplyObjectToList(redisClient *c, robj *o) {
    robj *tail;

    if (c->flags & REDIS_CLOSE_AFTER_REPLY) return;

    if (listLength(c->reply) == 0) {
        incrRefCount(o);
        listAddNodeTail(c->reply,o);
        c->reply_bytes += getStringObjectSdsUsedMemory(o);
    } else {
        tail = listNodeValue(listLast(c->reply));

        /* Append to this object when possible. */
        if (tail->ptr != NULL &&
            tail->encoding == REDIS_ENCODING_RAW &&
            sdslen(tail->ptr)+sdslen(o->ptr) <= REDIS_REPLY_CHUNK_BYTES)
        {
            c->reply_bytes -= zmalloc_size_sds(tail->ptr);
            tail = dupLastObjectIfNeeded(c->reply);
            tail->ptr = sdscatlen(tail->ptr,o->ptr,sdslen(o->ptr));
            c->reply_bytes += zmalloc_size_sds(tail->ptr);
        } else {
            incrRefCount(o);
            listAddNodeTail(c->reply,o);
            c->reply_bytes += getStringObjectSdsUsedMemory(o);
        }
    }
    asyncCloseExtraClientOnOutputBufferLimitReached(c);
}

void _addExtraReplyStringToList(redisClient *c, char *s, size_t len) {
    robj *tail;

    if (c->flags & REDIS_CLOSE_AFTER_REPLY) return;

    if (listLength(c->reply) == 0) {
        robj *o = createStringObject(s,len);

        listAddNodeTail(c->reply,o);
        c->reply_bytes += getStringObjectSdsUsedMemory(o);
    } else {
        tail = listNodeValue(listLast(c->reply));

        /* Append to this object when possible. */
        if (tail->ptr != NULL && tail->encoding == REDIS_ENCODING_RAW &&
            sdslen(tail->ptr)+len <= REDIS_REPLY_CHUNK_BYTES)
        {
            c->reply_bytes -= zmalloc_size_sds(tail->ptr);
            tail = dupLastObjectIfNeeded(c->reply);
            tail->ptr = sdscatlen(tail->ptr,s,len);
            c->reply_bytes += zmalloc_size_sds(tail->ptr);
        } else {
            robj *o = createStringObject(s,len);

            listAddNodeTail(c->reply,o);
            c->reply_bytes += getStringObjectSdsUsedMemory(o);
        }
    }
    asyncCloseExtraClientOnOutputBufferLimitReached(c);
}

static void addExtraReplyString(redisClient *c, char *s, size_t len) {
    if (prepareExtraClientToWrite(c) != REDIS_OK) return;
    if (_addExtraReplyToBuffer(c,s,len) != REDIS_OK)
        _addExtraReplyStringToList(c,s,len);
}

/* This method takes responsibility over the sds. When it is no longer
 * needed it will be free'd, otherwise it ends up in a robj. */
static void _addExtraReplySdsToList(redisClient *c, sds s) {
    robj *tail;

    if (c->flags & REDIS_CLOSE_AFTER_REPLY) {
        sdsfree(s);
        return;
    }

    if (listLength(c->reply) == 0) {
        listAddNodeTail(c->reply,createObject(REDIS_STRING,s));
        c->reply_bytes += zmalloc_size_sds(s);
    } else {
        tail = listNodeValue(listLast(c->reply));

        /* Append to this object when possible. */
        if (tail->ptr != NULL && tail->encoding == REDIS_ENCODING_RAW &&
            sdslen(tail->ptr)+sdslen(s) <= REDIS_REPLY_CHUNK_BYTES)
        {
            c->reply_bytes -= zmalloc_size_sds(tail->ptr);
            tail = dupLastObjectIfNeeded(c->reply);
            tail->ptr = sdscatlen(tail->ptr,s,sdslen(s));
            c->reply_bytes += zmalloc_size_sds(tail->ptr);
            sdsfree(s);
        } else {
            listAddNodeTail(c->reply,createObject(REDIS_STRING,s));
            c->reply_bytes += zmalloc_size_sds(s);
        }
    }
    asyncCloseExtraClientOnOutputBufferLimitReached(c);
}

static void addExtraReplySds(redisClient *c, sds s) {
    if (prepareExtraClientToWrite(c) != REDIS_OK) {
        /* The caller expects the sds to be free'd. */
        sdsfree(s);
        return;
    }
    if (_addExtraReplyToBuffer(c,s,sdslen(s)) == REDIS_OK) {
        sdsfree(s);
    } else {
        /* This method free's the sds when it is no longer needed. */
        _addExtraReplySdsToList(c,s);
    }
}

static void addExtraReplyErrorLength(redisClient *c, char *s, size_t len) {
    addExtraReplyString(c,"-ERR ",5);
    addExtraReplyString(c,s,len);
    addExtraReplyString(c,"\r\n",2);
}

static void addExtraReplyError(redisClient *c, char *err) {
    addExtraReplyErrorLength(c,err,strlen(err));
}

static void addExtraReplyErrorFormat(redisClient *c, const char *fmt, ...) {
    size_t l, j;
    va_list ap;
    va_start(ap,fmt);
    sds s = sdscatvprintf(sdsempty(),fmt,ap);
    va_end(ap);
    /* Make sure there are no newlines in the string, otherwise invalid protocol
     * is emitted. */
    l = sdslen(s);
    for (j = 0; j < l; j++) {
        if (s[j] == '\r' || s[j] == '\n') s[j] = ' ';
    }
    addExtraReplyErrorLength(c,s,sdslen(s));
    sdsfree(s);
}


/* -----------------------------------------------------------------------------
 * Higher level functions to queue data on the client output buffer.
 * The following functions are the ones that commands implementations will call.
 * -------------------------------------------------------------------------- */

static void addExtraReply(redisClient *c, robj *obj) {
    if (prepareExtraClientToWrite(c) != REDIS_OK) return;

    /* This is an important place where we can avoid copy-on-write
     * when there is a saving child running, avoiding touching the
     * refcount field of the object if it's not needed.
     *
     * If the encoding is RAW and there is room in the static buffer
     * we'll be able to send the object to the client without
     * messing with its page. */
    if (sdsEncodedObject(obj)) {
        if (_addExtraReplyToBuffer(c,obj->ptr,sdslen(obj->ptr)) != REDIS_OK)
            _addExtraReplyObjectToList(c,obj);
    } else if (obj->encoding == REDIS_ENCODING_INT) {
        /* Optimization: if there is room in the static buffer for 32 bytes
         * (more than the max chars a 64 bit integer can take as string) we
         * avoid decoding the object and go for the lower level approach. */
        if (listLength(c->reply) == 0 && (sizeof(c->buf) - c->bufpos) >= 32) {
            char buf[32];
            int len;

            len = ll2string(buf,sizeof(buf),(long)obj->ptr);
            if (_addExtraReplyToBuffer(c,buf,len) == REDIS_OK)
                return;
            /* else... continue with the normal code path, but should never
             * happen actually since we verified there is room. */
        }
        obj = getDecodedObject(obj);
        if (_addExtraReplyToBuffer(c,obj->ptr,sdslen(obj->ptr)) != REDIS_OK)
            _addExtraReplyObjectToList(c,obj);
        decrRefCount(obj);
    } else {
        redisPanic("Wrong obj->encoding in addReply()");
    }
}

/* Add a long long as integer reply or bulk len / multi bulk count.
 * Basically this is used to output <prefix><long long><crlf>. */
static void addExtraReplyLongLongWithPrefix(redisClient *c, long long ll, char prefix) {
    char buf[128];
    int len;

    /* Things like $3\r\n or *2\r\n are emitted very often by the protocol
     * so we have a few shared objects to use if the integer is small
     * like it is most of the times. */
    if (prefix == '*' && ll < REDIS_SHARED_BULKHDR_LEN) {
        addExtraReply(c,shared.mbulkhdr[ll]);
        return;
    } else if (prefix == '$' && ll < REDIS_SHARED_BULKHDR_LEN) {
        addExtraReply(c,shared.bulkhdr[ll]);
        return;
    }

    buf[0] = prefix;
    len = ll2string(buf+1,sizeof(buf)-1,ll);
    buf[len+1] = '\r';
    buf[len+2] = '\n';
    addExtraReplyString(c,buf,len+3);
}

static void addExtraReplyLongLong(redisClient *c, long long ll) {
    if (ll == 0)
        addExtraReply(c,shared.czero);
    else if (ll == 1)
        addExtraReply(c,shared.cone);
    else
        addExtraReplyLongLongWithPrefix(c,ll,':');
}

/* Create the length prefix of a bulk reply, example: $2234 */
static void addExtraReplyBulkLen(redisClient *c, robj *obj) {
    size_t len;

    if (sdsEncodedObject(obj)) {
        len = sdslen(obj->ptr);
    } else {
        long n = (long)obj->ptr;

        /* Compute how many bytes will take this integer as a radix 10 string */
        len = 1;
        if (n < 0) {
            len++;
            n = -n;
        }
        while((n = n/10) != 0) {
            len++;
        }
    }

    if (len < REDIS_SHARED_BULKHDR_LEN)
        addExtraReply(c,shared.bulkhdr[len]);
    else
        addExtraReplyLongLongWithPrefix(c,len,'$');
}

/* Add a C buffer as bulk reply */
static void addExtraReplyBulkCBuffer(redisClient *c, void *p, size_t len) {
    addExtraReplyLongLongWithPrefix(c,len,'$');
    addExtraReplyString(c,p,len);
    addExtraReply(c,shared.crlf);
}

/* Add a Redis Object as a bulk reply */
static void addExtraReplyBulk(redisClient *c, robj *obj) {
    addExtraReplyBulkLen(c,obj);
    addExtraReply(c,obj);
    addExtraReply(c,shared.crlf);
}

/* Helper function. Trims query buffer to make the function that processes
 * multi bulk requests idempotent. */
static void setExtraProtocolError(redisClient *c, int pos) {
    c->flags |= REDIS_CLOSE_AFTER_REPLY;
    sdsrange(c->querybuf,pos,-1);
}

int processInlineExtraBuffer(redisClient *c) {
    char *newline;
    int argc, j;
    sds *argv, aux;
    size_t querylen;

    /* Search for end of line */
    newline = strchr(c->querybuf,'\n');

    /* Nothing to do without a \r\n */
    if (newline == NULL) {
        if (sdslen(c->querybuf) > REDIS_INLINE_MAX_SIZE) {
            addExtraReplyError(c,"Protocol error: too big inline request");
            setExtraProtocolError(c,0);
        }
        return REDIS_ERR;
    }

    /* Handle the \r\n case. */
    if (newline && newline != c->querybuf && *(newline-1) == '\r')
        newline--;

    /* Split the input buffer up to the \r\n */
    querylen = newline-(c->querybuf);
    aux = sdsnewlen(c->querybuf,querylen);
    argv = sdssplitargs(aux,&argc);
    sdsfree(aux);
    if (argv == NULL) {
        addExtraReplyError(c,"Protocol error: unbalanced quotes in request");
        setExtraProtocolError(c,0);
        return REDIS_ERR;
    }

    /* Newline from slaves can be used to refresh the last ACK time.
     * This is useful for a slave to ping back while loading a big
     * RDB file. */
    if (querylen == 0 && c->flags & REDIS_SLAVE)
        c->repl_ack_time = rextra.unixtime;

    /* Leave data after the first line of the query in the buffer */
    sdsrange(c->querybuf,querylen+2,-1);

    /* Setup argv array on client structure */
    if (argc) {
        if (c->argv) zfree(c->argv);
        c->argv = zmalloc(sizeof(robj*)*argc);
    }

    /* Create redis objects for all arguments. */
    for (c->argc = 0, j = 0; j < argc; j++) {
        if (sdslen(argv[j])) {
            c->argv[c->argc] = createObject(REDIS_STRING,argv[j]);
            c->argc++;
        } else {
            sdsfree(argv[j]);
        }
    }
    zfree(argv);
    return REDIS_OK;
}

int processMultibulkExtraBuffer(redisClient *c) {
    char *newline = NULL;
    int pos = 0, ok;
    long long ll;

    if (c->multibulklen == 0) {
        /* The client should have been reset */
        //redisAssertWithInfo(c,NULL,c->argc == 0);

        /* Multi bulk length cannot be read without a \r\n */
        newline = strchr(c->querybuf,'\r');
        if (newline == NULL) {
            if (sdslen(c->querybuf) > REDIS_INLINE_MAX_SIZE) {
                addExtraReplyError(c,"Protocol error: too big mbulk count string");
                setExtraProtocolError(c,0);
            }
            return REDIS_ERR;
        }

        /* Buffer should also contain \n */
        if (newline-(c->querybuf) > ((signed)sdslen(c->querybuf)-2))
            return REDIS_ERR;

        /* We know for sure there is a whole line since newline != NULL,
         * so go ahead and find out the multi bulk length. */
        //redisAssertWithInfo(c,NULL,c->querybuf[0] == '*');
        ok = string2ll(c->querybuf+1,newline-(c->querybuf+1),&ll);
        if (!ok || ll > 1024*1024) {
            addExtraReplyError(c,"Protocol error: invalid multibulk length");
            setExtraProtocolError(c,pos);
            return REDIS_ERR;
        }

        pos = (newline-c->querybuf)+2;
        if (ll <= 0) {
            sdsrange(c->querybuf,pos,-1);
            return REDIS_OK;
        }

        c->multibulklen = ll;

        /* Setup argv array on client structure */
        if (c->argv) zfree(c->argv);
        c->argv = zmalloc(sizeof(robj*)*c->multibulklen);
    }

    //redisAssertWithInfo(c,NULL,c->multibulklen > 0);
    while(c->multibulklen) {
        /* Read bulk length if unknown */
        if (c->bulklen == -1) {
            newline = strchr(c->querybuf+pos,'\r');
            if (newline == NULL) {
                if (sdslen(c->querybuf) > REDIS_INLINE_MAX_SIZE) {
                    addExtraReplyError(c,
                        "Protocol error: too big bulk count string");
                    setExtraProtocolError(c,0);
                    return REDIS_ERR;
                }
                break;
            }

            /* Buffer should also contain \n */
            if (newline-(c->querybuf) > ((signed)sdslen(c->querybuf)-2))
                break;

            if (c->querybuf[pos] != '$') {
                addExtraReplyErrorFormat(c,
                    "Protocol error: expected '$', got '%c'",
                    c->querybuf[pos]);
                setExtraProtocolError(c,pos);
                return REDIS_ERR;
            }

            ok = string2ll(c->querybuf+pos+1,newline-(c->querybuf+pos+1),&ll);
            if (!ok || ll < 0 || ll > 512*1024*1024) {
                addExtraReplyError(c,"Protocol error: invalid bulk length");
                setExtraProtocolError(c,pos);
                return REDIS_ERR;
            }

            pos += newline-(c->querybuf+pos)+2;
            if (ll >= REDIS_MBULK_BIG_ARG) {
                size_t qblen;

                /* If we are going to read a large object from network
                 * try to make it likely that it will start at c->querybuf
                 * boundary so that we can optimize object creation
                 * avoiding a large copy of data. */
                sdsrange(c->querybuf,pos,-1);
                pos = 0;
                qblen = sdslen(c->querybuf);
                /* Hint the sds library about the amount of bytes this string is
                 * going to contain. */
                if (qblen < (size_t)ll+2)
                    c->querybuf = sdsMakeRoomFor(c->querybuf,ll+2-qblen);
            }
            c->bulklen = ll;
        }

        /* Read bulk argument */
        if (sdslen(c->querybuf)-pos < (unsigned)(c->bulklen+2)) {
            /* Not enough data (+2 == trailing \r\n) */
            break;
        } else {
            /* Optimization: if the buffer contains JUST our bulk element
             * instead of creating a new object by *copying* the sds we
             * just use the current sds string. */
            if (pos == 0 &&
                c->bulklen >= REDIS_MBULK_BIG_ARG &&
                (signed) sdslen(c->querybuf) == c->bulklen+2)
            {
                c->argv[c->argc++] = createObject(REDIS_STRING,c->querybuf);
                sdsIncrLen(c->querybuf,-2); /* remove CRLF */
                c->querybuf = sdsempty();
                /* Assume that if we saw a fat argument we'll see another one
                 * likely... */
                c->querybuf = sdsMakeRoomFor(c->querybuf,c->bulklen+2);
                pos = 0;
            } else {
                c->argv[c->argc++] =
                    createStringObject(c->querybuf+pos,c->bulklen);
                pos += c->bulklen+2;
            }
            c->bulklen = -1;
            c->multibulklen--;
        }
    }

    /* Trim to pos */
    if (pos) sdsrange(c->querybuf,pos,-1);

    /* We're done when c->multibulk == 0 */
    if (c->multibulklen == 0) return REDIS_OK;

    /* Still not read to process the command */
    return REDIS_ERR;
}

static struct redisCommand *lookupExtraCommand(sds name) {
    return dictFetchValue(rextra.commands, name);
}

/* ExtraCall() is the core of Redis execution of a extra command */
static void extraCall(redisClient *c, int flags) {
    long long start, duration;
    int client_old_flags = c->flags;

    /* Call the command. */
    c->flags &= ~(REDIS_FORCE_AOF|REDIS_FORCE_REPL);
    
    start = ustime();
    c->cmd->proc(c);
    duration = ustime()-start;

    if (flags & REDIS_CALL_STATS) {
        c->cmd->microseconds += duration;
        c->cmd->calls++;
    }

    /* Restore the old FORCE_AOF/REPL flags, since call can be executed
     * recursively. */
    c->flags &= ~(REDIS_FORCE_AOF|REDIS_FORCE_REPL);
    c->flags |= client_old_flags & (REDIS_FORCE_AOF|REDIS_FORCE_REPL);

    rextra.stat_numcommands ++;
}


/* If this function gets called we already read a whole
 * command, arguments are in the client argv/argc fields.
 * processCommand() execute the command or prepare the
 * server for a bulk read from the client.
 *
 * If 1 is returned the client is still alive and valid and
 * other operations can be performed by the caller. Otherwise
 * if 0 is returned the client was destroyed (i.e. after QUIT). */
static int processExtraCommand(redisClient *c) {
    /* The QUIT command is handled separately. Normal command procs will
     * go through checking for replication and QUIT will cause trouble
     * when FORCE_REPLICATION is enabled and would be implemented in
     * a regular command proc. */
    if (!strcasecmp(c->argv[0]->ptr,"quit")) {
        addExtraReply(c,shared.ok);
        c->flags |= REDIS_CLOSE_AFTER_REPLY;
        return REDIS_ERR;
    }

    /* Now lookup the command and check ASAP about trivial error conditions
     * such as wrong arity, bad command name and so forth. */
    c->cmd = c->lastcmd = lookupExtraCommand(c->argv[0]->ptr);
    if (!c->cmd) {
        flagTransaction(c);
        addExtraReplyErrorFormat(c,"unknown command '%s'",
            (char*)c->argv[0]->ptr);
        return REDIS_OK;
    } else if ((c->cmd->arity > 0 && c->cmd->arity != c->argc) ||
               (c->argc < -c->cmd->arity)) {
        flagTransaction(c);
        addExtraReplyErrorFormat(c,"wrong number of arguments for '%s' command",
            c->cmd->name);
        return REDIS_OK;
    }

    extraCall(c,REDIS_CALL_FULL);

    return REDIS_OK;
}

static void processExtraInputBuffer(redisClient *c) {
    /* Keep processing while there is something in the input buffer */
    while(sdslen(c->querybuf)) {

        /* Immediately abort if the client is in the middle of something. */
        if (c->flags & REDIS_BLOCKED) return;

        /* REDIS_CLOSE_AFTER_REPLY closes the connection once the reply is
         * written to the client. Make sure to not let the reply grow after
         * this flag has been set (i.e. don't process more commands). */
        if (c->flags & REDIS_CLOSE_AFTER_REPLY) return;

        /* Determine request type when unknown. */
        if (!c->reqtype) {
            if (c->querybuf[0] == '*') {
                c->reqtype = REDIS_REQ_MULTIBULK;
            } else {
                c->reqtype = REDIS_REQ_INLINE;
            }
        }

        if (c->reqtype == REDIS_REQ_INLINE) {
            if (processInlineExtraBuffer(c) != REDIS_OK) break;
        } else if (c->reqtype == REDIS_REQ_MULTIBULK) {
            if (processMultibulkExtraBuffer(c) != REDIS_OK) break;
        } else {
            redisPanic("Unknown request type");
        }

        /* Multibulk processing could see a <= 0 length. */
        if (c->argc == 0) {
            resetExtraClient(c);
        } else {
            /* Only reset the client when the command was executed. */
            if (processExtraCommand(c) == REDIS_OK)
                resetExtraClient(c);
        }
    }
}

static void readQueryFromExtraClient(aeEventLoop *el, int fd, void *privdata, int mask) {    
    redisClient *c = (redisClient*) privdata;
    int nread, readlen;
    size_t qblen;
    REDIS_NOTUSED(el);
    REDIS_NOTUSED(mask);

    rextra.current_client = c;
    readlen = REDIS_IOBUF_LEN;
    /* If this is a multi bulk request, and we are processing a bulk reply
     * that is large enough, try to maximize the probability that the query
     * buffer contains exactly the SDS string representing the object, even
     * at the risk of requiring more read(2) calls. This way the function
     * processMultiBulkBuffer() can avoid copying buffers to create the
     * Redis Object representing the argument. */
    if (c->reqtype == REDIS_REQ_MULTIBULK && c->multibulklen && c->bulklen != -1
        && c->bulklen >= REDIS_MBULK_BIG_ARG)
    {
        int remaining = (unsigned)(c->bulklen+2)-sdslen(c->querybuf);

        if (remaining < readlen) readlen = remaining;
    }

    qblen = sdslen(c->querybuf);
    if (c->querybuf_peak < qblen) c->querybuf_peak = qblen;
    c->querybuf = sdsMakeRoomFor(c->querybuf, readlen);
    nread = read(fd, c->querybuf+qblen, readlen);
    if (nread == -1) {
        if (errno == EAGAIN) {
            nread = 0;
        } else {
            //redisLog(REDIS_VERBOSE, "Reading from client: %s",strerror(errno));
            freeExtraClient(c);
            return;
        }
    } else if (nread == 0) {
        //redisLog(REDIS_VERBOSE, "Client closed connection");
        freeExtraClient(c);
        return;
    }
    if (nread) {
        sdsIncrLen(c->querybuf,nread);
        c->lastinteraction = rextra.unixtime;
        if (c->flags & REDIS_MASTER) c->reploff += nread;
        rextra.stat_net_input_bytes += nread;
    } else {
        rextra.current_client = NULL;
        return;
    }
    if (sdslen(c->querybuf) > rextra.client_max_querybuf_len) {
        //sds ci = catClientInfoString(sdsempty(),c), bytes = sdsempty();

        //bytes = sdscatrepr(bytes,c->querybuf,64);
        //redisLog(REDIS_WARNING,"Closing client that reached max query buffer length: %s (qbuf initial bytes: %s)", ci, bytes);
        //sdsfree(ci);
        //sdsfree(bytes);
        freeExtraClient(c);
        return;
    }
    processExtraInputBuffer(c);
    rextra.current_client = NULL;
}

#define MAX_ACCEPTS_PER_CALL 1000
static void acceptExtraCommonHandler(int fd, int flags) {
    redisClient *c;
    if ((c = createExtraClient(fd)) == NULL) {
        //redisLog(REDIS_WARNING,
        //    "Error registering fd event for the new client: %s (fd=%d)",
        //    strerror(errno),fd);
        close(fd); /* May be already closed, just ignore errors */
        return;
    }
    /* If maxclient directive is set and this is one client more... close the
     * connection. Note that we create the client instead to check before
     * for this condition, since now the socket is already set in non-blocking
     * mode and we can send an error for free using the Kernel I/O */
    if (listLength(rextra.clients) > rextra.maxclients) {
        char *err = "-ERR max number of clients reached\r\n";

        /* That's a best effort error message, don't check write errors */
        if (write(c->fd,err,strlen(err)) == -1) {
            /* Nothing to do, Just to avoid the warning... */
        }
        rextra.stat_rejected_conn++;
        freeExtraClient(c);
        return;
    }
    rextra.stat_numconnections++;
    c->flags |= flags;
}

static void acceptExtraTCPHandler(aeEventLoop *el, int fd, void *privdata, int mask) {
    int cport, cfd, max = MAX_ACCEPTS_PER_CALL;
    char cip[REDIS_IP_STR_LEN];
    REDIS_NOTUSED(el);
    REDIS_NOTUSED(mask);
    REDIS_NOTUSED(privdata);

    while(max--) {
        cfd = anetTcpAccept(rextra.neterr, fd, cip, sizeof(cip), &cport);
        if (cfd == ANET_ERR) {
            if (errno != EWOULDBLOCK) {
                //redisLog(REDIS_WARNING,
                //    "Accepting client connection: %s", rextra.neterr);
            }
            
            return;
        }
        //redisLog(REDIS_VERBOSE,"Accepted %s:%d", cip, cport);
        acceptExtraCommonHandler(cfd,0);
    }
}

static void freeExtraClientsInAsyncFreeQueue(void) {
    while (listLength(rextra.clients_to_close)) {
        listNode *ln = listFirst(rextra.clients_to_close);
        redisClient *c = listNodeValue(ln);

        c->flags &= ~REDIS_CLOSE_ASAP;
        freeExtraClient(c);
        listDelNode(rextra.clients_to_close,ln);
    }
}

/* This is our timer interrupt, called server.hz times per second.
 * Here is where we do a number of things that need to be done asynchronously.
 * For instance:
 *
 * - Close client in the rextra.clients_to_close list.
 * - Many more...
 *
 * Everything directly called here will be called server.hz times per second.
 */

static int extraCron(struct aeEventLoop *eventLoop, long long id, void *clientData) {
    REDIS_NOTUSED(eventLoop);
    REDIS_NOTUSED(id);
    REDIS_NOTUSED(clientData);

    /* Update the time cache. */
    rextra.unixtime = time(NULL);

    /* Close clients that need to be closed asynchronous */
    freeExtraClientsInAsyncFreeQueue();

    
    pthread_testcancel();
    return 1000/rextra.hz;
}


/* Populates the Redis Command Table starting from the hard coded list
 * we have on top of redis.c file. */
static void populateExtraCommandTable(void) {
    int j;
    int numcommands = sizeof(redisExtraCommandTable)/sizeof(struct redisCommand);

    for (j = 0; j < numcommands; j++) {
        struct redisCommand *c = redisExtraCommandTable+j;
        char *f = c->sflags;
        int retval1;

        while(*f != '\0') {
            switch(*f) {
            case 'w': c->flags |= REDIS_CMD_WRITE; break;
            case 'r': c->flags |= REDIS_CMD_READONLY; break;
            case 'm': c->flags |= REDIS_CMD_DENYOOM; break;
            case 'a': c->flags |= REDIS_CMD_ADMIN; break;
            case 'p': c->flags |= REDIS_CMD_PUBSUB; break;
            case 's': c->flags |= REDIS_CMD_NOSCRIPT; break;
            case 'R': c->flags |= REDIS_CMD_RANDOM; break;
            case 'S': c->flags |= REDIS_CMD_SORT_FOR_SCRIPT; break;
            case 'l': c->flags |= REDIS_CMD_LOADING; break;
            case 't': c->flags |= REDIS_CMD_STALE; break;
            case 'M': c->flags |= REDIS_CMD_SKIP_MONITOR; break;
            case 'k': c->flags |= REDIS_CMD_ASKING; break;
            case 'F': c->flags |= REDIS_CMD_FAST; break;
            default: redisPanic("Unsupported command flag"); break;
            }
            f++;
        }

        retval1 = dictAdd(rextra.commands, sdsnew(c->name), c);
        redisAssert(retval1 == DICT_OK);
    }
}

void initExtraConfig(void) {
    int i;
    
    rextra.thread = 0;
    rextra.enabled = 0;
    rextra.stat_starttime = 0;
    rextra.next_client_id = 1;  /* Client IDs, start from 1 .*/
    rextra.el = NULL;
    rextra.hz = 0;
    rextra.port = 0;
    rextra.efd_count = 0;
    rextra.neterr[0] = '\0';
    rextra.unixtime = 0;
    rextra.commands = NULL;    
    rextra.tcpkeepalive = 0;
    rextra.client_max_querybuf_len = 0;
    rextra.maxclients = 0;
    rextra.current_client = NULL;
    rextra.clients = NULL;
    rextra.clients_to_close = NULL;
    rextra.stat_rejected_conn = 0;
    rextra.stat_numconnections = 0;
    rextra.stat_net_input_bytes = 0;
    rextra.stat_net_output_bytes = 0;
    rextra.stat_numcommands = 0;

    for (i = 0; i < REDIS_CLIENT_TYPE_COUNT; i ++) {
        rextra.client_obuf_limits[i].hard_limit_bytes = 0;
        rextra.client_obuf_limits[i].soft_limit_bytes = 0;
    }
}

int initExtra(void){

    rextra.stat_starttime = time(NULL);
    rextra.stat_numconnections = 0;
    rextra.stat_net_input_bytes = 0;
    rextra.stat_net_output_bytes = 0;
    rextra.stat_rejected_conn = 0;
    rextra.stat_numcommands = 0;
    
    rextra.client_max_querybuf_len = 512;
    rextra.maxclients = 100;
    rextra.tcpkeepalive = 120;
    rextra.hz = 10;
    rextra.unixtime = time(NULL);

    rextra.commands = dictCreate(&commandTableDictType,NULL);
    populateExtraCommandTable();

    rextra.clients = listCreate();
    rextra.clients_to_close = listCreate();
    
    rextra.el = aeCreateEventLoop(rextra.maxclients+REDIS_EVENTLOOP_FDSET_INCR);
    
    /* Create the extraCron() time event, that's our main way to process
     * background operations. */
    if(aeCreateTimeEvent(rextra.el, 1, extraCron, NULL, NULL) == AE_ERR) {
        redisLog(REDIS_WARNING, "Can't create the extraCron time event.");
        return REDIS_ERR;
    }

    if (listenToPort(rextra.port,rextra.efd,&rextra.efd_count) == REDIS_ERR) {
        return REDIS_ERR;
    } else {
        int j;

        for (j = 0; j < rextra.efd_count; j++) {
            if (aeCreateFileEvent(rextra.el, rextra.efd[j], AE_READABLE,
                acceptExtraTCPHandler, NULL) == AE_ERR)
                    redisPanic("Unrecoverable error creating Nonblock thread "
                                "file event.");
        }
    }

    return REDIS_OK;
}

void deinitExtra(void) {
    int i;
    listNode *ln;
    redisClient *c;

    rextra.neterr[0] = '\0';
    
    if (rextra.clients) {
        while (listLength(rextra.clients) > 0) {
            ln = listFirst(rextra.clients);
            c = listNodeValue(ln);
            freeExtraClient(c);
        }
        listRelease(rextra.clients);
        rextra.clients = NULL;
    }

    if (rextra.clients_to_close) {
        listRelease(rextra.clients_to_close);
        rextra.clients_to_close = NULL;
    }
    
    if (rextra.el) {
        aeDeleteEventLoop(rextra.el);
        rextra.el = NULL;
    }

    for (i = 0; i < rextra.efd_count; i ++) {
        close(rextra.efd[i]);
    }
    rextra.efd_count = 0;
}

void *extraThreadRun(void *arg) {
    REDIS_NOTUSED(arg);

    /* Make the thread killable at any time, so that extraThreadKill()
     * can work reliably. */
    pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL);
    pthread_setcanceltype(PTHREAD_CANCEL_DEFERRED, NULL);

    aeMain(rextra.el);
    aeDeleteEventLoop(rextra.el);
}

int extraThreadInit(void) {
    int ret;
    pthread_attr_t attr;
    pthread_t thread;

    ret = initExtra();
    if (ret != REDIS_OK)
        return REDIS_ERR;

    pthread_attr_init(&attr);
    if (pthread_create(&thread,&attr,extraThreadRun,NULL) != 0) {
        redisLog(REDIS_WARNING,"Fatal: Can't initialize extra thread.");
        deinitExtra();
        return REDIS_ERR;
    }

    rextra.thread = thread;
    rextra.enabled = 1;

    redisLog(REDIS_NOTICE, "Extra thread started.");
    return REDIS_OK;
}

int extraThreadKill(void) {
    int err;
    
    if (pthread_cancel(rextra.thread) == 0) {
        if ((err = pthread_join(rextra.thread,NULL)) != 0) {
            redisLog(REDIS_WARNING,
                "Extra thread can be joined: %s.", strerror(err));
        } else {
            redisLog(REDIS_WARNING,
                "Extra thread terminated.");
        }
    }
    
    deinitExtra();
    rextra.enabled = 0;
    redisLog(REDIS_NOTICE, "Extra thread exited.");
    return REDIS_OK;
}

/* EXTRA command implementations.
 *
 * EXTRA RUN: run the extra thread.
 * EXTRA STOP: stop the extra thread.
 */
void extraCommand(redisClient *c) {
    int port, old_port;
    
    if (!strcasecmp(c->argv[1]->ptr,"run") && c->argc <= 3) {
        if (rextra.enabled) {
            addReplyError(c, "Extra thread is already running.");
            return;
        }

        if (c->argc == 3) {
            port = atoi(c->argv[2]->ptr);
            if (port <=0 || port > 65535) {
                addReplyError(c, "port is invalid.");
                return;
            }
        } else if (rextra.port <=0 || rextra.port > 65535) {
            addReplyError(c, "Original extra-port is invalid, set it at first.");
            return;
        } else {
            port = rextra.port;
        }

        old_port = rextra.port;
        rextra.port = port;
        if (extraThreadInit() == REDIS_OK) {
            addReply(c,shared.ok);
            return;
        } else {
            rextra.port = old_port;
            addReplyError(c, "Extra thread init failed.");
            return;
        }
    } else if (!strcasecmp(c->argv[1]->ptr,"stop") && c->argc == 2) {
        if(!rextra.enabled) {
            addReplyError(c, "Extra thread is not running.");
            return;
        }

        extraThreadKill();
        addReply(c,shared.ok);
    } else if (!strcasecmp(c->argv[1]->ptr,"status") && c->argc == 2) {
        sds s = sdscatfmt(sdsempty(),
            "%s %i",
            rextra.enabled?"enabled":"disabled",
            rextra.port);
        addReplyBulkCString(c,s);
        sdsfree(s);
    } else {
        addReply(c,shared.syntaxerr);
    }
}


