/*
 * QEMU posix-aio emulation
 *
 * Copyright IBM, Corp. 2008
 *
 * Authors:
 *  Anthony Liguori   <aliguori@us.ibm.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2.  See
 * the COPYING file in the top-level directory.
 *
 */

#include <sys/ioctl.h>
#include <pthread.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#include "osdep.h"
#include "qemu-common.h"
#include "posix-aio-compat.h"
#include "mytrace.h"

//extern int64_t GC_ENDTIME;
//#define DEBUG_LATENCY
//#define DEBUG_AIOCB

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cond = PTHREAD_COND_INITIALIZER;
static pthread_t thread_id;
static pthread_attr_t attr;
static int max_threads = 64;
static int cur_threads = 0;
static int idle_threads = 0;
static TAILQ_HEAD(, qemu_paiocb) request_list;

#ifdef HAVE_PREADV
static int preadv_present = 1;
#else
static int preadv_present = 0;
#endif

static void die2(int err, const char *what)
{
    fprintf(stderr, "%s failed: %s\n", what, strerror(err));
    abort();
}

static void die(const char *what)
{
    die2(errno, what);
}

static void mutex_lock(pthread_mutex_t *mutex)
{
    int ret = pthread_mutex_lock(mutex);
    if (ret) die2(ret, "pthread_mutex_lock");
}

static void mutex_unlock(pthread_mutex_t *mutex)
{
    int ret = pthread_mutex_unlock(mutex);
    if (ret) die2(ret, "pthread_mutex_unlock");
}

static int cond_timedwait(pthread_cond_t *cond, pthread_mutex_t *mutex,
                           struct timespec *ts)
{
    int ret = pthread_cond_timedwait(cond, mutex, ts);
    if (ret && ret != ETIMEDOUT) die2(ret, "pthread_cond_timedwait");
    return ret;
}

static void cond_signal(pthread_cond_t *cond)
{
    int ret = pthread_cond_signal(cond);
    if (ret) die2(ret, "pthread_cond_signal");
}

static void thread_create(pthread_t *thread, pthread_attr_t *attr,
                          void *(*start_routine)(void*), void *arg)
{
    int ret = pthread_create(thread, attr, start_routine, arg);
    if (ret) die2(ret, "pthread_create");
}

static size_t handle_aiocb_ioctl(struct qemu_paiocb *aiocb)
{
	int ret;

	ret = ioctl(aiocb->aio_fildes, aiocb->aio_ioctl_cmd, aiocb->aio_ioctl_buf);
	if (ret == -1)
		return -errno;

	/*
	 * This looks weird, but the aio code only consideres a request
	 * successfull if it has written the number full number of bytes.
	 *
	 * Now we overload aio_nbytes as aio_ioctl_cmd for the ioctl command,
	 * so in fact we return the ioctl command here to make posix_aio_read()
	 * happy..
	 */
	return aiocb->aio_nbytes;
}

#ifdef HAVE_PREADV

static ssize_t
qemu_preadv(int fd, const struct iovec *iov, int nr_iov, off_t offset)
{
    return preadv(fd, iov, nr_iov, offset);
}

static ssize_t
qemu_pwritev(int fd, const struct iovec *iov, int nr_iov, off_t offset)
{
    return pwritev(fd, iov, nr_iov, offset);
}

#else

static ssize_t
qemu_preadv(int fd, const struct iovec *iov, int nr_iov, off_t offset)
{
    return -ENOSYS;
}

static ssize_t
qemu_pwritev(int fd, const struct iovec *iov, int nr_iov, off_t offset)
{
    return -ENOSYS;
}

#endif

/*
 * Check if we need to copy the data in the aiocb into a new
 * properly aligned buffer.
 */
static int aiocb_needs_copy(struct qemu_paiocb *aiocb)
{
    if (aiocb->aio_flags & QEMU_AIO_SECTOR_ALIGNED) {
        int i;

        for (i = 0; i < aiocb->aio_niov; i++)
            if ((uintptr_t) aiocb->aio_iov[i].iov_base % 512)
                return 1;
    }

    return 0;
}

static size_t handle_aiocb_rw_vector(struct qemu_paiocb *aiocb)
{
    size_t offset = 0;
    ssize_t len;

    do {
        if (aiocb->aio_type == QEMU_PAIO_WRITE)
            len = qemu_pwritev(aiocb->aio_fildes,
                               aiocb->aio_iov,
                               aiocb->aio_niov,
                               aiocb->aio_offset + offset);
         else
            len = qemu_preadv(aiocb->aio_fildes,
                              aiocb->aio_iov,
                              aiocb->aio_niov,
                              aiocb->aio_offset + offset);
    } while (len == -1 && errno == EINTR);

    if (len == -1)
        return -errno;
    return len;
}

static size_t handle_aiocb_rw_linear(struct qemu_paiocb *aiocb, char *buf)
{
    size_t offset = 0;
    size_t len;

    while (offset < aiocb->aio_nbytes) {
         if (aiocb->aio_type == QEMU_PAIO_WRITE)
             len = pwrite(aiocb->aio_fildes,
                          (const char *)buf + offset,
                          aiocb->aio_nbytes - offset,
                          aiocb->aio_offset + offset);
         else
             len = pread(aiocb->aio_fildes,
                         buf + offset,
                         aiocb->aio_nbytes - offset,
                         aiocb->aio_offset + offset);

         if (len == -1 && errno == EINTR)
             continue;
         else if (len == -1) {
             offset = -errno;
             break;
         } else if (len == 0)
             break;

         offset += len;
    }

    return offset;
}

static size_t handle_aiocb_rw(struct qemu_paiocb *aiocb)
{
    size_t nbytes;
    char *buf;

    if (!aiocb_needs_copy(aiocb)) {
        /*
         * If there is just a single buffer, and it is properly aligned
         * we can just use plain pread/pwrite without any problems.
         */
        if (aiocb->aio_niov == 1)
             return handle_aiocb_rw_linear(aiocb, aiocb->aio_iov->iov_base);

        /*
         * We have more than one iovec, and all are properly aligned.
         *
         * Try preadv/pwritev first and fall back to linearizing the
         * buffer if it's not supported.
         */
	if (preadv_present) {
            nbytes = handle_aiocb_rw_vector(aiocb);
            if (nbytes == aiocb->aio_nbytes)
	        return nbytes;
            if (nbytes < 0 && nbytes != -ENOSYS)
                return nbytes;
            preadv_present = 0;
        }

        /*
         * XXX(hch): short read/write.  no easy way to handle the reminder
         * using these interfaces.  For now retry using plain
         * pread/pwrite?
         */
    }

    /*
     * Ok, we have to do it the hard way, copy all segments into
     * a single aligned buffer.
     */
    buf = qemu_memalign(512, aiocb->aio_nbytes);
    if (aiocb->aio_type == QEMU_PAIO_WRITE) {
        char *p = buf;
        int i;

        for (i = 0; i < aiocb->aio_niov; ++i) {
            memcpy(p, aiocb->aio_iov[i].iov_base, aiocb->aio_iov[i].iov_len);
            p += aiocb->aio_iov[i].iov_len;
        }
    }

    nbytes = handle_aiocb_rw_linear(aiocb, buf);
    if (aiocb->aio_type != QEMU_PAIO_WRITE) {
        char *p = buf;
        size_t count = aiocb->aio_nbytes, copy;
        int i;

        for (i = 0; i < aiocb->aio_niov && count; ++i) {
            copy = count;
            if (copy > aiocb->aio_iov[i].iov_len)
                copy = aiocb->aio_iov[i].iov_len;
            memcpy(aiocb->aio_iov[i].iov_base, p, copy);
            p     += copy;
            count -= copy;
        }
    }
    qemu_vfree(buf);

    return nbytes;
}

bool blocked_by_gc(struct qemu_paiocb *aiocb)
{
    if ((aiocb->is_from_ide == 1) && (aiocb->aio_type == QEMU_PAIO_READ) && 
            (get_timestamp() < aiocb->wait))
        return true;

    return false;
}

/* Coperd: previously kill is used by worker threads, would it be problematic 
 * here ??? 
 * Next step: may consider to add a specific qeuue for blocked I/Os and let 
 * worker threads handle this queue first 
 */
void ret_eio(struct qemu_paiocb *aiocb, pid_t pid)
{
    //mutex_lock(&lock);
    aiocb->is_blocked = 1;
    aiocb->active = 1;
    aiocb->ret = -EIO;
    //mutex_unlock(&lock);
    //mylog("EIO: is_from_die=%d, r/w: %d\n", aiocb->is_from_ide, aiocb->aio_type);
    //if (kill(pid, aiocb->ev_signo)) die("kill failed");
}

static void *aio_thread(void *unused)
{
    pid_t pid;
    sigset_t set;

    pid = getpid();

    /* block all signals */
    if (sigfillset(&set)) die("sigfillset");
    if (sigprocmask(SIG_BLOCK, &set, NULL)) die("sigprocmask");

    while (1) {
        struct qemu_paiocb *aiocb;
        size_t ret = 0;
        qemu_timeval tv;
        struct timespec ts;
        bool aiocb_blocked = false;

        qemu_gettimeofday(&tv);
        ts.tv_sec = tv.tv_sec + 10;
        ts.tv_nsec = 0;

        /* Coperd: fetch one aio from request_list */
        mutex_lock(&lock);

        /* Coperd: wait here when request queue is empty until timeout */
        while (TAILQ_EMPTY(&request_list) &&
                !(ret == ETIMEDOUT)) {
            ret = cond_timedwait(&cond, &lock, &ts);
        }

        if (TAILQ_EMPTY(&request_list))
            break;

#if 0
        mylog("This shoudn't be executed, please call 911 if you see this msg");
#endif
        aiocb = TAILQ_FIRST(&request_list);
        TAILQ_REMOVE(&request_list, aiocb, node);

#if 0
        mylog("=============Searching for Eligible IO=========\n");
        int tq_cnt = 0;
        TAILQ_FOREACH(aiocb, &request_list, node) {
            /* only remove non-GC requests out */
            tq_cnt++;

            if (!blocked_by_gc(aiocb)) {
                mylog("Get aiocb: ide: %d, type=%d, wait=%" PRId64 ", (%ld, %zd)\n", 
                        aiocb->is_from_ide, aiocb->aio_type, aiocb->wait,
                        aiocb->aio_offset/512, aiocb->aio_nbytes/512);
                break;
                //TAILQ_REMOVE(&request_list, aiocb, node);
                //break;
            }

            mylog("Skipping aiocb[%d]: ide: %d, type=%d, wait=%" PRId64 ", (%ld, %zd)\n", 
                    tq_cnt, aiocb->is_from_ide, aiocb->aio_type, aiocb->wait,
                    aiocb->aio_offset/512, aiocb->aio_nbytes/512);
        }
        mylog("=================End of Searching==============\n\n");

        if (aiocb == NULL)
            break;
        TAILQ_REMOVE(&request_list, aiocb, node);
#endif

        aiocb->active = 1;
        idle_threads--;
#ifdef DEBUG_IDLE_THREADS
        mylog("I get one aio, idle_thread=%d after (--)\n", idle_threads);
#endif
        mutex_unlock(&lock);

#ifdef DEBUG_LATENCY
        if (aiocb->is_from_ide == 1)
            mylog("aio start handling: sector_num=%" PRId64 " n=%zu\n", 
                    aiocb->aio_offset/512, aiocb->aio_nbytes/512);
#endif

        switch (aiocb->aio_type) {
            case QEMU_PAIO_READ:
                if (blocked_by_gc(aiocb)) {
#ifdef DEBUG_LATENCY
                    mylog("blocked: (%" PRId64 ", %zu), wait=%ld\n", 
                            aiocb->aio_offset/512, aiocb->aio_nbytes/512,
                            aiocb->wait - get_timestamp());
#endif
                    /* Coperd: insert I/O back to queue */
                    qemu_paio_reread(aiocb);
                    aiocb_blocked = true;
                    break;
                }
            case QEMU_PAIO_WRITE:
                ret = handle_aiocb_rw(aiocb);
                break;
            case QEMU_PAIO_IOCTL:
                ret = handle_aiocb_ioctl(aiocb);
                break;
            default:
                fprintf(stderr, "invalid aio request (0x%x)\n", aiocb->aio_type);
                ret = -EINVAL;
                break;
        }
#ifdef DEBUG_LATENCY
        if (aiocb->is_from_ide == 1) {
            mylog("Handled aiocb: ide: %d, type=%d, wait=%" PRId64 ", (%ld, %zd)\n", 
                    aiocb->is_from_ide, aiocb->aio_type, (aiocb->wait == 0) ? 0 : aiocb->wait - get_timestamp(),
                    aiocb->aio_offset/512, aiocb->aio_nbytes/512);
        }
#endif

        mutex_lock(&lock);
        if (!aiocb_blocked)
            aiocb->ret = ret;
        idle_threads++;
#ifdef DEBUG_IDLE_THREADS
        mylog("I finished one aio, idle_thread=%d after (++)\n", idle_threads);
#endif
        mutex_unlock(&lock);

        if (!aiocb_blocked)
            if (kill(pid, aiocb->ev_signo)) die("kill failed");
    }

    idle_threads--;
#ifdef DEBUG_IDLE_THREADS
    mylog("thread exiting, idle_thread=%d after (--)\n", idle_threads);
#endif
    cur_threads--;
    mutex_unlock(&lock);

    return NULL;
}

static void spawn_thread(void)
{
    cur_threads++;
    idle_threads++;
#ifdef DEBUG_IDLE_THREADS
    mylog("creating a new thread, idle_thread=%d after (++)\n", idle_threads);
#endif
    thread_create(&thread_id, &attr, aio_thread, NULL);
}

int qemu_paio_init(struct qemu_paioinit *aioinit)
{
    int ret;

    ret = pthread_attr_init(&attr);
    if (ret) die2(ret, "pthread_attr_init");

    ret = pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    if (ret) die2(ret, "pthread_attr_setdetachstate");

    TAILQ_INIT(&request_list);

    return 0;
}

static int nb_io_blocked;
static int nb_io_total;
static int nb_read_io_total;

static int qemu_paio_submit(struct qemu_paiocb *aiocb, int type)
{
    aiocb->aio_type = type;

    if (aiocb->is_from_ide == 1) {
        nb_io_total++;
        if (aiocb->aio_type == QEMU_PAIO_READ)
            nb_read_io_total++;
        if (nb_io_total % 10 == 0) {
            //mylog("AIO_Total_IOs %d\n", nb_io_total);
            //mylog("AIO_Read_IOs %d\n", nb_read_io_total);
        }
    }

    if (blocked_by_gc(aiocb)) {
        nb_io_blocked++;
        //mylog("AIO_Blocked_IOs %d\n", nb_io_blocked);
    }

    aiocb->ret = -EINPROGRESS;
    aiocb->active = 0;
    mutex_lock(&lock);
    if (idle_threads == 0 && cur_threads < max_threads) {
        spawn_thread();
    }

    TAILQ_INSERT_TAIL(&request_list, aiocb, node);
    mutex_unlock(&lock);
    cond_signal(&cond);

    return 0;
}

int qemu_paio_resubmit(struct qemu_paiocb *aiocb, int type)
{
    aiocb->aio_type = type;
    aiocb->ret = -EINPROGRESS;
    aiocb->active = 0;
    mutex_lock(&lock);
    if (/*idle_threads == 0 && */cur_threads < max_threads)
          spawn_thread();
    TAILQ_INSERT_TAIL(&request_list, aiocb, node);
    mutex_unlock(&lock);
    cond_signal(&cond);

    return 0;
}

int qemu_paio_reread(struct qemu_paiocb *aiocb)
{
    return qemu_paio_resubmit(aiocb, QEMU_PAIO_READ);
}

int qemu_paio_rewrite(struct qemu_paiocb *aiocb)
{
    return qemu_paio_resubmit(aiocb, QEMU_PAIO_WRITE);
}

int qemu_paio_read(struct qemu_paiocb *aiocb)
{
    return qemu_paio_submit(aiocb, QEMU_PAIO_READ);
}

int qemu_paio_write(struct qemu_paiocb *aiocb)
{
    return qemu_paio_submit(aiocb, QEMU_PAIO_WRITE);
}

int qemu_paio_ioctl(struct qemu_paiocb *aiocb)
{
    return qemu_paio_submit(aiocb, QEMU_PAIO_IOCTL);
}

ssize_t qemu_paio_return(struct qemu_paiocb *aiocb)
{
    ssize_t ret;

    mutex_lock(&lock);
    ret = aiocb->ret;
    mutex_unlock(&lock);

    return ret;
}

int qemu_paio_error(struct qemu_paiocb *aiocb)
{
    ssize_t ret = qemu_paio_return(aiocb);

    if (ret < 0)
        ret = -ret;
    else
        ret = 0;

    return ret;
}

int qemu_paio_cancel(int fd, struct qemu_paiocb *aiocb)
{
    int ret;

    mutex_lock(&lock);
    if (!aiocb->active) {
        TAILQ_REMOVE(&request_list, aiocb, node);
        aiocb->ret = -ECANCELED;
        ret = QEMU_PAIO_CANCELED;
    } else if (aiocb->ret == -EINPROGRESS)
        ret = QEMU_PAIO_NOTCANCELED;
    else
        ret = QEMU_PAIO_ALLDONE;
    mutex_unlock(&lock);

    return ret;
}
