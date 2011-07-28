/*
Copyright (c) 2011 Louis-Philippe Perron

Permission is hereby granted, free of charge, to any person obtaining a copy of this software
and associated documentation files (the "Software"), to deal in the Software without restriction,
including without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense,
and/or sell copies of the Software, and to permit persons to whom the Software is furnished to do so,
subject to the following conditions:

The above copyright notice and this permission notice shall be included in all copies or substantial
portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT
LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE
OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
*/

#include "m_pd.h"
#include <hiredis.h>
#include <async.h>
#include <stdlib.h>
#include <string.h>
#include <csv.h>

/* gcc -ansi -Wall -O2 -fPIC -bundle -undefined suppress -flat_namespace -arch i386 -I/Applications/Pd-extended.app/Contents/Resources/include -I./hiredis/includes ./hiredis/libhiredis.a -o puredis.pd_darwin puredis.c */

#define PUREDIS_MAJOR 0
#define PUREDIS_MINOR 4
#define PUREDIS_PATCH 2
#define PD_MAJOR_VERSION 0
#define PD_MINOR_VERSION 42

#define MAX_ARRAY_SIZE 512

static t_class *puredis_class;
static t_class *apuredis_class;
static t_class *spuredis_class;

typedef struct _puredis {
    t_object x_obj;
    redisContext * redis;
    
    char * r_host;
    int r_port;
    int out_count;
    t_atom out[MAX_ARRAY_SIZE];
    
    /* async vars */
    int async;
    int qcount;
    t_outlet *q_out;
    
    /* sub vars */
    int sub_num;
    int sub_run;
    t_clock  *sub_clock;
    
    /* loader vars */
    t_symbol * ltype;
    int lnumload;
    int lnumerror;
    char * lcmd;
    char * lkey;
    int lnew;
    int lcount;
    /* hash loader vars */
    int hargc;
    char ** hheaders;
    int hbufsize;
    int hcount;
    /* zset loader vars */
    int zcount;
    char * zscore;
} t_puredis;

/* declarations */
void puredis_setup(void);

/* memory */
static void freeVectorAndLengths(int argc, char ** vector, size_t * lengths);
void redis_free(t_puredis *x);

/* general */
void *redis_new(t_symbol *s, int argc, t_atom *argv);
void redis_command(t_puredis *x, t_symbol *s, int argc, t_atom *argv);
static void redis_postCommandAsync(t_puredis * x, int argc, char ** vector, size_t * lengths);
static void redis_prepareOutList(t_puredis *x, redisReply * reply);
static void redis_parseReply(t_puredis *x, redisReply * reply);

/* puredis */
static void setup_puredis(void);
static void puredis_postCommandSync(t_puredis * x, int argc, char ** vector, size_t * lengths);
static void puredis_csv_postCommand(t_puredis * x, int argc, char ** vector, size_t * lengths);
static void puredis_csv_parse(t_puredis *x, int argc, void *s, size_t i);
static void puredis_csv_cb1 (void *s, size_t i, void *userdata);
static void puredis_csv_cb2 (int c, void *userdata);
static void puredis_csv_init(t_puredis *x);
static void puredis_csv_free(t_puredis *x);
void puredis_csv(t_puredis *x, t_symbol *s, int argc, t_atom *argv);

/* async redis */
static void setup_apuredis(void);
static void apuredis_q_out(t_puredis * x);
void apuredis_yield(t_puredis * x);
void apuredis_bang(t_puredis *x);

/* subscriber redis */
static void setup_spuredis(void);
static void spuredis_run(t_puredis *x);
static void spuredis_schedule(t_puredis *x);
static void spuredis_manage(t_puredis *x, t_symbol *s, int argc);
void spuredis_bang(t_puredis *x);
void spuredis_start(t_puredis *x, t_symbol *s);
void spuredis_stop(t_puredis *x, t_symbol *s);
void spuredis_subscribe(t_puredis *x, t_symbol *s, int argc, t_atom *argv);


/* implementation */

/* setup */
void puredis_setup(void)
{
    setup_puredis();
    setup_apuredis();
    setup_spuredis();
    post("Puredis %i.%i.%i (MIT) 2011 Louis-Philippe Perron <lp@spiralix.org>", PUREDIS_MAJOR, PUREDIS_MINOR, PUREDIS_PATCH);
    post("Puredis: compiled for pd-%d.%d on %s %s", PD_MAJOR_VERSION, PD_MINOR_VERSION, __DATE__, __TIME__);
}

/* memory */

static void freeVectorAndLengths(int argc, char ** vector, size_t * lengths)
{
    int i;
    for (i = 0; i < argc; i++) free(vector[i]);
    free(vector);
    free(lengths);
}

void redis_free(t_puredis *x)
{
    redisFree(x->redis);
}

/* general */

void *redis_new(t_symbol *s, int argc, t_atom *argv)
{
    t_puredis *x = NULL;
    
    int port = 6379;
    char host[16] = "127.0.0.1";
    switch(argc){
        default:
            break;
        case 2:
            port = (int)atom_getint(argv+1);
        case 1:
            atom_string(argv, host, 16);
            break;
        case 0:
            break;
    }
    
    if (s == gensym("apuredis")) {
        x = (t_puredis*)pd_new(apuredis_class);
        x->redis = redisConnectNonBlock((char*)host,port);
        x->async = 1; x->qcount = 0;
    } else if (s == gensym("spuredis")) {
        x = (t_puredis*)pd_new(spuredis_class);
        x->redis = redisConnectNonBlock((char*)host,port);
        x->sub_clock = clock_new(x, (t_method)spuredis_run);
        x->sub_num = 0; x->sub_run = 0;
    } else {
        x = (t_puredis*)pd_new(puredis_class);
        x->redis = redisConnect((char*)host,port);
        x->async = 0;
    }
    x->r_host = (char*)host;
    x->r_port = port;
    outlet_new(&x->x_obj, NULL);
    if (x->async) {
        x->q_out = outlet_new(&x->x_obj, &s_float);
    }
    
    if (x->redis->err) {
        post("could not connect to redis...");
        return NULL;
    }
    post("Puredis %i.%i.%i connected to redis host: %s port: %u", PUREDIS_MAJOR, PUREDIS_MINOR, PUREDIS_PATCH, x->r_host, x->r_port);
    
    return (void*)x;
}

void redis_command(t_puredis *x, t_symbol *s, int argc, t_atom *argv)
{
    if (argc < 1) {
        post("puredis: wrong command"); return;
    }
    
    int i;
    char ** vector = NULL;
    size_t * lengths = NULL;
    
    if (((vector = malloc(argc*sizeof(char*))) == NULL) || ((lengths = malloc(argc*sizeof(size_t))) == NULL)) {
        post("puredis: can not proceed!!  Memory Error!"); return;
    }
    
    for (i = 0; i < argc; i++) {
        char cmdpart[256];
        atom_string(argv+i, cmdpart, 256);
        
        if ((vector[i] = malloc(strlen(cmdpart)+1)) == NULL) {
            post("puredis: can not proceed!!  Memory Error!"); return;
        }
        
        strcpy(vector[i], (char*)cmdpart);
        lengths[i] = strlen(vector[i]);
    }
    if (x->async) {
        redis_postCommandAsync(x, argc, vector, lengths);
        x->qcount++;
        apuredis_q_out(x);
    } else {
        puredis_postCommandSync(x, argc, vector, lengths);
    }
}

static void redis_postCommandAsync(t_puredis * x, int argc, char ** vector, size_t * lengths)
{
    redisAppendCommandArgv(x->redis, argc, (const char**)vector, (const size_t *)lengths);
    freeVectorAndLengths(argc, vector, lengths);
}

static void redis_prepareOutList(t_puredis *x, redisReply * reply)
{
    if (reply->type == REDIS_REPLY_ERROR) {
        SETSYMBOL(&x->out[x->out_count],gensym(reply->str));
        x->out_count++;
    } else if (reply->type == REDIS_REPLY_STATUS) {
        SETSYMBOL(&x->out[x->out_count],gensym(reply->str));
        x->out_count++;
    } else if (reply->type == REDIS_REPLY_STRING) {
        SETSYMBOL(&x->out[x->out_count],gensym(reply->str));
        x->out_count++;
    } else if (reply->type == REDIS_REPLY_ARRAY) {
        int i;
        for (i = 0; i < (int)reply->elements; i++) {
            redis_prepareOutList(x,reply->element[i]);
        }
    } else if (reply->type == REDIS_REPLY_INTEGER) {
        SETFLOAT(&x->out[x->out_count],reply->integer);
        x->out_count++;
    } else if (reply->type == REDIS_REPLY_NIL) {
        SETSYMBOL(&x->out[x->out_count],gensym("nil"));
        x->out_count++;
    }
}

static void redis_parseReply(t_puredis *x, redisReply * reply)
{
    if (reply->type == REDIS_REPLY_ERROR) {
        outlet_symbol(x->x_obj.ob_outlet, gensym(reply->str));
    } else if (reply->type == REDIS_REPLY_STATUS) {
        outlet_symbol(x->x_obj.ob_outlet, gensym(reply->str));
    } else if (reply->type == REDIS_REPLY_STRING) {
        outlet_symbol(x->x_obj.ob_outlet, gensym(reply->str));
    } else if (reply->type == REDIS_REPLY_ARRAY) {
        x->out_count = 0;
        redis_prepareOutList(x,reply);
        outlet_list(x->x_obj.ob_outlet, &s_list, x->out_count, &x->out[0]);
    } else if (reply->type == REDIS_REPLY_INTEGER) {
        t_atom value;
        SETFLOAT(&value, reply->integer);
        outlet_float(x->x_obj.ob_outlet, atom_getfloat(&value));
    } else if (reply->type == REDIS_REPLY_NIL) {
        outlet_symbol(x->x_obj.ob_outlet, gensym("nil"));
    }
    freeReplyObject(reply);
}

/* puredis */

static void setup_puredis(void)
{
    puredis_class = class_new(gensym("puredis"),
        (t_newmethod)redis_new,
        (t_method)redis_free,
        sizeof(t_puredis),
        CLASS_DEFAULT,
        A_GIMME, 0);
    
    class_addmethod(puredis_class,
        (t_method)redis_command, gensym("command"),
        A_GIMME, 0);
    class_addmethod(puredis_class,
        (t_method)puredis_csv, gensym("csv"),
        A_GIMME, 0);
    class_sethelpsymbol(puredis_class, gensym("puredis-help"));
}

static void puredis_postCommandSync(t_puredis * x, int argc, char ** vector, size_t * lengths)
{
    redisReply * reply = redisCommandArgv(x->redis, argc, (const char**)vector, (const size_t *)lengths);
    freeVectorAndLengths(argc, vector, lengths);
    redis_parseReply(x,reply);
}

static void puredis_csv_postCommand(t_puredis * x, int argc, char ** vector, size_t * lengths)
{
    redisReply * reply = redisCommandArgv(x->redis, argc, (const char**)vector, (const size_t *)lengths);
    freeVectorAndLengths(argc, vector, lengths);
    x->lnumload++;
    if (reply->type == REDIS_REPLY_ERROR) {
        x->lnumerror++;
        x->lnumload--;
        post("Puredis csv load Redis error: %s", reply->str);
    }
    freeReplyObject(reply);
}

static void puredis_csv_parse(t_puredis *x, int argc, void *s, size_t i)
{
    char ** vector = NULL;
    size_t * lengths = NULL;
    char * item = (char*)s;
    
    if (((vector = malloc(argc*sizeof(char*))) == NULL) || ((lengths = malloc(argc*sizeof(size_t))) == NULL)) {
        post("puredis: can not proceed!!  Memory Error!"); return;
    }
    
    if ((vector[0] = malloc(strlen(x->lcmd)+1)) == NULL) {
        post("puredis: can not proceed!!  Memory Error!"); return;
    }
    strcpy(vector[0], x->lcmd);
    lengths[0] = strlen(vector[0]);
    
    if ((vector[1] = malloc(strlen(x->lkey)+1)) == NULL) {
        post("puredis: can not proceed!!  Memory Error!"); return;
    }
    strcpy(vector[1], x->lkey);
    lengths[1] = strlen(vector[1]);
    
    if (x->ltype == gensym("hash")) {
        if ((vector[2] = malloc(strlen(x->hheaders[x->hcount])+1)) == NULL) {
            post("puredis: can not proceed!!  Memory Error!"); return;
        }
        strcpy(vector[2], x->hheaders[x->hcount]);
        lengths[2] = strlen(vector[2]);
    } else if (x->ltype == gensym("zset")) {
        if ((vector[2] = malloc(strlen(x->zscore)+1)) == NULL) {
            post("puredis: can not proceed!!  Memory Error!"); return;
        }
        strcpy(vector[2], x->zscore);
        lengths[2] = strlen(vector[2]);
    }
    
    if ((vector[argc-1] = malloc(i+1)) == NULL) {
        post("puredis: can not proceed!!  Memory Error!"); return;
    }
    strcpy(vector[argc-1], item);
    lengths[argc-1] = i;
    
    puredis_csv_postCommand(x, argc, vector, lengths);
}

static void puredis_csv_cb1 (void *s, size_t i, void *userdata)
{
    t_puredis *x = (t_puredis *)userdata;
    
    if (x->lnew && x->lkey[0] == '#') return;
    
    if (x->ltype == gensym("hash")) {
        if (x->lcount > 0) {
            if (x->lnew) {
                x->hcount++;
                puredis_csv_parse(x, 4, s,i);
            } else {
                x->lnew = 1;
                x->hcount = 0;
                free(x->lkey);
                if ((x->lkey = malloc(i+1)) == NULL) {
                    post("puredis: can not proceed!!  Memory Error!"); return;
                }
                strcpy(x->lkey, (char*)s);
            }
        } else {
            if ((x->hheaders[x->hcount] = malloc(i+1)) == NULL) {
                post("puredis: can not proceed!!  Memory Error!"); return;
            }
            strcpy(x->hheaders[x->hcount], (char*)s);
            x->hcount++;
            x->hargc++;
            if (x->hbufsize == x->hargc) {
                x->hbufsize += x->hbufsize / 2;
                x->hheaders = realloc(x->hheaders, x->hbufsize * sizeof(char*));
            }
        }
    } else if (x->ltype == gensym("zset")) {
        if (x->lnew) {
            if (x->zcount) {
                puredis_csv_parse(x, 4, s,i);
                x->zcount = 0;
            } else {
                x->zcount = 1;
                free(x->zscore);
                if ((x->zscore = malloc(i+1)) == NULL) {
                    post("puredis: can not proceed!!  Memory Error!"); return;
                }
                strcpy(x->zscore, (char*)s);
            }
        } else {
            x->lnew = 1;
            x->zcount = 0;
            free(x->lkey);
            if ((x->lkey = malloc(i+1)) == NULL) {
                post("puredis: can not proceed!!  Memory Error!"); return;
            }
            strcpy(x->lkey, (char*)s);
        }
    } else {
        if (x->lnew) {
            puredis_csv_parse(x, 3, s,i);
        } else {
            x->lnew = 1;
            
            free(x->lkey);
            if ((x->lkey = malloc(i+1)) == NULL) {
                post("puredis: can not proceed!!  Memory Error!"); return;
            }
            strcpy(x->lkey, (char*)s);
        }
    }
}

static void puredis_csv_cb2 (int c, void *userdata)
{
    t_puredis *x = (t_puredis *)userdata;
    x->lnew = 0;
    x->lcount++;
}

static void puredis_csv_init(t_puredis *x)
{
    x->lcount = 0; x->lnew = 0; x->lnumload = 0; x->lnumerror = 0;
    if ((x->lkey = malloc(8)) == NULL) {
        post("puredis: can not proceed!!  Memory Error!"); return;
    }
    
    char * cmd = NULL;
    if (x->ltype == gensym("string")) {
        cmd = "SET";
    } else if (x->ltype == gensym("list")) {
        cmd = "RPUSH";
    } else if (x->ltype == gensym("set")) {
        cmd = "SADD";
    } else if (x->ltype == gensym("zset")) {
        if ((x->zscore = malloc(8)) == NULL) {
            post("puredis: can not proceed!!  Memory Error!"); return;
        }
        cmd = "ZADD";
    } else if (x->ltype == gensym("hash")) {
        x->hargc = 0;
        x->hcount = 0;
        x->hbufsize = 2;
        x->hheaders = NULL;
        if ((x->hheaders = calloc(x->hbufsize, sizeof(char*))) == NULL) {
            post("puredis: can not proceed!!  Memory Error!"); return;
        }
        cmd = "HSET";
    }
    
    if ((x->lcmd = malloc(strlen(cmd)+1)) == NULL) {
        post("puredis: can not proceed!!  Memory Error!"); return;
    }
    strcpy(x->lcmd, cmd);
}

static void puredis_csv_free(t_puredis *x)
{
    free(x->lkey);
    free(x->lcmd);
    if (x->ltype == gensym("hash")) {
        int i;
        for (i = 0; i < x->hargc; i++) free(x->hheaders[i]);
        free(x->hheaders);
    } else if (x->ltype == gensym("zset")) {
        free(x->zscore);
    }
}

void puredis_csv(t_puredis *x, t_symbol *s, int argc, t_atom *argv)
{
    char buf[1024]; size_t i; char filename[256];
    atom_string(argv, filename, 256);
    x->ltype = atom_getsymbol(argv+1);
    
    struct csv_parser p;
    FILE *csvfile = NULL;
    csv_init(&p, 0);
    csv_set_opts(&p, CSV_APPEND_NULL);
    puredis_csv_init(x);
    
    csvfile = fopen((const char*)filename, "rb");
    if (csvfile == NULL) {
        post("Puredis failed to open csv file: %s", csvfile);
        puredis_csv_free(x);
        return;
    }
    
    while ((i=fread(buf, 1, 1024, csvfile)) > 0) {
        if (csv_parse(&p, buf, i, puredis_csv_cb1, puredis_csv_cb2, x) != i) {
            post("Puredis error parsing csv file: %s", csv_strerror(csv_error(&p)));
            fclose(csvfile);
            puredis_csv_free(x);
            return;
        }
    }
    
    csv_fini(&p, puredis_csv_cb1, puredis_csv_cb2, x);
    csv_free(&p);
    puredis_csv_free(x);
    
    t_atom stats[7];
    SETSYMBOL(&stats[0], gensym("csv-load-status"));
    SETSYMBOL(&stats[1], gensym("lines"));
    SETFLOAT(&stats[2], x->lcount);
    SETSYMBOL(&stats[3], gensym("entries"));
    SETFLOAT(&stats[4], x->lnumload);
    SETSYMBOL(&stats[5], gensym("error"));
    SETFLOAT(&stats[6], x->lnumerror);
    outlet_list(x->x_obj.ob_outlet, &s_list, 7, &stats[0]);
}

static void setup_apuredis(void)
{
    apuredis_class = class_new(gensym("apuredis"),
        (t_newmethod)redis_new,
        (t_method)redis_free,
        sizeof(t_puredis),
        CLASS_DEFAULT,
        A_GIMME, 0);
    
    class_addbang(apuredis_class,apuredis_bang);
    class_addmethod(apuredis_class,
        (t_method)redis_command, gensym("command"),
        A_GIMME, 0);
    class_sethelpsymbol(apuredis_class, gensym("apuredis-help"));
}

static void apuredis_q_out(t_puredis * x)
{
    t_atom value;
    SETFLOAT(&value, x->qcount);
    outlet_float(x->q_out, atom_getfloat(&value));
}

void apuredis_yield(t_puredis * x)
{
    if (x->qcount > 0) {
        void * tmpreply = NULL;
        if ( redisGetReply(x->redis, &tmpreply) == REDIS_ERR) return;
        if (tmpreply == NULL) {
            int wdone = 0;
            if (redisBufferWrite(x->redis,&wdone) == REDIS_ERR) return;
            
            if (redisBufferRead(x->redis) == REDIS_ERR)
                return;
            if (redisGetReplyFromReader(x->redis,&tmpreply) == REDIS_ERR)
                return;
            
            if (tmpreply != NULL) {
                x->qcount--;
                redisReply * reply = (redisReply*)tmpreply;
                redis_parseReply(x, reply);
            }
        } else {
            x->qcount--;
            redisReply * reply = (redisReply*)tmpreply;
            redis_parseReply(x, reply);
        }
    }
    apuredis_q_out(x);
}

void apuredis_bang(t_puredis *x)
{
    apuredis_yield(x);
}

static void setup_spuredis(void)
{
    spuredis_class = class_new(gensym("spuredis"),
        (t_newmethod)redis_new,
        (t_method)redis_free,
        sizeof(t_puredis),
        CLASS_DEFAULT,
        A_GIMME, 0);
    
    class_addbang(spuredis_class,spuredis_bang);
    class_addmethod(spuredis_class,
        (t_method)spuredis_stop, gensym("stop"),0);
    class_addmethod(spuredis_class,
        (t_method)spuredis_start, gensym("start"),0);
    class_addmethod(spuredis_class,
        (t_method)spuredis_subscribe, gensym("subscribe"),
        A_GIMME, 0);
    class_addmethod(spuredis_class,
        (t_method)spuredis_subscribe, gensym("unsubscribe"),
        A_GIMME, 0);
    class_sethelpsymbol(spuredis_class, gensym("spuredis-help"));
}

static void spuredis_run(t_puredis *x)
{
    if (x->sub_run) {
        void * tmpreply = NULL;
        if ( redisGetReply(x->redis, &tmpreply) == REDIS_ERR) return;
        if (tmpreply == NULL) {
            int wdone = 0;
            if (redisBufferWrite(x->redis,&wdone) == REDIS_ERR) return;
            
            if (redisBufferRead(x->redis) == REDIS_ERR)
                return;
            if (redisGetReplyFromReader(x->redis,&tmpreply) == REDIS_ERR)
                return;
            
            if (tmpreply != NULL) {
                redisReply * reply = (redisReply*)tmpreply;
                redis_parseReply(x, reply);
            }
        } else {
            redisReply * reply = (redisReply*)tmpreply;
            redis_parseReply(x, reply);
        }
        
        clock_delay(x->sub_clock, 100);
    }
}

static void spuredis_schedule(t_puredis *x)
{
    if (x->sub_run && x->sub_num < 1) {
        x->sub_run = 0;
        clock_unset(x->sub_clock);
    } else if ((!x->sub_run) && x->sub_num > 0) {
        x->sub_run = 1;
        clock_delay(x->sub_clock, 0);
    }
}

static void spuredis_manage(t_puredis *x, t_symbol *s, int argc)
{
    if (s == gensym("subscribe")) {
        x->sub_num = x->sub_num + argc;
    } else {
        x->sub_num = x->sub_num - argc;
    }
    
    spuredis_schedule(x);
}

void spuredis_bang(t_puredis *x)
{
    spuredis_schedule(x);
}

void spuredis_start(t_puredis *x, t_symbol *s)
{
    spuredis_schedule(x);
}

void spuredis_stop(t_puredis *x, t_symbol *s)
{
    x->sub_run = 0;
}

void spuredis_subscribe(t_puredis *x, t_symbol *s, int argc, t_atom *argv)
{
    if (argc < 1) {
        post("spuredis: subscribe need at least one channel"); return;
    }
    
    int i;
    char ** vector = NULL;
    size_t * lengths = NULL;
    
    if (((vector = malloc((argc+1)*sizeof(char*))) == NULL) || ((lengths = malloc((argc+1)*sizeof(size_t))) == NULL)) {
        post("puredis: can not proceed!!  Memory Error!"); return;
    }
    
    /* setting subscribe or unscribe as redis first command part */
    if (s == gensym("subscribe")) {
        if ((vector[0] = malloc(strlen("subscribe")+1)) == NULL) {
            post("puredis: can not proceed!!  Memory Error!"); return;
        }
        strcpy(vector[0], "subscribe");
        lengths[0] = strlen(vector[0]);
    } else {
        if ((vector[0] = malloc(strlen("unsubscribe")+1)) == NULL) {
            post("puredis: can not proceed!!  Memory Error!"); return;
        }
        strcpy(vector[0], "unsubscribe");
        lengths[0] = strlen(vector[0]);
    }
    
    for (i = 0; i < argc; i++) {
        char cmdpart[256];
        atom_string(argv+i, cmdpart, 256);
        
        if ((vector[i+1] = malloc(strlen(cmdpart)+1)) == NULL) {
            post("puredis: can not proceed!!  Memory Error!"); return;
        }
        
        strcpy(vector[i+1], (char*)cmdpart);
        lengths[i+1] = strlen(vector[i+1]);
    }
    redis_postCommandAsync(x, argc+1, vector, lengths);
    spuredis_manage(x, s, argc);
}

