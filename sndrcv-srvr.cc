/*
 * sndrcv-srvr.cc  test mercury  (server)
 * 03-Feb-2017  chuck@ece.cmu.edu
 */

/*
 * this program contains a mercury RPC server that receives and
 * responds to "count" number of RPC requests and then exits.
 *
 * the program can run multiple instances of the mercury server
 * in the same process.   listening port numbers are assigned
 * sequentially starting at BASEPORT (defined below as 19900).
 * (the address spec uses a printf "%d" to fill the port number...)
 *
 * usage: ./sndrcv-srvr n-instances local-addr-spec
 *
 * example:
 *   ./sndrcv-srvr 1 bmi+tcp://10.93.1.154:%d      # 1 instance w/bmi
 *   ./sndrcv-srvr 1 cci+tcp://10.93.1.154:%d      # 1 instance w/cci
 *                                                 # both on port 19900
 *
 *   ./sndrcv-srvr 2 cci+tcp://10.93.1.154:%d      # 2 instance w/cci
 *                                                 # ports=19900, 19901
 */

#include <assert.h>
#include <err.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>

#include <mercury.h>
#include <mercury_macros.h>

#define BASEPORT 19900   /* starting TCP port we listen on (instance 0) */
#define DEF_COUNT 5      /* default number of msgs to send and recv in a run */
#define TIMEOUT 120      /* set alarm time (seconds) */

/*
 * g: shared global data
 */
struct g {
    char *serverspec;        /* from the cmd line */
    int count;               /* number of msgs to recv in a run */
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
    char myfun[64];          /* my function name */
    int got;                 /* number of RPCs server has got */
};
struct is *is;    /* an array of state */

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
static hg_return_t rpchandler(hg_handle_t handle); /* server cb */
static hg_return_t reply_sent_cb(const struct hg_cb_info *cbi);  /* server cb */

/*
 * main program.  usage:
 *
 * ./merc-test n-instances server-spec
 *
 * the address specs use a %d for port (e.g. 'bmp+tcp://%d')
 */
int main(int argc, char **argv) {
    int n, lcv, rv;
    pthread_t *tarr;
    char *c;
    if (argc != 3) 
        errx(0, "usage: %s n-instances local-addr-spec", *argv);

    alarm(TIMEOUT);   /* so we don't hang forever */
    n = atoi(argv[1]);
    g.serverspec = argv[2];
    if ((c = getenv("COUNT")) != NULL && (rv = atoi(c)) > 0) {
        g.count = rv;
    } else {
        g.count = DEF_COUNT;
    }

    printf("main: starting %d ...\n", n);
    tarr = (pthread_t *)malloc(n * sizeof(pthread_t));
    if (!tarr) errx(1, "malloc tarr failed");
    is = (struct is *)malloc(n *sizeof(*is));    /* array */
    if (!is) errx(1, "malloc is failed");
    memset(is, 0, n * sizeof(*is));

    /* fork off a thread for each instance */
    for (lcv = 0 ; lcv < n ; lcv++) {
        is[lcv].n = lcv;
        rv = pthread_create(&tarr[lcv], NULL, run_instance, (void*)&is[lcv]);
        if (rv != 0) {
            printf("pthread create failed %d\n", rv);
            exit(1);
        }
    }

    /* now wait for everything to finish */
    printf("main: collecting\n");
    for (lcv = 0 ; lcv < n ; lcv++) {
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
    
    printf("%d: instance running\n", n);
    is[n].n = n;

    snprintf(is[n].myid, sizeof(is[n].myid), g.serverspec, n+BASEPORT);
    printf("%d: attempt to init %s\n", n, is[n].myid);
    is[n].hgclass = HG_Init(is[n].myid, HG_TRUE);
    if (is[n].hgclass == NULL)  errx(1, "HG_init failed");
    is[n].hgctx = HG_Context_create(is[n].hgclass);
    if (is[n].hgctx == NULL)  errx(1, "HG_Context_create failed");
    
    snprintf(is[n].myfun, sizeof(is[n].myfun), "f%d", n);
    printf("%d: function name is %s\n", n, is[n].myfun);
    is[n].myrpcid = HG_Register_name(is[n].hgclass, is[n].myfun, 
                                     hg_proc_rpcin_t, hg_proc_rpcout_t, 
                                     rpchandler);
    /* we use registered data to pass instance number to server callback */
    if (HG_Register_data(is[n].hgclass, is[n].myrpcid, &n, NULL) != HG_SUCCESS)
        errx(1, "unable to register n as data");

    /* fork off a progress/trigger thread */
    rv = pthread_create(&is[n].sthread, NULL, run_network, (void*)&n);
    if (rv != 0) errx(1, "pthread create srvr failed");

    /* wait for server to finish recv'ing count msgs and exit */
    printf("%d: init done.  waiting for recvs to complete\n", n);
    pthread_join(is[n].sthread, NULL);
    printf("%d: all recvs complete\n", n);
    HG_Context_destroy(is[n].hgctx);
    HG_Finalize(is[n].hgclass);
    printf("%d: instance done\n", n);
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
    is[n].got = actual = 0;

    printf("%d: network thread running\n", n);
    /* while (not done sending or not done recving */
    while (is[n].got < g.count) {

        do {
            ret = HG_Trigger(is[n].hgctx, 0, 1, &actual);
        } while (ret == HG_SUCCESS && actual);

        /* recheck, since trigger can change is[n].got */
        if (is[n].got < g.count) {
            HG_Progress(is[n].hgctx, 100);
        }
    }
    printf("%d: network thread complete\n", n);
}

/*
 * server side funcions....
 */

/*
 * rpchandler: called on the server when a new RPC comes in
 */
static hg_return_t rpchandler(hg_handle_t handle) {
    struct hg_info *hgi;
    int n, *np;
    hg_return_t ret;
    rpcin_t in;
    rpcout_t out;
     
    /* gotta extract "n" using handle, 'cause that's the only way pass it */
    hgi = HG_Get_info(handle);
    if (!hgi) errx(1, "bad hgi");
    np = (int *)HG_Registered_data(hgi->hg_class, hgi->id);
    if (!np) errx(1, "bad np");
    n = *np;

    ret = HG_Get_input(handle, &in);
    if (ret != HG_SUCCESS) errx(1, "HG_Get_input failed");
    printf("%d: got remote input %d\n", n, in.ret);
    out.ret = in.ret * -1;
    ret = HG_Free_input(handle, &in);

    /* the callback will bump "got" after respond has been sent */
    ret = HG_Respond(handle, reply_sent_cb, np, &out);
    if (ret != HG_SUCCESS) errx(1, "HG_Respond failed");

    return(HG_SUCCESS);
}

/*
 * reply_sent_cb: called after the server's reply to an RPC completes.
 */
static hg_return_t reply_sent_cb(const struct hg_cb_info *cbi) {
    int n;
    if (cbi->type != HG_CB_RESPOND) errx(1, "unexpected sent cb");
    n = *((int *)cbi->arg);

    /*
     * currently safe: there is only one network thread and we 
     * are in it (via trigger fn).
     */
    is[n].got++;

    /* return handle to the pool for reuse */
    HG_Destroy(cbi->info.respond.handle);
}
