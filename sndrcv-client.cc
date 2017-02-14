/*
 * sndrcv-client.cc  test mercury  (client)
 * 03-Feb-2017  chuck@ece.cmu.edu
 */

/*
 * this program contains a mercury RPC client that sends "count"
 * number of RPC requests and exits when all the replies in.
 *
 * the program can run multiple instances of the mercury client
 * in the same process.   server port numbers are assigned
 * sequentially starting at BASEPORT (defined below as 19900).
 * (the address spec uses a printf "%d" to fill the port number...)
 * we init the client side with ports after that...
 *
 * there are two sending modes: normally the client sends the
 * RPC requests in parallel, but if you setenv "SERIALSEND" it
 * will wait an RPC request to complete before sending the next one.
 *
 * note: the number of instances between the client and server
 * should match.
 *
 * usage: ./sndrcv-client n-instances local-addr-spec remote-addr-spec
 *
 * example:
 *   # server is on 10.93.1.154, local IP is 10.93.1.146
 *   ./sndrcv-client 1 bmi+tcp://10.93.1.146:%d bmi+tcp://10.93.1.154:%d
 *   ./sndrcv-client 1 cci+tcp://10.93.1.146:%d cci+tcp://10.93.1.154:%d
 *   # 1 instance, remote server port=19900, local port=19901
 *
 *   ./sndrcv-client 1 cci+tcp://10.93.1.146:%d cci+tcp://10.93.1.154:%d
 *   # 2 instances, remote server port=19900,19901 local port=19902,19903
 */


#include <assert.h>
#include <err.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>
#include <time.h>

#include <mercury.h>
#include <mercury_macros.h>

#define BASEPORT 19900   /* starting TCP port we contact (instance 0) */
#define DEF_COUNT 5      /* default number of msgs to send and recv in a run */
#define TIMEOUT 120      /* set alarm time (seconds) */

/*
 * g: shared global data
 */
struct g {
    int ninst;               /* from the cmd line */
    char *localspec;         /* from the cmd line */
    char *remotespec;        /* from the cmd line */
    int count;               /* number of msgs to send in a run */
    int serialsend;          /* use serial send mode */
    int quiet;               /* don't print during transfer */
} g;

/*
 * is: per-instance state structure.   we malloc an array of these at
 * startup.
 */
struct is {
    int n;                   /* our instance number (0 .. n-1) */
    hg_class_t *hgclass;     /* class for this instance */
    hg_context_t *hgctx;     /* context for this instance */
    hg_id_t myrpcid;         /* the ID of the instance's RPC */
    pthread_t sthread;       /* server thread */
    char myid[256];          /* my local merc address */
    char remoteid[256];      /* remote merc address */
    hg_addr_t remoteaddr;    /* encoded remote address */
    char myfun[64];          /* my function name */

    /* sending count stuff (nsent) */
    pthread_mutex_t slock;   /* nsent lock */
    pthread_cond_t scond;    /* nsent cond var */
    int nsent;               /* number succesfully sent - mutex protects */

    /* no mutex since only the main thread can write it */
    int sends_done;          /* set to non-zero when nsent is done */
};
struct is *is;    /* an array of state */

/*
 * lookup_state: for looking up an address
 */
struct lookup_state {
    pthread_mutex_t lock;    /* protect state */
    int n;                   /* instance number that owns lookup */
    int done;                /* set non-zero if done */
    pthread_cond_t lkupcond; /* caller waits on this */
};

/*
 * input and output structures (this also generates XDR fns using boost pp)
 */
MERCURY_GEN_PROC(rpcin_t, ((int32_t)(ret)))
MERCURY_GEN_PROC(rpcout_t, ((int32_t)(ret)))

/*
 * forward prototypes here, so we can structure the source code to
 * be more linear to make it easier to read.
 */
static void *run_instance(void *arg);   /* run one instance */
static void *run_network(void *arg);    /* per-instance network thread */
static hg_return_t lookup_cb(const struct hg_cb_info *cbi);  /* client cb */
static hg_return_t forw_cb(const struct hg_cb_info *cbi);  /* client cb */

/* fake server call back, we are not a server so shouldn't happen */
static hg_return_t rpchandler(hg_handle_t handle) {
    errx(1, "rpchandler called on client?!?!");
}

/*
 * main program.  usage:
 *
 * ./merc-test n-instances local-addr-spec remote-addr-spec
 *
 * the address specs use a %d for port (e.g. 'bmp+tcp://%d')
 */
int main(int argc, char **argv) {
    int lcv, rv;
    pthread_t *tarr;
    char *c;
    if (argc != 4) 
        errx(0, "usage: %s n-instances local-addr-spec remote-addr-spec\n", 
               *argv);

    alarm(TIMEOUT);             /* so we don't hang forever */
    g.ninst = atoi(argv[1]);
    g.localspec = argv[2];
    g.remotespec = argv[3];
    if ((c = getenv("COUNT")) != NULL && (rv = atoi(c)) > 0) {
        g.count = rv;
    } else {
        g.count = DEF_COUNT;
    }
    g.serialsend = (getenv("SERIALSEND") != NULL);
    g.quiet = (getenv("QUIET") != NULL);

    printf("main: starting %d ...\n", g.ninst);
    tarr = (pthread_t *)malloc(g.ninst * sizeof(pthread_t));
    if (!tarr) errx(1, "malloc tarr failed");
    is = (struct is *)malloc(g.ninst *sizeof(*is));    /* array */
    if (!is) errx(1, "malloc is failed");
    memset(is, 0, g.ninst * sizeof(*is));

    /* fork off a thread for each instance */
    for (lcv = 0 ; lcv < g.ninst ; lcv++) {
        is[lcv].n = lcv;
        rv = pthread_create(&tarr[lcv], NULL, run_instance, (void*)&is[lcv]);
        if (rv != 0) {
            printf("pthread create failed %d\n", rv);
            exit(1);
        }
    }

    /* now wait for everything to finish */
    printf("main: collecting\n");
    for (lcv = 0 ; lcv < g.ninst ; lcv++) {
        pthread_join(tarr[lcv], NULL);
    }
    printf("main: collection done\n");
    
    exit(0);
}

/*
 * run_instance: the main routine for running one instance of mercury.
 * we pass the instance state struct in as the arg...
 */
void *run_instance(void *arg) {
    struct is *isp = (struct is *)arg;
    int n = isp->n;               /* recover n from isp */
    int lcv, rv;
    hg_return_t ret;
    struct lookup_state lst;
    hg_op_id_t lookupop;
    struct timespec start, end;
    uint64_t diff;
    
    printf("%d: instance running\n", n);
    is[n].n = n;

    /* use a different port for local so we don't walk on server */
    snprintf(is[n].myid, sizeof(is[n].myid), g.localspec, g.ninst+n+BASEPORT);
    snprintf(is[n].remoteid, sizeof(is[n].remoteid), g.remotespec, n+BASEPORT);
    printf("%d: attempt to init %s\n", n, is[n].myid);
    is[n].hgclass = HG_Init(is[n].myid, HG_FALSE);
    if (is[n].hgclass == NULL)  errx(1, "HG_init failed");
    is[n].hgctx = HG_Context_create(is[n].hgclass);
    if (is[n].hgctx == NULL)  errx(1, "HG_Context_create failed");
    
    /* XXX: how else can we cvt myfun string to hg_id_t ? */
    snprintf(is[n].myfun, sizeof(is[n].myfun), "f%d", n);
    is[n].myrpcid = HG_Register_name(is[n].hgclass, is[n].myfun, 
                                     hg_proc_rpcin_t, hg_proc_rpcout_t, 
                                     rpchandler);

    /* fork off a progress/trigger thread */
    is[n].sends_done = 0;   /* run_network reads this */
    rv = pthread_create(&is[n].sthread, NULL, run_network, (void*)&n);
    if (rv != 0) errx(1, "pthread create srvr failed");

    /* poor man's barrier, since we don't want to deal with MPI */
    printf("%d: init done.  sleeping 10\n", n);
    sleep(10);

    /* 
     * resolve the remote address ... only need to do this once, since
     * it is fixed for this program...
     */
    printf("%d: remote address lookup %s\n", n, is[n].remoteid);
    if (pthread_mutex_init(&lst.lock, NULL) != 0) errx(1, "l mutex init");
    pthread_mutex_lock(&lst.lock);
    lst.n = n;
    lst.done = 0;
    if (pthread_cond_init(&lst.lkupcond, NULL) != 0) errx(1, "cond init?");

    ret = HG_Addr_lookup(is[n].hgctx, lookup_cb, &lst, 
                         is[n].remoteid, &lookupop);
    if (ret != HG_SUCCESS) errx(1, "HG addr lookup launch failed");
    while (lst.done == 0) {
        if (pthread_cond_wait(&lst.lkupcond, &lst.lock) != 0) 
            errx(1, "lk cond wait");
    }
    if (lst.done < 0) errx(1, "lookup failed");
    pthread_cond_destroy(&lst.lkupcond);
    pthread_mutex_unlock(&lst.lock);
    pthread_mutex_destroy(&lst.lock);
    printf("%d: done remote address lookup\n", n);
   
    /* poor man's barrier, since we don't want to deal with MPI */
    printf("%d: address lookup done.  sleeping 10 again\n", n);
    sleep(10);

    printf("%d: sending...\n", n);
    if (pthread_mutex_init(&is[n].slock, NULL) != 0) errx(1, "s mutex init");
    is[n].nsent = 0;
    if (pthread_cond_init(&is[n].scond, NULL) != 0) errx(1, "scond init");

    /* start the clock before initiating sends */
    clock_gettime(CLOCK_MONOTONIC, &start);

    for (lcv = 0 ; lcv < g.count ; lcv++) {
        hg_handle_t rpchand;
        rpcin_t in;
        ret = HG_Create(is[n].hgctx, is[n].remoteaddr,
                        is[n].myrpcid, &rpchand);
        if (ret != HG_SUCCESS) errx(1, "hg create failed");

        in.ret = (lcv+1);

        if (!g.quiet) printf("%d: launching %d\n", n, in.ret);
        ret = HG_Forward(rpchand, forw_cb, &n, &in);
        if (ret != HG_SUCCESS) errx(1, "hg forward failed");
        if (!g.quiet) printf("%d: launched %d\n", n, in.ret);
        if (g.serialsend) {  /* sending one at a time, don't overlap */
            pthread_mutex_lock(&is[n].slock);
            while (is[n].nsent <= lcv) {
                if (pthread_cond_wait(&is[n].scond, &is[n].slock) != 0)
                    errx(1, "snd ser cond wait");
            }
            pthread_mutex_unlock(&is[n].slock);
        }
    }

    /* wait until all sends are complete (already done if serialsend) */
    pthread_mutex_lock(&is[n].slock);
    while (is[n].nsent < g.count) {
        if (pthread_cond_wait(&is[n].scond, &is[n].slock) != 0)
            errx(1, "snd cond wait");
    }

    /* stop the clock now that all sends completed */
    clock_gettime(CLOCK_MONOTONIC, &end);

    /* print out rpc stats */
    diff = 1e9 * (end.tv_sec - start.tv_sec) + end.tv_nsec - start.tv_nsec;
    printf("%d: average time per rpc = %lu nsec\n", n, diff / g.count);

    pthread_cond_destroy(&is[n].scond);
    pthread_mutex_unlock(&is[n].slock);
    pthread_mutex_destroy(&is[n].slock);
    is[n].sends_done = 1;    /* tells network thread to exit */
    printf("%d: all sends complete\n", n);
    
    /* done sending, wait for server to finish and exit */
    pthread_join(is[n].sthread, NULL);
    if (is[n].remoteaddr) {
        HG_Addr_free(is[n].hgclass, is[n].remoteaddr);
        is[n].remoteaddr = NULL;
    }
    printf("%d: all recvs complete\n", n);
    HG_Context_destroy(is[n].hgctx);
    HG_Finalize(is[n].hgclass);
    printf("%d: instance done\n", n);
}

/*
 * lookup_cb: this gets called when HG_Addr_lookup() completes.
 * we need to stash the results and wake the caller.
 */
static hg_return_t lookup_cb(const struct hg_cb_info *cbi) {
    struct lookup_state *lstp = (struct lookup_state *)cbi->arg;
    int n;

#if 0  
    /* 
     * XXX: bug, hg_core_trigger_lookup_entry() sets it to HG_CB_BULK
     * instead of HG_CB_LOOKUP.  bug.  jerome fixed feb 2, 2017
     */
    if (cbi->type != HG_CB_LOOKUP) 
        errx(1, "lookup_cb mismatch %d", cbi->type);
#endif

    pthread_mutex_lock(&lstp->lock);
    if (cbi->ret != HG_SUCCESS) {
        warnx("lookup_cb failed %d\n", cbi->ret);
        lstp->done = -1;
    } else {
        n = lstp->n;
        is[n].remoteaddr = cbi->info.lookup.addr;
        lstp->done = 1;
    }
    pthread_mutex_unlock(&lstp->lock);
    pthread_cond_signal(&lstp->lkupcond);

    return(HG_SUCCESS);
}

/*
 * forw_cb: this gets called on the client side when HG_Forward() completes
 * (i.e. when we get the reply from the remote side).
 */
static hg_return_t forw_cb(const struct hg_cb_info *cbi) {
    int *np = (int *)cbi->arg;
    int n;
    hg_handle_t hand;
    hg_return_t ret;
    rpcout_t out;

    n = *np;
    if (cbi->ret != HG_SUCCESS) errx(1, "forw_cb failed");
    if (cbi->type != HG_CB_FORWARD) errx(1, "forw_cb wrong type");
    hand = cbi->info.forward.handle;

    ret = HG_Get_output(hand, &out);
    if (ret != HG_SUCCESS) errx(1, "get output failed");

    if (!g.quiet) printf("%d: forw complete (code=%d)\n", n, out.ret);

    HG_Free_output(hand, &out);

    if (HG_Destroy(hand) != HG_SUCCESS) errx(1, "forw_cb destroy hand");

    /* update records and see if we need to signal we are done */
    pthread_mutex_lock(&is[n].slock);
    is[n].nsent++;
    if (g.serialsend || is[n].nsent >= g.count)
        pthread_cond_signal(&is[n].scond);
    pthread_mutex_unlock(&is[n].slock);
    
    return(HG_SUCCESS);
}

/*
 * run_network: network support pthread.   need to call progress to push the
 * network and then trigger to run the callback.  we do this all in 
 * one thread (meaning that we shouldn't block in the trigger function, 
 * or we won't make progress).  since we only have one thread running
 * trigger callback, we do not need to worry about concurrent access to
 * "got" ...
 */
static void *run_network(void *arg) {
    int n = *((int *)arg);
    unsigned int actual; 
    hg_return_t ret;
    actual = 0;

    printf("%d: network thread running\n", n);
    /* while (not done sending or not done recving */
    while (!is[n].sends_done) {

        do {
            ret = HG_Trigger(is[n].hgctx, 0, 1, &actual);
        } while (ret == HG_SUCCESS && actual);

        if (!is[n].sends_done) {
            HG_Progress(is[n].hgctx, 100);
        }
    }
    printf("%d: network thread complete\n", n);
}
